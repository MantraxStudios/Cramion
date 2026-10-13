#ifndef CRAMION_VK_AUTO_EXPOSURE_MATH_H
#define CRAMION_VK_AUTO_EXPOSURE_MATH_H

// Matematicas de la auto-exposicion en la CPU: las mismas que
// shaders/exposure_average.comp (que es la que corre de verdad) y el peso al
// centro de exposure_histogram.comp. Existen para probarlas con histogramas
// sinteticos (CoreTests). Si se cambia una, se cambia la otra.

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace cramion::gfx::auto_exposure {

// Deben coincidir con exposure_histogram.comp / exposure_average.comp.
constexpr int kBins = 256;
constexpr float kMinLogLuminance = -12.0f;
constexpr float kLogLuminanceRange = 18.0f;
// Luminancia de la escena que debe quedar como gris medio.
constexpr float kTargetLuminance = 0.36f;

// Ventana de percentiles de la medicion (GpuExposurePush low/high_percent).
constexpr float kLowPercent = 0.35f;
constexpr float kHighPercent = 0.96f;
// Ventana de las luces fuertes (bombillas, farolas, el sol).
constexpr float kHighlightLow = 0.95f;
constexpr float kHighlightHigh = 0.995f;

// Adaptacion parcial a la oscuridad: hasta kKnee EV se compensa entera (el
// dia no cambia); por encima solo una fraccion kSlope. Asi la noche sigue
// siendo noche (~x3) pero el controlador sigue respondiendo dentro del rango
// (antes se clavaba en max_ev y una farola no cambiaba nada).
constexpr float kKnee = 0.75f;
constexpr float kSlope = 0.25f;
// Una luz fuerte en la vista tira de la exposicion hacia abajo (como el ojo),
// solo de la parte "extra" de la noche: nunca por debajo de min(full, kKnee).
constexpr float kHighlightLuminance = 4.0f;
constexpr float kHighlightWeight = 0.5f;

// Estabilidad: zona muerta (no persigue diferencias minimas) y tope de EV
// por segundo (sin bombeo brusco al mover la cabeza en VR).
constexpr float kDeadband = 0.05f;
constexpr float kMaxEvPerSecond = 4.0f;

inline float binLogLuminance(int bin) {
    return kMinLogLuminance + (static_cast<float>(bin) - 0.5f) / 254.0f * kLogLuminanceRange;
}

// Cubo de una luminancia (como binFor de exposure_histogram.comp).
inline int binFor(float luminance) {
    if (!(luminance >= std::exp2(kMinLogLuminance))) return 0;
    const float t = std::clamp((std::log2(luminance) - kMinLogLuminance) / kLogLuminanceRange, 0.0f, 1.0f);
    return static_cast<int>(t * 254.0f + 1.0f);
}

// Media de log2(luminancia) entre dos percentiles (sin el cubo 0, el negro).
// Devuelve false si la ventana esta vacia.
inline bool windowMeanLog(const std::uint32_t* bins, float low_percent, float high_percent, float& out) {
    float total = 0.0f;
    for (int i = 1; i < kBins; ++i) total += static_cast<float>(bins[i]);
    if (total <= 0.0f) return false;
    const float low = total * low_percent;
    const float high = total * high_percent;
    float accumulated = 0.0f;
    float weighted_sum = 0.0f;
    float weight_total = 0.0f;
    for (int i = 1; i < kBins; ++i) {
        const float start = accumulated;
        const float end = accumulated + static_cast<float>(bins[i]);
        accumulated = end;
        const float inside = std::max(std::min(end, high) - std::max(start, low), 0.0f);
        weighted_sum += binLogLuminance(i) * inside;
        weight_total += inside;
    }
    if (weight_total <= 0.0f) return false;
    out = weighted_sum / weight_total;
    return true;
}

// Exposicion objetivo (log2) a partir de la media de la ventana y la de las
// luces fuertes.
inline float targetLogExposure(float average_log, float highlight_log, float min_ev, float max_ev) {
    const float full = std::log2(kTargetLuminance) - average_log;
    const float soft = full > kKnee ? kKnee + (full - kKnee) * kSlope : full;
    const float limit = std::log2(kHighlightLuminance) - highlight_log;
    const float pulled = limit < soft ? soft + (limit - soft) * kHighlightWeight : soft;
    const float target = std::max(pulled, std::min(full, kKnee));
    if (!std::isfinite(target)) return 0.0f;
    return std::clamp(target, std::min(min_ev, max_ev), std::max(min_ev, max_ev));
}

// Objetivo de un histograma de 256 cubos (sin adaptacion temporal).
inline float meterHistogram(const std::uint32_t* bins, float min_ev, float max_ev) {
    float average_log = std::log2(kTargetLuminance);
    windowMeanLog(bins, kLowPercent, kHighPercent, average_log);
    float highlight_log = average_log;
    windowMeanLog(bins, kHighlightLow, kHighlightHigh, highlight_log);
    return targetLogExposure(average_log, highlight_log, min_ev, max_ev);
}

// Un paso de adaptacion temporal (log2). `reset` = corte de camara.
inline float adapt(float current, float target, float delta_seconds, float speed_up, float speed_down,
                   bool initialized, bool reset = false) {
    if (!initialized || reset || !std::isfinite(current)) return target;
    const float diff = target - current;
    const float error = diff - std::clamp(diff, -kDeadband, kDeadband);
    const float speed = target < current ? speed_up : speed_down;
    const float max_step = kMaxEvPerSecond * delta_seconds;
    const float step = std::clamp(error * (1.0f - std::exp(-delta_seconds * speed)), -max_step, max_step);
    return current + step;
}

}  // namespace cramion::gfx::auto_exposure

#endif  // CRAMION_VK_AUTO_EXPOSURE_MATH_H
