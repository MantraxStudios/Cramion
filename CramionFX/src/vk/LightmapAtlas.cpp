#include "CramionFX/vk/LightmapAtlas.h"

#include "CramionFX/vk/VulkanBuffer.h"
#include "CramionFX/vk/VulkanCompat.h"
#include "CramionFX/vk/VulkanDevice.h"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <vector>

namespace cramion::gfx {

namespace {

constexpr vk::Format kFormat = vk::Format::eR16G16B16A16Sfloat;
constexpr std::uint16_t kHalfOne = 0x3C00u;

vk::ImageMemoryBarrier2 layerBarrier(vk::Image image, std::uint32_t layers, vk::ImageLayout from, vk::ImageLayout to,
                                     vk::PipelineStageFlags2 src_stage, vk::AccessFlags2 src_access,
                                     vk::PipelineStageFlags2 dst_stage, vk::AccessFlags2 dst_access) {
    vk::ImageMemoryBarrier2 barrier{};
    barrier.srcStageMask = src_stage;
    barrier.srcAccessMask = src_access;
    barrier.dstStageMask = dst_stage;
    barrier.dstAccessMask = dst_access;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.image = image;
    barrier.subresourceRange = vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, layers};
    return barrier;
}

void barrier(const vk::raii::CommandBuffer& cmd, const vk::ImageMemoryBarrier2& b) {
    vk::DependencyInfo dependency{};
    dependency.setImageMemoryBarriers(b);
    compat::pipelineBarrier(cmd, dependency);
}

}  // namespace

bool LightmapAtlas::createImage(const VulkanDevice& device, std::uint32_t width, std::uint32_t height, std::uint32_t layers) {
    view_ = nullptr;
    image_ = nullptr;
    memory_ = nullptr;
    memory_ticket_.release();
    vk::ImageCreateInfo info{};
    info.imageType = vk::ImageType::e2D;
    info.format = kFormat;
    info.extent = vk::Extent3D{width, height, 1};
    info.mipLevels = 1;
    info.arrayLayers = layers;
    info.samples = vk::SampleCountFlagBits::e1;
    info.tiling = vk::ImageTiling::eOptimal;
    info.usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst;
    info.sharingMode = vk::SharingMode::eExclusive;
    info.initialLayout = vk::ImageLayout::eUndefined;
    image_ = vk::raii::Image(device.handle(), info);
    const vk::MemoryRequirements requirements = image_.getMemoryRequirements();
    vk::MemoryAllocateInfo allocate{};
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = device.findMemoryType(requirements.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal);
    memory_ = vk::raii::DeviceMemory(device.handle(), allocate);
    memory_ticket_ = GpuMemoryTicket(GpuMemoryKind::Lightmap, requirements.size);
    image_.bindMemory(*memory_, 0);
    vk::ImageViewCreateInfo view{};
    view.image = *image_;
    view.viewType = vk::ImageViewType::e2DArray;  // siempre array (sampler2DArray)
    view.format = kFormat;
    view.subresourceRange = vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, layers};
    view_ = vk::raii::ImageView(device.handle(), view);
    if (!*sampler_) {
        vk::SamplerCreateInfo sampler{};
        sampler.magFilter = vk::Filter::eLinear;
        sampler.minFilter = vk::Filter::eLinear;
        sampler.mipmapMode = vk::SamplerMipmapMode::eNearest;
        sampler.addressModeU = vk::SamplerAddressMode::eClampToEdge;
        sampler.addressModeV = vk::SamplerAddressMode::eClampToEdge;
        sampler.addressModeW = vk::SamplerAddressMode::eClampToEdge;
        sampler.minLod = 0.0f;
        sampler.maxLod = 0.0f;
        sampler_ = vk::raii::Sampler(device.handle(), sampler);
    }
    width_ = width;
    height_ = height;
    layers_ = layers;
    bytes_ = requirements.size;
    return true;
}

