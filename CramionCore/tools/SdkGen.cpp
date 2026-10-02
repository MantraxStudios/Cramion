// cramion_sdkgen: la API de Lua del motor, en C++, para los scripts de C++.
//
//   cramion_sdkgen <carpeta sdk/cramion>
//
// Lee la API real (ScriptSystem::apiReference: las tablas globales y los
// metodos de las entidades) y escribe:
//   Api.gen.h           namespace Audio { Value playOneShot(...); } ... (una
//                       funcion por cada funcion de Lua; los campos, como
//                       funciones sin argumentos)
//   EntityApi.gen.inc   los metodos de Lua de las entidades, dentro de
//                       cramion::Entity
// Solo reescribe si cambia (no fuerza a recompilar). Los nombres que ya da el
// SDK a mano (Script.h) se saltan, y las palabras reservadas de C++ llevan _.

#include <CramionCore/scripting/Scripting.h>

#include "LuaCompletion.h"  // la documentacion a mano de la API de Lua (CramionEditor/src)

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <vector>
#include <sstream>
#include <string>

using namespace cramion;

namespace {

const std::set<std::string> kKeywords = {
    "alignas", "alignof", "and", "asm", "auto", "bool", "break", "case", "catch", "char", "class", "const", "constexpr",
    "continue", "decltype", "default", "delete", "do", "double", "else", "enum", "explicit", "export", "extern", "false",
    "float", "for", "friend", "goto", "if", "inline", "int", "long", "mutable", "namespace", "new", "noexcept", "not",
    "nullptr", "operator", "or", "private", "protected", "public", "register", "requires", "return", "short", "signed",
    "sizeof", "static", "struct", "switch", "template", "this", "throw", "true", "try", "typedef", "typeid", "typename",
    "union", "unsigned", "using", "virtual", "void", "volatile", "while", "xor", "concept", "co_await", "co_return",
    "co_yield", "char8_t", "char16_t", "char32_t", "wchar_t", "near", "far", "small", "min", "max"};

// Nombres de tipos y espacios del SDK: una tabla de Lua con ese nombre no se
// genera (chocaria).
const std::set<std::string> kSkipTables = {"",       "Vec3",    "Quat",   "Vec2",    "Color",   "Entity",   "CVar",
                                           "CVars",  "Lua",     "Value",  "Values",  "Script",  "Property", "Range",
                                           "Model",  "Prefab",  "Material", "Texture", "Callback", "Collision",
                                           "Options", "Header", "Label",  "Tooltip", "Requires", "ForceMode", "Mathf",
                                           "string", "table",   "math",   "os",      "io",      "coroutine", "utf8",
                                           "debug",  "package", "Debug_"};

// Lo que el SDK ya tiene a mano en cada espacio (campos sin argumentos que chocarian).
const std::set<std::string> kSkipMembers = {"Time.deltaTime", "Time.time", "Time.fixedDeltaTime", "Time.frameCount",
                                            "Input.mouse",    "Input.key",  "Input.keyDown",       "Input.keyUp",
                                            "Input.axis",     "Input.mouseButton", "Input.keyMode",  "Debug.log",
                                            "Debug.warning",  "Debug.error",
                                            // Las del SDK escritas a mano (con tipos): la de Lua, con Lua::call.
                                            "Scene.find", "Scene.findWithTag", "Scene.create", "Scene.getPositions",
                                            "Scene.setPositions", "Physics.raycast"};

const std::set<std::string> kEntitySkip = {
    "id", "valid", "name", "setName", "tag", "position", "localPosition", "rotation", "scale", "forward", "right", "up",
    "setPosition", "setLocalPosition", "setRotation", "setScale", "translate", "rotate", "lookAt", "active", "setActive",
    "parent", "destroy", "has", "add", "component", "setComponent", "field", "setField", "getFloat", "getBool",
    "setFloat", "setBool", "velocity", "setVelocity", "angularVelocity", "setAngularVelocity", "addForce", "move",
    "isGrounded", "setMoveInput", "jump", "setCrouch", "call", "get", "set", "u32", "strU32", "str", "vec"};

bool identifier(const std::string& s) {
    if (s.empty() || std::isdigit(static_cast<unsigned char>(s[0]))) return false;
    for (const char c : s) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') return false;
    }
    return true;
}

