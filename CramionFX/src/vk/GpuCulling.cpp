#include "CramionFX/vk/GpuCulling.h"

#include "CramionFX/vk/VulkanDevice.h"
#include "CramionFX/vk/VulkanImage.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <iostream>

namespace cramion::gfx {
namespace {

struct CullPush {
    std::uint32_t cluster_count = 0;
    std::uint32_t phase = 0;
    std::uint32_t occlusion = 0;
    std::uint32_t pad = 0;
};

struct HizPush {
    std::uint32_t level = 0;
};

constexpr std::uint32_t kStatsCount = 4;
constexpr std::uint32_t kMaxHizLevels = 16;

void memoryBarrier(const vk::raii::CommandBuffer& cmd, vk::PipelineStageFlags2 src_stage,
                   vk::AccessFlags2 src_access, vk::PipelineStageFlags2 dst_stage,
                   vk::AccessFlags2 dst_access) {
    vk::MemoryBarrier2 barrier{};
    barrier.srcStageMask = src_stage;
    barrier.srcAccessMask = src_access;
    barrier.dstStageMask = dst_stage;
    barrier.dstAccessMask = dst_access;
    vk::DependencyInfo dependency{};
    dependency.setMemoryBarriers(barrier);
    cmd.pipelineBarrier2(dependency);
}

}  // namespace

void GpuCulling::create(const VulkanDevice& device) {
    destroy();

    using Type = vk::DescriptorType;
    const std::array<Type, 7> cull_bindings = {
        Type::eUniformBuffer, Type::eStorageBuffer, Type::eStorageBuffer, Type::eStorageBuffer,
        Type::eStorageBuffer, Type::eStorageBuffer, Type::eCombinedImageSampler};
    ComputePassDesc cull{};
    cull.shader = "cull.comp.spv";
    cull.bindings = cull_bindings;
    cull.push_constant_size = sizeof(CullPush);
    cull_pass_.create(device, cull);

    const std::array<Type, 3> hiz_bindings = {Type::eCombinedImageSampler, Type::eStorageImage,
                                              Type::eStorageImage};
    ComputePassDesc hiz{};
    hiz.shader = "hiz.comp.spv";
    hiz.bindings = hiz_bindings;
    hiz.push_constant_size = sizeof(HizPush);
    hiz_pass_.create(device, hiz);

    // Lectura exacta de un texel (texelFetch); el muestreador solo hace falta
    // para el descriptor.
    vk::SamplerCreateInfo sampler_info{};
    sampler_info.magFilter = vk::Filter::eNearest;
    sampler_info.minFilter = vk::Filter::eNearest;
    sampler_info.mipmapMode = vk::SamplerMipmapMode::eNearest;
    sampler_info.addressModeU = vk::SamplerAddressMode::eClampToEdge;
    sampler_info.addressModeV = vk::SamplerAddressMode::eClampToEdge;
    sampler_info.addressModeW = vk::SamplerAddressMode::eClampToEdge;
    sampler_info.maxLod = static_cast<float>(kMaxHizLevels);
    hiz_sampler_ = vk::raii::Sampler(device.handle(), sampler_info);

    constexpr std::uint32_t kCullSets = kMaxFramesInFlight * 2;
    const std::array<vk::DescriptorPoolSize, 4> pool_sizes = {
        vk::DescriptorPoolSize{Type::eUniformBuffer, kCullSets},
        vk::DescriptorPoolSize{Type::eStorageBuffer, kCullSets * 5},
        vk::DescriptorPoolSize{Type::eCombinedImageSampler, kCullSets + kMaxHizLevels},
        vk::DescriptorPoolSize{Type::eStorageImage, kMaxHizLevels * 2}};
    vk::DescriptorPoolCreateInfo pool_info{};
    pool_info.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    pool_info.maxSets = kCullSets + kMaxHizLevels;
    pool_info.setPoolSizes(pool_sizes);
    pool_ = vk::raii::DescriptorPool(device.handle(), pool_info);

    const std::vector<vk::DescriptorSetLayout> layouts(kCullSets,
                                                       *cull_pass_.descriptorSetLayout());
    vk::DescriptorSetAllocateInfo alloc{};
    alloc.descriptorPool = *pool_;
    alloc.setSetLayouts(layouts);
    cull_sets_ = vk::raii::DescriptorSets(device.handle(), alloc);

    for (VulkanBuffer& stats : stats_) {
        stats.create(device, sizeof(std::uint32_t) * kStatsCount,
                     vk::BufferUsageFlagBits::eStorageBuffer |
                         vk::BufferUsageFlagBits::eTransferDst,
                     vk::MemoryPropertyFlagBits::eHostVisible |
                         vk::MemoryPropertyFlagBits::eHostCoherent);
        std::memset(stats.mapped(), 0, sizeof(std::uint32_t) * kStatsCount);
    }
}

void GpuCulling::destroy() {
    hiz_sets_.clear();
    cull_sets_.clear();
    pool_ = nullptr;
    hiz_pass_.destroy();
    cull_pass_.destroy();
    hiz_sampler_ = nullptr;
    hiz_level_views_.clear();
    hiz_view_ = nullptr;
    hiz_image_ = nullptr;
    hiz_memory_ = nullptr;
    hiz_levels_ = 0;
    for (VulkanBuffer& buffer : clusters_) {
        buffer.destroy();
    }
    for (VulkanBuffer& buffer : stats_) {
        buffer.destroy();
    }
    visibility_.destroy();
    for (VulkanBuffer& buffer : commands_) {
        buffer.destroy();
    }
    for (VulkanBuffer& buffer : counts_) {
        buffer.destroy();
    }
    cluster_count_ = 0;
    cluster_capacity_ = 0;
    group_capacity_ = 0;
    slot_capacity_ = 0;
}

void GpuCulling::resize(const VulkanDevice& device, const VulkanImage& depth) {
    hiz_sets_.clear();
    hiz_level_views_.clear();
    hiz_view_ = nullptr;
    hiz_image_ = nullptr;
    hiz_memory_ = nullptr;

    // Nivel 0: la potencia de dos inmediatamente menor que el depth buffer
    // (1920x1009 -> 1024x512). Asi cada nivel mide exactamente la mitad del
    // anterior y cull.comp puede pasar de uv a texel con uv * tamano en
    // cualquier nivel. Con la mitad del depth (960x505) los niveles se
    // redondeaban hacia abajo (505, 252, 126, 63, 31, 15...), el ultimo texel
    // de cada fila impar cubria mas que los otros y ese mapeo leia el texel
    // equivocado: objetos visibles se daban por tapados y desaparecian al
    // moverse.
    const vk::Extent2D depth_extent = depth.extent();
    hiz_extent_ = vk::Extent2D{std::bit_floor(std::max(depth_extent.width, 1u)),
                               std::bit_floor(std::max(depth_extent.height, 1u))};
    hiz_levels_ = std::min<std::uint32_t>(
        std::bit_width(std::max(hiz_extent_.width, hiz_extent_.height)), kMaxHizLevels);

    vk::ImageCreateInfo image_info{};
    image_info.imageType = vk::ImageType::e2D;
    image_info.format = vk::Format::eR32Sfloat;
    image_info.extent = vk::Extent3D{hiz_extent_.width, hiz_extent_.height, 1};
    image_info.mipLevels = hiz_levels_;
    image_info.arrayLayers = 1;
    image_info.samples = vk::SampleCountFlagBits::e1;
    image_info.tiling = vk::ImageTiling::eOptimal;
    image_info.usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
                       vk::ImageUsageFlagBits::eTransferDst;
    image_info.initialLayout = vk::ImageLayout::eUndefined;
    hiz_image_ = vk::raii::Image(device.handle(), image_info);

    const vk::MemoryRequirements requirements = hiz_image_.getMemoryRequirements();
    vk::MemoryAllocateInfo allocate_info{};
    allocate_info.allocationSize = requirements.size;
    allocate_info.memoryTypeIndex = device.findMemoryType(
        requirements.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal);
    hiz_memory_ = vk::raii::DeviceMemory(device.handle(), allocate_info);
    hiz_image_.bindMemory(*hiz_memory_, 0);

    vk::ImageViewCreateInfo view_info{};
    view_info.image = *hiz_image_;
    view_info.viewType = vk::ImageViewType::e2D;
    view_info.format = vk::Format::eR32Sfloat;
    view_info.subresourceRange =
        vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, hiz_levels_, 0, 1};
    hiz_view_ = vk::raii::ImageView(device.handle(), view_info);
    for (std::uint32_t level = 0; level < hiz_levels_; ++level) {
        view_info.subresourceRange.baseMipLevel = level;
        view_info.subresourceRange.levelCount = 1;
        hiz_level_views_.emplace_back(device.handle(), view_info);
    }

