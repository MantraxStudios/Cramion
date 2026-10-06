#ifndef CRAMION_CORE_ENVIRONMENT_FOG_VOLUME_H
#define CRAMION_CORE_ENVIRONMENT_FOG_VOLUME_H

// Volumen de niebla local (como el Local Volumetric Fog de Unity / los Fog
// Volumes de Unreal): una caja (o esfera) de niebla con su densidad y color,
// borde suave y ruido que deriva con el viento. Recibe la luz del sol, del
// cielo y de las luces locales con sus sombras (va dentro de la luz
// volumetrica: necesita "Luz volumetrica" encendida en el post-proceso). La
// caja mide 1 m y se escala con el Transform. Hasta 16 a la vez (los mas
// cercanos a la camara).

#include "CramionCore/ecs/Reflection.h"

#include <CramionFX/core/Math.h>

namespace cramion::ecs {
class World;
}

namespace cramion::gfx {
class VulkanRenderer;
}

namespace cramion::environment {

struct FogVolume {
    bool enabled = true;
    int shape = 0;                          // 0 caja, 1 esfera
    core::Vec3 color{0.85f, 0.88f, 0.92f};  // albedo (sRGB)
    float density = 0.08f;                  // 1/m
    float edge_falloff = 0.35f;             // 0 = borde duro, 1 = todo degradado
    float noise = 0.5f;                     // 0 = uniforme
    float noise_scale = 0.25f;              // 1/m (mas = grumos mas pequenos)

    void reflect(ecs::PropertyVisitor& v);
};

void registerFogVolumeComponents();
// Manda al render los volumenes activos (los 16 mas cercanos a `eye`).
void syncFogVolumes(ecs::World& world, gfx::VulkanRenderer& renderer, const core::Vec3& eye);

}  // namespace cramion::environment

#endif  // CRAMION_CORE_ENVIRONMENT_FOG_VOLUME_H
