#include "CramionFX/asset/HouseGenerator.h"

#include "HouseGeo.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <random>
#include <thread>
#include <vector>

namespace cramion::asset {

namespace {

using core::Vec2;
using core::Vec3;
using core::Vec4;
using housegeo::kPi;
using housegeo::kUp;
using housegeo::kX;
using housegeo::kZ;
using housegeo::mix;
using housegeo::smoothstep;

// ---------------------------------------------------------------------------
// Ruido repetible (periodico) para las texturas
// ---------------------------------------------------------------------------

float hash2(int x, int y, std::uint32_t seed) {
    std::uint32_t h = static_cast<std::uint32_t>(x) * 0x8da6b343U ^ static_cast<std::uint32_t>(y) * 0xd8163841U ^
                      seed * 0xcb1ab31fU;
    h ^= h >> 13;
    h *= 0x5bd1e995U;
    h ^= h >> 15;
    return static_cast<float>(h & 0xFFFFFFU) / 16777216.0f;
}

int wrap(int i, int period) { return ((i % period) + period) % period; }

// Ruido de valor con periodo (px, py) celdas: (x, y) en celdas.
float valueNoise(float x, float y, int px, int py, std::uint32_t seed) {
    const float fx = std::floor(x);
    const float fy = std::floor(y);
    const int x0 = static_cast<int>(fx);
    const int y0 = static_cast<int>(fy);
    float tx = x - fx;
    float ty = y - fy;
    tx = tx * tx * tx * (tx * (tx * 6.0f - 15.0f) + 10.0f);
    ty = ty * ty * ty * (ty * (ty * 6.0f - 15.0f) + 10.0f);
    const float a = hash2(wrap(x0, px), wrap(y0, py), seed);
    const float b = hash2(wrap(x0 + 1, px), wrap(y0, py), seed);
    const float c = hash2(wrap(x0, px), wrap(y0 + 1, py), seed);
    const float d = hash2(wrap(x0 + 1, px), wrap(y0 + 1, py), seed);
    return mix(mix(a, b, tx), mix(c, d, tx), ty);
}

// fBm periodico: (u, v) en 0..1, `px`/`py` celdas en la primera octava.
float fbm(float u, float v, int px, int py, int octaves, std::uint32_t seed) {
    float sum = 0.0f;
    float norm = 0.0f;
    float amplitude = 0.5f;
    for (int o = 0; o < octaves; ++o) {
        sum += amplitude * valueNoise(u * static_cast<float>(px), v * static_cast<float>(py), px, py,
                                      seed + static_cast<std::uint32_t>(o) * 1013U);
        norm += amplitude;
        amplitude *= 0.5f;
        px *= 2;
        py *= 2;
    }
    return sum / norm;
}

// Voronoi periodico (nx x ny celdas): distancias a los dos puntos mas
// cercanos (en unidades de celda, con `aspect` = cuanto mas ancha que alta) y
// el id de la celda mas cercana.
struct Cell {
    float f1 = 9.0f;
    float f2 = 9.0f;
    int id = 0;
    Vec2 offset{};  // del punto a su centro
};
Cell voronoi(float u, float v, int nx, int ny, float aspect, std::uint32_t seed) {
    const float x = u * static_cast<float>(nx);
    const float y = v * static_cast<float>(ny);
    const int cx = static_cast<int>(std::floor(x));
    const int cy = static_cast<int>(std::floor(y));
    Cell out;
    for (int j = -2; j <= 2; ++j) {
        for (int i = -2; i <= 2; ++i) {
            const int gx = cx + i;
            const int gy = cy + j;
            const int wx = wrap(gx, nx);
            const int wy = wrap(gy, ny);
            const float px = static_cast<float>(gx) + 0.15f + 0.7f * hash2(wx, wy, seed);
            const float py = static_cast<float>(gy) + 0.15f + 0.7f * hash2(wx, wy, seed + 77U);
            const float dx = (x - px) * aspect;
            const float dy = y - py;
            const float d = std::sqrt(dx * dx + dy * dy);
            if (d < out.f1) {
                out.f2 = out.f1;
                out.f1 = d;
                out.id = wy * nx + wx;
                out.offset = Vec2{dx, dy};
            } else if (d < out.f2) {
                out.f2 = d;
            }
        }
    }
    return out;
}

void parallelRows(int rows, const std::function<void(int)>& fn) {
    const int threads = std::max(1, std::min(static_cast<int>(std::thread::hardware_concurrency()), 16));
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t) {
        pool.emplace_back([&, t] {
            for (int y = t; y < rows; y += threads) fn(y);
        });
    }
    for (std::thread& th : pool) th.join();
}

// Una muestra de material: color (sRGB 0..1), altura (0..1), rugosidad, oclusion.
struct Texel {
    Vec3 color{};
    float height = 0.5f;
    float roughness = 0.8f;
    float occlusion = 1.0f;
};
using TexelFunction = std::function<Texel(float u, float v)>;

// --- Troncos pelados: U a lo largo (2 m), V alrededor (una vuelta) ---
Texel logTexel(float u, float v, std::uint32_t seed) {
    Texel t;
    // Veta: vetas largas a lo largo del tronco.
    const float grain = fbm(u, v, 3, 28, 4, seed);
    const float fine = valueNoise(u * 12.0f, v * 160.0f, 12, 160, seed + 5U);
    // Grietas (fendas) largas y finas.
    const float ridge = 1.0f - std::abs(2.0f * fbm(u, v, 2, 9, 3, seed + 11U) - 1.0f);
    const float gate = smoothstep(0.45f, 0.65f, valueNoise(u * 4.0f, v * 3.0f, 4, 3, seed + 13U));
    const float crack = smoothstep(0.955f, 0.99f, ridge) * gate;
    // Nudos.
    float knot = 0.0f;
    float knot_ring = 0.0f;
    const Cell c = voronoi(u, v, 3, 2, 2.0f, seed + 17U);
    if (hash2(c.id, 3, seed + 19U) < 0.6f) {
        const float r = 0.045f + 0.03f * hash2(c.id, 5, seed);
        const float d = c.f1 * 0.33f;  // ~metros
        knot = 1.0f - smoothstep(r * 0.4f, r, d);
        knot_ring = smoothstep(r * 0.5f, r, d) * (1.0f - smoothstep(r, r * 1.8f, d));
    }
    // Sol y lluvia: zonas grises (madera curtida) y mas oscuras abajo.
    const float weather = fbm(u, v, 2, 3, 3, seed + 23U);
    const Vec3 honey{0.56f, 0.38f, 0.21f};
    const Vec3 dark{0.36f, 0.23f, 0.12f};
    const Vec3 gray{0.52f, 0.47f, 0.4f};
    Vec3 col = mix(dark, honey, std::clamp(grain * 1.3f - 0.1f + (fine - 0.5f) * 0.25f, 0.0f, 1.0f));
    col = mix(col, gray, smoothstep(0.45f, 0.75f, weather) * 0.55f);
    col = mix(col, Vec3{0.25f, 0.15f, 0.08f}, knot * 0.8f + knot_ring * 0.35f);
    col = mix(col, Vec3{0.12f, 0.08f, 0.05f}, crack);
    t.color = col;
    t.height = 0.55f + (grain - 0.5f) * 0.25f + (fine - 0.5f) * 0.08f - crack * 0.5f + knot * 0.06f - knot_ring * 0.05f;
    t.roughness = 0.72f + 0.2f * weather + crack * 0.08f;
    t.occlusion = 1.0f - crack * 0.7f - knot_ring * 0.2f;
    return t;
}

// --- Testa del tronco (disco centrado): anillos, medula y fendas radiales ---
Texel endTexel(float u, float v, std::uint32_t seed) {
    Texel t;
    const float dx = u - 0.5f;
    const float dy = v - 0.5f;
    const float r = std::sqrt(dx * dx + dy * dy) * 2.0f;
    const float a = std::atan2(dy, dx);
    const float wobble = fbm(u, v, 4, 4, 3, seed) * 0.08f;
    const float rings = std::sin((r + wobble) * 2.0f * kPi * 11.0f) * 0.5f + 0.5f;
    const float late = smoothstep(0.55f, 0.9f, rings);
    // Fendas: dos o tres del centro hacia fuera.
    float crack = 0.0f;
    for (int i = 0; i < 3; ++i) {
        const float ca = hash2(i, 1, seed) * 2.0f * kPi;
        const float len = 0.45f + 0.4f * hash2(i, 2, seed);
        float diff = std::abs(std::remainder(a - ca, 2.0f * kPi));
        const float width = 0.03f * (1.0f - r / std::max(len, 0.01f));
        if (r < len && diff < width) crack = std::max(crack, 1.0f - diff / std::max(width, 1e-4f));
    }
    const float grime = fbm(u, v, 3, 3, 3, seed + 3U);
    Vec3 col = mix(Vec3{0.66f, 0.5f, 0.32f}, Vec3{0.47f, 0.33f, 0.19f}, late);
    col = mix(col, Vec3{0.45f, 0.42f, 0.37f}, smoothstep(0.4f, 0.8f, grime) * 0.5f);  // gris del sol
    col = mix(col, Vec3{0.3f, 0.2f, 0.12f}, 1.0f - smoothstep(0.02f, 0.06f, r));      // medula
    col = mix(col, Vec3{0.12f, 0.08f, 0.05f}, crack);
    // Borde: la corteza/albura oscura.
    const float rim = smoothstep(0.88f, 0.95f, r);
    col = mix(col, Vec3{0.32f, 0.21f, 0.12f}, rim);
    t.color = col;
    t.height = 0.6f - late * 0.06f - crack * 0.45f - rim * 0.1f + (grime - 0.5f) * 0.1f;
    t.roughness = 0.85f;
    t.occlusion = 1.0f - crack * 0.6f;
    return t;
}

// --- Tablas verticales curtidas (1.2 x 1.2 m, 6 tablas) ---
Texel plankTexel(float u, float v, std::uint32_t seed) {
    Texel t;
    constexpr int kBoards = 6;
    const float x = u * kBoards;
    const int board = static_cast<int>(std::floor(x));
    const float local = x - std::floor(x);
    const float tint = hash2(board, 0, seed);
    const float shift = hash2(board, 1, seed);
    // Junta a tope en algun punto de la tabla.
    const float seam_v = hash2(board, 2, seed);
    const float seam_d = std::abs(std::remainder(v - seam_v, 1.0f));
    const float seam = hash2(board, 3, seed) < 0.6f ? 1.0f - smoothstep(0.002f, 0.006f, seam_d) : 0.0f;
    const float gap = 1.0f - smoothstep(0.015f, 0.04f, std::min(local, 1.0f - local));
    const float grain = fbm(u + shift, v + shift * 3.0f, 36, 3, 4, seed + 7U);
    const float fine = valueNoise((u + shift) * 240.0f, v * 12.0f, 240, 12, seed + 9U);
    const float weather = fbm(u, v, 3, 3, 3, seed + 11U);
    // Clavos arriba y abajo de cada tramo.
    float nail = 0.0f;
    for (float nv : {0.08f, 0.58f}) {
        for (float nx : {0.3f, 0.7f}) {
            const float dx = (local - nx) * 0.2f;  // metros
            const float dy = std::remainder(v - nv, 1.0f) * 1.2f;
            nail = std::max(nail, 1.0f - smoothstep(0.003f, 0.006f, std::sqrt(dx * dx + dy * dy)));
        }
    }
    const Vec3 brown{0.42f, 0.31f, 0.2f};
    const Vec3 gray{0.5f, 0.47f, 0.42f};
    Vec3 col = mix(brown, gray, std::clamp(0.25f + tint * 0.5f + (weather - 0.5f) * 0.6f, 0.0f, 1.0f));
    col = col * (0.78f + 0.42f * grain + (fine - 0.5f) * 0.12f);
    col = mix(col, Vec3{0.06f, 0.05f, 0.04f}, std::max(gap, seam * 0.8f));
    col = mix(col, Vec3{0.18f, 0.12f, 0.1f}, nail);
    t.color = col;
    t.height = 0.7f + (grain - 0.5f) * 0.12f + (fine - 0.5f) * 0.05f - gap * 0.65f - seam * 0.3f - nail * 0.08f;
    t.roughness = 0.82f + 0.1f * weather;
    t.occlusion = 1.0f - gap * 0.75f - seam * 0.3f;
    return t;
}

// --- Tablas solapadas pintadas (1 x 1 m, 6 hiladas) ---
Texel sidingTexel(float u, float v, std::uint32_t seed) {
    Texel t;
    constexpr int kCourses = 6;
    const float y = v * kCourses;
    const int course = static_cast<int>(std::floor(y));
    const float local = y - std::floor(y);  // 0 arriba de la tabla, 1 abajo (el canto)
    // Junta vertical en algun punto de la hilada.
    const float joint_u = hash2(course, 4, seed);
    const float joint = hash2(course, 5, seed) < 0.5f
                            ? 1.0f - smoothstep(0.0008f, 0.0025f, std::abs(std::remainder(u - joint_u, 1.0f)))
                            : 0.0f;
    const float brush = valueNoise(u * 180.0f, v * 30.0f, 180, 30, seed + 3U);
    const float wear = fbm(u, v, 4, 4, 5, seed + 7U);
    // Se pela sobre todo en el canto de cada tabla (donde escurre el agua).
    const float worn = smoothstep(0.74f, 0.78f, wear * 0.85f + smoothstep(0.8f, 1.0f, local) * 0.22f);
    const Vec3 paint{0.86f, 0.85f, 0.8f};
    const Vec3 wood{0.45f, 0.4f, 0.33f};
    Vec3 col = mix(paint * (0.94f + brush * 0.08f), wood, worn);
    col = col * (0.97f + 0.06f * hash2(course, 6, seed));
    // Sombra bajo el canto de la hilada de arriba.
    const float shadow = 1.0f - smoothstep(0.0f, 0.12f, local);
    col = col * (1.0f - shadow * 0.35f);
    col = mix(col, Vec3{0.2f, 0.19f, 0.17f}, joint * 0.8f);
    t.color = col;
    t.height = 0.25f + 0.65f * local - worn * 0.03f - joint * 0.1f;
    t.roughness = mix(0.5f, 0.85f, worn) + brush * 0.05f;
    t.occlusion = 1.0f - shadow * 0.45f - joint * 0.3f;
    return t;
}

// --- Mamposteria de piedra (1.5 x 1.5 m) ---
Texel stoneTexel(float u, float v, std::uint32_t seed) {
    Texel t;
    // Piedras mas anchas que altas: 5 x 8 celdas con aspecto 1.6.
    const float warp_u = u + (fbm(u, v, 6, 6, 3, seed + 1U) - 0.5f) * 0.03f;
    const float warp_v = v + (fbm(u, v, 6, 6, 3, seed + 2U) - 0.5f) * 0.03f;
    const Cell c = voronoi(warp_u, warp_v, 5, 8, 1.6f, seed);
    const float edge = c.f2 - c.f1;
    const float stone = smoothstep(0.06f, 0.16f, edge);
    const float dome = smoothstep(0.0f, 0.55f, edge);
    const float tone = hash2(c.id, 1, seed);
    const float warm = hash2(c.id, 2, seed);
    const float speck = valueNoise(u * 300.0f, v * 300.0f, 300, 300, seed + 5U);
    const float surf = fbm(u, v, 12, 12, 4, seed + 7U);
    Vec3 rock = mix(Vec3{0.43f, 0.42f, 0.4f}, Vec3{0.6f, 0.58f, 0.54f}, tone);
    rock = mix(rock, Vec3{0.55f, 0.47f, 0.36f}, warm * warm * 0.7f);
    rock = rock * (0.8f + 0.3f * surf + (speck - 0.5f) * 0.18f);
    // Liquen en algunas piedras.
    const float lichen = smoothstep(0.62f, 0.7f, fbm(u, v, 5, 5, 4, seed + 9U)) * (hash2(c.id, 3, seed) < 0.4f ? 1.0f : 0.0f);
    rock = mix(rock, Vec3{0.55f, 0.55f, 0.36f}, lichen * 0.6f);
    const Vec3 mortar = Vec3{0.5f, 0.48f, 0.44f} * (0.85f + 0.3f * speck);
    t.color = mix(mortar, rock, stone);
    t.height = mix(0.12f + speck * 0.05f, 0.45f + dome * 0.45f + (surf - 0.5f) * 0.15f, stone);
    t.roughness = mix(0.95f, 0.78f + 0.15f * surf, stone);
    t.occlusion = mix(0.45f, 0.75f + 0.25f * dome, stone);
    return t;
}

// --- Tablillas de madera del tejado (1 x 1 m, 4 hiladas; V baja por la pendiente) ---
Texel roofTexel(float u, float v, std::uint32_t seed) {
    Texel t;
    constexpr int kRows = 4;
    constexpr int kCols = 7;
    const float y = v * kRows;
    const int row = static_cast<int>(std::floor(y));
    const float local = y - std::floor(y);  // 0 arriba (bajo la hilada anterior), 1 el canto
    // Tablillas de ancho muy variable (bordes desplazados al azar) y hiladas
    // desplazadas entre si.
    const float x = u * kCols + hash2(wrap(row, kRows), 0, seed) * kCols;
    const auto edge = [&](int i) {
        return static_cast<float>(i) + (hash2(wrap(i, kCols), wrap(row, kRows), seed + 1U) - 0.5f) * 0.8f;
    };
    const int i = static_cast<int>(std::floor(x));
    float left = edge(i);
    float right = edge(i + 1);
    int id = i;
    if (x < left) {
        right = left;
        left = edge(i - 1);
        id = i - 1;
    } else if (x > right) {
        left = right;
        right = edge(i + 2);
        id = i + 1;
    }
    const float from_edge = std::min(x - left, right - x) / kCols;  // metros
    const float gap = 1.0f - smoothstep(0.0015f, 0.005f, from_edge);
    const float key = hash2(wrap(id, kCols), wrap(row, kRows), seed + 3U);
    const float key2 = hash2(wrap(id, kCols), wrap(row, kRows), seed + 4U);
    // Veta a lo largo de la tablilla (hacia abajo) y vetas oscuras de la lluvia.
    const float grain = valueNoise(u * 160.0f + key * 37.0f, v * 6.0f, 160, 6, seed + 5U);
    const float streak = fbm(u + key, v, 40, 2, 3, seed + 6U);
    // Algunas rajadas por la mitad.
    const float split = key2 < 0.2f ? 1.0f - smoothstep(0.001f, 0.003f, std::abs(x - mix(left, right, 0.35f + 0.3f * key)) / kCols) *
                                                 smoothstep(0.2f, 0.6f, local)
                                    : 0.0f;
    const float moss = smoothstep(0.6f, 0.8f, fbm(u, v, 3, 3, 4, seed + 7U)) * smoothstep(0.3f, 1.0f, local);
    const float shadow = 1.0f - smoothstep(0.0f, 0.22f, local);
    // Madera curtida: gris parduzco, cada tablilla con su tono.
    Vec3 col = mix(Vec3{0.2f, 0.18f, 0.15f}, Vec3{0.38f, 0.35f, 0.31f}, key);
    col = mix(col, Vec3{0.3f, 0.22f, 0.15f}, key2 * 0.35f);
    col = col * (0.72f + 0.34f * grain + (streak - 0.5f) * 0.3f);
    col = col * (0.9f + 0.2f * local);  // el canto, mas expuesto, mas claro
    col = mix(col, Vec3{0.2f, 0.24f, 0.1f}, moss * 0.5f);
    col = col * (1.0f - shadow * 0.55f);
    col = mix(col, Vec3{0.04f, 0.035f, 0.03f}, std::max(gap, split * 0.8f));
    t.color = col;
    // Cuna: fina arriba y gruesa en el canto; el canto algo irregular.
    t.height = 0.1f + 0.75f * std::pow(local, 1.3f) + (grain - 0.5f) * 0.1f - gap * 0.4f - split * 0.2f +
               (key2 - 0.5f) * 0.06f;
    t.roughness = 0.84f + moss * 0.1f + grain * 0.04f;
    t.occlusion = 1.0f - shadow * 0.6f - gap * 0.5f;
    return t;
}

// --- Madera pintada de los marcos (1 x 1 m, pinceladas a lo largo de U) ---
Texel trimTexel(float u, float v, std::uint32_t seed) {
    Texel t;
    const float brush = valueNoise(u * 12.0f, v * 220.0f, 12, 220, seed);
    const float chip = smoothstep(0.77f, 0.8f, fbm(u, v, 6, 6, 5, seed + 3U));
    const Vec3 paint{0.9f, 0.89f, 0.85f};
    Vec3 col = mix(paint * (0.95f + brush * 0.07f), Vec3{0.45f, 0.41f, 0.35f}, chip);
    t.color = col;
    t.height = 0.6f + brush * 0.04f - chip * 0.08f;
    t.roughness = mix(0.42f, 0.85f, chip) + brush * 0.05f;
    t.occlusion = 1.0f - chip * 0.2f;
    return t;
}


// --- Enlucido de cal (2 x 2 m): manchas, llana, desconchones y grietas ---
Texel plasterTexel(float u, float v, std::uint32_t seed) {
    Texel t;
    const float big = fbm(u, v, 4, 4, 5, seed);
    const float fine = valueNoise(u * 256.0f, v * 256.0f, 256, 256, seed + 3U);
    const float trowel = fbm(u, v, 20, 9, 3, seed + 5U);
    const float ridge = 1.0f - std::abs(2.0f * fbm(u, v, 6, 6, 4, seed + 7U) - 1.0f);
    const float crack = smoothstep(0.965f, 0.99f, ridge) * smoothstep(0.5f, 0.7f, fbm(u, v, 3, 3, 2, seed + 9U));
    const float stain = smoothstep(0.6f, 0.85f, fbm(u, v, 3, 3, 4, seed + 11U));
    // Desconchones: el enlucido se cayo y asoma el barro de debajo.
    const float flake = smoothstep(0.78f, 0.8f, fbm(u, v, 5, 5, 5, seed + 13U));
    const Vec3 base{0.87f, 0.83f, 0.74f};
    Vec3 col = base * (0.9f + 0.12f * big + (fine - 0.5f) * 0.06f + (trowel - 0.5f) * 0.05f);
    col = mix(col, Vec3{0.64f, 0.6f, 0.52f}, stain * 0.4f);
    col = mix(col, Vec3{0.47f, 0.38f, 0.27f} * (0.85f + 0.3f * fine), flake);
    col = mix(col, Vec3{0.34f, 0.31f, 0.27f}, crack);
    t.color = col;
    t.height = 0.6f + (big - 0.5f) * 0.18f + (trowel - 0.5f) * 0.1f + (fine - 0.5f) * 0.05f - crack * 0.4f - flake * 0.18f;
    t.roughness = 0.9f + 0.05f * fine;
    t.occlusion = 1.0f - crack * 0.5f - flake * 0.25f;
    return t;
}

// --- Paja de los tejados (1.5 x 1.5 m, 5 hiladas; V baja por la pendiente) ---
Texel thatchTexel(float u, float v, std::uint32_t seed) {
    Texel t;
    constexpr int kRows = 5;
    const float y = v * kRows;
    const int row = static_cast<int>(std::floor(y));
    const float shift = hash2(wrap(row, kRows), 1, seed) * 37.0f;
    // Borde de abajo de cada hilada desflecado.
    const float ragged = (valueNoise(u * 48.0f + shift, 0.5f, 48, 1, seed + 2U) - 0.5f) * 0.35f;
    const float local = std::clamp(y - std::floor(y) - ragged * 0.3f, 0.0f, 1.0f);
    const float strands = valueNoise(u * 420.0f + shift, v * 5.0f, 420, 5, seed + 3U);
    const float strands2 = valueNoise(u * 160.0f + shift * 0.5f, v * 3.0f, 160, 3, seed + 4U);
    const float weather = fbm(u, v, 3, 3, 4, seed + 5U);
    const float moss = smoothstep(0.66f, 0.84f, fbm(u, v, 4, 4, 4, seed + 6U)) * smoothstep(0.2f, 0.9f, local);
    const float shadow = 1.0f - smoothstep(0.0f, 0.3f, local);
    const Vec3 straw{0.66f, 0.55f, 0.33f};
    const Vec3 aged{0.43f, 0.38f, 0.29f};
    Vec3 col = mix(straw, aged, std::clamp(weather * 1.2f - 0.15f, 0.0f, 1.0f));
    col = col * (0.62f + 0.42f * strands + (strands2 - 0.5f) * 0.25f);
    col = mix(col, Vec3{0.27f, 0.31f, 0.16f}, moss * 0.55f);
    col = col * (1.0f - shadow * 0.5f);
    t.color = col;
    t.height = 0.15f + 0.7f * std::pow(local, 0.8f) + (strands - 0.5f) * 0.22f + (strands2 - 0.5f) * 0.1f;
    t.roughness = 0.95f;
    t.occlusion = 1.0f - shadow * 0.55f - (1.0f - strands) * 0.15f;
    return t;
}

// --- Teja curva de barro (1 x 1 m: 5 hiladas, 6 canales; V baja) ---
Texel tileTexel(float u, float v, std::uint32_t seed) {
    Texel t;
    constexpr int kRows = 5;
    constexpr int kCols = 6;
    const float y = v * kRows;
    const int row = static_cast<int>(std::floor(y));
    const float local = y - std::floor(y);  // 0 arriba (bajo la hilada anterior), 1 el borde
    const float x = u * kCols + (row % 2 == 0 ? 0.0f : 0.5f) * 0.0f;
    const int col_i = static_cast<int>(std::floor(x));
    const float lx = x - std::floor(x);
    const int id_x = wrap(col_i, kCols);
    const int id_y = wrap(row, kRows);
    const float key = hash2(id_x, id_y, seed);
    const float key2 = hash2(id_x, id_y, seed + 1U);
    // Perfil de la teja (media cana) y la junta entre canales.
    const float profile = std::cos((lx - 0.5f) * kPi);
    const float gap = 1.0f - smoothstep(0.02f, 0.07f, std::min(lx, 1.0f - lx));
    const float shadow = 1.0f - smoothstep(0.0f, 0.22f, local);
    const float lip = smoothstep(0.88f, 1.0f, local);
    const float grime = fbm(u, v, 3, 3, 4, seed + 3U);
    const float lichen = smoothstep(0.7f, 0.76f, fbm(u, v, 9, 9, 4, seed + 4U)) * (key2 < 0.5f ? 1.0f : 0.4f);
    const float speck = valueNoise(u * 300.0f, v * 300.0f, 300, 300, seed + 5U);
    Vec3 clay = mix(Vec3{0.68f, 0.34f, 0.19f}, Vec3{0.46f, 0.22f, 0.14f}, key);
    clay = mix(clay, Vec3{0.58f, 0.42f, 0.3f}, key2 * 0.3f);
    clay = clay * (0.85f + 0.25f * profile + (speck - 0.5f) * 0.12f);
    clay = mix(clay, Vec3{0.28f, 0.24f, 0.2f}, smoothstep(0.55f, 0.8f, grime) * 0.45f);
    clay = mix(clay, Vec3{0.78f, 0.76f, 0.62f}, lichen * 0.6f);
    clay = clay * (1.0f - shadow * 0.55f);
    clay = mix(clay, Vec3{0.08f, 0.05f, 0.04f}, gap * 0.85f);
    t.color = clay;
    t.height = 0.2f + 0.45f * profile * (0.6f + 0.4f * local) + lip * 0.08f - gap * 0.3f + (speck - 0.5f) * 0.04f;
    t.roughness = 0.75f + 0.15f * grime + lichen * 0.05f;
    t.occlusion = 1.0f - shadow * 0.6f - gap * 0.5f;
    return t;
}

// --- Tela de lana a rayas (1 x 1 m): trama fina y desgaste ---
Texel clothTexel(float u, float v, std::uint32_t seed) {
    Texel t;
    constexpr int kStripes = 8;
    const float sx = u * kStripes;
    const int stripe = static_cast<int>(std::floor(sx));
    const float local = sx - std::floor(sx);
    const bool red = stripe % 2 == 0;
    const float line = 1.0f - smoothstep(0.03f, 0.06f, std::abs(local - 0.5f) - 0.4f + 0.06f);
    const float weave_u = std::sin(u * 2.0f * kPi * 180.0f);
    const float weave_v = std::sin(v * 2.0f * kPi * 180.0f);
    const float weave = 0.5f + 0.25f * (weave_u * weave_v);
    const float wear = fbm(u, v, 4, 4, 4, seed);
    const float fuzz = valueNoise(u * 128.0f, v * 128.0f, 128, 128, seed + 3U);
    Vec3 col = red ? Vec3{0.55f, 0.14f, 0.11f} : Vec3{0.8f, 0.72f, 0.55f};
    col = mix(col, Vec3{0.2f, 0.12f, 0.08f}, line * 0.25f);
    col = col * (0.82f + 0.25f * weave + (fuzz - 0.5f) * 0.12f);
    col = mix(col, col * 0.75f + Vec3{0.08f, 0.07f, 0.06f}, smoothstep(0.55f, 0.85f, wear) * 0.6f);
    t.color = col;
    t.height = 0.5f + (weave - 0.5f) * 0.4f + (fuzz - 0.5f) * 0.1f;
    t.roughness = 0.95f;
    t.occlusion = 0.85f + 0.15f * weave;
    return t;
}

// --- Vigas de roble oscuro (U a lo largo 2 m, V 0.5 m): veta y fendas ---
Texel beamTexel(float u, float v, std::uint32_t seed) {
    Texel t;
    const float grain = fbm(u, v, 3, 12, 4, seed);
    const float fine = valueNoise(u * 14.0f, v * 90.0f, 14, 90, seed + 5U);
    const float ridge = 1.0f - std::abs(2.0f * fbm(u, v, 2, 6, 3, seed + 11U) - 1.0f);
    const float crack = smoothstep(0.95f, 0.985f, ridge) * smoothstep(0.4f, 0.6f, valueNoise(u * 5.0f, v * 2.0f, 5, 2, seed + 13U));
    const float adze = valueNoise(u * 22.0f, v * 4.0f, 22, 4, seed + 17U);  // golpes de azuela
    const Vec3 dark{0.17f, 0.11f, 0.07f};
    const Vec3 mid{0.32f, 0.21f, 0.13f};
    Vec3 col = mix(dark, mid, std::clamp(grain * 1.25f - 0.12f + (fine - 0.5f) * 0.3f, 0.0f, 1.0f));
    col = col * (0.9f + 0.18f * adze);
    col = mix(col, Vec3{0.07f, 0.05f, 0.03f}, crack);
    t.color = col;
    t.height = 0.6f + (grain - 0.5f) * 0.2f + (fine - 0.5f) * 0.08f + (adze - 0.5f) * 0.12f - crack * 0.5f;
    t.roughness = 0.72f + 0.12f * (1.0f - grain);
    t.occlusion = 1.0f - crack * 0.7f;
    return t;
}

// ---------------------------------------------------------------------------
// Geometria
// ---------------------------------------------------------------------------

using housegeo::Frame;
using housegeo::frameAt;
using housegeo::Geo;
using housegeo::Opening;
using housegeo::Rng;
using housegeo::Wall;

// Planta de la casa para repartir los muebles: rectangulos ocupados.
struct Room {
    float x0 = 0.0f, x1 = 0.0f, z0 = 0.0f, z1 = 0.0f, y = 0.0f;
    int floor = 0;
    std::vector<std::array<float, 4>> used;

