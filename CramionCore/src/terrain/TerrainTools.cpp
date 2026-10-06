#include "CramionCore/terrain/TerrainTools.h"
#include "CramionCore/jobs/JobSystem.h"

#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

namespace cramion::terrain {

using core::Vec3;

namespace {

// Ruido de valor con interpolacion suave (determinista por semilla).
float hash2(int x, int y, std::uint32_t seed) {
    std::uint32_t h = static_cast<std::uint32_t>(x) * 374761393u + static_cast<std::uint32_t>(y) * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return static_cast<float>(h & 0xFFFFFF) / static_cast<float>(0xFFFFFF);
}

float valueNoise(float x, float y, std::uint32_t seed) {
    const int xi = static_cast<int>(std::floor(x));
    const int yi = static_cast<int>(std::floor(y));
    const float fx = x - static_cast<float>(xi);
    const float fy = y - static_cast<float>(yi);
    const float ux = fx * fx * (3.0f - 2.0f * fx);
    const float uy = fy * fy * (3.0f - 2.0f * fy);
    const float a = hash2(xi, yi, seed);
    const float b = hash2(xi + 1, yi, seed);
    const float c = hash2(xi, yi + 1, seed);
    const float d = hash2(xi + 1, yi + 1, seed);
    return (a + (b - a) * ux) + ((c + (d - c) * ux) - (a + (b - a) * ux)) * uy;
}

float fbm(float x, float y, std::uint32_t seed, int octaves, float roughness) {
    float sum = 0.0f;
    float amplitude = 1.0f;
    float norm = 0.0f;
    for (int i = 0; i < octaves; ++i) {
        sum += (valueNoise(x, y, seed + static_cast<std::uint32_t>(i) * 101u) * 2.0f - 1.0f) * amplitude;
        norm += amplitude;
        amplitude *= roughness;
        x *= 2.03f;
        y *= 2.03f;
    }
    return sum / norm;
}

// Texeles de alturas afectados por un circulo (mundo).
struct Span {
    int x0, y0, x1, y1;
    float cell;  // metros por texel
};

Span heightSpan(const TerrainData& data, const Terrain& terrain, const Vec3& origin, float cx, float cz, float radius) {
    const float n = static_cast<float>(data.resolution() - 1);
    const float cell = terrain.size / n;
    const float u = (cx - origin.x) / terrain.size * n;
    const float v = (cz - origin.z) / terrain.size * n;
    const float r = radius / cell + 1.0f;
    const int max = static_cast<int>(data.resolution()) - 1;
    return Span{std::clamp(static_cast<int>(std::floor(u - r)), 0, max), std::clamp(static_cast<int>(std::floor(v - r)), 0, max),
                std::clamp(static_cast<int>(std::ceil(u + r)), 0, max), std::clamp(static_cast<int>(std::ceil(v + r)), 0, max),
                cell};
}

Vec3 texelWorld(const TerrainData& data, const Terrain& terrain, const Vec3& origin, int x, int y) {
    const float n = static_cast<float>(data.resolution() - 1);
    return Vec3{origin.x + static_cast<float>(x) / n * terrain.size, 0.0f, origin.z + static_cast<float>(y) / n * terrain.size};
}

}  // namespace

const char* toolName(TerrainTool tool) {
    switch (tool) {
        case TerrainTool::Raise: return "Esculpir";
        case TerrainTool::Lower: return "Bajar";
        case TerrainTool::Smooth: return "Suavizar";
        case TerrainTool::Flatten: return "Aplanar";
        case TerrainTool::Ramp: return "Rampa";
        case TerrainTool::Noise: return "Ruido";
        case TerrainTool::Erosion: return "Erosion";
        case TerrainTool::Hydraulic: return "Erosion hidraulica";
        case TerrainTool::Terrace: return "Terrazas";
        case TerrainTool::Paint: return "Pintar";
    }
    return "?";
}

float brushWeight(const TerrainBrush& brush, float distance) {
    if (distance >= brush.radius) return 0.0f;
    const float inner = brush.radius * (1.0f - std::clamp(brush.falloff, 0.0f, 1.0f));
    if (distance <= inner) return 1.0f;
    const float t = (distance - inner) / std::max(brush.radius - inner, 1e-4f);
    // Caida suave (coseno).
    return 0.5f + 0.5f * std::cos(t * core::kPi);
}

bool applyBrush(TerrainData& data, const Terrain& terrain, const Vec3& origin, const Vec3& center,
                const TerrainBrush& brush, float dt, bool invert) {
    if (data.resolution() < 3 || brush.radius <= 0.0f) return false;
    const float max_height = std::max(terrain.height, 1e-3f);

    // --- Pintar capas ---
    if (brush.tool == TerrainTool::Paint) {
        const auto res = static_cast<int>(data.splatResolution());
        const float cell = terrain.size / static_cast<float>(res);
        const float cu = (center.x - origin.x) / cell - 0.5f;
        const float cv = (center.z - origin.z) / cell - 0.5f;
        const float r = brush.radius / cell + 1.0f;
        const int x0 = std::clamp(static_cast<int>(std::floor(cu - r)), 0, res - 1);
        const int y0 = std::clamp(static_cast<int>(std::floor(cv - r)), 0, res - 1);
        const int x1 = std::clamp(static_cast<int>(std::ceil(cu + r)), 0, res - 1);
        const int y1 = std::clamp(static_cast<int>(std::ceil(cv + r)), 0, res - 1);
        const float rate = std::clamp(brush.strength, 0.0f, 1.0f) * dt * 6.0f;
        bool changed = false;
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                const float d = std::hypot((static_cast<float>(x) - cu) * cell, (static_cast<float>(y) - cv) * cell);
                const float w = brushWeight(brush, d) * rate;
                if (w <= 0.0f) continue;
                // Pesos del texel (8 capas), sumando 1.
                float weights[kMaxLayers];
                float total = 0.0f;
                for (int l = 0; l < kMaxLayers; ++l) {
                    weights[l] = static_cast<float>(data.weight(l, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y))) / 255.0f;
                    total += weights[l];
                }
                if (total <= 0.0f) {
                    weights[0] = 1.0f;
                    total = 1.0f;
                }
                for (float& value : weights) value /= total;
                const int layer = std::clamp(brush.layer, 0, kMaxLayers - 1);
                if (invert) {
                    // Borrar: la capa pasa a la de abajo (la 0) poco a poco.
                    const float take = std::min(weights[layer], w);
                    weights[layer] -= take;
                    weights[layer == 0 ? 1 : 0] += take;
                } else {
                    for (int l = 0; l < kMaxLayers; ++l) {
                        weights[l] = l == layer ? weights[l] + (1.0f - weights[l]) * std::min(w, 1.0f)
                                                : weights[l] * (1.0f - std::min(w, 1.0f));
                    }
                }
                // A bytes conservando la suma 255 (el resto a la capa principal).
                int sum = 0;
                int biggest = 0;
                std::uint8_t bytes[kMaxLayers];
                for (int l = 0; l < kMaxLayers; ++l) {
                    bytes[l] = static_cast<std::uint8_t>(std::clamp(std::lround(weights[l] * 255.0f), 0L, 255L));
                    sum += bytes[l];
                    if (weights[l] > weights[biggest]) biggest = l;
                }
                bytes[biggest] = static_cast<std::uint8_t>(std::clamp(static_cast<int>(bytes[biggest]) + 255 - sum, 0, 255));
                for (int l = 0; l < kMaxLayers; ++l) {
                    std::uint8_t* p = data.weightPtr(l, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
                    if (p != nullptr && *p != bytes[l]) {
                        *p = bytes[l];
                        changed = true;
                    }
                }
            }
        }
        if (changed) data.markSplat(x0, y0, x1, y1);
        return changed;
    }

    // --- Alturas ---
    const Span span = heightSpan(data, terrain, origin, center.x, center.z, brush.radius);
    const auto res = static_cast<int>(data.resolution());
    std::vector<float>& h = data.heights();
    const auto at = [&](int x, int y) -> float& { return h[static_cast<std::size_t>(y) * res + x]; };
    const float strength = std::clamp(brush.strength, 0.0f, 1.0f);
    bool changed = false;

    if (brush.tool == TerrainTool::Erosion || brush.tool == TerrainTool::Smooth) {
        // Copia de la zona (vecinos del borde incluidos) para leer sin
        // mezclar lo ya cambiado en este toque.
        const int x0 = std::max(span.x0 - 1, 0);
        const int y0 = std::max(span.y0 - 1, 0);
        const int x1 = std::min(span.x1 + 1, res - 1);
        const int y1 = std::min(span.y1 + 1, res - 1);
        const int w = x1 - x0 + 1;
        std::vector<float> copy(static_cast<std::size_t>(w) * (y1 - y0 + 1));
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) copy[static_cast<std::size_t>(y - y0) * w + (x - x0)] = at(x, y);
        }
        const auto old = [&](int x, int y) {
            x = std::clamp(x, x0, x1);
            y = std::clamp(y, y0, y1);
            return copy[static_cast<std::size_t>(y - y0) * w + (x - x0)];
        };
        // Talud maximo de la erosion: ~35 grados, en unidades 0..1 por texel.
        const float talus = std::tan(core::radians(35.0f)) * span.cell / max_height;
        for (int y = span.y0; y <= span.y1; ++y) {
            for (int x = span.x0; x <= span.x1; ++x) {
                const Vec3 p = texelWorld(data, terrain, origin, x, y);
                const float weight = brushWeight(brush, std::hypot(p.x - center.x, p.z - center.z));
                if (weight <= 0.0f) continue;
                const float here = old(x, y);
                float target = here;
                if (brush.tool == TerrainTool::Smooth) {
                    float sum = 0.0f;
                    for (int dy = -1; dy <= 1; ++dy) {
                        for (int dx = -1; dx <= 1; ++dx) sum += old(x + dx, y + dy) * (dx == 0 && dy == 0 ? 4.0f : (dx == 0 || dy == 0 ? 2.0f : 1.0f));
                    }
                    target = sum / 16.0f;
                } else {
                    // Erosion termica: lo que sobra por encima del talud baja
                    // a los vecinos (aqui solo se quita/recibe la mitad).
                    float delta = 0.0f;
                    for (int dy = -1; dy <= 1; ++dy) {
                        for (int dx = -1; dx <= 1; ++dx) {
                            if (dx == 0 && dy == 0) continue;
                            const float diff = here - old(x + dx, y + dy);
                            const float limit = talus * ((dx != 0 && dy != 0) ? 1.414f : 1.0f);
                            if (diff > limit) delta -= (diff - limit) * 0.125f;
                            else if (-diff > limit) delta += (-diff - limit) * 0.125f;
                        }
                    }
                    target = here + delta;
                }
                const float t = std::min(1.0f, weight * strength * dt * 12.0f);
                const float value = here + (target - here) * t;
                if (value != at(x, y)) {
                    at(x, y) = std::clamp(value, 0.0f, 1.0f);
                    changed = true;
                }
            }
        }
    } else if (brush.tool == TerrainTool::Hydraulic) {
        // Gotas de agua: bajan por la pendiente arrastrando sedimento y lo
        // dejan donde frenan (Mei et al. / Lague, simplificado).
        std::mt19937 random(brush.seed + static_cast<std::uint32_t>(data.version()));
        std::uniform_real_distribution<float> unit(-1.0f, 1.0f);
        const int drops = static_cast<int>(40.0f + 400.0f * strength * dt * 30.0f);
        const float n = static_cast<float>(res - 1);
        const auto height_uv = [&](float px, float py) {
            const int xi = std::clamp(static_cast<int>(px), 0, res - 2);
            const int yi = std::clamp(static_cast<int>(py), 0, res - 2);
            const float fx = px - static_cast<float>(xi);
            const float fy = py - static_cast<float>(yi);
            const float a = at(xi, yi), b = at(xi + 1, yi), c = at(xi, yi + 1), d = at(xi + 1, yi + 1);
            return std::array<float, 3>{a * (1 - fx) * (1 - fy) + b * fx * (1 - fy) + c * (1 - fx) * fy + d * fx * fy,
                                        (b - a) * (1 - fy) + (d - c) * fy, (c - a) * (1 - fx) + (d - b) * fx};
        };
        const float cu = (center.x - origin.x) / terrain.size * n;
        const float cv = (center.z - origin.z) / terrain.size * n;
        const float radius_texels = brush.radius / span.cell;
        for (int drop = 0; drop < drops; ++drop) {
            float px = cu + unit(random) * radius_texels;
            float py = cv + unit(random) * radius_texels;
            float dx = 0.0f, dy = 0.0f, speed = 1.0f, water = 1.0f, sediment = 0.0f;
            for (int life = 0; life < 30; ++life) {
                if (px < 0.0f || py < 0.0f || px >= n - 1.0f || py >= n - 1.0f) break;
                const auto [height, gx, gy] = height_uv(px, py);
                dx = dx * 0.1f - gx * 0.9f;
                dy = dy * 0.1f - gy * 0.9f;
                const float len = std::hypot(dx, dy);
                if (len < 1e-6f) break;
                dx /= len;
                dy /= len;
                const float nx = px + dx;
                const float ny = py + dy;
                if (nx < 0.0f || ny < 0.0f || nx >= n - 1.0f || ny >= n - 1.0f) break;
                const float delta_height = height_uv(nx, ny)[0] - height;
                const float capacity = std::max(-delta_height * speed * water * 4.0f, 0.00001f);
                const int ix = static_cast<int>(px);
                const int iy = static_cast<int>(py);
                if (sediment > capacity || delta_height > 0.0f) {
                    const float deposit = delta_height > 0.0f ? std::min(delta_height, sediment) : (sediment - capacity) * 0.3f;
                    sediment -= deposit;
                    at(ix, iy) = std::clamp(at(ix, iy) + deposit, 0.0f, 1.0f);
                } else {
                    const float erode = std::min((capacity - sediment) * 0.3f * strength, -delta_height);
                    at(ix, iy) = std::clamp(at(ix, iy) - erode, 0.0f, 1.0f);
                    sediment += erode;
                }
                speed = std::sqrt(std::max(speed * speed + delta_height * 40.0f, 0.0f));
                water *= 0.97f;
                px = nx;
                py = ny;
            }
        }
        changed = true;
    } else {
        const float rate = strength * dt;
        for (int y = span.y0; y <= span.y1; ++y) {
            for (int x = span.x0; x <= span.x1; ++x) {
                const Vec3 p = texelWorld(data, terrain, origin, x, y);
                const float weight = brushWeight(brush, std::hypot(p.x - center.x, p.z - center.z));
                if (weight <= 0.0f) continue;
                float& value = at(x, y);
                const float before = value;
                switch (brush.tool) {
                    case TerrainTool::Raise:
                    case TerrainTool::Lower: {
                        const bool down = (brush.tool == TerrainTool::Lower) != invert;
                        // ~10 m por segundo a fuerza 1, sea cual sea la altura maxima.
                        value += (down ? -1.0f : 1.0f) * weight * rate * 10.0f / max_height;
                        break;
                    }
                    case TerrainTool::Flatten: {
                        const float target = (brush.target_height - origin.y) / max_height;
                        value += (target - value) * std::min(1.0f, weight * rate * 8.0f);
                        break;
                    }
                    case TerrainTool::Noise: {
                        const float noise = fbm(p.x * brush.noise_scale, p.z * brush.noise_scale, brush.seed, 4, 0.5f);
                        value += noise * weight * rate * 4.0f / max_height * (invert ? -1.0f : 1.0f);
                        break;
                    }
                    case TerrainTool::Terrace: {
                        const float step = std::max(brush.terrace_step, 0.05f) / max_height;
                        const float level = std::floor(value / step);
                        const float frac = value / step - level;
                        // Escalon: la parte alta del peldano se lleva al borde.
                        const float sharp = std::clamp(brush.terrace_sharpness, 0.0f, 1.0f);
                        const float shaped = std::pow(frac, 1.0f + sharp * 8.0f);
                        const float target = (level + shaped) * step;
                        value += (target - value) * std::min(1.0f, weight * rate * 6.0f);
                        break;
                    }
                    default: break;
                }
                value = std::clamp(value, 0.0f, 1.0f);
                changed = changed || value != before;
            }
        }
    }
    if (changed) data.markHeights(span.x0, span.y0, span.x1, span.y1);
    return changed;
}

