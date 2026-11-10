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
    // Cada eje por separado. Antes, al pasar el umbral en un eje se
    // redondeaban los tres: alejandose en horizontal con la camara a mas de
    // step/2 (512 m) de altura, el origen quedaba en y = 1024 (o mas) y la y
    // local dejaba de ser la altura real. La niebla de altura, las nubes, el
    // agua y la lluvia la toman como altura (base de la niebla en y = 0): la
    // camara "bajo tierra" llenaba la pantalla de niebla (todo gris) hasta
    // otro desplazamiento con la camara baja (volver al centro).
    //   - Un eje se desplaza solo si se paso del umbral en ese eje.
    //   - Un eje con origen distinto de 0 vuelve a 0 cuando su coordenada real
    //     esta a menos de threshold/2 del 0 (con ese margen no oscila en el
    //     borde): cerca del centro el origen es siempre exactamente 0, y una
    //     escena guardada con el origen alto se arregla sola.
    // La altura casi nunca pasa del umbral (voxeles: 256 m), asi que en la
    // practica el origen se queda en y = 0.
    const DVec3& origin = world.origin();
    const auto axis = [&](float local, double current) -> float {
        if (std::abs(local) > settings.threshold) return std::round(local / settings.step) * settings.step;
        if (current != 0.0 && std::abs(current + static_cast<double>(local)) <= 0.5 * settings.threshold) {
            return static_cast<float>(-current);  // de vuelta al 0 (multiplo de step: exacto)
        }
        return 0.0f;
    };
    const core::Vec3 offset{axis(focus.x, origin.x), axis(focus.y, origin.y), axis(focus.z, origin.z)};
    if (offset.x == 0.0f && offset.y == 0.0f && offset.z == 0.0f) return std::nullopt;
    world.shiftOrigin(offset);
    return offset;
}

}  // namespace cramion::ecs

#endif  // CRAMION_CORE_ECS_FLOATING_ORIGIN_H
