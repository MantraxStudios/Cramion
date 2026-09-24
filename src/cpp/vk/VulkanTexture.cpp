#include "vk/VulkanTexture.h"

#include "vk/VulkanBuffer.h"
#include "vk/VulkanDevice.h"

#include <algorithm>
#include <bit>
#include <stdexcept>
#include <vector>

namespace cramion::gfx {

namespace {

constexpr vk::Format kTextureFormat = vk::Format::eR8G8B8A8Unorm;

vk::ImageMemoryBarrier2 mipBarrier(vk::Image image, std::uint32_t level,
                                   vk::ImageLayout old_layout, vk::ImageLayout new_layout,
                                   vk::PipelineStageFlags2 src_stage, vk::AccessFlags2 src_access,
                                   vk::PipelineStageFlags2 dst_stage,
                                   vk::AccessFlags2 dst_access) {
    vk::ImageMemoryBarrier2 barrier{};
    barrier.srcStageMask = src_stage;
    barrier.srcAccessMask = src_access;
    barrier.dstStageMask = dst_stage;
    barrier.dstAccessMask = dst_access;
    barrier.oldLayout = old_layout;
    barrier.newLayout = new_layout;
    barrier.image = image;
    barrier.subresourceRange = vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, level, 1, 0, 1};
    return barrier;
}

void pipelineBarrier(const vk::raii::CommandBuffer& cmd, const vk::ImageMemoryBarrier2& barrier) {
    vk::DependencyInfo dependency{};
    dependency.setImageMemoryBarriers(barrier);
    cmd.pipelineBarrier2(dependency);
}

}  // namespace

