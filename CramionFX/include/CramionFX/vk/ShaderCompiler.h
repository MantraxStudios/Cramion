#ifndef CRAMION_VK_SHADER_COMPILER_H
#define CRAMION_VK_SHADER_COMPILER_H

// Compilacion de GLSL a SPIR-V mientras el motor corre (shaders de superficie
// del usuario, .crshader). Usa shaderc_shared.dll (Vulkan SDK), que se carga
// la primera vez que hace falta: si no esta junto al ejecutable, compile()
// devuelve false con el motivo y el resto del motor sigue igual.

#include <cstdint>
#include <string>
#include <vector>

namespace cramion::gfx::shaders {

enum class Stage { Vertex, Fragment };

// `name` sale en los errores (archivo:linea: error). Optimiza para
// rendimiento y apunta a Vulkan 1.3, como los shaders del motor.
bool compile(const std::string& source, Stage stage, const std::string& name, std::vector<std::uint32_t>& spirv,
             std::string& error);

// Si se puede compilar (la DLL esta y carga).
bool compilerAvailable();

}  // namespace cramion::gfx::shaders

#endif  // CRAMION_VK_SHADER_COMPILER_H
