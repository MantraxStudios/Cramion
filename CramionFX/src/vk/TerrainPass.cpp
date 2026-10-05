#include "CramionFX/vk/VulkanCompat.h"
#include "CramionFX/vk/TerrainPass.h"
#include "CramionFX/vk/GBuffer.h"

#include "CramionFX/asset/ImageFile.h"
#include "CramionFX/core/Frustum.h"
#include "CramionFX/vk/VulkanDevice.h"
#include "CramionFX/vk/VulkanShader.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <iostream>

namespace cramion::gfx {

namespace {

constexpr std::uint32_t kPatchCells = 32;      // celdas por lado de un trozo
constexpr std::uint32_t kLayerSize = 512;      // texturas de capa (se reescalan)
constexpr vk::Format kHeightFormat = vk::Format::eR32Sfloat;
constexpr vk::Format kSplatFormat = vk::Format::eR8G8B8A8Unorm;
constexpr vk::Format kLayerFormat = vk::Format::eR8G8B8A8Unorm;

// Debe coincidir con TerrainParams de terrain.vert / terrain.frag.
struct GpuTerrainParams {
    core::Vec4 origin_size;  // xyz origen, w tamano
    core::Vec4 info;         // x altura maxima, y resolucion, z 1/resolucion, w capas
    core::Vec4 layer_params[kMaxTerrainLayers];  // x repeticion (m), y rugosidad, z metal, w fuerza normal
    core::Vec4 layer_tint[kMaxTerrainLayers];    // rgb tinte, a = tiene color
    core::Vec4 layer_extra[kMaxTerrainLayers];   // x = tiene normal map
};

// Debe coincidir con el push de terrain.vert.
struct GpuTerrainPush {
    core::Vec4 chunk;                 // xy = esquina (0..1), z = tamano (0..1), w = faldon (m)
    core::Mat4 light_view_projection;
    std::uint32_t shadow = 0;
    std::uint32_t pad[3] = {0, 0, 0};
};

void barrier(const vk::raii::CommandBuffer& cmd, vk::Image image, vk::ImageLayout from, vk::ImageLayout to,
             vk::PipelineStageFlags2 src_stage, vk::AccessFlags2 src_access, vk::PipelineStageFlags2 dst_stage,
             vk::AccessFlags2 dst_access, std::uint32_t base_mip = 0, std::uint32_t mips = 1,
             std::uint32_t layers = 1) {
    vk::ImageMemoryBarrier2 b{};
    b.srcStageMask = src_stage;
    b.srcAccessMask = src_access;
    b.dstStageMask = dst_stage;
    b.dstAccessMask = dst_access;
    b.oldLayout = from;
    b.newLayout = to;
    b.image = image;
    b.subresourceRange = vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, base_mip, mips, 0, layers};
    vk::DependencyInfo dependency{};
    dependency.setImageMemoryBarriers(b);
    compat::pipelineBarrier(cmd, dependency);
}

// Imagen RGBA8 (cualquier tamano) a kLayerSize x kLayerSize (bilineal).
std::vector<std::uint8_t> resizeSquare(const asset::ImageRgba8& image) {
    std::vector<std::uint8_t> out(static_cast<std::size_t>(kLayerSize) * kLayerSize * 4);
    for (std::uint32_t y = 0; y < kLayerSize; ++y) {
        const float fy = (static_cast<float>(y) + 0.5f) / kLayerSize * image.height - 0.5f;
        const auto y0 = static_cast<std::uint32_t>(std::clamp(std::floor(fy), 0.0f, static_cast<float>(image.height - 1)));
        const std::uint32_t y1 = std::min(y0 + 1, image.height - 1);
        const float ty = std::clamp(fy - static_cast<float>(y0), 0.0f, 1.0f);
        for (std::uint32_t x = 0; x < kLayerSize; ++x) {
            const float fx = (static_cast<float>(x) + 0.5f) / kLayerSize * image.width - 0.5f;
            const auto x0 = static_cast<std::uint32_t>(std::clamp(std::floor(fx), 0.0f, static_cast<float>(image.width - 1)));
            const std::uint32_t x1 = std::min(x0 + 1, image.width - 1);
            const float tx = std::clamp(fx - static_cast<float>(x0), 0.0f, 1.0f);
            for (int c = 0; c < 4; ++c) {
                const auto at = [&](std::uint32_t px, std::uint32_t py) {
                    return static_cast<float>(image.pixels[(static_cast<std::size_t>(py) * image.width + px) * 4 + c]);
                };
                const float top = at(x0, y0) + (at(x1, y0) - at(x0, y0)) * tx;
                const float bottom = at(x0, y1) + (at(x1, y1) - at(x0, y1)) * tx;
                out[(static_cast<std::size_t>(y) * kLayerSize + x) * 4 + c] =
                    static_cast<std::uint8_t>(std::clamp(top + (bottom - top) * ty + 0.5f, 0.0f, 255.0f));
            }
        }
    }
    return out;
}

std::vector<std::uint8_t> solidLayer(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a) {
    std::vector<std::uint8_t> out(static_cast<std::size_t>(kLayerSize) * kLayerSize * 4);
    for (std::size_t i = 0; i < out.size(); i += 4) {
        out[i] = r;
        out[i + 1] = g;
        out[i + 2] = b;
        out[i + 3] = a;
    }
    return out;
}

}  // namespace

// -----------------------------------------------------------------------------
// Creacion
// -----------------------------------------------------------------------------

void TerrainPass::create(const VulkanDevice& device, const vk::raii::DescriptorSetLayout& frame_layout,
                         std::array<vk::Format, GBuffer::kColorAttachmentCount> gbuffer_formats, vk::Format depth_format,
                         vk::Format shadow_format, std::uint32_t frames_in_flight) {
    destroy();
    device_ = &device;
    frames_ = frames_in_flight;

    // Set 1: alturas y parametros (vertice y fragmento), pesos y capas.
    // (La hierba lee alturas, parametros y pesos en su compute.)
    const auto stage_all =
        vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment | vk::ShaderStageFlagBits::eCompute;
    const auto stage_splat = vk::ShaderStageFlagBits::eFragment | vk::ShaderStageFlagBits::eCompute;
    const std::array<vk::DescriptorSetLayoutBinding, 6> bindings = {{
        {0, vk::DescriptorType::eCombinedImageSampler, 1, stage_all},
        {1, vk::DescriptorType::eUniformBuffer, 1, stage_all},
        {2, vk::DescriptorType::eCombinedImageSampler, 1, stage_splat},
        {3, vk::DescriptorType::eCombinedImageSampler, 1, stage_splat},
        {4, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eFragment},
        {5, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eFragment},
    }};
    vk::DescriptorSetLayoutCreateInfo set_info{};
    set_info.setBindings(bindings);
    set_layout_ = vk::raii::DescriptorSetLayout(device.handle(), set_info);

    const std::array<vk::DescriptorSetLayout, 2> set_layouts = {*frame_layout, *set_layout_};
    vk::PushConstantRange push{};
    push.stageFlags = vk::ShaderStageFlagBits::eVertex;
    push.size = sizeof(GpuTerrainPush);
    vk::PipelineLayoutCreateInfo layout_info{};
    layout_info.setSetLayouts(set_layouts);
    layout_info.setPushConstantRanges(push);
    layout_ = vk::raii::PipelineLayout(device.handle(), layout_info);

    // Hasta 16 terrenos con un set por frame en vuelo.
    const std::uint32_t max_sets = 16 * frames_in_flight;
    const std::array<vk::DescriptorPoolSize, 2> sizes = {{
        {vk::DescriptorType::eCombinedImageSampler, max_sets * 5},
        {vk::DescriptorType::eUniformBuffer, max_sets},
    }};
    vk::DescriptorPoolCreateInfo pool_info{};
    pool_info.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    pool_info.maxSets = max_sets;
    pool_info.setPoolSizes(sizes);
    pool_ = vk::raii::DescriptorPool(device.handle(), pool_info);

    vk::SamplerCreateInfo clamp{};
    clamp.magFilter = vk::Filter::eLinear;
    clamp.minFilter = vk::Filter::eLinear;
    clamp.mipmapMode = vk::SamplerMipmapMode::eNearest;
    clamp.addressModeU = clamp.addressModeV = clamp.addressModeW = vk::SamplerAddressMode::eClampToEdge;
    clamp_sampler_ = vk::raii::Sampler(device.handle(), clamp);

    vk::SamplerCreateInfo repeat{};
    repeat.magFilter = vk::Filter::eLinear;
    repeat.minFilter = vk::Filter::eLinear;
    repeat.mipmapMode = vk::SamplerMipmapMode::eLinear;
    repeat.addressModeU = repeat.addressModeV = repeat.addressModeW = vk::SamplerAddressMode::eRepeat;
    repeat.maxLod = VK_LOD_CLAMP_NONE;
    const vk::PhysicalDeviceFeatures features = device.physicalDevice().getFeatures();
    if (features.samplerAnisotropy) {
        repeat.anisotropyEnable = VK_TRUE;
        repeat.maxAnisotropy = std::min(8.0f, device.physicalDevice().getProperties().limits.maxSamplerAnisotropy);
    }
    repeat_sampler_ = vk::raii::Sampler(device.handle(), repeat);

    createPipelines(device, gbuffer_formats, depth_format, shadow_format);
    createGrassPipelines(device, frame_layout, gbuffer_formats, depth_format);
    createPatchMesh(device);
    staging_.resize(frames_in_flight);
}

