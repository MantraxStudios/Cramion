#ifndef CRAMION_CORE_VFX_VISUAL_EFFECT_H
#define CRAMION_CORE_VFX_VISUAL_EFFECT_H

// VFX Graph (como el VFX Graph de Unity o Niagara): efectos de particulas que
// se simulan enteros en la GPU (CramionFX/vk/VfxPass.h), de cientos a
// cientos de miles de particulas sin coste en la CPU.
//
//   VfxGraph       el asset .crvfx (JSON): parametros expuestos y cuatro
//                  contextos con sus bloques, como en Unity:
//                    Spawn       cuantas nacen (caudal, rafagas, por
//                                distancia, al recibir un evento)
//                    Initialize  como nacen (forma, velocidad, vida,
//                                tamano, color, giro...)
//                    Update      que les pasa (gravedad, rozamiento,
//                                turbulencia, atractores, vortices,
//                                colisiones contra la escena...)
//                    Output      como se ven (quads de cara a la camara,
//                                estirados, horizontales; textura y
//                                flipbook; color y tamano en la vida)
//                  Cada campo numerico de un bloque puede ir atado a un
//                  parametro expuesto ("Rate", "Color"...) que cambian el
//                  Inspector y los scripts.
//   VisualEffect   el componente: que efecto, reproducir al empezar,
//                  semilla y los valores de los parametros de este objeto.
//   VfxSystem      cada frame: el reloj y el Spawn de cada efecto en la CPU
//                  (pocos numeros), los bloques compilados al renderizador.

#include "CramionCore/asset/AssetTypes.h"
#include "CramionCore/ecs/Reflection.h"
#include "CramionCore/ecs/World.h"

#include <CramionFX/core/Math.h>
#include <CramionFX/vk/VfxPass.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace cramion::gfx {
class VulkanRenderer;
}
namespace cramion::assets {
struct ModelAsset;
}