void VulkanTexture::create(const VulkanDevice& device, std::uint32_t width, std::uint32_t height,
                           const std::uint8_t* rgba) {
    if (width == 0 || height == 0 || rgba == nullptr) {
        throw std::runtime_error("Textura vacia.");
    }

    destroy();

    mip_levels_ = static_cast<std::uint32_t>(std::bit_width(std::max(width, height)));

    // --- Imagen con todos los niveles ---
    vk::ImageCreateInfo image_info{};
    image_info.imageType = vk::ImageType::e2D;
    image_info.format = kTextureFormat;
    image_info.extent = vk::Extent3D{width, height, 1};
    image_info.mipLevels = mip_levels_;
    image_info.arrayLayers = 1;
    image_info.samples = vk::SampleCountFlagBits::e1;
    image_info.tiling = vk::ImageTiling::eOptimal;
    // TransferSrc ademas de Dst: cada nivel es origen del blit del siguiente.
    image_info.usage = vk::ImageUsageFlagBits::eTransferSrc |
                       vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled;
    image_info.sharingMode = vk::SharingMode::eExclusive;
    image_info.initialLayout = vk::ImageLayout::eUndefined;

    image_ = vk::raii::Image(device.handle(), image_info);

    const vk::MemoryRequirements requirements = image_.getMemoryRequirements();
    vk::MemoryAllocateInfo allocate_info{};
    allocate_info.allocationSize = requirements.size;
    allocate_info.memoryTypeIndex = device.findMemoryType(
        requirements.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal);
    memory_ = vk::raii::DeviceMemory(device.handle(), allocate_info);
    image_.bindMemory(*memory_, 0);

    // --- Subida del nivel 0 y generacion de los demas ---
    const vk::DeviceSize size = static_cast<vk::DeviceSize>(width) * height * 4;
    VulkanBuffer staging;
    staging.create(device, size, vk::BufferUsageFlagBits::eTransferSrc,
                   vk::MemoryPropertyFlagBits::eHostVisible |
                       vk::MemoryPropertyFlagBits::eHostCoherent);
    staging.write(rgba, size);

    const vk::Image image = *image_;
    const std::uint32_t levels = mip_levels_;

    device.submitOneTime([&](const vk::raii::CommandBuffer& cmd) {
        // Todos los niveles a destino de copia.
        vk::ImageMemoryBarrier2 to_transfer = mipBarrier(
            image, 0, vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
            vk::PipelineStageFlagBits2::eNone, vk::AccessFlagBits2::eNone,
            vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite);
        to_transfer.subresourceRange.levelCount = levels;
        pipelineBarrier(cmd, to_transfer);

        vk::BufferImageCopy region{};
        region.imageSubresource = vk::ImageSubresourceLayers{vk::ImageAspectFlagBits::eColor, 0, 0, 1};
        region.imageExtent = vk::Extent3D{width, height, 1};
        cmd.copyBufferToImage(*staging.handle(), image, vk::ImageLayout::eTransferDstOptimal, region);

        auto level_width = static_cast<std::int32_t>(width);
        auto level_height = static_cast<std::int32_t>(height);

        for (std::uint32_t level = 1; level < levels; ++level) {
            // El nivel anterior pasa a ser origen del blit.
            pipelineBarrier(cmd, mipBarrier(image, level - 1, vk::ImageLayout::eTransferDstOptimal,
                                            vk::ImageLayout::eTransferSrcOptimal,
                                            vk::PipelineStageFlagBits2::eTransfer,
                                            vk::AccessFlagBits2::eTransferWrite,
                                            vk::PipelineStageFlagBits2::eTransfer,
                                            vk::AccessFlagBits2::eTransferRead));

            const std::int32_t next_width = std::max(level_width / 2, 1);
            const std::int32_t next_height = std::max(level_height / 2, 1);

            vk::ImageBlit blit{};
            blit.srcSubresource = vk::ImageSubresourceLayers{vk::ImageAspectFlagBits::eColor, level - 1, 0, 1};
            blit.srcOffsets[1] = vk::Offset3D{level_width, level_height, 1};
            blit.dstSubresource = vk::ImageSubresourceLayers{vk::ImageAspectFlagBits::eColor, level, 0, 1};
            blit.dstOffsets[1] = vk::Offset3D{next_width, next_height, 1};
            cmd.blitImage(image, vk::ImageLayout::eTransferSrcOptimal, image,
                          vk::ImageLayout::eTransferDstOptimal, blit, vk::Filter::eLinear);

            // Y ya no se vuelve a tocar: lista para los shaders.
            pipelineBarrier(cmd, mipBarrier(image, level - 1, vk::ImageLayout::eTransferSrcOptimal,
                                            vk::ImageLayout::eShaderReadOnlyOptimal,
                                            vk::PipelineStageFlagBits2::eTransfer,
                                            vk::AccessFlagBits2::eTransferRead,
                                            vk::PipelineStageFlagBits2::eFragmentShader,
                                            vk::AccessFlagBits2::eShaderSampledRead));

            level_width = next_width;
            level_height = next_height;
        }

        // El ultimo nivel solo fue destino.
        pipelineBarrier(cmd, mipBarrier(image, levels - 1, vk::ImageLayout::eTransferDstOptimal,
                                        vk::ImageLayout::eShaderReadOnlyOptimal,
                                        vk::PipelineStageFlagBits2::eTransfer,
                                        vk::AccessFlagBits2::eTransferWrite,
                                        vk::PipelineStageFlagBits2::eFragmentShader,
                                        vk::AccessFlagBits2::eShaderSampledRead));
    });

    staging.destroy();

    vk::ImageViewCreateInfo view_info{};
    view_info.image = image;
    view_info.viewType = vk::ImageViewType::e2D;
    view_info.format = kTextureFormat;
    view_info.subresourceRange = vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, levels, 0, 1};
    view_ = vk::raii::ImageView(device.handle(), view_info);
}

