#ifndef CRAMION_VK_FOLIAGE_PASS_H
#define CRAMION_VK_FOLIAGE_PASS_H

// Vegetacion instanciada (como el Foliage / HISM de Unreal): millones de
// arboles sin una entidad ni una llamada de dibujo por arbol.
//
//   - Las instancias (16 bytes: posicion + giro, escala, especie y tono) se
//     suben UNA vez a la GPU.
//   - Cada frame un compute shader (foliage_cull.comp) recorre todas: descarta
//     las que estan fuera del campo de vision o demasiado lejos y elige su
//     nivel de detalle por distancia (0 cerca, 1 media, 2 lejos). Escribe los
//     indices visibles en una lista por (especie x nivel) y cuenta cuantas hay
//     en los comandos de dibujo indirecto.
//   - Se dibuja con UNA llamada indirecta instanciada por lista (9 en total)
//     en el G-buffer: reciben toda la iluminacion diferida (sol con sombras,
//     cielo, SSAO, GI, niebla...). Las cercanas tambien proyectan sombra (una
//     lista mas por especie, con el nivel medio).
//   - Las mallas de las especies (pino, roble y abedul, en 3 niveles) se
//     generan por codigo: color por vertice, oclusion en la copa y peso del
//     viento (las copas se mecen).

#include "CramionFX/asset/TreeGenerator.h"
#include "CramionFX/vk/GBuffer.h"
#include "CramionFX/core/Math.h"
#include "CramionFX/vk/VulkanBuffer.h"
#include "CramionFX/vk/VulkanCommon.h"

#include <array>
#include <cstdint>
#include <vector>

namespace cramion::gfx {

class VulkanDevice;

// Una instancia tal como la lee la GPU (std430, 16 bytes).
struct FoliageInstance {
    float x = 0.0f, y = 0.0f, z = 0.0f;
    // bits 0-9 giro (0..1023 -> 0..2pi), 10-17 escala (0..255 -> 0.25..4),
    // 18-19 especie, 20-27 tono (variacion de color 0..255).
    std::uint32_t packed = 0;

    static FoliageInstance make(const core::Vec3& position, float yaw_radians, float scale, std::uint32_t species,
                                float tint);
};
static_assert(sizeof(FoliageInstance) == 16, "FoliageInstance debe coincidir con foliage_cull.comp");

struct FoliageSettings {
    float lod1_distance = 120.0f;   // mas alla: malla media
    float lod2_distance = 450.0f;   // mas alla: malla lejana
    float max_distance = 3000.0f;   // mas alla no se dibujan
    float shadow_distance = 140.0f; // proyectan sombra hasta aqui
    bool cast_shadows = true;
    float wind = 1.0f;              // 0 = quietos
};

struct FoliageStats {
    std::uint64_t instances = 0;
    std::uint64_t memory_bytes = 0;
    // Del frame que acaba de terminar en la GPU (van un par de frames tarde).
    std::uint32_t visible[3] = {0, 0, 0};  // por nivel de detalle
    std::uint32_t shadow_casters = 0;
    std::uint64_t triangles = 0;
};

class FoliagePass {
public:
    // Dibujar el G-buffer en lineas (vista Wireframe del editor).
    void setWireframe(bool wireframe) { wireframe_ = wireframe; }
    static constexpr std::uint32_t kSpecies = 3;
    static constexpr std::uint32_t kLists = 4;  // 3 niveles + sombras
    static constexpr std::uint32_t kMaxInstances = 8'000'000;

    void create(const VulkanDevice& device, const vk::raii::DescriptorSetLayout& frame_layout,
                std::array<vk::Format, GBuffer::kColorAttachmentCount> gbuffer_formats, vk::Format depth_format, vk::Format shadow_format,
                std::uint32_t frames_in_flight);
    void destroy();

    // Todas las instancias (reemplaza las que habia). Espera a la GPU.
    void setInstances(const std::vector<FoliageInstance>& instances);
    void clear() { setInstances({}); }
    void setSettings(const FoliageSettings& settings) { settings_ = settings; }
    // Las 3 especies (arboles procedurales). Si cambian, se rehacen sus mallas
    // (espera a la GPU).
    void setSpecies(const std::array<asset::TreeSpecies, kSpecies>& species);
    const std::array<asset::TreeSpecies, kSpecies>& species() const { return species_; }
    const FoliageSettings& settings() const { return settings_; }
    bool empty() const { return instance_count_ == 0; }

