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
// Vehiculo (como el Wheel Collider de Unity, con el vehiculo de Jolt): en el
// objeto con Rigidbody dinamico; sus ruedas son los WheelCollider de los
// hijos. Motor, cambio automatico y diferenciales entre las ruedas motrices.
struct Vehicle {
    float engine_torque = 500.0f;  // Nm
    float min_rpm = 1000.0f;
    float max_rpm = 6000.0f;
    bool automatic = true;         // cambio automatico
    float max_pitch_roll = 60.0f;  // grados antes de dejar de volcar (180 = libre)
    bool keyboard = true;          // W/S acelerar/atras, A/D girar, Espacio freno de mano
    void reflect(ecs::PropertyVisitor& v);
};

// Una rueda (su posicion es la del objeto): suspension por raycast, direccion,
// traccion y frenos. `visual`: el objeto que se mueve y gira con la rueda (su
// eje de giro es su X local).
struct WheelCollider {
    float radius = 0.38f;
    float width = 0.25f;
    float suspension_min = 0.05f;  // m desde el anclaje (subida maxima)
    float suspension_max = 0.35f;  // m (bajada maxima)
    float spring_frequency = 1.6f;  // Hz (mas = mas dura)
    float damping = 0.5f;           // 0..1
    float max_steer_angle = 0.0f;   // grados (0 = no gira)
    bool drive = true;              // recibe el par del motor
    float max_brake_torque = 1500.0f;
    float max_handbrake_torque = 0.0f;
    float grip = 1.0f;              // friccion del neumatico
    Uuid visual{};
    void reflect(ecs::PropertyVisitor& v);
};

// Character Controller (el de Unity + el CharacterMovement de Unreal, sobre el
// CharacterVirtual de Jolt): una capsula vertical que se mueve chocando y
// deslizando por paredes, sube escalones, no sube rampas mas empinadas que su
// limite, se pega al suelo al bajar, sigue a las plataformas que se mueven y
// empuja a los Rigidbody. No la mueve la fisica: la mueve su movimiento.
//
//   Manual      como Unity: solo se mueve con move(desplazamiento) desde un
//               script (sin gravedad propia) o con la velocidad que se le pone.
//   Integrado   como Unreal: gravedad, salto, andar/correr/agacharse con
//               aceleracion y control en el aire. Lo dirige el teclado
//               (WASD/flechas, Shift correr, Espacio saltar, C/Ctrl agacharse,
//               relativo a la camara principal) o un script (setMoveInput).
//
// La entidad es el pie de la capsula si `center` es (0, altura/2, 0). Es
// collider: la tocan los rayos, entra en los triggers y da eventos Collision
// con lo que toca. Los otros colliders de la misma entidad se ignoran.
enum class CharacterMovement : int { Manual = 0, Integrated = 1 };

struct CharacterController {
    // Forma
    float height = 2.0f;                 // total, con las semiesferas
    float radius = 0.4f;
    core::Vec3 center{0.0f, 1.0f, 0.0f};  // centro de la capsula respecto a la entidad
    float slope_limit = 45.0f;           // grados: mas empinado = pared
    float step_offset = 0.35f;           // altura de escalon que sube sola
    float skin_width = 0.02f;            // distancia que guarda a lo que toca
    bool stick_to_floor = true;          // al bajar rampas/escalones no sale volando
    // Empujar
    float mass = 70.0f;                  // kg (empuje a los Rigidbody)
    float push_strength = 300.0f;        // N maximos con que empuja
    bool push_rigidbodies = true;
    // Movimiento integrado
    CharacterMovement movement = CharacterMovement::Integrated;
    bool keyboard = true;                // WASD / Shift / Espacio / C (si no: por script)
    float walk_speed = 4.0f;             // m/s
    float run_speed = 7.5f;
    float crouch_speed = 2.0f;
    float acceleration = 40.0f;          // m/s2 en el suelo (0 = al instante)
    float air_control = 0.35f;           // 0..1 de la aceleracion en el aire
    float jump_height = 1.2f;            // m
    int max_jumps = 1;                   // 2 = doble salto
    float coyote_time = 0.12f;           // s tras salir del borde en que aun puede saltar
    float gravity_scale = 1.0f;
    float crouch_height = 1.2f;
    bool rotate_to_movement = true;      // gira la entidad hacia donde anda
    float rotation_speed = 720.0f;       // grados/s

    void reflect(ecs::PropertyVisitor& v);
};

void registerPhysicsComponents();

// Collider por defecto para una primitiva integrada (cubo -> caja, esfera ->
// esfera, capsula y cilindro -> capsula, plano -> MeshCollider), como hace
// Unity al crear un objeto 3D. No hace nada si ya tiene collider.
void addDefaultCollider(ecs::Entity entity);

// true si la entidad tiene algun collider (el CharacterController cuenta).
bool hasCollider(const ecs::Entity& entity);

}  // namespace cramion::physics

#endif  // CRAMION_CORE_PHYSICS_COMPONENTS_H