void TerrainPass::destroy() {
    terrains_.clear();
    grass_pipeline_ = nullptr;
    grass_draw_layout_ = nullptr;
    grass_cull_pipeline_ = nullptr;
    grass_cull_layout_ = nullptr;
    grass_pool_ = nullptr;
    grass_set_layout_ = nullptr;
    grass_interactors_.clear();
    uploads_.clear();
    for (VulkanBuffer& buffer : staging_) buffer.destroy();
    staging_.clear();
    patch_vertices_.destroy();
    patch_indices_.destroy();
    shadow_pipeline_ = nullptr;
    gbuffer_pipeline_ = nullptr;
    gbuffer_wire_pipeline_ = nullptr;
    repeat_sampler_ = nullptr;
    clamp_sampler_ = nullptr;
    pool_ = nullptr;
    layout_ = nullptr;
    set_layout_ = nullptr;
    device_ = nullptr;
}

void TerrainPass::createPipelines(const VulkanDevice& device, std::array<vk::Format, GBuffer::kColorAttachmentCount> gbuffer_formats,
                                  vk::Format depth_format, vk::Format shadow_format) {
    const vk::raii::ShaderModule vertex = shaders::loadModule(device, "terrain.vert.spv");
    const vk::raii::ShaderModule fragment = shaders::loadModule(device, "terrain.frag.spv");

    const vk::VertexInputBindingDescription binding{0, sizeof(float) * 3, vk::VertexInputRate::eVertex};
    const vk::VertexInputAttributeDescription attribute{0, 0, vk::Format::eR32G32B32Sfloat, 0};
    vk::PipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.setVertexBindingDescriptions(binding);
    vertex_input.setVertexAttributeDescriptions(attribute);
    vk::PipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.topology = vk::PrimitiveTopology::eTriangleList;
    vk::PipelineViewportStateCreateInfo viewport{};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    vk::PipelineMultisampleStateCreateInfo multisample{};
    multisample.rasterizationSamples = vk::SampleCountFlagBits::e1;
    const std::array<vk::DynamicState, 2> dynamic_states = {vk::DynamicState::eViewport, vk::DynamicState::eScissor};
    vk::PipelineDynamicStateCreateInfo dynamic{};
    dynamic.setDynamicStates(dynamic_states);

    // --- G-buffer ---
    {
        const std::array<vk::PipelineShaderStageCreateInfo, 2> stages = {{
            {{}, vk::ShaderStageFlagBits::eVertex, *vertex, "main"},
            {{}, vk::ShaderStageFlagBits::eFragment, *fragment, "main"},
        }};
        vk::PipelineRasterizationStateCreateInfo raster{};
        raster.polygonMode = vk::PolygonMode::eFill;
        raster.cullMode = vk::CullModeFlagBits::eNone;  // faldones y vistas por debajo
        raster.lineWidth = 1.0f;
        vk::PipelineDepthStencilStateCreateInfo depth{};
        depth.depthTestEnable = VK_TRUE;
        depth.depthWriteEnable = VK_TRUE;
        depth.depthCompareOp = vk::CompareOp::eLessOrEqual;
        std::array<vk::PipelineColorBlendAttachmentState, GBuffer::kColorAttachmentCount> blends{};
        for (auto& b : blends) {
            b.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                               vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
        }
        vk::PipelineColorBlendStateCreateInfo blend{};
        blend.setAttachments(blends);
        vk::PipelineRenderingCreateInfo rendering{};
        rendering.setColorAttachmentFormats(gbuffer_formats);
        // Modo compatible: 4 destinos (GBuffer::activeColorAttachments).
        rendering.colorAttachmentCount = GBuffer::activeColorAttachments();
        blend.attachmentCount = GBuffer::activeColorAttachments();
        rendering.depthAttachmentFormat = depth_format;
        vk::GraphicsPipelineCreateInfo info{};
        info.pNext = &rendering;
        info.setStages(stages);
        info.pVertexInputState = &vertex_input;
        info.pInputAssemblyState = &input_assembly;
        info.pViewportState = &viewport;
        info.pRasterizationState = &raster;
        info.pMultisampleState = &multisample;
        info.pDepthStencilState = &depth;
        info.pColorBlendState = &blend;
        info.pDynamicState = &dynamic;
        info.layout = *layout_;
        gbuffer_pipeline_ = compat::makeGraphicsPipeline(device, info);
        if (device.fillModeNonSolidSupported()) {
            raster.polygonMode = vk::PolygonMode::eLine;
            gbuffer_wire_pipeline_ = compat::makeGraphicsPipeline(device, info);
            raster.polygonMode = vk::PolygonMode::eFill;
        }
    }
    // --- Sombras (solo profundidad) ---
    {
        const vk::PipelineShaderStageCreateInfo stage{{}, vk::ShaderStageFlagBits::eVertex, *vertex, "main"};
        vk::PipelineRasterizationStateCreateInfo raster{};
        raster.polygonMode = vk::PolygonMode::eFill;
        raster.cullMode = vk::CullModeFlagBits::eNone;
        raster.lineWidth = 1.0f;
        raster.depthClampEnable = device.depthClampSupported() ? VK_TRUE : VK_FALSE;
        raster.depthBiasEnable = VK_TRUE;
        raster.depthBiasConstantFactor = 0.8f;
        raster.depthBiasSlopeFactor = 1.5f;
        vk::PipelineDepthStencilStateCreateInfo depth{};
        depth.depthTestEnable = VK_TRUE;
        depth.depthWriteEnable = VK_TRUE;
        depth.depthCompareOp = vk::CompareOp::eLessOrEqual;
        vk::PipelineColorBlendStateCreateInfo blend{};
        vk::PipelineRenderingCreateInfo rendering{};
        rendering.depthAttachmentFormat = shadow_format;
        vk::GraphicsPipelineCreateInfo info{};
        info.pNext = &rendering;
        info.setStages(stage);
        info.pVertexInputState = &vertex_input;
        info.pInputAssemblyState = &input_assembly;
        info.pViewportState = &viewport;
        info.pRasterizationState = &raster;
        info.pMultisampleState = &multisample;
        info.pDepthStencilState = &depth;
        info.pColorBlendState = &blend;
        info.pDynamicState = &dynamic;
        info.layout = *layout_;
        shadow_pipeline_ = compat::makeGraphicsPipeline(device, info);
    }
}

// -----------------------------------------------------------------------------
// Hierba
// -----------------------------------------------------------------------------

namespace {

// Debe coincidir con GrassParams (grass_common.glsl).
constexpr std::uint32_t kMaxGrassInteractors = 32;
struct GpuGrassParams {
    core::Vec4 grid;
    core::Vec4 origin;
    core::Vec4 shape;
    core::Vec4 base_color;
    core::Vec4 tip_color;
    core::Vec4 dry_color;
    core::Vec4 wind;
    core::Vec4 camera_pos;
    std::int32_t layers[4];
    core::Mat4 view_projection;
    core::Vec4 interactors[kMaxGrassInteractors];
};
struct GpuGrassBlade {
    core::Vec4 position;
    core::Vec4 push;
};
static_assert(sizeof(GpuGrassBlade) == 32, "GpuGrassBlade debe coincidir con grass_common.glsl");
struct DrawIndirect {
    std::uint32_t vertex_count;
    std::uint32_t instance_count;
    std::uint32_t first_vertex;
    std::uint32_t first_instance;
};
constexpr std::uint32_t kNearSegments = 7;  // 7 tramos + punta = 45 vertices
constexpr std::uint32_t kFarSegments = 3;   // 3 tramos + punta = 21

}  // namespace

