#ifndef CRAMION_CORE_ANIM_PHYS_BONES_H
#define CRAMION_CORE_ANIM_PHYS_BONES_H

// Phys Bones (como los PhysBone de VRChat o el Dynamic Bone de Unity):
// huesos que se mueven solos con inercia, gravedad y choques — pelo, colas,
// orejas, capas, faldas, antenas, cadenas, colgantes.
//
// Cada hueso de la cadena es una particula en el mundo (Verlet):
//   - pull:      cuanto vuelve a la pose animada (muelle)
//   - spring:    cuanto conserva su velocidad (rebote, "momentum")
//   - stiffness: cuanto se queda en la direccion animada (rigidez directa)
//   - gravity:   peso (0..1 de la gravedad) y gravity_falloff: menos peso
//                cuando ya cuelga hacia abajo en la animacion
//   - immobile:  cuanto del movimiento del personaje se lleva sin inercia
//                (1 = rigido al andar, 0 = se queda atras)
//   - max_angle: cuanto puede separarse de la direccion animada (0 = libre)
//   - radius (y radius_tip en la punta): grosor para chocar con los colliders
//   - end_length: una punta extra (fraccion del ultimo hueso) para que el
//     ultimo hueso tambien se mueva
// Nunca se estiran. La raiz sigue a la animacion.

#include "CramionCore/anim/IK.h"

#include <CramionFX/core/Math.h>

#include <string>
#include <vector>

namespace cramion::physbone {

struct Settings {
    float pull = 0.2f;
    float spring = 0.2f;
    float stiffness = 0.2f;
    float gravity = 0.0f;
    float gravity_falloff = 0.0f;
    float immobile = 0.0f;
    float max_angle = 0.0f;   // grados
    float radius = 0.02f;     // metros
    float radius_tip = -1.0f; // < 0 = igual que radius
    float end_length = 0.0f;
    bool collide = true;
};

struct Collider {
    enum class Shape { Sphere, Capsule, Plane } shape = Shape::Sphere;
    core::Vec3 a{};                    // centro (esfera), extremo (capsula), punto (plano)
    core::Vec3 b{};                    // otro extremo (capsula)
    core::Vec3 normal{0.0f, 1.0f, 0.0f};  // plano
    float radius = 0.1f;
    bool inside = false;               // esfera/capsula: mantener dentro (faldas en una jaula)
};

struct Chain {
    std::vector<int> nodes;          // nodo de cada particula (-1 = punta extra)
    std::vector<int> parent;         // indice del padre (-1 = raiz)
    std::vector<bool> leads;         // el primer hijo de su padre (el que lo gira)
    std::vector<float> depth;        // 0 en la raiz, 1 en la punta mas lejana
    std::vector<core::Vec3> position;
    std::vector<core::Vec3> previous;
    core::Vec3 last_root{};
    bool ready = false;
};

// La cadena desde `root` y sus descendientes, sin los de `ignore` (y sus
// hijos). Con `end_length` > 0 cada punta lleva una particula extra.
Chain makeChain(const std::vector<asset::Node>& nodes, int root, const std::vector<int>& ignore, bool tips,
                int max_joints = 128);

// Simula y gira los huesos. `world`: modelo -> mundo (la entidad).
void update(const ik::Pose& pose, const core::Mat4& world, Chain& chain, const Settings& settings,
            const std::vector<Collider>& colliders, float dt);

// Donde queda cada particula (mundo), para dibujarla.
inline const std::vector<core::Vec3>& particles(const Chain& chain) { return chain.position; }

}  // namespace cramion::physbone

#endif  // CRAMION_CORE_ANIM_PHYS_BONES_H
