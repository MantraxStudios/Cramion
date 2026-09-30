#include "CramionFX/vk/SkinnedPass.h"

#include "CramionFX/asset/Model.h"
#include "CramionFX/vk/GBuffer.h"
#include "CramionFX/vk/GpuTypes.h"
#include "CramionFX/vk/VulkanDevice.h"
#include "CramionFX/vk/VulkanShader.h"

#include <array>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

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
    // Camara y clima tambien en fragmentos y vertices: los shaders de
    // superficie del usuario leen la posicion de la camara y los segundos.
    const vk::ShaderStageFlags both = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment;
    // Con mesh shaders, la camara y los huesos tambien los leen el task y el mesh shader.
    const char* no_mesh = std::getenv("CRAMION_NO_MESH");
    mesh_shaders_ = device.meshShaderSupported() && (no_mesh == nullptr || std::string_view(no_mesh) == "0");
    const vk::ShaderStageFlags mesh_stages =
        mesh_shaders_ ? vk::ShaderStageFlags(vk::ShaderStageFlagBits::eTaskEXT | vk::ShaderStageFlagBits::eMeshEXT)
                      : vk::ShaderStageFlags{};
    // Relieve teselado: la camara (factores) y la altura (set 1, binding 3)
    // tambien en la teselacion.
    tessellation_ = device.tessellationSupported();
    const vk::ShaderStageFlags tess_stages =
        tessellation_ ? vk::ShaderStageFlags(vk::ShaderStageFlagBits::eTessellationControl |
                                             vk::ShaderStageFlagBits::eTessellationEvaluation)
                      : vk::ShaderStageFlags{};
    geometry_push_stages_ = both | tess_stages;
    shadow_push_stages_ = vk::ShaderStageFlagBits::eVertex | tess_stages;
    std::array<vk::DescriptorSetLayoutBinding, 6> frame_bindings{};
    frame_bindings[0].binding = 0;
    frame_bindings[0].descriptorType = vk::DescriptorType::eUniformBuffer;
    frame_bindings[0].descriptorCount = 1;
    frame_bindings[0].stageFlags = both | mesh_stages | tess_stages;
    frame_bindings[1].binding = 1;
    frame_bindings[1].descriptorType = vk::DescriptorType::eStorageBuffer;
    frame_bindings[1].descriptorCount = 1;
    frame_bindings[1].stageFlags = vk::ShaderStageFlagBits::eVertex | mesh_stages;
    frame_bindings[2].binding = 2;
    frame_bindings[2].descriptorType = vk::DescriptorType::eCombinedImageSampler;
    frame_bindings[2].descriptorCount = 1;
    frame_bindings[2].stageFlags = vk::ShaderStageFlagBits::eFragment;
    frame_bindings[3].binding = 3;
    frame_bindings[3].descriptorType = vk::DescriptorType::eUniformBuffer;
    frame_bindings[3].descriptorCount = 1;
    frame_bindings[3].stageFlags = both;
    // Texturas de los decals (estampas): 8 ranuras.
    frame_bindings[4].binding = 4;
    frame_bindings[4].descriptorType = vk::DescriptorType::eCombinedImageSampler;
    frame_bindings[4].descriptorCount = kMaxDecalTextures;
    frame_bindings[4].stageFlags = vk::ShaderStageFlagBits::eFragment;
    // Propiedades de los materiales con shader propio (8 vec4 cada uno).
    frame_bindings[5].binding = 5;
    frame_bindings[5].descriptorType = vk::DescriptorType::eStorageBuffer;
    frame_bindings[5].descriptorCount = 1;
    frame_bindings[5].stageFlags = both;

    vk::DescriptorSetLayoutCreateInfo frame_layout_info{};
    frame_layout_info.setBindings(frame_bindings);
    frame_set_layout_ = vk::raii::DescriptorSetLayout(device.handle(), frame_layout_info);

    // --- Set 1: las texturas PBR del material ---
    // Las 4 ultimas son de los shaders del usuario (tambien en vertices).
    std::array<vk::DescriptorSetLayoutBinding, kMaterialBindingCount> material_bindings{};
    for (std::uint32_t i = 0; i < kMaterialBindingCount; ++i) {
        material_bindings[i].binding = i;
        material_bindings[i].descriptorType = vk::DescriptorType::eCombinedImageSampler;
        material_bindings[i].descriptorCount = 1;
        material_bindings[i].stageFlags = i < kMaterialTextureCount ? vk::ShaderStageFlags(vk::ShaderStageFlagBits::eFragment)
                                                                    : both;
        // La altura del relieve (G de la oclusion) la lee skinned.tese.
        if (i == 3 && tessellation_) {
            material_bindings[i].stageFlags |= vk::ShaderStageFlagBits::eTessellationEvaluation;
        }
    }

    vk::DescriptorSetLayoutCreateInfo material_layout_info{};
    material_layout_info.setBindings(material_bindings);
    material_set_layout_ = vk::raii::DescriptorSetLayout(device.handle(), material_layout_info);

    hdr_format_ = hdr_format;
    createGeometryPipeline(device, gbuffer);

    // --- Sombras: set 0 (los huesos) y set 1 (el material, para el alfa) ---
    vk::PushConstantRange shadow_push{};
    shadow_push.stageFlags = shadow_push_stages_;
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
    if (tessellation_) {
        shadow_tess_pipelines_[0] = createShadowPipeline(device, shadow_format, clamp, 1.1f, false, true);
        shadow_tess_pipelines_[1] = createShadowPipeline(device, shadow_format, clamp, 1.1f, true, true);
        shadow_tess_pipelines_[2] = createShadowPipeline(device, shadow_format, false, 1.5f, false, true);
        shadow_tess_pipelines_[3] = createShadowPipeline(device, shadow_format, false, 1.5f, true, true);
    }

    // --- Mesh shaders: set de los meshlets y sombras de las cascadas ---
    if (mesh_shaders_) {
        std::array<vk::DescriptorSetLayoutBinding, 4> meshlet_bindings{};
        for (std::uint32_t i = 0; i < meshlet_bindings.size(); ++i) {
            meshlet_bindings[i].binding = i;
            meshlet_bindings[i].descriptorType = vk::DescriptorType::eStorageBuffer;
            meshlet_bindings[i].descriptorCount = 1;
            meshlet_bindings[i].stageFlags = mesh_stages;
        }
        vk::DescriptorSetLayoutCreateInfo meshlet_layout_info{};
        meshlet_layout_info.setBindings(meshlet_bindings);
        meshlet_set_layout_ = vk::raii::DescriptorSetLayout(device.handle(), meshlet_layout_info);

        vk::PushConstantRange mesh_shadow_push{};
        mesh_shadow_push.stageFlags = mesh_stages;
        mesh_shadow_push.size = sizeof(GpuMeshletShadowPush);
        const std::array<vk::DescriptorSetLayout, 2> mesh_shadow_sets = {*frame_set_layout_, *meshlet_set_layout_};
        vk::PipelineLayoutCreateInfo mesh_shadow_layout_info{};
        mesh_shadow_layout_info.setSetLayouts(mesh_shadow_sets);
        mesh_shadow_layout_info.setPushConstantRanges(mesh_shadow_push);
        mesh_shadow_layout_ = vk::raii::PipelineLayout(device.handle(), mesh_shadow_layout_info);
        mesh_shadow_pipeline_ = createMeshShadowPipeline(device, shadow_format, clamp);

        // G-buffer: set 3 = comandos y contadores de cull.comp (los lee el task shader).
        std::array<vk::DescriptorSetLayoutBinding, 2> draw_bindings{};
        for (std::uint32_t i = 0; i < draw_bindings.size(); ++i) {
            draw_bindings[i].binding = i;
            draw_bindings[i].descriptorType = vk::DescriptorType::eStorageBuffer;
            draw_bindings[i].descriptorCount = 1;
            draw_bindings[i].stageFlags = vk::ShaderStageFlagBits::eTaskEXT;
        }
        vk::DescriptorSetLayoutCreateInfo draw_layout_info{};
        draw_layout_info.setBindings(draw_bindings);
        mesh_draw_set_layout_ = vk::raii::DescriptorSetLayout(device.handle(), draw_layout_info);

        vk::PushConstantRange geometry_push{};
        geometry_push.stageFlags = mesh_stages | vk::ShaderStageFlagBits::eFragment;
        geometry_push.size = sizeof(GpuSkinnedPush);
        const std::array<vk::DescriptorSetLayout, 4> geometry_sets = {*frame_set_layout_, *material_set_layout_,
                                                                      *meshlet_set_layout_, *mesh_draw_set_layout_};
        vk::PipelineLayoutCreateInfo geometry_layout_info{};
        geometry_layout_info.setSetLayouts(geometry_sets);
        geometry_layout_info.setPushConstantRanges(geometry_push);
        mesh_geometry_layout_ = vk::raii::PipelineLayout(device.handle(), geometry_layout_info);
        mesh_geometry_pipeline_ = createMeshGeometryPipeline(device);
        std::cout << "[Vulkan] Mesh shaders activos (geometria y sombras de las cascadas por meshlets)\n";
    }

    createGlassPipeline(device, hdr_format, gbuffer.depthFormat());
    outline_silhouette_pipeline_ = createOutlinePipeline(device, gbuffer.depthFormat(), false);
    outline_visible_pipeline_ = createOutlinePipeline(device, gbuffer.depthFormat(), true);
    pick_pipeline_ = createOutlinePipeline(device, gbuffer.depthFormat(), true, /*pick=*/true);

    std::cout << "[Vulkan] Pipelines de modelos con esqueleto creados (huesos en storage buffer)\n";
}

