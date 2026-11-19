#ifndef CRAMION_CORE_PHYSICS_JOINTS_H
#define CRAMION_CORE_PHYSICS_JOINTS_H

// Juntas de fisica (los Joints de Unity, las Physics Constraints de Unreal),
// con las constraints de Jolt:
//
//   FixedJoint         une dos cuerpos como si fueran uno (se puede romper)
//   HingeJoint         bisagra: gira en un eje (puertas, ruedas, palancas),
//                      con limites, muelle hacia un angulo y motor
//   SliderJoint        corredera: se mueve en un eje (piston, cajon,
//                      ascensor), con limites, muelle y motor
//   DistanceJoint      mantiene dos puntos a una distancia (cuerda, cadena),
//                      con margen y muelle opcional
//   SpringJoint        un DistanceJoint con muelle (el Spring Joint de Unity)
//   ConfigurableJoint  6 grados de libertad: cada eje bloqueado, limitado o
//                      libre, y empuje hacia una pose (6DOF)
//
// Van en la entidad con el Rigidbody (normalmente dinamico). `connected_body`
// es otra entidad con cuerpo (Rigidbody o colliders); vacio = el mundo. El
// anclaje (`anchor`) y el eje (`axis`) van en el espacio local de la entidad;
// con `auto_connected_anchor` el punto del otro lado es el mismo punto del
// mundo al empezar (si no, `connected_anchor`, local al conectado o del mundo).
// La pose de partida es la de cuando se crea la junta (al empezar el Play o
// al anadirla): los angulos y posiciones de los limites cuentan desde ahi.
//
// Rotura: con `break_force` (N) o `break_torque` (N m) mayor que 0, si la
// junta aguanta mas que eso en un paso se rompe: deja de unir y los scripts
// de la entidad reciben OnJointBreak {joint, force, torque, connected}. Una
// junta rota sigue rota hasta parar el Play (o PhysicsSystem::repairJoint).
//
// `enable_collision` = false (por defecto, como Unity): los dos cuerpos
// unidos no chocan entre si.
//
// Una entidad puede tener una de cada tipo. Con la fisica sin simular (modo
// edicion) no se crean.

#include "CramionCore/Uuid.h"
#include "CramionCore/ecs/Reflection.h"
#include "CramionCore/ecs/World.h"

#include <CramionFX/core/Math.h>

#include <string>

