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

#include "CramionFX/vk/GBuffer.h"
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

// Hierba de un terreno: millones de briznas generadas cada frame en la GPU
// alrededor de la camara (grass_cull.comp), sin mallas ni instancias en la
// CPU. Crece donde el terreno tiene su capa; mas lejos, menos y mas anchas.
struct GrassDesc {
    bool enabled = false;
    int layer = 0;               // capa del terreno donde crece
    float threshold = 0.25f;     // peso minimo de esa capa
    int dry_layer = 1;           // capa que la vuelve seca (-1 = ninguna)
    float density = 70.0f;       // briznas por m2 (de cerca)
    float max_distance = 80.0f;  // hasta donde se dibuja (m)
    float near_distance = 22.0f; // con todo el detalle hasta aqui
    float height = 0.45f;        // m
    float height_variation = 0.4f;
    float width = 0.028f;        // m (en la base)
    float bend = 0.35f;          // curvatura propia
    core::Vec3 base_color{0.1f, 0.16f, 0.045f};  // sRGB
    core::Vec3 tip_color{0.27f, 0.38f, 0.11f};
    core::Vec3 dry_color{0.5f, 0.45f, 0.25f};
    float color_variation = 0.25f;
    float wind = 1.0f;
    float wind_direction = 30.0f;  // grados (0 = +X)
    float interaction = 1.0f;      // cuanto la apartan los objetos
    std::uint32_t max_blades = 3000000;
};

struct TerrainDesc {
    core::Vec3 origin{};   // esquina (x minima, z minima) y altura 0
    float size = 256.0f;   // metros en X y en Z
    float max_height = 60.0f;
    bool visible = true;
    bool cast_shadows = true;
    float lod_distance = 2.0f;  // mas = mas detalle lejos
    std::vector<TerrainLayerDesc> layers;
    GrassDesc grass;
};