void SkinnedPass::createGeometryPipeline(const VulkanDevice& device, const GBuffer& gbuffer) {
    vk::PushConstantRange push_range{};
    push_range.stageFlags = geometry_push_stages_;
    push_range.size = sizeof(GpuSkinnedPush);

    const std::array<vk::DescriptorSetLayout, 2> set_layouts = {*frame_set_layout_,
                                                                *material_set_layout_};
    vk::PipelineLayoutCreateInfo layout_info{};
    layout_info.setSetLayouts(set_layouts);
    layout_info.setPushConstantRanges(push_range);
    geometry_layout_ = vk::raii::PipelineLayout(device.handle(), layout_info);

    const auto formats = gbuffer.colorFormats();
    gbuffer_color_formats_.assign(formats.begin(), formats.end());
    gbuffer_depth_format_ = gbuffer.depthFormat();

    const vk::raii::ShaderModule vertex_module = shaders::loadModule(device, "skinned.vert.spv");
    const vk::raii::ShaderModule fragment_module = shaders::loadModule(device, "skinned.frag.spv");
    geometry_pipeline_ = buildGeometryPipeline(device, vertex_module, fragment_module);
    if (tessellation_) {
        const vk::raii::ShaderModule control_module = shaders::loadModule(device, "skinned.tesc.spv");
        const vk::raii::ShaderModule evaluation_module = shaders::loadModule(device, "skinned.tese.spv");
        geometry_tess_pipeline_ = buildGeometryPipeline(device, vertex_module, fragment_module, GeometryVariant::Fill,
                                                        &control_module, &evaluation_module);
        std::cout << "[Vulkan] Relieve teselado disponible (materiales con teselacion)\n";
    }
    if (device.fillModeNonSolidSupported()) {
        geometry_wire_pipeline_ = buildGeometryPipeline(device, vertex_module, fragment_module, GeometryVariant::Wire);
        const vk::raii::ShaderModule wire_module = shaders::loadModule(device, "wire.frag.spv");
        wire_overlay_pipeline_ = buildGeometryPipeline(device, vertex_module, wire_module, GeometryVariant::WireOverlay);
    }
}

