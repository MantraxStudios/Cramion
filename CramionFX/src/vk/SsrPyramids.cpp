#include "CramionFX/vk/VulkanCompat.h"
#include "CramionFX/vk/SsrPyramids.h"

#include "CramionFX/vk/VulkanDevice.h"
#include "CramionFX/vk/VulkanImage.h"

#include <algorithm>
#include <array>
#include <bit>

namespace cramion::gfx {
namespace {

// Debe coincidir con el bloque de push constants de ssr_hiz.comp.
struct DepthPyramidPush {
    std::uint32_t level = 0;
};

// Niveles como mucho: la profundidad baja hasta 1 x 1 en cualquier pantalla
// razonable; la historia se queda en ~1/256 del lado (los conos mas anchos de
// ssr.frag no piden mas).
constexpr std::uint32_t kMaxDepthLevels = 13;
constexpr std::uint32_t kMaxColorLevels = 9;

constexpr vk::Format kDepthFormat = vk::Format::eR32Sfloat;
constexpr vk::Format kColorFormat = vk::Format::eR16G16B16A16Sfloat;

vk::ImageMemoryBarrier2 imageBarrier(vk::Image image, std::uint32_t base_mip, std::uint32_t mips,
                                     vk::ImageLayout old_layout, vk::ImageLayout new_layout,
                                     vk::PipelineStageFlags2 src_stage, vk::AccessFlags2 src_access,
                                     vk::PipelineStageFlags2 dst_stage, vk::AccessFlags2 dst_access) {
    vk::ImageMemoryBarrier2 barrier{};
    barrier.srcStageMask = src_stage;
    barrier.srcAccessMask = src_access;
    barrier.dstStageMask = dst_stage;
    barrier.dstAccessMask = dst_access;
    barrier.oldLayout = old_layout;
    barrier.newLayout = new_layout;
    barrier.image = image;
    barrier.subresourceRange = vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, base_mip, mips, 0, 1};
    return barrier;
}

void pipelineBarrier(const vk::raii::CommandBuffer& cmd, vk::ArrayProxy<const vk::ImageMemoryBarrier2> barriers) {
    vk::DependencyInfo dependency{};
    dependency.imageMemoryBarrierCount = barriers.size();
    dependency.pImageMemoryBarriers = barriers.data();
    compat::pipelineBarrier(cmd, dependency);
}

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

vk::Extent2D levelExtent(vk::Extent2D extent, std::uint32_t level) {
    return vk::Extent2D{std::max(extent.width >> level, 1u), std::max(extent.height >> level, 1u)};
}

}  // namespace

void SsrPyramids::create(const VulkanDevice& device) {
    destroy();

    using Type = vk::DescriptorType;
    const std::array<Type, 3> bindings = {Type::eCombinedImageSampler, Type::eStorageImage, Type::eStorageImage};
    ComputePassDesc desc{};
    desc.shader = "ssr_hiz.comp.spv";
    desc.bindings = bindings;
    desc.push_constant_size = sizeof(DepthPyramidPush);
    depth_pass_.create(device, desc);

    const std::array<vk::DescriptorPoolSize, 2> sizes = {
        vk::DescriptorPoolSize{Type::eCombinedImageSampler, kMaxDepthLevels},
        vk::DescriptorPoolSize{Type::eStorageImage, kMaxDepthLevels * 2}};
    vk::DescriptorPoolCreateInfo pool_info{};
    pool_info.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    pool_info.maxSets = kMaxDepthLevels;
    pool_info.setPoolSizes(sizes);
    pool_ = vk::raii::DescriptorPool(device.handle(), pool_info);

    // La historia: trilineal con todos sus mips (el mip sale del cono de la
    // rugosidad, fraccionario).
    vk::SamplerCreateInfo color_info{};
    color_info.magFilter = vk::Filter::eLinear;
    color_info.minFilter = vk::Filter::eLinear;
    color_info.mipmapMode = vk::SamplerMipmapMode::eLinear;
    color_info.addressModeU = vk::SamplerAddressMode::eClampToEdge;
    color_info.addressModeV = vk::SamplerAddressMode::eClampToEdge;
    color_info.addressModeW = vk::SamplerAddressMode::eClampToEdge;
    color_info.maxLod = VK_LOD_CLAMP_NONE;
    color_sampler_ = vk::raii::Sampler(device.handle(), color_info);

    // La profundidad se lee con texelFetch (el sampler no filtra nada).
    vk::SamplerCreateInfo depth_info = color_info;
    depth_info.magFilter = vk::Filter::eNearest;
    depth_info.minFilter = vk::Filter::eNearest;
    depth_info.mipmapMode = vk::SamplerMipmapMode::eNearest;
    depth_sampler_ = vk::raii::Sampler(device.handle(), depth_info);
}

