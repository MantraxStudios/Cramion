#include "CramionCore/ai/Expression.h"

#include "CramionCore/ai/StateMachine.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <utility>

namespace cramion::ai {

ExprValue ExprValue::boolean(bool b) {
    ExprValue v;
    v.type = Type::Bool;
    v.number = b ? 1.0 : 0.0;
    return v;
}

ExprValue ExprValue::num(double n) {
    ExprValue v;
    v.type = Type::Number;
    v.number = n;
    return v;
}

ExprValue ExprValue::str(std::string s) {
    ExprValue v;
    v.type = Type::String;
    v.text = std::move(s);
    return v;
}

ExprValue ExprValue::object(std::string id) {
    ExprValue v;
    v.type = Type::Object;
    v.text = std::move(id);
    return v;
}

namespace {

bool nameStart(char c) { return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_'; }
bool nameChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; }

// Palabras de Lua que no son de una expresion.
bool luaKeyword(const std::string& s) {
    static const char* const kWords[] = {"function", "return", "if",     "then", "else", "elseif", "end",  "local",
                                         "for",      "while",  "do",     "repeat", "until", "in",   "break", "goto"};
    for (const char* w : kWords) {
        if (s == w) return true;
    }
    return false;
}

// Un texto como numero (como Lua: "10" + 1 = 11).
bool toNumber(const ExprValue& v, double& out) {
    if (v.type == ExprValue::Type::Number) {
        out = v.number;
        return true;
    }
    if (v.type != ExprValue::Type::String) return false;
    const char* begin = v.text.c_str();
    char* end = nullptr;
    out = std::strtod(begin, &end);
    if (end == begin) return false;
    while (*end != '\0' && std::isspace(static_cast<unsigned char>(*end)) != 0) ++end;
    return *end == '\0';
}

std::string numberText(double n) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.14g", n);
    return buffer;
}

bool equal(const ExprValue& a, const ExprValue& b) {
    if (a.type != b.type) return false;
    switch (a.type) {
        case ExprValue::Type::Nil: return true;
        case ExprValue::Type::Bool:
        case ExprValue::Type::Number: return a.number == b.number;
        case ExprValue::Type::String:
        case ExprValue::Type::Object: return a.text == b.text;
    }
    return false;
}

}  // namespace

// --- Lectura ---------------------------------------------------------------------

class ExpressionParser {
public:
    ExpressionParser(std::string_view text, Expression& out) : s_(text), out_(out) {}

    void run() {
        next();
        if (kind_ == Tok::End && error_.empty()) {
            error_ = "expresion vacia";
        } else {
            const int root = parse(0);
            if (error_.empty() && kind_ != Tok::End) {
                fail(kind_ == Tok::Op && text_ == "(" ? "una llamada a una funcion (solo variables, numeros y comparaciones)"
                                                       : "sobra '" + text_ + "'");
            }
            if (error_.empty()) out_.root_ = root;
        }
        if (!error_.empty()) {
            out_.nodes_.clear();
            out_.root_ = -1;
            out_.error_ = error_;
        }
    }

private:
    enum class Tok : std::uint8_t { End, Number, String, Name, Op };
    using Node = Expression::Node;

    std::string_view s_;
    std::size_t pos_ = 0;
    Expression& out_;
    Tok kind_ = Tok::End;
    std::string text_;
    double number_ = 0.0;
    std::string error_;
    int depth_ = 0;

    int fail(const std::string& message) {
        if (error_.empty()) error_ = message;
        kind_ = Tok::End;
        return -1;
    }

    int add(Node n) {
        out_.nodes_.push_back(std::move(n));
        return static_cast<int>(out_.nodes_.size()) - 1;
    }

