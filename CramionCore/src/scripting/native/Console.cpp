// La consola (ScriptSystem::run): llamadas a la API escritas a mano en la
// Consola del editor (y por el MCP), sin Lua. Un interprete pequeno que solo
// llama a la API por su nombre (rt.native.call / get / set), asi que sirve
// para todo lo que registran los modulos, tambien lo que se anada despues.
//
//   Audio.playOneShot('clic.wav')
//   Scene.find('Jugador'):translate(Vec3(0, 1, 0))
//   Scene.find('Jugador').name                    -> escribe el nombre
//   Graphics.vsync = false
//   Graphics.set{ texture_quality = 'high', target_fps = 144 }
//   local j = Scene.find('Jugador'); j.position = Vec3(0, 2, 0); return j.position
//
// Gramatica:
//   programa   := { sentencia [';'] }           (tambien separa un salto de linea)
//   sentencia  := 'return' [expr]               (lo ultimo del programa)
//               | 'local' nombre '=' expr
//               | sufijo '=' expr               (asignacion)
//               | expr                          (una llamada o una lectura)
//   expr       := ['-'] sufijo                  ('-' solo con numeros y Vec3)
//   sufijo     := primario { '.' nombre | ':' nombre args | args | '[' expr ']' }
//   args       := '(' [expr {',' expr}] ')' | objeto | texto     (f{...} y f'x' como en Lua)
//   primario   := numero | texto | true | false | nil | lista | objeto | '(' expr ')'
//               | Vec3 '(' x, y, z ')' | Quat '(' x, y, z, w ')' | nombre
//   lista      := '[' [expr {',' expr}] ']'
//   objeto     := '{' [campo {(',' | ';') campo}] '}'    campo := nombre '=' expr
//                 ({a, b} sin claves tambien es una lista; {} es un objeto vacio)
//   numero     := 12, -3.5, 1e3        texto := "..." o '...' (con \n \t \\ \" \')
//   comentario := '--' hasta el final de la linea
//
// Que es cada nombre:
//   - Una variable: las de `local` (solo en ese programa) y las asignadas sin
//     `local` (`j = Scene.find('Jugador')`), que siguen para los siguientes
//     comandos hasta parar el juego.
//   - Si no, la API: `Tabla.funcion(...)` llama, `Tabla.prop` lee y
//     `Tabla.prop = v` escribe (tambien `Graphics.post.bloom`); `funcion(...)`
//     llama a una global (print).
//   - Sobre un valor: `v:metodo(...)` (o `v.metodo(...)`) llama a un metodo
//     (Entity:translate), `v.prop` lee o escribe una propiedad (Entity:name);
//     de un objeto, su campo ({point = ...}.point); de un Vec3 o Quat, x y z
//     (w); `v[i]` es el elemento i de una lista (desde 1, como la API) o la
//     clave de un objeto.
// Resultado: el valor de `return expr`, o el de la ultima sentencia si era una
// expresion (como texto: Value::asString); nada si era una asignacion. Si hay
// un error devuelve false y el mensaje (con la linea si hay varias). Primero
// se lee todo: con un error de sintaxis no se ejecuta nada.