bool applyRamp(TerrainData& data, const Terrain& terrain, const Vec3& origin, const Vec3& a, const Vec3& b,
               const TerrainBrush& brush) {
    const Vec3 ab{b.x - a.x, 0.0f, b.z - a.z};
    const float length = std::hypot(ab.x, ab.z);
    if (length < 1e-3f) return false;
    const float half = std::max(brush.ramp_width * 0.5f, 0.1f);
    const float cx = (a.x + b.x) * 0.5f;
    const float cz = (a.z + b.z) * 0.5f;
    const Span span = heightSpan(data, terrain, origin, cx, cz, length * 0.5f + half + 1.0f);
    const auto res = static_cast<int>(data.resolution());
    const float max_height = std::max(terrain.height, 1e-3f);
    bool changed = false;
    for (int y = span.y0; y <= span.y1; ++y) {
        for (int x = span.x0; x <= span.x1; ++x) {
            const Vec3 p = texelWorld(data, terrain, origin, x, y);
            const float t = std::clamp(((p.x - a.x) * ab.x + (p.z - a.z) * ab.z) / (length * length), 0.0f, 1.0f);
            const float px = a.x + ab.x * t;
            const float pz = a.z + ab.z * t;
            const float side = std::hypot(p.x - px, p.z - pz);
            TerrainBrush edge = brush;
            edge.radius = half;
            const float w = brushWeight(edge, side);
            if (w <= 0.0f) continue;
            const float target = ((a.y + (b.y - a.y) * t) - origin.y) / max_height;
            float& value = data.heights()[static_cast<std::size_t>(y) * res + x];
            const float next = std::clamp(value + (target - value) * w, 0.0f, 1.0f);
            if (next != value) {
                value = next;
                changed = true;
            }
        }
    }
    if (changed) data.markHeights(span.x0, span.y0, span.x1, span.y1);
    return changed;
}