void TerrainPass::createGrassPipelines(const VulkanDevice& device, const vk::raii::DescriptorSetLayout& frame_layout,
                                       std::array<vk::Format, GBuffer::kColorAttachmentCount> gbuffer_formats,
                                       vk::Format depth_format) {
    const auto compute = vk::ShaderStageFlagBits::eCompute;
    const vk::ShaderStageFlags all = vk::ShaderStageFlagBits::eCompute | vk::ShaderStageFlagBits::eVertex |
                                     vk::ShaderStageFlagBits::eFragment;
    const std::array<vk::DescriptorSetLayoutBinding, 3> bindings = {{
        {0, vk::DescriptorType::eUniformBuffer, 1, all},
        {1, vk::DescriptorType::eStorageBuffer, 1, compute | vk::ShaderStageFlagBits::eVertex},
        {2, vk::DescriptorType::eStorageBuffer, 1, compute},
    }};
    vk::DescriptorSetLayoutCreateInfo set_info{};
    set_info.setBindings(bindings);
    grass_set_layout_ = vk::raii::DescriptorSetLayout(device.handle(), set_info);

    const std::uint32_t max_sets = 16 * frames_;
    const std::array<vk::DescriptorPoolSize, 2> sizes = {{
        {vk::DescriptorType::eUniformBuffer, max_sets},
        {vk::DescriptorType::eStorageBuffer, max_sets * 2},
    }};
    vk::DescriptorPoolCreateInfo pool_info{};
    pool_info.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    pool_info.maxSets = max_sets;
    pool_info.setPoolSizes(sizes);
    grass_pool_ = vk::raii::DescriptorPool(device.handle(), pool_info);

    // --- Compute: set 0 el del terreno, set 1 el de la hierba ---
    {
        const std::array<vk::DescriptorSetLayout, 2> sets = {*set_layout_, *grass_set_layout_};
        vk::PipelineLayoutCreateInfo layout_info{};
        layout_info.setSetLayouts(sets);
        grass_cull_layout_ = vk::raii::PipelineLayout(device.handle(), layout_info);
        const vk::raii::ShaderModule module = shaders::loadModule(device, "grass_cull.comp.spv");
        vk::ComputePipelineCreateInfo info{};
        info.stage = vk::PipelineShaderStageCreateInfo{{}, compute, *module, "main"};
        info.layout = *grass_cull_layout_;
        grass_cull_pipeline_ = vk::raii::Pipeline(device.handle(), device.pipelineCache(), info);
    }
    // --- Dibujo: set 0 el del frame, 1 el del terreno, 2 el de la hierba ---
    {
        const std::array<vk::DescriptorSetLayout, 3> sets = {*frame_layout, *set_layout_, *grass_set_layout_};
        vk::PushConstantRange push{vk::ShaderStageFlagBits::eVertex, 0, sizeof(std::uint32_t)};
        vk::PipelineLayoutCreateInfo layout_info{};
        layout_info.setSetLayouts(sets);
        layout_info.setPushConstantRanges(push);
        grass_draw_layout_ = vk::raii::PipelineLayout(device.handle(), layout_info);

        const vk::raii::ShaderModule vertex = shaders::loadModule(device, "grass.vert.spv");
        const vk::raii::ShaderModule fragment = shaders::loadModule(device, "grass.frag.spv");
        const std::array<vk::PipelineShaderStageCreateInfo, 2> stages = {{
            {{}, vk::ShaderStageFlagBits::eVertex, *vertex, "main"},
            {{}, vk::ShaderStageFlagBits::eFragment, *fragment, "main"},
        }};
        vk::PipelineVertexInputStateCreateInfo vertex_input{};  // sin vertices: salen del indice
        vk::PipelineInputAssemblyStateCreateInfo input_assembly{};
        input_assembly.topology = vk::PrimitiveTopology::eTriangleList;
        vk::PipelineViewportStateCreateInfo viewport{};
        viewport.viewportCount = 1;
        viewport.scissorCount = 1;
        vk::PipelineRasterizationStateCreateInfo raster{};
        raster.polygonMode = vk::PolygonMode::eFill;
        raster.cullMode = vk::CullModeFlagBits::eNone;  // las briznas se ven por las dos caras
        raster.lineWidth = 1.0f;
        vk::PipelineMultisampleStateCreateInfo multisample{};
        multisample.rasterizationSamples = vk::SampleCountFlagBits::e1;
        vk::PipelineDepthStencilStateCreateInfo depth{};
        depth.depthTestEnable = VK_TRUE;
        depth.depthWriteEnable = VK_TRUE;
        depth.depthCompareOp = vk::CompareOp::eLessOrEqual;
        std::array<vk::PipelineColorBlendAttachmentState, GBuffer::kColorAttachmentCount> blends{};
        for (auto& b : blends) {
            b.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                               vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
        }
        vk::PipelineColorBlendStateCreateInfo blend{};
        blend.setAttachments(blends);
        const std::array<vk::DynamicState, 2> dynamic_states = {vk::DynamicState::eViewport, vk::DynamicState::eScissor};
        vk::PipelineDynamicStateCreateInfo dynamic{};
        dynamic.setDynamicStates(dynamic_states);
        vk::PipelineRenderingCreateInfo rendering{};
        rendering.setColorAttachmentFormats(gbuffer_formats);
        // Modo compatible: 4 destinos (GBuffer::activeColorAttachments).
        rendering.colorAttachmentCount = GBuffer::activeColorAttachments();
        blend.attachmentCount = GBuffer::activeColorAttachments();
        rendering.depthAttachmentFormat = depth_format;
        vk::GraphicsPipelineCreateInfo info{};
        info.pNext = &rendering;
        info.setStages(stages);
        info.pVertexInputState = &vertex_input;
        info.pInputAssemblyState = &input_assembly;
        info.pViewportState = &viewport;
        info.pRasterizationState = &raster;
        info.pMultisampleState = &multisample;
        info.pDepthStencilState = &depth;
        info.pColorBlendState = &blend;
        info.pDynamicState = &dynamic;
        info.layout = *grass_draw_layout_;
        grass_pipeline_ = compat::makeGraphicsPipeline(device, info);
    }
}

