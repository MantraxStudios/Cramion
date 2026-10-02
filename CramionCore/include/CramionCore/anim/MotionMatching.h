#ifndef CRAMION_CORE_ANIM_MOTION_MATCHING_H
#define CRAMION_CORE_ANIM_MOTION_MATCHING_H

// Motion Matching (como el de For Honor, The Last of Us 2 o el Pose Search de
// Unreal 5): en vez de una maquina de estados con transiciones a mano, una
// base de datos con TODOS los fotogramas de un conjunto de clips. Cada pocos
// frames se busca el fotograma que mejor encaja con
//
//   - la trayectoria que se pide (donde estara el personaje dentro de 0.33,
//     0.66 y 1 s y hacia donde mirara), sacada de la entrada del jugador o del
//     movimiento del objeto con un muelle criticamente amortiguado, y
//   - la pose que se ve ahora (posicion y velocidad de los pies y velocidad de
//     la cadera), para que el cambio sea continuo,
//
// y se salta alli con inercializacion (anim/Inertialization.h): el cuerpo
// conserva su impulso, sin fundidos que flotan.
//
// Asset .crmmdb (JSON con UUID): la lista de clips (.cranim o del modelo),
// sus etiquetas (agachado, combate...) y los pesos de cada rasgo. Los rasgos
// se calculan al usarla por primera vez con un modelo (un esqueleto puede
// tener sus propios huesos), se normalizan (media y desviacion de cada grupo)
// y se buscan por fuerza bruta con poda (miles de fotogramas en
// microsegundos).
//
// Clips "en el sitio" (el Locomotion Pack de Mixamo quita el avance de la
// cadera): su trayectoria sale de la velocidad y el giro que se le dan a mano
// en la base (m/s y grados/s). Clips con root motion: se mide de la cadera y,
// con "Quitar root motion", la pose se deja en el sitio (al personaje lo mueve
// su script o su Character Controller, como el Pose Search de Unreal con el
// Character Movement).

#include "CramionCore/Uuid.h"
#include "CramionCore/asset/AssetTypes.h"
#include "CramionCore/ecs/Reflection.h"

#include <CramionFX/asset/Model.h>
#include <CramionFX/core/Math.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace cramion::anim {

// Puntos de la trayectoria futura y tamano del vector de rasgos:
//   trayectoria 3 x (x, z) + direcciones 3 x (x, z) = 12
//   pie izquierdo y derecho: posicion (3 + 3) y velocidad (3 + 3) = 12
//   velocidad de la cadera = 3
inline constexpr int kTrajectoryPoints = 3;
inline constexpr int kMotionFeatureCount = 27;
// Donde empieza cada grupo en el vector.
inline constexpr int kFeatTrajectoryPosition = 0;   // 6
inline constexpr int kFeatTrajectoryDirection = 6;  // 6
inline constexpr int kFeatFootPosition = 12;        // 6
inline constexpr int kFeatFootVelocity = 18;        // 6
inline constexpr int kFeatHipVelocity = 24;         // 3

// Un clip de la base.
struct MotionClipEntry {
    assets::AssetRef clip{{}, assets::AssetType::AnimationClip};  // .cranim (manda si es valido)
    std::string clip_name;   // o un clip del propio modelo por su nombre
    std::string tags;        // "agachado, combate": separadas por comas
    bool loop = true;
    // Clips en el sitio: lo que avanzaria el personaje (m/s en los ejes del
    // modelo: +Z delante en Mixamo) y su giro (grados/s, + = izquierda).
    core::Vec3 velocity{};
    float turn_rate = 0.0f;
    // Recorte (segundos dentro del clip; end 0 = hasta el final).
    float start = 0.0f;
    float end = 0.0f;
    // Se suma al coste: > 0 lo elige menos, < 0 mas (un idle "preferido").
    float cost_bias = 0.0f;
};

struct MotionDatabaseAsset {
    Uuid uuid{};
    std::vector<MotionClipEntry> clips;
    float sample_rate = 30.0f;  // fotogramas por segundo de la base
    std::array<float, kTrajectoryPoints> trajectory_times{0.33f, 0.66f, 1.0f};
    // Pesos de cada grupo de rasgos (0 = no cuenta).
    float weight_trajectory_position = 1.0f;
    float weight_trajectory_direction = 1.5f;
    float weight_foot_position = 0.75f;
    float weight_foot_velocity = 1.0f;
    float weight_hip_velocity = 1.0f;
    // La pose se deja en el sitio (se quita lo que avanza y gira la cadera).
    bool strip_root_motion = true;
    // Huesos (vacio = se buscan solos en un humanoide).
    std::string hips_bone;
    std::string left_foot_bone;
    std::string right_foot_bone;
};