// -----------------------------------------------------------------------------
// Huellas y caminos
// -----------------------------------------------------------------------------

namespace {

// Punto del suelo en los ejes de la huella (x, z locales).
core::Vec2 footprintLocal(const Footprint& f, float x, float z) {
    const float a = f.yaw_degrees * core::kPi / 180.0f;
    const float c = std::cos(a);
    const float s = std::sin(a);
    const float dx = x - f.center.x;
    const float dz = z - f.center.z;
    return core::Vec2{c * dx - s * dz, s * dx + c * dz};
}

// Distancia (m) de un punto del suelo a la huella (0 dentro).
float footprintDistance(const Footprint& f, float x, float z) {
    const core::Vec2 l = footprintLocal(f, x, z);
    return std::hypot(std::max(std::abs(l.x) - f.half.x, 0.0f), std::max(std::abs(l.y) - f.half.y, 0.0f));
}

float footprintRadius(const Footprint& f) { return std::hypot(f.half.x, f.half.y); }

// Peso de una mezcla: 1 hasta `inner`, baja suave hasta `inner + blend`.
float blendWeight(float distance, float inner, float blend) {
    if (distance <= inner) return 1.0f;
    if (blend <= 0.0f || distance >= inner + blend) return 0.0f;
    const float t = (distance - inner) / blend;
    return 1.0f - t * t * (3.0f - 2.0f * t);
}

// Mezcla los pesos de un texel hacia `amount` de la capa `layer` (w = 0..1).
bool blendSplat(TerrainData& data, std::uint32_t x, std::uint32_t y, int layer, float amount, float w) {
    float weights[kMaxLayers];
    float total = 0.0f;
    for (int l = 0; l < kMaxLayers; ++l) {
        weights[l] = static_cast<float>(data.weight(l, x, y)) / 255.0f;
        total += weights[l];
    }
    if (total <= 0.0f) {
        weights[0] = 1.0f;
        total = 1.0f;
    }
    for (float& value : weights) value /= total;
    const float target = weights[layer] + (std::clamp(amount, 0.0f, 1.0f) - weights[layer]) * std::clamp(w, 0.0f, 1.0f);
    const float rest = 1.0f - weights[layer];
    const float new_rest = 1.0f - target;
    for (int l = 0; l < kMaxLayers; ++l) {
        if (l == layer) continue;
        weights[l] = rest > 1e-5f ? weights[l] * new_rest / rest : (l == (layer == 0 ? 1 : 0) ? new_rest : 0.0f);
    }
    weights[layer] = target;
    int sum = 0;
    int biggest = 0;
    std::uint8_t bytes[kMaxLayers];
    for (int l = 0; l < kMaxLayers; ++l) {
        bytes[l] = static_cast<std::uint8_t>(std::clamp(std::lround(weights[l] * 255.0f), 0L, 255L));
        sum += bytes[l];
        if (weights[l] > weights[biggest]) biggest = l;
    }
    bytes[biggest] = static_cast<std::uint8_t>(std::clamp(static_cast<int>(bytes[biggest]) + 255 - sum, 0, 255));
    bool changed = false;
    for (int l = 0; l < kMaxLayers; ++l) {
        std::uint8_t* p = data.weightPtr(l, x, y);
        if (p != nullptr && *p != bytes[l]) {
            *p = bytes[l];
            changed = true;
        }
    }
    return changed;
}

// Texeles de los pesos dentro de un circulo (mundo): rango inclusivo.
struct SplatSpan {
    int x0, y0, x1, y1;
    float cell;
};
SplatSpan splatSpan(const TerrainData& data, const Terrain& terrain, const Vec3& origin, float cx, float cz, float radius) {
    const auto res = static_cast<int>(data.splatResolution());
    const float cell = terrain.size / static_cast<float>(std::max(res, 1));
    const float cu = (cx - origin.x) / cell - 0.5f;
    const float cv = (cz - origin.z) / cell - 0.5f;
    const float r = radius / cell + 1.0f;
    return SplatSpan{std::clamp(static_cast<int>(std::floor(cu - r)), 0, res - 1),
                     std::clamp(static_cast<int>(std::floor(cv - r)), 0, res - 1),
                     std::clamp(static_cast<int>(std::ceil(cu + r)), 0, res - 1),
                     std::clamp(static_cast<int>(std::ceil(cv + r)), 0, res - 1), cell};
}

// Proyeccion de un punto del suelo sobre una polilinea: distancia horizontal
// y altura del camino alli.
bool projectOnPath(const std::vector<Vec3>& points, float x, float z, float& distance, float& height) {
    distance = 1e30f;
    bool any = false;
    for (std::size_t i = 0; i + 1 < points.size(); ++i) {
        const Vec3& a = points[i];
        const Vec3& b = points[i + 1];
        const float abx = b.x - a.x;
        const float abz = b.z - a.z;
        const float len2 = abx * abx + abz * abz;
        const float t = len2 > 1e-8f ? std::clamp(((x - a.x) * abx + (z - a.z) * abz) / len2, 0.0f, 1.0f) : 0.0f;
        const float d = std::hypot(x - (a.x + abx * t), z - (a.z + abz * t));
        if (d < distance) {
            distance = d;
            height = a.y + (b.y - a.y) * t;
            any = true;
        }
    }
    return any;
}

}  // namespace

