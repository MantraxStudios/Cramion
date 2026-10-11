// cramion_sdkgen: la API del motor, en C++, para los scripts de C++.
//
//   cramion_sdkgen <carpeta sdk/cramion>
//
// Lee el registro de la API (ScriptSystem::apiRegistry: cada funcion, metodo
// y propiedad que registran los modulos de src/scripting/native, con su
// documentacion api::Doc {args, descripcion, devuelve}) y escribe:
//   Api.gen.h           namespace Audio { Value playOneShot(...); } ... (una
//                       funcion por cada funcion de la API; las propiedades
//                       de las tablas, como funciones sin argumentos y setX)
//                       y los objetos del motor con metodos (Mesh...) como
//                       clases con su handle (un Value)
//   EntityApi.gen.inc   los metodos y propiedades de Entity, dentro de
//                       cramion::Entity
// Solo reescribe si cambia (no fuerza a recompilar). Los nombres que ya da el
// SDK a mano (Script.h, Test.h) se saltan, y las palabras reservadas de C++
// llevan _. Al regenerar escribe en stderr los simbolos que estaban en el
// archivo anterior y ya no estan (un script que los use dejaria de compilar).

#include <CramionCore/scripting/NativeApi.h>
#include <CramionCore/scripting/Scripting.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace cramion;
namespace api = cramion::scripting::api;

namespace {

const std::set<std::string> kKeywords = {
    "alignas", "alignof", "and", "asm", "auto", "bool", "break", "case", "catch", "char", "class", "const", "constexpr",
    "continue", "decltype", "default", "delete", "do", "double", "else", "enum", "explicit", "export", "extern", "false",
    "float", "for", "friend", "goto", "if", "inline", "int", "long", "mutable", "namespace", "new", "noexcept", "not",
    "nullptr", "operator", "or", "private", "protected", "public", "register", "requires", "return", "short", "signed",
    "sizeof", "static", "struct", "switch", "template", "this", "throw", "true", "try", "typedef", "typeid", "typename",
    "union", "unsigned", "using", "virtual", "void", "volatile", "while", "xor", "concept", "co_await", "co_return",
    "co_yield", "char8_t", "char16_t", "char32_t", "wchar_t", "near", "far", "small", "min", "max"};

// Nombres de tipos y espacios del SDK: una tabla con ese nombre no se genera
// (chocaria). Assert y Api los da el SDK a mano (Test.h, Script.h).
const std::set<std::string> kSkipTables = {"",       "Vec3",    "Quat",   "Vec2",    "Color",   "Entity",   "CVar",
                                           "CVars",  "Lua",     "Api",    "Assert",  "Value",   "Values",   "Script",
                                           "Property", "Range", "Model",  "Prefab",  "Material", "Texture", "Callback",
                                           "Collision", "Options", "Header", "Label", "Tooltip", "Requires", "ForceMode",
                                           "Mathf",  "string",  "table",  "math",    "os",      "io",       "coroutine",
                                           "utf8",   "debug",   "package", "Debug_", "detail",  "cppproto", "std"};

// Lo que el SDK ya tiene a mano en cada espacio (chocarian).
const std::set<std::string> kSkipMembers = {"Time.deltaTime", "Time.time", "Time.fixedDeltaTime", "Time.frameCount",
                                            "Input.mouse",    "Input.key",  "Input.keyDown",       "Input.keyUp",
                                            "Input.axis",     "Input.mouseButton", "Input.keyMode",  "Debug.log",
                                            "Debug.warning",  "Debug.error",
                                            // Las del SDK escritas a mano (con tipos): la de la API, con Api::call.
                                            "Scene.find", "Scene.findWithTag", "Scene.create", "Scene.getPositions",
                                            "Scene.setPositions", "Physics.raycast",
                                            // Las esperas de las pruebas son corrutinas (Test.h).
                                            "Test.wait", "Test.waitFrames", "Test.waitSeconds", "Test.waitUntil",
                                            "Test.Task", "Test.detail"};

const std::set<std::string> kEntitySkip = {
    "id", "valid", "name", "setName", "tag", "position", "localPosition", "rotation", "scale", "forward", "right", "up",
    "setPosition", "setLocalPosition", "setRotation", "setScale", "translate", "rotate", "lookAt", "active", "setActive",
    "parent", "destroy", "has", "add", "component", "setComponent", "field", "setField", "getFloat", "getBool",
    "setFloat", "setBool", "velocity", "setVelocity", "angularVelocity", "setAngularVelocity", "addForce", "move",
    "isGrounded", "setMoveInput", "jump", "setCrouch", "call", "get", "set", "u32", "strU32", "str", "vec", "script"};

// Objetos del motor (handles) con su descripcion; los demas tipos llevan una generica.
const std::vector<std::pair<std::string, std::string>> kTypeDocs = {
    {"Mesh", "Malla creada por codigo (Mesh::cube, Mesh::create, entity().mesh()). Cambiala y llama a apply()."},
    {"StateMachine", "Maquina de estados de un objeto (entity().getStateMachine()): go, trigger, set/get..."},
    {"BehaviorTree", "Behavior Tree de un objeto (entity().getBehaviorTree()): get/set de la pizarra, start/stop, finishTask..."},
};

bool identifier(const std::string& s) {
    if (s.empty() || std::isdigit(static_cast<unsigned char>(s[0]))) return false;
    for (const char c : s) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') return false;
    }
    return true;
}

