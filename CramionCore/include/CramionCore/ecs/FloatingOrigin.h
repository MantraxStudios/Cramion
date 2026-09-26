#ifndef CRAMION_CORE_ECS_FLOATING_ORIGIN_H
#define CRAMION_CORE_ECS_FLOATING_ORIGIN_H

// Origen flotante (el "World Origin Rebasing" de Unreal): cuando lo que se
// mira (la camara) se aleja mas de `threshold` metros del (0,0,0), el mundo
// entero se desplaza para que vuelva a estar cerca. Asi los float de 32 bits
// siempre trabajan con numeros pequenos: nada tiembla aunque el jugador este
// a 100 km (mallas, luz y sombras, fisica, gizmos).
//
// El desplazamiento va en pasos de `step` metros (un numero redondo, exacto
// en float y multiplo de los bloques y de la rejilla del editor: el cielo,
// la rejilla y los bloques no saltan).
//
// Uso (una vez por frame, con la camara):
//   if (auto offset = updateFloatingOrigin(world, camera_position)) {
//       physics.shiftOrigin(*offset); particles.shiftOrigin(*offset); ...
//   }
// World::shiftOrigin ya ha movido las entidades; cada sistema que guarda
// posiciones del mundo mueve las suyas.

#include "CramionCore/ecs/World.h"

#include <algorithm>
#include <cmath>
#include <optional>

namespace cramion::ecs {

struct FloatingOriginSettings {
    bool enabled = true;
    float threshold = 2048.0f;  // metros desde el origen antes de desplazar
    float step = 1024.0f;       // el desplazamiento se redondea a esto
};

// Desplaza el mundo si `focus` esta lejos del origen. Devuelve el
// desplazamiento aplicado (para los demas sistemas) o nada.
inline std::optional<core::Vec3> updateFloatingOrigin(World& world, const core::Vec3& focus,
                                                      const FloatingOriginSettings& settings = {}) {
    if (!settings.enabled) return std::nullopt;
    if (!(std::isfinite(focus.x) && std::isfinite(focus.y) && std::isfinite(focus.z))) return std::nullopt;
    const float distance = std::max({std::abs(focus.x), std::abs(focus.y), std::abs(focus.z)});
    if (distance <= settings.threshold) return std::nullopt;
    const auto snap = [&](float v) { return std::round(v / settings.step) * settings.step; };
    const core::Vec3 offset{snap(focus.x), snap(focus.y), snap(focus.z)};
    if (offset.x == 0.0f && offset.y == 0.0f && offset.z == 0.0f) return std::nullopt;
    world.shiftOrigin(offset);
    return offset;
}

}  // namespace cramion::ecs

#endif  // CRAMION_CORE_ECS_FLOATING_ORIGIN_H
