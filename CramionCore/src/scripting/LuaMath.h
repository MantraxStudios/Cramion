#ifndef CRAMION_CORE_SCRIPTING_LUA_MATH_H
#define CRAMION_CORE_SCRIPTING_LUA_MATH_H

// Libreria matematica de Lua (LuaMath.cpp): Vec3, Quat, Mathf, Random y ruido.

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

namespace cramion::scripting {

void bindMath(sol::state& L);

}  // namespace cramion::scripting

#endif  // CRAMION_CORE_SCRIPTING_LUA_MATH_H