std::string cppName(const std::string& name) { return kKeywords.contains(name) ? name + "_" : name; }

std::string trim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s;
}

bool documented(const api::Doc& d) { return !d.args.empty() || !d.description.empty() || !d.returns.empty(); }

// Los argumentos de la documentacion ("desde, hasta", "\"W\"", "i, Vec3")
// como nombres de parametros de C++. variadic: termina en "..." (o no se sabe).
std::vector<std::string> parameterNames(const std::string& args, bool& variadic) {
    std::vector<std::string> out;
    variadic = false;
    std::vector<std::string> parts;
    int depth = 0;
    bool quote = false;
    std::string cur;
    for (const char c : args) {
        if (c == '"') quote = !quote;
        if (!quote && (c == '(' || c == '{' || c == '[')) ++depth;
        if (!quote && (c == ')' || c == '}' || c == ']')) --depth;
        if (c == ',' && depth == 0 && !quote) {
            parts.push_back(trim(cur));
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!trim(cur).empty()) parts.push_back(trim(cur));
    std::set<std::string> used;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        std::string p = parts[i];
        if (p == "..." || p.rfind("...", 0) == 0) {
            variadic = true;
            break;
        }
        // "nombre = valor" o "nombre: tipo": el nombre.
        if (const std::size_t eq = p.find_first_of("=:"); eq != std::string::npos && p.find('"') == std::string::npos) p = trim(p.substr(0, eq));
        std::string name;
        bool ok = !p.empty() && (std::isalpha(static_cast<unsigned char>(p[0])) || p[0] == '_') && p != "true" && p != "false" &&
                  p != "nil";
        for (const char c : p) {
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') ok = false;
        }
        if (ok) {
            name = p;
            if (name == "Vec3" || name == "Quat") name = name == "Vec3" ? "vec" : "rot";
            if (std::isupper(static_cast<unsigned char>(name[0]))) name[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(name[0])));
        } else if (std::regex_match(p, std::regex(R"([A-Za-z]+( [A-Za-z]+)+)"))) {
            // Varias palabras: "freno de mano" -> frenoDeMano; "Vec3 grados" -> grados.
            std::istringstream words(p);
            std::vector<std::string> list;
            for (std::string w; words >> w;) list.push_back(w);
            static const std::set<std::string> types = {"Vec3", "Quat", "Entity", "Vec2", "Color", "numero", "texto", "tabla"};
            if (types.contains(list.front())) {
                name = list.back();
            } else {
                for (std::size_t k = 0; k < list.size(); ++k) {
                    std::string w = list[k];
                    for (char& ch : w) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                    if (k > 0) w[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(w[0])));
                    name += w;
                }
            }
        } else if (p.rfind("function", 0) == 0) {
            name = "callback";
        } else if (!p.empty() && p[0] == '"') {
            name = "texto";
        } else if (!p.empty() && p[0] == '{') {
            name = "tabla";
        } else if (p == "true" || p == "false") {
            name = "activar";
        } else if (!p.empty() && (std::isdigit(static_cast<unsigned char>(p[0])) || p[0] == '-' || p[0] == '.')) {
            name = "valor";
        } else {
            name = "arg";
        }
        name = cppName(name);
        std::string unique = name;
        for (int k = 2; used.contains(unique); ++k) unique = name + std::to_string(k);
        used.insert(unique);
        out.push_back(unique);
    }
    return out;
}

