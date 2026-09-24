#ifndef CRAMION_VK_LIGHTING_PASS_H
#define CRAMION_VK_LIGHTING_PASS_H

#include "vk/VulkanCommon.h"

namespace cramion::gfx {

class VulkanDevice;

// Segunda pasada del renderizador diferido: un triangulo a pantalla completa
// que lee el G-buffer y acumula todas las luces sobre la imagen final.
//
// Recursos que usa:
//   set 0, binding 0 -> uniform buffer de camara
//   set 0, binding 1 -> G-buffer albedo
//   set 0, binding 2 -> G-buffer normales
//   set 0, binding 3 -> G-buffer profundidad (de ahi sale la posicion)
//   set 0, binding 4 -> uniform buffer de luces
//   set 0, binding 5 -> array de mapas de sombra en cascada
//   set 0, binding 6 -> uniform buffer de las cascadas
//   set 0, binding 7 -> array de mapas de sombra de los focos
//   set 0, binding 8 -> array de mapas de sombra de las luces puntuales
//   set 0, binding 9 -> uniform buffer de las sombras locales
//   set 0, binding 10 -> oclusion ambiental de pantalla (SSAO)
//   set 0, binding 11 -> LUT del cielo fisico
//   set 0, binding 12 -> G-buffer de material (emision + metalicidad)
//   set 0, binding 13 -> IBL: entorno prefiltrado por rugosidad (cubo)
//   set 0, binding 14 -> IBL: LUT de la BRDF (split-sum)
//   set 0, binding 15 -> IBL: irradiancia difusa en armonicos esfericos
//   set 0, binding 16 -> iluminacion global de pantalla (SSGI)
//   set 0, binding 17 -> reflejos de pantalla (SSR)
//   set 0, binding 18 -> sonda de reflexion de la escena (cubo)
//
// Escribe HDR lineal: el tono y la gamma van en la composicion final.
class LightingPass {
public:
    // `color_format` es el formato de la imagen HDR intermedia.
    void create(const VulkanDevice& device, vk::Format color_format);
    void destroy();

    const vk::raii::Pipeline& pipeline() const { return pipeline_; }
    const vk::raii::PipelineLayout& layout() const { return pipeline_layout_; }
    const vk::raii::DescriptorSetLayout& descriptorSetLayout() const { return set_layout_; }

    // Muestreador compartido por los tres destinos del G-buffer.
    const vk::raii::Sampler& sampler() const { return sampler_; }

private:
    vk::raii::Sampler sampler_{nullptr};
    vk::raii::DescriptorSetLayout set_layout_{nullptr};
    vk::raii::PipelineLayout pipeline_layout_{nullptr};
    vk::raii::Pipeline pipeline_{nullptr};
};

}  // namespace cramion::gfx

#endif  // CRAMION_VK_LIGHTING_PASS_H
