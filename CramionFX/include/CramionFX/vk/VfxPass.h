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
//     EMIT_EVENTS  las de los eventos que otro efecto apunto el frame anterior
//     UPDATE   bloques Update (fuerzas, ruido, colisiones contra el depth de
//              la escena...), integracion, muerte o a la lista de vivas; los
//              Trigger (al morir, al chocar) apuntan eventos en otro efecto
//   Dibujo (VfxOutput): quads de cara a la camara, estirados por la velocidad
//   o horizontales, con textura (flipbook); cintas continuas entre las vivas
//   ordenadas por edad (estelas de espada y proyectil); o una malla por
//   particula (casquillos, escombros). Color y tamano a lo largo de la vida,
//   mezcla aditiva / alfa / premultiplicada y particulas suaves.
//
//   Eventos de la GPU (subemisores): un efecto que recibe eventos tiene una
//   lista (VfxInstanceDesc::event_target); los bloques TriggerOnDie /
//   TriggerOnCollide de otro apuntan en ella (posicion, velocidad, cuantas
//   nacen) y el frame siguiente nacen alli sin pasar por la CPU.
//
//   Canales de datos (eventos con nombre, como los Data Channels de
//   Niagara): los bloques Emit* de cualquier efecto (al morir, al chocar,
//   cada t segundos) y los scripts (pushEvents) apuntan {tipo, posicion,
//   velocidad, color, valor} en un canal con un tope por frame; los efectos
//   que lo escuchan (OnGpuEvent) nacen alli el frame siguiente con un
//   dispatch indirecto desde el contador del canal. Un solo efecto dibuja
//   todos los impactos de una zona. Sin nadie que escuche, no cuesta nada.

#include "CramionFX/core/Math.h"
#include "CramionFX/vk/VulkanBuffer.h"
#include "CramionFX/vk/VulkanCommon.h"
#include "CramionFX/vk/VulkanTexture.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <unordered_map>
#include <utility>
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
    FlipbookFrame = 16,    // a.x/a.y cuadro inicial min/max, a.z variacion de la velocidad (0..0.5: +-)
    InheritVelocity = 17,  // a.x factor de la velocidad del emisor
    // Cinta: a.x id de la cinta (0..255), a.y tiras (id + orden de nacimiento
    // % tiras), a.z ancho (m; 0 = el tamano), a.w giro min (grados); b.x giro
    // max (grados), b.yzw separacion entre tiras (espacio del emisor)
    RibbonAttributes = 48,
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
    // --- Eventos (Update): pad0 = indice en VfxInstanceDesc::event_targets
    // (el pase lo cambia por la ranura del destino), pad1 = su lista ---
    TriggerOnDie = 32,      // a.x nacen por evento, a.y probabilidad, a.w velocidad heredada
    TriggerOnCollide = 33,  // igual; a.z rapidez minima del choque (m/s)
    // --- Canales de datos (eventos con nombre): pad0 = canal (< kMaxChannels,
    // lo pone VfxSystem). Los Emit* (Update) apuntan {tipo, posicion,
    // velocidad, color, valor} y OnGpuEvent (escucha, va tras los Update) nace
    // alli el frame siguiente con un dispatch indirecto ---
    EmitOnDeath = 60,      // a rgba color; b.x tipo, b.y valor, b.z probabilidad; flags 1 = por el color de la particula
    EmitOnCollision = 61,  // igual; b.w rapidez minima del choque (m/s)
    EmitEvery = 62,        // igual; b.w periodo (s)
    OnGpuEvent = 63,       // a.x tipo (< 0 = todos), a.y nacen por evento, a.z maximo por frame (0 = sin limite),
                           // a.w velocidad heredada; flags 1 = color del evento, 2 = valor por el tamano
    // --- Malla ---
    MeshIndex = 80,        // (Initialize) a.x/a.y variante min/max: la guarda en el cuadro inicial (P3.w)
    AngularVelocity = 81,  // (Update) a.x aceleracion de giro (rad/s2), a.y rozamiento (1/s), a.z rad por m (rodar)
};

