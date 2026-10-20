#ifndef CRAMION_VK_SKINNED_MODEL_H
#define CRAMION_VK_SKINNED_MODEL_H

#include "CramionFX/asset/Model.h"
#include "CramionFX/core/Math.h"
#include "CramionFX/vk/VulkanBuffer.h"
#include "CramionFX/vk/VulkanCommon.h"
#include "CramionFX/vk/VulkanTexture.h"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace cramion::gfx {

class SkinnedPass;
class UploadBatch;
class UploadFence;
class VulkanDevice;

// Recarga en caliente: el archivo `path` (ruta absoluta o relativa, como
// aparece en TextureData::source_path o en una receta) cambio en disco. Las
// texturas perezosas que lo usan dejan de compartirse con las ya subidas: el
// siguiente SkinnedModel::create lo vuelve a leer. Cualquier hilo.
void invalidateSharedTextureFile(const std::string& path);

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
        // Modelo de sombreado de Disney empaquetado para push.pick_id (bits
        // 0-3 modelo, 4-7 mezcla, sin luz y color de vertice, 8-31 tres
        // parametros de 8 bits; ver shadingBits).
        std::uint32_t shading = 0;
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
        // rayos tienen que probarlo en cada candidato. Tambien si el alfa del
        // color base (el factor) baja de 0.5: el pase principal lo recorta.
        bool alpha_masked = false;
        // La textura del color base tiene alfa (para rehacer alpha_masked si
        // el factor cambia en vivo).
        bool texture_alpha = false;
        // El color base no tiene recortes (sin alfa en su textura ni Render
        // Texture de alfa desconocido): con base_color.w = 1 se dibuja con
        // la pipeline sin recorte (SkinnedPass::geometryOpaquePipeline, early-Z).
        bool opaque_albedo = false;
        // Shader de superficie del usuario (-1 = el estandar) y sus
        // propiedades; editables en vivo como el color.
        std::int32_t surface_shader = -1;
        std::array<core::Vec4, 8> surface_params{};
        // Tiling de las UV: xy = repeticion, zw = desplazamiento (lo mismo que
        // el binding SkinnedPass::kMaterialUvBinding de su set). Lo leen los
        // rayos.
        core::Vec4 uv_transform{1.0f, 1.0f, 0.0f, 0.0f};
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

    // `texture_lod`: mips grandes que no se suben (0 = todo; 1 = la mitad de
    // lado, 2 = un cuarto...): streaming de texturas por distancia.
    // Todas las copias del modelo (mallas, texturas, meshlets) van en un solo
    // lote (UploadBatch: un command buffer y una fence; con cola de
    // transferencia si la GPU la tiene). `wait_for_gpu`: esperar aqui a que
    // termine (como antes); false = no bloquear y mirar uploadReady() antes de
    // dibujarlo (hilos del streaming).
    void create(const VulkanDevice& device, const asset::ModelData& model, const SkinnedPass& pass, int texture_lod = 0,
                bool wait_for_gpu = true);
    int textureLod() const { return texture_lod_; }
    // La GPU termino de copiar todo lo suyo (y lo compartido que subio otro
    // modelo). No bloquea. Un modelo vacio o ya esperado: true.
    bool uploadReady() const;
    // Bloquea hasta uploadReady().
    void waitUpload() const;

    // Bits del relieve en GpuSkinnedPush::flags (parallax o teselado,
    // auto-sombra y teselacion maxima) sobre `flags`. `has_height`: el
    // material tiene mapa de alturas; `tessellation`: la GPU tesela.
    static std::uint32_t reliefFlags(std::uint32_t flags, const asset::MaterialData& material, bool has_height,
                                     bool tessellation);
    // El modelo de Disney del material, como lo leen skinned.frag y glass.frag.
    // Bits 0-3: modelo (8 = sin luz, opaco); 4-5: mezcla de lo transparente
    // (glass.frag: 0 vidrio, 1 alfa, 2 aditivo); 6: transparente sin luz; 7: el
    // color de vertice multiplica al color base; 8-31: los tres parametros.
    static constexpr int kShadingUnlit = 8;
    static constexpr std::uint32_t kBlendShift = 4;
    static constexpr std::uint32_t kShadingUnlitBit = 1u << 6;
    static constexpr std::uint32_t kShadingVertexColorBit = 1u << 7;
    static std::uint32_t shadingBits(const asset::MaterialData& material);
    void destroy();

    const VulkanBuffer& vertices() const { return vertices_ ? *vertices_ : emptyBuffer(); }
    const VulkanBuffer& indices() const { return indices_ ? *indices_ : emptyBuffer(); }
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
    // Tiene UV de lightmap (set 1, binding 10; sus materiales llevan
    // GpuSkinnedPush::kFlagLightmap).
    bool hasLightmapUvs() const { return lightmap_uvs_ != nullptr; }
    bool hasVertexColors() const { return vertex_colors_ != nullptr; }
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
    const std::vector<std::shared_ptr<VulkanTexture>>& textures() const { return textures_; }

    // Huecos de material que leen un Render Texture (material, binding, id).
    struct RenderTextureRef {
        std::uint32_t material = 0;
        std::uint32_t binding = 0;
        std::int32_t texture = -1;
    };
    const std::vector<RenderTextureRef>& renderTextureRefs() const { return render_texture_refs_; }
    // Cambia la imagen de un hueco (nullptr = la de por defecto: blanca en el
    // color, negra en la emision). Sin frames en vuelo que usen el set.
    void setMaterialImage(const VulkanDevice& device, const SkinnedPass& pass, std::uint32_t material,
                          std::uint32_t binding, vk::ImageView view);
    // Vuelve a escribir todos los sets de material con el sampler actual de la
    // pasada (otro filtrado de texturas: SkinnedPass::setTextureFiltering). Sin
    // frames en vuelo que usen los sets.
    void refreshSampler(const VulkanDevice& device, const SkinnedPass& pass);
    // La revision del sampler de la pasada con la que se escribieron los sets
    // (SkinnedPass::samplerRevision; uno creado en otro hilo antes de cambiar
    // el filtrado trae la de antes: installModel lo rehace).
    std::uint64_t materialSamplerRevision() const { return material_sampler_revision_; }

    // --- Meshlets (mesh shaders) ---
    // Cada submalla (cluster) de cada LOD partida en meshlets de hasta 64
    // vertices y 124 triangulos (meshoptimizer), con su esfera y su cono de
    // normales. Vacio sin mesh shaders o en modelos animados.
    struct MeshletRange {
        std::uint32_t first = 0;
        std::uint32_t count = 0;
    };
    bool hasMeshlets() const { return !meshlet_ranges_.empty(); }
    // Rango de cada submalla de un LOD (count 0: transparente).
    const std::vector<MeshletRange>& meshletRanges(std::uint32_t lod) const { return meshlet_ranges_[lod]; }
    // Malla cerrada (cada arista entre exactamente dos triangulos): se pueden
    // descartar los meshlets que miran hacia atras.
    bool closed(std::uint32_t lod) const { return lod < closed_.size() && closed_[lod]; }
    const vk::raii::DescriptorSet& meshletSet() const { return meshlet_sets_[0]; }
    // Meshlets del cluster mas grande (el task shader del G-buffer lleva hasta 256).
    std::uint32_t maxClusterMeshlets() const { return max_cluster_meshlets_; }

