#ifndef CRAMION_VK_VULKAN_TEXTURE_H
#define CRAMION_VK_VULKAN_TEXTURE_H

#include "CramionFX/vk/GpuMemoryStats.h"
#include "CramionFX/vk/VulkanCommon.h"

#include <cstddef>
#include <cstdint>

struct VmaAllocation_T;

namespace cramion::gfx {

class UploadBatch;
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
    ~VulkanTexture() { destroy(); }
    VulkanTexture(const VulkanTexture&) = delete;
    VulkanTexture& operator=(const VulkanTexture&) = delete;
    VulkanTexture(VulkanTexture&& other) noexcept;
    VulkanTexture& operator=(VulkanTexture&& other) noexcept;

    // `rgba` tiene width * height * 4 bytes, empezando por la fila de arriba.
    // Espera a la GPU.
    void create(const VulkanDevice& device, std::uint32_t width, std::uint32_t height,
                const std::uint8_t* rgba);
    // Igual, grabando la copia y los mips en `batch` (sin esperar). Quien la
    // crea la mantiene viva hasta que la GPU termina (UploadBatch::keepAlive).
    void create(const VulkanDevice& device, std::uint32_t width, std::uint32_t height,
                const std::uint8_t* rgba, UploadBatch& batch);

    // Textura comprimida por bloques (BC de un DDS) con sus `mip_levels`
    // niveles ya hechos, seguidos en `data`. Se sube tal cual: no se
    // descomprime ni se generan mips. Espera a la GPU.
    void createCompressed(const VulkanDevice& device, std::uint32_t width, std::uint32_t height,
                          vk::Format format, std::uint32_t block_bytes, std::uint32_t mip_levels,
                          const std::uint8_t* data, std::size_t size);
    // Igual, en `batch` (sin esperar; ver create).
    void createCompressed(const VulkanDevice& device, std::uint32_t width, std::uint32_t height,
                          vk::Format format, std::uint32_t block_bytes, std::uint32_t mip_levels,
                          const std::uint8_t* data, std::size_t size, UploadBatch& batch);
    void destroy();

    const vk::raii::ImageView& view() const { return view_; }
    std::uint32_t mipLevels() const { return mip_levels_; }

private:
    // Imagen con su memoria (de VMA, ver VulkanDevice::allocator).
    void createImage(const VulkanDevice& device, const vk::ImageCreateInfo& info);

    GpuMemoryTicket memory_ticket_;  // cuenta de VRAM (GpuMemoryStats.h)
    const VulkanDevice* device_ = nullptr;  // su asignador
    VmaAllocation_T* allocation_ = nullptr;
    vk::raii::Image image_{nullptr};
    vk::raii::ImageView view_{nullptr};
    std::uint32_t mip_levels_ = 1;
};

}  // namespace cramion::gfx

#endif  // CRAMION_VK_VULKAN_TEXTURE_H
