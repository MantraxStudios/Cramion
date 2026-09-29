#ifndef CRAMION_VK_POST_PROCESS_SETTINGS_H
#define CRAMION_VK_POST_PROCESS_SETTINGS_H

// CONTRATO COMPARTIDO: lo aplica VulkanRenderer::setPostProcess() y lo rellena
// el componente PostProcessing de CramionCore (como el Volume de Unity).

#include "CramionFX/core/Math.h"

#include <cstdint>

namespace cramion::gfx {

enum class Tonemapper : std::int32_t {
    Neutral = 0,  // Khronos PBR Neutral (por defecto)
    Aces = 1,     // ACES (ajuste de Stephen Hill)
    None = 2,     // lineal recortado (para depurar)
};

// Todo lo configurable del post-proceso y de los efectos de pantalla. Los
// valores por defecto son los que el motor usaba hasta ahora (asi la imagen
// no cambia si nadie los toca).
struct PostProcessSettings {
    // --- Exposicion ---
    bool auto_exposure = true;
    float exposure_compensation = 0.0f;  // EV (tambien con la manual)
    float manual_exposure = 1.0f;        // multiplicador con auto_exposure = false
    // Limites del ajuste de la auto-exposicion, en EV (log2 del
    // multiplicador): -2 = como mucho 4 veces mas oscuro, 1.4 = como mucho
    // ~2.6 veces mas claro que la exposicion neutra.
    float min_ev = -2.0f;
    float max_ev = 1.4f;
    float adaptation_speed_up = 3.0f;    // hacia una escena mas brillante (por segundo)
    float adaptation_speed_down = 1.2f;  // hacia una escena mas oscura
    // Vision nocturna del ojo (efecto Purkinje): con poca luz ven los
    // bastones, que no distinguen colores y son mas sensibles al azul. La
    // noche se ve gris azulada y lo que ilumina una farola conserva su
    // color. 0 = apagada, 1 = la del ojo.
    float night_vision = 1.0f;

    // --- Tonemapping ---
    Tonemapper tonemapper = Tonemapper::Neutral;

    // --- Bloom ---
    bool bloom = true;
    float bloom_intensity = 0.06f;
    float bloom_threshold = 0.0f;  // luminancia desde la que brilla (0 = todo)
    float bloom_scatter = 1.0f;    // radio del filtro de subida (0.5..2)
    core::Vec3 bloom_tint{1.0f, 1.0f, 1.0f};

    // --- Gradacion de color ---
    float temperature = 0.0f;  // -100 (frio) .. 100 (calido), balance de blancos
    float tint = 0.0f;         // -100 (verde) .. 100 (magenta)
    float contrast = 1.08f;    // 1 = sin cambio
    float saturation = 1.12f;  // 1 = sin cambio, 0 = blanco y negro
    float vibrance = 0.25f;    // satura mas lo apagado
    core::Vec3 color_filter{1.0f, 1.0f, 1.0f};
    core::Vec3 lift{0.0f, 0.0f, 0.0f};   // sombras (suma)
    core::Vec3 gamma{1.0f, 1.0f, 1.0f};  // medios (potencia)
    core::Vec3 gain{1.0f, 1.0f, 1.0f};   // luces (multiplica)

    // --- Vineta ---
    bool vignette = true;
    float vignette_intensity = 0.35f;
    float vignette_smoothness = 0.5f;
    core::Vec3 vignette_color{0.0f, 0.0f, 0.0f};

    // --- Lente ---
    float chromatic_aberration = 0.0f;  // 0..1
    float film_grain = 0.0f;            // 0..1
    // Distorsion de la lente: > 0 barril (gran angular), < 0 cojin (tele).
    float lens_distortion = 0.0f;       // -1..1
    // Destellos del sol en la lente (fantasmas, halo y estrella): 0 = no.
    float lens_flare = 0.0f;            // 0..2

    // --- Motion blur (camara y objetos, con los vectores de movimiento) ---
    bool motion_blur = false;
    // Fraccion del frame que el obturador esta abierto (0.5 = 180 grados).
    float motion_blur_intensity = 0.5f;
    // Tope del rastro, en fraccion del alto de la pantalla.
    float motion_blur_max = 0.05f;

    // --- Profundidad de campo (bokeh con la formula de una lente real) ---
    bool depth_of_field = false;
    bool dof_auto_focus = false;        // enfoca lo que hay en el centro de la pantalla
    float dof_focus_distance = 10.0f;   // metros
    float dof_aperture = 5.6f;          // numero f (menos = mas desenfoque)
    float dof_focal_length = 50.0f;     // milimetros (mas = mas desenfoque)

    // --- Rayos de luz del sol (pantalla) ---
    bool light_shafts = true;
    float light_shaft_intensity = 1.0f;

    // --- Antialiasing ---
    bool fxaa = true;

    // --- Efectos de pantalla (on/off y parametros principales) ---
    bool ambient_occlusion = true;
    bool global_illumination = true;
    bool reflections = true;
    bool volumetric_light = true;
    // Sombras de contacto del sol (rayo corto en pantalla): sombras pequenas
    // que las cascadas no resuelven. Largo del rayo en metros.
    bool contact_shadows = true;
    float contact_shadow_length = 0.5f;
    float volumetric_density = 0.02f;
    float volumetric_anisotropy = 0.6f;
    // Niebla por altura: densidad a la altura 0 (1/m) y lo rapido que se
    // aclara al subir (1/m). La de siempre es fina para escenarios pequenos;
    // un mundo abierto de kilometros necesita mucha menos.
    float fog_density = 0.0018f;
    float fog_height_falloff = 0.08f;
    // Tramos por rayo del polvo (volumetric.frag). El presupuesto adaptativo
    // lo baja a la mitad en vez de apagar el polvo: si lo apagaba, solo
    // volvia cuando el frame se abarataba (mirando al cielo o al sol).
    int volumetric_steps = 32;

    // --- Rendimiento ---
    // LODs automaticos: cada objeto estatico se dibuja (tambien en las
    // sombras) con el nivel mas simple cuyo error en pantalla no pasa de
    // `lod_pixel_error` pixeles. Mas alto = mas rapido y menos detalle lejos.
    bool lods = true;
    float lod_pixel_error = 1.0f;
};

}  // namespace cramion::gfx

#endif  // CRAMION_VK_POST_PROCESS_SETTINGS_H
