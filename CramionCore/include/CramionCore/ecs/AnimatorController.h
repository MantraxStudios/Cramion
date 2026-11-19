#ifndef CRAMION_CORE_ECS_ANIMATOR_CONTROLLER_H
#define CRAMION_CORE_ECS_ANIMATOR_CONTROLLER_H

// Animator Controller (como el de Unity): una maquina de estados que elige
// que clip suena en un componente Animator. Se guarda como asset
// .cranimator (JSON con UUID) y se edita en la ventana Animator del editor.
//
//   Parametros  float / int / bool / trigger (los cambia el juego o el editor)
//   Estados     un clip (por nombre del modelo o un .cranim) o un Blend Tree,
//               con velocidad y bucle
//   Blend Trees mezclan varios clips segun parametros float: 1D (andar ->
//               trotar -> correr con "velocidad") o 2D libre (moverse en 8
//               direcciones con "x" e "y"), con los ciclos sincronizados
//   Transiciones de un estado (o de "Cualquier estado") a otro cuando se
//               cumplen TODAS sus condiciones y, si se pide, al llegar a un
//               punto del clip (exit time, normalizado 0..1); con fundido
//               (duracion en segundos) entre la pose vieja y la nueva
//
// Los clips extraidos de un modelo se guardan como .cranim (AnimationClip):
// sus pistas van por NOMBRE de nodo, asi sirven para cualquier modelo con el
// mismo esqueleto.
//
// Capas (como las de Unity): ademas de la maquina de siempre (la capa base,
// `states`/`transitions`), `layers` son capas de encima, cada una con su
// maquina, su peso, Override o Aditiva, una mascara de huesos (lista o .crmask)
// y, si se quiere, sincronizada con otra capa (sigue sus estados y su ciclo con
// otros clips). Un controlador sin `layers` funciona exactamente como antes.
// Montages: un clip puntual (golpe, recarga) por encima del grafo, en el
// cuerpo entero o en el slot de una capa, con fundidos y secciones; al acabar
// vuelve solo al grafo. Eventos y curvas de los clips: ver ClipMeta.

#include "CramionCore/Uuid.h"
#include "CramionCore/asset/AssetTypes.h"

#include <CramionFX/anim/Animator.h>
#include <CramionFX/asset/Model.h>
#include <CramionFX/core/Math.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace cramion::ecs {

enum class AnimatorParameterType : int { Float = 0, Int = 1, Bool = 2, Trigger = 3 };

struct AnimatorParameter {
    std::string name;
    AnimatorParameterType type = AnimatorParameterType::Float;
    float default_value = 0.0f;  // bool/trigger: 0 o 1
};

enum class AnimatorConditionMode : int {
    If = 0,        // bool/trigger verdadero
    IfNot = 1,     // bool falso
    Greater = 2,   // float/int
    Less = 3,
    Equals = 4,    // int
    NotEquals = 5,
};

struct AnimatorCondition {
    std::string parameter;
    AnimatorConditionMode mode = AnimatorConditionMode::If;
    float threshold = 0.0f;
};

// Que suena en un estado.
enum class AnimatorMotion : int {
    Clip = 0,         // un clip
    BlendTree1D = 1,  // mezcla por un parametro (umbral de cada clip)
    BlendTree2D = 2,  // mezcla por dos parametros (posicion de cada clip), libre cartesiano
};

// Un clip de un Blend Tree.
struct BlendTreeChild {
    std::string clip_name;  // clip del modelo por nombre
    assets::AssetRef clip{{}, assets::AssetType::AnimationClip};  // o un .cranim (manda si es valido)
    float threshold = 0.0f;  // 1D: valor del parametro en que suena solo este
    core::Vec2 position{};   // 2D: punto (x, y) en que suena solo este
    float speed = 1.0f;      // multiplica la velocidad de este clip
};