#include "Modules.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <optional>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace cramion::scripting::native {
namespace {

using Variables = std::unordered_map<std::string, api::Value>;

// Un error de la consola (de sintaxis o al ejecutar) con su linea.
struct ConsoleError : std::runtime_error {
    ConsoleError(const std::string& message, int at_line) : std::runtime_error(message), line(at_line) {}
    int line;
};

struct Token {
    enum class Kind { End, Newline, Name, Number, String, Symbol };
    Kind kind = Kind::End;
    std::string text;  // el nombre, el texto (ya sin escapes) o el simbolo
    double number = 0.0;
    int line = 1;
};

bool isNameStart(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
bool isNameChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }
bool isDigit(char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }

// Las palabras de Lua que la consola no tiene (un mensaje claro en vez de
// un error de sintaxis raro).
bool luaKeyword(const std::string& name) {
    static const char* const kWords[] = {"and",      "break", "do",    "else", "elseif", "end",    "for",
                                         "function", "goto",  "if",    "in",   "not",    "or",     "repeat",
                                         "then",     "until", "while"};
    return std::any_of(std::begin(kWords), std::end(kWords), [&](const char* w) { return name == w; });
}

std::vector<Token> tokenize(const std::string& code) {
    std::vector<Token> out;
    int line = 1;
    int depth = 0;  // dentro de (), [] o {} un salto de linea no separa
    std::size_t i = 0;
    const std::size_t n = code.size();
    const auto add = [&](Token::Kind kind, std::string text, double number = 0.0) {
        Token t;
        t.kind = kind;
        t.text = std::move(text);
        t.number = number;
        t.line = line;
        out.push_back(std::move(t));
    };
    const auto fail = [&](const std::string& message) { throw ConsoleError(message, line); };
    while (i < n) {
        const char c = code[i];
        if (c == '\n') {
            if (depth == 0) add(Token::Kind::Newline, "\n");
            ++line;
            ++i;
        } else if (c == ' ' || c == '\t' || c == '\r') {
            ++i;
        } else if (c == '-' && i + 1 < n && code[i + 1] == '-') {
            while (i < n && code[i] != '\n') ++i;  // comentario
        } else if (isDigit(c) || (c == '.' && i + 1 < n && isDigit(code[i + 1]))) {
            std::size_t end = i;
            while (end < n && (isDigit(code[end]) || code[end] == '.')) ++end;
            if (end < n && (code[end] == 'e' || code[end] == 'E')) {
                std::size_t e = end + 1;
                if (e < n && (code[e] == '+' || code[e] == '-')) ++e;
                if (e < n && isDigit(code[e])) {
                    end = e;
                    while (end < n && isDigit(code[end])) ++end;
                }
            }
            double value = 0.0;
            const auto r = std::from_chars(code.data() + i, code.data() + end, value);
            if (r.ec != std::errc{} || r.ptr != code.data() + end || (end < n && isNameChar(code[end]))) {
                std::size_t stop = end;
                while (stop < n && isNameChar(code[stop])) ++stop;
                fail("numero mal escrito: " + code.substr(i, stop - i));
            }
            add(Token::Kind::Number, code.substr(i, end - i), value);
            i = end;
        } else if (isNameStart(c)) {
            std::size_t end = i + 1;
            while (end < n && isNameChar(code[end])) ++end;
            add(Token::Kind::Name, code.substr(i, end - i));
            i = end;
        } else if (c == '"' || c == '\'') {
            std::string text;
            std::size_t j = i + 1;
            for (;; ++j) {
                if (j >= n || code[j] == '\n') fail("texto sin cerrar (falta " + std::string(1, c) + ")");
                if (code[j] == c) break;
                if (code[j] != '\\') {
                    text += code[j];
                    continue;
                }
                if (++j >= n) fail("texto sin cerrar (falta " + std::string(1, c) + ")");
                switch (code[j]) {
                    case 'n': text += '\n'; break;
                    case 't': text += '\t'; break;
                    case 'r': text += '\r'; break;
                    case '0': text += '\0'; break;
                    case '\\': case '"': case '\'': text += code[j]; break;
                    default: fail(std::string("escape desconocido en un texto: \\") + code[j]);
                }
            }
            add(Token::Kind::String, std::move(text));
            i = j + 1;
        } else if (std::string_view("()[]{},;.:=-").find(c) != std::string_view::npos) {
            // Operadores de Lua que la consola no tiene.
            const char next = i + 1 < n ? code[i + 1] : '\0';
            if ((c == '.' && next == '.') || (c == '=' && next == '=')) {
                fail(std::string("la consola no tiene operadores ('") + c + next +
                     "'): solo llamadas, lecturas y asignaciones");
            }
            if (c == '(' || c == '[' || c == '{') ++depth;
            if ((c == ')' || c == ']' || c == '}') && depth > 0) --depth;
            add(Token::Kind::Symbol, std::string(1, c));
            ++i;
        } else if (std::string_view("+*/%^#<>~&|!").find(c) != std::string_view::npos) {
            fail(std::string("la consola no tiene operadores ('") + c + "'): solo llamadas, lecturas y asignaciones");
        } else {
            fail("caracter inesperado '" + std::string(1, c) + "'");
        }
    }
    add(Token::Kind::End, {});
    return out;
}

// "numero", "entidad", "Mesh"...: para los mensajes.
std::string typeLabel(const api::Value& v) {
    switch (v.type()) {
        case api::Value::Type::Nil: return "nil";
        case api::Value::Type::Bool: return "bool";
        case api::Value::Type::Number: return "numero";
        case api::Value::Type::String: return "texto";
        case api::Value::Type::Vec3: return "Vec3";
        case api::Value::Type::Quat: return "Quat";
        case api::Value::Type::Entity: return "entidad";
        case api::Value::Type::Array: return "lista";
        case api::Value::Type::Object: return "objeto";
        case api::Value::Type::Function: return "funcion";
        case api::Value::Type::Handle: return std::string(v.asHandle()->typeName());
    }
    return "?";
}

// Un objeto del motor (entidad o handle): tiene metodos y propiedades de la API.
bool engineObject(const api::Value& v) { return v.isEntity() || v.isHandle(); }

class Interpreter {
public:
    Interpreter(Runtime& rt, Variables& globals) : rt_(rt), globals_(globals) {}

