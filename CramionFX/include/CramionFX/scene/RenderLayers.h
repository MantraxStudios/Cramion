#ifndef CRAMION_SCENE_RENDER_LAYERS_H
#define CRAMION_SCENE_RENDER_LAYERS_H

// Capas de render, arma en primera persona y contornos de juego (RM-capas).
//
// Capas: cada actor lleva una mascara (Actor::layers, la de su MeshRenderer:
// bit i = capa i, las 32 capas con nombre del proyecto). La camara dibuja lo
// que comparte alguna capa con su culling mask; cada luz proyecta la sombra
// de lo que comparte alguna con su shadow_layers. Por defecto todo esta en la
// capa 0 y las mascaras de camaras y luces son "todas": nada cambia.
//
// Viewmodel: la camara puede dibujar las capas de su Viewmodel con otro campo
// de vision y al principio del rango de profundidad (como el glDepthRange del
// arma de Quake o el First Person Rendering de UE5): viewmodelMatrix() lleva
// el actor al espacio que da ese campo de vision (se aplica a su transform,
// asi que el G-buffer, la luz y los vectores de movimiento salen solos) y el
// viewport de su dibujo comprime la profundidad en [0, depth_range].
//
// Contornos: OutlineStyle describe un estilo (color, grosor, modo y relleno de
// lo tapado); packOutlineStyle() lo deja en los dos enteros que lee
// game_outline.frag. Todo lo de aqui no toca la GPU (se prueba sin ella).

#include "CramionFX/core/Math.h"
#include "CramionFX/scene/Camera.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace cramion::scene {

inline constexpr std::uint32_t kAllRenderLayers = 0xFFFFFFFFu;
inline constexpr std::uint32_t kDefaultRenderLayers = 1u;  // la capa 0

// La camara (o la luz) con `mask` ve a un objeto de `layers`.
constexpr bool layersVisible(std::uint32_t layers, std::uint32_t mask) { return (layers & mask) != 0u; }

// --- Viewmodel -----------------------------------------------------------------

// Factor de escala en x/y (espacio de vista) que da al viewmodel su campo de
// vision: tan(fov_camara / 2) / tan(fov_viewmodel / 2).
inline float viewmodelFovScale(float camera_fov_y, float viewmodel_fov_y) {
    const float a = std::tan(std::clamp(camera_fov_y, 0.01f, 3.1f) * 0.5f);
    const float b = std::tan(std::clamp(viewmodel_fov_y, 0.01f, 3.1f) * 0.5f);
    return a / b;
}

// Cuanto se aleja el arma (escala uniforme desde el ojo) para que su punto mas
// cercano (viewmodel near) no lo corte el plano cercano de la camara. Nunca la
// acerca: la profundidad comprimida ya la pone delante de todo.
inline float viewmodelDepthScale(float camera_near, float viewmodel_near) {
    return std::max(1.0f, camera_near / std::max(viewmodel_near, 1e-4f));
}

// Matriz (en el mundo) que se antepone al transform de un actor del viewmodel:
// inversa(vista) * escala(k*s, k*s, s) * vista. Lo que estaba a d metros del
// ojo queda a s*d en la misma direccion de pantalla, abierto por el campo de
// vision del viewmodel. Identidad si no aplica (ortografica o sin viewmodel).
inline core::Mat4 viewmodelMatrix(const Camera& camera) {
    const Camera::Viewmodel& vm = camera.viewmodel();
    if (!vm.active() || camera.orthographic()) return core::Mat4::identity();
    const float k = viewmodelFovScale(camera.fovY(), vm.fov_y);
    const float s = viewmodelDepthScale(camera.nearPlane(), vm.near_plane);
    const core::Mat4 view = camera.view();
    return core::inverse(view) * core::scale(core::Vec3{k * s, k * s, s}) * view;
}

// Rango del viewport para el viewmodel: [0, depth_range] (profundidad de 0
// cerca a 1 lejos). Acotado para que siempre quede algo de rango.
inline float viewmodelDepthRange(const Camera::Viewmodel& vm) { return std::clamp(vm.depth_range, 0.0005f, 0.5f); }

// --- Contornos de juego ------------------------------------------------------------

// Siempre: alrededor de toda la silueta (tambien la tapada).
// Tapado: solo donde algo lo tapa (silueta a traves de paredes, MOBA), con
//         relleno opcional de la parte tapada.
// Visible: solo alrededor de lo que se ve.
enum class OutlineMode : std::uint32_t { Always = 0, Occluded = 1, Visible = 2 };

