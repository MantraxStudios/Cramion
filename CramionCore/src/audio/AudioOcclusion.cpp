// Oclusion del audio con la fisica: cuantos cuerpos solidos cortan la linea
// entre el oyente y el sonido.

#include "CramionCore/audio/Audio.h"
#include "CramionCore/physics/PhysicsSystem.h"

#include <cmath>
#include <unordered_set>

namespace cramion::audio {

namespace {

// `e` es `other`, o su padre, abuelo... o su hijo, nieto...
bool related(ecs::Entity e, ecs::Entity other) {
    if (!e.valid() || !other.valid()) return false;
    return e == other || e.isAncestorOf(other) || other.isAncestorOf(e);
}

}  // namespace

AudioSystem::OcclusionQuery physicsOcclusionQuery(const physics::PhysicsSystem& physics) {
    return [&physics](const core::Vec3& from, const core::Vec3& to, ecs::Entity source, ecs::Entity listener) -> float {
        const core::Vec3 delta = to - from;
        const float distance = core::length(delta);
        if (distance < 0.05f) return 0.0f;
        physics::QueryFilter filter;
        filter.triggers = physics::QueryTriggers::Ignore;  // los triggers no tapan
        filter.record = false;                             // sin gizmos de consulta
        const std::vector<physics::RaycastHit> hits = physics.raycastAll(from, delta * (1.0f / distance), distance, filter);
        std::unordered_set<std::uint32_t> walls;
        for (const physics::RaycastHit& hit : hits) {
            if (!hit.entity.valid() || hit.trigger) continue;
            // El propio sonido (la radio y su carcasa) y el oyente (el cuerpo del jugador).
            if (related(hit.entity, source) || related(hit.entity, listener)) continue;
            // Casi pegado al sonido: es su soporte, no una pared en medio.
            if (distance - hit.distance < 0.05f) continue;
            walls.insert(static_cast<std::uint32_t>(hit.entity.handle()));
        }
        return static_cast<float>(walls.size());
    };
}

}  // namespace cramion::audio
