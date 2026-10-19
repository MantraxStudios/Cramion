// Lightmapper (PLAN-LIGHTMAPS.md 2.2): el horneado entero (bakeLighting) y lo
// que no es raster ni luz: UV2 de los receptores (A), escala y presupuesto
// (B), paginas (C), geometria del trazador (D), salida (I) y sondas (J).

#include "CramionCore/asset/AssetManager.h"
#include "CramionCore/lighting/BakeLights.h"
#include "LightmapperInternal.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace cramion::lighting {

using core::Vec2;
using core::Vec3;
using core::Vec4;

// --- Utilidades -------------------------------------------------------------------

LightmapBakeSettings lightmapPreset(LightmapPreset preset) {
    LightmapBakeSettings s;
    switch (preset) {
        case LightmapPreset::Preview:
            s.texels_per_meter = 8.0f;
            s.sun_samples = 16;
            s.indirect_samples = 32;
            s.bounces = 1;
            s.direct_samples = 8;
            break;
        case LightmapPreset::Medium:
            s.texels_per_meter = 20.0f;
            s.sun_samples = 64;
            s.indirect_samples = 128;
            s.bounces = 2;
            break;
        case LightmapPreset::High:
            s.texels_per_meter = 40.0f;
            s.sun_samples = 128;
            s.indirect_samples = 512;
            s.bounces = 3;
            s.direct_samples = 32;
            s.chart_iterations = 2;
            s.max_page_size = 4096;
            break;
    }
    return s;
}

std::uint16_t floatToHalf(float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const std::uint32_t sign = (bits >> 16) & 0x8000u;
    const std::int32_t exponent = static_cast<std::int32_t>((bits >> 23) & 0xFFu) - 127 + 15;
    std::uint32_t mantissa = bits & 0x7FFFFFu;
    if (((bits >> 23) & 0xFFu) == 0xFFu) {  // inf / nan
        return static_cast<std::uint16_t>(sign | 0x7C00u | (mantissa != 0 ? 0x200u : 0u));
    }
    if (exponent >= 31) return static_cast<std::uint16_t>(sign | 0x7BFFu);  // el mayor finito (65504)
    if (exponent <= 0) {
        if (exponent < -10) return static_cast<std::uint16_t>(sign);
        mantissa |= 0x800000u;
        const std::uint32_t shift = static_cast<std::uint32_t>(14 - exponent);
        std::uint32_t half = mantissa >> shift;
        const std::uint32_t rest = mantissa & ((1u << shift) - 1u);
        const std::uint32_t halfway = 1u << (shift - 1u);
        if (rest > halfway || (rest == halfway && (half & 1u))) ++half;
        return static_cast<std::uint16_t>(sign | half);
    }
    std::uint32_t half = (static_cast<std::uint32_t>(exponent) << 10) | (mantissa >> 13);
    const std::uint32_t rest = mantissa & 0x1FFFu;
    if (rest > 0x1000u || (rest == 0x1000u && (half & 1u))) ++half;  // redondeo al par
    if (half >= 0x7C00u) half = 0x7BFFu;
    return static_cast<std::uint16_t>(sign | half);
}

float halfToFloat(std::uint16_t h) {
    const std::uint32_t sign = (static_cast<std::uint32_t>(h) & 0x8000u) << 16;
    const std::uint32_t exponent = (h >> 10) & 0x1Fu;
    std::uint32_t mantissa = h & 0x3FFu;
    std::uint32_t bits = 0;
    if (exponent == 0) {
        if (mantissa == 0) {
            bits = sign;
        } else {
            int e = -1;
            do {
                ++e;
                mantissa <<= 1;
            } while ((mantissa & 0x400u) == 0);
            bits = sign | (static_cast<std::uint32_t>(127 - 15 - e) << 23) | ((mantissa & 0x3FFu) << 13);
        }
    } else if (exponent == 31) {
        bits = sign | 0x7F800000u | (mantissa << 13);
    } else {
        bits = sign | ((exponent + 127 - 15) << 23) | (mantissa << 13);
    }
    float out = 0.0f;
    std::memcpy(&out, &bits, sizeof(out));
    return out;
}

Vec4 sampleLightmapPage(const gfx::LightmapPage& page, float u, float v) {
    if (page.width == 0 || page.height == 0 || page.texels.size() < static_cast<std::size_t>(page.width) * page.height * 4) {
        return Vec4{};
    }
    const float px = u * static_cast<float>(page.width) - 0.5f;
    const float py = v * static_cast<float>(page.height) - 0.5f;
    const int x0 = static_cast<int>(std::floor(px));
    const int y0 = static_cast<int>(std::floor(py));
    const float fx = px - static_cast<float>(x0);
    const float fy = py - static_cast<float>(y0);
    const auto at = [&](int x, int y) {
        x = std::clamp(x, 0, static_cast<int>(page.width) - 1);
        y = std::clamp(y, 0, static_cast<int>(page.height) - 1);
        const std::size_t i = (static_cast<std::size_t>(y) * page.width + static_cast<std::size_t>(x)) * 4;
        return Vec4{halfToFloat(page.texels[i]), halfToFloat(page.texels[i + 1]), halfToFloat(page.texels[i + 2]),
                    halfToFloat(page.texels[i + 3])};
    };
    const auto mix = [](const Vec4& a, const Vec4& b, float t) {
        return Vec4{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t};
    };
    return mix(mix(at(x0, y0), at(x0 + 1, y0), fx), mix(at(x0, y0 + 1), at(x0 + 1, y0 + 1), fx), fy);
}

