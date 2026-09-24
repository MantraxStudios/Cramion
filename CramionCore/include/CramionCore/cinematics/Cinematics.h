#ifndef CRAMION_CORE_CINEMATICS_H
#define CRAMION_CORE_CINEMATICS_H

// Camaras de cine y cinematicas, como Cinemachine + Timeline de Unity:
//
//   DollyTrack         riel: puntos unidos por una curva (Catmull-Rom), en
//                      el espacio local de su entidad; abierto o en bucle
//   DollyCart          mueve su entidad por un riel a una velocidad
//   VirtualCamera      una "camara virtual": prioridad, lente, a quien
//                      sigue (Follow) y a quien mira (LookAt), como se mueve
//                      (Body: Transposer, riel, orbita...) y como apunta
//                      (Aim: Composer...), con amortiguacion y ruido
//   CameraBrain        en la camara real (la del componente Camera): elige
//                      la camara virtual activa y mezcla al cambiar
//   CinematicSequence  una cinematica: planos (que camara virtual, cuando y
//                      cuanto dura, con que mezcla entra) y objetos que se
//                      activan en tramos. Manda sobre las prioridades
//                      mientras suena.
//
// CinematicSystem lo mueve todo cada frame (despues de la fisica, antes de
// dibujar). Se registran con registerCinematicComponents().

#include "CramionCore/ecs/World.h"

