#include "CramionFX/vk/VulkanCompat.h"
#include "CramionFX/vk/VfxPass.h"

#include "CramionFX/asset/ImageFile.h"
#include "CramionFX/vk/VulkanDevice.h"
#include "CramionFX/vk/VulkanShader.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>

namespace cramion::gfx {

namespace {

// Etapas de vfx_sim.comp.
enum Stage : std::uint32_t { kReset = 0, kBegin = 1, kEmit = 2, kUpdate = 3 };

constexpr std::uint32_t kBindings = 10;
constexpr std::uint32_t kCountersPerSlot = 8;
constexpr std::uint32_t kInitialPool = 65536;
constexpr std::uint32_t kGroup = 64;  // local_size_x de vfx_sim.comp

// Bits de flags.z (vfx_common.glsl).
constexpr std::uint32_t kWorldSpace = 1u;
constexpr std::uint32_t kColorOverLife = 2u;
constexpr std::uint32_t kSizeOverLife = 4u;
constexpr std::uint32_t kColorBySpeed = 8u;
constexpr std::uint32_t kLit = 16u;

// MISMO orden que vfx_common.glsl (VfxFrame, std140).
struct FrameGpu {
    core::Mat4 view;
    core::Mat4 projection;
    core::Mat4 view_projection;
    core::Mat4 inverse_view_projection;
    core::Mat4 inverse_projection;
    core::Vec4 camera;
    core::Vec4 right;
    core::Vec4 up;
    core::Vec4 viewport;
    core::Vec4 light;
};
static_assert(sizeof(FrameGpu) == 5 * 64 + 5 * 16);

// MISMO orden que vfx_common.glsl (VfxInstance, std430).
struct InstanceGpu {
    core::Mat4 transform;
    core::Mat4 local_to_world;
    core::Mat4 world_to_local;
    std::uint32_t range[4];
    std::uint32_t blocks[4];
    core::Vec4 time;
    std::uint32_t flags[4];
    core::Vec4 output0;
    core::Vec4 output1;
    core::Vec4 speed_slow;
    core::Vec4 speed_fast;
    core::Vec4 speed_range;
    core::Vec4 shift;
    core::Vec4 emitter_velocity;
    core::Vec4 color_lut[kVfxLutSize];
    core::Vec4 size_lut[kVfxLutSize / 4];
};
static_assert(sizeof(InstanceGpu) == 1008);

struct Push {
    std::uint32_t stage = 0;
    std::uint32_t slot = 0;
    std::uint32_t count = 0;
    std::uint32_t pad = 0;
};

constexpr vk::ShaderStageFlags kAllStages =
    vk::ShaderStageFlagBits::eCompute | vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment;

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

void computeBarrier(const vk::raii::CommandBuffer& cmd) {
    memoryBarrier(cmd, vk::PipelineStageFlagBits2::eComputeShader,
                  vk::AccessFlagBits2::eShaderStorageWrite | vk::AccessFlagBits2::eShaderStorageRead,
                  vk::PipelineStageFlagBits2::eComputeShader,
                  vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite);
}

std::uint32_t groups(std::uint32_t threads) {
    return std::max((threads + kGroup - 1) / kGroup, 1u);
}

}  // namespace

// -----------------------------------------------------------------------------
// Creacion
// -----------------------------------------------------------------------------

void VfxPass::create(const VulkanDevice& device, vk::Format color_format, vk::Format depth_format,
                     std::uint32_t frames_in_flight) {
    destroy();
    device_ = &device;
    frames_ = frames_in_flight;
    const auto& dev = device.handle();

    // --- Set 0 ---
    {
        using Type = vk::DescriptorType;
        const std::array<Type, kBindings> types = {Type::eUniformBuffer,        Type::eStorageBuffer, Type::eStorageBuffer,
                                                   Type::eStorageBuffer,        Type::eStorageBuffer, Type::eStorageBuffer,
                                                   Type::eStorageBuffer,        Type::eStorageBuffer,
                                                   Type::eCombinedImageSampler, Type::eCombinedImageSampler};
        std::array<vk::DescriptorSetLayoutBinding, kBindings> bindings{};
        for (std::uint32_t i = 0; i < kBindings; ++i) {
            bindings[i] = vk::DescriptorSetLayoutBinding{i, types[i], i == 9 ? kMaxTextures : 1u, kAllStages};
        }
        vk::DescriptorSetLayoutCreateInfo info{};
        info.setBindings(bindings);
        set_layout_ = vk::raii::DescriptorSetLayout(dev, info);
    }
    {
        const std::array<vk::DescriptorPoolSize, 3> sizes = {{
            {vk::DescriptorType::eUniformBuffer, frames_in_flight},
            {vk::DescriptorType::eStorageBuffer, 7 * frames_in_flight},
            {vk::DescriptorType::eCombinedImageSampler, (1 + kMaxTextures) * frames_in_flight},
        }};
        vk::DescriptorPoolCreateInfo pool_info{};
        pool_info.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
        pool_info.maxSets = frames_in_flight;
        pool_info.setPoolSizes(sizes);
        pool_ = vk::raii::DescriptorPool(dev, pool_info);
        const std::vector<vk::DescriptorSetLayout> layouts(frames_in_flight, *set_layout_);
        vk::DescriptorSetAllocateInfo alloc{};
        alloc.descriptorPool = *pool_;
        alloc.setSetLayouts(layouts);
        vk::raii::DescriptorSets sets(dev, alloc);
        sets_.clear();
        for (auto& s : sets) sets_.push_back(std::move(s));
    }
    {
        vk::PushConstantRange push{kAllStages, 0, sizeof(Push)};
        vk::PipelineLayoutCreateInfo layout_info{};
        layout_info.setSetLayouts(*set_layout_);
        layout_info.setPushConstantRanges(push);
        layout_ = vk::raii::PipelineLayout(dev, layout_info);
    }
    {
        const vk::raii::ShaderModule module = shaders::loadModule(device, "vfx_sim.comp.spv");
        vk::ComputePipelineCreateInfo pipeline{};
        pipeline.stage = vk::PipelineShaderStageCreateInfo{{}, vk::ShaderStageFlagBits::eCompute, *module, "main"};
        pipeline.layout = *layout_;
        sim_pipeline_ = vk::raii::Pipeline(dev, device.pipelineCache(), pipeline);
    }
    draw_pipelines_[0] = createDrawPipeline(color_format, depth_format, VfxBlend::Additive);
    draw_pipelines_[1] = createDrawPipeline(color_format, depth_format, VfxBlend::Alpha);
    draw_pipelines_[2] = createDrawPipeline(color_format, depth_format, VfxBlend::Premultiplied);

    {
        vk::SamplerCreateInfo info{};
        info.magFilter = vk::Filter::eNearest;
        info.minFilter = vk::Filter::eNearest;
        info.mipmapMode = vk::SamplerMipmapMode::eNearest;
        info.addressModeU = vk::SamplerAddressMode::eClampToEdge;
        info.addressModeV = vk::SamplerAddressMode::eClampToEdge;
        info.addressModeW = vk::SamplerAddressMode::eClampToEdge;
        depth_sampler_ = vk::raii::Sampler(dev, info);
    }
    {
        vk::SamplerCreateInfo info{};
        info.magFilter = vk::Filter::eLinear;
        info.minFilter = vk::Filter::eLinear;
        info.mipmapMode = vk::SamplerMipmapMode::eLinear;
        info.addressModeU = vk::SamplerAddressMode::eClampToEdge;
        info.addressModeV = vk::SamplerAddressMode::eClampToEdge;
        info.addressModeW = vk::SamplerAddressMode::eClampToEdge;
        info.maxLod = VK_LOD_CLAMP_NONE;
        texture_sampler_ = vk::raii::Sampler(dev, info);
    }

    const vk::MemoryPropertyFlags host = vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;
    frame_ubo_.resize(frames_in_flight);
    instance_buf_.resize(frames_in_flight);
    block_buf_.resize(frames_in_flight);
    point_buf_.resize(frames_in_flight);
    readback_.resize(frames_in_flight);
    for (std::uint32_t f = 0; f < frames_in_flight; ++f) {
        frame_ubo_[f].create(device, sizeof(FrameGpu), vk::BufferUsageFlagBits::eUniformBuffer, host);
        instance_buf_[f].create(device, sizeof(InstanceGpu) * kMaxInstances, vk::BufferUsageFlagBits::eStorageBuffer, host);
        block_buf_[f].create(device, sizeof(VfxBlock) * kMaxBlocks, vk::BufferUsageFlagBits::eStorageBuffer, host);
        point_buf_[f].create(device, sizeof(core::Vec4) * 2 * kMaxPoints, vk::BufferUsageFlagBits::eStorageBuffer, host);
        const vk::DeviceSize readback_bytes = sizeof(std::int32_t) * kCountersPerSlot * kMaxInstances;
        try {
            readback_[f].create(device, readback_bytes, vk::BufferUsageFlagBits::eTransferDst,
                                host | vk::MemoryPropertyFlagBits::eHostCached);
        } catch (const std::exception&) {
            readback_[f].create(device, readback_bytes, vk::BufferUsageFlagBits::eTransferDst, host);
        }
    }
    readback_ready_.assign(frames_in_flight, false);
    draws_.assign(frames_in_flight, {});
    counters_.create(device, sizeof(std::int32_t) * kCountersPerSlot * kMaxInstances,
                     vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eIndirectBuffer |
                         vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst,
                     vk::MemoryPropertyFlagBits::eDeviceLocal);
    device.submitOneTime([&](const vk::raii::CommandBuffer& cmd) {
        cmd.fillBuffer(*counters_.handle(), 0, VK_WHOLE_SIZE, 0u);
    });

    const std::uint8_t white[4] = {255, 255, 255, 255};
    white_.create(device, 1, 1, white);
    slot_used_.assign(kMaxInstances, false);
    createPool(kInitialPool);
}

vk::raii::Pipeline VfxPass::createDrawPipeline(vk::Format color_format, vk::Format depth_format, VfxBlend mode) const {
    const VulkanDevice& device = *device_;
    const vk::raii::ShaderModule vertex = shaders::loadModule(device, "vfx.vert.spv");
    const vk::raii::ShaderModule fragment = shaders::loadModule(device, "vfx.frag.spv");
    const std::array<vk::PipelineShaderStageCreateInfo, 2> stages = {{
        {{}, vk::ShaderStageFlagBits::eVertex, *vertex, "main"},
        {{}, vk::ShaderStageFlagBits::eFragment, *fragment, "main"},
    }};
    vk::PipelineVertexInputStateCreateInfo vertex_input{};
    vk::PipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.topology = vk::PrimitiveTopology::eTriangleList;
    vk::PipelineViewportStateCreateInfo viewport{};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    vk::PipelineRasterizationStateCreateInfo raster{};
    raster.polygonMode = vk::PolygonMode::eFill;
    raster.cullMode = vk::CullModeFlagBits::eNone;
    raster.frontFace = vk::FrontFace::eCounterClockwise;
    raster.lineWidth = 1.0f;
    vk::PipelineMultisampleStateCreateInfo multisample{};
    multisample.rasterizationSamples = vk::SampleCountFlagBits::e1;
    vk::PipelineDepthStencilStateCreateInfo depth{};
    depth.depthTestEnable = VK_TRUE;
    depth.depthWriteEnable = VK_FALSE;
    depth.depthCompareOp = vk::CompareOp::eLessOrEqual;

    // El alfa de la imagen HDR guarda la distancia de lo que se ve: no se toca.
    vk::PipelineColorBlendAttachmentState blend{};
    blend.blendEnable = VK_TRUE;
    switch (mode) {
        case VfxBlend::Additive:
            blend.srcColorBlendFactor = vk::BlendFactor::eOne;
            blend.dstColorBlendFactor = vk::BlendFactor::eOne;
            break;
        case VfxBlend::Alpha:
            blend.srcColorBlendFactor = vk::BlendFactor::eSrcAlpha;
            blend.dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
            break;
        case VfxBlend::Premultiplied:
            blend.srcColorBlendFactor = vk::BlendFactor::eOne;
            blend.dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
            break;
    }
    blend.colorBlendOp = vk::BlendOp::eAdd;
    blend.srcAlphaBlendFactor = vk::BlendFactor::eZero;
    blend.dstAlphaBlendFactor = vk::BlendFactor::eOne;
    blend.alphaBlendOp = vk::BlendOp::eAdd;
    blend.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                           vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
    vk::PipelineColorBlendStateCreateInfo color_blend{};
    color_blend.setAttachments(blend);
    const std::array<vk::DynamicState, 2> dynamic_states = {vk::DynamicState::eViewport, vk::DynamicState::eScissor};
    vk::PipelineDynamicStateCreateInfo dynamic{};
    dynamic.setDynamicStates(dynamic_states);
    vk::PipelineRenderingCreateInfo rendering{};
    rendering.setColorAttachmentFormats(color_format);
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
    info.pColorBlendState = &color_blend;
    info.pDynamicState = &dynamic;
    info.layout = *layout_;
    return compat::makeGraphicsPipeline(device, info);
}

void VfxPass::destroy() {
    sets_.clear();
    pool_ = nullptr;
    for (VulkanBuffer& b : frame_ubo_) b.destroy();
    for (VulkanBuffer& b : instance_buf_) b.destroy();
    for (VulkanBuffer& b : block_buf_) b.destroy();
    for (VulkanBuffer& b : point_buf_) b.destroy();
    for (VulkanBuffer& b : readback_) b.destroy();
    frame_ubo_.clear();
    instance_buf_.clear();
    block_buf_.clear();
    point_buf_.clear();
    readback_.clear();
    readback_ready_.clear();
    draws_.clear();
    particles_.destroy();
    dead_.destroy();
    alive_.destroy();
    counters_.destroy();
    white_.destroy();
    for (VulkanTexture& t : textures_) t.destroy();
    texture_paths_ = {};
    sim_pipeline_ = nullptr;
    for (vk::raii::Pipeline& p : draw_pipelines_) p = nullptr;
    layout_ = nullptr;
    set_layout_ = nullptr;
    depth_sampler_ = nullptr;
    texture_sampler_ = nullptr;
    instances_.clear();
    slots_.clear();
    slot_used_.clear();
    free_.clear();
    pool_size_ = 0;
    sim_pending_ = false;
    stats_ = VfxStats{};
    device_ = nullptr;
}

// -----------------------------------------------------------------------------
// Pool de particulas
// -----------------------------------------------------------------------------

void VfxPass::createPool(std::uint32_t size) {
    size = std::clamp(size, kInitialPool, kMaxPool);
    const vk::BufferUsageFlags storage = vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst;
    particles_.create(*device_, sizeof(core::Vec4) * 4 * static_cast<vk::DeviceSize>(size), storage,
                      vk::MemoryPropertyFlagBits::eDeviceLocal);
    dead_.create(*device_, sizeof(std::uint32_t) * static_cast<vk::DeviceSize>(size), storage,
                 vk::MemoryPropertyFlagBits::eDeviceLocal);
    alive_.create(*device_, sizeof(std::uint32_t) * static_cast<vk::DeviceSize>(size), storage,
                  vk::MemoryPropertyFlagBits::eDeviceLocal);
    pool_size_ = size;
    free_.clear();
    free_.push_back(Range{0, size});
    stats_.pool = size;
    stats_.memory_bytes = static_cast<std::uint64_t>(size) * (sizeof(core::Vec4) * 4 + sizeof(std::uint32_t) * 2);
}

bool VfxPass::allocate(std::uint32_t size, std::uint32_t& offset) {
    // Primer hueco que cabe.
    for (std::size_t i = 0; i < free_.size(); ++i) {
        if (free_[i].size < size) continue;
        offset = free_[i].offset;
        free_[i].offset += size;
        free_[i].size -= size;
        if (free_[i].size == 0) free_.erase(free_.begin() + static_cast<std::ptrdiff_t>(i));
        return true;
    }
    return false;
}

void VfxPass::release(std::uint32_t offset, std::uint32_t size) {
    if (size == 0) return;
    free_.push_back(Range{offset, size});
    std::sort(free_.begin(), free_.end(), [](const Range& a, const Range& b) { return a.offset < b.offset; });
    // Juntar los huecos seguidos.
    std::vector<Range> merged;
    for (const Range& r : free_) {
        if (!merged.empty() && merged.back().offset + merged.back().size == r.offset) {
            merged.back().size += r.size;
        } else {
            merged.push_back(r);
        }
    }
    free_ = std::move(merged);
}

bool VfxPass::placeSlot(Slot& slot, std::uint32_t capacity) {
    std::uint32_t offset = 0;
    if (allocate(capacity, offset)) {
        slot.offset = offset;
        slot.capacity = capacity;
        slot.reset = true;
        slot.simulated = false;
        return true;
    }
    // No cabe: pool mas grande (se pierden las particulas de todos; pasa pocas
    // veces y solo al crecer).
    std::uint32_t used = capacity;
    for (const auto& [id, s] : slots_) used += s.capacity;
    if (used > kMaxPool) {
        if (!warned_full_) {
            std::cerr << "[VFX] Sin memoria para mas particulas (maximo " << kMaxPool << " en total)\n";
            warned_full_ = true;
        }
        slot.capacity = 0;
        return false;
    }
    std::uint32_t size = std::max(pool_size_ * 2, kInitialPool);
    while (size < used + used / 4 && size < kMaxPool) size *= 2;
    device_->waitIdle();
    createPool(std::min(size, kMaxPool));
    for (auto& [id, s] : slots_) {
        if (&s == &slot || s.capacity == 0) continue;
        std::uint32_t o = 0;
        if (allocate(s.capacity, o)) {
            s.offset = o;
        } else {
            s.capacity = 0;
        }
        s.reset = true;
        s.simulated = false;
    }
    if (!allocate(capacity, offset)) {
        slot.capacity = 0;
        return false;
    }
    slot.offset = offset;
    slot.capacity = capacity;
    slot.reset = true;
    slot.simulated = false;
    return true;
}

// -----------------------------------------------------------------------------
// Datos de la CPU
// -----------------------------------------------------------------------------

void VfxPass::setInstances(std::vector<VfxInstanceDesc> instances) {
    if (device_ == nullptr) return;
    if (instances.size() > kMaxInstances) instances.resize(kMaxInstances);
    // Los que ya no estan: fuera (su rango y su ranura).
    for (auto it = slots_.begin(); it != slots_.end();) {
        const bool present = std::any_of(instances.begin(), instances.end(),
                                         [&](const VfxInstanceDesc& d) { return d.id == it->first; });
        if (present) {
            ++it;
            continue;
        }
        release(it->second.offset, it->second.capacity);
        slot_used_[it->second.index] = false;
        it = slots_.erase(it);
    }
    // Los nuevos (o con otra capacidad).
    for (VfxInstanceDesc& d : instances) {
        d.capacity = std::clamp<std::uint32_t>(d.capacity, 1u, kMaxCapacity);
        auto it = slots_.find(d.id);
        if (it == slots_.end()) {
            const auto free_slot = std::find(slot_used_.begin(), slot_used_.end(), false);
            if (free_slot == slot_used_.end()) continue;
            Slot slot;
            slot.index = static_cast<std::uint32_t>(free_slot - slot_used_.begin());
            *free_slot = true;
            it = slots_.emplace(d.id, slot).first;
            placeSlot(it->second, d.capacity);
        } else if (it->second.capacity != d.capacity) {
            release(it->second.offset, it->second.capacity);
            it->second.capacity = 0;
            placeSlot(it->second, d.capacity);
        }
        if (d.reset) it->second.reset = true;
    }
    instances_ = std::move(instances);
    sim_pending_ = true;
}

void VfxPass::clear() {
    for (auto& [id, slot] : slots_) slot.reset = true;
}

int VfxPass::loadTexture(const std::filesystem::path& file) {
    if (device_ == nullptr || file.empty()) return -1;
    std::error_code error;
    const std::filesystem::path key = std::filesystem::weakly_canonical(file, error);
    for (std::uint32_t slot = 0; slot < kMaxTextures; ++slot) {
        if (!texture_paths_[slot].empty() && texture_paths_[slot] == key) return static_cast<int>(slot);
    }
    std::uint32_t slot = 0;
    while (slot < kMaxTextures && !texture_paths_[slot].empty()) ++slot;
    if (slot == kMaxTextures) {
        std::cerr << "[VFX] Sin ranuras para mas texturas (maximo " << kMaxTextures << ")\n";
        return -1;
    }
    asset::ImageRgba8 image;
    if (!asset::loadImageRgba8(file, image, 2048) || image.width == 0 || image.height == 0) {
        std::cerr << "[VFX] No se pudo leer la textura " << file.string() << "\n";
        return -1;
    }
    // La ranura es nueva (no la usa ningun frame en vuelo), pero el set de
    // descriptores se reescribe en cada prepare: no hace falta esperar.
    textures_[slot].create(*device_, image.width, image.height, image.pixels.data());
    texture_paths_[slot] = key;
    return static_cast<int>(slot);
}

// -----------------------------------------------------------------------------
// Grabacion
// -----------------------------------------------------------------------------

void VfxPass::readBack(std::uint32_t frame) {
    if (frame >= readback_.size() || !readback_ready_[frame]) return;
    readback_ready_[frame] = false;
    const auto* counters = static_cast<const std::int32_t*>(readback_[frame].mapped());
    if (counters == nullptr) return;
    std::uint32_t alive = 0;
    for (auto& [id, slot] : slots_) {
        if (!slot.simulated || slot.capacity == 0) continue;
        const std::int32_t value = counters[slot.index * kCountersPerSlot + 1];
        slot.alive = static_cast<std::uint32_t>(std::clamp<std::int32_t>(value, 0, static_cast<std::int32_t>(slot.capacity)));
        alive += slot.alive;
    }
    stats_.alive = alive;
}

bool VfxPass::prepare(std::uint32_t frame, const VfxView& view, bool simulate) {
    if (device_ == nullptr || frame >= frames_) return false;
    readBack(frame);
    draws_[frame].clear();
    if (instances_.empty()) {
        stats_.effects = 0;
        stats_.capacity = 0;
        return false;
    }
    simulate = simulate && sim_pending_;
    if (simulate) frame_shift_ = pending_shift_;

    // --- Camara ---
    FrameGpu f{};
    f.view = view.view;
    f.projection = view.projection;
    f.view_projection = view.view_projection;
    f.inverse_view_projection = core::inverse(view.view_projection);
    f.inverse_projection = core::inverse(view.projection);
    const bool has_depth = static_cast<VkImageView>(view.scene_depth) != VK_NULL_HANDLE;
    f.camera = core::Vec4{view.camera_position, has_depth ? 1.0f : 0.0f};
    // Ejes de la camara: las filas de la rotacion de la vista.
    const core::Vec3 right{view.view.m[0][0], view.view.m[1][0], view.view.m[2][0]};
    const core::Vec3 up{view.view.m[0][1], view.view.m[1][1], view.view.m[2][1]};
    f.right = core::Vec4{core::normalize(right), 0.0f};
    f.up = core::Vec4{core::normalize(up), 0.0f};
    const float w = static_cast<float>(std::max(view.extent.width, 1u));
    const float h = static_cast<float>(std::max(view.extent.height, 1u));
    f.viewport = core::Vec4{w, h, 1.0f / w, 1.0f / h};
    f.light = core::Vec4{view.light, 1.0f};
    frame_ubo_[frame].write(&f, sizeof(f));

    // --- Efectos, bloques y puntos ---
    std::vector<InstanceGpu> gpu(kMaxInstances);
    std::vector<VfxBlock> block_data;
    std::vector<core::Vec4> point_data;
    block_data.reserve(256);
    std::uint32_t capacity_total = 0;
    std::uint32_t effects = 0;
    for (const VfxInstanceDesc& d : instances_) {
        const auto it = slots_.find(d.id);
        if (it == slots_.end() || it->second.capacity == 0) continue;
        const Slot& slot = it->second;
        InstanceGpu& g = gpu[slot.index];
        g.local_to_world = d.transform;
        g.world_to_local = core::inverse(d.transform);
        g.transform = d.world_space ? core::Mat4::identity() : d.transform;
        // Puntos de la malla (antes de los bloques: les dan su primer indice).
        std::uint32_t first_point = static_cast<std::uint32_t>(point_data.size() / 2);
        std::uint32_t point_count = 0;
        if (d.points && !d.points->empty()) {
            const std::size_t pairs = d.points->size() / 2;
            const std::size_t room = kMaxPoints - std::min<std::size_t>(point_data.size() / 2, kMaxPoints);
            point_count = static_cast<std::uint32_t>(std::min(pairs, room));
            point_data.insert(point_data.end(), d.points->begin(),
                              d.points->begin() + static_cast<std::ptrdiff_t>(point_count * 2));
        }
        const auto push_blocks = [&](const std::vector<VfxBlock>& list, std::uint32_t& first, std::uint32_t& count) {
            first = static_cast<std::uint32_t>(block_data.size());
            count = 0;
            for (VfxBlock b : list) {
                if (block_data.size() >= kMaxBlocks) break;
                if (b.type == static_cast<std::uint32_t>(VfxBlockType::PositionMesh)) {
                    b.a.x = static_cast<float>(first_point);
                    b.a.y = static_cast<float>(point_count);
                }
                block_data.push_back(b);
                ++count;
            }
        };
        push_blocks(d.initialize, g.blocks[0], g.blocks[1]);
        push_blocks(d.update, g.blocks[2], g.blocks[3]);
        g.range[0] = slot.offset;
        g.range[1] = slot.capacity;
        g.range[2] = slot.index;
        g.range[3] = std::min(d.spawn_count, slot.capacity);
        g.time = core::Vec4{std::clamp(d.delta_seconds, 0.0f, 0.25f), d.time, 0.0f, 0.0f};
        std::uint32_t bits = 0;
        if (d.world_space) bits |= kWorldSpace;
        if (d.color_over_life) bits |= kColorOverLife;
        if (d.size_over_life) bits |= kSizeOverLife;
        if (d.color_by_speed) bits |= kColorBySpeed;
        if (d.lit) bits |= kLit;
        g.flags[0] = d.seed;
        g.flags[1] = frame_counter_;
        g.flags[2] = bits;
        g.flags[3] = d.texture >= 0 && d.texture < static_cast<int>(kMaxTextures) && !texture_paths_[static_cast<std::size_t>(d.texture)].empty()
                         ? static_cast<std::uint32_t>(d.texture)
                         : 0xFFFFFFFFu;
        g.output0 = core::Vec4{static_cast<float>(d.orient), static_cast<float>(d.blend), std::max(d.soft_distance, 0.0f),
                               std::max(d.intensity, 0.0f)};
        g.output1 = core::Vec4{std::max(d.stretch, 0.0f), static_cast<float>(std::max(d.flip_cols, 1u)),
                               static_cast<float>(std::max(d.flip_rows, 1u)), std::max(d.flip_fps, 0.0f)};
        g.speed_slow = d.speed_color_slow;
        g.speed_fast = d.speed_color_fast;
        g.speed_range = core::Vec4{d.speed_min, std::max(d.speed_max, d.speed_min + 1e-3f), std::clamp(d.alpha_clip, 0.0f, 1.0f), 0.0f};
        g.shift = simulate ? core::Vec4{frame_shift_, 0.0f} : core::Vec4{};
        g.emitter_velocity = core::Vec4{d.emitter_velocity, 0.0f};
        for (std::uint32_t i = 0; i < kVfxLutSize; ++i) g.color_lut[i] = d.color_lut[i];
        for (std::uint32_t i = 0; i < kVfxLutSize / 4; ++i) {
            g.size_lut[i] = core::Vec4{d.size_lut[i * 4], d.size_lut[i * 4 + 1], d.size_lut[i * 4 + 2], d.size_lut[i * 4 + 3]};
        }
        capacity_total += slot.capacity;
        ++effects;
        // Se dibujan las que ya tienen contadores validos (o los tendran tras
        // la simulacion de este mismo frame).
        if (d.visible && (simulate || (slot.simulated && !slot.reset))) {
            const core::Vec3 position{d.transform.m[3][0], d.transform.m[3][1], d.transform.m[3][2]};
            draws_[frame].push_back(DrawItem{slot.index, d.blend, core::length(position - view.camera_position)});
        }
    }
    instance_buf_[frame].write(gpu.data(), sizeof(InstanceGpu) * gpu.size());
    if (!block_data.empty()) block_buf_[frame].write(block_data.data(), sizeof(VfxBlock) * block_data.size());
    if (!point_data.empty()) point_buf_[frame].write(point_data.data(), sizeof(core::Vec4) * point_data.size());
    stats_.effects = effects;
    stats_.capacity = capacity_total;

    // Transparentes de atras adelante, luego las aditivas.
    std::stable_sort(draws_[frame].begin(), draws_[frame].end(), [](const DrawItem& a, const DrawItem& b) {
        const bool aa = a.blend != VfxBlend::Additive;
        const bool ba = b.blend != VfxBlend::Additive;
        if (aa != ba) return aa;
        return aa && a.distance > b.distance;
    });

    // --- Descriptores (todos, cada vez: el depth puede cambiar de tamano) ---
    const vk::DescriptorBufferInfo ubo{*frame_ubo_[frame].handle(), 0, sizeof(FrameGpu)};
    const std::array<vk::DescriptorBufferInfo, 7> storage = {{
        {*particles_.handle(), 0, VK_WHOLE_SIZE},
        {*dead_.handle(), 0, VK_WHOLE_SIZE},
        {*alive_.handle(), 0, VK_WHOLE_SIZE},
        {*counters_.handle(), 0, VK_WHOLE_SIZE},
        {*instance_buf_[frame].handle(), 0, VK_WHOLE_SIZE},
        {*block_buf_[frame].handle(), 0, VK_WHOLE_SIZE},
        {*point_buf_[frame].handle(), 0, VK_WHOLE_SIZE},
    }};
    const vk::DescriptorImageInfo depth_info =
        has_depth ? vk::DescriptorImageInfo{*depth_sampler_, view.scene_depth, compat::depthReadOnlyLayout()}
                  : vk::DescriptorImageInfo{*depth_sampler_, *white_.view(), vk::ImageLayout::eShaderReadOnlyOptimal};
    std::array<vk::DescriptorImageInfo, kMaxTextures> textures{};
    for (std::uint32_t i = 0; i < kMaxTextures; ++i) {
        const bool loaded = !texture_paths_[i].empty();
        textures[i] = vk::DescriptorImageInfo{*texture_sampler_, loaded ? *textures_[i].view() : *white_.view(),
                                              vk::ImageLayout::eShaderReadOnlyOptimal};
    }
    std::array<vk::WriteDescriptorSet, kBindings> writes{};
    for (std::uint32_t i = 0; i < kBindings; ++i) {
        writes[i].dstSet = *sets_[frame];
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
    }
    writes[0].descriptorType = vk::DescriptorType::eUniformBuffer;
    writes[0].pBufferInfo = &ubo;
    for (std::uint32_t i = 0; i < 7; ++i) {
        writes[1 + i].descriptorType = vk::DescriptorType::eStorageBuffer;
        writes[1 + i].pBufferInfo = &storage[i];
    }
    writes[8].descriptorType = vk::DescriptorType::eCombinedImageSampler;
    writes[8].pImageInfo = &depth_info;
    writes[9].descriptorType = vk::DescriptorType::eCombinedImageSampler;
    writes[9].descriptorCount = kMaxTextures;
    writes[9].pImageInfo = textures.data();
    device_->handle().updateDescriptorSets(writes, nullptr);
    return effects > 0;
}

void VfxPass::recordSimulate(const vk::raii::CommandBuffer& cmd, std::uint32_t frame) {
    if (device_ == nullptr || !sim_pending_ || frame >= frames_) return;
    sim_pending_ = false;
    pending_shift_ = pending_shift_ - frame_shift_;
    frame_shift_ = core::Vec3{};
    ++frame_counter_;

    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *sim_pipeline_);
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *layout_, 0, *sets_[frame], nullptr);
    // Lo que leyo el frame anterior (dibujo, lectura) antes de escribir.
    memoryBarrier(cmd,
                  vk::PipelineStageFlagBits2::eVertexShader | vk::PipelineStageFlagBits2::eFragmentShader |
                      vk::PipelineStageFlagBits2::eDrawIndirect | vk::PipelineStageFlagBits2::eCopy,
                  vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eIndirectCommandRead |
                      vk::AccessFlagBits2::eTransferRead,
                  vk::PipelineStageFlagBits2::eComputeShader,
                  vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite);
    // El depth de la escena (escrito por la geometria) se lee en las colisiones.
    memoryBarrier(cmd, vk::PipelineStageFlagBits2::eLateFragmentTests | vk::PipelineStageFlagBits2::eEarlyFragmentTests,
                  vk::AccessFlagBits2::eDepthStencilAttachmentWrite, vk::PipelineStageFlagBits2::eComputeShader,
                  vk::AccessFlagBits2::eShaderSampledRead);

    const auto dispatch = [&](std::uint32_t stage, std::uint32_t slot, std::uint32_t count) {
        Push push{};
        push.stage = stage;
        push.slot = slot;
        push.count = count;
        cmd.pushConstants<Push>(*layout_, kAllStages, 0, push);
        cmd.dispatch(groups(count), 1, 1);
    };

    // RESET de las nuevas o reiniciadas.
    bool any_reset = false;
    for (const VfxInstanceDesc& d : instances_) {
        auto it = slots_.find(d.id);
        if (it == slots_.end() || it->second.capacity == 0 || !it->second.reset) continue;
        dispatch(kReset, it->second.index, it->second.capacity);
        it->second.reset = false;
        any_reset = true;
    }
    if (any_reset) computeBarrier(cmd);
    // BEGIN: las vivas del dibujo a 0 (todas las ranuras).
    dispatch(kBegin, 0, kMaxInstances);
    computeBarrier(cmd);
    // EMIT
    bool any_emit = false;
    for (const VfxInstanceDesc& d : instances_) {
        const auto it = slots_.find(d.id);
        if (it == slots_.end() || it->second.capacity == 0) continue;
        const std::uint32_t spawn = std::min(d.spawn_count, it->second.capacity);
        if (spawn == 0) continue;
        dispatch(kEmit, it->second.index, spawn);
        any_emit = true;
    }
    if (any_emit) computeBarrier(cmd);
    // UPDATE
    for (const VfxInstanceDesc& d : instances_) {
        auto it = slots_.find(d.id);
        if (it == slots_.end() || it->second.capacity == 0) continue;
        dispatch(kUpdate, it->second.index, it->second.capacity);
        it->second.simulated = true;
    }

    // Lectura de los contadores (llega cuando este hueco de frame vuelva).
    memoryBarrier(cmd, vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderStorageWrite,
                  vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferRead);
    cmd.copyBuffer(*counters_.handle(), *readback_[frame].handle(),
                   vk::BufferCopy{0, 0, sizeof(std::int32_t) * kCountersPerSlot * kMaxInstances});
    memoryBarrier(cmd, vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferWrite,
                  vk::PipelineStageFlagBits2::eHost, vk::AccessFlagBits2::eHostRead);
    readback_ready_[frame] = true;
    // Para dibujar: particulas y lista de vivas (vertices) y el contador (indirecto).
    memoryBarrier(cmd, vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderStorageWrite,
                  vk::PipelineStageFlagBits2::eVertexShader | vk::PipelineStageFlagBits2::eFragmentShader |
                      vk::PipelineStageFlagBits2::eDrawIndirect,
                  vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eIndirectCommandRead);
}