void SsrPyramids::releaseImages() {
    depth_sets_.clear();
    depth_level_views_.clear();
    depth_view_ = nullptr;
    depth_image_ = nullptr;
    depth_memory_ = nullptr;
    depth_ticket_.release();
    depth_levels_ = 0;

    color_view_ = nullptr;
    color_image_ = nullptr;
    color_memory_ = nullptr;
    color_ticket_.release();
    color_levels_ = 0;
    extent_ = vk::Extent2D{0, 0};
}

void SsrPyramids::resize(const VulkanDevice& device, const VulkanImage& depth) {
    if (*pool_ == nullptr) return;
    releaseImages();

    extent_ = depth.extent();
    const std::uint32_t full_levels = std::bit_width(std::max(extent_.width, extent_.height));
    depth_levels_ = std::min(full_levels, kMaxDepthLevels);
    color_levels_ = std::min(full_levels, kMaxColorLevels);

    const auto allocate = [&](const vk::raii::Image& image, vk::raii::DeviceMemory& memory, GpuMemoryTicket& ticket) {
        const vk::MemoryRequirements requirements = image.getMemoryRequirements();
        vk::MemoryAllocateInfo allocate_info{};
        allocate_info.allocationSize = requirements.size;
        allocate_info.memoryTypeIndex =
            device.findMemoryType(requirements.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal);
        memory = vk::raii::DeviceMemory(device.handle(), allocate_info);
        ticket = GpuMemoryTicket(GpuMemoryKind::Image, requirements.size);
        image.bindMemory(*memory, 0);
    };

    // --- Profundidad minima: el nivel 0 del tamano del depth buffer ---
    vk::ImageCreateInfo depth_info{};
    depth_info.imageType = vk::ImageType::e2D;
    depth_info.format = kDepthFormat;
    depth_info.extent = vk::Extent3D{extent_.width, extent_.height, 1};
    depth_info.mipLevels = depth_levels_;
    depth_info.arrayLayers = 1;
    depth_info.samples = vk::SampleCountFlagBits::e1;
    depth_info.tiling = vk::ImageTiling::eOptimal;
    depth_info.usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
                       vk::ImageUsageFlagBits::eTransferDst;
    depth_info.initialLayout = vk::ImageLayout::eUndefined;
    depth_image_ = vk::raii::Image(device.handle(), depth_info);
    allocate(depth_image_, depth_memory_, depth_ticket_);

    vk::ImageViewCreateInfo depth_view_info{};
    depth_view_info.image = *depth_image_;
    depth_view_info.viewType = vk::ImageViewType::e2D;
    depth_view_info.format = kDepthFormat;
    depth_view_info.subresourceRange = vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, depth_levels_, 0, 1};
    depth_view_ = vk::raii::ImageView(device.handle(), depth_view_info);
    for (std::uint32_t level = 0; level < depth_levels_; ++level) {
        depth_view_info.subresourceRange.baseMipLevel = level;
        depth_view_info.subresourceRange.levelCount = 1;
        depth_level_views_.emplace_back(device.handle(), depth_view_info);
    }

    // --- Historia: la imagen HDR con mips (copia y blits) ---
    vk::ImageCreateInfo color_info = depth_info;
    color_info.format = kColorFormat;
    color_info.mipLevels = color_levels_;
    color_info.usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferSrc |
                       vk::ImageUsageFlagBits::eTransferDst;
    color_image_ = vk::raii::Image(device.handle(), color_info);
    allocate(color_image_, color_memory_, color_ticket_);

    vk::ImageViewCreateInfo color_view_info{};
    color_view_info.image = *color_image_;
    color_view_info.viewType = vk::ImageViewType::e2D;
    color_view_info.format = kColorFormat;
    color_view_info.subresourceRange = vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, color_levels_, 0, 1};
    color_view_ = vk::raii::ImageView(device.handle(), color_view_info);

    // Antes de la primera vez: la profundidad "todo lejos" en General y la
    // historia en negro como textura (los sets las enlazan desde ya).
    const vk::Image depth_handle = *depth_image_;
    const vk::Image color_handle = *color_image_;
    const std::uint32_t depth_levels = depth_levels_;
    const std::uint32_t color_levels = color_levels_;
    device.submitOneTime([&](const vk::raii::CommandBuffer& cmd) {
        using Stage = vk::PipelineStageFlagBits2;
        using Access = vk::AccessFlagBits2;
        pipelineBarrier(cmd, {imageBarrier(depth_handle, 0, depth_levels, vk::ImageLayout::eUndefined,
                                           vk::ImageLayout::eGeneral, Stage::eNone, Access::eNone, Stage::eTransfer,
                                           Access::eTransferWrite),
                              imageBarrier(color_handle, 0, color_levels, vk::ImageLayout::eUndefined,
                                           vk::ImageLayout::eTransferDstOptimal, Stage::eNone, Access::eNone,
                                           Stage::eTransfer, Access::eTransferWrite)});
        cmd.clearColorImage(depth_handle, vk::ImageLayout::eGeneral, vk::ClearColorValue{1.0f, 1.0f, 1.0f, 1.0f},
                            vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, depth_levels, 0, 1});
        cmd.clearColorImage(color_handle, vk::ImageLayout::eTransferDstOptimal,
                            vk::ClearColorValue{0.0f, 0.0f, 0.0f, 0.0f},
                            vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, color_levels, 0, 1});
        pipelineBarrier(cmd, {imageBarrier(depth_handle, 0, depth_levels, vk::ImageLayout::eGeneral,
                                           vk::ImageLayout::eGeneral, Stage::eTransfer, Access::eTransferWrite,
                                           Stage::eAllCommands, Access::eShaderRead | Access::eShaderWrite),
                              imageBarrier(color_handle, 0, color_levels, vk::ImageLayout::eTransferDstOptimal,
                                           vk::ImageLayout::eShaderReadOnlyOptimal, Stage::eTransfer,
                                           Access::eTransferWrite, Stage::eAllCommands, Access::eShaderSampledRead)});
    });

    // --- Sets de la piramide: depth + nivel anterior + este nivel ---
    const std::vector<vk::DescriptorSetLayout> layouts(depth_levels_, *depth_pass_.descriptorSetLayout());
    vk::DescriptorSetAllocateInfo alloc{};
    alloc.descriptorPool = *pool_;
    alloc.setSetLayouts(layouts);
    depth_sets_ = vk::raii::DescriptorSets(device.handle(), alloc);
    for (std::uint32_t level = 0; level < depth_levels_; ++level) {
        vk::DescriptorImageInfo depth_buffer{};
        depth_buffer.sampler = *depth_sampler_;
        depth_buffer.imageView = *depth.view();
        depth_buffer.imageLayout = compat::depthReadOnlyLayout();

        // El nivel 0 copia el depth: su "nivel anterior" es el mismo, sin usarlo.
        vk::DescriptorImageInfo source{};
        source.imageView = *depth_level_views_[level == 0 ? 0 : level - 1];
        source.imageLayout = vk::ImageLayout::eGeneral;
        vk::DescriptorImageInfo target{};
        target.imageView = *depth_level_views_[level];
        target.imageLayout = vk::ImageLayout::eGeneral;

        std::array<vk::WriteDescriptorSet, 3> writes{};
        writes[0].dstSet = *depth_sets_[level];
        writes[0].dstBinding = 0;
        writes[0].descriptorType = vk::DescriptorType::eCombinedImageSampler;
        writes[0].setImageInfo(depth_buffer);
        writes[1].dstSet = *depth_sets_[level];
        writes[1].dstBinding = 1;
        writes[1].descriptorType = vk::DescriptorType::eStorageImage;
        writes[1].setImageInfo(source);
        writes[2].dstSet = *depth_sets_[level];
        writes[2].dstBinding = 2;
        writes[2].descriptorType = vk::DescriptorType::eStorageImage;
        writes[2].setImageInfo(target);
        device.handle().updateDescriptorSets(writes, nullptr);
    }
}