    // El siguiente token en kind_ / text_ / number_.
    void next() {
        if (!error_.empty()) {
            kind_ = Tok::End;
            return;
        }
        // Espacios y comentarios (-- hasta el final de la linea).
        for (;;) {
            while (pos_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[pos_])) != 0) ++pos_;
            if (pos_ + 1 < s_.size() && s_[pos_] == '-' && s_[pos_ + 1] == '-') {
                while (pos_ < s_.size() && s_[pos_] != '\n') ++pos_;
                continue;
            }
            break;
        }
        text_.clear();
        if (pos_ >= s_.size()) {
            kind_ = Tok::End;
            return;
        }
        const char c = s_[pos_];
        const char d = pos_ + 1 < s_.size() ? s_[pos_ + 1] : '\0';
        if (std::isdigit(static_cast<unsigned char>(c)) != 0 || (c == '.' && std::isdigit(static_cast<unsigned char>(d)) != 0)) {
            std::size_t end = pos_;
            while (end < s_.size() && (nameChar(s_[end]) || s_[end] == '.' ||
                                       ((s_[end] == '+' || s_[end] == '-') && end > pos_ &&
                                        (s_[end - 1] == 'e' || s_[end - 1] == 'E' || s_[end - 1] == 'p' || s_[end - 1] == 'P')))) {
                ++end;
            }
            text_ = std::string(s_.substr(pos_, end - pos_));
            pos_ = end;
            char* stop = nullptr;
            const bool hex = text_.size() > 2 && text_[0] == '0' && (text_[1] == 'x' || text_[1] == 'X');
            number_ = hex && text_.find_first_of(".pP") == std::string::npos
                          ? static_cast<double>(std::strtoull(text_.c_str() + 2, &stop, 16))
                          : std::strtod(text_.c_str(), &stop);
            if (stop == nullptr || *stop != '\0') {
                fail("numero mal escrito: " + text_);
                return;
            }
            kind_ = Tok::Number;
            return;
        }
        if (c == '"' || c == '\'') {
            ++pos_;
            while (pos_ < s_.size() && s_[pos_] != c) {
                char ch = s_[pos_++];
                if (ch == '\n') break;
                if (ch == '\\' && pos_ < s_.size()) {
                    const char e = s_[pos_++];
                    switch (e) {
                        case 'n': ch = '\n'; break;
                        case 't': ch = '\t'; break;
                        case 'r': ch = '\r'; break;
                        case '0': ch = '\0'; break;
                        default: ch = e; break;  // \\ \" \'
                    }
                }
                text_ += ch;
            }
            if (pos_ >= s_.size() || s_[pos_] != c) {
                fail("falta cerrar un texto");
                return;
            }
            ++pos_;
            kind_ = Tok::String;
            return;
        }
        if (nameStart(c)) {
            std::size_t end = pos_;
            while (end < s_.size() && nameChar(s_[end])) ++end;
            text_ = std::string(s_.substr(pos_, end - pos_));
            pos_ = end;
            kind_ = Tok::Name;
            return;
        }
        static const char* const kPairs[] = {"==", "~=", "!=", "<=", ">=", "&&", "||", "..", "//"};
        for (const char* pair : kPairs) {
            if (c == pair[0] && d == pair[1]) {
                if (c == '.' && pos_ + 2 < s_.size() && s_[pos_ + 2] == '.') {
                    fail("'...' es de Lua");
                    return;
                }
                text_ = pair;
                pos_ += 2;
                kind_ = Tok::Op;
                return;
            }
        }
        static const std::string_view kSingle = "+-*/%^<>()!.:,[]{}#";
        if (kSingle.find(c) != std::string_view::npos) {
            text_ = std::string(1, c);
            ++pos_;
            kind_ = Tok::Op;
            return;
        }
        if (c == '=') {
            fail("'=' no compara: usa ==");
            return;
        }
        fail(std::string("caracter inesperado '") + c + "'");
    }

    struct Binary {
        int precedence = 0;  // 0 = no es un operador
        bool right = false;
        Node::Kind kind = Node::Kind::Binary;
        char op = 0;
    };

    Binary binary() const {
        if (kind_ == Tok::Name) {
            if (text_ == "or") return {1, false, Node::Kind::Or, 0};
            if (text_ == "and") return {2, false, Node::Kind::And, 0};
            return {};
        }
        if (kind_ != Tok::Op) return {};
        const std::string& t = text_;
        if (t == "||") return {1, false, Node::Kind::Or, 0};
        if (t == "&&") return {2, false, Node::Kind::And, 0};
        if (t == "==") return {3, false, Node::Kind::Binary, '='};
        if (t == "~=" || t == "!=") return {3, false, Node::Kind::Binary, '!'};
        if (t == "<") return {3, false, Node::Kind::Binary, '<'};
        if (t == "<=") return {3, false, Node::Kind::Binary, 'l'};
        if (t == ">") return {3, false, Node::Kind::Binary, '>'};
        if (t == ">=") return {3, false, Node::Kind::Binary, 'g'};
        if (t == "..") return {4, true, Node::Kind::Binary, 'c'};
        if (t == "+") return {5, false, Node::Kind::Binary, '+'};
        if (t == "-") return {5, false, Node::Kind::Binary, '-'};
        if (t == "*") return {6, false, Node::Kind::Binary, '*'};
        if (t == "/") return {6, false, Node::Kind::Binary, '/'};
        if (t == "//") return {6, false, Node::Kind::Binary, 'f'};
        if (t == "%") return {6, false, Node::Kind::Binary, '%'};
        if (t == "^") return {8, true, Node::Kind::Binary, '^'};
        return {};
    }

    // Los operadores de precedencia >= min (or 1, and 2, comparaciones 3, .. 4,
    // + - 5, * / // % 6, not y - unarios 7, ^ 8), como en Lua.
    int parse(int min) {
        if (++depth_ > 200) return fail("expresion demasiado larga");
        int left = unary();
        for (;;) {
            if (!error_.empty()) break;
            const Binary op = binary();
            if (op.precedence == 0 || op.precedence < min) break;
            next();
            const int right = parse(op.right ? op.precedence : op.precedence + 1);
            if (!error_.empty()) break;
            Node n;
            n.kind = op.kind;
            n.op = op.op;
            n.a = left;
            n.b = right;
            left = add(std::move(n));
        }
        --depth_;
        return error_.empty() ? left : -1;
    }

    int unary() {
        const bool is_not = (kind_ == Tok::Name && text_ == "not") || (kind_ == Tok::Op && text_ == "!");
        const bool is_minus = kind_ == Tok::Op && text_ == "-";
        if (!is_not && !is_minus) return primary();
        next();
        const int operand = parse(8);
        if (!error_.empty()) return -1;
        Node n;
        n.kind = is_not ? Node::Kind::Not : Node::Kind::Negate;
        n.a = operand;
        return add(std::move(n));
    }

    int constant(ExprValue v) {
        Node n;
        n.kind = Node::Kind::Constant;
        n.value = std::move(v);
        next();
        return add(std::move(n));
    }

    int variable(std::string name) {
        Node n;
        n.kind = Node::Kind::Variable;
        n.name = std::move(name);
        return add(std::move(n));
    }

    // vars.nombre (despues de "vars").
    int varsField() {
        next();
        if (!(kind_ == Tok::Op && text_ == ".")) return fail("vars sin campo: escribe la variable por su nombre");
        next();
        if (kind_ != Tok::Name) return fail("falta el nombre de la variable despues de vars.");
        std::string name = text_;
        next();
        if (kind_ == Tok::Op && (text_ == "." || text_ == ":" || text_ == "(" || text_ == "[")) {
            return fail("vars." + name + text_ + "...: solo variables, numeros y comparaciones");
        }
        return variable(std::move(name));
    }

    int primary() {
        switch (kind_) {
            case Tok::Number: return constant(ExprValue::num(number_));
            case Tok::String: return constant(ExprValue::str(text_));
            case Tok::End: return fail("falta un valor al final");
            case Tok::Op:
                if (text_ == "(") {
                    next();
                    const int inner = parse(0);
                    if (!error_.empty()) return -1;
                    if (!(kind_ == Tok::Op && text_ == ")")) return fail("falta ')'");
                    next();
                    return inner;
                }
                if (text_ == "{" || text_ == "[" || text_ == "#") return fail("'" + text_ + "' (tablas de Lua) no se puede usar");
                return fail("falta un valor antes de '" + text_ + "'");
            case Tok::Name: break;
        }
        const std::string name = text_;
        if (name == "true") return constant(ExprValue::boolean(true));
        if (name == "false") return constant(ExprValue::boolean(false));
        if (name == "nil") return constant(ExprValue{});
        if (name == "and" || name == "or") return fail("falta un valor antes de '" + name + "'");
        if (luaKeyword(name)) return fail("'" + name + "' es de Lua (aqui solo va una expresion)");
        if (name == "vars") return varsField();
        if (name == "self") {
            // self.vars.x (como en el codigo de los estados); lo demas de self, no.
            next();
            if (kind_ == Tok::Op && text_ == ".") {
                next();
                if (kind_ == Tok::Name && text_ == "vars") return varsField();
            }
            return fail("self (Lua) no se puede usar: escribe la variable por su nombre");
        }
        if (name == "entity" || name == "sm" || name == "bt") {
            return fail(name + " (Lua) no se puede usar: calcula el valor en un script de C++ y guardalo en una variable");
        }
        next();
        if (kind_ == Tok::Op && text_ == "(") return fail("llamada a una funcion (" + name + "): solo variables, numeros y comparaciones");
        if (kind_ == Tok::Op && (text_ == "." || text_ == ":" || text_ == "[")) {
            return fail("campos de Lua (" + name + text_ + "...): solo variables, numeros y comparaciones");
        }
        return variable(name);
    }
};

