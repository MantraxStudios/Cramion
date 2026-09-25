#ifndef CRAMION_EDITOR_LUA_COMPLETION_H
#define CRAMION_EDITOR_LUA_COMPLETION_H

// Autocompletado (IntelliSense) de los scripts Lua del editor: palabras clave,
// la API del motor (Vec3, Entity, Scene, Input, Time, Physics, Audio, Debug,
// Mathf), los miembros segun lo que hay antes del punto o los dos puntos
// (self., self.entity:, Input., Vec3.), las propiedades y variables del
// propio archivo y los metodos que llama el motor (Start, Update...), con su
// firma y descripcion.

#include <string>
#include <vector>

namespace cramion::editor {

struct LuaCompletion {
    std::string label;   // lo que se ve (y se filtra)
    std::string insert;  // lo que se escribe (puede llevar parentesis)
    std::string detail;  // firma / descripcion
    int kind = 0;        // 0 palabra clave, 1 tabla/global, 2 funcion, 3 propiedad, 4 local, 5 metodo del motor
};

struct LuaCompletionContext {
    std::size_t prefix_start = 0;  // donde empieza lo que se esta escribiendo
    std::string prefix;
    std::string receiver;          // "Input", "self.entity"... (vacio = global)
    char accessor = 0;             // '.', ':' o 0
    bool after_function = false;   // "function Clase:" -> metodos del motor
};

// Que se esta escribiendo en `cursor` (0 si no hay nada que completar).
bool luaCompletionContext(const std::string& text, std::size_t cursor, LuaCompletionContext& context);

// Sugerencias para ese contexto, ordenadas (primero las que empiezan igual).
std::vector<LuaCompletion> luaCompletions(const std::string& text, const LuaCompletionContext& context);

}  // namespace cramion::editor

#endif  // CRAMION_EDITOR_LUA_COMPLETION_H
