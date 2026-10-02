#ifndef CRAMION_VK_SPRITE_GEOMETRY_H
#define CRAMION_VK_SPRITE_GEOMETRY_H

// CONTRATO COMPARTIDO (renderizador <-> sistema 2D de CramionCore): sprites y
// tiles como cuadrados con textura en el mundo, dibujados sobre la imagen HDR
// de la escena (despues de la iluminacion y el vidrio, antes de las
// particulas), con prueba de profundidad contra la escena 3D y sin escribirla.
// Los da ya ordenados quien los genera (capa de orden, orden en la capa y
// distancia): se dibujan en ese orden, el primero al fondo. Coste cero si
// esta vacia.

#include "CramionFX/core/Math.h"

#include <cstdint>
#include <filesystem>
#include <vector>

namespace cramion::gfx {

// Una imagen de disco (PNG, JPG, TGA...). `point_filter`: pixel art (sin
// suavizar ni mipmaps borrosos).
struct SpriteTexture {
    std::filesystem::path file;
    bool point_filter = false;
};

struct SpriteQuad {
    // Esquinas en el mundo: abajo-izquierda, abajo-derecha, arriba-derecha,
    // arriba-izquierda. Con sus coordenadas de textura (0..1, v hacia abajo).
    core::Vec3 corners[4]{};
    core::Vec2 uvs[4]{};
    core::Vec4 color{1.0f, 1.0f, 1.0f, 1.0f};  // rgb lineal (tinte), a = opacidad
    std::uint32_t texture = 0;  // indice en SpriteDrawList::textures
    float alpha_cutoff = 0.0f;  // > 0: descarta los pixeles con menos alfa
    bool lit = false;           // le afectan las luces 2D (si no, se ve tal cual)
};

// Luz 2D (como las Light 2D de Unity): circulo en el plano XY del mundo.
struct SpriteLight {
    core::Vec3 position{};
    float radius = 5.0f;
    core::Vec3 color{1.0f, 1.0f, 1.0f};  // lineal
    float intensity = 1.0f;
    float falloff = 1.0f;  // exponente de la caida hacia el borde
};

struct SpriteDrawList {
    std::vector<SpriteTexture> textures;
    std::vector<SpriteQuad> quads;
    std::vector<SpriteLight> lights;
    // Luz global de los sprites iluminados (la Global Light 2D).
    core::Vec3 ambient{1.0f, 1.0f, 1.0f};

    bool empty() const { return quads.empty(); }
};

}  // namespace cramion::gfx

#endif  // CRAMION_VK_SPRITE_GEOMETRY_H
