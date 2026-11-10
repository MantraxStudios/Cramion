#ifndef CRAMION_VK_VOXEL_PASS_H
#define CRAMION_VK_VOXEL_PASS_H

// Mundos de bloques (voxeles), dibujados en el G-buffer y en las cascadas de
// sombra: reciben toda la iluminacion diferida (PBR, sol con sombras, cielo,
// SSAO, SSGI, niebla, luz volumetrica) como cualquier otra superficie.
//
//   - La malla va por secciones de 16^3 bloques. Cada seccion es una lista de
//     caras (cuadrados de 4 vertices de 8 bytes) que CramionCore genera en
//     otros hilos; aqui solo se suben y se dibujan.
//   - Memoria: paginas grandes de vertices en la GPU repartidas entre las
//     secciones (sin una reserva de Vulkan por seccion). Cambiar un bloque
//     sube solo su seccion, grabado en el command buffer del frame; la malla
//     vieja se libera cuando ningun frame en vuelo la usa.
//   - Texturas PBR en arrays (una capa por textura de bloque): color con alfa
//     (recorte: hojas, plantas), normal + altura + oclusion, y rugosidad,
//     metal y emision. Relieve con parallax cerca de la camara.
//   - TESELADO (setTessellation): relieve de verdad en las caras de bloque
//     opacas de las secciones cercanas (voxel.tesc / voxel.tese): cada cara se
//     parte segun la distancia y se hunde segun la altura de su textura, con
//     los bordes de cada bloque fijos (sin grietas). Mas lejos, el parallax.
//   - Por vertice: oclusion ambiental de las esquinas, luz del cielo y de las
//     antorchas (0..15, como Minecraft) y un tinte (hierba y hojas por bioma).
//
// Formato del vertice (2 x uint32), igual que voxel.vert:
//   a: x 0..16 (5 bits) | y (5) | z (5) | cara 0..6 (3; 6 = planta en cruz) |
//      u (1) | v (1) | oclusion 0..3 (2) | ondea (1)
//   b: capa de textura (10) | luz del cielo 0..15 (4) | luz de bloque (4) |
//      tinte R G B de 4 bits (12)

#include "CramionFX/vk/GBuffer.h"
#include "CramionFX/core/Math.h"
#include "CramionFX/core/Frustum.h"
#include "CramionFX/vk/TextureFiltering.h"
#include "CramionFX/vk/VoxelOccupancy.h"
#include "CramionFX/vk/VulkanBuffer.h"
#include "CramionFX/vk/VulkanCommon.h"

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

namespace cramion::gfx {

class VulkanDevice;

// Una textura de bloque: tres imagenes RGBA8 de size x size.
struct VoxelTextureLayer {
    std::vector<std::uint8_t> albedo;    // sRGB, alfa = recorte
    std::vector<std::uint8_t> normal;    // xy normal (tangente), z altura, w oclusion
    std::vector<std::uint8_t> material;  // r rugosidad, g metal, b emision, a reflectancia
};

struct VoxelStats {
    std::uint32_t sections = 0;
    std::uint32_t visible = 0;
    std::uint64_t quads = 0;
    std::uint64_t memory_bytes = 0;
    std::uint32_t pending_uploads = 0;
    // Terreno lejano (setFarSection).
    std::uint32_t far_sections = 0;
    std::uint32_t far_visible = 0;
    std::uint64_t far_quads = 0;
    // Secciones cerradas (ninguna cara junto a aire con luz del cielo: cuevas
    // enterradas): no van a las cascadas del sol (siguen en las luces locales).
    std::uint32_t enclosed_sections = 0;
    std::uint64_t enclosed_quads = 0;
    // Caras de bloque que cubren los quads (las fusionadas cuentan su area):
    // quads / block_faces = lo que ahorra la fusion (Mesher.cpp).
    std::uint64_t block_faces = 0;
    // Lo que dibujo el ultimo G-buffer (con el descarte por caras): quads,
    // de ellos lejanos, y llamadas de dibujo.
    std::uint64_t drawn_quads = 0;
    std::uint64_t drawn_far_quads = 0;
    std::uint32_t draw_calls = 0;
    // TESELADO: caras dibujadas con relieve teselado en el ultimo G-buffer.
    std::uint64_t tessellated_quads = 0;
};

class VoxelPass {
public:
    // Dibujar el G-buffer en lineas (vista Wireframe del editor).
    void setWireframe(bool wireframe) { wireframe_ = wireframe; }
    static constexpr std::uint32_t kMaxQuadsPerSection = 32768;