// --- Paquete de paginas (skyline bottom-left) ---------------------------------------

namespace {

std::uint32_t nextPow2(std::uint32_t v, std::uint32_t minimum) {
    std::uint32_t p = minimum;
    while (p < v) p <<= 1;
    return p;
}

struct Skyline {
    struct Segment {
        std::uint32_t x, y, width;
    };
    std::vector<Segment> segments;
    std::uint32_t size = 0;
    std::uint32_t used_w = 0, used_h = 0;

    explicit Skyline(std::uint32_t page_size) : size(page_size) { segments.push_back(Segment{0, 0, page_size}); }

    // Lo mas abajo (y luego a la izquierda) donde cabe w x h.
    bool find(std::uint32_t w, std::uint32_t h, std::uint32_t& out_x, std::uint32_t& out_y) const {
        bool found = false;
        std::uint32_t best_y = 0, best_x = 0;
        for (std::size_t i = 0; i < segments.size(); ++i) {
            const std::uint32_t x = segments[i].x;
            if (x + w > size) break;
            std::uint32_t y = 0;
            std::uint32_t covered = 0;
            for (std::size_t j = i; j < segments.size() && covered < w; ++j) {
                y = std::max(y, segments[j].y);
                covered += segments[j].width;
            }
            if (covered < w || y + h > size) continue;
            if (!found || y < best_y || (y == best_y && x < best_x)) {
                found = true;
                best_y = y;
                best_x = x;
            }
        }
        out_x = best_x;
        out_y = best_y;
        return found;
    }

    void place(std::uint32_t x, std::uint32_t y, std::uint32_t w, std::uint32_t h) {
        std::vector<Segment> next;
        next.reserve(segments.size() + 2);
        const std::uint32_t end = x + w;
        bool inserted = false;
        for (const Segment& s : segments) {
            const std::uint32_t s_end = s.x + s.width;
            if (s_end <= x || s.x >= end) {
                if (!inserted && s.x >= end) {
                    next.push_back(Segment{x, y + h, w});
                    inserted = true;
                }
                next.push_back(s);
                continue;
            }
            // Se solapa: quedan los trozos de fuera de [x, end).
            if (s.x < x) next.push_back(Segment{s.x, s.y, x - s.x});
            if (!inserted) {
                next.push_back(Segment{x, y + h, w});
                inserted = true;
            }
            if (s_end > end) next.push_back(Segment{end, s.y, s_end - end});
        }
        if (!inserted) next.push_back(Segment{x, y + h, w});
        // Juntar los vecinos a la misma altura.
        segments.clear();
        for (const Segment& s : next) {
            if (!segments.empty() && segments.back().y == s.y && segments.back().x + segments.back().width == s.x) {
                segments.back().width += s.width;
            } else {
                segments.push_back(s);
            }
        }
        used_w = std::max(used_w, end);
        used_h = std::max(used_h, y + h);
    }
};

}  // namespace

LightmapPacking packLightmapRects(std::vector<LightmapRect>& rects, std::uint32_t page_size, std::uint32_t gap) {
    LightmapPacking packing;
    page_size = std::max(page_size, 64u);
    std::vector<std::uint32_t> order(rects.size());
    for (std::uint32_t i = 0; i < order.size(); ++i) order[i] = i;
    // Por alto descendente (y ancho; el indice desempata: determinista).
    std::stable_sort(order.begin(), order.end(), [&](std::uint32_t a, std::uint32_t b) {
        if (rects[a].height != rects[b].height) return rects[a].height > rects[b].height;
        return rects[a].width > rects[b].width;
    });
    std::vector<Skyline> pages;
    for (const std::uint32_t i : order) {
        LightmapRect& r = rects[i];
        r.placed_width = std::max(r.width, 1u);
        r.placed_height = std::max(r.height, 1u);
        r.scaled = false;
        const std::uint32_t limit = page_size - gap;
        if (r.placed_width > limit || r.placed_height > limit) {
            // No cabe en una pagina: se escala (aviso del horneado).
            const float f = static_cast<float>(limit) / static_cast<float>(std::max(r.placed_width, r.placed_height));
            r.placed_width = std::max(1u, static_cast<std::uint32_t>(static_cast<float>(r.placed_width) * f));
            r.placed_height = std::max(1u, static_cast<std::uint32_t>(static_cast<float>(r.placed_height) * f));
            r.scaled = true;
        }
        const std::uint32_t w = r.placed_width + gap;
        const std::uint32_t h = r.placed_height + gap;
        bool placed = false;
        for (std::uint32_t p = 0; p < pages.size() && !placed; ++p) {
            std::uint32_t x = 0, y = 0;
            if (pages[p].find(w, h, x, y)) {
                pages[p].place(x, y, w, h);
                r.page = p;
                r.x = x + gap / 2;
                r.y = y + gap / 2;
                placed = true;
            }
        }
        if (!placed) {
            pages.emplace_back(page_size);
            std::uint32_t x = 0, y = 0;
            pages.back().find(w, h, x, y);
            pages.back().place(x, y, w, h);
            r.page = static_cast<std::uint32_t>(pages.size() - 1);
            r.x = x + gap / 2;
            r.y = y + gap / 2;
        }
    }
    packing.pages = static_cast<std::uint32_t>(pages.size());
    for (const Skyline& s : pages) {
        packing.page_width.push_back(std::min(nextPow2(s.used_w, 256u), std::max(page_size, 256u)));
        packing.page_height.push_back(std::min(nextPow2(s.used_h, 256u), std::max(page_size, 256u)));
        packing.width = std::max(packing.width, packing.page_width.back());
        packing.height = std::max(packing.height, packing.page_height.back());
    }
    return packing;
}

