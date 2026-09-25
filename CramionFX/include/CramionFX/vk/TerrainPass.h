#ifndef CRAMION_VK_TERRAIN_PASS_H
#define CRAMION_VK_TERRAIN_PASS_H

// Terrenos de mapa de alturas (como el Landscape de Unreal), dibujados en el
// G-buffer y en las cascadas de sombra.
//
//   - Alturas en una textura R32F (0..1, por la altura maxima) que lee el
//     vertex shader: esculpir solo sube el trozo cambiado.
//   - Geometria: una malla de trozo (32 x 32 celdas con faldon) repetida con
//     LOD por distancia (quadtree): cerca, una celda por texel; lejos, menos.
//     Los faldones tapan las grietas entre trozos de distinto LOD.
//   - Hasta 8 capas de textura (color + normal map, tamano de repeticion,
//     rugosidad, metal) mezcladas con dos mapas de pesos RGBA8 (splat). Sin
//     textura, la capa es su color con una variacion procedural.
//   - La superficie pasa por gbuffer_surface.glsl: lluvia, charcos, humedad y
//     decals como cualquier suelo.
//
// Las subidas (alturas, pesos) se graban en el command buffer del frame, con
// un staging por frame en vuelo: esculpir no para la GPU.

#include "CramionFX/core/Math.h"
#include "CramionFX/vk/VulkanBuffer.h"
#include "CramionFX/vk/VulkanCommon.h"
#include "CramionFX/vk/VulkanImage.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <unordered_map>
#include <vector>

namespace cramion::gfx {

class VulkanDevice;

inline constexpr std::uint32_t kMaxTerrainLayers = 8;

struct TerrainLayerDesc {
    std::filesystem::path albedo;  // vacia = sin textura (el color `tint`)
    std::filesystem::path normal;  // vacia = plano
    float tiling = 8.0f;           // metros que ocupa una repeticion de la textura
    float roughness = 0.85f;
    float metallic = 0.0f;
    float normal_strength = 1.0f;
    core::Vec3 tint{1.0f, 1.0f, 1.0f};  // multiplica el color (sRGB)
};

struct TerrainDesc {
    core::Vec3 origin{};   // esquina (x minima, z minima) y altura 0
    float size = 256.0f;   // metros en X y en Z
    float max_height = 60.0f;
    bool visible = true;
    bool cast_shadows = true;
    float lod_distance = 2.0f;  // mas = mas detalle lejos
    std::vector<TerrainLayerDesc> layers;
};

class TerrainPass {
public:
    // `frame_layout`: el set 0 de la geometria (camara, lluvia, clima,
    // decals), que el terreno comparte con los modelos.
    void create(const VulkanDevice& device, const vk::raii::DescriptorSetLayout& frame_layout,
                std::array<vk::Format, 4> gbuffer_formats, vk::Format depth_format, vk::Format shadow_format,
                std::uint32_t frames_in_flight);
    void destroy();

    // --- Terrenos ---
    std::uint32_t createTerrain(std::uint32_t resolution, std::uint32_t splat_resolution);
    void destroyTerrain(std::uint32_t id);
    void setDesc(std::uint32_t id, const TerrainDesc& desc);
    // Sube una region (x, y, ancho, alto en texeles) de las alturas completas
    // (resolution^2 valores 0..1) / de los pesos (splat_resolution^2 * 4 bytes
    // por mapa). Se graba en el siguiente frame.
    void updateHeights(std::uint32_t id, const float* heights, std::uint32_t x, std::uint32_t y, std::uint32_t w,
                       std::uint32_t h);
    void updateSplat(std::uint32_t id, const std::uint8_t* splat0, const std::uint8_t* splat1, std::uint32_t x,
                     std::uint32_t y, std::uint32_t w, std::uint32_t h);
    bool empty() const { return terrains_.empty(); }

