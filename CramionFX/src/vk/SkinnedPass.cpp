#include "CramionFX/vk/SkinnedPass.h"

#include "CramionFX/asset/Model.h"
#include "CramionFX/vk/GBuffer.h"
#include "CramionFX/vk/GpuTypes.h"
#include "CramionFX/vk/VulkanDevice.h"
#include "CramionFX/vk/VulkanShader.h"

#include <array>
#include <cstddef>
#include <iostream>

namespace cramion::gfx {

namespace {

// Formato de asset::SkinnedVertex. Los indices de hueso son uvec4 de 32 bits:
// con indices de 8 bits el esqueleto quedaria limitado a 256 huesos.
struct VertexLayout {
    vk::VertexInputBindingDescription binding{0, sizeof(asset::SkinnedVertex),
                                              vk::VertexInputRate::eVertex};
    std::array<vk::VertexInputAttributeDescription, 6> attributes = {{
        {0, 0, vk::Format::eR32G32B32Sfloat, offsetof(asset::SkinnedVertex, position)},
        {1, 0, vk::Format::eR32G32B32Sfloat, offsetof(asset::SkinnedVertex, normal)},
        {2, 0, vk::Format::eR32G32Sfloat, offsetof(asset::SkinnedVertex, uv)},
        {3, 0, vk::Format::eR32G32B32A32Uint, offsetof(asset::SkinnedVertex, joints)},
        {4, 0, vk::Format::eR32G32B32A32Sfloat, offsetof(asset::SkinnedVertex, weights)},
        {5, 0, vk::Format::eR32G32B32A32Sfloat, offsetof(asset::SkinnedVertex, tangent)},
    }};
};

vk::PipelineShaderStageCreateInfo stage(vk::ShaderStageFlagBits flag,
                                        const vk::raii::ShaderModule& module) {
    vk::PipelineShaderStageCreateInfo info{};
    info.stage = flag;
    info.module = *module;
    info.pName = "main";
    return info;
}

}  // namespace

void SkinnedPass::create(const VulkanDevice& device, const GBuffer& gbuffer,
                         vk::Format shadow_format, vk::Format hdr_format) {
    destroy();

    // --- Muestreador de las texturas: trilineal y repetido ---
    vk::SamplerCreateInfo sampler_info{};
    sampler_info.magFilter = vk::Filter::eLinear;
    sampler_info.minFilter = vk::Filter::eLinear;
    sampler_info.mipmapMode = vk::SamplerMipmapMode::eLinear;
    sampler_info.addressModeU = vk::SamplerAddressMode::eRepeat;
    sampler_info.addressModeV = vk::SamplerAddressMode::eRepeat;
    sampler_info.addressModeW = vk::SamplerAddressMode::eRepeat;
    sampler_info.maxLod = VK_LOD_CLAMP_NONE;
    sampler_ = vk::raii::Sampler(device.handle(), sampler_info);

    // --- Set 0: camara + huesos + lluvia (mapa y parametros) ---
    std::array<vk::DescriptorSetLayoutBinding, 5> frame_bindings{};
    frame_bindings[0].binding = 0;
    frame_bindings[0].descriptorType = vk::DescriptorType::eUniformBuffer;
    frame_bindings[0].descriptorCount = 1;
    frame_bindings[0].stageFlags = vk::ShaderStageFlagBits::eVertex;
    frame_bindings[1].binding = 1;
    frame_bindings[1].descriptorType = vk::DescriptorType::eStorageBuffer;
    frame_bindings[1].descriptorCount = 1;
    frame_bindings[1].stageFlags = vk::ShaderStageFlagBits::eVertex;
    frame_bindings[2].binding = 2;
    frame_bindings[2].descriptorType = vk::DescriptorType::eCombinedImageSampler;
    frame_bindings[2].descriptorCount = 1;
    frame_bindings[2].stageFlags = vk::ShaderStageFlagBits::eFragment;
    frame_bindings[3].binding = 3;
    frame_bindings[3].descriptorType = vk::DescriptorType::eUniformBuffer;
    frame_bindings[3].descriptorCount = 1;
    frame_bindings[3].stageFlags = vk::ShaderStageFlagBits::eFragment;
    // Texturas de los decals (estampas): 8 ranuras.
    frame_bindings[4].binding = 4;
    frame_bindings[4].descriptorType = vk::DescriptorType::eCombinedImageSampler;
    frame_bindings[4].descriptorCount = 8;
    frame_bindings[4].stageFlags = vk::ShaderStageFlagBits::eFragment;

    vk::DescriptorSetLayoutCreateInfo frame_layout_info{};
    frame_layout_info.setBindings(frame_bindings);
    frame_set_layout_ = vk::raii::DescriptorSetLayout(device.handle(), frame_layout_info);

    // --- Set 1: las texturas PBR del material ---
    std::array<vk::DescriptorSetLayoutBinding, kMaterialTextureCount> material_bindings{};
    for (std::uint32_t i = 0; i < kMaterialTextureCount; ++i) {
        material_bindings[i].binding = i;
        material_bindings[i].descriptorType = vk::DescriptorType::eCombinedImageSampler;
        material_bindings[i].descriptorCount = 1;
        material_bindings[i].stageFlags = vk::ShaderStageFlagBits::eFragment;
    }

    vk::DescriptorSetLayoutCreateInfo material_layout_info{};
    material_layout_info.setBindings(material_bindings);
    material_set_layout_ = vk::raii::DescriptorSetLayout(device.handle(), material_layout_info);

    createGeometryPipeline(device, gbuffer);

    // --- Sombras: set 0 (los huesos) y set 1 (el material, para el alfa) ---
    vk::PushConstantRange shadow_push{};
    shadow_push.stageFlags = vk::ShaderStageFlagBits::eVertex;
    shadow_push.size = sizeof(GpuSkinnedShadowPush);

    const std::array<vk::DescriptorSetLayout, 2> shadow_set_layouts = {*frame_set_layout_,
                                                                       *material_set_layout_};
    vk::PipelineLayoutCreateInfo shadow_layout_info{};
    shadow_layout_info.setSetLayouts(shadow_set_layouts);
    shadow_layout_info.setPushConstantRanges(shadow_push);
    shadow_layout_ = vk::raii::PipelineLayout(device.handle(), shadow_layout_info);

    const bool clamp = device.depthClampSupported();
    shadow_pipeline_ = createShadowPipeline(device, shadow_format, clamp, 1.1f, true);
    shadow_opaque_pipeline_ = createShadowPipeline(device, shadow_format, clamp, 1.1f, false);
    local_shadow_pipeline_ = createShadowPipeline(device, shadow_format, false, 1.5f, true);
    local_shadow_opaque_pipeline_ =
        createShadowPipeline(device, shadow_format, false, 1.5f, false);

    createGlassPipeline(device, hdr_format, gbuffer.depthFormat());
    outline_silhouette_pipeline_ = createOutlinePipeline(device, gbuffer.depthFormat(), false);
    outline_visible_pipeline_ = createOutlinePipeline(device, gbuffer.depthFormat(), true);
    pick_pipeline_ = createOutlinePipeline(device, gbuffer.depthFormat(), true, /*pick=*/true);

    std::cout << "[Vulkan] Pipelines de modelos con esqueleto creados (huesos en storage buffer)\n";
}

void SkinnedPass::createGeometryPipeline(const VulkanDevice& device, const GBuffer& gbuffer) {
    vk::PushConstantRange push_range{};
    push_range.stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment;
    push_range.size = sizeof(GpuSkinnedPush);

    const std::array<vk::DescriptorSetLayout, 2> set_layouts = {*frame_set_layout_,
                                                                *material_set_layout_};
    vk::PipelineLayoutCreateInfo layout_info{};
    layout_info.setSetLayouts(set_layouts);
    layout_info.setPushConstantRanges(push_range);
    geometry_layout_ = vk::raii::PipelineLayout(device.handle(), layout_info);

    const vk::raii::ShaderModule vertex_module = shaders::loadModule(device, "skinned.vert.spv");
    const vk::raii::ShaderModule fragment_module = shaders::loadModule(device, "skinned.frag.spv");
    const std::array<vk::PipelineShaderStageCreateInfo, 2> stages = {
        stage(vk::ShaderStageFlagBits::eVertex, vertex_module),
        stage(vk::ShaderStageFlagBits::eFragment, fragment_module)};

    const VertexLayout layout;
    vk::PipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.setVertexBindingDescriptions(layout.binding);
    vertex_input.setVertexAttributeDescriptions(layout.attributes);

    vk::PipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.topology = vk::PrimitiveTopology::eTriangleList;

    vk::PipelineViewportStateCreateInfo viewport_state{};
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;

    vk::PipelineRasterizationStateCreateInfo rasterization{};
    rasterization.polygonMode = vk::PolygonMode::eFill;
    // Doble cara: la vegetacion, las telas y muchos objetos de los escenarios
    // son planos de una sola cara. skinned.frag gira la normal de las caras
    // traseras (gl_FrontFacing) para que se iluminen del lado que se ven.
    rasterization.cullMode = vk::CullModeFlagBits::eNone;
    // Assimp entrega los triangulos en sentido antihorario.
    rasterization.frontFace = vk::FrontFace::eCounterClockwise;
    rasterization.lineWidth = 1.0f;

    vk::PipelineMultisampleStateCreateInfo multisample{};
    multisample.rasterizationSamples = vk::SampleCountFlagBits::e1;

    vk::PipelineDepthStencilStateCreateInfo depth_stencil{};
    depth_stencil.depthTestEnable = VK_TRUE;
    depth_stencil.depthWriteEnable = VK_TRUE;
    depth_stencil.depthCompareOp = vk::CompareOp::eLess;

    vk::PipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.colorWriteMask =
        vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
        vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
    std::array<vk::PipelineColorBlendAttachmentState, GBuffer::kColorAttachmentCount>
        blend_attachments{};
    blend_attachments.fill(blend_attachment);

    vk::PipelineColorBlendStateCreateInfo color_blend{};
    color_blend.setAttachments(blend_attachments);

    const std::array<vk::DynamicState, 2> dynamic_states = {vk::DynamicState::eViewport,
                                                            vk::DynamicState::eScissor};
    vk::PipelineDynamicStateCreateInfo dynamic_state{};
    dynamic_state.setDynamicStates(dynamic_states);

    const auto color_formats = gbuffer.colorFormats();
    vk::PipelineRenderingCreateInfo rendering_info{};
    rendering_info.setColorAttachmentFormats(color_formats);
    rendering_info.depthAttachmentFormat = gbuffer.depthFormat();

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
    pipeline_info.layout = *geometry_layout_;

    geometry_pipeline_ = vk::raii::Pipeline(device.handle(), nullptr, pipeline_info);
}

void SkinnedPass::createGlassPipeline(const VulkanDevice& device, vk::Format color_format,
                                      vk::Format depth_format) {
    // --- Set 2: lo que el vidrio necesita para reflejar la escena ---
    using Type = vk::DescriptorType;
    constexpr std::array<Type, kGlassBindingCount> kTypes = {
        Type::eUniformBuffer,        Type::eUniformBuffer,        Type::eUniformBuffer,
        Type::eCombinedImageSampler, Type::eCombinedImageSampler, Type::eCombinedImageSampler,
        Type::eCombinedImageSampler, Type::eCombinedImageSampler, Type::eCombinedImageSampler};
    std::array<vk::DescriptorSetLayoutBinding, kGlassBindingCount> glass_bindings{};
    for (std::uint32_t i = 0; i < kGlassBindingCount; ++i) {
        glass_bindings[i].binding = i;
        glass_bindings[i].descriptorType = kTypes[i];
        glass_bindings[i].descriptorCount = 1;
        glass_bindings[i].stageFlags = vk::ShaderStageFlagBits::eFragment;
    }
    vk::DescriptorSetLayoutCreateInfo glass_set_info{};
    glass_set_info.setBindings(glass_bindings);
    glass_set_layout_ = vk::raii::DescriptorSetLayout(device.handle(), glass_set_info);

    vk::PushConstantRange push_range{};
    push_range.stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment;
    push_range.size = sizeof(GpuSkinnedPush);

    const std::array<vk::DescriptorSetLayout, 3> set_layouts = {
        *frame_set_layout_, *material_set_layout_, *glass_set_layout_};
    vk::PipelineLayoutCreateInfo layout_info{};
    layout_info.setSetLayouts(set_layouts);
    layout_info.setPushConstantRanges(push_range);
    glass_layout_ = vk::raii::PipelineLayout(device.handle(), layout_info);

    const vk::raii::ShaderModule vertex_module = shaders::loadModule(device, "skinned.vert.spv");
    const vk::raii::ShaderModule fragment_module = shaders::loadModule(device, "glass.frag.spv");
    const std::array<vk::PipelineShaderStageCreateInfo, 2> stages = {
        stage(vk::ShaderStageFlagBits::eVertex, vertex_module),
        stage(vk::ShaderStageFlagBits::eFragment, fragment_module)};

    const VertexLayout layout;
    vk::PipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.setVertexBindingDescriptions(layout.binding);
    vertex_input.setVertexAttributeDescriptions(layout.attributes);

    vk::PipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.topology = vk::PrimitiveTopology::eTriangleList;

    vk::PipelineViewportStateCreateInfo viewport_state{};
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;

    // Los cristales son laminas: se ven por las dos caras.
    vk::PipelineRasterizationStateCreateInfo rasterization{};
    rasterization.polygonMode = vk::PolygonMode::eFill;
    rasterization.cullMode = vk::CullModeFlagBits::eNone;
    rasterization.frontFace = vk::FrontFace::eCounterClockwise;
    rasterization.lineWidth = 1.0f;

    vk::PipelineMultisampleStateCreateInfo multisample{};
    multisample.rasterizationSamples = vk::SampleCountFlagBits::e1;

    // Prueba contra lo opaco, sin escribir: el depth del G-buffer sigue
    // siendo el de las superficies que ilumino la pasada diferida.
    vk::PipelineDepthStencilStateCreateInfo depth_stencil{};
    depth_stencil.depthTestEnable = VK_TRUE;
    depth_stencil.depthWriteEnable = VK_FALSE;
    depth_stencil.depthCompareOp = vk::CompareOp::eLessOrEqual;

    // resultado = reflejo + destino * transmitancia (alfa del shader). El alfa
    // del destino (distancia, para la sonda) no se toca.
    vk::PipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.blendEnable = VK_TRUE;
    blend_attachment.srcColorBlendFactor = vk::BlendFactor::eOne;
    blend_attachment.dstColorBlendFactor = vk::BlendFactor::eSrcAlpha;
    blend_attachment.colorBlendOp = vk::BlendOp::eAdd;
    blend_attachment.srcAlphaBlendFactor = vk::BlendFactor::eZero;
    blend_attachment.dstAlphaBlendFactor = vk::BlendFactor::eOne;
    blend_attachment.alphaBlendOp = vk::BlendOp::eAdd;
    blend_attachment.colorWriteMask = vk::ColorComponentFlagBits::eR |
                                      vk::ColorComponentFlagBits::eG |
                                      vk::ColorComponentFlagBits::eB;

    vk::PipelineColorBlendStateCreateInfo color_blend{};
    color_blend.setAttachments(blend_attachment);

    const std::array<vk::DynamicState, 2> dynamic_states = {vk::DynamicState::eViewport,
                                                            vk::DynamicState::eScissor};
    vk::PipelineDynamicStateCreateInfo dynamic_state{};
    dynamic_state.setDynamicStates(dynamic_states);

    vk::PipelineRenderingCreateInfo rendering_info{};
    rendering_info.setColorAttachmentFormats(color_format);
    rendering_info.depthAttachmentFormat = depth_format;

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
    pipeline_info.layout = *glass_layout_;

    glass_pipeline_ = vk::raii::Pipeline(device.handle(), nullptr, pipeline_info);
}

vk::raii::Pipeline SkinnedPass::createOutlinePipeline(const VulkanDevice& device,
                                                      vk::Format depth_format,
                                                      bool visible_only, bool pick) const {
    // skinned.vert, igual que el G-buffer: la profundidad de la mascara sale
    // identica a la del G-buffer y la prueba "menor o igual" es exacta.
    const vk::raii::ShaderModule vertex_module = shaders::loadModule(device, "skinned.vert.spv");
    // (El picking usa el mismo pipeline con pick.frag y una imagen de IDs.)
    const vk::raii::ShaderModule fragment_module =
        shaders::loadModule(device, pick ? "pick.frag.spv" : "outline_mask.frag.spv");
    const std::array<vk::PipelineShaderStageCreateInfo, 2> stages = {
        stage(vk::ShaderStageFlagBits::eVertex, vertex_module),
        stage(vk::ShaderStageFlagBits::eFragment, fragment_module)};

    const VertexLayout layout;
    vk::PipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.setVertexBindingDescriptions(layout.binding);
    vertex_input.setVertexAttributeDescriptions(layout.attributes);

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
    depth_stencil.depthTestEnable = visible_only ? VK_TRUE : VK_FALSE;
    depth_stencil.depthWriteEnable = VK_FALSE;
    depth_stencil.depthCompareOp = vk::CompareOp::eLessOrEqual;

    vk::PipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.colorWriteMask =
        visible_only && !pick ? vk::ColorComponentFlagBits::eG : vk::ColorComponentFlagBits::eR;

    vk::PipelineColorBlendStateCreateInfo color_blend{};
    color_blend.setAttachments(blend_attachment);

    const std::array<vk::DynamicState, 2> dynamic_states = {vk::DynamicState::eViewport,
                                                            vk::DynamicState::eScissor};
    vk::PipelineDynamicStateCreateInfo dynamic_state{};
    dynamic_state.setDynamicStates(dynamic_states);

    const vk::Format color_format = pick ? kPickFormat : kOutlineMaskFormat;
    vk::PipelineRenderingCreateInfo rendering_info{};
    rendering_info.setColorAttachmentFormats(color_format);
    rendering_info.depthAttachmentFormat = depth_format;

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
    pipeline_info.layout = *geometry_layout_;

    return vk::raii::Pipeline(device.handle(), nullptr, pipeline_info);
}

vk::raii::Pipeline SkinnedPass::createShadowPipeline(const VulkanDevice& device,
                                                     vk::Format depth_format, bool depth_clamp,
                                                     float slope_bias, bool alpha_tested) const {
    const vk::raii::ShaderModule vertex_module =
        shaders::loadModule(device, "skinned_shadow.vert.spv");
    const vk::raii::ShaderModule fragment_module =
        shaders::loadModule(device, "skinned_shadow.frag.spv");
    const std::array<vk::PipelineShaderStageCreateInfo, 2> all_stages = {
        stage(vk::ShaderStageFlagBits::eVertex, vertex_module),
        stage(vk::ShaderStageFlagBits::eFragment, fragment_module)};
    // Lo opaco: solo el vertex shader (la profundidad la escribe la GPU).
    const vk::ArrayProxyNoTemporaries<const vk::PipelineShaderStageCreateInfo> shadow_stages(
        alpha_tested ? 2u : 1u, all_stages.data());

    const VertexLayout layout;
    vk::PipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.setVertexBindingDescriptions(layout.binding);
    vertex_input.setVertexAttributeDescriptions(layout.attributes);

    vk::PipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.topology = vk::PrimitiveTopology::eTriangleList;

    vk::PipelineViewportStateCreateInfo viewport_state{};
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;

    // Sin descartar caras: la vegetacion y muchos objetos son planos de una
    // sola cara, y sin la cara trasera la luz los atravesaria.
    vk::PipelineRasterizationStateCreateInfo rasterization{};
    rasterization.polygonMode = vk::PolygonMode::eFill;
    rasterization.cullMode = vk::CullModeFlagBits::eNone;
    rasterization.frontFace = vk::FrontFace::eCounterClockwise;
    rasterization.lineWidth = 1.0f;
    rasterization.depthBiasEnable = VK_TRUE;
    rasterization.depthBiasConstantFactor = 0.6f;
    rasterization.depthBiasSlopeFactor = slope_bias;
    rasterization.depthClampEnable = depth_clamp ? VK_TRUE : VK_FALSE;

    vk::PipelineMultisampleStateCreateInfo multisample{};
    multisample.rasterizationSamples = vk::SampleCountFlagBits::e1;

    vk::PipelineDepthStencilStateCreateInfo depth_stencil{};
    depth_stencil.depthTestEnable = VK_TRUE;
    depth_stencil.depthWriteEnable = VK_TRUE;
    depth_stencil.depthCompareOp = vk::CompareOp::eLess;

    vk::PipelineColorBlendStateCreateInfo color_blend{};

    const std::array<vk::DynamicState, 2> dynamic_states = {vk::DynamicState::eViewport,
                                                            vk::DynamicState::eScissor};
    vk::PipelineDynamicStateCreateInfo dynamic_state{};
    dynamic_state.setDynamicStates(dynamic_states);

    vk::PipelineRenderingCreateInfo rendering_info{};
    rendering_info.depthAttachmentFormat = depth_format;

    vk::GraphicsPipelineCreateInfo pipeline_info{};
    pipeline_info.pNext = &rendering_info;
    pipeline_info.setStages(shadow_stages);
    pipeline_info.pVertexInputState = &vertex_input;
    pipeline_info.pInputAssemblyState = &input_assembly;
    pipeline_info.pViewportState = &viewport_state;
    pipeline_info.pRasterizationState = &rasterization;
    pipeline_info.pMultisampleState = &multisample;
    pipeline_info.pDepthStencilState = &depth_stencil;
    pipeline_info.pColorBlendState = &color_blend;
    pipeline_info.pDynamicState = &dynamic_state;
    pipeline_info.layout = *shadow_layout_;

    return vk::raii::Pipeline(device.handle(), nullptr, pipeline_info);
}

void SkinnedPass::destroy() {
    outline_visible_pipeline_ = nullptr;
    pick_pipeline_ = nullptr;
    outline_silhouette_pipeline_ = nullptr;
    glass_pipeline_ = nullptr;
    glass_layout_ = nullptr;
    glass_set_layout_ = nullptr;
    local_shadow_opaque_pipeline_ = nullptr;
    shadow_opaque_pipeline_ = nullptr;
    local_shadow_pipeline_ = nullptr;
    shadow_pipeline_ = nullptr;
    shadow_layout_ = nullptr;
    geometry_pipeline_ = nullptr;
    geometry_layout_ = nullptr;
    material_set_layout_ = nullptr;
    frame_set_layout_ = nullptr;
    sampler_ = nullptr;
}

}  // namespace cramion::gfx