    // Vive siempre en General (se escribe como storage y se lee con texelFetch).
    // Hasta el primer frame que la construya vale 0: nada se da por tapado.
    const vk::Image image = *hiz_image_;
    const std::uint32_t levels = hiz_levels_;
    device.submitOneTime([&](const vk::raii::CommandBuffer& cmd) {
        vk::ImageMemoryBarrier2 barrier{};
        barrier.srcStageMask = vk::PipelineStageFlagBits2::eNone;
        barrier.dstStageMask = vk::PipelineStageFlagBits2::eTransfer;
        barrier.dstAccessMask = vk::AccessFlagBits2::eTransferWrite;
        barrier.oldLayout = vk::ImageLayout::eUndefined;
        barrier.newLayout = vk::ImageLayout::eGeneral;
        barrier.image = image;
        barrier.subresourceRange =
            vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, levels, 0, 1};
        vk::DependencyInfo dependency{};
        dependency.setImageMemoryBarriers(barrier);
        cmd.pipelineBarrier2(dependency);
        cmd.clearColorImage(image, vk::ImageLayout::eGeneral,
                            vk::ClearColorValue{1.0f, 1.0f, 1.0f, 1.0f},
                            barrier.subresourceRange);
        barrier.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
        barrier.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
        barrier.dstStageMask = vk::PipelineStageFlagBits2::eComputeShader;
        barrier.dstAccessMask = vk::AccessFlagBits2::eShaderRead;
        barrier.oldLayout = vk::ImageLayout::eGeneral;
        cmd.pipelineBarrier2(dependency);
    });

    // --- Sets de la piramide: depth + nivel anterior + este nivel ---
    const std::vector<vk::DescriptorSetLayout> layouts(hiz_levels_,
                                                       *hiz_pass_.descriptorSetLayout());
    vk::DescriptorSetAllocateInfo alloc{};
    alloc.descriptorPool = *pool_;
    alloc.setSetLayouts(layouts);
    hiz_sets_ = vk::raii::DescriptorSets(device.handle(), alloc);

    for (std::uint32_t level = 0; level < hiz_levels_; ++level) {
        vk::DescriptorImageInfo depth_info{};
        depth_info.sampler = *hiz_sampler_;
        depth_info.imageView = *depth.view();
        depth_info.imageLayout = vk::ImageLayout::eDepthReadOnlyOptimal;

        // El nivel 0 no lee "nivel anterior": se enlaza a si mismo, sin usarlo.
        vk::DescriptorImageInfo source_info{};
        source_info.imageView = *hiz_level_views_[level == 0 ? 0 : level - 1];
        source_info.imageLayout = vk::ImageLayout::eGeneral;

        vk::DescriptorImageInfo target_info{};
        target_info.imageView = *hiz_level_views_[level];
        target_info.imageLayout = vk::ImageLayout::eGeneral;

        std::array<vk::WriteDescriptorSet, 3> writes{};
        writes[0].dstSet = *hiz_sets_[level];
        writes[0].dstBinding = 0;
        writes[0].descriptorType = vk::DescriptorType::eCombinedImageSampler;
        writes[0].setImageInfo(depth_info);
        writes[1].dstSet = *hiz_sets_[level];
        writes[1].dstBinding = 1;
        writes[1].descriptorType = vk::DescriptorType::eStorageImage;
        writes[1].setImageInfo(source_info);
        writes[2].dstSet = *hiz_sets_[level];
        writes[2].dstBinding = 2;
        writes[2].descriptorType = vk::DescriptorType::eStorageImage;
        writes[2].setImageInfo(target_info);
        device.handle().updateDescriptorSets(writes, nullptr);
    }

    // Los sets del culling apuntan a la piramide: si ya habia buffers, se
    // actualiza solo esa entrada.
    if (cluster_capacity_ > 0) {
        for (const vk::raii::DescriptorSet& set : cull_sets_) {
            vk::DescriptorImageInfo hiz_info{};
            hiz_info.sampler = *hiz_sampler_;
            hiz_info.imageView = *hiz_view_;
            hiz_info.imageLayout = vk::ImageLayout::eGeneral;
            vk::WriteDescriptorSet write{};
            write.dstSet = *set;
            write.dstBinding = 6;
            write.descriptorType = vk::DescriptorType::eCombinedImageSampler;
            write.setImageInfo(hiz_info);
            device.handle().updateDescriptorSets(write, nullptr);
        }
    }
}