void LightmapAtlas::createDummy(const VulkanDevice& device) {
    createImage(device, 1, 1, 1);
    dummy_ = true;
    const std::uint16_t texel[4] = {0, 0, 0, kHalfOne};
    VulkanBuffer staging;
    staging.create(device, sizeof(texel), vk::BufferUsageFlagBits::eTransferSrc,
                   vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
    staging.write(texel, sizeof(texel));
    const vk::Image image = *image_;
    device.submitOneTime([&](const vk::raii::CommandBuffer& cmd) {
        barrier(cmd, layerBarrier(image, 1, vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
                                  vk::PipelineStageFlagBits2::eNone, vk::AccessFlagBits2::eNone,
                                  vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite));
        vk::BufferImageCopy region{};
        region.imageSubresource = vk::ImageSubresourceLayers{vk::ImageAspectFlagBits::eColor, 0, 0, 1};
        region.imageExtent = vk::Extent3D{1, 1, 1};
        cmd.copyBufferToImage(*staging.handle(), image, vk::ImageLayout::eTransferDstOptimal, region);
        barrier(cmd, layerBarrier(image, 1, vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
                                  vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite,
                                  vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderSampledRead));
    });
}

bool LightmapAtlas::upload(const VulkanDevice& device, const LightmapSet& set) {
    std::uint32_t width = 0, height = 0, layers = 0;
    for (const LightmapPage& page : set.pages) {
        if (page.width == 0 || page.height == 0 ||
            page.texels.size() < static_cast<std::size_t>(page.width) * page.height * 4) {
            break;
        }
        width = std::max(width, page.width);
        height = std::max(height, page.height);
        ++layers;
    }
    if (layers == 0 || layers != set.pages.size()) {
        if (!set.pages.empty()) std::cerr << "[Lightmap] Paginas del lightmap danadas: no se usan\n";
        createDummy(device);
        return false;
    }
    width = std::max(width, set.width);
    height = std::max(height, set.height);
    const std::uint32_t max_size = device.physicalDevice().getProperties().limits.maxImageDimension2D;
    const std::uint32_t max_layers = device.physicalDevice().getProperties().limits.maxImageArrayLayers;
    if (width > max_size || height > max_size || layers > max_layers) {
        std::cerr << "[Lightmap] El atlas (" << width << " x " << height << " x " << layers
                  << ") no cabe en esta GPU: sin lightmaps\n";
        createDummy(device);
        return false;
    }
    createImage(device, width, height, layers);
    dummy_ = false;
    // Todo por un staging (las paginas seguidas).
    vk::DeviceSize total = 0;
    for (const LightmapPage& page : set.pages) total += static_cast<vk::DeviceSize>(page.width) * page.height * 8;
    VulkanBuffer staging;
    staging.create(device, total, vk::BufferUsageFlagBits::eTransferSrc,
                   vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
    std::vector<vk::BufferImageCopy> regions;
    vk::DeviceSize offset = 0;
    for (std::uint32_t layer = 0; layer < layers; ++layer) {
        const LightmapPage& page = set.pages[layer];
        const vk::DeviceSize bytes = static_cast<vk::DeviceSize>(page.width) * page.height * 8;
        staging.write(page.texels.data(), bytes, offset);
        vk::BufferImageCopy region{};
        region.bufferOffset = offset;
        region.imageSubresource = vk::ImageSubresourceLayers{vk::ImageAspectFlagBits::eColor, 0, layer, 1};
        region.imageExtent = vk::Extent3D{page.width, page.height, 1};
        regions.push_back(region);
        offset += bytes;
    }
    const vk::Image image = *image_;
    device.submitOneTime([&](const vk::raii::CommandBuffer& cmd) {
        barrier(cmd, layerBarrier(image, layers, vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
                                  vk::PipelineStageFlagBits2::eNone, vk::AccessFlagBits2::eNone,
                                  vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite));
        // Lo que no cubre una pagina mas pequena que el array: sin luz y sin sombra.
        const vk::ClearColorValue clear{0.0f, 0.0f, 0.0f, 1.0f};
        cmd.clearColorImage(image, vk::ImageLayout::eTransferDstOptimal, clear,
                            vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, layers});
        barrier(cmd, layerBarrier(image, layers, vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eTransferDstOptimal,
                                  vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite,
                                  vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite));
        cmd.copyBufferToImage(*staging.handle(), image, vk::ImageLayout::eTransferDstOptimal, regions);
        barrier(cmd, layerBarrier(image, layers, vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
                                  vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite,
                                  vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderSampledRead));
    });
    return true;
}

void LightmapAtlas::destroy() {
    view_ = nullptr;
    image_ = nullptr;
    memory_ = nullptr;
    sampler_ = nullptr;
    memory_ticket_.release();
    width_ = height_ = layers_ = 0;
    bytes_ = 0;
    dummy_ = true;
}

}  // namespace cramion::gfx
