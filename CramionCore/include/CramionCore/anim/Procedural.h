#ifndef CRAMION_CORE_ANIM_PROCEDURAL_H
#define CRAMION_CORE_ANIM_PROCEDURAL_H

// Animacion procedural: movimiento calculado en el momento, encima de la
// animacion (o sin ella), sobre la pose del esqueleto (ik::Pose):
//
//   - Huesos con muelle (movimiento secundario, como los spring bones de
//     VRM o el AnimDynamics de Unreal): pelo, colas, capas, antenas. Cada
//     hueso de la cadena es una particula en el mundo que sigue a la pose
//     animada con inercia, gravedad y amortiguacion, sin estirarse, y choca
//     con esferas (el cuerpo). Al mover el personaje se quedan atras y rebotan.
//   - Patas procedurales (arañas, cangrejos, robots, dragones): cada pie se
//     queda clavado en el suelo y da un PASO (en arco) cuando el cuerpo se
//     aleja demasiado; los grupos se turnan (tripode, diagonales) y el cuerpo
//     se sube, se baja y se inclina con el suelo que pisan.
//   - Capas: respirar (pecho), inclinarse al acelerar y girar (columna) y
//     ruido suave en huesos sueltos (antenas, colas en reposo, balanceo).
//
// Todo en el espacio del mundo para la fisica y en el del modelo para girar
// huesos; `world` es la matriz de la entidad (modelo -> mundo).

#include "CramionCore/anim/IK.h"

#include <CramionFX/core/Math.h>

#include <functional>
#include <vector>

namespace cramion::procedural {

// --- Huesos con muelle ------------------------------------------------------------

struct SpringSettings {
    float stiffness = 40.0f;  // vuelta a la pose animada (1/s²): mas = mas rigido
    float damping = 0.2f;     // 0..1 por paso: mas = rebota menos
    float gravity = 4.0f;     // m/s² hacia abajo (0 = sin peso)
    float radius = 0.03f;     // grosor de cada hueso para chocar (m)
};

struct Sphere {
    core::Vec3 center{};  // mundo
    float radius = 0.1f;
};

struct SpringChain {
    std::vector<int> joints;    // nodos: [0] = la raiz (sigue a la animacion)
    std::vector<int> parent;    // indice en `joints` del padre (-1 la raiz)
    std::vector<bool> leads;    // es el primer hijo de su padre (el que lo gira)
    std::vector<core::Vec3> position;  // particulas en el mundo
    std::vector<core::Vec3> previous;
    bool ready = false;
};

// La cadena desde `root` (el nodo y sus descendientes, hasta `max_joints`).
SpringChain makeSpringChain(const std::vector<asset::Node>& nodes, int root, int max_joints = 64);
void updateSprings(const ik::Pose& pose, const core::Mat4& world, SpringChain& chain, const SpringSettings& settings,
                   const std::vector<Sphere>& colliders, float dt);

// --- Patas ------------------------------------------------------------------------

struct LegSettings {
    float step_distance = 0.35f;  // cuanto se aleja el pie de su sitio antes de dar un paso (m)
    float step_height = 0.12f;    // altura del arco (m)
    float step_duration = 0.22f;  // segundos que dura un paso
    float overshoot = 0.5f;       // adelanta el pie segun la velocidad (0..1)
    bool adjust_body = true;      // el cuerpo sube, baja y se inclina con los pies
    float body_weight = 1.0f;
};

struct Leg {
    int end = -1, mid = -1, upper = -1;  // pie, rodilla, cadera (nodos)
    int group = 0;                        // se turnan por grupos
    core::Vec3 planted{};                 // donde esta el pie (mundo)
    core::Vec3 from{}, to{};
    float t = 0.0f;
    bool stepping = false;
    bool ready = false;
};

// Suelo bajo un punto: rayo en el mundo -> punto y normal.
using GroundQuery = std::function<bool(const core::Vec3& origin, const core::Vec3& direction, float max_distance,
                                       core::Vec3& point, core::Vec3& normal)>;

// `body` = nodo que se sube/inclina (el ancestro comun de las patas).
// `velocity` = del personaje en el mundo (m/s). Devuelve cuantos pasos empezaron.
int updateLegs(const ik::Pose& pose, const core::Mat4& world, std::vector<Leg>& legs, int body,
               const LegSettings& settings, const GroundQuery& ground, const core::Vec3& velocity, float dt);

// Ancestro comun mas bajo de varios nodos (-1 si no hay).
int commonAncestor(const std::vector<asset::Node>& nodes, const std::vector<int>& of);

// --- Capas -------------------------------------------------------------------------

// Gira `bone` alrededor de `axis` (modelo) `degrees`.
void rotateBone(const ik::Pose& pose, int bone, const core::Vec3& axis, float degrees);

// Ruido suave (-1..1) de periodo ~1/frequency, distinto por `seed`.
float smoothNoise(float time, float frequency, float seed);

}  // namespace cramion::procedural

#endif  // CRAMION_CORE_ANIM_PROCEDURAL_H
