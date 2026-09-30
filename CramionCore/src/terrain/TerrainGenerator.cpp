#include "CramionCore/terrain/TerrainGenerator.h"

#include <CramionFX/asset/ImageFile.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <queue>
#include <random>
#include <thread>
#include <utility>
#include <vector>

namespace cramion::terrain {

namespace {

using core::Vec2;
using core::Vec3;

constexpr float kPi = 3.14159265f;

// -----------------------------------------------------------------------------
// Ruido
// -----------------------------------------------------------------------------

inline std::uint32_t mixHash(std::uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

inline std::uint32_t hashCell(int x, int y, std::uint32_t seed) {
    return mixHash(static_cast<std::uint32_t>(x) * 0x8da6b343U ^ static_cast<std::uint32_t>(y) * 0xd8163841U ^
                   seed * 0xcb1ab31fU);
}

constexpr float kGradients[8][2] = {{1.0f, 0.0f},        {-1.0f, 0.0f},        {0.0f, 1.0f},
                                    {0.0f, -1.0f},       {0.70710678f, 0.70710678f},
                                    {-0.70710678f, 0.70710678f}, {0.70710678f, -0.70710678f},
                                    {-0.70710678f, -0.70710678f}};

inline float fade(float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); }

// Ruido de gradiente (Perlin) en -1..1. Con `period` > 0 se repite cada
// `period` celdas (texturas que encajan).
float perlin(float x, float y, std::uint32_t seed, int period = 0) {
    const float fx = std::floor(x);
    const float fy = std::floor(y);
    int x0 = static_cast<int>(fx);
    int y0 = static_cast<int>(fy);
    const float dx = x - fx;
    const float dy = y - fy;
    int x1 = x0 + 1;
    int y1 = y0 + 1;
    if (period > 0) {
        x0 = ((x0 % period) + period) % period;
        y0 = ((y0 % period) + period) % period;
        x1 = ((x1 % period) + period) % period;
        y1 = ((y1 % period) + period) % period;
    }
    const auto dot = [&](int ix, int iy, float ox, float oy) {
        const float* g = kGradients[hashCell(ix, iy, seed) & 7U];
        return g[0] * ox + g[1] * oy;
    };
    const float u = fade(dx);
    const float v = fade(dy);
    const float a = dot(x0, y0, dx, dy) + (dot(x1, y0, dx - 1.0f, dy) - dot(x0, y0, dx, dy)) * u;
    const float b = dot(x0, y1, dx, dy - 1.0f) + (dot(x1, y1, dx - 1.0f, dy - 1.0f) - dot(x0, y1, dx, dy - 1.0f)) * u;
    return (a + (b - a) * v) * 1.41421356f;
}

float fbm(float x, float y, std::uint32_t seed, int octaves, float gain = 0.5f, int period = 0) {
    float sum = 0.0f;
    float amplitude = 1.0f;
    float total = 0.0f;
    for (int i = 0; i < octaves; ++i) {
        sum += perlin(x, y, seed + static_cast<std::uint32_t>(i) * 1013U, period) * amplitude;
        total += amplitude;
        amplitude *= gain;
        x *= 2.0f;
        y *= 2.0f;
        if (period > 0) period *= 2;
    }
    return sum / total;
}

// Ridged multifractal (Musgrave): crestas afiladas que se ramifican. 0..1.
float ridged(float x, float y, std::uint32_t seed, int octaves, float sharpness) {
    float sum = 0.0f;
    float amplitude = 0.5f;
    float weight = 1.0f;
    float total = 0.0f;
    for (int i = 0; i < octaves; ++i) {
        float signal = 1.0f - std::abs(perlin(x, y, seed + static_cast<std::uint32_t>(i) * 7919U));
        signal = std::pow(std::max(signal, 0.0f), 1.0f + sharpness * 2.0f);
        signal *= weight;
        weight = std::clamp(signal * 2.0f, 0.0f, 1.0f);
        sum += signal * amplitude;
        total += amplitude;
        amplitude *= 0.5f;
        x *= 2.03f;
        y *= 2.03f;
    }
    return sum / total;
}

// Distancia a los dos puntos mas cercanos (Voronoi) en una rejilla que se
// repite cada `period` celdas. id = celda mas cercana.
void voronoi(float x, float y, std::uint32_t seed, int period, float& f1, float& f2, std::uint32_t& id) {
    const int cx = static_cast<int>(std::floor(x));
    const int cy = static_cast<int>(std::floor(y));
    f1 = 1e9f;
    f2 = 1e9f;
    id = 0;
    for (int oy = -1; oy <= 1; ++oy) {
        for (int ox = -1; ox <= 1; ++ox) {
            const int gx = cx + ox;
            const int gy = cy + oy;
            const int wx = period > 0 ? ((gx % period) + period) % period : gx;
            const int wy = period > 0 ? ((gy % period) + period) % period : gy;
            const std::uint32_t h = hashCell(wx, wy, seed);
            const float px = static_cast<float>(gx) + static_cast<float>(h & 0xFFFFU) / 65535.0f;
            const float py = static_cast<float>(gy) + static_cast<float>(h >> 16) / 65535.0f;
            const float d = std::sqrt((px - x) * (px - x) + (py - y) * (py - y));
            if (d < f1) {
                f2 = f1;
                f1 = d;
                id = h;
            } else if (d < f2) {
                f2 = d;
            }
        }
    }
}

inline float smoothstep(float edge0, float edge1, float x) {
    const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

inline float lerp(float a, float b, float t) { return a + (b - a) * t; }

// Filas en paralelo.
template <class F>
void parallelFor(int count, F&& body) {
    const unsigned threads = std::max(1U, std::min(std::thread::hardware_concurrency(), 32U));
    std::atomic<int> next{0};
    std::vector<std::thread> pool;
    pool.reserve(threads);
    for (unsigned t = 0; t < threads; ++t) {
        pool.emplace_back([&]() {
            for (int i = next.fetch_add(1); i < count; i = next.fetch_add(1)) body(i);
        });
    }
    for (std::thread& th : pool) th.join();
}

// -----------------------------------------------------------------------------
// Rejilla
// -----------------------------------------------------------------------------

struct Grid {
    int n = 0;
    std::vector<float> h;
    float& at(int x, int y) { return h[static_cast<std::size_t>(y) * static_cast<std::size_t>(n) + static_cast<std::size_t>(x)]; }
    float at(int x, int y) const {
        return h[static_cast<std::size_t>(y) * static_cast<std::size_t>(n) + static_cast<std::size_t>(x)];
    }
    float bilinear(float x, float y) const {
        x = std::clamp(x, 0.0f, static_cast<float>(n - 1) - 1e-4f);
        y = std::clamp(y, 0.0f, static_cast<float>(n - 1) - 1e-4f);
        const int x0 = static_cast<int>(x);
        const int y0 = static_cast<int>(y);
        const float fx = x - static_cast<float>(x0);
        const float fy = y - static_cast<float>(y0);
        const float a = lerp(at(x0, y0), at(x0 + 1, y0), fx);
        const float b = lerp(at(x0, y0 + 1), at(x0 + 1, y0 + 1), fx);
        return lerp(a, b, fy);
    }
};

// -----------------------------------------------------------------------------
// 1. Forma
// -----------------------------------------------------------------------------

// Escalones: tramos llanos y una subida entre ellos (mesetas).
float terrace(float h, float steps) {
    const float scaled = h * steps;
    const float step = std::floor(scaled);
    return (step + smoothstep(0.25f, 0.9f, scaled - step)) / steps;
}

void generateShape(const GenSettings& s, Grid& g) {
    const int n = g.n;
    const float cell = s.size / static_cast<float>(n - 1);
    const float scale = 1.0f / (1000.0f * std::max(s.feature_scale, 0.05f));
    const std::uint32_t seed = s.seed * 7919U + 17U;
    const bool sea = s.shape != GenShape::Mountains && s.shape != GenShape::Canyons;

    parallelFor(n, [&](int y) {
        for (int x = 0; x < n; ++x) {
            const float wx = static_cast<float>(x) * cell;
            const float wz = static_cast<float>(y) * cell;
            const float u = static_cast<float>(x) / static_cast<float>(n - 1) * 2.0f - 1.0f;
            const float v = static_cast<float>(y) / static_cast<float>(n - 1) * 2.0f - 1.0f;

            // Domain warping: las formas dejan de parecer ruido.
            const float warp_amount = s.warp * 650.0f / scale / 1000.0f;
            const float qx = fbm(wx * scale * 0.5f, wz * scale * 0.5f, seed + 11U, 4);
            const float qz = fbm(wx * scale * 0.5f + 5.2f, wz * scale * 0.5f + 1.3f, seed + 12U, 4);
            const float px = wx + qx * warp_amount;
            const float pz = wz + qz * warp_amount;

            // --- Mascara de tierra (m > 0) ---
            float m = 1.0f;
            const float coast_noise = fbm(px * scale * 1.2f, pz * scale * 1.2f, seed + 21U, 5);
            switch (s.shape) {
                case GenShape::Island: {
                    const float d = std::sqrt(u * u + v * v) * (1.0f + s.coast * 0.28f * coast_noise);
                    m = (0.74f - d) / 0.32f + 0.25f * fbm(px * scale * 0.35f, pz * scale * 0.35f, seed + 3U, 4);
                    break;
                }
                case GenShape::Archipelago: {
                    const float edge = std::max(std::abs(u), std::abs(v));
                    m = fbm(px * scale * 0.9f, pz * scale * 0.9f, seed + 3U, 5) * 2.2f - 0.1f +
                        s.coast * 0.35f * coast_noise - smoothstep(0.7f, 1.0f, edge) * 2.0f;
                    break;
                }
                case GenShape::Continent: {
                    // Tierra en casi todo el mapa, con costa en una diagonal.
                    const float side = u * 0.55f + v * 0.45f;
                    m = 0.55f + fbm(px * scale * 0.3f, pz * scale * 0.3f, seed + 3U, 5) * 0.9f -
                        std::max(side - 0.1f, 0.0f) * 2.6f + s.coast * 0.3f * coast_noise;
                    break;
                }
                case GenShape::Mountains:
                case GenShape::Canyons:
                    m = 1.0f;
                    break;
            }
            m = std::clamp(m, -1.0f, 1.0f);

            // Tierra adentro (0 en la costa, 1 lejos).
            const float inland = sea ? smoothstep(0.0f, 0.45f, m) : 1.0f;

            // --- Relieve sobre el mar (0..1 del rango de tierra) ---
            const float hills = fbm(px * scale * 1.7f, pz * scale * 1.7f, seed + 5U, 5) * 0.5f + 0.5f;
            const float plains = 0.02f + s.hills * 0.14f * hills;
            const float mask_noise = fbm(px * scale * 0.45f, pz * scale * 0.45f, seed + 7U, 4) * 0.5f + 0.5f;
            const float mountain_mask =
                smoothstep(0.62f - s.mountains * 0.5f, 0.9f - s.mountains * 0.45f, mask_noise);
            const float ridge = ridged(px * scale, pz * scale, seed + 9U, 7, s.ridges);
            const float mountain = mountain_mask * std::pow(ridge, 1.35f) * (0.3f + 0.7f * inland);
            float elevation = inland * plains + mountain * (0.35f + 0.65f * s.mountains) + inland * 0.03f;

            if (s.shape == GenShape::Canyons) {
                // Meseta alta cortada por canones que se ramifican.
                const float canyon_lines = ridged(px * scale * 0.8f, pz * scale * 0.8f, seed + 13U, 5, 0.9f);
                const float canyon = smoothstep(0.55f, 0.85f, canyon_lines);
                elevation = 0.55f + hills * s.hills * 0.12f + mountain * 0.25f - canyon * 0.5f;
            }
            if (s.plateaus > 0.0f || s.shape == GenShape::Canyons) {
                const float amount = s.shape == GenShape::Canyons ? std::max(s.plateaus, 0.7f) : s.plateaus;
                elevation = lerp(elevation, terrace(std::max(elevation, 0.0f), 7.0f), amount);
            }

            float h;
            if (!sea) {
                h = s.sea_level + std::max(elevation, 0.0f) * (1.0f - s.sea_level);
            } else if (m < 0.0f) {
                // Fondo del mar: plataforma cerca de la costa y luego cae.
                const float depth = smoothstep(0.0f, 0.6f, -m);
                h = s.sea_level * (1.0f - 0.15f - depth * 0.8f) + hills * 0.01f;
            } else {
                // Costa: de un poco bajo el mar a tierra, sin escalon.
                const float shore = smoothstep(0.0f, 0.12f, m);
                const float land = s.sea_level + std::max(elevation, 0.0f) * (1.0f - s.sea_level);
                h = lerp(s.sea_level * 0.85f, land, shore);
            }
            g.at(x, y) = std::clamp(h, 0.0f, 1.2f);
        }
    });

    // Que lo mas alto quede en 1 (la altura maxima del terreno).
    float top = 0.0f;
    for (float h : g.h) top = std::max(top, h);
    if (top > 1.0f) {
        const float k = (1.0f - s.sea_level) / (top - s.sea_level);
        for (float& h : g.h) {
            if (h > s.sea_level) h = s.sea_level + (h - s.sea_level) * k;
        }
    }
}

// -----------------------------------------------------------------------------
// 2. Erosion
// -----------------------------------------------------------------------------

// Termica: lo que pasa del angulo de reposo cae al vecino mas bajo.
void thermalErosion(const GenSettings& s, Grid& g, int passes) {
    const int n = g.n;
    const float cell = s.size / static_cast<float>(n - 1);
    const float talus = std::tan(s.talus * kPi / 180.0f) * cell / s.height;  // diferencia en 0..1
    std::vector<float> delta(g.h.size());
    for (int p = 0; p < passes; ++p) {
        std::fill(delta.begin(), delta.end(), 0.0f);
        // Cada celda resta de si misma y suma en su vecino mas bajo (su fila
        // o las de al lado): en paralelo por filas de 3 en 3 para que dos
        // hilos nunca escriban la misma fila.
        for (int phase = 0; phase < 3; ++phase) {
            parallelFor((n - 2 + 2 - phase) / 3, [&](int row) {
                const int y = 1 + phase + row * 3;
                if (y >= n - 1) return;
                for (int x = 1; x < n - 1; ++x) {
                    const float h = g.at(x, y);
                    float lowest = h;
                    int lx = x;
                    int ly = y;
                    constexpr int dx[4] = {1, -1, 0, 0};
                    constexpr int dy[4] = {0, 0, 1, -1};
                    for (int k = 0; k < 4; ++k) {
                        const float nh = g.at(x + dx[k], y + dy[k]);
                        if (nh < lowest) {
                            lowest = nh;
                            lx = x + dx[k];
                            ly = y + dy[k];
                        }
                    }
                    const float diff = h - lowest;
                    if (diff > talus) {
                        const float move = (diff - talus) * 0.25f;
                        delta[static_cast<std::size_t>(y) * n + x] -= move;
                        // El vecino puede estar en la fila de al lado (otra
                        // paridad): se escribe en su propio acumulador.
                        delta[static_cast<std::size_t>(ly) * n + lx] += move;
                    }
                }
            });
        }
        for (std::size_t i = 0; i < g.h.size(); ++i) g.h[i] += delta[i];
    }
}

struct Droplet {
    float inertia = 0.05f;
    float capacity = 4.0f;
    float min_capacity = 0.01f;
    float erode = 0.3f;
    float deposit = 0.3f;
    float evaporate = 0.01f;
    float gravity = 4.0f;
    int lifetime = 40;
    int radius = 3;
};

struct Brush {
    std::vector<int> dx;
    std::vector<int> dy;
    std::vector<float> w;
};

Brush makeBrush(int radius) {
    Brush b;
    float total = 0.0f;
    for (int y = -radius; y <= radius; ++y) {
        for (int x = -radius; x <= radius; ++x) {
            const float d = std::sqrt(static_cast<float>(x * x + y * y));
            if (d <= static_cast<float>(radius)) {
                const float w = 1.0f - d / static_cast<float>(radius + 1);
                b.dx.push_back(x);
                b.dy.push_back(y);
                b.w.push_back(w);
                total += w;
            }
        }
    }
    for (float& w : b.w) w /= total;
    return b;
}

// Altura y pendiente en una posicion (celdas), bilineal.
inline void heightGradient(const Grid& g, float px, float py, float& height, float& gx, float& gy) {
    const int x = static_cast<int>(px);
    const int y = static_cast<int>(py);
    const float u = px - static_cast<float>(x);
    const float v = py - static_cast<float>(y);
    const float nw = g.at(x, y);
    const float ne = g.at(x + 1, y);
    const float sw = g.at(x, y + 1);
    const float se = g.at(x + 1, y + 1);
    gx = (ne - nw) * (1.0f - v) + (se - sw) * v;
    gy = (sw - nw) * (1.0f - u) + (se - ne) * u;
    height = nw * (1.0f - u) * (1.0f - v) + ne * u * (1.0f - v) + sw * (1.0f - u) * v + se * u * v;
}

// Erosion hidraulica por gotas (Beyer 2015, Lague 2019), en paralelo por
// baldosas que no se tocan: cada gota vive dentro de la suya.
float hydraulicErosion(const GenSettings& s, Grid& g, std::vector<float>& deposit_map, const GenProgress& progress,
                       float progress_from, float progress_to, bool& cancelled) {
    const int n = g.n;
    const auto cells = static_cast<double>(n) * static_cast<double>(n);
    const auto total = static_cast<std::int64_t>(cells * 1.3 * static_cast<double>(s.erosion));
    if (total <= 0) return 0.0f;

    // Las constantes de Lague estan pensadas para un mapa de 256 celdas con
    // alturas 0..1: se escala la altura para tener las mismas pendientes.
    const float k = static_cast<float>(n - 1) / 256.0f * (s.height / 420.0f) * (2048.0f / s.size) * 1.6f;
    for (float& h : g.h) h *= k;

    Droplet d;
    d.erode = 0.12f + 0.5f * s.erosion_strength;
    d.deposit = 0.25f;
    const Brush brush = makeBrush(d.radius);
    const int tile = 128;
    const int margin = d.radius + 2;
    const int tiles = (n + tile - 1) / tile + 1;
    const int passes = 6;
    const std::int64_t per_tile =
        std::max<std::int64_t>(1, total / (passes * static_cast<std::int64_t>((n / tile + 1) * (n / tile + 1))));
    std::atomic<double> moved{0.0};
    std::mt19937 rng(s.seed * 31U + 5U);

    for (int pass = 0; pass < passes && !cancelled; ++pass) {
        const int ox = static_cast<int>(rng() % tile) - tile;
        const int oy = static_cast<int>(rng() % tile) - tile;
        for (int color = 0; color < 4 && !cancelled; ++color) {
            std::vector<std::pair<int, int>> list;
            for (int ty = 0; ty <= tiles; ++ty) {
                for (int tx = 0; tx <= tiles; ++tx) {
                    if (((tx & 1) | ((ty & 1) << 1)) == color) list.emplace_back(tx, ty);
                }
            }
            parallelFor(static_cast<int>(list.size()), [&](int i) {
                const int x0 = std::max(ox + list[static_cast<std::size_t>(i)].first * tile, 0) + margin;
                const int y0 = std::max(oy + list[static_cast<std::size_t>(i)].second * tile, 0) + margin;
                const int x1 = std::min(ox + (list[static_cast<std::size_t>(i)].first + 1) * tile, n - 1) - margin;
                const int y1 = std::min(oy + (list[static_cast<std::size_t>(i)].second + 1) * tile, n - 1) - margin;
                if (x1 - x0 < 4 || y1 - y0 < 4) return;
                std::mt19937 local(static_cast<std::uint32_t>(s.seed * 7919U + pass * 104729U + color * 1299709U +
                                                              static_cast<std::uint32_t>(x0 * 131 + y0)));
                std::uniform_real_distribution<float> rx(static_cast<float>(x0), static_cast<float>(x1));
                std::uniform_real_distribution<float> ry(static_cast<float>(y0), static_cast<float>(y1));
                double local_moved = 0.0;
                for (std::int64_t drop = 0; drop < per_tile; ++drop) {
                    float px = rx(local);
                    float py = ry(local);
                    float dir_x = 0.0f;
                    float dir_y = 0.0f;
                    float speed = 1.0f;
                    float water = 1.0f;
                    float sediment = 0.0f;
                    for (int life = 0; life < d.lifetime; ++life) {
                        const int node_x = static_cast<int>(px);
                        const int node_y = static_cast<int>(py);
                        const float cu = px - static_cast<float>(node_x);
                        const float cv = py - static_cast<float>(node_y);
                        float height;
                        float gx;
                        float gy;
                        heightGradient(g, px, py, height, gx, gy);
                        dir_x = dir_x * d.inertia - gx * (1.0f - d.inertia);
                        dir_y = dir_y * d.inertia - gy * (1.0f - d.inertia);
                        const float len = std::sqrt(dir_x * dir_x + dir_y * dir_y);
                        if (len < 1e-6f) break;
                        dir_x /= len;
                        dir_y /= len;
                        px += dir_x;
                        py += dir_y;
                        if (px < static_cast<float>(x0) || px >= static_cast<float>(x1) ||
                            py < static_cast<float>(y0) || py >= static_cast<float>(y1)) {
                            break;
                        }
                        float new_height;
                        float ngx;
                        float ngy;
                        heightGradient(g, px, py, new_height, ngx, ngy);
                        const float delta_h = new_height - height;
                        const float capacity =
                            std::max(-delta_h * speed * water * d.capacity, d.min_capacity);
                        if (sediment > capacity || delta_h > 0.0f) {
                            const float amount =
                                delta_h > 0.0f ? std::min(delta_h, sediment) : (sediment - capacity) * d.deposit;
                            sediment -= amount;
                            const float w00 = amount * (1.0f - cu) * (1.0f - cv);
                            const float w10 = amount * cu * (1.0f - cv);
                            const float w01 = amount * (1.0f - cu) * cv;
                            const float w11 = amount * cu * cv;
                            g.at(node_x, node_y) += w00;
                            g.at(node_x + 1, node_y) += w10;
                            g.at(node_x, node_y + 1) += w01;
                            g.at(node_x + 1, node_y + 1) += w11;
                            const std::size_t base = static_cast<std::size_t>(node_y) * n + node_x;
                            deposit_map[base] += w00;
                            deposit_map[base + 1] += w10;
                            deposit_map[base + n] += w01;
                            deposit_map[base + n + 1] += w11;
                        } else {
                            const float amount = std::min((capacity - sediment) * d.erode, -delta_h);
                            for (std::size_t b = 0; b < brush.w.size(); ++b) {
                                float& cellh = g.at(node_x + brush.dx[b], node_y + brush.dy[b]);
                                const float take = std::min(cellh, amount * brush.w[b]);
                                cellh -= take;
                                sediment += take;
                            }
                            local_moved += amount;
                        }
                        speed = std::sqrt(std::max(speed * speed - delta_h * d.gravity, 0.0f));
                        water *= 1.0f - d.evaporate;
                    }
                }
                double current = moved.load();
                while (!moved.compare_exchange_weak(current, current + local_moved)) {
                }
            });
            if (progress) {
                const float t = (static_cast<float>(pass) + static_cast<float>(color + 1) / 4.0f) / passes;
                if (!progress(lerp(progress_from, progress_to, t), "Erosion hidraulica")) cancelled = true;
            }
        }
    }
    for (float& h : g.h) h /= k;
    for (float& v : deposit_map) v /= k;
    const float cell = s.size / static_cast<float>(n - 1);
    return static_cast<float>(moved.load() / k * s.height * cell * cell);
}

// -----------------------------------------------------------------------------
// 3. Hidrologia
// -----------------------------------------------------------------------------

struct Hydrology {
    std::vector<float> filled;          // alturas con las cuencas llenas
    std::vector<int> receiver;          // a donde va el agua de cada celda (-1 = sale)
    std::vector<int> order;             // de abajo arriba
    std::vector<float> accumulation;    // celdas que desaguan por aqui
};

Hydrology computeHydrology(const GenSettings& s, const Grid& g) {
    const int n = g.n;
    const std::size_t count = g.h.size();
    Hydrology hy;
    hy.filled.assign(count, 0.0f);
    hy.receiver.assign(count, -1);
    hy.order.reserve(count);
    std::vector<std::uint8_t> done(count, 0);
    using Item = std::pair<float, int>;
    std::priority_queue<Item, std::vector<Item>, std::greater<>> open;
    const bool sea = s.ocean && s.shape != GenShape::Mountains && s.shape != GenShape::Canyons;
    // Salidas: el borde del mapa y, con mar, todo lo que esta bajo el mar.
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
            const bool edge = x == 0 || y == 0 || x == n - 1 || y == n - 1;
            const std::size_t i = static_cast<std::size_t>(y) * n + x;
            if (edge || (sea && g.h[i] <= s.sea_level)) {
                hy.filled[i] = g.h[i];
                done[i] = 1;
                open.emplace(g.h[i], static_cast<int>(i));
            }
        }
    }
    const float epsilon = 1e-6f;
    constexpr int dx[8] = {1, -1, 0, 0, 1, 1, -1, -1};
    constexpr int dy[8] = {0, 0, 1, -1, 1, -1, 1, -1};
    while (!open.empty()) {
        const auto [height, index] = open.top();
        open.pop();
        hy.order.push_back(index);
        const int x = index % n;
        const int y = index / n;
        for (int k = 0; k < 8; ++k) {
            const int nx = x + dx[k];
            const int ny = y + dy[k];
            if (nx < 0 || ny < 0 || nx >= n || ny >= n) continue;
            const std::size_t j = static_cast<std::size_t>(ny) * n + nx;
            if (done[j]) continue;
            done[j] = 1;
            hy.filled[j] = std::max(g.h[j], height + epsilon);
            hy.receiver[j] = index;
            open.emplace(hy.filled[j], static_cast<int>(j));
        }
    }
    hy.accumulation.assign(count, 1.0f);
    for (auto it = hy.order.rbegin(); it != hy.order.rend(); ++it) {
        const int r = hy.receiver[static_cast<std::size_t>(*it)];
        if (r >= 0) hy.accumulation[static_cast<std::size_t>(r)] += hy.accumulation[static_cast<std::size_t>(*it)];
    }
    return hy;
}

// Rios: de las desembocaduras con mas caudal hacia arriba, por el afluente
// mayor. Se excavan en `g` y se devuelven como puntos del mundo.
void carveRivers(const GenSettings& s, Grid& g, const Hydrology& hy, std::vector<float>& river_mask,
                 GenResult& result) {
    if (s.rivers <= 0) return;
    const int n = g.n;
    const float cell = s.size / static_cast<float>(n - 1);
    const bool sea = s.ocean && s.shape != GenShape::Mountains && s.shape != GenShape::Canyons;

    // Desembocaduras: celdas de tierra que desaguan en el mar o fuera del mapa.
    std::vector<int> mouths;
    for (std::size_t i = 0; i < g.h.size(); ++i) {
        const int r = hy.receiver[i];
        if (r < 0) continue;
        const bool land = !sea || g.h[i] > s.sea_level;
        const bool outlet = hy.receiver[static_cast<std::size_t>(r)] < 0;
        if (land && outlet) mouths.push_back(static_cast<int>(i));
    }
    std::sort(mouths.begin(), mouths.end(), [&](int a, int b) {
        return hy.accumulation[static_cast<std::size_t>(a)] > hy.accumulation[static_cast<std::size_t>(b)];
    });

    // Hijos de cada celda (quien le desagua): para subir por el rio.
    std::vector<int> first_child(g.h.size(), -1);
    std::vector<int> next_sibling(g.h.size(), -1);
    for (std::size_t i = 0; i < g.h.size(); ++i) {
        const int r = hy.receiver[i];
        if (r < 0) continue;
        next_sibling[i] = first_child[static_cast<std::size_t>(r)];
        first_child[static_cast<std::size_t>(r)] = static_cast<int>(i);
    }

    const float min_separation = s.size * 0.08f / cell;
    std::vector<int> chosen;
    const float km2 = cell * cell / 1e6f;
    for (int mouth : mouths) {
        if (static_cast<int>(chosen.size()) >= s.rivers) break;
        if (hy.accumulation[static_cast<std::size_t>(mouth)] * km2 < 0.4f) break;  // menos de 0.4 km2: arroyo
        const int mx = mouth % n;
        const int my = mouth / n;
        bool far = true;
        for (int c : chosen) {
            const float ddx = static_cast<float>(c % n - mx);
            const float ddy = static_cast<float>(c / n - my);
            if (ddx * ddx + ddy * ddy < min_separation * min_separation) far = false;
        }
        if (far) chosen.push_back(mouth);
    }

    for (int mouth : chosen) {
        // Subir hasta que el caudal sea el 2 % del de la desembocadura.
        const float stop = std::max(hy.accumulation[static_cast<std::size_t>(mouth)] * 0.02f, 0.05f / km2);
        std::vector<int> path{mouth};
        int c = mouth;
        while (true) {
            int best = -1;
            float best_acc = 0.0f;
            for (int ch = first_child[static_cast<std::size_t>(c)]; ch >= 0; ch = next_sibling[static_cast<std::size_t>(ch)]) {
                if (hy.accumulation[static_cast<std::size_t>(ch)] > best_acc) {
                    best_acc = hy.accumulation[static_cast<std::size_t>(ch)];
                    best = ch;
                }
            }
            if (best < 0 || best_acc < stop) break;
            path.push_back(best);
            c = best;
        }
        if (path.size() < 12) continue;
        std::reverse(path.begin(), path.end());  // del nacimiento a la desembocadura

        // Anchura y profundidad por el area que desagua; el lecho siempre baja.
        const std::size_t count = path.size();
        std::vector<float> width(count);
        std::vector<float> depth(count);
        std::vector<float> bed(count);
        float previous_bed = 1e9f;
        for (std::size_t i = 0; i < count; ++i) {
            const auto p = static_cast<std::size_t>(path[i]);
            const float area = hy.accumulation[p] * km2;
            width[i] = s.river_width * std::clamp(3.0f + 9.0f * std::sqrt(area), 3.0f, 80.0f);
            depth[i] = s.river_depth * std::clamp(0.8f + 0.9f * std::sqrt(area), 0.8f, 7.0f);
            const float target = hy.filled[p] - depth[i] / s.height;
            previous_bed = std::min(previous_bed, target);
            bed[i] = previous_bed;
        }
        // Excavar: fondo parabolico dentro del cauce y orillas suaves.
        for (std::size_t i = 0; i < count; ++i) {
            const int px = path[i] % n;
            const int py = path[i] / n;
            const float half = width[i] * 0.5f / cell;
            const float bank = std::max(width[i] * 0.8f / cell, 2.0f);
            const int reach = static_cast<int>(std::ceil(half + bank)) + 1;
            for (int yy = std::max(py - reach, 0); yy <= std::min(py + reach, n - 1); ++yy) {
                for (int xx = std::max(px - reach, 0); xx <= std::min(px + reach, n - 1); ++xx) {
                    const float dist = std::sqrt(static_cast<float>((xx - px) * (xx - px) + (yy - py) * (yy - py)));
                    if (dist > half + bank) continue;
                    const float t = std::min(dist / std::max(half, 0.5f), 1.0f);
                    const float channel = bed[i] + t * t * depth[i] * 0.9f / s.height;
                    const float blend = smoothstep(half, half + bank, dist);
                    float& h = g.at(xx, yy);
                    const float carved = lerp(channel, h, blend);
                    if (carved < h) h = carved;
                    const std::size_t mi = static_cast<std::size_t>(yy) * n + xx;
                    river_mask[mi] = std::max(river_mask[mi], 1.0f - smoothstep(half * 0.6f, half + bank, dist));
                }
            }
        }
        // Puntos de la superficie (cada ~ancho, suavizados).
        GenRiver river;
        const float spacing = 1.0f;
        float accumulated = 1e9f;
        for (std::size_t i = 0; i < count; ++i) {
            accumulated += cell;
            const bool last = i + 1 == count;
            if (accumulated < std::max(width[i] * spacing, cell * 3.0f) && !last) continue;
            accumulated = 0.0f;
            const int px = path[i] % n;
            const int py = path[i] / n;
            float surface = bed[i] + depth[i] * 0.7f / s.height;
            if (sea && last) surface = std::max(surface, s.sea_level);
            river.points.push_back(Vec3{result.origin.x + static_cast<float>(px) * cell,
                                        result.origin.y + surface * s.height,
                                        result.origin.z + static_cast<float>(py) * cell});
            river.widths.push_back(width[i]);
        }
        // Chaikin (2 veces), sin mover los extremos.
        for (int iter = 0; iter < 2 && river.points.size() > 3; ++iter) {
            std::vector<Vec3> p2{river.points.front()};
            std::vector<float> w2{river.widths.front()};
            for (std::size_t i = 0; i + 1 < river.points.size(); ++i) {
                const Vec3& a = river.points[i];
                const Vec3& b = river.points[i + 1];
                p2.push_back(a * 0.75f + b * 0.25f);
                p2.push_back(a * 0.25f + b * 0.75f);
                w2.push_back(lerp(river.widths[i], river.widths[i + 1], 0.25f));
                w2.push_back(lerp(river.widths[i], river.widths[i + 1], 0.75f));
            }
            p2.push_back(river.points.back());
            w2.push_back(river.widths.back());
            river.points = std::move(p2);
            river.widths = std::move(w2);
        }
        // El agua siempre baja (el suavizado no la hace subir).
        for (std::size_t i = 1; i < river.points.size(); ++i) {
            river.points[i].y = std::min(river.points[i].y, river.points[i - 1].y);
        }
        if (river.points.size() >= 2) result.rivers.push_back(std::move(river));
    }
}

// Lagos: cuencas hondas y grandes (sin tocar el borde ni el mar).
void findLakes(const GenSettings& s, Grid& g, const Hydrology& hy, std::vector<float>& lake_mask, GenResult& result) {
    if (!s.lakes) return;
    const int n = g.n;
    const float cell = s.size / static_cast<float>(n - 1);
    const float min_depth = s.lake_min_depth / s.height;
    std::vector<std::uint8_t> seen(g.h.size(), 0);
    const bool sea = s.ocean && s.shape != GenShape::Mountains && s.shape != GenShape::Canyons;
    for (int y = 1; y < n - 1; ++y) {
        for (int x = 1; x < n - 1; ++x) {
            const std::size_t start = static_cast<std::size_t>(y) * n + x;
            if (seen[start] || hy.filled[start] - g.h[start] < min_depth * 0.5f) continue;
            // Componente de celdas inundadas (lo que se relleno).
            std::vector<int> stack{static_cast<int>(start)};
            std::vector<int> cells;
            seen[start] = 1;
            float level = 0.0f;
            float deepest = 0.0f;
            int x0 = x, x1 = x, y0 = y, y1 = y;
            while (!stack.empty()) {
                const int c = stack.back();
                stack.pop_back();
                cells.push_back(c);
                const auto ci = static_cast<std::size_t>(c);
                level = std::max(level, hy.filled[ci]);
                deepest = std::max(deepest, hy.filled[ci] - g.h[ci]);
                const int cx = c % n;
                const int cy = c / n;
                x0 = std::min(x0, cx);
                x1 = std::max(x1, cx);
                y0 = std::min(y0, cy);
                y1 = std::max(y1, cy);
                constexpr int dx[4] = {1, -1, 0, 0};
                constexpr int dy[4] = {0, 0, 1, -1};
                for (int k = 0; k < 4; ++k) {
                    const int nx = cx + dx[k];
                    const int ny = cy + dy[k];
                    if (nx < 0 || ny < 0 || nx >= n || ny >= n) continue;
                    const std::size_t j = static_cast<std::size_t>(ny) * n + nx;
                    if (seen[j] || hy.filled[j] - g.h[j] < 1e-4f) continue;
                    seen[j] = 1;
                    stack.push_back(static_cast<int>(j));
                }
            }
            const float area = static_cast<float>(cells.size()) * cell * cell;
            if (deepest < min_depth || area < 1500.0f) continue;
            if (sea && level <= s.sea_level + 0.5f / s.height) continue;
            // Lo que quede por debajo del nivel dentro del rectangulo y fuera
            // del lago (el valle de salida) se veria bajo el agua: si es mucho,
            // no es un buen lago; si es poco, se sube un poco (una presa).
            int outside_low = 0;
            int box = 0;
            std::vector<std::uint8_t> in_lake;
            for (int yy = y0; yy <= y1; ++yy) {
                for (int xx = x0; xx <= x1; ++xx) {
                    ++box;
                    const std::size_t j = static_cast<std::size_t>(yy) * n + xx;
                    if (hy.filled[j] - g.h[j] < 1e-4f && g.h[j] < level) ++outside_low;
                }
            }
            if (outside_low > box / 8) continue;
            for (int yy = y0; yy <= y1; ++yy) {
                for (int xx = x0; xx <= x1; ++xx) {
                    const std::size_t j = static_cast<std::size_t>(yy) * n + xx;
                    if (hy.filled[j] - g.h[j] < 1e-4f && g.h[j] < level + 0.3f / s.height) {
                        g.h[j] = level + 0.3f / s.height;
                    }
                }
            }
            for (int c : cells) lake_mask[static_cast<std::size_t>(c)] = 1.0f;
            GenLake lake;
            lake.center = Vec3{result.origin.x + (static_cast<float>(x0 + x1) * 0.5f) * cell,
                               result.origin.y + level * s.height - 0.15f,
                               result.origin.z + (static_cast<float>(y0 + y1) * 0.5f) * cell};
            lake.size = Vec2{static_cast<float>(x1 - x0 + 2) * cell, static_cast<float>(y1 - y0 + 2) * cell};
            result.lakes.push_back(lake);
        }
    }
}

// -----------------------------------------------------------------------------
// 4. Capas
// -----------------------------------------------------------------------------

void paintLayers(const GenSettings& s, const Grid& g, const std::vector<float>& deposit_map,
                 const Hydrology& hy, const std::vector<float>& river_mask, const std::vector<float>& lake_mask,
                 TerrainData& data) {
    const int n = g.n;
    const int sr = static_cast<int>(data.splatResolution());
    const float cell = s.size / static_cast<float>(n - 1);
    const bool sea = s.ocean && s.shape != GenShape::Mountains && s.shape != GenShape::Canyons;
    const float land_range = s.height * (1.0f - s.sea_level);
    float max_log_acc = 1.0f;
    for (float a : hy.accumulation) max_log_acc = std::max(max_log_acc, std::log(a));
    const std::uint32_t seed = s.seed * 131U + 7U;

    Grid dep{n, deposit_map};
    Grid riv{n, river_mask};
    Grid lak{n, lake_mask};

    parallelFor(sr, [&](int sy) {
        for (int sx = 0; sx < sr; ++sx) {
            const float u = (static_cast<float>(sx) + 0.5f) / static_cast<float>(sr);
            const float v = (static_cast<float>(sy) + 0.5f) / static_cast<float>(sr);
            const float gx = u * static_cast<float>(n - 1);
            const float gy = v * static_cast<float>(n - 1);
            const float h = g.bilinear(gx, gy);
            const float hx = g.bilinear(gx + 1.0f, gy) - g.bilinear(gx - 1.0f, gy);
            const float hy2 = g.bilinear(gx, gy + 1.0f) - g.bilinear(gx, gy - 1.0f);
            const float gradient = std::sqrt(hx * hx + hy2 * hy2) * s.height / (2.0f * cell);
            const float slope = std::atan(gradient) * 180.0f / kPi;
            const float above = (h - s.sea_level) * s.height;  // metros sobre el mar
            const int ix = std::clamp(static_cast<int>(gx + 0.5f), 0, n - 1);
            const int iy = std::clamp(static_cast<int>(gy + 0.5f), 0, n - 1);
            const float wet = std::log(hy.accumulation[static_cast<std::size_t>(iy) * n + ix]) / max_log_acc;
            const float deposit = dep.bilinear(gx, gy) * s.height;  // metros depositados
            const float river = riv.bilinear(gx, gy);
            const float lake = lak.bilinear(gx, gy);
            const float wx = u * s.size;
            const float wz = v * s.size;
            const float patch = fbm(wx * 0.012f, wz * 0.012f, seed + 1U, 4) * 0.5f + 0.5f;
            const float fine = fbm(wx * 0.06f, wz * 0.06f, seed + 2U, 3) * 0.5f + 0.5f;

            std::array<float, 8> w{};
            // Roca en los taludes (con el borde roto por el ruido).
            const float rock = smoothstep(s.rock_slope - 7.0f, s.rock_slope + 5.0f, slope + (fine - 0.5f) * 10.0f);
            // Nieve arriba y no en lo muy empinado.
            const float snow_height = s.snow_line * land_range;
            const float snow = sea || s.shape == GenShape::Mountains || s.shape == GenShape::Canyons
                                   ? smoothstep(snow_height - 25.0f, snow_height + 25.0f,
                                                above + (patch - 0.5f) * 60.0f) *
                                         (1.0f - smoothstep(38.0f, 52.0f, slope))
                                   : 0.0f;
            // Playa: la franja sobre el mar, poco inclinada, y el fondo marino.
            float sand = 0.0f;
            if (sea) {
                sand = smoothstep(s.beach_width + 3.0f, s.beach_width - 3.0f, above + (fine - 0.5f) * 4.0f) *
                       (1.0f - smoothstep(22.0f, 34.0f, slope));
                if (above < 0.0f) sand = 1.0f;
            }
            // Grava: sedimentos de la erosion y el lecho de los rios.
            // (Solo en los cauces por los que corre el agua: las llanuras de
            // sedimentos de verdad estan cubiertas de hierba.)
            const float gravel = std::clamp(deposit * 0.5f - 0.1f, 0.0f, 1.0f) * smoothstep(0.55f, 0.8f, wet) *
                                     (1.0f - rock) +
                                 river * 0.8f;
            // Barro: orillas y zonas por las que pasa mucha agua, abajo.
            const float mud = std::clamp(river * 1.5f - 0.3f, 0.0f, 1.0f) * 0.6f + lake * 0.8f +
                              smoothstep(0.62f, 0.8f, wet) * smoothstep(60.0f, 5.0f, above) * 0.5f;
            // Tierra: laderas medias y claros.
            // (La vegetacion aguanta hasta ~35 grados: la tierra asoma cerca
            // de la roca, no en cualquier ladera.)
            const float dirt = smoothstep(s.rock_slope - 12.0f, s.rock_slope - 2.0f, slope) * (1.0f - rock) * 0.8f +
                               smoothstep(0.78f, 0.92f, patch) * 0.35f;
            // Hierba seca: manchas en lo seco y lo alto.
            const float dry = s.dry_grass * smoothstep(0.45f, 0.75f, 1.0f - wet * 0.6f + (patch - 0.5f)) *
                              (0.5f + 0.5f * smoothstep(0.1f * land_range, 0.5f * land_range, above));

            w[kGenLayerRock] = rock;
            w[kGenLayerSnow] = snow;
            w[kGenLayerSand] = sand;
            w[kGenLayerGravel] = gravel;
            w[kGenLayerMud] = mud;
            w[kGenLayerDirt] = dirt;
            w[kGenLayerDryGrass] = dry;
            // Cada capa tapa a las de "debajo" en este orden (de mas a menos
            // dominante): nieve, roca, arena, grava, barro, tierra, seca, hierba.
            std::array<float, 8> final_w{};
            float remaining = 1.0f;
            for (int layer : {kGenLayerSnow, kGenLayerRock, kGenLayerSand, kGenLayerGravel, kGenLayerMud,
                              kGenLayerDirt, kGenLayerDryGrass}) {
                const float take = std::clamp(w[static_cast<std::size_t>(layer)], 0.0f, 1.0f) * remaining;
                final_w[static_cast<std::size_t>(layer)] = take;
                remaining -= take;
            }
            final_w[kGenLayerGrass] = remaining;

            int total = 0;
            std::array<int, 8> bytes{};
            for (int l = 0; l < 8; ++l) {
                bytes[static_cast<std::size_t>(l)] =
                    static_cast<int>(final_w[static_cast<std::size_t>(l)] * 255.0f + 0.5f);
                total += bytes[static_cast<std::size_t>(l)];
            }
            bytes[kGenLayerGrass] += 255 - total;  // que sumen exactamente 255
            if (bytes[kGenLayerGrass] < 0) {
                int biggest = 0;
                for (int l = 1; l < 8; ++l) {
                    if (bytes[static_cast<std::size_t>(l)] > bytes[static_cast<std::size_t>(biggest)]) biggest = l;
                }
                bytes[static_cast<std::size_t>(biggest)] += bytes[kGenLayerGrass];
                bytes[kGenLayerGrass] = 0;
            }
            for (int l = 0; l < 8; ++l) {
                if (std::uint8_t* p = data.weightPtr(l, static_cast<std::uint32_t>(sx), static_cast<std::uint32_t>(sy))) {
                    *p = static_cast<std::uint8_t>(std::clamp(bytes[static_cast<std::size_t>(l)], 0, 255));
                }
            }
        }
    });
}

// -----------------------------------------------------------------------------
// Texturas de las capas
// -----------------------------------------------------------------------------

struct Rgb {
    float r, g, b;
};

Rgb mixRgb(Rgb a, Rgb b, float t) { return {lerp(a.r, b.r, t), lerp(a.g, b.g, t), lerp(a.b, b.b, t)}; }

// Una capa: color (sRGB 0..1) y altura (0..1) en (u, v) 0..1, repetible.
using LayerFunction = std::function<void(float u, float v, Rgb& color, float& height)>;

LayerFunction layerFunction(int layer, std::uint32_t seed) {
    const auto tile_fbm = [seed](float u, float v, float cells, int octaves, std::uint32_t salt, float gain = 0.5f) {
        const int period = static_cast<int>(cells);
        return fbm(u * cells, v * cells, seed + salt, octaves, gain, period);
    };
    switch (layer) {
        case kGenLayerGrass:
            return [tile_fbm](float u, float v, Rgb& c, float& h) {
                const float big = tile_fbm(u, v, 4.0f, 4, 1) * 0.5f + 0.5f;
                const float clumps = tile_fbm(u, v, 16.0f, 3, 2) * 0.5f + 0.5f;
                const float blades = tile_fbm(u * 1.0f, v * 0.25f, 128.0f, 2, 3) * 0.5f + 0.5f;
                c = mixRgb({0.1f, 0.17f, 0.05f}, {0.24f, 0.33f, 0.1f}, big);
                c = mixRgb(c, {0.33f, 0.34f, 0.14f}, smoothstep(0.62f, 0.85f, clumps) * 0.4f);
                c = mixRgb(c, {0.08f, 0.14f, 0.04f}, (1.0f - blades) * 0.35f);
                h = clumps * 0.4f + blades * 0.6f;
            };
        case kGenLayerDryGrass:
            return [tile_fbm](float u, float v, Rgb& c, float& h) {
                const float big = tile_fbm(u, v, 4.0f, 4, 11) * 0.5f + 0.5f;
                const float blades = tile_fbm(u * 0.3f, v * 1.0f, 128.0f, 2, 12) * 0.5f + 0.5f;
                c = mixRgb({0.36f, 0.31f, 0.17f}, {0.52f, 0.46f, 0.27f}, big);
                c = mixRgb(c, {0.3f, 0.26f, 0.14f}, (1.0f - blades) * 0.4f);
                h = blades;
            };
        case kGenLayerDirt:
            return [tile_fbm, seed](float u, float v, Rgb& c, float& h) {
                const float base = tile_fbm(u, v, 6.0f, 5, 21) * 0.5f + 0.5f;
                float f1;
                float f2;
                std::uint32_t id;
                voronoi(u * 24.0f, v * 24.0f, seed + 22U, 24, f1, f2, id);
                const bool pebble = (id & 7U) == 0U && f1 < 0.3f;
                c = mixRgb({0.2f, 0.14f, 0.09f}, {0.38f, 0.29f, 0.2f}, base);
                h = base * 0.5f;
                if (pebble) {
                    const float dome = 1.0f - f1 / 0.3f;
                    c = mixRgb(c, {0.42f, 0.4f, 0.36f}, smoothstep(0.0f, 0.3f, dome));
                    h += dome * 0.5f;
                }
                const float moist = tile_fbm(u, v, 3.0f, 3, 23);
                c = mixRgb(c, {0.13f, 0.09f, 0.06f}, smoothstep(0.2f, 0.6f, moist) * 0.5f);
            };
        case kGenLayerRock:
            return [tile_fbm, seed](float u, float v, Rgb& c, float& h) {
                const float warp = tile_fbm(u, v, 3.0f, 4, 31) * 0.08f;
                const float strata = std::sin((v + warp) * 2.0f * kPi * 14.0f) * 0.5f + 0.5f;
                const float detail = tile_fbm(u, v, 12.0f, 5, 32) * 0.5f + 0.5f;
                // Fracturas finas e irregulares (ruido "ridged"), no celdas: las
                // celdas de Voronoi con grietas oscuras se veian como un mosaico
                // de poligonos repetido en todas las laderas.
                const float ridge = 1.0f - std::abs(tile_fbm(u + warp, v, 5.0f, 4, 35));
                const float gate = smoothstep(-0.1f, 0.3f, tile_fbm(u, v, 3.0f, 2, 36));
                const float crack = smoothstep(0.9f, 0.985f, ridge) * gate;
                (void)seed;
                const float tone = tile_fbm(u, v, 2.0f, 3, 37) * 0.5f + 0.5f;
                c = mixRgb({0.3f, 0.29f, 0.27f}, {0.52f, 0.5f, 0.46f}, detail * 0.7f + tone * 0.3f);
                c = mixRgb(c, {0.44f, 0.38f, 0.31f}, strata * 0.25f);
                c = mixRgb(c, {0.16f, 0.15f, 0.14f}, crack * 0.55f);
                const float lichen = smoothstep(0.7f, 0.9f, tile_fbm(u, v, 10.0f, 3, 34) * 0.5f + 0.5f);
                c = mixRgb(c, {0.45f, 0.47f, 0.3f}, lichen * 0.35f);
                h = detail * 0.5f + strata * 0.2f + (1.0f - crack) * 0.3f + tone * 0.15f;
            };
        case kGenLayerSand:
            return [tile_fbm](float u, float v, Rgb& c, float& h) {
                const float warp = tile_fbm(u, v, 2.0f, 3, 41) * 0.15f;
                const float ripples = std::sin((u + v * 0.35f + warp) * 2.0f * kPi * 22.0f) * 0.5f + 0.5f;
                const float grain = tile_fbm(u, v, 96.0f, 2, 42) * 0.5f + 0.5f;
                const float big = tile_fbm(u, v, 4.0f, 3, 43) * 0.5f + 0.5f;
                c = mixRgb({0.66f, 0.58f, 0.43f}, {0.8f, 0.73f, 0.57f}, big * 0.6f + grain * 0.4f);
                h = ripples * 0.6f + grain * 0.4f;
            };
        case kGenLayerGravel:
            return [seed, tile_fbm](float u, float v, Rgb& c, float& h) {
                float f1;
                float f2;
                std::uint32_t id;
                voronoi(u * 20.0f, v * 20.0f, seed + 51U, 20, f1, f2, id);
                const float edge = smoothstep(0.0f, 0.12f, f2 - f1);
                const float tone = static_cast<float>(id & 255U) / 255.0f;
                const float warm = static_cast<float>((id >> 8) & 255U) / 255.0f;
                c = mixRgb({0.34f, 0.33f, 0.31f}, {0.58f, 0.55f, 0.5f}, tone);
                c = mixRgb(c, {0.5f, 0.42f, 0.33f}, warm * 0.35f);
                c = mixRgb({0.12f, 0.1f, 0.08f}, c, edge);
                const float grain = tile_fbm(u, v, 64.0f, 2, 52) * 0.5f + 0.5f;
                h = edge * (0.7f + 0.3f * (1.0f - f1)) + grain * 0.1f;
            };
        case kGenLayerSnow:
            return [tile_fbm](float u, float v, Rgb& c, float& h) {
                const float soft = tile_fbm(u, v, 4.0f, 5, 61) * 0.5f + 0.5f;
                const float fine = tile_fbm(u, v, 48.0f, 2, 62) * 0.5f + 0.5f;
                c = mixRgb({0.78f, 0.82f, 0.88f}, {0.95f, 0.96f, 0.98f}, soft * 0.7f + fine * 0.3f);
                h = soft * 0.8f + fine * 0.2f;
            };
        default:  // barro
            return [tile_fbm](float u, float v, Rgb& c, float& h) {
                const float base = tile_fbm(u, v, 5.0f, 5, 71) * 0.5f + 0.5f;
                const float puddle = smoothstep(0.62f, 0.7f, tile_fbm(u, v, 3.0f, 4, 72) * 0.5f + 0.5f);
                c = mixRgb({0.13f, 0.1f, 0.07f}, {0.27f, 0.21f, 0.15f}, base);
                c = mixRgb(c, {0.08f, 0.07f, 0.06f}, puddle * 0.6f);
                h = base * (1.0f - puddle * 0.8f);
            };
    }
}

constexpr std::array<const char*, 8> kLayerFiles = {"Hierba", "HierbaSeca", "Tierra", "Roca",
                                                    "Arena",  "Grava",      "Nieve",  "Barro"};

}  // namespace