struct AnimatorState {
    std::string name = "Estado";
    std::string clip_name;           // clip del modelo por nombre
    assets::AssetRef clip{{}, assets::AssetType::AnimationClip};  // o un .cranim (manda si es valido)
    float speed = 1.0f;
    bool loop = true;
    core::Vec2 position{};           // en el grafo del editor
    // Blend Tree (motion != Clip): los clips de `children` en vez de `clip`.
    AnimatorMotion motion = AnimatorMotion::Clip;
    std::string blend_parameter;     // 1D, y la X del 2D
    std::string blend_parameter_y;   // la Y del 2D
    std::vector<BlendTreeChild> children;

    bool isBlendTree() const { return motion != AnimatorMotion::Clip; }
};

// Peso de cada hijo de un Blend Tree para los valores (x, y) de sus
// parametros (y solo en 2D). Suman 1; vacio si no tiene hijos.
//   1D: entre los dos umbrales vecinos, lineal; fuera, el extremo.
//   2D libre cartesiano (Gradient Band, como Unity): cada hijo pesa segun lo
//       cerca que esta el punto de el respecto a cada uno de los demas.
std::vector<float> blendTreeWeights(const AnimatorState& state, float x, float y = 0.0f);
// Igual, en `weights` (se reutiliza: sin reservar memoria cada frame).
void blendTreeWeights(const AnimatorState& state, float x, float y, std::vector<float>& weights);

// Origen de una transicion desde "Cualquier estado".
inline constexpr int kAnyState = -1;

struct AnimatorTransition {
    int from = 0;  // indice de estado o kAnyState
    int to = 0;
    bool has_exit_time = false;
    float exit_time = 1.0f;  // normalizado (1 = al terminar el clip)
    // Fundido: segundos en que la pose pasa del estado viejo al nuevo (0 =
    // corte seco, como antes de la 0.7).
    float duration = 0.0f;
    // Inercial (por defecto, como Unreal): pasa en seco a la pose nueva y
    // apaga poco a poco la diferencia conservando el impulso de cada hueso.
    // Si no, fundido cruzado entre las dos poses (el de Unity).
    bool inertial = true;
    // El estado nuevo sigue en el punto del ciclo del viejo (andar -> correr:
    // el mismo pie delante) en vez de empezar desde el principio.
    bool sync_phase = false;
    std::vector<AnimatorCondition> conditions;
};

// --- Mascaras de huesos (Avatar Mask) -------------------------------------------

// Un hueso de una mascara: el nodo con ese nombre (o "prefijo:nombre", como
// "mixamorig:Spine") y, si `children`, todo lo que cuelga de el.
struct BoneMaskEntry {
    std::string bone;
    bool children = true;
    float weight = 1.0f;  // 0 = lo quita (p. ej. "Spine" con hijos y "LeftArm" a 0)
};

// Mascara reutilizable (.crmask, JSON con UUID).
struct BoneMask {
    Uuid uuid{};
    std::vector<BoneMaskEntry> bones;
};

bool loadBoneMask(const std::filesystem::path& path, BoneMask& out, std::string* error = nullptr);
bool saveBoneMask(const BoneMask& mask, const std::filesystem::path& path, std::string* error = nullptr);

// Peso de cada nodo del modelo segun las entradas, en orden (la ultima que
// toca a un nodo manda). Sin entradas, 1 en todos.
std::vector<float> buildMaskWeights(const std::vector<asset::Node>& nodes, const std::vector<BoneMaskEntry>& entries);
// El nombre del nodo coincide con el del hueso (igual o tras un prefijo "x:" o "x|").
bool boneNameMatches(const std::string& node, const std::string& bone);

// --- Capas ----------------------------------------------------------------------

enum class AnimatorLayerBlend : int { Override = 0, Additive = 1 };

