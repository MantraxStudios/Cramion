#include "CramionFX/vk/VulkanCompat.h"
#include "CramionFX/vk/VoxelPass.h"

#include "CramionFX/core/Frustum.h"
#include "CramionFX/vk/GBuffer.h"
#include "CramionFX/vk/VulkanDevice.h"
#include "CramionFX/vk/VulkanShader.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <optional>

namespace cramion::gfx {

namespace {

constexpr std::uint64_t kPageBytes = 64ull << 20;  // 64 MB por pagina de vertices
constexpr std::uint64_t kQuadBytes = 32;           // 4 vertices de 8 bytes
constexpr std::uint64_t kUploadBudget = 24ull << 20;  // por frame (el resto espera)
constexpr vk::Format kLayerFormat = vk::Format::eR8G8B8A8Unorm;

// Debe coincidir con el push de voxel.vert / voxel.frag.
struct GpuVoxelPush {
    core::Vec4 origin_time;  // xyz esquina de la seccion, w segundos
    core::Mat4 light_view_projection;
    std::uint32_t shadow = 0;
    float previous_time = 0.0f;  // segundos del frame anterior (vectores de movimiento del viento)
    std::uint32_t style = 0;     // bit 0 color liso (variacion por bloque), bit 1 terreno lejano
    float far_fade_end = 0.0f;   // lejano: distancia (m) a la que acaba de deshacerse (0 = sin)
    core::Vec4 distant{};            // lejano: x z del hueco, su radio, bloques por columna
    // TESELADO: x distancia (m), y profundidad (bloques; 0 = apagado), z trozos por bloque, w 0.
    core::Vec4 tess{};
};
// 128: lo minimo que garantiza Vulkan (maxPushConstantsSize).
static_assert(sizeof(GpuVoxelPush) == 128, "GpuVoxelPush debe coincidir con VoxelPush (voxel_common.glsl)");

// Personajes que apartan las plantas: debe coincidir con FoliageInteractors
// (voxel.vert, set 1 binding 3, std140).
constexpr std::uint32_t kMaxInteractors = 16;
struct GpuFoliageInteractors {
    std::uint32_t count[4] = {};                 // x este frame, y el anterior
    core::Vec4 box_min{};                        // xyz: caja de lo que pueden tocar todos
    core::Vec4 box_max{};
    core::Vec4 feet[kMaxInteractors];            // xyz pies, w radio
    core::Vec4 trail[kMaxInteractors];           // xyz estela, w fuerza
    core::Vec4 previous_feet[kMaxInteractors];
    core::Vec4 previous_trail[kMaxInteractors];
};
static_assert(sizeof(GpuFoliageInteractors) == 48 + 64 * kMaxInteractors, "GpuFoliageInteractors: std140");
constexpr float kInteractorRange = 64.0f;  // m desde la camara: mas lejos no se nota
constexpr float kInteractorMerge = 0.5f;   // m: dos fuentes del mismo personaje (moveBox y su objeto de red)
constexpr float kInteractorMatch = 2.0f;   // m: lo maximo que se mueve uno entre dos frames (si no, es otro)
constexpr float kInteractorRise = 0.12f;   // s hasta la fuerza completa al llegar
constexpr float kInteractorFade = 0.7f;    // s hasta enderezarse del todo al irse
constexpr float kInteractorTrail = 0.3f;   // s de retraso de la estela

void imageBarrier(const vk::raii::CommandBuffer& cmd, vk::Image image, vk::ImageLayout from, vk::ImageLayout to,
                  vk::PipelineStageFlags2 src_stage, vk::AccessFlags2 src_access, vk::PipelineStageFlags2 dst_stage,
                  vk::AccessFlags2 dst_access, std::uint32_t base_mip, std::uint32_t mips, std::uint32_t layers) {
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

}  // namespace

// -----------------------------------------------------------------------------
// Creacion
// -----------------------------------------------------------------------------

void VoxelPass::create(const VulkanDevice& device, const vk::raii::DescriptorSetLayout& frame_layout,
                       std::array<vk::Format, GBuffer::kColorAttachmentCount> gbuffer_formats, vk::Format depth_format, vk::Format shadow_format,
                       std::uint32_t frames_in_flight) {
    destroy();
    device_ = &device;
    frames_ = frames_in_flight;

    // 3: los personajes que apartan las plantas (un hueco por frame en vuelo,
    // con offset dinamico: el mismo set para todos los frames).
    // TESELADO: con teselacion, la altura (binding 1) tambien en voxel.tese y el push en las dos etapas nuevas.
    const vk::ShaderStageFlags tess_stages =
        device.tessellationSupported() ? vk::ShaderStageFlags(vk::ShaderStageFlagBits::eTessellationControl |
                                                              vk::ShaderStageFlagBits::eTessellationEvaluation)
                                       : vk::ShaderStageFlags{};
    push_stages_ = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment | tess_stages;
    const vk::ShaderStageFlags height_stages =
        vk::ShaderStageFlagBits::eFragment |
        (device.tessellationSupported() ? vk::ShaderStageFlags(vk::ShaderStageFlagBits::eTessellationEvaluation)
                                        : vk::ShaderStageFlags{});
    const std::array<vk::DescriptorSetLayoutBinding, 4> bindings = {{
        {0, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eFragment},
        {1, vk::DescriptorType::eCombinedImageSampler, 1, height_stages},
        {2, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eFragment},
        {3, vk::DescriptorType::eUniformBufferDynamic, 1, vk::ShaderStageFlagBits::eVertex},
    }};
    vk::DescriptorSetLayoutCreateInfo set_info{};
    set_info.setBindings(bindings);
    set_layout_ = vk::raii::DescriptorSetLayout(device.handle(), set_info);

    const std::array<vk::DescriptorSetLayout, 2> set_layouts = {*frame_layout, *set_layout_};
    vk::PushConstantRange push{};
    push.stageFlags = push_stages_;
    push.size = sizeof(GpuVoxelPush);
    vk::PipelineLayoutCreateInfo layout_info{};
    layout_info.setSetLayouts(set_layouts);
    layout_info.setPushConstantRanges(push);
    layout_ = vk::raii::PipelineLayout(device.handle(), layout_info);

    const std::array<vk::DescriptorPoolSize, 2> sizes = {{
        {vk::DescriptorType::eCombinedImageSampler, 3},
        {vk::DescriptorType::eUniformBufferDynamic, 1},
    }};
    vk::DescriptorPoolCreateInfo pool_info{};
    pool_info.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    pool_info.maxSets = 1;
    pool_info.setPoolSizes(sizes);
    pool_ = vk::raii::DescriptorPool(device.handle(), pool_info);

    // Personajes: un hueco alineado por frame en vuelo, empezando sin ninguno.
    const vk::DeviceSize align = std::max<vk::DeviceSize>(device.limits().minUniformBufferOffsetAlignment, 16);
    interactor_slot_ = (sizeof(GpuFoliageInteractors) + align - 1) / align * align;
    interactor_buffer_.create(device, interactor_slot_ * frames_in_flight, vk::BufferUsageFlagBits::eUniformBuffer,
                              vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
                              "Voxeles: personajes");
    const GpuFoliageInteractors none{};
    for (std::uint32_t i = 0; i < frames_in_flight; ++i) interactor_buffer_.write(&none, sizeof(none), interactor_slot_ * i);

    createSampler(device);

    createPipelines(device, gbuffer_formats, depth_format, shadow_format);

    // Indices de los cuadrados: 0 1 2, 0 2 3 (+4 por cara), para cualquier seccion.
    std::vector<std::uint32_t> indices;
    indices.reserve(static_cast<std::size_t>(kMaxQuadsPerSection) * 6);
    for (std::uint32_t q = 0; q < kMaxQuadsPerSection; ++q) {
        const std::uint32_t b = q * 4;
        indices.insert(indices.end(), {b, b + 1, b + 2, b, b + 2, b + 3});
    }
    quad_indices_ = VulkanBuffer::createDeviceLocal(device, indices.data(), indices.size() * sizeof(std::uint32_t),
                                                    vk::BufferUsageFlagBits::eIndexBuffer);
    staging_.resize(frames_in_flight);
    occupancy_.create(device, frames_in_flight);
}

// Repetido, trilineal y con el filtrado de la configuracion grafica.
void VoxelPass::createSampler(const VulkanDevice& device) {
    vk::SamplerCreateInfo sampler{};
    sampler.magFilter = vk::Filter::eLinear;
    sampler.minFilter = vk::Filter::eLinear;
    sampler.mipmapMode = vk::SamplerMipmapMode::eLinear;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = vk::SamplerAddressMode::eRepeat;
    sampler.maxLod = VK_LOD_CLAMP_NONE;
    applyTextureFiltering(sampler, device.physicalDevice(), filtering_);
    sampler_ = vk::raii::Sampler(device.handle(), sampler);
}

void VoxelPass::setTextureFiltering(const TextureFiltering& filtering) {
    if (filtering == filtering_) return;
    filtering_ = filtering;
    if (device_ == nullptr || !*sampler_) return;
    createSampler(*device_);
    if (*set_ && *albedo_.view) writeSet(*device_);
}

void VoxelPass::destroy() {
    sections_.clear();
    upload_queue_.clear();
    retired_.clear();
    pages_.clear();
    for (VulkanBuffer& b : staging_) b.destroy();
    staging_.clear();
    quad_indices_.destroy();
    interactor_buffer_.destroy();
    interactor_slot_ = 0;
    interactors_.clear();
    interactor_gpu_.clear();
    interactor_count_ = 0;
    interactor_frame_ = ~0ull;
    albedo_ = ArrayTexture{};
    normal_ = ArrayTexture{};
    material_ = ArrayTexture{};
    layer_count_ = 0;
    set_ = nullptr;
    shadow_pipeline_ = nullptr;
    gbuffer_pipeline_ = nullptr;
    gbuffer_wire_pipeline_ = nullptr;
    gbuffer_lite_pipeline_ = nullptr;
    gbuffer_opaque_pipeline_ = nullptr;
    gbuffer_opaque_lite_pipeline_ = nullptr;
    gbuffer_tess_pipeline_ = nullptr;
    gbuffer_tess_wire_pipeline_ = nullptr;
    sampler_ = nullptr;
    pool_ = nullptr;
    layout_ = nullptr;
    set_layout_ = nullptr;
    visible_keys_.clear();
    all_keys_.clear();
    sun_keys_.clear();
    water_keys_.clear();
    water_count_ = 0;
    occupancy_.destroy();
    device_ = nullptr;
}

void VoxelPass::createPipelines(const VulkanDevice& device, std::array<vk::Format, GBuffer::kColorAttachmentCount> gbuffer_formats,
                                vk::Format depth_format, vk::Format shadow_format) {
    const vk::raii::ShaderModule vertex = shaders::loadModule(device, "voxel.vert.spv");
    const vk::raii::ShaderModule fragment = shaders::loadModule(device, "voxel.frag.spv");
    const vk::raii::ShaderModule shadow_fragment = shaders::loadModule(device, "voxel_shadow.frag.spv");

    const vk::VertexInputBindingDescription binding{0, sizeof(std::uint32_t) * 2, vk::VertexInputRate::eVertex};
    const vk::VertexInputAttributeDescription attribute{0, 0, vk::Format::eR32G32Uint, 0};
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

    // --- G-buffer (caras hacia fuera en sentido antihorario: se quitan las de atras) ---
    {
        const std::array<vk::PipelineShaderStageCreateInfo, 2> stages = {{
            {{}, vk::ShaderStageFlagBits::eVertex, *vertex, "main"},
            {{}, vk::ShaderStageFlagBits::eFragment, *fragment, "main"},
        }};
        vk::PipelineRasterizationStateCreateInfo raster{};
        raster.polygonMode = vk::PolygonMode::eFill;
        raster.cullMode = vk::CullModeFlagBits::eBack;
        raster.frontFace = vk::FrontFace::eCounterClockwise;
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
        // Variantes: constante 9 = ligero (gbuffer_surface.glsl, movil en Baja)
        // y 10 = opaco (voxel.frag sin discard).
        const auto variant = [&](bool lite, bool opaque) {
            const std::array<VkBool32, 2> values = {lite ? VK_TRUE : VK_FALSE, opaque ? VK_TRUE : VK_FALSE};
            const std::array<vk::SpecializationMapEntry, 2> entries = {
                vk::SpecializationMapEntry{9, 0, sizeof(VkBool32)},
                vk::SpecializationMapEntry{10, sizeof(VkBool32), sizeof(VkBool32)}};
            vk::SpecializationInfo spec{};
            spec.setMapEntries(entries);
            spec.dataSize = sizeof(values);
            spec.pData = values.data();
            std::array<vk::PipelineShaderStageCreateInfo, 2> variant_stages = stages;
            variant_stages[1].pSpecializationInfo = &spec;
            info.setStages(variant_stages);
            return compat::makeGraphicsPipeline(device, info);
        };
        gbuffer_lite_pipeline_ = variant(true, false);
        gbuffer_opaque_pipeline_ = variant(false, true);
        gbuffer_opaque_lite_pipeline_ = variant(true, true);
        info.setStages(stages);

        // TESELADO: las caras opacas cercanas como parches de 4 vertices (voxel.tesc / voxel.tese), con voxel.frag
        // opaco (constante 10) y sin el parallax que ya pone el teselado (11). Sin la caracteristica
        // tessellationShader no se crea: se queda el parallax.
        if (device.tessellationSupported()) {
            const vk::raii::ShaderModule control = shaders::loadModule(device, "voxel.tesc.spv");
            const vk::raii::ShaderModule evaluation = shaders::loadModule(device, "voxel.tese.spv");
            const std::array<VkBool32, 3> values = {VK_FALSE, VK_TRUE, VK_TRUE};
            const std::array<vk::SpecializationMapEntry, 3> entries = {
                vk::SpecializationMapEntry{9, 0, sizeof(VkBool32)},
                vk::SpecializationMapEntry{10, sizeof(VkBool32), sizeof(VkBool32)},
                vk::SpecializationMapEntry{11, 2 * sizeof(VkBool32), sizeof(VkBool32)}};
            vk::SpecializationInfo spec{};
            spec.setMapEntries(entries);
            spec.dataSize = sizeof(values);
            spec.pData = values.data();
            const std::array<vk::PipelineShaderStageCreateInfo, 4> tess_stages = {{
                {{}, vk::ShaderStageFlagBits::eVertex, *vertex, "main"},
                {{}, vk::ShaderStageFlagBits::eTessellationControl, *control, "main"},
                {{}, vk::ShaderStageFlagBits::eTessellationEvaluation, *evaluation, "main"},
                {{}, vk::ShaderStageFlagBits::eFragment, *fragment, "main", &spec},
            }};
            vk::PipelineInputAssemblyStateCreateInfo patches{};
            patches.topology = vk::PrimitiveTopology::ePatchList;
            vk::PipelineTessellationStateCreateInfo tessellation{};
            tessellation.patchControlPoints = 4;
            info.setStages(tess_stages);
            info.pInputAssemblyState = &patches;
            info.pTessellationState = &tessellation;
            gbuffer_tess_pipeline_ = compat::makeGraphicsPipeline(device, info);
            if (device.fillModeNonSolidSupported()) {
                raster.polygonMode = vk::PolygonMode::eLine;
                gbuffer_tess_wire_pipeline_ = compat::makeGraphicsPipeline(device, info);
                raster.polygonMode = vk::PolygonMode::eFill;
            }
            info.setStages(stages);
            info.pInputAssemblyState = &input_assembly;
            info.pTessellationState = nullptr;
        }
    }
    // --- Sombras (profundidad; el fragmento solo recorta hojas y plantas) ---
    {
        const std::array<vk::PipelineShaderStageCreateInfo, 2> stages = {{
            {{}, vk::ShaderStageFlagBits::eVertex, *vertex, "main"},
            {{}, vk::ShaderStageFlagBits::eFragment, *shadow_fragment, "main"},
        }};
        vk::PipelineRasterizationStateCreateInfo raster{};
        raster.polygonMode = vk::PolygonMode::eFill;
        raster.cullMode = vk::CullModeFlagBits::eNone;
        raster.lineWidth = 1.0f;
        raster.depthClampEnable = device.depthClampSupported() ? VK_TRUE : VK_FALSE;
        raster.depthBiasEnable = VK_TRUE;
        raster.depthBiasConstantFactor = 1.0f;
        raster.depthBiasSlopeFactor = 1.75f;
        vk::PipelineDepthStencilStateCreateInfo depth{};
        depth.depthTestEnable = VK_TRUE;
        depth.depthWriteEnable = VK_TRUE;
        depth.depthCompareOp = vk::CompareOp::eLessOrEqual;
        vk::PipelineColorBlendStateCreateInfo blend{};
        vk::PipelineRenderingCreateInfo rendering{};
        rendering.depthAttachmentFormat = shadow_format;
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
        shadow_pipeline_ = compat::makeGraphicsPipeline(device, info);
    }
}

// -----------------------------------------------------------------------------
// Texturas
// -----------------------------------------------------------------------------

void VoxelPass::setTextures(std::uint32_t size, const std::vector<VoxelTextureLayer>& layers) {
    if (device_ == nullptr || layers.empty() || size < 4 || !std::has_single_bit(size)) return;
    const VulkanDevice& device = *device_;
    device.waitIdle();
    const auto count = static_cast<std::uint32_t>(layers.size());
    // Para los rayos (VoxelOccupancy): color medio de lo opaco de cada capa y
    // si tiene huecos (hojas, cristal: no tapan en la ocupacion).
    {
        std::vector<VoxelOccupancy::LayerInfo> infos(count);
        const std::size_t texels = static_cast<std::size_t>(size) * size;
        // Sin saber (capa sin datos): con recorte, como antes.
        layer_cutout_.assign(count, 1);
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::vector<std::uint8_t>& albedo = layers[i].albedo;
            if (albedo.size() != texels * 4) continue;
            double sum[3] = {0.0, 0.0, 0.0};
            std::size_t solid = 0;
            for (std::size_t t = 0; t < texels; ++t) {
                if (albedo[t * 4 + 3] < 128) continue;
                for (int c = 0; c < 3; ++c) sum[c] += std::pow(albedo[t * 4 + c] / 255.0, 2.2);
                ++solid;
            }
            infos[i].opaque = static_cast<double>(solid) >= 0.9 * static_cast<double>(texels);
            layer_cutout_[i] = solid < texels ? 1 : 0;
            if (solid > 0) {
                infos[i].albedo = core::Vec3{static_cast<float>(sum[0] / solid), static_cast<float>(sum[1] / solid),
                                             static_cast<float>(sum[2] / solid)};
            }
        }
        occupancy_.setLayers(std::move(infos));
    }
    const std::uint32_t mips = static_cast<std::uint32_t>(std::bit_width(size));
    const vk::DeviceSize layer_bytes = static_cast<vk::DeviceSize>(size) * size * 4;