const char* genShapeName(GenShape shape) {
    switch (shape) {
        case GenShape::Island: return "Isla";
        case GenShape::Archipelago: return "Archipielago";
        case GenShape::Continent: return "Continente";
        case GenShape::Mountains: return "Cordilleras";
        case GenShape::Canyons: return "Canones";
    }
    return "Isla";
}

std::vector<TerrainLayer> generatorLayers(const std::string& texture_folder) {
    struct Def {
        const char* name;
        Vec3 tint;
        float tiling;
        float roughness;
        float normal;
    };
    const std::array<Def, 8> defs = {{{"Hierba", {0.3f, 0.42f, 0.16f}, 4.0f, 0.95f, 1.0f},
                                      {"Hierba seca", {0.55f, 0.48f, 0.27f}, 4.0f, 0.95f, 0.8f},
                                      {"Tierra", {0.33f, 0.25f, 0.17f}, 5.0f, 0.95f, 1.2f},
                                      {"Roca", {0.45f, 0.43f, 0.4f}, 10.0f, 0.8f, 1.6f},
                                      {"Arena", {0.74f, 0.66f, 0.5f}, 5.0f, 0.9f, 0.8f},
                                      {"Grava", {0.45f, 0.43f, 0.4f}, 3.0f, 0.85f, 1.5f},
                                      {"Nieve", {0.9f, 0.92f, 0.95f}, 8.0f, 0.6f, 0.6f},
                                      {"Barro", {0.2f, 0.16f, 0.11f}, 5.0f, 0.45f, 1.0f}}};
    std::vector<TerrainLayer> layers;
    for (std::size_t i = 0; i < defs.size(); ++i) {
        TerrainLayer l;
        l.name = defs[i].name;
        l.tiling = defs[i].tiling;
        l.roughness = defs[i].roughness;
        l.normal_strength = defs[i].normal;
        if (texture_folder.empty()) {
            l.tint = defs[i].tint;
        } else {
            l.tint = Vec3{1.0f, 1.0f, 1.0f};
            l.albedo = texture_folder + "/" + kLayerFiles[i] + "_Color.png";
            l.normal = texture_folder + "/" + kLayerFiles[i] + "_Normal.png";
        }
        layers.push_back(l);
    }
    return layers;
}