    bool run(const std::string& code, std::string* output) {
        std::string result;
        bool ok = true;
        try {
            tokens_ = tokenize(code);
            // Primero solo se lee (un error de sintaxis no ejecuta nada) y despues se ejecuta.
            for (const bool exec : {false, true}) {
                exec_ = exec;
                pos_ = 0;
                locals_.clear();
                last_.reset();
                returned_ = false;
                program();
            }
            if (last_) result = last_->asString();
        } catch (const ConsoleError& e) {
            ok = false;
            result = withLine(code, e.line, e.what());
        } catch (const std::exception& e) {
            ok = false;
            result = withLine(code, line_, e.what());
        }
        if (output != nullptr) *output = std::move(result);
        return ok;
    }

private:
    // Lo que se lee o se asigna, aun sin leer (asi se puede asignar).
    struct Ref {
        enum class Kind { Value, Variable, Path, Member, Index };
        Kind kind = Kind::Value;
        api::Value value;  // Value: el valor; Member e Index: el objeto
        std::string name;  // Variable; Path ("Time.deltaTime", o un nombre suelto); Member: el campo
        api::Value key;    // Index: la clave
    };
    static Ref valueRef(api::Value v) {
        Ref r;
        r.value = std::move(v);
        return r;
    }
    static Ref namedRef(Ref::Kind kind, std::string name, api::Value object = {}) {
        Ref r;
        r.kind = kind;
        r.name = std::move(name);
        r.value = std::move(object);
        return r;
    }

    static std::string withLine(const std::string& code, int line, const std::string& message) {
        std::size_t last = code.find_last_not_of(" \t\r\n");
        std::size_t first = code.find_first_not_of(" \t\r\n");
        const bool multiline = first != std::string::npos && code.find('\n', first) < last;
        return multiline ? "linea " + std::to_string(line) + ": " + message : message;
    }

    // --- Tokens ---
    const Token& peek(std::size_t ahead = 0) const { return tokens_[std::min(pos_ + ahead, tokens_.size() - 1)]; }
    const Token& next() {
        const Token& t = peek();
        if (pos_ < tokens_.size() - 1) ++pos_;
        return t;
    }
    bool isSymbol(const Token& t, const char* s) const { return t.kind == Token::Kind::Symbol && t.text == s; }
    bool acceptSymbol(const char* s) {
        if (!isSymbol(peek(), s)) return false;
        next();
        return true;
    }
    bool acceptName(const char* s) {
        if (peek().kind != Token::Kind::Name || peek().text != s) return false;
        next();
        return true;
    }
    [[noreturn]] void syntax(const std::string& message) const {
        const Token& t = peek();
        std::string near = t.kind == Token::Kind::End       ? "el final"
                           : t.kind == Token::Kind::Newline ? "el salto de linea"
                           : t.kind == Token::Kind::String  ? "'\"" + t.text + "\"'"
                                                            : "'" + t.text + "'";
        throw ConsoleError(message + " cerca de " + near, t.line);
    }
    void expectSymbol(const char* s) {
        if (!acceptSymbol(s)) syntax(std::string("se esperaba '") + s + "'");
    }
    std::string expectName() {
        if (peek().kind != Token::Kind::Name) syntax("se esperaba un nombre");
        return next().text;
    }
    bool atStatementEnd() const {
        const Token& t = peek();
        return t.kind == Token::Kind::End || t.kind == Token::Kind::Newline || isSymbol(t, ";");
    }
    void skipSeparators() {
        while (peek().kind == Token::Kind::Newline || isSymbol(peek(), ";")) next();
    }
    [[noreturn]] void fail(const std::string& message) const { throw ConsoleError(message, line_); }

