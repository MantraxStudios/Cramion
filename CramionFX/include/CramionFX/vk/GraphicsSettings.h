#ifndef CRAMION_VK_GRAPHICS_SETTINGS_H
#define CRAMION_VK_GRAPHICS_SETTINGS_H

// Configuracion grafica que cambia la resolucion interna o la presentacion
// (VulkanRenderer::setGraphicsSettings rehace los destinos si hace falta).
//
//   Escalador   Off:  resolucion nativa, FXAA.
//               TAA:  antialiasing temporal; con resolucion menor que la de
//                     pantalla es un escalado temporal (TAAU, como Unreal).
//               FSR1: AMD FidelityFX Super Resolution 1 (espacial: EASU + RCAS).
//               FSR3 / DLSS: escalado temporal de AMD / NVIDIA (con
//                     generacion de frames), cuando sus SDK estan integrados.
//   Calidad     resolucion interna respecto a la de pantalla, con los mismos
//               factores que DLSS y FSR (Calidad 67 %, Equilibrado 58 %...).

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace cramion::gfx {

enum class Upscaler : std::int32_t { Off = 0, Taa = 1, Fsr1 = 2, Fsr3 = 3, Dlss = 4 };

enum class UpscaleQuality : std::int32_t {
    Native = 0,            // 100 % (DLAA / FSR Native AA)
    Quality = 1,           // 67 %
    Balanced = 2,          // 58 %
    Performance = 3,       // 50 %
    UltraPerformance = 4,  // 33 %
    Custom = 5,
};

struct GraphicsSettings {
    Upscaler upscaler = Upscaler::Off;
    UpscaleQuality quality = UpscaleQuality::Native;
    float custom_scale = 0.75f;  // con Custom
    float sharpness = 0.25f;     // 0..1 (RCAS)
    bool vsync = false;          // FIFO; sin el, mailbox (sin tearing, sin tope)
    bool frame_generation = false;
    // Presupuesto adaptativo (FrameBudget.h): baja solo la calidad necesaria
    // para llegar a `target_fps`.
    bool adaptive = true;
    float target_fps = 60.0f;
    // Mapa de sombras del sol por cascada; 0 = segun el perfil de hardware.
    std::int32_t shadow_resolution = 0;
    // Lado maximo de las texturas de los modelos; 0 = segun el perfil de
    // hardware. Se aplica a lo que se carga despues (reabrir la escena).
    std::int32_t texture_max_size = 0;
    // Filtrado anisotropico de las texturas de los materiales: 1 = apagado
    // (trilineal), 2, 4, 8 o 16; 0 = segun el perfil de hardware. Se aplica
    // al momento (rehace los samplers).
    std::int32_t texture_filtering = 0;
    // Sesgo de los mips (-1..2): negativo = texturas mas nitidas (y mas
    // parpadeo de lejos), positivo = mas suaves. Se aplica al momento.
    float texture_lod_bias = 0.0f;
    // VR: resolucion de cada ojo respecto a la que recomienda el casco
    // (0.5..1). Con el casco en marcha sustituye a la calidad del escalador
    // de escritorio (los presets Baja/Media/Alta no bajan los ojos).
    float vr_render_scale = 1.0f;

    friend bool operator==(const GraphicsSettings&, const GraphicsSettings&) = default;
};

inline float renderScale(const GraphicsSettings& s) {
    if (s.upscaler == Upscaler::Off) return 1.0f;
    switch (s.quality) {
        case UpscaleQuality::Native: return 1.0f;
        case UpscaleQuality::Quality: return 1.0f / 1.5f;
        case UpscaleQuality::Balanced: return 1.0f / 1.7f;
        case UpscaleQuality::Performance: return 0.5f;
        case UpscaleQuality::UltraPerformance: return 1.0f / 3.0f;
        case UpscaleQuality::Custom: return std::clamp(s.custom_scale, 0.25f, 1.0f);
    }
    return 1.0f;
}

// VR: tope de la escala de cada ojo por la VRAM. Cada destino (G-buffer, GI,
// reflejos, historias por ojo) va al tamano del ojo; con poca memoria no cabe
// el ojo entero. Pixeles por ojo que se dejan: ~0.31 veces 1080p por GB (8 GB
// = 2.5x 1080p, 0.87 en un ojo de Quest 3), nunca menos de 1.5x 1080p (lo que
// habia antes con cualquier GPU de menos de 12 GB) y sin tope desde 12 GB.
// 1 = sin tope.
inline float vrVramScaleCap(std::uint64_t vram_mb, std::uint32_t eye_width, std::uint32_t eye_height) {
    if (vram_mb == 0 || vram_mb >= 12 * 1024 || eye_width == 0 || eye_height == 0) return 1.0f;
    const double gb = static_cast<double>(vram_mb) / 1024.0;
    const double factor = std::clamp(gb * 0.3125, 1.5, 3.75);
    const double max_pixels = 1920.0 * 1080.0 * factor;
    const double eye_pixels = static_cast<double>(eye_width) * static_cast<double>(eye_height);
    const double cap = std::sqrt(std::min(1.0, max_pixels / eye_pixels));
    return static_cast<float>(std::clamp(cap, 0.25, 1.0));
}

// Escritorio: lo mismo para la ventana. Los destinos de la escena son unos
// 290 bytes por pixel: una ventana maximizada en un monitor de 5760x3240
// (5559x3144, 17,5 Mpx) eran ~5 GB y con 8 GB no cabian (ErrorOutOfDeviceMemory
// al arrancar). Pixeles internos que se dejan: 0.5 veces 1080p por GB (8 GB =
// 4K), nunca menos de 1440p, y sin tope desde 24 GB. 1 = sin tope.
inline float desktopVramScaleCap(std::uint64_t vram_mb, std::uint32_t width, std::uint32_t height) {
    if (vram_mb == 0 || vram_mb >= 24 * 1024 || width == 0 || height == 0) return 1.0f;
    const double gb = static_cast<double>(vram_mb) / 1024.0;
    const double max_pixels = std::max(1920.0 * 1080.0 * gb * 0.5, 2560.0 * 1440.0);
    const double pixels = static_cast<double>(width) * static_cast<double>(height);
    const double cap = std::sqrt(std::min(1.0, max_pixels / pixels));
    return static_cast<float>(std::clamp(cap, 0.25, 1.0));
}

// Sesgo de mips al dibujar por debajo de la resolucion de salida (lo que
// recomiendan FSR y DLSS): log2(interna / salida), entre -1 y 0. Con FSR 1
// (espacial, sin jitter) la mitad: el sesgo entero daba parpadeo.
inline float upscaleMipBias(float render_over_output, Upscaler upscaler) {
    if (upscaler == Upscaler::Off || !(render_over_output > 0.0f) || render_over_output >= 0.999f) return 0.0f;
    float bias = std::clamp(std::log2(render_over_output), -1.0f, 0.0f);
    if (upscaler == Upscaler::Fsr1) bias *= 0.5f;
    return bias;
}

}  // namespace cramion::gfx

#endif  // CRAMION_VK_GRAPHICS_SETTINGS_H