    // Antes de las sombras y fuera de cualquier render pass: recorte y niveles.
    void recordCull(const vk::raii::CommandBuffer& cmd, std::uint32_t frame, const core::Vec3& camera_position,
                    const core::Mat4& view_projection, float delta_seconds);
    void recordGBuffer(const vk::raii::CommandBuffer& cmd, std::uint32_t frame,
                       const vk::raii::DescriptorSet& frame_set) const;
    void recordShadow(const vk::raii::CommandBuffer& cmd, std::uint32_t frame, const vk::raii::DescriptorSet& frame_set,
                      const core::Mat4& light_view_projection) const;

    FoliageStats stats() const { return stats_; }
    // Origen flotante: las instancias estan en coordenadas absolutas; el
    // programa dice cada frame donde esta el origen del mundo y los shaders lo
    // restan (desplazar el mundo no obliga a sembrar de nuevo).
    void setOrigin(const core::Vec3& origin) { origin_offset_ = origin; }

private:
    struct Mesh {
        std::uint32_t first_index = 0;
        std::uint32_t index_count = 0;
        std::int32_t vertex_offset = 0;
    };

    void createPipelines(const VulkanDevice& device, std::array<vk::Format, GBuffer::kColorAttachmentCount> gbuffer_formats,
                         vk::Format depth_format, vk::Format shadow_format);
    void buildMeshes();
    void createTextures();
    struct TextureArray {
        vk::raii::DeviceMemory memory{nullptr};
        vk::raii::Image image{nullptr};
        vk::raii::ImageView view{nullptr};
    };
    void writeSets();
    void draw(const vk::raii::CommandBuffer& cmd, std::uint32_t frame, bool shadow) const;

    const VulkanDevice* device_ = nullptr;
    std::uint32_t frames_ = 2;
    FoliageSettings settings_{};
    FoliageStats stats_{};

    // Culling (computo).
    vk::raii::DescriptorSetLayout cull_set_layout_{nullptr};
    vk::raii::PipelineLayout cull_layout_{nullptr};
    vk::raii::Pipeline cull_pipeline_{nullptr};
    // Dibujo.
    vk::raii::DescriptorSetLayout draw_set_layout_{nullptr};
    vk::raii::PipelineLayout draw_layout_{nullptr};
    vk::raii::Pipeline gbuffer_pipeline_{nullptr};
    vk::raii::Pipeline gbuffer_wire_pipeline_{nullptr};  // vista Wireframe del editor
    bool wireframe_ = false;
    vk::raii::Pipeline shadow_pipeline_{nullptr};
    vk::raii::DescriptorPool pool_{nullptr};
    std::vector<vk::raii::DescriptorSet> cull_sets_;
    std::vector<vk::raii::DescriptorSet> draw_sets_;

    VulkanBuffer vertices_;
    VulkanBuffer indices_;
    VulkanBuffer bounds_;  // vec4 por especie: y del centro, radio, altura
    std::array<asset::TreeSpecies, kSpecies> species_{};
    TextureArray albedo_array_;
    TextureArray normal_array_;
    vk::raii::Sampler texture_sampler_{nullptr};
    std::array<Mesh, kSpecies * 3> meshes_{};  // especie * 3 + nivel
    std::array<std::uint32_t, kSpecies * 3> triangles_{};

    VulkanBuffer instances_;
    std::vector<VulkanBuffer> visible_;   // por frame: 4 listas por especie
    std::vector<VulkanBuffer> commands_;  // por frame: 12 comandos de dibujo indirecto
    std::vector<VulkanBuffer> readback_;  // por frame: los comandos (cuantos se dibujaron)
    std::vector<bool> readback_ready_;
    std::uint32_t instance_count_ = 0;
    std::array<std::uint32_t, kSpecies> species_count_{};
    std::array<std::uint32_t, kSpecies * kLists> template_first_instance_{};

    core::Vec3 origin_offset_{};
    float time_ = 0.0f;
    float last_delta_ = 0.0f;  // segundos del ultimo frame (viento del frame anterior, vectores de movimiento)
};

}  // namespace cramion::gfx

#endif  // CRAMION_VK_FOLIAGE_PASS_H