namespace cramion::physics {

enum class JointType : int { Fixed = 0, Hinge, Slider, Distance, Spring, Configurable, Count };
inline constexpr int kJointTypeCount = static_cast<int>(JointType::Count);

// "FixedJoint", "HingeJoint"...
const char* jointComponentName(JointType type);
// Del nombre del componente o corto ("HingeJoint", "Hinge", "hinge", "6dof") al tipo.
bool jointTypeFromName(const std::string& name, JointType& out);

// Lo comun a todas (el Joint de Unity).
struct JointSettings {
    Uuid connected_body{};                  // con quien; vacio = el mundo
    core::Vec3 anchor{};                     // punto de la junta, local a la entidad
    bool auto_connected_anchor = true;       // el otro lado: el mismo punto del mundo al empezar
    core::Vec3 connected_anchor{};           // local al conectado (o del mundo si no hay)
    core::Vec3 axis{1.0f, 0.0f, 0.0f};       // eje (bisagra, corredera, X de la 6DOF), local
    float break_force = 0.0f;                // N para romperse (0 = no se rompe)
    float break_torque = 0.0f;               // N m para romperse (0 = no se rompe)
    bool enable_collision = false;           // los dos cuerpos unidos chocan entre si
};

struct FixedJoint : JointSettings {
    void reflect(ecs::PropertyVisitor& v);
};

struct HingeJoint : JointSettings {
    bool use_limits = false;
    float min_angle = -90.0f;     // grados (-180..0) desde la pose de partida
    float max_angle = 90.0f;      // grados (0..180)
    bool use_spring = false;      // muelle hacia target_angle
    float spring = 50.0f;         // N m / rad
    float damper = 5.0f;          // N m s / rad
    float target_angle = 0.0f;    // grados
    bool use_motor = false;       // motor (manda sobre el muelle)
    float motor_speed = 90.0f;    // grados/s
    float motor_force = 100.0f;   // N m maximos
    float friction = 0.0f;        // N m de friccion sin motor
    void reflect(ecs::PropertyVisitor& v);
};

struct SliderJoint : JointSettings {
    bool use_limits = false;
    float min_distance = -1.0f;   // m (<= 0) desde la pose de partida
    float max_distance = 1.0f;    // m (>= 0)
    bool use_spring = false;      // muelle hacia target_position
    float spring = 100.0f;        // N/m
    float damper = 10.0f;         // N s/m
    float target_position = 0.0f; // m
    bool use_motor = false;
    float motor_speed = 1.0f;     // m/s
    float motor_force = 1000.0f;  // N maximos
    float friction = 0.0f;        // N
    void reflect(ecs::PropertyVisitor& v);
};

struct DistanceJoint : JointSettings {
    float min_distance = -1.0f;    // m (-1 = la de partida)
    float max_distance = -1.0f;    // m (-1 = la de partida)
    float spring_frequency = 0.0f; // Hz (0 = rigida; mas = mas dura)
    float spring_damping = 0.5f;   // 0..1
    void reflect(ecs::PropertyVisitor& v);
};

// Un DistanceJoint con muelle por defecto (el SpringJoint de Unity). Con el
// anclaje conectado automatico se engancha al origen del conectado.
struct SpringJoint : DistanceJoint {
    SpringJoint() {
        spring_frequency = 2.0f;
        spring_damping = 0.2f;
    }
    void reflect(ecs::PropertyVisitor& v);
};

enum class JointMotion : int { Locked = 0, Limited = 1, Free = 2 };

struct ConfigurableJoint : JointSettings {
    core::Vec3 secondary_axis{0.0f, 1.0f, 0.0f};  // Y de la junta (local; se hace perpendicular a axis)
    JointMotion x_motion = JointMotion::Locked;
    JointMotion y_motion = JointMotion::Locked;
    JointMotion z_motion = JointMotion::Locked;
    JointMotion angular_x_motion = JointMotion::Locked;  // giro en el eje (twist)
    JointMotion angular_y_motion = JointMotion::Locked;  // balanceo (swing)
    JointMotion angular_z_motion = JointMotion::Locked;
    core::Vec3 linear_limit_min{-1.0f, -1.0f, -1.0f};    // m (Limited)
    core::Vec3 linear_limit_max{1.0f, 1.0f, 1.0f};
    core::Vec3 angular_limit_min{-45.0f, -45.0f, -45.0f};  // grados (Limited)
    core::Vec3 angular_limit_max{45.0f, 45.0f, 45.0f};
    float linear_limit_spring = 0.0f;  // Hz: limites lineales blandos (0 = rigidos)
    // Empuje hacia una pose (drives): en los ejes que no estan bloqueados.
    bool position_drive = false;
    core::Vec3 target_position{};      // m, en los ejes de la junta
    bool rotation_drive = false;
    core::Vec3 target_rotation{};      // grados (X, Y, Z de la junta)
    float drive_frequency = 2.0f;      // Hz
    float drive_damping = 1.0f;        // 0..1
    float drive_max_force = 1000.0f;   // N
    float drive_max_torque = 1000.0f;  // N m
    void reflect(ecs::PropertyVisitor& v);
};

// Se rompio una junta (PhysicsSystem::addJointBreakListener; los scripts:
// OnJointBreak).
struct JointBreakEvent {
    ecs::Entity entity;      // la que tiene la junta
    JointType type = JointType::Fixed;
    ecs::Entity connected;   // con quien estaba unida (vacia = el mundo)
    float force = 0.0f;      // N en el paso en que se rompio
    float torque = 0.0f;     // N m
};

// Estado de una junta en Play.
struct JointState {
    bool exists = false;     // la entidad tiene ese componente
    bool active = false;     // une ahora (constraint creada)
    bool broken = false;
    float force = 0.0f;      // N del ultimo paso
    float torque = 0.0f;     // N m del ultimo paso
    float angle = 0.0f;      // Hinge: grados desde la pose de partida
    float position = 0.0f;   // Slider: m desde la pose de partida; Distance: distancia actual
};

// Para dibujar las juntas (gizmos).
struct JointDebug {
    ecs::Entity entity;
    JointType type = JointType::Fixed;
    core::Vec3 anchor{};            // en el mundo
    core::Vec3 connected_anchor{};  // en el mundo
    core::Vec3 axis{1.0f, 0.0f, 0.0f};
    bool active = false;
    bool broken = false;
};

// Registra los componentes (lo llama registerPhysicsComponents).
void registerJointComponents();

}  // namespace cramion::physics

#endif  // CRAMION_CORE_PHYSICS_JOINTS_H
