#ifndef CRAMION_VK_VULKAN_SHADER_H
#define CRAMION_VK_VULKAN_SHADER_H

#include "vk/VulkanCommon.h"

#include <filesystem>
#include <string>

namespace cramion::gfx {

class VulkanDevice;

// Carga de modulos SPIR-V ya compilados.
//
// Los .spv los genera CMake con glslc a partir de shaders/*.vert|frag y los
// deja en la carpeta "shaders" junto al ejecutable.
namespace shaders {

// Carpeta de shaders junto al ejecutable, resuelta en tiempo de ejecucion.
std::filesystem::path directory();

// Carga <directory()>/<file_name> como modulo de shader.
vk::raii::ShaderModule loadModule(const VulkanDevice& device, const std::string& file_name);

}  // namespace shaders

}  // namespace cramion::gfx

#endif  // CRAMION_VK_VULKAN_SHADER_H
