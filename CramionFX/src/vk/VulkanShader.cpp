#include "CramionFX/vk/VulkanShader.h"

#include "CramionFX/vk/VulkanDevice.h"

#include <windows.h>

#include <fstream>
#include <stdexcept>
#include <vector>

namespace cramion::gfx::shaders {

std::filesystem::path directory() {
    // Se resuelve desde la ruta del ejecutable para que funcione sea cual sea
    // el directorio de trabajo desde el que se lance.
    std::wstring buffer(MAX_PATH, L'\0');
    DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));

    while (length == buffer.size()) {  // Ruta mas larga que el buffer: ampliar.
        buffer.resize(buffer.size() * 2);
        length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    }

    if (length == 0) {
        throw std::runtime_error("No se pudo obtener la ruta del ejecutable.");
    }

    buffer.resize(length);
    return std::filesystem::path(buffer).parent_path() / "shaders";
}

vk::raii::ShaderModule loadModule(const VulkanDevice& device, const std::string& file_name) {
    const std::filesystem::path path = directory() / file_name;

    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        throw std::runtime_error("No se pudo abrir el shader: " + path.string() +
                                 "\nCompila el objetivo 'shaders' o reconfigura CMake.");
    }

    const auto size = static_cast<std::streamsize>(file.tellg());
    if (size <= 0 || (size % 4) != 0) {
        throw std::runtime_error("SPIR-V invalido (tamano no multiplo de 4): " + path.string());
    }

    std::vector<std::uint32_t> code(static_cast<std::size_t>(size) / 4);
    file.seekg(0);
    file.read(reinterpret_cast<char*>(code.data()), size);

    if (!file) {
        throw std::runtime_error("Error leyendo el shader: " + path.string());
    }

    vk::ShaderModuleCreateInfo create_info{};
    create_info.setCode(code);

    return vk::raii::ShaderModule(device.handle(), create_info);
}

}  // namespace cramion::gfx::shaders
