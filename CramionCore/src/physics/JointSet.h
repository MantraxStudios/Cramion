#ifndef CRAMION_CORE_PHYSICS_JOINT_SET_H
#define CRAMION_CORE_PHYSICS_JOINT_SET_H

// Las juntas en marcha (Joints.h) con las constraints de Jolt. Interno de la
// fisica: lo usa PhysicsSystem (sync antes de los pasos, afterStep despues de
// cada uno, removeBody antes de destruir un cuerpo y clear al parar).
//
// Cada junta guarda su marco (punto y ejes) local a cada uno de los dos
// cuerpos, fijado la primera vez que se crea: si hay que rehacer la
// constraint (otro cuerpo, otros ajustes) conserva la pose de partida.

#include "CramionCore/physics/Joints.h"

#include <Jolt/Jolt.h>

#include <Jolt/Physics/Body/BodyID.h>
#include <Jolt/Physics/Constraints/TwoBodyConstraint.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <unordered_map>
#include <utility>
#include <vector>

namespace JPH {
class PhysicsSystem;
}

namespace cramion::physics {

class JointSet {
public:
    // Cuerpo solido de una entidad (invalido si no tiene).
    using BodyOf = std::function<JPH::BodyID(entt::entity)>;

    // Crea, rehace, actualiza o quita las constraints segun los componentes.
    void sync(ecs::World& world, JPH::PhysicsSystem& system, const BodyOf& body_of);
    // Tras un paso: fuerzas de cada junta y roturas (takeBreaks).
    void afterStep(ecs::World* world, JPH::PhysicsSystem& system, float dt);
    // Antes de destruir un cuerpo: sus constraints fuera (se rehacen en el
    // siguiente sync si el cuerpo vuelve; el marco se conserva).
    void removeBody(JPH::PhysicsSystem& system, const JPH::BodyID& id);
    // Origen flotante: las juntas unidas al mundo (sin cuerpo conectado)
    // guardan su punto del mundo; se mueve y su constraint se rehace en el
    // siguiente sync (antes del paso). Las que unen dos cuerpos no cambian.
    void shiftOrigin(JPH::PhysicsSystem& system, JPH::Vec3Arg offset);
    // Todo fuera (stop): constraints, marcos y roturas.
    void clear(JPH::PhysicsSystem* system);

    // Dos cuerpos unidos por una junta sin enable_collision no chocan (datos
    // de usuario de Jolt = la entidad). Se lee desde los hilos de Jolt; solo
    // cambia en sync/clear (fuera del paso).
    bool collisionDisabled(std::uint64_t a, std::uint64_t b) const {
        return !pairs_.empty() && pairs_.contains(pairKey(a, b));
    }

    std::vector<JointBreakEvent> takeBreaks();
    // Rompe ya una junta (type < 0: todas las de la entidad). true si rompio alguna.
    bool breakJoint(ecs::World* world, JPH::PhysicsSystem& system, entt::entity entity, int type, bool notify);
    // Una junta rota vuelve a unir (en el siguiente sync, con su marco).
    bool repair(entt::entity entity, int type);
    JointState state(const ecs::World& world, entt::entity entity, JointType type, const JPH::PhysicsSystem* system) const;
    std::vector<JointDebug> debug(ecs::World& world, const JPH::PhysicsSystem* system) const;
    std::size_t activeCount() const;

    static std::uint64_t pairKey(std::uint64_t a, std::uint64_t b) {
        const std::uint64_t low = std::min(a, b) & 0xFFFFFFFFull;
        const std::uint64_t high = std::max(a, b) & 0xFFFFFFFFull;
        return (high << 32) | low;
    }

private:
    struct Frame {
        JPH::Vec3 point = JPH::Vec3::sZero();   // local al cuerpo (o del mundo)
        JPH::Vec3 axis_x = JPH::Vec3::sAxisX();
        JPH::Vec3 axis_y = JPH::Vec3::sAxisY();
    };
    struct Entry {
        JPH::Ref<JPH::TwoBodyConstraint> constraint;
        JPH::BodyID body1;  // el conectado (invalido = el mundo)
        JPH::BodyID body2;  // el de la entidad
        entt::entity connected = entt::null;
        bool framed = false;
        std::uint64_t frame_sig = 0;
        Frame frame1, frame2;
        float rest_distance = 0.0f;  // Distance/Spring: la de partida
        float break_force = 0.0f;    // copia de los del componente (afterStep no lo lee)
        float break_torque = 0.0f;
        std::uint64_t build_sig = 0;
        std::uint64_t live_sig = 0;
        bool broken = false;
        float force = 0.0f;
        float torque = 0.0f;
        std::uint64_t pair = 0;  // pareja sin colision registrada (0 = ninguna)
        std::uint64_t seen = 0;
    };
    using Key = std::pair<entt::entity, int>;

    void removeConstraint(JPH::PhysicsSystem& system, Entry& entry);
    void setPair(Entry& entry, std::uint64_t pair, JPH::PhysicsSystem& system);

    std::map<Key, Entry> joints_;
    std::unordered_map<std::uint64_t, int> pairs_;  // pareja -> cuantas juntas la piden
    std::vector<JointBreakEvent> breaks_;
    std::uint64_t generation_ = 0;
};

}  // namespace cramion::physics

#endif  // CRAMION_CORE_PHYSICS_JOINT_SET_H
