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
//                                colisiones contra la escena...) y eventos:
//                                al morir / al chocar nacen particulas de
//                                otro .crvfx en ese punto (subemisor, en la
//                                GPU, sin pasar por la CPU); y canales de
//                                datos: al morir / al chocar / cada t s
//                                apuntan {tipo, posicion, velocidad, color,
//                                valor} en un canal con nombre que escuchan
//                                otros efectos (Spawn "Al recibir un evento
//                                de la GPU") y en el que tambien escriben
//                                los scripts (Effects.pushEvent)
//                    Output      como se ven: quads (de cara a la camara,
//                                estirados, horizontales; textura y
//                                flipbook), una cinta continua entre las
//                                vivas (estelas) o una malla 3D por
//                                particula; color y tamano en la vida.
//                                Bloque Luz por particula: las vivas tambien
//                                iluminan (luces por clusteres, sin sombra)
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
    // Update: eventos (al final para no renumerar los de antes)
    TriggerOnDie,
    TriggerOnCollide,
    // Initialize: atributos de la cinta (id, tiras, ancho, giro)
    RibbonAttributes,
    // Canales de datos (eventos de la GPU con nombre): Update apunta, Spawn escucha
    EmitOnDeath,
    EmitOnCollision,
    EmitEvery,
    OnGpuEvent,
    // Malla: variante por particula (Initialize) y velocidad de giro (Update)
    MeshIndex,
    AngularVelocity,
    // Output: luz por particula (las vivas tambien iluminan el mundo)
    ParticleLight,
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
    bool has_effect = false;    // TriggerOnDie / TriggerOnCollide: el .crvfx que nace
    // Que es el texto (has_text) en el editor; nullptr = "Evento". Los de los
    // canales de datos: "Canal".
    const char* text_label = nullptr;
};

// Bloques de los canales de datos (Emit* y "Al recibir un evento de la GPU").
inline bool isChannelBlock(BlockKind kind) {
    return kind == BlockKind::EmitOnDeath || kind == BlockKind::EmitOnCollision || kind == BlockKind::EmitEvery ||
           kind == BlockKind::OnGpuEvent;
}

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
    assets::AssetRef effect{{}, assets::AssetType::VisualEffect};  // Trigger*: el subemisor
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
    bool flip_blend = false;      // funde cada cuadro con el siguiente (sin saltos)

    // Que se dibuja: quads, una cinta entre las vivas o una malla por particula.
    gfx::VfxOutput type = gfx::VfxOutput::Quads;
    // Cinta: ancho = tamano, color y tamano en la vida a lo largo; la textura
    // a lo largo (U) y a lo ancho (V).
    gfx::VfxRibbonFacing ribbon_facing = gfx::VfxRibbonFacing::Camera;
    gfx::VfxRibbonUv ribbon_uv = gfx::VfxRibbonUv::Stretch;
    float ribbon_tile = 1.0f;  // m por repeticion (UV por longitud)
    float ribbon_gap = 0.0f;   // s entre dos puntos para cortar la cinta (0 = nunca)
    core::Vec3 ribbon_vector{0.0f, 1.0f, 0.0f};  // CustomFacing / CustomSide (espacio del emisor)
    float ribbon_tension = 0.0f;   // suavizado: 0 = Catmull-Rom, 1 = tramos rectos
    int ribbon_subdivisions = 1;   // tramos por segmento (1..16; 1 = sin suavizar)
    bool ribbon_adaptive = false;  // menos tramos donde la cinta va recta
    int ribbon_atlas_cell = -1;    // celda de la textura (columnas x filas); -1 = entera
    core::Vec2 ribbon_uv_scale{1.0f, 1.0f};
    core::Vec2 ribbon_uv_offset{0.0f, 0.0f};
    // Malla: el modelo (todas sus piezas juntas), como gira y su escala (por
    // el tamano de la particula). Usa la textura de arriba con su UV.
    assets::AssetRef mesh{{}, assets::AssetType::Model};
    gfx::VfxMeshOrient mesh_orient = gfx::VfxMeshOrient::Random;
    core::Vec3 mesh_scale{1.0f, 1.0f, 1.0f};
    // Hasta 3 modelos mas (variantes o niveles de LOD): cada particula usa el
    // de su indice (bloque "Indice de malla") o, con mesh_lod_distance > 0, el
    // de su distancia (un nivel cada tantos m; 0 = el principal).
    std::vector<assets::AssetRef> mesh_variants;
    float mesh_lod_distance = 0.0f;
    float mesh_sprite_distance = 0.0f;  // m: mas lejos, un quad con la textura (0 = nunca)
    core::Vec4 mesh_rotation{0.0f, 0.0f, 0.0f, 1.0f};  // cuaternion de la orientacion Fija
    core::Vec3 mesh_scale_jitter{};     // 0..1 por eje: escala al azar por particula
    // Material (.crmat): color base, textura base, emision, opaco o
    // transparente (alfa / aditivo) y sin luz. Manda sobre la mezcla, "lit" y
    // la textura de arriba.
    assets::AssetRef material{{}, assets::AssetType::Material};
    // Mezcla Distorsion: px a 1080p (0..5) y la textura como mapa de alturas
    // (si no, es un normal map; sin textura, una lente blanda).
    float distortion = 3.0f;
    bool distortion_height = false;
};