// Bloque que apunta en un canal de datos (Emit*).
inline bool vfxWritesChannel(std::uint32_t type) {
    return type >= static_cast<std::uint32_t>(VfxBlockType::EmitOnDeath) &&
           type <= static_cast<std::uint32_t>(VfxBlockType::EmitEvery);
}

// Un evento de un canal que apunta la CPU (scripts: impactos que confirma el
// servidor...). Lo mismo que apuntan los Emit* en la GPU.
struct VfxEventRecord {
    float type = 0.0f;
    core::Vec3 position{};
    core::Vec3 velocity{};
    core::Vec4 color{1.0f, 1.0f, 1.0f, 1.0f};
    float value = 1.0f;
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

// Distortion: no pinta; suma un desplazamiento al buffer de distorsion de la
// escena (el de ParticlePass, lo aplica composite.frag). Opaque: sin mezcla
// (mallas con material opaco; sin escribir el depth, ordenadas de atras
// adelante).
enum class VfxBlend : std::uint32_t { Additive = 0, Alpha = 1, Premultiplied = 2, Distortion = 3, Opaque = 4 };
enum class VfxOrient : std::uint32_t { FaceCamera = 0, Stretched = 1, Horizontal = 2, Vertical = 3 };
// Que se dibuja por particula.
enum class VfxOutput : std::uint32_t { Quads = 0, Ribbon = 1, Mesh = 2 };
// Cinta: hacia donde se abre su ancho y como corre la textura.
// Emitter: su eje Y al nacer cada punto; CustomFacing: la cinta mira hacia
// ribbon_vector (ancho = tangente x vector); CustomSide: el ancho va por
// ribbon_vector (los dos en el espacio del emisor).
enum class VfxRibbonFacing : std::uint32_t { Camera = 0, Emitter = 1, CustomFacing = 2, CustomSide = 3 };
// Stretch: 0..1 por orden en la lista; Tile (Distancia): una repeticion cada
// ribbon_tile m recorridos por el emisor (no resbala); Age: edad / vida de
// cada punto (0 en la cabeza, 1 en la cola).
enum class VfxRibbonUv : std::uint32_t { Stretch = 0, Tile = 1, Age = 2 };
// Malla: como gira cada una. Fixed: el cuaternion mesh_rotation y el giro de
// la particula en su Y; Camera: su +Z hacia la camara (giro = alabeo).
enum class VfxMeshOrient : std::uint32_t { Random = 0, Velocity = 1, Fixed = 2, Camera = 3 };
// Mallas por efecto: la principal y hasta 3 variantes (o niveles de LOD).
inline constexpr std::uint32_t kVfxMeshVariants = 4;
// Distorsion: desplazamiento maximo (px a 1080p).
inline constexpr float kVfxMaxDistortionPixels = 5.0f;

// --- Salida Luz: las particulas tambien iluminan el mundo ---
// La simulacion apunta las vivas que dan luz en un buffer pequeno; frames
// despues (cuando vuelve su hueco de frame) la CPU las lee, elige las que
// entran (VfxLightSelector) y el renderizador las suma a las luces por
// clusteres, sin sombras.
// Caida: la fisica (inversa del cuadrado, como las demas luces) o lineal con
// exponente: (1 - d / alcance) ^ exponente.
enum class VfxLightFalloff : std::uint32_t { Physical = 0, Linear = 1 };
// Cuales ganan si no caben: las mas fuertes (intensidad x alcance) o las mas
// cercanas a la camara.
enum class VfxLightPriority : std::uint32_t { IntensityRadius = 0, CameraDistance = 1 };

// Una luz de particula lista para los clusteres.
struct VfxLight {
    core::Vec3 position{};          // mundo
    float radius = 1.0f;            // m (alcance)
    core::Vec3 color{};             // lineal, ya por la intensidad, el alfa y el fundido
    float exponent = 0.0f;          // > 0: caida lineal con este exponente; 0: fisica
    bool affects_translucent = false;
};

// Lo que la GPU apunto de una particula (ya con su efecto resuelto).
struct VfxLightCandidate {
    std::uint64_t key = 0;          // estable mientras viva: efecto y particula
    std::uint32_t emitter = 0;      // indice en los Emitter de select()
    core::Vec3 position{};
    float radius = 1.0f;
    core::Vec3 color{};
    float exponent = 0.0f;
    bool affects_translucent = false;
};

// Elige las luces de particula de cada frame sin parpadeos: primero el
// maximo de cada efecto y luego el tope global, con prioridad estable (las
// que ya lucen ganan un 50 % de ventaja) y fundidos (una nueva que entra
// cuando todo esta lleno sube en 0,1 s; una que pierde su hueco baja en
// 0,1 s antes de dejarlo). Sin GPU: lo prueban los tests.
class VfxLightSelector {
public:
    struct Emitter {
        std::uint32_t max_lights = 4;
        VfxLightPriority priority = VfxLightPriority::IntensityRadius;
    };
    // `dt`: s desde la eleccion anterior. `out` se rehace (como mucho `budget`).
    void select(const std::vector<VfxLightCandidate>& candidates, const std::vector<Emitter>& emitters,
                const core::Vec3& camera, std::uint32_t budget, float dt, std::vector<VfxLight>& out);
    void clear() { tracked_.clear(); }
    std::size_t tracked() const { return tracked_.size(); }

private:
    struct Tracked {
        float fade = 0.0f;
        bool leaving = false;  // perdio su hueco (o su particula): baja hasta 0 y sale
        bool wanted = false;   // entra en el tope este frame
        VfxLight light;        // lo ultimo que se supo (para bajar sin su particula)
    };
    std::unordered_map<std::uint64_t, Tracked> tracked_;
    bool full_ = false;  // el frame anterior se lleno el tope
    std::vector<std::pair<float, std::uint32_t>> order_;
    std::vector<std::uint32_t> per_emitter_;
    std::vector<std::uint32_t> chosen_;
};

// Una malla para la salida Malla: por vertice dos vec4, (posicion, u) y
// (normal, v), en el espacio de la malla; triangulos con indices en esos
// vertices.
struct VfxMeshData {
    std::vector<core::Vec4> vertices;
    std::vector<std::uint32_t> indices;
};

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
    bool flip_blend = false;       // fundir el cuadro con el siguiente (indice fraccionario)
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

