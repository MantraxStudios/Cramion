#include "vk/LightingPass.h"

#include "vk/GBuffer.h"
#include "vk/VulkanDevice.h"
#include "vk/VulkanShader.h"

#include <array>
#include <iostream>

namespace cramion::gfx {

void LightingPass::create(const VulkanDevice& device, vk::Format color_format) {
    destroy();

    // --- Muestreador del G-buffer ---
    // Filtrado lineal y bordes fijados: la pasada lee exactamente un texel por
    // pixel, pero el modo clamp evita artefactos en los bordes de la pantalla.
    vk::SamplerCreateInfo sampler_info{};
    sampler_info.magFilter = vk::Filter::eLinear;
    sampler_info.minFilter = vk::Filter::eLinear;
    sampler_info.mipmapMode = vk::SamplerMipmapMode::eNearest;
    sampler_info.addressModeU = vk::SamplerAddressMode::eClampToEdge;
    sampler_info.addressModeV = vk::SamplerAddressMode::eClampToEdge;
    sampler_info.addressModeW = vk::SamplerAddressMode::eClampToEdge;
    sampler_info.borderColor = vk::BorderColor::eFloatOpaqueBlack;

    sampler_ = vk::raii::Sampler(device.handle(), sampler_info);

    // --- Descriptores ---
    std::array<vk::DescriptorSetLayoutBinding, 20> bindings{};

    bindings[0].binding = 0;
    bindings[0].descriptorType = vk::DescriptorType::eUniformBuffer;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = vk::ShaderStageFlagBits::eFragment;

    // Bindings 1 y 2: albedo y normales. Binding 3: la profundidad, de la que
    // el shader deduce la posicion del mundo.
    for (std::uint32_t i = 0; i < 3; ++i) {
        bindings[1 + i].binding = 1 + i;
        bindings[1 + i].descriptorType = vk::DescriptorType::eCombinedImageSampler;
        bindings[1 + i].descriptorCount = 1;
        bindings[1 + i].stageFlags = vk::ShaderStageFlagBits::eFragment;
    }

    bindings[4].binding = 4;
    bindings[4].descriptorType = vk::DescriptorType::eUniformBuffer;
    bindings[4].descriptorCount = 1;
    bindings[4].stageFlags = vk::ShaderStageFlagBits::eFragment;

    // Binding 5: array de mapas de sombra (muestreador de comparacion).
    bindings[5].binding = 5;
    bindings[5].descriptorType = vk::DescriptorType::eCombinedImageSampler;
    bindings[5].descriptorCount = 1;
    bindings[5].stageFlags = vk::ShaderStageFlagBits::eFragment;

    // Binding 6: matrices y cortes de las cascadas.
    bindings[6].binding = 6;
    bindings[6].descriptorType = vk::DescriptorType::eUniformBuffer;
    bindings[6].descriptorCount = 1;
    bindings[6].stageFlags = vk::ShaderStageFlagBits::eFragment;

    // Bindings 7 y 8: mapas de sombra de los focos y de las caras de las luces
    // puntuales. Binding 9: sus matrices.
    for (std::uint32_t binding = 7; binding <= 8; ++binding) {
        bindings[binding].binding = binding;
        bindings[binding].descriptorType = vk::DescriptorType::eCombinedImageSampler;
        bindings[binding].descriptorCount = 1;
        bindings[binding].stageFlags = vk::ShaderStageFlagBits::eFragment;
    }

    bindings[9].binding = 9;
    bindings[9].descriptorType = vk::DescriptorType::eUniformBuffer;
    bindings[9].descriptorCount = 1;
    bindings[9].stageFlags = vk::ShaderStageFlagBits::eFragment;

    // Binding 10: oclusion ambiental de pantalla (SSAO).
    bindings[10].binding = 10;
    bindings[10].descriptorType = vk::DescriptorType::eCombinedImageSampler;
    bindings[10].descriptorCount = 1;
    bindings[10].stageFlags = vk::ShaderStageFlagBits::eFragment;

    // Binding 11: LUT del cielo fisico.
    bindings[11].binding = 11;
    bindings[11].descriptorType = vk::DescriptorType::eCombinedImageSampler;
    bindings[11].descriptorCount = 1;
    bindings[11].stageFlags = vk::ShaderStageFlagBits::eFragment;

    // Binding 12: G-buffer de material (emision + metalicidad).
    // Bindings 13 y 14: IBL, entorno prefiltrado (cubo) y LUT de la BRDF.
    for (std::uint32_t binding = 12; binding <= 14; ++binding) {
        bindings[binding].binding = binding;
        bindings[binding].descriptorType = vk::DescriptorType::eCombinedImageSampler;
        bindings[binding].descriptorCount = 1;
        bindings[binding].stageFlags = vk::ShaderStageFlagBits::eFragment;
    }

    // Binding 15: irradiancia del entorno en armonicos esfericos.
    bindings[15].binding = 15;
    bindings[15].descriptorType = vk::DescriptorType::eStorageBuffer;
    bindings[15].descriptorCount = 1;
    bindings[15].stageFlags = vk::ShaderStageFlagBits::eFragment;

    // Binding 16: iluminacion global de pantalla (media resolucion).
    bindings[16].binding = 16;
    bindings[16].descriptorType = vk::DescriptorType::eCombinedImageSampler;
    bindings[16].descriptorCount = 1;
    bindings[16].stageFlags = vk::ShaderStageFlagBits::eFragment;

    // Binding 17: reflejos de pantalla (SSR).
    bindings[17].binding = 17;
    bindings[17].descriptorType = vk::DescriptorType::eCombinedImageSampler;
    bindings[17].descriptorCount = 1;
    bindings[17].stageFlags = vk::ShaderStageFlagBits::eFragment;

    // Bindings 18 y 19: los dos cubos de la sonda de reflexion de la escena.
    for (std::uint32_t binding = 18; binding <= 19; ++binding) {
        bindings[binding].binding = binding;
        bindings[binding].descriptorType = vk::DescriptorType::eCombinedImageSampler;
        bindings[binding].descriptorCount = 1;
        bindings[binding].stageFlags = vk::ShaderStageFlagBits::eFragment;
    }

    vk::DescriptorSetLayoutCreateInfo set_layout_info{};
    set_layout_info.setBindings(bindings);
    set_layout_ = vk::raii::DescriptorSetLayout(device.handle(), set_layout_info);

    const vk::DescriptorSetLayout raw_set_layout = *set_layout_;

    vk::PipelineLayoutCreateInfo layout_info{};
    layout_info.setSetLayouts(raw_set_layout);
    pipeline_layout_ = vk::raii::PipelineLayout(device.handle(), layout_info);

    // --- Shaders ---
    const vk::raii::ShaderModule vertex_module = shaders::loadModule(device, "lighting.vert.spv");
    const vk::raii::ShaderModule fragment_module =
        shaders::loadModule(device, "lighting.frag.spv");

    std::array<vk::PipelineShaderStageCreateInfo, 2> stages{};
    stages[0].stage = vk::ShaderStageFlagBits::eVertex;
    stages[0].module = *vertex_module;
    stages[0].pName = "main";
    stages[1].stage = vk::ShaderStageFlagBits::eFragment;
    stages[1].module = *fragment_module;
    stages[1].pName = "main";

    // El triangulo se genera en el vertex shader: no hay buffer de vertices.
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

    // Sin profundidad: cubre la pantalla entera.
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

    std::cout << "[Vulkan] Pipeline de iluminacion creado\n";
}

void LightingPass::destroy() {
    pipeline_ = nullptr;
    pipeline_layout_ = nullptr;
    set_layout_ = nullptr;
    sampler_ = nullptr;
}

}  // namespace cramion::gfx
