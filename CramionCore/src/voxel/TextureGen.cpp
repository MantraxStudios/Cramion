// Texturas PBR de los bloques, generadas por codigo (sin archivos): cada una
// es un campo de altura + color + rugosidad (+ metal, emision, recorte) de
// ruido periodico, asi que se repiten sin costuras de un bloque al vecino.
// De la altura salen el normal map (con la convencion de OpenGL: verde hacia
// arriba), el relieve del parallax y la oclusion de las grietas.
//
// Si en la carpeta de texturas hay un PNG con el nombre (<nombre>.png,
// <nombre>_n.png, <nombre>_m.png), se usa ese en su lugar: se pueden cambiar
// por fotos o por un pack de texturas.

#include "CramionCore/voxel/Voxel.h"

#include <CramionFX/asset/ImageFile.h>
#include <CramionFX/vk/VoxelPass.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iostream>

namespace cramion::voxel {

namespace {

constexpr float kTau = 6.28318530718f;

// --- Ruido periodico ---------------------------------------------------------------

std::uint32_t hash2(int x, int y, std::uint32_t seed) {
    std::uint32_t h = static_cast<std::uint32_t>(x) * 374761393u + static_cast<std::uint32_t>(y) * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}
float rand01(int x, int y, std::uint32_t seed) { return static_cast<float>(hash2(x, y, seed) & 0xFFFFFF) / 16777215.0f; }

int wrap(int v, int period) { return ((v % period) + period) % period; }

// Ruido de gradiente 2D que se repite cada `period` unidades.
float gradient(float x, float y, int period, std::uint32_t seed) {
    const int x0 = static_cast<int>(std::floor(x));
    const int y0 = static_cast<int>(std::floor(y));
    const float fx = x - static_cast<float>(x0);
    const float fy = y - static_cast<float>(y0);
    const auto dot = [&](int ix, int iy, float dx, float dy) {
        const float a = rand01(wrap(ix, period), wrap(iy, period), seed) * kTau;
        return std::cos(a) * dx + std::sin(a) * dy;
    };
    const float u = fx * fx * fx * (fx * (fx * 6.0f - 15.0f) + 10.0f);
    const float v = fy * fy * fy * (fy * (fy * 6.0f - 15.0f) + 10.0f);
    const float n00 = dot(x0, y0, fx, fy);
    const float n10 = dot(x0 + 1, y0, fx - 1.0f, fy);
    const float n01 = dot(x0, y0 + 1, fx, fy - 1.0f);
    const float n11 = dot(x0 + 1, y0 + 1, fx - 1.0f, fy - 1.0f);
    const float a = n00 + (n10 - n00) * u;
    const float b = n01 + (n11 - n01) * u;
    return (a + (b - a) * v) * 1.41f;  // ~-1..1
}

// fbm periodico en (u, v) de 0..1: `base` celdas por lado en la primera octava.
float fbm(float u, float v, int base, int octaves, std::uint32_t seed, float gain = 0.5f) {
    float sum = 0.0f;
    float amp = 1.0f;
    float norm = 0.0f;
    int period = base;
    for (int i = 0; i < octaves; ++i) {
        sum += gradient(u * static_cast<float>(period), v * static_cast<float>(period), period, seed + static_cast<std::uint32_t>(i) * 101u) * amp;
        norm += amp;
        amp *= gain;
        period *= 2;
    }
    return sum / norm;  // -1..1
}
float fbm01(float u, float v, int base, int octaves, std::uint32_t seed, float gain = 0.5f) {
    return std::clamp(fbm(u, v, base, octaves, seed, gain) * 0.5f + 0.5f, 0.0f, 1.0f);
}

// Voronoi periodico: F1, F2 (distancias) y el id de la celda mas cercana.
struct Cell {
    float f1 = 9.0f;
    float f2 = 9.0f;
    std::uint32_t id = 0;
    float cx = 0.0f, cy = 0.0f;  // centro (en celdas)
};
Cell voronoi(float u, float v, int cells, std::uint32_t seed, float jitter = 0.9f) {
    const float x = u * static_cast<float>(cells);
    const float y = v * static_cast<float>(cells);
    const int ix = static_cast<int>(std::floor(x));
    const int iy = static_cast<int>(std::floor(y));
    Cell c;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            const int gx = ix + dx, gy = iy + dy;
            const int wx = wrap(gx, cells), wy = wrap(gy, cells);
            const float px = static_cast<float>(gx) + 0.5f + (rand01(wx, wy, seed) - 0.5f) * jitter;
            const float py = static_cast<float>(gy) + 0.5f + (rand01(wx, wy, seed + 7u) - 0.5f) * jitter;
            const float d = std::hypot(px - x, py - y);
            if (d < c.f1) {
                c.f2 = c.f1;
                c.f1 = d;
                c.id = hash2(wx, wy, seed + 31u);
                c.cx = px;
                c.cy = py;
            } else if (d < c.f2) {
                c.f2 = d;
            }
        }
    }
    return c;
}
float idRand(std::uint32_t id, std::uint32_t salt) { return static_cast<float>(hash2(static_cast<int>(id), static_cast<int>(salt), 99u) & 0xFFFF) / 65535.0f; }