// .crmmdb. Al guardar, una base sin UUID recibe uno nuevo.
bool loadMotionDatabase(const std::filesystem::path& path, MotionDatabaseAsset& out, std::string* error = nullptr);
bool saveMotionDatabase(MotionDatabaseAsset& database, const std::filesystem::path& path,
                        std::string* error = nullptr);

// "agachado, Combate " -> {"agachado", "combate"} (minusculas, sin espacios).
std::vector<std::string> splitMotionTags(const std::string& text);

// Un fotograma de la base.
struct MotionFrame {
    int entry = 0;           // en MotionDatabaseAsset::clips
    int clip = -1;           // en ModelData::animations
    float time = 0.0f;       // segundos desde el inicio del recorte
    std::uint32_t tags = 0;  // bit i = MotionFeatures::tag_names[i]
    bool can_start = true;   // false al final de un clip sin bucle (no se salta ahi)
};

// Rasgos de una base para un modelo concreto.
struct MotionFeatures {
    std::vector<MotionFrame> frames;
    std::vector<float> raw;       // frames x kMotionFeatureCount, sin normalizar (unidades del modelo)
    std::vector<float> features;  // normalizados y con los pesos: se comparan tal cual
    std::array<float, kMotionFeatureCount> mean{};
    std::array<float, kMotionFeatureCount> scale{};  // desviacion del grupo / peso
    std::vector<std::string> tag_names;
    // Primer fotograma y cuantos tiene cada entrada (-1 / 0 si su clip no esta).
    std::vector<int> entry_first;
    std::vector<int> entry_count;
    std::vector<float> entry_duration;  // segundos del recorte
    std::vector<float> entry_start;     // segundo del clip en que empieza el recorte
    std::vector<int> entry_clip;        // indice en ModelData::animations
    // Raiz simulada de cada entrada (relativa a su primer fotograma), una
    // muestra por fotograma: posicion (modelo) y giro (radianes, + = izquierda).
    // La total (lo medido en la cadera + la velocidad y el giro puestos a mano)
    // da la trayectoria; la medida sola es donde esta la pose (quitar root motion).
    std::vector<std::vector<core::Vec3>> root_positions;
    std::vector<std::vector<float>> root_yaws;
    std::vector<std::vector<core::Vec3>> measured_positions;
    std::vector<std::vector<float>> measured_yaws;
    std::vector<bool> entry_loop;
    std::vector<float> entry_bias;
    int hips = -1;
    int left_foot = -1;
    int right_foot = -1;
    core::Vec3 rest_forward{0.0f, 0.0f, 1.0f};  // delante del personaje en el modelo (plano XZ)
    float sample_rate = 30.0f;
    std::array<float, kTrajectoryPoints> trajectory_times{0.33f, 0.66f, 1.0f};
    std::string error;

    bool valid() const { return !frames.empty(); }
    const float* feature(std::size_t frame) const { return features.data() + frame * kMotionFeatureCount; }
    const float* rawFeature(std::size_t frame) const { return raw.data() + frame * kMotionFeatureCount; }
    // Mascara de las etiquetas de un texto ("agachado, combate"); las que no
    // existen en la base se ignoran.
    std::uint32_t tagMask(const std::string& text) const;
    // Fotograma de la entrada en ese instante (el mas cercano), o -1.
    int frameAt(int entry, float time) const;
};

// Calcula los rasgos. `clip_indices[i]` = indice en model.animations del clip
// de la entrada i (-1 = no esta: se salta). false (y out.error) si el modelo
// no tiene los huesos o no queda ningun fotograma.
bool buildMotionFeatures(const asset::ModelData& model, const MotionDatabaseAsset& database,
                         const std::vector<int>& clip_indices, MotionFeatures& out);

// Rasgo crudo -> normalizado (el espacio en que se busca).
void normalizeMotionFeature(const MotionFeatures& features, const float* raw, float* out);
// Al reves.
void denormalizeMotionFeature(const MotionFeatures& features, const float* normalized, float* out);

struct MotionSearchResult {
    int frame = -1;
    float cost = 0.0f;
};
// El fotograma mas parecido a `query` (normalizada) con todas las etiquetas
// de `required_tags` (si ninguno las tiene, sin filtrar). Los de la entrada
// `skip_entry` entre `skip_from` y `skip_to` (segundos) no cuentan: es lo que
// ya esta sonando.
MotionSearchResult searchMotion(const MotionFeatures& features, const float* query, std::uint32_t required_tags,
                                int skip_entry = -1, float skip_from = 0.0f, float skip_to = 0.0f);
// Coste de un fotograma concreto (para saber si cambiar compensa).
float motionCost(const MotionFeatures& features, const float* query, int frame);