Vec4 lightmapScaleOffset(const LightmapRect& rect, const LightmapPacking& packing) {
    const float aw = static_cast<float>(std::max(packing.width, 1u));
    const float ah = static_cast<float>(std::max(packing.height, 1u));
    return Vec4{static_cast<float>(rect.placed_width) / aw, static_cast<float>(rect.placed_height) / ah,
                static_cast<float>(rect.x) / aw, static_cast<float>(rect.y) / ah};
}

namespace lm {

void parallelFor(const Context& ctx, std::size_t count, const std::function<void(std::size_t)>& fn) {
    if (count == 0) return;
    std::atomic<std::size_t> next{0};
    const auto worker = [&]() {
#ifdef _WIN32
        // El editor sigue respondiendo mientras se hornea (minutos).
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
        for (;;) {
            if (ctx.cancelled()) return;
            const std::size_t i = next.fetch_add(1);
            if (i >= count) return;
            fn(i);
        }
    };
    const int threads = static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(std::max(ctx.threads, 1)), count));
    if (threads <= 1) {
        worker();
        return;
    }
    std::vector<std::thread> pool;
    pool.reserve(static_cast<std::size_t>(threads));
    for (int t = 0; t < threads; ++t) pool.emplace_back(worker);
    for (std::thread& t : pool) t.join();
}

std::uint32_t raySeed(std::uint32_t page, std::uint32_t texel, std::uint32_t sample, std::uint32_t pass) {
    return hashCombine(hashCombine(hashCombine(hash32(page * 0x632BE5ABu + 1u), texel), sample), pass);
}

void r2Sample(std::uint32_t j, float rot_x, float rot_y, float& r1, float& r2) {
    // Roberts R2: a1 = 1/g, a2 = 1/g^2 con g la constante plastica.
    constexpr double kA1 = 0.7548776662466927;
    constexpr double kA2 = 0.5698402909980532;
    const double x = 0.5 + kA1 * static_cast<double>(j) + static_cast<double>(rot_x);
    const double y = 0.5 + kA2 * static_cast<double>(j) + static_cast<double>(rot_y);
    r1 = static_cast<float>(x - std::floor(x));
    r2 = static_cast<float>(y - std::floor(y));
}

void orthonormalBasis(const Vec3& n, Vec3& t, Vec3& b) {
    const Vec3 helper = std::fabs(n.y) < 0.99f ? Vec3{0.0f, 1.0f, 0.0f} : Vec3{1.0f, 0.0f, 0.0f};
    t = core::normalize(core::cross(helper, n));
    b = core::cross(n, t);
}

namespace {

Vec3 transformPoint(const core::Mat4& m, const Vec3& p) {
    const Vec4 r = m * Vec4{p.x, p.y, p.z, 1.0f};
    return Vec3{r.x, r.y, r.z};
}

double seconds(std::chrono::steady_clock::time_point since) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - since).count();
}