// Sube al cambiar como se generan las texturas: los proyectos las rehacen.
constexpr const char* kTextureVersion = "cramion-terrain-textures-3";

bool generatorTexturesCurrent(const std::string& folder) {
    std::FILE* f = std::fopen((std::filesystem::path(folder) / "version.txt").string().c_str(), "rb");
    if (f == nullptr) return false;
    char buffer[64] = {};
    const std::size_t read = std::fread(buffer, 1, sizeof(buffer) - 1, f);
    std::fclose(f);
    return std::string(buffer, read) == kTextureVersion;
}

bool writeGeneratorTextures(const std::string& folder, int size, std::uint32_t seed) {
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    bool ok = true;
    for (int layer = 0; layer < 8; ++layer) {
        const LayerFunction f = layerFunction(layer, seed + static_cast<std::uint32_t>(layer) * 97U);
        const auto n = static_cast<std::size_t>(size);
        std::vector<float> height(n * n);
        asset::ImageRgba8 color;
        color.width = static_cast<std::uint32_t>(size);
        color.height = static_cast<std::uint32_t>(size);
        color.pixels.resize(n * n * 4);
        parallelFor(size, [&](int y) {
            for (int x = 0; x < size; ++x) {
                Rgb c{};
                float h = 0.0f;
                f((static_cast<float>(x) + 0.5f) / static_cast<float>(size),
                  (static_cast<float>(y) + 0.5f) / static_cast<float>(size), c, h);
                const std::size_t i = static_cast<std::size_t>(y) * n + static_cast<std::size_t>(x);
                height[i] = h;
                color.pixels[i * 4 + 0] = static_cast<std::uint8_t>(std::clamp(c.r, 0.0f, 1.0f) * 255.0f + 0.5f);
                color.pixels[i * 4 + 1] = static_cast<std::uint8_t>(std::clamp(c.g, 0.0f, 1.0f) * 255.0f + 0.5f);
                color.pixels[i * 4 + 2] = static_cast<std::uint8_t>(std::clamp(c.b, 0.0f, 1.0f) * 255.0f + 0.5f);
                color.pixels[i * 4 + 3] = 255;
            }
        });
        // Normal map (OpenGL, +Y arriba) de la altura, con los bordes que se repiten.
        asset::ImageRgba8 normal;
        normal.width = color.width;
        normal.height = color.height;
        normal.pixels.resize(n * n * 4);
        const float strength = 4.0f;
        parallelFor(size, [&](int y) {
            for (int x = 0; x < size; ++x) {
                const auto at = [&](int xx, int yy) {
                    xx = (xx + size) % size;
                    yy = (yy + size) % size;
                    return height[static_cast<std::size_t>(yy) * n + static_cast<std::size_t>(xx)];
                };
                const float dx = (at(x + 1, y) - at(x - 1, y)) * strength;
                const float dy = (at(x, y + 1) - at(x, y - 1)) * strength;
                float nx = -dx;
                float ny = dy;  // +Y arriba en la imagen
                float nz = 1.0f;
                const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
                nx /= len;
                ny /= len;
                nz /= len;
                const std::size_t i = (static_cast<std::size_t>(y) * n + static_cast<std::size_t>(x)) * 4;
                normal.pixels[i + 0] = static_cast<std::uint8_t>((nx * 0.5f + 0.5f) * 255.0f + 0.5f);
                normal.pixels[i + 1] = static_cast<std::uint8_t>((ny * 0.5f + 0.5f) * 255.0f + 0.5f);
                normal.pixels[i + 2] = static_cast<std::uint8_t>((nz * 0.5f + 0.5f) * 255.0f + 0.5f);
                normal.pixels[i + 3] = 255;
            }
        });
        const std::filesystem::path base = std::filesystem::path(folder) / kLayerFiles[static_cast<std::size_t>(layer)];
        ok = asset::saveImagePng(base.string() + "_Color.png", color) && ok;
        ok = asset::saveImagePng(base.string() + "_Normal.png", normal) && ok;
    }
    if (ok) {
        if (std::FILE* f = std::fopen((std::filesystem::path(folder) / "version.txt").string().c_str(), "wb")) {
            std::fputs(kTextureVersion, f);
            std::fclose(f);
        }
    }
    return ok;
}