void GpuCulling::createBuffers(const VulkanDevice& device, std::uint32_t cluster_capacity,
                               std::uint32_t group_capacity, std::uint32_t slot_capacity) {
    for (VulkanBuffer& buffer : clusters_) {
        buffer.create(device, sizeof(GpuCluster) * cluster_capacity,
                      vk::BufferUsageFlagBits::eStorageBuffer,
                      vk::MemoryPropertyFlagBits::eHostVisible |
                          vk::MemoryPropertyFlagBits::eHostCoherent);
    }

    // Visibilidad a 0: el primer frame todo se prueba en la fase tardia.
    const std::vector<std::uint32_t> zeros(cluster_capacity, 0);
    visibility_ = VulkanBuffer::createDeviceLocal(device, zeros.data(),
                                                  sizeof(std::uint32_t) * cluster_capacity,
                                                  vk::BufferUsageFlagBits::eStorageBuffer);

    for (std::uint32_t phase = 0; phase < 2; ++phase) {
        commands_[phase].create(device, static_cast<vk::DeviceSize>(kCommandSize) * slot_capacity,
                                vk::BufferUsageFlagBits::eStorageBuffer |
                                    vk::BufferUsageFlagBits::eIndirectBuffer,
                                vk::MemoryPropertyFlagBits::eDeviceLocal);
        counts_[phase].create(device, sizeof(std::uint32_t) * group_capacity,
                              vk::BufferUsageFlagBits::eStorageBuffer |
                                  vk::BufferUsageFlagBits::eIndirectBuffer |
                                  vk::BufferUsageFlagBits::eTransferDst,
                              vk::MemoryPropertyFlagBits::eDeviceLocal);
    }

    cluster_capacity_ = cluster_capacity;
    group_capacity_ = group_capacity;
    slot_capacity_ = slot_capacity;
}