vk::raii::Pipeline SkinnedPass::createSurfacePipeline(const VulkanDevice& device,
                                                      const std::vector<std::uint32_t>& vertex_spirv,
                                                      const std::vector<std::uint32_t>& fragment_spirv) const {
    vk::ShaderModuleCreateInfo vertex_info{};
    vertex_info.setCode(vertex_spirv);
    const vk::raii::ShaderModule vertex_module(device.handle(), vertex_info);
    vk::ShaderModuleCreateInfo fragment_info{};
    fragment_info.setCode(fragment_spirv);
    const vk::raii::ShaderModule fragment_module(device.handle(), fragment_info);
    return buildGeometryPipeline(device, vertex_module, fragment_module);
}

vk::raii::Pipeline SkinnedPass::buildGeometryPipeline(const VulkanDevice& device,
                                                      const vk::raii::ShaderModule& vertex_module,
                                                      const vk::raii::ShaderModule& fragment_module,
                                                      GeometryVariant variant,
                                                      const vk::raii::ShaderModule* tess_control,
                                                      const vk::raii::ShaderModule* tess_evaluation) const {
    const bool lines = variant != GeometryVariant::Fill;
    const bool overlay = variant == GeometryVariant::WireOverlay;
    const bool tessellated = tess_control != nullptr && tess_evaluation != nullptr;
    std::vector<vk::PipelineShaderStageCreateInfo> stages = {stage(vk::ShaderStageFlagBits::eVertex, vertex_module)};
    if (tessellated) {
        stages.push_back(stage(vk::ShaderStageFlagBits::eTessellationControl, *tess_control));
        stages.push_back(stage(vk::ShaderStageFlagBits::eTessellationEvaluation, *tess_evaluation));
    }
    stages.push_back(stage(vk::ShaderStageFlagBits::eFragment, fragment_module));
    // Triangulos como parches de 3 puntos de control para la teselacion.
    vk::PipelineTessellationStateCreateInfo tessellation_state{};
    tessellation_state.patchControlPoints = 3;

    const VertexLayout layout;
    vk::PipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.setVertexBindingDescriptions(layout.binding);
    vertex_input.setVertexAttributeDescriptions(layout.attributes);

    vk::PipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.topology = tessellated ? vk::PrimitiveTopology::ePatchList : vk::PrimitiveTopology::eTriangleList;

    vk::PipelineViewportStateCreateInfo viewport_state{};
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;

    vk::PipelineRasterizationStateCreateInfo rasterization{};
    rasterization.polygonMode = lines ? vk::PolygonMode::eLine : vk::PolygonMode::eFill;
    // Las lineas encima de la imagen: un poco hacia la camara (si no, se
    // pelean con la superficie que ya esta en el depth).
    if (overlay) {
        rasterization.depthBiasEnable = VK_TRUE;
        rasterization.depthBiasConstantFactor = -2.0f;
        rasterization.depthBiasSlopeFactor = -1.5f;
    }
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
    depth_stencil.depthWriteEnable = overlay ? VK_FALSE : VK_TRUE;
    depth_stencil.depthCompareOp = overlay ? vk::CompareOp::eLessOrEqual : vk::CompareOp::eLess;

    vk::PipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.colorWriteMask =
        vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
        vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
    std::array<vk::PipelineColorBlendAttachmentState, GBuffer::kColorAttachmentCount>
        blend_attachments{};
    blend_attachments.fill(blend_attachment);

    vk::PipelineColorBlendStateCreateInfo color_blend{};
    color_blend.setAttachments(blend_attachments);
    // Encima de la imagen HDR: mezcla por alfa en el color; su alfa (la
    // distancia de la superficie) se queda como estaba.
    vk::PipelineColorBlendAttachmentState overlay_blend = blend_attachment;
    overlay_blend.blendEnable = VK_TRUE;
    overlay_blend.srcColorBlendFactor = vk::BlendFactor::eSrcAlpha;
    overlay_blend.dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
    overlay_blend.colorBlendOp = vk::BlendOp::eAdd;
    overlay_blend.srcAlphaBlendFactor = vk::BlendFactor::eZero;
    overlay_blend.dstAlphaBlendFactor = vk::BlendFactor::eOne;
    overlay_blend.alphaBlendOp = vk::BlendOp::eAdd;
    if (overlay) color_blend.setAttachments(overlay_blend);

    const std::array<vk::DynamicState, 2> dynamic_states = {vk::DynamicState::eViewport,
                                                            vk::DynamicState::eScissor};
    vk::PipelineDynamicStateCreateInfo dynamic_state{};
    dynamic_state.setDynamicStates(dynamic_states);

    vk::PipelineRenderingCreateInfo rendering_info{};
    rendering_info.setColorAttachmentFormats(gbuffer_color_formats_);
    if (overlay) rendering_info.setColorAttachmentFormats(hdr_format_);
    rendering_info.depthAttachmentFormat = gbuffer_depth_format_;

    vk::GraphicsPipelineCreateInfo pipeline_info{};
    pipeline_info.pNext = &rendering_info;
    pipeline_info.setStages(stages);
    pipeline_info.pVertexInputState = &vertex_input;
    pipeline_info.pInputAssemblyState = &input_assembly;
    if (tessellated) pipeline_info.pTessellationState = &tessellation_state;
    pipeline_info.pViewportState = &viewport_state;
    pipeline_info.pRasterizationState = &rasterization;
    pipeline_info.pMultisampleState = &multisample;
    pipeline_info.pDepthStencilState = &depth_stencil;
    pipeline_info.pColorBlendState = &color_blend;
    pipeline_info.pDynamicState = &dynamic_state;
    pipeline_info.layout = *geometry_layout_;

    return vk::raii::Pipeline(device.handle(), device.pipelineCache(), pipeline_info);
}