    bool free(float ax0, float ax1, float az0, float az1) const {
        if (ax0 < x0 - 1e-3f || ax1 > x1 + 1e-3f || az0 < z0 - 1e-3f || az1 > z1 + 1e-3f) return false;
        for (const auto& r : used) {
            if (ax0 < r[1] - 1e-3f && ax1 > r[0] + 1e-3f && az0 < r[3] - 1e-3f && az1 > r[2] + 1e-3f) return false;
        }
        return true;
    }
    void use(float ax0, float ax1, float az0, float az1) { used.push_back({ax0, ax1, az0, az1}); }
    bool take(float ax0, float ax1, float az0, float az1) {
        if (!free(ax0, ax1, az0, az1)) return false;
        use(ax0, ax1, az0, az1);
        return true;
    }
};

// Un tramo de escalera: de la planta `floor` a la de arriba.
struct Flight {
    int floor = 0;
    Frame frame;          // pie de la escalera, sube hacia +Z local
    float rise = 0.0f;
    float run = 0.0f;
    float width = 0.85f;
    std::array<float, 4> foot{};  // lo que ocupa abajo (x0, x1, z0, z1)
    std::array<float, 4> hole{};  // hueco en el suelo de arriba
};

class HouseBuilder {
public:
    explicit HouseBuilder(const HouseSettings& s) : s_(s), rng_(s.seed) {}