// Lo que la salida Malla toma de un .crmat (VfxSystem lo lee una vez).
struct VfxMaterial {
    bool valid = false;
    core::Vec4 tint{1.0f, 1.0f, 1.0f, 1.0f};
    core::Vec3 emissive{};
    std::string albedo;  // ruta dentro de Assets/ (vacia = la textura de la salida)
    gfx::VfxBlend blend = gfx::VfxBlend::Opaque;
    bool lit = true;
    int texture = -2;  // ranura de `albedo` en el pase (-2 = sin cargar aun, -1 = no se pudo)
};
// Lee lo que usa la salida Malla de un MaterialAsset (sin E/S).
struct VfxMaterialSource {
    bool transparent = false;
    int transparent_blend = 0;  // assets::TransparentBlend: 0 vidrio, 1 alfa, 2 aditivo
    bool unlit = false;
    core::Vec4 base_color{1.0f, 1.0f, 1.0f, 1.0f};
    core::Vec3 emissive{};
    float emissive_intensity = 1.0f;
    std::string albedo;
};
VfxMaterial vfxMaterialFrom(const VfxMaterialSource& source);
// Del material a la instancia (mezcla, luz, color y emision; la textura la
// carga quien llama).
void applyVfxMaterial(const VfxMaterial& material, gfx::VfxInstanceDesc& desc);

// Distorsion del VFX Graph: la cvar render.vfx.Distortion y la calidad
// (apagada en Baja, nivel 0).
bool vfxDistortionCvar();
inline bool vfxDistortionForQuality(int quality_level) { return quality_level >= 1; }

// --- Escalabilidad por tipo de efecto ------------------------------------------------
// Un MMO con muchos jugadores lanzando a la vez: cada efecto dice que es y la
// calidad (y la plataforma) decide cuanto se permite de cada tipo: hasta que
// distancia, cuantos a la vez, cuantas particulas, que salidas caras se
// apagan y que pasa al pasarse. "Sin tipo" (lo de antes): sin limites.
enum class EffectType : int {
    Hero = 0,     // los del jugador propio (nunca se quitan por distancia)
    Combat = 1,   // habilidades de los demas
    Impact = 2,   // golpes e impactos (nacen y mueren sin parar)
    Ambient = 3,  // fuego de antorchas, polvo, cascadas...
    None = 4,     // sin tipo: sin limites (los .crvfx de antes)
};
inline constexpr int kEffectTypes = 4;      // los que tienen presupuesto
inline constexpr int kEffectTypeSlots = 5;  // + "sin tipo" (estadisticas)
const char* effectTypeKey(EffectType type);    // "Hero"... (en el .crvfx)
const char* effectTypeLabel(EffectType type);  // "Heroe"... (editor)
EffectType effectTypeFromKey(const std::string& key, EffectType fallback = EffectType::None);

