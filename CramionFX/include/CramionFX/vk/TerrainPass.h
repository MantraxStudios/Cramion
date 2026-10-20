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
// un staging fijo por frame en vuelo y un tope de bytes por frame (las
// grandes se parten en bandas de filas): esculpir no para la GPU.
//
// Streaming de teselas sin tirones: prepareTerrainAsync (en un hilo de
// trabajo) crea las imagenes y envia su copia con submitAsync; el hilo
// principal solo las adopta (adoptTerrain: sets de descriptores y alta en el
// mapa) cuando la GPU termino. Nunca se espera a la GPU en el frame.

#include "CramionFX/vk/GBuffer.h"
#include "CramionFX/core/Math.h"
#include "CramionFX/vk/FrameBudget.h"
#include "CramionFX/vk/TextureFiltering.h"
#include "CramionFX/vk/VulkanBuffer.h"
#include "CramionFX/vk/VulkanCommon.h"
#include "CramionFX/vk/VulkanImage.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace cramion::gfx {

class VulkanDevice;
class GpuTicket;

// Alturas y pesos de un terreno, inmutables y compartidos (sin copiar) con
// quien los genero (el snapshot de CramionCore: CramionFX no depende de el).
struct TerrainPixels {
    std::uint32_t resolution = 0;
    std::uint32_t splat_resolution = 0;
    std::shared_ptr<const std::vector<float>> heights;         // resolution^2 (0..1)
    std::shared_ptr<const std::vector<std::uint8_t>> splat0;   // splat_resolution^2 x 4
    std::shared_ptr<const std::vector<std::uint8_t>> splat1;
    std::uint64_t version = 0;
};
// Un terreno preparado en otro hilo (imagenes subiendose). Opaco.
struct PreparedTerrain;

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
// Cerca, muchas briznas muy finas (como pelo): las distancias y la densidad
// son las del nivel Alto; los otros niveles del hardware las escalan
// (TerrainPass::setQualityTier).
struct GrassDesc {
    bool enabled = false;
    int layer = 0;               // capa del terreno donde crece
    float threshold = 0.25f;     // peso minimo de esa capa
    int dry_layer = 1;           // capa que la vuelve seca (-1 = ninguna)
    float density = 500.0f;      // briznas por m2 (de cerca)
    float max_distance = 80.0f;  // hasta donde se dibuja (m)
    float near_distance = 12.0f; // con todo el detalle hasta aqui
    float height = 0.35f;        // m
    float height_variation = 0.4f;
    float width = 0.007f;        // m (en la base)
    float bend = 0.35f;          // curvatura propia
    core::Vec3 base_color{0.13f, 0.20f, 0.06f};  // sRGB
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
    // Filtrado de las texturas de las capas (anisotropico y sesgo de los mips):
    // rehace su sampler y los sets de los terrenos. Con la GPU parada; antes
    // de create solo lo guarda.
    void setTextureFiltering(const TextureFiltering& filtering);

    // --- Terrenos ---
    // Sincrono (terrenos que no van en streaming): las imagenes se limpian
    // (plano, capa 0) en el command buffer del siguiente frame y no se dibuja
    // hasta entonces. Sin esperar a la GPU.
    std::uint32_t createTerrain(std::uint32_t resolution, std::uint32_t splat_resolution);
    // Desde CUALQUIER hilo (con la pasada creada): imagenes, parametros y la
    // copia de `pixels` enviada con submitAsync. nullptr si los datos no
    // valen o no hay memoria. No toca nada del frame.
    std::shared_ptr<PreparedTerrain> prepareTerrainAsync(const TerrainPixels& pixels) const;
    // ¿Termino ya su copia en la GPU? (no bloquea; cualquier hilo)
    static bool preparedTerrainReady(const std::shared_ptr<PreparedTerrain>& prepared);
    // Hilo principal, O(1): sets de descriptores y alta. Devuelve el id, o 0
    // si aun no esta listo o no hay descriptores ahora (se reintenta despues).
    std::uint32_t adoptTerrain(const std::shared_ptr<PreparedTerrain>& prepared);
    // Bytes como mucho por frame de las subidas de esculpir/pintar (0 = sin
    // tope, como antes). CVar stream.UploadMBPerFrame.
    void setUploadBudget(std::size_t bytes_per_frame);
    std::size_t uploadBudget() const { return upload_budget_; }
    // Bytes subidos en el ultimo recordUploads y los que quedan en cola.
    std::size_t lastUploadBytes() const { return last_upload_bytes_; }
    std::size_t pendingUploadBytes() const;
    // Texturas de las capas leidas en segundo plano (true, por defecto) o en
    // el momento (false, como antes). CVar render.terrain.AsyncLayers.
    void setAsyncLayers(bool async) { async_layers_ = async; }
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
    // Lo mismo para una vista aislada (las caras de la sonda de reflexion):
    // solo cambia los trozos que se dibujan y los parametros de su hueco, sin
    // lo retirado ni la camara del LOD de las luces locales (prepare() va una
    // vez por frame). `include` = false: el terreno no se dibuja en esa vista.
    // El siguiente prepare() vuelve a elegir los de la camara de pantalla.
    // `frame`: el hueco en que se dibuja la vista (sus parametros se escriben).
    void prepareView(std::uint32_t frame, const core::Vec3& camera_position, const core::Mat4& view_projection,
                     bool include);
    // Firma de lo que el terreno pinta en las cascadas del sol: un hash de
    // (id, height_revision, desc.cast_shadows, sitio y tamano, y los trozos
    // elegidos con su LOD) de todos los terrenos. Se calcula al final de
    // prepare() y de prepareView(); si cambia, las cascadas cacheadas ya no
    // coinciden con el terreno (una tesela entra o sale, se esculpe, cambia
    // de LOD...) y hay que redibujarlas. El terreno ya no cuenta como "se
    // mueve solo".
    std::uint64_t shadowSignature() const { return shadow_signature_; }