    // --- Salida: cinta o malla ---
    VfxOutput output = VfxOutput::Quads;
    VfxRibbonFacing ribbon_facing = VfxRibbonFacing::Camera;
    VfxRibbonUv ribbon_uv = VfxRibbonUv::Stretch;
    float ribbon_tile = 1.0f;       // m por repeticion de la textura (Tile)
    float ribbon_gap = 0.0f;        // s: dos puntos nacidos mas separados no se unen (0 = nunca)
    float emitter_distance = 0.0f;  // m que lleva recorridos el emisor (la U por longitud)
    // Varias cintas por efecto (bloque RibbonAttributes): se ordenan por (id,
    // edad) y no se unen puntos de ids distintos.
    bool ribbon_ids = false;
    core::Vec3 ribbon_vector{0.0f, 1.0f, 0.0f};  // CustomFacing / CustomSide (espacio del emisor)
    float ribbon_tension = 0.0f;     // 0 = Catmull-Rom, 1 = tramos rectos
    int ribbon_subdivisions = 1;     // 1..16 tramos por segmento (1 = sin suavizar, coste cero)
    bool ribbon_adaptive = false;    // menos tramos en los segmentos rectos (por el angulo)
    int ribbon_atlas_cell = -1;      // celda de la textura (columnas x filas); -1 = entera
    core::Vec4 ribbon_uv_transform{1.0f, 1.0f, 0.0f, 0.0f};  // escala U V, desplazamiento U V
    int mesh = -1;                  // ranura de loadMesh (-1: no dibuja nada)
    VfxMeshOrient mesh_orient = VfxMeshOrient::Random;
    core::Vec3 mesh_scale{1.0f, 1.0f, 1.0f};  // por el tamano de la particula
    // Variantes 1..3 (ranuras de loadMesh; -1 = la principal). Cada particula
    // usa la de su indice (bloque MeshIndex) o, con mesh_lod_distance > 0, la
    // de su distancia (un nivel cada tantos m).
    std::array<int, kVfxMeshVariants - 1> mesh_variants{-1, -1, -1};
    std::uint32_t mesh_variant_count = 1;  // 1..kVfxMeshVariants
    float mesh_lod_distance = 0.0f;
    float mesh_sprite_distance = 0.0f;     // m: mas lejos, un quad (0 = nunca)
    core::Vec4 mesh_rotation{0.0f, 0.0f, 0.0f, 1.0f};  // cuaternion de Fixed
    core::Vec3 mesh_scale_jitter{};        // 0..1 por eje: escala al azar por particula
    // Material (.crmat) de la malla: color base y emision.
    core::Vec4 tint{1.0f, 1.0f, 1.0f, 1.0f};
    core::Vec3 emissive{};
    // Distorsion: px a 1080p (hasta kVfxMaxDistortionPixels); la textura es
    // un normal map o, con distortion_height, un mapa de alturas.
    float distortion = 3.0f;
    bool distortion_height = false;
    // Nacen en la posicion del emisor de su instante dentro del frame (solo
    // en el mundo): estelas sin escalones con el emisor rapido.
    bool interpolate_spawn = false;

