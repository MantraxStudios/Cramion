#include "CramionFX/vk/VulkanCompat.h"
#include "CramionFX/vk/VulkanTexture.h"

#include "CramionFX/vk/VulkanDevice.h"
#include "CramionFX/vk/VulkanDeviceUpload.h"

#include <vk_mem_alloc.h>

#include <algorithm>
#include <bit>
#include <stdexcept>
#include <utility>
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
    compat::pipelineBarrier(cmd, dependency);
}

}  // namespace

VulkanTexture::VulkanTexture(VulkanTexture&& other) noexcept
    : memory_ticket_(std::move(other.memory_ticket_)),
      device_(std::exchange(other.device_, nullptr)),
      allocation_(std::exchange(other.allocation_, nullptr)),
      image_(std::move(other.image_)),
      view_(std::move(other.view_)),
      mip_levels_(std::exchange(other.mip_levels_, 1u)) {}

VulkanTexture& VulkanTexture::operator=(VulkanTexture&& other) noexcept {
    if (this != &other) {
        destroy();
        memory_ticket_ = std::move(other.memory_ticket_);
        device_ = std::exchange(other.device_, nullptr);
        allocation_ = std::exchange(other.allocation_, nullptr);
        image_ = std::move(other.image_);
        view_ = std::move(other.view_);
        mip_levels_ = std::exchange(other.mip_levels_, 1u);
    }
    return *this;
}

void VulkanTexture::createImage(const VulkanDevice& device, const vk::ImageCreateInfo& info) {
    image_ = vk::raii::Image(device.handle(), info);

    // Un hueco en un bloque de VMA (respeta bufferImageGranularity); las
    // imagenes grandes pueden llevar su propia asignacion si el driver lo pide.
    VmaAllocationCreateInfo allocation_info{};
    allocation_info.usage = VMA_MEMORY_USAGE_UNKNOWN;
    allocation_info.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    VmaAllocation allocation = nullptr;
    VmaAllocationInfo result_info{};
    const VkResult result = vmaAllocateMemoryForImage(device.allocator(), static_cast<VkImage>(*image_),
                                                      &allocation_info, &allocation, &result_info);
    if (result != VK_SUCCESS) {
        image_ = nullptr;
        throw std::runtime_error("No hay memoria de la GPU para una textura (" +
                                 vk::to_string(static_cast<vk::Result>(result)) + ").");
    }
    if (vmaBindImageMemory(device.allocator(), allocation, static_cast<VkImage>(*image_)) != VK_SUCCESS) {
        image_ = nullptr;
        vmaFreeMemory(device.allocator(), allocation);
        throw std::runtime_error("No se pudo enlazar la memoria de una textura.");
    }
    device_ = &device;
    allocation_ = allocation;
    memory_ticket_ = GpuMemoryTicket(GpuMemoryKind::Texture, result_info.size);
}

void VulkanTexture::create(const VulkanDevice& device, std::uint32_t width, std::uint32_t height,
                           const std::uint8_t* rgba) {
    UploadBatch batch(device);
    create(device, width, height, rgba, batch);
    batch.submitAndWait();
}

void VulkanTexture::create(const VulkanDevice& device, std::uint32_t width, std::uint32_t height,
                           const std::uint8_t* rgba, UploadBatch& batch) {
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
    createImage(device, image_info);

    // --- Subida del nivel 0 (cola de copias) ---
    const vk::DeviceSize size = static_cast<vk::DeviceSize>(width) * height * 4;
    const UploadBatch::Staging staging = batch.stage(rgba, size, 16);

    const vk::Image image = *image_;
    const std::uint32_t levels = mip_levels_;

    {
        const vk::raii::CommandBuffer& cmd = batch.copyCommands();
        // Todos los niveles a destino de copia.
        vk::ImageMemoryBarrier2 to_transfer = mipBarrier(
            image, 0, vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
            vk::PipelineStageFlagBits2::eNone, vk::AccessFlagBits2::eNone,
            vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite);
        to_transfer.subresourceRange.levelCount = levels;
        pipelineBarrier(cmd, to_transfer);

        vk::BufferImageCopy region{};
        region.bufferOffset = staging.offset;
        region.imageSubresource = vk::ImageSubresourceLayers{vk::ImageAspectFlagBits::eColor, 0, 0, 1};
        region.imageExtent = vk::Extent3D{width, height, 1};
        cmd.copyBufferToImage(staging.buffer, image, vk::ImageLayout::eTransferDstOptimal, region);
    }

    // A la cola de graficos (los blits de los mips no se pueden hacer en una
    // cola solo de copias), sin cambiar de layout.
    {
        vk::ImageMemoryBarrier2 hand_off = mipBarrier(
            image, 0, vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eTransferDstOptimal,
            vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite,
            vk::PipelineStageFlagBits2::eTransfer,
            vk::AccessFlagBits2::eTransferRead | vk::AccessFlagBits2::eTransferWrite);
        hand_off.subresourceRange.levelCount = levels;
        batch.handOffImage(hand_off);
    }

    // --- Generacion de los demas niveles (cola de graficos) ---
    const vk::raii::CommandBuffer& cmd = batch.graphicsCommands();
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
    UploadBatch batch(device);
    createCompressed(device, width, height, format, block_bytes, mip_levels, data, size, batch);
    batch.submitAndWait();
}

void VulkanTexture::createCompressed(const VulkanDevice& device, std::uint32_t width,
                                     std::uint32_t height, vk::Format format,
                                     std::uint32_t block_bytes, std::uint32_t mip_levels,
                                     const std::uint8_t* data, std::size_t size, UploadBatch& batch) {
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
    createImage(device, image_info);

    // (Alineado a 16: el desplazamiento de una copia de bloques tiene que ser
    // multiplo del tamano del bloque, 8 o 16 bytes.)
    const UploadBatch::Staging staging = batch.stage(data, size, 16);

    // Una copia por nivel: bloques de 4x4, como minimo uno por eje.
    std::vector<vk::BufferImageCopy> regions;
    vk::DeviceSize offset = 0;
    std::uint32_t level_width = width;
    std::uint32_t level_height = height;
    for (std::uint32_t level = 0; level < mip_levels; ++level) {
        vk::BufferImageCopy region{};
        region.bufferOffset = staging.offset + offset;
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
    {
        const vk::raii::CommandBuffer& cmd = batch.copyCommands();
        vk::ImageMemoryBarrier2 to_transfer = mipBarrier(
            image, 0, vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
            vk::PipelineStageFlagBits2::eNone, vk::AccessFlagBits2::eNone,
            vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite);
        to_transfer.subresourceRange.levelCount = mip_levels;
        pipelineBarrier(cmd, to_transfer);

        cmd.copyBufferToImage(staging.buffer, image, vk::ImageLayout::eTransferDstOptimal, regions);
    }

    // Lista para los shaders (con cola de transferencia, ademas pasa a la de
    // graficos).
    vk::ImageMemoryBarrier2 to_read = mipBarrier(
        image, 0, vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
        vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite,
        vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderSampledRead);
    to_read.subresourceRange.levelCount = mip_levels;
    batch.handOffImage(to_read);

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
    // (Sin asignador: el dispositivo ya se cerro con esto vivo; se deja.)
    if (allocation_ != nullptr && device_ != nullptr && device_->allocator() != nullptr) {
        vmaFreeMemory(device_->allocator(), allocation_);
    }
    allocation_ = nullptr;
    device_ = nullptr;
    memory_ticket_.release();
    mip_levels_ = 1;
}

}  // namespace cramion::gfx
