#ifndef CRAMION_VK_REFLECTION_PROBE_H
#define CRAMION_VK_REFLECTION_PROBE_H

#include "vk/ComputePass.h"
#include "vk/VulkanCommon.h"

#include <cstdint>
#include <vector>

namespace cramion::gfx {

class VulkanDevice;

// Sonda de reflexion de la escena, como la "Sphere Reflection Capture" de
// Unreal: un cubo con lo que se ve desde un punto (paredes, columnas, suelo,
// ventanas), no solo el cielo. Es lo que se refleja donde el SSR no encuentra
// nada: lo que queda fuera de la pantalla o detras de la camara (un espejo o
// un suelo pulido visto de frente).
//
// El renderizador dibuja la escena entera desde la sonda, una cara del cubo
// por frame, y copia cada imagen aqui (recordFaceCopy). Con las seis caras,
// recordPrefilter hace los mips de la captura y la prefiltra por rugosidad
// (probe_prefilter.comp) en el cubo que lee la iluminacion.
//
// El alfa guarda la distancia de la sonda a lo que se ve en cada direccion:
// con ella lighting.frag corrige el paralaje (lo que se refleja depende de
// donde esta el punto que refleja, no solo de la direccion).
class ReflectionProbe {
public:
    static constexpr std::uint32_t kCaptureSize = 256;
    static constexpr std::uint32_t kCaptureMips = 9;  // 256 -> 1
    // Mismo tamano y niveles que el IBL del cielo: la iluminacion elige el
    // mip con la misma formula para los dos.
    static constexpr std::uint32_t kSize = 128;
    static constexpr std::uint32_t kMips = 6;
    static constexpr vk::Format kFormat = vk::Format::eR16G16B16A16Sfloat;

    void create(const VulkanDevice& device);
    void destroy();

    // Copia el cuadrado `region` de `source` (en layout de origen de copia) a
    // la cara `face` de la captura. El render tiene la X al reves que las
    // caras de un cubo, asi que la copia la invierte.
    void recordFaceCopy(const vk::raii::CommandBuffer& cmd, vk::Image source,
                        const vk::Rect2D& region, std::uint32_t face) const;

    // Con las seis caras copiadas: mips de la captura y prefiltrado. Al
    // terminar el cubo queda listo para leerse en los fragment shaders.
    void recordPrefilter(const vk::raii::CommandBuffer& cmd) const;

    const vk::raii::ImageView& view() const { return probe_view_; }
    const vk::raii::Sampler& sampler() const { return sampler_; }

private:
    struct Image {
        vk::raii::DeviceMemory memory{nullptr};
        vk::raii::Image image{nullptr};
    };

    static Image createImage(const VulkanDevice& device, const vk::ImageCreateInfo& info);

    // Captura: nivel 0 con las caras, el resto para el filtrado.
    Image capture_;
    vk::raii::ImageView capture_view_{nullptr};

    // Cubo prefiltrado que lee la iluminacion.
    Image probe_;
    vk::raii::ImageView probe_view_{nullptr};
    std::vector<vk::raii::ImageView> probe_mip_views_;  // array 2D por mip (escritura)

    vk::raii::Sampler sampler_{nullptr};
    ComputePass prefilter_pass_;
    vk::raii::DescriptorPool pool_{nullptr};
    std::vector<vk::raii::DescriptorSet> prefilter_sets_;  // uno por mip
};

}  // namespace cramion::gfx

#endif  // CRAMION_VK_REFLECTION_PROBE_H