Expression::Expression(std::string_view text) {
    ExpressionParser parser(text, *this);
    parser.run();
}

// --- Evaluacion ------------------------------------------------------------------

ExprValue Expression::eval(int index, const ExprLookup& lookup, bool& ok) const {
    const Node& n = nodes_[static_cast<std::size_t>(index)];
    switch (n.kind) {
        case Node::Kind::Constant: return n.value;
        case Node::Kind::Variable: return lookup ? lookup(n.name) : ExprValue{};
        case Node::Kind::Not: {
            const ExprValue v = eval(n.a, lookup, ok);
            return ok ? ExprValue::boolean(!v.truthy()) : ExprValue{};
        }
        case Node::Kind::Negate: {
            const ExprValue v = eval(n.a, lookup, ok);
            double x = 0.0;
            if (!ok || !toNumber(v, x)) {
                ok = false;
                return {};
            }
            return ExprValue::num(-x);
        }
        case Node::Kind::And: {
            ExprValue left = eval(n.a, lookup, ok);
            if (!ok || !left.truthy()) return left;
            return eval(n.b, lookup, ok);
        }
        case Node::Kind::Or: {
            ExprValue left = eval(n.a, lookup, ok);
            if (!ok || left.truthy()) return left;
            return eval(n.b, lookup, ok);
        }
        case Node::Kind::Binary: break;
    }
    const ExprValue a = eval(n.a, lookup, ok);
    if (!ok) return {};
    const ExprValue b = eval(n.b, lookup, ok);
    if (!ok) return {};
    switch (n.op) {
        case '=': return ExprValue::boolean(equal(a, b));
        case '!': return ExprValue::boolean(!equal(a, b));
        case '<':
        case 'l':
        case '>':
        case 'g': {
            int order = 0;
            if (a.type == ExprValue::Type::Number && b.type == ExprValue::Type::Number) {
                if (std::isnan(a.number) || std::isnan(b.number)) return ExprValue::boolean(false);
                order = a.number < b.number ? -1 : (a.number > b.number ? 1 : 0);
            } else if (a.type == ExprValue::Type::String && b.type == ExprValue::Type::String) {
                const int c = a.text.compare(b.text);
                order = c < 0 ? -1 : (c > 0 ? 1 : 0);
            } else {
                ok = false;  // Lua: "attempt to compare"
                return {};
            }
            if (n.op == '<') return ExprValue::boolean(order < 0);
            if (n.op == 'l') return ExprValue::boolean(order <= 0);
            if (n.op == '>') return ExprValue::boolean(order > 0);
            return ExprValue::boolean(order >= 0);
        }
        case 'c': {
            const auto text = [](const ExprValue& v, std::string& out) {
                if (v.type == ExprValue::Type::String) out = v.text;
                else if (v.type == ExprValue::Type::Number) out = numberText(v.number);
                else return false;
                return true;
            };
            std::string left, right;
            if (!text(a, left) || !text(b, right)) {
                ok = false;
                return {};
            }
            return ExprValue::str(left + right);
        }
        default: break;
    }
    double x = 0.0;
    double y = 0.0;
    if (!toNumber(a, x) || !toNumber(b, y)) {
        ok = false;  // Lua: "attempt to perform arithmetic"
        return {};
    }
    switch (n.op) {
        case '+': return ExprValue::num(x + y);
        case '-': return ExprValue::num(x - y);
        case '*': return ExprValue::num(x * y);
        case '/': return ExprValue::num(x / y);
        case 'f': return ExprValue::num(std::floor(x / y));
        case '%': {
            double m = std::fmod(x, y);
            if (m != 0.0 && ((m < 0.0) != (y < 0.0))) m += y;  // como Lua: el signo del divisor
            return ExprValue::num(m);
        }
        case '^': return ExprValue::num(std::pow(x, y));
        default: break;
    }
    ok = false;
    return {};
}