    // --- Eventos de la GPU ---
    // Recibe eventos (otro efecto lo tiene en event_targets): tiene lista.
    bool event_target = false;
    // Destinos de sus bloques Trigger (VfxBlock::pad0 es el indice aqui).
    std::vector<std::uint64_t> event_targets;

    // --- Canales de datos ---
    // Sus bloques "Al recibir un evento de la GPU" (OnGpuEvent, pad0 = canal;
    // como mucho VfxPass::kMaxListens). Con alguno, simula en el mundo.
    std::vector<VfxBlock> listens;

    // --- Escalabilidad (CramionCore/vfx: tipo de efecto y calidad) ---
    // Orden de dibujo entre efectos: menor primero (debajo); a igual orden,
    // los transparentes de atras adelante y luego los aditivos.
    int sort_order = 0;
    // Salidas que la calidad apaga para este efecto (kVfxOut*): con
    // kVfxOutMesh la salida Malla no se dibuja.
    std::uint32_t disabled_outputs = 0;

    // --- Salida Luz (bloque ParticleLight): las vivas tambien iluminan ---
    // Color = el de la particula (con Color en la vida y su alfa). Con
    // kVfxOutLight en disabled_outputs no da luz.
    bool light = false;
    float light_radius = 2.0f;      // m
    float light_intensity = 1.0f;
    VfxLightFalloff light_falloff = VfxLightFalloff::Physical;
    float light_exponent = 2.0f;    // caida lineal: (1 - d / alcance) ^ exponente
    std::uint32_t light_max = 4;    // luces de este efecto como mucho
    VfxLightPriority light_priority = VfxLightPriority::IntensityRadius;
    bool light_translucent = false;  // tambien el vidrio, lo transparente y las particulas iluminadas
};

// Salidas caras que la calidad puede apagar por tipo de efecto (bits de
// VfxInstanceDesc::disabled_outputs). Luz y distorsion: para las salidas que
// emiten luz o deforman la imagen (quien las dibuje mira el bit).
enum VfxOutputBits : std::uint32_t { kVfxOutLight = 1u, kVfxOutDistortion = 2u, kVfxOutMesh = 4u };

// Orden de dibujo entre efectos (VfxPass::prepare). true si `a` va antes.
struct VfxDrawKey {
    int sort_order = 0;
    bool additive = true;
    float distance = 0.0f;  // a la camara
};
inline bool vfxDrawBefore(const VfxDrawKey& a, const VfxDrawKey& b) {
    if (a.sort_order != b.sort_order) return a.sort_order < b.sort_order;
    if (a.additive != b.additive) return !a.additive;  // transparentes primero
    return !a.additive && a.distance > b.distance;     // de atras adelante
}