// A + B: la geometria de cada (modelo, pieza), sus UV2 (cache o xatlas) y la
// escala de cada instancia. Devuelve false si se cancela o falla.
bool prepareMeshes(Context& ctx, std::vector<std::pair<LightmapUvPartKey, std::shared_ptr<const LightmapUvLayout>>>& layouts,
                   std::string* error) {
    const LightmapBakeInput& in = *ctx.in;
    LightmapBakeStats& st = *ctx.stats;
    const auto start = std::chrono::steady_clock::now();
    ctx.stage("Lightmaps: leyendo la geometría de los receptores", 0.02f);

    // Una malla por (modelo, pieza): la geometria cruda (tal como sale del
    // .crdata: el hash de la UV2 es de esa malla).
    std::unordered_map<LightmapUvPartKey, std::uint32_t, LightmapUvPartKeyHash> mesh_of;
    std::vector<std::shared_ptr<const asset::ModelData>> raw;
    std::vector<std::uint32_t> receiver_mesh(in.receivers.size(), kNone);
    {
        std::map<Uuid, std::vector<std::uint32_t>> by_model;  // receptores por modelo (orden fijo)
        for (std::uint32_t r = 0; r < in.receivers.size(); ++r) {
            const LightmapReceiver& rec = in.receivers[r];
            if (rec.lightmap_scale <= 0.0f) continue;
            by_model[rec.model].push_back(r);
        }
        for (const auto& [model, list] : by_model) {
            if (ctx.cancelled()) return false;
            std::shared_ptr<assets::ModelAsset> asset;
            bool from_input = false;
            for (const std::uint32_t r : list) {
                if (r < in.receiver_meshes.size() && in.receiver_meshes[r]) from_input = true;
            }
            if (!from_input) {
                std::vector<bool> wanted;
                for (const std::uint32_t r : list) {
                    const auto part = static_cast<std::size_t>(std::max(in.receivers[r].part, 0));
                    if (wanted.size() <= part) wanted.resize(part + 1, false);
                    wanted[part] = true;
                }
                const LightmapReceiver& first = in.receivers[list.front()];
                asset = assets::AssetManager::readModelGeometry(model, first.file, first.name, wanted);
            }
            for (const std::uint32_t r : list) {
                const LightmapReceiver& rec = in.receivers[r];
                const LightmapUvPartKey key{model, rec.part};
                const auto found = mesh_of.find(key);
                if (found != mesh_of.end()) {
                    receiver_mesh[r] = found->second;
                    continue;
                }
                std::shared_ptr<const asset::ModelData> data;
                if (r < in.receiver_meshes.size() && in.receiver_meshes[r]) {
                    data = in.receiver_meshes[r];
                } else if (asset && rec.part >= 0 && static_cast<std::size_t>(rec.part) < asset->parts.size()) {
                    data = asset->parts[static_cast<std::size_t>(rec.part)];
                }
                if (!data || data->indices.size() < 3 || data->vertices.empty()) {
                    st.warnings.push_back(rec.name + ": no se pudo leer su malla (lo iluminan las sondas)");
                    continue;
                }
                const auto index = static_cast<std::uint32_t>(ctx.meshes.size());
                mesh_of.emplace(key, index);
                receiver_mesh[r] = index;
                lm::Mesh mesh;
                mesh.key = key;
                mesh.name = rec.name;
                mesh.area = meshSurfaceArea(*data);
                ctx.meshes.push_back(std::move(mesh));
                raw.push_back(data);
            }
        }
    }
    st.seconds_geometry = seconds(start);

    // --- B: escala por instancia y densidad efectiva ---
    double weighted_area = 0.0;  // sum(area_mundo * escala^2)
    for (std::uint32_t r = 0; r < in.receivers.size(); ++r) {
        const std::uint32_t m = receiver_mesh[r];
        if (m == kNone) {
            ++st.probe_only;
            continue;
        }
        const LightmapReceiver& rec = in.receivers[r];
        const asset::ModelData& data = *raw[m];
        if (data.bones.size() > 1 || !data.animations.empty()) {
            // Con esqueleto o animaciones: se deforma, va con sondas.
            ++st.probe_only;
            continue;
        }
        // Como la dibuja RenderSync: la entidad por el reposo de la pieza.
        const core::Mat4 world = rec.world * restPoseMatrix(data);
        const std::vector<asset::SkinnedVertex>& vertices = data.vertices.get();
        const std::vector<std::uint32_t>& indices = data.indices.get();
        double area = 0.0;
        for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
            if (indices[i] >= vertices.size() || indices[i + 1] >= vertices.size() || indices[i + 2] >= vertices.size()) continue;
            const Vec3 a = transformPoint(world, vertices[indices[i]].position);
            const Vec3 b = transformPoint(world, vertices[indices[i + 1]].position);
            const Vec3 c = transformPoint(world, vertices[indices[i + 2]].position);
            area += 0.5 * static_cast<double>(core::length(core::cross(b - a, c - a)));
        }
        const float model_area = ctx.meshes[m].area;
        if (area <= 1e-12 || model_area <= 1e-12f) {
            ++st.probe_only;
            continue;
        }
        Instance inst;
        inst.receiver = r;
        inst.mesh = m;
        inst.world = world;
        inst.area_world = static_cast<float>(area);
        inst.scale = std::sqrt(static_cast<float>(area) / model_area) * rec.lightmap_scale;
        ctx.meshes[m].max_scale = std::max(ctx.meshes[m].max_scale, inst.scale);
        weighted_area += area * static_cast<double>(rec.lightmap_scale) * static_cast<double>(rec.lightmap_scale);
        ctx.instances.push_back(inst);
    }
    st.receivers = ctx.instances.size();
    float density = std::max(ctx.settings.texels_per_meter, 0.1f);
    st.texels_per_meter = density;
    if (weighted_area > 0.0) {
        const double fit = std::sqrt(0.6 * static_cast<double>(ctx.settings.texel_budget) / weighted_area);
        if (fit < static_cast<double>(density)) {
            char text[200];
            std::snprintf(text, sizeof(text),
                          "Presupuesto de %.1f M texeles superado: densidad bajada de %.1f a %.1f texeles/m en toda la escena",
                          static_cast<double>(ctx.settings.texel_budget) / 1048576.0, static_cast<double>(density), fit);
            st.warnings.push_back(text);
            density = static_cast<float>(fit);
        }
    }
    st.effective_texels_per_meter = density;

    // --- A: UV2 de cada malla (cache o xatlas; las enormes de una en una) ---
    const auto uv_start = std::chrono::steady_clock::now();
    std::vector<std::size_t> pending;
    std::mutex mutex;
    std::size_t done = 0;
    std::string failure;
    for (std::size_t m = 0; m < ctx.meshes.size(); ++m) pending.push_back(m);
    // Las grandes primero (el progreso no se queda parado al final).
    std::stable_sort(pending.begin(), pending.end(),
                     [&](std::size_t a, std::size_t b) { return raw[a]->indices.size() > raw[b]->indices.size(); });
    const auto unwrap_one = [&](std::size_t m) {
        lm::Mesh& mesh = ctx.meshes[m];
        if (mesh.max_scale <= 0.0f) return;  // sin instancias (todas con sondas)
        const asset::ModelData& data = *raw[m];
        LightmapUvSettings s;
        s.texels_per_unit = std::max(density * std::max(mesh.max_scale, 1e-4f), 1e-3f);
        s.padding = ctx.settings.padding;
        s.min_component_texels = ctx.settings.min_component_texels;
        s.max_size = ctx.settings.max_page_size;
        s.chart_iterations = ctx.settings.chart_iterations;
        const std::uint64_t source_hash = lightmapMeshHash(data);
        auto layout = std::make_shared<LightmapUvLayout>();
        bool hit = false;
        const std::filesystem::path cache_file =
            in.uv_cache.empty() ? std::filesystem::path{} : lightmapUvCacheFile(in.uv_cache, source_hash, s.hash());
        if (!cache_file.empty()) {
            std::error_code ec;
            hit = std::filesystem::exists(cache_file, ec) && loadLightmapUvLayout(cache_file, *layout) &&
                  layout->source_hash == source_hash && layout->settings_hash == s.hash();
        }
        bool ok = hit;
        std::string unwrap_error;
#if CRAMION_HAS_LIGHTMAP_UNWRAP
        if (!hit) {
            const std::function<bool(float)> on_progress = [&](float) { return !ctx.cancelled(); };
            ok = unwrapLightmapUvs(data, s, *layout, on_progress, &unwrap_error);
            if (ok && !cache_file.empty()) saveLightmapUvLayout(cache_file, *layout);
        }
#else
        if (!hit) unwrap_error = "este binario no genera UV de lightmap";
#endif
        std::lock_guard lock(mutex);
        ++done;
        if (hit) ++st.layout_cache_hits;
        if (!ok) {
            if (failure.empty() && unwrap_error != "cancelado") failure = mesh.name + ": " + unwrap_error;
            return;
        }
        mesh.layout = layout;
        if (ctx.progress) {
            ctx.progress->fraction.store(0.1f + 0.3f * static_cast<float>(done) / static_cast<float>(std::max<std::size_t>(ctx.meshes.size(), 1)));
        }
    };
    ctx.stage("Lightmaps: UV de lightmap de " + std::to_string(ctx.meshes.size()) + " mallas", 0.1f);
    {
        // Como mucho dos mallas a la vez (la RAM de xatlas) y las de mas de
        // medio millon de caras solas.
        std::size_t first_small = 0;
        while (first_small < pending.size() && raw[pending[first_small]]->indices.size() / 3 > 500000) {
            if (ctx.cancelled()) return false;
            unwrap_one(pending[first_small]);
            ++first_small;
        }
        std::atomic<std::size_t> next{first_small};
        const auto worker = [&]() {
#ifdef _WIN32
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
            for (;;) {
                if (ctx.cancelled()) return;
                const std::size_t i = next.fetch_add(1);
                if (i >= pending.size()) return;
                unwrap_one(pending[i]);
            }
        };
        std::thread second(worker);
        worker();
        second.join();
    }
    if (ctx.cancelled()) {
        if (error) *error = "cancelado";
        return false;
    }
    if (!failure.empty()) {
        if (error) *error = "UV de lightmap: " + failure;
        return false;
    }
    st.seconds_uv = seconds(uv_start);

    // Aplicar cada disposicion a su copia cruda (partida) y quedarse solo con
    // posicion, normal, UV2 e indices (se suelta el vertice de 80 B).
    for (std::size_t m = 0; m < ctx.meshes.size(); ++m) {
        lm::Mesh& mesh = ctx.meshes[m];
        if (mesh.max_scale <= 0.0f) {
            raw[m].reset();
            continue;
        }
        asset::ModelData copy = *raw[m];
        if (!mesh.layout || !applyLightmapUvs(copy, *mesh.layout)) {
            if (error) *error = mesh.name + ": la disposicion de UV2 no cuadra con la malla";
            return false;
        }
        const std::vector<asset::SkinnedVertex>& vertices = copy.vertices.get();
        mesh.positions.resize(vertices.size());
        mesh.normals.resize(vertices.size());
        for (std::size_t i = 0; i < vertices.size(); ++i) {
            mesh.positions[i] = vertices[i].position;
            mesh.normals[i] = vertices[i].normal;
        }
        mesh.uv = copy.lightmap_uvs;
        mesh.indices = copy.indices.get();
        mesh.tri_material.assign(mesh.indices.size() / 3, 0);
        for (const asset::SubMesh& sm : copy.submeshes) {
            for (std::uint32_t i = sm.first_index; i + 2 < sm.first_index + sm.index_count && i + 2 < mesh.indices.size(); i += 3) {
                mesh.tri_material[i / 3] = sm.material;
            }
        }
        layouts.emplace_back(mesh.key, mesh.layout);
        raw[m].reset();
    }
    st.layouts = layouts.size();
    return true;
}