bool flattenFootprint(TerrainData& data, const Terrain& terrain, const Vec3& origin, const Footprint& footprint,
                      float height, float blend) {
    if (data.resolution() < 3) return false;
    const float cell = terrain.size / static_cast<float>(data.resolution() - 1);
    // Toda celda que toca la huella: sus cuatro vertices (el muestreo
    // bilineal usa esos) a lo sumo a una diagonal de celda.
    const float cover = cell * 1.5f;
    blend = std::max(blend, 0.0f);
    const Span span = heightSpan(data, terrain, origin, footprint.center.x, footprint.center.z,
                                 footprintRadius(footprint) + cover + blend);
    const auto res = static_cast<int>(data.resolution());
    const float max_height = std::max(terrain.height, 1e-3f);
    const float target = std::clamp((height - origin.y) / max_height, 0.0f, 1.0f);
    bool changed = false;
    for (int y = span.y0; y <= span.y1; ++y) {
        for (int x = span.x0; x <= span.x1; ++x) {
            const Vec3 p = texelWorld(data, terrain, origin, x, y);
            const float w = blendWeight(footprintDistance(footprint, p.x, p.z), cover, blend);
            if (w <= 0.0f) continue;
            float& value = data.heights()[static_cast<std::size_t>(y) * res + x];
            const float next = value + (target - value) * w;
            if (next != value) {
                value = next;
                changed = true;
            }
        }
    }
    if (changed) data.markHeights(span.x0, span.y0, span.x1, span.y1);
    return changed;
}

