#ifndef CRAMION_VK_SSR_PYRAMIDS_H
#define CRAMION_VK_SSR_PYRAMIDS_H

#include "CramionFX/vk/ComputePass.h"
#include "CramionFX/vk/GpuMemoryStats.h"
#include "CramionFX/vk/VulkanCommon.h"

#include <cstdint>
#include <vector>

namespace cramion::gfx {

class VulkanDevice;
class VulkanImage;

// Las dos piramides de los reflejos de pantalla (ssr.frag):
//
//  - Profundidad MINIMA, lo mas cercano de cada bloque (ssr_hiz.comp). El rayo
//    cruza de un salto los bloques cuyo punto mas cercano queda por detras de
//    el y solo baja al pixel donde de verdad puede chocar, como la "Hi-Z" de
//    AMD FidelityFX SSSR: no se salta objetos finos (patas, marcos) y lo que
//    pasa por detras de un objeto no cuenta como choque. (La del culling,
//    GpuCulling, guarda lo mas LEJANO: no sirve para esto.)
//  - La imagen iluminada del frame anterior con mips (la historia). Los
//    reflejos de una superficie satinada la leen en el mip del cono de su
//    rugosidad: borrosos como en la sonda, en vez de cinco copias en cruz.
//
// Solo las vistas no aisladas escriben la historia (recordColorPyramid): la
// segunda vista del editor y las caras de la sonda no la tocan, asi que el
// SSR y la GI leen el frame anterior de su camara. En VR estereo la escriben
// los dos ojos: cada ojo lee la del otro, reproyectada con la vista-
// proyeccion guardada con ella.
class SsrPyramids {
public:
    void create(const VulkanDevice& device);
    // Imagenes del tamano de `depth` (la resolucion interna) y los sets de la
    // piramide de profundidad. Otra vez cada vez que cambia el G-buffer.
    void resize(const VulkanDevice& device, const VulkanImage& depth);
    void destroy();

    // Piramide de profundidad de este frame. El depth ya tiene que estar en
    // solo lectura (despues de la geometria). La deja lista para ssr.frag.
    void recordDepthPyramid(const vk::raii::CommandBuffer& cmd) const;
    // La imagen HDR de este frame (`scene`, como destino de color) al nivel 0
    // de la historia y, con `with_mips`, sus mips (solo los lee el SSR).
    // `scene` vuelve a quedar como destino de color y la historia como textura
    // (la leen el SSR, la GI y los rayos).
    void recordColorPyramid(const vk::raii::CommandBuffer& cmd, vk::Image scene, bool with_mips) const;

    const vk::raii::ImageView& depthView() const { return depth_view_; }
    const vk::raii::ImageView& colorView() const { return color_view_; }
    // Trilineal con todos los mips (la historia) y al mas cercano (la
    // profundidad, que se lee con texelFetch).
    const vk::raii::Sampler& colorSampler() const { return color_sampler_; }
    const vk::raii::Sampler& depthSampler() const { return depth_sampler_; }
    std::uint32_t depthLevels() const { return depth_levels_; }
    std::uint32_t colorLevels() const { return color_levels_; }
    bool isValid() const { return *color_image_ != nullptr && *depth_image_ != nullptr; }

private:
    void releaseImages();

    vk::Extent2D extent_{0, 0};

    // El pool, los samplers y el pipeline sobreviven a los sets y a las
    // vistas: se declaran antes (se destruyen despues).
    vk::raii::DescriptorPool pool_{nullptr};
    vk::raii::Sampler color_sampler_{nullptr};
    vk::raii::Sampler depth_sampler_{nullptr};
    ComputePass depth_pass_;

    // Profundidad minima: vive en General (se escribe como storage y se lee con
    // texelFetch), como la del culling.
    GpuMemoryTicket depth_ticket_;
    vk::raii::DeviceMemory depth_memory_{nullptr};
    vk::raii::Image depth_image_{nullptr};
    vk::raii::ImageView depth_view_{nullptr};
    std::vector<vk::raii::ImageView> depth_level_views_;
    std::vector<vk::raii::DescriptorSet> depth_sets_;  // uno por nivel
    std::uint32_t depth_levels_ = 0;

    // Historia con mips: entre frames, como textura.
    GpuMemoryTicket color_ticket_;
    vk::raii::DeviceMemory color_memory_{nullptr};
    vk::raii::Image color_image_{nullptr};
    vk::raii::ImageView color_view_{nullptr};
    std::uint32_t color_levels_ = 0;
};

}  // namespace cramion::gfx

#endif  // CRAMION_VK_SSR_PYRAMIDS_H
