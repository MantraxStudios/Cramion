#ifndef CRAMION_VK_ENVIRONMENT_MAP_H
#define CRAMION_VK_ENVIRONMENT_MAP_H

#include "CramionFX/core/Math.h"
#include "CramionFX/vk/VulkanCommon.h"

#include <cstdint>
#include <filesystem>

namespace cramion::gfx {

class VulkanDevice;

// Mapa de entorno HDR equirectangular (un .hdr de Radiance, como el
// san_giuseppe_bridge_4k.hdr que trae Bistro): el cielo fotografiado en vez
// del cielo fisico. Lo usan el fondo, la niebla, el IBL y los rayos que no
// chocan con nada.
//
// Al cargarlo:
//   - Se busca el sol de la foto (la zona mas brillante): la luz direccional
//     de la escena se coloca ahi, asi las sombras cuadran con el cielo.
//   - Se recorta el sol de la imagen: su luz directa ya la pone la luz
//     direccional (con sombras); dejarlo en el entorno la sumaria dos veces.
//   - Se escala para que el cielo tenga el brillo del cielo fisico del motor
//     (la foto no esta en las unidades de la escena).
//
// Convencion (igual en environment.glsl): u = atan(z, x) / 2pi + 0.5,
// v = acos(y) / pi (la fila 0 es el cenit).
class EnvironmentMap {
public:
    static constexpr vk::Format kFormat = vk::Format::eR16G16B16A16Sfloat;
    static constexpr std::uint32_t kMaxWidth = 2048;

    // false si no se puede leer.
    bool load(const VulkanDevice& device, const std::filesystem::path& path);
    void destroy();

    bool loaded() const { return *view_ != nullptr; }
    const vk::raii::ImageView& view() const { return view_; }
    const vk::raii::Sampler& sampler() const { return sampler_; }
    std::uint32_t width() const { return width_; }

    // Direccion hacia el sol de la foto (unitaria).
    const core::Vec3& sunDirection() const { return sun_direction_; }

private:
    vk::raii::DeviceMemory memory_{nullptr};
    vk::raii::Image image_{nullptr};
    vk::raii::ImageView view_{nullptr};
    vk::raii::Sampler sampler_{nullptr};
    std::uint32_t width_ = 0;
    core::Vec3 sun_direction_{0.0f, 1.0f, 0.0f};
};

}  // namespace cramion::gfx

#endif  // CRAMION_VK_ENVIRONMENT_MAP_H