    const auto build = [&](ArrayTexture& texture, int which) {
        texture = ArrayTexture{};
        vk::ImageCreateInfo info{};
        info.imageType = vk::ImageType::e2D;
        info.format = kLayerFormat;
        info.extent = vk::Extent3D{size, size, 1};
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
        allocate.memoryTypeIndex =
            device.findMemoryType(requirements.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal);
        texture.memory = vk::raii::DeviceMemory(device.handle(), allocate);
        texture.image.bindMemory(*texture.memory, 0);

        VulkanBuffer staging;
        staging.create(device, layer_bytes * count, vk::BufferUsageFlagBits::eTransferSrc,
                       vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
        std::vector<std::uint8_t> fallback(static_cast<std::size_t>(layer_bytes), 255);
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::vector<std::uint8_t>& src =
                which == 0 ? layers[i].albedo : (which == 1 ? layers[i].normal : layers[i].material);
            staging.write(src.size() == layer_bytes ? src.data() : fallback.data(), layer_bytes, layer_bytes * i);
        }
        const vk::Image image = *texture.image;
        device.submitOneTime([&](const vk::raii::CommandBuffer& cmd) {
            imageBarrier(cmd, image, vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
                         vk::PipelineStageFlagBits2::eTopOfPipe, vk::AccessFlagBits2::eNone,
                         vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite, 0, mips, count);
            std::vector<vk::BufferImageCopy> regions;
            for (std::uint32_t i = 0; i < count; ++i) {
                vk::BufferImageCopy region{};
                region.bufferOffset = layer_bytes * i;
                region.imageSubresource = vk::ImageSubresourceLayers{vk::ImageAspectFlagBits::eColor, 0, i, 1};
                region.imageExtent = vk::Extent3D{size, size, 1};
                regions.push_back(region);
            }
            cmd.copyBufferToImage(*staging.handle(), image, vk::ImageLayout::eTransferDstOptimal, regions);
            std::int32_t s = static_cast<std::int32_t>(size);
            for (std::uint32_t level = 1; level < mips; ++level) {
                imageBarrier(cmd, image, vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eTransferSrcOptimal,
                             vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite,
                             vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead, level - 1, 1,
                             count);
                vk::ImageBlit blit{};
                blit.srcSubresource = vk::ImageSubresourceLayers{vk::ImageAspectFlagBits::eColor, level - 1, 0, count};
                blit.srcOffsets[1] = vk::Offset3D{s, s, 1};
                const std::int32_t next = std::max(s / 2, 1);
                blit.dstSubresource = vk::ImageSubresourceLayers{vk::ImageAspectFlagBits::eColor, level, 0, count};
                blit.dstOffsets[1] = vk::Offset3D{next, next, 1};
                cmd.blitImage(image, vk::ImageLayout::eTransferSrcOptimal, image, vk::ImageLayout::eTransferDstOptimal,
                              blit, vk::Filter::eLinear);
                imageBarrier(cmd, image, vk::ImageLayout::eTransferSrcOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
                             vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead,
                             vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderSampledRead,
                             level - 1, 1, count);
                s = next;
            }
            imageBarrier(cmd, image, vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
                         vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite,
                         vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderSampledRead,
                         mips - 1, 1, count);
        });
        vk::ImageViewCreateInfo view{};
        view.image = image;
        view.viewType = vk::ImageViewType::e2DArray;
        view.format = kLayerFormat;
        view.subresourceRange = vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, mips, 0, count};
        texture.view = vk::raii::ImageView(device.handle(), view);
    };
    build(albedo_, 0);
    build(normal_, 1);
    build(material_, 2);
    layer_count_ = count;