bool TerrainPass::ensureGrass(Terrain& t) {
    const GrassDesc& g = t.desc.grass;
    const std::uint32_t capacity = std::clamp<std::uint32_t>(g.max_blades / 2, 1024, 4000000);
    if (t.grass && t.grass->capacity == capacity) return true;
    device_->waitIdle();
    auto grass = std::make_unique<Grass>();
    grass->capacity = capacity;
    grass->blades.create(*device_, static_cast<vk::DeviceSize>(capacity) * 2 * sizeof(GpuGrassBlade),
                         vk::BufferUsageFlagBits::eStorageBuffer, vk::MemoryPropertyFlagBits::eDeviceLocal);
    grass->args.create(*device_, sizeof(DrawIndirect) * 2,
                       vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eIndirectBuffer |
                           vk::BufferUsageFlagBits::eTransferDst,
                       vk::MemoryPropertyFlagBits::eDeviceLocal);
    grass->params.resize(frames_);
    for (VulkanBuffer& b : grass->params) {
        b.create(*device_, sizeof(GpuGrassParams), vk::BufferUsageFlagBits::eUniformBuffer,
                 vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
    }
    const std::vector<vk::DescriptorSetLayout> layouts(frames_, *grass_set_layout_);
    vk::DescriptorSetAllocateInfo alloc{};
    alloc.descriptorPool = *grass_pool_;
    alloc.setSetLayouts(layouts);
    try {
        grass->sets = vk::raii::DescriptorSets(device_->handle(), alloc);
    } catch (const std::exception&) {
        return false;  // mas de 16 terrenos con hierba
    }
    for (std::uint32_t f = 0; f < frames_; ++f) {
        const vk::DescriptorBufferInfo params{*grass->params[f].handle(), 0, sizeof(GpuGrassParams)};
        const vk::DescriptorBufferInfo blades{*grass->blades.handle(), 0, VK_WHOLE_SIZE};
        const vk::DescriptorBufferInfo args{*grass->args.handle(), 0, VK_WHOLE_SIZE};
        const std::array<vk::WriteDescriptorSet, 3> writes = {{
            {*grass->sets[f], 0, 0, 1, vk::DescriptorType::eUniformBuffer, nullptr, &params},
            {*grass->sets[f], 1, 0, 1, vk::DescriptorType::eStorageBuffer, nullptr, &blades},
            {*grass->sets[f], 2, 0, 1, vk::DescriptorType::eStorageBuffer, nullptr, &args},
        }};
        device_->handle().updateDescriptorSets(writes, nullptr);
    }
    t.grass = std::move(grass);
    return true;
}

void TerrainPass::setGrassInteractors(const std::vector<core::Vec4>& spheres) {
    grass_interactors_.assign(spheres.begin(), spheres.begin() + std::min<std::size_t>(spheres.size(), kMaxGrassInteractors));
}

void TerrainPass::recordGrassCull(const vk::raii::CommandBuffer& cmd, std::uint32_t frame,
                                  const core::Vec3& camera_position, const core::Mat4& view_projection,
                                  float delta_seconds) {
    if (device_ != nullptr && !device_->drawIndirectFirstInstanceSupported()) return;  // sin firstInstance (algunos moviles)
    grass_previous_seconds_ = grass_seconds_;
    grass_seconds_ += std::clamp(delta_seconds, 0.0f, 0.1f);
    if (!*grass_cull_pipeline_) return;
    for (auto& [id, terrain] : terrains_) {
        Terrain& t = *terrain;
        const GrassDesc& g = t.desc.grass;
        if (t.grass) t.grass->culled = false;
        if (!g.enabled || !t.desc.visible || frame >= t.sets.size() || !t.arrays_ready) continue;
        if (!ensureGrass(t)) continue;
        Grass& grass = *t.grass;

        // Rejilla: una celda por brizna posible (con la densidad de cerca).
        const float max_distance = std::clamp(g.max_distance, 2.0f, 400.0f);
        float spacing = 1.0f / std::sqrt(std::max(g.density, 0.01f));
        float cells = std::ceil(max_distance * 2.0f / spacing) + 1.0f;
        if (cells > 4096.0f) {
            spacing = max_distance * 2.0f / 4095.0f;
            cells = 4096.0f;
        }
        GpuGrassParams p{};
        p.grid = core::Vec4{spacing, cells, max_distance, std::min(g.near_distance, max_distance)};
        p.origin = core::Vec4{camera_position.x - max_distance, camera_position.z - max_distance,
                              std::clamp(g.threshold, 0.0f, 0.99f), static_cast<float>(grass.capacity)};
        p.shape = core::Vec4{g.height, g.height_variation, g.width, g.bend};
        p.base_color = core::Vec4{g.base_color.x, g.base_color.y, g.base_color.z, g.color_variation};
        p.tip_color = core::Vec4{g.tip_color.x, g.tip_color.y, g.tip_color.z, g.wind};
        p.dry_color = core::Vec4{g.dry_color.x, g.dry_color.y, g.dry_color.z, g.interaction};
        const float wind_angle = g.wind_direction * 3.14159265f / 180.0f;
        p.wind = core::Vec4{std::cos(wind_angle), std::sin(wind_angle), grass_seconds_, grass_previous_seconds_};
        p.camera_pos = core::Vec4{camera_position.x, camera_position.y, camera_position.z, static_cast<float>(id % 97)};
        p.layers[0] = g.layer;
        p.layers[1] = g.dry_layer;
        p.layers[2] = static_cast<std::int32_t>(grass_interactors_.size());
        p.view_projection = view_projection;
        for (std::size_t i = 0; i < grass_interactors_.size(); ++i) p.interactors[i] = grass_interactors_[i];
        grass.params[frame].write(&p, sizeof(p));

        // Contadores a cero (despues de que el frame anterior dibujara).
        vk::MemoryBarrier2 before{};
        before.srcStageMask = vk::PipelineStageFlagBits2::eDrawIndirect | vk::PipelineStageFlagBits2::eVertexShader;
        before.srcAccessMask = vk::AccessFlagBits2::eIndirectCommandRead | vk::AccessFlagBits2::eShaderStorageRead;
        before.dstStageMask = vk::PipelineStageFlagBits2::eTransfer | vk::PipelineStageFlagBits2::eComputeShader;
        before.dstAccessMask = vk::AccessFlagBits2::eTransferWrite | vk::AccessFlagBits2::eShaderStorageWrite;
        compat::pipelineBarrier(cmd, vk::DependencyInfo{}.setMemoryBarriers(before));
        const std::array<DrawIndirect, 2> reset = {{{kNearSegments * 6 + 3, 0, 0, 0},
                                                    {kFarSegments * 6 + 3, 0, 0, grass.capacity}}};
        cmd.updateBuffer<DrawIndirect>(*grass.args.handle(), 0, reset);
        vk::MemoryBarrier2 cleared{};
        cleared.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
        cleared.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
        cleared.dstStageMask = vk::PipelineStageFlagBits2::eComputeShader;
        cleared.dstAccessMask = vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite;
        compat::pipelineBarrier(cmd, vk::DependencyInfo{}.setMemoryBarriers(cleared));

        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *grass_cull_pipeline_);
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *grass_cull_layout_, 0,
                               {*t.sets[frame], *grass.sets[frame]}, nullptr);
        const auto groups = static_cast<std::uint32_t>((cells + 7.0f) / 8.0f);
        cmd.dispatch(groups, groups, 1);

        vk::MemoryBarrier2 done{};
        done.srcStageMask = vk::PipelineStageFlagBits2::eComputeShader;
        done.srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite;
        done.dstStageMask = vk::PipelineStageFlagBits2::eDrawIndirect | vk::PipelineStageFlagBits2::eVertexShader;
        done.dstAccessMask = vk::AccessFlagBits2::eIndirectCommandRead | vk::AccessFlagBits2::eShaderStorageRead;
        compat::pipelineBarrier(cmd, vk::DependencyInfo{}.setMemoryBarriers(done));
        grass.culled = true;
    }
}

void TerrainPass::recordGrassGBuffer(const vk::raii::CommandBuffer& cmd, std::uint32_t frame,
                                     const vk::raii::DescriptorSet& frame_set) const {
    if (device_ != nullptr && !device_->drawIndirectFirstInstanceSupported()) return;  // sin firstInstance (algunos moviles)
    bool bound = false;
    for (const auto& [id, terrain] : terrains_) {
        const Terrain& t = *terrain;
        if (!t.grass || !t.grass->culled || frame >= t.grass->sets.size()) continue;
        if (!bound) {
            cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *grass_pipeline_);
            cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *grass_draw_layout_, 0, *frame_set, nullptr);
            bound = true;
        }
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *grass_draw_layout_, 1,
                               {*t.sets[frame], *t.grass->sets[frame]}, nullptr);
        cmd.pushConstants<std::uint32_t>(*grass_draw_layout_, vk::ShaderStageFlagBits::eVertex, 0, kNearSegments);
        cmd.drawIndirect(*t.grass->args.handle(), 0, 1, sizeof(DrawIndirect));
        cmd.pushConstants<std::uint32_t>(*grass_draw_layout_, vk::ShaderStageFlagBits::eVertex, 0, kFarSegments);
        cmd.drawIndirect(*t.grass->args.handle(), sizeof(DrawIndirect), 1, sizeof(DrawIndirect));
    }
}

