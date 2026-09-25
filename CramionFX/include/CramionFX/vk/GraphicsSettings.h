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

}  // namespace cramion::gfx

#endif  // CRAMION_VK_GRAPHICS_SETTINGS_H
