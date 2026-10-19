#ifndef CRAMION_VK_LIGHTMAP_ATLAS_H
#define CRAMION_VK_LIGHTMAP_ATLAS_H

// Atlas de los lightmaps horneados en la GPU (PLAN-LIGHTMAPS.md 2.5): una
// imagen 2D array RGBA16F (capa = pagina; rgb = irradiancia, a = visibilidad
// del sol) con vista SIEMPRE de array (gbuffer_surface.glsl la lee como
// sampler2DArray: con una sola pagina VulkanImage daria una vista 2D). Sin
// mips (textureLod 0), filtro lineal y clamp a los bordes. Sin lightmaps, un
// ficticio de 1x1x1 con (0, 0, 0, 1): el binding 7 del set de la geometria
// siempre es valido.

#include "CramionFX/vk/BakedGi.h"
#include "CramionFX/vk/GpuMemoryStats.h"
#include "CramionFX/vk/VulkanCommon.h"

#include <cstdint>

namespace cramion::gfx {

class VulkanDevice;

class LightmapAtlas {
public:
    // El ficticio (1x1x1). Tambien la primera vez, antes de los sets.
    void createDummy(const VulkanDevice& device);
    // Sube las paginas (por staging, una copia por capa). false si no hay
    // paginas validas: queda el ficticio. Sin frames en vuelo que la lean.
    bool upload(const VulkanDevice& device, const LightmapSet& set);
    void destroy();

    vk::ImageView view() const { return *view_; }
    vk::Sampler sampler() const { return *sampler_; }
    bool real() const { return layers_ > 0 && !dummy_; }
    std::uint32_t layers() const { return layers_; }
    std::uint32_t width() const { return width_; }
    std::uint32_t height() const { return height_; }
    std::uint64_t bytes() const { return bytes_; }

private:
    bool createImage(const VulkanDevice& device, std::uint32_t width, std::uint32_t height, std::uint32_t layers);

    GpuMemoryTicket memory_ticket_;
    vk::raii::DeviceMemory memory_{nullptr};
    vk::raii::Image image_{nullptr};
    vk::raii::ImageView view_{nullptr};
    vk::raii::Sampler sampler_{nullptr};
    std::uint32_t width_ = 0, height_ = 0, layers_ = 0;
    std::uint64_t bytes_ = 0;
    bool dummy_ = true;
};

}  // namespace cramion::gfx

#endif  // CRAMION_VK_LIGHTMAP_ATLAS_H
