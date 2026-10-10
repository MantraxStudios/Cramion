#include "CramionFX/vk/LocalShadowMaps.h"

#include "CramionFX/vk/VulkanDevice.h"

#include <iostream>

namespace cramion::gfx {

void LocalShadowMaps::create(const VulkanDevice& device, bool spots, bool points) {
    destroy();

    const vk::ImageUsageFlags usage =
        vk::ImageUsageFlagBits::eDepthStencilAttachment | vk::ImageUsageFlagBits::eSampled;

    const std::uint32_t spot_size = spots ? kSpotResolution : 1u;
    const std::uint32_t point_size = points ? kPointResolution : 1u;
    spot_depth_.create(device, vk::Extent2D{spot_size, spot_size}, device.depthFormat(), usage,
                       vk::ImageAspectFlagBits::eDepth, kSpotLayerCount);
    point_depth_.create(device, vk::Extent2D{point_size, point_size}, device.depthFormat(), usage,
                        vk::ImageAspectFlagBits::eDepth, kPointLayerCount);

    if (spots || points) {
        std::cout << "[Vulkan] Mapas de sombra locales: " << kSpotLayerCount << " focos de " << spot_size << "x"
                  << spot_size << ", " << scene::kMaxShadowedPointLights << " luces puntuales x 6 caras de "
                  << point_size << "x" << point_size << "\n";
    }
}

void LocalShadowMaps::destroy() {
    point_depth_.destroy();
    spot_depth_.destroy();
}

}  // namespace cramion::gfx