// Que pasa con los de un tipo que no caben en su presupuesto.
enum class OverBudget : int {
    DontCreate = 0,      // los nuevos no entran (los que ya estaban siguen)
    RemoveFarthest = 1,  // entran los mas cercanos; los lejanos se quitan
    Pause = 2,           // los lejanos se quedan congelados y sin dibujar
};
const char* overBudgetKey(OverBudget policy);

struct EffectTypeBudget {
    float max_distance = 0.0f;           // m a la camara (0 = sin limite)
    int max_instances = 0;               // a la vez (0 = sin limite)
    float particle_scale = 1.0f;         // multiplica lo que nace (y la capacidad)
    std::uint32_t disabled_outputs = 0;  // gfx::kVfxOut* apagadas
    OverBudget policy = OverBudget::RemoveFarthest;
};

enum class VfxPlatform : int { Desktop = 0, Mobile = 1 };
VfxPlatform currentPlatform();

struct VfxScalability {
    std::array<EffectTypeBudget, kEffectTypes> types{};
    // Precalentado: repartido en tantos frames (sin tirones); 0 = sin
    // precalentado (las calidades bajas).
    int prewarm_frames = 8;
};
// Tabla por defecto: calidad 0 Baja, 1 Media, 2 Alta, 3 Ultra (vfx.quality).
// Pensada para un MMO: en Baja (Radeon Vega 7) el VFX entero cabe en ~2 ms.
VfxScalability defaultScalability(int quality, VfxPlatform platform);

// Un efecto que pide sitio este frame (VfxSystem lo rellena; aparte para los tests).
struct BudgetCandidate {
    EffectType type = EffectType::None;
    float distance = 0.0f;      // a la camara
    bool in_view = true;        // dentro de la vista (con caja fija) o sin caja
    bool running = false;       // ya estaba vivo el frame anterior
    // Salida: la decision.
    enum class Decision : int { Run = 0, Hidden = 1, Paused = 2, Removed = 3 };
    Decision decision = Decision::Run;
};
// Aplica distancias, cuantos a la vez y la politica de cada tipo. Hidden:
// fuera de la vista (se simula sin dibujar si no tiene bucle; si no, en pausa).
void applyBudget(std::vector<BudgetCandidate>& candidates, const VfxScalability& scalability);

// Rafagas repartidas. takeSpread: cuantas nacen este frame. Con frames > 1,
// `added` va a lo pendiente y sale en `frames` frames (a `rate` por frame,
// como poco `min_per_frame`: las pocas salen ya); con frames <= 1, `added`
// sale entero. Lo pendiente (precalentado de queueSpread) sale a su ritmo.
std::uint32_t takeSpread(std::uint32_t& backlog, std::uint32_t& rate, std::uint32_t added, int frames,
                         std::uint32_t min_per_frame = 64);
// Deja `count` pendientes para repartir en `frames` frames (precalentado).
void queueSpread(std::uint32_t& backlog, std::uint32_t& rate, std::uint32_t count, int frames,
                 std::uint32_t min_per_frame = 64);

// Esfera dentro de la vista (planos laterales de view_projection; sin el
// cercano ni el lejano: vale con cualquier convencion de profundidad).
bool sphereInView(const core::Mat4& view_projection, const core::Vec3& center, float radius);

// --- Asset -----------------------------------------------------------------------