    HouseModel build() {
        layout();
        foundation();
        if (logs()) {
            logWalls();
        } else {
            flatWalls();
        }
        if (framed()) timberFrame();
        for (const Opening& o : openings_) {
            if (o.door) {
                doorFrame(o);
            } else {
                window(o);
            }
        }
        upperFloors();
        roof();
        if (s_.porch && !jettied() && !framed()) {
            porch();
        } else {
            frontSteps();
        }
        if (s_.chimney) chimney();
        if (s_.use == HouseUse::Tavern) tavernSign();
        if (s_.use == HouseUse::Smithy) smithyShed();
        if (s_.interior) interior();

        HouseModel out;
        geo_.finish(out.house, houseStyleName(s_.style));
        out.triangles = geo_.triangles();
        door_geo_.finish(out.door, "Puerta");
        out.triangles += door_geo_.triangles();
        out.door_hinge = door_hinge_;
        out.door_size = door_size_;
        housegeo::modelBounds(out.house, out.bounds_min, out.bounds_max);
        out.lights = geo_.lights;
        return out;
    }

private:
    HouseSettings s_;
    Rng rng_;
    Geo geo_;
    Geo door_geo_;
    std::array<Wall, 4> walls_{};  // 0 delante (+Z), 1 atras, 2 derecha (+X), 3 izquierda
    std::vector<Opening> openings_;
    std::vector<Flight> flights_;
    std::vector<Room> rooms_;
    float t_ = 0.2f;        // grosor de las paredes
    float plinth_ = 0.45f;  // altura del zocalo (el suelo de dentro)
    float top_ = 3.0f;      // altura de lo alto de las paredes
    float storey_ = 2.7f;   // altura de cada planta
    int floors_ = 1;
    float jetty_ = 0.0f;    // vuelo de las plantas altas (entramado)
    float tan_ = 0.78f;     // pendiente del tejado
    float zo_ = 3.0f;       // media profundidad por fuera de las paredes (las de arriba)
    float xo_ = 4.0f;       // medio ancho por fuera
    float ridge_ = 5.0f;    // cara de abajo del tejado en la cumbrera
    int roof_m_ = kHouseRoof;
    float roof_th_ = 0.16f;
    float log_r_ = 0.15f;
    float log_step_ = 0.26f;
    float log_ext_ = 0.35f;
    float door_s_ = 0.0f;
    Vec3 door_hinge_{};
    Vec3 door_size_{};

    bool logs() const { return s_.style == HouseStyle::LogCabin; }
    bool framed() const { return s_.style == HouseStyle::HalfTimbered || s_.style == HouseStyle::Thatched; }
    bool jettied() const { return jetty_ > 0.0f; }
    float upperBase() const { return plinth_ + storey_; }
    int wallMaterial() const {
        switch (s_.style) {
            case HouseStyle::StoneCottage: return kHouseStone;
            case HouseStyle::Farmhouse: return kHouseSiding;
            case HouseStyle::HalfTimbered: return kHouseStone;
            case HouseStyle::Thatched: return kHousePlaster;
            default: return kHousePlanks;
        }
    }
    // La cara de dentro: encalada en las de piedra y entramado.
    int innerMaterial() const {
        switch (s_.style) {
            case HouseStyle::StoneCottage:
            case HouseStyle::HalfTimbered:
            case HouseStyle::Thatched: return kHousePlaster;
            case HouseStyle::Farmhouse: return kHousePlanks;
            default: return kHousePlanks;
        }
    }
    int frameMaterial() const { return framed() ? kHouseBeams : kHouseTrim; }
    // Paredes de las plantas altas: delante y detras vuelan `jetty_`.
    Wall upperWall(int w) const {
        Wall wall = walls_[static_cast<std::size_t>(w)];
        if (jettied() && w < 2) wall.center = wall.center + wall.out * jetty_;
        return wall;
    }
    Wall wallOf(const Opening& o) const {
        return jettied() && o.y0 >= upperBase() - 1e-3f ? upperWall(o.wall) : walls_[static_cast<std::size_t>(o.wall)];
    }
    float innerInset() const { return logs() ? log_r_ : t_ * 0.5f; }
    Vec3 at(const Wall& w, float s, float y, float d) const { return housegeo::at(w, s, y, d); }
    // Altura de la cara de abajo del tejado a una distancia |z| del centro.
    float roofUnder(float z) const { return top_ + (zo_ - std::abs(z)) * tan_; }

    void layout() {
        const float W = s_.width;
        const float D = s_.depth;
        tan_ = std::tan(std::clamp(s_.roof_pitch, 10.0f, 62.0f) * kPi / 180.0f);
        switch (s_.style) {
            case HouseStyle::LogCabin: t_ = 2.0f * log_r_; plinth_ = 0.5f; break;
            case HouseStyle::TimberCabin: t_ = 0.16f; plinth_ = 0.45f; break;
            case HouseStyle::StoneCottage: t_ = 0.5f; plinth_ = 0.3f; break;
            case HouseStyle::Farmhouse: t_ = 0.2f; plinth_ = 0.6f; break;
            case HouseStyle::HalfTimbered: t_ = 0.3f; plinth_ = 0.35f; break;
            case HouseStyle::Thatched: t_ = 0.25f; plinth_ = 0.25f; break;
        }
        floors_ = std::clamp(s_.floors, 1, 3);
        const float wall_h = std::max(s_.wall_height, 2.2f) * static_cast<float>(floors_);
        if (logs()) {
            log_step_ = 2.0f * log_r_ * 0.88f;
            const int courses = std::max(4, static_cast<int>(std::round(wall_h / log_step_)));
            top_ = plinth_ + log_r_ * 0.95f + static_cast<float>(courses - 1) * log_step_ + log_r_ * 0.9f;
        } else {
            top_ = plinth_ + wall_h;
        }
        storey_ = (top_ - plinth_) / static_cast<float>(floors_);
        jetty_ = s_.style == HouseStyle::HalfTimbered && floors_ >= 2 ? std::clamp(s_.jetty, 0.0f, 0.9f) : 0.0f;
        zo_ = D * 0.5f + t_ * 0.5f + jetty_;
        xo_ = W * 0.5f + t_ * 0.5f;
        ridge_ = top_ + zo_ * tan_;
        switch (s_.style) {
            case HouseStyle::HalfTimbered: roof_m_ = kHouseTile; roof_th_ = 0.14f; break;
            case HouseStyle::Thatched: roof_m_ = kHouseThatch; roof_th_ = 0.38f; break;
            default: roof_m_ = kHouseRoof; roof_th_ = 0.16f; break;
        }
        walls_ = housegeo::rectWalls(W, D);

        // --- Puertas y ventanas ---
        float win_w = 0.9f;
        float win_h = 1.15f;
        float sill = 0.95f;
        switch (s_.style) {
            case HouseStyle::LogCabin: win_w = 0.85f; win_h = 1.0f; sill = 0.9f; break;
            case HouseStyle::StoneCottage: win_w = 0.8f; win_h = 1.05f; sill = 0.95f; break;
            case HouseStyle::Farmhouse: win_w = 0.85f; win_h = 1.45f; sill = 0.8f; break;
            case HouseStyle::HalfTimbered: win_w = 0.8f; win_h = 1.1f; sill = 0.9f; break;
            case HouseStyle::Thatched: win_w = 0.7f; win_h = 0.9f; sill = 0.9f; break;
            default: break;
        }
        const float door_w = s_.use == HouseUse::Tavern ? 1.1f : 1.0f;
        const float door_h = 2.1f;
        int slots = s_.windows > 0 ? s_.windows + 1 : std::max(2, static_cast<int>(std::round(W / 2.3f)));
        if (slots % 2 == 0 && s_.windows <= 0) slots += 1;
        const int door_slot = slots / 2;
        const float slot_w = W / static_cast<float>(slots);
        for (int floor = 0; floor < floors_; ++floor) {
            const float base = plinth_ + storey_ * static_cast<float>(floor);
            // Delante: la puerta en el centro y ventanas.
            for (int i = 0; i < slots; ++i) {
                const float s = -W * 0.5f + (static_cast<float>(i) + 0.5f) * slot_w;
                if (floor == 0 && i == door_slot) {
                    door_s_ = s;
                    openings_.push_back(Opening{0, s - door_w * 0.5f, s + door_w * 0.5f, base, base + door_h, true});
                    continue;
                }
                openings_.push_back(Opening{0, s - win_w * 0.5f, s + win_w * 0.5f, base + sill, base + sill + win_h, false});
            }
            // Atras: ventanas, alguna menos al azar.
            for (int i = 0; i < slots; ++i) {
                if (slots > 2 && rng_.chance(0.3f)) continue;
                const float s = -W * 0.5f + (static_cast<float>(i) + 0.5f) * slot_w;
                openings_.push_back(Opening{1, s - win_w * 0.5f, s + win_w * 0.5f, base + sill, base + sill + win_h, false});
            }
            // Lados: una ventana (o dos a los lados de la chimenea).
            for (int side = 2; side <= 3; ++side) {
                if (side == 2 && s_.chimney) {
                    if (D > 5.2f) {
                        for (float s : {-D * 0.3f, D * 0.3f}) {
                            openings_.push_back(Opening{side, s - win_w * 0.4f, s + win_w * 0.4f, base + sill,
                                                        base + sill + win_h * 0.85f, false});
                        }
                    }
                    continue;
                }
                openings_.push_back(Opening{side, -win_w * 0.5f, win_w * 0.5f, base + sill, base + sill + win_h, false});
            }
        }
        // Troncos: los huecos se ajustan a las juntas entre hiladas.
        if (logs()) {
            for (Opening& o : openings_) {
                const float offset = (o.wall >= 2 ? log_step_ * 0.5f : 0.0f) + plinth_ + log_r_ * 0.95f;
                const auto snap = [&](float y) {
                    return offset + (std::round((y - offset) / log_step_ - 0.5f) + 0.5f) * log_step_;
                };
                o.y1 = std::min(snap(o.y1), top_ - log_step_ * 0.6f);
                if (o.door) {
                    o.y0 = plinth_;
                } else {
                    o.y0 = snap(o.y0);
                }
            }
        }

        // --- Escaleras (una por planta de arriba) ---
        if (s_.interior && floors_ >= 2) {
            const float inset = innerInset() + 0.03f;
            const float gx0 = -W * 0.5f + inset;
            const float gz0 = -D * 0.5f + inset;
            const float gx1 = W * 0.5f - inset;
            const float gz1 = D * 0.5f - inset;
            for (int f = 0; f + 1 < floors_; ++f) {
                Flight fl;
                fl.floor = f;
                fl.rise = storey_;
                fl.run = storey_ * 1.05f;
                fl.width = 0.85f;
                const float y = plinth_ + storey_ * static_cast<float>(f) + 0.03f;
                // Casas muy pequenas: la escalera se empina hasta 50 grados; si
                // ni asi cabe, esa planta queda sin escalera (forjado entero).
                const float room = f % 2 == 0 ? gx1 - gx0 - 0.35f - 0.6f : gz1 - gz0 - 1.15f - 0.9f;
                fl.run = std::min(fl.run, room);
                if (fl.run < storey_ * 0.84f) break;
                if (f % 2 == 0) {
                    // Contra la pared de atras, sube hacia +X.
                    const float x0 = gx0 + 0.35f;
                    const float zc = gz0 + fl.width * 0.5f + 0.02f;
                    fl.frame = frameAt(Vec3{x0, y, zc}, 1);
                    fl.foot = {x0 - 0.3f, x0 + fl.run + 0.1f, gz0, gz0 + fl.width + 0.1f};
                    fl.hole = {x0 - 0.05f, x0 + fl.run + 0.05f, gz0 - 0.03f, gz0 + fl.width + 0.08f};
                } else {
                    // Contra la pared de la izquierda, sube hacia +Z.
                    const float xc = gx0 + fl.width * 0.5f + 0.02f;
                    const float z0 = gz0 + 1.15f;
                    fl.frame = frameAt(Vec3{xc, y, z0}, 0);
                    fl.foot = {gx0, gx0 + fl.width + 0.1f, z0 - 0.3f, z0 + fl.run + 0.1f};
                    fl.hole = {gx0, gx0 + fl.width + 0.08f, z0 - 0.05f, z0 + fl.run + 0.05f};
                }
                flights_.push_back(fl);
            }
        }
    }

    void foundation() {
        const float margin = logs() ? log_r_ + 0.05f : 0.05f;
        const float hx = s_.width * 0.5f + margin;
        const float hz = s_.depth * 0.5f + margin;
        const float top = plinth_;
        geo_.boxMinMax(kHouseStone, Vec3{-hx, -1.5f, -hz}, Vec3{hx, top, hz}, false, 4);
        // Suelo de tablas por dentro.
        const float in_x = s_.width * 0.5f - (logs() ? log_r_ * 0.7f : t_ * 0.5f);
        const float in_z = s_.depth * 0.5f - (logs() ? log_r_ * 0.7f : t_ * 0.5f);
        geo_.boxMinMax(kHousePlanks, Vec3{-in_x, top - 0.02f, -in_z}, Vec3{in_x, top + 0.03f, in_z}, true, 4);
    }

