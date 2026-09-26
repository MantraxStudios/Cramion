#include "CramionFX/vk/FrameBudget.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace cramion::gfx {
namespace {

constexpr std::array<std::uint8_t, kLeverCount> kMaxLevel = {
    3,  // Lod
    3,  // ShadowDetail
    1,  // Volumetric
    1,  // ContactShadows
    1,  // Ssao
    1,  // Reflections
    1,  // Gi
    5,  // RenderScale
};

constexpr std::array<float, 6> kRenderScales = {1.0f, 0.87f, 0.77f, 0.67f, 0.58f, 0.5f};

// Promedio exponencial con constante de tiempo `tau` segundos.
float smooth(float value, float target, float dt, float tau) {
    return value + (target - value) * (1.0f - std::exp(-dt / tau));
}

}  // namespace

HardwareTier tierFor(std::uint64_t vram_mb, bool integrated) {
    if (integrated || vram_mb < 3 * 1024) return HardwareTier::Low;
    if (vram_mb < 6 * 1024) return HardwareTier::Medium;
    if (vram_mb < 11 * 1024) return HardwareTier::High;
    return HardwareTier::Ultra;
}

const char* tierName(HardwareTier tier) {
    switch (tier) {
        case HardwareTier::Low: return "Bajo";
        case HardwareTier::Medium: return "Medio";
        case HardwareTier::High: return "Alto";
        case HardwareTier::Ultra: return "Ultra";
    }
    return "?";
}

std::uint32_t shadowResolutionFor(HardwareTier tier) {
    switch (tier) {
        case HardwareTier::Low: return 1536;
        case HardwareTier::Medium: return 2048;
        case HardwareTier::High: return 4096;
        case HardwareTier::Ultra: return 6144;
    }
    return 4096;
}

std::uint32_t textureSizeFor(HardwareTier tier) {
    switch (tier) {
        case HardwareTier::Low: return 2048;
        case HardwareTier::Medium: return 4096;
        case HardwareTier::High: return 8192;
        case HardwareTier::Ultra: return 0;
    }
    return 8192;
}

FrameBudget::FrameBudget() {
    for (auto& row : learned_) row.fill(-1.0f);
}

void FrameBudget::setEnabled(bool enabled) {
    if (enabled == enabled_) return;
    enabled_ = enabled;
    // Apagado: todo a la calidad del usuario. Encendido: desde el perfil.
    levels_ = enabled ? start_levels_ : std::array<std::uint8_t, kLeverCount>{};
    pending_.active = false;
    over_seconds_ = under_seconds_ = 0.0f;
}

void FrameBudget::setTargetFps(float fps) {
    target_fps_ = std::clamp(fps, 15.0f, 360.0f);
}

void FrameBudget::setStartLevels(HardwareTier tier) {
    start_levels_ = {};
    if (tier == HardwareTier::Low) {
        start_levels_[static_cast<std::size_t>(Lever::Lod)] = 1;
        start_levels_[static_cast<std::size_t>(Lever::ShadowDetail)] = 1;
        start_levels_[static_cast<std::size_t>(Lever::Volumetric)] = 1;
        start_levels_[static_cast<std::size_t>(Lever::RenderScale)] = 2;  // 77 %
    } else if (tier == HardwareTier::Medium) {
        start_levels_[static_cast<std::size_t>(Lever::Lod)] = 1;
    }
    if (enabled_) levels_ = start_levels_;
}

std::uint8_t FrameBudget::maxLevel(Lever lever) const {
    return kMaxLevel[static_cast<std::size_t>(lever)];
}

const char* FrameBudget::leverName(Lever lever) {
    switch (lever) {
        case Lever::Lod: return "Detalle de mallas (LOD)";
        case Lever::ShadowDetail: return "Detalle de sombras";
        case Lever::Volumetric: return "Luz volumetrica";
        case Lever::ContactShadows: return "Sombras de contacto";
        case Lever::Ssao: return "Oclusion ambiental";
        case Lever::Reflections: return "Reflejos";
        case Lever::Gi: return "Luz rebotada (GI)";
        case Lever::RenderScale: return "Resolucion interna";
        case Lever::Count: break;
    }
    return "?";
}

float FrameBudget::learnedSaving(Lever lever, std::uint8_t level) const {
    return level < kMaxLevels ? learned_[static_cast<std::size_t>(lever)][level] : -1.0f;
}

// Cuanto se espera ahorrar al bajar un paso desde `level`, con el coste
// medido de las pasadas que toca la palanca.
float FrameBudget::estimateSaving(Lever lever, std::uint8_t level) const {
    const PassCosts& p = passes_;
    switch (lever) {
        case Lever::Lod: return 0.35f * (p.geometry + 0.5f * p.shadows);
        case Lever::ShadowDetail: return 0.35f * (p.shadows + p.local_shadows);
        case Lever::Volumetric: return 0.95f * (p.volumetric + p.shafts);
        case Lever::ContactShadows: return 0.25f * p.lighting;
        case Lever::Ssao: return 0.95f * p.ssao;
        case Lever::Reflections: return 0.9f * p.reflections;
        case Lever::Gi: return 0.95f * p.gi;
        case Lever::RenderScale: {
            // Lo que depende de los pixeles (casi todo menos las sombras) baja
            // con el area de la imagen.
            const float pixel_ms = std::max(p.total - p.shadows - p.local_shadows, 0.0f);
            const float now = kRenderScales[std::min<std::size_t>(level, 5)];
            const float next = kRenderScales[std::min<std::size_t>(level + 1u, 5)];
            return pixel_ms * (1.0f - (next * next) / (now * now));
        }
        case Lever::Count: break;
    }
    return 0.0f;
}