    if (!*set_) {
        vk::DescriptorSetAllocateInfo allocate{};
        allocate.descriptorPool = *pool_;
        allocate.setSetLayouts(*set_layout_);
        set_ = std::move(vk::raii::DescriptorSets(device.handle(), allocate).front());
    }
    writeSet(device);
}

void VoxelPass::writeSet(const VulkanDevice& device) {
    const vk::DescriptorImageInfo a{*sampler_, *albedo_.view, vk::ImageLayout::eShaderReadOnlyOptimal};
    const vk::DescriptorImageInfo n{*sampler_, *normal_.view, vk::ImageLayout::eShaderReadOnlyOptimal};
    const vk::DescriptorImageInfo m{*sampler_, *material_.view, vk::ImageLayout::eShaderReadOnlyOptimal};
    const vk::DescriptorBufferInfo people{*interactor_buffer_.handle(), 0, sizeof(GpuFoliageInteractors)};
    const std::array<vk::WriteDescriptorSet, 4> writes = {{
        {*set_, 0, 0, 1, vk::DescriptorType::eCombinedImageSampler, &a},
        {*set_, 1, 0, 1, vk::DescriptorType::eCombinedImageSampler, &n},
        {*set_, 2, 0, 1, vk::DescriptorType::eCombinedImageSampler, &m},
        {*set_, 3, 0, 1, vk::DescriptorType::eUniformBufferDynamic, nullptr, &people},
    }};
    device.handle().updateDescriptorSets(writes, nullptr);
}

