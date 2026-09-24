#ifndef CRAMION_VK_SKINNED_PASS_H
#define CRAMION_VK_SKINNED_PASS_H

#include "CramionFX/vk/VulkanCommon.h"

namespace cramion::gfx {

class GBuffer;
class VulkanDevice;

// Pipelines de los modelos con esqueleto (animacion por skinning en la GPU).
//
// Los huesos van en un STORAGE buffer, no en un uniform buffer:
//
//   layout(std430) readonly buffer Bones { mat4 bones[]; };
//
// Un uniform buffer obliga a fijar el tamano del array en el shader (y tiene
// un limite de 64 KB, unas 1000 matrices en el mejor caso). El storage buffer
// es un array de longitud variable limitado solo por la memoria: un modelo
// puede tener tantos huesos como quiera, y todos los modelos de la escena
// comparten el mismo buffer, cada instancia a partir de su `bone_offset`.
//
// Recursos:
//   set 0, binding 0 -> uniform buffer de camara
//   set 0, binding 1 -> storage buffer de matrices de hueso (por frame)
//   set 1, binding 0 -> albedo (color base)
//   set 1, binding 1 -> metal/rugosidad (glTF: B = metal, G = rugosidad)
//   set 1, binding 2 -> normal map (espacio tangente)
//   set 1, binding 3 -> oclusion ambiental
//   set 1, binding 4 -> emision
//   push constant    -> modelo, color base y primer hueso de la instancia
//
// Hay tres pipelines: la de G-buffer y dos de sombras, una para las cascadas
// (con depth clamp) y otra para las luces locales (sin el). Las de sombras
// recortan por alfa con la textura del material (hojas, rejas).
//
// Y una cuarta para el vidrio (glass.frag): forward, sobre la imagen HDR ya
// iluminada, con el depth del G-buffer solo para la prueba. Su set 2 trae lo
// que necesita para reflejar la escena (ver glassSetLayout).
class SkinnedPass {
public:
    void create(const VulkanDevice& device, const GBuffer& gbuffer, vk::Format shadow_format,
                vk::Format hdr_format);
    void destroy();

    const vk::raii::DescriptorSetLayout& frameSetLayout() const { return frame_set_layout_; }
    // Texturas por material, en el orden de los bindings del set 1.
    static constexpr std::uint32_t kMaterialTextureCount = 5;

    const vk::raii::DescriptorSetLayout& materialSetLayout() const { return material_set_layout_; }
    const vk::raii::Sampler& sampler() const { return sampler_; }

    const vk::raii::Pipeline& geometryPipeline() const { return geometry_pipeline_; }
    const vk::raii::PipelineLayout& geometryLayout() const { return geometry_layout_; }

    const vk::raii::Pipeline& shadowPipeline() const { return shadow_pipeline_; }
    const vk::raii::Pipeline& localShadowPipeline() const { return local_shadow_pipeline_; }
    const vk::raii::PipelineLayout& shadowLayout() const { return shadow_layout_; }

    // Set 2 del vidrio: 0 camara, 1 luces, 2 cascadas (datos), 3 mapa de las
    // cascadas, 4 profundidad, 5 imagen HDR sin vidrio, 6 entorno IBL,
    // 7 y 8 cubos de la sonda de reflexion.
    static constexpr std::uint32_t kGlassBindingCount = 9;
    const vk::raii::DescriptorSetLayout& glassSetLayout() const { return glass_set_layout_; }
    const vk::raii::Pipeline& glassPipeline() const { return glass_pipeline_; }
    const vk::raii::PipelineLayout& glassLayout() const { return glass_layout_; }

private:
    void createGeometryPipeline(const VulkanDevice& device, const GBuffer& gbuffer);
    void createGlassPipeline(const VulkanDevice& device, vk::Format color_format,
                             vk::Format depth_format);
    vk::raii::Pipeline createShadowPipeline(const VulkanDevice& device, vk::Format depth_format,
                                            bool depth_clamp, float slope_bias) const;

    vk::raii::Sampler sampler_{nullptr};
    vk::raii::DescriptorSetLayout frame_set_layout_{nullptr};
    vk::raii::DescriptorSetLayout material_set_layout_{nullptr};

    vk::raii::PipelineLayout geometry_layout_{nullptr};
    vk::raii::Pipeline geometry_pipeline_{nullptr};

    vk::raii::PipelineLayout shadow_layout_{nullptr};
    vk::raii::Pipeline shadow_pipeline_{nullptr};
    vk::raii::Pipeline local_shadow_pipeline_{nullptr};

    vk::raii::DescriptorSetLayout glass_set_layout_{nullptr};
    vk::raii::PipelineLayout glass_layout_{nullptr};
    vk::raii::Pipeline glass_pipeline_{nullptr};
};

}  // namespace cramion::gfx

#endif  // CRAMION_VK_SKINNED_PASS_H