void VulkanTexture::createCompressed(const VulkanDevice& device, std::uint32_t width,
                                     std::uint32_t height, vk::Format format,
                                     std::uint32_t block_bytes, std::uint32_t mip_levels,
                                     const std::uint8_t* data, std::size_t size) {
    if (width == 0 || height == 0 || data == nullptr || mip_levels == 0) {
        throw std::runtime_error("Textura comprimida vacia.");
    }
    if (!device.textureCompressionBcSupported()) {
        throw std::runtime_error("La GPU no admite texturas comprimidas BC (DDS).");
    }

    destroy();
    mip_levels_ = mip_levels;

    vk::ImageCreateInfo image_info{};
    image_info.imageType = vk::ImageType::e2D;
    image_info.format = format;
    image_info.extent = vk::Extent3D{width, height, 1};
    image_info.mipLevels = mip_levels;
    image_info.arrayLayers = 1;
    image_info.samples = vk::SampleCountFlagBits::e1;
    image_info.tiling = vk::ImageTiling::eOptimal;
    image_info.usage = vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled;
    image_info.sharingMode = vk::SharingMode::eExclusive;
    image_info.initialLayout = vk::ImageLayout::eUndefined;
    image_ = vk::raii::Image(device.handle(), image_info);

    const vk::MemoryRequirements requirements = image_.getMemoryRequirements();
    vk::MemoryAllocateInfo allocate_info{};
    allocate_info.allocationSize = requirements.size;
    allocate_info.memoryTypeIndex = device.findMemoryType(
        requirements.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal);
    memory_ = vk::raii::DeviceMemory(device.handle(), allocate_info);
    image_.bindMemory(*memory_, 0);

    VulkanBuffer staging;
    staging.create(device, size, vk::BufferUsageFlagBits::eTransferSrc,
                   vk::MemoryPropertyFlagBits::eHostVisible |
                       vk::MemoryPropertyFlagBits::eHostCoherent);
    staging.write(data, size);

    // Una copia por nivel: bloques de 4x4, como minimo uno por eje.
    std::vector<vk::BufferImageCopy> regions;
    vk::DeviceSize offset = 0;
    std::uint32_t level_width = width;
    std::uint32_t level_height = height;
    for (std::uint32_t level = 0; level < mip_levels; ++level) {
        vk::BufferImageCopy region{};
        region.bufferOffset = offset;
        region.imageSubresource =
            vk::ImageSubresourceLayers{vk::ImageAspectFlagBits::eColor, level, 0, 1};
        region.imageExtent = vk::Extent3D{level_width, level_height, 1};
        regions.push_back(region);

        offset += static_cast<vk::DeviceSize>(std::max(1u, (level_width + 3) / 4)) *
                  std::max(1u, (level_height + 3) / 4) * block_bytes;
        level_width = std::max(1u, level_width / 2);
        level_height = std::max(1u, level_height / 2);
    }

    const vk::Image image = *image_;
    device.submitOneTime([&](const vk::raii::CommandBuffer& cmd) {
        vk::ImageMemoryBarrier2 to_transfer = mipBarrier(
            image, 0, vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
            vk::PipelineStageFlagBits2::eNone, vk::AccessFlagBits2::eNone,
            vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite);
        to_transfer.subresourceRange.levelCount = mip_levels;
        pipelineBarrier(cmd, to_transfer);

        cmd.copyBufferToImage(*staging.handle(), image, vk::ImageLayout::eTransferDstOptimal,
                              regions);

        vk::ImageMemoryBarrier2 to_read = mipBarrier(
            image, 0, vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
            vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite,
            vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderSampledRead);
        to_read.subresourceRange.levelCount = mip_levels;
        pipelineBarrier(cmd, to_read);
    });

    staging.destroy();

    vk::ImageViewCreateInfo view_info{};
    view_info.image = image;
    view_info.viewType = vk::ImageViewType::e2D;
    view_info.format = format;
    view_info.subresourceRange =
        vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, mip_levels, 0, 1};
    view_ = vk::raii::ImageView(device.handle(), view_info);
}

void VulkanTexture::destroy() {
    view_ = nullptr;
    image_ = nullptr;
    memory_ = nullptr;
    mip_levels_ = 1;
}

}  // namespace cramion::gfx