    // Dentro del pase de geometria (G-buffer abierto, set 0 del frame).
    void recordGBuffer(const vk::raii::CommandBuffer& cmd, std::uint32_t frame,
                       const vk::raii::DescriptorSet& frame_set) const;
    // Dentro de una cascada de sombra, o del mapa de una luz puntual/foco
    // (`local`: se descartan los trozos fuera del volumen de la luz).
    void recordShadow(const vk::raii::CommandBuffer& cmd, std::uint32_t frame, const vk::raii::DescriptorSet& frame_set,
                      const core::Mat4& light_view_projection, bool local = false) const;

    std::uint32_t chunkCount() const;

    // --- Hierba ---
    // Bloques de hierba (buffers de briznas, ~96 MB cada uno) que se reparten
    // las teselas mas cercanas a la camara; una tesela sin bloque no tiene
    // hierba. 0 = uno por tesela. CVar render.terrain.GrassPool.
    void setGrassPoolSize(std::uint32_t blocks) { grass_pool_size_ = blocks; }
    std::uint32_t grassBlocks() const { return static_cast<std::uint32_t>(grass_blocks_.size()); }
    // Nivel del hardware: cuantas briznas, hasta donde cada lista y cuantos
    // tramos tiene cada brizna. Por defecto, Alto.
    void setQualityTier(HardwareTier tier) { grass_tier_ = tier; }
    // Hacia el sol (normalizado; se normaliza aqui por si acaso): la
    // auto-sombra analitica dentro del cesped (grass.frag). Cada frame, antes
    // de recordGrassCull. Por defecto, el sol en lo alto (0, 1, 0).
    void setGrassLighting(const core::Vec3& to_sun);
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
        // Los mismos datos, compartidos: un hilo de trabajo puede leerlos
        // aunque despues se esculpa (se copian al escribir si alguien los tiene).
        std::shared_ptr<const std::vector<float>> heights_data;
        std::shared_ptr<const std::vector<std::uint8_t>> splat0_data;
        std::shared_ptr<const std::vector<std::uint8_t>> splat1_data;
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
        std::uint32_t done_rows = 0;       // filas ya subidas (bandas, por el tope por frame)
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
    // Las texturas de las capas: compartidas por los terrenos con las mismas
    // (las teselas de un TerrainGrid usan todas el mismo juego).
    struct LayerSet {
        ArrayTexture albedo;
        ArrayTexture normal;
        std::array<std::filesystem::path, kMaxTerrainLayers> albedo_paths{};  // las que se leyeron
        std::array<std::filesystem::path, kMaxTerrainLayers> normal_paths{};
        std::array<core::Vec3, kMaxTerrainLayers> average{};  // media de cada textura (sRGB)
    };
    struct Grass {
        VulkanBuffer blades;               // 3 listas (cerca, media, lejos) de `capacity`
        VulkanBuffer args;                 // 3 comandos de dibujo indirecto
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
        std::shared_ptr<LayerSet> layer_set;  // nullptr hasta el primer setDesc (no se dibuja)
        std::array<std::filesystem::path, kMaxTerrainLayers> albedo_paths{};  // las pedidas
        std::array<std::filesystem::path, kMaxTerrainLayers> normal_paths{};
        std::vector<VulkanBuffer> params;  // uno por frame en vuelo
        std::vector<vk::raii::DescriptorSet> sets;
        std::vector<Chunk> chunks;
        Grass* grass = nullptr;  // bloque prestado de grass_blocks_ (o ninguno)
        // Limpiar las imagenes en el siguiente recordUploads (createTerrain):
        // hasta entonces no se dibuja.
        bool needs_clear = false;
        // Capas pedidas que aun se estan leyendo (clave de layer_sets_).
        std::string pending_layers;
        // Copia en la CPU para la escena de rayos (setKeepCpuCopy). Compartida
        // con el snapshot del que salio (sin copiar) y con los hilos que hacen
        // la malla de los rayos: se copia al escribir si no es solo nuestra.
        std::shared_ptr<const std::vector<float>> cpu_heights;
        std::shared_ptr<const std::vector<std::uint8_t>> cpu_splat0;
        std::shared_ptr<const std::vector<std::uint8_t>> cpu_splat1;
        bool cpu_heights_owned = false;  // creada aqui: se puede escribir si nadie mas la tiene
        bool cpu_splat_owned = false;
        std::uint64_t height_revision = 0;
        std::uint64_t look_revision = 0;
        std::uint64_t look_signature = 0;
    };