struct AnimatorLayer {
    std::string name = "Capa";
    float weight = 1.0f;
    AnimatorLayerBlend blending = AnimatorLayerBlend::Override;
    // Mascara: un .crmask (manda si es valido) o la lista de aqui. Sin ninguna,
    // todo el cuerpo.
    assets::AssetRef mask{{}, assets::AssetType::BoneMask};
    std::vector<BoneMaskEntry> mask_bones;
    // Aditiva: la pose que se resta. Sin clip, el primer fotograma de cada clip
    // de la capa; si no, este clip (nombre del modelo o .cranim) en `reference_time`.
    std::string reference_clip_name;
    assets::AssetRef reference_clip{{}, assets::AssetType::AnimationClip};
    float reference_time = 0.0f;
    // Sincronizada: sigue el estado y el ciclo de otra capa (0 = la base, k =
    // layers[k-1], solo de las de antes) con sus propios clips: `states[i]`
    // pone el movimiento del estado i de esa capa (sin clip = el de la origen).
    // -1 = maquina propia.
    int sync_layer = -1;
    std::vector<AnimatorState> states;
    std::vector<AnimatorTransition> transitions;
    int default_state = 0;
    core::Vec2 entry_position{-260.0f, 0.0f};
    core::Vec2 any_state_position{-260.0f, 120.0f};
};

struct AnimatorController {
    Uuid uuid{};
    std::vector<AnimatorParameter> parameters;
    // Capa base (la de siempre).
    std::vector<AnimatorState> states;
    std::vector<AnimatorTransition> transitions;
    int default_state = 0;
    core::Vec2 entry_position{-260.0f, 0.0f};
    core::Vec2 any_state_position{-260.0f, 120.0f};
    std::string base_layer_name = "Base";
    // Capas por encima de la base (indice de capa k = layers[k-1]).
    std::vector<AnimatorLayer> layers;

    const AnimatorParameter* findParameter(const std::string& name) const;
    // Quita un estado y arregla los indices de transiciones y el de defecto.
    void removeState(int index);
    // Capa por nombre (0 = base) o -1.
    int findLayer(const std::string& name) const;
};

// Quita un estado de una lista con sus transiciones (lo que hace removeState
// en la base; tambien para las capas).
void removeAnimatorState(std::vector<AnimatorState>& states, std::vector<AnimatorTransition>& transitions,
                         int& default_state, int index);

// .cranimator. Un controlador nuevo recibe UUID nuevo al guardarse si no
// tiene. Los errores van a `error` (si no es nulo).
bool loadAnimatorController(const std::filesystem::path& path, AnimatorController& out,
                            std::string* error = nullptr);
bool saveAnimatorController(const AnimatorController& controller, const std::filesystem::path& path,
                            std::string* error = nullptr);

// Estado de una capa de encima en una entidad.
struct AnimatorLayerRuntime {
    int state = -1;            // -1 = aun no entro
    float state_time = 0.0f;
    int last_transition = -1;
    float phase = 0.0f;        // 0..1 del estado actual
    int fade_state = -1;       // el anterior mientras dura el fundido
    float fade_phase = 0.0f;
    float fade_time = 0.0f;
    float fade_duration = 0.0f;
    bool entered = false;      // entro en `state` en el ultimo paso (eventos del principio)
    float dphase = 0.0f;       // fase avanzada en el ultimo paso
    float fade_weight = 0.0f;  // peso del estado que se va en el ultimo paso
    float weight = -1.0f;      // el del juego (setLayerWeight); < 0 = el del controlador
    float effective_weight = 0.0f;  // el que se uso en el ultimo frame (lo pone RenderSync)
    std::string state_name;
    // CrossFade del juego en esta capa (crossFadeLayer).
    std::string cross_fade;
    float cross_fade_time = 0.15f;
};