// Malla de un trozo: (kPatchCells + 1)^2 vertices (x, z en 0..1, faldon 0)
// y un anillo de faldon (faldon 1) que baja desde el borde.
void TerrainPass::createPatchMesh(const VulkanDevice& device) {
    constexpr std::uint32_t n = kPatchCells + 1;
    std::vector<float> vertices;
    vertices.reserve(n * n * 3 + n * 4 * 3);
    for (std::uint32_t z = 0; z < n; ++z) {
        for (std::uint32_t x = 0; x < n; ++x) {
            vertices.push_back(static_cast<float>(x) / kPatchCells);
            vertices.push_back(static_cast<float>(z) / kPatchCells);
            vertices.push_back(0.0f);
        }
    }
    std::vector<std::uint32_t> indices;
    for (std::uint32_t z = 0; z < kPatchCells; ++z) {
        for (std::uint32_t x = 0; x < kPatchCells; ++x) {
            const std::uint32_t a = z * n + x;
            const std::uint32_t b = a + 1;
            const std::uint32_t c = a + n;
            const std::uint32_t d = c + 1;
            indices.insert(indices.end(), {a, c, b, b, c, d});
        }
    }
    // Faldones: por cada lado, una tira del borde hacia abajo.
    const auto border = [&](std::uint32_t i, int side) {
        switch (side) {
            case 0: return i;                      // z = 0
            case 1: return (n - 1) * n + i;        // z = 1
            case 2: return i * n;                  // x = 0
            default: return i * n + (n - 1);       // x = 1
        }
    };
    for (int side = 0; side < 4; ++side) {
        const auto first = static_cast<std::uint32_t>(vertices.size() / 3);
        for (std::uint32_t i = 0; i < n; ++i) {
            const std::uint32_t top = border(i, side);
            vertices.push_back(vertices[top * 3]);
            vertices.push_back(vertices[top * 3 + 1]);
            vertices.push_back(1.0f);
        }
        for (std::uint32_t i = 0; i + 1 < n; ++i) {
            const std::uint32_t t0 = border(i, side);
            const std::uint32_t t1 = border(i + 1, side);
            const std::uint32_t s0 = first + i;
            const std::uint32_t s1 = first + i + 1;
            indices.insert(indices.end(), {t0, s0, t1, t1, s0, s1});
        }
    }
    patch_vertices_ = VulkanBuffer::createDeviceLocal(device, vertices.data(), vertices.size() * sizeof(float),
                                                      vk::BufferUsageFlagBits::eVertexBuffer);
    patch_indices_ = VulkanBuffer::createDeviceLocal(device, indices.data(), indices.size() * sizeof(std::uint32_t),
                                                     vk::BufferUsageFlagBits::eIndexBuffer);
    patch_index_count_ = static_cast<std::uint32_t>(indices.size());
}

// -----------------------------------------------------------------------------
// Terrenos
// -----------------------------------------------------------------------------

std::uint32_t TerrainPass::createTerrain(std::uint32_t resolution, std::uint32_t splat_resolution) {
    if (device_ == nullptr || resolution < 3 || splat_resolution < 2) return 0;
    const std::uint32_t id = next_id_++;
    auto terrain = std::make_unique<Terrain>();
    terrain->resolution = resolution;
    terrain->splat_resolution = splat_resolution;
    const auto usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst;
    terrain->heights.create(*device_, vk::Extent2D{resolution, resolution}, kHeightFormat, usage,
                            vk::ImageAspectFlagBits::eColor);
    terrain->splat0.create(*device_, vk::Extent2D{splat_resolution, splat_resolution}, kSplatFormat, usage,
                           vk::ImageAspectFlagBits::eColor);
    terrain->splat1.create(*device_, vk::Extent2D{splat_resolution, splat_resolution}, kSplatFormat, usage,
                           vk::ImageAspectFlagBits::eColor);
    // Que tengan contenido valido desde el principio (plano, capa 0).
    const std::vector<float> flat(static_cast<std::size_t>(resolution) * resolution, 0.0f);
    std::vector<std::uint8_t> first(static_cast<std::size_t>(splat_resolution) * splat_resolution * 4, 0);
    for (std::size_t i = 0; i < first.size(); i += 4) first[i] = 255;
    const std::vector<std::uint8_t> zero(first.size(), 0);
    device_->submitOneTime([&](const vk::raii::CommandBuffer& cmd) {
        for (const VulkanImage* image : {&terrain->heights, &terrain->splat0, &terrain->splat1}) {
            barrier(cmd, *image->handle(), vk::ImageLayout::eUndefined, vk::ImageLayout::eShaderReadOnlyOptimal,
                    vk::PipelineStageFlagBits2::eTopOfPipe, vk::AccessFlagBits2::eNone,
                    vk::PipelineStageFlagBits2::eVertexShader | vk::PipelineStageFlagBits2::eFragmentShader,
                    vk::AccessFlagBits2::eShaderSampledRead);
        }
    });
    terrains_.emplace(id, std::move(terrain));
    updateHeights(id, flat.data(), 0, 0, resolution, resolution);
    updateSplat(id, first.data(), zero.data(), 0, 0, splat_resolution, splat_resolution);

    Terrain& t = *terrains_.at(id);
    t.params.resize(frames_);
    for (VulkanBuffer& buffer : t.params) {
        buffer.create(*device_, sizeof(GpuTerrainParams), vk::BufferUsageFlagBits::eUniformBuffer,
                      vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
    }
    std::vector<vk::DescriptorSetLayout> layouts(frames_, *set_layout_);
    vk::DescriptorSetAllocateInfo allocate{};
    allocate.descriptorPool = *pool_;
    allocate.setSetLayouts(layouts);
    t.sets = vk::raii::DescriptorSets(device_->handle(), allocate);
    loadLayerTextures(t);
    return id;
}

void TerrainPass::destroyTerrain(std::uint32_t id) {
    if (terrains_.count(id) == 0) return;
    device_->waitIdle();  // algun frame en vuelo puede estar usandolo
    terrains_.erase(id);
    uploads_.erase(std::remove_if(uploads_.begin(), uploads_.end(), [&](const Upload& u) { return u.id == id; }),
                   uploads_.end());
}

void TerrainPass::setDesc(std::uint32_t id, const TerrainDesc& desc) {
    const auto it = terrains_.find(id);
    if (it == terrains_.end()) return;
    Terrain& t = *it->second;
    t.desc = desc;
    if (t.desc.layers.size() > kMaxTerrainLayers) t.desc.layers.resize(kMaxTerrainLayers);
    // Las texturas de las capas solo se recargan si cambian sus rutas.
    bool changed = !t.arrays_ready;
    for (std::uint32_t i = 0; i < kMaxTerrainLayers; ++i) {
        const std::filesystem::path albedo = i < t.desc.layers.size() ? t.desc.layers[i].albedo : std::filesystem::path{};
        const std::filesystem::path normal = i < t.desc.layers.size() ? t.desc.layers[i].normal : std::filesystem::path{};
        changed = changed || albedo != t.albedo_paths[i] || normal != t.normal_paths[i];
    }
    if (changed) {
        device_->waitIdle();
        loadLayerTextures(t);
    }
    // Lo que cambia el color del terreno para los rayos: capas, tintes y hierba.
    std::uint64_t signature = 1469598103934665603ull ^ t.desc.layers.size();
    const auto mix_float = [&](float f) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &f, sizeof(bits));
        signature = (signature ^ bits) * 1099511628211ull;
    };
    for (const TerrainLayerDesc& layer : t.desc.layers) {
        for (const float f : {layer.tint.x, layer.tint.y, layer.tint.z}) mix_float(f);
    }
    const GrassDesc& grass = t.desc.grass;
    for (const float f : {grass.enabled ? 1.0f : 0.0f, static_cast<float>(grass.layer), static_cast<float>(grass.dry_layer),
                          grass.threshold, grass.base_color.x, grass.base_color.y, grass.base_color.z, grass.tip_color.x,
                          grass.tip_color.y, grass.tip_color.z, grass.dry_color.x, grass.dry_color.y,
                          grass.dry_color.z}) {
        mix_float(f);
    }
    if (signature != t.look_signature) {
        t.look_signature = signature;
        ++t.look_revision;
    }
}

