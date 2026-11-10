// Luces reales de los bloques que dan luz: ver BlockLights.h.

#include "BlockLights.h"

#include "CramionCore/cvar/CVar.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>

namespace cramion::voxel {

namespace {

using core::Vec3;

cvar::CVar<bool> g_enabled("voxel.lights.Enabled", true,
                           "Luces reales de los bloques que dan luz (antorchas, faroles, farolas, hogueras, lamparas): "
                           "iluminan a personajes y objetos, no solo a los bloques");
cvar::CVar<int> g_max("voxel.lights.Max", 48,
                      "Luces de bloques como mucho a la vez (las mas cercanas y brillantes; las que entran y salen "
                      "se funden)",
                      cvar::None, 0, 256);
cvar::CVar<float> g_distance("voxel.lights.Distance", 48.0f, "Hasta donde se buscan luces de bloques (m)", cvar::None,
                             8.0f, 160.0f);
cvar::CVar<float> g_intensity("voxel.lights.Intensity", 1.0f, "Multiplicador del brillo de las luces de bloques",
                              cvar::None, 0.0f, 10.0f);
cvar::CVar<int> g_min_level("voxel.lights.MinLevel", 10,
                            "Luz minima (0..15) de un bloque para tener luz real (las debiles, como el musgo o el "
                            "magma, solo con su brillo)",
                            cvar::None, 1, 15);
cvar::CVar<bool> g_shadows("voxel.lights.Shadows", false,
                           "Las luces de bloques proyectan sombra (caro: usa los huecos de sombra de las luces locales)");

// Secciones nuevas que se exploran por frame como mucho (~3 us cada una).
constexpr int kScanBudget = 48;
// Bloques juntados en una luz: los del mismo tipo a 2 bloques o menos.
constexpr float kMergeDistance = 2.0f;
constexpr std::size_t kMaxPerSection = 48;
// Brillo base (intensidad de la luz puntual con luz 15).
constexpr float kBaseIntensity = 10.0f;

int floorDiv(int a, int b) { return (a >= 0) ? a / b : -((-a + b - 1) / b); }

// Como se ve la luz de cada tipo de bloque.
struct Kind {
    Vec3 color{1.0f, 0.85f, 0.6f};
    float strength = 1.0f;  // brillo relativo
    float flicker = 0.0f;   // 0 fija .. 1 llama
    float lift = 0.0f;      // altura de la llama sobre el centro del bloque (m)
};

Kind kindOf(BlockId id) {
    const Vec3 flame{1.0f, 0.58f, 0.26f};
    const Vec3 lamp{1.0f, 0.74f, 0.44f};
    switch (id) {
        case block::Torch: return {flame, 1.0f, 1.0f, 0.15f};
        case block::StandingTorch: return {flame, 1.05f, 1.0f, 0.35f};
        case block::Fire: return {Vec3{1.0f, 0.52f, 0.2f}, 1.15f, 1.0f, -0.1f};
        case block::Campfire: return {Vec3{1.0f, 0.52f, 0.2f}, 1.25f, 1.0f, -0.05f};
        case block::Brazier: return {flame, 1.1f, 1.0f, 0.4f};
        case block::SoulBrazier: return {Vec3{0.35f, 1.0f, 0.5f}, 1.0f, 1.0f, 0.4f};
        case block::Candles: return {Vec3{1.0f, 0.68f, 0.38f}, 0.6f, 0.8f, -0.1f};
        case block::Chandelier: return {Vec3{1.0f, 0.7f, 0.4f}, 1.1f, 0.6f, 0.0f};
        case block::Lantern: return {lamp, 1.0f, 0.5f, 0.0f};
        case block::StreetLamp: return {Vec3{1.0f, 0.8f, 0.55f}, 1.25f, 0.2f, 0.0f};
        case block::PaperLantern: return {Vec3{1.0f, 0.42f, 0.28f}, 0.9f, 0.5f, 0.0f};
        case block::Glowstone: return {Vec3{1.0f, 0.82f, 0.5f}, 1.0f, 0.0f, 0.0f};
        case block::CrystalLamp: return {Vec3{0.7f, 0.88f, 1.0f}, 1.0f, 0.0f, 0.0f};
        case block::LampRed: return {Vec3{1.0f, 0.22f, 0.18f}, 1.0f, 0.0f, 0.0f};
        case block::LampBlue: return {Vec3{0.25f, 0.45f, 1.0f}, 1.0f, 0.0f, 0.0f};
        case block::LampGreen: return {Vec3{0.3f, 1.0f, 0.4f}, 1.0f, 0.0f, 0.0f};
        case block::SignalLampLit: return {Vec3{0.5f, 1.0f, 0.9f}, 0.9f, 0.0f, 0.0f};  // senales (Rails.h)
        case block::CrystalPink: return {Vec3{1.0f, 0.45f, 0.8f}, 0.6f, 0.0f, 0.0f};
        case block::CrystalBlue: return {Vec3{0.35f, 0.6f, 1.0f}, 0.6f, 0.0f, 0.0f};
        case block::CrystalViolet: return {Vec3{0.7f, 0.4f, 1.0f}, 0.6f, 0.0f, 0.0f};
        case block::CrystalGreen: return {Vec3{0.4f, 1.0f, 0.6f}, 0.6f, 0.0f, 0.0f};
        case block::SkyCrystal: return {Vec3{0.6f, 0.85f, 1.0f}, 0.6f, 0.0f, 0.0f};
        case block::GlowMushroom: return {Vec3{0.45f, 0.75f, 1.0f}, 0.5f, 0.0f, 0.0f};
        default: return {};
    }
}

// Luz que da cada bloque para esta cache (0 = ninguna). La lava no: son mares
// enteros de celdas y llenarian todos los huecos (ya brilla sola).
const std::array<std::uint8_t, block::Count>& emitTable() {
    static const std::array<std::uint8_t, block::Count> table = [] {
        std::array<std::uint8_t, block::Count> t{};
        const std::vector<BlockDef>& defs = blocks();
        for (std::size_t i = 0; i < defs.size() && i < t.size(); ++i) {
            if (static_cast<BlockId>(i) == block::Lava) continue;
            t[i] = defs[i].light;
        }
        return t;
    }();
    return table;
}

}  // namespace

void BlockLightCache::scanSection(const ChunkData& data, int sy, std::vector<Source>& out) const {
    out.clear();
    const std::array<std::uint8_t, block::Count>& emits = emitTable();
    const int cells = kChunkSize * kChunkSize * kChunkSize;
    const std::size_t first = static_cast<std::size_t>(sy) * static_cast<std::size_t>(cells);
    if (first + static_cast<std::size_t>(cells) > data.blocks.size()) return;
    const BlockId* b = data.blocks.data() + first;
    for (int i = 0; i < cells; ++i) {
        const BlockId id = b[i];
        const std::uint8_t level = emits[id];
        if (level == 0) continue;
        // cellIndex: x + z * 16 + y * 256.
        const float x = static_cast<float>(data.cx * kChunkSize + (i & 15)) + 0.5f;
        const float z = static_cast<float>(data.cz * kChunkSize + ((i >> 4) & 15)) + 0.5f;
        const float y = static_cast<float>(sy * kChunkSize + (i >> 8)) + 0.5f;
        bool merged = false;
        for (Source& s : out) {
            if (s.id != id || std::abs(s.x - x) > kMergeDistance || std::abs(s.y - y) > kMergeDistance ||
                std::abs(s.z - z) > kMergeDistance || s.count == 255) {
                continue;
            }
            const float n = static_cast<float>(s.count);
            s.x = (s.x * n + x) / (n + 1.0f);
            s.y = (s.y * n + y) / (n + 1.0f);
            s.z = (s.z * n + z) / (n + 1.0f);
            ++s.count;
            merged = true;
            break;
        }
        if (!merged && out.size() < kMaxPerSection) out.push_back(Source{x, y, z, level, id, 1});
    }
}

void BlockLightCache::collect(const Vec3& eye, const Vec3& origin, const ColumnLookup& lookup, float seconds,
                              std::vector<scene::PointLight>& out) {
    const int max_lights = g_max.get();
    if (!g_enabled.get() || max_lights <= 0) {
        if (!entries_.empty()) entries_.clear();
        return;
    }
    ++frame_;
    // Columnas que hace mucho que no se miran (lejos o descargadas).
    if (frame_ % 120 == 0) {
        for (auto it = entries_.begin(); it != entries_.end();) {
            it = frame_ - it->second.last_frame > 600 ? entries_.erase(it) : std::next(it);
        }
    }

    const float radius = g_distance.get();
    const float fade = std::max(2.0f, std::min(6.0f, radius * 0.25f));
    const int min_level = g_min_level.get();
    const int ecx = floorDiv(static_cast<int>(std::floor(eye.x)), kChunkSize);
    const int ecz = floorDiv(static_cast<int>(std::floor(eye.z)), kChunkSize);
    const int reach = static_cast<int>(std::ceil(radius / static_cast<float>(kChunkSize)));

    // Columnas que tocan el circulo, las mas cercanas primero (se exploran antes).
    columns_.clear();
    for (int dz = -reach; dz <= reach; ++dz) {
        for (int dx = -reach; dx <= reach; ++dx) {
            const int cx = ecx + dx, cz = ecz + dz;
            const float x0 = static_cast<float>(cx * kChunkSize), z0 = static_cast<float>(cz * kChunkSize);
            const float nx = std::clamp(eye.x, x0, x0 + kChunkSize) - eye.x;
            const float nz = std::clamp(eye.z, z0, z0 + kChunkSize) - eye.z;
            const float d = std::sqrt(nx * nx + nz * nz);
            if (d > radius) continue;
            const std::int64_t key =
                (static_cast<std::int64_t>(cx) << 32) ^ static_cast<std::int64_t>(static_cast<std::uint32_t>(cz));
            columns_.emplace_back(d, key);
        }
    }
    std::sort(columns_.begin(), columns_.end());

    const int sy0 = std::clamp(static_cast<int>(std::floor((eye.y - radius) / kChunkSize)), 0, kSectionCount - 1);
    const int sy1 = std::clamp(static_cast<int>(std::floor((eye.y + radius) / kChunkSize)), 0, kSectionCount - 1);
    int budget = kScanBudget;
    candidates_.clear();
    for (const auto& [distance, key] : columns_) {
        const int cx = static_cast<int>(key >> 32);
        const int cz = static_cast<int>(static_cast<std::int32_t>(static_cast<std::uint32_t>(key & 0xFFFFFFFF)));
        Column column;
        if (!lookup(cx, cz, column) || column.data == nullptr) continue;
        Entry& e = entries_[key];
        if (e.serial != column.serial || e.edits != column.edits) {
            e.serial = column.serial;
            e.edits = column.edits;
            e.scanned = 0;
            for (std::vector<Source>& list : e.sections) list.clear();
        }
        e.last_frame = frame_;
        const int last = std::min(sy1, std::clamp(column.top, 0, kWorldHeight - 1) / kChunkSize);
        for (int s = sy0; s <= last; ++s) {
            const std::uint32_t bit = 1u << s;
            if ((e.scanned & bit) == 0) {
                if (budget <= 0) continue;  // el frame siguiente
                scanSection(*column.data, s, e.sections[static_cast<std::size_t>(s)]);
                e.scanned |= bit;
                --budget;
            }
            for (const Source& src : e.sections[static_cast<std::size_t>(s)]) {
                if (src.level < min_level) continue;
                const float dx = src.x - eye.x, dy = src.y - eye.y, dz = src.z - eye.z;
                const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (d >= radius) continue;
                // Las brillantes cuentan como mas cerca (una farola gana a un cristal).
                const float score = d * (1.6f - 0.6f * static_cast<float>(src.level) / 15.0f);
                candidates_.push_back(Candidate{score, d, &src});
            }
        }
    }
    if (candidates_.empty()) return;

    // Las N mejores; la primera que se queda fuera marca el corte: las que
    // estan cerca de el se funden (al moverse, la que entra y la que sale
    // cambian poco a poco, sin saltos).
    const std::size_t keep = static_cast<std::size_t>(max_lights);
    float cut = 1e30f;
    const auto by_score = [](const Candidate& a, const Candidate& b) { return a.score < b.score; };
    if (candidates_.size() > keep) {
        std::nth_element(candidates_.begin(), candidates_.begin() + static_cast<std::ptrdiff_t>(keep), candidates_.end(),
                         by_score);
        cut = candidates_[keep].score;
        candidates_.resize(keep);
    }
    std::sort(candidates_.begin(), candidates_.end(), by_score);  // las principales (GpuLights), primero

    const float gain = g_intensity.get();
    const bool shadows = g_shadows.get();
    for (const Candidate& c : candidates_) {
        const Source& src = *c.source;
        const float weight = std::clamp((cut - c.score) / fade, 0.0f, 1.0f) * std::clamp((radius - c.distance) / fade, 0.0f, 1.0f);
        if (weight <= 0.002f) continue;
        const Kind kind = kindOf(src.id);
        const float level = static_cast<float>(src.level) / 15.0f;
        const float group = 1.0f + 0.12f * static_cast<float>(std::min<int>(src.count, 6) - 1);
        Vec3 position{src.x - origin.x, src.y + kind.lift - origin.y, src.z - origin.z};
        float flicker = 1.0f;
        if (kind.flicker > 0.0f) {
            // Titileo: la llama se mueve unos centimetros (lo que mas se nota en
            // quien esta al lado) y el brillo cambia muy poco (las sondas de
            // reflejos se recapturan si las luces cambian mas de un 3 %).
            const float phase = std::fmod(src.x * 12.9898f + src.y * 78.233f + src.z * 37.719f, 6.2831853f);
            const float a = kind.flicker;
            position.x += 0.07f * a * std::sin(seconds * 5.1f + phase);
            position.y += 0.04f * a * std::sin(seconds * 6.7f + phase * 0.5f);
            position.z += 0.07f * a * std::cos(seconds * 4.3f + phase * 1.3f);
            flicker = 1.0f + 0.02f * a * (0.6f * std::sin(seconds * 7.3f + phase) + 0.4f * std::sin(seconds * 17.9f + phase * 1.7f));
        }
        scene::PointLight light;
        light.position = position;
        light.color = kind.color;
        light.intensity = kBaseIntensity * kind.strength * level * level * group * gain * weight * flicker;
        light.range = (3.0f + 9.0f * level) * (src.count > 1 ? 1.15f : 1.0f);
        light.cast_shadows = shadows;
        light.shadow_strength = 1.0f;
        light.source_radius = 0.15f;
        // Solo para personajes y objetos: los voxeles ya llevan esta luz
        // (propagada por celdas, que no atraviesa paredes). Sin sombra, en los
        // voxeles la puntual cruzaba muros y techos y sumaba la luz dos veces.
        light.skip_voxels = true;
        if (light.intensity > 0.0f) out.push_back(light);
    }
}

}  // namespace cramion::voxel