// Montage: un clip puntual sobre el grafo (Unreal: Montage en un slot).
struct AnimatorMontage {
    // Lo pide el juego (playMontage).
    std::string clip_name;  // clip del modelo por nombre...
    assets::AssetRef clip{{}, assets::AssetType::AnimationClip};  // ...o un .cranim (manda si es valido)
    std::string slot;       // "" = cuerpo entero; si no, una capa (usa su mascara)
    float blend_in = 0.15f;
    float blend_out = 0.2f;
    float speed = 1.0f;
    std::string start_section;
    // Estado (lo lleva stepMontage).
    bool active = false;
    bool just_started = false;  // aun no ha dado su primer paso
    int clip_index = -1;
    float time = 0.0f;
    float delta = 0.0f;         // avanzado en el ultimo paso (root motion)
    float weight = 0.0f;
    float elapsed = 0.0f;
    bool stopping = false;      // stopMontage: se apaga en stop_blend segundos
    float stop_blend = 0.2f;
    float stop_elapsed = 0.0f;
    std::string jump_section;   // montageJumpToSection: se aplica en el siguiente paso
    std::string section;        // la seccion en la que va
    std::uint32_t serial = 0;   // sube con cada playMontage
};

// Evento de un clip que ya sono, pendiente de llegar a los scripts.
struct AnimatorFiredEvent {
    std::string name;
    std::string clip;
    std::string params;
    float time = 0.0f;
    int layer = 0;  // 0 = base, k = capa, -1 = montage, -2 = Motion Matching
};

// Modo de root motion del componente Animator.
enum class RootMotionMode : int {
    Off = 0,                  // la pose tal cual (como siempre)
    Transform = 1,            // mueve y gira el Transform de la entidad del Animator
    CharacterController = 2,  // move() del Character Controller (la entidad o un padre)
    Rigidbody = 3,            // velocidad del Rigidbody (la entidad o un padre)
    Script = 4,               // no mueve nada: llega OnAnimatorMove y lo hace el script
};

// Estado de la maquina en una entidad (lo guarda el componente Animator, no
// se serializa).
struct AnimatorRuntime {
    int state = -1;               // -1 = aun no entro (ira al de defecto)
    float state_time = 0.0f;      // segundos en el estado actual
    std::unordered_map<std::string, float> values;  // parametros actuales
    int last_transition = -1;     // la que causo el ultimo cambio (-1 = entrada)
    // Repeticiones (replay/Replay.h): el estado y el tiempo los pone la
    // repeticion; la maquina no avanza ni cambia de estado sola.
    bool replay_control = false;
    // CrossFade del juego (entity().crossFade("Golpe", 0.1)): salta a ese estado
    // por su nombre sin transicion, con fundido o inercializacion. Se consume
    // al aplicarse; el mismo estado vuelve a empezar.
    std::string cross_fade;
    float cross_fade_time = 0.15f;
    bool cross_fade_inertial = true;
    std::string state_name;  // nombre del estado que suena (lo pone RenderSync)

    // --- Capas, montages, eventos, curvas y root motion ---
    std::vector<AnimatorLayerRuntime> layers;  // una por controller.layers
    std::vector<std::string> layer_names;      // 0 = base (lo pone RenderSync; para buscar por nombre)
    AnimatorMontage montage;
    // Eventos que ya sonaron y aun no han llegado a los scripts (los saca la
    // fase Events del juego). Tope kMaxPendingEvents: fuera de Play nadie los
    // recoge.
    static constexpr std::size_t kMaxPendingEvents = 64;
    std::vector<AnimatorFiredEvent> events;
    // Valor de las curvas de los clips que suenan (entity().animatorCurve).
    std::unordered_map<std::string, float> curves;
    // Root motion pendiente de aplicar (mundo; lo suma RenderSync y lo consume
    // el juego): desplazamiento y giro en Y (radianes).
    core::Vec3 root_delta_position{};
    float root_delta_yaw = 0.0f;
    // El ultimo aplicado (rootMotionDelta() en los scripts).
    core::Vec3 last_root_position{};
    float last_root_yaw = 0.0f;
    // Vista previa del editor (ventana de clips): este clip del modelo en este
    // instante, por encima de todo. -1 = nada.
    int preview_clip = -1;
    float preview_time = 0.0f;

    void pushEvent(AnimatorFiredEvent event) {
        if (events.size() >= kMaxPendingEvents) events.erase(events.begin());
        events.push_back(std::move(event));
    }
};