struct VfxGraph {
    Uuid uuid{};
    int capacity = 2048;          // particulas vivas como mucho
    bool world_space = true;      // las vivas no siguen al emisor
    float duration = 5.0f;        // s de un ciclo de Spawn
    bool loop = true;
    float start_delay = 0.0f;
    float prewarm = 0.0f;         // s simulados al empezar (aproximado: nacen ya)
    // Escalabilidad: tipo (presupuesto por calidad), orden de dibujo entre
    // transparentes, caja fija (en el espacio del emisor: fuera de la vista no
    // se dibuja) y rafagas grandes repartidas en varios frames.
    EffectType effect_type = EffectType::None;
    int sort_order = 0;               // mayor = encima
    bool fixed_bounds = false;
    core::Vec3 bounds_center{};
    core::Vec3 bounds_size{4.0f, 4.0f, 4.0f};
    int spawn_spread_frames = 1;      // 1 = todas en su frame
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
// 6 Luciernagas, 7 Nieve, 8 Lluvia, 9 Estela/rastro, 10 Estela (cinta),
// 11 Estela de proyectil).
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
// desactivados fuera) y la salida (color/tamano en la vida...). Los Trigger
// llevan en VfxBlock::pad0 su indice en triggerTargets(graph): quien compila
// pone en out.event_targets el id de la instancia de cada uno.
void compileGraph(const VfxGraph& graph, const ParamValues& params, gfx::VfxInstanceDesc& out);
// Los .crvfx que hacen nacer los bloques Trigger activos, en su orden (el
// indice de cada bloque; uno repetido sale repetido).
std::vector<Uuid> triggerTargets(const VfxGraph& graph);
// Los canales de datos que usa el efecto (bloques Emit* activos y "Al recibir
// un evento de la GPU"), sin repetir: el VfxBlock::pad1 de esos bloques es el
// indice aqui y pad0 el numero del canal, que pone VfxSystem (hasta entonces
// VfxPass::kNoChannel: no apuntan ni escuchan nada). Los "Al
// recibir" van en out.listens (como mucho VfxPass::kMaxListens); con alguno
// el efecto simula en el mundo.
std::vector<std::string> eventChannels(const VfxGraph& graph);

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

// --- Las mismas cuentas que los shaders (vfx_sim.comp / vfx.vert), para los
// tests: si cambian alli, cambian aqui ---
// Flipbook: P3.w de una particula = cuadro inicial + (velocidad - 0.5) en la
// fraccion (velocidad 1 sin fraccion: los .crvfx de antes).
float flipbookStartValue(int frame, float speed);
struct FlipbookSample {
    int current = 0;
    int next = 0;
    float blend = 0.0f;  // cuanto del siguiente (flip_blend)
};
FlipbookSample flipbookSample(float start_value, float age, float lifetime, int frames, float fps);
// Cinta: id (0..255) y eje "de cara al emisor" en un float (vfxRibbonPack) y
// la clave de la ordenacion con varias cintas (id, edad).
float ribbonPack(std::uint32_t id, const core::Vec3& axis);
std::uint32_t ribbonPackedId(float packed);
core::Vec3 ribbonPackedAxis(float packed);
float ribbonSortKey(std::uint32_t id, float age);

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

// Por tipo de efecto (perfilador, editor, MCP).
struct VfxTypeStats {
    std::uint32_t instances = 0;  // simulandose este frame
    std::uint32_t culled = 0;     // fuera por distancia, vista o presupuesto
    std::uint32_t particles = 0;  // vivas (lectura de la GPU, un par de frames tarde)
    float cpu_ms = 0.0f;          // VfxSystem::update de los de este tipo
    float gpu_ms = 0.0f;          // estimado: el pase VFX repartido por particulas
};

struct VfxSystemStats {
    std::uint32_t effects = 0;
    std::uint32_t alive = 0;
    std::uint32_t capacity = 0;
    std::uint32_t pool = 0;
    std::uint64_t memory_bytes = 0;
    std::array<VfxTypeStats, kEffectTypeSlots> types{};  // EffectType (None al final)
    std::uint32_t one_shots = 0;  // playAt vivos
    std::uint32_t parked = 0;     // ranuras en el pool de instancias
    int quality = 2;              // la de la tabla que se aplica
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

    // --- API (scripts, MCP, Inspector) ---
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

