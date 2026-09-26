#ifndef CRAMION_VK_SKINNED_MODEL_H
#define CRAMION_VK_SKINNED_MODEL_H

#include "CramionFX/asset/Model.h"
#include "CramionFX/core/Math.h"
#include "CramionFX/vk/VulkanBuffer.h"
#include "CramionFX/vk/VulkanCommon.h"
#include "CramionFX/vk/VulkanTexture.h"

#include <array>
#include <cstdint>
#include <vector>

namespace cramion::gfx {

class SkinnedPass;
class VulkanDevice;

// Modelo con esqueleto ya en la GPU: vertices e indices, las texturas y un
// descriptor set por material. Lo comparten todas sus instancias; lo que
// cambia de una a otra (transformacion y huesos) va aparte.
class SkinnedModel {
public:
    struct Material {
        core::Vec4 base_color{1.0f, 1.0f, 1.0f, 1.0f};
        // rgb = emision; w = relieve del parallax (0 = sin mapa de alturas).
        core::Vec4 emissive{0.0f, 0.0f, 0.0f, 0.0f};
        // Bits de GpuSkinnedPush::flags del material (kFlagSpecularMap...).
        std::uint32_t shader_flags = 0;
        // x = metalicidad, y = rugosidad, z = fuerza de la oclusion, w = escala
        // del normal map.
        core::Vec4 params{0.0f, 0.8f, 1.0f, 1.0f};
        // F0 de la parte no metalica.
        float reflectance = 0.04f;
        bool transparent = false;
        // Texturas en textures() (ya con las de por defecto): color base,
        // metal/rugosidad y emision. Las leen los shaders de rayos.
        std::uint32_t albedo_texture = 0;
        std::uint32_t metallic_roughness_texture = 0;
        std::uint32_t emissive_texture = 0;
        // El color base tiene alfa: recortado por alfa (follaje, rejas). Los
        // rayos tienen que probarlo en cada candidato.
        bool alpha_masked = false;
        // Shader de superficie del usuario (-1 = el estandar) y sus
        // propiedades; editables en vivo como el color.
        std::int32_t surface_shader = -1;
        std::array<core::Vec4, 8> surface_params{};
    };

    // Submallas opacas de un mismo material: el culling en GPU escribe sus
    // comandos de dibujo en huecos consecutivos y se dibujan con una sola
    // llamada indirecta.
    struct DrawGroup {
        std::uint32_t material = 0;
        std::uint32_t first_slot = 0;  // dentro de los huecos de este modelo
        std::uint32_t capacity = 0;    // submallas del grupo
    };
    static constexpr std::uint32_t kNoGroup = UINT32_MAX;  // transparente: no se dibuja

    void create(const VulkanDevice& device, const asset::ModelData& model, const SkinnedPass& pass);
    void destroy();

    const VulkanBuffer& vertices() const { return vertices_; }
    const VulkanBuffer& indices() const { return indices_; }
    std::uint32_t indexCount() const { return index_count_; }

    const std::vector<asset::SubMesh>& submeshes() const { return submeshes_; }

    // LODs automaticos (asset::generateLods): nivel 0 = submeshes(). Sus
    // indices van en el mismo buffer, detras de los de LOD0.
    struct Lod {
        std::vector<asset::SubMesh> submeshes;
        std::vector<std::uint32_t> groups;  // grupo de dibujo de cada una (kNoGroup: transparente)
        float error = 0.0f;                 // unidades del modelo
    };
    std::uint32_t lodCount() const { return static_cast<std::uint32_t>(lods_.size()) + 1; }
    const std::vector<Lod>& lods() const { return lods_; }
    const std::vector<asset::SubMesh>& lodSubmeshes(std::uint32_t lod) const {
        return lod == 0 ? submeshes_ : lods_[lod - 1].submeshes;
    }
    std::uint32_t lodSubmeshGroup(std::uint32_t lod, std::uint32_t submesh) const {
        return lod == 0 ? submesh_groups_[submesh] : lods_[lod - 1].groups[submesh];
    }

    // Sin animaciones: la pose no cambia y las cajas de las submallas valen.
    bool rigid() const { return rigid_; }
    const std::vector<Material>& materials() const { return materials_; }
    // Editable en vivo: color, emision y parametros se leen al dibujar
    // (push constants), asi que un cambio se ve en el frame siguiente.
    std::vector<Material>& materials() { return materials_; }
    const vk::raii::DescriptorSet& materialSet(std::uint32_t material) const {
        return material_sets_[material];
    }

    const std::vector<DrawGroup>& drawGroups() const { return draw_groups_; }
    // Grupo de cada submalla (kNoGroup si es transparente).
    std::uint32_t submeshGroup(std::uint32_t submesh) const { return submesh_groups_[submesh]; }
    std::uint32_t slotCount() const { return slot_count_; }

    // Todas las texturas del modelo, con las tres de por defecto al final.
    const std::vector<VulkanTexture>& textures() const { return textures_; }

private:
    VulkanBuffer vertices_;
    VulkanBuffer indices_;
    std::uint32_t index_count_ = 0;
    bool rigid_ = false;

    std::vector<asset::SubMesh> submeshes_;
    std::vector<Lod> lods_;
    std::vector<Material> materials_;
    std::vector<DrawGroup> draw_groups_;
    std::vector<std::uint32_t> submesh_groups_;
    std::uint32_t slot_count_ = 0;

    // Texturas del modelo; al final, tres texeles por defecto para los mapas
    // que falten (blanco, normal plana y negro), asi el shader no necesita
    // ramas aparte.
    std::vector<VulkanTexture> textures_;

    // El pool debe sobrevivir a los sets: se declara antes.
    vk::raii::DescriptorPool pool_{nullptr};
    std::vector<vk::raii::DescriptorSet> material_sets_;
};

}  // namespace cramion::gfx

#endif  // CRAMION_VK_SKINNED_MODEL_H
