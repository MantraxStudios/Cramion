#ifndef CRAMION_VK_CLOUD_NOISE_H
#define CRAMION_VK_CLOUD_NOISE_H

#include "CramionFX/vk/VulkanCommon.h"

#include <cstdint>

namespace cramion::gfx {

class VulkanDevice;

// Textura 3D de ruido de las nubes volumetricas (cloud_noise.comp): forma en
// r (Perlin-Worley) y erosion en g, b, a (Worley a tres frecuencias). Se
// genera en la GPU una vez, al crearla, y se repite por el cielo sin costuras
// (muestreador en modo repetir).
class CloudNoise {
public:
    static constexpr std::uint32_t kSize = 128;
    static constexpr vk::Format kFormat = vk::Format::eR8G8B8A8Unorm;

    void create(const VulkanDevice& device);
    void destroy();

    const vk::raii::ImageView& view() const { return view_; }
    const vk::raii::Sampler& sampler() const { return sampler_; }

private:
    // La vista antes que la imagen, y la imagen antes que su memoria (orden
    // de destruccion inverso).
    vk::raii::DeviceMemory memory_{nullptr};
    vk::raii::Image image_{nullptr};
    vk::raii::ImageView view_{nullptr};
    vk::raii::Sampler sampler_{nullptr};
};

}  // namespace cramion::gfx

#endif  // CRAMION_VK_CLOUD_NOISE_H