    // Tramos [a, b] de una linea que no pisan los huecos de la pared a la altura y.
    std::vector<std::pair<float, float>> segments(int wall, float a, float b, float y, float reach) const {
        std::vector<std::pair<float, float>> cuts;
        for (const Opening& o : openings_) {
            if (o.wall != wall) continue;
            if (y + reach <= o.y0 || y - reach >= o.y1) continue;
            cuts.push_back({o.s0, o.s1});
        }
        std::sort(cuts.begin(), cuts.end());
        std::vector<std::pair<float, float>> out;
        float from = a;
        for (const auto& c : cuts) {
            if (c.first > from) out.push_back({from, std::min(c.first, b)});
            from = std::max(from, c.second);
        }
        if (from < b) out.push_back({from, b});
        return out;
    }

    // --- Cabana de troncos: hiladas que se cruzan en las esquinas ---
    void logWalls() {
        const int sides = 10;
        for (int w = 0; w < 4; ++w) {
            const Wall& wall = walls_[static_cast<std::size_t>(w)];
            const bool end_wall = w >= 2;
            const float offset = (end_wall ? log_step_ * 0.5f : 0.0f) + plinth_ + log_r_ * 0.95f;
            for (int k = 0;; ++k) {
                const float y = offset + static_cast<float>(k) * log_step_;
                float half;
                bool caps = true;
                if (y + log_r_ * 0.5f <= top_) {
                    half = wall.half + log_ext_;
                } else if (end_wall) {
                    // Hastial: cada vez mas corto bajo el tejado.
                    half = zo_ - (y + log_r_ - top_) / tan_;
                    if (half < 0.3f) break;
                } else {
                    break;
                }
                const float r = log_r_ * rng_.range(0.93f, 1.07f);
                const float u_off = rng_.range(0.0f, 1.0f);
                const float twist = rng_.range(0.0f, 6.28f);
                const float jitter = rng_.range(-0.04f, 0.04f);
                for (const auto& seg : segments(w, -half + jitter, half + jitter, y, 0.0f)) {
                    if (seg.second - seg.first < 0.05f) continue;
                    geo_.log(at(wall, seg.first, y, 0.0f), at(wall, seg.second, y, 0.0f), r, sides, caps, caps, u_off,
                             twist);
                }
            }
        }
        // Correas: troncos bajo el tejado que asoman por los hastiales.
        const float over = xo_ + log_ext_ + std::max(s_.roof_overhang * 0.7f, 0.45f) - 0.08f;
        for (float z : {0.0f, -zo_ * 0.5f, zo_ * 0.5f}) {
            const float r = z == 0.0f ? 0.14f : 0.12f;
            const float y = roofUnder(z) - r;
            geo_.log(Vec3{-over, y, z}, Vec3{over, y, z}, r, 10, true, true, rng_.range(0.0f, 1.0f), 0.0f);
        }
    }

    // --- Paredes planas (tablas, piedra, tablas solapadas, enlucido) ---
    void flatWalls() {
        const int m = wallMaterial();
        const int im = innerMaterial();
        const float W = s_.width;
        const float D = s_.depth;
        // Delante y atras de esquina a esquina; los lados entre medias.
        for (int w = 0; w < 4; ++w) {
            const Wall& wall = walls_[static_cast<std::size_t>(w)];
            const float half = w < 2 ? W * 0.5f + t_ * 0.5f : D * 0.5f - t_ * 0.5f;
            if (jettied()) {
                // Planta baja de piedra; las de arriba, de entramado, vuelan
                // delante y detras sobre la calle.
                housegeo::wallPanel(geo_, kHouseStone, wall, -half, half, plinth_, upperBase(), t_, openings_, w, true, true, im);
                const float upper_half = w < 2 ? half : D * 0.5f + jetty_ - t_ * 0.5f;
                housegeo::wallPanel(geo_, kHousePlaster, upperWall(w), -upper_half, upper_half, upperBase(), top_, t_, openings_, w,
                                    true, true, im);
            } else {
                housegeo::wallPanel(geo_, m, wall, -half, half, plinth_, top_, t_, openings_, w, true, true, im);
            }
        }
        // Hastiales (triangulos bajo el tejado) en los lados.
        const int gm = framed() ? kHousePlaster : m;
        for (int w = 2; w < 4; ++w) housegeo::gableWall(geo_, gm, walls_[static_cast<std::size_t>(w)], zo_, top_, ridge_, t_, im);
        // Esquineras (tablas y casa de campo) y la faja entre plantas.
        if (s_.style == HouseStyle::TimberCabin || s_.style == HouseStyle::Farmhouse) {
            const int cm = s_.style == HouseStyle::Farmhouse ? kHouseTrim : kHousePlanks;
            const auto span = [](float a, float b) { return std::pair<float, float>{std::min(a, b), std::max(a, b)}; };
            for (float sx : {-1.0f, 1.0f}) {
                for (float sz : {-1.0f, 1.0f}) {
                    // Una tabla en cada cara de la esquina.
                    auto x = span(sx * (xo_ - 0.13f), sx * (xo_ + 0.005f));
                    auto z = span(sz * (zo_ - 0.01f), sz * (zo_ + 0.03f));
                    geo_.boxMinMax(cm, Vec3{x.first, plinth_, z.first}, Vec3{x.second, top_, z.second});
                    x = span(sx * (xo_ - 0.01f), sx * (xo_ + 0.03f));
                    z = span(sz * (zo_ - 0.13f), sz * (zo_ + 0.005f));
                    geo_.boxMinMax(cm, Vec3{x.first, plinth_, z.first}, Vec3{x.second, top_, z.second});
                }
            }
            for (int f = 1; f < floors_; ++f) {
                const float y = plinth_ + storey_ * static_cast<float>(f);
                geo_.boxMinMax(cm, Vec3{-xo_ - 0.02f, y - 0.08f, zo_}, Vec3{xo_ + 0.02f, y + 0.08f, zo_ + 0.03f});
                geo_.boxMinMax(cm, Vec3{-xo_ - 0.02f, y - 0.08f, -zo_ - 0.03f}, Vec3{xo_ + 0.02f, y + 0.08f, -zo_});
                geo_.boxMinMax(cm, Vec3{xo_, y - 0.08f, -zo_}, Vec3{xo_ + 0.03f, y + 0.08f, zo_});
                geo_.boxMinMax(cm, Vec3{-xo_ - 0.03f, y - 0.08f, -zo_}, Vec3{-xo_, y + 0.08f, zo_});
            }
        }
    }

    // --- Entramado: vigas oscuras sobre el enlucido (soleras, postes, pies
    // derechos junto a los huecos, travesanos y tornapuntas en cruz) ---
    void timberFrame() {
        const float d0 = t_ * 0.5f - 0.01f;
        const float d1 = t_ * 0.5f + 0.035f;
        const float bw = 0.17f;
        const int first = jettied() ? 1 : 0;
        for (int w = 0; w < 4; ++w) {
            const Wall wall = upperWall(w);
            const float half = w < 2 ? xo_ : zo_ - t_;
            for (int f = first; f < floors_; ++f) {
                const float yb = plinth_ + storey_ * static_cast<float>(f);
                const float yt = f + 1 == floors_ ? top_ : yb + storey_;
                // Solera y carrera.
                housegeo::wallBox(geo_, kHouseBeams, wall, -half, half, yb, yb + bw, d0, d1 + 0.01f);
                housegeo::wallBox(geo_, kHouseBeams, wall, -half, half, yt - bw, yt, d0, d1 + 0.01f);
                // Pies derechos: esquinas y a los lados de cada hueco de esta planta.
                std::vector<const Opening*> holes;
                for (const Opening& o : openings_) {
                    if (o.wall == w && o.y0 >= yb - 1e-3f && o.y0 < yt) holes.push_back(&o);
                }
                std::vector<float> posts = {-half + bw * 0.5f, half - bw * 0.5f};
                for (const Opening* o : holes) {
                    posts.push_back(o->s0 - bw * 0.5f);
                    posts.push_back(o->s1 + bw * 0.5f);
                }
                std::sort(posts.begin(), posts.end());
                std::vector<float> merged;
                for (float p : posts) {
                    if (p < -half + bw * 0.5f - 1e-3f || p > half - bw * 0.5f + 1e-3f) continue;
                    if (merged.empty() || p - merged.back() > bw * 1.3f) merged.push_back(p);
                }
                const auto bay_has_hole = [&](float a, float b) {
                    for (const Opening* o : holes) {
                        if (o->s0 < b - 1e-3f && o->s1 > a + 1e-3f) return true;
                    }
                    return false;
                };
                // Mas pies derechos en los panos anchos sin huecos.
                std::vector<float> all;
                for (std::size_t i = 0; i < merged.size(); ++i) {
                    all.push_back(merged[i]);
                    if (i + 1 < merged.size() && !bay_has_hole(merged[i], merged[i + 1])) {
                        const float gap = merged[i + 1] - merged[i];
                        const int extra = static_cast<int>(std::ceil(gap / 1.25f)) - 1;
                        for (int k = 1; k <= extra; ++k) all.push_back(merged[i] + gap * static_cast<float>(k) / static_cast<float>(extra + 1));
                    }
                }
                std::sort(all.begin(), all.end());
                for (float p : all) housegeo::wallBox(geo_, kHouseBeams, wall, p - bw * 0.5f, p + bw * 0.5f, yb + bw, yt - bw, d0, d1);
                // Panos: tornapuntas en los de las esquinas, travesano en los demas.
                const float dm = (d0 + d1) * 0.5f;
                for (std::size_t i = 0; i + 1 < all.size(); ++i) {
                    const float a = all[i] + bw * 0.5f;
                    const float b = all[i + 1] - bw * 0.5f;
                    if (b - a < 0.35f) continue;
                    if (bay_has_hole(a, b)) {
                        // Dintel y antepecho del hueco entre sus pies derechos.
                        for (const Opening* o : holes) {
                            if (!(o->s0 < b && o->s1 > a)) continue;
                            if (o->y1 + bw < yt - bw) housegeo::wallBox(geo_, kHouseBeams, wall, a, b, o->y1, o->y1 + bw * 0.8f, d0, d1);
                            if (!o->door && o->y0 - bw > yb + bw) {
                                housegeo::wallBox(geo_, kHouseBeams, wall, a, b, o->y0 - bw * 0.8f, o->y0, d0, d1);
                            }
                        }
                        continue;
                    }
                    const bool end_bay = i == 0 || i + 2 == all.size();
                    if (end_bay || rng_.chance(0.35f)) {
                        const bool rising = (i == 0) != (rng_.chance(0.5f) && !end_bay);
                        const float ya = yb + bw;
                        const float yb2 = yt - bw;
                        const Vec3 p0 = at(wall, rising ? a : b, ya, dm);
                        const Vec3 p1 = at(wall, rising ? b : a, yb2, dm);
                        geo_.beam(kHouseBeams, p0, p1, 0.14f, 0.045f, wall.out);
                    } else {
                        const float ym = yb + (yt - yb) * 0.5f;
                        housegeo::wallBox(geo_, kHouseBeams, wall, a, b, ym - bw * 0.4f, ym + bw * 0.4f, d0, d1);
                    }
                }
            }
        }
        // Hastiales: pendolon, nudillo y jabalcones.
        for (int w = 2; w < 4; ++w) {
            const Wall& wall = walls_[static_cast<std::size_t>(w)];
            const float rise = ridge_ - top_;
            housegeo::wallBox(geo_, kHouseBeams, wall, -bw * 0.5f, bw * 0.5f, top_, ridge_ - 0.25f, d0, d1);
            const float yc = top_ + rise * 0.45f;
            const float hc = zo_ * (1.0f - 0.45f) - 0.12f;
            housegeo::wallBox(geo_, kHouseBeams, wall, -hc, hc, yc - bw * 0.4f, yc + bw * 0.4f, d0, d1);
            const float dm = (d0 + d1) * 0.5f;
            for (float sx : {-1.0f, 1.0f}) {
                geo_.beam(kHouseBeams, at(wall, sx * zo_ * 0.62f, top_ + 0.05f, dm), at(wall, sx * 0.12f, yc - 0.05f, dm), 0.13f, 0.045f,
                          wall.out);
            }
        }
        // Vuelo: cabezas de las vigas del forjado bajo las plantas altas.
        if (jettied()) {
            for (int f = 1; f < floors_; ++f) {
                const float y = plinth_ + storey_ * static_cast<float>(f);
                for (float side : {1.0f, -1.0f}) {
                    const float z_in = side * (s_.depth * 0.5f + t_ * 0.5f - 0.05f);
                    const float z_out = side * (s_.depth * 0.5f + t_ * 0.5f + jetty_ + 0.06f);
                    for (float x = -s_.width * 0.5f + 0.2f; x <= s_.width * 0.5f - 0.15f; x += 0.55f) {
                        geo_.boxMinMax(kHouseBeams, Vec3{x - 0.07f, y - 0.2f, std::min(z_in, z_out)}, Vec3{x + 0.07f, y, std::max(z_in, z_out)},
                                       true);
                    }
                    // Viga de borde bajo la pared que vuela.
                    geo_.boxMinMax(kHouseBeams, Vec3{-xo_, y - 0.22f, std::min(z_out - side * 0.14f, z_out)},
                                   Vec3{xo_, y - 0.02f, std::max(z_out - side * 0.14f, z_out)});
                }
            }
        }
    }

    float outerFace() const { return logs() ? log_r_ : t_ * 0.5f; }

    void wallBox(Geo& g, int m, const Wall& w, float s0, float s1, float y0, float y1, float d0, float d1,
                 bool rotate = false) {
        housegeo::wallBox(g, m, w, s0, s1, y0, y1, d0, d1, rotate);
    }