namespace cramion::vfx {

inline constexpr const char* kVfxExtension = ".crvfx";

// --- Bloques ---------------------------------------------------------------------

enum class Context : int { Spawn = 0, Initialize = 1, Update = 2, Output = 3 };

enum class BlockKind : int {
    // Spawn (CPU)
    ConstantRate = 0,
    Burst,
    PeriodicBurst,
    RateOverDistance,
    OnEvent,
    // Initialize
    PositionSphere,
    PositionBox,
    PositionCone,
    PositionCircle,
    PositionLine,
    PositionMesh,
    VelocityRandom,
    VelocityFromShape,
    Lifetime,
    Size,
    Color,
    Rotation,
    FlipbookFrame,
    InheritVelocity,
    // Update
    Gravity,
    Wind,
    Drag,
    Turbulence,
    Attractor,
    Vortex,
    CollideDepth,
    CollidePlane,
    CollideSphere,
    KillBox,
    SpeedLimit,
    ConformSphere,
    // Output
    ColorOverLife,
    SizeOverLife,
    ColorBySpeed,
    Count
};

enum class FieldType : int { Float = 0, Vec3, Color, Bool, Int, Angle };

// Un campo editable de un bloque. Ocupa `width` huecos seguidos de
// Block::values a partir de `slot` (Vec3 = 3, Color = 4, el resto 1).
struct FieldInfo {
    const char* key;      // clave estable en el .crvfx
    const char* label;    // lo que ve el usuario
    FieldType type = FieldType::Float;
    int slot = 0;
    std::array<float, 4> defaults{};
    float min = 0.0f;     // min == max: sin limites
    float max = 0.0f;
    const char* tooltip = nullptr;
    int width() const { return type == FieldType::Vec3 ? 3 : type == FieldType::Color ? 4 : 1; }
};

struct BlockInfo {
    BlockKind kind;
    Context context;
    const char* key;          // "PositionSphere" (en el .crvfx)
    const char* label;        // "Posicion: esfera"
    const char* description;  // una linea para el editor
    std::vector<FieldInfo> fields;
    bool has_text = false;      // OnEvent: nombre del evento
    bool has_gradient = false;  // ColorOverLife
    bool has_curve = false;     // SizeOverLife
    bool has_mesh = false;      // PositionMesh
};

const std::vector<BlockInfo>& blockInfos();
const BlockInfo* blockInfo(BlockKind kind);
const BlockInfo* blockInfo(const std::string& key);

struct GradientKey {
    float time = 0.0f;  // 0..1 de la vida
    core::Vec4 color{1.0f, 1.0f, 1.0f, 1.0f};
};
struct CurveKey {
    float time = 0.0f;
    float value = 1.0f;
};

inline constexpr int kBlockValues = 16;

struct Block {
    BlockKind kind = BlockKind::ConstantRate;
    bool enabled = true;
    std::array<float, kBlockValues> values{};
    // Parametro expuesto atado a cada hueco (vacio = el valor de arriba).
    // Un Vec3/Color atado usa el parametro entero desde su primer hueco.
    std::array<std::string, kBlockValues> bindings{};
    std::string text;                     // OnEvent
    std::vector<GradientKey> gradient;    // ColorOverLife
    std::vector<CurveKey> curve;          // SizeOverLife
    assets::AssetRef mesh{{}, assets::AssetType::Model};  // PositionMesh
};

// Bloque nuevo con los valores por defecto de su tipo.
Block makeBlock(BlockKind kind);

// --- Parametros expuestos --------------------------------------------------------

enum class ParamType : int { Float = 0, Vector3 = 1, Color = 2, Bool = 3 };

struct ExposedParam {
    std::string name;
    ParamType type = ParamType::Float;
    core::Vec4 value{};  // Float = x, Vector3 = xyz, Color = rgba, Bool = x (0/1)
    std::string tooltip;
};

// --- Salida ----------------------------------------------------------------------

struct OutputSettings {
    gfx::VfxOrient orient = gfx::VfxOrient::FaceCamera;
    gfx::VfxBlend blend = gfx::VfxBlend::Additive;
    float intensity = 1.0f;       // HDR (> 1 brilla con el bloom)
    float soft_distance = 0.25f;  // m (particulas suaves; 0 = no)
    float stretch = 0.08f;        // s de estela (Estirada)
    float alpha_clip = 0.0f;
    bool lit = false;             // recibe la luz del sol y el cielo (humo, polvo)
    std::string texture;          // imagen dentro de Assets/ (vacio = disco suave)
    int flip_cols = 1;
    int flip_rows = 1;
    float flip_fps = 0.0f;        // 0 = el flipbook dura la vida de la particula
};

// --- Asset -----------------------------------------------------------------------

struct VfxGraph {
    Uuid uuid{};
    int capacity = 2048;          // particulas vivas como mucho
    bool world_space = true;      // las vivas no siguen al emisor
    float duration = 5.0f;        // s de un ciclo de Spawn
    bool loop = true;
    float start_delay = 0.0f;
    float prewarm = 0.0f;         // s simulados al empezar (aproximado: nacen ya)
    std::vector<ExposedParam> params;
    std::vector<Block> spawn;
    std::vector<Block> initialize;
    std::vector<Block> update;
    std::vector<Block> output_blocks;
    OutputSettings output;