void TerrainPass::createArrayTexture(ArrayTexture& texture, const std::vector<std::vector<std::uint8_t>>& layers) {
    const VulkanDevice& device = *device_;
    const auto count = static_cast<std::uint32_t>(layers.size());
    const std::uint32_t mips = static_cast<std::uint32_t>(std::bit_width(kLayerSize));
    texture = ArrayTexture{};
    vk::ImageCreateInfo info{};
    info.imageType = vk::ImageType::e2D;
    info.format = kLayerFormat;
    info.extent = vk::Extent3D{kLayerSize, kLayerSize, 1};
    info.mipLevels = mips;
    info.arrayLayers = count;
    info.samples = vk::SampleCountFlagBits::e1;
    info.tiling = vk::ImageTiling::eOptimal;
    info.usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst |
                 vk::ImageUsageFlagBits::eTransferSrc;
    info.initialLayout = vk::ImageLayout::eUndefined;
    texture.image = vk::raii::Image(device.handle(), info);
    const vk::MemoryRequirements requirements = texture.image.getMemoryRequirements();
    vk::MemoryAllocateInfo allocate{};
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = device.findMemoryType(requirements.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal);
    texture.memory = vk::raii::DeviceMemory(device.handle(), allocate);
    texture.image.bindMemory(*texture.memory, 0);

    const vk::DeviceSize layer_bytes = static_cast<vk::DeviceSize>(kLayerSize) * kLayerSize * 4;
    VulkanBuffer staging;
    staging.create(device, layer_bytes * count, vk::BufferUsageFlagBits::eTransferSrc,
                   vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
    for (std::uint32_t i = 0; i < count; ++i) staging.write(layers[i].data(), layer_bytes, layer_bytes * i);

    const vk::Image image = *texture.image;
    device.submitOneTime([&](const vk::raii::CommandBuffer& cmd) {
        barrier(cmd, image, vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
                vk::PipelineStageFlagBits2::eTopOfPipe, vk::AccessFlagBits2::eNone, vk::PipelineStageFlagBits2::eTransfer,
                vk::AccessFlagBits2::eTransferWrite, 0, mips, count);
        std::vector<vk::BufferImageCopy> regions;
        for (std::uint32_t i = 0; i < count; ++i) {
            vk::BufferImageCopy region{};
            region.bufferOffset = layer_bytes * i;
            region.imageSubresource = vk::ImageSubresourceLayers{vk::ImageAspectFlagBits::eColor, 0, i, 1};
            region.imageExtent = vk::Extent3D{kLayerSize, kLayerSize, 1};
            regions.push_back(region);
        }
        cmd.copyBufferToImage(*staging.handle(), image, vk::ImageLayout::eTransferDstOptimal, regions);
        // Mipmaps: cada nivel desde el anterior (blit lineal), todas las capas.
        std::int32_t size = static_cast<std::int32_t>(kLayerSize);
        for (std::uint32_t level = 1; level < mips; ++level) {
            barrier(cmd, image, vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eTransferSrcOptimal,
                    vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite,
                    vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead, level - 1, 1, count);
            vk::ImageBlit blit{};
            blit.srcSubresource = vk::ImageSubresourceLayers{vk::ImageAspectFlagBits::eColor, level - 1, 0, count};
            blit.srcOffsets[1] = vk::Offset3D{size, size, 1};
            const std::int32_t next = std::max(size / 2, 1);
            blit.dstSubresource = vk::ImageSubresourceLayers{vk::ImageAspectFlagBits::eColor, level, 0, count};
            blit.dstOffsets[1] = vk::Offset3D{next, next, 1};
            cmd.blitImage(image, vk::ImageLayout::eTransferSrcOptimal, image, vk::ImageLayout::eTransferDstOptimal, blit,
                          vk::Filter::eLinear);
            barrier(cmd, image, vk::ImageLayout::eTransferSrcOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
                    vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead,
                    vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderSampledRead, level - 1, 1,
                    count);
            size = next;
        }
        barrier(cmd, image, vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
                vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite,
                vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderSampledRead, mips - 1, 1, count);
    });

    vk::ImageViewCreateInfo view{};
    view.image = image;
    view.viewType = vk::ImageViewType::e2DArray;
    view.format = kLayerFormat;
    view.subresourceRange = vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, mips, 0, count};
    texture.view = vk::raii::ImageView(device.handle(), view);
}

void TerrainPass::loadLayerTextures(Terrain& terrain) {
    std::vector<std::vector<std::uint8_t>> albedo(kMaxTerrainLayers);
    std::vector<std::vector<std::uint8_t>> normal(kMaxTerrainLayers);
    for (std::uint32_t i = 0; i < kMaxTerrainLayers; ++i) {
        const TerrainLayerDesc* layer = i < terrain.desc.layers.size() ? &terrain.desc.layers[i] : nullptr;
        terrain.albedo_paths[i] = layer != nullptr ? layer->albedo : std::filesystem::path{};
        terrain.normal_paths[i] = layer != nullptr ? layer->normal : std::filesystem::path{};
        asset::ImageRgba8 image;
        if (!terrain.albedo_paths[i].empty() && asset::loadImageRgba8(terrain.albedo_paths[i], image, 2048)) {
            albedo[i] = resizeSquare(image);
        } else {
            if (!terrain.albedo_paths[i].empty()) {
                std::cerr << "[Terreno] No se pudo leer " << terrain.albedo_paths[i].string() << "\n";
                terrain.albedo_paths[i].clear();
            }
            albedo[i] = solidLayer(255, 255, 255, 255);
        }
        if (!terrain.normal_paths[i].empty() && asset::loadImageRgba8(terrain.normal_paths[i], image, 2048)) {
            normal[i] = resizeSquare(image);
        } else {
            terrain.normal_paths[i].clear();
            normal[i] = solidLayer(128, 128, 255, 255);
        }
    }
    createArrayTexture(terrain.albedo_array, albedo);
    createArrayTexture(terrain.normal_array, normal);
    terrain.arrays_ready = true;
    // Color medio de cada capa (lo que se ve de lejos, y lo que ven los rayos).
    for (std::uint32_t i = 0; i < kMaxTerrainLayers; ++i) {
        double sum[3] = {0.0, 0.0, 0.0};
        const std::vector<std::uint8_t>& pixels = albedo[i];
        const std::size_t count = pixels.size() / 4;
        for (std::size_t p = 0; p < count; ++p) {
            for (std::size_t c = 0; c < 3; ++c) sum[c] += pixels[p * 4 + c];
        }
        const double scale = count > 0 ? 1.0 / (255.0 * static_cast<double>(count)) : 0.0;
        terrain.layer_average[i] = core::Vec3{static_cast<float>(sum[0] * scale), static_cast<float>(sum[1] * scale),
                                              static_cast<float>(sum[2] * scale)};
    }
    ++terrain.look_revision;
    writeDescriptors(terrain);
}

void TerrainPass::writeDescriptors(Terrain& terrain) {
    for (std::uint32_t frame = 0; frame < terrain.sets.size(); ++frame) {
        const vk::DescriptorImageInfo heights{*clamp_sampler_, *terrain.heights.view(), vk::ImageLayout::eShaderReadOnlyOptimal};
        const vk::DescriptorBufferInfo params{*terrain.params[frame].handle(), 0, sizeof(GpuTerrainParams)};
        const vk::DescriptorImageInfo splat0{*clamp_sampler_, *terrain.splat0.view(), vk::ImageLayout::eShaderReadOnlyOptimal};
        const vk::DescriptorImageInfo splat1{*clamp_sampler_, *terrain.splat1.view(), vk::ImageLayout::eShaderReadOnlyOptimal};
        const vk::DescriptorImageInfo albedo{*repeat_sampler_, *terrain.albedo_array.view, vk::ImageLayout::eShaderReadOnlyOptimal};
        const vk::DescriptorImageInfo normal{*repeat_sampler_, *terrain.normal_array.view, vk::ImageLayout::eShaderReadOnlyOptimal};
        const vk::DescriptorSet set = *terrain.sets[frame];
        const std::array<vk::WriteDescriptorSet, 6> writes = {{
            {set, 0, 0, 1, vk::DescriptorType::eCombinedImageSampler, &heights},
            {set, 1, 0, 1, vk::DescriptorType::eUniformBuffer, nullptr, &params},
            {set, 2, 0, 1, vk::DescriptorType::eCombinedImageSampler, &splat0},
            {set, 3, 0, 1, vk::DescriptorType::eCombinedImageSampler, &splat1},
            {set, 4, 0, 1, vk::DescriptorType::eCombinedImageSampler, &albedo},
            {set, 5, 0, 1, vk::DescriptorType::eCombinedImageSampler, &normal},
        }};
        device_->handle().updateDescriptorSets(writes, nullptr);
    }
}

// -----------------------------------------------------------------------------
// Subidas
// -----------------------------------------------------------------------------

void TerrainPass::updateHeights(std::uint32_t id, const float* heights, std::uint32_t x, std::uint32_t y,
                                std::uint32_t w, std::uint32_t h) {
    const auto it = terrains_.find(id);
    if (it == terrains_.end() || heights == nullptr || w == 0 || h == 0) return;
    const std::uint32_t res = it->second->resolution;
    x = std::min(x, res - 1);
    y = std::min(y, res - 1);
    w = std::min(w, res - x);
    h = std::min(h, res - y);
    Upload upload{id, 0, x, y, w, h, {}};
    upload.bytes.resize(static_cast<std::size_t>(w) * h * sizeof(float));
    for (std::uint32_t row = 0; row < h; ++row) {
        std::memcpy(upload.bytes.data() + static_cast<std::size_t>(row) * w * sizeof(float),
                    heights + static_cast<std::size_t>(y + row) * res + x, w * sizeof(float));
    }
    uploads_.push_back(std::move(upload));
    if (keep_cpu_copy_) {
        Terrain& t = *it->second;
        t.cpu_heights.resize(static_cast<std::size_t>(res) * res, 0.0f);
        for (std::uint32_t row = 0; row < h; ++row) {
            const std::size_t at = static_cast<std::size_t>(y + row) * res + x;
            std::memcpy(t.cpu_heights.data() + at, heights + at, w * sizeof(float));
        }
        ++t.height_revision;
    }
}