    // --- Sentencias ---
    void program() {
        for (;;) {
            skipSeparators();
            if (peek().kind == Token::Kind::End) return;
            statement();
            if (returned_) {
                skipSeparators();
                if (peek().kind != Token::Kind::End) syntax("'return' tiene que ser lo ultimo");
                return;
            }
        }
    }

    void statement() {
        line_ = peek().line;
        if (acceptName("return")) {
            last_.reset();
            if (!atStatementEnd()) {
                api::Value v = expression();
                if (exec_) last_ = std::move(v);
            }
            returned_ = true;
            return;
        }
        if (acceptName("local")) {
            const std::string name = expectName();
            if (luaKeyword(name) || name == "return" || name == "local" || name == "true" || name == "false" || name == "nil") {
                syntax("'" + name + "' no vale como nombre de variable");
            }
            expectSymbol("=");
            api::Value v = expression();
            if (exec_) locals_[name] = std::move(v);
            last_.reset();
            return;
        }
        Ref target = expressionRef();
        if (acceptSymbol("=")) {
            if (target.kind == Ref::Kind::Value) syntax("no se puede asignar a esto");
            api::Value v = expression();
            store(target, v);
            last_.reset();
            return;
        }
        api::Value v = load(target);
        if (exec_) last_ = std::move(v);
    }

    // --- Expresiones ---
    api::Value expression() { return load(expressionRef()); }

    Ref expressionRef() {
        if (!acceptSymbol("-")) return suffixed();
        const api::Value v = load(suffixed());
        if (!exec_) return {};
        if (v.isNumber()) return valueRef(api::Value(-v.asNumber()));
        if (v.isVec3()) {
            const core::Vec3 p = v.asVec3();
            return valueRef(api::Value(core::Vec3{0.0f - p.x, 0.0f - p.y, 0.0f - p.z}));  // sin -0
        }
        fail("'-' solo vale con numeros y Vec3 (llego " + typeLabel(v) + ")");
    }

    Ref suffixed() {
        if (++depth_ > 200) syntax("demasiado anidado");
        Ref r = primary();
        for (;;) {
            if (acceptSymbol(".")) {
                const std::string name = expectName();
                if (r.kind == Ref::Kind::Path) {
                    r.name += "." + name;  // Tabla.subtabla.campo: un solo nombre de la API
                } else {
                    r = namedRef(Ref::Kind::Member, name, load(r));
                }
            } else if (acceptSymbol(":")) {
                const std::string name = expectName();
                const api::Value self = load(r);
                if (!startsArguments()) syntax("se esperaba '(' despues de :" + name);
                const api::Value::Array args = arguments();
                r = valueRef(callMethod(self, name, args));
            } else if (startsArguments()) {
                const api::Value::Array args = arguments();
                r = valueRef(call(r, args));
            } else if (acceptSymbol("[")) {
                api::Value object = load(r);
                api::Value key = expression();
                expectSymbol("]");
                Ref index;
                index.kind = Ref::Kind::Index;
                index.value = std::move(object);
                index.key = std::move(key);
                r = std::move(index);
            } else {
                break;
            }
        }
        --depth_;
        return r;
    }

    bool startsArguments() const {
        const Token& t = peek();
        return isSymbol(t, "(") || isSymbol(t, "{") || t.kind == Token::Kind::String;
    }