std::string cppName(const std::string& lua) { return kKeywords.contains(lua) ? lua + "_" : lua; }

std::string trim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s;
}

// Los argumentos de la documentacion de Lua ("desde, hasta", "\"W\"", "i, Vec3")
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

// Una funcion de Lua como funcion de C++: con los nombres de sus argumentos y su
// descripcion (el IntelliSense la ensena). Todos los argumentos aceptan cualquier
// cosa que sea un Value y son opcionales (los nil del final no se envian).
//   prefix: "inline " (tablas) / "" (metodos); call: como se llama a Lua.
std::string function(const std::string& lua_name, const std::string& cpp_name, const std::string& doc_key, const std::string& prefix,
                     const std::string& returns, const std::string& call_head, const std::string& suffix,
                     const std::string& call_tail = ")") {
    std::string args, description;
    bool is_function = true;
    const bool documented = editor::luaApiDoc(doc_key, args, description, is_function);
    bool variadic = !documented || trim(args).empty();
    std::vector<std::string> names;
    if (documented && !trim(args).empty()) names = parameterNames(args, variadic);
    std::ostringstream o;
    o << "/// " << lua_name << "(" << (documented ? args : std::string("...")) << ")";
    if (documented && !description.empty()) o << "\n/// " << description;
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

// Un "metodo" de la API que en Lua es un campo (entity.text, mesh.vertices):
// la documentacion lo tiene como propiedad y no como funcion.
bool documentedField(const std::string& type, const std::string& name) {
    std::string args, description;
    bool is_function = true;
    if (editor::luaApiDoc(type + ":" + name, args, description, is_function) && is_function) return false;
    return editor::luaApiDoc(type + "." + name, args, description, is_function) && !is_function;
}

std::string fieldDoc(const std::string& key, const std::string& fallback) {
    std::string args, description;
    bool is_function = false;
    return editor::luaApiDoc(key, args, description, is_function) && !description.empty() ? description : fallback;
}

std::string capital(const std::string& s) {
    std::string out = s;
    if (!out.empty()) out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
    return out;
}

bool writeIfChanged(const std::filesystem::path& file, const std::string& text) {
    std::ifstream in(file, std::ios::binary);
    std::stringstream old;
    old << in.rdbuf();
    if (in && old.str() == text) return false;
    std::ofstream out(file, std::ios::binary);
    out << text;
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "Uso: cramion_sdkgen <carpeta sdk/cramion>\n");
        return 2;
    }
    const std::filesystem::path folder = argv[1];
    const auto api = scripting::ScriptSystem::apiReference();

    std::ostringstream h;
    h << "// GENERADO por cramion_sdkgen desde la API de Lua del motor: no lo edites.\n"
         "// Cada funcion de Lua (Tabla.funcion) es una funcion de C++ con los mismos\n"
         "// argumentos (Value: numeros, texto, Vec3, entidades, listas, objetos,\n"
         "// funciones...) que devuelve un Value. Ver la referencia de Lua en el manual.\n"
         "#pragma once\n\n"
         "namespace cramion {\n";
    int functions = 0, fields = 0;
    for (const auto& [owner, members] : api) {
        if (kSkipTables.contains(owner) || owner.back() == ':' || !identifier(owner)) continue;
        std::ostringstream body;
        int count = 0;
        for (const auto& m : members) {
            if (!identifier(m.name) || kSkipMembers.contains(owner + "." + m.name)) continue;
            const std::string name = cppName(m.name);
            if (m.function) {
                body << function(owner + "." + m.name, name, owner + "." + m.name, "inline ", "Value",
                                 "detail::lua(\"" + owner + "." + m.name + "\", ", "");
                ++functions;
            } else {
                body << "/// " << owner << "." << m.name << ": " << fieldDoc(owner + "." + m.name, "campo") << "\n"
                     << "inline Value " << name << "() { return detail::luaGet(\"" << owner << "." << m.name << "\"); }\n";
                ++fields;
            }
            ++count;
        }
        if (count == 0) continue;
        h << "\nnamespace " << owner << " {\n" << body.str() << "}  // namespace " << owner << "\n";
    }
    // Objetos de Lua con metodos (mallas, maquinas de estados): una clase con su handle.
    struct ObjectType {
        const char* lua;   // "Mesh:"
        const char* name;  // "Mesh"
        const char* doc;
        std::set<std::string> statics;
    };
    const ObjectType objects[] = {
        {"Mesh:", "Mesh", "Malla creada por codigo (Mesh::cube, Mesh::create, entity().mesh()). Cambiala y llama a apply().",
         {"new", "cube", "quad", "plane", "sphere", "cylinder", "capsule", "wireCube"}},
        {"StateMachine:", "StateMachine", "Maquina de estados de un objeto (entity().getStateMachine()): go, trigger, set/get...",
         {"new", "of"}},
        {"BehaviorTree:", "BehaviorTree",
         "Behavior Tree de un objeto (entity().getBehaviorTree()): get/set de la pizarra, start/stop, finishTask...",
         {"of", "registerTask", "reportNoise", "broadcast"}},
    };
    for (const ObjectType& type : objects) {
        const auto it = api.find(type.lua);
        if (it == api.end()) continue;
        const std::string tname = type.name;
        h << "\n/// " << type.doc << "\nclass " << tname << " {\npublic:\n    Value handle;\n    " << tname << "() = default;\n    "
          << tname << "(Value v) : handle(std::move(v)) {}\n    operator Value() const { return handle; }\n"
          << "    explicit operator bool() const { return handle.truthy(); }\n";
        std::set<std::string> done;
        for (const auto& m : it->second) {
            if (!identifier(m.name) || done.contains(m.name)) continue;
            done.insert(m.name);
            std::string name = cppName(m.name);
            if (name == "new_") name = "create";
            if (name == "handle") continue;
            std::string body;
            if (type.statics.contains(m.name)) {
                body = function(tname + "." + m.name, name, tname + "." + m.name, "static ", tname,
                                tname + "(detail::lua(\"" + tname + "." + m.name + "\", ", "", "))");
            } else if (m.function && !documentedField(tname, m.name)) {
                body = function(std::string(type.lua) + m.name, name, std::string(type.lua) + m.name, "", "Value",
                                "handle.call(\"" + m.name + "\", ", " const");
            } else {
                body = "/// " + tname + "." + m.name + ": " + fieldDoc(tname + "." + m.name, "campo") + "\nValue " + name +
                       "() const { return handle.get(\"" + m.name + "\"); }\n";
                const std::string setter = "set" + capital(m.name);
                if (!done.contains(setter)) {
                    body += "/// Cambia " + tname + "." + m.name + "\nvoid " + setter + "(const Value& v) const { handle.setField(\"" +
                            m.name + "\", v); }\n";
                }
            }
            std::istringstream lines(body);
            for (std::string line; std::getline(lines, line);) h << "    " << line << "\n";
            ++functions;
        }
        h << "};\n";
    }
    h << "\n}  // namespace cramion\n";

    std::ostringstream e;
    e << "// GENERADO por cramion_sdkgen: los metodos de Lua de las entidades (entity:metodo(...)).\n"
         "// Se incluye dentro de cramion::Entity.\n";
    int methods = 0;
    std::set<std::string> taken = kEntitySkip;
    if (const auto it = api.find("Entity:"); it != api.end()) {
        for (const auto& m : it->second) {
            if (!identifier(m.name) || taken.contains(m.name)) continue;
            const std::string name = cppName(m.name);
            taken.insert(m.name);
            if (m.function && !documentedField("Entity", m.name)) {
                e << function("entity:" + m.name, name, "Entity:" + m.name, "", "Value", "call(\"" + m.name + "\", ", " const");
            } else {
                e << "/// entity." << m.name << ": " << fieldDoc("Entity." + m.name, "campo") << "\n"
                  << "Value " << name << "() const { return get(\"" << m.name << "\"); }\n";
                const std::string setter = "set" + capital(m.name);
                if (!taken.contains(setter)) {
                    e << "void " << setter << "(const Value& v) const { set(\"" << m.name << "\", v); }\n";
                    taken.insert(setter);
                }
            }
            ++methods;
        }
    }
    const bool a = writeIfChanged(folder / "Api.gen.h", h.str());
    const bool b = writeIfChanged(folder / "EntityApi.gen.inc", e.str());
    std::printf("SDK de C++: %d funciones, %d campos, %d de entidad%s\n", functions, fields, methods,
                a || b ? " (actualizado)" : " (sin cambios)");
    return 0;
}
