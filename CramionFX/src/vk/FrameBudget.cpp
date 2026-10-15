#include "CramionFX/vk/FrameBudget.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace cramion::gfx {
namespace {

constexpr std::array<std::uint8_t, kLeverCount> kMaxLevel = {
    3,  // Lod
    3,  // ShadowDetail
    3,  // Volumetric: 16 tramos sin rayos de luz, 8 tramos, sin luces locales
    1,  // ContactShadows
    1,  // Lighting
    1,  // Ssao
    1,  // Reflections
    1,  // Gi
    5,  // RenderScale
    2,  // CameraFx: la mitad de muestras, sin profundidad de campo ni motion blur
    2,  // Clouds: 64 pasos, 32 pasos
};

constexpr std::array<float, 6> kRenderScales = {1.0f, 0.87f, 0.77f, 0.67f, 0.58f, 0.5f};

// Tiron: una muestra por encima de max(3 x media, media + 20 ms). Si dura mas
// de esto (tiempo y muestras) ya no es un tiron: es la nueva normalidad.
constexpr float kSpikeSeconds = 0.75f;
constexpr std::uint32_t kSpikeSamples = 3;
// Calentamiento: tras sembrar la media no se baja ni se sube nada hasta pasar
// este tiempo y estas muestras (el primer dato puede ser de un frame raro).
constexpr float kWarmupSeconds = 0.5f;
constexpr std::uint32_t kWarmupSamples = 4;
// Un cambio se mide con al menos estas muestras, ademas de settleSeconds: a
// 10 FPS medio segundo son 5 frames, y los 2 primeros aun son de antes.
constexpr std::uint32_t kSettleSamples = 6;
// Reinicio entero pedido por la escena (qualityRaised): como mucho cada...
constexpr float kResetInterval = 10.0f;
// Un paso de RenderScale como mucho cada... (rehace los destinos: waitIdle,
// samplers e historia del TAA).
constexpr float kRenderScaleRest = 3.0f;
// Por encima de 1.5 veces el objetivo durante esto, la resolucion puede bajar
// del suelo.
constexpr float kFloorLiftSeconds = 3.0f;

// Promedio exponencial con constante de tiempo `tau` segundos.
float smooth(float value, float target, float dt, float tau) {
    return value + (target - value) * (1.0f - std::exp(-dt / tau));
}

constexpr std::size_t leverIndex(Lever lever) { return static_cast<std::size_t>(lever); }

// Pasadas que van despues del escalado, a la resolucion de salida (no bajan
// con la resolucion interna).
bool outputPass(const std::string& n) {
    return n == "Motion blur + profundidad de campo" || n == "Bloom" || n == "Rayos de luz" || n == "FSR 1" ||
           n == "FSR 3" || n == "DLSS" || n == "TAA / escalado" || n == "Auto-exposicion" || n == "Composicion" ||
           n == "Video (planos de camara)" || n == "UI en el mundo" || n == "Contorno de seleccion" ||
           n == "Picking" || n == "Gizmos 3D" || n == "FXAA + presentacion";
}

// Pasadas de antes del escalado que tampoco dependen de la resolucion
// interna: el compute del principio (terreno, TLAS, hierba, oceano, liquidos;
// antes se cobraba a las sombras), mapas de tamano fijo y subidas.
bool fixedPass(const std::string& n) {
    return n == "Preparacion (compute)" || n == "Oclusion del cielo" || n == "Cielo + IBL" || n == "Mapa de lluvia" ||
           n == "Video (subidas)" || n == "UI en el mundo (texturas)";
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

// Cuatro cascadas D32: 3072 son 144 MB (y otros tantos de cache estatica si
// hay personajes animados); 4096, 256 MB. Antes Alto era 4096 y Ultra 6144
// (576 MB): con las cascadas bien ajustadas no se distingue de 4096 y era la
// mitad de la VRAM de una escena pequena. (Unreal usa 2048 por cascada.)
std::uint32_t shadowResolutionFor(HardwareTier tier) {
    switch (tier) {
        case HardwareTier::Low: return 1536;
        case HardwareTier::Medium: return 2048;
        case HardwareTier::High: return 3072;
        case HardwareTier::Ultra: return 4096;
    }
    return 3072;
}

// Lado maximo de las texturas: 8K solo en Ultra (una de 8K son 43 MB aun en
// BC1); sin tope, una de 16K se comia la VRAM de una escena entera.
std::uint32_t textureSizeFor(HardwareTier tier) {
    switch (tier) {
        case HardwareTier::Low: return 2048;
        case HardwareTier::Medium: return 4096;
        case HardwareTier::High: return 4096;
        case HardwareTier::Ultra: return 8192;
    }
    return 4096;
}

// Anisotropico: en una GPU de escritorio 16x apenas cuesta (lo hace el
// hardware de texturas); en las integradas y las de poca memoria se queda
// en 4x, que ya quita casi todo el borroso de los suelos en angulo.
std::uint32_t anisotropyFor(HardwareTier tier) {
    switch (tier) {
        case HardwareTier::Low: return 4;
        case HardwareTier::Medium: return 8;
        case HardwareTier::High: return 16;
        case HardwareTier::Ultra: return 16;
    }
    return 8;
}

// Objetos diminutos: con 0.5 px en todos los PC apenas se ocultaba nada (en un
// mundo grande, 16-35 de 3.600 objetos). En una GPU floja lo de menos de 3
// pixeles de ancho no se distingue y cada objeto cuesta lo mismo de preparar.
float tinyObjectPixelsFor(HardwareTier tier) {
    switch (tier) {
        case HardwareTier::Low: return 1.5f;
        case HardwareTier::Medium: return 1.0f;
        case HardwareTier::High: return 0.75f;
        case HardwareTier::Ultra: return 0.5f;
    }
    return 0.75f;
}

GraphicsSettings shippedGraphics(const GraphicsSettings& project, HardwareTier tier, float display_hz, bool locked) {
    GraphicsSettings g = project;
    if (locked) return g;
    g.adaptive = true;
    // Los Hz de la pantalla como mucho (mas FPS no se ven) y 144 como mucho
    // (240 lo dejaba todo al minimo en cualquier PC); nunca menos de 30.
    const float hz = std::isfinite(display_hz) && display_hz > 1.0f ? display_hz : 60.0f;
    const float top = std::clamp(hz, 30.0f, 144.0f);
    g.target_fps = std::clamp(std::isfinite(g.target_fps) ? g.target_fps : 60.0f, 30.0f, top);
    // Lo que el proyecto fija pasa a ser tope del de la gama (0 = segun el PC).
    const auto cap = [](std::int32_t value, std::uint32_t limit) {
        if (value <= 0 || limit == 0) return value;
        return std::min(value, static_cast<std::int32_t>(limit));
    };
    g.shadow_resolution = cap(g.shadow_resolution, shadowResolutionFor(tier));
    g.texture_max_size = cap(g.texture_max_size, textureSizeFor(tier));
    g.texture_filtering = cap(g.texture_filtering, anisotropyFor(tier));
    if ((tier == HardwareTier::Low || tier == HardwareTier::Medium) && !(g.texture_lod_bias >= 0.0f)) {
        g.texture_lod_bias = 0.0f;
    }
    return g;
}

int renderTextureIntervalFor(HardwareTier tier) {
    switch (tier) {
        case HardwareTier::Low: return 3;
        case HardwareTier::Medium: return 2;
        case HardwareTier::High: return 1;
        case HardwareTier::Ultra: return 1;
    }
    return 1;
}

std::array<std::uint8_t, kLeverCount> startLevelsFor(HardwareTier tier) {
    std::array<std::uint8_t, kLeverCount> levels{};
    const auto set = [&](Lever lever, std::uint8_t level) { levels[leverIndex(lever)] = level; };
    if (tier == HardwareTier::Low) {
        // Una GPU integrada o de gama baja empieza ya ligera: si empezaba a
        // tope tardaba muchos segundos en bajar (y se jugaba a tirones). Lo
        // que sobre lo vuelve a subir el gobernador. Tambien lo mas caro que
        // antes no se tocaba: SSAO, reflejos, desenfoques de camara y nubes.
        set(Lever::Lod, 1);
        set(Lever::ShadowDetail, 2);
        set(Lever::Volumetric, 1);
        set(Lever::ContactShadows, 1);
        set(Lever::Lighting, 1);
        set(Lever::Ssao, 1);
        set(Lever::Reflections, 1);
        set(Lever::RenderScale, 3);  // 67 % (0 si el preset ya escala: renderScaleStartLevel)
        set(Lever::CameraFx, 2);
        set(Lever::Clouds, 1);
    } else if (tier == HardwareTier::Medium) {
        set(Lever::Lod, 1);
        set(Lever::ShadowDetail, 1);
        set(Lever::Volumetric, 1);
        set(Lever::CameraFx, 1);
    }
    return levels;
}

std::uint8_t renderScaleStartLevel(HardwareTier tier, float preset_scale) {
    if (preset_scale < 0.9f) return 0;
    return startLevelsFor(tier)[leverIndex(Lever::RenderScale)];
}

float renderScaleFloor(std::uint32_t output_height, float preset_scale) {
    if (output_height == 0 || !(preset_scale > 0.0f)) return 0.0f;
    const float height = static_cast<float>(output_height);
    // Por debajo de 540 lineas (o de la mitad en pantallas grandes) la
    // imagen escalada se ve borrosa; en una salida mas baja, la propia salida.
    const float min_height = std::min(height, std::max(540.0f, 0.5f * height));
    return std::clamp(min_height / (height * preset_scale), 0.0f, 1.0f);
}

FrameBudget::FrameBudget() {
    for (auto& row : learned_) row.fill(-1.0f);
    caps_ = kMaxLevel;
}

void FrameBudget::clampToCaps() {
    for (std::size_t i = 0; i < kLeverCount; ++i) levels_[i] = std::min(levels_[i], caps_[i]);
}

void FrameBudget::setLevelCap(Lever lever, std::uint8_t cap) {
    const auto i = leverIndex(lever);
    const std::uint8_t clamped = std::min(cap, kMaxLevel[i]);
    if (caps_[i] == clamped) return;
    caps_[i] = clamped;
    if (pending_.active && pending_.lever == lever && pending_.to > clamped) pending_.active = false;
    // Al quitar el tope se vuelve a medir desde el punto de partida (lo que
    // se bajo con el tope puesto no vale sin el).
    if (enabled_ && levels_[i] > clamped) levels_[i] = clamped;
    else if (enabled_ && clamped == kMaxLevel[i]) levels_[i] = std::min(start_levels_[i], clamped);
}

void FrameBudget::setEnabled(bool enabled) {
    if (enabled == enabled_) return;
    enabled_ = enabled;
    // Apagado: todo a la calidad del usuario. Encendido: desde el perfil.
    levels_ = enabled ? start_levels_ : std::array<std::uint8_t, kLeverCount>{};
    clampToCaps();
    pending_.active = false;
    over_seconds_ = under_seconds_ = 0.0f;
}

void FrameBudget::setTargetFps(float fps) {
    target_fps_ = std::clamp(fps, 15.0f, 360.0f);
}

void FrameBudget::setStartLevels(HardwareTier tier) {
    start_levels_ = startLevelsFor(tier);
    if (enabled_) levels_ = start_levels_;
    clampToCaps();
}

void FrameBudget::setStartLevel(Lever lever, std::uint8_t level) {
    const auto i = leverIndex(lever);
    start_levels_[i] = std::min(level, kMaxLevel[i]);
    if (enabled_) levels_[i] = std::min(start_levels_[i], caps_[i]);
}

void FrameBudget::reset() {
    const std::uint8_t scale_before = levels_[leverIndex(Lever::RenderScale)];
    levels_ = enabled_ ? start_levels_ : std::array<std::uint8_t, kLeverCount>{};
    clampToCaps();
    pending_.active = false;
    over_seconds_ = under_seconds_ = 0.0f;
    has_sample_ = false;
    spike_seconds_ = 0.0f;
    spike_samples_ = 0;
    far_over_seconds_ = 0.0f;
    floor_lifted_ = false;
    for (auto& row : learned_) row.fill(-1.0f);
    raise_blocked_until_ = {};
    raise_backoff_ = {};
    raised_at_ = {};
    if (levels_[leverIndex(Lever::RenderScale)] != scale_before) render_scale_changed_at_ = clock_;
    reset_at_ = clock_;
    last_action_ = "Ajustes cambiados: calidad restablecida";
}

void FrameBudget::restoreLevers(std::uint32_t mask) {
    for (std::size_t i = 0; i < kLeverCount; ++i) {
        if ((mask & (1u << i)) == 0) continue;
        const std::uint8_t start = enabled_ ? std::min(start_levels_[i], caps_[i]) : 0;
        if (levels_[i] > start) {
            if (static_cast<Lever>(i) == Lever::RenderScale) render_scale_changed_at_ = clock_;
            levels_[i] = start;
        }
        learned_[i].fill(-1.0f);
        raise_blocked_until_[i] = 0.0f;
        raise_backoff_[i] = 0.0f;
        raised_at_[i] = 0.0f;
        if (pending_.active && leverIndex(pending_.lever) == i) pending_.active = false;
    }
}

void FrameBudget::qualityRaised(std::uint32_t levers) {
    if (levers == 0) return;
    if (clock_ - reset_at_ >= kResetInterval) reset();
    else restoreLevers(levers);
}

std::uint8_t FrameBudget::maxLevel(Lever lever) const {
    return caps_[leverIndex(lever)];
}

const char* FrameBudget::leverName(Lever lever) {
    switch (lever) {
        case Lever::Lod: return "Detalle de mallas (LOD)";
        case Lever::ShadowDetail: return "Detalle de sombras";
        case Lever::Volumetric: return "Luz volumetrica";
        case Lever::ContactShadows: return "Sombras de contacto";
        case Lever::Lighting: return "Iluminacion ligera";
        case Lever::Ssao: return "Oclusion ambiental";
        case Lever::Reflections: return "Reflejos";
        case Lever::Gi: return "Luz rebotada (GI)";
        case Lever::RenderScale: return "Resolucion interna";
        case Lever::CameraFx: return "Desenfoque de cámara";
        case Lever::Clouds: return "Nubes volumétricas";
        case Lever::Count: break;
    }
    return "?";
}

float FrameBudget::learnedSaving(Lever lever, std::uint8_t level) const {
    return level < kMaxLevels ? learned_[leverIndex(lever)][level] : -1.0f;
}

void FrameBudget::setRenderScaleFloor(float factor) {
    render_scale_floor_ = std::isfinite(factor) ? std::clamp(factor, 0.0f, 1.0f) : 0.0f;
}

float FrameBudget::scaleAt(std::uint8_t level) const {
    const float scale = kRenderScales[std::min<std::size_t>(level, kRenderScales.size() - 1)];
    return floor_lifted_ ? scale : std::max(scale, render_scale_floor_);
}

bool FrameBudget::renderScaleStepUseful(std::uint8_t level) const {
    return scaleAt(static_cast<std::uint8_t>(level + 1)) < scaleAt(level) - 1e-4f;
}

bool FrameBudget::renderScaleRested() const {
    return clock_ - render_scale_changed_at_ >= kRenderScaleRest;
}

// Cuanto se espera ahorrar al bajar un paso desde `level`, con el coste
// medido de las pasadas que toca la palanca.
float FrameBudget::estimateSaving(Lever lever, std::uint8_t level) const {
    const PassCosts& p = passes_;
    switch (lever) {
        case Lever::Lod: return 0.35f * (p.geometry + 0.5f * p.shadows);
        case Lever::ShadowDetail: return 0.35f * (p.shadows + p.local_shadows);
        case Lever::Volumetric:
            if (level == 0) return 0.95f * p.shafts + 0.45f * p.volumetric;  // 32 -> 16 tramos, sin rayos de luz
            if (level == 1) return 0.45f * p.volumetric;                     // 16 -> 8 tramos
            // Sin luces locales: con muchas luces era casi todo el coste (si
            // no hay ninguna, lo aprendido queda en 0 y no se vuelve a probar).
            return 0.5f * p.volumetric;
        case Lever::ContactShadows: return 0.25f * p.lighting;
        // Las sombras suaves del sol (28 lecturas del mapa por pixel) y la
        // oclusion del cielo (12) son lo mas caro de la iluminacion. Y sin
        // leerla ya no se dibuja el mapa del cielo visto desde arriba.
        case Lever::Lighting: return 0.35f * p.lighting + p.sky_map;
        case Lever::Ssao: return 0.95f * p.ssao;
        case Lever::Reflections: return 0.9f * p.reflections;
        case Lever::Gi: return 0.95f * p.gi;
        case Lever::RenderScale: {
            // Lo que depende de los pixeles baja con el area de la imagen:
            // todo menos las sombras, lo que va despues del escalado
            // (desenfoques, bloom, composicion: a la resolucion de salida) y
            // lo que no depende de la resolucion (compute, mapas fijos).
            const float pixel_ms = std::max(p.total - p.shadows - p.local_shadows - p.output - p.fixed, 0.0f);
            const float now = scaleAt(level);
            const float next = scaleAt(static_cast<std::uint8_t>(level + 1));
            return now > 0.0f ? pixel_ms * (1.0f - (next * next) / (now * now)) : 0.0f;
        }
        // La mitad de muestras; despues, sin el pase.
        case Lever::CameraFx: return (level == 0 ? 0.5f : 0.9f) * p.camera_fx;
        // 128 -> 64 pasos (6 -> 4 hacia el sol); 64 -> 32 (4 -> 2) y la sombra
        // cada 4 frames. El rayo acaba antes donde la nube ya tapa todo, asi
        // que no baja en proporcion a los pasos.
        case Lever::Clouds: return (level == 0 ? 0.5f : 0.6f) * p.clouds;
        case Lever::Count: break;
    }
    return 0.0f;
}

// Cuanto se nota bajar un paso desde `level` (mas alto = se nota mas).
float FrameBudget::penalty(Lever lever, std::uint8_t level) const {
    switch (lever) {
        case Lever::Lod: return 1.0f + 0.75f * level;
        case Lever::ShadowDetail: return 1.0f + 0.75f * level;
        case Lever::Volumetric: return 2.0f + 0.5f * level;  // 16 tramos, 8 tramos, sin el halo de las lamparas
        case Lever::ContactShadows: return 1.5f;
        case Lever::Lighting: return 1.6f;
        case Lever::Ssao: return 3.0f;
        case Lever::Reflections: return 3.0f;
        case Lever::Gi: return 4.0f;
        case Lever::RenderScale: return 2.0f + 0.6f * level;
        // Bokeh con algo mas de ruido (el TAA lo suaviza); despues sin
        // desenfoques, que se nota mas, pero menos que quitar la luz rebotada.
        case Lever::CameraFx: return level == 0 ? 1.8f : 3.0f;
        case Lever::Clouds: return level == 0 ? 1.5f : 2.5f;
        case Lever::Count: break;
    }
    return 1.0f;
}

// Lo que tarda un cambio en verse en el tiempo de GPU (los timestamps van un
// par de frames tarde, la resolucion rehace los destinos y el historial).
float FrameBudget::settleSeconds(Lever lever) const {
    // Muy por encima del presupuesto (una GPU de gama baja recien arrancada)
    // se mide mas deprisa: cada medio segundo a 10 FPS se nota.
    const bool far_over = has_sample_ && ema_ms_ > targetMilliseconds() * 1.5f;
    if (lever == Lever::RenderScale) return far_over ? 0.8f : 1.5f;
    return far_over ? 0.25f : 0.5f;
}

void FrameBudget::change(Lever lever, std::uint8_t to, const char* verb, float predicted) {
    const auto i = leverIndex(lever);
    pending_ = Pending{true, lever, levels_[i], to, ema_ms_, 0.0f, 0};
    levels_[i] = to;
    if (lever == Lever::RenderScale) render_scale_changed_at_ = clock_;
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
        if (level >= caps_[i]) continue;
        // La resolucion: un paso cada 3 s como mucho, y no si el suelo lo tapa.
        if (lever == Lever::RenderScale && (!renderScaleRested() || !renderScaleStepUseful(level))) continue;
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
        if (static_cast<Lever>(i) == Lever::RenderScale &&
            (!renderScaleRested() || !renderScaleStepUseful(static_cast<std::uint8_t>(level - 1)))) {
            continue;
        }
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

void FrameBudget::seed(float gpu_ms) {
    ema_ms_ = gpu_ms;
    has_sample_ = true;
    seeded_at_ = clock_;
    samples_since_seed_ = 1;
    spike_seconds_ = 0.0f;
    spike_samples_ = 0;
    over_seconds_ = under_seconds_ = 0.0f;
    far_over_seconds_ = 0.0f;
}

void FrameBudget::update(float dt, float gpu_ms, const std::vector<GpuTiming>& passes) {
    if (dt <= 0.0f) return;
    clock_ += dt;
    // Sin medida no cuenta. Antes, en el 2.o frame (el perfilador aun no
    // tenia datos) se sembraba la media con 0 y el filtro de tirones tiraba
    // despues todo lo de 8 ms o mas: en una GPU lenta la media se quedaba en
    // 0, el gobernador creia que sobraba tiempo y lo subia todo ("media 0").
    if (!(gpu_ms > 0.0f) || !std::isfinite(gpu_ms)) return;

    // Coste de cada pasada que toca alguna palanca.
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
        else if (n == "Nubes") now.clouds += ms;
        else if (n == "Motion blur + profundidad de campo") now.camera_fx += ms;
        else if (n == "Oclusion del cielo") now.sky_map += ms;
        if (outputPass(n)) now.output += ms;
        if (fixedPass(n)) now.fixed += ms;
    }
    now.total = gpu_ms;
    // Sin tiempos por pasada (GPU sin timestamps: solo el del frame), un
    // reparto tipico del frame para poder elegir palanca; lo que ahorra cada
    // una de verdad se aprende al probarla.
    if (passes.empty()) {
        now.geometry = 0.25f * gpu_ms;
        now.shadows = 0.15f * gpu_ms;
        now.local_shadows = 0.04f * gpu_ms;
        now.lighting = 0.2f * gpu_ms;
        now.ssao = 0.07f * gpu_ms;
        now.reflections = 0.04f * gpu_ms;
        now.gi = 0.04f * gpu_ms;
        now.volumetric = 0.03f * gpu_ms;
        now.shafts = 0.02f * gpu_ms;
    }

    // Tiempo de GPU y de cada pasada, suavizados. Un tiron (una carga, un
    // frame de 200 ms) no cuenta: no es culpa de la calidad. Pero si dura
    // (kSpikeSeconds y kSpikeSamples) es la nueva normalidad (otra zona,
    // otra camara) y la media se vuelve a sembrar con el. Antes el filtro era
    // fijo (4 x media + 8 ms) y no se rendia nunca.
    bool accepted = true;
    if (!has_sample_) {
        seed(gpu_ms);
        passes_ = now;
    } else if (gpu_ms > std::max(ema_ms_ * 3.0f, ema_ms_ + 20.0f)) {
        accepted = false;
        spike_seconds_ += dt;
        ++spike_samples_;
        if (spike_seconds_ >= kSpikeSeconds && spike_samples_ >= kSpikeSamples) {
            seed(gpu_ms);
            passes_ = now;
            // Lo de antes del salto no se puede comparar con lo de despues.
            pending_.active = false;
        }
    } else {
        spike_seconds_ = 0.0f;
        spike_samples_ = 0;
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
        blend(passes_.camera_fx, now.camera_fx);
        blend(passes_.clouds, now.clouds);
        blend(passes_.output, now.output);
        blend(passes_.fixed, now.fixed);
        blend(passes_.sky_map, now.sky_map);
        ema_ms_ = smooth(ema_ms_, gpu_ms, dt, 0.25f);
        ++samples_since_seed_;
    }

    if (!enabled_ || !accepted) return;
    // Calentamiento tras sembrar: el primer dato puede ser de un frame raro.
    if (samples_since_seed_ < kWarmupSamples || clock_ - seeded_at_ < kWarmupSeconds) return;

    const float target = targetMilliseconds();
    // Suelo de la resolucion interna: solo se levanta si la GPU no llega ni
    // de lejos durante segundos; vuelve cuando la resolucion sube a el.
    far_over_seconds_ = ema_ms_ > target * 1.5f ? far_over_seconds_ + dt : 0.0f;
    if (far_over_seconds_ > kFloorLiftSeconds) {
        floor_lifted_ = true;
    } else if (floor_lifted_ && far_over_seconds_ <= 0.0f &&
               kRenderScales[std::min<std::size_t>(level(Lever::RenderScale), kRenderScales.size() - 1)] >=
                   render_scale_floor_ - 1e-4f) {
        floor_lifted_ = false;
    }

    // Un cambio en observacion: al asentarse se apunta lo que ahorro de
    // verdad (o lo que costo al subir). Ese dato manda en adelante.
    if (pending_.active) {
        pending_.timer += dt;
        ++pending_.samples;
        if (pending_.timer < settleSeconds(pending_.lever) || pending_.samples < kSettleSamples) return;
        const auto i = leverIndex(pending_.lever);
        const std::uint8_t step = std::min(pending_.from, pending_.to);
        const float delta = pending_.to > pending_.from ? pending_.before_ms - ema_ms_ : ema_ms_ - pending_.before_ms;
        learned_[i][step] = std::max(delta, 0.0f);
        pending_.active = false;
    }

    over_seconds_ = ema_ms_ > target ? over_seconds_ + dt : 0.0f;
    under_seconds_ = ema_ms_ < target * 0.8f ? under_seconds_ + dt : 0.0f;

    // Muy por encima: se reacciona antes (sin esperar 0.3 s cada vez).
    const float react = ema_ms_ > target * 1.5f ? 0.1f : 0.3f;
    if (over_seconds_ > react) {
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
    // El polvo no se apaga (el usuario lo ve desaparecer y volver segun a
    // donde mire): se dibuja con la mitad de tramos y sin los rayos en
    // pantalla, despues con 8 tramos y al final sin las luces locales (solo
    // el sol y el cielo: las farolas pierden el halo).
    const std::uint8_t volumetric = level(Lever::Volumetric);
    if (volumetric >= 1) {
        s.volumetric_steps = std::min(s.volumetric_steps, 16);
        s.light_shafts = false;
    }
    if (volumetric >= 2) s.volumetric_steps = std::min(s.volumetric_steps, 8);
    if (volumetric >= 3) s.volumetric_local_lights = false;
    if (level(Lever::ContactShadows) > 0) s.contact_shadows = false;
    if (level(Lever::Ssao) > 0) s.ambient_occlusion = false;
    if (level(Lever::Reflections) > 0) s.reflections = false;
    if (level(Lever::Gi) > 0) s.global_illumination = false;
    // Profundidad de campo y motion blur: la mitad de muestras (algo mas de
    // ruido en el bokeh, que el TAA suaviza) y despues sin ellos.
    const std::uint8_t camera_fx = level(Lever::CameraFx);
    if (camera_fx >= 1) {
        s.dof_samples = std::min(s.dof_samples, 32);
        s.motion_blur_samples = std::min(s.motion_blur_samples, 12);
    }
    if (camera_fx >= 2) {
        s.depth_of_field = false;
        s.motion_blur = false;
    }
    // Nubes: menos pasos por rayo y, en el ultimo nivel, su sombra cada 4 frames.
    const std::uint8_t clouds = level(Lever::Clouds);
    if (clouds >= 1) s.cloud_steps = std::min(s.cloud_steps, 64);
    if (clouds >= 2) {
        s.cloud_steps = std::min(s.cloud_steps, 32);
        s.cloud_shadow_interval = std::max(s.cloud_shadow_interval, 4);
    }
    return s;
}

float FrameBudget::renderScale() const {
    return enabled_ ? scaleAt(level(Lever::RenderScale)) : 1.0f;
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
