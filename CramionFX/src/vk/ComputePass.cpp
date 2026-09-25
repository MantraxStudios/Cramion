#include "CramionFX/vk/ComputePass.h"

#include "CramionFX/vk/VulkanDevice.h"
#include "CramionFX/vk/VulkanShader.h"

#include <vector>

namespace cramion::gfx {

void ComputePass::create(const VulkanDevice& device, const ComputePassDesc& desc) {
    destroy();

    vk::SamplerCreateInfo sampler_info{};
    sampler_info.magFilter = vk::Filter::eLinear;
    sampler_info.minFilter = vk::Filter::eLinear;
    sampler_info.mipmapMode = vk::SamplerMipmapMode::eNearest;
    sampler_info.addressModeU = vk::SamplerAddressMode::eClampToEdge;
    sampler_info.addressModeV = vk::SamplerAddressMode::eClampToEdge;
    sampler_info.addressModeW = vk::SamplerAddressMode::eClampToEdge;
    sampler_ = vk::raii::Sampler(device.handle(), sampler_info);

    std::vector<vk::DescriptorSetLayoutBinding> bindings(desc.bindings.size());
    for (std::uint32_t i = 0; i < bindings.size(); ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = desc.bindings[i];
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = vk::ShaderStageFlagBits::eCompute;
    }

    vk::DescriptorSetLayoutCreateInfo set_layout_info{};
    set_layout_info.setBindings(bindings);
    set_layout_ = vk::raii::DescriptorSetLayout(device.handle(), set_layout_info);

    const vk::DescriptorSetLayout raw_set_layout = *set_layout_;

    vk::PushConstantRange push_range{};
    push_range.stageFlags = vk::ShaderStageFlagBits::eCompute;
    push_range.offset = 0;
    push_range.size = desc.push_constant_size;

    vk::PipelineLayoutCreateInfo layout_info{};
    layout_info.setSetLayouts(raw_set_layout);
    if (desc.push_constant_size > 0) {
        layout_info.setPushConstantRanges(push_range);
    }
    pipeline_layout_ = vk::raii::PipelineLayout(device.handle(), layout_info);

    const vk::raii::ShaderModule module = shaders::loadModule(device, desc.shader);

    vk::ComputePipelineCreateInfo pipeline_info{};
    pipeline_info.stage.stage = vk::ShaderStageFlagBits::eCompute;
    pipeline_info.stage.module = *module;
    pipeline_info.stage.pName = "main";
    pipeline_info.layout = *pipeline_layout_;

    pipeline_ = vk::raii::Pipeline(device.handle(), device.pipelineCache(), pipeline_info);
}

void ComputePass::destroy() {
    pipeline_ = nullptr;
    pipeline_layout_ = nullptr;
    set_layout_ = nullptr;
    sampler_ = nullptr;
}

}  // namespace cramion::gfx
