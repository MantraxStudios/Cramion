#ifndef CRAMION_VK_VFX_PASS_H
#define CRAMION_VK_VFX_PASS_H

// VFX Graph: particulas en la GPU (como el VFX Graph de Unity o Niagara).
//
//   CONTRATO COMPARTIDO (renderizador <-> CramionCore/vfx): cada frame la CPU
//   da la lista de efectos vivos (VfxInstanceDesc): su capacidad, su matriz,
//   cuantas particulas nacen, el tiempo que avanzan y sus BLOQUES ya
//   compilados (VfxBlock, 64 bytes). Un unico compute shader (vfx_sim.comp)
//   interpreta los bloques: no hay que compilar shaders por efecto.
//
//   Memoria: un pool de particulas compartido. Cada efecto tiene su rango
//   [offset, offset + capacidad), su lista de muertas (pila con contador
//   atomico) y su lista de vivas que rehace cada frame para el dibujo
//   indirecto (vkCmdDrawIndirect: 6 vertices x vivas). La CPU nunca toca
//   una particula.
//
//   Etapas por frame (vfx_sim.comp, push.stage):
//     RESET    rango nuevo o reiniciado: todas muertas en su pila
//     BEGIN    vivas del dibujo a 0 (un hilo por efecto)
//     EMIT     las que nacen: se sacan de la pila y pasan por Initialize
//     UPDATE   bloques Update (fuerzas, ruido, colisiones contra el depth de
//              la escena...), integracion, muerte o a la lista de vivas
//   Dibujo: quads de cara a la camara, estirados por la velocidad o
//   horizontales, con textura (flipbook), color y tamano a lo largo de la
//   vida, mezcla aditiva / alfa / premultiplicada y particulas suaves.

#include "CramionFX/core/Math.h"
#include "CramionFX/vk/VulkanBuffer.h"
#include "CramionFX/vk/VulkanCommon.h"
#include "CramionFX/vk/VulkanTexture.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <unordered_map>
#include <vector>

namespace cramion::gfx {

class VulkanDevice;

// Tipos de bloque de la GPU. MISMO orden y numeros que vfx_common.glsl.
enum class VfxBlockType : std::uint32_t {
    None = 0,
    // --- Initialize ---
    PositionSphere = 1,    // a.xyz centro, a.w radio; b.x volumen (0 superficie .. 1 lleno), b.y arco (grados)
    PositionBox = 2,       // a.xyz centro; b.xyz tamano, b.w solo superficie (0/1)
    PositionCone = 3,      // a.xyz base, a.w radio; b.x angulo (grados), b.y largo, b.z volumen 0..1
    PositionCircle = 4,    // a.xyz centro, a.w radio; b.x volumen 0..1 (plano XZ)
    PositionLine = 5,      // a.xyz inicio; b.xyz fin
    PositionMesh = 6,      // a.x primer punto, a.y puntos; b.x separacion por la normal
    VelocityRandom = 10,   // a.xyz min; b.xyz max (por componente); flags 1 = sumar
    VelocityFromShape = 11,  // a.x vel min, a.y vel max; a.z dispersion (grados)
    Lifetime = 12,         // a.x min, a.y max (s)
    Size = 13,             // a.x min, a.y max (m)
    Color = 14,            // a rgba A, b rgba B (al azar entre los dos)
    Rotation = 15,         // a.x/a.y angulo min/max, a.z/a.w giro min/max (grados, grados/s)
    FlipbookFrame = 16,    // a.x/a.y cuadro inicial min/max
    InheritVelocity = 17,  // a.x factor de la velocidad del emisor
    // --- Update ---
    Gravity = 20,          // a.xyz aceleracion (m/s2)
    Wind = 21,             // a.xyz velocidad del viento, a.w coeficiente (1/s)
    Drag = 22,             // a.x lineal (1/s), a.y cuadratico
    Turbulence = 23,       // a.x intensidad, a.y frecuencia, a.z octavas, a.w velocidad del ruido
    Attractor = 24,        // a.xyz posicion, a.w fuerza; b.x radio (0 = sin limite), b.y radio que mata
    Vortex = 25,           // a.xyz centro, a.w giro; b.xyz eje, b.w atraccion al eje
    CollideDepth = 26,     // a.x rebote, a.y friccion, a.z vida perdida, a.w escala del radio; b.x grosor, b.y matar al chocar
    CollidePlane = 27,     // a.xyz normal, a.w distancia; b.x rebote, b.y friccion, b.z vida perdida
    CollideSphere = 28,    // a.xyz centro, a.w radio; b.x rebote, b.y friccion, b.z dentro (0/1)
    KillBox = 29,          // a.xyz centro; b.xyz tamano; flags 1 = matar fuera (si no, dentro)
    SpeedLimit = 30,       // a.x velocidad maxima, a.y amortiguacion
    ConformSphere = 31,    // a.xyz centro, a.w radio; b.x velocidad de atraccion, b.y pegajosidad
};

// Un bloque compilado (std430, 64 bytes; vfx_common.glsl VfxBlock).
struct VfxBlock {
    std::uint32_t type = 0;
    std::uint32_t flags = 0;
    std::uint32_t pad0 = 0;
    std::uint32_t pad1 = 0;
    core::Vec4 a{};
    core::Vec4 b{};
    core::Vec4 c{};
};
static_assert(sizeof(VfxBlock) == 64);

enum class VfxBlend : std::uint32_t { Additive = 0, Alpha = 1, Premultiplied = 2 };
enum class VfxOrient : std::uint32_t { FaceCamera = 0, Stretched = 1, Horizontal = 2, Vertical = 3 };

inline constexpr std::uint32_t kVfxLutSize = 32;

// Un efecto vivo este frame.
struct VfxInstanceDesc {
    std::uint64_t id = 0;            // estable mientras viva (sus particulas siguen entre frames)
    std::uint32_t capacity = 1024;   // particulas como mucho
    core::Mat4 transform = core::Mat4::identity();  // el emisor: su espacio -> mundo
    bool world_space = true;         // las vivas no siguen al emisor
    bool reset = false;              // borrar sus particulas este frame
    std::uint32_t spawn_count = 0;   // nacen este frame
    float delta_seconds = 0.0f;      // 0 = en pausa (se dibuja igual)
    float time = 0.0f;               // reloj del efecto (ruido)
    std::uint32_t seed = 1;
    core::Vec3 emitter_velocity{};   // m/s (InheritVelocity)
    std::vector<VfxBlock> initialize;
    std::vector<VfxBlock> update;
    // Superficie de una malla (PositionMesh): pares (posicion, normal) en el
    // espacio del efecto. Se comparte entre frames.
    std::shared_ptr<const std::vector<core::Vec4>> points;