void GpuCulling::writeCullSets(const VulkanDevice& device,
                               const std::vector<VulkanBuffer>& camera_buffers) {
    const auto buffer_info = [](const VulkanBuffer& buffer) {
        vk::DescriptorBufferInfo info{};
        info.buffer = *buffer.handle();
        info.range = VK_WHOLE_SIZE;
        return info;
    };

    for (std::uint32_t frame = 0; frame < kMaxFramesInFlight; ++frame) {
        for (std::uint32_t phase = 0; phase < 2; ++phase) {
            const vk::raii::DescriptorSet& set = cull_sets_[frame * 2 + phase];

            const std::array<vk::DescriptorBufferInfo, 6> buffers = {
                buffer_info(camera_buffers[frame]), buffer_info(clusters_[frame]),
                buffer_info(visibility_),           buffer_info(commands_[phase]),
                buffer_info(counts_[phase]),        buffer_info(stats_[frame])};

            std::array<vk::WriteDescriptorSet, 7> writes{};
            for (std::uint32_t binding = 0; binding < buffers.size(); ++binding) {
                writes[binding].dstSet = *set;
                writes[binding].dstBinding = binding;
                writes[binding].descriptorType = binding == 0
                                                     ? vk::DescriptorType::eUniformBuffer
                                                     : vk::DescriptorType::eStorageBuffer;
                writes[binding].setBufferInfo(buffers[binding]);
            }

            vk::DescriptorImageInfo hiz_info{};
            hiz_info.sampler = *hiz_sampler_;
            hiz_info.imageView = *hiz_view_;
            hiz_info.imageLayout = vk::ImageLayout::eGeneral;
            writes[6].dstSet = *set;
            writes[6].dstBinding = 6;
            writes[6].descriptorType = vk::DescriptorType::eCombinedImageSampler;
            writes[6].setImageInfo(hiz_info);

            device.handle().updateDescriptorSets(writes, nullptr);
        }
    }
}

void GpuCulling::setClusters(const VulkanDevice& device, std::uint32_t frame_index,
                             const std::vector<GpuCluster>& clusters, std::uint32_t group_count,
                             std::uint32_t slot_count,
                             const std::vector<VulkanBuffer>& camera_buffers) {
    const auto count = static_cast<std::uint32_t>(clusters.size());
    if (count > cluster_capacity_ || group_count > group_capacity_ ||
        slot_count > slot_capacity_) {
        // Pasa al cargar el escenario: la GPU puede estar usando los buffers
        // viejos en otro frame en vuelo.
        device.waitIdle();
        createBuffers(device, std::max(count, 1u), std::max(group_count, 1u),
                      std::max(slot_count, 1u));
        writeCullSets(device, camera_buffers);
        std::cout << "[Vulkan] Culling en GPU: " << count << " clusteres, " << group_count
                  << " grupos de dibujo\n";
    }

    cluster_count_ = count;
    if (count > 0) {
        clusters_[frame_index].write(clusters.data(), sizeof(GpuCluster) * count);
    }
}