// -----------------------------------------------------------------------------
// Memoria de vertices (paginas repartidas)
// -----------------------------------------------------------------------------

VoxelPass::Allocation VoxelPass::allocate(std::uint64_t bytes) {
    bytes = (bytes + kQuadBytes - 1) / kQuadBytes * kQuadBytes;
    const auto take = [&](int index) -> Allocation {
        Page& page = *pages_[static_cast<std::size_t>(index)];
        for (auto it = page.free.begin(); it != page.free.end(); ++it) {
            if (it->second < bytes) continue;
            Allocation a{index, it->first, bytes};
            it->first += bytes;
            it->second -= bytes;
            if (it->second == 0) page.free.erase(it);
            return a;
        }
        return {};
    };
    for (int i = 0; i < static_cast<int>(pages_.size()); ++i) {
        if (Allocation a = take(i); a.valid()) return a;
    }
    auto page = std::make_unique<Page>();
    page->capacity = std::max(kPageBytes, bytes);
    page->buffer.create(*device_, page->capacity,
                        vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eTransferDst,
                        vk::MemoryPropertyFlagBits::eDeviceLocal);
    page->free.emplace_back(0, page->capacity);
    pages_.push_back(std::move(page));
    return take(static_cast<int>(pages_.size()) - 1);
}

void VoxelPass::release(const Allocation& allocation) {
    if (!allocation.valid()) return;
    Page& page = *pages_[static_cast<std::size_t>(allocation.page)];
    auto& free = page.free;
    const auto it = std::lower_bound(free.begin(), free.end(), std::make_pair(allocation.offset, std::uint64_t{0}));
    auto inserted = free.insert(it, {allocation.offset, allocation.size});
    // Une con los huecos vecinos.
    if (std::next(inserted) != free.end() && inserted->first + inserted->second == std::next(inserted)->first) {
        inserted->second += std::next(inserted)->second;
        free.erase(std::next(inserted));
    }
    if (inserted != free.begin() && std::prev(inserted)->first + std::prev(inserted)->second == inserted->first) {
        std::prev(inserted)->second += inserted->second;
        free.erase(inserted);
    }
}

// Se libera cuando ningun frame en vuelo la puede estar leyendo.
void VoxelPass::retire(const Allocation& allocation) {
    if (allocation.valid()) retired_.push_back({allocation, frame_counter_ + frames_ + 1});
}

// -----------------------------------------------------------------------------
// Secciones
// -----------------------------------------------------------------------------

void VoxelPass::setSection(std::uint64_t key, const core::Vec3& origin, const std::uint32_t* vertices,
                           std::uint32_t vertex_count, float scale) {
    if (device_ == nullptr) return;
    const std::uint32_t quads = std::min(vertex_count / 4, kMaxQuadsPerSection);
    if (quads == 0 || vertices == nullptr) {
        removeSection(key);
        return;
    }
    const bool is_new = sections_.find(key) == sections_.end();
    Section& section = sections_[key];
    section.origin = origin;
    section.water = (key & kWaterKey) != 0;
    section.distant = (key & kFarKey) != 0;
    section.scale = scale;
    if (is_new && section.water) ++water_count_;
    section.pending_data.assign(vertices, vertices + static_cast<std::size_t>(quads) * 8);
    // Quads por cara (orden estable dentro de cada una). El agua no: su orden
    // es el de la mezcla.
    section.pending_face_start.fill(0);
    if (section.water) {
        section.pending_face_start[16] = quads;  // todo en el ultimo grupo (drawWater lo dibuja entero)
    } else {
        // Grupo = cara + 8 si su capa tiene recorte (lo lejano, todo con
        // recorte: se deshace con un tramado).
        const bool distant = section.distant;
        const auto group = [&](const std::uint32_t* v) {
            const std::uint32_t face = (v[0] >> 15) & 7u;
            if (distant) return face + 8u;
            const std::uint32_t layer = v[1] & 1023u;
            const bool cut = layer >= layer_cutout_.size() || layer_cutout_[layer] != 0;
            return face + (cut ? 8u : 0u);
        };
        std::array<std::uint32_t, 16> count{};
        for (std::uint32_t q = 0; q < quads; ++q) ++count[group(vertices + static_cast<std::size_t>(q) * 8)];
        for (std::size_t f = 0; f < 16; ++f) section.pending_face_start[f + 1] = section.pending_face_start[f] + count[f];
        bool sorted = true;  // ya ordenados (el caso tipico no lo es, pero asi no se copia de mas)
        for (std::uint32_t q = 1; q < quads && sorted; ++q) {
            sorted = group(vertices + static_cast<std::size_t>(q - 1) * 8) <= group(vertices + static_cast<std::size_t>(q) * 8);
        }
        if (!sorted) {
            std::array<std::uint32_t, 16> next{};
            for (std::size_t f = 0; f < 16; ++f) next[f] = section.pending_face_start[f];
            for (std::uint32_t q = 0; q < quads; ++q) {
                const std::uint32_t* src = vertices + static_cast<std::size_t>(q) * 8;
                const std::uint32_t dst = next[group(src)]++;
                std::copy(src, src + 8, section.pending_data.begin() + static_cast<std::ptrdiff_t>(dst) * 8);
            }
        }
    }
    // Luz del cielo de sus vertices (b, bits 10..13; tambien en los finos): si
    // ninguna cara da a aire con luz del cielo es una cueva enterrada y no
    // proyecta al sol (recordShadow con sun). Con el redondeo de la media de
    // las esquinas, una cara junto a luz 1 puede quedar en 0: alli el sol
    // tampoco llega (la luz 15 esta a 14 pasos).
    section.pending_sky = true;
    if (!section.water && !section.distant) {
        bool sky = false;
        for (std::size_t i = 1; i < section.pending_data.size(); i += 2) {
            if ((section.pending_data[i] & (15u << 10)) != 0) {
                sky = true;
                break;
            }
        }
        section.pending_sky = sky;
    }
    section.pending_quads = quads;
    // Caras de bloque: un quad fusionado (b bit 31) cuenta su area en bloques.
    section.pending_faces = quads;
    if (!section.water && !section.distant) {
        for (std::uint32_t q = 0; q < quads; ++q) {
            const std::uint32_t* v = vertices + static_cast<std::size_t>(q) * 8;
            if ((v[1] >> 31u) == 0u) continue;
            std::uint32_t lo[3] = {31, 31, 31}, hi[3] = {0, 0, 0};
            for (int k = 0; k < 4; ++k) {
                const std::uint32_t a = v[k * 2];
                const std::uint32_t p[3] = {a & 31u, (a >> 5u) & 31u, (a >> 10u) & 31u};
                for (int i = 0; i < 3; ++i) {
                    lo[i] = std::min(lo[i], p[i]);
                    hi[i] = std::max(hi[i], p[i]);
                }
            }
            std::uint32_t area = 1;
            for (int i = 0; i < 3; ++i) area *= std::max(hi[i] - lo[i], 1u);
            section.pending_faces += area - 1;
        }
    }
    // Los rayos ven los bloques por su ocupacion (VoxelOccupancy), no por la malla.
    if (!section.water && !section.distant) occupancy_.setSection(key, origin, vertices, quads);
    if (!section.queued) {
        section.queued = true;
        upload_queue_.push_back(key);
    }
}