struct OutlineStyle {
    core::Vec3 color{1.0f, 0.6f, 0.1f};  // sRGB 0..1 (se mezcla sobre la imagen final)
    float alpha = 1.0f;                  // con el pulso ya aplicado
    float width = 2.0f;                  // pixeles de pantalla (0.5..15)
    OutlineMode mode = OutlineMode::Always;
    float occluded_fill = 0.0f;  // opacidad del relleno de la parte tapada (0..1)
};

// Lo que cabe en las push constants de game_outline.frag: 15 estilos (2
// enteros cada uno) y el hueco 15 con (cuantos, radio de busqueda en px).
inline constexpr std::uint32_t kMaxOutlineStyles = 15;

inline std::uint32_t outlineUnorm8(float v) {
    return static_cast<std::uint32_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
}

// word0 = RGBA8 (r en el byte bajo); word1 = grosor en 1/16 px (bits 0-7),
// modo (bits 8-9) y relleno de lo tapado (bits 16-23).
inline std::array<std::uint32_t, 2> packOutlineStyle(const OutlineStyle& style) {
    const std::uint32_t color = outlineUnorm8(style.color.x) | (outlineUnorm8(style.color.y) << 8) |
                                (outlineUnorm8(style.color.z) << 16) | (outlineUnorm8(style.alpha) << 24);
    const auto width = static_cast<std::uint32_t>(std::lround(std::clamp(style.width, 0.5f, 15.9f) * 16.0f));
    const std::uint32_t params = (width & 0xFFu) | ((static_cast<std::uint32_t>(style.mode) & 3u) << 8) |
                                 (outlineUnorm8(style.occluded_fill) << 16);
    return {color, params};
}

inline float unpackOutlineWidth(std::uint32_t params) { return static_cast<float>(params & 0xFFu) / 16.0f; }
inline OutlineMode unpackOutlineMode(std::uint32_t params) { return static_cast<OutlineMode>((params >> 8) & 3u); }

// Pulso del contorno: multiplica a la opacidad, de `minimum` a 1 con
// `speed_hz` ciclos por segundo (0 = fijo en 1).
inline float outlinePulse(float time_seconds, float speed_hz, float minimum) {
    if (speed_hz <= 0.0f) return 1.0f;
    const float m = std::clamp(minimum, 0.0f, 1.0f);
    const float wave = 0.5f + 0.5f * std::cos(time_seconds * speed_hz * 2.0f * core::kPi);
    return m + (1.0f - m) * wave;
}

// Contornos de un frame: los estilos distintos (como mucho kMaxOutlineStyles;
// los de mas usan el ultimo) y los actores con el suyo (indice en styles).
struct OutlinedActor {
    std::uint32_t actor = 0;  // indice en Scene::actors()
    std::uint32_t style = 0;
};
struct GameOutlines {
    std::vector<OutlineStyle> styles;
    std::vector<OutlinedActor> actors;
    bool empty() const { return actors.empty() || styles.empty(); }

    // Indice del estilo (reutiliza uno igual ya anadido).
    std::uint32_t addStyle(const OutlineStyle& style) {
        const std::array<std::uint32_t, 2> packed = packOutlineStyle(style);
        for (std::size_t i = 0; i < styles.size(); ++i) {
            if (packOutlineStyle(styles[i]) == packed) return static_cast<std::uint32_t>(i);
        }
        if (styles.size() >= kMaxOutlineStyles) return kMaxOutlineStyles - 1;
        styles.push_back(style);
        return static_cast<std::uint32_t>(styles.size() - 1);
    }

    // Las push constants de game_outline.frag (32 enteros: 128 bytes).
    std::array<std::uint32_t, 32> pack() const {
        std::array<std::uint32_t, 32> out{};
        float radius = 0.0f;
        const std::size_t count = std::min<std::size_t>(styles.size(), kMaxOutlineStyles);
        for (std::size_t i = 0; i < count; ++i) {
            const std::array<std::uint32_t, 2> p = packOutlineStyle(styles[i]);
            out[i * 2] = p[0];
            out[i * 2 + 1] = p[1];
            radius = std::max(radius, unpackOutlineWidth(p[1]));
        }
        out[30] = static_cast<std::uint32_t>(count);
        out[31] = static_cast<std::uint32_t>(std::ceil(radius));
        return out;
    }
};

}  // namespace cramion::scene

#endif  // CRAMION_SCENE_RENDER_LAYERS_H
