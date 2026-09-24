#include "vk/ReflectionProbe.h"

#include "vk/VulkanDevice.h"

#include <algorithm>
#include <array>
#include <iostream>

namespace cramion::gfx {
namespace {

// Debe coincidir con el bloque de push constants de probe_prefilter.comp.
struct PrefilterPush {
    float roughness = 0.0f;
    std::uint32_t face_size = 0;
    std::uint32_t sample_count = 0;
    float capture_size = 0.0f;
};

constexpr std::uint32_t kPrefilterSamples = 64;

vk::ImageMemoryBarrier2 barrier(vk::Image image, std::uint32_t base_mip, std::uint32_t mips,
                                std::uint32_t base_layer, std::uint32_t layers,
                                vk::ImageLayout old_layout, vk::ImageLayout new_layout,
                                vk::PipelineStageFlags2 src_stage, vk::AccessFlags2 src_access,
                                vk::PipelineStageFlags2 dst_stage, vk::AccessFlags2 dst_access) {
    vk::ImageMemoryBarrier2 result{};
    result.srcStageMask = src_stage;
    result.srcAccessMask = src_access;
    result.dstStageMask = dst_stage;
    result.dstAccessMask = dst_access;
    result.oldLayout = old_layout;
    result.newLayout = new_layout;
    result.image = image;
    result.subresourceRange = vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, base_mip,
                                                        mips, base_layer, layers};
    return result;
}

void pipelineBarrier(const vk::raii::CommandBuffer& cmd,
                     vk::ArrayProxy<const vk::ImageMemoryBarrier2> barriers) {
    vk::DependencyInfo dependency{};
    dependency.imageMemoryBarrierCount = barriers.size();
    dependency.pImageMemoryBarriers = barriers.data();
    cmd.pipelineBarrier2(dependency);
}

}  // namespace

ReflectionProbe::Image ReflectionProbe::createImage(const VulkanDevice& device,
                                                    const vk::ImageCreateInfo& info) {
    Image result;
    result.image = vk::raii::Image(device.handle(), info);

    const vk::MemoryRequirements requirements = result.image.getMemoryRequirements();
    vk::MemoryAllocateInfo allocate_info{};
    allocate_info.allocationSize = requirements.size;
    allocate_info.memoryTypeIndex = device.findMemoryType(
        requirements.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal);
    result.memory = vk::raii::DeviceMemory(device.handle(), allocate_info);
    result.image.bindMemory(*result.memory, 0);
    return result;
}