    api::Value::Array arguments() {
        api::Value::Array args;
        if (peek().kind == Token::Kind::String) {
            args.emplace_back(next().text);
        } else if (isSymbol(peek(), "{")) {
            args.push_back(table());
        } else {
            expectSymbol("(");
            if (!acceptSymbol(")")) {
                do {
                    args.push_back(expression());
                } while (acceptSymbol(","));
                expectSymbol(")");
            }
        }
        return args;
    }

    Ref primary() {
        const Token& t = peek();
        switch (t.kind) {
            case Token::Kind::Number: return valueRef(api::Value(next().number));
            case Token::Kind::String: return valueRef(api::Value(next().text));
            case Token::Kind::Name: {
                const std::string name = t.text;
                if (name == "true" || name == "false") {
                    next();
                    return valueRef(api::Value(name == "true"));
                }
                if (name == "nil") {
                    next();
                    return valueRef(api::Value{});
                }
                if (name == "return" || name == "local") syntax("'" + name + "' solo vale al principio de una sentencia");
                if (luaKeyword(name)) {
                    syntax("la consola no tiene '" + name + "' (solo llamadas, lecturas y asignaciones)");
                }
                if ((name == "Vec3" || name == "Quat") && isSymbol(peek(1), "(") && !isVariable(name)) {
                    next();
                    return valueRef(construct(name));
                }
                next();
                return namedRef(isVariable(name) ? Ref::Kind::Variable : Ref::Kind::Path, name);
            }
            case Token::Kind::Symbol:
                if (acceptSymbol("(")) {
                    api::Value v = expression();
                    expectSymbol(")");
                    return valueRef(std::move(v));
                }
                if (isSymbol(t, "[")) return valueRef(list());
                if (isSymbol(t, "{")) return valueRef(table());
                break;
            case Token::Kind::End:
            case Token::Kind::Newline: break;
        }
        syntax("se esperaba un valor");
    }

    // [a, b, c]
    api::Value list() {
        expectSymbol("[");
        api::Value::Array items;
        while (!acceptSymbol("]")) {
            items.push_back(expression());
            if (!acceptSymbol(",") && !isSymbol(peek(), "]")) syntax("se esperaba ',' o ']'");
        }
        return api::Value(std::move(items));
    }

    // {clave = valor, ...} (un objeto) o {a, b} (una lista).
    api::Value table() {
        expectSymbol("{");
        api::Value object = api::Value::object();
        api::Value::Array items;
        while (!acceptSymbol("}")) {
            if (peek().kind == Token::Kind::Name && isSymbol(peek(1), "=")) {
                const std::string key = next().text;
                next();
                object.set(key, expression());
            } else {
                items.push_back(expression());
            }
            if (!acceptSymbol(",") && !acceptSymbol(";") && !isSymbol(peek(), "}")) syntax("se esperaba ',' o '}'");
        }
        if (!items.empty() && object.size() > 0) fail("un {} es un objeto (clave = valor) o una lista, no las dos cosas");
        return items.empty() ? object : api::Value(std::move(items));
    }

    // Vec3(x, y, z) / Vec3(x, y) / Vec3() y Quat(x, y, z, w) / Quat().
    api::Value construct(const std::string& type) {
        const api::Value::Array args = arguments();
        if (!exec_) return {};
        const bool numbers = std::all_of(args.begin(), args.end(), [](const api::Value& a) { return a.isNumber(); });
        const auto f = [&](std::size_t i) { return static_cast<float>(args[i].asNumber()); };
        if (type == "Vec3") {
            if (args.size() == 1 && args[0].isVec3()) return args[0];
            if (numbers && args.empty()) return api::Value(core::Vec3{});
            if (numbers && args.size() == 2) return api::Value(core::Vec3{f(0), f(1), 0.0f});
            if (numbers && args.size() == 3) return api::Value(core::Vec3{f(0), f(1), f(2)});
            fail("Vec3(x, y, z): se esperaban tres numeros");
        }
        if (args.size() == 1 && args[0].isQuat()) return args[0];
        if (numbers && args.empty()) return api::Value(core::Quat{0.0f, 0.0f, 0.0f, 1.0f});
        if (numbers && args.size() == 4) return api::Value(core::Quat{f(0), f(1), f(2), f(3)});
        fail("Quat(x, y, z, w): se esperaban cuatro numeros");
    }

