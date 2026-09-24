#ifndef CRAMION_VK_LOCAL_SHADOW_MAPS_H
#define CRAMION_VK_LOCAL_SHADOW_MAPS_H

#include "scene/LocalLightShadows.h"
#include "vk/VulkanCommon.h"
#include "vk/VulkanImage.h"

namespace cramion::gfx {

class VulkanDevice;

// Mapas de sombra de las luces locales, en dos arrays de profundidad:
//
//   - Focos:    una capa por foco.
//   - Puntuales: seis capas consecutivas por luz, una por cara del cubo
//                (capa = hueco * 6 + cara).
//
// Las caras del cubo se guardan como capas 2D normales en vez de en un cubemap:
// asi se leen con el mismo sampler2DArrayShadow y el mismo PCF que las cascadas,
// y no hace falta la caracteristica opcional imageCubeArray.
//
// Se muestrean con el sampler de comparacion de ShadowMap, que tiene justo la
// configuracion necesaria (borde blanco = iluminado).
class LocalShadowMaps {
public:
    // 8 focos x 2048^2 x 4 bytes = 128 MB; 8 luces x 6 caras x 1024^2 x 4 = 192 MB.
    // Gracias a la cache solo se redibujan los mapas de las luces que se mueven,
    // asi que la resolucion cuesta sobre todo memoria, no tiempo por frame.
    static constexpr std::uint32_t kSpotResolution = 2048;
    static constexpr std::uint32_t kPointResolution = 1024;

    static constexpr std::uint32_t kSpotLayerCount = scene::kMaxShadowedSpotLights;
    static constexpr std::uint32_t kPointLayerCount =
        scene::kMaxShadowedPointLights * scene::kPointShadowFaceCount;

    void create(const VulkanDevice& device);
    void destroy();

    const VulkanImage& spotImage() const { return spot_depth_; }
    const VulkanImage& pointImage() const { return point_depth_; }

    const vk::raii::ImageView& spotView(std::uint32_t slot) const {
        return spot_depth_.layerView(slot);
    }
    const vk::raii::ImageView& pointFaceView(std::uint32_t slot, std::uint32_t face) const {
        return point_depth_.layerView(slot * scene::kPointShadowFaceCount + face);
    }

    vk::Format format() const { return spot_depth_.format(); }
    vk::Extent2D spotExtent() const { return vk::Extent2D{kSpotResolution, kSpotResolution}; }
    vk::Extent2D pointExtent() const { return vk::Extent2D{kPointResolution, kPointResolution}; }

private:
    VulkanImage spot_depth_;
    VulkanImage point_depth_;
};

}  // namespace cramion::gfx

#endif  // CRAMION_VK_LOCAL_SHADOW_MAPS_H