    // --- Salida ---
    bool visible = true;
    VfxBlend blend = VfxBlend::Additive;
    VfxOrient orient = VfxOrient::FaceCamera;
    float soft_distance = 0.25f;   // m (0 = bordes duros contra la escena)
    float intensity = 1.0f;        // multiplica el color (HDR: brilla con el bloom)
    float stretch = 0.1f;          // s de estela (Stretched)
    float alpha_clip = 0.0f;
    bool lit = false;              // multiplica por la luz del frame (humo)
    int texture = -1;              // ranura de loadTexture (-1 = disco suave)
    std::uint32_t flip_cols = 1;
    std::uint32_t flip_rows = 1;
    float flip_fps = 0.0f;         // > 0 cuadros/s; 0 = a lo largo de la vida
    // A lo largo de la vida (0..1): color (multiplica) y tamano (multiplica).
    bool color_over_life = false;
    std::array<core::Vec4, kVfxLutSize> color_lut{};
    bool size_over_life = false;
    std::array<float, kVfxLutSize> size_lut{};
    // Color segun la velocidad (multiplica).
    bool color_by_speed = false;
    core::Vec4 speed_color_slow{1.0f, 1.0f, 1.0f, 1.0f};
    core::Vec4 speed_color_fast{1.0f, 1.0f, 1.0f, 1.0f};
    float speed_min = 0.0f;
    float speed_max = 10.0f;
};

// Lo que el renderizador necesita de la vista para simular y dibujar.
struct VfxView {
    core::Mat4 view = core::Mat4::identity();
    core::Mat4 projection = core::Mat4::identity();
    core::Mat4 view_projection = core::Mat4::identity();
    core::Vec3 camera_position{};
    core::Vec3 light{0.5f, 0.5f, 0.5f};  // luz del frame (HDR lineal) para "lit"
    vk::Extent2D extent{1, 1};           // del depth
    vk::ImageView scene_depth{};         // depth de la escena (solo lectura)
};

struct VfxStats {
    std::uint32_t effects = 0;
    std::uint32_t alive = 0;       // vivas (lectura de la GPU, un par de frames tarde)
    std::uint32_t capacity = 0;    // de los efectos vivos
    std::uint32_t pool = 0;        // tamano del pool
    std::uint64_t memory_bytes = 0;
};

class VfxPass {
public:
    static constexpr std::uint32_t kMaxInstances = 256;
    static constexpr std::uint32_t kMaxBlocks = 4096;       // por frame (todos los efectos)
    static constexpr std::uint32_t kMaxPoints = 32768;      // pares por frame (todos los efectos)
    static constexpr std::uint32_t kMaxTextures = 16;
    static constexpr std::uint32_t kMaxPool = 1u << 21;     // particulas en total
    static constexpr std::uint32_t kMaxCapacity = 1u << 20; // por efecto

    void create(const VulkanDevice& device, vk::Format color_format, vk::Format depth_format,
                std::uint32_t frames_in_flight);
    void destroy();