void VoxelPass::removeSection(std::uint64_t key) {
    occupancy_.removeSection(key);
    const auto it = sections_.find(key);
    if (it == sections_.end()) return;
    retire(it->second.live);
    retire(it->second.pending);
    if (it->second.water && water_count_ > 0) --water_count_;
    sections_.erase(it);  // su entrada en la cola se salta al subir
}

void VoxelPass::clearSections() {
    for (auto& [key, section] : sections_) {
        retire(section.live);
        retire(section.pending);
    }
    sections_.clear();
    upload_queue_.clear();
    water_count_ = 0;
    water_keys_.clear();
    occupancy_.clear();
}

bool VoxelPass::recordUploads(const vk::raii::CommandBuffer& cmd, std::uint32_t frame) {
    ++frame_counter_;
    // Lo retirado hace mas de frames_ frames ya no lo lee nadie.
    for (auto it = retired_.begin(); it != retired_.end();) {
        if (it->release_frame <= frame_counter_) {
            release(it->allocation);
            it = retired_.erase(it);
        } else {
            ++it;
        }
    }
    if (upload_queue_.empty() || frame >= staging_.size()) return false;

    // Cuanto cabe este frame.
    std::vector<std::uint64_t> batch;
    std::uint64_t total = 0;
    bool near_changed = false;  // el terreno lejano no proyecta: no invalida las sombras cacheadas
    while (!upload_queue_.empty()) {
        const std::uint64_t key = upload_queue_.front();
        const auto it = sections_.find(key);
        if (it == sections_.end() || !it->second.queued) {
            upload_queue_.pop_front();
            continue;
        }
        const std::uint64_t bytes = static_cast<std::uint64_t>(it->second.pending_quads) * kQuadBytes;
        if (!batch.empty() && total + bytes > kUploadBudget) break;
        upload_queue_.pop_front();
        batch.push_back(key);
        total += bytes;
        if (!it->second.distant) near_changed = true;
    }
    if (batch.empty()) return false;

    VulkanBuffer& staging = staging_[frame];
    if (!staging.isValid() || staging.size() < total) {
        staging.destroy();
        staging.create(*device_, std::max<vk::DeviceSize>(total * 2, 4ull << 20), vk::BufferUsageFlagBits::eTransferSrc,
                       vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
    }
    std::uint64_t offset = 0;
    std::vector<vk::BufferMemoryBarrier2> barriers;
    for (const std::uint64_t key : batch) {
        Section& s = sections_.at(key);
        const std::uint64_t bytes = static_cast<std::uint64_t>(s.pending_quads) * kQuadBytes;
        const Allocation a = allocate(bytes);
        if (!a.valid()) continue;
        staging.write(s.pending_data.data(), bytes, offset);
        const vk::BufferCopy region{offset, a.offset, bytes};
        const vk::Buffer target = *pages_[static_cast<std::size_t>(a.page)]->buffer.handle();
        cmd.copyBuffer(*staging.handle(), target, region);
        vk::BufferMemoryBarrier2 b{};
        b.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
        b.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
        b.dstStageMask = vk::PipelineStageFlagBits2::eVertexAttributeInput;
        b.dstAccessMask = vk::AccessFlagBits2::eVertexAttributeRead;
        b.buffer = target;
        b.offset = a.offset;
        b.size = bytes;
        barriers.push_back(b);
        offset += bytes;
        // La nueva pasa a dibujarse; la vieja espera a que la suelten los frames en vuelo.
        retire(s.live);
        s.live = a;
        s.quads = s.pending_quads;
        s.faces = s.pending_faces;
        s.face_start = s.pending_face_start;
        s.sky = s.pending_sky;
        s.pending_data.clear();
        s.pending_data.shrink_to_fit();
        s.queued = false;
    }
    if (!barriers.empty()) {
        vk::DependencyInfo dependency{};
        dependency.setBufferMemoryBarriers(barriers);
        compat::pipelineBarrier(cmd, dependency);
    }
    return near_changed;
}

core::Aabb VoxelPass::sectionBox(const Section& s) const {
    if (!s.distant) return core::Aabb{s.origin, s.origin + core::Vec3{16.0f, 16.0f, 16.0f}};
    const float size = 16.0f * s.scale;
    return core::Aabb{s.origin, s.origin + core::Vec3{size, 320.0f, size}};
}

void VoxelPass::prepare(std::uint32_t frame, const core::Vec3& camera_position, const core::Mat4& view_projection) {
    // El tiempo con el que se dibujo el frame anterior: el viento de las
    // hojas se evalua tambien entonces para su vector de movimiento.
    previous_time_ = prepared_time_;
    prepared_time_ = time_;
    view_position_ = camera_position;
    updateInteractors(frame, camera_position);
    visible_keys_.clear();
    all_keys_.clear();
    sun_keys_.clear();
    water_keys_.clear();
    if (!visible_ || layer_count_ == 0) return;
    const core::Frustum frustum(view_projection);
    std::vector<std::pair<float, std::uint64_t>> sorted;
    std::vector<std::pair<float, std::uint64_t>> water;
    sorted.reserve(sections_.size());
    far_visible_ = 0;
    const float hole2 = far_hole_radius_ > 0.0f ? far_hole_radius_ * far_hole_radius_ : -1.0f;
    // Diagnostico: CRAMION_VOXEL_SUN_ALL=1 mete tambien las cuevas cerradas en
    // las cascadas del sol (como antes), para comparar.
    static const bool sun_all = std::getenv("CRAMION_VOXEL_SUN_ALL") != nullptr;
    for (const auto& [key, s] : sections_) {
        if (!s.live.valid() || s.quads == 0) continue;
        if (!s.water && !s.distant) {  // lo lejano no proyecta sombra
            all_keys_.push_back(key);
            if (s.sky || sun_all) sun_keys_.push_back(key);
        }
        const core::Aabb box = sectionBox(s);
        if (s.distant) {
            // Toda dentro de los chunks de verdad: no se ve.
            const float x0 = box.min.x - far_hole_.x, x1 = box.max.x - far_hole_.x;
            const float z0 = box.min.z - far_hole_.z, z1 = box.max.z - far_hole_.z;
            const float fx = std::max(x0 * x0, x1 * x1), fz = std::max(z0 * z0, z1 * z1);
            if (fx + fz < hole2) continue;
        }
        if (!frustum.intersects(box)) continue;
        if (s.distant) ++far_visible_;
        const core::Vec3 d = (box.min + box.max) * 0.5f - camera_position;
        // (Las lejanas, por la distancia a su punto mas cercano en el suelo:
        // se dibujan despues de los chunks de verdad que las tapan.)
        float distance = core::dot(d, d);
        if (s.distant) {
            const float dx = std::max({box.min.x - camera_position.x, 0.0f, camera_position.x - box.max.x});
            const float dz = std::max({box.min.z - camera_position.z, 0.0f, camera_position.z - box.max.z});
            distance = std::max(dx * dx + dz * dz, hole2) + 1.0f;
        }
        (s.water ? water : sorted).emplace_back(distance, key);
    }
    // De cerca a lejos: lo cercano tapa y la prueba de profundidad descarta el resto.
    std::sort(sorted.begin(), sorted.end());
    for (const auto& [distance, key] : sorted) visible_keys_.push_back(key);
    // El agua (transparente), de lejos a cerca.
    std::sort(water.begin(), water.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (const auto& [distance, key] : water) water_keys_.push_back(key);
}

void VoxelPass::drawWater(const vk::raii::CommandBuffer& cmd, const std::function<void(const core::Vec3&)>& before_draw) const {
    if (water_keys_.empty()) return;
    cmd.bindIndexBuffer(*quad_indices_.handle(), 0, vk::IndexType::eUint32);
    int bound_page = -1;
    for (const std::uint64_t key : water_keys_) {
        const auto it = sections_.find(key);
        if (it == sections_.end() || !it->second.live.valid()) continue;
        const Section& s = it->second;
        if (s.live.page != bound_page) {
            cmd.bindVertexBuffers(0, *pages_[static_cast<std::size_t>(s.live.page)]->buffer.handle(), {0});
            bound_page = s.live.page;
        }
        before_draw(s.origin);
        cmd.drawIndexed(s.quads * 6, 1, 0, static_cast<std::int32_t>(s.live.offset / 8), 0);
    }
}

bool VoxelPass::tessNear(const Section& s) const {
    if (s.distant || s.water) return false;
    const core::Aabb box = sectionBox(s);
    const core::Vec3& c = view_position_;
    const float dx = std::max({box.min.x - c.x, 0.0f, c.x - box.max.x});
    const float dy = std::max({box.min.y - c.y, 0.0f, c.y - box.max.y});
    const float dz = std::max({box.min.z - c.z, 0.0f, c.z - box.max.z});
    return dx * dx + dy * dy + dz * dz < tess_distance_ * tess_distance_;
}

bool VoxelPass::setTessellation(bool enabled, float distance, float depth, float density) {
    tess_enabled_ = enabled;
    tess_distance_ = std::clamp(distance, 0.0f, 128.0f);
    // Mas de ~0.15 se saldria de lo que voxelLiftOrigin (rayos) y el empuje de las sombras cubren.
    tess_depth_ = std::clamp(depth, 0.0f, 0.15f);
    tess_density_ = std::clamp(density, 1.0f, 64.0f);
    // Lo que proyectan las sombras depende de si hay empuje y de cuanto.
    const float wanted = tessellationActive() ? tess_depth_ : 0.0f;
    const bool changed = wanted != tess_shadow_depth_;
    tess_shadow_depth_ = wanted;
    return changed;
}

void VoxelPass::drawSections(const vk::raii::CommandBuffer& cmd, const std::vector<std::uint64_t>& keys,
                             const core::Mat4* light_view_projection,
                             const std::function<bool(const core::Aabb&)>* keep, bool ignore_near, int groups,
                             int tess) const {
    int bound_page = -1;
    std::optional<core::Frustum> light_frustum;
    if (light_view_projection != nullptr) light_frustum.emplace(*light_view_projection, ignore_near);
    const bool tess_active = tessellationActive() && tess_depth_ > 0.0f;
    for (const std::uint64_t key : keys) {
        const auto it = sections_.find(key);
        if (it == sections_.end() || !it->second.live.valid()) continue;
        const Section& s = it->second;
        if (light_frustum && !light_frustum->intersects(sectionBox(s))) {
            continue;
        }
        if (keep != nullptr && *keep && !(*keep)(sectionBox(s))) continue;
        // Sin ordenar por grupos (no deberia pasar) o sombras: entera, en la
        // pasada de recorte.
        const bool whole = light_view_projection != nullptr || s.face_start[16] != s.quads;
        const std::uint32_t g_begin = groups == 1 ? 8u : 0u, g_end = groups == 0 ? 8u : 16u;
        if (whole ? groups == 0 : s.face_start[g_begin] == s.face_start[g_end]) continue;
        // TESELADO: las cercanas van en dos pasadas (sus caras 0-5 con teselado, lo demas sin).
        const bool close = tess != 0 && !whole && tessNear(s);
        if (tess == 1 && !close) continue;
        if (s.live.page != bound_page) {
            cmd.bindVertexBuffers(0, *pages_[static_cast<std::size_t>(s.live.page)]->buffer.handle(), {0});
            bound_page = s.live.page;
        }
        GpuVoxelPush push{};
        push.origin_time = core::Vec4{s.origin.x, s.origin.y, s.origin.z, time_};
        push.previous_time = previous_time_;
        push.style = (flat_colors_ ? 1u : 0u) | (low_quality_ ? 4u : 0u) | (lowest_quality_ ? 8u : 0u);
        if (s.distant) {
            push.style |= 2u;
            push.distant = core::Vec4{far_hole_.x, far_hole_.z, far_hole_radius_, s.scale};
            push.far_fade_end = far_fade_end_;
        }
        if (light_view_projection != nullptr) {
            push.light_view_projection = *light_view_projection;
            push.shadow = 1;
        }
        // TESELADO: en el G-buffer, para el relieve y el parallax que le sigue; en las sombras, el empuje de las
        // caras hacia la luz (voxel.vert shadowCaster), en todas las secciones (no depende de la camara).
        if (tess_active && !s.distant) {
            push.tess = core::Vec4{tess_distance_, tess_depth_, tess_density_, 0.0f};
        }
        cmd.pushConstants<GpuVoxelPush>(*layout_, push_stages_, 0, push);
        const auto first_vertex = static_cast<std::int32_t>(s.live.offset / 8);
        const bool stats = light_view_projection == nullptr;
        if (whole) {
            cmd.drawIndexed(s.quads * 6, 1, 0, first_vertex, 0);
            if (stats) {
                drawn_quads_ += s.quads;
                if (s.distant) drawn_far_quads_ += s.quads;
                ++draw_calls_;
            }
            continue;
        }
        // G-buffer: solo los grupos de caras que pueden mirar a la camara (el
        // pipeline quita las de atras, pero antes las sombreaba el vertex
        // shader). Con 2 bloques de margen por el viento y las plantas que se
        // apartan. Los grupos seguidos van en una sola llamada. Con camaras
        // ortograficas (setFaceCulling(false)), todos.
        const core::Aabb box = sectionBox(s);
        const core::Vec3& c = view_position_;
        constexpr float kMargin = 2.0f;
        // Plantas (cruces, grupo 6; PASTO-HZ: las matas de hierba, cara 7) solo cerca si hay tope (setPlantDistance).
        bool plants = true;
        if (face_culling_ && plant_distance_ > 0.0f && !s.distant) {
            const float dx = std::max({box.min.x - c.x, 0.0f, c.x - box.max.x});
            const float dy = std::max({box.min.y - c.y, 0.0f, c.y - box.max.y});
            const float dz = std::max({box.min.z - c.z, 0.0f, c.z - box.max.z});
            plants = dx * dx + dy * dy + dz * dz <= plant_distance_ * plant_distance_;
        }
        bool visible[8] = {c.x > box.min.x - kMargin, c.x < box.max.x + kMargin, c.y > box.min.y - kMargin,
                           c.y < box.max.y + kMargin, c.z > box.min.z - kMargin, c.z < box.max.z + kMargin,
                           plants, plants};  // (grupo 7: laminas de hierba, como las plantas)
        if (!face_culling_) {
            for (bool& v : visible) v = true;
        }
        if (tess == 1) {
            visible[6] = visible[7] = false;  // plantas y demas: sin teselado
        } else if (close) {
            for (int f = 0; f < 6; ++f) visible[f] = false;  // ya van teselados
        }
        // Parches de 4 vertices sin indices (el pipeline teselado) o cuadrados de 6 indices.
        const auto draw_run = [&](std::uint32_t begin, std::uint32_t end) {
            if (tess == 1) {
                cmd.draw((end - begin) * 4, 1, static_cast<std::uint32_t>(first_vertex) + begin * 4, 0);
                drawn_tess_quads_ += end - begin;
            } else {
                cmd.drawIndexed((end - begin) * 6, 1, begin * 6, first_vertex, 0);
            }
            drawn_quads_ += end - begin;
            if (s.distant) drawn_far_quads_ += end - begin;
            ++draw_calls_;
        };
        std::uint32_t run_begin = 0, run_end = 0;
        for (std::size_t f = g_begin; f < g_end; ++f) {
            const std::uint32_t begin = s.face_start[f], end = s.face_start[f + 1];
            if (begin == end) continue;
            if (visible[f & 7u] && run_end == begin && run_end > run_begin) {
                run_end = end;  // sigue la tanda
                continue;
            }
            if (run_end > run_begin) draw_run(run_begin, run_end);
            run_begin = run_end = 0;
            if (visible[f & 7u]) {
                run_begin = begin;
                run_end = end;
            }
        }
        if (run_end > run_begin) draw_run(run_begin, run_end);
    }
}

void VoxelPass::recordGBuffer(const vk::raii::CommandBuffer& cmd, std::uint32_t frame,
                              const vk::raii::DescriptorSet& frame_set) const {
    if (visible_keys_.empty() || !*set_) return;
    const bool wire = wireframe_ && *gbuffer_wire_pipeline_;
    const bool lite = lowest_quality_ && *gbuffer_lite_pipeline_;
    const bool split = !wire && *gbuffer_opaque_pipeline_ && *gbuffer_opaque_lite_pipeline_;
    // TESELADO: las caras opacas cercanas primero (son las que mas tapan), con su pipeline.
    const bool tess = tessellationActive() && tess_depth_ > 0.0f && !lite &&
                      (wire ? *gbuffer_tess_wire_pipeline_ != nullptr : split);
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *layout_, 0, *frame_set, nullptr);
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *layout_, 1, *set_, interactorOffset(frame));
    cmd.bindIndexBuffer(*quad_indices_.handle(), 0, vk::IndexType::eUint32);
    drawn_quads_ = drawn_far_quads_ = drawn_tess_quads_ = 0;
    draw_calls_ = 0;
    if (tess) {
        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, wire ? *gbuffer_tess_wire_pipeline_ : *gbuffer_tess_pipeline_);
        drawSections(cmd, visible_keys_, nullptr, nullptr, false, 0, 1);
    }
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics,
                     wire    ? *gbuffer_wire_pipeline_
                     : split ? (lite ? *gbuffer_opaque_lite_pipeline_ : *gbuffer_opaque_pipeline_)
                     : lite  ? *gbuffer_lite_pipeline_
                             : *gbuffer_pipeline_);
    if (!split) {
        if (tess) {  // (vista Wireframe) lo opaco sin lo ya teselado, despues lo recortado
            drawSections(cmd, visible_keys_, nullptr, nullptr, false, 0, 2);
            drawSections(cmd, visible_keys_, nullptr, nullptr, false, 1);
            return;
        }
        drawSections(cmd, visible_keys_, nullptr);
        return;
    }
    // Primero lo opaco (sin discard: la profundidad se escribe antes del
    // shader y tapa lo de detras), despues lo recortado y lo lejano.
    drawSections(cmd, visible_keys_, nullptr, nullptr, false, 0, tess ? 2 : 0);
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, lite ? *gbuffer_lite_pipeline_ : *gbuffer_pipeline_);
    drawSections(cmd, visible_keys_, nullptr, nullptr, false, 1);
}

