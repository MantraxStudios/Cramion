#ifndef CRAMION_VK_PARTICLE_GEOMETRY_H
#define CRAMION_VK_PARTICLE_GEOMETRY_H

// CONTRATO COMPARTIDO (renderizador <-> simulacion de particulas): puntos
// que el renderizador dibuja como discos suaves de cara a la camara, sobre la
// imagen HDR de la escena (despues del vidrio y antes del bloom: las
// brillantes brillan), con prueba de profundidad contra la escena y sin
// escribirla. Coste cero si esta vacia.

#include "CramionFX/core/Math.h"

#include <vector>

namespace cramion::gfx {

struct ParticleInstance {
    core::Vec3 position{};  // en el mundo
    float size = 0.1f;      // diametro en metros
    core::Vec4 color{1.0f, 1.0f, 1.0f, 1.0f};  // rgb lineal (puede pasar de 1), a = opacidad
};

struct ParticleDrawList {
    // Mezcla alfa: ordenadas de atras adelante (lo hace quien las da).
    std::vector<ParticleInstance> alpha;
    // Aditivas (fuego, chispas): el orden no importa.
    std::vector<ParticleInstance> additive;

    bool empty() const { return alpha.empty() && additive.empty(); }
    std::size_t size() const { return alpha.size() + additive.size(); }
};

}  // namespace cramion::gfx

#endif  // CRAMION_VK_PARTICLE_GEOMETRY_H
