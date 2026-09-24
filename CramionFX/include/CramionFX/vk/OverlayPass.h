#ifndef CRAMION_VK_OVERLAY_PASS_H
#define CRAMION_VK_OVERLAY_PASS_H

#include "CramionFX/vk/OverlayGeometry.h"
#include "CramionFX/vk/VulkanBuffer.h"
#include "CramionFX/vk/VulkanCommon.h"

#include <cstdint>
#include <vector>

namespace cramion::gfx {

class VulkanDevice;

// Dibuja OverlayGeometry (gizmos y ayudas del editor) sobre una imagen de
// color con prueba de profundidad contra el depth de la escena, en dos
// pasadas:
//
//   1. lo visible (depth <= escena): con la opacidad de su color,
//   2. lo tapado  (depth >  escena): con occluded_alpha ("rayos X").
//
// Las lineas se expanden en la CPU a quads de grosor constante en pixeles,
// directamente en espacio de recorte (recortadas antes contra el plano
// cercano): la GPU solo rasteriza. Un buffer de vertices por frame en vuelo,
// visible desde la CPU, que crece si hace falta.
class OverlayPass {
public:
    void create(const VulkanDevice& device, vk::Format color_format, vk::Format depth_format,
                std::uint32_t frames_in_flight);
    void destroy();

    // Prepara los vertices del frame (recorte y expansion de lineas).
    // Devuelve false si no hay nada que dibujar.
    bool prepare(const VulkanDevice& device, std::uint32_t frame, const OverlayGeometry& geometry,
                 const core::Mat4& view_projection, vk::Extent2D viewport);

    // Graba las dos pasadas (con el renderizado ya abierto sobre el destino
    // de color y el depth de la escena en solo lectura).
    void record(const vk::raii::CommandBuffer& cmd, std::uint32_t frame, vk::Extent2D viewport,
                const OverlayGeometry& geometry) const;

private:
    struct Vertex {
        float clip[4];
        std::uint32_t color;
        float edge;  // pixeles desde el eje (antialias)
    };

    vk::raii::Pipeline createPipeline(const VulkanDevice& device, vk::Format color_format,
                                      vk::Format depth_format, vk::CompareOp compare) const;

    vk::raii::PipelineLayout layout_{nullptr};
    vk::raii::Pipeline visible_pipeline_{nullptr};
    vk::raii::Pipeline occluded_pipeline_{nullptr};
    std::vector<VulkanBuffer> buffers_;
    std::vector<std::uint32_t> vertex_counts_;
    std::vector<Vertex> scratch_;
};

}  // namespace cramion::gfx

#endif  // CRAMION_VK_OVERLAY_PASS_H