    void window(const Opening& o) {
        if (framed()) {
            const Wall w = wallOf(o);
            const bool stone = jettied() && o.y0 < upperBase();
            housegeo::windowFrame(geo_, w, o, t_, outerFace(), stone, s_.shutters, kHouseBeams);
            return;
        }
        const Wall& w = walls_[static_cast<std::size_t>(o.wall)];
        const float h = t_ * 0.5f;
        const float face = outerFace();
        const float fw = 0.07f;
        // Marco (en el hueco, de lado a lado de la pared).
        const float fd0 = -h + 0.02f;
        const float fd1 = s_.style == HouseStyle::StoneCottage ? -h + 0.2f : h + 0.02f;
        wallBox(geo_, kHouseTrim, w, o.s0, o.s0 + fw, o.y0, o.y1, fd0, fd1);
        wallBox(geo_, kHouseTrim, w, o.s1 - fw, o.s1, o.y0, o.y1, fd0, fd1);
        wallBox(geo_, kHouseTrim, w, o.s0 + fw, o.s1 - fw, o.y1 - fw, o.y1, fd0, fd1);
        wallBox(geo_, kHouseTrim, w, o.s0 + fw, o.s1 - fw, o.y0, o.y0 + fw, fd0, fd1);
        // Vidrio (dos caras) y cruceta.
        const float gd = (fd0 + fd1) * 0.5f;
        const float gs0 = o.s0 + fw;
        const float gs1 = o.s1 - fw;
        const float gy0 = o.y0 + fw;
        const float gy1 = o.y1 - fw;
        for (int side = 0; side < 2; ++side) {
            const Vec3 n = w.out * (side == 0 ? 1.0f : -1.0f);
            geo_.flat(kHouseGlass, {at(w, gs0, gy0, gd), at(w, gs1, gy0, gd), at(w, gs1, gy1, gd), at(w, gs0, gy1, gd)}, n,
                      w.along, kUp * -1.0f);
        }
        const float mid_s = (o.s0 + o.s1) * 0.5f;
        const float mid_y = (o.y0 + o.y1) * 0.5f + (o.y1 - o.y0) * 0.08f;
        wallBox(geo_, kHouseTrim, w, mid_s - 0.02f, mid_s + 0.02f, gy0, gy1, gd - 0.025f, gd + 0.025f);
        wallBox(geo_, kHouseTrim, w, gs0, gs1, mid_y - 0.02f, mid_y + 0.02f, gd - 0.025f, gd + 0.025f);
        if (o.y1 - o.y0 > 1.3f) {
            for (float f : {0.3f, 0.75f}) {
                const float yy = o.y0 + (o.y1 - o.y0) * f;
                wallBox(geo_, kHouseTrim, w, gs0, gs1, yy - 0.015f, yy + 0.015f, gd - 0.02f, gd + 0.02f);
            }
        }
        // Alfeizar que sobresale y dintel.
        wallBox(geo_, kHouseTrim, w, o.s0 - 0.07f, o.s1 + 0.07f, o.y0 - 0.05f, o.y0, fd1 - 0.04f, face + 0.07f);
        if (s_.style == HouseStyle::StoneCottage) {
            wallBox(geo_, kHousePlanks, w, o.s0 - 0.18f, o.s1 + 0.18f, o.y1, o.y1 + 0.2f, -h + 0.01f, h + 0.01f, true);
        } else {
            // Tapajuntas alrededor por fuera.
            const float cd0 = face - 0.01f;
            const float cd1 = face + 0.025f;
            wallBox(geo_, kHouseTrim, w, o.s0 - 0.09f, o.s0, o.y0, o.y1, cd0, cd1);
            wallBox(geo_, kHouseTrim, w, o.s1, o.s1 + 0.09f, o.y0, o.y1, cd0, cd1);
            wallBox(geo_, kHouseTrim, w, o.s0 - 0.12f, o.s1 + 0.12f, o.y1, o.y1 + 0.12f, cd0, cd1 + 0.01f);
        }
        // Contraventanas abiertas.
        if (s_.shutters && !logs()) {
            const float sw = (o.s1 - o.s0) * 0.5f;
            const float d0 = face + 0.03f;
            wallBox(geo_, kHousePlanks, w, o.s0 - 0.11f - sw, o.s0 - 0.11f, o.y0 + 0.02f, o.y1 - 0.02f, d0, d0 + 0.035f);
            wallBox(geo_, kHousePlanks, w, o.s1 + 0.11f, o.s1 + 0.11f + sw, o.y0 + 0.02f, o.y1 - 0.02f, d0, d0 + 0.035f);
        }
    }

    void doorFrame(const Opening& o) {
        const Wall& w = walls_[static_cast<std::size_t>(o.wall)];
        const float h = t_ * 0.5f;
        const float face = outerFace();
        const float fw = 0.08f;
        const float fd0 = -h - 0.01f;
        const float fd1 = h + 0.02f;
        const int fm = frameMaterial();
        const bool stone = s_.style == HouseStyle::StoneCottage || jettied();
        wallBox(geo_, fm, w, o.s0, o.s0 + fw, o.y0, o.y1, fd0, fd1);
        wallBox(geo_, fm, w, o.s1 - fw, o.s1, o.y0, o.y1, fd0, fd1);
        wallBox(geo_, fm, w, o.s0, o.s1, o.y1 - fw, o.y1, fd0, fd1);
        wallBox(geo_, kHousePlanks, w, o.s0, o.s1, o.y0, o.y0 + 0.035f, fd0, face + 0.04f, true);  // umbral
        if (stone) {
            wallBox(geo_, jettied() ? kHouseBeams : kHousePlanks, w, o.s0 - 0.2f, o.s1 + 0.2f, o.y1, o.y1 + 0.22f, -h + 0.01f,
                    h + 0.01f, true);
        } else {
            const float cd0 = face - 0.01f;
            const float cd1 = face + 0.03f;
            wallBox(geo_, fm, w, o.s0 - 0.1f, o.s0, o.y0, o.y1, cd0, cd1);
            wallBox(geo_, fm, w, o.s1, o.s1 + 0.1f, o.y0, o.y1, cd0, cd1);
            wallBox(geo_, fm, w, o.s0 - 0.13f, o.s1 + 0.13f, o.y1, o.y1 + 0.14f, cd0, cd1 + 0.01f);
        }
        // La hoja: pieza aparte con la bisagra en su origen (lado izquierdo).
        const float dw = (o.s1 - o.s0) - 2.0f * fw - 0.01f;
        const float dh = (o.y1 - o.y0) - fw - 0.04f;
        const float thick = 0.055f;
        const float hinge_d = h - thick * 0.5f - 0.02f;
        door_hinge_ = at(w, o.s0 + fw + 0.005f, o.y0 + 0.035f, hinge_d);
        door_size_ = Vec3{dw, dh, thick};
        housegeo::doorLeaf(door_geo_, dw, dh);
    }

    // --- Forjados de las plantas altas (con el hueco de la escalera) ---
    void upperFloors() {
        if (floors_ < 2) return;
        const float inset = innerInset();
        const float x0 = -s_.width * 0.5f + inset;
        const float x1 = -x0;
        for (int f = 1; f < floors_; ++f) {
            const float y = plinth_ + storey_ * static_cast<float>(f);
            const float zz = s_.depth * 0.5f + jetty_ - inset;
            const float z0 = -zz;
            const float z1 = zz;
            const Flight* fl = nullptr;
            for (const Flight& candidate : flights_) {
                if (candidate.floor + 1 == f) fl = &candidate;
            }
            const auto slab = [&](float ax0, float ax1, float az0, float az1) {
                if (ax1 - ax0 < 0.02f || az1 - az0 < 0.02f) return;
                geo_.boxMinMax(kHousePlanks, Vec3{ax0, y - 0.12f, az0}, Vec3{ax1, y + 0.03f, az1}, true);
            };
            if (fl == nullptr) {
                slab(x0, x1, z0, z1);
            } else {
                const float hx0 = std::clamp(fl->hole[0], x0, x1);
                const float hx1 = std::clamp(fl->hole[1], x0, x1);
                const float hz0 = std::clamp(fl->hole[2], z0, z1);
                const float hz1 = std::clamp(fl->hole[3], z0, z1);
                slab(x0, hx0, z0, z1);
                slab(hx1, x1, z0, z1);
                slab(hx0, hx1, z0, hz0);
                slab(hx0, hx1, hz1, z1);
                // Barandilla alrededor del hueco (menos por donde se llega).
                const bool along_x = fl->floor % 2 == 0;
                const float ry = y + 0.03f;
                const auto rail = [&](const Vec3& a, const Vec3& b) {
                    geo_.beam(kHouseBeams, a + kUp * (ry + 0.9f), b + kUp * (ry + 0.9f), 0.07f, 0.07f);
                    const Vec3 d = b - a;
                    const float len = core::length(d);
                    const int n = std::max(1, static_cast<int>(len / 0.9f));
                    for (int i = 0; i <= n; ++i) {
                        const Vec3 p = a + d * (static_cast<float>(i) / static_cast<float>(n));
                        geo_.boxMinMax(kHouseBeams, Vec3{p.x - 0.035f, ry, p.z - 0.035f}, Vec3{p.x + 0.035f, ry + 0.9f, p.z + 0.035f});
                    }
                };
                if (along_x) {
                    rail(Vec3{hx0, 0.0f, hz1 + 0.04f}, Vec3{hx1, 0.0f, hz1 + 0.04f});
                    rail(Vec3{hx0 - 0.04f, 0.0f, hz0}, Vec3{hx0 - 0.04f, 0.0f, hz1 + 0.04f});
                } else {
                    rail(Vec3{hx1 + 0.04f, 0.0f, hz0}, Vec3{hx1 + 0.04f, 0.0f, hz1});
                    rail(Vec3{hx0, 0.0f, hz0 - 0.04f}, Vec3{hx1 + 0.04f, 0.0f, hz0 - 0.04f});
                }
                // La escalera.
                housegeo::stairs(geo_, fl->frame, fl->rise, fl->run, fl->width);
            }
            // Vigas del forjado vistas desde abajo (a lo largo de Z).
            for (float x = x0 + 0.5f; x < x1 - 0.3f; x += 1.15f) {
                if (fl != nullptr && x > fl->hole[0] - 0.1f && x < fl->hole[1] + 0.1f) continue;
                geo_.boxMinMax(kHouseBeams, Vec3{x - 0.08f, y - 0.3f, z0}, Vec3{x + 0.08f, y - 0.12f, z1}, true);
            }
        }
    }

    // --- Tejado a dos aguas: tablillas, teja o paja, sofito, frentes y cumbrera ---
    void roof() {
        const float ov = std::max(s_.roof_overhang, 0.2f);
        const float ovg = logs() ? std::max(ov * 0.7f, 0.45f) + log_ext_ : ov * 0.7f;
        const float th = roof_th_;
        const bool thatch = roof_m_ == kHouseThatch;
        const int fascia = thatch ? kHouseThatch : kHouseTrim;
        const float x0 = -xo_ - ovg;
        const float x1 = xo_ + ovg;
        const float ze = zo_ + ov;
        const float ye = roofUnder(ze);
        for (float side : {1.0f, -1.0f}) {
            const Vec3 n = core::normalize(Vec3{0.0f, 1.0f, tan_ * side});
            const Vec3 up = n * th;
            const Vec3 r0{x0, ridge_, 0.0f};
            const Vec3 r1{x1, ridge_, 0.0f};
            const Vec3 e0{x0, ye, ze * side};
            const Vec3 e1{x1, ye, ze * side};
            const Vec3 down = core::normalize(e0 - r0);
            // Arriba: V baja por la pendiente (las hileras en horizontal).
            const float slope = core::length(e0 - r0);
            const std::array<float, 2> rm = houseTextureMeters(roof_m_);
            const float vo = side > 0.0f ? 0.0f : 0.37f;
            geo_.polygon(roof_m_, {r0 + up, r1 + up, e1 + up, e0 + up}, {n, n, n, n},
                         {Vec2{x0 / rm[0], vo}, Vec2{x1 / rm[0], vo}, Vec2{x1 / rm[0], vo + slope / rm[1]},
                          Vec2{x0 / rm[0], vo + slope / rm[1]}},
                         n);
            // Abajo: tablas del sofito (a lo largo de la pendiente).
            geo_.flat(kHousePlanks, {r0, r1, e1, e0}, n * -1.0f, kX, down, Vec2{0.0f, 0.0f});
            // Frente del alero y cantos de los hastiales.
            const Vec3 fz{0.0f, 0.0f, side};
            geo_.flat(fascia, {e0, e1, e1 + up, e0 + up}, fz, kX, kUp * -1.0f);
            for (float xs : {-1.0f, 1.0f}) {
                const float x = xs < 0.0f ? x0 : x1;
                const Vec3 a{x, ridge_, 0.0f};
                const Vec3 b{x, ye, ze * side};
                if (thatch) {
                    // La paja termina en un canto grueso.
                    geo_.flat(kHouseThatch, {a, b, b + up, a + up}, Vec3{xs, 0.0f, 0.0f}, kZ, kUp * -1.0f);
                } else {
                    // Tabla de canto (barge board): un poco mas ancha que el tejado.
                    const Vec3 c = (a + b) * 0.5f + up * 0.5f - n * 0.05f + Vec3{xs * 0.02f, 0.0f, 0.0f};
                    geo_.box(frameMaterial() == kHouseBeams ? kHouseBeams : kHouseTrim, c,
                             Vec3{0.025f, th * 0.5f + 0.06f, core::length(b - a) * 0.5f + 0.02f}, kX, n, down, true);
                }
            }
        }
        // Cumbrera.
        const float top = ridge_ + th * std::sqrt(1.0f + tan_ * tan_);
        if (thatch) {
            geo_.cylinder(kHouseThatch, Vec3{x0 + 0.05f, top - 0.12f, 0.0f}, Vec3{x1 - 0.05f, top - 0.12f, 0.0f}, 0.26f, 10, true, true);
        } else {
            const float d = 0.13f;
            const Vec3 ay = core::normalize(Vec3{0.0f, 1.0f, 1.0f});
            const Vec3 az = core::normalize(Vec3{0.0f, -1.0f, 1.0f});
            geo_.box(roof_m_, Vec3{0.0f, top - d * 0.35f, 0.0f}, Vec3{(x1 - x0) * 0.5f + 0.03f, d, d}, kX, ay, az, false, 0);
        }
    }

