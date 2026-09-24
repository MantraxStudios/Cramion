#include "vk/FullscreenPass.h"

#include "vk/VulkanDevice.h"
#include "vk/VulkanShader.h"

#include <array>
#include <vector>

namespace cramion::gfx {

void FullscreenPass::create(const VulkanDevice& device, const FullscreenPassDesc& desc) {
    destroy();

    // Bordes fijados: los filtros de bloom y SSAO muestrean fuera de la imagen
    // y deben repetir el ultimo texel, no envolver al lado contrario.
    vk::SamplerCreateInfo sampler_info{};
    sampler_info.magFilter = desc.filter;
    sampler_info.minFilter = desc.filter;
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
        bindings[i].stageFlags = vk::ShaderStageFlagBits::eFragment;
    }

    vk::DescriptorSetLayoutCreateInfo set_layout_info{};
    set_layout_info.setBindings(bindings);
    set_layout_ = vk::raii::DescriptorSetLayout(device.handle(), set_layout_info);

    const vk::DescriptorSetLayout raw_set_layout = *set_layout_;

    vk::PushConstantRange push_range{};
    push_range.stageFlags = vk::ShaderStageFlagBits::eFragment;
    push_range.offset = 0;
    push_range.size = desc.push_constant_size;

    vk::PipelineLayoutCreateInfo layout_info{};
    layout_info.setSetLayouts(raw_set_layout);
    if (desc.push_constant_size > 0) {
        layout_info.setPushConstantRanges(push_range);
    }
    pipeline_layout_ = vk::raii::PipelineLayout(device.handle(), layout_info);

    const vk::raii::ShaderModule vertex_module = shaders::loadModule(device, "lighting.vert.spv");
    const vk::raii::ShaderModule fragment_module =
        shaders::loadModule(device, desc.fragment_shader);

    std::array<vk::PipelineShaderStageCreateInfo, 2> stages{};
    stages[0].stage = vk::ShaderStageFlagBits::eVertex;
    stages[0].module = *vertex_module;
    stages[0].pName = "main";
    stages[1].stage = vk::ShaderStageFlagBits::eFragment;
    stages[1].module = *fragment_module;
    stages[1].pName = "main";

    vk::PipelineVertexInputStateCreateInfo vertex_input{};

    vk::PipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.topology = vk::PrimitiveTopology::eTriangleList;

    vk::PipelineViewportStateCreateInfo viewport_state{};
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;

    vk::PipelineRasterizationStateCreateInfo rasterization{};
    rasterization.polygonMode = vk::PolygonMode::eFill;
    rasterization.cullMode = vk::CullModeFlagBits::eNone;
    rasterization.frontFace = vk::FrontFace::eCounterClockwise;
    rasterization.lineWidth = 1.0f;

    vk::PipelineMultisampleStateCreateInfo multisample{};
    multisample.rasterizationSamples = vk::SampleCountFlagBits::e1;

    vk::PipelineDepthStencilStateCreateInfo depth_stencil{};
    depth_stencil.depthTestEnable = VK_FALSE;
    depth_stencil.depthWriteEnable = VK_FALSE;

    vk::PipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.blendEnable = desc.additive_blend ? VK_TRUE : VK_FALSE;
    blend_attachment.srcColorBlendFactor = vk::BlendFactor::eOne;
    blend_attachment.dstColorBlendFactor = vk::BlendFactor::eOne;
    blend_attachment.colorBlendOp = vk::BlendOp::eAdd;
    blend_attachment.srcAlphaBlendFactor = vk::BlendFactor::eOne;
    blend_attachment.dstAlphaBlendFactor = vk::BlendFactor::eOne;
    blend_attachment.alphaBlendOp = vk::BlendOp::eAdd;
    blend_attachment.colorWriteMask =
        vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
        vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;

    vk::PipelineColorBlendStateCreateInfo color_blend{};
    color_blend.setAttachments(blend_attachment);

    const std::array<vk::DynamicState, 2> dynamic_states = {vk::DynamicState::eViewport,
                                                            vk::DynamicState::eScissor};
    vk::PipelineDynamicStateCreateInfo dynamic_state{};
    dynamic_state.setDynamicStates(dynamic_states);

    vk::PipelineRenderingCreateInfo rendering_info{};
    rendering_info.setColorAttachmentFormats(desc.color_format);

    vk::GraphicsPipelineCreateInfo pipeline_info{};
    pipeline_info.pNext = &rendering_info;
    pipeline_info.setStages(stages);
    pipeline_info.pVertexInputState = &vertex_input;
    pipeline_info.pInputAssemblyState = &input_assembly;
    pipeline_info.pViewportState = &viewport_state;
    pipeline_info.pRasterizationState = &rasterization;
    pipeline_info.pMultisampleState = &multisample;
    pipeline_info.pDepthStencilState = &depth_stencil;
    pipeline_info.pColorBlendState = &color_blend;
    pipeline_info.pDynamicState = &dynamic_state;
    pipeline_info.layout = *pipeline_layout_;

    pipeline_ = vk::raii::Pipeline(device.handle(), nullptr, pipeline_info);
}

void FullscreenPass::destroy() {
    pipeline_ = nullptr;
    pipeline_layout_ = nullptr;
    set_layout_ = nullptr;
    sampler_ = nullptr;
}

}  // namespace cramion::gfx