void VoxelPass::recordShadow(const vk::raii::CommandBuffer& cmd, std::uint32_t frame,
                             const vk::raii::DescriptorSet& frame_set, const core::Mat4& light_view_projection,
                             bool sun, const std::function<bool(const core::Aabb&)>& keep) const {
    const std::vector<std::uint64_t>& keys = sun ? sun_keys_ : all_keys_;
    if (keys.empty() || !*set_) return;
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *shadow_pipeline_);
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *layout_, 0, *frame_set, nullptr);
    // (Las plantas tambien se apartan en las sombras.)
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *layout_, 1, *set_, interactorOffset(frame));
    cmd.bindIndexBuffer(*quad_indices_.handle(), 0, vk::IndexType::eUint32);
    // Cascadas del sol: sin plano cercano, como el terreno y los actores. Se
    // dibujan con depth clamp, asi que lo que queda entre el sol y la caja de
    // la cascada (mas de radio + 60 m hacia el sol) tambien tapa. Sin esto, al
    // atardecer (sol rasante) la corteza de un monte o el techo de una cueva
    // quedaba fuera y, como las secciones cerradas ya no se dibujan al sol,
    // nada tapaba: el sol atravesaba los voxeles.
    drawSections(cmd, keys, &light_view_projection, &keep, /*ignore_near=*/sun);
}