class TerrainPass {
public:
    // Dibujar el G-buffer en lineas (vista Wireframe del editor).
    void setWireframe(bool wireframe) { wireframe_ = wireframe; }
    // `frame_layout`: el set 0 de la geometria (camara, lluvia, clima,
    // decals), que el terreno comparte con los modelos.
    void create(const VulkanDevice& device, const vk::raii::DescriptorSetLayout& frame_layout,
                std::array<vk::Format, GBuffer::kColorAttachmentCount> gbuffer_formats, vk::Format depth_format, vk::Format shadow_format,
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
    // Dentro de una cascada de sombra, o del mapa de una luz puntual/foco
    // (`local`: se descartan los trozos fuera del volumen de la luz).
    void recordShadow(const vk::raii::CommandBuffer& cmd, std::uint32_t frame, const vk::raii::DescriptorSet& frame_set,
                      const core::Mat4& light_view_projection, bool local = false) const;

    std::uint32_t chunkCount() const;

    // --- Hierba ---
    // Lo que aparta la hierba (objetos fisicos, personajes): xyz = centro, w = radio.
    void setGrassInteractors(const std::vector<core::Vec4>& spheres);
    // Fuera de cualquier pase de dibujo, antes del G-buffer: genera las
    // briznas visibles de este frame (compute).
    void recordGrassCull(const vk::raii::CommandBuffer& cmd, std::uint32_t frame, const core::Vec3& camera_position,
                         const core::Mat4& view_projection, float delta_seconds);
    // Dentro del pase de geometria (G-buffer abierto, set 0 del frame).
    void recordGrassGBuffer(const vk::raii::CommandBuffer& cmd, std::uint32_t frame,
                            const vk::raii::DescriptorSet& frame_set) const;

    // Firma de los trozos (y su LOD) que tocan el volumen de una luz local. Si
    // cambia, el mapa cacheado de esa luz ya no coincide con el terreno que ve
    // la camara y hay que redibujarlo.
    std::uint64_t localSignature(const core::Vec3& position, float range) const;

    // --- Para el trazado de rayos: el terreno tambien en la escena de rayos ---
    // Solo con rayos por hardware: se guarda una copia de las alturas y los
    // pesos en la CPU (si no, sobra).
    void setKeepCpuCopy(bool keep) { keep_cpu_copy_ = keep; }
    struct RayTracingSource {
        std::uint32_t id = 0;
        std::uint32_t resolution = 0;
        std::uint32_t splat_resolution = 0;
        core::Vec3 origin{};       // esquina (x, z minimas) y altura 0
        float size = 0.0f;
        float max_height = 0.0f;
        bool visible = true;
        const float* heights = nullptr;        // resolution^2 (0..1)
        const std::uint8_t* splat0 = nullptr;  // splat_resolution^2 x 4
        const std::uint8_t* splat1 = nullptr;
        std::uint32_t layer_count = 0;
        // Color de cada capa como lo pinta terrain.frag (sRGB): la media de su
        // textura por el tinte.
        std::array<core::Vec3, kMaxTerrainLayers> layer_color{};
        std::uint64_t height_revision = 0;  // cambia al esculpir
        std::uint64_t look_revision = 0;    // pesos, texturas, tintes o hierba
        // Hierba de la GPU: donde crece, lo que se ve (y lo que rebota la
        // luz) es el color de las briznas, no la textura del suelo de debajo.
        bool grass = false;
        std::uint32_t grass_layer = 0;
        std::int32_t grass_dry_layer = -1;
        float grass_threshold = 0.25f;
        core::Vec3 grass_color{};      // sRGB: briznas vistas de arriba (base y punta)
        core::Vec3 grass_dry_color{};  // sRGB
    };
    std::vector<RayTracingSource> rayTracingSources() const;

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
    struct Grass {
        VulkanBuffer blades;               // 2 listas (cerca, lejos) de `capacity`
        VulkanBuffer args;                 // 2 comandos de dibujo indirecto
        std::vector<VulkanBuffer> params;  // uno por frame en vuelo
        std::vector<vk::raii::DescriptorSet> sets;
        std::uint32_t capacity = 0;
        bool culled = false;               // se genero este frame
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
        std::unique_ptr<Grass> grass;
        // Copia en la CPU para la escena de rayos (setKeepCpuCopy).
        std::vector<float> cpu_heights;
        std::vector<std::uint8_t> cpu_splat0;
        std::vector<std::uint8_t> cpu_splat1;
        std::array<core::Vec3, kMaxTerrainLayers> layer_average{};  // media de cada textura (sRGB)
        std::uint64_t height_revision = 0;
        std::uint64_t look_revision = 0;
        std::uint64_t look_signature = 0;
    };

    void createPipelines(const VulkanDevice& device, std::array<vk::Format, GBuffer::kColorAttachmentCount> gbuffer_formats,
                         vk::Format depth_format, vk::Format shadow_format);
    void createPatchMesh(const VulkanDevice& device);
    void loadLayerTextures(Terrain& terrain);
    void createArrayTexture(ArrayTexture& texture, const std::vector<std::vector<std::uint8_t>>& layers);
    void writeDescriptors(Terrain& terrain);
    void selectChunks(Terrain& terrain, const core::Vec3& camera, const core::Mat4& view_projection);
    void createGrassPipelines(const VulkanDevice& device, const vk::raii::DescriptorSetLayout& frame_layout,
                              std::array<vk::Format, GBuffer::kColorAttachmentCount> gbuffer_formats,
                              vk::Format depth_format);
    bool ensureGrass(Terrain& terrain);

    const VulkanDevice* device_ = nullptr;
    std::uint32_t frames_ = 2;
    vk::raii::DescriptorSetLayout set_layout_{nullptr};
    vk::raii::PipelineLayout layout_{nullptr};
    vk::raii::Pipeline gbuffer_pipeline_{nullptr};
    vk::raii::Pipeline gbuffer_wire_pipeline_{nullptr};  // vista Wireframe del editor
    bool wireframe_ = false;
    vk::raii::Pipeline shadow_pipeline_{nullptr};
    vk::raii::DescriptorPool pool_{nullptr};
    vk::raii::Sampler clamp_sampler_{nullptr};
    vk::raii::Sampler repeat_sampler_{nullptr};
    VulkanBuffer patch_vertices_;
    VulkanBuffer patch_indices_;
    std::uint32_t patch_index_count_ = 0;
    std::unordered_map<std::uint32_t, std::unique_ptr<Terrain>> terrains_;
    std::uint32_t next_id_ = 1;
    // Camara del ultimo prepare(): decide el LOD tambien en los mapas de las
    // luces locales.
    core::Vec3 lod_camera_{};
    std::vector<Upload> uploads_;
    std::vector<VulkanBuffer> staging_;  // por frame en vuelo
    bool keep_cpu_copy_ = false;

    // Hierba.
    vk::raii::DescriptorSetLayout grass_set_layout_{nullptr};
    vk::raii::PipelineLayout grass_cull_layout_{nullptr};
    vk::raii::Pipeline grass_cull_pipeline_{nullptr};
    vk::raii::PipelineLayout grass_draw_layout_{nullptr};
    vk::raii::Pipeline grass_pipeline_{nullptr};
    vk::raii::DescriptorPool grass_pool_{nullptr};
    std::vector<core::Vec4> grass_interactors_;
    float grass_seconds_ = 0.0f;
    float grass_previous_seconds_ = 0.0f;
};

}  // namespace cramion::gfx

#endif  // CRAMION_VK_TERRAIN_PASS_H