    void createPipelines(const VulkanDevice& device, std::array<vk::Format, GBuffer::kColorAttachmentCount> gbuffer_formats,
                         vk::Format depth_format, vk::Format shadow_format);
    void createPatchMesh(const VulkanDevice& device);
    // Pide las capas de `terrain` (rutas de su desc): ya cargadas, se ponen;
    // si no, se leen en segundo plano (o en el momento sin setAsyncLayers).
    void requestLayerTextures(Terrain& terrain);
    // Pone otras capas a un terreno sin parar la GPU: sets nuevos y los de
    // antes retirados. false si no hay descriptores ahora.
    bool swapLayerSet(Terrain& terrain, std::shared_ptr<LayerSet> set);
    // Lo que llego de los hilos (capas leidas). Hilo principal, en prepare().
    void pollAsync();
    using LayerPaths = std::array<std::filesystem::path, kMaxTerrainLayers>;
    // Hilos que comprimen las capas a BC7 para la cache del usuario (definido
    // en el .cpp). destroy() los para y los espera.
    struct LayerBc7Jobs;
    // Desde cualquier hilo: lee, reescala y sube (submitAsync) las capas. Si
    // no estaban en la cache de BC7, lanza su compresion en `bc7_jobs`.
    static std::shared_ptr<LayerSet> buildLayerSet(const VulkanDevice& device, const LayerPaths& albedo_paths,
                                                   const LayerPaths& normal_paths,
                                                   std::vector<std::shared_ptr<GpuTicket>>& tickets,
                                                   const std::shared_ptr<LayerBc7Jobs>& bc7_jobs);
    // `owner` (el LayerSet de la textura) vive hasta que la GPU termina la copia.
    // `bc7`: las capas ya en BC7 con todos sus mips (de la cache del usuario);
    // si no, RGBA8 y los mips con la GPU, como siempre.
    static std::shared_ptr<GpuTicket> createArrayTexture(const VulkanDevice& device, ArrayTexture& texture,
                                                         const std::vector<std::vector<std::uint8_t>>& layers,
                                                         std::shared_ptr<const void> owner,
                                                         const std::vector<std::vector<std::vector<std::uint8_t>>>* bc7 = nullptr);
    void writeDescriptors(Terrain& terrain);
    void selectChunks(Terrain& terrain, const core::Vec3& camera, const core::Mat4& view_projection);
    // Los parametros del terreno (GpuTerrainParams) en el buffer del hueco `frame`.
    void writeParams(Terrain& terrain, std::uint32_t frame);
    // Recalcula shadow_signature_ (final de prepare y prepareView).
    void updateShadowSignature();
    void createGrassPipelines(const VulkanDevice& device, const vk::raii::DescriptorSetLayout& frame_layout,
                              std::array<vk::Format, GBuffer::kColorAttachmentCount> gbuffer_formats,
                              vk::Format depth_format);
    // Presta a `terrain` un bloque de hierba de su capacidad. false si no queda.
    bool ensureGrass(Terrain& terrain, std::uint32_t id);
    void releaseGrass(Terrain& terrain);
    std::unique_ptr<Grass> createGrassBlock(std::uint32_t capacity);

