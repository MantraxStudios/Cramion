#include "CramionFX/vk/GBuffer.h"

#include "CramionFX/vk/VulkanDevice.h"

#include <iostream>

namespace cramion::gfx {

void GBuffer::create(const VulkanDevice& device, vk::Extent2D extent) {
    destroy();

    // Los destinos de color se escriben como attachment y se leen despues como
    // textura en la pasada de iluminacion.
    const vk::ImageUsageFlags color_usage =
        vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled;

    albedo_.create(device, extent, kAlbedoFormat, color_usage, vk::ImageAspectFlagBits::eColor);
    normal_.create(device, extent, kNormalFormat, color_usage, vk::ImageAspectFlagBits::eColor);
    material_.create(device, extent, kMaterialFormat, color_usage,
                     vk::ImageAspectFlagBits::eColor);
    velocity_.create(device, extent, kVelocityFormat, color_usage, vk::ImageAspectFlagBits::eColor);

    // La profundidad tambien se muestrea desde la pasada de iluminacion para
    // reconstruir la posicion del mundo.
    depth_.create(device, extent, device.depthFormat(),
                  vk::ImageUsageFlagBits::eDepthStencilAttachment |
                      vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferSrc,
                  vk::ImageAspectFlagBits::eDepth);

    extent_ = extent;

    std::cout << "[Vulkan] G-buffer creado: " << extent.width << "x" << extent.height
              << " (albedo RGBA8, normal RGBA16F, profundidad "
              << vk::to_string(depth_.format()) << ")\n";
}

void GBuffer::destroy() {
    depth_.destroy();
    velocity_.destroy();
    material_.destroy();
    normal_.destroy();
    albedo_.destroy();
    extent_ = vk::Extent2D{0, 0};
}

}  // namespace cramion::gfx
