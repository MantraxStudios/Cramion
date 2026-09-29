#ifndef CRAMION_EDITOR_LUA_COMPLETION_H
#define CRAMION_EDITOR_LUA_COMPLETION_H

// Autocompletado (IntelliSense) de los scripts Lua del editor:
//
//   - La API completa del motor: se la da el propio motor al abrir el editor
//     (setLuaApiReference con ScriptSystem::apiReference), asi que sale todo
//     lo que existe (Input, XR, Screen, Graphics, Network...), con la firma y
//     la descripcion de lo que esta documentado aqui.
//   - Tipos: sabe que `self.entity`, `other` o `Scene.find(...)` son una
//     entidad, que `.position` o `Vec3(...)` es un Vec3, `Quat.euler(...)` un
//     Quat, `Physics.raycast(...)` un choque... y lo sigue por las variables
//     del archivo (`local p = self.entity.position` -> `p.` da x, y, z).
//   - Dentro de un texto, lo del proyecto: componentes y sus campos
//     (addComponent, getField/setField), acciones y contextos de entrada,
//     teclas, tags, objetos de la escena, escenas, prefabs, sonidos,
//     materiales, manos y botones de VR...
//   - Palabras clave, variables, funciones y propiedades del propio archivo y
//     los metodos que llama el motor (Start, Update...).
//   - La firma de la funcion que se esta llamando (luaSignatureAt).

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace cramion::editor {

struct LuaCompletion {
    std::string label;   // lo que se ve (y se filtra)
    std::string insert;  // lo que se escribe (puede llevar parentesis)
    std::string detail;  // firma / descripcion
    int kind = 0;        // 0 palabra clave, 1 tabla/global, 2 funcion, 3 propiedad, 4 local, 5 metodo del motor, 6 valor
};

struct LuaCompletionContext {
    std::size_t prefix_start = 0;  // donde empieza lo que se esta escribiendo
    std::string prefix;
    std::string receiver;          // "Input", "self.entity", "Scene.find('x')"... (vacio = global)
    char accessor = 0;             // '.', ':' o 0
    bool after_function = false;   // "function Clase:" -> metodos del motor
    // Dentro de un texto que es argumento de una llamada: "Input.getKey" o
    // "Entity:getField" (los metodos van con el tipo), el argumento (desde 0)
    // y el texto del primero (el componente de getField).
    bool in_string = false;
    std::string call;
    int argument = 0;
    std::string first_argument;
};

// La API del motor tal como la ve Lua: "Input" -> sus miembros, "Entity:"
// -> los de una entidad, "" -> funciones globales...
struct LuaApiMember {
    std::string name;
    bool function = false;
};
void setLuaApiReference(const std::map<std::string, std::vector<LuaApiMember>>& reference);

// Lo del proyecto abierto, para completar dentro de los textos.
struct LuaProjectSymbols {
    struct Component {
        std::string name;   // "Light"
        std::string label;  // "Luz"
        std::vector<std::pair<std::string, std::string>> fields;  // clave, descripcion
    };
    std::vector<Component> components;
    std::vector<std::string> actions, contexts, input_sources, keys, tags, entities, scenes, prefabs, audio, materials,
        textures, scripts;
};
void setLuaProjectSymbols(LuaProjectSymbols symbols);

// Que se esta escribiendo en `cursor` (false si no hay nada que completar).
bool luaCompletionContext(const std::string& text, std::size_t cursor, LuaCompletionContext& context);

// Sugerencias para ese contexto, ordenadas (primero las que empiezan igual).
std::vector<LuaCompletion> luaCompletions(const std::string& text, const LuaCompletionContext& context);

// La llamada en la que esta el cursor: su firma ("Input.getAction(nombre)"),
// la descripcion y que argumento se esta escribiendo (desde 0).
struct LuaSignature {
    std::string label;
    std::string detail;
    int argument = 0;
};
bool luaSignatureAt(const std::string& text, std::size_t cursor, LuaSignature& signature);

// Tipo de una expresion ("Entity", "Vec3", "Quat", "Mesh", "hit"...; vacio
// si no se sabe). Para las pruebas.
std::string luaExpressionType(const std::string& text, const std::string& expression);

// Tablas y funciones globales del motor (para el resaltado).
bool luaIsApiName(std::string_view word);

}  // namespace cramion::editor

#endif  // CRAMION_EDITOR_LUA_COMPLETION_H