bool generateTerrain(const GenSettings& settings, GenResult& result, const GenProgress& progress) {
    const auto started = std::chrono::steady_clock::now();
    GenSettings s = settings;
    s.resolution = std::clamp(s.resolution, 129, 4097);
    s.splat_resolution = std::clamp(s.splat_resolution, 64, 4096);
    s.sea_level = std::clamp(s.sea_level, 0.0f, 0.9f);
    const bool sea = s.ocean && s.shape != GenShape::Mountains && s.shape != GenShape::Canyons;
    if (!sea) s.ocean = false;
    const auto stage = [&](float t, const char* name) { return !progress || progress(t, name); };

    result = GenResult{};
    result.origin = Vec3{-s.size * 0.5f, -s.sea_level * s.height, -s.size * 0.5f};

    Grid g;
    g.n = s.resolution;
    g.h.assign(static_cast<std::size_t>(g.n) * static_cast<std::size_t>(g.n), 0.0f);

    if (!stage(0.02f, "Forma")) return false;
    generateShape(s, g);

    if (!stage(0.15f, "Erosion termica")) return false;
    thermalErosion(s, g, static_cast<int>(4.0f + s.thermal * 30.0f));

    std::vector<float> deposit_map(g.h.size(), 0.0f);
    bool cancelled = false;
    result.erosion_volume = hydraulicErosion(s, g, deposit_map, progress, 0.2f, 0.72f, cancelled);
    if (cancelled) return false;
    // Un repaso termico suaviza los bordes que deja la erosion.
    thermalErosion(s, g, static_cast<int>(2.0f + s.thermal * 6.0f));

    if (!stage(0.75f, "Rios y lagos")) return false;
    std::vector<float> river_mask(g.h.size(), 0.0f);
    std::vector<float> lake_mask(g.h.size(), 0.0f);
    {
        const Hydrology hy = computeHydrology(s, g);
        carveRivers(s, g, hy, river_mask, result);
    }
    // Despues de excavar, el agua cambia de camino: se recalcula para los lagos
    // y la humedad de las capas.
    const Hydrology hy = computeHydrology(s, g);
    findLakes(s, g, hy, lake_mask, result);

    if (!stage(0.88f, "Capas")) return false;
    for (float& h : g.h) h = std::clamp(h, 0.0f, 1.0f);
    result.data.create(static_cast<std::uint32_t>(s.resolution), static_cast<std::uint32_t>(s.splat_resolution), 0.0f);
    result.data.heights() = g.h;
    paintLayers(s, g, deposit_map, hy, river_mask, lake_mask, result.data);
    result.data.markAll();

    result.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    stage(1.0f, "Listo");
    return true;
}

}  // namespace cramion::terrain
