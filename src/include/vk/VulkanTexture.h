#ifndef CRAMION_VK_VULKAN_TEXTURE_H
#define CRAMION_VK_VULKAN_TEXTURE_H

#include "vk/VulkanCommon.h"

#include <cstddef>
#include <cstdint>

namespace cramion::gfx {

class VulkanDevice;

// Textura de solo lectura con su cadena completa de mipmaps.
//
// Los niveles se generan en la GPU a partir del nivel 0, cada uno la mitad
// del anterior con un blit lineal. Sin mips, una textura de 2048 vista de lejos
// se muestrearia saltando texeles y "herviria" al moverse la camara.
//
// Formato RGBA8 UNORM (no sRGB): el G-buffer guarda el albedo tal cual y la
// pasada de iluminacion lo linealiza, igual que el color de los bloques.
class VulkanTexture {
public:
    VulkanTexture() = default;
    VulkanTexture(const VulkanTexture&) = delete;
    VulkanTexture& operator=(const VulkanTexture&) = delete;
    VulkanTexture(VulkanTexture&&) = default;
    VulkanTexture& operator=(VulkanTexture&&) = default;

    // `rgba` tiene width * height * 4 bytes, empezando por la fila de arriba.
    void create(const VulkanDevice& device, std::uint32_t width, std::uint32_t height,
                const std::uint8_t* rgba);

    // Textura comprimida por bloques (BC de un DDS) con sus `mip_levels`
    // niveles ya hechos, seguidos en `data`. Se sube tal cual: no se
    // descomprime ni se generan mips.
    void createCompressed(const VulkanDevice& device, std::uint32_t width, std::uint32_t height,
                          vk::Format format, std::uint32_t block_bytes, std::uint32_t mip_levels,
                          const std::uint8_t* data, std::size_t size);
    void destroy();

    const vk::raii::ImageView& view() const { return view_; }
    std::uint32_t mipLevels() const { return mip_levels_; }

private:
    vk::raii::DeviceMemory memory_{nullptr};
    vk::raii::Image image_{nullptr};
    vk::raii::ImageView view_{nullptr};
    std::uint32_t mip_levels_ = 1;
};

}  // namespace cramion::gfx

#endif  // CRAMION_VK_VULKAN_TEXTURE_H