ExprValue Expression::evaluate(const ExprLookup& lookup, bool* ok) const {
    bool fine = valid();
    ExprValue v = fine ? eval(root_, lookup, fine) : ExprValue{};
    if (!fine) v = ExprValue{};
    if (ok != nullptr) *ok = fine;
    return v;
}

bool Expression::test(const ExprLookup& lookup) const {
    bool ok = false;
    const ExprValue v = evaluate(lookup, &ok);
    return ok && v.truthy();
}

bool Expression::test(const std::vector<Variable>& vars) const {
    if (!valid()) return false;
    return test([&vars](const std::string& name) {
        for (const Variable& v : vars) {
            if (v.name == name) return exprValueOf(v.value);
        }
        return ExprValue{};
    });
}

ExprValue exprValueOf(const Value& v) {
    switch (v.type) {
        case VarType::Bool: return ExprValue::boolean(v.b);
        case VarType::Int:
        case VarType::Float: return ExprValue::num(v.n);
        case VarType::String: return ExprValue::str(v.s);
        case VarType::Entity: return v.entity == kNoEntity ? ExprValue{} : ExprValue::object("#" + std::to_string(v.entity));
        case VarType::Vector: return ExprValue::object(v.text());
    }
    return {};
}

const Expression& ExpressionCache::get(const std::string& text) const {
    if (!expression_ || source_ != text) {
        expression_ = std::make_shared<const Expression>(text);
        source_ = text;
    }
    return *expression_;
}

}  // namespace cramion::ai
