#ifndef CRAMION_VK_TEXTURE_FILTERING_H
#define CRAMION_VK_TEXTURE_FILTERING_H

// Filtrado de las texturas de los materiales (modelos, terreno, vegetacion,
// agua y voxeles): el anisotropico y el sesgo de los mips. Lo elige el
// jugador (GraphicsSettings::texture_filtering / texture_lod_bias) y
// VulkanRenderer::applyTextureFiltering lo pasa a cada pasada, que rehace su
// sampler y vuelve a escribir los descriptor sets que lo usan (con la GPU
// parada: cambiarlo es raro, desde el menu de opciones).
//
//   anisotropy  1 = sin anisotropico (trilineal), 2, 4, 8 o 16. Se limita al
//               maximo de la GPU (maxSamplerAnisotropy).
//   lod_bias    sesgo de los mips: negativo = mas nitidas (con mas
//               parpadeo), positivo = mas suaves (y menos ancho de banda).

#include "CramionFX/vk/VulkanCommon.h"

#include <algorithm>

namespace cramion::gfx {

struct TextureFiltering {
    float anisotropy = 8.0f;
    float lod_bias = 0.0f;

    friend bool operator==(const TextureFiltering&, const TextureFiltering&) = default;
};

// Rellena el anisotropico y el sesgo de un sampler de texturas de material.
inline void applyTextureFiltering(vk::SamplerCreateInfo& info, const vk::raii::PhysicalDevice& physical,
                                  const TextureFiltering& filtering) {
    info.mipLodBias = std::clamp(filtering.lod_bias, -1.0f, 2.0f);
    info.anisotropyEnable = VK_FALSE;
    info.maxAnisotropy = 1.0f;
    if (filtering.anisotropy > 1.0f && physical.getFeatures().samplerAnisotropy) {
        const float limit = physical.getProperties().limits.maxSamplerAnisotropy;
        info.anisotropyEnable = VK_TRUE;
        info.maxAnisotropy = std::clamp(filtering.anisotropy, 1.0f, std::max(limit, 1.0f));
    }
}

}  // namespace cramion::gfx

#endif  // CRAMION_VK_TEXTURE_FILTERING_H