void TerrainPass::updateSplat(std::uint32_t id, const std::uint8_t* splat0, const std::uint8_t* splat1,
                              std::uint32_t x, std::uint32_t y, std::uint32_t w, std::uint32_t h) {
    const auto it = terrains_.find(id);
    if (it == terrains_.end() || splat0 == nullptr || splat1 == nullptr || w == 0 || h == 0) return;
    const std::uint32_t res = it->second->splat_resolution;
    x = std::min(x, res - 1);
    y = std::min(y, res - 1);
    w = std::min(w, res - x);
    h = std::min(h, res - y);
    Upload upload{id, 1, x, y, w, h, {}};
    const std::size_t plane = static_cast<std::size_t>(w) * h * 4;
    upload.bytes.resize(plane * 2);
    for (std::uint32_t row = 0; row < h; ++row) {
        const std::size_t source = (static_cast<std::size_t>(y + row) * res + x) * 4;
        std::memcpy(upload.bytes.data() + static_cast<std::size_t>(row) * w * 4, splat0 + source, w * 4);
        std::memcpy(upload.bytes.data() + plane + static_cast<std::size_t>(row) * w * 4, splat1 + source, w * 4);
    }
    uploads_.push_back(std::move(upload));
    if (keep_cpu_copy_) {
        Terrain& t = *it->second;
        t.cpu_splat0.resize(static_cast<std::size_t>(res) * res * 4, 0);
        t.cpu_splat1.resize(static_cast<std::size_t>(res) * res * 4, 0);
        for (std::uint32_t row = 0; row < h; ++row) {
            const std::size_t at = (static_cast<std::size_t>(y + row) * res + x) * 4;
            std::memcpy(t.cpu_splat0.data() + at, splat0 + at, w * 4);
            std::memcpy(t.cpu_splat1.data() + at, splat1 + at, w * 4);
        }
        ++t.look_revision;
    }
}

std::vector<TerrainPass::RayTracingSource> TerrainPass::rayTracingSources() const {
    std::vector<RayTracingSource> out;
    for (const auto& [id, terrain] : terrains_) {
        const Terrain& t = *terrain;
        const std::size_t heights = static_cast<std::size_t>(t.resolution) * t.resolution;
        const std::size_t splat = static_cast<std::size_t>(t.splat_resolution) * t.splat_resolution * 4;
        if (t.cpu_heights.size() != heights || t.cpu_splat0.size() != splat || t.cpu_splat1.size() != splat) continue;
        RayTracingSource s;
        s.id = id;
        s.resolution = t.resolution;
        s.splat_resolution = t.splat_resolution;
        s.origin = t.desc.origin;
        s.size = t.desc.size;
        s.max_height = t.desc.max_height;
        s.visible = t.desc.visible;
        s.heights = t.cpu_heights.data();
        s.splat0 = t.cpu_splat0.data();
        s.splat1 = t.cpu_splat1.data();
        s.layer_count = static_cast<std::uint32_t>(std::min<std::size_t>(t.desc.layers.size(), kMaxTerrainLayers));
        for (std::uint32_t i = 0; i < kMaxTerrainLayers; ++i) {
            // Sin textura, terrain.frag pinta su tinte con manchas de media ~1.
            const core::Vec3 tint = i < t.desc.layers.size() ? t.desc.layers[i].tint : core::Vec3{1.0f, 1.0f, 1.0f};
            const core::Vec3 base = t.layer_average[i];
            s.layer_color[i] = core::Vec3{base.x * tint.x, base.y * tint.y, base.z * tint.z};
        }
        s.height_revision = t.height_revision;
        s.look_revision = t.look_revision;
        const GrassDesc& grass = t.desc.grass;
        if (grass.enabled && grass.layer >= 0 && static_cast<std::uint32_t>(grass.layer) < kMaxTerrainLayers) {
            s.grass = true;
            s.grass_layer = static_cast<std::uint32_t>(grass.layer);
            s.grass_dry_layer = grass.dry_layer;
            s.grass_threshold = grass.threshold;
            // La brizna es casi toda del color de la punta (grass.vert); la
            // base solo asoma entre ellas.
            s.grass_color = grass.base_color * 0.15f + grass.tip_color * 0.85f;
            s.grass_dry_color = grass.dry_color;
        }
        out.push_back(s);
    }
    return out;
}

