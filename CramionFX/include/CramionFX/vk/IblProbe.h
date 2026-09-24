#ifndef CRAMION_VK_IBL_PROBE_H
#define CRAMION_VK_IBL_PROBE_H

#include "CramionFX/core/Math.h"
#include "CramionFX/vk/ComputePass.h"
#include "CramionFX/vk/VulkanBuffer.h"
#include "CramionFX/vk/VulkanCommon.h"

#include <cstdint>
#include <vector>

namespace cramion::gfx {

class VulkanDevice;
class VulkanImage;

// Iluminacion basada en imagen (IBL) del entorno exterior, como el "Sky
// Light" de Unreal en modo captura en tiempo real:
//
//   - Cubo de entorno prefiltrado: cada nivel de mip es el entorno visto a
//     traves de un lobulo GGX mas ancho (rugosidad 0 en el nivel 0, 1 en el
//     ultimo). Da los reflejos de metales y superficies pulidas.
//   - Irradiancia en armonicos esfericos (9 coeficientes): la luz difusa del
//     cielo y el suelo para cualquier normal.
//   - LUT de la BRDF (split-sum): se calcula una sola vez.
//
// El entorno sale de la LUT del cielo fisico, asi que se regenera cada frame:
// sigue al ciclo dia/noche, los atardeceres y la luna sin coste de captura.
class IblProbe {
public:
    static constexpr std::uint32_t kEnvironmentSize = 128;
    static constexpr std::uint32_t kEnvironmentMips = 6;  // 128 -> 4
    static constexpr std::uint32_t kBrdfLutSize = 128;
    static constexpr vk::Format kFormat = vk::Format::eR16G16B16A16Sfloat;

    void create(const VulkanDevice& device, const VulkanImage& sky_lut);
    void destroy();

    // Graba la regeneracion del entorno (y la LUT de la BRDF la primera vez).
    // La LUT del cielo debe estar ya escrita y legible desde compute. Al
    // terminar, todo queda listo para leerse en los fragment shaders.
    // `light_radiance` y `to_light` son los de la luz direccional activa (sol
    // o luna), para iluminar el suelo del entorno.
    // `hdr`: el entorno es el mapa HDR de setEnvironment(), no el cielo.
    // `wet`: fraccion del suelo cubierta de agua (lluvia): refleja el cielo.
    void record(const vk::raii::CommandBuffer& cmd, const core::Vec3& light_radiance,
                const core::Vec3& to_light, bool hdr, float wet);

    // Mapa de entorno HDR (equirectangular) que se usa con `hdr` = true.
    void setEnvironment(const VulkanDevice& device, vk::ImageView view, vk::Sampler sampler);

    const vk::raii::ImageView& environmentView() const { return environment_view_; }
    const vk::raii::ImageView& brdfLutView() const { return brdf_view_; }
    const vk::raii::Sampler& sampler() const { return sampler_; }
    const VulkanBuffer& irradianceBuffer() const { return irradiance_; }

private:
    struct Image {
        vk::raii::DeviceMemory memory{nullptr};
        vk::raii::Image image{nullptr};
    };

    static Image createImage(const VulkanDevice& device, const vk::ImageCreateInfo& info);

    Image environment_;
    vk::raii::ImageView environment_view_{nullptr};           // cubo, todos los mips
    std::vector<vk::raii::ImageView> environment_mip_views_;  // array 2D por mip (escritura)

    Image brdf_;
    vk::raii::ImageView brdf_view_{nullptr};

    VulkanBuffer irradiance_;
    vk::raii::Sampler sampler_{nullptr};

    ComputePass prefilter_pass_;
    ComputePass sh_pass_;
    ComputePass brdf_pass_;

    vk::raii::DescriptorPool pool_{nullptr};
    std::vector<vk::raii::DescriptorSet> prefilter_sets_;  // uno por mip
    std::vector<vk::raii::DescriptorSet> sh_sets_;
    std::vector<vk::raii::DescriptorSet> brdf_sets_;

    bool brdf_ready_ = false;
};

}  // namespace cramion::gfx

#endif  // CRAMION_VK_IBL_PROBE_H