// Raiz simulada de una entrada en `time` (segundos desde el inicio del
// recorte; fuera de el, con bucle se encadenan ciclos y sin bucle se sigue
// con la ultima velocidad).
void motionRootAt(const MotionFeatures& features, int entry, float time, core::Vec3& position, float& yaw);
// Lo mismo solo con lo medido en la cadera (donde esta la pose).
void motionMeasuredRootAt(const MotionFeatures& features, int entry, float time, core::Vec3& position, float& yaw);

// Giro (radianes, + = izquierda) de una direccion en el plano XZ respecto a
// +Z, y al reves.
float yawOfDirection(const core::Vec3& direction);
core::Vec3 directionOfYaw(float yaw);
core::Vec3 rotateYaw(const core::Vec3& v, float yaw);

// Muelle criticamente amortiguado sobre la velocidad (Holden, "Spring-It-On"):
// avanza (velocity, acceleration) hacia `goal_velocity` y predice donde
// estara el personaje en cada instante de `times` (relativo a `position`).
void springVelocityUpdate(core::Vec3& velocity, core::Vec3& acceleration, const core::Vec3& goal_velocity,
                          float halflife, float delta_seconds);
core::Vec3 springPredictPosition(const core::Vec3& position, const core::Vec3& velocity,
                                 const core::Vec3& acceleration, const core::Vec3& goal_velocity, float halflife,
                                 float time);
// Angulo que se acerca a `goal` con amortiguamiento critico (sin velocidad
// inicial): el giro previsto dentro de `time` segundos.
float springPredictYaw(float yaw, float goal, float halflife, float time);

// --- Componente ----------------------------------------------------------------

enum class MotionInputMode : int {
    ObjectMovement = 0,  // sigue como se mueve el objeto (script, Character Controller, NavAgent)
    Script = 1,          // lo pide el juego: entity:setMotionVelocity / setMotionFacing
};

// Estado en el juego (no se guarda en la escena).
struct MotionMatchingRuntime {
    // Modo Script: velocidad (mundo, m/s) y hacia donde mirar (cero = hacia
    // donde se mueve).
    core::Vec3 desired_velocity{};
    core::Vec3 desired_facing{};
    std::string extra_tags;  // entity:setMotionTags (se suman a las del componente)
    // Repeticiones (replay/Replay.h): entry y time los pone la repeticion; no
    // se busca ni se avanza.
    bool replay_control = false;

    // Lo que suena.
    int entry = -1;
    int frame = -1;
    float time = 0.0f;
    float search_timer = 0.0f;
    float last_cost = 0.0f;
    int searches = 0;
    int switches = 0;
    std::string clip_label;

    // Simulacion (muelles) en el mundo.
    bool sim_valid = false;
    core::Vec3 last_position{};
    float last_yaw = 0.0f;
    float yaw_rate = 0.0f;           // rad/s medidos
    core::Vec3 measured_velocity{};  // m/s medidos (suavizados)
    core::Vec3 sim_velocity{};
    core::Vec3 sim_acceleration{};

    // Depuracion (mundo): trayectoria pedida y la del fotograma elegido.
    bool debug_valid = false;
    core::Vec3 debug_origin{};
    std::array<core::Vec3, kTrajectoryPoints> debug_desired{};
    std::array<core::Vec3, kTrajectoryPoints> debug_desired_dir{};
    std::array<core::Vec3, kTrajectoryPoints> debug_matched{};
    std::array<core::Vec3, kTrajectoryPoints> debug_matched_dir{};
    std::string error;
};

// Va en la entidad del modelo animado (la del MeshRenderer, junto al Animator
// si lo hay): mientras esta activo y tiene base, manda sobre el Animator.
struct MotionMatching {
    assets::AssetRef database{{}, assets::AssetType::MotionDatabase};
    bool enabled = true;
    MotionInputMode input = MotionInputMode::ObjectMovement;
    float search_interval = 0.1f;   // segundos entre busquedas
    float transition_time = 0.25f;  // inercializacion al saltar
    float switch_threshold = 0.05f; // mejora minima del coste para saltar
    float velocity_halflife = 0.2f; // muelle de la velocidad (s)
    float facing_halflife = 0.25f;  // muelle del giro (s)
    float speed = 1.0f;
    std::string required_tags;      // solo fotogramas con estas etiquetas
    bool debug_draw = true;         // trayectorias en la vista de Escena

    MotionMatchingRuntime runtime;

    void reflect(ecs::PropertyVisitor& v);
};

// Registra el componente (idempotente).
void registerMotionMatchingComponents();

}  // namespace cramion::anim

#endif  // CRAMION_CORE_ANIM_MOTION_MATCHING_H