    // Los efectos de este frame (los que no estan se borran).
    void setInstances(std::vector<VfxInstanceDesc> instances);
    bool active() const { return !instances_.empty(); }
    // Borra todas las particulas (al entrar o salir de Play).
    void clear();
    // Origen flotante: el mundo se desplazo -offset (las del espacio del mundo tambien).
    void shiftOrigin(const core::Vec3& offset) { pending_shift_ = pending_shift_ + offset; }

    // Carga una imagen para las particulas y devuelve su ranura (la misma
    // ruta, la misma ranura). -1 si no se pudo o no quedan ranuras.
    int loadTexture(const std::filesystem::path& file);

    // --- Grabacion (VulkanRenderer) ---
    // Prepara este hueco de frame: camara, depth, efectos y bloques. Cada
    // vista (tambien las secundarias) lo llama; `simulate`: despues se
    // grabara recordSimulate (la vista principal). false si no hay nada.
    bool prepare(std::uint32_t frame, const VfxView& view, bool simulate);
    // Simulacion: fuera de cualquier render pass, con el depth de la escena
    // ya escrito (lo leen las colisiones). Una vez por frame de la CPU (las
    // vistas secundarias no la repiten).
    void recordSimulate(const vk::raii::CommandBuffer& cmd, std::uint32_t frame);
    // Dibujo (renderizado abierto sobre la imagen HDR, depth en solo lectura).
    void recordDraw(const vk::raii::CommandBuffer& cmd, std::uint32_t frame, vk::Extent2D viewport) const;

    // Vivas de un efecto (lectura, un par de frames tarde). 0 si no existe.
    std::uint32_t aliveCount(std::uint64_t id) const;
    VfxStats stats() const { return stats_; }

private:
    struct Slot {
        std::uint32_t index = 0;     // contadores e instancia en la GPU
        std::uint32_t offset = 0;    // en el pool
        std::uint32_t capacity = 0;  // 0 = sin rango (no cabe)
        bool reset = true;           // la proxima simulacion la vacia
        bool simulated = false;      // ya tiene contadores validos (se puede dibujar)
        std::uint32_t alive = 0;     // ultima lectura
    };
    struct Range {
        std::uint32_t offset = 0;
        std::uint32_t size = 0;
    };
    struct DrawItem {
        std::uint32_t slot = 0;
        VfxBlend blend = VfxBlend::Additive;
        float distance = 0.0f;
    };

    vk::raii::Pipeline createDrawPipeline(vk::Format color_format, vk::Format depth_format, VfxBlend blend) const;
    void createPool(std::uint32_t size);
    bool allocate(std::uint32_t size, std::uint32_t& offset);
    void release(std::uint32_t offset, std::uint32_t size);
    bool placeSlot(Slot& slot, std::uint32_t capacity);
    void readBack(std::uint32_t frame);

    const VulkanDevice* device_ = nullptr;
    std::uint32_t frames_ = 2;

    std::vector<VfxInstanceDesc> instances_;
    std::unordered_map<std::uint64_t, Slot> slots_;
    std::vector<bool> slot_used_;
    std::vector<Range> free_;
    std::uint32_t pool_size_ = 0;
    bool sim_pending_ = false;
    core::Vec3 pending_shift_{};
    core::Vec3 frame_shift_{};  // el que lleva la simulacion de este frame
    std::uint32_t frame_counter_ = 0;
    bool warned_full_ = false;
    VfxStats stats_{};

    vk::raii::DescriptorSetLayout set_layout_{nullptr};
    vk::raii::DescriptorPool pool_{nullptr};
    std::vector<vk::raii::DescriptorSet> sets_;
    vk::raii::PipelineLayout layout_{nullptr};
    vk::raii::Pipeline sim_pipeline_{nullptr};
    std::array<vk::raii::Pipeline, 3> draw_pipelines_{nullptr, nullptr, nullptr};
    vk::raii::Sampler depth_sampler_{nullptr};
    vk::raii::Sampler texture_sampler_{nullptr};

    // Pool (en la GPU).
    VulkanBuffer particles_;  // 4 vec4 por particula
    VulkanBuffer dead_;       // uint por particula (pila de cada efecto)
    VulkanBuffer alive_;      // uint por particula (lista de dibujo)
    VulkanBuffer counters_;   // 8 int por ranura: muertas, vivas, -, -, dibujo indirecto (4)
    // Por frame en vuelo (CPU -> GPU, y la lectura de los contadores).
    std::vector<VulkanBuffer> frame_ubo_;
    std::vector<VulkanBuffer> instance_buf_;
    std::vector<VulkanBuffer> block_buf_;
    std::vector<VulkanBuffer> point_buf_;
    std::vector<VulkanBuffer> readback_;
    std::vector<bool> readback_ready_;
    std::vector<std::vector<DrawItem>> draws_;

    VulkanTexture white_;
    std::array<VulkanTexture, kMaxTextures> textures_;
    std::array<std::filesystem::path, kMaxTextures> texture_paths_;
};

}  // namespace cramion::gfx

#endif  // CRAMION_VK_VFX_PASS_H