void ReflectionProbe::create(const VulkanDevice& device) {
    destroy();

    // --- Captura: cubo con todos los mips (destino y origen de copias) ---
    vk::ImageCreateInfo capture_info{};
    capture_info.flags = vk::ImageCreateFlagBits::eCubeCompatible;
    capture_info.imageType = vk::ImageType::e2D;
    capture_info.format = kFormat;
    capture_info.extent = vk::Extent3D{kCaptureSize, kCaptureSize, 1};
    capture_info.mipLevels = kCaptureMips;
    capture_info.arrayLayers = 6;
    capture_info.samples = vk::SampleCountFlagBits::e1;
    capture_info.tiling = vk::ImageTiling::eOptimal;
    capture_info.usage = vk::ImageUsageFlagBits::eTransferDst |
                         vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eSampled;
    capture_info.initialLayout = vk::ImageLayout::eUndefined;
    capture_ = createImage(device, capture_info);

    vk::ImageViewCreateInfo capture_view{};
    capture_view.image = *capture_.image;
    capture_view.viewType = vk::ImageViewType::eCube;
    capture_view.format = kFormat;
    capture_view.subresourceRange =
        vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, kCaptureMips, 0, 6};
    capture_view_ = vk::raii::ImageView(device.handle(), capture_view);

    // --- Cubo prefiltrado ---
    vk::ImageCreateInfo probe_info = capture_info;
    probe_info.extent = vk::Extent3D{kSize, kSize, 1};
    probe_info.mipLevels = kMips;
    probe_info.usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
                       vk::ImageUsageFlagBits::eTransferDst;
    probe_ = createImage(device, probe_info);

    vk::ImageViewCreateInfo probe_view = capture_view;
    probe_view.image = *probe_.image;
    probe_view.subresourceRange.levelCount = kMips;
    probe_view_ = vk::raii::ImageView(device.handle(), probe_view);

    for (std::uint32_t mip = 0; mip < kMips; ++mip) {
        vk::ImageViewCreateInfo mip_view{};
        mip_view.image = *probe_.image;
        mip_view.viewType = vk::ImageViewType::e2DArray;
        mip_view.format = kFormat;
        mip_view.subresourceRange =
            vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, mip, 1, 0, 6};
        probe_mip_views_.emplace_back(device.handle(), mip_view);
    }

    // Trilineal con todos los mips: la rugosidad (iluminacion) o el angulo
    // solido de cada muestra (prefiltrado) eligen un mip fraccionario.
    vk::SamplerCreateInfo sampler_info{};
    sampler_info.magFilter = vk::Filter::eLinear;
    sampler_info.minFilter = vk::Filter::eLinear;
    sampler_info.mipmapMode = vk::SamplerMipmapMode::eLinear;
    sampler_info.addressModeU = vk::SamplerAddressMode::eClampToEdge;
    sampler_info.addressModeV = vk::SamplerAddressMode::eClampToEdge;
    sampler_info.addressModeW = vk::SamplerAddressMode::eClampToEdge;
    sampler_info.maxLod = static_cast<float>(kCaptureMips);
    sampler_ = vk::raii::Sampler(device.handle(), sampler_info);

    // --- Prefiltrado ---
    using Type = vk::DescriptorType;
    const std::array<Type, 2> bindings = {Type::eCombinedImageSampler, Type::eStorageImage};
    ComputePassDesc prefilter{};
    prefilter.shader = "probe_prefilter.comp.spv";
    prefilter.bindings = bindings;
    prefilter.push_constant_size = sizeof(PrefilterPush);
    prefilter_pass_.create(device, prefilter);

    const std::array<vk::DescriptorPoolSize, 2> pool_sizes = {
        vk::DescriptorPoolSize{Type::eCombinedImageSampler, kMips},
        vk::DescriptorPoolSize{Type::eStorageImage, kMips}};
    vk::DescriptorPoolCreateInfo pool_info{};
    pool_info.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    pool_info.maxSets = kMips;
    pool_info.setPoolSizes(pool_sizes);
    pool_ = vk::raii::DescriptorPool(device.handle(), pool_info);

    const std::vector<vk::DescriptorSetLayout> layouts(kMips,
                                                       *prefilter_pass_.descriptorSetLayout());
    vk::DescriptorSetAllocateInfo alloc{};
    alloc.descriptorPool = *pool_;
    alloc.setSetLayouts(layouts);
    prefilter_sets_ = vk::raii::DescriptorSets(device.handle(), alloc);

    for (std::uint32_t mip = 0; mip < kMips; ++mip) {
        vk::DescriptorImageInfo capture_image{};
        capture_image.sampler = *sampler_;
        capture_image.imageView = *capture_view_;
        capture_image.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;

        vk::DescriptorImageInfo storage{};
        storage.imageView = *probe_mip_views_[mip];
        storage.imageLayout = vk::ImageLayout::eGeneral;

        std::array<vk::WriteDescriptorSet, 2> writes{};
        writes[0].dstSet = *prefilter_sets_[mip];
        writes[0].dstBinding = 0;
        writes[0].descriptorType = Type::eCombinedImageSampler;
        writes[0].setImageInfo(capture_image);
        writes[1].dstSet = *prefilter_sets_[mip];
        writes[1].dstBinding = 1;
        writes[1].descriptorType = Type::eStorageImage;
        writes[1].setImageInfo(storage);
        device.handle().updateDescriptorSets(writes, nullptr);
    }

    // El cubo se enlaza a la iluminacion desde el principio, antes de la
    // primera captura (que no lo usara hasta estar lista): en negro y como
    // textura.
    const vk::Image probe_image = *probe_.image;
    device.submitOneTime([&](const vk::raii::CommandBuffer& cmd) {
        using Stage = vk::PipelineStageFlagBits2;
        using Access = vk::AccessFlagBits2;
        pipelineBarrier(cmd, barrier(probe_image, 0, kMips, 0, 6, vk::ImageLayout::eUndefined,
                                     vk::ImageLayout::eTransferDstOptimal, Stage::eNone,
                                     Access::eNone, Stage::eTransfer, Access::eTransferWrite));
        const vk::ClearColorValue black{0.0f, 0.0f, 0.0f, 0.0f};
        const vk::ImageSubresourceRange all{vk::ImageAspectFlagBits::eColor, 0, kMips, 0, 6};
        cmd.clearColorImage(probe_image, vk::ImageLayout::eTransferDstOptimal, black, all);
        pipelineBarrier(cmd, barrier(probe_image, 0, kMips, 0, 6,
                                     vk::ImageLayout::eTransferDstOptimal,
                                     vk::ImageLayout::eShaderReadOnlyOptimal, Stage::eTransfer,
                                     Access::eTransferWrite, Stage::eFragmentShader,
                                     Access::eShaderSampledRead));
    });

    std::cout << "[Vulkan] Sonda de reflexion lista: captura de " << kCaptureSize
              << " px, cubo de " << kSize << " px con " << kMips << " niveles\n";
}