// C: rectangulo de cada instancia en las paginas.
void packInstances(Context& ctx) {
    std::vector<LightmapRect> rects(ctx.instances.size());
    for (std::size_t i = 0; i < ctx.instances.size(); ++i) {
        const Instance& inst = ctx.instances[i];
        const lm::Mesh& mesh = ctx.meshes[inst.mesh];
        const float ratio = mesh.max_scale > 0.0f ? inst.scale / mesh.max_scale : 1.0f;
        rects[i].width = std::max(1u, static_cast<std::uint32_t>(std::ceil(static_cast<float>(mesh.layout->width) * ratio)));
        rects[i].height = std::max(1u, static_cast<std::uint32_t>(std::ceil(static_cast<float>(mesh.layout->height) * ratio)));
    }
    ctx.packing = packLightmapRects(rects, ctx.settings.max_page_size, 2);
    for (std::size_t i = 0; i < ctx.instances.size(); ++i) {
        Instance& inst = ctx.instances[i];
        const LightmapRect& r = rects[i];
        inst.page = r.page;
        inst.x = r.x;
        inst.y = r.y;
        inst.w = r.placed_width;
        inst.h = r.placed_height;
        inst.scale_offset = lightmapScaleOffset(r, ctx.packing);
        const std::string& name = ctx.in->receivers[inst.receiver].name;
        if (r.scaled) ctx.stats->warnings.push_back(name + " no cabe en una página: su lightmap va más pequeño");
        const lm::Mesh& mesh = ctx.meshes[inst.mesh];
        if (mesh.max_scale > 0.0f && inst.scale / mesh.max_scale < 0.5f) {
            ctx.stats->warnings.push_back(name + ": menos de la mitad de resolución que otra copia de su malla (puede "
                                                 "sangrar entre cartas; sube su Escala en lightmap)");
        }
    }
    ctx.pages.assign(ctx.packing.pages, Page{});
    for (std::uint32_t p = 0; p < ctx.packing.pages; ++p) {
        Page& page = ctx.pages[p];
        page.width = ctx.packing.page_width[p];
        page.height = ctx.packing.page_height[p];
        const std::size_t pixels = static_cast<std::size_t>(page.width) * page.height;
        page.texel_of.assign(pixels, kNone);
        page.value.assign(pixels, Vec4{0.0f, 0.0f, 0.0f, 1.0f});
        page.state.assign(pixels, 0);
    }
}