// -----------------------------------------------------------------------------
// Personajes que apartan las plantas
// -----------------------------------------------------------------------------

void VoxelPass::updateInteractors(std::uint32_t frame, const core::Vec3& camera_position) {
    if (!interactor_buffer_.isValid() || frames_ == 0) return;
    // Una vez por frame (prepare puede repetirse en el mismo: vistas aisladas).
    if (interactor_frame_ == frame_counter_ && !interactor_gpu_.empty()) return;
    interactor_frame_ = frame_counter_;
    const float dt = std::clamp(time_ - interactor_time_, 0.0f, 0.1f);
    interactor_time_ = time_;

    // Las fuentes juntas: sin repetir al mismo personaje (su Voxel.moveBox y su
    // objeto de red), solo cerca de la camara.
    std::vector<core::Vec4> incoming;
    for (std::size_t s = interactor_sources_.size(); s-- > 0;) {  // primero los voxeles (pies exactos)
        for (const core::Vec4& sphere : interactor_sources_[s]) {
            const core::Vec3 p{sphere.x, sphere.y, sphere.z};
            if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) continue;
            if (core::length(p - camera_position) > kInteractorRange) continue;
            bool repeated = false;
            for (core::Vec4& other : incoming) {
                const float dx = other.x - p.x, dz = other.z - p.z;
                if (dx * dx + dz * dz < kInteractorMerge * kInteractorMerge && std::abs(other.y - p.y) < 1.2f) {
                    other.w = std::max(other.w, sphere.w);
                    repeated = true;
                    break;
                }
            }
            if (!repeated) incoming.push_back(core::Vec4{p.x, p.y, p.z, std::clamp(sphere.w, 0.1f, 3.0f)});
        }
    }

    // Cada uno con el que ya se seguia mas cerca (por parejas, de la mas
    // cercana a la mas lejana): asi conserva su estela y su fuerza.
    for (Interactor& it : interactors_) it.seen = false;
    std::vector<bool> taken(incoming.size(), false);
    struct Pair {
        float distance;
        std::size_t tracked;
        std::size_t candidate;
    };
    std::vector<Pair> pairs;
    for (std::size_t t = 0; t < interactors_.size(); ++t) {
        for (std::size_t c = 0; c < incoming.size(); ++c) {
            const core::Vec3 p{incoming[c].x, incoming[c].y, incoming[c].z};
            const float d = core::length(p - interactors_[t].feet);
            if (d < kInteractorMatch) pairs.push_back({d, t, c});
        }
    }
    std::sort(pairs.begin(), pairs.end(), [](const Pair& a, const Pair& b) { return a.distance < b.distance; });
    for (const Pair& pair : pairs) {
        Interactor& it = interactors_[pair.tracked];
        if (it.seen || taken[pair.candidate]) continue;
        taken[pair.candidate] = true;
        it.seen = true;
        const core::Vec4& c = incoming[pair.candidate];
        it.feet = core::Vec3{c.x, c.y, c.z};
        it.radius = c.w;
    }
    for (std::size_t c = 0; c < incoming.size(); ++c) {
        if (taken[c]) continue;
        Interactor it;
        it.feet = it.trail = core::Vec3{incoming[c].x, incoming[c].y, incoming[c].z};
        it.radius = incoming[c].w;
        it.seen = true;
        interactors_.push_back(it);
    }
    // Fuerza y estela; los que se fueron se enderezan poco a poco.
    const float follow = dt > 0.0f ? 1.0f - std::exp(-dt / kInteractorTrail) : 0.0f;
    for (Interactor& it : interactors_) {
        it.strength = it.seen ? std::min(1.0f, it.strength + dt / kInteractorRise)
                              : std::max(0.0f, it.strength - dt / kInteractorFade);
        it.trail = it.trail + (it.feet - it.trail) * follow;
    }
    interactors_.erase(std::remove_if(interactors_.begin(), interactors_.end(),
                                      [](const Interactor& it) { return !it.seen && it.strength <= 0.0f; }),
                       interactors_.end());
    // Hasta 16: los mas cercanos a la camara (los que se ven).
    if (interactors_.size() > kMaxInteractors) {
        std::sort(interactors_.begin(), interactors_.end(), [&](const Interactor& a, const Interactor& b) {
            return core::length(a.feet - camera_position) < core::length(b.feet - camera_position);
        });
        interactors_.resize(kMaxInteractors);
    }

    // Lo de este frame y, como "anterior", exactamente lo que se subio en el
    // anterior (los vectores de movimiento del TAA).
    GpuFoliageInteractors gpu{};
    std::uint32_t count = 0;
    for (const Interactor& it : interactors_) {
        if (it.strength <= 0.0f) continue;
        gpu.feet[count] = core::Vec4{it.feet.x, it.feet.y, it.feet.z, it.radius};
        gpu.trail[count] = core::Vec4{it.trail.x, it.trail.y, it.trail.z, it.strength};
        ++count;
    }
    gpu.count[0] = count;
    if (interactor_gpu_.size() == 2 * kMaxInteractors) {
        gpu.count[1] = interactor_count_;
        for (std::uint32_t i = 0; i < interactor_count_; ++i) {
            gpu.previous_feet[i] = interactor_gpu_[i];
            gpu.previous_trail[i] = interactor_gpu_[kMaxInteractors + i];
        }
    }
    interactor_gpu_.assign(2 * kMaxInteractors, core::Vec4{});
    for (std::uint32_t i = 0; i < count; ++i) {
        interactor_gpu_[i] = gpu.feet[i];
        interactor_gpu_[kMaxInteractors + i] = gpu.trail[i];
    }
    interactor_count_ = count;
    // Caja de todo lo que pueden tocar (este frame y el anterior, con su
    // alcance y la ventana de altura de foliagePush): las hojas de los
    // arboles lejos de todos se la saltan sin recorrer la lista.
    if (gpu.count[0] + gpu.count[1] > 0) {
        core::Vec3 lo{1e30f, 1e30f, 1e30f};
        core::Vec3 hi{-1e30f, -1e30f, -1e30f};
        const auto grow = [&](const core::Vec4& v, float reach) {
            lo = core::Vec3{std::min(lo.x, v.x - reach), std::min(lo.y, v.y - 1.0f), std::min(lo.z, v.z - reach)};
            hi = core::Vec3{std::max(hi.x, v.x + reach), std::max(hi.y, v.y + 2.7f), std::max(hi.z, v.z + reach)};
        };
        for (std::uint32_t i = 0; i < gpu.count[0]; ++i) {
            grow(gpu.feet[i], gpu.feet[i].w + 0.5f);
            grow(gpu.trail[i], gpu.feet[i].w + 0.5f);
        }
        for (std::uint32_t i = 0; i < gpu.count[1]; ++i) {
            grow(gpu.previous_feet[i], gpu.previous_feet[i].w + 0.5f);
            grow(gpu.previous_trail[i], gpu.previous_feet[i].w + 0.5f);
        }
        gpu.box_min = core::Vec4{lo.x, lo.y, lo.z, 0.0f};
        gpu.box_max = core::Vec4{hi.x, hi.y, hi.z, 0.0f};
    }
    interactor_buffer_.write(&gpu, sizeof(gpu), interactorOffset(frame));
}