float footprintHeight(const TerrainData& data, const Terrain& terrain, const Vec3& origin, const Footprint& footprint,
                      float* min_height, float* max_height) {
    std::vector<float> samples;
    constexpr int kSteps = 9;
    const float a = footprint.yaw_degrees * core::kPi / 180.0f;
    const float c = std::cos(a);
    const float s = std::sin(a);
    for (int i = 0; i < kSteps; ++i) {
        for (int j = 0; j < kSteps; ++j) {
            const float lx = (static_cast<float>(i) / (kSteps - 1) * 2.0f - 1.0f) * footprint.half.x;
            const float lz = (static_cast<float>(j) / (kSteps - 1) * 2.0f - 1.0f) * footprint.half.y;
            // Local -> mundo (giro en Y).
            const float x = footprint.center.x + c * lx + s * lz;
            const float z = footprint.center.z - s * lx + c * lz;
            if (x < origin.x || z < origin.z || x > origin.x + terrain.size || z > origin.z + terrain.size) continue;
            samples.push_back(heightAt(data, terrain, origin, x, z));
        }
    }
    if (samples.empty()) {
        const float h = heightAt(data, terrain, origin, footprint.center.x, footprint.center.z);
        if (min_height) *min_height = h;
        if (max_height) *max_height = h;
        return h;
    }
    std::sort(samples.begin(), samples.end());
    if (min_height) *min_height = samples.front();
    if (max_height) *max_height = samples.back();
    return samples[samples.size() / 2];
}