// Ranuras aparcadas (pool de instancias): un efecto que desaparece deja su
// rango del pool y su ranura aqui un rato; uno nuevo con la misma capacidad
// los reutiliza sin buscar hueco ni crecer el pool (impactos que nacen y
// mueren cada frame). Lo usa VfxPass; aparte para probarlo sin la GPU.
template <typename Token>
class VfxSlotRecycler {
public:
    explicit VfxSlotRecycler(std::size_t max_parked = 32) : max_(max_parked) {}
    // false si no cabe (quien llama libera el token).
    bool park(std::uint32_t capacity, const Token& token) {
        if (parked_.size() >= max_ || capacity == 0) return false;
        parked_.push_back(Entry{capacity, token});
        return true;
    }
    // Uno aparcado con esa capacidad exacta (el mas reciente).
    bool take(std::uint32_t capacity, Token& out) {
        for (std::size_t i = parked_.size(); i-- > 0;) {
            if (parked_[i].capacity != capacity) continue;
            out = parked_[i].token;
            parked_.erase(parked_.begin() + static_cast<std::ptrdiff_t>(i));
            return true;
        }
        return false;
    }
    // El mas antiguo (para liberarlo si hace falta sitio).
    bool evictOldest(Token& out) {
        if (parked_.empty()) return false;
        out = parked_.front().token;
        parked_.erase(parked_.begin());
        return true;
    }
    std::size_t size() const { return parked_.size(); }
    void clear() { parked_.clear(); }

private:
    struct Entry {
        std::uint32_t capacity = 0;
        Token token{};
    };
    std::size_t max_ = 32;
    std::vector<Entry> parked_;
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
    core::Vec3 to_sun{0.0f, 1.0f, 0.0f};  // hacia el sol (mallas iluminadas)
    // Luz real de los efectos "lit" (vfx.frag): los buffers de ESTE hueco de
    // frame del renderizador. Sin `lights`, "lit" sigue con `light` solo.
    core::Vec3 ambient{0.3f, 0.3f, 0.3f};   // `light` sin el sol (HDR lineal)
    vk::Buffer lights{};                    // GpuLights (uniform)
    vk::Buffer shadows{};                   // GpuShadows (uniform)
    vk::ImageView shadow_map{};             // cascadas (array de depth)
    vk::Sampler shadow_sampler{};           // de comparacion
    vk::Buffer cluster_lights{};            // cabecera + GpuClusterLight (storage)
    vk::Buffer cluster_grid{};              // listas por cluster (storage)
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
    // Texturas de las particulas: hasta 64 si el dispositivo lo permite
    // (textureLimit(); como poco 16, lo de antes). vfx_common.glsl recibe el
    // numero por una constante de especializacion.
    static constexpr std::uint32_t kMaxTextures = 64;
    static constexpr std::uint32_t kMinTextures = 16;
    static constexpr std::uint32_t kMaxPool = 1u << 21;     // particulas en total
    static constexpr std::uint32_t kMaxCapacity = 1u << 20; // por efecto
    // Mallas de la salida Malla (todas juntas; se quedan hasta destroy, como
    // las texturas).
    static constexpr std::uint32_t kMaxMeshes = 16;
    static constexpr std::uint32_t kMaxMeshVertices = 65536;
    static constexpr std::uint32_t kMaxMeshIndices = 196608;
    // Eventos: efectos que los reciben a la vez y eventos por efecto y frame
    // (los de mas se pierden). MISMO numero que VFX_MAX_EVENTS.
    static constexpr std::uint32_t kMaxEventLists = 32;
    static constexpr std::uint32_t kMaxEvents = 512;
    // Canales de datos: canales a la vez, eventos por canal y frame (los de
    // mas se pierden) y bloques de escucha por efecto. MISMOS numeros que
    // VFX_MAX_CHANNELS / VFX_MAX_CHANNEL_EVENTS / VFX_MAX_LISTENS.
    static constexpr std::uint32_t kMaxChannels = 8;
    static constexpr std::uint32_t kMaxChannelEvents = 2048;
    static constexpr std::uint32_t kMaxListens = 4;
    static constexpr std::uint32_t kNoChannel = 0xFFFFFFFFu;

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
    // Eventos de la CPU para el canal `channel` (antes de setInstances): se
    // juntan con los que apunta la GPU en la proxima simulacion y los leen
    // los que escuchan el frame siguiente. Sin nadie que escuche, se tiran.
    void pushEvents(std::uint32_t channel, const VfxEventRecord* records, std::size_t count);

