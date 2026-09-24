#include "CramionFX/vk/PostProcessPass.h"

#include "CramionFX/vk/GpuTypes.h"
#include "CramionFX/vk/VulkanDevice.h"
#include "CramionFX/vk/VulkanShader.h"

#include <array>
#include <iostream>

namespace cramion::gfx {

void PostProcessPass::create(const VulkanDevice& device, vk::Format color_format) {
    destroy();

    // Filtrado lineal: FXAA muestrea en posiciones fraccionarias de pixel.
    vk::SamplerCreateInfo sampler_info{};
    sampler_info.magFilter = vk::Filter::eLinear;
    sampler_info.minFilter = vk::Filter::eLinear;
    sampler_info.mipmapMode = vk::SamplerMipmapMode::eNearest;
    sampler_info.addressModeU = vk::SamplerAddressMode::eClampToEdge;
    sampler_info.addressModeV = vk::SamplerAddressMode::eClampToEdge;
    sampler_info.addressModeW = vk::SamplerAddressMode::eClampToEdge;

    sampler_ = vk::raii::Sampler(device.handle(), sampler_info);

    vk::DescriptorSetLayoutBinding scene_binding{};
    scene_binding.binding = 0;
    scene_binding.descriptorType = vk::DescriptorType::eCombinedImageSampler;
    scene_binding.descriptorCount = 1;
    scene_binding.stageFlags = vk::ShaderStageFlagBits::eFragment;

    vk::DescriptorSetLayoutCreateInfo set_layout_info{};
    set_layout_info.setBindings(scene_binding);
    set_layout_ = vk::raii::DescriptorSetLayout(device.handle(), set_layout_info);

    vk::PushConstantRange push_range{};
    push_range.stageFlags = vk::ShaderStageFlagBits::eFragment;
    push_range.offset = 0;
    push_range.size = sizeof(GpuPostProcessPush);

    const vk::DescriptorSetLayout raw_set_layout = *set_layout_;

    vk::PipelineLayoutCreateInfo layout_info{};
    layout_info.setSetLayouts(raw_set_layout);
    layout_info.setPushConstantRanges(push_range);
    pipeline_layout_ = vk::raii::PipelineLayout(device.handle(), layout_info);

    // Reutiliza el triangulo a pantalla completa de la pasada de iluminacion.
    const vk::raii::ShaderModule vertex_module = shaders::loadModule(device, "lighting.vert.spv");
    const vk::raii::ShaderModule fragment_module = shaders::loadModule(device, "fxaa.frag.spv");

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
    blend_attachment.blendEnable = VK_FALSE;
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
    rendering_info.setColorAttachmentFormats(color_format);

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

    std::cout << "[Vulkan] Pipeline de post-proceso (FXAA) creado\n";
}

void PostProcessPass::destroy() {
    pipeline_ = nullptr;
    pipeline_layout_ = nullptr;
    set_layout_ = nullptr;
    sampler_ = nullptr;
}

}  // namespace cramion::gfx