    const VulkanDevice* device_ = nullptr;
    std::uint32_t frames_ = 2;
    vk::raii::DescriptorSetLayout set_layout_{nullptr};
    vk::raii::PipelineLayout layout_{nullptr};
    vk::raii::Pipeline gbuffer_pipeline_{nullptr};
    vk::raii::Pipeline gbuffer_wire_pipeline_{nullptr};  // vista Wireframe del editor
    bool wireframe_ = false;
    vk::raii::Pipeline shadow_pipeline_{nullptr};
    // Pools de los sets de los terrenos: crecen bajo demanda (muchas teselas
    // vivas mas las retiradas que esperan a la GPU). Van antes que terrains_ y
    // retired_ para destruirse despues de sus sets.
    std::vector<vk::raii::DescriptorPool> pools_;
    bool pool_warned_ = false;
    void addPool();
    bool allocateSets(std::vector<vk::raii::DescriptorSet>& sets);
    vk::raii::Sampler clamp_sampler_{nullptr};
    vk::raii::Sampler repeat_sampler_{nullptr};
    TextureFiltering filtering_{};
    void createRepeatSampler();
    VulkanBuffer patch_vertices_;
    VulkanBuffer patch_indices_;
    std::uint32_t patch_index_count_ = 0;
    std::unordered_map<std::uint32_t, std::unique_ptr<Terrain>> terrains_;
    std::uint64_t shadow_signature_ = 0;  // ver shadowSignature()
    // Juegos de capas cargados. Se guardan aunque ya no los use ningun
    // terreno durante un rato (kLayerKeepFrames): las teselas que salen y
    // vuelven a entrar (streaming) no los vuelven a leer del disco.
    struct LayerEntry {
        std::shared_ptr<LayerSet> set;
        std::uint32_t unused_frames = 0;
    };
    std::unordered_map<std::string, LayerEntry> layer_sets_;
    // Capas leyendose en segundo plano (misma clave que layer_sets_).
    struct PendingLayers {
        std::shared_ptr<LayerSet> set;                    // lo escribe la tarea
        std::vector<std::shared_ptr<GpuTicket>> tickets;  // lo escribe la tarea
        std::atomic<int> state{0};                        // 0 leyendo, 1 enviado, 2 fallo
    };
    std::unordered_map<std::string, std::shared_ptr<PendingLayers>> pending_layers_;
    bool async_layers_ = true;
    std::shared_ptr<LayerBc7Jobs> layer_bc7_;  // se crea con la primera peticion de capas
    // Lo quitado espera a que ningun frame en vuelo lo use (sin parar la GPU:
    // el streaming de teselas quita terrenos a menudo).
    struct Retired {
        std::unique_ptr<Terrain> terrain;
        std::shared_ptr<LayerSet> layers;
        std::uint32_t frames = 0;
        std::vector<vk::raii::DescriptorSet> sets;  // los de antes de cambiar de capas
    };
    std::vector<Retired> retired_;
    std::uint32_t next_id_ = 1;
    // Camara del ultimo prepare(): decide el LOD tambien en los mapas de las
    // luces locales.
    core::Vec3 lod_camera_{};
    std::vector<Upload> uploads_;
    std::vector<VulkanBuffer> staging_;  // por frame en vuelo (tamano fijo con tope)
    std::size_t upload_budget_ = std::size_t{16} << 20;
    std::size_t last_upload_bytes_ = 0;
    std::atomic<bool> keep_cpu_copy_{false};  // lo lee prepareTerrainAsync (otro hilo)

    // Hierba.
    vk::raii::DescriptorSetLayout grass_set_layout_{nullptr};
    vk::raii::PipelineLayout grass_cull_layout_{nullptr};
    vk::raii::Pipeline grass_cull_pipeline_{nullptr};
    vk::raii::PipelineLayout grass_draw_layout_{nullptr};
    vk::raii::Pipeline grass_pipeline_{nullptr};
    vk::raii::DescriptorPool grass_pool_{nullptr};
    // Bloques de hierba (despues de grass_pool_: sus sets se liberan antes).
    struct GrassBlock {
        std::unique_ptr<Grass> grass;
        std::uint32_t owner = 0;     // id del terreno (0 = libre)
        std::uint32_t cooldown = 0;  // frames hasta que ningun frame en vuelo lo use
    };
    std::vector<GrassBlock> grass_blocks_;
    struct RetiredGrass {
        std::unique_ptr<Grass> grass;
        std::uint32_t frames = 0;
    };
    std::vector<RetiredGrass> retired_grass_;
    std::uint32_t grass_pool_size_ = 6;
    std::vector<core::Vec4> grass_interactors_;
    HardwareTier grass_tier_ = HardwareTier::High;
    core::Vec3 grass_to_sun_{0.0f, 1.0f, 0.0f};
    float grass_seconds_ = 0.0f;
    float grass_previous_seconds_ = 0.0f;
};

}  // namespace cramion::gfx

#endif  // CRAMION_VK_TERRAIN_PASS_H