    // Carga una imagen para las particulas y devuelve su ranura (la misma
    // ruta, la misma ranura). -1 si no se pudo o no quedan ranuras.
    int loadTexture(const std::filesystem::path& file);
    // Sube una malla para la salida Malla y devuelve su ranura (la misma
    // `key`, la misma ranura). -1 si no cabe o no quedan ranuras. Sin esperas:
    // se escribe en una parte del buffer que ningun frame en vuelo lee.
    int loadMesh(std::uint64_t key, const VfxMeshData& mesh);

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
    // Distorsion: los efectos con mezcla Distortion de este hueco de frame
    // (tras prepare). recordDistortion, con el renderizado abierto sobre el
    // buffer de distorsion (RG16F, mezcla aditiva) y el depth en solo
    // lectura; sin ninguno no hace nada.
    bool hasDistortion(std::uint32_t frame) const {
        return frame < distortion_draws_.size() && !distortion_draws_[frame].empty();
    }
    void recordDistortion(const vk::raii::CommandBuffer& cmd, std::uint32_t frame, vk::Extent2D viewport) const;
    // La distorsion se puede apagar por calidad (preset Baja) y por la cvar
    // render.vfx.Distortion: apagada, esos efectos no se dibujan.
    void setDistortionQuality(bool enabled) { distortion_quality_ = enabled; }
    void setDistortionEnabled(bool enabled) { distortion_enabled_ = enabled; }
    bool distortionAllowed() const { return distortion_quality_ && distortion_enabled_; }

    // Vivas de un efecto (lectura, un par de frames tarde). 0 si no existe.
    std::uint32_t aliveCount(std::uint64_t id) const;
    VfxStats stats() const { return stats_; }

    // Ranuras de textura que admite este dispositivo (16..kMaxTextures).
    std::uint32_t textureLimit() const { return texture_limit_; }
    // Turbulencia con el ruido rizado horneado (volumen de 32^3, una lectura
    // por octava) en lugar de calcularlo por particula. Sin el volumen
    // (no se pudo crear) sigue el calculado.
    void setBakedCurlNoise(bool enabled) { baked_curl_ = enabled; }
    bool bakedCurlNoise() const { return baked_curl_ && curl_ready_; }
    // Ranuras aparcadas para reutilizar (pool de instancias).
    std::size_t parkedSlots() const { return recycler_.size(); }

    // --- Salida Luz (luces de particula) ---
    // Candidatas que la GPU apunta por frame entre todos los efectos (las de
    // mas se pierden; cada efecto solo apunta una parte estable de sus vivas).
    static constexpr std::uint32_t kMaxLightCandidates = 1024;
    // Tope de luces de particula por frame: el menor entre la cvar
    // (render.vfx.MaxParticleLights, VfxSystem) y el de la calidad grafica.
    void setLightBudget(std::uint32_t budget) { light_budget_ = budget; }
    void setLightQualityCap(std::uint32_t cap) { light_quality_cap_ = cap; }
    std::uint32_t lightBudget() const { return std::min(light_budget_, light_quality_cap_); }
    // Las luces de particula de este frame, para los clusteres (la
    // iluminacion de VulkanRenderer, con la fence de `frame` ya esperada).
    // Lee lo que apunto la simulacion de este hueco de frame (frames_in_flight
    // frames tarde: se adelantan con su velocidad) y elige con fundidos; sin
    // lectura nueva, las de la ultima vez. Vacio (sin coste) si ningun efecto
    // da luz.
    const std::vector<VfxLight>& gatherLights(std::uint32_t frame, const core::Vec3& camera);

private:
    struct Slot {
        std::uint32_t index = 0;     // contadores e instancia en la GPU
        std::uint32_t offset = 0;    // en el pool
        std::uint32_t capacity = 0;  // 0 = sin rango (no cabe)
        bool reset = true;           // la proxima simulacion la vacia
        bool simulated = false;      // ya tiene contadores validos (se puede dibujar)
        std::uint32_t alive = 0;     // ultima lectura
        std::uint32_t event_list = 0xFFFFFFFFu;  // recibe eventos (kMaxEventLists)
    };
    struct MeshSlot {
        std::uint64_t key = 0;
        std::uint32_t first_index = 0;
        std::uint32_t index_count = 0;
    };
    struct Range {
        std::uint32_t offset = 0;
        std::uint32_t size = 0;
    };
    struct DrawItem {
        std::uint32_t slot = 0;
        VfxBlend blend = VfxBlend::Additive;
        bool mesh = false;  // pipeline de mallas (caras traseras fuera)
        float distance = 0.0f;
        int sort_order = 0;  // VfxInstanceDesc::sort_order
    };
    // Una ranura aparcada: su indice y su rango del pool (VfxSlotRecycler).
    struct ParkedSlot {
        std::uint32_t index = 0;
        std::uint32_t offset = 0;
        std::uint32_t capacity = 0;
    };
    // Libera una aparcada (su rango y su indice).
    void releaseParked(const ParkedSlot& parked);