// D: los triangulos del trazador: los de la escena que no son de un receptor
// y los de los receptores (de su copia partida, en el mundo).
void buildTracer(Context& ctx, const std::vector<std::uint8_t>& scene_skip) {
    const BakeScene& scene = ctx.in->scene;
    std::vector<Vec3> v0, e1, e2;
    std::vector<std::uint32_t> material;
    std::size_t total = 0;
    for (std::size_t t = 0; t < scene.triangles.size(); ++t) total += scene_skip[t] ? 0u : 1u;
    for (const Instance& inst : ctx.instances) total += ctx.meshes[inst.mesh].indices.size() / 3;
    v0.reserve(total);
    e1.reserve(total);
    e2.reserve(total);
    material.reserve(total);
    ctx.tracer_instance.clear();
    ctx.tracer_local.clear();
    ctx.tracer_material.clear();
    ctx.tracer_albedo.clear();
    ctx.tracer_emission.clear();
    const auto push = [&](const Vec3& a, const Vec3& b, const Vec3& c, std::uint32_t mat, std::uint32_t inst, std::uint32_t local,
                          const Vec3& albedo, const Vec3& emission) {
        v0.push_back(a);
        e1.push_back(b - a);
        e2.push_back(c - a);
        material.push_back(mat);
        ctx.tracer_instance.push_back(inst);
        ctx.tracer_local.push_back(local);
        ctx.tracer_material.push_back(mat);
        ctx.tracer_albedo.push_back(albedo);
        ctx.tracer_emission.push_back(emission);
    };
    for (std::size_t t = 0; t < scene.triangles.size(); ++t) {
        if (scene_skip[t]) continue;
        const BakeTriangle& tri = scene.triangles[t];
        push(tri.a, tri.b, tri.c, tri.material, kNone, static_cast<std::uint32_t>(t), tri.albedo, tri.emission);
    }
    for (std::uint32_t i = 0; i < ctx.instances.size(); ++i) {
        Instance& inst = ctx.instances[i];
        const lm::Mesh& mesh = ctx.meshes[inst.mesh];
        const LightmapReceiver& rec = ctx.in->receivers[inst.receiver];
        inst.tracer_first = static_cast<std::uint32_t>(v0.size());
        inst.tracer_of_triangle.assign(mesh.indices.size() / 3, kNone);
        for (std::size_t t = 0; t * 3 + 2 < mesh.indices.size(); ++t) {
            const std::uint32_t part_material = mesh.tri_material[t];
            const std::uint32_t slot = part_material < rec.materials.size() ? rec.materials[part_material] : kNoBakeMaterial;
            const bool transparent = part_material < rec.transparent.size() && rec.transparent[part_material] != 0;
            if (transparent) continue;  // vidrio, agua: dejan pasar la luz (y no llevan lightmap)
            const Vec3 a = transformPoint(inst.world, mesh.positions[mesh.indices[t * 3]]);
            const Vec3 b = transformPoint(inst.world, mesh.positions[mesh.indices[t * 3 + 1]]);
            const Vec3 c = transformPoint(inst.world, mesh.positions[mesh.indices[t * 3 + 2]]);
            Vec3 albedo{0.5f, 0.5f, 0.5f};
            Vec3 emission{};
            if (slot < scene.materials.size()) {
                albedo = scene.materials[slot].albedo;
                emission = scene.materials[slot].emission;
            }
            inst.tracer_of_triangle[t] = static_cast<std::uint32_t>(v0.size());
            push(a, b, c, slot, i, static_cast<std::uint32_t>(t), albedo, emission);
        }
    }
    std::vector<float> coverage(scene.materials.size(), 1.0f);
    for (std::size_t m = 0; m < scene.materials.size(); ++m) coverage[m] = scene.materials[m].coverage;
    ctx.tracer.build(std::move(v0), std::move(e1), std::move(e2), std::move(material), std::move(coverage), ctx.progress);
}