bool paintFootprint(TerrainData& data, const Terrain& terrain, const Vec3& origin, const Footprint& footprint, int layer,
                    float blend, float amount) {
    if (data.splatResolution() == 0 || layer < 0 || layer >= kMaxLayers) return false;
    blend = std::max(blend, 0.0f);
    const SplatSpan span = splatSpan(data, terrain, origin, footprint.center.x, footprint.center.z,
                                     footprintRadius(footprint) + blend);
    bool changed = false;
    for (int y = span.y0; y <= span.y1; ++y) {
        for (int x = span.x0; x <= span.x1; ++x) {
            const float px = origin.x + (static_cast<float>(x) + 0.5f) * span.cell;
            const float pz = origin.z + (static_cast<float>(y) + 0.5f) * span.cell;
            const float w = blendWeight(footprintDistance(footprint, px, pz), span.cell * 0.5f, blend);
            if (w <= 0.0f) continue;
            changed = blendSplat(data, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y), layer, amount, w) || changed;
        }
    }
    if (changed) data.markSplat(span.x0, span.y0, span.x1, span.y1);
    return changed;
}

bool flattenPath(TerrainData& data, const Terrain& terrain, const Vec3& origin, const std::vector<Vec3>& points,
                 float width, float blend) {
    if (data.resolution() < 3 || points.size() < 2) return false;
    const float cell = terrain.size / static_cast<float>(data.resolution() - 1);
    const float inner = std::max(width * 0.5f, cell * 0.75f);
    blend = std::max(blend, 0.0f);
    const auto res = static_cast<int>(data.resolution());
    const float max_height = std::max(terrain.height, 1e-3f);
    // Tramo a tramo (cada uno con su caja) para no recorrer todo el terreno.
    bool changed = false;
    for (std::size_t i = 0; i + 1 < points.size(); ++i) {
        const Vec3& a = points[i];
        const Vec3& b = points[i + 1];
        const float half_len = std::hypot(b.x - a.x, b.z - a.z) * 0.5f;
        const Span span = heightSpan(data, terrain, origin, (a.x + b.x) * 0.5f, (a.z + b.z) * 0.5f, half_len + inner + blend);
        const std::vector<Vec3> segment = {a, b};
        for (int y = span.y0; y <= span.y1; ++y) {
            for (int x = span.x0; x <= span.x1; ++x) {
                const Vec3 p = texelWorld(data, terrain, origin, x, y);
                float distance = 0.0f;
                float height = 0.0f;
                // La distancia a TODO el camino (no solo a este tramo): asi
                // cada vertice lo decide el tramo mas cercano, sin escalones.
                if (!projectOnPath(points, p.x, p.z, distance, height)) continue;
                float seg_distance = 0.0f;
                float seg_height = 0.0f;
                projectOnPath(segment, p.x, p.z, seg_distance, seg_height);
                if (seg_distance > distance + 1e-4f) continue;  // lo hace otro tramo
                const float w = blendWeight(distance, inner, blend);
                if (w <= 0.0f) continue;
                const float target = std::clamp((height - origin.y) / max_height, 0.0f, 1.0f);
                float& value = data.heights()[static_cast<std::size_t>(y) * res + x];
                const float next = value + (target - value) * w;
                if (next != value) {
                    value = next;
                    changed = true;
                }
            }
        }
        if (changed) data.markHeights(span.x0, span.y0, span.x1, span.y1);
    }
    return changed;
}