    vk::raii::Pipeline createDrawPipeline(vk::Format color_format, vk::Format depth_format, VfxBlend blend,
                                          bool mesh) const;
    // Crea (la primera vez) las de Opaque o Distortion. false si no se pudo.
    bool ensurePipelines(VfxBlend blend);
    void createPool(std::uint32_t size);
    bool allocate(std::uint32_t size, std::uint32_t& offset);
    void release(std::uint32_t offset, std::uint32_t size);
    bool placeSlot(Slot& slot, std::uint32_t capacity);
    void readBack(std::uint32_t frame);
    // Sitio para los canales en events_ (la primera vez que alguien escucha).
    void ensureChannels();

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
    bool warned_events_ = false;
    VfxStats stats_{};

    vk::raii::DescriptorSetLayout set_layout_{nullptr};
    vk::raii::DescriptorPool pool_{nullptr};
    std::vector<vk::raii::DescriptorSet> sets_;
    vk::raii::PipelineLayout layout_{nullptr};
    vk::raii::Pipeline sim_pipeline_{nullptr};
    // Opaca y Distorsion (quads y cintas, mallas): se crean la primera vez
    // que un efecto las usa (sin coste si nadie las usa).
    std::array<vk::raii::Pipeline, 2> opaque_pipelines_{nullptr, nullptr};
    std::array<vk::raii::Pipeline, 2> distortion_pipelines_{nullptr, nullptr};
    vk::Format color_format_ = vk::Format::eUndefined;
    vk::Format depth_format_ = vk::Format::eUndefined;
    std::vector<std::vector<DrawItem>> distortion_draws_;
    bool distortion_quality_ = true;
    bool distortion_enabled_ = true;
    // Por mezcla: quads y cintas (0..2) y mallas (3..5).
    std::array<vk::raii::Pipeline, 6> draw_pipelines_{nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    vk::raii::Sampler depth_sampler_{nullptr};
    vk::raii::Sampler texture_sampler_{nullptr};
    // Luz real: sombras y clusteres solo fuera del modo compatible (13..17;
    // en el compatible solo 13, las luces principales).
    bool full_lighting_ = true;
    VulkanBuffer dummy_uniform_;  // si la vista no trae las luces (lighting desactivada)
    VulkanBuffer dummy_storage_;  // cabecera de clusteres vacia

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
    // Ordenaciones de recordSimulate (ranura, tamano).
    struct SortJob {
        std::uint32_t slot = 0;
        std::uint32_t size = 0;  // potencia de 2, >= el bloque de vfx_sim.comp
    };
    std::vector<SortJob> sort_scratch_;
    // Lo que prepare() junta cada frame (sin reservar memoria cada vez).
    std::vector<std::uint8_t> instance_scratch_;
    std::vector<VfxBlock> block_scratch_;
    std::vector<core::Vec4> point_scratch_;

    // Mallas (CPU -> GPU, una vez) y eventos (solo GPU).
    VulkanBuffer mesh_vertices_;  // 2 vec4 por vertice
    VulkanBuffer mesh_indices_;
    std::uint32_t mesh_vertex_count_ = 0;
    std::uint32_t mesh_index_count_ = 0;
    std::array<MeshSlot, kMaxMeshes> meshes_{};
    std::uint32_t mesh_slots_used_ = 0;
    VulkanBuffer events_;  // kMaxEventLists x 2 frames x kMaxEvents x 2 vec4 (y los canales detras)
    std::vector<bool> event_list_used_;
    // Canales de datos: su sitio en events_ se reserva al primer uso; sin
    // nadie que escuche no se graba nada (ni la etapa ni las barreras).
    bool channels_ready_ = false;
    bool channels_active_ = false;       // este frame alguien escucha
    bool channels_were_active_ = false;  // el anterior (al volver, contadores a cero)
    std::array<std::vector<VfxEventRecord>, kMaxChannels> script_events_;
    std::vector<core::Vec4> upload_scratch_;

    // Salida Luz: por hueco de frame, la cabecera, los parametros por ranura
    // (los escribe prepare) y las candidatas (las escribe UPDATE; la CPU las
    // lee cuando vuelve el hueco). Ver vfx_sim.comp (VfxLightOut).
    std::vector<VulkanBuffer> light_buf_;
    std::vector<bool> light_ready_;           // la simulacion de este hueco apunto luces
    std::vector<bool> light_params_dirty_;    // sus parametros no estan a cero
    std::vector<float> light_written_clock_;  // light_clock_ al apuntarlas
    std::vector<core::Vec4> light_params_scratch_;
    float light_clock_ = 0.0f;                // s simulados (para adelantarlas)
    float light_step_ = 0.0f;                 // los del ultimo frame simulado
    float light_gather_clock_ = 0.0f;
    bool any_light_ = false;                  // algun efecto de este frame da luz
    std::uint32_t light_budget_ = 32;
    std::uint32_t light_quality_cap_ = 32;
    VfxLightSelector light_selector_;
    std::vector<VfxLightCandidate> light_candidates_;
    std::vector<VfxLightSelector::Emitter> light_emitters_;
    std::vector<VfxLight> lights_;

    VulkanTexture white_;
    std::array<VulkanTexture, kMaxTextures> textures_;
    std::array<std::filesystem::path, kMaxTextures> texture_paths_;
    std::uint32_t texture_limit_ = kMinTextures;

    // Pool de instancias: ranuras de los que desaparecen, para los nuevos.
    VfxSlotRecycler<ParkedSlot> recycler_{32};
    // Ruido rizado horneado (vfx_sim.comp, binding kCurlBinding).
    VulkanBuffer curl_volume_;
    bool curl_ready_ = false;
    bool baked_curl_ = true;
};

// --- Ruido rizado horneado ----------------------------------------------------
// Un volumen de kVfxCurlSize^3 que se repite (tileable) con el rotacional de
// un ruido de valor periodico de kVfxCurlPeriod celdas: texel i = el campo en
// q = i / kVfxCurlTexelsPerUnit. vfx_sim.comp suma una lectura (trilineal)
// por octava en q * 2^o: lo mismo que su curlNoise (cada octava pesa 0.5^o y
// su frecuencia 2^o se cancelan en el rotacional) a una fraccion del coste.
inline constexpr std::uint32_t kVfxCurlSize = 32;
inline constexpr std::uint32_t kVfxCurlPeriod = 8;  // celdas del ruido por vuelta
inline constexpr float kVfxCurlTexelsPerUnit = static_cast<float>(kVfxCurlSize) / static_cast<float>(kVfxCurlPeriod);
// El campo exacto (una octava) en q (periodico en kVfxCurlPeriod).
core::Vec3 vfxCurlReference(const core::Vec3& q);
// El volumen: kVfxCurlSize^3 vec4 (xyz el campo), x mas rapido.
void bakeVfxCurlVolume(std::vector<core::Vec4>& out);
// Lectura trilineal y con vuelta, como vfx_sim.comp (para probarlo).
core::Vec3 sampleVfxCurlVolume(const std::vector<core::Vec4>& volume, const core::Vec3& q);

}  // namespace cramion::gfx

#endif  // CRAMION_VK_VFX_PASS_H