void GpuCulling::recordCull(const vk::raii::CommandBuffer& cmd, std::uint32_t frame_index,
                            std::uint32_t phase, bool occlusion) const {
    using Stage = vk::PipelineStageFlagBits2;
    using Access = vk::AccessFlagBits2;
    if (cluster_count_ == 0) {
        return;
    }

    if (phase == 0) {
        // El frame anterior leyo los comandos (dibujo indirecto) y escribio la
        // visibilidad y las estadisticas: se espera antes de reescribirlos.
        memoryBarrier(cmd, Stage::eDrawIndirect | Stage::eComputeShader,
                      Access::eIndirectCommandRead | Access::eShaderRead |
                          Access::eShaderWrite,
                      Stage::eTransfer | Stage::eComputeShader,
                      Access::eTransferWrite | Access::eShaderRead | Access::eShaderWrite);
        cmd.fillBuffer(*counts_[0].handle(), 0, VK_WHOLE_SIZE, 0);
        cmd.fillBuffer(*counts_[1].handle(), 0, VK_WHOLE_SIZE, 0);
        cmd.fillBuffer(*stats_[frame_index].handle(), 0, VK_WHOLE_SIZE, 0);
        memoryBarrier(cmd, Stage::eTransfer, Access::eTransferWrite, Stage::eComputeShader,
                      Access::eShaderRead | Access::eShaderWrite);
    } else {
        // La piramide recien construida.
        memoryBarrier(cmd, Stage::eComputeShader, Access::eShaderWrite, Stage::eComputeShader,
                      Access::eShaderRead | Access::eShaderWrite);
    }

    CullPush push{};
    push.cluster_count = cluster_count_;
    push.phase = phase;
    push.occlusion = occlusion ? 1u : 0u;

    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *cull_pass_.pipeline());
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *cull_pass_.layout(), 0,
                           *cull_sets_[frame_index * 2 + phase], nullptr);
    cmd.pushConstants<CullPush>(*cull_pass_.layout(), vk::ShaderStageFlagBits::eCompute, 0, push);
    cmd.dispatch((cluster_count_ + 63) / 64, 1, 1);

    // Los comandos y contadores pasan al dibujo indirecto; las estadisticas,
    // a la CPU.
    memoryBarrier(cmd, Stage::eComputeShader, Access::eShaderWrite,
                  Stage::eDrawIndirect | Stage::eComputeShader | Stage::eHost,
                  Access::eIndirectCommandRead | Access::eShaderRead | Access::eHostRead);
}

void GpuCulling::recordHiZ(const vk::raii::CommandBuffer& cmd) const {
    using Stage = vk::PipelineStageFlagBits2;
    using Access = vk::AccessFlagBits2;
    if (cluster_count_ == 0) {
        return;
    }

    // La piramide la leyo el culling (de este frame o del anterior).
    memoryBarrier(cmd, Stage::eComputeShader, Access::eShaderRead, Stage::eComputeShader,
                  Access::eShaderWrite);

    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *hiz_pass_.pipeline());
    for (std::uint32_t level = 0; level < hiz_levels_; ++level) {
        const std::uint32_t width = std::max(hiz_extent_.width >> level, 1u);
        const std::uint32_t height = std::max(hiz_extent_.height >> level, 1u);

        HizPush push{};
        push.level = level;
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *hiz_pass_.layout(), 0,
                               *hiz_sets_[level], nullptr);
        cmd.pushConstants<HizPush>(*hiz_pass_.layout(), vk::ShaderStageFlagBits::eCompute, 0,
                                   push);
        cmd.dispatch((width + 7) / 8, (height + 7) / 8, 1);

        // El siguiente nivel lee este.
        memoryBarrier(cmd, Stage::eComputeShader, Access::eShaderWrite, Stage::eComputeShader,
                      Access::eShaderRead);
    }
}

GpuCulling::Stats GpuCulling::readStats(std::uint32_t frame_index) const {
    Stats stats{};
    if (stats_[frame_index].mapped() == nullptr) {
        return stats;
    }
    std::uint32_t values[kStatsCount] = {};
    std::memcpy(values, stats_[frame_index].mapped(), sizeof(values));
    stats.early = values[0];
    stats.late = values[1];
    stats.occluded = values[2];
    stats.outside = values[3];
    return stats;
}

}  // namespace cramion::gfx