// Cuanto se nota bajar un paso desde `level` (mas alto = se nota mas).
float FrameBudget::penalty(Lever lever, std::uint8_t level) const {
    switch (lever) {
        case Lever::Lod: return 1.0f + 0.75f * level;
        case Lever::ShadowDetail: return 1.0f + 0.75f * level;
        case Lever::Volumetric: return 2.0f;
        case Lever::ContactShadows: return 1.5f;
        case Lever::Ssao: return 3.0f;
        case Lever::Reflections: return 3.0f;
        case Lever::Gi: return 4.0f;
        case Lever::RenderScale: return 2.0f + 0.6f * level;
        case Lever::Count: break;
    }
    return 1.0f;
}

// Lo que tarda un cambio en verse en el tiempo de GPU (los timestamps van un
// par de frames tarde, la resolucion rehace los destinos y el historial).
float FrameBudget::settleSeconds(Lever lever) const {
    return lever == Lever::RenderScale ? 1.5f : 0.5f;
}

void FrameBudget::change(Lever lever, std::uint8_t to, const char* verb, float predicted) {
    const auto index = static_cast<std::size_t>(lever);
    pending_ = Pending{true, lever, levels_[index], to, ema_ms_, 0.0f};
    levels_[index] = to;
    over_seconds_ = under_seconds_ = 0.0f;
    char text[160];
    std::snprintf(text, sizeof(text), "%s %s a %u/%u (prevision %.2f ms)", verb, leverName(lever),
                  static_cast<unsigned>(to), static_cast<unsigned>(maxLevel(lever)), predicted);
    last_action_ = text;
}

bool FrameBudget::lowerOne() {
    int best = -1;
    float best_score = 0.0f;
    float best_saving = 0.0f;
    for (std::size_t i = 0; i < kLeverCount; ++i) {
        const auto lever = static_cast<Lever>(i);
        const std::uint8_t level = levels_[i];
        if (level >= kMaxLevel[i]) continue;
        const float learned = learned_[i][level];
        const float saving = learned >= 0.0f ? learned : estimateSaving(lever, level);
        if (saving < 0.05f) continue;  // no ahorra nada (p. ej. el usuario ya lo apago)
        const float score = saving / penalty(lever, level);
        if (score > best_score) {
            best_score = score;
            best_saving = saving;
            best = static_cast<int>(i);
        }
    }
    if (best < 0) return false;
    const auto lever = static_cast<Lever>(best);
    change(lever, static_cast<std::uint8_t>(levels_[static_cast<std::size_t>(best)] + 1), "Baja", best_saving);
    return true;
}

bool FrameBudget::raiseOne() {
    const float budget = targetMilliseconds() * 0.85f;
    int best = -1;
    float best_score = 0.0f;
    float best_cost = 0.0f;
    for (std::size_t i = 0; i < kLeverCount; ++i) {
        const std::uint8_t level = levels_[i];
        if (level == 0 || clock_ < raise_blocked_until_[i]) continue;
        // Lo que costaba ese paso cuando se midio; sin medir, prudente.
        const float learned = learned_[i][level - 1];
        const float cost = learned >= 0.0f ? learned : 1.0f;
        if (ema_ms_ + cost > budget) continue;
        // Primero lo que mas se nota por cada ms que cuesta.
        const float score = penalty(static_cast<Lever>(i), static_cast<std::uint8_t>(level - 1)) /
                            std::max(cost, 0.1f);
        if (score > best_score) {
            best_score = score;
            best_cost = cost;
            best = static_cast<int>(i);
        }
    }
    if (best < 0) return false;
    raised_at_[static_cast<std::size_t>(best)] = clock_;
    change(static_cast<Lever>(best), static_cast<std::uint8_t>(levels_[static_cast<std::size_t>(best)] - 1), "Sube",
           best_cost);
    return true;
}