void SkinnedPass::createGlassPipeline(const VulkanDevice& device, vk::Format color_format,
                                      vk::Format depth_format) {
    // --- Set 2: lo que el vidrio necesita para reflejar la escena ---
    using Type = vk::DescriptorType;
    constexpr std::array<Type, kGlassBindingCount> kTypes = {
        Type::eUniformBuffer,        Type::eUniformBuffer,        Type::eUniformBuffer,
        Type::eCombinedImageSampler, Type::eCombinedImageSampler, Type::eCombinedImageSampler,
        Type::eCombinedImageSampler, Type::eCombinedImageSampler, Type::eCombinedImageSampler,
        Type::eCombinedImageSampler};
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

    glass_pipeline_ = vk::raii::Pipeline(device.handle(), device.pipelineCache(), pipeline_info);
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

    return vk::raii::Pipeline(device.handle(), device.pipelineCache(), pipeline_info);
}

vk::raii::Pipeline SkinnedPass::createShadowPipeline(const VulkanDevice& device,
                                                     vk::Format depth_format, bool depth_clamp,
                                                     float slope_bias, bool alpha_tested, bool tessellated) const {
    const vk::raii::ShaderModule vertex_module =
        shaders::loadModule(device, tessellated ? "skinned_shadow_tess.vert.spv" : "skinned_shadow.vert.spv");
    const vk::raii::ShaderModule fragment_module =
        shaders::loadModule(device, "skinned_shadow.frag.spv");
    std::vector<vk::PipelineShaderStageCreateInfo> shadow_stages = {
        stage(vk::ShaderStageFlagBits::eVertex, vertex_module)};
    // Relieve teselado: la teselacion sube los vertices antes de proyectar.
    vk::raii::ShaderModule control_module{nullptr};
    vk::raii::ShaderModule evaluation_module{nullptr};
    if (tessellated) {
        control_module = shaders::loadModule(device, "skinned_shadow.tesc.spv");
        evaluation_module = shaders::loadModule(device, "skinned_shadow.tese.spv");
        shadow_stages.push_back(stage(vk::ShaderStageFlagBits::eTessellationControl, control_module));
        shadow_stages.push_back(stage(vk::ShaderStageFlagBits::eTessellationEvaluation, evaluation_module));
    }
    // Lo opaco: sin fragment shader (la profundidad la escribe la GPU).
    if (alpha_tested) shadow_stages.push_back(stage(vk::ShaderStageFlagBits::eFragment, fragment_module));
    vk::PipelineTessellationStateCreateInfo tessellation_state{};
    tessellation_state.patchControlPoints = 3;

    const VertexLayout layout;
    vk::PipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.setVertexBindingDescriptions(layout.binding);
    vertex_input.setVertexAttributeDescriptions(layout.attributes);

    vk::PipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.topology = tessellated ? vk::PrimitiveTopology::ePatchList : vk::PrimitiveTopology::eTriangleList;

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
    if (tessellated) pipeline_info.pTessellationState = &tessellation_state;
    pipeline_info.pViewportState = &viewport_state;
    pipeline_info.pRasterizationState = &rasterization;
    pipeline_info.pMultisampleState = &multisample;
    pipeline_info.pDepthStencilState = &depth_stencil;
    pipeline_info.pColorBlendState = &color_blend;
    pipeline_info.pDynamicState = &dynamic_state;
    pipeline_info.layout = *shadow_layout_;

    return vk::raii::Pipeline(device.handle(), device.pipelineCache(), pipeline_info);
}