void SsrPyramids::destroy() {
    releaseImages();
    depth_pass_.destroy();
    depth_sampler_ = nullptr;
    color_sampler_ = nullptr;
    pool_ = nullptr;
}

void SsrPyramids::recordDepthPyramid(const vk::raii::CommandBuffer& cmd) const {
    if (!isValid()) return;
    using Stage = vk::PipelineStageFlagBits2;
    using Access = vk::AccessFlagBits2;

    // El depth de esta geometria (ya en solo lectura) para el compute, y la
    // piramide que leyo el SSR del frame anterior, para reescribirla.
    memoryBarrier(cmd,
                  Stage::eLateFragmentTests | Stage::eEarlyFragmentTests | Stage::eFragmentShader |
                      Stage::eComputeShader,
                  Access::eDepthStencilAttachmentWrite | Access::eShaderSampledRead | Access::eShaderStorageRead,
                  Stage::eComputeShader, Access::eShaderSampledRead | Access::eShaderStorageWrite);

    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *depth_pass_.pipeline());
    for (std::uint32_t level = 0; level < depth_levels_; ++level) {
        const vk::Extent2D size = levelExtent(extent_, level);
        DepthPyramidPush push{};
        push.level = level;
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *depth_pass_.layout(), 0, *depth_sets_[level],
                               nullptr);
        cmd.pushConstants<DepthPyramidPush>(*depth_pass_.layout(), vk::ShaderStageFlagBits::eCompute, 0, push);
        cmd.dispatch((size.width + 7) / 8, (size.height + 7) / 8, 1);
        // El siguiente nivel lee este.
        memoryBarrier(cmd, Stage::eComputeShader, Access::eShaderStorageWrite, Stage::eComputeShader,
                      Access::eShaderStorageRead);
    }
    // ssr.frag la recorre con texelFetch.
    memoryBarrier(cmd, Stage::eComputeShader, Access::eShaderStorageWrite, Stage::eFragmentShader,
                  Access::eShaderSampledRead);
}