bool paintPath(TerrainData& data, const Terrain& terrain, const Vec3& origin, const std::vector<Vec3>& points, float width,
               int layer, float blend, float amount) {
    if (data.splatResolution() == 0 || points.size() < 2 || layer < 0 || layer >= kMaxLayers) return false;
    blend = std::max(blend, 0.0f);
    const float inner = width * 0.5f;
    bool changed = false;
    for (std::size_t i = 0; i + 1 < points.size(); ++i) {
        const Vec3& a = points[i];
        const Vec3& b = points[i + 1];
        const float half_len = std::hypot(b.x - a.x, b.z - a.z) * 0.5f;
        const SplatSpan span = splatSpan(data, terrain, origin, (a.x + b.x) * 0.5f, (a.z + b.z) * 0.5f, half_len + inner + blend);
        const std::vector<Vec3> segment = {a, b};
        bool any = false;
        for (int y = span.y0; y <= span.y1; ++y) {
            for (int x = span.x0; x <= span.x1; ++x) {
                const float px = origin.x + (static_cast<float>(x) + 0.5f) * span.cell;
                const float pz = origin.z + (static_cast<float>(y) + 0.5f) * span.cell;
                float distance = 0.0f;
                float height = 0.0f;
                if (!projectOnPath(points, px, pz, distance, height)) continue;
                float seg_distance = 0.0f;
                projectOnPath(segment, px, pz, seg_distance, height);
                if (seg_distance > distance + 1e-4f) continue;
                const float w = blendWeight(distance, inner, blend);
                if (w <= 0.0f) continue;
                any = blendSplat(data, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y), layer, amount, w) || any;
            }
        }
        if (any) data.markSplat(span.x0, span.y0, span.x1, span.y1);
        changed = changed || any;
    }
    return changed;
}

HeightPatch captureFootprint(const TerrainData& data, const Terrain& terrain, const Vec3& origin, const Footprint& footprint,
                             float blend) {
    HeightPatch patch;
    if (data.resolution() < 3) return patch;
    const float cell = terrain.size / static_cast<float>(data.resolution() - 1);
    const Span span = heightSpan(data, terrain, origin, footprint.center.x, footprint.center.z,
                                 footprintRadius(footprint) + cell * 1.5f + std::max(blend, 0.0f));
    patch.x0 = span.x0;
    patch.y0 = span.y0;
    patch.x1 = span.x1;
    patch.y1 = span.y1;
    const auto res = static_cast<int>(data.resolution());
    patch.heights.reserve(static_cast<std::size_t>(span.x1 - span.x0 + 1) * static_cast<std::size_t>(span.y1 - span.y0 + 1));
    for (int y = span.y0; y <= span.y1; ++y) {
        for (int x = span.x0; x <= span.x1; ++x) patch.heights.push_back(data.heights()[static_cast<std::size_t>(y) * res + x]);
    }
    return patch;
}

void restorePatch(TerrainData& data, const HeightPatch& patch) {
    if (!patch.valid()) return;
    const auto res = static_cast<int>(data.resolution());
    if (patch.x1 >= res || patch.y1 >= res) return;
    const std::size_t w = static_cast<std::size_t>(patch.x1 - patch.x0 + 1);
    if (patch.heights.size() != w * static_cast<std::size_t>(patch.y1 - patch.y0 + 1)) return;
    for (int y = patch.y0; y <= patch.y1; ++y) {
        for (int x = patch.x0; x <= patch.x1; ++x) {
            data.heights()[static_cast<std::size_t>(y) * res + x] =
                patch.heights[static_cast<std::size_t>(y - patch.y0) * w + static_cast<std::size_t>(x - patch.x0)];
        }
    }
    data.markHeights(patch.x0, patch.y0, patch.x1, patch.y1);
}

void generateRelief(TerrainData& data, std::uint32_t seed, float frequency, float roughness, float ridges,
                    float base_height) {
    const auto res = static_cast<int>(data.resolution());
    std::vector<float>& h = data.heights();
    // Filas en paralelo (job system): cada una escribe solo las suyas.
    jobs::parallelFor(static_cast<std::size_t>(res), 8, [&](std::size_t row0, std::size_t row1) {
    for (int y = static_cast<int>(row0); y < static_cast<int>(row1); ++y) {
        for (int x = 0; x < res; ++x) {
            const float u = static_cast<float>(x) / static_cast<float>(res - 1) * frequency;
            const float v = static_cast<float>(y) / static_cast<float>(res - 1) * frequency;
            const float base = fbm(u, v, seed, 6, roughness) * 0.5f + 0.5f;
            // Crestas: 1 - |ruido| da lomos afilados (montanas).
            const float ridge = 1.0f - std::abs(fbm(u * 1.7f + 11.3f, v * 1.7f - 4.1f, seed + 7u, 5, roughness));
            const float value = base * (1.0f - ridges) + ridge * ridge * ridges;
            h[static_cast<std::size_t>(y) * res + x] = std::clamp(base_height + value * (1.0f - base_height), 0.0f, 1.0f);
        }
    }
    });
    data.markHeights(0, 0, res - 1, res - 1);
    data.commitCollision();
}