    void create(const VulkanDevice& device, const vk::raii::DescriptorSetLayout& frame_layout,
                std::array<vk::Format, GBuffer::kColorAttachmentCount> gbuffer_formats, vk::Format depth_format, vk::Format shadow_format,
                std::uint32_t frames_in_flight);
    void destroy();
    // Filtrado de las texturas de los bloques (anisotropico y sesgo de los
    // mips): rehace el sampler y el set. Con la GPU parada; antes de create
    // solo lo guarda.
    void setTextureFiltering(const TextureFiltering& filtering);

    // Las texturas de los bloques (todas del mismo tamano, potencia de dos).
    void setTextures(std::uint32_t size, const std::vector<VoxelTextureLayer>& layers);
    bool hasTextures() const { return layer_count_ > 0; }

    // Malla de una seccion (clave libre, p. ej. sus coordenadas empaquetadas):
    // `origin` = esquina minima en el mundo; `vertices` = 2 uint32 por
    // vertice, 4 vertices por cara. Sin vertices se quita.
    void setSection(std::uint64_t key, const core::Vec3& origin, const std::uint32_t* vertices,
                    std::uint32_t vertex_count) {
        setSection(key, origin, vertices, vertex_count, 1.0f);
    }
    void removeSection(std::uint64_t key);
    void clearSections();
    // Agua de bloques (la dibuja el pase del agua con water_voxel.vert, no el
    // G-buffer ni las sombras): mismas paginas y subidas, otra lista.
    void setWaterSection(std::uint64_t key, const core::Vec3& origin, const std::uint32_t* vertices,
                         std::uint32_t vertex_count) {
        setSection(key | kWaterKey, origin, vertices, vertex_count);
    }
    void removeWaterSection(std::uint64_t key) { removeSection(key | kWaterKey); }
    // Terreno lejano (CramionCore voxel/FarTerrain.cpp, estilo Distant Horizons):
    // teselas de columnas coloreadas en el G-buffer (sin texturas ni sombras
    // propias). Vertice lejano (voxel.vert, push.style bit 2):
    //   a: x 0..16 (5) | y bits 0..4 (5) | z 0..16 (5) | cara (3) | u (1) | v (1) |
    //      oclusion 0..3 (2) | 0 (1) | y bits 5..8 (4)
    //   b: R G B sRGB (8 + 8 + 8) | rugosidad 0..15 (4) | agua (1)
    // x y z se multiplican por `scale` (bloques por columna); y en bloques.
    // No cambian las sombras cacheadas ni el mapa de lluvia (no proyectan).
    void setFarSection(std::uint64_t key, const core::Vec3& origin, float scale, const std::uint32_t* vertices,
                       std::uint32_t vertex_count) {
        setSection(key | kFarKey, origin, vertices, vertex_count, scale);
    }
    void removeFarSection(std::uint64_t key) { removeSection(key | kFarKey); }
    // Circulo (x, z; radio) donde ya estan los chunks de verdad: ahi no se
    // dibuja lo lejano (radio <= 0: en todas partes). `fade_end` (m desde la
    // camara, 0 = sin): el ultimo 15 % antes se deshace en el cielo (tramado
    // que el TAA suaviza), sin un borde duro al final del mundo.
    void setFarHole(const core::Vec3& center, float radius, float fade_end = 0.0f) {
        far_hole_ = center;
        far_hole_radius_ = radius;
        far_fade_end_ = fade_end;
    }
    bool hasWater() const { return water_count_ > 0; }
    // Las secciones de agua visibles, de lejos a cerca: enlaza los vertices y
    // los indices y llama a `before_draw` con la esquina de cada una (para
    // sus push constants) antes de dibujarla. El pipeline ya enlazado.
    void drawWater(const vk::raii::CommandBuffer& cmd, const std::function<void(const core::Vec3&)>& before_draw) const;
    // Segundos (el vaiven de hojas y plantas).
    void setTime(float seconds) { time_ = seconds; }
    // Personajes que apartan las plantas y las hojas a ras de suelo (voxel.vert,
    // foliagePush): esferas xyz = pies, w = radio, de una fuente (kInteractors*).
    // Se juntan (sin repetir) en prepare(): los 16 mas cercanos a la camara,
    // cada uno con una estela que se queda atras (lo que deja se endereza poco
    // a poco) y una fuerza que sube al llegar y baja al irse (sin saltos).
    static constexpr std::uint32_t kInteractorsScene = 0;   // RenderSync: cuerpos fisicos, objetos de red, efectos
    static constexpr std::uint32_t kInteractorsVoxels = 1;  // VoxelSystem: Voxel.moveBox y Voxel.pushFoliage
    void setInteractors(std::uint32_t source, const std::vector<core::Vec4>& spheres) {
        if (source < interactor_sources_.size()) interactor_sources_[source] = spheres;
    }
    std::uint32_t interactorCount() const { return interactor_count_; }
    // Color liso: cada bloque con una variacion suave de su color (voxel.frag).
    void setFlatColors(bool flat) { flat_colors_ = flat; }
    void setVisible(bool visible) { visible_ = visible; }

