#ifndef CRAMION_VK_COMPUTE_PASS_H
#define CRAMION_VK_COMPUTE_PASS_H

#include "vk/VulkanCommon.h"

#include <cstdint>
#include <span>
#include <string>

namespace cramion::gfx {

class VulkanDevice;

// Pipeline de computo con un unico set de descriptores (binding i =
// bindings[i]) y push constants opcionales. Lo usa la auto-exposicion.
struct ComputePassDesc {
    std::string shader;  // p. ej. "exposure_histogram.comp.spv"
    std::span<const vk::DescriptorType> bindings;
    std::uint32_t push_constant_size = 0;
};

class ComputePass {
public:
    void create(const VulkanDevice& device, const ComputePassDesc& desc);
    void destroy();

    const vk::raii::Pipeline& pipeline() const { return pipeline_; }
    const vk::raii::PipelineLayout& layout() const { return pipeline_layout_; }
    const vk::raii::DescriptorSetLayout& descriptorSetLayout() const { return set_layout_; }
    // Muestreador lineal con bordes fijados, para los bindings de textura.
    const vk::raii::Sampler& sampler() const { return sampler_; }

private:
    vk::raii::Sampler sampler_{nullptr};
    vk::raii::DescriptorSetLayout set_layout_{nullptr};
    vk::raii::PipelineLayout pipeline_layout_{nullptr};
    vk::raii::Pipeline pipeline_{nullptr};
};

}  // namespace cramion::gfx

#endif  // CRAMION_VK_COMPUTE_PASS_H