    std::vector<Block>& blocks(Context context);
    const std::vector<Block>& blocks(Context context) const;
    const ExposedParam* findParam(const std::string& name) const;
};

std::string vfxGraphToText(const VfxGraph& graph);
bool vfxGraphFromText(const std::string& text, VfxGraph& out, std::string* error = nullptr);
bool loadVfxGraph(const std::filesystem::path& path, VfxGraph& out, std::string* error = nullptr);
bool saveVfxGraph(const VfxGraph& graph, const std::filesystem::path& path, std::string* error = nullptr);

// Plantillas del menu Crear (0 Chispas por defecto, 1 Fuego, 2 Humo,
// 3 Chispas que rebotan, 4 Explosion (evento "Explode"), 5 Magia (vortice),
// 6 Luciernagas, 7 Nieve, 8 Lluvia, 9 Estela/rastro).
int vfxPresetCount();
const char* vfxPresetName(int preset);
VfxGraph vfxPreset(int preset);

// --- Compilacion (CPU -> GPU) ----------------------------------------------------

// Valores de los parametros de un objeto: los del asset con lo que cambia el
// componente y los scripts.
using ParamValues = std::unordered_map<std::string, core::Vec4>;

// Valor de un hueco de un bloque (con su parametro atado, si lo hay).
float blockValue(const Block& block, int slot, const ParamValues& params);
core::Vec3 blockVec3(const Block& block, int slot, const ParamValues& params);
core::Vec4 blockColor(const Block& block, int slot, const ParamValues& params);

// Muestrea un degradado / una curva en `t` (0..1).
core::Vec4 sampleGradient(const std::vector<GradientKey>& keys, float t);
float sampleCurve(const std::vector<CurveKey>& keys, float t);

// Bloques Initialize/Update del asset ya listos para la GPU (los
// desactivados fuera) y la salida (color/tamano en la vida...).
void compileGraph(const VfxGraph& graph, const ParamValues& params, gfx::VfxInstanceDesc& out);

// Spawn de un frame en la CPU: cuantas nacen. Lo usa VfxSystem y lo prueban
// los tests.
struct SpawnState {
    float time = 0.0f;          // dentro del ciclo
    float total_time = 0.0f;    // desde que empezo
    float rate_carry = 0.0f;    // fraccion de particula del caudal
    float distance_carry = 0.0f;
    bool started = false;
    bool finished = false;      // sin loop y el ciclo termino
    std::vector<int> burst_cycles;  // rafagas hechas en este ciclo (por bloque)
    std::vector<std::string> pending_events;
};
// `moved`: metros que se movio el emisor este frame (RateOverDistance).
// `emitting`: si no, solo los eventos pendientes hacen nacer.
std::uint32_t advanceSpawn(const VfxGraph& graph, const ParamValues& params, SpawnState& state, float dt,
                           float moved, bool emitting, std::mt19937& random);

// --- Componente ------------------------------------------------------------------

struct ParamOverride {
    std::string name;
    core::Vec3 value{};
    float w = 1.0f;  // alfa de un color
};

struct VisualEffect {
    assets::AssetRef graph{{}, assets::AssetType::VisualEffect};
    bool play_on_awake = true;
    int seed = 0;                   // 0 = al azar cada vez
    float simulation_speed = 1.0f;
    bool preview_in_editor = true;  // simular en la escena si esta seleccionado
    std::vector<ParamOverride> params;

    void reflect(ecs::PropertyVisitor& v);
};

void registerVfxComponents();

// --- Sistema ---------------------------------------------------------------------

struct VfxSystemStats {
    std::uint32_t effects = 0;
    std::uint32_t alive = 0;
    std::uint32_t capacity = 0;
    std::uint32_t pool = 0;
    std::uint64_t memory_bytes = 0;
};

class VfxSystem {
public:
    // UUID de un .crvfx -> su archivo (vacio si no existe).
    using GraphResolver = std::function<std::filesystem::path(const Uuid&)>;
    // Modelo para PositionMesh (puede devolver nullptr si no esta cargado aun).
    using ModelProvider = std::function<std::shared_ptr<const assets::ModelAsset>(const Uuid&)>;

    void setGraphResolver(GraphResolver resolver) { resolver_ = std::move(resolver); }
    void setModelProvider(ModelProvider provider) { model_provider_ = std::move(provider); }
    // Carpeta Assets/ (las texturas de las particulas son rutas dentro).
    void setAssetsRoot(const std::filesystem::path& root) { assets_root_ = root; }