bool TerrainPass::recordUploads(const vk::raii::CommandBuffer& cmd, std::uint32_t frame) {
    if (uploads_.empty() || frame >= staging_.size()) return false;
    vk::DeviceSize total = 0;
    for (const Upload& u : uploads_) total += (u.bytes.size() + 15) & ~static_cast<vk::DeviceSize>(15);
    VulkanBuffer& staging = staging_[frame];
    if (!staging.isValid() || staging.size() < total) {
        staging.destroy();
        staging.create(*device_, std::max<vk::DeviceSize>(total * 2, 1 << 20), vk::BufferUsageFlagBits::eTransferSrc,
                       vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
    }
    bool heights_changed = false;
    vk::DeviceSize offset = 0;
    for (const Upload& u : uploads_) {
        const auto it = terrains_.find(u.id);
        if (it == terrains_.end()) continue;
        Terrain& t = *it->second;
        staging.write(u.bytes.data(), u.bytes.size(), offset);
        const auto copy = [&](const VulkanImage& image, vk::DeviceSize at) {
            const vk::Image handle = *image.handle();
            barrier(cmd, handle, vk::ImageLayout::eShaderReadOnlyOptimal, vk::ImageLayout::eTransferDstOptimal,
                    vk::PipelineStageFlagBits2::eVertexShader | vk::PipelineStageFlagBits2::eFragmentShader,
                    vk::AccessFlagBits2::eShaderSampledRead, vk::PipelineStageFlagBits2::eTransfer,
                    vk::AccessFlagBits2::eTransferWrite);
            vk::BufferImageCopy region{};
            region.bufferOffset = at;
            region.imageSubresource = vk::ImageSubresourceLayers{vk::ImageAspectFlagBits::eColor, 0, 0, 1};
            region.imageOffset = vk::Offset3D{static_cast<std::int32_t>(u.x), static_cast<std::int32_t>(u.y), 0};
            region.imageExtent = vk::Extent3D{u.w, u.h, 1};
            cmd.copyBufferToImage(*staging.handle(), handle, vk::ImageLayout::eTransferDstOptimal, region);
            barrier(cmd, handle, vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
                    vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite,
                    vk::PipelineStageFlagBits2::eVertexShader | vk::PipelineStageFlagBits2::eFragmentShader,
                    vk::AccessFlagBits2::eShaderSampledRead);
        };
        if (u.kind == 0) {
            copy(t.heights, offset);
            heights_changed = true;
        } else {
            copy(t.splat0, offset);
            copy(t.splat1, offset + u.bytes.size() / 2);
        }
        offset += (u.bytes.size() + 15) & ~static_cast<vk::DeviceSize>(15);
    }
    uploads_.clear();
    return heights_changed;
}

// -----------------------------------------------------------------------------
// Trozos (LOD)
// -----------------------------------------------------------------------------

namespace {

// Recorre el arbol de trozos de un terreno: el LOD lo decide solo la
// distancia a `camera`, y `keep` recorta (una caja por nodo). Asi el G-buffer
// y los mapas de las luces locales eligen el MISMO detalle para un mismo
// sitio aunque recorten con volumenes distintos.
template <typename Keep, typename Visit>
void walkChunks(const TerrainDesc& d, std::uint32_t resolution, const core::Vec3& camera, Keep&& keep,
                Visit&& visit) {
    if (!d.visible || resolution < 3) return;
    // El trozo mas pequeno: kPatchCells texeles de alturas por lado.
    const float min_size = std::min(1.0f, static_cast<float>(kPatchCells) / static_cast<float>(resolution - 1));
    struct Node {
        float u, v, size;
    };
    std::vector<Node> stack = {{0.0f, 0.0f, 1.0f}};
    while (!stack.empty()) {
        const Node node = stack.back();
        stack.pop_back();
        const core::Vec3 lo{d.origin.x + node.u * d.size, d.origin.y, d.origin.z + node.v * d.size};
        const core::Vec3 hi{lo.x + node.size * d.size, d.origin.y + d.max_height, lo.z + node.size * d.size};
        if (!keep(core::Aabb{lo, hi})) continue;
        // Distancia de la camara a la caja del trozo.
        const core::Vec3 closest{std::clamp(camera.x, lo.x, hi.x), std::clamp(camera.y, lo.y, hi.y),
                                 std::clamp(camera.z, lo.z, hi.z)};
        const float distance = core::length(closest - camera);
        const float world_size = node.size * d.size;
        if (node.size > min_size * 1.001f && distance < world_size * std::max(d.lod_distance, 0.5f)) {
            const float half = node.size * 0.5f;
            stack.push_back({node.u, node.v, half});
            stack.push_back({node.u + half, node.v, half});
            stack.push_back({node.u, node.v + half, half});
            stack.push_back({node.u + half, node.v + half, half});
            continue;
        }
        // Faldon: lo bastante para tapar el salto con el vecino de otro LOD.
        const float cell = world_size / kPatchCells;
        visit(node.u, node.v, node.size, cell * 2.0f + d.max_height * 0.02f + 0.5f);
    }
}

}  // namespace

void TerrainPass::selectChunks(Terrain& terrain, const core::Vec3& camera, const core::Mat4& view_projection) {
    terrain.chunks.clear();
    const core::Frustum frustum(view_projection);
    walkChunks(
        terrain.desc, terrain.resolution, camera, [&](const core::Aabb& box) { return frustum.intersects(box); },
        [&](float u, float v, float size, float skirt) { terrain.chunks.push_back(Chunk{u, v, size, skirt}); });
}

std::uint64_t TerrainPass::localSignature(const core::Vec3& position, float range) const {
    // FNV-1a de los trozos (con su LOD) que tocan la caja de la luz.
    std::uint64_t hash = 1469598103934665603ull;
    const auto mix = [&hash](std::uint64_t value) {
        hash ^= value;
        hash *= 1099511628211ull;
    };
    const core::Vec3 r{range, range, range};
    const core::Vec3 light_lo = position - r;
    const core::Vec3 light_hi = position + r;
    for (const auto& [id, terrain] : terrains_) {
        if (!terrain->desc.cast_shadows) continue;
        mix(id);
        walkChunks(
            terrain->desc, terrain->resolution, lod_camera_,
            [&](const core::Aabb& box) {
                // El faldon cuelga por debajo del origen (como mucho el ancho
                // del trozo).
                const float skirt_bottom = box.min.y - (box.max.x - box.min.x);
                return box.min.x <= light_hi.x && box.max.x >= light_lo.x && skirt_bottom <= light_hi.y &&
                       box.max.y >= light_lo.y && box.min.z <= light_hi.z && box.max.z >= light_lo.z;
            },
            [&](float u, float v, float size, float) {
                mix(std::bit_cast<std::uint32_t>(u));
                mix(std::bit_cast<std::uint32_t>(v));
                mix(std::bit_cast<std::uint32_t>(size));
            });
    }
    return hash;
}

void TerrainPass::prepare(std::uint32_t frame, const core::Vec3& camera_position, const core::Mat4& view_projection) {
    lod_camera_ = camera_position;
    for (auto& [id, terrain] : terrains_) {
        selectChunks(*terrain, camera_position, view_projection);
        if (frame >= terrain->params.size()) continue;
        const TerrainDesc& d = terrain->desc;
        GpuTerrainParams params{};
        params.origin_size = core::Vec4{d.origin.x, d.origin.y, d.origin.z, d.size};
        params.info = core::Vec4{d.max_height, static_cast<float>(terrain->resolution),
                                 1.0f / static_cast<float>(terrain->resolution),
                                 static_cast<float>(std::max<std::size_t>(d.layers.size(), 1))};
        for (std::uint32_t i = 0; i < kMaxTerrainLayers; ++i) {
            const TerrainLayerDesc layer = i < d.layers.size() ? d.layers[i] : TerrainLayerDesc{};
            params.layer_params[i] = core::Vec4{std::max(layer.tiling, 0.01f), layer.roughness, layer.metallic,
                                                layer.normal_strength};
            params.layer_tint[i] = core::Vec4{layer.tint.x, layer.tint.y, layer.tint.z,
                                              terrain->albedo_paths[i].empty() ? 0.0f : 1.0f};
            params.layer_extra[i] = core::Vec4{terrain->normal_paths[i].empty() ? 0.0f : 1.0f, 0.0f, 0.0f, 0.0f};
        }
        terrain->params[frame].write(&params, sizeof(params));
    }
}

void TerrainPass::recordGBuffer(const vk::raii::CommandBuffer& cmd, std::uint32_t frame,
                                const vk::raii::DescriptorSet& frame_set) const {
    bool bound = false;
    for (const auto& [id, terrain] : terrains_) {
        if (terrain->chunks.empty() || frame >= terrain->sets.size()) continue;
        if (!bound) {
            cmd.bindPipeline(vk::PipelineBindPoint::eGraphics,
                     wireframe_ && *gbuffer_wire_pipeline_ ? *gbuffer_wire_pipeline_ : *gbuffer_pipeline_);
            cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *layout_, 0, *frame_set, nullptr);
            cmd.bindVertexBuffers(0, *patch_vertices_.handle(), {0});
            cmd.bindIndexBuffer(*patch_indices_.handle(), 0, vk::IndexType::eUint32);
            bound = true;
        }
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *layout_, 1, *terrain->sets[frame], nullptr);
        for (const Chunk& chunk : terrain->chunks) {
            GpuTerrainPush push{};
            push.chunk = core::Vec4{chunk.u, chunk.v, chunk.size, chunk.skirt};
            cmd.pushConstants<GpuTerrainPush>(*layout_, vk::ShaderStageFlagBits::eVertex, 0, push);
            cmd.drawIndexed(patch_index_count_, 1, 0, 0, 0);
        }
    }
}

void TerrainPass::recordShadow(const vk::raii::CommandBuffer& cmd, std::uint32_t frame,
                               const vk::raii::DescriptorSet& frame_set, const core::Mat4& light_view_projection,
                               bool local) const {
    bool bound = false;
    const core::Frustum light_frustum(light_view_projection, /*ignore_near=*/!local);
    for (const auto& [id, terrain] : terrains_) {
        if (terrain->chunks.empty() || !terrain->desc.cast_shadows || frame >= terrain->sets.size()) continue;
        if (!bound) {
            cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *shadow_pipeline_);
            cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *layout_, 0, *frame_set, nullptr);
            cmd.bindVertexBuffers(0, *patch_vertices_.handle(), {0});
            cmd.bindIndexBuffer(*patch_indices_.handle(), 0, vk::IndexType::eUint32);
            bound = true;
        }
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *layout_, 1, *terrain->sets[frame], nullptr);
        const auto draw = [&](float u, float v, float size, float skirt) {
            GpuTerrainPush push{};
            push.chunk = core::Vec4{u, v, size, skirt};
            push.light_view_projection = light_view_projection;
            push.shadow = 1;
            cmd.pushConstants<GpuTerrainPush>(*layout_, vk::ShaderStageFlagBits::eVertex, 0, push);
            cmd.drawIndexed(patch_index_count_, 1, 0, 0, 0);
        };
        if (local) {
            // Luz local: sus propios trozos, recortados por SU volumen pero con
            // el LOD de la camara. Con los trozos de la camara faltaba lo que
            // queda fuera de pantalla y, como el mapa se cachea, al moverse la
            // camara el terreno cambiaba de LOD y el del mapa no: el terreno se
            // sombreaba a si mismo a manchas (ruido). Ver localSignature().
            walkChunks(
                terrain->desc, terrain->resolution, lod_camera_,
                [&](const core::Aabb& box) {
                    // El faldon cuelga por debajo del origen (como mucho el
                    // ancho del trozo).
                    const core::Vec3 skirt{0.0f, box.max.x - box.min.x, 0.0f};
                    return light_frustum.intersects(core::Aabb{box.min - skirt, box.max});
                },
                draw);
        } else {
            for (const Chunk& chunk : terrain->chunks) draw(chunk.u, chunk.v, chunk.size, chunk.skirt);
        }
    }
}

std::uint32_t TerrainPass::chunkCount() const {
    std::uint32_t count = 0;
    for (const auto& [id, terrain] : terrains_) count += static_cast<std::uint32_t>(terrain->chunks.size());
    return count;
}

}  // namespace cramion::gfx