vk::raii::Pipeline SkinnedPass::createMeshShadowPipeline(const VulkanDevice& device, vk::Format depth_format,
                                                         bool depth_clamp) const {
    const vk::raii::ShaderModule task_module = shaders::loadModule(device, "shadow_meshlet.task.spv");
    const vk::raii::ShaderModule mesh_module = shaders::loadModule(device, "shadow_meshlet.mesh.spv");
    const std::array<vk::PipelineShaderStageCreateInfo, 2> stages = {
        stage(vk::ShaderStageFlagBits::eTaskEXT, task_module), stage(vk::ShaderStageFlagBits::eMeshEXT, mesh_module)};

    vk::PipelineViewportStateCreateInfo viewport_state{};
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;

    // Igual que la de las cascadas de solo profundidad (createShadowPipeline).
    vk::PipelineRasterizationStateCreateInfo rasterization{};
    rasterization.polygonMode = vk::PolygonMode::eFill;
    rasterization.cullMode = vk::CullModeFlagBits::eNone;
    rasterization.frontFace = vk::FrontFace::eCounterClockwise;
    rasterization.lineWidth = 1.0f;
    rasterization.depthBiasEnable = VK_TRUE;
    rasterization.depthBiasConstantFactor = 0.6f;
    rasterization.depthBiasSlopeFactor = 1.1f;
    rasterization.depthClampEnable = depth_clamp ? VK_TRUE : VK_FALSE;

    vk::PipelineMultisampleStateCreateInfo multisample{};
    multisample.rasterizationSamples = vk::SampleCountFlagBits::e1;

    vk::PipelineDepthStencilStateCreateInfo depth_stencil{};
    depth_stencil.depthTestEnable = VK_TRUE;
    depth_stencil.depthWriteEnable = VK_TRUE;
    depth_stencil.depthCompareOp = vk::CompareOp::eLess;

    vk::PipelineColorBlendStateCreateInfo color_blend{};
    const std::array<vk::DynamicState, 2> dynamic_states = {vk::DynamicState::eViewport, vk::DynamicState::eScissor};
    vk::PipelineDynamicStateCreateInfo dynamic_state{};
    dynamic_state.setDynamicStates(dynamic_states);

    vk::PipelineRenderingCreateInfo rendering_info{};
    rendering_info.depthAttachmentFormat = depth_format;

    // Con mesh shaders no hay entrada de vertices ni ensamblado.
    vk::GraphicsPipelineCreateInfo pipeline_info{};
    pipeline_info.pNext = &rendering_info;
    pipeline_info.setStages(stages);
    pipeline_info.pViewportState = &viewport_state;
    pipeline_info.pRasterizationState = &rasterization;
    pipeline_info.pMultisampleState = &multisample;
    pipeline_info.pDepthStencilState = &depth_stencil;
    pipeline_info.pColorBlendState = &color_blend;
    pipeline_info.pDynamicState = &dynamic_state;
    pipeline_info.layout = *mesh_shadow_layout_;
    return vk::raii::Pipeline(device.handle(), device.pipelineCache(), pipeline_info);
}

