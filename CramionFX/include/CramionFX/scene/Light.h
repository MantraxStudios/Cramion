#ifndef CRAMION_SCENE_LIGHT_H
#define CRAMION_SCENE_LIGHT_H

#include "CramionFX/core/Math.h"

#include <cstdint>
#include <vector>

namespace cramion::scene {

// Topes de luces por frame. Deben coincidir con las constantes del shader
// shaders/lighting.frag.
inline constexpr std::uint32_t kMaxPointLights = 32;
inline constexpr std::uint32_t kMaxSpotLights = 8;

// Luz direccional: el sol. No tiene posicion, solo direccion de incidencia.
struct DirectionalLight {
    core::Vec3 direction{-0.45f, -0.80f, -0.40f};  // Hacia donde viajan los rayos.
    core::Vec3 color{1.0f, 0.95f, 0.85f};
    float intensity = 2.2f;
};

// Luz puntual: irradia en todas direcciones y se atenua con la distancia.
struct PointLight {
    core::Vec3 position{};
    core::Vec3 color{1.0f, 0.75f, 0.35f};
    float intensity = 12.0f;
    float range = 18.0f;
};

// Foco: luz puntual limitada a un cono. Entre el angulo interior y el exterior
// el borde se difumina.
struct SpotLight {
    core::Vec3 position{};
    core::Vec3 direction{0.0f, -1.0f, 0.0f};
    core::Vec3 color{1.0f, 1.0f, 0.95f};
    float intensity = 25.0f;
    float range = 40.0f;
    float inner_angle = core::radians(14.0f);
    float outer_angle = core::radians(24.0f);
    bool enabled = true;
};

// Iluminacion ambiental de relleno (cielo).
struct AmbientLight {
    core::Vec3 color{0.45f, 0.58f, 0.78f};
    float intensity = 0.18f;
};

// Posicion de los astros para pintar el cielo. A diferencia de la luz
// direccional, aqui el sol sigue existiendo aunque este bajo el horizonte.
struct SkyState {
    core::Vec3 to_sun{0.0f, 1.0f, 0.0f};   // Direccion hacia el sol (unitaria).
    core::Vec3 to_moon{0.0f, -1.0f, 0.0f}; // Direccion hacia la luna (unitaria).
    float daylight = 1.0f;  // 0 = noche cerrada, 1 = pleno dia.
    float twilight = 0.0f;  // 1 con el sol en el horizonte (amanecer / ocaso).
};

// Conjunto de luces que se envia a la pasada de iluminacion.
struct LightSet {
    // Luz direccional activa: el sol de dia y la luna de noche. Es la que
    // ilumina y proyecta las sombras en cascada.
    DirectionalLight sun;
    AmbientLight ambient;
    SkyState sky;
    std::vector<PointLight> points;
    std::vector<SpotLight> spots;
};

}  // namespace cramion::scene

#endif  // CRAMION_SCENE_LIGHT_H