#include <CramionFX/core/Math.h>

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace cramion::cinema {

// --- Rieles -------------------------------------------------------------------

// Como se mide una posicion sobre un riel.
enum class PositionUnits : int {
    Distance = 0,    // metros desde el principio
    Normalized = 1,  // 0 = principio, 1 = final
    PathUnits = 2,   // indice de punto (1.5 = a mitad entre el 1 y el 2)
};

struct DollyWaypoint {
    core::Vec3 position{};  // en el espacio local del riel
    float roll = 0.0f;      // grados (inclinacion de la camara en ese punto)
    // Tangente (modo Bezier): el asa de salida es position + tangent y la de
    // entrada position - tangent. Cero = automatica (como el modo Suave).
    core::Vec3 tangent{};
};

enum class PathMode : int {
    Smooth = 0,  // curva que pasa por todos los puntos, tangentes automaticas
    Bezier = 1,  // cada punto con sus asas (tangentes) editables
};

struct DollyTrack {
    std::vector<DollyWaypoint> waypoints{{core::Vec3{-4.0f, 0.0f, 0.0f}, 0.0f},
                                         {core::Vec3{0.0f, 0.0f, -3.0f}, 0.0f},
                                         {core::Vec3{4.0f, 0.0f, 0.0f}, 0.0f}};
    PathMode mode = PathMode::Smooth;
    bool looped = false;
    int resolution = 16;  // tramos por segmento (medida de la longitud y gizmo)

    void reflect(ecs::PropertyVisitor& v);
};

struct DollyCart {
    Uuid track{};
    float speed = 2.0f;  // en las unidades de `units` por segundo
    float position = 0.0f;
    PositionUnits units = PositionUnits::Distance;
    bool orient_to_path = true;
    bool play_in_editor = false;  // moverse tambien fuera de Play

    void reflect(ecs::PropertyVisitor& v);
};

// Un riel ya evaluado en el mundo: longitud, puntos y el punto mas cercano.
class DollyPath {
public:
    DollyPath(const DollyTrack& track, const core::Mat4& world);

    bool valid() const { return points_.size() >= 2; }
    bool looped() const { return looped_; }
    float length() const { return cumulative_.empty() ? 0.0f : cumulative_.back(); }
    // Unidades de punto: de 0 a segments().
    float segments() const;

    float toPathUnits(float position, PositionUnits units) const;
    float fromPathUnits(float path_units, PositionUnits units) const;
    // Recorta (abierto) o da la vuelta (en bucle).
    float wrap(float path_units) const;

    core::Vec3 point(float path_units) const;
    core::Vec3 tangent(float path_units) const;
    float roll(float path_units) const;
    // Unidades del punto del riel mas cercano a `p`. Con `hint` (la posicion
    // del frame anterior, >= 0) se prefiere el minimo cercano a ella si es
    // casi tan bueno como el global: la camara no salta entre dos tramos del
    // riel que pasan a la misma distancia.
    float closest(const core::Vec3& p, float hint = -1.0f) const;
    // Unidades de la muestra i de samples().
    float sampleUnits(std::size_t i) const { return i < sample_units_.size() ? sample_units_[i] : 0.0f; }
    // Tangente efectiva del punto i en el mundo (asa de salida = punto + ella).
    core::Vec3 handle(std::size_t i) const { return i < tangents_.size() ? tangents_[i] : core::Vec3{}; }
    // Tangente automatica del punto i (la que usa el modo Suave), en el mundo.
    core::Vec3 autoTangent(std::size_t i) const;

    // Puntos de control en el mundo y la curva muestreada (gizmos).
    const std::vector<core::Vec3>& points() const { return points_; }
    const std::vector<core::Vec3>& samples() const { return samples_; }

private:
    core::Vec3 evaluate(float path_units) const;

    std::vector<core::Vec3> points_;
    std::vector<core::Vec3> tangents_;  // efectivas, en el mundo
    std::vector<float> rolls_;
    bool looped_ = false;
    int resolution_ = 16;
    // Muestras de la curva: al menos resolution_ por segmento y mas en los
    // largos (una cada ~25 cm), para que medir y buscar el punto mas cercano
    // sea fino aunque el riel mida cientos de metros.
    std::vector<core::Vec3> samples_;
    std::vector<float> sample_units_;   // unidades de punto de cada muestra
    std::vector<float> cumulative_;     // distancia hasta cada muestra
    float refine(const core::Vec3& p, std::size_t sample) const;
};

// --- Camaras virtuales ----------------------------------------------------------

enum class BodyMode : int {
    DoNothing = 0,         // se queda donde este su Transform
    Transposer = 1,        // a una distancia fija del objetivo (Follow)
    TrackedDolly = 2,      // sobre un riel (siguiendo al objetivo si Auto dolly)
    Orbital = 3,           // girando alrededor del objetivo
    HardLockToTarget = 4,  // pegada al objetivo
};

enum class BindingMode : int {
    World = 0,         // el desfase va en ejes del mundo
    TargetLocal = 1,   // el desfase gira con el objetivo (camara de persecucion)
};

enum class AimMode : int {
    DoNothing = 0,         // la rotacion de su Transform
    Composer = 1,          // mira al objetivo (LookAt) con amortiguacion
    HardLookAt = 2,        // mira al objetivo al instante
    SameAsFollowTarget = 3 // la rotacion del objetivo Follow
};

enum class BlendCurve : int { Cut = 0, EaseInOut = 1, Linear = 2, EaseIn = 3, EaseOut = 4 };
float applyBlendCurve(BlendCurve curve, float t);

struct VirtualCamera {
    int priority = 10;
    float fov = 60.0f;     // grados, vertical
    float dutch = 0.0f;    // grados (giro sobre el eje de la vista)
    Uuid follow{};
    Uuid look_at{};

    BodyMode body = BodyMode::DoNothing;
    BindingMode binding = BindingMode::World;
    core::Vec3 follow_offset{0.0f, 2.5f, 6.0f};
    core::Vec3 damping{1.0f, 1.0f, 1.0f};  // segundos (0 = rigido), por eje del mundo
    // Orbital.
    float orbit_radius = 6.0f;
    float orbit_height = 2.0f;
    float orbit_angle = 0.0f;   // grados
    float orbit_speed = 20.0f;  // grados por segundo
    // Riel.
    Uuid track{};
    float path_position = 0.0f;
    PositionUnits path_units = PositionUnits::Normalized;
    bool auto_dolly = true;        // buscar en el riel el punto mas cercano al objetivo
    float auto_dolly_offset = 0.0f;  // unidades de punto por delante (+) o por detras (-)
    float dolly_damping = 0.5f;

    AimMode aim = AimMode::Composer;
    core::Vec3 look_offset{0.0f, 1.0f, 0.0f};
    float aim_damping = 0.4f;

    // Ruido de camara en mano.
    float noise_amplitude = 0.0f;  // grados
    float noise_frequency = 0.5f;  // Hz

    void reflect(ecs::PropertyVisitor& v);
};

struct CameraBrain {
    float default_blend = 1.5f;  // segundos
    BlendCurve curve = BlendCurve::EaseInOut;
    bool update_in_editor = true;  // mover la camara tambien fuera de Play

    void reflect(ecs::PropertyVisitor& v);
};

// --- Secuencias (Timeline) ------------------------------------------------------

struct CinematicShot {
    Uuid camera{};  // la VirtualCamera del plano
    float start = 0.0f;
    float duration = 3.0f;
    float blend_in = 1.0f;  // segundos de mezcla desde el plano anterior (0 = corte)
    BlendCurve curve = BlendCurve::EaseInOut;
};

struct CinematicActivation {
    Uuid entity{};  // activo solo entre start y end (en Play)
    float start = 0.0f;
    float end = 2.0f;
};

struct CinematicSequence {
    std::vector<CinematicShot> shots;
    std::vector<CinematicActivation> activations;
    bool play_on_start = true;
    bool loop = false;
    float speed = 1.0f;

    float duration() const;
    // Plano que suena en `time` (el ultimo que haya empezado), o -1.
    int shotAt(float time) const;

    void reflect(ecs::PropertyVisitor& v);
};

// Registra los componentes de cinematicas (idempotente).
void registerCinematicComponents();

// --- Sistema ------------------------------------------------------------------

struct CameraPose {
    core::Vec3 position{};
    core::Quat rotation{};
    float fov = 60.0f;
};

class CinematicSystem {
public:
    // `playing` = modo Play: amortiguaciones, carros y secuencias. Fuera de
    // Play las camaras se colocan al instante (vista previa) y solo suena la
    // secuencia que se previsualiza en la ventana Cinematica.
    void update(ecs::World& world, float delta_seconds, bool playing);
    // Olvida el estado (al entrar o salir de Play).
    void reset();

    // --- Secuencias ---
    void play(ecs::Entity sequence, float from = 0.0f);
    void stop(ecs::Entity sequence);
    bool isPlaying(ecs::Entity sequence) const;
    float time(ecs::Entity sequence) const;
    // Vista previa en el editor: la secuencia suena (o se queda en `time` si
    // `advance` es false) aunque no estemos en Play.
    void setPreview(ecs::Entity sequence, float time, bool advance);
    void clearPreview();
    ecs::Entity previewSequence() const { return preview_; }

    // "Solo" (como en Cinemachine): la camara real usa esta camara virtual
    // pase lo que pase (para encuadrar un plano). Entity{} = sin solo.
    void setSolo(ecs::Entity camera) { solo_ = camera; }
    ecs::Entity solo() const { return solo_; }

    // --- Estado de la camara real ---
    ecs::Entity brain() const { return brain_; }
    ecs::Entity liveCamera() const { return live_; }
    ecs::Entity previousCamera() const { return blend_from_camera_; }
    bool blending() const { return blend_duration_ > 0.0f && blend_time_ < blend_duration_; }
    float blendProgress() const;
    // Hubo un corte seco este frame (el renderizador descarta sus historias).
    bool cutThisFrame() const { return cut_; }
    // Pose calculada de una camara virtual (con ruido), si la hay.
    bool poseOf(ecs::Entity camera, CameraPose& pose) const;

private:
    struct VcamState {
        core::Vec3 position{};
        core::Quat rotation{};
        float dolly = 0.0f;       // unidades de punto (amortiguadas)
        float orbit = 0.0f;       // grados
        bool initialized = false;
        CameraPose output{};
        std::uint64_t frame = 0;
    };
    struct SequenceState {
        float time = 0.0f;
        bool playing = false;
        bool started = false;
    };

    void updateCarts(ecs::World& world, float dt, bool playing);
    void updateSequences(ecs::World& world, float dt, bool playing);
    void updateCamera(ecs::World& world, ecs::Entity entity, VirtualCamera& vcam, float dt, bool playing);
    void updateBrain(ecs::World& world, float dt, bool playing);

    std::unordered_map<entt::entity, VcamState> vcams_;
    std::unordered_map<entt::entity, SequenceState> sequences_;
    std::uint64_t frame_ = 0;
    float clock_ = 0.0f;

    ecs::Entity solo_;
    ecs::Entity preview_;
    float preview_time_ = 0.0f;
    bool preview_advance_ = false;

    ecs::Entity brain_;
    ecs::Entity live_;
    ecs::Entity blend_from_camera_;
    CameraPose blend_from_{};
    CameraPose output_{};
    bool has_output_ = false;
    float blend_time_ = 0.0f;
    float blend_duration_ = 0.0f;
    BlendCurve blend_curve_ = BlendCurve::EaseInOut;
    int live_shot_ = -1;
    ecs::Entity live_sequence_;
    bool cut_ = false;
};

}  // namespace cramion::cinema

#endif  // CRAMION_CORE_CINEMATICS_H