    // --- Escalabilidad (MMO) ---
    // La camara de este frame (antes de update): distancia y vista de cada
    // efecto con tipo o caja fija. Sin ella, ni distancia ni vista.
    void setCamera(const core::Vec3& position, const core::Mat4& view_projection);
    // Tabla propia (en lugar de defaultScalability de vfx.quality); reset vuelve a la de serie.
    void setScalability(const VfxScalability& table);
    void resetScalability();
    // La que se aplica este frame.
    const VfxScalability& scalability() const { return frame_scalability_; }
    // Tiempo del juego para los efectos (pausa = 0, camara lenta < 1): la
    // simulacion y el flipbook (va por la edad). Se multiplica por vfx.timeScale.
    void setTimeScale(float scale) { time_scale_ = scale > 0.0f ? scale : 0.0f; }
    float timeScale() const;
    // Pool de instancias: un efecto suelto en un punto, sin entidad (impactos,
    // golpes). Sin bucle termina solo; las ranuras de la GPU se reutilizan.
    // Sin subemisores. false si el .crvfx no existe o su tipo no cabe
    // (DontCreate).
    bool playAt(const Uuid& graph, const core::Vec3& position, const core::Vec3& forward = core::Vec3{0.0f, 0.0f, 1.0f});

    // --- Canales de datos (Effects.pushEvent, MCP vfx_push_event) ---
    // Un evento {tipo, posicion, velocidad, color, valor} en el canal: lo
    // leen el frame siguiente los efectos con "Al recibir un evento de la
    // GPU" de ese canal (impactos que confirma el servidor: un solo efecto los
    // dibuja todos). Como mucho VfxPass::kMaxChannelEvents por canal y frame.
    // false si ya hay VfxPass::kMaxChannels canales distintos.
    bool pushGpuEvent(const std::string& channel, const gfx::VfxEventRecord& event);
    // Numero del canal (se crea al pedirlo); VfxPass::kNoChannel si no caben mas.
    std::uint32_t channelId(const std::string& channel);
    // Eventos de los scripts que aun no se han dado al pase (todos los canales).
    std::size_t pendingGpuEvents() const;

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
        double distance = 0.0;      // m recorridos por el emisor (cintas con UV por longitud)
        // Escalabilidad: rafagas y precalentado repartidos, fraccion de la
        // escala de particulas y si corrio el frame anterior (DontCreate).
        std::uint32_t spawn_backlog = 0;
        std::uint32_t spread_rate = 0;
        float scale_carry = 0.0f;
        bool budget_running = false;
        // Cintas: lo del ultimo frame, para dejar la cola apagandose (Ghost)
        // si la entidad desaparece.
        bool ribbon = false;
        core::Mat4 last_transform = core::Mat4::identity();
        ParamValues last_params;
        // Subemisores (los .crvfx de sus bloques Trigger): instancias propias
        // que solo nacen por eventos de la GPU, una por .crvfx.
        struct SubEmitter {
            Uuid graph{};
            std::uint64_t id = 0;
            bool reset = true;
            bool seen = false;
            std::shared_ptr<const std::vector<core::Vec4>> points;
            Uuid points_model{};
        };
        std::vector<SubEmitter> children;
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
    // Textura, malla de la salida y puntos de PositionMesh de un efecto.
    void resolveResources(const Uuid& graph_uuid, const VfxGraph& graph, std::shared_ptr<const std::vector<core::Vec4>>& points,
                          Uuid& points_model, gfx::VfxInstanceDesc& desc, gfx::VulkanRenderer& renderer);
    // Ranura en el pase de la malla de la salida Malla (-1 si no esta lista).
    int outputMesh(const Uuid& model, gfx::VulkanRenderer& renderer);
    // pad0 de los bloques de canales: del indice en eventChannels(graph) al
    // numero del canal (sin bloques de canales, no hace nada).
    void resolveChannels(const VfxGraph& graph, gfx::VfxInstanceDesc& desc);