float smooth(float a, float b, float x) {
    const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

struct Rgb {
    float r, g, b;
};
Rgb operator*(Rgb a, float s) { return {a.r * s, a.g * s, a.b * s}; }
Rgb operator+(Rgb a, Rgb b) { return {a.r + b.r, a.g + b.g, a.b + b.b}; }
Rgb mix(Rgb a, Rgb b, float t) { return a * (1.0f - t) + b * t; }

// --- Una textura en construccion ------------------------------------------------------

struct Texel {
    Rgb color{0.5f, 0.5f, 0.5f};  // sRGB 0..1
    float alpha = 1.0f;
    float height = 0.5f;
    float roughness = 0.85f;
    float metal = 0.0f;
    float emission = 0.0f;
    float reflectance = 0.5f;  // 0.5 -> F0 0.04
};

using Recipe = std::function<Texel(float u, float v)>;

struct Style {
    float normal_strength = 3.0f;
    float cavity = 0.8f;  // oscurece las grietas (oclusion)
};

// --- Recetas ----------------------------------------------------------------------------

Texel stoneTexel(float u, float v, std::uint32_t seed, Rgb tone) {
    const float large = fbm(u, v, 3, 5, seed);
    const float fine = fbm(u, v, 16, 3, seed + 3u);
    const Cell cracks = voronoi(u, v, 5, seed + 11u, 1.0f);
    const float crack = 1.0f - smooth(0.0f, 0.045f, cracks.f2 - cracks.f1);
    Texel t;
    t.height = 0.55f + large * 0.25f + fine * 0.08f - crack * 0.35f;
    const float shade = 0.9f + large * 0.12f + fine * 0.06f - crack * 0.25f;
    t.color = mix(tone, tone * 1.12f + Rgb{0.02f, 0.01f, 0.0f}, fbm01(u, v, 2, 3, seed + 5u)) * shade;
    t.roughness = 0.78f + fine * 0.08f;
    return t;
}

Texel dirtTexel(float u, float v, std::uint32_t seed) {
    const float n = fbm(u, v, 4, 5, seed);
    const Cell pebbles = voronoi(u, v, 12, seed + 3u);
    const float pebble = smooth(0.32f, 0.18f, pebbles.f1) * (idRand(pebbles.id, 1) > 0.55f ? 1.0f : 0.0f);
    Texel t;
    t.height = 0.45f + n * 0.2f + pebble * 0.25f;
    const Rgb base{0.42f, 0.30f, 0.19f};
    t.color = base * (0.85f + n * 0.25f);
    t.color = mix(t.color, Rgb{0.50f, 0.44f, 0.38f} * (0.8f + idRand(pebbles.id, 2) * 0.4f), pebble * 0.8f);
    t.roughness = 0.93f;
    return t;
}

// Briznas de hierba vistas desde arriba (color verde neutro: el bioma lo tine).
Texel grassTopTexel(float u, float v, std::uint32_t seed) {
    const float n = fbm(u, v, 4, 4, seed);
    const float blades = fbm(u, v * 0.25f, 32, 2, seed + 9u) * 0.5f + fbm(u * 0.3f, v, 24, 2, seed + 13u) * 0.5f;
    Texel t;
    t.height = 0.5f + blades * 0.35f + n * 0.1f;
    const Rgb base{0.33f, 0.52f, 0.20f};
    t.color = base * (0.78f + blades * 0.35f + n * 0.15f);
    t.color = mix(t.color, Rgb{0.46f, 0.55f, 0.24f}, smooth(0.3f, 0.8f, fbm01(u, v, 3, 3, seed + 4u)) * 0.35f);
    t.roughness = 0.82f;
    return t;
}

Texel logBarkTexel(float u, float v, std::uint32_t seed, Rgb tone, float ridges) {
    const float warp = fbm(u, v, 2, 3, seed) * 0.08f;
    const float stripes = fbm(u + warp, v * 0.18f, static_cast<int>(ridges), 3, seed + 3u);
    const float fine = fbm(u, v, 12, 3, seed + 7u);
    const float groove = smooth(0.15f, -0.35f, stripes);
    Texel t;
    t.height = 0.6f + stripes * 0.3f + fine * 0.05f;
    t.color = tone * (0.82f + stripes * 0.2f + fine * 0.08f) * (1.0f - groove * 0.45f);
    t.roughness = 0.9f;
    return t;
}

Texel logTopTexel(float u, float v, std::uint32_t seed, Rgb wood, Rgb bark) {
    const float dx = u - 0.5f, dy = v - 0.5f;
    const float r = std::sqrt(dx * dx + dy * dy) + fbm(u, v, 3, 3, seed) * 0.03f;
    const float rings = 0.5f + 0.5f * std::sin(r * 70.0f);
    const float edge = smooth(0.40f, 0.46f, std::max(std::abs(dx), std::abs(dy)));
    Texel t;
    t.height = 0.5f + rings * 0.1f - edge * 0.05f;
    t.color = mix(wood * (0.85f + rings * 0.2f), bark * (0.9f + fbm(u, v, 10, 2, seed + 5u) * 0.2f), edge);
    t.roughness = 0.75f + edge * 0.15f;
    return t;
}

Texel planksTexel(float u, float v, std::uint32_t seed, Rgb tone) {
    const int plank = static_cast<int>(std::floor(v * 4.0f));
    const float local_v = v * 4.0f - static_cast<float>(plank);
    const float offset = rand01(plank, 0, seed) * 7.0f;
    const float grain = fbm(u * 0.25f + offset, v * 4.0f, 4, 4, seed + static_cast<std::uint32_t>(plank));
    const float lines = std::sin((v * 4.0f + grain * 0.6f) * 26.0f) * 0.5f + 0.5f;
    const float gap = smooth(0.06f, 0.0f, local_v) + smooth(0.94f, 1.0f, local_v);
    // Junta vertical en un sitio distinto de cada tabla.
    const float joint_u = rand01(plank, 3, seed);
    const float joint = smooth(0.012f, 0.0f, std::abs(std::fmod(u - joint_u + 1.0f, 1.0f) - 0.0f)) +
                        smooth(0.012f, 0.0f, std::abs(std::fmod(u - joint_u + 1.0f, 1.0f) - 1.0f));
    const float tone_var = 0.9f + rand01(plank, 1, seed) * 0.2f;
    Texel t;
    t.height = 0.6f + lines * 0.05f - (gap + joint) * 0.45f;
    t.color = tone * tone_var * (0.88f + lines * 0.1f + grain * 0.08f) * (1.0f - std::min(gap + joint, 1.0f) * 0.55f);
    t.roughness = 0.62f + grain * 0.08f;
    return t;
}

Texel sandTexel(float u, float v, std::uint32_t seed, Rgb tone) {
    const float grain = fbm(u, v, 48, 2, seed);
    const float ripples = std::sin((v + fbm(u, v, 2, 3, seed + 3u) * 0.15f) * kTau * 5.0f) * 0.5f + 0.5f;
    Texel t;
    t.height = 0.5f + grain * 0.12f + ripples * 0.12f;
    t.color = tone * (0.92f + grain * 0.1f + ripples * 0.04f);
    t.roughness = 0.94f;
    return t;
}

Texel pebblesTexel(float u, float v, std::uint32_t seed, int cells) {
    const Cell c = voronoi(u, v, cells, seed, 1.0f);
    const float stone = smooth(0.55f, 0.15f, c.f1);
    const float gap = 1.0f - smooth(0.0f, 0.08f, c.f2 - c.f1);
    const float tone = idRand(c.id, 1);
    const Rgb gray{0.55f, 0.53f, 0.50f};
    const Rgb brown{0.52f, 0.44f, 0.36f};
    Texel t;
    t.height = 0.3f + stone * 0.6f - gap * 0.2f;
    t.color = mix(gray, brown, idRand(c.id, 2)) * (0.65f + tone * 0.45f) * (0.75f + stone * 0.3f) * (1.0f - gap * 0.4f);
    t.roughness = 0.86f;
    return t;
}

Texel cobbleTexel(float u, float v, std::uint32_t seed) {
    const Cell c = voronoi(u, v, 4, seed, 0.85f);
    const float mortar = 1.0f - smooth(0.02f, 0.12f, c.f2 - c.f1);
    const float dome = smooth(0.75f, 0.0f, c.f1);
    const float surface = fbm(u, v, 12, 3, seed + 4u);
    Texel t;
    t.height = 0.35f + dome * 0.5f + surface * 0.06f - mortar * 0.35f;
    const float shade = 0.72f + idRand(c.id, 1) * 0.35f;
    t.color = Rgb{0.50f, 0.50f, 0.49f} * shade * (0.9f + surface * 0.12f);
    t.color = mix(t.color, Rgb{0.30f, 0.29f, 0.28f}, mortar * 0.85f);
    t.roughness = 0.85f;
    return t;
}

Texel oreTexel(float u, float v, std::uint32_t seed, Rgb mineral, float metal, float rough, float glow) {
    Texel t = stoneTexel(u, v, seed, Rgb{0.47f, 0.47f, 0.47f});
    const Cell c = voronoi(u, v, 5, seed + 101u, 1.0f);
    const bool vein = idRand(c.id, 3) > 0.62f;
    const float blob = vein ? smooth(0.42f, 0.2f, c.f1 + fbm(u, v, 8, 2, seed + 7u) * 0.12f) : 0.0f;
    const float sparkle = fbm(u, v, 24, 2, seed + 9u);
    t.color = mix(t.color, mineral * (0.8f + sparkle * 0.3f), blob);
    t.height += blob * 0.18f;
    t.metal = metal * blob;
    t.roughness = t.roughness * (1.0f - blob) + rough * blob;
    t.emission = glow * blob;
    t.reflectance = 0.5f + blob * 0.4f;
    return t;
}

Texel bricksTexel(float u, float v, std::uint32_t seed, int rows, Rgb tone, Rgb mortar_color) {
    const int row = static_cast<int>(std::floor(v * static_cast<float>(rows)));
    const float lv = v * static_cast<float>(rows) - static_cast<float>(row);
    const float shift = (row % 2) * 0.25f;
    const float bu = std::fmod(u * 2.0f + shift + 2.0f, 1.0f);
    const int brick = static_cast<int>(std::floor(u * 2.0f + shift + 2.0f));
    const float m = 0.07f;
    const float mortar = std::max(smooth(m, m * 0.4f, std::min(lv, 1.0f - lv)), smooth(m * 0.5f, m * 0.2f, std::min(bu, 1.0f - bu)));
    const float surface = fbm(u, v, 16, 3, seed);
    const float var = rand01(brick, row, seed);
    Texel t;
    t.height = 0.65f + surface * 0.08f - mortar * 0.45f;
    t.color = mix(tone * (0.8f + var * 0.35f) * (0.92f + surface * 0.12f), mortar_color * (0.9f + surface * 0.1f), mortar);
    t.roughness = 0.8f + mortar * 0.1f;
    return t;
}

// Planta: fondo transparente y briznas/tallos dibujados.
Texel tallGrassTexel(float u, float v, std::uint32_t seed, Rgb tone) {
    Texel t;
    t.alpha = 0.0f;
    t.height = 1.0f;
    t.roughness = 0.75f;
    t.color = tone;
    for (int i = 0; i < 26; ++i) {
        const float base = rand01(i, 0, seed) * 0.9f + 0.05f;
        const float top = 0.08f + rand01(i, 1, seed) * 0.55f;  // v de la punta (0 = arriba)
        const float lean = (rand01(i, 2, seed) - 0.5f) * 0.35f;
        if (v < top) continue;
        const float k = (1.0f - v) / (1.0f - top);  // 0 abajo, 1 en la punta
        const float x = base + lean * k * k;
        const float width = 0.022f * (1.0f - k) + 0.003f;
        if (std::abs(u - x) < width) {
            t.alpha = 1.0f;
            t.color = tone * (0.55f + 0.55f * k) * (0.9f + rand01(i, 3, seed) * 0.2f);
        }
    }
    return t;
}

Texel flowerTexel(float u, float v, std::uint32_t seed, Rgb petal, Rgb center) {
    Texel t;
    t.alpha = 0.0f;
    t.height = 1.0f;
    t.roughness = 0.6f;
    // Tallo con dos hojas.
    const float stem_x = 0.5f + std::sin(v * 5.0f) * 0.015f;
    if (v > 0.35f && std::abs(u - stem_x) < 0.018f) {
        t.alpha = 1.0f;
        t.color = Rgb{0.22f, 0.45f, 0.16f};
    }
    for (int side = -1; side <= 1; side += 2) {
        const float lx = u - (0.5f + side * 0.09f);
        const float ly = v - 0.68f;
        if ((lx * lx) / 0.008f + (ly * ly) / 0.0015f < 1.0f) {
            t.alpha = 1.0f;
            t.color = Rgb{0.26f, 0.52f, 0.18f};
        }
    }
    // Flor: petalos alrededor del centro.
    const float fx = u - 0.5f, fy = v - 0.24f;
    const float r = std::sqrt(fx * fx + fy * fy);
    const float angle = std::atan2(fy, fx);
    const float petals = 0.13f + 0.035f * std::cos(angle * 6.0f + static_cast<float>(seed % 7));
    if (r < petals) {
        t.alpha = 1.0f;
        t.color = petal * (0.8f + 0.3f * (1.0f - r / petals));
        t.roughness = 0.5f;
    }
    if (r < 0.04f) t.color = center;
    return t;
}

Texel leavesTexel(float u, float v, std::uint32_t seed, Rgb tone, float holes, int cells) {
    const Cell c = voronoi(u, v, cells, seed, 1.0f);
    // Cada celda es una hoja (elipse girada); entre hojas, huecos.
    const float a = idRand(c.id, 1) * kTau;
    const float lx = u * static_cast<float>(cells) - c.cx;
    const float ly = v * static_cast<float>(cells) - c.cy;
    const float rx = lx * std::cos(a) + ly * std::sin(a);
    const float ry = -lx * std::sin(a) + ly * std::cos(a);
    const float leaf = (rx * rx) / 0.30f + (ry * ry) / 0.10f;
    const float n = fbm(u, v, 6, 3, seed + 5u);
    Texel t;
    t.alpha = (leaf < 1.0f || n > holes) ? 1.0f : 0.0f;
    t.height = 0.5f + (1.0f - std::min(leaf, 1.0f)) * 0.4f;
    t.color = tone * (0.7f + idRand(c.id, 2) * 0.45f) * (0.85f + (1.0f - std::min(leaf, 1.0f)) * 0.25f);
    t.roughness = 0.6f;
    return t;
}

Texel needlesTexel(float u, float v, std::uint32_t seed, Rgb tone) {
    const float n = fbm(u, v, 8, 3, seed);
    const float needles = std::abs(std::sin((u + v * 0.6f + n * 0.2f) * kTau * 18.0f)) *
                          std::abs(std::sin((u - v * 0.6f - n * 0.2f) * kTau * 18.0f));
    Texel t;
    t.alpha = (needles > 0.08f || n > 0.25f) ? 1.0f : 0.0f;
    t.height = 0.5f + needles * 0.3f;
    t.color = tone * (0.75f + needles * 0.35f + n * 0.1f);
    t.roughness = 0.7f;
    return t;
}

Texel glassTexel(float u, float v, std::uint32_t seed) {
    const float border = std::min(std::min(u, 1.0f - u), std::min(v, 1.0f - v));
    const float streak = std::abs((u - v) - 0.35f) < 0.018f || std::abs((u - v) + 0.15f) < 0.01f ? 1.0f : 0.0f;
    Texel t;
    const bool frame = border < 0.055f;
    t.alpha = (frame || (streak > 0.5f && border > 0.1f)) ? 1.0f : 0.0f;
    t.color = frame ? Rgb{0.78f, 0.84f, 0.86f} * (0.9f + fbm(u, v, 8, 2, seed) * 0.1f) : Rgb{0.9f, 0.96f, 1.0f};
    t.height = frame ? 0.8f : 0.5f;
    t.roughness = 0.08f;
    t.reflectance = 1.0f;
    return t;
}

Texel torchTexel(float u, float v, std::uint32_t seed) {
    Texel t;
    t.alpha = 0.0f;
    t.height = 1.0f;
    const float x = std::abs(u - 0.5f);
    if (v > 0.38f && x < 0.045f) {  // palo
        t.alpha = 1.0f;
        t.color = Rgb{0.42f, 0.29f, 0.16f} * (0.85f + fbm(u, v, 16, 2, seed) * 0.2f);
        t.roughness = 0.8f;
    }
    const float fy = (v - 0.3f) / 0.13f;
    if (fy > -1.0f && fy < 1.0f && x < 0.075f * (1.0f - fy * fy * 0.6f)) {  // llama
        t.alpha = 1.0f;
        const float core = 1.0f - x / 0.075f;
        t.color = mix(Rgb{1.0f, 0.55f, 0.12f}, Rgb{1.0f, 0.95f, 0.65f}, core);
        t.emission = 1.0f;
        t.roughness = 1.0f;
    }
    return t;
}

Texel deadBushTexel(float u, float v, std::uint32_t seed) {
    Texel t;
    t.alpha = 0.0f;
    t.height = 1.0f;
    t.color = Rgb{0.45f, 0.32f, 0.18f};
    for (int i = 0; i < 7; ++i) {
        const float angle = (rand01(i, 0, seed) - 0.5f) * 1.8f;
        const float length = 0.35f + rand01(i, 1, seed) * 0.45f;
        // Rama recta desde abajo (0.5, 1).
        const float dx = u - 0.5f, dy = 1.0f - v;
        const float along = dx * std::sin(angle) + dy * std::cos(angle);
        const float across = dx * std::cos(angle) - dy * std::sin(angle);
        if (along > 0.0f && along < length && std::abs(across) < 0.012f + 0.01f * (1.0f - along / length)) {
            t.alpha = 1.0f;
            t.color = Rgb{0.42f, 0.30f, 0.17f} * (0.8f + rand01(i, 2, seed) * 0.3f);
        }
    }
    t.roughness = 0.85f;
    return t;
}

Texel glowstoneTexel(float u, float v, std::uint32_t seed) {
    const Cell c = voronoi(u, v, 6, seed, 1.0f);
    const float core = smooth(0.45f, 0.05f, c.f1);
    const float n = fbm(u, v, 8, 3, seed + 2u);
    Texel t;
    t.height = 0.4f + core * 0.4f + n * 0.1f;
    t.color = mix(Rgb{0.62f, 0.44f, 0.20f}, Rgb{1.0f, 0.90f, 0.55f}, core * (0.6f + idRand(c.id, 1) * 0.4f));
    t.emission = 0.35f + core * 0.65f;
    t.roughness = 0.5f;
    return t;
}

Texel cactusSideTexel(float u, float v, std::uint32_t seed) {
    const float ribs = std::cos(u * kTau * 4.0f) * 0.5f + 0.5f;
    const float n = fbm(u, v, 8, 3, seed);
    Texel t;
    t.height = 0.3f + ribs * 0.5f + n * 0.05f;
    t.color = Rgb{0.28f, 0.52f, 0.22f} * (0.7f + ribs * 0.35f + n * 0.1f);
    // Espinas: puntos claros en las costillas.
    const Cell c = voronoi(u, v, 8, seed + 3u);
    if (c.f1 < 0.06f && ribs > 0.6f) t.color = Rgb{0.9f, 0.88f, 0.7f};
    t.roughness = 0.55f;
    return t;
}

Texel craftingTopTexel(float u, float v, std::uint32_t seed) {
    Texel t = planksTexel(u, v, seed, Rgb{0.60f, 0.44f, 0.27f});
    const float border = std::min(std::min(u, 1.0f - u), std::min(v, 1.0f - v));
    const float grid = std::min(std::abs(std::fmod(u * 3.0f + 3.0f, 1.0f) - 0.5f), std::abs(std::fmod(v * 3.0f + 3.0f, 1.0f) - 0.5f));
    if (border < 0.06f) {
        t.color = t.color * 0.55f;
        t.height += 0.15f;
    } else if (grid > 0.47f) {
        t.color = t.color * 0.6f;
        t.height -= 0.1f;
    }
    return t;
}

Texel craftingSideTexel(float u, float v, std::uint32_t seed) {
    Texel t = planksTexel(u, v, seed + 5u, Rgb{0.55f, 0.40f, 0.24f});
    // Herramientas colgadas: una sierra y un martillo (siluetas oscuras).
    const bool saw = u > 0.15f && u < 0.45f && v > 0.25f && v < 0.38f;
    const bool handle = u > 0.12f && u < 0.2f && v > 0.22f && v < 0.42f;
    const bool hammer_head = u > 0.58f && u < 0.86f && v > 0.22f && v < 0.33f;
    const bool hammer_handle = u > 0.69f && u < 0.75f && v > 0.33f && v < 0.7f;
    if (saw || hammer_head) {
        t.color = Rgb{0.55f, 0.56f, 0.58f};
        t.metal = 0.9f;
        t.roughness = 0.35f;
        t.height += 0.2f;
    } else if (handle || hammer_handle) {
        t.color = Rgb{0.30f, 0.20f, 0.11f};
        t.height += 0.15f;
    }
    return t;
}

Texel snowTexel(float u, float v, std::uint32_t seed) {
    const float n = fbm(u, v, 4, 5, seed);
    const float fine = fbm(u, v, 32, 2, seed + 3u);
    Texel t;
    t.height = 0.5f + n * 0.25f + fine * 0.04f;
    t.color = Rgb{0.93f, 0.95f, 0.98f} * (0.95f + n * 0.05f);
    t.roughness = 0.55f + fine * 0.2f;
    t.reflectance = 0.6f;
    return t;
}

Texel iceTexel(float u, float v, std::uint32_t seed) {
    const Cell c = voronoi(u, v, 3, seed, 1.0f);
    const float crack = 1.0f - smooth(0.0f, 0.03f, c.f2 - c.f1);
    const float n = fbm(u, v, 4, 4, seed + 1u);
    Texel t;
    t.height = 0.6f + n * 0.1f - crack * 0.25f;
    t.color = mix(Rgb{0.62f, 0.78f, 0.95f}, Rgb{0.85f, 0.93f, 1.0f}, crack * 0.8f + n * 0.15f);
    t.roughness = 0.08f + crack * 0.3f;
    t.reflectance = 0.7f;
    return t;
}

Texel grassSideTexel(float u, float v, std::uint32_t seed, bool snowy) {
    Texel t = dirtTexel(u, v, seed);
    // Borde de hierba (o nieve) que cuelga irregular.
    const float edge = 0.16f + fbm(u, 0.0f, 8, 3, seed + 21u) * 0.07f + (rand01(static_cast<int>(u * 48.0f), 0, seed) - 0.5f) * 0.06f;
    if (v < edge) {
        Texel top = snowy ? snowTexel(u, v, seed + 5u) : grassTopTexel(u, v, seed + 5u);
        if (!snowy) top.color = top.color * 0.95f;
        const float k = smooth(edge, edge - 0.03f, v);
        t.color = mix(t.color, top.color, k);
        t.height = t.height * (1.0f - k) + (top.height * 0.5f + 0.5f) * k;
        t.roughness = t.roughness * (1.0f - k) + top.roughness * k;
    }
    return t;
}

Recipe recipeFor(const std::string& name) {
    const std::uint32_t seed = static_cast<std::uint32_t>(std::hash<std::string>{}(name) & 0xFFFFFF);
    if (name == "stone") return [=](float u, float v) { return stoneTexel(u, v, seed, Rgb{0.47f, 0.47f, 0.46f}); };
    if (name == "dirt") return [=](float u, float v) { return dirtTexel(u, v, seed); };
    if (name == "grass_top") return [=](float u, float v) { return grassTopTexel(u, v, seed); };
    if (name == "grass_side") return [=](float u, float v) { return grassSideTexel(u, v, seed, false); };
    if (name == "grass_side_snow") return [=](float u, float v) { return grassSideTexel(u, v, seed, true); };
    if (name == "cobblestone") return [=](float u, float v) { return cobbleTexel(u, v, seed); };
    if (name == "oak_log") return [=](float u, float v) { return logBarkTexel(u, v, seed, Rgb{0.36f, 0.26f, 0.16f}, 12.0f); };
    if (name == "oak_log_top") {
        return [=](float u, float v) { return logTopTexel(u, v, seed, Rgb{0.66f, 0.51f, 0.32f}, Rgb{0.33f, 0.24f, 0.15f}); };
    }
    if (name == "oak_planks") return [=](float u, float v) { return planksTexel(u, v, seed, Rgb{0.66f, 0.50f, 0.31f}); };
    if (name == "oak_leaves") return [=](float u, float v) { return leavesTexel(u, v, seed, Rgb{0.30f, 0.50f, 0.18f}, 0.18f, 7); };
    if (name == "sand") return [=](float u, float v) { return sandTexel(u, v, seed, Rgb{0.86f, 0.79f, 0.60f}); };
    if (name == "gravel") return [=](float u, float v) { return pebblesTexel(u, v, seed, 9); };
    if (name == "bedrock") {
        return [=](float u, float v) {
            Texel t = stoneTexel(u, v, seed, Rgb{0.22f, 0.22f, 0.23f});
            t.color = t.color * (0.6f + fbm01(u, v, 6, 3, seed + 1u) * 0.8f);
            return t;
        };
    }
    if (name == "coal_ore") return [=](float u, float v) { return oreTexel(u, v, seed, Rgb{0.07f, 0.07f, 0.08f}, 0.0f, 0.6f, 0.0f); };
    if (name == "iron_ore") return [=](float u, float v) { return oreTexel(u, v, seed, Rgb{0.80f, 0.62f, 0.48f}, 0.5f, 0.45f, 0.0f); };
    if (name == "gold_ore") return [=](float u, float v) { return oreTexel(u, v, seed, Rgb{0.98f, 0.82f, 0.30f}, 1.0f, 0.3f, 0.0f); };
    if (name == "diamond_ore") {
        return [=](float u, float v) { return oreTexel(u, v, seed, Rgb{0.40f, 0.92f, 0.90f}, 0.0f, 0.08f, 0.06f); };
    }
    if (name == "snow") return [=](float u, float v) { return snowTexel(u, v, seed); };
    if (name == "sandstone") {
        return [=](float u, float v) {
            Texel t = sandTexel(u, v, seed, Rgb{0.84f, 0.74f, 0.53f});
            const float bands = std::sin((v + fbm(u, v, 2, 2, seed + 3u) * 0.05f) * kTau * 3.0f);
            t.color = t.color * (0.92f + bands * 0.06f);
            t.height += bands * 0.08f;
            t.roughness = 0.88f;
            return t;
        };
    }
    if (name == "sandstone_top") return [=](float u, float v) { return sandTexel(u, v, seed, Rgb{0.86f, 0.77f, 0.56f}); };
    if (name == "glass") return [=](float u, float v) { return glassTexel(u, v, seed); };
    if (name == "bricks") {
        return [=](float u, float v) { return bricksTexel(u, v, seed, 4, Rgb{0.62f, 0.30f, 0.22f}, Rgb{0.72f, 0.70f, 0.66f}); };
    }
    if (name == "stone_bricks") {
        return [=](float u, float v) {
            Texel t = bricksTexel(u, v, seed, 2, Rgb{0.50f, 0.50f, 0.49f}, Rgb{0.36f, 0.35f, 0.34f});
            const Texel s = stoneTexel(u, v, seed + 1u, Rgb{0.5f, 0.5f, 0.5f});
            t.height = t.height * 0.7f + s.height * 0.3f;
            t.color = t.color * (0.85f + (s.color.r - 0.4f) * 0.4f);
            return t;
        };
    }
    if (name == "torch") return [=](float u, float v) { return torchTexel(u, v, seed); };
    if (name == "tall_grass") return [=](float u, float v) { return tallGrassTexel(u, v, seed, Rgb{0.36f, 0.58f, 0.22f}); };
    if (name == "red_flower") return [=](float u, float v) { return flowerTexel(u, v, seed, Rgb{0.85f, 0.12f, 0.10f}, Rgb{0.15f, 0.1f, 0.05f}); };
    if (name == "yellow_flower") return [=](float u, float v) { return flowerTexel(u, v, seed, Rgb{0.98f, 0.82f, 0.15f}, Rgb{0.9f, 0.6f, 0.1f}); };
    if (name == "birch_log") {
        return [=](float u, float v) {
            Texel t = logBarkTexel(u, v, seed, Rgb{0.86f, 0.84f, 0.79f}, 6.0f);
            const Cell c = voronoi(u, v * 3.0f, 6, seed + 3u, 1.0f);
            const float mark = idRand(c.id, 1) > 0.6f ? smooth(0.25f, 0.1f, std::abs(v * 3.0f * 6.0f - c.cy) * 3.0f + std::abs(u * 6.0f - c.cx) * 0.3f) : 0.0f;
            t.color = mix(t.color, Rgb{0.10f, 0.09f, 0.08f}, mark);
            t.height -= mark * 0.2f;
            return t;
        };
    }
    if (name == "birch_log_top") {
        return [=](float u, float v) { return logTopTexel(u, v, seed, Rgb{0.80f, 0.72f, 0.52f}, Rgb{0.85f, 0.83f, 0.78f}); };
    }
    if (name == "birch_leaves") return [=](float u, float v) { return leavesTexel(u, v, seed, Rgb{0.45f, 0.62f, 0.26f}, 0.2f, 8); };
    if (name == "spruce_log") return [=](float u, float v) { return logBarkTexel(u, v, seed, Rgb{0.26f, 0.18f, 0.11f}, 16.0f); };
    if (name == "spruce_log_top") {
        return [=](float u, float v) { return logTopTexel(u, v, seed, Rgb{0.50f, 0.36f, 0.22f}, Rgb{0.24f, 0.17f, 0.10f}); };
    }
    if (name == "spruce_leaves") return [=](float u, float v) { return needlesTexel(u, v, seed, Rgb{0.16f, 0.30f, 0.19f}); };
    if (name == "cactus_side") return [=](float u, float v) { return cactusSideTexel(u, v, seed); };
    if (name == "cactus_top") {
        return [=](float u, float v) {
            Texel t = cactusSideTexel(u, v, seed);
            const float r = std::hypot(u - 0.5f, v - 0.5f);
            t.color = t.color * (0.8f + 0.3f * std::cos(r * 40.0f) * 0.5f);
            return t;
        };
    }
    if (name == "crafting_table_top") return [=](float u, float v) { return craftingTopTexel(u, v, seed); };
    if (name == "crafting_table_side") return [=](float u, float v) { return craftingSideTexel(u, v, seed); };
    if (name == "glowstone") return [=](float u, float v) { return glowstoneTexel(u, v, seed); };
    if (name == "clay") {
        return [=](float u, float v) {
            Texel t;
            const float n = fbm(u, v, 3, 5, seed);
            t.height = 0.5f + n * 0.12f;
            t.color = Rgb{0.60f, 0.62f, 0.68f} * (0.92f + n * 0.1f);
            t.roughness = 0.6f;
            return t;
        };
    }
    if (name == "dead_bush") return [=](float u, float v) { return deadBushTexel(u, v, seed); };
    if (name == "ice") return [=](float u, float v) { return iceTexel(u, v, seed); };
    // Desconocida: gris liso.
    return [](float, float) { return Texel{}; };
}

Style styleFor(const std::string& name) {
    Style s;
    if (name == "cobblestone" || name == "gravel" || name == "bricks" || name == "stone_bricks") s.normal_strength = 5.0f;
    if (name == "sand" || name == "snow" || name == "clay") s.normal_strength = 2.0f;
    if (name.find("leaves") != std::string::npos || name == "tall_grass" || name.find("flower") != std::string::npos) {
        s.normal_strength = 2.0f;
        s.cavity = 0.4f;
    }
    if (name == "glass" || name == "torch") {
        s.normal_strength = 1.0f;
        s.cavity = 0.0f;
    }
    return s;
}

std::uint8_t byte(float v) { return static_cast<std::uint8_t>(std::clamp(v * 255.0f + 0.5f, 0.0f, 255.0f)); }

// Imagen de `folder` reescalada a size x size (bilineal). false si no existe.
bool loadResized(const std::filesystem::path& file, std::uint32_t size, std::vector<std::uint8_t>& out) {
    std::error_code error;
    if (file.empty() || !std::filesystem::exists(file, error)) return false;
    asset::ImageRgba8 image;
    if (!asset::loadImageRgba8(file, image, 4096) || image.width == 0 || image.height == 0) return false;
    out.assign(static_cast<std::size_t>(size) * size * 4, 0);
    for (std::uint32_t y = 0; y < size; ++y) {
        const float fy = (static_cast<float>(y) + 0.5f) / static_cast<float>(size) * static_cast<float>(image.height) - 0.5f;
        const int y0 = std::clamp(static_cast<int>(std::floor(fy)), 0, static_cast<int>(image.height) - 1);
        const int y1 = std::min(y0 + 1, static_cast<int>(image.height) - 1);
        const float ty = std::clamp(fy - static_cast<float>(y0), 0.0f, 1.0f);
        for (std::uint32_t x = 0; x < size; ++x) {
            const float fx = (static_cast<float>(x) + 0.5f) / static_cast<float>(size) * static_cast<float>(image.width) - 0.5f;
            const int x0 = std::clamp(static_cast<int>(std::floor(fx)), 0, static_cast<int>(image.width) - 1);
            const int x1 = std::min(x0 + 1, static_cast<int>(image.width) - 1);
            const float tx = std::clamp(fx - static_cast<float>(x0), 0.0f, 1.0f);
            for (int c = 0; c < 4; ++c) {
                const auto at = [&](int px, int py) {
                    return static_cast<float>(image.pixels[(static_cast<std::size_t>(py) * image.width + static_cast<std::size_t>(px)) * 4 + static_cast<std::size_t>(c)]);
                };
                const float top = at(x0, y0) + (at(x1, y0) - at(x0, y0)) * tx;
                const float bottom = at(x0, y1) + (at(x1, y1) - at(x0, y1)) * tx;
                out[(static_cast<std::size_t>(y) * size + x) * 4 + static_cast<std::size_t>(c)] =
                    static_cast<std::uint8_t>(std::clamp(top + (bottom - top) * ty + 0.5f, 0.0f, 255.0f));
            }
        }
    }
    return true;
}

// Genera las tres imagenes de una receta.
void generate(const std::string& name, std::uint32_t size, gfx::VoxelTextureLayer& out) {
    const Recipe recipe = recipeFor(name);
    const Style style = styleFor(name);
    const std::size_t n = static_cast<std::size_t>(size) * size;
    std::vector<Texel> texels(n);
    for (std::uint32_t y = 0; y < size; ++y) {
        for (std::uint32_t x = 0; x < size; ++x) {
            texels[static_cast<std::size_t>(y) * size + x] =
                recipe((static_cast<float>(x) + 0.5f) / static_cast<float>(size), (static_cast<float>(y) + 0.5f) / static_cast<float>(size));
        }
    }
    const auto height = [&](int x, int y) {
        return texels[static_cast<std::size_t>(wrap(y, static_cast<int>(size))) * size + static_cast<std::size_t>(wrap(x, static_cast<int>(size)))].height;
    };
    out.albedo.resize(n * 4);
    out.normal.resize(n * 4);
    out.material.resize(n * 4);
    const float texel_scale = static_cast<float>(size) / 64.0f;  // misma pendiente a cualquier resolucion
    const int blur = std::max(1, static_cast<int>(size / 64));
    for (std::uint32_t y = 0; y < size; ++y) {
        for (std::uint32_t x = 0; x < size; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * size + x;
            const Texel& t = texels[i];
            const int ix = static_cast<int>(x), iy = static_cast<int>(y);
            // Normal (verde hacia arriba de la imagen: la fila baja es -Y).
            const float dx = (height(ix + 1, iy) - height(ix - 1, iy)) * 0.5f * texel_scale * style.normal_strength;
            const float dy = (height(ix, iy + 1) - height(ix, iy - 1)) * 0.5f * texel_scale * style.normal_strength;
            float nx = -dx, ny = dy, nz = 1.0f;
            const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
            nx /= len;
            ny /= len;
            // Oclusion de las grietas: mas bajo que su entorno.
            float around = 0.0f;
            for (int oy = -1; oy <= 1; ++oy) {
                for (int ox = -1; ox <= 1; ++ox) around += height(ix + ox * blur * 2, iy + oy * blur * 2);
            }
            around /= 9.0f;
            const float ao = std::clamp(1.0f - (around - t.height) * 2.5f * style.cavity, 0.35f, 1.0f);
            out.albedo[i * 4 + 0] = byte(t.color.r);
            out.albedo[i * 4 + 1] = byte(t.color.g);
            out.albedo[i * 4 + 2] = byte(t.color.b);
            out.albedo[i * 4 + 3] = byte(t.alpha);
            out.normal[i * 4 + 0] = byte(nx * 0.5f + 0.5f);
            out.normal[i * 4 + 1] = byte(ny * 0.5f + 0.5f);
            out.normal[i * 4 + 2] = byte(std::clamp(t.height, 0.0f, 1.0f));
            out.normal[i * 4 + 3] = byte(ao);
            out.material[i * 4 + 0] = byte(t.roughness);
            out.material[i * 4 + 1] = byte(t.metal);
            out.material[i * 4 + 2] = byte(t.emission);
            out.material[i * 4 + 3] = byte(t.reflectance);
        }
    }
}

}  // namespace