void FrameBudget::update(float dt, float gpu_ms, const std::vector<GpuTiming>& passes) {
    if (dt <= 0.0f) return;
    clock_ += dt;

    // Coste de cada pasada que toca alguna palanca (media suave).
    PassCosts now{};
    for (const GpuTiming& t : passes) {
        const std::string& n = t.name;
        const float ms = t.milliseconds;
        if (n == "Geometria + culling") now.geometry += ms;
        else if (n == "Sombras (cascadas)") now.shadows += ms;
        else if (n == "Sombras locales") now.local_shadows += ms;
        else if (n == "Volumetrica") now.volumetric += ms;
        else if (n == "Rayos de luz") now.shafts += ms;
        else if (n == "Iluminacion") now.lighting += ms;
        else if (n == "SSAO") now.ssao += ms;
        else if (n == "Reflejos") now.reflections += ms;
        else if (n == "GI") now.gi += ms;
    }
    now.total = gpu_ms;
    const auto blend = [&](float& value, float sample) { value = smooth(value, sample, dt, 0.5f); };
    blend(passes_.geometry, now.geometry);
    blend(passes_.shadows, now.shadows);
    blend(passes_.local_shadows, now.local_shadows);
    blend(passes_.volumetric, now.volumetric);
    blend(passes_.shafts, now.shafts);
    blend(passes_.lighting, now.lighting);
    blend(passes_.ssao, now.ssao);
    blend(passes_.reflections, now.reflections);
    blend(passes_.gi, now.gi);
    blend(passes_.total, now.total);

    // Tiempo de GPU suavizado; los tirones (cargas, un frame de 200 ms) no
    // cuentan: no son culpa de la calidad.
    if (!has_sample_) {
        ema_ms_ = gpu_ms;
        has_sample_ = true;
    } else if (gpu_ms < ema_ms_ * 4.0f + 8.0f) {
        ema_ms_ = smooth(ema_ms_, gpu_ms, dt, 0.25f);
    }

    if (!enabled_) return;

    // Un cambio en observacion: al asentarse se apunta lo que ahorro de
    // verdad (o lo que costo al subir). Ese dato manda en adelante.
    if (pending_.active) {
        pending_.timer += dt;
        if (pending_.timer < settleSeconds(pending_.lever)) return;
        const auto i = static_cast<std::size_t>(pending_.lever);
        const std::uint8_t step = std::min(pending_.from, pending_.to);
        const float delta = pending_.to > pending_.from ? pending_.before_ms - ema_ms_ : ema_ms_ - pending_.before_ms;
        learned_[i][step] = std::max(delta, 0.0f);
        pending_.active = false;
    }

    const float target = targetMilliseconds();
    over_seconds_ = ema_ms_ > target ? over_seconds_ + dt : 0.0f;
    under_seconds_ = ema_ms_ < target * 0.8f ? under_seconds_ + dt : 0.0f;

    if (over_seconds_ > 0.3f) {
        // Si se sube algo y en menos de 5 s hay que bajarlo, esa palanca
        // no se vuelve a subir en un rato (y cada vez mas).
        for (std::size_t i = 0; i < kLeverCount; ++i) {
            if (raised_at_[i] > 0.0f && clock_ - raised_at_[i] < 5.0f) {
                raise_backoff_[i] = std::min(std::max(raise_backoff_[i] * 2.0f, 15.0f), 240.0f);
                raise_blocked_until_[i] = clock_ + raise_backoff_[i];
                raised_at_[i] = 0.0f;
            }
        }
        if (!lowerOne()) over_seconds_ = 0.0f;  // ya esta todo al minimo
    } else if (under_seconds_ > 3.0f) {
        if (!raiseOne()) under_seconds_ = 0.0f;
    }
}

PostProcessSettings FrameBudget::apply(const PostProcessSettings& user) const {
    PostProcessSettings s = user;
    if (!enabled_) return s;
    static constexpr float kLodFactor[] = {1.0f, 2.0f, 4.0f, 8.0f};
    s.lod_pixel_error *= kLodFactor[std::min<std::uint8_t>(level(Lever::Lod), 3)];
    if (level(Lever::Volumetric) > 0) {
        s.volumetric_light = false;
        s.light_shafts = false;
    }
    if (level(Lever::ContactShadows) > 0) s.contact_shadows = false;
    if (level(Lever::Ssao) > 0) s.ambient_occlusion = false;
    if (level(Lever::Reflections) > 0) s.reflections = false;
    if (level(Lever::Gi) > 0) s.global_illumination = false;
    return s;
}

float FrameBudget::renderScale() const {
    return enabled_ ? kRenderScales[std::min<std::uint8_t>(level(Lever::RenderScale), 5)] : 1.0f;
}

float FrameBudget::cullPixels() const {
    static constexpr float kCull[] = {0.5f, 1.0f, 2.0f, 3.5f};
    return enabled_ ? kCull[std::min<std::uint8_t>(level(Lever::Lod), 3)] : 0.5f;
}

float FrameBudget::shadowTexelFactor() const {
    static constexpr float kFactor[] = {1.0f, 2.0f, 3.0f, 4.0f};
    return enabled_ ? kFactor[std::min<std::uint8_t>(level(Lever::ShadowDetail), 3)] : 1.0f;
}

float FrameBudget::shadowMinTexels() const {
    static constexpr float kMin[] = {0.5f, 1.0f, 2.0f, 3.0f};
    return enabled_ ? kMin[std::min<std::uint8_t>(level(Lever::ShadowDetail), 3)] : 0.5f;
}

std::uint32_t FrameBudget::farCascadeInterval() const {
    static constexpr std::uint32_t kInterval[] = {1, 2, 3, 4};
    return enabled_ ? kInterval[std::min<std::uint8_t>(level(Lever::ShadowDetail), 3)] : 1u;
}

}  // namespace cramion::gfx