    GraphResolver resolver_;
    ModelProvider model_provider_;
    std::filesystem::path assets_root_;
    std::unordered_map<Uuid, CachedGraph> graphs_;
    std::unordered_map<Uuid, VfxGraph> overrides_;
    std::unordered_map<entt::entity, Instance> instances_;
    std::unordered_map<Uuid, std::shared_ptr<const std::vector<core::Vec4>>> mesh_points_;
    // Mallas de la salida Malla ya preparadas (CPU; el pase las sube una vez
    // por clave) y las que no caben (no se reintenta cada frame).
    std::unordered_map<Uuid, std::shared_ptr<const gfx::VfxMeshData>> output_meshes_;
    std::unordered_map<Uuid, bool> output_mesh_failed_;
    // Materiales (.crmat) de la salida Malla, leidos una vez (reloadGraphs los
    // vuelve a leer).
    VfxMaterial& vfxMaterial(const Uuid& uuid);
    std::unordered_map<Uuid, VfxMaterial> materials_;
    std::vector<Uuid> trigger_scratch_;
    // Canales de datos: nombre -> numero (0..kMaxChannels-1, mientras viva el
    // sistema) y los eventos de los scripts por canal hasta el proximo update.
    std::unordered_map<std::string, std::uint32_t> channels_;
    std::vector<std::vector<gfx::VfxEventRecord>> channel_events_;
    // Cintas cuya entidad desaparecio (el proyectil impacto): se siguen
    // simulando sin nacer hasta que muere su ultimo punto, asi la cola se
    // apaga sola en vez de cortarse de golpe.
    struct Ghost {
        gfx::VfxInstanceDesc desc;
        float remaining = 0.0f;  // s hasta que muere el ultimo punto
    };
    std::vector<Ghost> ghosts_;

    // --- Escalabilidad ---
    // Presupuesto de este frame (planFrame) por entidad; las que no estan, Run.
    void planFrame(ecs::World& world, bool simulate, const std::vector<entt::entity>& preview);
    BudgetCandidate::Decision decisionFor(entt::entity handle) const;
    // Lo que la calidad cambia en un efecto ya compilado (orden, salidas,
    // capacidad, visible).
    void applyTypeLimits(const VfxGraph& graph, BudgetCandidate::Decision decision, gfx::VfxInstanceDesc& desc) const;
    // Cuantas nacen con la escala de particulas de su tipo (y lo repartido).
    std::uint32_t scaledSpawn(const VfxGraph& graph, Instance& inst, std::uint32_t spawn, bool first, std::uint32_t warm) const;
    // Los playAt de este frame (se suman a `descs`).
    void updateOneShots(float delta_seconds, std::vector<gfx::VfxInstanceDesc>& descs, VfxSystemStats& stats,
                        gfx::VulkanRenderer& renderer);
    // Instancias por tipo, particulas, ms (CPU y GPU estimado) y el perfilador.
    void finishStats(VfxSystemStats& stats, gfx::VulkanRenderer& renderer);
    void noteScalable(const VfxGraph& graph);

    VfxScalability custom_scalability_{};
    bool has_custom_scalability_ = false;
    VfxScalability frame_scalability_{};
    bool any_scalable_ = false;  // algun .crvfx cargado con tipo o caja fija (si no, planFrame no hace nada)
    bool has_camera_ = false;
    core::Vec3 camera_position_{};
    core::Mat4 camera_view_projection_ = core::Mat4::identity();
    float time_scale_ = 1.0f;
    std::unordered_map<entt::entity, BudgetCandidate::Decision> decisions_;
    std::vector<BudgetCandidate> plan_scratch_;
    std::vector<entt::entity> plan_entities_;
    // id de cada instancia de este frame -> su tipo (estadisticas).
    std::vector<std::pair<std::uint64_t, int>> type_ids_;
    struct OneShot {
        Instance inst;
        core::Mat4 transform = core::Mat4::identity();
        float lifetime = 0.0f;  // s hasta que se da por terminado
    };
    std::vector<OneShot> one_shots_;

    std::uint64_t next_id_ = 1;
    VfxSystemStats stats_{};
    gfx::VulkanRenderer* renderer_ = nullptr;
};

// El sistema activo (el del editor o el juego): lo usan los scripts y el MCP.
VfxSystem* activeSystem();
void setActiveSystem(VfxSystem* system);

// Calidad de los efectos que ponen los presets de graficos (0 Baja .. 3
// Ultra; por defecto 2). La CVar vfx.quality (>= 0) manda sobre ella.
void setQualityPreset(int level);
int qualityLevel();

}  // namespace cramion::vfx

#endif  // CRAMION_CORE_VFX_VISUAL_EFFECT_H
