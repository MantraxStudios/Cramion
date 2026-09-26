#ifndef CRAMION_VK_SHADOW_MAP_H
#define CRAMION_VK_SHADOW_MAP_H

#include "CramionFX/scene/ShadowCascades.h"
#include "CramionFX/vk/VulkanCommon.h"
#include "CramionFX/vk/VulkanImage.h"

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
    // Resolucion por cascada: la elige el perfil de hardware (FrameBudget.h:
    // 1536 en un PC de gama baja = 36 MB; 6144 en Ultra = 4 x 6144^2 x 4 bytes
    // = 576 MB) o el usuario. En un mundo de bloques de un metro los bordes se
    // notan: con 2048 las cascadas lejanas tienen texeles de 6-14 cm, con
    // 6144 de 0.5-4.7 cm.
    static constexpr std::uint32_t kDefaultResolution = 4096;

    void create(const VulkanDevice& device, std::uint32_t resolution = kDefaultResolution);
    std::uint32_t resolution() const { return resolution_; }
    void destroy();

    const VulkanImage& image() const { return depth_; }
    const vk::raii::Sampler& sampler() const { return sampler_; }

    // Vista de una cascada concreta, para renderizar en ella.
    const vk::raii::ImageView& cascadeView(std::uint32_t cascade) const {
        return depth_.layerView(cascade);
    }

    vk::Format format() const { return depth_.format(); }
    vk::Extent2D extent() const { return vk::Extent2D{resolution_, resolution_}; }
    bool isValid() const { return depth_.isValid(); }

private:
    std::uint32_t resolution_ = kDefaultResolution;
    VulkanImage depth_;
    vk::raii::Sampler sampler_{nullptr};
};

}  // namespace cramion::gfx

#endif  // CRAMION_VK_SHADOW_MAP_H
