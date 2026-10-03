#ifndef CRAMION_CORE_ANIM_FOOT_PLACEMENT_H
#define CRAMION_CORE_ANIM_FOOT_PLACEMENT_H

// Pies al suelo (IK de piernas y patas) para humanoides y animales:
//
//   - Cada pie mira el suelo bajo el tobillo y bajo la punta (si el esqueleto
//     la tiene): en un escalon la punta no se mete en el y el talon no se
//     queda colgando del borde. El pie se gira con la pendiente (limitado).
//   - El suelo de cada pie se guarda en el MUNDO: cuando el personaje sube
//     un escalon de golpe (el Character Controller lo sube en un paso), el pie
//     apoyado se queda donde estaba y la cadera no pega un salto.
//   - "Apoyado" sale de la animacion: el pie que esta abajo (cerca de la
//     altura minima que le da el clip). Solo esos bajan a buscar el suelo y
//     sujetan la cadera; el que va por el aire sigue a la cadera y solo sube
//     si debajo hay algo mas alto (no tropieza con el escalon).
//   - La cadera (o el cuerpo de un animal) baja lo que haga falta para que
//     los pies apoyados lleguen y sube si todos pisan mas alto; los saltos de
//     la base se suavizan (filtro que no se retrasa en rampas ni ascensores).
//   - En el aire (saltar, caer) se desvanece: no tira de los pies hacia el
//     suelo ni hunde la cadera al despegar.
//   - Animales con 3 o mas patas: el cuerpo se inclina con el suelo.
//
// Sin dependencias del ECS: RenderSync lo llama con la pose del Animator y un
// rayo contra la fisica; las pruebas, con un suelo de mentira.

#include "CramionCore/anim/IK.h"

#include <CramionFX/core/Math.h>

#include <functional>
#include <vector>

namespace cramion::ik {

// Suelo bajo un punto: rayo en el mundo -> punto y normal (false = nada).
using GroundRay = std::function<bool(const core::Vec3& origin, const core::Vec3& direction, float max_distance,
                                     core::Vec3& point, core::Vec3& normal)>;

struct GroundLeg {
    std::vector<int> joints;  // de la cadera/hombro al pie; 3 = dos huesos (analitico), mas = FABRIK
    int toe = -1;             // la punta del pie (opcional)
    float weight = 1.0f;
    // Hacia donde se doblan sus articulaciones si la animacion las tiene
    // rectas (modelo): uno por articulacion o vacio. Ver twoBone/chain.
    std::vector<core::Vec3> bend_hints;
    const core::Vec3* pole = nullptr;  // modelo (opcional)
};

struct GroundSettings {
    float weight = 1.0f;
    float max_step = 0.5f;          // cuanto puede subir o bajar un pie (m)
    bool align_feet = true;         // el pie sigue la pendiente
    float max_foot_angle = 40.0f;   // grados
    bool move_body = true;          // la cadera/el cuerpo sube y baja
    bool align_body = false;        // inclinar el cuerpo (3 patas o mas)
    float body_align_weight = 1.0f;
    float max_body_angle = 30.0f;   // grados
};

// Lo que se recuerda entre frames (uno por personaje).
struct GroundState {
    struct Foot {
        bool valid = false;
        float ground = 0.0f;                // altura del suelo bajo el pie (mundo, suavizada)
        core::Vec3 normal{0.0f, 1.0f, 0.0f};  // mundo, suavizada
        bool has_floor = false;
        float floor = 0.0f;                 // la altura mas baja que le da la animacion (sobre la base)
        float lift = 0.0f;                  // altura de la animacion sobre la base (este frame)
        float plant = 0.0f;                 // 0..1: apoyado segun la animacion
        float offset = 0.0f;                // cuanto se movio el pie en vertical (mundo, este frame)
        bool has_last = false;
        core::Vec3 last{};                  // tobillo de la animacion el frame anterior (mundo)
    };
    std::vector<Foot> feet;
    bool has_base = false;
    core::Vec3 base{};             // origen del modelo el frame anterior (mundo)
    float base_velocity = 0.0f;    // m/s en vertical sin los escalones de golpe
    int step_frames = 0;           // frames seguidos con saltos (3 = se mueve, no es un escalon)
    float body = 0.0f;             // la cadera/el cuerpo respecto a la animacion (m, sin el peso)
    float body_speed = 0.0f;       // su velocidad (sube con un muelle critico)
    float rising = 0.0f;           // segundos seguidos subiendo deprisa (saltar)
    float falling = 0.0f;          // segundos seguidos cayendo deprisa
    float active = 1.0f;           // 0 en el aire, 1 en el suelo (fundido)
    float body_offset = 0.0f;      // lo que se movio el cuerpo (mundo, este frame)
};

// ¿Que sabe la fisica del personaje? (Character Controller.)
enum class Support : int { Unknown = -1, Air = 0, Ground = 1 };

// Apoya `legs` en el suelo. `world`: modelo -> mundo; `body`: el nodo que
// sube, baja y se inclina (la cadera del humanoide o el ancestro comun de
// las patas; -1 = ninguno); `forward`: delante del cuerpo (modelo).
void groundLegs(const Pose& pose, const core::Mat4& world, int body, const std::vector<GroundLeg>& legs,
                const core::Vec3& forward, const GroundSettings& settings, const GroundRay& ground, GroundState& state,
                float delta_seconds, Support support = Support::Unknown);

}  // namespace cramion::ik

#endif  // CRAMION_CORE_ANIM_FOOT_PLACEMENT_H
