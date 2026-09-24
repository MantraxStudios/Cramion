#ifndef CRAMION_VK_SHADOW_MAP_H
#define CRAMION_VK_SHADOW_MAP_H

#include "scene/ShadowCascades.h"
#include "vk/VulkanCommon.h"
#include "vk/VulkanImage.h"

namespace cramion::gfx {

class VulkanDevice;

// Recursos del mapa de sombras en cascada: un array de imagenes de
// profundidad, una capa por cascada, y el muestreador de comparacion con el
// que el shader de iluminacion las lee.
//
// El muestreador tiene `compareEnable`: la GPU compara la profundidad pedida
// contra la guardada y devuelve el resultado ya filtrado bilinealmente. Eso da
// un PCF de 2x2 gratis en hardware, sobre el que el shader anade mas muestras.
//
// A diferencia del G-buffer, su resolucion no depende de la ventana.
class ShadowMap {
public:
    // Resolucion por cascada (4 cascadas x 6144^2 x 4 bytes = 576 MB). En un
    // mundo de bloques de un metro los bordes de sombra se notan mucho: con
    // 2048 las cascadas lejanas tenian texeles de 6-14 cm y los contornos
    // salian borrosos o en escalera. Con 6144 quedan en 0.5-4.7 cm.
    static constexpr std::uint32_t kResolution = 6144;

    void create(const VulkanDevice& device);
    void destroy();

    const VulkanImage& image() const { return depth_; }
    const vk::raii::Sampler& sampler() const { return sampler_; }

    // Vista de una cascada concreta, para renderizar en ella.
    const vk::raii::ImageView& cascadeView(std::uint32_t cascade) const {
        return depth_.layerView(cascade);
    }

    vk::Format format() const { return depth_.format(); }
    vk::Extent2D extent() const { return vk::Extent2D{kResolution, kResolution}; }
    bool isValid() const { return depth_.isValid(); }

private:
    VulkanImage depth_;
    vk::raii::Sampler sampler_{nullptr};
};

}  // namespace cramion::gfx

#endif  // CRAMION_VK_SHADOW_MAP_H