    // Una vez por frame: sube lo pendiente y libera lo que ya no se usa.
    // true si cambio la geometria (sombras cacheadas).
    bool recordUploads(const vk::raii::CommandBuffer& cmd, std::uint32_t frame);
    // Secciones visibles (frustum), de cerca a lejos.
    void prepare(std::uint32_t frame, const core::Vec3& camera_position, const core::Mat4& view_projection);
    void recordGBuffer(const vk::raii::CommandBuffer& cmd, std::uint32_t frame,
                       const vk::raii::DescriptorSet& frame_set) const;
    // `sun` = cascadas del sol: sin las secciones cerradas (cuevas enterradas,
    // sin ninguna cara junto a aire con luz del cielo). El sol no llega a
    // ellas y la corteza del terreno (sus caras si dan al aire iluminado) ya
    // lo tapa: la sombra no cambia y se ahorra dibujar las cuevas en cada
    // cascada. Las luces locales (antorchas) y la lluvia las dibujan todas.
    // `keep` (opcional): ademas del frustum de la luz, la caja de la seccion
    // tiene que pasarlo (cascadas: que su sombra pueda caer en lo que se ve).
    void recordShadow(const vk::raii::CommandBuffer& cmd, std::uint32_t frame, const vk::raii::DescriptorSet& frame_set,
                      const core::Mat4& light_view_projection, bool sun = false,
                      const std::function<bool(const core::Aabb&)>& keep = {}) const;

    VoxelStats stats() const;
    bool empty() const { return sections_.empty(); }
    // Origen flotante: las secciones ya subidas, -offset.
    void shiftOrigin(const core::Vec3& offset) {
        for (auto& [key, section] : sections_) section.origin = section.origin - offset;
        shiftInteractors(offset);
        occupancy_.shiftOrigin(offset);
    }
    // Ocupacion de los bloques para los rayos (VoxelOccupancy): se sube una
    // vez por frame de la vista principal, antes de los pases de rayos.
    void recordOccupancy(const vk::raii::CommandBuffer& cmd, std::uint32_t frame, const core::Vec3& camera_position) {
        occupancy_.record(cmd, frame, camera_position);
    }
    const VoxelOccupancy& occupancy() const { return occupancy_; }
    void setOccupancyHeight(float meters) { occupancy_.setUpReach(meters); }

private:
    struct Allocation {
        int page = -1;
        std::uint64_t offset = 0;  // bytes
        std::uint64_t size = 0;
        bool valid() const { return page >= 0; }
    };
    struct Page {
        VulkanBuffer buffer;
        std::uint64_t capacity = 0;
        std::vector<std::pair<std::uint64_t, std::uint64_t>> free;  // (offset, size), ordenados
    };
    struct Section {
        core::Vec3 origin{};
        Allocation live;           // lo que se dibuja
        std::uint32_t quads = 0;
        Allocation pending;        // lo que se esta subiendo
        std::uint32_t pending_quads = 0;
        std::uint32_t faces = 0, pending_faces = 0;  // caras de bloque cubiertas (VoxelStats::block_faces)
        std::vector<std::uint32_t> pending_data;
        bool queued = false;
        bool water = false;
        bool distant = false;     // terreno lejano (kFarKey)
        float scale = 1.0f;   // lejano: bloques por columna
        // Alguna cara con luz del cielo (> 0) en sus vertices: proyecta al
        // sol. La de lo que se dibuja y la de lo que se esta subiendo.
        bool sky = true;
        bool pending_sky = true;
        // Quads ordenados por cara (voxel.vert: a >> 15 & 7; 0..5 = +X -X +Y
        // -Y +Z -Z, 6-7 = plantas y demas): el grupo f va de face_start[f] a
        // face_start[f + 1]. El G-buffer se salta los grupos que miran al lado
        // contrario de la camara (drawSections): la mitad de los vertices.
        // Grupos 0-7 = caras de capas sin huecos (pipeline sin discard: la GPU
        // descarta lo tapado antes del shader), 8-15 = las de capas con recorte
        // (hojas, plantas, cristal) y lo lejano.
        std::array<std::uint32_t, 17> face_start{};
        std::array<std::uint32_t, 17> pending_face_start{};
    };
    static constexpr std::uint64_t kWaterKey = 1ull << 62;
    static constexpr std::uint64_t kFarKey = 1ull << 61;
    void setSection(std::uint64_t key, const core::Vec3& origin, const std::uint32_t* vertices, std::uint32_t vertex_count,
                    float scale);
    // Caja de la seccion (las lejanas son mas grandes).
    core::Aabb sectionBox(const Section& s) const;
    struct Retired {
        Allocation allocation;
        std::uint64_t release_frame = 0;
    };

