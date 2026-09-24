#ifndef CRAMION_SCENE_SHADOW_CASCADES_H
#define CRAMION_SCENE_SHADOW_CASCADES_H

#include "CramionFX/core/Math.h"
#include "CramionFX/scene/Camera.h"
#include "CramionFX/scene/Light.h"

#include <array>
#include <cstdint>

namespace cramion::scene {

// Numero de cascadas. Debe coincidir con kShadowCascadeCount de
// shaders/lighting.frag.
inline constexpr std::uint32_t kShadowCascadeCount = 4;

// Datos de una cascada, listos para enviarse a la GPU.
struct ShadowCascade {
    core::Mat4 light_view_projection = core::Mat4::identity();

    // Distancia de vista (en unidades del mundo) donde termina esta cascada.
    float split_distance = 0.0f;

    // Cuanto mide un texel del mapa de sombras en el mundo. El shader lo usa
    // para desplazar la posicion a lo largo de la normal y evitar el acne.
    float texel_world_size = 0.0f;
};

// Cascaded Shadow Maps para la luz direccional, al estilo del que usa Unreal.
//
// Ideas clave:
//
//   - El frustum de la camara se parte en varias secciones (cascadas). Las
//     cercanas cubren poco terreno con el mismo numero de texeles, asi que las
//     sombras de cerca salen nitidas y las de lejos baratas.
//
//   - El reparto de los cortes mezcla una progresion logaritmica con una
//     uniforme; `distribution` es el mismo mando que en Unreal se llama
//     "Cascade Distribution Exponent" (1 = logaritmico, 0 = uniforme).
//
//     Ojo con el plano cercano: la parte logaritmica usa near como base, y con
//     un near de 0.1 las primeras cascadas salen diminutas y toda la escena
//     acaba cayendo en la ultima, que es la mas basta. Por eso la formula usa
//     un near efectivo (kDistributionNearFraction del alcance) en vez del de la
//     camara; la primera cascada sigue empezando en el near real.
//
//   - Cada cascada se encierra en una ESFERA, no en una caja. El radio de una
//     esfera no cambia al girar la camara, asi que el area cubierta es
//     constante y las sombras no hierven ("shadow shimmering").
//
//   - La proyeccion se ajusta ademas a la rejilla de texeles del mapa, para que
//     al moverse la camara las sombras no vibren texel a texel.
class ShadowCascades {
public:
    // Recalcula las cascadas para la camara y el sol de este frame.
    // `shadow_map_size` es la resolucion (en texeles) de cada cascada.
    void update(const Camera& camera, const DirectionalLight& sun,
                std::uint32_t shadow_map_size);

    const std::array<ShadowCascade, kShadowCascadeCount>& cascades() const { return cascades_; }
    const ShadowCascade& cascade(std::uint32_t index) const { return cascades_[index]; }

    // Distancia maxima a la que se proyectan sombras. Mas alla solo hay luz
    // directa sin sombrear, igual que la "Dynamic Shadow Distance" de Unreal.
    float shadowDistance() const { return shadow_distance_; }
    void setShadowDistance(float distance);

    float distribution() const { return distribution_; }
    void setDistribution(float distribution);

private:
    std::array<ShadowCascade, kShadowCascadeCount> cascades_{};

    // Base del reparto logaritmico, como fraccion del alcance de sombras.
    static constexpr float kDistributionNearFraction = 0.015f;

    float shadow_distance_ = 100.0f;
    float distribution_ = 0.75f;

    // Margen que se anade por detras de la cascada para que los objetos altos
    // situados fuera de ella sigan proyectando sombra dentro.
    float caster_margin_ = 60.0f;
};

}  // namespace cramion::scene

#endif  // CRAMION_SCENE_SHADOW_CASCADES_H
