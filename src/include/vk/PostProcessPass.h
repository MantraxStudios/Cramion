#ifndef CRAMION_VK_POST_PROCESS_PASS_H
#define CRAMION_VK_POST_PROCESS_PASS_H

#include "vk/VulkanCommon.h"

namespace cramion::gfx {

class VulkanDevice;

// Ultima pasada del frame: toma la imagen ya iluminada, le aplica FXAA y la
// escribe en la imagen de la swapchain.
//
// Sin esta pasada todos los bordes quedan en escalera: un renderizador diferido
// no puede usar MSAA sin multiplicar el coste del G-buffer, asi que el
// antialiasing tiene que hacerse sobre el color final.
//
// Recursos que usa:
//   set 0, binding 0 -> imagen iluminada de la escena
//   push constant    -> 1/resolucion y el interruptor de FXAA
class PostProcessPass {
public:
    void create(const VulkanDevice& device, vk::Format color_format);
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

#endif  // CRAMION_VK_POST_PROCESS_PASS_H