private:
    void buildMeshlets(const VulkanDevice& device, const asset::ModelData& model, const SkinnedPass& pass,
                       UploadBatch& batch);
    static const VulkanBuffer& emptyBuffer();

    // Fin de las copias del modelo (nulo: ya terminaron). Se suelta al
    // comprobarlo (mutable: uploadReady es const).
    mutable std::shared_ptr<UploadFence> upload_;

    // Compartidos entre modelos con la misma geometria (cache por contenido en
    // SkinnedModel.cpp): las variantes de material (.crmat) de un modelo usan
    // los mismos vertices e indices que el original. Antes cada variante subia
    // su copia y una escena con materiales propios ocupaba el doble de VRAM.
    std::shared_ptr<VulkanBuffer> vertices_;
    std::shared_ptr<VulkanBuffer> indices_;
    std::shared_ptr<VulkanBuffer> lightmap_uvs_;  // unorm16x2 por vertice (o nada)
    std::shared_ptr<VulkanBuffer> vertex_colors_;  // RGBA8 por vertice (o nada)
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
    // ramas aparte. Compartidas: los modelos que usan la misma imagen (las
    // casas de un pueblo con sus materiales comunes) la tienen una sola vez
    // en la GPU (cache por contenido en SkinnedModel.cpp).
    std::vector<std::shared_ptr<VulkanTexture>> textures_;
    int texture_lod_ = 0;
    std::size_t white_index_ = 0;
    std::size_t black_index_ = 0;
    std::vector<RenderTextureRef> render_texture_refs_;

    // Tiling de las UV de cada material (un vec4 alineado por material; el de
    // identidad, compartido por todos los modelos sin tiling).
    std::shared_ptr<VulkanBuffer> uv_buffer_;
    // El pool debe sobrevivir a los sets: se declara antes.
    vk::raii::DescriptorPool pool_{nullptr};
    std::vector<vk::raii::DescriptorSet> material_sets_;
    // La imagen de cada hueco de cada set (para reescribirlos con otro sampler).
    std::vector<std::vector<vk::ImageView>> material_views_;
    std::uint64_t material_sampler_revision_ = 0;

    // (Compartidos con el lote de subida, que los mantiene vivos hasta que la
    // GPU termina de copiarlos aunque el modelo se descarte antes.)
    std::shared_ptr<VulkanBuffer> meshlets_;
    std::shared_ptr<VulkanBuffer> meshlet_vertices_;
    std::shared_ptr<VulkanBuffer> meshlet_triangles_;
    std::vector<std::vector<MeshletRange>> meshlet_ranges_;  // [lod][submalla]
    std::vector<bool> closed_;                               // [lod]
    std::uint32_t max_cluster_meshlets_ = 0;
    vk::raii::DescriptorPool meshlet_pool_{nullptr};
    std::vector<vk::raii::DescriptorSet> meshlet_sets_;
};

}  // namespace cramion::gfx

#endif  // CRAMION_VK_SKINNED_MODEL_H
