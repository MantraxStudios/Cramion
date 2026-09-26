#include "CramionFX/vk/VoxelPass.h"

#include "CramionFX/core/Frustum.h"
#include "CramionFX/vk/GBuffer.h"
#include "CramionFX/vk/VulkanDevice.h"
#include "CramionFX/vk/VulkanShader.h"

#include <algorithm>
#include <bit>
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
    std::uint32_t pad[3] = {0, 0, 0};
};

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
    cmd.pipelineBarrier2(dependency);
}

}  // namespace

// -----------------------------------------------------------------------------
// Creacion
// -----------------------------------------------------------------------------

void VoxelPass::create(const VulkanDevice& device, const vk::raii::DescriptorSetLayout& frame_layout,
                       std::array<vk::Format, 4> gbuffer_formats, vk::Format depth_format, vk::Format shadow_format,
                       std::uint32_t frames_in_flight) {
    destroy();
    device_ = &device;
    frames_ = frames_in_flight;

    const std::array<vk::DescriptorSetLayoutBinding, 3> bindings = {{
        {0, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eFragment},
        {1, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eFragment},
        {2, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eFragment},
    }};
    vk::DescriptorSetLayoutCreateInfo set_info{};
    set_info.setBindings(bindings);
    set_layout_ = vk::raii::DescriptorSetLayout(device.handle(), set_info);

    const std::array<vk::DescriptorSetLayout, 2> set_layouts = {*frame_layout, *set_layout_};
    vk::PushConstantRange push{};
    push.stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment;
    push.size = sizeof(GpuVoxelPush);
    vk::PipelineLayoutCreateInfo layout_info{};
    layout_info.setSetLayouts(set_layouts);
    layout_info.setPushConstantRanges(push);
    layout_ = vk::raii::PipelineLayout(device.handle(), layout_info);

    const vk::DescriptorPoolSize size{vk::DescriptorType::eCombinedImageSampler, 3};
    vk::DescriptorPoolCreateInfo pool_info{};
    pool_info.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    pool_info.maxSets = 1;
    pool_info.setPoolSizes(size);
    pool_ = vk::raii::DescriptorPool(device.handle(), pool_info);

    vk::SamplerCreateInfo sampler{};
    sampler.magFilter = vk::Filter::eLinear;
    sampler.minFilter = vk::Filter::eLinear;
    sampler.mipmapMode = vk::SamplerMipmapMode::eLinear;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = vk::SamplerAddressMode::eRepeat;
    sampler.maxLod = VK_LOD_CLAMP_NONE;
    if (device.physicalDevice().getFeatures().samplerAnisotropy) {
        sampler.anisotropyEnable = VK_TRUE;
        sampler.maxAnisotropy = std::min(16.0f, device.physicalDevice().getProperties().limits.maxSamplerAnisotropy);
    }
    sampler_ = vk::raii::Sampler(device.handle(), sampler);

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
}

void VoxelPass::destroy() {
    sections_.clear();
    upload_queue_.clear();
    retired_.clear();
    pages_.clear();
    for (VulkanBuffer& b : staging_) b.destroy();
    staging_.clear();
    quad_indices_.destroy();
    albedo_ = ArrayTexture{};
    normal_ = ArrayTexture{};
    material_ = ArrayTexture{};
    layer_count_ = 0;
    set_ = nullptr;
    shadow_pipeline_ = nullptr;
    gbuffer_pipeline_ = nullptr;
    sampler_ = nullptr;
    pool_ = nullptr;
    layout_ = nullptr;
    set_layout_ = nullptr;
    visible_keys_.clear();
    all_keys_.clear();
    device_ = nullptr;
}