    void createPipelines(const VulkanDevice& device, std::array<vk::Format, GBuffer::kColorAttachmentCount> gbuffer_formats,
                         vk::Format depth_format, vk::Format shadow_format);
    Allocation allocate(std::uint64_t bytes);
    void release(const Allocation& allocation);
    void retire(const Allocation& allocation);
    // Camara de la vista (prepare) y si se pueden saltar los grupos de caras de
    // espaldas a ella (no con camaras ortograficas: setFaceCulling).
    core::Vec3 view_position_{};
    bool face_culling_ = true;
    bool low_quality_ = false;
    bool lowest_quality_ = false;
    float plant_distance_ = 0.0f;
public:
    void setFaceCulling(bool enabled) { face_culling_ = enabled; }
    // Sin relieve (parallax) en los bloques: el movil en Baja y Media.
    void setLowQuality(bool low) { low_quality_ = low; }
    // Lo minimo (movil en Baja): sin normal map y lecturas de textura con
    // derivadas implicitas (textureGrad va mas lento en las GPU de movil).
    void setLowestQuality(bool lowest) { lowest_quality_ = lowest; }
    // Plantas (grupo de caras 6) solo en las secciones a menos de esto (m); 0 = todas.
    void setPlantDistance(float meters) { plant_distance_ = meters; }
    // TESELADO: relieve teselado de las caras de bloque opacas (grupos 0-5) en las
    // secciones a menos de `distance` m: se hunden hasta `depth` bloques segun la
    // altura de su textura, partidas en hasta `density` trozos por bloque de cerca.
    // Necesita tessellationShader (si no, se queda el parallax). Lo apaga
    // setLowQuality. Devuelve true si cambia lo que proyectan las sombras (el
    // empuje de voxel.vert): hay que redibujar las sombras guardadas.
    bool setTessellation(bool enabled, float distance, float depth, float density);
    bool tessellationSupported() const { return *gbuffer_tess_pipeline_ != nullptr; }
    // Activo este frame (soportado, pedido y no en calidad baja).
    bool tessellationActive() const {
        return tess_enabled_ && !low_quality_ && !lowest_quality_ && tessellationSupported() && !flat_colors_;
    }
private:
    bool tess_enabled_ = false;
    float tess_distance_ = 24.0f;
    float tess_depth_ = 0.1f;
    float tess_density_ = 16.0f;
    float tess_shadow_depth_ = 0.0f;  // el empuje de las sombras que esta en las guardadas
    vk::ShaderStageFlags push_stages_{};
    // `groups`: -1 todo, 0 los grupos opacos (0-7), 1 los de recorte (8-15).
    // `tess` (TESELADO, con groups = 0): 0 sin teselado; 1 solo los grupos 0-5 de
    // las secciones cercanas, como parches de 4 vertices (pipeline teselado); 2
    // lo demas (en las cercanas, sin sus grupos 0-5).
    void drawSections(const vk::raii::CommandBuffer& cmd, const std::vector<std::uint64_t>& keys,
                      const core::Mat4* light_view_projection,
                      const std::function<bool(const core::Aabb&)>* keep = nullptr, bool ignore_near = false,
                      int groups = -1, int tess = 0) const;
    bool tessNear(const Section& s) const;