    void porch() {
        const float P = 2.0f;
        const float z0 = zo_;
        const float z1 = zo_ + P;
        const float hx = logs() ? xo_ + log_ext_ * 0.5f : xo_;
        const float deck = plinth_ - 0.03f;
        const bool farm = s_.style == HouseStyle::Farmhouse;
        // Tarima (tablas a lo largo) y faldon.
        geo_.boxMinMax(kHousePlanks, Vec3{-hx, deck - 0.08f, z0}, Vec3{hx, deck, z1}, true);
        const int skirt = s_.style == HouseStyle::StoneCottage ? kHouseStone : kHousePlanks;
        geo_.boxMinMax(skirt, Vec3{-hx + 0.05f, -1.0f, z1 - 0.12f}, Vec3{hx - 0.05f, deck - 0.08f, z1 - 0.06f}, false, 4);
        geo_.boxMinMax(skirt, Vec3{-hx + 0.05f, -1.0f, z0}, Vec3{-hx + 0.11f, deck - 0.08f, z1 - 0.06f}, false, 4);
        geo_.boxMinMax(skirt, Vec3{hx - 0.11f, -1.0f, z0}, Vec3{hx - 0.05f, deck - 0.08f, z1 - 0.06f}, false, 4);
        // Tejadillo: pendiente suave; empieza en la pared o sobre el tejado.
        const float t12 = std::tan(14.0f * kPi / 180.0f);
        const float zp = z1 - 0.15f;
        const float beam_bottom = deck + 2.25f;
        const float beam_h = 0.18f;
        const float under_at_post = beam_bottom + beam_h;
        const auto under = [&](float z) { return under_at_post + (zp - z) * t12; };
        const float th = 0.12f;
        const float sec12 = std::sqrt(1.0f + t12 * t12);
        const float main_top_ridge = ridge_ + roof_th_ * std::sqrt(1.0f + tan_ * tan_);
        // Donde la cara de arriba del tejadillo toca la del tejado principal.
        float zs = (main_top_ridge - under_at_post - zp * t12 - th * sec12 + 0.03f) / (tan_ - t12);
        const float main_eave = zo_ + std::max(s_.roof_overhang, 0.2f);
        // Mas alla del alero: el tejadillo queda por debajo y sale de la pared.
        // Si no, nace del tejado (el tramo de dentro queda oculto en el).
        if (zs > main_eave - 0.05f) zs = z0;
        zs = std::max(zs, zo_ * 0.4f);
        const float ze = z1 + 0.3f;
        const float x0 = -hx - 0.2f;
        const float x1 = hx + 0.2f;
        const Vec3 n = core::normalize(Vec3{0.0f, 1.0f, t12});
        const Vec3 up = n * th;
        const Vec3 a0{x0, under(zs), zs};
        const Vec3 a1{x1, under(zs), zs};
        const Vec3 b0{x0, under(ze), ze};
        const Vec3 b1{x1, under(ze), ze};
        const Vec3 down = core::normalize(b0 - a0);
        const std::array<float, 2> rm = houseTextureMeters(kHouseRoof);
        const float slope = core::length(b0 - a0);
        geo_.polygon(kHouseRoof, {a0 + up, a1 + up, b1 + up, b0 + up}, {n, n, n, n},
                     {Vec2{x0 / rm[0], 0.1f}, Vec2{x1 / rm[0], 0.1f}, Vec2{x1 / rm[0], 0.1f + slope / rm[1]},
                      Vec2{x0 / rm[0], 0.1f + slope / rm[1]}},
                     n);
        geo_.flat(kHousePlanks, {a0, a1, b1, b0}, n * -1.0f, kX, down);
        geo_.flat(kHouseTrim, {b0, b1, b1 + up, b0 + up}, kZ, kX, kUp * -1.0f);
        for (float x : {x0, x1}) {
            const float xs = x < 0.0f ? -1.0f : 1.0f;
            geo_.flat(kHouseTrim, {a0 + Vec3{x - x0, 0.0f, 0.0f}, b0 + Vec3{x - x0, 0.0f, 0.0f}, b0 + up + Vec3{x - x0, 0.0f, 0.0f},
                                   a0 + up + Vec3{x - x0, 0.0f, 0.0f}},
                      Vec3{xs, 0.0f, 0.0f}, kZ, kUp * -1.0f);
        }
        // Viga y postes.
        if (logs()) {
            geo_.log(Vec3{x0 + 0.1f, beam_bottom + beam_h * 0.5f, zp}, Vec3{x1 - 0.1f, beam_bottom + beam_h * 0.5f, zp}, 0.11f, 10,
                     true, true, 0.3f, 0.0f);
        } else {
            geo_.boxMinMax(farm ? kHouseTrim : kHousePlanks, Vec3{x0 + 0.1f, beam_bottom, zp - 0.08f},
                           Vec3{x1 - 0.1f, beam_bottom + beam_h, zp + 0.08f}, true);
        }
        std::vector<float> posts;
        const int count = std::max(2, static_cast<int>(std::round((2.0f * hx) / 2.6f)) + 1);
        for (int i = 0; i < count; ++i) {
            float x = -hx + 0.15f + (2.0f * hx - 0.3f) * static_cast<float>(i) / static_cast<float>(count - 1);
            if (std::abs(x - door_s_) < 0.85f) x = door_s_ + (x < door_s_ ? -0.85f : 0.85f);
            posts.push_back(x);
        }
        for (float x : posts) {
            if (logs()) {
                geo_.log(Vec3{x, deck, zp}, Vec3{x, beam_bottom + 0.02f, zp}, 0.1f, 10, false, true, x, 0.0f);
            } else {
                geo_.boxMinMax(farm ? kHouseTrim : kHousePlanks, Vec3{x - 0.07f, deck, zp - 0.07f},
                               Vec3{x + 0.07f, beam_bottom, zp + 0.07f});
            }
        }
        // Barandilla (sin el hueco de los escalones).
        const int rail_m = farm ? kHouseTrim : kHousePlanks;
        const auto rail = [&](Vec3 a, Vec3 b) {
            const Vec3 d = b - a;
            const float len = core::length(d);
            if (len < 0.2f) return;
            const Vec3 dir = d * (1.0f / len);
            const Vec3 side = core::cross(kUp, dir);
            for (float y : {deck + 0.9f, deck + 0.14f}) {
                geo_.box(rail_m, (a + b) * 0.5f + kUp * y, Vec3{0.04f, 0.035f, len * 0.5f}, side, kUp, dir, true);
            }
            const int n_bal = static_cast<int>(len / 0.14f);
            for (int i = 1; i < n_bal; ++i) {
                const Vec3 p = a + dir * (len * static_cast<float>(i) / static_cast<float>(n_bal));
                geo_.box(rail_m, p + kUp * (deck + 0.52f), Vec3{0.02f, 0.34f, 0.02f}, side, kUp, dir, false, 4 | 8);
            }
        };
        const float gap0 = door_s_ - 0.75f;
        const float gap1 = door_s_ + 0.75f;
        std::vector<float> front = posts;
        front.push_back(gap0);
        front.push_back(gap1);
        std::sort(front.begin(), front.end());
        for (std::size_t i = 0; i + 1 < front.size(); ++i) {
            const float a = front[i];
            const float b = front[i + 1];
            if (a >= gap0 - 1e-3f && b <= gap1 + 1e-3f) continue;
            rail(Vec3{a + 0.08f, 0.0f, zp}, Vec3{b - 0.08f, 0.0f, zp});
        }
        for (float x : {posts.front(), posts.back()}) rail(Vec3{x, 0.0f, z0 + (logs() ? log_r_ : 0.05f)}, Vec3{x, 0.0f, zp - 0.08f});
        steps(Vec3{door_s_, deck, z1}, 1.4f);
    }

    // Escalones que bajan de `top` (borde de arriba, mirando a +Z) al suelo.
    void steps(const Vec3& top, float width) {
        const int count = std::max(1, static_cast<int>(std::ceil(top.y / 0.19f)) - 1);
        const float rise = top.y / static_cast<float>(count + 1);
        const int m = s_.style == HouseStyle::TimberCabin || logs() ? kHousePlanks : kHouseStone;
        for (int i = 0; i < count; ++i) {
            const float y1 = top.y - rise * static_cast<float>(i + 1);
            const float z0 = top.z + 0.3f * static_cast<float>(i);
            geo_.boxMinMax(m, Vec3{top.x - width * 0.5f, -1.0f, z0 - 0.02f}, Vec3{top.x + width * 0.5f, y1, z0 + 0.3f}, true, 4);
        }
    }

    void frontSteps() {
        const float z = s_.depth * 0.5f + outerFace() + (logs() ? 0.05f : 0.0f);
        steps(Vec3{door_s_, plinth_, z}, 1.3f);
    }

    // --- Chimenea de piedra en el hastial derecho ---
    void chimney() {
        const float xb = xo_ + (logs() ? log_ext_ * 0.2f : 0.0f);
        const float base_d = 0.85f;
        const float base_w = 1.2f;
        const float shoulder = top_ - 0.5f;
        const float stack_w = 0.72f;
        const float stack_d = 0.55f;
        const float stack_top = ridge_ + roof_th_ * std::sqrt(1.0f + tan_ * tan_) + 0.9f;
        const float xc = xb + base_d * 0.5f;
        geo_.boxMinMax(kHouseStone, Vec3{xb, -1.2f, -base_w * 0.5f}, Vec3{xb + base_d, shoulder, base_w * 0.5f}, false, 4);
        // Hombros inclinados (se estrecha).
        const float sh = 0.45f;
        geo_.boxMinMax(kHouseStone, Vec3{xc - stack_d * 0.5f - 0.1f, shoulder, -stack_w * 0.5f - 0.12f},
                       Vec3{xc + stack_d * 0.5f + 0.1f, shoulder + sh, stack_w * 0.5f + 0.12f}, false, 4);
        geo_.boxMinMax(kHouseStone, Vec3{xc - stack_d * 0.5f, shoulder + sh, -stack_w * 0.5f},
                       Vec3{xc + stack_d * 0.5f, stack_top, stack_w * 0.5f}, false, 4 | 8);
        // Remate y boca.
        geo_.boxMinMax(kHouseStone, Vec3{xc - stack_d * 0.5f - 0.06f, stack_top, -stack_w * 0.5f - 0.06f},
                       Vec3{xc + stack_d * 0.5f + 0.06f, stack_top + 0.1f, stack_w * 0.5f + 0.06f}, false, 8);
        const float ri = 0.14f;
        geo_.flat(kHouseIron,
                  {Vec3{xc - ri, stack_top + 0.1f, -ri * 1.3f}, Vec3{xc + ri, stack_top + 0.1f, -ri * 1.3f},
                   Vec3{xc + ri, stack_top + 0.1f, ri * 1.3f}, Vec3{xc - ri, stack_top + 0.1f, ri * 1.3f}},
                  kUp, kX, kZ);
        std::vector<Vec3> ring = {
            Vec3{xc - stack_d * 0.5f - 0.06f, stack_top + 0.1f, -stack_w * 0.5f - 0.06f},
            Vec3{xc + stack_d * 0.5f + 0.06f, stack_top + 0.1f, -stack_w * 0.5f - 0.06f},
            Vec3{xc + stack_d * 0.5f + 0.06f, stack_top + 0.1f, stack_w * 0.5f + 0.06f},
            Vec3{xc - stack_d * 0.5f - 0.06f, stack_top + 0.1f, stack_w * 0.5f + 0.06f},
        };
        // Borde de arriba alrededor de la boca (cuatro franjas).
        const float y = stack_top + 0.1f;
        geo_.flat(kHouseStone, {ring[0], ring[1], Vec3{ring[1].x, y, -ri * 1.3f}, Vec3{ring[0].x, y, -ri * 1.3f}}, kUp, kX, kZ);
        geo_.flat(kHouseStone, {Vec3{ring[3].x, y, ri * 1.3f}, Vec3{ring[2].x, y, ri * 1.3f}, ring[2], ring[3]}, kUp, kX, kZ);
        geo_.flat(kHouseStone, {Vec3{ring[0].x, y, -ri * 1.3f}, Vec3{xc - ri, y, -ri * 1.3f}, Vec3{xc - ri, y, ri * 1.3f},
                                Vec3{ring[0].x, y, ri * 1.3f}},
                  kUp, kX, kZ);
        geo_.flat(kHouseStone, {Vec3{xc + ri, y, -ri * 1.3f}, Vec3{ring[1].x, y, -ri * 1.3f}, Vec3{ring[1].x, y, ri * 1.3f},
                                Vec3{xc + ri, y, ri * 1.3f}},
                  kUp, kX, kZ);
    }

    // --- Taberna: cartel colgado de una mensula junto a la puerta y barriles ---
    void tavernSign() {
        const Wall w = floors_ >= 2 ? upperWall(0) : walls_[0];
        const float s = door_s_ + 1.25f;
        const float y = floors_ >= 2 ? upperBase() + 0.35f : top_ - 0.45f;
        const float face = outerFace();
        const Vec3 root = at(w, s, y, face);
        const Vec3 tip = at(w, s, y, face + 1.15f);
        geo_.beam(kHouseBeams, root, tip, 0.09f, 0.11f);
        geo_.beam(kHouseBeams, at(w, s, y - 0.6f, face), at(w, s, y - 0.04f, face + 0.7f), 0.07f, 0.07f, w.along);
        // Cadenas y la tabla (con un marco), colgando de la punta.
        for (float o : {0.32f, 0.88f}) {
            geo_.box(kHouseIron, at(w, s, y - 0.2f, face + o), Vec3{0.008f, 0.16f, 0.008f}, w.along, kUp, w.out);
        }
        housegeo::wallBox(geo_, kHousePlanks, w, s - 0.03f, s + 0.03f, y - 0.95f, y - 0.36f, face + 0.25f, face + 0.95f, true);
        housegeo::wallBox(geo_, kHouseBeams, w, s - 0.04f, s + 0.04f, y - 0.4f, y - 0.35f, face + 0.22f, face + 0.98f);
        housegeo::wallBox(geo_, kHouseBeams, w, s - 0.04f, s + 0.04f, y - 0.98f, y - 0.93f, face + 0.22f, face + 0.98f);
        // Una jarra pintada (un toque de hierro en la tabla) y barriles en la puerta.
        housegeo::wallBox(geo_, kHouseIron, w, s - 0.045f, s + 0.045f, y - 0.78f, y - 0.52f, face + 0.5f, face + 0.7f);
        const Vec3 b0 = at(walls_[0], door_s_ - 1.25f, 0.0f, outerFace() + 0.45f);
        housegeo::barrel(geo_, Vec3{b0.x, 0.0f, b0.z}, 0.3f, 0.85f, 0.2f);
        housegeo::barrel(geo_, Vec3{b0.x - 0.7f, 0.0f, b0.z + 0.1f}, 0.28f, 0.8f, 0.7f);
        housegeo::bench(geo_, frameAt(at(walls_[0], door_s_ + 2.0f, 0.0f, outerFace() + 0.35f), 0), 1.6f);
    }

    // --- Herreria: cobertizo abierto en el lado izquierdo con yunque y lena ---
    void smithyShed() {
        const float x_wall = -xo_;
        const float depth = 3.4f;
        const float x_out = x_wall - depth;
        const float hz = s_.depth * 0.5f;
        // Por debajo del alero del hastial (no lo atraviesa).
        const float y_high = std::min(top_ - 0.4f, 3.0f);
        const float y_low = std::min(2.3f, y_high - 0.3f);
        // Postes y viga.
        for (float z : {-hz + 0.2f, 0.0f, hz - 0.2f}) {
            geo_.boxMinMax(kHouseBeams, Vec3{x_out + 0.15f - 0.08f, 0.0f, z - 0.08f}, Vec3{x_out + 0.15f + 0.08f, y_low, z + 0.08f});
            geo_.beam(kHouseBeams, Vec3{x_out + 0.15f, y_low - 0.6f, z}, Vec3{x_out + 0.75f, y_low - 0.02f, z}, 0.07f, 0.07f, kZ);
        }
        geo_.boxMinMax(kHouseBeams, Vec3{x_out + 0.05f, y_low - 0.02f, -hz - 0.1f}, Vec3{x_out + 0.25f, y_low + 0.16f, hz + 0.1f}, true);
        // Tejadillo inclinado (tablillas arriba, tablas abajo).
        const float ov = 0.35f;
        const Vec3 a0{x_wall + 0.02f, y_high, -hz - 0.3f};
        const Vec3 a1{x_wall + 0.02f, y_high, hz + 0.3f};
        const float slope_t = (y_high - y_low - 0.16f) / (x_wall - x_out - 0.15f);
        const Vec3 b0{x_out - ov, y_low + 0.16f - ov * slope_t, -hz - 0.3f};
        const Vec3 b1{x_out - ov, y_low + 0.16f - ov * slope_t, hz + 0.3f};
        const Vec3 n = core::normalize(core::cross(a1 - a0, b0 - a0)) * -1.0f;
        const Vec3 nn = n.y < 0.0f ? n * -1.0f : n;
        const Vec3 up = nn * 0.12f;
        const std::array<float, 2> rm = houseTextureMeters(kHouseRoof);
        const float slope = core::length(b0 - a0);
        geo_.polygon(kHouseRoof, {a0 + up, a1 + up, b1 + up, b0 + up}, {nn, nn, nn, nn},
                     {Vec2{a0.z / rm[0], 0.0f}, Vec2{a1.z / rm[0], 0.0f}, Vec2{a1.z / rm[0], slope / rm[1]}, Vec2{a0.z / rm[0], slope / rm[1]}},
                     nn);
        geo_.flat(kHousePlanks, {a0, a1, b1, b0}, nn * -1.0f, kZ, core::normalize(b0 - a0));
        geo_.flat(kHouseTrim, {b0, b1, b1 + up, b0 + up}, Vec3{-1.0f, 0.0f, 0.0f}, kZ, kUp * -1.0f);
        // Lo de debajo: yunque, pila de agua, lena y una muela.
        housegeo::anvil(geo_, frameAt(Vec3{x_wall - 1.6f, 0.0f, -0.6f}, 1));
        housegeo::barrel(geo_, Vec3{x_wall - 1.0f, 0.0f, -1.5f}, 0.32f, 0.7f, 0.4f);
        housegeo::woodpile(geo_, frameAt(Vec3{x_wall - 0.4f, 0.0f, 1.2f}, 3), 1.6f, 1.2f, rng_);
        const Vec3 wheel{x_out + 1.0f, 0.75f, 1.2f};
        geo_.cylinder(kHouseStone, wheel - kZ * 0.08f, wheel + kZ * 0.08f, 0.45f, 16, true, true);
        geo_.boxMinMax(kHouseBeams, Vec3{x_out + 0.9f, 0.0f, 1.0f}, Vec3{x_out + 1.1f, 0.75f, 1.08f});
        geo_.boxMinMax(kHouseBeams, Vec3{x_out + 0.9f, 0.0f, 1.32f}, Vec3{x_out + 1.1f, 0.75f, 1.4f});
    }

