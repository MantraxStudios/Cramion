#include "CramionFX/vk/VulkanCompat.h"
#include "CramionFX/vk/FoliagePass.h"

#include "CramionFX/asset/TreeGenerator.h"

#include "CramionFX/vk/GBuffer.h"
#include "CramionFX/vk/VulkanDevice.h"
#include "CramionFX/vk/VulkanShader.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>

namespace cramion::gfx {

namespace {

constexpr float kPi = 3.14159265f;

using FoliageVertex = asset::TreeVertex;  // 64 bytes (foliage.vert)

// VkDrawIndexedIndirectCommand (20 bytes), igual que en foliage_cull.comp.
struct DrawCommand {
    std::uint32_t index_count = 0;
    std::uint32_t instance_count = 0;
    std::uint32_t first_index = 0;
    std::int32_t vertex_offset = 0;
    std::uint32_t first_instance = 0;
};
static_assert(sizeof(DrawCommand) == 20);

struct CullPush {
    core::Mat4 view_projection;
    core::Vec4 camera;   // xyz, w distancia maxima
    core::Vec4 lod;      // x nivel 1, y nivel 2, z sombras, w proyectan (0/1)
    // x total de instancias, y distancia de las sombras con la malla de
    // detalle (bits de un float; 0 = todas con el nivel medio).
    std::uint32_t counts[4] = {0, 0, 0, 0};
    core::Vec4 offset;
};
static_assert(sizeof(CullPush) == 128, "el minimo que garantiza Vulkan");

// Vertices y fragmentos (foliage.vert / foliage.frag, mismo bloque).
struct DrawPush {
    core::Mat4 light_view_projection;
    core::Vec4 params;  // x segundos, y sombra, z viento, w segundos del ultimo frame
    core::Vec4 offset;  // xyz origen flotante, w alfa con ruido en las hojas (0/1)
    core::Vec4 sun;     // xyz hacia el sol, w distancia de las sombras
};
static_assert(sizeof(DrawPush) == 112, "FoliagePush de foliage.vert/.frag");

constexpr vk::ShaderStageFlags kDrawPushStages = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment;

// Hasta donde las sombras usan la malla de detalle (nivel 0) en Alto/Ultra:
// de cerca la sombra de las hojas del nivel medio (menos tarjetas y mas
// grandes) se ve a manchas.
constexpr float kNearShadowDistance = 40.0f;

// Juegos de sets de instancias que caben en el pool a la vez (el actual y los
// retirados esperando a los frames en vuelo).
constexpr std::uint32_t kSetGenerations = 6;

void memoryBarrier(const vk::raii::CommandBuffer& cmd, vk::PipelineStageFlags2 src_stage, vk::AccessFlags2 src_access,
                   vk::PipelineStageFlags2 dst_stage, vk::AccessFlags2 dst_access) {
    vk::MemoryBarrier2 barrier{};
    barrier.srcStageMask = src_stage;
    barrier.srcAccessMask = src_access;
    barrier.dstStageMask = dst_stage;
    barrier.dstAccessMask = dst_access;
    vk::DependencyInfo dependency{};
    dependency.setMemoryBarriers(barrier);
    compat::pipelineBarrier(cmd, dependency);
}

}  // namespace

FoliageInstance FoliageInstance::make(const core::Vec3& position, float yaw_radians, float scale, std::uint32_t species,
                                      float tint) {
    FoliageInstance f;
    f.x = position.x;
    f.y = position.y;
    f.z = position.z;
    float turn = std::fmod(yaw_radians / (2.0f * kPi), 1.0f);
    if (turn < 0.0f) turn += 1.0f;
    const auto yaw = static_cast<std::uint32_t>(turn * 1024.0f) & 1023u;
    const auto sc = static_cast<std::uint32_t>(std::clamp((scale - 0.25f) / 3.75f * 255.0f + 0.5f, 0.0f, 255.0f));
    const auto tn = static_cast<std::uint32_t>(std::clamp(tint, 0.0f, 1.0f) * 255.0f + 0.5f);
    f.packed = yaw | (sc << 10) | ((species & 3u) << 18) | (tn << 20);
    return f;
}

// -----------------------------------------------------------------------------
// Creacion
// -----------------------------------------------------------------------------

void FoliagePass::create(const VulkanDevice& device, const vk::raii::DescriptorSetLayout& frame_layout,
                         std::array<vk::Format, GBuffer::kColorAttachmentCount> gbuffer_formats, vk::Format depth_format, vk::Format shadow_format,
                         std::uint32_t frames_in_flight) {
    destroy();
    device_ = &device;
    frames_ = frames_in_flight;

    // Computo: instancias, visibles, comandos.
    {
        // 3: la esfera de cada especie (cambia al regenerar los arboles).
        const std::array<vk::DescriptorSetLayoutBinding, 4> bindings = {{
            {0, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute},
            {1, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute},
            {2, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute},
            {3, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute},
        }};
        vk::DescriptorSetLayoutCreateInfo info{};
        info.setBindings(bindings);
        cull_set_layout_ = vk::raii::DescriptorSetLayout(device.handle(), info);
        vk::PushConstantRange push{vk::ShaderStageFlagBits::eCompute, 0, sizeof(CullPush)};
        vk::PipelineLayoutCreateInfo layout_info{};
        layout_info.setSetLayouts(*cull_set_layout_);
        layout_info.setPushConstantRanges(push);
        cull_layout_ = vk::raii::PipelineLayout(device.handle(), layout_info);
        const vk::raii::ShaderModule module = shaders::loadModule(device, "foliage_cull.comp.spv");
        vk::ComputePipelineCreateInfo pipeline{};
        pipeline.stage = vk::PipelineShaderStageCreateInfo{{}, vk::ShaderStageFlagBits::eCompute, *module, "main"};
        pipeline.layout = *cull_layout_;
        cull_pipeline_ = vk::raii::Pipeline(device.handle(), device.pipelineCache(), pipeline);
    }
    // Dibujo: set 0 = el de la geometria (camara, lluvia, decals), set 1 = instancias y visibles.
    {
        // 2 y 3: texturas de los arboles (color con alfa recortado, normal).
        const std::array<vk::DescriptorSetLayoutBinding, 4> bindings = {{
            {0, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eVertex},
            {1, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eVertex},
            {2, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eFragment},
            {3, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eFragment},
        }};
        vk::DescriptorSetLayoutCreateInfo info{};
        info.setBindings(bindings);
        draw_set_layout_ = vk::raii::DescriptorSetLayout(device.handle(), info);
        const std::array<vk::DescriptorSetLayout, 2> layouts = {*frame_layout, *draw_set_layout_};
        vk::PushConstantRange push{kDrawPushStages, 0, sizeof(DrawPush)};
        vk::PipelineLayoutCreateInfo layout_info{};
        layout_info.setSetLayouts(layouts);
        layout_info.setPushConstantRanges(push);
        draw_layout_ = vk::raii::PipelineLayout(device.handle(), layout_info);
    }
    // Sitio para kSetGenerations juegos de sets: el actual y los retirados que
    // aun usan los frames en vuelo (cambiar las instancias no para la GPU).
    const std::array<vk::DescriptorPoolSize, 2> sizes = {{
        {vk::DescriptorType::eStorageBuffer, 6 * frames_in_flight * kSetGenerations},
        {vk::DescriptorType::eCombinedImageSampler, 2 * frames_in_flight * kSetGenerations},
    }};
    vk::DescriptorPoolCreateInfo pool_info{};
    pool_info.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    pool_info.maxSets = 2 * frames_in_flight * kSetGenerations;
    pool_info.setPoolSizes(sizes);
    pool_ = vk::raii::DescriptorPool(device.handle(), pool_info);

    createPipelines(device, gbuffer_formats, depth_format, shadow_format);
    // Las texturas de los arboles (11 capas de 1024 x 2, ~117 MB de VRAM y su
    // tiempo de generarlas) se hacen con los primeros arboles (setInstances):
    // antes ocupaban memoria en cualquier escena, sin un arbol.
    species_ = {asset::treePreset(asset::TreeKind::Pine), asset::treePreset(asset::TreeKind::Oak),
                asset::treePreset(asset::TreeKind::Birch)};
    bounds_.create(device, sizeof(core::Vec4) * kSpecies, vk::BufferUsageFlagBits::eStorageBuffer,
                   vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
    buildMeshes();
    readback_ready_.assign(frames_in_flight, false);
}

void FoliagePass::destroy() {
    // Una subida en segundo plano puede estar creando buffers: que termine.
    if (device_ != nullptr) device_->drainTasks();
    pending_.reset();
    retired_instances_.clear();
    cpu_instances_.reset();
    cull_sets_.clear();
    draw_sets_.clear();
    instances_.destroy();
    for (VulkanBuffer& b : visible_) b.destroy();
    for (VulkanBuffer& b : commands_) b.destroy();
    for (VulkanBuffer& b : readback_) b.destroy();
    visible_.clear();
    commands_.clear();
    readback_.clear();
    vertices_.destroy();
    indices_.destroy();
    bounds_.destroy();
    albedo_array_ = TextureArray{};
    normal_array_ = TextureArray{};
    albedo_layers_ = 0;
    texture_sampler_ = nullptr;
    gbuffer_pipeline_ = nullptr;
    gbuffer_wire_pipeline_ = nullptr;
    shadow_pipeline_ = nullptr;
    cull_pipeline_ = nullptr;
    pool_ = nullptr;
    draw_layout_ = nullptr;
    cull_layout_ = nullptr;
    draw_set_layout_ = nullptr;
    cull_set_layout_ = nullptr;
    instance_count_ = 0;
    stats_ = FoliageStats{};
    device_ = nullptr;
}

void FoliagePass::createPipelines(const VulkanDevice& device, std::array<vk::Format, GBuffer::kColorAttachmentCount> gbuffer_formats,
                                  vk::Format depth_format, vk::Format shadow_format) {
    const vk::raii::ShaderModule vertex = shaders::loadModule(device, "foliage.vert.spv");
    const vk::raii::ShaderModule fragment = shaders::loadModule(device, "foliage.frag.spv");

    const vk::VertexInputBindingDescription binding{0, sizeof(FoliageVertex), vk::VertexInputRate::eVertex};
    static_assert(sizeof(FoliageVertex) == 64);
    const std::array<vk::VertexInputAttributeDescription, 8> attributes = {{
        {0, 0, vk::Format::eR32G32B32Sfloat, 0},
        {1, 0, vk::Format::eR32G32B32Sfloat, 12},
        {2, 0, vk::Format::eR8G8B8A8Unorm, 24},
        {3, 0, vk::Format::eR32Sfloat, 28},
        {4, 0, vk::Format::eR32G32Sfloat, 32},     // uv
        {5, 0, vk::Format::eR32G32Sfloat, 40},     // capa de textura, temblor (0 = corteza)
        {6, 0, vk::Format::eR32G32B32Sfloat, 48},  // pivote de la rama principal
        {7, 0, vk::Format::eR8G8B8A8Unorm, 60},    // fase y flexibilidad de la rama, fase de la hoja, extra
    }};
    vk::PipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.setVertexBindingDescriptions(binding);
    vertex_input.setVertexAttributeDescriptions(attributes);
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
    vk::PipelineDepthStencilStateCreateInfo depth{};
    depth.depthTestEnable = VK_TRUE;
    depth.depthWriteEnable = VK_TRUE;
    depth.depthCompareOp = vk::CompareOp::eLessOrEqual;

    // --- G-buffer (las dos caras: la normal se da la vuelta en el fragmento) ---
    {
        const std::array<vk::PipelineShaderStageCreateInfo, 2> stages = {{
            {{}, vk::ShaderStageFlagBits::eVertex, *vertex, "main"},
            {{}, vk::ShaderStageFlagBits::eFragment, *fragment, "main"},
        }};
        vk::PipelineRasterizationStateCreateInfo raster{};
        raster.polygonMode = vk::PolygonMode::eFill;
        raster.cullMode = vk::CullModeFlagBits::eNone;
        raster.lineWidth = 1.0f;
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
        info.layout = *draw_layout_;
        gbuffer_pipeline_ = compat::makeGraphicsPipeline(device, info);
        if (device.fillModeNonSolidSupported()) {
            raster.polygonMode = vk::PolygonMode::eLine;
            gbuffer_wire_pipeline_ = compat::makeGraphicsPipeline(device, info);
            raster.polygonMode = vk::PolygonMode::eFill;
        }
    }
    // --- Sombras (profundidad, con el recorte de las hojas) ---
    {
        const vk::raii::ShaderModule shadow_fragment = shaders::loadModule(device, "foliage_shadow.frag.spv");
        const std::array<vk::PipelineShaderStageCreateInfo, 2> stage = {{
            {{}, vk::ShaderStageFlagBits::eVertex, *vertex, "main"},
            {{}, vk::ShaderStageFlagBits::eFragment, *shadow_fragment, "main"},
        }};
        vk::PipelineRasterizationStateCreateInfo raster{};
        raster.polygonMode = vk::PolygonMode::eFill;
        raster.cullMode = vk::CullModeFlagBits::eNone;
        raster.lineWidth = 1.0f;
        raster.depthClampEnable = device.depthClampSupported() ? VK_TRUE : VK_FALSE;
        raster.depthBiasEnable = VK_TRUE;
        // Bias menor: con 1.0 / 1.75 la sombra se despegaba de las ramas y las
        // hojas (copas que flotaban sobre su sombra). Vigilar el acne en la corteza.
        raster.depthBiasConstantFactor = 0.5f;
        raster.depthBiasSlopeFactor = 1.0f;
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
        info.layout = *draw_layout_;
        shadow_pipeline_ = compat::makeGraphicsPipeline(device, info);
    }
}

void FoliagePass::buildMeshes() {
    std::vector<FoliageVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::array<core::Vec4, kSpecies> bounds{};
    for (std::uint32_t s = 0; s < kSpecies; ++s) {
        for (std::uint32_t l = 0; l < 3; ++l) {
            const asset::TreeMeshData m = asset::buildTree(species_[s], static_cast<int>(l));
            Mesh& mesh = meshes_[s * 3 + l];
            mesh.first_index = static_cast<std::uint32_t>(indices.size());
            mesh.index_count = static_cast<std::uint32_t>(m.indices.size());
            mesh.vertex_offset = static_cast<std::int32_t>(vertices.size());
            triangles_[s * 3 + l] = mesh.index_count / 3;
            vertices.insert(vertices.end(), m.vertices.begin(), m.vertices.end());
            indices.insert(indices.end(), m.indices.begin(), m.indices.end());
            // La esfera del nivel 0 (el mas grande) para el recorte.
            if (l == 0) bounds[s] = core::Vec4{m.center_y, m.radius * 1.05f, m.height, 0.0f};
            if (l == 1) rt_meshes_[s] = m;
        }
    }
    ++species_revision_;
    vertices_.destroy();
    indices_.destroy();
    vertices_ = VulkanBuffer::createDeviceLocal(*device_, vertices.data(), vertices.size() * sizeof(FoliageVertex),
                                                vk::BufferUsageFlagBits::eVertexBuffer);
    indices_ = VulkanBuffer::createDeviceLocal(*device_, indices.data(), indices.size() * sizeof(std::uint32_t),
                                               vk::BufferUsageFlagBits::eIndexBuffer);
    bounds_.write(bounds.data(), sizeof(bounds));
}

void FoliagePass::setSpecies(const std::array<asset::TreeSpecies, kSpecies>& species) {
    if (device_ == nullptr) return;
    bool same = true;
    for (std::uint32_t s = 0; s < kSpecies; ++s) same = same && species[s] == species_[s];
    if (same) return;
    device_->waitIdle();
    species_ = species;
    buildMeshes();
}

void FoliagePass::createTextures() {
    const asset::TreeTextures t = asset::generateTreeTextures(1024);
    const auto upload = [&](TextureArray& texture, const std::vector<std::vector<std::vector<std::uint8_t>>>& layers,
                            vk::Format format) {
        const VulkanDevice& device = *device_;
        const auto count = static_cast<std::uint32_t>(layers.size());
        const std::uint32_t mips = t.mips;
        vk::ImageCreateInfo info{};
        info.imageType = vk::ImageType::e2D;
        info.format = format;
        info.extent = vk::Extent3D{t.size, t.size, 1};
        info.mipLevels = mips;
        info.arrayLayers = count;
        info.samples = vk::SampleCountFlagBits::e1;
        info.tiling = vk::ImageTiling::eOptimal;
        info.usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst;
        info.initialLayout = vk::ImageLayout::eUndefined;
        // El color (SRGB) tambien se ve como UNORM desde los rayos.
        if (format == vk::Format::eR8G8B8A8Srgb) info.flags = vk::ImageCreateFlagBits::eMutableFormat;
        texture.image = vk::raii::Image(device.handle(), info);
        const vk::MemoryRequirements requirements = texture.image.getMemoryRequirements();
        vk::MemoryAllocateInfo allocate{};
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = device.findMemoryType(requirements.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal);
        texture.memory = vk::raii::DeviceMemory(device.handle(), allocate);
        texture.image.bindMemory(*texture.memory, 0);

        vk::DeviceSize total = 0;
        for (const auto& layer : layers) {
            for (const auto& level : layer) total += level.size();
        }
        VulkanBuffer staging;
        staging.create(device, total, vk::BufferUsageFlagBits::eTransferSrc,
                       vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
        std::vector<vk::BufferImageCopy> regions;
        vk::DeviceSize offset = 0;
        for (std::uint32_t l = 0; l < count; ++l) {
            std::uint32_t size = t.size;
            for (std::uint32_t m = 0; m < mips && m < layers[l].size(); ++m) {
                staging.write(layers[l][m].data(), layers[l][m].size(), offset);
                vk::BufferImageCopy region{};
                region.bufferOffset = offset;
                region.imageSubresource = vk::ImageSubresourceLayers{vk::ImageAspectFlagBits::eColor, m, l, 1};
                region.imageExtent = vk::Extent3D{size, size, 1};
                regions.push_back(region);
                offset += layers[l][m].size();
                size = std::max(size / 2, 1U);
            }
        }
        const vk::Image image = *texture.image;
        device.submitOneTime([&](const vk::raii::CommandBuffer& cmd) {
            vk::ImageMemoryBarrier2 to_copy{};
            to_copy.srcStageMask = vk::PipelineStageFlagBits2::eTopOfPipe;
            to_copy.dstStageMask = vk::PipelineStageFlagBits2::eTransfer;
            to_copy.dstAccessMask = vk::AccessFlagBits2::eTransferWrite;
            to_copy.oldLayout = vk::ImageLayout::eUndefined;
            to_copy.newLayout = vk::ImageLayout::eTransferDstOptimal;
            to_copy.image = image;
            to_copy.subresourceRange = vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, mips, 0, count};
            compat::pipelineBarrier(cmd, vk::DependencyInfo{}.setImageMemoryBarriers(to_copy));
            cmd.copyBufferToImage(*staging.handle(), image, vk::ImageLayout::eTransferDstOptimal, regions);
            vk::ImageMemoryBarrier2 to_read = to_copy;
            to_read.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
            to_read.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
            to_read.dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader;
            to_read.dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead;
            to_read.oldLayout = vk::ImageLayout::eTransferDstOptimal;
            to_read.newLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
            compat::pipelineBarrier(cmd, vk::DependencyInfo{}.setImageMemoryBarriers(to_read));
        });
        vk::ImageViewCreateInfo view{};
        view.image = image;
        view.viewType = vk::ImageViewType::e2DArray;
        view.format = format;
        view.subresourceRange = vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, mips, 0, count};
        texture.view = vk::raii::ImageView(device.handle(), view);
    };
    upload(albedo_array_, t.albedo, vk::Format::eR8G8B8A8Srgb);
    albedo_mips_ = t.mips;
    albedo_layers_ = static_cast<std::uint32_t>(t.albedo.size());
    upload(normal_array_, t.normal, vk::Format::eR8G8B8A8Unorm);

    createTextureSampler();
}

// Repetido, trilineal y con el filtrado de la configuracion grafica.
void FoliagePass::createTextureSampler() {
    vk::SamplerCreateInfo sampler{};
    sampler.magFilter = vk::Filter::eLinear;
    sampler.minFilter = vk::Filter::eLinear;
    sampler.mipmapMode = vk::SamplerMipmapMode::eLinear;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = vk::SamplerAddressMode::eRepeat;
    sampler.maxLod = VK_LOD_CLAMP_NONE;
    applyTextureFiltering(sampler, device_->physicalDevice(), filtering_);
    texture_sampler_ = vk::raii::Sampler(device_->handle(), sampler);
}

void FoliagePass::setTextureFiltering(const TextureFiltering& filtering) {
    if (filtering == filtering_) return;
    filtering_ = filtering;
    if (device_ == nullptr || !*texture_sampler_) return;
    createTextureSampler();
    // Solo las dos texturas de los sets de dibujo (los buffers no cambian).
    for (std::size_t f = 0; f < draw_sets_.size(); ++f) {
        const vk::DescriptorImageInfo albedo{*texture_sampler_, *albedo_array_.view, vk::ImageLayout::eShaderReadOnlyOptimal};
        const vk::DescriptorImageInfo normal{*texture_sampler_, *normal_array_.view, vk::ImageLayout::eShaderReadOnlyOptimal};
        std::array<vk::WriteDescriptorSet, 2> writes{};
        writes[0].dstSet = *draw_sets_[f];
        writes[0].dstBinding = 2;
        writes[0].descriptorType = vk::DescriptorType::eCombinedImageSampler;
        writes[0].setImageInfo(albedo);
        writes[1].dstSet = *draw_sets_[f];
        writes[1].dstBinding = 3;
        writes[1].descriptorType = vk::DescriptorType::eCombinedImageSampler;
        writes[1].setImageInfo(normal);
        device_->handle().updateDescriptorSets(writes, nullptr);
    }
}

// -----------------------------------------------------------------------------
// Instancias
// -----------------------------------------------------------------------------

const std::vector<FoliageInstance>& FoliagePass::cpuInstances() const {
    static const std::vector<FoliageInstance> kEmpty;
    return cpu_instances_ ? *cpu_instances_ : kEmpty;
}

std::shared_ptr<FoliagePass::InstanceBuffers> FoliagePass::buildInstances(
    const VulkanDevice& device, std::uint32_t frames, const std::vector<FoliageInstance>& input,
    std::shared_ptr<const std::vector<FoliageInstance>> shared, bool keep_cpu, bool async,
    std::shared_ptr<GpuTicket>& ticket) {
    auto out = std::make_shared<InstanceBuffers>();
    const std::size_t count = std::min<std::size_t>(input.size(), kMaxInstances);
    out->count = static_cast<std::uint32_t>(count);
    if (keep_cpu) {
        // Los rayos leen las mismas (sin copiar si son todas).
        out->cpu = shared && count == input.size()
                       ? std::move(shared)
                       : std::make_shared<const std::vector<FoliageInstance>>(
                             input.begin(), input.begin() + static_cast<std::ptrdiff_t>(count));
    }
    if (count == 0) return out;
    for (std::size_t i = 0; i < count; ++i) {
        ++out->species_count[std::min<std::uint32_t>((input[i].packed >> 18) & 3u, kSpecies - 1)];
    }
    const vk::DeviceSize instance_bytes = count * sizeof(FoliageInstance);
    if (async) {
        auto staging = std::make_shared<VulkanBuffer>();
        staging->create(device, instance_bytes, vk::BufferUsageFlagBits::eTransferSrc,
                        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
        staging->write(input.data(), instance_bytes);
        out->instances.create(device, instance_bytes,
                              vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst,
                              vk::MemoryPropertyFlagBits::eDeviceLocal);
        const vk::Buffer source = *staging->handle();
        const vk::Buffer target = *out->instances.handle();
        // El destino (todo `out`) y el staging viven hasta que la GPU termina.
        ticket = device.submitAsync(
            [&](const vk::raii::CommandBuffer& cmd) {
                vk::BufferCopy region{};
                region.size = instance_bytes;
                cmd.copyBuffer(source, target, region);
                memoryBarrier(cmd, vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite,
                              vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eVertexShader,
                              vk::AccessFlagBits2::eShaderStorageRead);
            },
            {staging, out});
    } else {
        out->instances = VulkanBuffer::createDeviceLocal(device, input.data(), instance_bytes,
                                                         vk::BufferUsageFlagBits::eStorageBuffer);
    }
    // Cada lista cabe todas las de su especie: kLists listas por especie.
    out->visible_bytes = static_cast<vk::DeviceSize>(count) * kLists * sizeof(std::uint32_t);
    std::uint32_t offset = 0;
    for (std::uint32_t s = 0; s < kSpecies; ++s) {
        for (std::uint32_t l = 0; l < kLists; ++l) {
            out->template_first_instance[s * kLists + l] = offset * kLists + l * out->species_count[s];
        }
        offset += out->species_count[s];
    }
    for (std::uint32_t f = 0; f < frames; ++f) {
        VulkanBuffer visible;
        visible.create(device, out->visible_bytes, vk::BufferUsageFlagBits::eStorageBuffer,
                       vk::MemoryPropertyFlagBits::eDeviceLocal);
        out->visible.push_back(std::move(visible));
        VulkanBuffer commands;
        commands.create(device, sizeof(DrawCommand) * kSpecies * kLists,
                        vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eIndirectBuffer |
                            vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eTransferSrc,
                        vk::MemoryPropertyFlagBits::eDeviceLocal);
        out->commands.push_back(std::move(commands));
        VulkanBuffer readback;
        readback.create(device, sizeof(DrawCommand) * kSpecies * kLists, vk::BufferUsageFlagBits::eTransferDst,
                        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
        out->readback.push_back(std::move(readback));
    }
    return out;
}

bool FoliagePass::adoptInstances(const std::shared_ptr<InstanceBuffers>& buffers) {
    InstanceBuffers& b = *buffers;
    std::vector<vk::raii::DescriptorSet> cull_sets;
    std::vector<vk::raii::DescriptorSet> draw_sets;
    if (b.count > 0) {
        if (albedo_layers_ == 0) {
            // Las texturas de los arboles, con los primeros (una vez).
            createTextures();
            ++species_revision_;  // la escena de rayos las incluye al rehacerse
        }
        if (!allocateSets(cull_sets, draw_sets)) return false;
    }
    // Lo de antes lo pueden estar usando los frames en vuelo: se retira.
    if (instances_.isValid() || !cull_sets_.empty() || !draw_sets_.empty()) {
        RetiredInstances old;
        old.instances = std::move(instances_);
        old.visible = std::move(visible_);
        old.commands = std::move(commands_);
        old.readback = std::move(readback_);
        old.cull_sets = std::move(cull_sets_);
        old.draw_sets = std::move(draw_sets_);
        old.frames = frames_ + 1;
        retired_instances_.push_back(std::move(old));
    }
    instances_ = std::move(b.instances);
    visible_ = std::move(b.visible);
    commands_ = std::move(b.commands);
    readback_ = std::move(b.readback);
    cull_sets_ = std::move(cull_sets);
    draw_sets_ = std::move(draw_sets);
    instance_count_ = b.count;
    species_count_ = b.species_count;
    template_first_instance_ = b.template_first_instance;
    readback_ready_.assign(frames_, false);
    stats_ = FoliageStats{};
    stats_.instances = b.count;
    stats_.memory_bytes = static_cast<std::uint64_t>(b.count) * sizeof(FoliageInstance) + b.visible_bytes * frames_;
    cpu_instances_ = keep_cpu_copy_ ? std::move(b.cpu) : nullptr;
    ++instances_revision_;
    if (instance_count_ > 0) writeSets();
    return true;
}

void FoliagePass::setInstances(const std::vector<FoliageInstance>& input_all) {
    if (device_ == nullptr) return;
    // Lo que se estuviera subiendo en segundo plano queda viejo.
    pending_.reset();
    // GPU sin firstInstance en los comandos indirectos (algunos moviles): las
    // listas de cada especie empiezan en otro sitio del buffer y no se pueden
    // dibujar asi. Sin arboles instanciados (mejor que un render roto).
    static const std::vector<FoliageInstance> kNone;
    const std::vector<FoliageInstance>& input = device_->drawIndirectFirstInstanceSupported() ? input_all : kNone;
    // Sin waitIdle: buffers nuevos (su copia si se espera, submitOneTime) y
    // los de antes se retiran unos frames.
    std::shared_ptr<GpuTicket> ticket;
    std::shared_ptr<InstanceBuffers> buffers =
        buildInstances(*device_, frames_, input, nullptr, keep_cpu_copy_, false, ticket);
    if (!adoptInstances(buffers)) {
        // Pool lleno de juegos retirados (muchos cambios seguidos): solo
        // entonces se espera a la GPU para soltarlos.
        device_->waitIdle();
        retired_instances_.clear();
        adoptInstances(buffers);
    }
}

void FoliagePass::setInstancesAsync(std::shared_ptr<const std::vector<FoliageInstance>> instances) {
    if (device_ == nullptr) return;
    if (!instances) instances = std::make_shared<const std::vector<FoliageInstance>>();
    if (!device_->drawIndirectFirstInstanceSupported()) instances = std::make_shared<const std::vector<FoliageInstance>>();
    // La nueva sustituye a la que se estuviera subiendo (su resultado se
    // descarta; sus buffers viven en su ticket hasta que la GPU termina).
    auto pending = std::make_shared<PendingInstances>();
    pending_ = pending;
    const VulkanDevice* device = device_;
    const std::uint32_t frames = frames_;
    const bool keep_cpu = keep_cpu_copy_;
    device_->runTask([device, frames, keep_cpu, pending, instances = std::move(instances)]() {
        try {
            std::shared_ptr<GpuTicket> ticket;
            std::shared_ptr<InstanceBuffers> buffers =
                buildInstances(*device, frames, *instances, instances, keep_cpu, true, ticket);
            pending->buffers = std::move(buffers);
            pending->ticket = std::move(ticket);
            pending->state.store(1, std::memory_order_release);
        } catch (const std::exception& e) {
            std::cerr << "[Vegetacion] No se pudieron subir las instancias: " << e.what() << "\n";
            pending->state.store(2, std::memory_order_release);
        }
    });
}

bool FoliagePass::pollAsync() {
    for (RetiredInstances& r : retired_instances_) {
        if (r.frames > 0) --r.frames;
    }
    std::erase_if(retired_instances_, [](const RetiredInstances& r) { return r.frames == 0; });
    if (!pending_) return false;
    const int state = pending_->state.load(std::memory_order_acquire);
    if (state == 0) return false;
    if (state == 2 || !pending_->buffers) {
        pending_.reset();
        return false;
    }
    if (pending_->ticket && !pending_->ticket->done()) return false;
    if (!adoptInstances(pending_->buffers)) return false;  // sin sets ahora: el frame siguiente
    pending_.reset();
    return true;
}

bool FoliagePass::allocateSets(std::vector<vk::raii::DescriptorSet>& cull_sets,
                               std::vector<vk::raii::DescriptorSet>& draw_sets) {
    const vk::raii::Device& device = device_->handle();
    std::vector<vk::DescriptorSetLayout> cull_layouts(frames_, *cull_set_layout_);
    std::vector<vk::DescriptorSetLayout> draw_layouts(frames_, *draw_set_layout_);
    try {
        vk::DescriptorSetAllocateInfo cull_info{};
        cull_info.descriptorPool = *pool_;
        cull_info.setSetLayouts(cull_layouts);
        cull_sets = vk::raii::DescriptorSets(device, cull_info);
        vk::DescriptorSetAllocateInfo draw_info{};
        draw_info.descriptorPool = *pool_;
        draw_info.setSetLayouts(draw_layouts);
        draw_sets = vk::raii::DescriptorSets(device, draw_info);
    } catch (const std::exception&) {
        cull_sets.clear();
        draw_sets.clear();
        return false;
    }
    return true;
}

void FoliagePass::writeSets() {
    const vk::raii::Device& device = device_->handle();
    for (std::uint32_t f = 0; f < frames_ && f < cull_sets_.size() && f < draw_sets_.size(); ++f) {
        const vk::DescriptorBufferInfo instances{*instances_.handle(), 0, VK_WHOLE_SIZE};
        const vk::DescriptorBufferInfo visible{*visible_[f].handle(), 0, VK_WHOLE_SIZE};
        const vk::DescriptorBufferInfo commands{*commands_[f].handle(), 0, VK_WHOLE_SIZE};
        const vk::DescriptorBufferInfo bounds{*bounds_.handle(), 0, VK_WHOLE_SIZE};
        const vk::DescriptorImageInfo albedo{*texture_sampler_, *albedo_array_.view, vk::ImageLayout::eShaderReadOnlyOptimal};
        const vk::DescriptorImageInfo normal{*texture_sampler_, *normal_array_.view, vk::ImageLayout::eShaderReadOnlyOptimal};
        std::array<vk::WriteDescriptorSet, 8> writes{};
        const auto write = [&](vk::WriteDescriptorSet& w, const vk::raii::DescriptorSet& set, std::uint32_t binding,
                               const vk::DescriptorBufferInfo& info) {
            w.dstSet = *set;
            w.dstBinding = binding;
            w.descriptorType = vk::DescriptorType::eStorageBuffer;
            w.setBufferInfo(info);
        };
        write(writes[0], cull_sets_[f], 0, instances);
        write(writes[1], cull_sets_[f], 1, visible);
        write(writes[2], cull_sets_[f], 2, commands);
        write(writes[3], draw_sets_[f], 0, instances);
        write(writes[4], draw_sets_[f], 1, visible);
        write(writes[5], cull_sets_[f], 3, bounds);
        writes[6].dstSet = *draw_sets_[f];
        writes[6].dstBinding = 2;
        writes[6].descriptorType = vk::DescriptorType::eCombinedImageSampler;
        writes[6].setImageInfo(albedo);
        writes[7].dstSet = *draw_sets_[f];
        writes[7].dstBinding = 3;
        writes[7].descriptorType = vk::DescriptorType::eCombinedImageSampler;
        writes[7].setImageInfo(normal);
        device.updateDescriptorSets(writes, nullptr);
    }
}

// -----------------------------------------------------------------------------
// Por frame
// -----------------------------------------------------------------------------

void FoliagePass::setSun(const core::Vec3& to_sun) {
    const float len = core::length(to_sun);
    to_sun_ = len > 1e-6f ? to_sun * (1.0f / len) : core::Vec3{0.0f, 1.0f, 0.0f};
}

void FoliagePass::recordCull(const vk::raii::CommandBuffer& cmd, std::uint32_t frame, const core::Vec3& camera_position,
                             const core::Mat4& view_projection, float delta_seconds) {
    using Stage = vk::PipelineStageFlagBits2;
    using Access = vk::AccessFlagBits2;
    last_delta_ = std::clamp(delta_seconds, 0.0f, 0.25f);
    time_ += last_delta_;
    if (instance_count_ == 0 || frame >= commands_.size()) return;

    // Lo que conto la GPU la ultima vez que uso este frame (ya termino).
    if (readback_ready_[frame]) {
        const auto* done = static_cast<const DrawCommand*>(readback_[frame].mapped());
        stats_.visible[0] = stats_.visible[1] = stats_.visible[2] = 0;
        stats_.shadow_casters = 0;
        stats_.triangles = 0;
        for (std::uint32_t s = 0; s < kSpecies; ++s) {
            for (std::uint32_t l = 0; l < 3; ++l) {
                const std::uint32_t n = done[s * kLists + l].instance_count;
                stats_.visible[l] += n;
                stats_.triangles += static_cast<std::uint64_t>(n) * triangles_[s * 3 + l];
            }
            stats_.shadow_casters += done[s * kLists + 3].instance_count + done[s * kLists + 4].instance_count;
        }
    }

    // Comandos con 0 instancias (el computo las cuenta).
    std::array<DrawCommand, kSpecies * kLists> commands{};
    for (std::uint32_t s = 0; s < kSpecies; ++s) {
        for (std::uint32_t l = 0; l < kLists; ++l) {
            // Listas 0-2: su nivel; 3: sombras (nivel medio); 4: sombras cercanas (nivel 0).
            const Mesh& mesh = meshes_[s * 3 + (l < 3 ? l : (l == 3 ? 1 : 0))];
            DrawCommand& c = commands[s * kLists + l];
            c.index_count = mesh.index_count;
            c.first_index = mesh.first_index;
            c.vertex_offset = mesh.vertex_offset;
            c.first_instance = template_first_instance_[s * kLists + l];
        }
    }
    memoryBarrier(cmd, Stage::eDrawIndirect | Stage::eVertexShader | Stage::eTransfer,
                  Access::eIndirectCommandRead | Access::eShaderRead | Access::eTransferRead, Stage::eTransfer,
                  Access::eTransferWrite);
    cmd.updateBuffer<DrawCommand>(*commands_[frame].handle(), 0, commands);
    memoryBarrier(cmd, Stage::eTransfer, Access::eTransferWrite, Stage::eComputeShader, Access::eShaderRead | Access::eShaderWrite);

    CullPush push{};
    push.view_projection = view_projection;
    push.camera = core::Vec4{camera_position.x, camera_position.y, camera_position.z, settings_.max_distance};
    const float lod1 = settings_.lod1_distance * lod_scale_;
    push.lod = core::Vec4{lod1, std::max(settings_.lod2_distance * lod_scale_, lod1), settings_.shadow_distance,
                          settings_.cast_shadows ? 1.0f : 0.0f};
    push.counts[0] = instance_count_;
    // Sombras cercanas con la malla de detalle solo en Alto/Ultra (cuesta
    // los triangulos del nivel 0 en el mapa de sombras).
    const bool near_shadows = tier_ == HardwareTier::High || tier_ == HardwareTier::Ultra;
    const float near_shadow = near_shadows ? std::min(kNearShadowDistance, settings_.shadow_distance) : 0.0f;
    std::memcpy(&push.counts[1], &near_shadow, sizeof(float));
    push.offset = core::Vec4{origin_offset_.x, origin_offset_.y, origin_offset_.z, 0.0f};
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *cull_pipeline_);
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *cull_layout_, 0, *cull_sets_[frame], nullptr);
    cmd.pushConstants<CullPush>(*cull_layout_, vk::ShaderStageFlagBits::eCompute, 0, push);
    const std::uint32_t group = compat::lite() ? 128u : 256u;  // foliage_cull.comp
    cmd.dispatch((instance_count_ + group - 1) / group, 1, 1);

    memoryBarrier(cmd, Stage::eComputeShader, Access::eShaderWrite, Stage::eDrawIndirect | Stage::eVertexShader | Stage::eTransfer,
                  Access::eIndirectCommandRead | Access::eShaderRead | Access::eTransferRead);
    // Copia para las estadisticas (se lee la proxima vez que toque este frame).
    const vk::BufferCopy region{0, 0, sizeof(DrawCommand) * kSpecies * kLists};
    cmd.copyBuffer(*commands_[frame].handle(), *readback_[frame].handle(), region);
    memoryBarrier(cmd, Stage::eTransfer, Access::eTransferWrite, Stage::eHost, Access::eHostRead);
    readback_ready_[frame] = true;
}

void FoliagePass::draw(const vk::raii::CommandBuffer& cmd, std::uint32_t frame, bool shadow) const {
    cmd.bindVertexBuffers(0, *vertices_.handle(), {0});
    cmd.bindIndexBuffer(*indices_.handle(), 0, vk::IndexType::eUint32);
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *draw_layout_, 1, *draw_sets_[frame], nullptr);
    for (std::uint32_t s = 0; s < kSpecies; ++s) {
        if (species_count_[s] == 0) continue;
        if (shadow) {
            // Nivel medio y, de cerca (Alto/Ultra; si no, con 0 instancias), el de detalle.
            cmd.drawIndexedIndirect(*commands_[frame].handle(), (s * kLists + 3) * sizeof(DrawCommand), 1, sizeof(DrawCommand));
            cmd.drawIndexedIndirect(*commands_[frame].handle(), (s * kLists + 4) * sizeof(DrawCommand), 1, sizeof(DrawCommand));
        } else {
            // Tres niveles de detalle, uno por llamada (sin multiDrawIndirect en muchos moviles).
            for (std::uint32_t level = 0; level < 3; ++level) {
                cmd.drawIndexedIndirect(*commands_[frame].handle(), (s * kLists + level) * sizeof(DrawCommand), 1,
                                        sizeof(DrawCommand));
            }
        }
    }
}