// Avanza la maquina: aplica la primera transicion que se cumpla (consume los
// triggers de sus condiciones). `clip_duration` es la del clip del estado
// actual (0 si no tiene; en un Blend Tree, la de la mezcla). Devuelve true si
// cambio de estado (runtime.last_transition dice por cual).
bool stepAnimatorController(const AnimatorController& controller, AnimatorRuntime& runtime,
                            float clip_duration);

float animatorParameterValue(const AnimatorController& controller, const AnimatorRuntime& runtime,
                             const std::string& name);

// Como pasar de un clip de un estado a un indice del modelo y su duracion (lo
// da RenderSync; en las pruebas, el propio modelo).
struct AnimatorClipLookup {
    std::function<int(const std::string& name, const assets::AssetRef& ref)> resolve;  // -1 = no hay
    std::function<float(int clip)> duration;
};

// Muestras de un estado en su fase (0..1) con un peso (se anaden a `out` si no
// es nulo; `dphase` = fase avanzada en este paso, para el root motion).
// Devuelve la duracion del ciclo (la media de sus clips segun sus pesos).
float animatorStateSamples(const AnimatorController& controller, const AnimatorRuntime& runtime,
                           const AnimatorState& state, float phase, float dphase, float weight,
                           const AnimatorClipLookup& lookup, std::vector<anim::ClipSample>* out,
                           std::vector<float>& scratch);

// Paso de una maquina de estados cualquiera (la base o una capa): primera
// transicion que se cumpla (consume los triggers). Devuelve true si cambio.
bool stepStateMachine(const AnimatorController& controller, const std::vector<AnimatorState>& states,
                      const std::vector<AnimatorTransition>& transitions, int default_state, AnimatorRuntime& params,
                      int& state, float& state_time, int& last_transition, float clip_duration);

// De donde sale una capa sincronizada (estado y fase de la capa origen).
struct AnimatorSyncSource {
    int state = -1;
    float phase = 0.0f;
    float dphase = 0.0f;
    int fade_state = -1;
    float fade_phase = 0.0f;
    float fade_weight = 0.0f;  // peso del estado que se va (0 = sin fundido)
    bool entered = false;
};

// Avanza la capa `layer` (1..layers.size()) `dt` segundos (por `speed`):
// cross fade pedido, transiciones, fundido y fase; deja sus muestras en `out`
// (pesos que suman 1; vacio = no suena nada) y en `current_begin` el indice de
// la primera muestra del estado actual (las de antes son del que se va). Con
// `sync`, sigue a esa capa en vez de a su maquina.
void stepAnimatorLayer(const AnimatorController& controller, int layer, AnimatorRuntime& runtime, float dt,
                       float speed, const AnimatorClipLookup& lookup, std::vector<anim::ClipSample>& out,
                       std::size_t& current_begin, const AnimatorSyncSource* sync, std::vector<float>& scratch);

// Peso efectivo de una capa (0 = base: siempre 1; el del juego si lo puso, si
// no el del controlador).
float animatorLayerWeight(const AnimatorController& controller, const AnimatorRuntime& runtime, int layer);

// --- Montages --------------------------------------------------------------------

// Pide un montage (lo que hace playMontage): empieza en el siguiente paso.
void startMontage(AnimatorMontage& montage);
// Avanza un montage `dt` segundos con su clip ya resuelto: secciones,
// fundidos y final. Deja en `events` (si no es nulo) los eventos del clip que
// cruza (una vez cada uno). Al acabar, active = false y weight = 0.
void stepMontage(AnimatorMontage& montage, const asset::AnimationClip& clip, float dt,
                 std::vector<std::uint32_t>* events);
// Indice de la seccion en la que esta `time` (-1 = antes de la primera o sin secciones).
int clipSectionAt(const asset::AnimationClip& clip, float time);

// --- Eventos y curvas (ClipMeta) ---------------------------------------------------