    // ---------------------------------------------------------------------
    // Interior
    // ---------------------------------------------------------------------

    // Hay una ventana en la pared `wall` (planta `floor`) entre a y b (x en las
    // paredes 0/1, z en las 2/3)?
    bool windowOver(int wall, int floor, float a, float b) const {
        const float yb = plinth_ + storey_ * static_cast<float>(floor);
        const float yt = yb + storey_;
        for (const Opening& o : openings_) {
            if (o.wall != wall || o.door || o.y0 < yb || o.y0 >= yt) continue;
            // s -> coordenada del mundo segun la pared.
            float lo = o.s0;
            float hi = o.s1;
            if (wall == 1 || wall == 2) {
                lo = -o.s1;
                hi = -o.s0;
            }
            if (lo < b + 0.05f && hi > a - 0.05f) return true;
        }
        return false;
    }

    void interior() {
        const float inset = innerInset() + 0.03f;
        for (int f = 0; f < floors_; ++f) {
            Room r;
            r.floor = f;
            r.y = plinth_ + storey_ * static_cast<float>(f) + 0.03f;
            r.x0 = -s_.width * 0.5f + inset;
            r.x1 = -r.x0;
            const float extra = f > 0 ? jetty_ : 0.0f;
            r.z0 = -s_.depth * 0.5f + inset - extra;
            r.z1 = s_.depth * 0.5f - inset + extra;
            rooms_.push_back(r);
        }
        // Lo que no se puede pisar: la puerta, la escalera y su hueco (y la
        // llegada arriba).
        Room& ground = rooms_[0];
        ground.use(door_s_ - 0.9f, door_s_ + 0.9f, ground.z1 - 1.35f, ground.z1 + 0.5f);
        for (const Flight& fl : flights_) {
            Room& below = rooms_[static_cast<std::size_t>(fl.floor)];
            below.use(fl.foot[0], fl.foot[1], fl.foot[2], fl.foot[3]);
            Room& above = rooms_[static_cast<std::size_t>(fl.floor + 1)];
            above.use(fl.hole[0] - 0.15f, fl.hole[1] + 1.0f, fl.hole[2] - 0.1f, fl.hole[3] + 0.9f);
            // Lo que ocupa la escalera de la planta de arriba en la de abajo no
            // deja poner un mueble alto: con el pie basta.
        }
        switch (s_.use) {
            case HouseUse::Tavern: tavernInterior(); break;
            case HouseUse::Smithy: smithyInterior(); break;
            case HouseUse::Shop: shopInterior(); break;
            default: homeInterior(); break;
        }
        // Vigas de atado bajo el tejado en la planta de arriba del todo.
        const Room& topf = rooms_.back();
        const float y = top_ - 0.1f;
        const float x_hood = s_.chimney ? topf.x1 - 1.05f : 1e9f;
        for (float x = topf.x0 + 0.8f; x < topf.x1 - 0.4f; x += 2.0f) {
            if (x > x_hood - 0.2f) continue;
            if (logs()) {
                geo_.log(Vec3{x, y, topf.z0 - 0.05f}, Vec3{x, y, topf.z1 + 0.05f}, 0.11f, 8, false, false, x, 0.0f);
            } else {
                geo_.boxMinMax(kHouseBeams, Vec3{x - 0.09f, y - 0.11f, topf.z0}, Vec3{x + 0.09f, y + 0.09f, topf.z1}, true);
            }
        }
    }

    float ceilingOf(int floor) const {
        return floor + 1 >= floors_ ? top_ - 0.05f : plinth_ + storey_ * static_cast<float>(floor + 1) - 0.13f;
    }

    // Hogar contra la pared de la chimenea (+X), con alfombra, taburetes y lena.
    void hearth(Room& r) {
        if (!s_.chimney) return;
        r.use(r.x1 - 1.05f, r.x1, -0.95f, 0.95f);
        housegeo::fireplace(geo_, frameAt(Vec3{r.x1 - 0.33f, r.y, 0.0f}, 3), 1.5f, ceilingOf(r.floor));
        if (r.free(r.x1 - 2.45f, r.x1 - 1.1f, -0.75f, 0.75f)) housegeo::rug(geo_, frameAt(Vec3{r.x1 - 1.75f, r.y, 0.0f}, 3), 1.3f, 1.2f);
        for (float z : {-0.95f, 0.95f}) {
            if (r.take(r.x1 - 1.9f, r.x1 - 1.45f, z - 0.22f, z + 0.22f)) housegeo::stool(geo_, frameAt(Vec3{r.x1 - 1.67f, r.y, z}, 3));
        }
        if (r.take(r.x1 - 0.5f, r.x1, -2.0f, -1.0f)) housegeo::woodpile(geo_, frameAt(Vec3{r.x1 - 0.27f, r.y, -1.5f}, 3), 0.9f, 0.7f, rng_);
    }

    // Cama en la primera esquina libre (con su arcon a los pies).
    bool placeBed(Room& r, float w = 1.0f) {
        const float l = 2.0f;
        struct Corner {
            float cx, cz;
            int facing;  // hacia donde mira el pie de la cama
            bool along_x;
        };
        const Corner corners[] = {
            {r.x0 + w * 0.5f + 0.02f, r.z0 + l * 0.5f + 0.02f, 0, false},
            {r.x1 - w * 0.5f - 0.02f, r.z0 + l * 0.5f + 0.02f, 0, false},
            {r.x0 + l * 0.5f + 0.02f, r.z1 - w * 0.5f - 0.02f, 1, true},
            {r.x0 + l * 0.5f + 0.02f, r.z0 + w * 0.5f + 0.02f, 1, true},
            {r.x1 - l * 0.5f - 0.02f, r.z0 + w * 0.5f + 0.02f, 3, true},
            {r.x0 + w * 0.5f + 0.02f, r.z1 - l * 0.5f - 0.02f, 2, false},
        };
        for (const Corner& c : corners) {
            const float hx = (c.along_x ? l : w) * 0.5f;
            const float hz = (c.along_x ? w : l) * 0.5f;
            if (!r.take(c.cx - hx, c.cx + hx, c.cz - hz, c.cz + hz)) continue;
            const Frame f = frameAt(Vec3{c.cx, r.y, c.cz}, c.facing);
            housegeo::bed(geo_, f, w, l);
            // Arcon a los pies.
            const Vec3 foot = f.p(0.0f, 0.0f, l * 0.5f + 0.32f);
            const float ax = c.along_x ? 0.27f : 0.47f;
            const float az = c.along_x ? 0.47f : 0.27f;
            if (r.take(foot.x - ax, foot.x + ax, foot.z - az, foot.z + az)) {
                housegeo::chest(geo_, frameAt(Vec3{foot.x, r.y, foot.z}, c.facing), 0.85f, 0.48f, 0.5f);
            }
            return true;
        }
        return false;
    }

    // Mesa con bancos (o taburetes si no cabe) en el primer sitio libre.
    bool placeTable(Room& r, float w = 1.5f) {
        const float cx = (r.x0 + r.x1) * 0.5f;
        const float cz = (r.z0 + r.z1) * 0.5f;
        std::vector<std::pair<float, float>> spots;
        for (float dz : {0.0f, -0.6f, 0.6f, -1.2f, 1.2f}) {
            for (float dx : {-0.25f, -0.9f, 0.4f, -1.6f, 1.1f, -2.4f, 1.8f}) {
                spots.push_back({cx + dx * (r.x1 - r.x0) * 0.25f, cz + dz * (r.z1 - r.z0) * 0.25f});
            }
        }
        for (const auto& [x, z] : spots) {
            const float hx = w * 0.5f + 0.15f;
            if (!r.take(x - hx, x + hx, z - 0.95f, z + 0.95f)) continue;
            housegeo::table(geo_, frameAt(Vec3{x, r.y, z}, 0), w, 0.8f);
            housegeo::bench(geo_, frameAt(Vec3{x, r.y, z - 0.62f}, 0), w - 0.1f);
            housegeo::bench(geo_, frameAt(Vec3{x, r.y, z + 0.62f}, 2), w - 0.1f);
            return true;
        }
        for (const auto& [x, z] : spots) {
            if (!r.take(x - 0.75f, x + 0.75f, z - 0.75f, z + 0.75f)) continue;
            housegeo::table(geo_, frameAt(Vec3{x, r.y, z}, 0), 0.9f, 0.7f);
            housegeo::stool(geo_, frameAt(Vec3{x - 0.6f, r.y, z}, 1));
            housegeo::stool(geo_, frameAt(Vec3{x + 0.6f, r.y, z}, 3));
            return true;
        }
        return false;
    }

    // Mueble contra una pared (0 delante, 1 atras, 2 derecha, 3 izquierda): lo
    // prueba a lo largo de ella; `tall` solo donde no hay ventana.
    bool placeAgainstWall(Room& r, int wall, float w, float d, bool tall, const std::function<void(const Frame&)>& make) {
        const float span0 = wall < 2 ? r.x0 : r.z0;
        const float span1 = wall < 2 ? r.x1 : r.z1;
        for (float t : {0.75f, 0.25f, 0.5f, 0.9f, 0.1f, 0.62f, 0.38f}) {
            const float c = span0 + w * 0.5f + (span1 - span0 - w) * t;
            if (tall && windowOver(wall, r.floor, c - w * 0.5f, c + w * 0.5f)) continue;
            float x0, x1, z0, z1;
            int facing = 0;
            Vec3 o;
            switch (wall) {
                case 0: x0 = c - w * 0.5f; x1 = c + w * 0.5f; z0 = r.z1 - d; z1 = r.z1; facing = 2; o = Vec3{c, r.y, r.z1 - d * 0.5f}; break;
                case 1: x0 = c - w * 0.5f; x1 = c + w * 0.5f; z0 = r.z0; z1 = r.z0 + d; facing = 0; o = Vec3{c, r.y, r.z0 + d * 0.5f}; break;
                case 2: x0 = r.x1 - d; x1 = r.x1; z0 = c - w * 0.5f; z1 = c + w * 0.5f; facing = 3; o = Vec3{r.x1 - d * 0.5f, r.y, c}; break;
                default: x0 = r.x0; x1 = r.x0 + d; z0 = c - w * 0.5f; z1 = c + w * 0.5f; facing = 1; o = Vec3{r.x0 + d * 0.5f, r.y, c}; break;
            }
            if (!r.take(x0, x1, z0, z1)) continue;
            make(frameAt(o, facing));
            return true;
        }
        return false;
    }

    // Barriles y cajas en una esquina libre.
    void storageCorner(Room& r) {
        const float pts[4][2] = {{r.x0 + 0.45f, r.z1 - 0.45f}, {r.x1 - 0.45f, r.z1 - 0.45f}, {r.x1 - 0.45f, r.z0 + 0.45f},
                                 {r.x0 + 0.45f, r.z0 + 0.45f}};
        for (const auto& p : pts) {
            if (!r.take(p[0] - 0.42f, p[0] + 0.42f, p[1] - 0.42f, p[1] + 0.42f)) continue;
            if (rng_.chance(0.6f)) {
                housegeo::barrel(geo_, Vec3{p[0], r.y, p[1]}, 0.3f, 0.85f, rng_.range(0.0f, 1.0f));
            } else {
                housegeo::crate(geo_, frameAt(Vec3{p[0], r.y, p[1]}, rng_.pick(4)), 0.62f);
                if (rng_.chance(0.5f)) housegeo::crate(geo_, frameAt(Vec3{p[0], r.y, p[1]}, rng_.pick(4)), 0.45f, 0.62f);
            }
            return;
        }
    }

    void homeInterior() {
        Room& g = rooms_[0];
        hearth(g);
        if (floors_ == 1) placeBed(g, 1.1f);
        placeTable(g, (g.x1 - g.x0) > 6.0f ? 1.6f : 1.3f);
        placeAgainstWall(g, 1, 1.2f, 0.45f, true, [&](const Frame& f) { housegeo::shelf(geo_, f, 1.2f, 1.75f, 0.4f, rng_); });
        placeAgainstWall(g, 3, 1.0f, 0.5f, false, [&](const Frame& f) { housegeo::cupboard(geo_, f, 1.0f, 0.8f, 0.48f); });
        storageCorner(g);
        storageCorner(g);
        if (floors_ == 1 && g.x1 - g.x0 > 6.5f) placeBed(g, 0.9f);
        // Plantas altas: dormitorios.
        for (std::size_t f = 1; f < rooms_.size(); ++f) {
            Room& r = rooms_[f];
            placeBed(r, 1.2f);
            placeBed(r, 0.9f);
            placeAgainstWall(r, 1, 1.0f, 0.45f, true, [&](const Frame& fr) { housegeo::shelf(geo_, fr, 1.0f, 1.6f, 0.38f, rng_); });
            placeAgainstWall(r, 3, 0.9f, 0.5f, false, [&](const Frame& fr) { housegeo::chest(geo_, fr, 0.9f, 0.48f, 0.52f); });
            const float cx = (r.x0 + r.x1) * 0.5f;
            const float cz = (r.z0 + r.z1) * 0.5f;
            if (r.free(cx - 0.8f, cx + 0.8f, cz - 0.6f, cz + 0.6f)) housegeo::rug(geo_, frameAt(Vec3{cx, r.y, cz}, 0), 1.6f, 1.1f);
            storageCorner(r);
        }
    }

