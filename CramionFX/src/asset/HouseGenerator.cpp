#include "CramionFX/asset/HouseGenerator.h"

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

constexpr float kPi = 3.14159265f;
const Vec3 kUp{0.0f, 1.0f, 0.0f};

float smoothstep(float a, float b, float x) {
    const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
float mix(float a, float b, float t) { return a + (b - a) * t; }
Vec3 mix(const Vec3& a, const Vec3& b, float t) { return a + (b - a) * t; }

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

// ---------------------------------------------------------------------------
// Geometria
// ---------------------------------------------------------------------------

struct Geo {
    std::array<std::vector<SkinnedVertex>, kHouseMaterialCount> vertices;
    std::array<std::vector<std::uint32_t>, kHouseMaterialCount> indices;

    std::size_t triangles() const {
        std::size_t n = 0;
        for (const auto& i : indices) n += i.size() / 3;
        return n;
    }

    Vec2 uvAt(int m, const Vec3& p, const Vec3& su, const Vec3& sv) const {
        const std::array<float, 2> meters = houseTextureMeters(m);
        return Vec2{core::dot(p, su) / meters[0], core::dot(p, sv) / meters[1]};
    }

    // Poligono convexo (abanico) con una normal por vertice; se orienta para
    // que la cara mire hacia `facing`.
    void polygon(int m, const std::vector<Vec3>& p, const std::vector<Vec3>& n, const std::vector<Vec2>& uv,
                 const Vec3& facing) {
        auto& verts = vertices[static_cast<std::size_t>(m)];
        auto& idx = indices[static_cast<std::size_t>(m)];
        const auto base = static_cast<std::uint32_t>(verts.size());
        for (std::size_t i = 0; i < p.size(); ++i) {
            SkinnedVertex v;
            v.position = p[i];
            v.normal = n[i];
            v.uv = uv[i];
            v.weights[0] = 1.0f;
            verts.push_back(v);
        }
        Vec3 area{};
        for (std::size_t i = 1; i + 1 < p.size(); ++i) area += core::cross(p[i] - p[0], p[i + 1] - p[0]);
        const bool flip = core::dot(area, facing) < 0.0f;
        for (std::uint32_t i = 1; i + 1 < p.size(); ++i) {
            if (flip) {
                idx.insert(idx.end(), {base, base + i + 1, base + i});
            } else {
                idx.insert(idx.end(), {base, base + i, base + i + 1});
            }
        }
    }

    // Cara plana con UV proyectadas en el mundo (su = eje de U, sv = eje de V).
    void flat(int m, const std::vector<Vec3>& p, const Vec3& normal, const Vec3& su, const Vec3& sv,
              Vec2 offset = {}) {
        std::vector<Vec3> n(p.size(), normal);
        std::vector<Vec2> uv;
        uv.reserve(p.size());
        for (const Vec3& q : p) {
            Vec2 t = uvAt(m, q, su, sv);
            uv.push_back(Vec2{t.x + offset.x, t.y + offset.y});
        }
        polygon(m, p, n, uv, normal);
    }

    // Caja orientada: centro, medias medidas y ejes (ortonormales). `rotate`
    // gira la veta 90 grados. `skip`: bits de caras que no se crean
    // (1 -X, 2 +X, 4 -Y, 8 +Y, 16 -Z, 32 +Z).
    void box(int m, const Vec3& c, const Vec3& half, const Vec3& ax, const Vec3& ay, const Vec3& az,
             bool rotate = false, int skip = 0, Vec2 offset = {}) {
        const Vec3 axes[3] = {ax, ay, az};
        const float h[3] = {half.x, half.y, half.z};
        for (int a = 0; a < 3; ++a) {
            for (int s = 0; s < 2; ++s) {
                if (skip & (1 << (a * 2 + s))) continue;
                const Vec3 n = axes[a] * (s == 0 ? -1.0f : 1.0f);
                const Vec3 e1 = axes[(a + 1) % 3] * h[(a + 1) % 3];
                const Vec3 e2 = axes[(a + 2) % 3] * h[(a + 2) % 3];
                const Vec3 fc = c + n * h[a];
                const std::vector<Vec3> p = {fc - e1 - e2, fc + e1 - e2, fc + e1 + e2, fc - e1 + e2};
                // Ejes de las UV: en las caras verticales, U horizontal y V hacia
                // abajo; en las horizontales, los ejes X/Z de la caja.
                Vec3 su;
                Vec3 sv;
                if (std::abs(n.y) > 0.7f) {
                    su = ax;
                    sv = az;
                } else {
                    Vec3 horizontal = core::normalize(core::cross(kUp, n));
                    if (!std::isfinite(horizontal.x)) horizontal = ax;
                    su = horizontal;
                    sv = core::normalize(core::cross(n, su)) * -1.0f;
                }
                if (rotate) std::swap(su, sv);
                flat(m, p, n, su, sv, offset);
            }
        }
    }
    void box(int m, const Vec3& c, const Vec3& half, bool rotate = false, int skip = 0, Vec2 offset = {}) {
        box(m, c, half, Vec3{1.0f, 0.0f, 0.0f}, kUp, Vec3{0.0f, 0.0f, 1.0f}, rotate, skip, offset);
    }
    // Caja entre dos esquinas (alineada con los ejes).
    void boxMinMax(int m, const Vec3& lo, const Vec3& hi, bool rotate = false, int skip = 0) {
        box(m, (lo + hi) * 0.5f, (hi - lo) * 0.5f, rotate, skip);
    }

    // Tronco: cilindro de `a` a `b` con testas (caps) opcionales.
    void log(const Vec3& a, const Vec3& b, float r, int sides, bool cap_a, bool cap_b, float u_offset,
             float twist) {
        const Vec3 d = core::normalize(b - a);
        Vec3 e1 = core::cross(d, kUp);
        if (core::length(e1) < 1e-3f) e1 = Vec3{1.0f, 0.0f, 0.0f};
        e1 = core::normalize(e1);
        const Vec3 e2 = core::cross(e1, d);
        const float len = core::length(b - a);
        const std::array<float, 2> meters = houseTextureMeters(kHouseLogs);
        const float u0 = core::dot(a, d) / meters[0] + u_offset;
        const float u1 = u0 + len / meters[0];
        for (int k = 0; k < sides; ++k) {
            const float a0 = twist + 2.0f * kPi * static_cast<float>(k) / static_cast<float>(sides);
            const float a1 = twist + 2.0f * kPi * static_cast<float>(k + 1) / static_cast<float>(sides);
            const Vec3 n0 = e1 * std::cos(a0) + e2 * std::sin(a0);
            const Vec3 n1 = e1 * std::cos(a1) + e2 * std::sin(a1);
            const float v0 = static_cast<float>(k) / static_cast<float>(sides);
            const float v1 = static_cast<float>(k + 1) / static_cast<float>(sides);
            polygon(kHouseLogs, {a + n0 * r, b + n0 * r, b + n1 * r, a + n1 * r}, {n0, n0, n1, n1},
                    {Vec2{u0, v0}, Vec2{u1, v0}, Vec2{u1, v1}, Vec2{u0, v1}}, (n0 + n1) * 0.5f);
        }
        for (int end = 0; end < 2; ++end) {
            if ((end == 0 && !cap_a) || (end == 1 && !cap_b)) continue;
            const Vec3 center = end == 0 ? a : b;
            const Vec3 n = end == 0 ? d * -1.0f : d;
            std::vector<Vec3> p;
            std::vector<Vec2> uv;
            const float spin = u_offset * 7.0f;
            for (int k = 0; k < sides; ++k) {
                const float ang = twist + 2.0f * kPi * static_cast<float>(k) / static_cast<float>(sides);
                p.push_back(center + (e1 * std::cos(ang) + e2 * std::sin(ang)) * r);
                uv.push_back(Vec2{0.5f + 0.48f * std::cos(ang + spin), 0.5f + 0.48f * std::sin(ang + spin)});
            }
            polygon(kHouseLogEnds, p, std::vector<Vec3>(p.size(), n), uv, n);
        }
    }

    void finish(ModelData& model, const std::string& name) const {
        model.name = name;
        model.materials.clear();
        for (int m = 0; m < kHouseMaterialCount; ++m) {
            MaterialData mat;
            mat.name = houseMaterialName(m);
            switch (m) {
                case kHouseLogs: mat.base_color = Vec4{0.45f, 0.31f, 0.18f, 1.0f}; mat.roughness = 0.85f; break;
                case kHouseLogEnds: mat.base_color = Vec4{0.55f, 0.42f, 0.27f, 1.0f}; mat.roughness = 0.85f; break;
                case kHousePlanks: mat.base_color = Vec4{0.42f, 0.36f, 0.3f, 1.0f}; mat.roughness = 0.85f; break;
                case kHouseSiding: mat.base_color = Vec4{0.8f, 0.79f, 0.75f, 1.0f}; mat.roughness = 0.6f; break;
                case kHouseStone: mat.base_color = Vec4{0.5f, 0.49f, 0.46f, 1.0f}; mat.roughness = 0.9f; break;
                case kHouseRoof: mat.base_color = Vec4{0.33f, 0.29f, 0.25f, 1.0f}; mat.roughness = 0.9f; break;
                case kHouseTrim: mat.base_color = Vec4{0.85f, 0.84f, 0.8f, 1.0f}; mat.roughness = 0.5f; break;
                case kHouseGlass:
                    mat.base_color = Vec4{0.02f, 0.025f, 0.03f, 1.0f};
                    mat.roughness = 0.04f;
                    mat.reflectance = 0.06f;
                    break;
                case kHouseIron: mat.base_color = Vec4{0.1f, 0.1f, 0.1f, 1.0f}; mat.metallic = 1.0f; mat.roughness = 0.6f; break;
                default: break;
            }
            model.materials.push_back(mat);
        }
        model.vertices.clear();
        model.indices.clear();
        model.submeshes.clear();
        for (int m = 0; m < kHouseMaterialCount; ++m) {
            const auto& verts = vertices[static_cast<std::size_t>(m)];
            const auto& idx = indices[static_cast<std::size_t>(m)];
            if (idx.empty()) continue;
            const auto base = static_cast<std::uint32_t>(model.vertices.size());
            const auto first = static_cast<std::uint32_t>(model.indices.size());
            // Tangentes (mismo convenio que el lector de OBJ: V hacia abajo).
            std::vector<Vec3> tangents(verts.size(), Vec3{});
            std::vector<Vec3> bitangents(verts.size(), Vec3{});
            for (std::size_t i = 0; i + 2 < idx.size(); i += 3) {
                const SkinnedVertex& va = verts[idx[i]];
                const SkinnedVertex& vb = verts[idx[i + 1]];
                const SkinnedVertex& vc = verts[idx[i + 2]];
                const Vec3 e1 = vb.position - va.position;
                const Vec3 e2 = vc.position - va.position;
                const float du1 = vb.uv.x - va.uv.x;
                const float dv1 = vb.uv.y - va.uv.y;
                const float du2 = vc.uv.x - va.uv.x;
                const float dv2 = vc.uv.y - va.uv.y;
                const float det = du1 * dv2 - du2 * dv1;
                if (std::abs(det) < 1e-12f) continue;
                const float r = 1.0f / det;
                const Vec3 t = (e1 * dv2 - e2 * dv1) * r;
                const Vec3 bt = (e2 * du1 - e1 * du2) * r;
                for (std::uint32_t k : {idx[i], idx[i + 1], idx[i + 2]}) {
                    tangents[k] += t;
                    bitangents[k] += bt;
                }
            }
            SubMesh sub;
            sub.first_index = first;
            sub.index_count = static_cast<std::uint32_t>(idx.size());
            sub.material = static_cast<std::uint32_t>(m);
            sub.bounds_min = Vec3{1e9f, 1e9f, 1e9f};
            sub.bounds_max = Vec3{-1e9f, -1e9f, -1e9f};
            for (std::size_t k = 0; k < verts.size(); ++k) {
                SkinnedVertex v = verts[k];
                Vec3 t = tangents[k] - v.normal * core::dot(v.normal, tangents[k]);
                if (core::length(t) < 1e-9f) {
                    v.tangent = Vec4{1.0f, 0.0f, 0.0f, 1.0f};
                } else {
                    t = core::normalize(t);
                    const float w = core::dot(core::cross(v.normal, t), bitangents[k]) < 0.0f ? -1.0f : 1.0f;
                    v.tangent = Vec4{t, w};
                }
                sub.bounds_min = Vec3{std::min(sub.bounds_min.x, v.position.x), std::min(sub.bounds_min.y, v.position.y),
                                      std::min(sub.bounds_min.z, v.position.z)};
                sub.bounds_max = Vec3{std::max(sub.bounds_max.x, v.position.x), std::max(sub.bounds_max.y, v.position.y),
                                      std::max(sub.bounds_max.z, v.position.z)};
                model.vertices.push_back(v);
            }
            for (std::uint32_t i : idx) model.indices.push_back(base + i);
            model.submeshes.push_back(sub);
        }
        model.nodes = {Node{name, -1, core::Mat4::identity()}};
        model.bones = {Bone{name, 0, core::Mat4::identity()}};
    }
};

// Hueco en una pared: s = a lo largo (desde el centro de la pared), y = alturas.
struct Opening {
    int wall = 0;
    float s0 = 0.0f;
    float s1 = 0.0f;
    float y0 = 0.0f;
    float y1 = 0.0f;
    bool door = false;
};

// Una pared: centro de su linea, eje a lo largo y normal hacia fuera.
struct Wall {
    Vec3 center{};
    Vec3 along{};
    Vec3 out{};
    float half = 0.0f;  // media longitud de la linea (entre ejes de esquina)
};

struct Rng {
    std::mt19937 engine;
    explicit Rng(std::uint32_t seed) : engine(seed * 2654435761U + 12345U) {}
    float range(float a, float b) { return std::uniform_real_distribution<float>(a, b)(engine); }
    bool chance(float p) { return range(0.0f, 1.0f) < p; }
};

class HouseBuilder {
public:
    explicit HouseBuilder(const HouseSettings& s) : s_(s), rng_(s.seed) {}

    HouseModel build() {
        layout();
        foundation();
        if (s_.style == HouseStyle::LogCabin) {
            logWalls();
        } else {
            flatWalls();
        }
        for (const Opening& o : openings_) {
            if (o.door) {
                doorFrame(o);
            } else {
                window(o);
            }
        }
        roof();
        if (s_.porch) porch();
        else frontSteps();
        if (s_.chimney) chimney();

        HouseModel out;
        geo_.finish(out.house, houseStyleName(s_.style));
        out.triangles = geo_.triangles();
        door_geo_.finish(out.door, "Puerta");
        out.triangles += door_geo_.triangles();
        out.door_hinge = door_hinge_;
        out.door_size = door_size_;
        out.bounds_min = Vec3{1e9f, 1e9f, 1e9f};
        out.bounds_max = Vec3{-1e9f, -1e9f, -1e9f};
        for (const SkinnedVertex& v : out.house.vertices) {
            out.bounds_min = Vec3{std::min(out.bounds_min.x, v.position.x), std::min(out.bounds_min.y, v.position.y),
                                  std::min(out.bounds_min.z, v.position.z)};
            out.bounds_max = Vec3{std::max(out.bounds_max.x, v.position.x), std::max(out.bounds_max.y, v.position.y),
                                  std::max(out.bounds_max.z, v.position.z)};
        }
        return out;
    }

private:
    HouseSettings s_;
    Rng rng_;
    Geo geo_;
    Geo door_geo_;
    std::array<Wall, 4> walls_{};  // 0 delante (+Z), 1 atras, 2 derecha (+X), 3 izquierda
    std::vector<Opening> openings_;
    float t_ = 0.2f;        // grosor de las paredes
    float plinth_ = 0.45f;  // altura del zocalo (el suelo de dentro)
    float top_ = 3.0f;      // altura de lo alto de las paredes
    float tan_ = 0.78f;     // pendiente del tejado
    float zo_ = 3.0f;       // media profundidad por fuera de las paredes
    float xo_ = 4.0f;       // medio ancho por fuera
    float ridge_ = 5.0f;    // cara de abajo del tejado en la cumbrera
    float log_r_ = 0.15f;
    float log_step_ = 0.26f;
    float log_ext_ = 0.35f;
    float door_s_ = 0.0f;
    Vec3 door_hinge_{};
    Vec3 door_size_{};

    bool logs() const { return s_.style == HouseStyle::LogCabin; }
    int wallMaterial() const {
        switch (s_.style) {
            case HouseStyle::StoneCottage: return kHouseStone;
            case HouseStyle::Farmhouse: return kHouseSiding;
            default: return kHousePlanks;
        }
    }
    Vec3 at(const Wall& w, float s, float y, float d) const { return w.center + w.along * s + w.out * d + kUp * y; }
    // Altura de la cara de abajo del tejado a una distancia |z| del centro.
    float roofUnder(float z) const { return top_ + (zo_ - std::abs(z)) * tan_; }

    void layout() {
        const float W = s_.width;
        const float D = s_.depth;
        tan_ = std::tan(std::clamp(s_.roof_pitch, 10.0f, 60.0f) * kPi / 180.0f);
        switch (s_.style) {
            case HouseStyle::LogCabin: t_ = 2.0f * log_r_; plinth_ = 0.5f; break;
            case HouseStyle::TimberCabin: t_ = 0.16f; plinth_ = 0.45f; break;
            case HouseStyle::StoneCottage: t_ = 0.5f; plinth_ = 0.3f; break;
            case HouseStyle::Farmhouse: t_ = 0.2f; plinth_ = 0.6f; break;
        }
        const int floors = std::clamp(s_.floors, 1, 2);
        const float wall_h = std::max(s_.wall_height, 2.2f) * static_cast<float>(floors);
        if (logs()) {
            log_step_ = 2.0f * log_r_ * 0.88f;
            const int courses = std::max(4, static_cast<int>(std::round(wall_h / log_step_)));
            top_ = plinth_ + log_r_ * 0.95f + static_cast<float>(courses - 1) * log_step_ + log_r_ * 0.9f;
        } else {
            top_ = plinth_ + wall_h;
        }
        zo_ = D * 0.5f + t_ * 0.5f;
        xo_ = W * 0.5f + t_ * 0.5f;
        ridge_ = top_ + zo_ * tan_;
        walls_[0] = Wall{Vec3{0.0f, 0.0f, D * 0.5f}, Vec3{1.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, 1.0f}, W * 0.5f};
        walls_[1] = Wall{Vec3{0.0f, 0.0f, -D * 0.5f}, Vec3{-1.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, -1.0f}, W * 0.5f};
        walls_[2] = Wall{Vec3{W * 0.5f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, -1.0f}, Vec3{1.0f, 0.0f, 0.0f}, D * 0.5f};
        walls_[3] = Wall{Vec3{-W * 0.5f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, 1.0f}, Vec3{-1.0f, 0.0f, 0.0f}, D * 0.5f};

        // --- Puertas y ventanas ---
        float win_w = 0.9f;
        float win_h = 1.15f;
        float sill = 0.95f;
        switch (s_.style) {
            case HouseStyle::LogCabin: win_w = 0.85f; win_h = 1.0f; sill = 0.9f; break;
            case HouseStyle::StoneCottage: win_w = 0.8f; win_h = 1.05f; sill = 0.95f; break;
            case HouseStyle::Farmhouse: win_w = 0.85f; win_h = 1.45f; sill = 0.8f; break;
            default: break;
        }
        const float door_w = 1.0f;
        const float door_h = 2.1f;
        const float storey = (top_ - plinth_) / static_cast<float>(floors);
        int slots = s_.windows > 0 ? s_.windows + 1 : std::max(2, static_cast<int>(std::round(W / 2.3f)));
        if (slots % 2 == 0 && s_.windows <= 0) slots += 1;
        const int door_slot = slots / 2;
        const float slot_w = W / static_cast<float>(slots);
        for (int floor = 0; floor < floors; ++floor) {
            const float base = plinth_ + storey * static_cast<float>(floor);
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
    }

    void foundation() {
        const float margin = logs() ? log_r_ + 0.05f : 0.05f;
        const float hx = s_.width * 0.5f + margin;
        const float hz = s_.depth * 0.5f + margin;
        const float top = plinth_;
        geo_.boxMinMax(kHouseStone, Vec3{-hx, -0.6f, -hz}, Vec3{hx, top, hz}, false, 4);
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

    // --- Paredes planas (tablas, piedra, tablas solapadas) ---
    void flatPanel(int m, const Wall& w, float s0, float s1, float y0, float y1, int wall) {
        const float h = t_ * 0.5f;
        std::vector<float> xs = {s0, s1};
        std::vector<float> ys = {y0, y1};
        std::vector<const Opening*> holes;
        for (const Opening& o : openings_) {
            if (o.wall != wall) continue;
            holes.push_back(&o);
            xs.push_back(o.s0);
            xs.push_back(o.s1);
            ys.push_back(std::clamp(o.y0, y0, y1));
            ys.push_back(std::clamp(o.y1, y0, y1));
        }
        std::sort(xs.begin(), xs.end());
        xs.erase(std::unique(xs.begin(), xs.end()), xs.end());
        std::sort(ys.begin(), ys.end());
        ys.erase(std::unique(ys.begin(), ys.end()), ys.end());
        const Vec3 sv = kUp * -1.0f;
        for (std::size_t i = 0; i + 1 < xs.size(); ++i) {
            for (std::size_t j = 0; j + 1 < ys.size(); ++j) {
                const float cx = (xs[i] + xs[i + 1]) * 0.5f;
                const float cy = (ys[j] + ys[j + 1]) * 0.5f;
                bool hole = false;
                for (const Opening* o : holes) hole = hole || (cx > o->s0 && cx < o->s1 && cy > o->y0 && cy < o->y1);
                if (hole) continue;
                for (int side = 0; side < 2; ++side) {
                    const float d = side == 0 ? h : -h;
                    const Vec3 n = w.out * (side == 0 ? 1.0f : -1.0f);
                    geo_.flat(m,
                              {at(w, xs[i], ys[j], d), at(w, xs[i + 1], ys[j], d), at(w, xs[i + 1], ys[j + 1], d),
                               at(w, xs[i], ys[j + 1], d)},
                              n, w.along, sv);
                }
            }
        }
        // Mochetas de los huecos (el grosor de la pared).
        for (const Opening* o : holes) {
            const float ya = std::max(o->y0, y0);
            const float yb = std::min(o->y1, y1);
            if (yb <= ya) continue;
            geo_.flat(m, {at(w, o->s0, ya, h), at(w, o->s0, ya, -h), at(w, o->s0, yb, -h), at(w, o->s0, yb, h)}, w.along,
                      w.out, sv);
            geo_.flat(m, {at(w, o->s1, ya, h), at(w, o->s1, ya, -h), at(w, o->s1, yb, -h), at(w, o->s1, yb, h)},
                      w.along * -1.0f, w.out, sv);
            if (o->y1 <= y1) {
                geo_.flat(m, {at(w, o->s0, o->y1, h), at(w, o->s1, o->y1, h), at(w, o->s1, o->y1, -h), at(w, o->s0, o->y1, -h)},
                          kUp * -1.0f, w.along, w.out);
            }
            if (o->y0 >= y0 && !o->door) {
                geo_.flat(m, {at(w, o->s0, o->y0, h), at(w, o->s1, o->y0, h), at(w, o->s1, o->y0, -h), at(w, o->s0, o->y0, -h)},
                          kUp, w.along, w.out);
            }
        }
        // Remate de arriba y cantos de los extremos.
        geo_.flat(m, {at(w, s0, y1, h), at(w, s1, y1, h), at(w, s1, y1, -h), at(w, s0, y1, -h)}, kUp, w.along, w.out);
        geo_.flat(m, {at(w, s0, y0, h), at(w, s0, y0, -h), at(w, s0, y1, -h), at(w, s0, y1, h)}, w.along * -1.0f, w.out, sv);
        geo_.flat(m, {at(w, s1, y0, h), at(w, s1, y0, -h), at(w, s1, y1, -h), at(w, s1, y1, h)}, w.along, w.out, sv);
    }

    void flatWalls() {
        const int m = wallMaterial();
        const float W = s_.width;
        const float D = s_.depth;
        // Delante y atras de esquina a esquina; los lados entre medias.
        for (int w = 0; w < 4; ++w) {
            const Wall& wall = walls_[static_cast<std::size_t>(w)];
            const float half = w < 2 ? W * 0.5f + t_ * 0.5f : D * 0.5f - t_ * 0.5f;
            flatPanel(m, wall, -half, half, plinth_, top_, w);
        }
        // Hastiales (triangulos bajo el tejado) en los lados.
        for (int w = 2; w < 4; ++w) {
            const Wall& wall = walls_[static_cast<std::size_t>(w)];
            for (int side = 0; side < 2; ++side) {
                const float d = side == 0 ? t_ * 0.5f : -t_ * 0.5f;
                const Vec3 n = wall.out * (side == 0 ? 1.0f : -1.0f);
                geo_.flat(m, {at(wall, -zo_, top_, d), at(wall, zo_, top_, d), at(wall, 0.0f, ridge_, d)}, n, wall.along,
                          kUp * -1.0f);
            }
        }
        // Esquineras (tablas y casa de campo) y la faja entre plantas.
        if (s_.style != HouseStyle::StoneCottage) {
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
            if (s_.floors >= 2) {
                const float y = plinth_ + (top_ - plinth_) * 0.5f;
                geo_.boxMinMax(cm, Vec3{-xo_ - 0.02f, y - 0.08f, zo_}, Vec3{xo_ + 0.02f, y + 0.08f, zo_ + 0.03f});
                geo_.boxMinMax(cm, Vec3{-xo_ - 0.02f, y - 0.08f, -zo_ - 0.03f}, Vec3{xo_ + 0.02f, y + 0.08f, -zo_});
                geo_.boxMinMax(cm, Vec3{xo_, y - 0.08f, -zo_}, Vec3{xo_ + 0.03f, y + 0.08f, zo_});
                geo_.boxMinMax(cm, Vec3{-xo_ - 0.03f, y - 0.08f, -zo_}, Vec3{-xo_, y + 0.08f, zo_});
                // Suelo de la planta de arriba.
                geo_.boxMinMax(kHousePlanks, Vec3{-xo_ + t_, y - 0.1f, -zo_ + t_}, Vec3{xo_ - t_, y + 0.05f, zo_ - t_}, true);
            }
        }
    }

    // Caja en coordenadas de una pared (s, y, d): de lo a hi.
    void wallBox(Geo& g, int m, const Wall& w, float s0, float s1, float y0, float y1, float d0, float d1,
                 bool rotate = false) {
        const Vec3 c = at(w, (s0 + s1) * 0.5f, (y0 + y1) * 0.5f, (d0 + d1) * 0.5f);
        g.box(m, c, Vec3{std::abs(s1 - s0) * 0.5f, std::abs(y1 - y0) * 0.5f, std::abs(d1 - d0) * 0.5f}, w.along, kUp,
              w.out, rotate);
    }

    float outerFace() const { return logs() ? log_r_ : t_ * 0.5f; }

    void window(const Opening& o) {
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
        wallBox(geo_, kHouseTrim, w, o.s0, o.s0 + fw, o.y0, o.y1, fd0, fd1);
        wallBox(geo_, kHouseTrim, w, o.s1 - fw, o.s1, o.y0, o.y1, fd0, fd1);
        wallBox(geo_, kHouseTrim, w, o.s0, o.s1, o.y1 - fw, o.y1, fd0, fd1);
        wallBox(geo_, kHousePlanks, w, o.s0, o.s1, o.y0, o.y0 + 0.035f, fd0, face + 0.04f, true);  // umbral
        if (s_.style == HouseStyle::StoneCottage) {
            wallBox(geo_, kHousePlanks, w, o.s0 - 0.2f, o.s1 + 0.2f, o.y1, o.y1 + 0.22f, -h + 0.01f, h + 0.01f, true);
        } else {
            const float cd0 = face - 0.01f;
            const float cd1 = face + 0.03f;
            wallBox(geo_, kHouseTrim, w, o.s0 - 0.1f, o.s0, o.y0, o.y1, cd0, cd1);
            wallBox(geo_, kHouseTrim, w, o.s1, o.s1 + 0.1f, o.y0, o.y1, cd0, cd1);
            wallBox(geo_, kHouseTrim, w, o.s0 - 0.13f, o.s1 + 0.13f, o.y1, o.y1 + 0.14f, cd0, cd1 + 0.01f);
        }
        // La hoja: pieza aparte con la bisagra en su origen (lado izquierdo).
        const float dw = (o.s1 - o.s0) - 2.0f * fw - 0.01f;
        const float dh = (o.y1 - o.y0) - fw - 0.04f;
        const float thick = 0.055f;
        const float hinge_d = h - thick * 0.5f - 0.02f;
        door_hinge_ = at(w, o.s0 + fw + 0.005f, o.y0 + 0.035f, hinge_d);
        door_size_ = Vec3{dw, dh, thick};
        const Vec3 X{1.0f, 0.0f, 0.0f};
        const Vec3 Z{0.0f, 0.0f, 1.0f};
        door_geo_.box(kHousePlanks, Vec3{dw * 0.5f, dh * 0.5f, 0.0f}, Vec3{dw * 0.5f, dh * 0.5f, thick * 0.5f}, X, kUp, Z);
        // Travesanos por fuera (puerta de tablas) y herrajes.
        for (float yy : {0.35f, dh - 0.35f}) {
            door_geo_.box(kHousePlanks, Vec3{dw * 0.5f, yy, thick * 0.5f + 0.012f}, Vec3{dw * 0.5f - 0.03f, 0.07f, 0.012f},
                          X, kUp, Z, true);
            door_geo_.box(kHouseIron, Vec3{dw * 0.3f, yy, thick * 0.5f + 0.027f}, Vec3{dw * 0.3f, 0.022f, 0.004f}, X, kUp, Z);
        }
        door_geo_.box(kHousePlanks, Vec3{dw * 0.5f, dh * 0.5f, thick * 0.5f + 0.012f}, Vec3{0.07f, dh * 0.5f - 0.42f, 0.012f},
                      X, kUp, Z);
        for (float side : {1.0f, -1.0f}) {
            const float z = side * (thick * 0.5f + 0.03f);
            door_geo_.box(kHouseIron, Vec3{dw - 0.1f, 1.0f, z}, Vec3{0.012f, 0.09f, 0.012f}, X, kUp, Z);
            door_geo_.box(kHouseIron, Vec3{dw - 0.1f, 1.0f, z * 0.8f}, Vec3{0.03f, 0.03f, 0.008f}, X, kUp, Z);
        }
    }

    // --- Tejado a dos aguas: tablillas, sofito de tablas, frentes y cumbrera ---
    void roof() {
        const float ov = std::max(s_.roof_overhang, 0.2f);
        const float ovg = logs() ? std::max(ov * 0.7f, 0.45f) + log_ext_ : ov * 0.7f;
        const float th = 0.16f;
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
            // Arriba: V baja por la pendiente (las hileras de tablillas en horizontal).
            const float slope = core::length(e0 - r0);
            const std::array<float, 2> rm = houseTextureMeters(kHouseRoof);
            const float vo = side > 0.0f ? 0.0f : 0.37f;
            geo_.polygon(kHouseRoof, {r0 + up, r1 + up, e1 + up, e0 + up}, {n, n, n, n},
                         {Vec2{x0 / rm[0], vo}, Vec2{x1 / rm[0], vo}, Vec2{x1 / rm[0], vo + slope / rm[1]},
                          Vec2{x0 / rm[0], vo + slope / rm[1]}},
                         n);
            // Abajo: tablas del sofito (a lo largo de la pendiente).
            geo_.flat(kHousePlanks, {r0, r1, e1, e0}, n * -1.0f, Vec3{1.0f, 0.0f, 0.0f}, down, Vec2{0.0f, 0.0f});
            // Frente del alero y cantos de los hastiales.
            const Vec3 fz{0.0f, 0.0f, side};
            geo_.flat(kHouseTrim, {e0, e1, e1 + up, e0 + up}, fz, Vec3{1.0f, 0.0f, 0.0f}, kUp * -1.0f);
            for (float xs : {-1.0f, 1.0f}) {
                const float x = xs < 0.0f ? x0 : x1;
                const Vec3 a{x, ridge_, 0.0f};
                const Vec3 b{x, ye, ze * side};
                // Tabla de canto (barge board): un poco mas ancha que el tejado.
                const Vec3 c = (a + b) * 0.5f + up * 0.5f - n * 0.05f + Vec3{xs * 0.02f, 0.0f, 0.0f};
                geo_.box(kHouseTrim, c, Vec3{0.025f, th * 0.5f + 0.06f, core::length(b - a) * 0.5f + 0.02f},
                         Vec3{1.0f, 0.0f, 0.0f}, n, down, true);
            }
        }
        // Cumbrera.
        const float top = ridge_ + th * std::sqrt(1.0f + tan_ * tan_);
        const float d = 0.13f;
        const Vec3 ax{1.0f, 0.0f, 0.0f};
        const Vec3 ay = core::normalize(Vec3{0.0f, 1.0f, 1.0f});
        const Vec3 az = core::normalize(Vec3{0.0f, -1.0f, 1.0f});
        geo_.box(kHouseRoof, Vec3{0.0f, top - d * 0.35f, 0.0f}, Vec3{(x1 - x0) * 0.5f + 0.03f, d, d}, ax, ay, az, false, 0);
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
        geo_.boxMinMax(skirt, Vec3{-hx + 0.05f, -0.4f, z1 - 0.12f}, Vec3{hx - 0.05f, deck - 0.08f, z1 - 0.06f}, false, 4);
        geo_.boxMinMax(skirt, Vec3{-hx + 0.05f, -0.4f, z0}, Vec3{-hx + 0.11f, deck - 0.08f, z1 - 0.06f}, false, 4);
        geo_.boxMinMax(skirt, Vec3{hx - 0.11f, -0.4f, z0}, Vec3{hx - 0.05f, deck - 0.08f, z1 - 0.06f}, false, 4);
        // Tejadillo: pendiente suave; empieza en la pared o sobre el tejado.
        const float t12 = std::tan(14.0f * kPi / 180.0f);
        const float zp = z1 - 0.15f;
        const float beam_bottom = deck + 2.25f;
        const float beam_h = 0.18f;
        const float under_at_post = beam_bottom + beam_h;
        const auto under = [&](float z) { return under_at_post + (zp - z) * t12; };
        const float th = 0.12f;
        const float sec12 = std::sqrt(1.0f + t12 * t12);
        const float main_top_ridge = ridge_ + 0.16f * std::sqrt(1.0f + tan_ * tan_);
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
        geo_.flat(kHousePlanks, {a0, a1, b1, b0}, n * -1.0f, Vec3{1.0f, 0.0f, 0.0f}, down);
        geo_.flat(kHouseTrim, {b0, b1, b1 + up, b0 + up}, Vec3{0.0f, 0.0f, 1.0f}, Vec3{1.0f, 0.0f, 0.0f}, kUp * -1.0f);
        for (float x : {x0, x1}) {
            const float xs = x < 0.0f ? -1.0f : 1.0f;
            geo_.flat(kHouseTrim, {a0 + Vec3{x - x0, 0.0f, 0.0f}, b0 + Vec3{x - x0, 0.0f, 0.0f}, b0 + up + Vec3{x - x0, 0.0f, 0.0f},
                                   a0 + up + Vec3{x - x0, 0.0f, 0.0f}},
                      Vec3{xs, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, 1.0f}, kUp * -1.0f);
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
        const int m = s_.style == HouseStyle::StoneCottage || s_.style == HouseStyle::Farmhouse ? kHouseStone : kHousePlanks;
        for (int i = 0; i < count; ++i) {
            const float y1 = top.y - rise * static_cast<float>(i + 1);
            const float z0 = top.z + 0.3f * static_cast<float>(i);
            geo_.boxMinMax(m, Vec3{top.x - width * 0.5f, -0.3f, z0 - 0.02f}, Vec3{top.x + width * 0.5f, y1, z0 + 0.3f}, true, 4);
        }
    }

    void frontSteps() {
        const float z = logs() ? zo_ + 0.05f : zo_;
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
        const float stack_top = ridge_ + 0.16f * std::sqrt(1.0f + tan_ * tan_) + 0.9f;
        const float xc = xb + base_d * 0.5f;
        geo_.boxMinMax(kHouseStone, Vec3{xb, -0.4f, -base_w * 0.5f}, Vec3{xb + base_d, shoulder, base_w * 0.5f}, false, 4);
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
                  kUp, Vec3{1.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, 1.0f});
        std::vector<Vec3> ring = {
            Vec3{xc - stack_d * 0.5f - 0.06f, stack_top + 0.1f, -stack_w * 0.5f - 0.06f},
            Vec3{xc + stack_d * 0.5f + 0.06f, stack_top + 0.1f, -stack_w * 0.5f - 0.06f},
            Vec3{xc + stack_d * 0.5f + 0.06f, stack_top + 0.1f, stack_w * 0.5f + 0.06f},
            Vec3{xc - stack_d * 0.5f - 0.06f, stack_top + 0.1f, stack_w * 0.5f + 0.06f},
        };
        // Borde de arriba alrededor de la boca (cuatro franjas).
        const Vec3 X{1.0f, 0.0f, 0.0f};
        const Vec3 Z{0.0f, 0.0f, 1.0f};
        const float y = stack_top + 0.1f;
        geo_.flat(kHouseStone, {ring[0], ring[1], Vec3{ring[1].x, y, -ri * 1.3f}, Vec3{ring[0].x, y, -ri * 1.3f}}, kUp, X, Z);
        geo_.flat(kHouseStone, {Vec3{ring[3].x, y, ri * 1.3f}, Vec3{ring[2].x, y, ri * 1.3f}, ring[2], ring[3]}, kUp, X, Z);
        geo_.flat(kHouseStone, {Vec3{ring[0].x, y, -ri * 1.3f}, Vec3{xc - ri, y, -ri * 1.3f}, Vec3{xc - ri, y, ri * 1.3f},
                                Vec3{ring[0].x, y, ri * 1.3f}},
                  kUp, X, Z);
        geo_.flat(kHouseStone, {Vec3{xc + ri, y, -ri * 1.3f}, Vec3{ring[1].x, y, -ri * 1.3f}, Vec3{ring[1].x, y, ri * 1.3f},
                                Vec3{xc + ri, y, ri * 1.3f}},
                  kUp, X, Z);
    }
};

}  // namespace

const char* houseStyleName(HouseStyle style) {
    switch (style) {
        case HouseStyle::LogCabin: return "Cabana de troncos";
        case HouseStyle::TimberCabin: return "Cabana de tablas";
        case HouseStyle::StoneCottage: return "Casita de piedra";
        case HouseStyle::Farmhouse: return "Casa de campo";
    }
    return "Casa";
}

const char* houseMaterialName(int material) {
    static const char* const kNames[kHouseMaterialCount] = {"Troncos", "Veta",    "Tablas", "Revestimiento", "Piedra",
                                                             "Tejado",  "Marcos", "Vidrio", "Hierro"};
    return material >= 0 && material < kHouseMaterialCount ? kNames[material] : "";
}

const char* houseTextureName(int material) {
    static const char* const kNames[kHouseMaterialCount] = {"Troncos", "Veta", "Tablas", "Revestimiento", "Piedra",
                                                             "Tejado",  "Marcos", "", ""};
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
        default: return {1.0f, 1.0f};
    }
}

HouseSettings housePreset(HouseStyle style, std::uint32_t seed) {
    HouseSettings s;
    s.style = style;
    s.seed = seed;
    Rng rng(seed * 31U + static_cast<std::uint32_t>(style));
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