void ReflectionProbe::destroy() {
    prefilter_sets_.clear();
    pool_ = nullptr;
    prefilter_pass_.destroy();
    sampler_ = nullptr;
    probe_mip_views_.clear();
    probe_view_ = nullptr;
    probe_ = Image{};
    capture_view_ = nullptr;
    capture_ = Image{};
}

void ReflectionProbe::recordFaceCopy(const vk::raii::CommandBuffer& cmd, vk::Image source,
                                     const vk::Rect2D& region, std::uint32_t face) const {
    using Stage = vk::PipelineStageFlagBits2;
    using Access = vk::AccessFlagBits2;
    const vk::Image capture = *capture_.image;

    // La cara se reescribe entera; la leyo el ultimo prefiltrado.
    pipelineBarrier(cmd, barrier(capture, 0, 1, face, 1, vk::ImageLayout::eUndefined,
                                 vk::ImageLayout::eTransferDstOptimal, Stage::eComputeShader,
                                 Access::eShaderSampledRead, Stage::eBlit,
                                 Access::eTransferWrite));

    // X invertida: la base de la vista (derecha = forward x arriba) es la
    // contraria a la de las caras de un cubo de Vulkan.
    const auto x0 = region.offset.x;
    const auto y0 = region.offset.y;
    const auto x1 = x0 + static_cast<std::int32_t>(region.extent.width);
    const auto y1 = y0 + static_cast<std::int32_t>(region.extent.height);

    vk::ImageBlit blit{};
    blit.srcSubresource = vk::ImageSubresourceLayers{vk::ImageAspectFlagBits::eColor, 0, 0, 1};
    blit.srcOffsets[0] = vk::Offset3D{x1, y0, 0};
    blit.srcOffsets[1] = vk::Offset3D{x0, y1, 1};
    blit.dstSubresource = vk::ImageSubresourceLayers{vk::ImageAspectFlagBits::eColor, 0, face, 1};
    blit.dstOffsets[0] = vk::Offset3D{0, 0, 0};
    blit.dstOffsets[1] = vk::Offset3D{static_cast<std::int32_t>(kCaptureSize),
                                      static_cast<std::int32_t>(kCaptureSize), 1};
    cmd.blitImage(source, vk::ImageLayout::eTransferSrcOptimal, capture,
                  vk::ImageLayout::eTransferDstOptimal, blit, vk::Filter::eLinear);
}