// I: paginas RGBA16F, registros y limites.
void writeOutput(Context& ctx, gfx::BakedLighting& out) {
    gfx::LightmapSet& set = out.lightmaps;
    set.width = ctx.packing.width;
    set.height = ctx.packing.height;
    set.texels_per_meter = ctx.stats->effective_texels_per_meter;
    set.sun_visibility = ctx.sun_active;
    set.has_irradiance = ctx.irradiance;
    set.pages.resize(ctx.pages.size());
    std::size_t valid = 0, invalid = 0, total = 0, dilated = 0;
    for (std::size_t p = 0; p < ctx.pages.size(); ++p) {
        const Page& page = ctx.pages[p];
        gfx::LightmapPage& o = set.pages[p];
        o.width = page.width;
        o.height = page.height;
        o.texels.resize(static_cast<std::size_t>(page.width) * page.height * 4);
        for (std::size_t i = 0; i < page.value.size(); ++i) {
            const Vec4& v = page.value[i];
            o.texels[i * 4 + 0] = floatToHalf(std::max(v.x, 0.0f));
            o.texels[i * 4 + 1] = floatToHalf(std::max(v.y, 0.0f));
            o.texels[i * 4 + 2] = floatToHalf(std::max(v.z, 0.0f));
            o.texels[i * 4 + 3] = floatToHalf(std::clamp(v.w, 0.0f, 1.0f));
            switch (page.state[i]) {
                case 1: ++valid; break;
                case 2: ++invalid; break;
                case 3: ++dilated; break;
                default: break;
            }
        }
        total += page.value.size();
    }
    // Registros y limites de lo horneado.
    Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    for (const Instance& inst : ctx.instances) {
        const LightmapReceiver& rec = ctx.in->receivers[inst.receiver];
        const lm::Mesh& mesh = ctx.meshes[inst.mesh];
        gfx::LightmapRecord r;
        r.entity = {rec.entity.high, rec.entity.low};
        r.model = {rec.model.high, rec.model.low};
        r.part = rec.part;
        r.page = inst.page;
        r.scale_offset = inst.scale_offset;
        r.layout_hash = mesh.layout ? mesh.layout->layout_hash : 0;
        // 3x4 por columnas, tal como esta la escena (relativa a world_origin,
        // que va aparte en el .crbake).
        for (int c = 0; c < 4; ++c) {
            for (int row = 0; row < 3; ++row) r.world[static_cast<std::size_t>(c * 3 + row)] = rec.world.m[c][row];
        }
        set.records.push_back(r);
        for (const Vec3& p : mesh.positions) {
            const Vec3 w = transformPoint(inst.world, p);
            lo = Vec3{std::min(lo.x, w.x), std::min(lo.y, w.y), std::min(lo.z, w.z)};
            hi = Vec3{std::max(hi.x, w.x), std::max(hi.y, w.y), std::max(hi.z, w.z)};
        }
    }
    if (!ctx.instances.empty()) {
        set.bounds_min = lo;
        set.bounds_max = hi;
    }
    LightmapBakeStats& st = *ctx.stats;
    st.pages = set.pages.size();
    st.texels = total;
    st.valid_texels = valid;
    st.invalid_texels = invalid;
    st.dilated_texels = dilated;
}

}  // namespace
}  // namespace lm