// Una funcion de la API como funcion de C++: con los nombres de sus argumentos
// y su descripcion (el IntelliSense la ensena). Todos los argumentos aceptan
// cualquier cosa que sea un Value y son opcionales (los nil del final no se envian).
//   prefix: "inline " (tablas) / "static " / "" (metodos); call_head: como se llama a la API.
std::string function(const std::string& shown, const std::string& cpp_name, const api::Doc& doc, const std::string& prefix,
                     const std::string& returns, const std::string& call_head, const std::string& suffix,
                     const std::string& call_tail = ")") {
    const bool known = documented(doc);
    bool variadic = !known || trim(doc.args).empty();
    std::vector<std::string> names;
    if (known && !trim(doc.args).empty()) names = parameterNames(doc.args, variadic);
    std::ostringstream o;
    o << "/// " << shown << "(" << (known ? doc.args : std::string("...")) << ")";
    if (!doc.description.empty()) o << "\n/// " << doc.description;
    if (!doc.returns.empty()) o << "\n/// Devuelve: " << doc.returns;
    o << "\n";
    std::ostringstream tpl, params, values;
    for (std::size_t i = 0; i < names.size(); ++i) {
        tpl << (i ? ", " : "") << "typename T" << i << " = Value";
        params << (i ? ", " : "") << "const T" << i << "& " << names[i] << " = {}";
        values << (i ? ", " : "") << "Value(" << names[i] << ")";
    }
    if (variadic) {
        tpl << (names.empty() ? "" : ", ") << "typename... Mas";
        params << (names.empty() ? "" : ", ") << "Mas&&... mas";
        values << (names.empty() ? "" : ", ") << "Value(std::forward<Mas>(mas))...";
    }
    if (!tpl.str().empty()) o << "template <" << tpl.str() << ">\n";
    o << prefix << returns << " " << cpp_name << "(" << params.str() << ")" << suffix << " {\n    return " << call_head << "Values{" << values.str()
      << "}" << call_tail << ";\n}\n";
    return o.str();
}

// "/// Tabla.x: descripcion (tipo)" de una propiedad.
std::string propertyDoc(const std::string& shown, const api::Doc& doc) {
    std::string text = "/// " + shown + ": " + (doc.description.empty() ? std::string("propiedad") : doc.description);
    if (!doc.returns.empty()) text += " (" + doc.returns + ")";
    return text + "\n";
}

std::string capital(const std::string& s) {
    std::string out = s;
    if (!out.empty()) out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
    return out;
}

std::string indent(const std::string& body) {
    std::ostringstream out;
    std::istringstream lines(body);
    for (std::string line; std::getline(lines, line);) out << "    " << line << "\n";
    return out.str();
}