// Lo que lleva un clip ademas de sus pistas: eventos, curvas y secciones. En
// un .cranim va en la cabecera (clave "meta", con su version; los motores de
// antes la ignoran). Los clips de dentro de un modelo (.crdata) lo guardan en
// un archivo al lado, <modelo>.crdata.crclipmeta (JSON, por nombre de clip):
// el .crdata no se toca.
struct ClipMeta {
    std::vector<asset::ClipEvent> events;
    std::vector<asset::ClipCurve> curves;
    std::vector<asset::ClipSection> sections;

    bool empty() const { return events.empty() && curves.empty() && sections.empty(); }
    // Ordena eventos, claves y secciones por tiempo.
    void sort();
};

inline constexpr int kClipMetaVersion = 1;

ClipMeta clipMetaOf(const asset::AnimationClip& clip);
void applyClipMeta(asset::AnimationClip& clip, ClipMeta meta);

// Cabecera de un .cranim: lee / reescribe solo su "meta" (las pistas no se
// tocan; escritura atomica).
bool loadClipMetaFromCranim(const std::filesystem::path& path, ClipMeta& out, std::string* error = nullptr);
bool saveClipMetaToCranim(const std::filesystem::path& path, const ClipMeta& meta, std::string* error = nullptr);

// Archivo lateral de un modelo: por nombre de clip.
std::filesystem::path clipMetaSidecarPath(const std::filesystem::path& model_path);
bool loadModelClipMeta(const std::filesystem::path& sidecar, std::map<std::string, ClipMeta>& out,
                       std::string* error = nullptr);
bool saveModelClipMeta(const std::filesystem::path& sidecar, const std::map<std::string, ClipMeta>& clips,
                       std::string* error = nullptr);

// Edicion (ventana de clips del editor). Devuelven el indice nuevo del evento.
int addClipEvent(ClipMeta& meta, const std::string& name, float time, float duration);
int moveClipEvent(ClipMeta& meta, int index, float time, float duration);
bool removeClipEvent(ClipMeta& meta, int index);

// Eventos y curvas de un juego de muestras (lo que suena en una capa):
// eventos del estado que entra (desde `current_begin`) y, en un Blend Tree,
// solo del clip que mas pesa; curvas: la media por pesos de todas.
void collectSampleEvents(const asset::ModelData& model, const anim::ClipSample* samples, std::size_t count,
                         bool include_start, int layer, AnimatorRuntime& runtime);
// Suma `weight` * curvas de las muestras a `curves` (y su peso a `weights`).
void accumulateSampleCurves(const asset::ModelData& model, const anim::ClipSample* samples, std::size_t count,
                            float weight, std::unordered_map<std::string, float>& curves);

// Linea de tiempo de la ventana de clips: tiempo bajo un punto (recortado y,
// con fps > 0, al fotograma mas cercano) y al reves.
float timelineTimeAt(float x, float x0, float width, float duration, float fps);
float timelineX(float time, float x0, float width, float duration);

// Reproduccion en la ventana de clips.
struct ClipPreviewPlayer {
    float time = 0.0f;
    bool playing = false;
    bool loop = true;
    float speed = 1.0f;
    // Avanza; sin bucle se para al final.
    void step(float dt, float duration);
};
// --- Clips sueltos (.cranim) -------------------------------------------------

// Guarda un clip de un modelo con las pistas por nombre de nodo. La primera
// linea del archivo es la cabecera (UUID, nombre, duracion) para que la base
// de datos no lea todo el clip.
bool saveAnimationClip(const asset::ModelData& model, const asset::AnimationClip& clip,
                       const std::filesystem::path& path, std::string* error = nullptr);

// Clip con las pistas reasignadas a los nodos de `model` (por nombre; las de
// nodos que no existan se descartan). false si no se pudo leer.
bool loadAnimationClip(const std::filesystem::path& path, const asset::ModelData& model,
                       asset::AnimationClip& out, std::string* error = nullptr);

}  // namespace cramion::ecs

#endif  // CRAMION_CORE_ECS_ANIMATOR_CONTROLLER_H