bool bakeLighting(const LightmapBakeInput& in, gfx::BakedLighting& out,
                  std::vector<std::pair<LightmapUvPartKey, std::shared_ptr<const LightmapUvLayout>>>& layouts,
                  BakeProgress* progress, LightmapBakeStats* stats, std::string* error) {
    using namespace lm;
    const auto start = std::chrono::steady_clock::now();
    LightmapBakeStats local_stats;
    LightmapBakeStats& st = stats != nullptr ? *stats : local_stats;
    st = LightmapBakeStats{};
    layouts.clear();
    out = gfx::BakedLighting{};
    out.version = 4;
    out.sun_in_probes = false;
    out.world_origin = in.world_origin;
    out.scene_uuid = {in.scene_uuid.high, in.scene_uuid.low};

    Context ctx;
    ctx.in = &in;
    ctx.settings = in.lightmaps;
    ctx.progress = progress;
    ctx.stats = &st;
    const unsigned hardware = std::max(1u, std::thread::hardware_concurrency());
    ctx.threads = in.lightmaps.threads > 0 ? in.lightmaps.threads : static_cast<int>(std::max(1u, hardware > 2 ? hardware - 2 : 1u));
    const scene::DirectionalLight& sun = in.scene.lights.sun;
    if (core::length(sun.direction) > 1e-6f) ctx.to_sun = core::normalize(sun.direction) * -1.0f;
    ctx.sun_radiance = srgbToLinear(sun.color) * std::max(sun.intensity, 0.0f);
    ctx.sun_active = sun.intensity > 0.0f && core::length(sun.direction) > 1e-6f;

    const bool lightmaps = in.lightmaps.enabled && !in.receivers.empty();
    if (lightmaps) {
        if (!prepareMeshes(ctx, layouts, error)) return false;
        if (!ctx.instances.empty()) {
            ctx.stage("Lightmaps: páginas", 0.4f);
            packInstances(ctx);
            // D
            const auto bvh_start = std::chrono::steady_clock::now();
            std::vector<std::uint8_t> skip(in.scene.triangles.size(), 0);
            for (const Instance& inst : ctx.instances) {
                const LightmapReceiver& rec = in.receivers[inst.receiver];
                const std::size_t end = std::min<std::size_t>(static_cast<std::size_t>(rec.scene_first) + rec.scene_count,
                                                              skip.size());
                for (std::size_t t = rec.scene_first; t < end; ++t) skip[t] = 1;
            }
            ctx.stage("Lightmaps: construyendo el BVH", 0.42f);
            buildTracer(ctx, skip);
            st.seconds_bvh = std::chrono::duration<double>(std::chrono::steady_clock::now() - bvh_start).count();
            if (ctx.cancelled()) {
                if (error) *error = "cancelado";
                return false;
            }
            // E, F, G (+ Fase 3), H
            if (!rasterizeTexels(ctx) || !computeValidity(ctx) || !bakeSunVisibility(ctx) || !bakeIrradiance(ctx)) {
                if (error) *error = "cancelado";
                return false;
            }
            dilatePages(ctx, static_cast<int>(ctx.settings.padding) + 2);
            writeOutput(ctx, out);
        }
    }
    // Ya no hacen falta: fuera de la memoria antes de las sondas.
    ctx.pages.clear();
    ctx.tracer = BakeTracer{};

    // J: sondas con la funcion de siempre: sin la directa del sol y sin celdas.
    if (in.lightmaps.probes && !in.scene.volumes.empty()) {
        const auto probes_start = std::chrono::steady_clock::now();
        const BakeScene* probe_scene = &in.scene;
        std::unique_ptr<BakeScene> adjusted;
        if (!in.scene.settings.exclude_sun_direct || in.scene.settings.surface_texel > 0.0f ||
            in.scene.settings.auto_surface_texel > 0.0f) {
            adjusted = std::make_unique<BakeScene>(in.scene);
            adjusted->settings.exclude_sun_direct = true;
            adjusted->settings.surface_texel = 0.0f;
            adjusted->settings.auto_surface_texel = 0.0f;
            probe_scene = adjusted.get();
        }
        gfx::BakedLighting probes;
        std::string probe_error;
        if (!bakeProbes(*probe_scene, probes, progress, &st.probes, &probe_error)) {
            if (progress != nullptr && progress->cancel.load()) {
                if (error) *error = "cancelado";
                return false;
            }
            st.warnings.push_back("Sondas: " + probe_error);
        } else {
            out.volumes = std::move(probes.volumes);
            out.probes = std::move(probes.probes);
        }
        st.seconds_probes = std::chrono::duration<double>(std::chrono::steady_clock::now() - probes_start).count();
    }
    out.sun_baked = in.scene.sun_baked;
    out.sun_direction = sun.direction;
    out.baked_lights = in.scene.baked_light_ids;
    st.rays = static_cast<double>(ctx.rays.load());
    const double total = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    const double tracing = st.seconds_validity + st.seconds_sun + st.seconds_indirect;
    st.rays_per_second = tracing > 0.0 ? st.rays / tracing : 0.0;
    (void)total;
    if (progress) progress->fraction.store(1.0f);
    return true;
}

}  // namespace cramion::lighting