std::string readFile(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

bool writeIfChanged(const std::filesystem::path& file, const std::string& old, const std::string& text) {
    if (old == text && std::filesystem::exists(file)) return false;
    std::ofstream out(file, std::ios::binary);
    out << text;
    return true;
}

// Los simbolos de un archivo generado ("Audio::playOneShot", "Mesh::apply",
// "Entity::playSound"): los ambitos son "namespace X {" y "class X {".
std::set<std::string> symbols(const std::string& text, const std::string& outer) {
    static const std::regex scope_open(R"(^\s*(?:namespace|class) ([A-Za-z_]\w*) \{)");
    static const std::regex declaration(R"(^\s*(?:inline |static )?([A-Za-z_][\w:]*) ([A-Za-z_]\w*)\()");
    std::set<std::string> out;
    std::vector<std::string> scopes;
    if (!outer.empty()) scopes.push_back(outer);
    std::istringstream lines(text);
    for (std::string line; std::getline(lines, line);) {
        std::smatch m;
        if (std::regex_search(line, m, scope_open)) {
            if (m[1] != "cramion") scopes.push_back(m[1]);
            continue;
        }
        if ((line.rfind("}  // namespace ", 0) == 0 && line != "}  // namespace cramion") || line == "};") {
            if (!scopes.empty() && (scopes.size() > 1 || outer.empty())) scopes.pop_back();
            continue;
        }
        if (line.find("///") != std::string::npos || !std::regex_search(line, m, declaration)) continue;
        const std::string type = m[1];
        if (type == "return" || type == "operator" || type == "explicit" || type == "template") continue;
        out.insert((scopes.empty() ? std::string() : scopes.back() + "::") + std::string(m[2]));
    }
    return out;
}

// Lo que ya no esta (al regenerar): a stderr. Devuelve cuantos.
int reportMissing(const std::string& file, const std::string& old, const std::string& now, const std::string& outer) {
    if (old.empty()) return 0;
    const std::set<std::string> before = symbols(old, outer);
    const std::set<std::string> after = symbols(now, outer);
    int missing = 0;
    for (const std::string& s : before) {
        if (after.contains(s)) continue;
        std::fprintf(stderr, "cramion_sdkgen: %s ya no esta en %s\n", s.c_str(), file.c_str());
        ++missing;
    }
    return missing;
}

using Members = std::vector<const api::Entry*>;

// Por nombre y sin repetir (el primero registrado gana).
void sortMembers(Members& list) {
    std::stable_sort(list.begin(), list.end(), [](const api::Entry* a, const api::Entry* b) { return a->name < b->name; });
    list.erase(std::unique(list.begin(), list.end(), [](const api::Entry* a, const api::Entry* b) { return a->name == b->name; }),
               list.end());
}

bool hasMember(const Members& list, const std::string& name) {
    return std::any_of(list.begin(), list.end(), [&](const api::Entry* e) { return e->name == name; });
}

// Una tabla ("Audio"): sus funciones y propiedades dentro de su namespace.
std::string tableBody(const std::string& owner, const Members& members, int& functions, int& fields) {
    std::ostringstream body;
    for (const api::Entry* e : members) {
        const std::string key = owner + "." + e->name;
        if (!identifier(e->name) || kSkipMembers.contains(key)) continue;
        const std::string name = cppName(e->name);
        if (e->kind == api::Entry::Kind::Function) {
            body << function(key, name, e->doc, "inline ", "Value", "detail::api(\"" + key + "\", ", "");
            ++functions;
            continue;
        }
        body << propertyDoc(key, e->doc) << "inline Value " << name << "() { return detail::apiGet(\"" << key << "\"); }\n";
        const std::string setter = "set" + capital(e->name);
        if (e->assign && !hasMember(members, setter) && !kSkipMembers.contains(owner + "." + setter)) {
            body << "/// Cambia " << key << "\ninline void " << setter << "(const Value& v) { detail::apiSet(\"" << key << "\", v); }\n";
        }
        ++fields;
    }
    return body.str();
}

// Un objeto del motor (handle) como clase: sus metodos y propiedades, y las
// funciones de la tabla con su nombre (Mesh.cube) como estaticas.
std::string typeClass(const std::string& tname, const std::string& doc, const Members& members, const Members& statics,
                      int& functions) {
    std::ostringstream h;
    h << "\n/// " << doc << "\nclass " << tname << " {\npublic:\n    Value handle;\n    " << tname << "() = default;\n    " << tname
      << "(Value v) : handle(std::move(v)) {}\n    operator Value() const { return handle; }\n"
      << "    explicit operator bool() const { return handle.truthy(); }\n";
    // Todo junto y por nombre (como antes: estaticas, metodos y propiedades mezclados).
    struct Item {
        const api::Entry* e;
        bool is_static;
    };
    std::vector<Item> items;
    for (const api::Entry* e : statics) items.push_back({e, true});
    for (const api::Entry* e : members) items.push_back({e, false});
    std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.e->name < b.e->name; });
    std::set<std::string> done, all;
    for (const Item& it : items) all.insert(it.e->name);
    for (const Item& it : items) {
        const api::Entry& e = *it.e;
        if (!identifier(e.name) || done.contains(e.name)) continue;
        done.insert(e.name);
        std::string name = cppName(e.name);
        if (name == "new_") name = "create";
        if (name == "handle" || name == tname) continue;
        std::string body;
        if (it.is_static && e.kind == api::Entry::Kind::Function) {
            // Las que crean o buscan el objeto devuelven la clase (Mesh::cube); las demas, un Value.
            const bool same = e.doc.returns.empty() || e.doc.returns.rfind(tname, 0) == 0;
            const std::string key = tname + "." + e.name;
            body = same ? function(key, name, e.doc, "static ", tname, tname + "(detail::api(\"" + key + "\", ", "", "))")
                        : function(key, name, e.doc, "static ", "Value", "detail::api(\"" + key + "\", ", "");
        } else if (it.is_static) {
            const std::string key = tname + "." + e.name;
            body = propertyDoc(key, e.doc) + "static Value " + name + "() { return detail::apiGet(\"" + key + "\"); }\n";
        } else if (e.kind == api::Entry::Kind::Method) {
            body = function(tname + ":" + e.name, name, e.doc, "", "Value", "handle.call(\"" + e.name + "\", ", " const");
        } else {
            body = propertyDoc(tname + "." + e.name, e.doc) + "Value " + name + "() const { return handle.get(\"" + e.name + "\"); }\n";
            const std::string setter = "set" + capital(e.name);
            if (e.assign && !all.contains(setter)) {
                body += "/// Cambia " + tname + "." + e.name + "\nvoid " + setter + "(const Value& v) const { handle.setField(\"" + e.name +
                        "\", v); }\n";
            }
        }
        h << indent(body);
        ++functions;
    }
    h << "};\n";
    return h.str();
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "Uso: cramion_sdkgen <carpeta sdk/cramion>\n");
        return 2;
    }
    const std::filesystem::path folder = argv[1];
    const api::NativeApi& registry = scripting::ScriptSystem::apiRegistry();

    // Tablas ("Audio": funciones y propiedades) y tipos ("Entity", "Mesh": metodos y propiedades).
    std::map<std::string, Members> tables, types;
    for (const api::Entry& e : registry.entries()) (e.member() ? types : tables)[e.owner].push_back(&e);
    for (auto& [owner, list] : tables) sortMembers(list);
    for (auto& [owner, list] : types) sortMembers(list);

    // Los tipos con clase propia (todos menos Entity y los nombres del SDK).
    std::vector<std::string> classes;
    for (const auto& [name, doc] : kTypeDocs) {
        if (types.contains(name)) classes.push_back(name);
    }
    for (const auto& [owner, list] : types) {
        if (owner == "Entity" || !identifier(owner) || kSkipTables.contains(owner)) continue;
        if (std::find(classes.begin(), classes.end(), owner) == classes.end()) classes.push_back(owner);
    }

    std::ostringstream h;
    h << "// GENERADO por cramion_sdkgen desde el registro de la API del motor: no lo edites.\n"
         "// Cada funcion de la API (Tabla.funcion) es una funcion de C++ con los mismos\n"
         "// argumentos (Value: numeros, texto, Vec3, entidades, listas, objetos,\n"
         "// funciones...) que devuelve un Value. Ver la referencia de la API en el manual.\n"
         "#pragma once\n\n"
         "namespace cramion {\n";
    int functions = 0, fields = 0;
    for (const auto& [owner, members] : tables) {
        if (kSkipTables.contains(owner) || !identifier(owner)) continue;
        if (std::find(classes.begin(), classes.end(), owner) != classes.end()) continue;  // estaticas de su clase
        const std::string body = tableBody(owner, members, functions, fields);
        if (body.empty()) continue;
        h << "\nnamespace " << owner << " {\n" << body << "}  // namespace " << owner << "\n";
    }
    for (const std::string& tname : classes) {
        std::string doc = "Objeto del motor (" + tname + "): sus metodos y propiedades, sobre su handle.";
        for (const auto& [name, text] : kTypeDocs) {
            if (name == tname) doc = text;
        }
        const auto statics = tables.find(tname);
        h << typeClass(tname, doc, types[tname], statics != tables.end() ? statics->second : Members{}, functions);
    }
    h << "\n}  // namespace cramion\n";

    std::ostringstream e;
    e << "// GENERADO por cramion_sdkgen: los metodos y propiedades de la API de las entidades\n"
         "// (Entity:metodo, Entity.propiedad). Se incluye dentro de cramion::Entity.\n";
    int methods = 0;
    std::set<std::string> taken = kEntitySkip;
    const Members& entity = types["Entity"];
    for (const api::Entry* m : entity) {
        if (!identifier(m->name) || taken.contains(m->name)) continue;
        const std::string name = cppName(m->name);
        taken.insert(m->name);
        if (m->kind == api::Entry::Kind::Method) {
            e << function("entity:" + m->name, name, m->doc, "", "Value", "call(\"" + m->name + "\", ", " const");
        } else {
            e << propertyDoc("entity." + m->name, m->doc) << "Value " << name << "() const { return get(\"" << m->name << "\"); }\n";
            const std::string setter = "set" + capital(m->name);
            if (m->assign && !taken.contains(setter) && !hasMember(entity, setter)) {
                e << "void " << setter << "(const Value& v) const { set(\"" << m->name << "\", v); }\n";
                taken.insert(setter);
            }
        }
        ++methods;
    }

    const std::string old_h = readFile(folder / "Api.gen.h");
    const std::string old_e = readFile(folder / "EntityApi.gen.inc");
    const int missing = reportMissing("Api.gen.h", old_h, h.str(), "") + reportMissing("EntityApi.gen.inc", old_e, e.str(), "Entity");
    const bool a = writeIfChanged(folder / "Api.gen.h", old_h, h.str());
    const bool b = writeIfChanged(folder / "EntityApi.gen.inc", old_e, e.str());
    std::printf("SDK de C++: %d funciones, %d propiedades, %d de entidad%s", functions, fields, methods,
                a || b ? " (actualizado)" : " (sin cambios)");
    if (missing > 0) std::printf(", %d simbolos ya no estan (ver stderr)", missing);
    std::printf("\n");
    return 0;
}