void VoxelPass::createPipelines(const VulkanDevice& device, std::array<vk::Format, 4> gbuffer_formats,
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
        gbuffer_pipeline_ = vk::raii::Pipeline(device.handle(), device.pipelineCache(), info);
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
        shadow_pipeline_ = vk::raii::Pipeline(device.handle(), device.pipelineCache(), info);
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
    const vk::DescriptorImageInfo a{*sampler_, *albedo_.view, vk::ImageLayout::eShaderReadOnlyOptimal};
    const vk::DescriptorImageInfo n{*sampler_, *normal_.view, vk::ImageLayout::eShaderReadOnlyOptimal};
    const vk::DescriptorImageInfo m{*sampler_, *material_.view, vk::ImageLayout::eShaderReadOnlyOptimal};
    const std::array<vk::WriteDescriptorSet, 3> writes = {{
        {*set_, 0, 0, 1, vk::DescriptorType::eCombinedImageSampler, &a},
        {*set_, 1, 0, 1, vk::DescriptorType::eCombinedImageSampler, &n},
        {*set_, 2, 0, 1, vk::DescriptorType::eCombinedImageSampler, &m},
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
                           std::uint32_t vertex_count) {
    if (device_ == nullptr) return;
    const std::uint32_t quads = std::min(vertex_count / 4, kMaxQuadsPerSection);
    if (quads == 0 || vertices == nullptr) {
        removeSection(key);
        return;
    }
    Section& section = sections_[key];
    section.origin = origin;
    section.pending_data.assign(vertices, vertices + static_cast<std::size_t>(quads) * 8);
    section.pending_quads = quads;
    if (!section.queued) {
        section.queued = true;
        upload_queue_.push_back(key);
    }
}

void VoxelPass::removeSection(std::uint64_t key) {
    const auto it = sections_.find(key);
    if (it == sections_.end()) return;
    retire(it->second.live);
    retire(it->second.pending);
    sections_.erase(it);  // su entrada en la cola se salta al subir
}

void VoxelPass::clearSections() {
    for (auto& [key, section] : sections_) {
        retire(section.live);
        retire(section.pending);
    }
    sections_.clear();
    upload_queue_.clear();
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
        s.pending_data.clear();
        s.pending_data.shrink_to_fit();
        s.queued = false;
    }
    if (!barriers.empty()) {
        vk::DependencyInfo dependency{};
        dependency.setBufferMemoryBarriers(barriers);
        cmd.pipelineBarrier2(dependency);
    }
    return true;
}

void VoxelPass::prepare(std::uint32_t /*frame*/, const core::Vec3& camera_position, const core::Mat4& view_projection) {
    visible_keys_.clear();
    all_keys_.clear();
    if (!visible_ || layer_count_ == 0) return;
    const core::Frustum frustum(view_projection);
    std::vector<std::pair<float, std::uint64_t>> sorted;
    sorted.reserve(sections_.size());
    for (const auto& [key, s] : sections_) {
        if (!s.live.valid() || s.quads == 0) continue;
        all_keys_.push_back(key);
        const core::Aabb box{s.origin, s.origin + core::Vec3{16.0f, 16.0f, 16.0f}};
        if (!frustum.intersects(box)) continue;
        const core::Vec3 d = (s.origin + core::Vec3{8.0f, 8.0f, 8.0f}) - camera_position;
        sorted.emplace_back(core::dot(d, d), key);
    }
    // De cerca a lejos: lo cercano tapa y la prueba de profundidad descarta el resto.
    std::sort(sorted.begin(), sorted.end());
    for (const auto& [distance, key] : sorted) visible_keys_.push_back(key);
}

void VoxelPass::drawSections(const vk::raii::CommandBuffer& cmd, const std::vector<std::uint64_t>& keys,
                             const core::Mat4* light_view_projection) const {
    int bound_page = -1;
    std::optional<core::Frustum> light_frustum;
    if (light_view_projection != nullptr) light_frustum.emplace(*light_view_projection);
    for (const std::uint64_t key : keys) {
        const auto it = sections_.find(key);
        if (it == sections_.end() || !it->second.live.valid()) continue;
        const Section& s = it->second;
        if (light_frustum && !light_frustum->intersects(core::Aabb{s.origin, s.origin + core::Vec3{16.0f, 16.0f, 16.0f}})) {
            continue;
        }
        if (s.live.page != bound_page) {
            cmd.bindVertexBuffers(0, *pages_[static_cast<std::size_t>(s.live.page)]->buffer.handle(), {0});
            bound_page = s.live.page;
        }
        GpuVoxelPush push{};
        push.origin_time = core::Vec4{s.origin.x, s.origin.y, s.origin.z, time_};
        if (light_view_projection != nullptr) {
            push.light_view_projection = *light_view_projection;
            push.shadow = 1;
        }
        cmd.pushConstants<GpuVoxelPush>(*layout_, vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
                                        0, push);
        const auto first_vertex = static_cast<std::int32_t>(s.live.offset / 8);
        cmd.drawIndexed(s.quads * 6, 1, 0, first_vertex, 0);
    }
}

void VoxelPass::recordGBuffer(const vk::raii::CommandBuffer& cmd, std::uint32_t /*frame*/,
                              const vk::raii::DescriptorSet& frame_set) const {
    if (visible_keys_.empty() || !*set_) return;
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *gbuffer_pipeline_);
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *layout_, 0, *frame_set, nullptr);
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *layout_, 1, *set_, nullptr);
    cmd.bindIndexBuffer(*quad_indices_.handle(), 0, vk::IndexType::eUint32);
    drawSections(cmd, visible_keys_, nullptr);
}

void VoxelPass::recordShadow(const vk::raii::CommandBuffer& cmd, std::uint32_t /*frame*/,
                             const vk::raii::DescriptorSet& frame_set, const core::Mat4& light_view_projection) const {
    if (all_keys_.empty() || !*set_) return;
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *shadow_pipeline_);
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *layout_, 0, *frame_set, nullptr);
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *layout_, 1, *set_, nullptr);
    cmd.bindIndexBuffer(*quad_indices_.handle(), 0, vk::IndexType::eUint32);
    drawSections(cmd, all_keys_, &light_view_projection);
}

VoxelStats VoxelPass::stats() const {
    VoxelStats s;
    s.sections = static_cast<std::uint32_t>(sections_.size());
    s.visible = static_cast<std::uint32_t>(visible_keys_.size());
    for (const auto& [key, section] : sections_) s.quads += section.quads;
    for (const auto& page : pages_) s.memory_bytes += page->capacity;
    s.pending_uploads = static_cast<std::uint32_t>(upload_queue_.size());
    return s;
}

}  // namespace cramion::gfx