    // El editor cambia un asset abierto: se ve al momento, sin guardarlo.
    void setGraphOverride(const Uuid& uuid, const VfxGraph& graph);
    void clearGraphOverride(const Uuid& uuid);
    // Vuelve a leer los .crvfx (se guardo uno, o se refresco el proyecto).
    void reloadGraphs();
    // El asset de un efecto (cargado si hace falta); nullptr si no hay.
    const VfxGraph* graph(const Uuid& uuid);

    // Cada frame. `simulate`: Play (el tiempo avanza en todos). Fuera de Play
    // solo se simulan los de `preview` (los seleccionados en el editor); el
    // resto no se dibuja.
    void update(ecs::World& world, float delta_seconds, bool simulate, const std::vector<entt::entity>& preview,
                gfx::VulkanRenderer& renderer);
    // Borra todo (al entrar o salir de Play).
    void clear();
    void shiftOrigin(const core::Vec3& offset);

    // --- API (Lua, MCP, Inspector) ---
    void play(ecs::Entity entity);                         // reinicia y emite
    void stop(ecs::Entity entity, bool clear_particles = false);  // deja de emitir
    void pause(ecs::Entity entity, bool paused);
    bool isPlaying(ecs::Entity entity) const;
    void sendEvent(ecs::Entity entity, const std::string& event);
    void setFloat(ecs::Entity entity, const std::string& name, float value);
    void setVector(ecs::Entity entity, const std::string& name, const core::Vec3& value);
    void setColor(ecs::Entity entity, const std::string& name, const core::Vec4& value);
    void setBool(ecs::Entity entity, const std::string& name, bool value);
    // Valor actual de un parametro (Float = x). false si no existe.
    bool getParam(ecs::Entity entity, const std::string& name, core::Vec4& value);
    std::uint32_t aliveCount(ecs::Entity entity) const;
    VfxSystemStats stats() const { return stats_; }

private:
    struct Instance {
        std::uint64_t id = 0;
        Uuid graph{};
        SpawnState spawn;
        std::mt19937 random{1};
        std::uint32_t seed = 1;
        bool playing = true;
        bool paused = false;
        bool reset = true;
        bool started = false;      // play_on_awake ya decidido
        bool seen = false;
        float time = 0.0f;
        core::Vec3 last_position{};
        bool has_last_position = false;
        ParamValues script_params;  // lo que cambian los scripts (gana al componente)
        std::shared_ptr<const std::vector<core::Vec4>> points;
        Uuid points_model{};
    };
    struct CachedGraph {
        VfxGraph graph;
        bool valid = false;
        int texture = -1;
        std::string texture_path;
    };

    Instance& instanceOf(ecs::Entity entity);
    const Instance* findInstance(ecs::Entity entity) const;
    CachedGraph* cached(const Uuid& uuid);
    ParamValues paramsFor(const VfxGraph& graph, const VisualEffect& component, const Instance& instance) const;
    std::shared_ptr<const std::vector<core::Vec4>> meshPoints(const Uuid& model);

    GraphResolver resolver_;
    ModelProvider model_provider_;
    std::filesystem::path assets_root_;
    std::unordered_map<Uuid, CachedGraph> graphs_;
    std::unordered_map<Uuid, VfxGraph> overrides_;
    std::unordered_map<entt::entity, Instance> instances_;
    std::unordered_map<Uuid, std::shared_ptr<const std::vector<core::Vec4>>> mesh_points_;
    std::uint64_t next_id_ = 1;
    VfxSystemStats stats_{};
    gfx::VulkanRenderer* renderer_ = nullptr;
};

// El sistema activo (el del editor o el juego): lo usan Lua y el MCP.
VfxSystem* activeSystem();
void setActiveSystem(VfxSystem* system);

}  // namespace cramion::vfx

#endif  // CRAMION_CORE_VFX_VISUAL_EFFECT_H