void buildTexture(const std::string& name, std::uint32_t size, const std::filesystem::path& folder,
                  gfx::VoxelTextureLayer& out) {
    generate(name, size, out);
    if (folder.empty()) return;
    // Las que haya en disco mandan (cada una por separado).
    std::vector<std::uint8_t> image;
    if (loadResized(folder / (name + ".png"), size, image)) out.albedo = std::move(image);
    if (loadResized(folder / (name + "_n.png"), size, image)) out.normal = std::move(image);
    if (loadResized(folder / (name + "_m.png"), size, image)) out.material = std::move(image);
}

bool writeTexturePngs(const std::filesystem::path& folder, std::uint32_t size) {
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    bool ok = true;
    for (const std::string& name : textureNames()) {
        gfx::VoxelTextureLayer layer;
        generate(name, size, layer);
        const auto save = [&](const std::string& file, const std::vector<std::uint8_t>& pixels) {
            asset::ImageRgba8 image;
            image.width = size;
            image.height = size;
            image.pixels = pixels;
            ok = asset::saveImagePng(folder / file, image) && ok;
        };
        save(name + ".png", layer.albedo);
        save(name + "_n.png", layer.normal);
        save(name + "_m.png", layer.material);
    }
    return ok;
}

bool writeBlockIcons(const std::filesystem::path& folder, std::uint32_t size, const std::filesystem::path& textures_folder) {
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    constexpr std::uint32_t kSource = 64;
    std::vector<gfx::VoxelTextureLayer> cache(textureNames().size());
    std::vector<bool> loaded(textureNames().size(), false);
    const auto texture = [&](const std::string& name) -> const gfx::VoxelTextureLayer& {
        const int layer = textureLayer(name);
        if (!loaded[static_cast<std::size_t>(layer)]) {
            buildTexture(name, kSource, textures_folder, cache[static_cast<std::size_t>(layer)]);
            loaded[static_cast<std::size_t>(layer)] = true;
        }
        return cache[static_cast<std::size_t>(layer)];
    };
    const auto sample = [&](const gfx::VoxelTextureLayer& t, float u, float v, float shade, float out[4]) {
        const auto x = std::min(static_cast<std::uint32_t>(std::clamp(u, 0.0f, 0.9999f) * kSource), kSource - 1);
        const auto y = std::min(static_cast<std::uint32_t>(std::clamp(v, 0.0f, 0.9999f) * kSource), kSource - 1);
        const std::size_t i = (static_cast<std::size_t>(y) * kSource + x) * 4;
        for (int c = 0; c < 3; ++c) out[c] = static_cast<float>(t.albedo[i + static_cast<std::size_t>(c)]) / 255.0f * shade;
        out[3] = static_cast<float>(t.albedo[i + 3]) / 255.0f;
    };
    bool ok = true;
    for (std::size_t id = 1; id < blocks().size(); ++id) {
        const BlockDef& def = blocks()[id];
        if (def.shape == BlockShape::Air || def.shape == BlockShape::Liquid) continue;
        asset::ImageRgba8 image;
        image.width = image.height = size;
        image.pixels.assign(static_cast<std::size_t>(size) * size * 4, 0);
        // Tinte tipico del bloque (hierba y hojas).
        const float tint[3] = {def.tint == BlockTint::None ? 1.0f : 0.82f, 1.0f, def.tint == BlockTint::None ? 1.0f : 0.72f};
        for (std::uint32_t py = 0; py < size; ++py) {
            for (std::uint32_t px = 0; px < size; ++px) {
                const float x = (static_cast<float>(px) + 0.5f) / static_cast<float>(size);
                const float y = (static_cast<float>(py) + 0.5f) / static_cast<float>(size);
                float color[4] = {0, 0, 0, 0};
                if (def.shape == BlockShape::Cross) {
                    sample(texture(def.side), (x - 0.1f) / 0.8f, (y - 0.1f) / 0.8f, 1.0f, color);
                    if (x < 0.1f || x > 0.9f || y < 0.1f || y > 0.9f) color[3] = 0.0f;
                } else {
                    // Cubo isometrico: rombo de arriba y dos caras laterales.
                    const float cx = x - 0.5f;
                    const float h = 0.25f;   // media altura del rombo
                    const float top_center = 0.28f;
                    // Coordenadas en la cara de arriba (u, v de 0..1).
                    const float tu = (cx / 0.43f + (y - top_center) / h) * 0.5f + 0.5f;
                    const float tv = (-cx / 0.43f + (y - top_center) / h) * 0.5f + 0.5f;
                    if (tu >= 0.0f && tu <= 1.0f && tv >= 0.0f && tv <= 1.0f) {
                        sample(texture(def.top), tu, tv, 1.0f, color);
                        if (def.tint == BlockTint::Grass || def.tint == BlockTint::Foliage) {
                            for (int c = 0; c < 3; ++c) color[c] *= tint[c];
                        }
                    } else {
                        const bool left = cx < 0.0f;
                        const float edge_y = top_center + h * (1.0f - std::abs(cx) / 0.43f);  // borde inferior del rombo
                        const float su = left ? (cx + 0.43f) / 0.43f : cx / 0.43f;
                        const float sv = (y - edge_y) / 0.47f;
                        if (std::abs(cx) <= 0.43f && sv >= 0.0f && sv <= 1.0f) {
                            sample(texture(def.side), su, sv, left ? 0.78f : 0.6f, color);
                            if (def.tint == BlockTint::Foliage) {
                                for (int c = 0; c < 3; ++c) color[c] *= tint[c];
                            }
                        }
                    }
                }
                const std::size_t i = (static_cast<std::size_t>(py) * size + px) * 4;
                for (int c = 0; c < 4; ++c) image.pixels[i + static_cast<std::size_t>(c)] = byte(color[c]);
            }
        }
        ok = asset::saveImagePng(folder / (def.name + ".png"), image) && ok;
    }
    return ok;
}

}  // namespace cramion::voxel