void ReflectionProbe::recordPrefilter(const vk::raii::CommandBuffer& cmd) const {
    using Stage = vk::PipelineStageFlagBits2;
    using Access = vk::AccessFlagBits2;
    const vk::Image capture = *capture_.image;
    const vk::Image probe = *probe_.image;

    // --- Mips de la captura, cada uno a partir del anterior ---
    pipelineBarrier(cmd, barrier(capture, 0, 1, 0, 6, vk::ImageLayout::eTransferDstOptimal,
                                 vk::ImageLayout::eTransferSrcOptimal, Stage::eBlit,
                                 Access::eTransferWrite, Stage::eBlit, Access::eTransferRead));
    for (std::uint32_t mip = 1; mip < kCaptureMips; ++mip) {
        pipelineBarrier(cmd, barrier(capture, mip, 1, 0, 6, vk::ImageLayout::eUndefined,
                                     vk::ImageLayout::eTransferDstOptimal, Stage::eComputeShader,
                                     Access::eShaderSampledRead, Stage::eBlit,
                                     Access::eTransferWrite));

        const auto source_size = static_cast<std::int32_t>(std::max(kCaptureSize >> (mip - 1), 1u));
        const auto target_size = static_cast<std::int32_t>(std::max(kCaptureSize >> mip, 1u));
        vk::ImageBlit blit{};
        blit.srcSubresource =
            vk::ImageSubresourceLayers{vk::ImageAspectFlagBits::eColor, mip - 1, 0, 6};
        blit.srcOffsets[1] = vk::Offset3D{source_size, source_size, 1};
        blit.dstSubresource = vk::ImageSubresourceLayers{vk::ImageAspectFlagBits::eColor, mip, 0, 6};
        blit.dstOffsets[1] = vk::Offset3D{target_size, target_size, 1};
        cmd.blitImage(capture, vk::ImageLayout::eTransferSrcOptimal, capture,
                      vk::ImageLayout::eTransferDstOptimal, blit, vk::Filter::eLinear);

        pipelineBarrier(cmd, barrier(capture, mip, 1, 0, 6, vk::ImageLayout::eTransferDstOptimal,
                                     vk::ImageLayout::eTransferSrcOptimal, Stage::eBlit,
                                     Access::eTransferWrite, Stage::eBlit,
                                     Access::eTransferRead));
    }

    // --- La captura pasa a textura y el cubo a destino del compute ---
    pipelineBarrier(
        cmd, {barrier(capture, 0, kCaptureMips, 0, 6, vk::ImageLayout::eTransferSrcOptimal,
                      vk::ImageLayout::eShaderReadOnlyOptimal, Stage::eBlit,
                      Access::eTransferRead | Access::eTransferWrite, Stage::eComputeShader,
                      Access::eShaderSampledRead),
              barrier(probe, 0, kMips, 0, 6, vk::ImageLayout::eUndefined,
                      vk::ImageLayout::eGeneral, Stage::eFragmentShader,
                      Access::eShaderSampledRead, Stage::eComputeShader,
                      Access::eShaderStorageWrite)});

    PrefilterPush push{};
    push.sample_count = kPrefilterSamples;
    push.capture_size = static_cast<float>(kCaptureSize);

    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *prefilter_pass_.pipeline());
    for (std::uint32_t mip = 0; mip < kMips; ++mip) {
        const std::uint32_t size = std::max(kSize >> mip, 1u);
        push.roughness = static_cast<float>(mip) / static_cast<float>(kMips - 1);
        push.face_size = size;

        cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *prefilter_pass_.layout(), 0,
                               *prefilter_sets_[mip], nullptr);
        cmd.pushConstants<PrefilterPush>(*prefilter_pass_.layout(),
                                         vk::ShaderStageFlagBits::eCompute, 0, push);
        cmd.dispatch((size + 7) / 8, (size + 7) / 8, 6);
    }

    // --- Listo para la iluminacion ---
    pipelineBarrier(cmd, barrier(probe, 0, kMips, 0, 6, vk::ImageLayout::eGeneral,
                                 vk::ImageLayout::eShaderReadOnlyOptimal, Stage::eComputeShader,
                                 Access::eShaderStorageWrite, Stage::eFragmentShader,
                                 Access::eShaderSampledRead));
}

}  // namespace cramion::gfx
