#include "vk/LocalShadowMaps.h"

#include "vk/VulkanDevice.h"

#include <iostream>

namespace cramion::gfx {

void LocalShadowMaps::create(const VulkanDevice& device) {
    destroy();

    const vk::ImageUsageFlags usage =
        vk::ImageUsageFlagBits::eDepthStencilAttachment | vk::ImageUsageFlagBits::eSampled;

    spot_depth_.create(device, spotExtent(), device.depthFormat(), usage,
                       vk::ImageAspectFlagBits::eDepth, kSpotLayerCount);
    point_depth_.create(device, pointExtent(), device.depthFormat(), usage,
                        vk::ImageAspectFlagBits::eDepth, kPointLayerCount);

    std::cout << "[Vulkan] Mapas de sombra locales creados: " << kSpotLayerCount << " focos de "
              << kSpotResolution << "x" << kSpotResolution << ", "
              << scene::kMaxShadowedPointLights << " luces puntuales x 6 caras de "
              << kPointResolution << "x" << kPointResolution << "\n";
}

void LocalShadowMaps::destroy() {
    point_depth_.destroy();
    spot_depth_.destroy();
}

}  // namespace cramion::gfx