void VfxPass::recordDraw(const vk::raii::CommandBuffer& cmd, std::uint32_t frame, vk::Extent2D viewport) const {
    if (device_ == nullptr || frame >= draws_.size() || draws_[frame].empty()) return;
    cmd.setViewport(0, vk::Viewport{0.0f, 0.0f, static_cast<float>(viewport.width), static_cast<float>(viewport.height),
                                    0.0f, 1.0f});
    cmd.setScissor(0, vk::Rect2D{vk::Offset2D{0, 0}, viewport});
    int bound = -1;
    for (const DrawItem& item : draws_[frame]) {
        const auto pipeline = static_cast<int>(item.blend);
        if (pipeline != bound) {
            cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *draw_pipelines_[static_cast<std::size_t>(pipeline)]);
            cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *layout_, 0, *sets_[frame], nullptr);
            bound = pipeline;
        }
        Push push{};
        push.slot = item.slot;
        cmd.pushConstants<Push>(*layout_, kAllStages, 0, push);
        const vk::DeviceSize offset = sizeof(std::int32_t) * (static_cast<vk::DeviceSize>(item.slot) * kCountersPerSlot + 4);
        cmd.drawIndirect(*counters_.handle(), offset, 1, 16);
    }
}

std::uint32_t VfxPass::aliveCount(std::uint64_t id) const {
    const auto it = slots_.find(id);
    return it != slots_.end() ? it->second.alive : 0u;
}

}  // namespace cramion::gfx
