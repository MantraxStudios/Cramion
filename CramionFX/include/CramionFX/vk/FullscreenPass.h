#ifndef CRAMION_VK_FULLSCREEN_PASS_H
#define CRAMION_VK_FULLSCREEN_PASS_H

#include "CramionFX/vk/VulkanCommon.h"

#include <cstdint>
#include <span>
#include <string>

namespace cramion::gfx {

class VulkanDevice;

// Pipeline generico de un triangulo a pantalla completa (lighting.vert) con un
// fragment shader cualquiera. Lo usan las pasadas de post-proceso que solo
// leen texturas y escriben un destino de color: SSAO, bloom y composicion.
//
// Los bindings del set 0 se declaran en orden (binding i = bindings[i]).
struct FullscreenPassDesc {
    std::string fragment_shader;              // p. ej. "ssao.frag.spv"
    std::span<const vk::DescriptorType> bindings;
    std::uint32_t push_constant_size = 0;     // 0 = sin push constants
    vk::Format color_format = vk::Format::eUndefined;
    // Suma el resultado a lo que ya hay en el destino (subida del bloom).
    bool additive_blend = false;
    // Mezcla con el alfa del shader sobre lo que hay (contorno de seleccion).
    bool alpha_blend = false;
    // Filtro del muestreador compartido por todas las texturas.
    vk::Filter filter = vk::Filter::eLinear;
};

class FullscreenPass {
public:
    void create(const VulkanDevice& device, const FullscreenPassDesc& desc);
    void destroy();

    const vk::raii::Pipeline& pipeline() const { return pipeline_; }
    const vk::raii::PipelineLayout& layout() const { return pipeline_layout_; }
    const vk::raii::DescriptorSetLayout& descriptorSetLayout() const { return set_layout_; }
    const vk::raii::Sampler& sampler() const { return sampler_; }

private:
    vk::raii::Sampler sampler_{nullptr};
    vk::raii::DescriptorSetLayout set_layout_{nullptr};
    vk::raii::PipelineLayout pipeline_layout_{nullptr};
    vk::raii::Pipeline pipeline_{nullptr};
};

}  // namespace cramion::gfx

#endif  // CRAMION_VK_FULLSCREEN_PASS_H
