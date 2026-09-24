#ifndef CRAMION_VK_SKINNED_MODEL_H
#define CRAMION_VK_SKINNED_MODEL_H

#include "asset/Model.h"
#include "core/Math.h"
#include "vk/VulkanBuffer.h"
#include "vk/VulkanCommon.h"
#include "vk/VulkanTexture.h"

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
        core::Vec4 emissive{0.0f, 0.0f, 0.0f, 0.0f};
        // x = metalicidad, y = rugosidad, z = fuerza de la oclusion, w = escala
        // del normal map.
        core::Vec4 params{0.0f, 0.8f, 1.0f, 1.0f};
        bool transparent = false;
    };

    void create(const VulkanDevice& device, const asset::ModelData& model, const SkinnedPass& pass);
    void destroy();

    const VulkanBuffer& vertices() const { return vertices_; }
    const VulkanBuffer& indices() const { return indices_; }
    std::uint32_t indexCount() const { return index_count_; }

    const std::vector<asset::SubMesh>& submeshes() const { return submeshes_; }

    // Sin animaciones: la pose no cambia y las cajas de las submallas valen.
    bool rigid() const { return rigid_; }
    const std::vector<Material>& materials() const { return materials_; }
    const vk::raii::DescriptorSet& materialSet(std::uint32_t material) const {
        return material_sets_[material];
    }

private:
    VulkanBuffer vertices_;
    VulkanBuffer indices_;
    std::uint32_t index_count_ = 0;
    bool rigid_ = false;

    std::vector<asset::SubMesh> submeshes_;
    std::vector<Material> materials_;

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
