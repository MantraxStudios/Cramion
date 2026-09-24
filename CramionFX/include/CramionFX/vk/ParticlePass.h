#ifndef CRAMION_VK_PARTICLE_PASS_H
#define CRAMION_VK_PARTICLE_PASS_H

#include "CramionFX/vk/ParticleGeometry.h"
#include "CramionFX/vk/VulkanBuffer.h"
#include "CramionFX/vk/VulkanCommon.h"

#include <cstdint>
#include <vector>

namespace cramion::gfx {

class VulkanDevice;

// Dibuja ParticleDrawList sobre la imagen HDR con prueba de profundidad
// (sin escribirla): primero las transparentes (mezcla alfa) y luego las
// aditivas. Cada particula se expande en la CPU a un quad de cara a la
// camara, ya en espacio de recorte; el fragment shader lo recorta a un disco
// de borde suave. Un buffer de vertices por frame en vuelo, que crece.
class ParticlePass {
public:
    void create(const VulkanDevice& device, vk::Format color_format, vk::Format depth_format,
                std::uint32_t frames_in_flight);
    void destroy();

    // Prepara los vertices del frame. false si no hay nada que dibujar.
    bool prepare(const VulkanDevice& device, std::uint32_t frame, const ParticleDrawList& particles,
                 const core::Mat4& view, const core::Mat4& view_projection);

    // Graba el dibujo (con el renderizado ya abierto sobre la imagen HDR y el
    // depth de la escena en solo lectura).
    void record(const vk::raii::CommandBuffer& cmd, std::uint32_t frame, vk::Extent2D viewport) const;

private:
    struct Vertex {
        float clip[4];
        float color[4];
        float uv[2];
    };

    vk::raii::Pipeline createPipeline(const VulkanDevice& device, vk::Format color_format,
                                      vk::Format depth_format, bool additive) const;

    vk::raii::PipelineLayout layout_{nullptr};
    vk::raii::Pipeline alpha_pipeline_{nullptr};
    vk::raii::Pipeline additive_pipeline_{nullptr};
    std::vector<VulkanBuffer> buffers_;
    std::vector<std::uint32_t> alpha_counts_;
    std::vector<std::uint32_t> additive_counts_;
    std::vector<Vertex> scratch_;
};

}  // namespace cramion::gfx

#endif  // CRAMION_VK_PARTICLE_PASS_H