void FoliagePass::recordGBuffer(const vk::raii::CommandBuffer& cmd, std::uint32_t frame,
                                const vk::raii::DescriptorSet& frame_set) const {
    if (instance_count_ == 0 || frame >= draw_sets_.size()) return;
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics,
                     wireframe_ && *gbuffer_wire_pipeline_ ? *gbuffer_wire_pipeline_ : *gbuffer_pipeline_);
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *draw_layout_, 0, *frame_set, nullptr);
    DrawPush push{};
    push.params = core::Vec4{time_, 0.0f, settings_.wind, last_delta_};
    // Alfa con ruido en el borde de las hojas (el TAA lo funde): no en Bajo.
    const float hashed_alpha = tier_ == HardwareTier::Low ? 0.0f : 1.0f;
    push.offset = core::Vec4{origin_offset_.x, origin_offset_.y, origin_offset_.z, hashed_alpha};
    push.sun = core::Vec4{to_sun_.x, to_sun_.y, to_sun_.z, settings_.cast_shadows ? settings_.shadow_distance : 0.0f};
    cmd.pushConstants<DrawPush>(*draw_layout_, kDrawPushStages, 0, push);
    draw(cmd, frame, false);
}

void FoliagePass::recordShadow(const vk::raii::CommandBuffer& cmd, std::uint32_t frame,
                               const vk::raii::DescriptorSet& frame_set, const core::Mat4& light_view_projection) const {
    if (instance_count_ == 0 || !settings_.cast_shadows || frame >= draw_sets_.size()) return;
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *shadow_pipeline_);
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *draw_layout_, 0, *frame_set, nullptr);
    DrawPush push{};
    push.light_view_projection = light_view_projection;
    push.params = core::Vec4{time_, 1.0f, settings_.wind, 0.0f};
    push.offset = core::Vec4{origin_offset_.x, origin_offset_.y, origin_offset_.z, 0.0f};
    push.sun = core::Vec4{to_sun_.x, to_sun_.y, to_sun_.z, settings_.shadow_distance};
    cmd.pushConstants<DrawPush>(*draw_layout_, kDrawPushStages, 0, push);
    draw(cmd, frame, true);
}

}  // namespace cramion::gfx