    // --- Variables y la API ---
    bool isVariable(const std::string& name) const { return locals_.count(name) != 0 || globals_.count(name) != 0; }

    // Una llamada a la API con su nombre delante del error (salvo que ya lo lleve).
    template <typename F>
    api::Value guarded(const std::string& key, F&& fn) {
        try {
            return fn();
        } catch (const ConsoleError&) {
            throw;
        } catch (const std::exception& e) {
            const std::string what = e.what();
            fail(what.find(key) != std::string::npos ? what : key + ": " + what);
        }
    }

    static std::string ownerOf(const api::Value& v) {
        return v.isEntity() ? std::string("Entity") : std::string(v.asHandle()->typeName());
    }

    // Por que un nombre suelto no es nada (ni variable ni de la API).
    [[noreturn]] void unknownName(const std::string& name) const {
        if (const api::Entry* e = rt_.native.find(name); e != nullptr && e->kind == api::Entry::Kind::Function) {
            fail(name + " es una funcion: escribe " + name + "(...)");
        }
        const auto& entries = rt_.native.entries();
        const bool table = std::any_of(entries.begin(), entries.end(), [&](const api::Entry& e) {
            return !e.member() && (e.owner == name || e.owner.rfind(name + ".", 0) == 0);
        });
        if (table) fail(name + " es una tabla de la API: escribe " + name + ".funcion(...) o " + name + ".propiedad");
        fail("no existe '" + name + "' (ni es una variable ni esta en la API)");
    }

    api::Value load(const Ref& r) {
        if (!exec_) return {};
        switch (r.kind) {
            case Ref::Kind::Value: return r.value;
            case Ref::Kind::Variable: {
                if (const auto it = locals_.find(r.name); it != locals_.end()) return it->second;
                return globals_[r.name];
            }
            case Ref::Kind::Path: {
                const api::Entry* e = rt_.native.find(r.name);
                if (r.name.find('.') == std::string::npos && (e == nullptr || e->kind != api::Entry::Kind::Property)) {
                    unknownName(r.name);
                }
                if (e != nullptr && e->kind == api::Entry::Kind::Function) {
                    fail(r.name + " es una funcion: escribe " + r.name + "(...)");
                }
                return guarded(r.name, [&] { return rt_.native.get(r.name); });
            }
            case Ref::Kind::Member: return member(r.value, r.name);
            case Ref::Kind::Index: return index(r.value, r.key);
        }
        return {};
    }

    api::Value member(const api::Value& object, const std::string& name) {
        if (engineObject(object)) {
            return guarded(ownerOf(object) + ":" + name, [&] { return rt_.native.get(name, object); });
        }
        if (object.isObject()) return object[name];
        if (object.isVec3() || object.isQuat()) {
            const bool quat = object.isQuat();
            const core::Quat q = object.asQuat();  // x, y, z (y w) en los dos casos
            if (name == "x") return api::Value(q.x);
            if (name == "y") return api::Value(q.y);
            if (name == "z") return api::Value(q.z);
            if (quat && name == "w") return api::Value(q.w);
            fail(std::string(quat ? "un Quat solo tiene x, y, z, w" : "un Vec3 solo tiene x, y, z") + " (no ." + name + ")");
        }
        if (object.isNil()) fail("se intenta leer ." + name + " de nil (no existe el objeto?)");
        fail("un " + typeLabel(object) + " no tiene ." + name);
    }

    api::Value index(const api::Value& object, const api::Value& key) {
        if (object.isArray() && key.isNumber()) {
            const double i = key.asNumber();
            if (i < 1.0 || i > static_cast<double>(object.size())) return {};
            return object[static_cast<std::size_t>(i) - 1];
        }
        if (key.isString()) {
            if (object.isObject()) return object[key.asString()];
            if (engineObject(object)) return member(object, key.asString());
        }
        if (object.isNil()) fail("se intenta indexar nil");
        fail("no se puede indexar un " + typeLabel(object) + " con un " + typeLabel(key));
    }

