#include "CramionFX/vk/VulkanImage.h"

#include "CramionFX/vk/VulkanDevice.h"

#include <stdexcept>

namespace cramion::gfx {

void VulkanImage::create(const VulkanDevice& device, vk::Extent2D extent, vk::Format format,
                         vk::ImageUsageFlags usage, vk::ImageAspectFlags aspect,
                         std::uint32_t layers) {
    if (extent.width == 0 || extent.height == 0) {
        throw std::runtime_error("No se puede crear una imagen de area 0.");
    }

    destroy();

    vk::ImageCreateInfo image_info{};
    image_info.imageType = vk::ImageType::e2D;
    image_info.format = format;
    image_info.extent = vk::Extent3D{extent.width, extent.height, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = layers;
    image_info.samples = vk::SampleCountFlagBits::e1;
    image_info.tiling = vk::ImageTiling::eOptimal;
    image_info.usage = usage;
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

    vk::ImageViewCreateInfo view_info{};
    view_info.image = *image_;
    view_info.viewType = (layers > 1) ? vk::ImageViewType::e2DArray : vk::ImageViewType::e2D;
    view_info.format = format;
    view_info.subresourceRange = vk::ImageSubresourceRange{aspect, 0, 1, 0, layers};

    view_ = vk::raii::ImageView(device.handle(), view_info);

    // Una vista por capa: cada cascada de sombras se renderiza por separado.
    layer_views_.reserve(layers);
    for (std::uint32_t layer = 0; layer < layers; ++layer) {
        vk::ImageViewCreateInfo layer_info{};
        layer_info.image = *image_;
        layer_info.viewType = vk::ImageViewType::e2D;
        layer_info.format = format;
        layer_info.subresourceRange = vk::ImageSubresourceRange{aspect, 0, 1, layer, 1};

        layer_views_.emplace_back(device.handle(), layer_info);
    }

    format_ = format;
    extent_ = extent;
    aspect_ = aspect;
    layers_ = layers;
}

void VulkanImage::destroy() {
    layer_views_.clear();
    view_ = nullptr;
    image_ = nullptr;
    memory_ = nullptr;

    format_ = vk::Format::eUndefined;
    extent_ = vk::Extent2D{0, 0};
    layers_ = 1;
}

}  // namespace cramion::gfx