void VoxelPass::shiftInteractors(const core::Vec3& offset) {
    for (Interactor& it : interactors_) {
        it.feet = it.feet - offset;
        it.trail = it.trail - offset;
    }
    // El "anterior" tambien: si no, el salto del origen pareceria movimiento.
    for (core::Vec4& v : interactor_gpu_) {
        v.x -= offset.x;
        v.y -= offset.y;
        v.z -= offset.z;
    }
}

VoxelStats VoxelPass::stats() const {
    VoxelStats s;
    s.sections = static_cast<std::uint32_t>(sections_.size());
    s.visible = static_cast<std::uint32_t>(visible_keys_.size());
    for (const auto& [key, section] : sections_) {
        s.quads += section.quads;
        if (!section.distant) s.block_faces += section.faces;
        if (section.distant) {
            ++s.far_sections;
            s.far_quads += section.quads;
        } else if (!section.water && !section.sky) {
            ++s.enclosed_sections;
            s.enclosed_quads += section.quads;
        }
    }
    s.far_visible = far_visible_;
    s.drawn_quads = drawn_quads_;
    s.drawn_far_quads = drawn_far_quads_;
    s.draw_calls = draw_calls_;
    s.tessellated_quads = drawn_tess_quads_;
    for (const auto& page : pages_) s.memory_bytes += page->capacity;
    s.pending_uploads = static_cast<std::uint32_t>(upload_queue_.size());
    return s;
}

}  // namespace cramion::gfx