    void store(Ref& r, const api::Value& value) {
        if (!exec_) return;
        switch (r.kind) {
            case Ref::Kind::Value: fail("no se puede asignar a un valor");
            case Ref::Kind::Variable:
            case Ref::Kind::Path:
                if (r.name.find('.') == std::string::npos) {
                    // Una variable: la local si la hay; si no, de la consola.
                    if (const auto it = locals_.find(r.name); it != locals_.end()) {
                        it->second = value;
                    } else {
                        globals_[r.name] = value;
                    }
                    return;
                }
                guarded(r.name, [&] {
                    rt_.native.set(r.name, value);
                    return api::Value{};
                });
                return;
            case Ref::Kind::Member: assignMember(r.value, r.name, value); return;
            case Ref::Kind::Index:
                if (r.key.isString() && (r.value.isObject() || engineObject(r.value))) {
                    assignMember(r.value, r.key.asString(), value);
                    return;
                }
                fail(r.value.isArray() ? "los elementos de una lista no se cambian desde la consola"
                                       : "no se puede asignar en un " + typeLabel(r.value));
        }
    }

    void assignMember(api::Value& object, const std::string& name, const api::Value& value) {
        if (engineObject(object)) {
            guarded(ownerOf(object) + ":" + name, [&] {
                rt_.native.set(name, value, object);
                return api::Value{};
            });
            return;
        }
        if (object.isObject()) {
            object.set(name, value);  // el objeto es compartido: cambia el de la variable
            return;
        }
        if (object.isVec3() || object.isQuat()) fail("." + name + " de un " + typeLabel(object) + " no se cambia: asigna uno nuevo");
        if (object.isNil()) fail("se intenta asignar ." + name + " de nil (no existe el objeto?)");
        fail("un " + typeLabel(object) + " no tiene ." + name);
    }

    api::Value call(const Ref& r, const api::Value::Array& args) {
        if (!exec_) return {};
        switch (r.kind) {
            case Ref::Kind::Path:
                return guarded(r.name, [&] { return rt_.native.call(r.name, args); });
            case Ref::Kind::Member:
                if (engineObject(r.value)) return callMethod(r.value, r.name, args);
                if (r.value.isNil()) fail("se intenta llamar a ." + r.name + "() de nil (no existe el objeto?)");
                fail("." + r.name + " de un " + typeLabel(r.value) + " no es una funcion");
            case Ref::Kind::Variable: fail(r.name + " no es una funcion (es un " + typeLabel(load(r)) + ")");
            case Ref::Kind::Value:
            case Ref::Kind::Index: break;
        }
        fail("no se puede llamar a un " + typeLabel(load(r)));
    }

    api::Value callMethod(const api::Value& self, const std::string& name, const api::Value::Array& args) {
        if (!exec_) return {};
        if (engineObject(self)) {
            return guarded(ownerOf(self) + ":" + name, [&] { return rt_.native.call(name, args, self); });
        }
        if (self.isNil()) fail("se intenta llamar a :" + name + "() de nil (no existe el objeto?)");
        fail("un " + typeLabel(self) + " no tiene metodos (:" + name + ")");
    }

    Runtime& rt_;
    Variables& globals_;
    Variables locals_;
    std::vector<Token> tokens_;
    std::size_t pos_ = 0;
    int depth_ = 0;
    bool exec_ = false;
    bool returned_ = false;
    int line_ = 1;                     // la de la sentencia que corre (para los errores)
    std::optional<api::Value> last_;   // el valor de la ultima sentencia (o del return)
};

}  // namespace

void registerConsole(Runtime& rt) {
    // Las variables de la consola (asignadas sin `local`): hasta parar el juego.
    auto globals = std::make_shared<Variables>();
    rt.console = [&rt, globals](const std::string& code, std::string* output) {
        Interpreter interpreter(rt, *globals);
        return interpreter.run(code, output);
    };
    rt.onStop([globals](bool) { globals->clear(); });
}

}  // namespace cramion::scripting::native