    // Subidas pendientes al command buffer del frame. true si cambiaron
    // alturas (las sombras cacheadas hay que rehacerlas).
    bool recordUploads(const vk::raii::CommandBuffer& cmd, std::uint32_t frame);

    // Elige los trozos de cada terreno (LOD por distancia a la camara y
    // recorte por el frustum). Una vez por frame antes de dibujar.
    void prepare(std::uint32_t frame, const core::Vec3& camera_position, const core::Mat4& view_projection);

    // Dentro del pase de geometria (G-buffer abierto, set 0 del frame).
    void recordGBuffer(const vk::raii::CommandBuffer& cmd, std::uint32_t frame,
                       const vk::raii::DescriptorSet& frame_set) const;
    // Dentro de una cascada de sombra.
    void recordShadow(const vk::raii::CommandBuffer& cmd, std::uint32_t frame, const vk::raii::DescriptorSet& frame_set,
                      const core::Mat4& light_view_projection) const;

    std::uint32_t chunkCount() const;

private:
    struct Upload {
        std::uint32_t id = 0;
        int kind = 0;  // 0 alturas, 1 pesos
        std::uint32_t x = 0, y = 0, w = 0, h = 0;
        std::vector<std::uint8_t> bytes;   // alturas: floats; pesos: splat0 y luego splat1
    };
    struct Chunk {
        float u = 0.0f, v = 0.0f, size = 1.0f, skirt = 1.0f;
    };
    // Array 2D de texturas con mipmaps (las capas).
    struct ArrayTexture {
        vk::raii::DeviceMemory memory{nullptr};
        vk::raii::Image image{nullptr};
        vk::raii::ImageView view{nullptr};
    };
    struct Terrain {
        std::uint32_t resolution = 0;
        std::uint32_t splat_resolution = 0;
        TerrainDesc desc;
        VulkanImage heights;
        VulkanImage splat0;
        VulkanImage splat1;
        ArrayTexture albedo_array;
        ArrayTexture normal_array;
        std::array<std::filesystem::path, kMaxTerrainLayers> albedo_paths{};
        std::array<std::filesystem::path, kMaxTerrainLayers> normal_paths{};
        bool arrays_ready = false;
        std::vector<VulkanBuffer> params;  // uno por frame en vuelo
        std::vector<vk::raii::DescriptorSet> sets;
        std::vector<Chunk> chunks;
    };

    void createPipelines(const VulkanDevice& device, std::array<vk::Format, 4> gbuffer_formats,
                         vk::Format depth_format, vk::Format shadow_format);
    void createPatchMesh(const VulkanDevice& device);
    void loadLayerTextures(Terrain& terrain);
    void createArrayTexture(ArrayTexture& texture, const std::vector<std::vector<std::uint8_t>>& layers);
    void writeDescriptors(Terrain& terrain);
    void selectChunks(Terrain& terrain, const core::Vec3& camera, const core::Mat4& view_projection);

    const VulkanDevice* device_ = nullptr;
    std::uint32_t frames_ = 2;
    vk::raii::DescriptorSetLayout set_layout_{nullptr};
    vk::raii::PipelineLayout layout_{nullptr};
    vk::raii::Pipeline gbuffer_pipeline_{nullptr};
    vk::raii::Pipeline shadow_pipeline_{nullptr};
    vk::raii::DescriptorPool pool_{nullptr};
    vk::raii::Sampler clamp_sampler_{nullptr};
    vk::raii::Sampler repeat_sampler_{nullptr};
    VulkanBuffer patch_vertices_;
    VulkanBuffer patch_indices_;
    std::uint32_t patch_index_count_ = 0;
    std::unordered_map<std::uint32_t, std::unique_ptr<Terrain>> terrains_;
    std::uint32_t next_id_ = 1;
    std::vector<Upload> uploads_;
    std::vector<VulkanBuffer> staging_;  // por frame en vuelo
};

}  // namespace cramion::gfx

#endif  // CRAMION_VK_TERRAIN_PASS_H