    void tavernInterior() {
        Room& g = rooms_[0];
        hearth(g);
        // Barra contra la pared de atras, con barriles tumbados y baldas detras.
        float cx0 = g.x0 + 0.6f;
        for (const Flight& fl : flights_) {
            if (fl.floor == 0) cx0 = std::max(cx0, fl.foot[1] + 0.3f);
        }
        const float cx1 = g.x1 - (s_.chimney ? 1.2f : 0.5f);
        const float length = std::min(cx1 - cx0, 5.0f);
        if (length > 1.6f) {
            const float cxc = cx1 - length * 0.5f;
            const float cz = g.z0 + 1.35f;
            if (g.take(cx1 - length - 0.1f, cx1 + 0.05f, g.z0, cz + 0.4f)) {
                housegeo::counter(geo_, frameAt(Vec3{cxc, g.y, cz}, 0), length);
                // Estanteria de barriles tumbados (dos alturas) contra la pared.
                const float rack_z = g.z0 + 0.35f;
                geo_.boxMinMax(kHouseBeams, Vec3{cxc - length * 0.5f, g.y, rack_z - 0.3f}, Vec3{cxc + length * 0.5f, g.y + 0.12f, rack_z + 0.3f}, true);
                for (float x = cxc - length * 0.5f + 0.4f; x < cxc + length * 0.5f - 0.3f; x += 0.75f) {
                    housegeo::barrelLying(geo_, Vec3{x, g.y + 0.42f, rack_z}, kZ, 0.29f, 0.62f);
                    if (x + 0.37f < cxc + length * 0.5f - 0.3f) housegeo::barrelLying(geo_, Vec3{x + 0.37f, g.y + 0.98f, rack_z}, kZ, 0.26f, 0.58f);
                }
                // Jarras en la barra.
                for (float x = cxc - length * 0.35f; x < cxc + length * 0.4f; x += rng_.range(0.4f, 0.9f)) {
                    geo_.frustum(kHouseIron, Vec3{x, g.y + 1.08f, cz + 0.05f}, Vec3{x, g.y + 1.22f, cz + 0.05f}, 0.05f, 0.045f, 8, false, true);
                }
            }
        }
        for (int i = 0; i < 5; ++i) {
            if (!placeTable(g, 1.4f)) break;
        }
        storageCorner(g);
        storageCorner(g);
        // Arriba: cuartos con su cama, arcon y un biombo de tablas entre camas.
        for (std::size_t f = 1; f < rooms_.size(); ++f) {
            Room& r = rooms_[f];
            for (int b = 0; b < 4; ++b) {
                if (!placeBed(r, 0.95f)) break;
            }
            placeAgainstWall(r, 3, 0.9f, 0.5f, false, [&](const Frame& fr) { housegeo::chest(geo_, fr, 0.9f, 0.48f, 0.5f); });
            if (r.x1 - r.x0 > 6.0f) {
                const float xm = (r.x0 + r.x1) * 0.5f;
                if (r.take(xm - 0.04f, xm + 0.04f, r.z0, r.z0 + 2.3f)) {
                    geo_.boxMinMax(kHousePlanks, Vec3{xm - 0.03f, r.y, r.z0}, Vec3{xm + 0.03f, r.y + 1.95f, r.z0 + 2.3f}, true);
                }
            }
            storageCorner(r);
        }
    }

    void smithyInterior() {
        Room& g = rooms_[0];
        // Fragua contra la pared de la chimenea.
        if (g.take(g.x1 - 1.3f, g.x1, -0.95f, 0.95f)) {
            housegeo::forge(geo_, frameAt(Vec3{g.x1 - 0.6f, g.y, 0.0f}, 3), ceilingOf(0));
        }
        if (g.take(g.x1 - 2.3f, g.x1 - 1.5f, -0.45f, 0.45f)) housegeo::anvil(geo_, frameAt(Vec3{g.x1 - 1.9f, g.y, 0.0f}, 1));
        if (g.take(g.x1 - 1.9f, g.x1 - 1.2f, 1.0f, 1.7f)) housegeo::barrel(geo_, Vec3{g.x1 - 1.55f, g.y, 1.35f}, 0.33f, 0.72f, 0.3f);
        // Banco de trabajo con herramientas y el estante de los martillos.
        placeAgainstWall(g, 1, 1.8f, 0.75f, false, [&](const Frame& f) {
            housegeo::table(geo_, frameAt(f.p(0.0f, 0.0f, 0.0f), 0), 1.8f, 0.7f, 0.85f);
            for (float x = -0.6f; x <= 0.6f; x += 0.3f) {
                housegeo::fbox(geo_, kHouseIron, f, Vec3{x, 0.88f, rng_.range(-0.15f, 0.15f)}, Vec3{0.1f, 0.02f, 0.03f});
            }
        });
        placeAgainstWall(g, 3, 1.4f, 0.25f, true, [&](const Frame& f) {
            housegeo::fbox(geo_, kHouseBeams, f, Vec3{0.0f, 1.55f, -0.08f}, Vec3{0.7f, 0.05f, 0.04f});
            for (float x = -0.55f; x <= 0.56f; x += 0.22f) {
                housegeo::fbox(geo_, kHouseBeams, f, Vec3{x, 1.3f, 0.0f}, Vec3{0.015f, 0.25f, 0.015f});
                housegeo::fbox(geo_, kHouseIron, f, Vec3{x, 1.05f, 0.0f}, Vec3{0.06f, 0.035f, 0.03f});
            }
        });
        // Carbon y hierro en bruto.
        if (g.take(g.x1 - 0.9f, g.x1, -2.0f, -1.1f)) {
            geo_.boxMinMax(kHouseIron, Vec3{g.x1 - 0.85f, g.y, -1.95f}, Vec3{g.x1 - 0.05f, g.y + 0.3f, -1.15f}, false, 4);
        }
        storageCorner(g);
        storageCorner(g);
    }

    void shopInterior() {
        Room& g = rooms_[0];
        hearth(g);
        // Mostrador mirando a la puerta y estanterias en las paredes.
        const float cz = (g.z0 + g.z1) * 0.5f - 0.3f;
        const float len = std::min(3.0f, (g.x1 - g.x0) * 0.5f);
        if (g.take(door_s_ - len * 0.5f - 0.1f, door_s_ + len * 0.5f + 0.1f, cz - 0.4f, cz + 0.4f)) {
            housegeo::counter(geo_, frameAt(Vec3{door_s_, g.y, cz}, 0), len);
        }
        for (int i = 0; i < 3; ++i) {
            placeAgainstWall(g, 1, 1.3f, 0.42f, true, [&](const Frame& f) { housegeo::shelf(geo_, f, 1.3f, 1.8f, 0.4f, rng_); });
        }
        placeAgainstWall(g, 3, 1.2f, 0.42f, true, [&](const Frame& f) { housegeo::shelf(geo_, f, 1.2f, 1.8f, 0.4f, rng_); });
        storageCorner(g);
        storageCorner(g);
        storageCorner(g);
        for (std::size_t f = 1; f < rooms_.size(); ++f) {
            Room& r = rooms_[f];
            placeBed(r, 1.2f);
            placeAgainstWall(r, 3, 0.9f, 0.5f, false, [&](const Frame& fr) { housegeo::chest(geo_, fr, 0.9f, 0.48f, 0.52f); });
            storageCorner(r);
        }
    }
};

}  // namespace


const char* houseStyleName(HouseStyle style) {
    switch (style) {
        case HouseStyle::LogCabin: return "Cabana de troncos";
        case HouseStyle::TimberCabin: return "Cabana de tablas";
        case HouseStyle::StoneCottage: return "Casita de piedra";
        case HouseStyle::Farmhouse: return "Casa de campo";
        case HouseStyle::HalfTimbered: return "Casa de entramado";
        case HouseStyle::Thatched: return "Cabana de paja";
    }
    return "Casa";
}

const char* houseUseName(HouseUse use) {
    switch (use) {
        case HouseUse::Home: return "Vivienda";
        case HouseUse::Tavern: return "Taberna";
        case HouseUse::Smithy: return "Herreria";
        case HouseUse::Shop: return "Tienda";
    }
    return "Vivienda";
}

const char* houseMaterialName(int material) {
    static const char* const kNames[kHouseMaterialCount] = {"Troncos", "Veta",    "Tablas",  "Revestimiento", "Piedra",
                                                             "Tejado",  "Marcos",  "Vidrio",  "Hierro",        "Enlucido",
                                                             "Paja",    "Teja",    "Tela",    "Vigas"};
    return material >= 0 && material < kHouseMaterialCount ? kNames[material] : "";
}

const char* houseTextureName(int material) {
    static const char* const kNames[kHouseMaterialCount] = {"Troncos", "Veta",   "Tablas", "Revestimiento", "Piedra",
                                                             "Tejado",  "Marcos", "",       "",              "Enlucido",
                                                             "Paja",    "Teja",   "Tela",   "Vigas"};
    return material >= 0 && material < kHouseMaterialCount ? kNames[material] : "";
}

std::array<float, 2> houseTextureMeters(int material) {
    switch (material) {
        case kHouseLogs: return {2.0f, 1.0f};
        case kHousePlanks: return {1.2f, 1.2f};
        case kHouseSiding: return {1.0f, 1.0f};
        case kHouseStone: return {1.5f, 1.5f};
        case kHouseRoof: return {1.0f, 1.0f};
        case kHouseTrim: return {1.0f, 1.0f};
        case kHousePlaster: return {2.0f, 2.0f};
        case kHouseThatch: return {1.5f, 1.5f};
        case kHouseTile: return {1.0f, 1.0f};
        case kHouseCloth: return {1.0f, 1.0f};
        case kHouseBeams: return {2.0f, 0.5f};
        default: return {1.0f, 1.0f};
    }
}

HouseSettings housePreset(HouseStyle style, std::uint32_t seed) {
    HouseSettings s;
    s.style = style;
    s.seed = seed;
    housegeo::Rng rng(seed * 31U + static_cast<std::uint32_t>(style));
    switch (style) {
        case HouseStyle::LogCabin:
            s.width = rng.range(6.5f, 8.5f);
            s.depth = rng.range(5.0f, 6.2f);
            s.wall_height = 2.75f;
            s.roof_pitch = rng.range(34.0f, 42.0f);
            s.roof_overhang = 0.6f;
            s.shutters = false;
            break;
        case HouseStyle::TimberCabin:
            s.width = rng.range(5.5f, 7.5f);
            s.depth = rng.range(4.5f, 5.8f);
            s.wall_height = 2.8f;
            s.roof_pitch = rng.range(35.0f, 45.0f);
            s.roof_overhang = 0.45f;
            s.porch = rng.chance(0.6f);
            break;
        case HouseStyle::StoneCottage:
            s.width = rng.range(6.0f, 8.0f);
            s.depth = rng.range(5.0f, 6.0f);
            s.wall_height = 2.7f;
            s.roof_pitch = rng.range(42.0f, 50.0f);
            s.roof_overhang = 0.35f;
            s.porch = false;
            break;
        case HouseStyle::Farmhouse:
            s.width = rng.range(8.0f, 10.0f);
            s.depth = rng.range(6.0f, 7.5f);
            s.floors = 2;
            s.wall_height = 2.75f;
            s.roof_pitch = rng.range(36.0f, 42.0f);
            s.roof_overhang = 0.5f;
            break;
        case HouseStyle::HalfTimbered:
            s.width = rng.range(6.5f, 9.0f);
            s.depth = rng.range(5.5f, 7.0f);
            s.floors = rng.chance(0.3f) ? 3 : 2;
            s.wall_height = 2.6f;
            s.roof_pitch = rng.range(48.0f, 56.0f);
            s.roof_overhang = 0.35f;
            s.porch = false;
            s.jetty = rng.range(0.3f, 0.5f);
            break;
        case HouseStyle::Thatched:
            s.width = rng.range(6.0f, 8.0f);
            s.depth = rng.range(4.8f, 5.8f);
            s.wall_height = 2.35f;
            s.roof_pitch = rng.range(48.0f, 54.0f);
            s.roof_overhang = 0.6f;
            s.porch = false;
            s.shutters = rng.chance(0.5f);
            break;
    }
    return s;
}

HouseModel buildHouse(const HouseSettings& settings) { return HouseBuilder(settings).build(); }

bool generateHouseTexture(int material, int size, std::uint32_t seed, HouseTextureSet& out) {
    TexelFunction f;
    float strength = 3.0f;
    switch (material) {
        case kHouseLogs: f = [seed](float u, float v) { return logTexel(u, v, seed); }; strength = 3.0f; break;
        case kHouseLogEnds: f = [seed](float u, float v) { return endTexel(u, v, seed + 1U); }; strength = 3.0f; break;
        case kHousePlanks: f = [seed](float u, float v) { return plankTexel(u, v, seed + 2U); }; strength = 3.5f; break;
        case kHouseSiding: f = [seed](float u, float v) { return sidingTexel(u, v, seed + 3U); }; strength = 5.0f; break;
        case kHouseStone: f = [seed](float u, float v) { return stoneTexel(u, v, seed + 4U); }; strength = 4.5f; break;
        case kHouseRoof: f = [seed](float u, float v) { return roofTexel(u, v, seed + 5U); }; strength = 5.0f; break;
        case kHouseTrim: f = [seed](float u, float v) { return trimTexel(u, v, seed + 6U); }; strength = 1.5f; break;
        case kHousePlaster: f = [seed](float u, float v) { return plasterTexel(u, v, seed + 7U); }; strength = 2.0f; break;
        case kHouseThatch: f = [seed](float u, float v) { return thatchTexel(u, v, seed + 8U); }; strength = 5.0f; break;
        case kHouseTile: f = [seed](float u, float v) { return tileTexel(u, v, seed + 9U); }; strength = 5.5f; break;
        case kHouseCloth: f = [seed](float u, float v) { return clothTexel(u, v, seed + 10U); }; strength = 1.5f; break;
        case kHouseBeams: f = [seed](float u, float v) { return beamTexel(u, v, seed + 11U); }; strength = 3.0f; break;
        default: return false;
    }
    const int n = std::max(size, 16);
    const auto count = static_cast<std::size_t>(n) * static_cast<std::size_t>(n);
    std::vector<float> height(count);
    for (ImageRgba8* img : {&out.color, &out.normal, &out.roughness, &out.occlusion, &out.height}) {
        img->width = static_cast<std::uint32_t>(n);
        img->height = static_cast<std::uint32_t>(n);
        img->pixels.assign(count * 4, 255);
    }
    const auto byte = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    parallelRows(n, [&](int y) {
        for (int x = 0; x < n; ++x) {
            const Texel t = f((static_cast<float>(x) + 0.5f) / static_cast<float>(n),
                              (static_cast<float>(y) + 0.5f) / static_cast<float>(n));
            const std::size_t i = static_cast<std::size_t>(y) * static_cast<std::size_t>(n) + static_cast<std::size_t>(x);
            height[i] = std::clamp(t.height, 0.0f, 1.0f);
            out.color.pixels[i * 4 + 0] = byte(t.color.x);
            out.color.pixels[i * 4 + 1] = byte(t.color.y);
            out.color.pixels[i * 4 + 2] = byte(t.color.z);
            const std::uint8_t r = byte(t.roughness);
            const std::uint8_t o = byte(t.occlusion);
            const std::uint8_t h = byte(t.height);
            for (int c = 0; c < 3; ++c) {
                out.roughness.pixels[i * 4 + static_cast<std::size_t>(c)] = r;
                out.occlusion.pixels[i * 4 + static_cast<std::size_t>(c)] = o;
                out.height.pixels[i * 4 + static_cast<std::size_t>(c)] = h;
            }
        }
    });
    // Normal map (OpenGL, +Y arriba) de la altura; la fuerza no depende de la resolucion.
    const float k = strength * static_cast<float>(n) / 1024.0f;
    parallelRows(n, [&](int y) {
        for (int x = 0; x < n; ++x) {
            const auto at = [&](int xx, int yy) {
                xx = wrap(xx, n);
                yy = wrap(yy, n);
                return height[static_cast<std::size_t>(yy) * static_cast<std::size_t>(n) + static_cast<std::size_t>(xx)];
            };
            const float dx = (at(x + 1, y) - at(x - 1, y)) * k;
            const float dy = (at(x, y + 1) - at(x, y - 1)) * k;
            Vec3 nrm = core::normalize(Vec3{-dx, dy, 1.0f});
            const std::size_t i = (static_cast<std::size_t>(y) * static_cast<std::size_t>(n) + static_cast<std::size_t>(x)) * 4;
            out.normal.pixels[i + 0] = byte(nrm.x * 0.5f + 0.5f);
            out.normal.pixels[i + 1] = byte(nrm.y * 0.5f + 0.5f);
            out.normal.pixels[i + 2] = byte(nrm.z * 0.5f + 0.5f);
        }
    });
    return true;
}

}  // namespace cramion::asset
