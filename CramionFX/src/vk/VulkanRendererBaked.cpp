// VulkanRenderer: GI horneada en volumenes de sondas (BakedGi.h). Aparte para
// no engordar VulkanRenderer.cpp. El pase (baked_gi.frag) escribe la imagen
// de GI a media resolucion en lugar del SSGI o los rayos; el filtro y la
// iluminacion no cambian.

#include "CramionFX/vk/VulkanCompat.h"
#include "CramionFX/vk/VulkanRenderer.h"

#include <algorithm>
#include <cstring>

namespace cramion::gfx {

namespace {

struct GpuBakedVolume {
    core::Vec4 min_intensity;
    core::Vec4 size;
    std::uint32_t counts[4];
};

struct GpuBakedVolumes {
    std::uint32_t info[4];
    GpuBakedVolume volumes[kMaxBakedVolumes];
};

}  // namespace

void VulkanRenderer::createBakedGi() {
    using Type = vk::DescriptorType;
    // 5 y 6: el lightmap de superficie (claves de la tabla hash y texeles).
    const std::array<Type, 7> bindings = {Type::eUniformBuffer, Type::eCombinedImageSampler, Type::eCombinedImageSampler,
                                          Type::eStorageBuffer, Type::eUniformBuffer,      Type::eStorageBuffer,
                                          Type::eStorageBuffer};
    FullscreenPassDesc desc{};
    desc.fragment_shader = "baked_gi.frag.spv";
    desc.bindings = bindings;
    desc.push_constant_size = sizeof(GpuSsgiPush);
    desc.color_format = kHdrFormat;
    desc.filter = vk::Filter::eNearest;
    baked_gi_pass_.create(device_, desc);

    const std::array<vk::DescriptorPoolSize, 3> sizes = {
        vk::DescriptorPoolSize{Type::eUniformBuffer, kMaxFramesInFlight * 2},
        vk::DescriptorPoolSize{Type::eCombinedImageSampler, kMaxFramesInFlight * 2},
        vk::DescriptorPoolSize{Type::eStorageBuffer, kMaxFramesInFlight * 3}};
    vk::DescriptorPoolCreateInfo pool_info{};
    pool_info.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    pool_info.maxSets = kMaxFramesInFlight;
    pool_info.setPoolSizes(sizes);
    baked_gi_pool_ = vk::raii::DescriptorPool(device_.handle(), pool_info);
    const std::vector<vk::DescriptorSetLayout> layouts(kMaxFramesInFlight, *baked_gi_pass_.descriptorSetLayout());
    vk::DescriptorSetAllocateInfo alloc{};
    alloc.descriptorPool = *baked_gi_pool_;
    alloc.setSetLayouts(layouts);
    baked_gi_sets_ = vk::raii::DescriptorSets(device_.handle(), alloc);

    // Buffers vacios validos (se rehacen al dar datos).
    const core::Vec4 empty_probe[kBakedProbeVec4] = {};
    baked_probe_buffer_.create(device_, sizeof(empty_probe), vk::BufferUsageFlagBits::eStorageBuffer,
                               vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
    baked_probe_buffer_.write(empty_probe, sizeof(empty_probe));
    baked_volume_buffer_.create(device_, sizeof(GpuBakedVolumes), vk::BufferUsageFlagBits::eUniformBuffer,
                                vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
    GpuBakedVolumes none{};
    baked_volume_buffer_.write(&none, sizeof(none));
    const std::uint32_t empty_keys[2] = {kSurfaceEmptyKey, kSurfaceEmptyKey};
    baked_surface_keys_.create(device_, sizeof(empty_keys), vk::BufferUsageFlagBits::eStorageBuffer,
                               vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
    baked_surface_keys_.write(empty_keys, sizeof(empty_keys));
    const core::Vec4 empty_texel[kSurfaceTexelVec4] = {};
    baked_surface_texels_.create(device_, sizeof(empty_texel), vk::BufferUsageFlagBits::eStorageBuffer,
                                 vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
    baked_surface_texels_.write(empty_texel, sizeof(empty_texel));

    // Sondas dinamicas: un set con las sondas (escritura) y los volumenes.
    std::array<vk::DescriptorSetLayoutBinding, 2> probe_bindings{};
    probe_bindings[0] = vk::DescriptorSetLayoutBinding{0, Type::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute};
    probe_bindings[1] = vk::DescriptorSetLayoutBinding{1, Type::eUniformBuffer, 1, vk::ShaderStageFlagBits::eCompute};
    vk::DescriptorSetLayoutCreateInfo probe_layout_info{};
    probe_layout_info.setBindings(probe_bindings);
    probe_update_layout_ = vk::raii::DescriptorSetLayout(device_.handle(), probe_layout_info);
    const std::array<vk::DescriptorPoolSize, 2> probe_sizes = {vk::DescriptorPoolSize{Type::eStorageBuffer, 1},
                                                               vk::DescriptorPoolSize{Type::eUniformBuffer, 1}};
    vk::DescriptorPoolCreateInfo probe_pool_info{};
    probe_pool_info.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    probe_pool_info.maxSets = 1;
    probe_pool_info.setPoolSizes(probe_sizes);
    probe_update_pool_ = vk::raii::DescriptorPool(device_.handle(), probe_pool_info);
    const vk::DescriptorSetLayout probe_layout = *probe_update_layout_;
    vk::DescriptorSetAllocateInfo probe_alloc{};
    probe_alloc.descriptorPool = *probe_update_pool_;
    probe_alloc.setSetLayouts(probe_layout);
    probe_update_sets_ = vk::raii::DescriptorSets(device_.handle(), probe_alloc);
}

void VulkanRenderer::destroyBakedGi() {
    baked_gi_sets_.clear();
    baked_gi_pool_ = nullptr;
    probe_update_sets_.clear();
    probe_update_pool_ = nullptr;
    probe_update_layout_ = nullptr;
    baked_probe_buffer_.destroy();
    baked_volume_buffer_.destroy();
    baked_surface_keys_.destroy();
    baked_surface_texels_.destroy();
    baked_gi_pass_.destroy();
}

void VulkanRenderer::updateBakedGiSets() {
    if (baked_gi_sets_.empty() || !baked_probe_buffer_.isValid() || !baked_volume_buffer_.isValid()) return;
    if (gbuffer_.depth().extent().width == 0) return;
    for (std::uint32_t i = 0; i < kMaxFramesInFlight && i < baked_gi_sets_.size(); ++i) {
        if (!camera_buffers_[i].isValid()) continue;
        vk::DescriptorBufferInfo camera_info{*camera_buffers_[i].handle(), 0, sizeof(GpuCamera)};
        vk::DescriptorImageInfo depth_info{*baked_gi_pass_.sampler(), *gbuffer_.depth().view(), compat::depthReadOnlyLayout()};
        vk::DescriptorImageInfo normal_info{*baked_gi_pass_.sampler(), *gbuffer_.normal().view(),
                                            vk::ImageLayout::eShaderReadOnlyOptimal};
        vk::DescriptorBufferInfo probe_info{*baked_probe_buffer_.handle(), 0, VK_WHOLE_SIZE};
        vk::DescriptorBufferInfo volume_info{*baked_volume_buffer_.handle(), 0, sizeof(GpuBakedVolumes)};
        vk::DescriptorBufferInfo keys_info{*baked_surface_keys_.handle(), 0, VK_WHOLE_SIZE};
        vk::DescriptorBufferInfo texels_info{*baked_surface_texels_.handle(), 0, VK_WHOLE_SIZE};
        std::array<vk::WriteDescriptorSet, 7> writes{};
        for (std::uint32_t b = 0; b < 7; ++b) {
            writes[b].dstSet = *baked_gi_sets_[i];
            writes[b].dstBinding = b;
            writes[b].descriptorCount = 1;
        }
        writes[0].descriptorType = vk::DescriptorType::eUniformBuffer;
        writes[0].pBufferInfo = &camera_info;
        writes[1].descriptorType = vk::DescriptorType::eCombinedImageSampler;
        writes[1].pImageInfo = &depth_info;
        writes[2].descriptorType = vk::DescriptorType::eCombinedImageSampler;
        writes[2].pImageInfo = &normal_info;
        writes[3].descriptorType = vk::DescriptorType::eStorageBuffer;
        writes[3].pBufferInfo = &probe_info;
        writes[4].descriptorType = vk::DescriptorType::eUniformBuffer;
        writes[4].pBufferInfo = &volume_info;
        writes[5].descriptorType = vk::DescriptorType::eStorageBuffer;
        writes[5].pBufferInfo = &keys_info;
        writes[6].descriptorType = vk::DescriptorType::eStorageBuffer;
        writes[6].pBufferInfo = &texels_info;
        device_.handle().updateDescriptorSets(writes, nullptr);
    }
    if (!probe_update_sets_.empty()) {
        vk::DescriptorBufferInfo probes{*baked_probe_buffer_.handle(), 0, VK_WHOLE_SIZE};
        vk::DescriptorBufferInfo volumes{*baked_volume_buffer_.handle(), 0, sizeof(GpuBakedVolumes)};
        std::array<vk::WriteDescriptorSet, 2> w{};
        w[0].dstSet = *probe_update_sets_[0];
        w[0].dstBinding = 0;
        w[0].descriptorType = vk::DescriptorType::eStorageBuffer;
        w[0].setBufferInfo(probes);
        w[1].dstSet = *probe_update_sets_[0];
        w[1].dstBinding = 1;
        w[1].descriptorType = vk::DescriptorType::eUniformBuffer;
        w[1].setBufferInfo(volumes);
        device_.handle().updateDescriptorSets(w, nullptr);
    }
}

void VulkanRenderer::setBakedLighting(BakedLighting data) {
    // Los frames en vuelo leen los buffers viejos: se espera (pasa al abrir
    // una escena o al terminar un horneado, no cada frame).
    device_.waitIdle();
    baked_ = std::move(data);
    if (baked_.volumes.size() > kMaxBakedVolumes) baked_.volumes.resize(kMaxBakedVolumes);
    if (static_cast<VkPipeline>(*baked_gi_pass_.pipeline()) == VK_NULL_HANDLE) return;
    const std::size_t probe_bytes = std::max<std::size_t>(baked_.probes.size(), kBakedProbeVec4) * sizeof(core::Vec4);
    baked_probe_buffer_.destroy();
    baked_probe_buffer_.create(device_, probe_bytes, vk::BufferUsageFlagBits::eStorageBuffer,
                               vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
    if (!baked_.probes.empty()) {
        baked_probe_buffer_.write(baked_.probes.data(), baked_.probes.size() * sizeof(core::Vec4));
    } else {
        const core::Vec4 empty_probe[kBakedProbeVec4] = {};
        baked_probe_buffer_.write(empty_probe, sizeof(empty_probe));
    }
    GpuBakedVolumes volumes{};
    const std::size_t probe_count = baked_.probes.size() / kBakedProbeVec4;
    std::uint32_t used = 0;
    for (const BakedProbeVolume& v : baked_.volumes) {
        const std::size_t needed = static_cast<std::size_t>(v.first) + static_cast<std::size_t>(v.nx) * v.ny * v.nz;
        if (v.nx < 2 || v.ny < 2 || v.nz < 2 || needed > probe_count) continue;
        GpuBakedVolume& g = volumes.volumes[used++];
        g.min_intensity = core::Vec4{v.min, v.intensity};
        g.size = core::Vec4{v.size, 0.0f};
        g.counts[0] = v.nx;
        g.counts[1] = v.ny;
        g.counts[2] = v.nz;
        g.counts[3] = v.first;
    }
    volumes.info[0] = used;
    // Lightmap de superficie: capacidad de la tabla y lado del texel.
    const SurfaceLightmap& surface = baked_.surface;
    baked_surface_keys_.destroy();
    baked_surface_texels_.destroy();
    const auto host = vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;
    if (!surface.empty() && surface.keys.size() == static_cast<std::size_t>(surface.capacity) * 2 &&
        surface.texels.size() == static_cast<std::size_t>(surface.capacity) * kSurfaceTexelVec4) {
        baked_surface_keys_.create(device_, surface.keys.size() * sizeof(std::uint32_t), vk::BufferUsageFlagBits::eStorageBuffer, host);
        baked_surface_keys_.write(surface.keys.data(), surface.keys.size() * sizeof(std::uint32_t));
        baked_surface_texels_.create(device_, surface.texels.size() * sizeof(core::Vec4), vk::BufferUsageFlagBits::eStorageBuffer, host);
        baked_surface_texels_.write(surface.texels.data(), surface.texels.size() * sizeof(core::Vec4));
        volumes.info[1] = surface.capacity;
        std::memcpy(&volumes.info[2], &surface.cell_size, sizeof(float));
        baked_surface_capacity_ = surface.capacity;
    } else {
        const std::uint32_t empty_keys[2] = {kSurfaceEmptyKey, kSurfaceEmptyKey};
        baked_surface_keys_.create(device_, sizeof(empty_keys), vk::BufferUsageFlagBits::eStorageBuffer, host);
        baked_surface_keys_.write(empty_keys, sizeof(empty_keys));
        const core::Vec4 empty_texel[kSurfaceTexelVec4] = {};
        baked_surface_texels_.create(device_, sizeof(empty_texel), vk::BufferUsageFlagBits::eStorageBuffer, host);
        baked_surface_texels_.write(empty_texel, sizeof(empty_texel));
        baked_surface_capacity_ = 0;
    }
    baked_volume_buffer_.write(&volumes, sizeof(volumes));
    baked_volume_count_ = used;
    baked_probe_count_ = static_cast<std::uint32_t>(probe_count);
    dynamic_probe_cursor_ = 0;
    updateBakedGiSets();
}

bool VulkanRenderer::dynamicProbesActive() const {
    return dynamic_probes_ && bakedGiActive() && baked_probe_count_ > 0 && rayTracingActive() && !isolated() &&
           !probe_update_sets_.empty();
}

bool VulkanRenderer::bakedGiActive() const {
    return lighting_mode_ == LightingMode::Baked && (baked_volume_count_ > 0 || baked_surface_capacity_ > 0);
}

}  // namespace cramion::gfx