vk::raii::Pipeline SkinnedPass::createMeshGeometryPipeline(const VulkanDevice& device) const {
    const vk::raii::ShaderModule task_module = shaders::loadModule(device, "gbuffer_meshlet.task.spv");
    const vk::raii::ShaderModule mesh_module = shaders::loadModule(device, "gbuffer_meshlet.mesh.spv");
    const vk::raii::ShaderModule fragment_module = shaders::loadModule(device, "skinned.frag.spv");
    const std::array<vk::PipelineShaderStageCreateInfo, 3> stages = {
        stage(vk::ShaderStageFlagBits::eTaskEXT, task_module), stage(vk::ShaderStageFlagBits::eMeshEXT, mesh_module),
        stage(vk::ShaderStageFlagBits::eFragment, fragment_module)};

    vk::PipelineViewportStateCreateInfo viewport_state{};
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;

    // El mismo estado que buildGeometryPipeline (relleno, doble cara).
    vk::PipelineRasterizationStateCreateInfo rasterization{};
    rasterization.polygonMode = vk::PolygonMode::eFill;
    rasterization.cullMode = vk::CullModeFlagBits::eNone;
    rasterization.frontFace = vk::FrontFace::eCounterClockwise;
    rasterization.lineWidth = 1.0f;

    vk::PipelineMultisampleStateCreateInfo multisample{};
    multisample.rasterizationSamples = vk::SampleCountFlagBits::e1;

    vk::PipelineDepthStencilStateCreateInfo depth_stencil{};
    depth_stencil.depthTestEnable = VK_TRUE;
    depth_stencil.depthWriteEnable = VK_TRUE;
    depth_stencil.depthCompareOp = vk::CompareOp::eLess;

    vk::PipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                                      vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
    std::vector<vk::PipelineColorBlendAttachmentState> blend_attachments(gbuffer_color_formats_.size(),
                                                                         blend_attachment);
    vk::PipelineColorBlendStateCreateInfo color_blend{};
    color_blend.setAttachments(blend_attachments);

    const std::array<vk::DynamicState, 2> dynamic_states = {vk::DynamicState::eViewport, vk::DynamicState::eScissor};
    vk::PipelineDynamicStateCreateInfo dynamic_state{};
    dynamic_state.setDynamicStates(dynamic_states);

    vk::PipelineRenderingCreateInfo rendering_info{};
    rendering_info.setColorAttachmentFormats(gbuffer_color_formats_);
    rendering_info.depthAttachmentFormat = gbuffer_depth_format_;

    vk::GraphicsPipelineCreateInfo pipeline_info{};
    pipeline_info.pNext = &rendering_info;
    pipeline_info.setStages(stages);
    pipeline_info.pViewportState = &viewport_state;
    pipeline_info.pRasterizationState = &rasterization;
    pipeline_info.pMultisampleState = &multisample;
    pipeline_info.pDepthStencilState = &depth_stencil;
    pipeline_info.pColorBlendState = &color_blend;
    pipeline_info.pDynamicState = &dynamic_state;
    pipeline_info.layout = *mesh_geometry_layout_;
    return vk::raii::Pipeline(device.handle(), device.pipelineCache(), pipeline_info);
}

void SkinnedPass::destroy() {
    for (vk::raii::Pipeline& pipeline : shadow_tess_pipelines_) pipeline = nullptr;
    geometry_tess_pipeline_ = nullptr;
    mesh_geometry_pipeline_ = nullptr;
    mesh_geometry_layout_ = nullptr;
    mesh_draw_set_layout_ = nullptr;
    mesh_shadow_pipeline_ = nullptr;
    mesh_shadow_layout_ = nullptr;
    meshlet_set_layout_ = nullptr;
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
    geometry_wire_pipeline_ = nullptr;
    wire_overlay_pipeline_ = nullptr;
    geometry_layout_ = nullptr;
    material_set_layout_ = nullptr;
    frame_set_layout_ = nullptr;
    sampler_ = nullptr;
}

}  // namespace cramion::gfx
