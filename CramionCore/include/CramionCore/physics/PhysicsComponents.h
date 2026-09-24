#ifndef CRAMION_CORE_PHYSICS_COMPONENTS_H
#define CRAMION_CORE_PHYSICS_COMPONENTS_H

// Componentes de fisica (Jolt Physics), como los de Unity:
//
//   Rigidbody        cuerpo que simula la fisica (dinamico), que se mueve
//                    por su Transform empujando a los demas (cinematico) o
//                    fijo (estatico)
//   BoxCollider, SphereCollider, CapsuleCollider, MeshCollider, PlaneCollider
//                    la forma del cuerpo. Un collider SIN Rigidbody es un
//                    cuerpo estatico (suelos, paredes).
//   ParticleSystem   emisor de particulas que chocan con los colliders
//                    (Particles.h)
//
// Cada collider tiene su material (friccion y rebote) y puede ser un trigger
// ("Es trigger"): detecta a los que entran y salen pero no choca. En una
// misma entidad se pueden mezclar colliders solidos y triggers: los solidos
// forman un cuerpo y los triggers un sensor que lo acompana.
//
// La capa de la entidad (EntityInfo::layer, en la cabecera del Inspector)
// decide con quien choca segun la matriz de PhysicsSettings.
//
// Varios colliders en la misma entidad se combinan en una forma compuesta.
// La escala del Transform (en el mundo) se aplica a las formas.
//
// Se registran con registerPhysicsComponents() (lo llama PhysicsSystem al
// crearse; el editor lo llama antes de abrir escenas para que aparezcan en
// "Add Component" y se lean de los .crscene).

#include "CramionCore/ecs/Reflection.h"

#include <CramionFX/core/Math.h>

#include <cstdint>

namespace cramion::ecs {
class Entity;
}

namespace cramion::physics {

enum class BodyType : int {
    Static = 0,
    Dynamic = 1,
    Kinematic = 2,
};

struct Rigidbody {
    BodyType type = BodyType::Dynamic;
    float mass = 1.0f;  // kg (solo dinamicos)
    float linear_damping = 0.05f;
    float angular_damping = 0.05f;
    bool use_gravity = true;
    float gravity_scale = 1.0f;
    bool lock_position_x = false;  // congela el movimiento en ese eje (del mundo)
    bool lock_position_y = false;
    bool lock_position_z = false;
    bool lock_rotation_x = false;  // congela el giro en ese eje (del mundo)
    bool lock_rotation_y = false;
    bool lock_rotation_z = false;
    core::Vec3 initial_velocity{};          // m/s al empezar
    core::Vec3 initial_angular_velocity{};  // rad/s
    bool continuous = false;   // CCD: objetos rapidos que no atraviesen paredes
    bool allow_sleep = true;   // se duerme quieto (ahorra CPU; no da eventos Stay)
    // Anular capas (Layer Overrides de Unity), para todo el cuerpo: incluir =
    // chocar con esas capas aunque la matriz diga que no; excluir = no chocar
    // aunque diga que si. Excluir gana.
    std::uint32_t include_layers = 0;
    std::uint32_t exclude_layers = 0;
    bool interpolate = true;   // Transform suave entre pasos de fisica (a cualquier FPS)

    void reflect(ecs::PropertyVisitor& v);
};

// Lo comun a todos los colliders (el Collider base de Unity + su material).
struct ColliderMaterial {
    bool is_trigger = false;   // sensor: detecta pero no choca
    float friction = 0.6f;     // 0 = hielo, 1 = goma
    float bounciness = 0.0f;   // 0 = no rebota, 1 = rebote perfecto
    // Anular capas de este collider (ver Rigidbody::include_layers).
    std::uint32_t include_layers = 0;
    std::uint32_t exclude_layers = 0;
};

struct BoxCollider {
    ColliderMaterial material{};
    core::Vec3 size{1.0f, 1.0f, 1.0f};  // medidas completas (no la mitad)
    core::Vec3 center{};

    void reflect(ecs::PropertyVisitor& v);
};

struct SphereCollider {
    ColliderMaterial material{};
    float radius = 0.5f;
    core::Vec3 center{};

    void reflect(ecs::PropertyVisitor& v);
};

enum class CapsuleAxis : int { X = 0, Y = 1, Z = 2 };

struct CapsuleCollider {
    ColliderMaterial material{};
    float radius = 0.5f;
    float height = 2.0f;  // altura total, con las semiesferas (como Unity)
    CapsuleAxis axis = CapsuleAxis::Y;
    core::Vec3 center{};

    void reflect(ecs::PropertyVisitor& v);
};

// La geometria de la pieza del MeshRenderer de la misma entidad. Malla de
// triangulos: solo estatica (o cinematica); convexa: vale para dinamicos (se
// calcula la envolvente convexa de los vertices).
struct MeshCollider {
    ColliderMaterial material{};
    bool convex = false;

    void reflect(ecs::PropertyVisitor& v);
};

// Plano infinito por el origen de la entidad con su +Y como normal (el suelo
// de una escena). Siempre estatico.
struct PlaneCollider {
    ColliderMaterial material{};

    void reflect(ecs::PropertyVisitor& v);
};

// Registra los componentes de fisica y de particulas en ComponentRegistry
// (idempotente).
void registerPhysicsComponents();

// Collider por defecto para una primitiva integrada (cubo -> caja, esfera ->
// esfera, capsula y cilindro -> capsula, plano -> MeshCollider), como hace
// Unity al crear un objeto 3D. No hace nada si ya tiene collider.
void addDefaultCollider(ecs::Entity entity);

// true si la entidad tiene algun collider.
bool hasCollider(const ecs::Entity& entity);

}  // namespace cramion::physics

#endif  // CRAMION_CORE_PHYSICS_COMPONENTS_H
