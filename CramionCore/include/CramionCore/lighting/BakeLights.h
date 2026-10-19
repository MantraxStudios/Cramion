#ifndef CRAMION_CORE_LIGHTING_BAKE_LIGHTS_H
#define CRAMION_CORE_LIGHTING_BAKE_LIGHTS_H

// Cuentas de la luz que comparten el horneado (ProbeBaker, Lightmapper) y el
// render (lighting.frag, ibl_sh.comp): las mismas formulas, para que lo
// horneado y el tiempo real den la misma luz (PLAN-LIGHTMAPS.md 1.5).

#include <CramionFX/core/Math.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace cramion::lighting {

// Cono de un foco: 0 fuera del angulo exterior, 1 dentro del interior y
// entre medias x^2 lineal (copia de lighting.frag, focos).
inline float spotCone(float cos_angle, float cos_inner, float cos_outer) {
    const float x = std::clamp((cos_angle - cos_outer) / std::max(cos_inner - cos_outer, 0.0001f), 0.0f, 1.0f);
    return x * x;
}

// Atenuacion de las luces locales: 1/(d^2 + 1) con un corte suave al llegar
// al alcance (lighting.frag attenuation()).
inline float lightAttenuation(float distance, float range) {
    const float ratio = distance / std::max(range, 1e-4f);
    const float window = std::clamp(1.0f - ratio * ratio * ratio * ratio, 0.0f, 1.0f);
    return window * window / (distance * distance + 1.0f);
}

// sRGB -> lineal como el render (pow 2.2).
inline core::Vec3 srgbToLinear(const core::Vec3& c) {
    return core::Vec3{std::pow(std::max(c.x, 0.0f), 2.2f), std::pow(std::max(c.y, 0.0f), 2.2f),
                      std::pow(std::max(c.z, 0.0f), 2.2f)};
}

// Base de armonicos esfericos de orden 2 (la misma que ibl_sh.comp).
inline void shBasis9(const core::Vec3& d, float y[9]) {
    y[0] = 0.282095f;
    y[1] = 0.488603f * d.y;
    y[2] = 0.488603f * d.z;
    y[3] = 0.488603f * d.x;
    y[4] = 1.092548f * d.x * d.y;
    y[5] = 1.092548f * d.y * d.z;
    y[6] = 0.315392f * (3.0f * d.z * d.z - 1.0f);
    y[7] = 1.092548f * d.x * d.z;
    y[8] = 0.546274f * (d.x * d.x - d.y * d.y);
}

// Convolucion con el coseno / pi por banda (Ramamoorthi y Hanrahan).
inline constexpr float kShBand[9] = {1.0f, 2.0f / 3.0f, 2.0f / 3.0f, 2.0f / 3.0f, 0.25f, 0.25f, 0.25f, 0.25f, 0.25f};

// Irradiancia del cielo (ya / pi) en la normal `n` desde los 9 coeficientes
// del render (irradianceSh de lighting.frag).
inline core::Vec3 irradianceFromSh(const std::array<core::Vec3, 9>& sh, const core::Vec3& n) {
    float y[9];
    shBasis9(n, y);
    core::Vec3 r{};
    for (int k = 0; k < 9; ++k) r += sh[static_cast<std::size_t>(k)] * y[k];
    return core::Vec3{std::max(r.x, 0.0f), std::max(r.y, 0.0f), std::max(r.z, 0.0f)};
}

// Radiancia del cielo en la direccion `d` reconstruida de esos coeficientes:
// se deshace la convolucion de cada banda. Con un cielo constante L,
// irradianceFromSh(n) = L.
inline core::Vec3 skyRadianceFromSh(const std::array<core::Vec3, 9>& sh, const core::Vec3& d) {
    float y[9];
    shBasis9(d, y);
    core::Vec3 r{};
    for (int k = 0; k < 9; ++k) r += sh[static_cast<std::size_t>(k)] * (y[k] / kShBand[k]);
    return core::Vec3{std::max(r.x, 0.0f), std::max(r.y, 0.0f), std::max(r.z, 0.0f)};
}

// Generador determinista: hash de enteros (semilla por sonda, texel, muestra
// y pasada; nunca por hilo).
inline std::uint32_t hash32(std::uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}
inline std::uint32_t hashCombine(std::uint32_t a, std::uint32_t b) { return hash32(a ^ (b * 0x9E3779B9u + 0x7F4A7C15u)); }
inline float hashToUnit(std::uint32_t h) { return static_cast<float>(h >> 8) * (1.0f / 16777216.0f); }

}  // namespace cramion::lighting

#endif  // CRAMION_CORE_LIGHTING_BAKE_LIGHTS_H