void SsrPyramids::recordColorPyramid(const vk::raii::CommandBuffer& cmd, vk::Image scene, bool with_mips) const {
    if (!isValid()) return;
    using Stage = vk::PipelineStageFlagBits2;
    using Access = vk::AccessFlagBits2;
    const vk::Image history = *color_image_;
    // Niveles que se reescriben: sin mips, solo el 0 (los demas se quedan
    // como estaban, como textura).
    const std::uint32_t levels = with_mips ? color_levels_ : 1u;

    // 1) La imagen de este frame a origen de copia. Lo que se reescribe de la
    //    historia (lo de antes no importa), cuando terminen de leerla el SSR,
    //    la GI y los rayos de este frame.
    pipelineBarrier(cmd, {imageBarrier(scene, 0, 1, vk::ImageLayout::eColorAttachmentOptimal,
                                       vk::ImageLayout::eTransferSrcOptimal, Stage::eColorAttachmentOutput,
                                       Access::eColorAttachmentWrite, Stage::eTransfer, Access::eTransferRead),
                          imageBarrier(history, 0, levels, vk::ImageLayout::eUndefined,
                                       vk::ImageLayout::eTransferDstOptimal, Stage::eAllCommands, Access::eNone,
                                       Stage::eTransfer, Access::eTransferWrite)});
    vk::ImageCopy region{};
    region.srcSubresource = vk::ImageSubresourceLayers{vk::ImageAspectFlagBits::eColor, 0, 0, 1};
    region.dstSubresource = region.srcSubresource;
    region.extent = vk::Extent3D{extent_.width, extent_.height, 1};
    cmd.copyImage(scene, vk::ImageLayout::eTransferSrcOptimal, history, vk::ImageLayout::eTransferDstOptimal, region);

    // 2) La escena vuelve a destino de color: la escalan o la componen despues.
    pipelineBarrier(cmd, imageBarrier(scene, 0, 1, vk::ImageLayout::eTransferSrcOptimal,
                                      vk::ImageLayout::eColorAttachmentOptimal, Stage::eTransfer,
                                      Access::eTransferRead, Stage::eColorAttachmentOutput,
                                      Access::eColorAttachmentRead | Access::eColorAttachmentWrite));

    // 3) Cada mip a partir del anterior (filtro lineal a la mitad: media 2x2).
    for (std::uint32_t level = 1; level < levels; ++level) {
        pipelineBarrier(cmd, imageBarrier(history, level - 1, 1, vk::ImageLayout::eTransferDstOptimal,
                                          vk::ImageLayout::eTransferSrcOptimal, Stage::eTransfer,
                                          Access::eTransferWrite, Stage::eTransfer, Access::eTransferRead));
        const vk::Extent2D source = levelExtent(extent_, level - 1);
        const vk::Extent2D target = levelExtent(extent_, level);
        vk::ImageBlit blit{};
        blit.srcSubresource = vk::ImageSubresourceLayers{vk::ImageAspectFlagBits::eColor, level - 1, 0, 1};
        blit.srcOffsets[1] = vk::Offset3D{static_cast<std::int32_t>(source.width),
                                          static_cast<std::int32_t>(source.height), 1};
        blit.dstSubresource = vk::ImageSubresourceLayers{vk::ImageAspectFlagBits::eColor, level, 0, 1};
        blit.dstOffsets[1] = vk::Offset3D{static_cast<std::int32_t>(target.width),
                                          static_cast<std::int32_t>(target.height), 1};
        cmd.blitImage(history, vk::ImageLayout::eTransferSrcOptimal, history, vk::ImageLayout::eTransferDstOptimal,
                      blit, vk::Filter::eLinear);
    }

    // 4) Lo reescrito a textura para el frame siguiente (fragment, compute y
    //    rayos: de ahi todas las etapas).
    std::vector<vk::ImageMemoryBarrier2> to_read;
    if (levels > 1) {
        to_read.push_back(imageBarrier(history, 0, levels - 1, vk::ImageLayout::eTransferSrcOptimal,
                                       vk::ImageLayout::eShaderReadOnlyOptimal, Stage::eTransfer,
                                       Access::eTransferRead, Stage::eAllCommands, Access::eShaderSampledRead));
    }
    to_read.push_back(imageBarrier(history, levels - 1, 1, vk::ImageLayout::eTransferDstOptimal,
                                   vk::ImageLayout::eShaderReadOnlyOptimal, Stage::eTransfer, Access::eTransferWrite,
                                   Stage::eAllCommands, Access::eShaderSampledRead));
    pipelineBarrier(cmd, to_read);
}

}  // namespace cramion::gfx
