#ifndef CRAMION_VK_GBUFFER_H
#define CRAMION_VK_GBUFFER_H

#include "CramionFX/vk/VulkanCommon.h"
#include "CramionFX/vk/VulkanImage.h"

#include <array>

namespace cramion::gfx {

class VulkanDevice;

// Destinos intermedios del renderizador diferido.
//
// La pasada de geometria escribe aqui las propiedades de cada superficie y la
// pasada de iluminacion las lee como texturas:
//
//   albedo    RGBA8      rgb = color base,  a = oclusion ambiental
//   normal    RGBA16F    rg = normal (octaedro), b = rugosidad, a = libre
//   material  RGBA16F    rgb = emision (radiancia HDR lineal), a = metalicidad
//   depth     D32        profundidad; la pasada de iluminacion la muestrea y
//                        reconstruye con ella la posicion del mundo
//
// La posicion NO se guarda: en media precision (RGBA16F) los saltos de un half
// cerca de y=40 son de ~0.03 unidades, y como la atenuacion de las luces
// depende de la distancia, eso se ve como bandas en los degradados. Deducirla
// del depth de 32 bits con la inversa de view-projection da precision completa
// y ahorra 8 bytes por pixel de ancho de banda.
//
// Se recrea con cada cambio de tamano de la ventana.
class GBuffer {
public:
    // Numero de destinos de color (sin contar la profundidad).
    static constexpr std::size_t kColorAttachmentCount = 3;

    static constexpr vk::Format kAlbedoFormat = vk::Format::eR8G8B8A8Unorm;
    static constexpr vk::Format kNormalFormat = vk::Format::eR16G16B16A16Sfloat;
    static constexpr vk::Format kMaterialFormat = vk::Format::eR16G16B16A16Sfloat;

    void create(const VulkanDevice& device, vk::Extent2D extent);
    void destroy();

    const VulkanImage& albedo() const { return albedo_; }
    const VulkanImage& normal() const { return normal_; }
    const VulkanImage& material() const { return material_; }
    const VulkanImage& depth() const { return depth_; }

    // Los destinos de color en el orden en que los declara el shader.
    std::array<const VulkanImage*, kColorAttachmentCount> colorAttachments() const {
        return {&albedo_, &normal_, &material_};
    }

    std::array<vk::Format, kColorAttachmentCount> colorFormats() const {
        return {kAlbedoFormat, kNormalFormat, kMaterialFormat};
    }

    vk::Format depthFormat() const { return depth_.format(); }
    vk::Extent2D extent() const { return extent_; }
    bool isValid() const { return albedo_.isValid(); }

private:
    VulkanImage albedo_;
    VulkanImage normal_;
    VulkanImage material_;
    VulkanImage depth_;

    vk::Extent2D extent_{0, 0};
};

}  // namespace cramion::gfx

#endif  // CRAMION_VK_GBUFFER_H