    const VulkanDevice* device_ = nullptr;
    std::uint32_t frames_ = 2;
    vk::raii::DescriptorSetLayout set_layout_{nullptr};
    vk::raii::PipelineLayout layout_{nullptr};
    vk::raii::Pipeline gbuffer_pipeline_{nullptr};
    vk::raii::Pipeline gbuffer_wire_pipeline_{nullptr};  // vista Wireframe del editor
    // Movil en Baja (setLowestQuality): voxel.frag con kSurfaceLite (sin lluvia,
    // nieve, fuego ni otono: menos registros por hilo).
    vk::raii::Pipeline gbuffer_lite_pipeline_{nullptr};
    // Las mismas sin discard (voxel.frag kVoxelOpaque) para los grupos 0-7.
    vk::raii::Pipeline gbuffer_opaque_pipeline_{nullptr};
    vk::raii::Pipeline gbuffer_opaque_lite_pipeline_{nullptr};
    // TESELADO: caras opacas cercanas con relieve teselado (sin discard), y en lineas (vista Wireframe).
    vk::raii::Pipeline gbuffer_tess_pipeline_{nullptr};
    vk::raii::Pipeline gbuffer_tess_wire_pipeline_{nullptr};
    // Capas con algun texel recortado (alfa < 128): sus caras van a los grupos 8-15.
    std::vector<std::uint8_t> layer_cutout_;
    bool wireframe_ = false;
    vk::raii::Pipeline shadow_pipeline_{nullptr};
    vk::raii::DescriptorPool pool_{nullptr};
    vk::raii::DescriptorSet set_{nullptr};
    vk::raii::Sampler sampler_{nullptr};
    TextureFiltering filtering_{};
    void createSampler(const VulkanDevice& device);
    void writeSet(const VulkanDevice& device);

    struct ArrayTexture {
        vk::raii::DeviceMemory memory{nullptr};
        vk::raii::Image image{nullptr};
        vk::raii::ImageView view{nullptr};
    };
    ArrayTexture albedo_;
    ArrayTexture normal_;
    ArrayTexture material_;
    std::uint32_t layer_count_ = 0;

    VulkanBuffer quad_indices_;
    std::vector<std::unique_ptr<Page>> pages_;
    std::unordered_map<std::uint64_t, Section> sections_;
    std::deque<std::uint64_t> upload_queue_;
    std::vector<Retired> retired_;
    std::vector<VulkanBuffer> staging_;
    std::uint64_t frame_counter_ = 0;
    float time_ = 0.0f;
    float prepared_time_ = 0.0f;  // el de prepare() de este frame
    float previous_time_ = 0.0f;  // el del frame anterior
    bool flat_colors_ = false;
    bool visible_ = true;
    core::Vec3 far_hole_{};
    float far_hole_radius_ = 0.0f;
    float far_fade_end_ = 0.0f;
    std::uint32_t far_visible_ = 0;
    mutable std::uint64_t drawn_quads_ = 0, drawn_far_quads_ = 0;  // ultimo G-buffer (stats)
    mutable std::uint32_t draw_calls_ = 0;
    mutable std::uint64_t drawn_tess_quads_ = 0;

    // Personajes que apartan las plantas (setInteractors).
    struct Interactor {
        core::Vec3 feet{};
        float radius = 0.4f;
        core::Vec3 trail{};     // sigue a los pies con retraso
        float strength = 0.0f;  // 0..1: sube al aparecer, baja al irse
        bool seen = false;      // llego este frame
    };
    void updateInteractors(std::uint32_t frame, const core::Vec3& camera_position);
    void shiftInteractors(const core::Vec3& offset);
    std::uint32_t interactorOffset(std::uint32_t frame) const {
        return static_cast<std::uint32_t>(interactor_slot_ * (frames_ > 0 ? frame % frames_ : 0));
    }
    std::array<std::vector<core::Vec4>, 2> interactor_sources_;
    std::vector<Interactor> interactors_;
    std::vector<core::Vec4> interactor_gpu_;  // feet[16] y trail[16] de lo ultimo subido (el "anterior" del siguiente)
    std::uint32_t interactor_count_ = 0;
    std::uint64_t interactor_frame_ = ~0ull;  // frame_counter_ de la ultima actualizacion
    float interactor_time_ = 0.0f;
    VulkanBuffer interactor_buffer_;           // un hueco por frame en vuelo (offset dinamico)
    vk::DeviceSize interactor_slot_ = 0;

    std::vector<std::uint64_t> visible_keys_;
    std::vector<std::uint64_t> all_keys_;  // para las sombras (su propio recorte)
    std::vector<std::uint64_t> sun_keys_;  // las de all_keys_ con luz del cielo (cascadas del sol)
    std::vector<std::uint64_t> water_keys_;  // agua visible, de lejos a cerca
    std::size_t water_count_ = 0;            // secciones de agua (subidas o no)
    VoxelOccupancy occupancy_;               // los bloques para los rayos
};

}  // namespace cramion::gfx

#endif  // CRAMION_VK_VOXEL_PASS_H