void paintByRules(TerrainData& data, const Terrain& terrain, int layer, float min_slope_deg, float max_slope_deg,
                  float min_height, float max_height) {
    const auto res = static_cast<int>(data.splatResolution());
    const Vec3 origin{};
    jobs::parallelFor(static_cast<std::size_t>(res), 8, [&](std::size_t row0, std::size_t row1) {
    for (int y = static_cast<int>(row0); y < static_cast<int>(row1); ++y) {
        for (int x = 0; x < res; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(res);
            const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(res);
            const Vec3 n = normalAt(data, terrain, origin, u * terrain.size, v * terrain.size);
            const float slope = std::acos(std::clamp(n.y, -1.0f, 1.0f)) * 180.0f / core::kPi;
            const float h = data.sample(u, v);
            if (slope < min_slope_deg || slope > max_slope_deg || h < min_height || h > max_height) continue;
            for (int l = 0; l < kMaxLayers; ++l) {
                std::uint8_t* p = data.weightPtr(l, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
                if (p != nullptr) *p = l == layer ? 255 : 0;
            }
        }
    }
    });
    data.markSplat(0, 0, res - 1, res - 1);
}

// -----------------------------------------------------------------------------
// Consultas
// -----------------------------------------------------------------------------

float heightAt(const TerrainData& data, const Terrain& terrain, const Vec3& origin, float x, float z) {
    return origin.y + data.sample((x - origin.x) / terrain.size, (z - origin.z) / terrain.size) * terrain.height;
}

Vec3 normalAt(const TerrainData& data, const Terrain& terrain, const Vec3& origin, float x, float z) {
    const float e = terrain.size / static_cast<float>(std::max<std::uint32_t>(data.resolution() - 1, 1));
    const float hl = heightAt(data, terrain, origin, x - e, z);
    const float hr = heightAt(data, terrain, origin, x + e, z);
    const float hd = heightAt(data, terrain, origin, x, z - e);
    const float hu = heightAt(data, terrain, origin, x, z + e);
    return core::normalize(Vec3{hl - hr, 2.0f * e, hd - hu});
}

bool raycast(const TerrainData& data, const Terrain& terrain, const Vec3& origin, const Vec3& ray_origin,
             const Vec3& ray_direction, float max_distance, Vec3& hit) {
    if (data.resolution() < 3) return false;
    const Vec3 d = core::normalize(ray_direction);
    // Recortar el rayo a la caja del terreno.
    const float lo[3] = {origin.x, origin.y - 1.0f, origin.z};
    const float hi[3] = {origin.x + terrain.size, origin.y + terrain.height + 1.0f, origin.z + terrain.size};
    const float o[3] = {ray_origin.x, ray_origin.y, ray_origin.z};
    const float dir[3] = {d.x, d.y, d.z};
    float t0 = 0.0f;
    float t1 = max_distance;
    for (int i = 0; i < 3; ++i) {
        if (std::abs(dir[i]) < 1e-9f) {
            if (o[i] < lo[i] || o[i] > hi[i]) return false;
            continue;
        }
        float a = (lo[i] - o[i]) / dir[i];
        float b = (hi[i] - o[i]) / dir[i];
        if (a > b) std::swap(a, b);
        t0 = std::max(t0, a);
        t1 = std::min(t1, b);
        if (t0 > t1) return false;
    }
    // Paso de medio texel y afinado por biseccion.
    const float step = terrain.size / static_cast<float>(data.resolution() - 1) * 0.5f;
    const auto above = [&](float t) {
        const Vec3 p = ray_origin + d * t;
        return p.y - heightAt(data, terrain, origin, p.x, p.z);
    };
    float previous = t0;
    float previous_above = above(t0);
    if (previous_above < 0.0f) {
        hit = ray_origin + d * t0;
        return true;
    }
    for (float t = t0 + step; t <= t1 + step; t += step) {
        const float current = std::min(t, t1);
        const float current_above = above(current);
        if (current_above <= 0.0f) {
            float a = previous;
            float b = current;
            for (int i = 0; i < 20; ++i) {
                const float mid = (a + b) * 0.5f;
                (above(mid) > 0.0f ? a : b) = mid;
            }
            hit = ray_origin + d * ((a + b) * 0.5f);
            return true;
        }
        previous = current;
        previous_above = current_above;
        if (current >= t1) break;
    }
    return false;
}

}  // namespace cramion::terrain
