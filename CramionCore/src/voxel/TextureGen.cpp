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

// --- Vegetacion del generador Aventura ----------------------------------------------

// Helecho: frondas arqueadas desde abajo con foliolos a los lados.
Texel fernTexel(float u, float v, std::uint32_t seed) {
    Texel t;
    t.alpha = 0.0f;
    t.height = 1.0f;
    t.roughness = 0.7f;
    const Rgb tone{0.24f, 0.48f, 0.18f};
    for (int i = 0; i < 6; ++i) {
        const float angle = (static_cast<float>(i) / 5.0f - 0.5f) * 1.9f + (rand01(i, 0, seed) - 0.5f) * 0.3f;
        const float length = 0.55f + rand01(i, 1, seed) * 0.35f;
        const float dx = u - 0.5f, dy = 1.0f - v;
        const float along = dx * std::sin(angle) + dy * std::cos(angle);
        if (along < 0.0f || along > length) continue;
        // La fronda se curva hacia fuera al subir.
        const float bend = along * along * 0.35f * (angle < 0.0f ? -1.0f : 1.0f);
        const float across = dx * std::cos(angle) - dy * std::sin(angle) - bend;
        const float k = along / length;
        const float leaflet = 0.07f * (1.0f - k) * (0.6f + 0.4f * std::abs(std::sin(along * 60.0f)));
        if (std::abs(across) < 0.008f + leaflet) {
            t.alpha = 1.0f;
            t.color = tone * (0.75f + 0.45f * k) * (0.9f + rand01(i, 2, seed) * 0.2f);
        }
    }
    return t;
}

// Arbusto: una mata redonda de hojas apretadas.
Texel bushTexel(float u, float v, std::uint32_t seed) {
    Texel t = leavesTexel(u, v, seed, Rgb{0.28f, 0.47f, 0.17f}, 0.55f, 9);
    const float dx = (u - 0.5f) / 0.46f, dy = (v - 0.58f) / 0.42f;
    const float edge = fbm(u, v, 6, 2, seed + 13u) * 0.12f;
    if (dx * dx + dy * dy > 1.0f + edge) t.alpha = 0.0f;
    return t;
}

// Seta: pie claro y sombrero; `spots` = motas blancas, `glow` = emision.
Texel mushroomTexel(float u, float v, std::uint32_t seed, Rgb cap, bool spots, float glow) {
    Texel t;
    t.alpha = 0.0f;
    t.height = 1.0f;
    t.roughness = 0.55f;
    // Pie.
    if (v > 0.58f && std::abs(u - 0.5f) < 0.06f + (v - 0.58f) * 0.05f) {
        t.alpha = 1.0f;
        t.color = Rgb{0.88f, 0.84f, 0.74f};
    }
    // Sombrero: media elipse.
    const float cx = (u - 0.5f) / 0.3f, cy = (v - 0.6f) / 0.24f;
    if (cy < 0.0f && cx * cx + cy * cy < 1.0f) {
        t.alpha = 1.0f;
        t.color = cap * (0.85f + 0.25f * (1.0f + cy));
        t.emission = glow;
        if (spots) {
            const Cell c = voronoi(u, v, 7, seed + 3u, 0.8f);
            if (c.f1 < 0.18f) t.color = Rgb{0.95f, 0.93f, 0.88f};
        }
    }
    return t;
}

// Juncos: tallos rectos y altos con la punta parda.
Texel reedsTexel(float u, float v, std::uint32_t seed) {
    Texel t;
    t.alpha = 0.0f;
    t.height = 1.0f;
    t.roughness = 0.7f;
    for (int i = 0; i < 9; ++i) {
        const float x = 0.12f + rand01(i, 0, seed) * 0.76f;
        const float top = rand01(i, 1, seed) * 0.15f;
        if (v < top || std::abs(u - x) > 0.016f) continue;
        t.alpha = 1.0f;
        const bool tip = v < top + 0.16f && i % 3 == 0;
        t.color = tip ? Rgb{0.42f, 0.28f, 0.15f} : Rgb{0.38f, 0.56f, 0.24f} * (0.8f + rand01(i, 2, seed) * 0.3f);
    }
    return t;
}

// Terracota: franjas horizontales de tonos de barro (las mesetas).
Texel terracottaTexel(float u, float v, std::uint32_t seed) {
    Texel t;
    const float band = std::sin((v + fbm(u, v, 2, 2, seed) * 0.05f) * kTau * 3.0f);
    const Rgb clay{0.70f, 0.40f, 0.26f};
    const Rgb cream{0.82f, 0.66f, 0.50f};
    t.color = mix(clay, cream, std::clamp(band * 0.5f + 0.5f, 0.0f, 1.0f) * 0.6f) * (0.92f + fbm(u, v, 12, 2, seed + 5u) * 0.06f);
    t.height = 0.5f + fbm(u, v, 8, 3, seed + 7u) * 0.15f;
    t.roughness = 0.9f;
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

// --- Dimensiones (abismo, cielo, cristal) ---------------------------------------------

// Cristal facetado que brilla (emision): vetas claras sobre su color.
Texel crystalTexel(float u, float v, std::uint32_t seed, Rgb tone, float glow) {
    const Cell c = voronoi(u, v, 4, seed, 1.0f);
    const float facet = idRand(c.id, 1);
    const float edge = 1.0f - smooth(0.0f, 0.05f, c.f2 - c.f1);
    Texel t;
    t.color = mix(tone * (0.8f + facet * 0.35f), Rgb{1.0f, 1.0f, 1.0f}, edge * 0.45f);
    t.height = 0.6f + facet * 0.3f - edge * 0.2f;
    t.roughness = 0.15f;
    t.reflectance = 0.8f;
    t.emission = glow * (0.6f + facet * 0.4f);
    return t;
}

// Planta con hojas finas (fondo transparente) del color dado; `glow` = emision.
Texel bladesTexel(float u, float v, std::uint32_t seed, Rgb tone, float glow) {
    Texel t = tallGrassTexel(u, v, seed, tone);
    if (t.alpha > 0.5f) t.emission = glow;
    return t;
}

// Brote de cristal: tres puntas facetadas.
Texel sproutTexel(float u, float v, std::uint32_t seed, Rgb tone) {
    Texel t;
    t.alpha = 0.0f;
    t.height = 1.0f;
    t.roughness = 0.2f;
    for (int i = 0; i < 3; ++i) {
        const float x = 0.3f + 0.2f * static_cast<float>(i) + (rand01(i, 0, seed) - 0.5f) * 0.08f;
        const float top = 0.15f + rand01(i, 1, seed) * 0.35f;
        const float w = 0.09f * std::clamp((v - top) / 0.25f, 0.0f, 1.0f);
        if (v > top && std::abs(u - x) < w) {
            t.alpha = 1.0f;
            t.color = mix(tone, Rgb{1.0f, 1.0f, 1.0f}, (u < x ? 0.35f : 0.0f));
            t.emission = 0.8f;
        }
    }
    return t;
}

Recipe dimensionRecipe(const std::string& name, std::uint32_t seed) {
    // Abismo
    if (name == "basalt") {
        return [=](float u, float v) {
            Texel t = stoneTexel(u, v * 0.3f, seed, Rgb{0.24f, 0.23f, 0.27f});
            const float col = std::abs(std::sin(u * kTau * 2.0f));
            t.height -= (1.0f - smooth(0.0f, 0.08f, col)) * 0.3f;
            return t;
        };
    }
    if (name == "basalt_top") return [=](float u, float v) { return pebblesTexel(u, v, seed, 3); };
    if (name == "obsidian") return [=](float u, float v) { return crystalTexel(u, v, seed, Rgb{0.12f, 0.06f, 0.20f}, 0.0f); };
    if (name == "lava") {
        return [=](float u, float v) {
            const float n = fbm01(u, v, 3, 4, seed);
            Texel t;
            t.color = mix(Rgb{1.0f, 0.32f, 0.04f}, Rgb{1.0f, 0.78f, 0.25f}, smooth(0.45f, 0.8f, n));
            t.height = 0.5f + n * 0.1f;
            t.roughness = 0.6f;
            t.emission = 0.85f + n * 0.15f;
            return t;
        };
    }
    if (name == "magma") {
        return [=](float u, float v) {
            const Cell c = voronoi(u, v, 4, seed, 1.0f);
            const float crack = 1.0f - smooth(0.0f, 0.07f, c.f2 - c.f1);
            Texel t = stoneTexel(u, v, seed, Rgb{0.30f, 0.12f, 0.08f});
            t.color = mix(t.color, Rgb{1.0f, 0.45f, 0.08f}, crack);
            t.emission = crack * 0.9f + 0.12f;
            return t;
        };
    }
    if (name == "ash") return [=](float u, float v) { return sandTexel(u, v, seed, Rgb{0.36f, 0.33f, 0.33f}); };
    if (name == "fire_stem") return [=](float u, float v) { return logBarkTexel(u, v, seed, Rgb{0.55f, 0.42f, 0.32f}, 10.0f); };
    if (name == "fire_cap") {
        return [=](float u, float v) {
            const Cell c = voronoi(u, v, 5, seed, 0.8f);
            Texel t;
            t.color = c.f1 < 0.16f ? Rgb{1.0f, 0.85f, 0.35f} : Rgb{0.95f, 0.28f, 0.08f} * (0.9f + fbm(u, v, 6, 2, seed) * 0.1f);
            t.height = c.f1 < 0.16f ? 0.75f : 0.55f;
            t.roughness = 0.55f;
            t.emission = c.f1 < 0.16f ? 0.9f : 0.35f;
            return t;
        };
    }
    if (name == "ember_grass") return [=](float u, float v) { return bladesTexel(u, v, seed, Rgb{1.0f, 0.42f, 0.10f}, 0.8f); };
    if (name == "abyss_bricks") {
        return [=](float u, float v) { return bricksTexel(u, v, seed, 4, Rgb{0.58f, 0.20f, 0.15f}, Rgb{0.20f, 0.10f, 0.09f}); };
    }
    // Cielo
    if (name == "sky_grass_top") {
        return [=](float u, float v) {
            Texel t = grassTopTexel(u, v, seed);
            t.color = Rgb{t.color.r * 0.9f + 0.05f, t.color.g * 1.35f, t.color.b * 1.6f + 0.06f};  // verde turquesa
            return t;
        };
    }
    if (name == "sky_grass_side") {
        return [=](float u, float v) {
            Texel t = grassSideTexel(u, v, seed, false);
            if (v > 0.2f) t.color = Rgb{t.color.r * 1.25f + 0.05f, t.color.g * 1.05f + 0.02f, t.color.b * 1.05f};
            return t;
        };
    }
    if (name == "sky_soil") {
        return [=](float u, float v) {
            Texel t = dirtTexel(u, v, seed);
            t.color = Rgb{t.color.r * 1.3f + 0.05f, t.color.g * 1.05f + 0.02f, t.color.b * 1.05f};
            return t;
        };
    }
    if (name == "sky_stone") return [=](float u, float v) { return stoneTexel(u, v, seed, Rgb{0.80f, 0.76f, 0.70f}); };
    if (name == "cloud") {
        return [=](float u, float v) {
            Texel t;
            t.color = Rgb{0.97f, 0.97f, 1.0f} * (0.95f + fbm(u, v, 3, 3, seed) * 0.05f);
            t.height = 0.5f;
            t.roughness = 1.0f;
            return t;
        };
    }
    if (name == "golden_log") return [=](float u, float v) { return logBarkTexel(u, v, seed, Rgb{0.90f, 0.86f, 0.78f}, 14.0f); };
    if (name == "golden_log_top") {
        return [=](float u, float v) { return logTopTexel(u, v, seed, Rgb{0.92f, 0.78f, 0.50f}, Rgb{0.88f, 0.84f, 0.76f}); };
    }
    if (name == "golden_leaves") return [=](float u, float v) { return leavesTexel(u, v, seed, Rgb{1.0f, 0.74f, 0.18f}, 0.18f, 7); };
    if (name == "sky_crystal") return [=](float u, float v) { return crystalTexel(u, v, seed, Rgb{0.45f, 0.85f, 1.0f}, 0.55f); };
    if (name == "falling_water") {
        return [=](float u, float v) {
            const float streak = fbm01(u * 4.0f, v * 0.5f, 4, 3, seed);
            Texel t;
            t.color = mix(Rgb{0.35f, 0.70f, 0.95f}, Rgb{0.85f, 0.95f, 1.0f}, smooth(0.55f, 0.8f, streak));
            t.height = 0.5f;
            t.roughness = 0.1f;
            t.reflectance = 0.6f;
            t.emission = 0.05f;
            return t;
        };
    }
    if (name == "sky_flower") {
        return [=](float u, float v) { return flowerTexel(u, v, seed, Rgb{0.55f, 0.80f, 1.0f}, Rgb{1.0f, 1.0f, 0.85f}); };
    }
    // Cristal
    if (name == "glow_moss") {
        return [=](float u, float v) {
            Texel t = sandTexel(u, v, seed, Rgb{0.16f, 0.62f, 0.55f});
            const Cell c = voronoi(u, v, 9, seed + 3u, 0.9f);
            const float spot = smooth(0.22f, 0.08f, c.f1) * (idRand(c.id, 1) > 0.6f ? 1.0f : 0.0f);
            t.color = mix(t.color, Rgb{0.55f, 1.0f, 0.85f}, spot);
            t.emission = 0.10f + spot * 0.6f;
            return t;
        };
    }
    if (name == "glow_moss_side") {
        return [=](float u, float v) {
            Texel t = dirtTexel(u, v, seed);
            t.color = Rgb{t.color.r * 0.6f, t.color.g * 0.55f, t.color.b * 1.1f + 0.08f};
            if (v < 0.2f + fbm(u, 0.0f, 8, 2, seed) * 0.06f) {
                t.color = Rgb{0.16f, 0.62f, 0.55f};
                t.emission = 0.1f;
            }
            return t;
        };
    }
    if (name == "dusk_soil") {
        return [=](float u, float v) {
            Texel t = dirtTexel(u, v, seed);
            t.color = Rgb{t.color.r * 0.6f, t.color.g * 0.55f, t.color.b * 1.1f + 0.08f};
            return t;
        };
    }
    if (name == "dusk_stone") return [=](float u, float v) { return stoneTexel(u, v, seed, Rgb{0.24f, 0.22f, 0.36f}); };
    if (name == "crystal_pink") return [=](float u, float v) { return crystalTexel(u, v, seed, Rgb{1.0f, 0.42f, 0.78f}, 0.5f); };
    if (name == "crystal_blue") return [=](float u, float v) { return crystalTexel(u, v, seed, Rgb{0.30f, 0.62f, 1.0f}, 0.5f); };
    if (name == "crystal_violet") return [=](float u, float v) { return crystalTexel(u, v, seed, Rgb{0.66f, 0.38f, 1.0f}, 0.5f); };
    if (name == "crystal_green") return [=](float u, float v) { return crystalTexel(u, v, seed, Rgb{0.35f, 1.0f, 0.62f}, 0.5f); };
    if (name == "shroom_stem") return [=](float u, float v) { return logBarkTexel(u, v, seed, Rgb{0.86f, 0.84f, 0.92f}, 8.0f); };
    if (name == "shroom_cap_blue" || name == "shroom_cap_violet") {
        const Rgb tone = name == "shroom_cap_blue" ? Rgb{0.18f, 0.52f, 1.0f} : Rgb{0.62f, 0.25f, 0.95f};
        return [=](float u, float v) {
            const Cell c = voronoi(u, v, 5, seed, 0.8f);
            const bool spot = c.f1 < 0.15f;
            Texel t;
            t.color = spot ? Rgb{0.85f, 1.0f, 1.0f} : tone * (0.9f + fbm(u, v, 6, 2, seed) * 0.1f);
            t.height = spot ? 0.75f : 0.55f;
            t.roughness = 0.5f;
            t.emission = spot ? 0.9f : 0.35f;
            return t;
        };
    }
    if (name == "glow_grass") return [=](float u, float v) { return bladesTexel(u, v, seed, Rgb{0.30f, 0.95f, 0.85f}, 0.5f); };
    if (name == "crystal_sprout") return [=](float u, float v) { return sproutTexel(u, v, seed, Rgb{1.0f, 0.5f, 0.85f}); };
    return {};
}

// --- Estructuras: construccion, muebles y mazmorras --------------------------------------
// (v crece hacia abajo en la textura: las patas de una mesa estan en v alto.)

// Color liso con un poco de grano.
Texel plainTexel(float u, float v, std::uint32_t seed, Rgb tone, float grain = 0.08f) {
    const float n = fbm(u, v, 6, 3, seed);
    Texel t;
    t.height = 0.55f + n * 0.08f;
    t.color = tone * (1.0f + n * grain);
    t.roughness = 0.85f;
    return t;
}

// Tela (lana): trama fina.
Texel clothTexel(float u, float v, std::uint32_t seed, Rgb tone) {
    const float weave = (std::sin(u * kTau * 32.0f) * std::sin(v * kTau * 32.0f)) * 0.5f + 0.5f;
    Texel t = plainTexel(u, v, seed, tone, 0.1f);
    t.color = t.color * (0.94f + weave * 0.08f);
    t.height += weave * 0.05f;
    t.roughness = 0.95f;
    return t;
}

// Tejas: filas escalonadas con el borde de abajo redondeado y oscuro.
Texel roofTexel(float u, float v, std::uint32_t seed, Rgb tone) {
    constexpr int kRows = 4;
    const int row = static_cast<int>(std::floor(v * kRows));
    const float lv = v * kRows - static_cast<float>(row);
    const float shift = (row % 2) * 0.125f;
    const float tu = std::fmod(u * 4.0f + shift + 4.0f, 1.0f);
    const int tile = static_cast<int>(std::floor(u * 4.0f + shift + 4.0f));
    const float curve = 0.82f + 0.12f * std::sin(tu * 3.14159f);  // borde inferior en arco
    const float shade = lv > curve ? 0.62f : (0.86f + lv * 0.2f);
    const float joint = smooth(0.05f, 0.0f, std::min(tu, 1.0f - tu));
    Texel t;
    t.height = 0.4f + lv * 0.5f - joint * 0.2f;
    t.color = tone * shade * (0.92f + rand01(tile, row, seed) * 0.16f) * (1.0f - joint * 0.25f);
    t.roughness = 0.7f;
    return t;
}

// Paja: hebras verticales.
Texel strawTexel(float u, float v, std::uint32_t seed, Rgb tone) {
    const float strands = fbm(u * 8.0f, v * 0.5f, 8, 3, seed);
    Texel t;
    t.height = 0.5f + strands * 0.3f;
    t.color = tone * (0.85f + strands * 0.3f);
    t.roughness = 0.95f;
    return t;
}

// Llamas (cruz o la parte de arriba de un brasero): lenguas de fuego desde v0.
float flameMask(float u, float v, float v0, float height, std::uint32_t seed, float& core) {
    core = 0.0f;
    float best = 0.0f;
    for (int i = 0; i < 4; ++i) {
        const float cx = 0.25f + 0.5f * rand01(i, 0, seed);
        const float h = height * (0.6f + 0.4f * rand01(i, 1, seed));
        const float k = (v0 - v) / h;  // 0 en la base, 1 en la punta
        if (k < 0.0f || k > 1.0f) continue;
        const float width = 0.16f * (1.0f - k * k) * (0.7f + 0.3f * rand01(i, 2, seed));
        const float d = std::abs(u - cx - std::sin(k * 4.0f + static_cast<float>(i)) * 0.03f);
        if (d < width) {
            best = 1.0f;
            core = std::max(core, 1.0f - d / width * 0.8f - k * 0.5f);
        }
    }
    return best;
}

Texel flameTexel(float u, float v, std::uint32_t seed, float base, float height) {
    Texel t;
    t.alpha = 0.0f;
    float core = 0.0f;
    if (flameMask(u, v, base, height, seed, core) > 0.0f) {
        t.alpha = 1.0f;
        t.color = mix(Rgb{0.95f, 0.35f, 0.08f}, Rgb{1.0f, 0.92f, 0.55f}, std::clamp(core, 0.0f, 1.0f));
        t.emission = 1.0f;
        t.roughness = 1.0f;
    }
    return t;
}

// Marco (borde de `border`) de un color y el resto de otro.
bool inFrame(float u, float v, float border) { return u < border || v < border || u > 1.0f - border || v > 1.0f - border; }

Texel woodPiece(float u, float v, std::uint32_t seed, Rgb tone) {
    Texel t = planksTexel(u, v, seed, tone);
    return t;
}

Texel tableSideTexel(float u, float v, std::uint32_t seed, Rgb tone, float top_band, float leg) {
    Texel t = woodPiece(u, v, seed, tone);
    const bool on = v < top_band || u < leg || u > 1.0f - leg;
    t.alpha = on ? 1.0f : 0.0f;
    if (v < top_band) t.color = t.color * 1.08f;
    return t;
}

Texel chairTexel(float u, float v, std::uint32_t seed) {
    Texel t = woodPiece(u, v, seed, Rgb{0.55f, 0.38f, 0.22f});
    const bool post = u < 0.14f || u > 0.86f;
    const bool seat = v > 0.48f && v < 0.6f;
    const bool rail = v > 0.06f && v < 0.16f;
    t.alpha = (post || seat || rail) ? 1.0f : 0.0f;
    return t;
}

Texel bedTopTexel(float u, float v, std::uint32_t seed) {
    Texel t = clothTexel(u, v, seed, Rgb{0.74f, 0.17f, 0.15f});
    if (v < 0.3f && u > 0.12f && u < 0.88f) t.color = Rgb{0.95f, 0.94f, 0.9f} * (0.95f + fbm(u, v, 8, 2, seed) * 0.05f);  // almohada
    if (std::abs(v - 0.42f) < 0.04f) t.color = Rgb{0.95f, 0.92f, 0.85f};  // embozo
    if (inFrame(u, v, 0.05f)) t.color = Rgb{0.36f, 0.24f, 0.14f};
    return t;
}

Texel bedSideTexel(float u, float v, std::uint32_t seed) {
    if (v > 0.55f) {
        Texel t = woodPiece(u, v, seed, Rgb{0.36f, 0.24f, 0.14f});
        return t;
    }
    Texel t = clothTexel(u, v, seed, Rgb{0.74f, 0.17f, 0.15f});
    if (v < 0.1f) t.color = Rgb{0.95f, 0.92f, 0.85f};
    return t;
}

Texel bookshelfTexel(float u, float v, std::uint32_t seed) {
    Texel t = woodPiece(u, v, seed, Rgb{0.36f, 0.24f, 0.14f});
    const bool frame = inFrame(u, v, 0.07f) || std::abs(v - 0.5f) < 0.04f;
    if (frame) return t;
    // Lomos de libros de colores y alturas distintas.
    const int shelf = v < 0.5f ? 0 : 1;
    const float x = (u - 0.07f) / 0.86f;
    const int book = static_cast<int>(x * 9.0f);
    const float r = rand01(book, shelf, seed);
    static const Rgb kColors[6] = {{0.62f, 0.15f, 0.12f}, {0.16f, 0.30f, 0.58f}, {0.18f, 0.45f, 0.22f},
                                   {0.70f, 0.55f, 0.18f}, {0.42f, 0.22f, 0.45f}, {0.55f, 0.35f, 0.20f}};
    const float top = (shelf == 0 ? 0.07f : 0.54f) + r * 0.1f;
    if (v < top) {
        t.color = Rgb{0.12f, 0.08f, 0.05f};
        t.height = 0.2f;
        return t;
    }
    t.color = kColors[static_cast<int>(rand01(book, shelf + 7, seed) * 5.99f)] * (0.85f + r * 0.25f);
    const float bx = x * 9.0f - static_cast<float>(book);
    if (bx < 0.1f || bx > 0.9f) t.color = t.color * 0.6f;
    if (std::abs(v - top - 0.08f) < 0.015f) t.color = Rgb{0.85f, 0.72f, 0.3f};  // letras doradas
    t.height = 0.55f;
    return t;
}

Texel barrelTexel(float u, float v, std::uint32_t seed) {
    Texel t = woodPiece(v, u, seed, Rgb{0.50f, 0.32f, 0.17f});  // duelas verticales
    const float stave = std::fmod(u * 6.0f, 1.0f);
    t.color = t.color * (0.85f + 0.2f * std::sin(stave * 3.14159f));
    if (std::abs(v - 0.2f) < 0.05f || std::abs(v - 0.8f) < 0.05f) {
        t.color = Rgb{0.25f, 0.25f, 0.27f};
        t.metal = 0.7f;
        t.roughness = 0.45f;
    }
    return t;
}

Texel barrelTopTexel(float u, float v, std::uint32_t seed) {
    Texel t = logTopTexel(u, v, seed, Rgb{0.58f, 0.40f, 0.22f}, Rgb{0.25f, 0.25f, 0.27f});
    return t;
}

Texel chestTexel(float u, float v, std::uint32_t seed, bool top) {
    Texel t = woodPiece(u, v, seed, Rgb{0.55f, 0.34f, 0.15f});
    const bool band = inFrame(u, v, 0.08f) || (!top && std::abs(v - 0.36f) < 0.04f) || (top && std::abs(u - 0.5f) < 0.05f);
    if (band) {
        t.color = Rgb{0.30f, 0.28f, 0.27f};
        t.metal = 0.6f;
    }
    if (!top && std::abs(u - 0.5f) < 0.08f && v > 0.3f && v < 0.5f) {
        t.color = Rgb{0.95f, 0.78f, 0.25f};  // cerradura dorada
        t.metal = 1.0f;
        t.roughness = 0.3f;
    }
    return t;
}

Texel crateTexel(float u, float v, std::uint32_t seed) {
    Texel t = woodPiece(u, v, seed, Rgb{0.72f, 0.56f, 0.34f});
    if (inFrame(u, v, 0.1f) || std::abs(u - v) < 0.06f) t.color = Rgb{0.50f, 0.36f, 0.20f};
    return t;
}

Texel counterTexel(float u, float v, std::uint32_t seed) {
    Texel t = woodPiece(v, u, seed, Rgb{0.42f, 0.27f, 0.15f});
    if (inFrame(u, v, 0.08f) || std::abs(u - 0.5f) < 0.03f) t.color = Rgb{0.28f, 0.18f, 0.10f};
    return t;
}

Texel shelfTexel(float u, float v, std::uint32_t seed) {
    Texel t = woodPiece(u, v, seed, Rgb{0.36f, 0.24f, 0.14f});
    if (inFrame(u, v, 0.07f) || std::abs(v - 0.5f) < 0.04f) return t;
    t.color = Rgb{0.15f, 0.1f, 0.07f};
    // Frascos (pociones de colores).
    const int shelf = v < 0.5f ? 0 : 1;
    const int jar = static_cast<int>((u - 0.07f) / 0.86f * 4.0f);
    const float cx = 0.07f + (static_cast<float>(jar) + 0.5f) * 0.215f;
    const float bottom = shelf == 0 ? 0.46f : 0.93f;
    const float h = 0.22f + rand01(jar, shelf, seed) * 0.1f;
    static const Rgb kPotions[4] = {{0.85f, 0.15f, 0.2f}, {0.2f, 0.4f, 0.95f}, {0.25f, 0.8f, 0.3f}, {0.8f, 0.35f, 0.9f}};
    const float dx = std::abs(u - cx);
    const float k = (bottom - v) / h;
    if (k > 0.0f && k < 1.0f && dx < (k > 0.75f ? 0.03f : 0.075f)) {
        t.color = k > 0.75f ? Rgb{0.85f, 0.85f, 0.8f} : kPotions[static_cast<int>(rand01(jar, shelf + 3, seed) * 3.99f)];
        t.roughness = 0.15f;
        t.emission = k > 0.75f ? 0.0f : 0.25f;
    }
    return t;
}

Texel anvilTexel(float u, float v, std::uint32_t seed, bool top) {
    Texel t = plainTexel(u, v, seed, Rgb{0.24f, 0.24f, 0.26f}, 0.15f);
    t.metal = 0.85f;
    t.roughness = 0.4f;
    if (!top) {
        const float half = v < 0.3f ? 0.5f : (v < 0.68f ? 0.2f : 0.36f);
        t.alpha = std::abs(u - 0.5f) < half && v > 0.12f ? 1.0f : 0.0f;
    }
    return t;
}

Texel forgeTexel(float u, float v, std::uint32_t seed, bool top) {
    Texel t = bricksTexel(u, v, seed, 4, Rgb{0.40f, 0.38f, 0.36f}, Rgb{0.2f, 0.19f, 0.18f});
    if (top) {
        if (!inFrame(u, v, 0.15f)) {
            const float n = fbm(u, v, 6, 3, seed + 5u) * 0.5f + 0.5f;
            t.color = mix(Rgb{0.9f, 0.25f, 0.05f}, Rgb{1.0f, 0.85f, 0.4f}, n);
            t.emission = 0.9f;
        }
    } else if (v > 0.5f && v < 0.85f && std::abs(u - 0.5f) < 0.28f) {
        t.color = mix(Rgb{0.95f, 0.35f, 0.06f}, Rgb{1.0f, 0.8f, 0.35f}, fbm01(u, v, 5, 2, seed + 9u));
        t.emission = 1.0f;
    }
    return t;
}

Texel ovenTexel(float u, float v, std::uint32_t seed) {
    Texel t = bricksTexel(u, v, seed, 4, Rgb{0.62f, 0.34f, 0.24f}, Rgb{0.5f, 0.46f, 0.42f});
    const float dx = (u - 0.5f) / 0.3f, dy = (v - 0.72f) / 0.3f;
    if (v < 0.92f && v > 0.42f && (dy > 0.0f || dx * dx + dy * dy < 1.0f) && std::abs(dx) < 1.0f) {
        t.color = mix(Rgb{0.9f, 0.3f, 0.05f}, Rgb{1.0f, 0.8f, 0.4f}, fbm01(u, v, 5, 2, seed + 3u));
        t.emission = 0.9f;
    }
    return t;
}

Texel rugTexel(float u, float v, std::uint32_t seed, Rgb tone, Rgb trim) {
    Texel t = clothTexel(u, v, seed, tone);
    const float b = std::min(std::min(u, 1.0f - u), std::min(v, 1.0f - v));
    if ((b > 0.06f && b < 0.11f) || (std::abs(u - 0.5f) + std::abs(v - 0.5f) < 0.18f && std::abs(u - 0.5f) + std::abs(v - 0.5f) > 0.12f)) {
        t.color = trim;
    }
    return t;
}

Texel paintingTexel(float u, float v, std::uint32_t seed) {
    Texel t;
    if (inFrame(u, v, 0.1f)) {
        t = plainTexel(u, v, seed, Rgb{0.78f, 0.6f, 0.22f}, 0.2f);
        t.metal = 0.6f;
        return t;
    }
    // Paisaje: cielo, sol, montes y prado.
    const float hill = 0.55f + std::sin(u * 9.0f + 1.0f) * 0.08f + fbm(u, 0.0f, 3, 2, seed) * 0.08f;
    const float distant = 0.48f + std::sin(u * 5.0f + 3.0f) * 0.1f;
    if (std::hypot(u - 0.7f, v - 0.3f) < 0.08f) t.color = Rgb{1.0f, 0.88f, 0.45f};
    else if (v < distant) t.color = mix(Rgb{0.45f, 0.65f, 0.92f}, Rgb{0.85f, 0.9f, 0.95f}, v * 1.5f);
    else if (v < hill) t.color = Rgb{0.42f, 0.48f, 0.62f};
    else t.color = Rgb{0.32f, 0.58f, 0.25f} * (0.9f + fbm(u, v, 8, 2, seed) * 0.15f);
    t.roughness = 0.6f;
    return t;
}

Texel benchTexel(float u, float v, std::uint32_t seed) {
    Texel t = woodPiece(u, v, seed, Rgb{0.50f, 0.34f, 0.20f});
    const bool seat = v > 0.38f && v < 0.54f;
    const bool leg = (u < 0.14f || u > 0.86f) && v >= 0.54f;
    t.alpha = (seat || leg) ? 1.0f : 0.0f;
    return t;
}

Texel goldTrim(Texel t) {
    t.color = Rgb{0.95f, 0.78f, 0.28f};
    t.metal = 1.0f;
    t.roughness = 0.3f;
    return t;
}

Texel altarTexel(float u, float v, std::uint32_t seed, bool top) {
    Texel t = stoneTexel(u, v, seed, Rgb{0.88f, 0.86f, 0.82f});
    if (top) {
        if (std::abs(u - 0.5f) < 0.22f) t = clothTexel(u, v, seed, Rgb{0.65f, 0.12f, 0.14f});
        if (std::abs(std::abs(u - 0.5f) - 0.22f) < 0.03f) t = goldTrim(t);
        return t;
    }
    if (v < 0.12f || std::abs(v - 0.9f) < 0.04f) t = goldTrim(t);
    if (std::abs(u - 0.5f) < 0.04f && v > 0.3f && v < 0.75f) t = goldTrim(t);  // cruz / simbolo
    if (std::abs(v - 0.45f) < 0.04f && std::abs(u - 0.5f) < 0.16f) t = goldTrim(t);
    return t;
}

Texel throneTexel(float u, float v, std::uint32_t seed, bool top) {
    Texel t = clothTexel(u, v, seed, Rgb{0.62f, 0.10f, 0.14f});
    if (inFrame(u, v, top ? 0.12f : 0.14f)) t = goldTrim(t);
    if (!top && std::abs(u - 0.5f) < 0.06f && v < 0.35f) t = goldTrim(t);
    return t;
}

Texel bannerTexel(float u, float v, std::uint32_t seed, Rgb tone) {
    Texel t = clothTexel(u, v, seed, tone);
    if (v < 0.08f || v > 0.92f) t.color = tone * 0.6f;
    const float d = std::abs(u - 0.5f) + std::abs(v - 0.45f) * 0.8f;
    if (d < 0.2f && d > 0.12f) t = goldTrim(t);
    if (d < 0.07f) t = goldTrim(t);
    return t;
}

Texel goldTexel(float u, float v, std::uint32_t seed) {
    Texel t = plainTexel(u, v, seed, Rgb{0.98f, 0.80f, 0.30f}, 0.12f);
    if (inFrame(u, v, 0.06f)) t.color = t.color * 0.8f;
    t.metal = 1.0f;
    t.roughness = 0.25f;
    return t;
}

Texel weaponRackTexel(float u, float v, std::uint32_t seed) {
    Texel t = woodPiece(u, v, seed, Rgb{0.36f, 0.24f, 0.14f});
    const bool board = v > 0.85f || v < 0.1f;
    t.alpha = board ? 1.0f : 0.0f;
    // Tres espadas verticales.
    for (int i = 0; i < 3; ++i) {
        const float cx = 0.22f + 0.28f * static_cast<float>(i);
        if (std::abs(u - cx) < 0.035f && v > 0.12f && v < 0.68f) {
            t.alpha = 1.0f;
            t.color = Rgb{0.78f, 0.80f, 0.84f};
            t.metal = 1.0f;
            t.roughness = 0.25f;
        } else if (std::abs(u - cx) < 0.09f && std::abs(v - 0.7f) < 0.025f) {
            t.alpha = 1.0f;
            t.color = Rgb{0.6f, 0.48f, 0.2f};
        } else if (std::abs(u - cx) < 0.025f && v >= 0.72f && v < 0.85f) {
            t.alpha = 1.0f;
            t.color = Rgb{0.3f, 0.18f, 0.1f};
        }
    }
    return t;
}

Texel poolWaterTexel(float u, float v, std::uint32_t seed) {
    const float ripple = fbm(u, v, 4, 3, seed) * 0.5f + 0.5f;
    Texel t;
    t.color = mix(Rgb{0.18f, 0.45f, 0.75f}, Rgb{0.35f, 0.70f, 0.90f}, ripple);
    t.height = 0.5f + ripple * 0.1f;
    t.roughness = 0.08f;
    t.reflectance = 0.6f;
    return t;
}

Texel dungeonBricksTexel(float u, float v, std::uint32_t seed, int variant) {
    Texel t = bricksTexel(u, v, seed, 3, Rgb{0.34f, 0.33f, 0.37f}, Rgb{0.17f, 0.16f, 0.18f});
    if (variant == 1) {  // musgo
        const float m = fbm(u, v, 4, 4, seed + 3u);
        if (m > 0.05f) t.color = mix(t.color, Rgb{0.26f, 0.44f, 0.18f} * (0.8f + m), std::clamp(m * 4.0f, 0.0f, 1.0f));
    } else if (variant == 2) {  // grietas
        const Cell c = voronoi(u, v, 3, seed + 7u, 1.0f);
        const float crack = 1.0f - smooth(0.0f, 0.025f, c.f2 - c.f1);
        t.color = mix(t.color, Rgb{0.08f, 0.08f, 0.09f}, crack * 0.9f);
        t.height -= crack * 0.3f;
    }
    return t;
}

Texel carvedStoneTexel(float u, float v, std::uint32_t seed, bool top) {
    Texel t = stoneTexel(u, v, seed, Rgb{0.56f, 0.54f, 0.50f});
    const float b = std::min(std::min(u, 1.0f - u), std::min(v, 1.0f - v));
    const bool groove = (b > 0.08f && b < 0.12f) || (!top && std::abs(std::abs(u - 0.5f) + std::abs(v - 0.5f) - 0.2f) < 0.025f);
    if (groove) {
        t.color = t.color * 0.6f;
        t.height -= 0.25f;
    }
    return t;
}

Texel ironBarsTexel(float u, float v, std::uint32_t seed) {
    Texel t = plainTexel(u, v, seed, Rgb{0.30f, 0.30f, 0.32f}, 0.1f);
    t.metal = 0.8f;
    t.roughness = 0.5f;
    const bool bar = std::abs(std::fmod(u * 4.0f, 1.0f) - 0.5f) < 0.12f;
    const bool rail = std::abs(v - 0.12f) < 0.04f || std::abs(v - 0.88f) < 0.04f;
    t.alpha = (bar || rail) ? 1.0f : 0.0f;
    return t;
}

Texel spikesTexel(float u, float v, std::uint32_t seed) {
    Texel t = plainTexel(u, v, seed, Rgb{0.62f, 0.62f, 0.66f}, 0.1f);
    t.metal = 0.9f;
    t.roughness = 0.35f;
    const float cell = std::fmod(u * 4.0f, 1.0f);
    const float h = 0.25f + rand01(static_cast<int>(u * 4.0f), 0, seed) * 0.25f;  // la punta (v)
    const float k = (v - h) / (1.0f - h);
    t.alpha = (k > 0.0f && std::abs(cell - 0.5f) < k * 0.42f) ? 1.0f : 0.0f;
    if (v > 0.9f) t.alpha = 1.0f;
    return t;
}

Texel bonesTexel(float u, float v, std::uint32_t seed) {
    Texel t;
    t.alpha = 0.0f;
    t.color = Rgb{0.90f, 0.88f, 0.80f};
    t.roughness = 0.7f;
    // Calavera y dos huesos en el suelo.
    if (std::hypot(u - 0.45f, (v - 0.72f) * 1.1f) < 0.16f && !(std::hypot(u - 0.39f, v - 0.71f) < 0.04f || std::hypot(u - 0.51f, v - 0.71f) < 0.04f)) t.alpha = 1.0f;
    if (v > 0.84f && v < 0.92f && u > 0.1f && u < 0.9f) t.alpha = 1.0f;
    if (std::abs((u - 0.2f) - (v - 0.6f) * 0.8f) < 0.04f && v > 0.6f && v < 0.95f) t.alpha = 1.0f;
    (void)seed;
    return t;
}

Texel cobwebTexel(float u, float v, std::uint32_t seed) {
    Texel t;
    t.alpha = 0.0f;
    t.color = Rgb{0.92f, 0.92f, 0.95f};
    const float dx = u - 0.5f, dy = v - 0.5f;
    const float r = std::hypot(dx, dy);
    const float a = std::atan2(dy, dx);
    const bool spoke = std::abs(std::sin(a * 4.0f)) < 0.06f / std::max(r, 0.05f) * 0.3f;
    const bool ring = std::abs(std::fmod(r * 9.0f + fbm(u, v, 3, 2, seed) * 0.3f, 1.0f) - 0.5f) < 0.06f;
    if (r < 0.5f && (spoke || ring)) t.alpha = 1.0f;
    return t;
}

Texel coffinTexel(float u, float v, std::uint32_t seed, bool top) {
    Texel t = carvedStoneTexel(u, v, seed, true);
    if (top) {
        if (std::abs(u - 0.5f) < 0.05f && v > 0.15f && v < 0.85f) t.color = t.color * 0.65f;
        if (std::abs(v - 0.35f) < 0.05f && std::abs(u - 0.5f) < 0.2f) t.color = t.color * 0.65f;
    } else if (std::abs(v - 0.25f) < 0.04f) {
        t.color = t.color * 0.6f;
    }
    return t;
}

Texel runeStoneTexel(float u, float v, std::uint32_t seed) {
    Texel t = stoneTexel(u, v, seed, Rgb{0.20f, 0.17f, 0.26f});
    const Cell c = voronoi(u, v, 3, seed + 5u, 0.6f);
    const float lx = u * 3.0f - c.cx, ly = v * 3.0f - c.cy;
    const bool rune = idRand(c.id, 1) > 0.3f && ((std::abs(lx) < 0.05f && std::abs(ly) < 0.25f) ||
                                                 (std::abs(ly - lx * (idRand(c.id, 2) - 0.5f) * 2.0f) < 0.05f && std::abs(lx) < 0.2f));
    if (rune) {
        t.color = Rgb{0.35f, 0.85f, 1.0f};
        t.emission = 1.0f;
    }
    return t;
}

Texel candlesTexel(float u, float v, std::uint32_t seed) {
    Texel t;
    t.alpha = 0.0f;
    static constexpr float kCandles[3][2] = {{0.32f, 0.55f}, {0.5f, 0.42f}, {0.68f, 0.6f}};  // (u, v de arriba)
    for (const auto& c : kCandles) {
        if (std::abs(u - c[0]) < 0.04f && v > c[1] && v < 0.97f) {
            t.alpha = 1.0f;
            t.color = Rgb{0.95f, 0.92f, 0.82f};
        }
        const float fy = (v - (c[1] - 0.06f)) / 0.06f;
        if (std::abs(fy) < 1.0f && std::abs(u - c[0]) < 0.03f * (1.0f - fy * fy * 0.5f)) {
            t.alpha = 1.0f;
            t.color = Rgb{1.0f, 0.82f, 0.35f};
            t.emission = 1.0f;
        }
    }
    (void)seed;
    return t;
}

Texel lanternTexel(float u, float v, std::uint32_t seed, bool top) {
    Texel t = plainTexel(u, v, seed, Rgb{0.18f, 0.17f, 0.17f}, 0.1f);
    t.metal = 0.7f;
    t.roughness = 0.5f;
    if (top) {
        t.alpha = (std::abs(u - 0.5f) < 0.3f && std::abs(v - 0.5f) < 0.3f) ? 1.0f : 0.0f;
        return t;
    }
    const float half = v < 0.2f ? 0.18f : 0.3f;
    t.alpha = (std::abs(u - 0.5f) < half && v > 0.06f && v < 0.94f) ? 1.0f : 0.0f;
    const bool frame = std::abs(u - 0.5f) > half - 0.06f || v < 0.26f || v > 0.86f || std::abs(u - 0.5f) < 0.025f;
    if (t.alpha > 0.0f && !frame) {
        t.color = mix(Rgb{1.0f, 0.75f, 0.3f}, Rgb{1.0f, 0.95f, 0.7f}, 1.0f - std::abs(u - 0.5f) * 3.0f);
        t.emission = 1.0f;
        t.metal = 0.0f;
    }
    return t;
}

Texel brazierTexel(float u, float v, std::uint32_t seed, bool top) {
    if (top) {
        Texel t = plainTexel(u, v, seed, Rgb{0.2f, 0.19f, 0.19f}, 0.1f);
        if (std::hypot(u - 0.5f, v - 0.5f) < 0.36f) {
            t.color = mix(Rgb{0.95f, 0.3f, 0.05f}, Rgb{1.0f, 0.85f, 0.4f}, fbm01(u, v, 5, 2, seed));
            t.emission = 1.0f;
        }
        return t;
    }
    Texel t = flameTexel(u, v, seed, 0.55f, 0.45f);
    if (v >= 0.55f && v < 0.75f && std::abs(u - 0.5f) < 0.42f - (v - 0.55f)) {  // cuenco
        t = plainTexel(u, v, seed, Rgb{0.22f, 0.21f, 0.21f}, 0.1f);
        t.metal = 0.7f;
        t.alpha = 1.0f;
    } else if (v >= 0.75f && (std::abs(u - 0.5f) < 0.05f || (v > 0.92f && std::abs(u - 0.5f) < 0.25f))) {  // pie
        t = plainTexel(u, v, seed, Rgb{0.22f, 0.21f, 0.21f}, 0.1f);
        t.metal = 0.7f;
        t.alpha = 1.0f;
    }
    return t;
}

Texel fenceTexel(float u, float v, std::uint32_t seed, bool top) {
    Texel t = woodPiece(u, v, seed, Rgb{0.55f, 0.40f, 0.24f});
    if (top) {
        t.alpha = (std::abs(u - 0.5f) < 0.14f && std::abs(v - 0.5f) < 0.14f) ? 1.0f : 0.0f;
        return t;
    }
    const bool post = std::abs(u - 0.5f) < 0.14f;
    const bool rail = std::abs(v - 0.3f) < 0.07f || std::abs(v - 0.65f) < 0.07f;
    t.alpha = (post || rail) ? 1.0f : 0.0f;
    return t;
}

Texel doorTexel(float u, float v, std::uint32_t seed) {
    Texel t = woodPiece(v, u, seed, Rgb{0.48f, 0.31f, 0.16f});  // tablas verticales
    if (inFrame(u, v, 0.07f)) t.color = Rgb{0.30f, 0.19f, 0.10f};
    if (std::abs(v - 0.25f) < 0.04f || std::abs(v - 0.75f) < 0.04f) {
        t.color = Rgb{0.22f, 0.22f, 0.24f};
        t.metal = 0.7f;
    }
    if (std::hypot(u - 0.78f, v - 0.52f) < 0.05f) t = goldTrim(t);
    return t;
}

// Mitad de arriba de la puerta (2 de alto; la de abajo es "door"): sin el
// pomo y con un ventanuco, para que no se vea la misma puerta dos veces.
Texel doorTopTexel(float u, float v, std::uint32_t seed) {
    Texel t = woodPiece(v, u, seed, Rgb{0.48f, 0.31f, 0.16f});  // tablas verticales
    if (inFrame(u, v, 0.07f)) t.color = Rgb{0.30f, 0.19f, 0.10f};
    if (std::abs(v - 0.55f) < 0.04f) {
        t.color = Rgb{0.22f, 0.22f, 0.24f};
        t.metal = 0.7f;
    }
    if (std::abs(u - 0.5f) < 0.17f && v > 0.14f && v < 0.36f) {  // ventanuco con luz calida
        t.color = std::abs(u - 0.5f) < 0.03f ? Rgb{0.22f, 0.22f, 0.24f} : Rgb{0.95f, 0.75f, 0.40f};
        t.emission = std::abs(u - 0.5f) < 0.03f ? 0.0f : 0.25f;
    }
    return t;
}

Texel hayTexel(float u, float v, std::uint32_t seed, bool top) {
    Texel t = top ? strawTexel(v, u, seed, Rgb{0.86f, 0.72f, 0.32f}) : strawTexel(u, v, seed, Rgb{0.86f, 0.72f, 0.32f});
    if (!top && (std::abs(v - 0.25f) < 0.04f || std::abs(v - 0.75f) < 0.04f)) t.color = Rgb{0.55f, 0.22f, 0.12f};
    return t;
}

Recipe structureRecipe(const std::string& name, std::uint32_t seed) {
    using R = Rgb;
    const auto make = [](auto f) { return Recipe(f); };
    if (name == "plaster") return make([=](float u, float v) { return plainTexel(u, v, seed, R{0.93f, 0.89f, 0.80f}, 0.05f); });
    if (name == "timber") return make([=](float u, float v) { return logBarkTexel(u, v, seed, R{0.32f, 0.21f, 0.13f}, 10.0f); });
    if (name == "timber_top") return make([=](float u, float v) { return logTopTexel(u, v, seed, R{0.45f, 0.31f, 0.18f}, R{0.30f, 0.20f, 0.12f}); });
    if (name == "dark_planks") return make([=](float u, float v) { return planksTexel(u, v, seed, R{0.36f, 0.24f, 0.15f}); });
    if (name == "spruce_planks") return make([=](float u, float v) { return planksTexel(u, v, seed, R{0.50f, 0.35f, 0.21f}); });
    if (name == "roof_red") return make([=](float u, float v) { return roofTexel(u, v, seed, R{0.78f, 0.22f, 0.17f}); });
    if (name == "roof_blue") return make([=](float u, float v) { return roofTexel(u, v, seed, R{0.20f, 0.38f, 0.72f}); });
    if (name == "roof_green") return make([=](float u, float v) { return roofTexel(u, v, seed, R{0.15f, 0.58f, 0.52f}); });
    if (name == "roof_orange") return make([=](float u, float v) { return roofTexel(u, v, seed, R{0.90f, 0.50f, 0.18f}); });
    if (name == "roof_purple") return make([=](float u, float v) { return roofTexel(u, v, seed, R{0.48f, 0.26f, 0.68f}); });
    if (name == "roof_slate") return make([=](float u, float v) { return roofTexel(u, v, seed, R{0.30f, 0.34f, 0.42f}); });
    if (name == "thatch") return make([=](float u, float v) { return strawTexel(u, v, seed, R{0.78f, 0.63f, 0.34f}); });
    if (name == "mud_bricks") return make([=](float u, float v) { return bricksTexel(u, v, seed, 4, R{0.76f, 0.58f, 0.40f}, R{0.66f, 0.54f, 0.40f}); });
    if (name == "marble") return make([=](float u, float v) { return plainTexel(u, v, seed, R{0.90f, 0.89f, 0.86f}, 0.04f); });
    if (name == "marble_pillar") {
        return make([=](float u, float v) {
            Texel t = plainTexel(u, v, seed, R{0.90f, 0.89f, 0.86f}, 0.04f);
            const float flute = std::sin(u * kTau * 4.0f) * 0.5f + 0.5f;
            t.color = t.color * (0.86f + flute * 0.14f);
            t.height = 0.4f + flute * 0.3f;
            return t;
        });
    }
    if (name == "paving") return make([=](float u, float v) { return bricksTexel(u, v, seed, 2, R{0.64f, 0.63f, 0.60f}, R{0.42f, 0.41f, 0.39f}); });
    if (name == "dirt_path") return make([=](float u, float v) { return sandTexel(u, v, seed, R{0.60f, 0.47f, 0.31f}); });
    if (name == "farmland") {
        return make([=](float u, float v) {
            Texel t = dirtTexel(u, v, seed);
            const float furrow = std::sin(v * kTau * 4.0f) * 0.5f + 0.5f;
            t.color = t.color * (0.6f + furrow * 0.3f);
            return t;
        });
    }
    if (name == "wheat") return make([=](float u, float v) { return tallGrassTexel(u, v, seed, R{0.88f, 0.74f, 0.30f}); });
    if (name == "carrots") {
        return make([=](float u, float v) {
            Texel t = tallGrassTexel(u, v, seed, R{0.30f, 0.62f, 0.20f});
            if (v > 0.86f && std::abs(std::fmod(u * 3.0f, 1.0f) - 0.5f) < 0.12f) {
                t.alpha = 1.0f;
                t.color = R{0.95f, 0.50f, 0.12f};
            }
            return t;
        });
    }
    if (name == "hay_top") return make([=](float u, float v) { return hayTexel(u, v, seed, true); });
    if (name == "hay_side") return make([=](float u, float v) { return hayTexel(u, v, seed, false); });
    if (name == "fence") return make([=](float u, float v) { return fenceTexel(u, v, seed, false); });
    if (name == "fence_top") return make([=](float u, float v) { return fenceTexel(u, v, seed, true); });
    if (name == "door") return make([=](float u, float v) { return doorTexel(u, v, seed); });
    if (name == "door_top") return make([=](float u, float v) { return doorTopTexel(u, v, seed); });
    if (name == "lantern") return make([=](float u, float v) { return lanternTexel(u, v, seed, false); });
    if (name == "lantern_top") return make([=](float u, float v) { return lanternTexel(u, v, seed, true); });
    if (name == "brazier") return make([=](float u, float v) { return brazierTexel(u, v, seed, false); });
    if (name == "brazier_top") return make([=](float u, float v) { return brazierTexel(u, v, seed, true); });
    if (name == "fire") return make([=](float u, float v) { return flameTexel(u, v, seed, 1.0f, 0.85f); });
    if (name == "table_top") return make([=](float u, float v) { return planksTexel(u, v, seed, R{0.62f, 0.44f, 0.26f}); });
    if (name == "table_side") return make([=](float u, float v) { return tableSideTexel(u, v, seed, R{0.58f, 0.40f, 0.24f}, 0.16f, 0.14f); });
    if (name == "chair") return make([=](float u, float v) { return chairTexel(u, v, seed); });
    if (name == "chair_top") {
        return make([=](float u, float v) {
            Texel t = planksTexel(u, v, seed, R{0.55f, 0.38f, 0.22f});
            t.alpha = inFrame(u, v, 0.08f) || (u > 0.15f && u < 0.85f && v > 0.15f && v < 0.85f) ? 1.0f : 0.0f;
            return t;
        });
    }
    if (name == "bed_top") return make([=](float u, float v) { return bedTopTexel(u, v, seed); });
    if (name == "bed_side") return make([=](float u, float v) { return bedSideTexel(u, v, seed); });
    if (name == "bookshelf") return make([=](float u, float v) { return bookshelfTexel(u, v, seed); });
    if (name == "barrel") return make([=](float u, float v) { return barrelTexel(u, v, seed); });
    if (name == "barrel_top") return make([=](float u, float v) { return barrelTopTexel(u, v, seed); });
    if (name == "crate") return make([=](float u, float v) { return crateTexel(u, v, seed); });
    if (name == "chest") return make([=](float u, float v) { return chestTexel(u, v, seed, false); });
    if (name == "chest_top") return make([=](float u, float v) { return chestTexel(u, v, seed, true); });
    if (name == "counter") return make([=](float u, float v) { return counterTexel(u, v, seed); });
    if (name == "counter_top") return make([=](float u, float v) { return planksTexel(u, v, seed, R{0.52f, 0.34f, 0.19f}); });
    if (name == "shelf") return make([=](float u, float v) { return shelfTexel(u, v, seed); });
    if (name == "anvil") return make([=](float u, float v) { return anvilTexel(u, v, seed, false); });
    if (name == "anvil_top") return make([=](float u, float v) { return anvilTexel(u, v, seed, true); });
    if (name == "forge") return make([=](float u, float v) { return forgeTexel(u, v, seed, false); });
    if (name == "forge_top") return make([=](float u, float v) { return forgeTexel(u, v, seed, true); });
    if (name == "oven") return make([=](float u, float v) { return ovenTexel(u, v, seed); });
    if (name == "rug_red") return make([=](float u, float v) { return rugTexel(u, v, seed, R{0.66f, 0.14f, 0.14f}, R{0.92f, 0.76f, 0.30f}); });
    if (name == "rug_blue") return make([=](float u, float v) { return rugTexel(u, v, seed, R{0.18f, 0.28f, 0.62f}, R{0.90f, 0.86f, 0.70f}); });
    if (name == "painting") return make([=](float u, float v) { return paintingTexel(u, v, seed); });
    if (name == "bench") return make([=](float u, float v) { return benchTexel(u, v, seed); });
    if (name == "bench_top") return make([=](float u, float v) { return planksTexel(v, u, seed, R{0.50f, 0.34f, 0.20f}); });
    if (name == "altar") return make([=](float u, float v) { return altarTexel(u, v, seed, false); });
    if (name == "altar_top") return make([=](float u, float v) { return altarTexel(u, v, seed, true); });
    if (name == "throne") return make([=](float u, float v) { return throneTexel(u, v, seed, false); });
    if (name == "throne_top") return make([=](float u, float v) { return throneTexel(u, v, seed, true); });
    if (name == "banner_red") return make([=](float u, float v) { return bannerTexel(u, v, seed, R{0.70f, 0.12f, 0.12f}); });
    if (name == "banner_blue") return make([=](float u, float v) { return bannerTexel(u, v, seed, R{0.14f, 0.24f, 0.62f}); });
    if (name == "cloth_red") return make([=](float u, float v) { return clothTexel(u, v, seed, R{0.76f, 0.16f, 0.14f}); });
    if (name == "cloth_blue") return make([=](float u, float v) { return clothTexel(u, v, seed, R{0.18f, 0.34f, 0.72f}); });
    if (name == "cloth_yellow") return make([=](float u, float v) { return clothTexel(u, v, seed, R{0.95f, 0.78f, 0.22f}); });
    if (name == "cloth_white") return make([=](float u, float v) { return clothTexel(u, v, seed, R{0.94f, 0.93f, 0.89f}); });
    if (name == "gold_block") return make([=](float u, float v) { return goldTexel(u, v, seed); });
    if (name == "weapon_rack") return make([=](float u, float v) { return weaponRackTexel(u, v, seed); });
    if (name == "pool_water") return make([=](float u, float v) { return poolWaterTexel(u, v, seed); });
    if (name == "dungeon_bricks") return make([=](float u, float v) { return dungeonBricksTexel(u, v, seed, 0); });
    if (name == "mossy_bricks") return make([=](float u, float v) { return dungeonBricksTexel(u, v, seed, 1); });
    if (name == "cracked_bricks") return make([=](float u, float v) { return dungeonBricksTexel(u, v, seed, 2); });
    if (name == "carved_stone") return make([=](float u, float v) { return carvedStoneTexel(u, v, seed, false); });
    if (name == "carved_stone_top") return make([=](float u, float v) { return carvedStoneTexel(u, v, seed, true); });
    if (name == "iron_bars") return make([=](float u, float v) { return ironBarsTexel(u, v, seed); });
    if (name == "spikes") return make([=](float u, float v) { return spikesTexel(u, v, seed); });
    if (name == "bones") return make([=](float u, float v) { return bonesTexel(u, v, seed); });
    if (name == "cobweb") return make([=](float u, float v) { return cobwebTexel(u, v, seed); });
    if (name == "coffin") return make([=](float u, float v) { return coffinTexel(u, v, seed, false); });
    if (name == "coffin_top") return make([=](float u, float v) { return coffinTexel(u, v, seed, true); });
    if (name == "rune_stone") return make([=](float u, float v) { return runeStoneTexel(u, v, seed); });
    if (name == "candles") return make([=](float u, float v) { return candlesTexel(u, v, seed); });
    return {};
}

// Texturas de muebles y adornos que conservan su dibujo en el estilo de color
// liso (una estanteria lisa seria una mancha parda).
bool structureKeepsPattern(const std::string& name) {
    static const char* const kNames[] = {"bookshelf", "painting", "banner_red", "banner_blue", "chest", "chest_top",
                                         "barrel", "door", "door_top", "bed_top", "bed_side", "shelf", "weapon_rack", "oven",
                                         "forge", "forge_top", "lantern", "brazier", "brazier_top", "rune_stone",
                                         "altar", "altar_top", "throne", "throne_top", "rug_red", "rug_blue",
                                         "hay_side", "crate", "counter"};
    for (const char* n : kNames) {
        if (name == n) return true;
    }
    return false;
}

// --- Tierras salvajes del Aventura (GeneratorWild.cpp) ----------------------------------

// Arenisca por capas del color dado (como "sandstone").
Texel bandedSandstoneTexel(float u, float v, std::uint32_t seed, Rgb tone) {
    Texel t = sandTexel(u, v, seed, tone);
    const float bands = std::sin((v + fbm(u, v, 2, 2, seed + 3u) * 0.05f) * kTau * 3.0f);
    t.color = t.color * (0.92f + bands * 0.07f);
    t.height += bands * 0.08f;
    t.roughness = 0.88f;
    return t;
}

Recipe wildRecipe(const std::string& name, std::uint32_t seed) {
    if (name == "red_sandstone") return [=](float u, float v) { return bandedSandstoneTexel(u, v, seed, Rgb{0.72f, 0.36f, 0.20f}); };
    if (name == "red_sandstone_top") return [=](float u, float v) { return sandTexel(u, v, seed, Rgb{0.76f, 0.40f, 0.23f}); };
    if (name == "pink_sand") return [=](float u, float v) { return sandTexel(u, v, seed, Rgb{0.94f, 0.66f, 0.72f}); };
    if (name == "pink_sandstone") return [=](float u, float v) { return bandedSandstoneTexel(u, v, seed, Rgb{0.86f, 0.56f, 0.63f}); };
    if (name == "pink_sandstone_top") return [=](float u, float v) { return sandTexel(u, v, seed, Rgb{0.90f, 0.62f, 0.68f}); };
    if (name == "rose_quartz") {
        return [=](float u, float v) {
            Texel t = crystalTexel(u, v, seed, Rgb{0.95f, 0.60f, 0.74f}, 0.0f);
            t.roughness = 0.3f;
            return t;
        };
    }
    if (name == "blight_grass_top") {
        return [=](float u, float v) {
            Texel t = grassTopTexel(u, v, seed);
            const float k = (t.color.r + t.color.g + t.color.b) / 3.0f;
            t.color = Rgb{0.28f + k * 0.42f, 0.20f + k * 0.28f, 0.30f + k * 0.46f};  // violeta grisaceo enfermizo
            return t;
        };
    }
    if (name == "blight_grass_side") {
        return [=](float u, float v) {
            Texel t = dirtTexel(u, v, seed);
            t.color = Rgb{t.color.r * 0.6f, t.color.g * 0.55f, t.color.b * 1.1f + 0.08f};  // como dusk_soil
            if (v < 0.22f + fbm(u, 0.0f, 8, 2, seed) * 0.07f) t.color = Rgb{0.42f, 0.22f, 0.48f} * (0.9f + fbm(u, v, 12, 2, seed + 2u) * 0.1f);
            return t;
        };
    }
    if (name == "blight_thorns") {
        return [=](float u, float v) {
            Texel t = deadBushTexel(u, v, seed);
            if (t.alpha > 0.5f) t.color = Rgb{0.30f, 0.14f, 0.34f} * (0.8f + t.color.r * 0.6f);
            return t;
        };
    }
    if (name == "brimstone") {
        return [=](float u, float v) {
            Texel t = pebblesTexel(u, v, seed, 7);
            const float k = (t.color.r + t.color.g + t.color.b) / 3.0f;
            t.color = Rgb{0.70f + k * 0.3f, 0.60f + k * 0.25f, 0.12f + k * 0.1f};
            t.emission = 0.08f;
            return t;
        };
    }
    return {};
}

// --- Luces (Blocks.cpp addLightBlocks; piezas finas en Mesher.cpp) --------------------------
// Las texturas de las piezas van por franjas horizontales: la fila (v * 16)
// es la altura de la pieza (v = 1 - y), las columnas casi no importan.

Texel ironTexel(float u, float v, std::uint32_t seed, Rgb tone = Rgb{0.17f, 0.17f, 0.18f}) {
    Texel t = plainTexel(u, v, seed, tone, 0.12f);
    t.metal = 0.75f;
    t.roughness = 0.45f;
    return t;
}

Texel stickTexel(float u, float v, std::uint32_t seed) {
    Texel t;
    t.color = Rgb{0.42f, 0.29f, 0.16f} * (0.85f + fbm(u, v, 16, 2, seed) * 0.2f);
    t.roughness = 0.8f;
    return t;
}

// Llama por filas: punta clara arriba (k = 0) y naranja hacia la base (k = 1).
Texel flameBand(float k) {
    Texel t;
    t.color = mix(Rgb{1.0f, 0.95f, 0.62f}, Rgb{1.0f, 0.55f, 0.12f}, std::clamp(k, 0.0f, 1.0f));
    t.emission = 1.0f;
    t.roughness = 1.0f;
    return t;
}

// Cristal que brilla dentro de un marco de hierro (lamparas).
Texel lampPaneTexel(float u, float v, std::uint32_t seed, Rgb glow) {
    if (inFrame(u, v, 0.09f) || std::abs(u - 0.5f) < 0.035f || std::abs(v - 0.5f) < 0.035f) return ironTexel(u, v, seed);
    Texel t;
    const float r = std::hypot(u - 0.5f, v - 0.5f);
    const float n = fbm(u, v, 6, 3, seed);
    t.color = mix(glow, Rgb{1.0f, 1.0f, 0.95f}, std::clamp(0.55f - r, 0.0f, 0.5f)) * (0.92f + n * 0.1f);
    t.emission = 0.8f + std::clamp(0.4f - r, 0.0f, 0.2f);
    t.roughness = 0.2f;
    t.reflectance = 0.8f;
    return t;
}

Recipe lightRecipe(const std::string& name, std::uint32_t seed) {
    const auto row = [](float v) { return v * 16.0f; };  // fila 0 (arriba) .. 16 (abajo)
    if (name == "torch_stick") {
        return [=](float u, float v) {
            const float r = row(v);
            Texel t;
            t.alpha = 0.0f;
            if (r >= 6.0f) return stickTexel(u, v, seed);
            if (r >= 2.0f) return flameBand((r - 2.0f) / 4.0f);
            return t;
        };
    }
    if (name == "torch_flame_top") {
        return [=](float u, float v) {
            Texel t = flameBand(0.25f + fbm(u, v, 4, 2, seed) * 0.3f);
            return t;
        };
    }
    if (name == "standing_torch") {
        return [=](float u, float v) {
            const float r = row(v);
            if (r >= 6.0f) {
                Texel t = stickTexel(u, v, seed);
                t.color = t.color * 0.8f;
                return t;
            }
            if (r >= 4.0f) return ironTexel(u, v, seed);
            return flameBand(r / 4.0f);
        };
    }
    if (name == "lamp_post" || name == "lamp_post_top" || name == "street_lamp_top") {
        return [=](float u, float v) {
            Texel t = ironTexel(u, v, seed);
            if (name == "lamp_post") t.color = t.color * (0.85f + 0.3f * std::max(0.0f, std::cos((u - 0.35f) * kTau)));
            return t;
        };
    }
    if (name == "street_lamp") {
        return [=](float u, float v) {
            const float r = row(v);
            const float c = u * 16.0f;
            const bool glass = r >= 6.0f && r < 11.0f && c >= 6.0f && c < 10.0f;
            if (!glass) return ironTexel(u, v, seed, r >= 3.0f && r < 5.0f ? Rgb{0.12f, 0.16f, 0.14f} : Rgb{0.17f, 0.17f, 0.18f});
            Texel t;
            t.color = mix(Rgb{1.0f, 0.78f, 0.40f}, Rgb{1.0f, 0.96f, 0.75f}, 1.0f - std::abs(u - 0.5f) * 6.0f);
            t.emission = 1.0f;
            t.roughness = 0.2f;
            return t;
        };
    }
    if (name == "campfire_log") {
        return [=](float u, float v) {
            Texel t = logBarkTexel(v, u, seed, Rgb{0.33f, 0.23f, 0.14f}, 8.0f);  // vetas a lo largo (u)
            const float char_spot = fbm(u, v, 5, 2, seed + 9u);
            if (char_spot > 0.25f) t.color = t.color * 0.45f;  // chamuscado
            if (char_spot > 0.55f) {
                t.color = Rgb{1.0f, 0.42f, 0.10f};  // brasas
                t.emission = 0.6f;
            }
            return t;
        };
    }
    if (name == "campfire_fire") return [=](float u, float v) { return flameTexel(u, v, seed, 0.94f, 0.8f); };
    if (name == "crystal_lamp") {
        return [=](float u, float v) {
            if (inFrame(u, v, 0.08f)) return ironTexel(u, v, seed, Rgb{0.55f, 0.45f, 0.22f});
            Texel t = crystalTexel(u, v, seed, Rgb{0.55f, 0.90f, 1.0f}, 1.0f);
            t.emission = std::max(t.emission, 0.7f);
            return t;
        };
    }
    if (name == "lamp_red") return [=](float u, float v) { return lampPaneTexel(u, v, seed, Rgb{0.95f, 0.22f, 0.15f}); };
    if (name == "lamp_blue") return [=](float u, float v) { return lampPaneTexel(u, v, seed, Rgb{0.25f, 0.50f, 1.0f}); };
    if (name == "lamp_green") return [=](float u, float v) { return lampPaneTexel(u, v, seed, Rgb{0.30f, 0.90f, 0.35f}); };
    if (name == "paper_lantern" || name == "paper_lantern_top") {
        return [=](float u, float v) {
            const Rgb wood{0.25f, 0.15f, 0.09f};
            if (name == "paper_lantern_top") return plainTexel(u, v, seed, wood, 0.1f);
            const float r = row(v);
            const float c = u * 16.0f;
            if (r < 4.0f) {  // cordon (solo en el centro: la cadena del farol)
                Texel t = plainTexel(u, v, seed, Rgb{0.45f, 0.08f, 0.06f}, 0.1f);
                t.alpha = c >= 7.0f && c < 9.0f ? 1.0f : 0.0f;
                return t;
            }
            if (r < 6.0f || r >= 14.0f) return plainTexel(u, v, seed, wood, 0.1f);
            Texel t = clothTexel(u, v, seed, Rgb{0.92f, 0.20f, 0.12f});
            const bool rib = (r >= 8.0f && r < 8.6f) || (r >= 11.0f && r < 11.6f) || c < 6.0f || c >= 10.0f;
            t.color = rib ? t.color * 0.7f : mix(t.color, Rgb{1.0f, 0.70f, 0.40f}, 0.25f);
            t.emission = rib ? 0.35f : 0.75f;
            return t;
        };
    }
    if (name == "chandelier") return [=](float u, float v) { return ironTexel(u, v, seed, Rgb{0.62f, 0.48f, 0.22f}); };
    if (name == "chandelier_candle") {
        return [=](float u, float v) {
            const float r = row(v);
            Texel t;
            t.alpha = 0.0f;
            if (r >= 5.0f) {
                t = plainTexel(u, v, seed, Rgb{0.95f, 0.92f, 0.82f}, 0.05f);
                t.roughness = 0.6f;
                return t;
            }
            if (r >= 3.0f) return flameBand((r - 3.0f) / 2.0f);
            return t;
        };
    }
    return {};
}

// Las de las luces conservan su dibujo en el estilo de color liso.
bool lightKeepsPattern(const std::string& name) {
    static const char* const kNames[] = {"torch_stick", "standing_torch", "street_lamp", "campfire_fire", "paper_lantern",
                                         "chandelier_candle", "lamp_red", "lamp_blue", "lamp_green", "crystal_lamp"};
    for (const char* n : kNames) {
        if (name == n) return true;
    }
    return false;
}

// --- Asentamientos y castillos (ids 166..180) -----------------------------------------------

// Calavera de frente en (cu, cv) con radio r (para estandartes y montones).
bool skullMask(float u, float v, float cu, float cv, float r, bool& hole) {
    const float dx = (u - cu) / r, dy = (v - cv) / r;
    const bool head = dx * dx + dy * dy < 1.0f && dy < 0.55f;
    const bool jaw = std::abs(dx) < 0.55f && dy >= 0.4f && dy < 1.05f;
    hole = (std::hypot(dx + 0.38f, dy - 0.05f) < 0.26f) || (std::hypot(dx - 0.38f, dy - 0.05f) < 0.26f) ||
           (std::abs(dx) < 0.1f && dy > 0.35f && dy < 0.55f) || (jaw && dy > 0.75f && std::fmod((dx + 1.0f) * 3.5f, 1.0f) < 0.18f);
    return head || jaw;
}

Texel settlementBannerTexel(float u, float v, std::uint32_t seed, Rgb tone, int emblem) {
    Texel t = clothTexel(u, v, seed, tone);
    if (v < 0.08f || v > 0.92f) t.color = tone * 0.6f;
    if (emblem == 0) {  // calavera de hueso
        bool hole = false;
        if (skullMask(u, v, 0.5f, 0.42f, 0.2f, hole)) {
            t.color = hole ? Rgb{0.05f, 0.05f, 0.06f} : Rgb{0.90f, 0.88f, 0.80f};
            t.height += 0.1f;
        }
    } else if (emblem == 1) {  // ojo del culto
        const float e = std::hypot((u - 0.5f) * 1.0f, (v - 0.45f) * 2.2f);
        if (e < 0.3f && e > 0.2f) t.color = Rgb{0.95f, 0.78f, 0.28f};
        if (std::hypot(u - 0.5f, v - 0.45f) < 0.07f) {
            t.color = Rgb{0.9f, 0.2f, 0.25f};
            t.emission = 0.6f;
        }
    } else {  // hoja de plata
        const float lu = u - 0.5f, lv = v - 0.45f;
        if (std::abs(lu) < 0.16f * (1.0f - std::abs(lv) / 0.28f) && std::abs(lv) < 0.28f) t.color = Rgb{0.86f, 0.90f, 0.86f};
        if (std::abs(lu) < 0.015f && std::abs(lv) < 0.3f) t.color = Rgb{0.6f, 0.7f, 0.6f};
    }
    return t;
}

Recipe settlementRecipe(const std::string& name, std::uint32_t seed) {
    using R = Rgb;
    if (name == "blackstone_bricks") return [=](float u, float v) { return bricksTexel(u, v, seed, 3, R{0.17f, 0.16f, 0.19f}, R{0.08f, 0.07f, 0.09f}); };
    if (name == "cracked_blackstone") {
        return [=](float u, float v) {
            Texel t = bricksTexel(u, v, seed, 3, R{0.19f, 0.17f, 0.20f}, R{0.08f, 0.07f, 0.09f});
            const Cell c = voronoi(u, v, 3, seed + 7u, 1.0f);
            const float crack = 1.0f - smooth(0.0f, 0.025f, c.f2 - c.f1);
            t.color = mix(t.color, R{0.02f, 0.02f, 0.03f}, crack * 0.9f);
            t.height -= crack * 0.3f;
            return t;
        };
    }
    if (name == "banner_skull") return [=](float u, float v) { return settlementBannerTexel(u, v, seed, R{0.10f, 0.09f, 0.10f}, 0); };
    if (name == "banner_purple") return [=](float u, float v) { return settlementBannerTexel(u, v, seed, R{0.36f, 0.12f, 0.48f}, 1); };
    if (name == "banner_green") return [=](float u, float v) { return settlementBannerTexel(u, v, seed, R{0.14f, 0.46f, 0.26f}, 2); };
    if (name == "hide_tent") {
        return [=](float u, float v) {
            // Pieles cosidas: parches pardos de varios tonos con costuras.
            const Cell c = voronoi(u, v, 3, seed + 3u, 0.8f);
            const float k = idRand(c.id, 4);
            Texel t = clothTexel(u, v, seed, mix(R{0.50f, 0.36f, 0.22f}, R{0.66f, 0.52f, 0.34f}, k));
            const float seam = 1.0f - smooth(0.0f, 0.03f, c.f2 - c.f1);
            if (seam > 0.5f) {
                t.color = R{0.25f, 0.17f, 0.10f};
                t.height -= 0.15f;
            }
            return t;
        };
    }
    if (name == "palisade") {
        return [=](float u, float v) {
            Texel t = logBarkTexel(u, v, seed, R{0.40f, 0.29f, 0.18f}, 12.0f);
            if (std::abs(u - 0.5f) > 0.46f) t.color = t.color * 0.6f;  // junta entre troncos
            return t;
        };
    }
    if (name == "palisade_top") return [=](float u, float v) { return logTopTexel(u, v, seed, R{0.62f, 0.47f, 0.30f}, R{0.36f, 0.26f, 0.16f}); };
    if (name == "soul_brazier" || name == "soul_brazier_top") {
        const bool top = name == "soul_brazier_top";
        return [=](float u, float v) {
            Texel t = brazierTexel(u, v, seed, top);
            if (t.emission > 0.0f) {  // llamas verdes
                const float k = (t.color.r + t.color.g + t.color.b) / 3.0f;
                t.color = R{0.25f + k * 0.35f, 0.75f + k * 0.25f, 0.55f + k * 0.2f};
            }
            return t;
        };
    }
    if (name == "gravestone" || name == "gravestone_top") {
        const bool top = name == "gravestone_top";
        return [=](float u, float v) {
            Texel t = stoneTexel(u, v, seed, R{0.52f, 0.52f, 0.54f});
            if (top) {
                t.alpha = std::abs(v - 0.5f) < 0.12f && std::abs(u - 0.5f) < 0.36f ? 1.0f : 0.0f;
                return t;
            }
            // Lapida redondeada sobre un zocalo; una cruz grabada.
            const float dx = u - 0.5f;
            const bool slab = std::abs(dx) < 0.32f && v > 0.22f && (v > 0.4f || std::hypot(dx, (v - 0.4f) * 1.4f) < 0.32f);
            const bool base = v > 0.86f && std::abs(dx) < 0.42f;
            t.alpha = slab || base ? 1.0f : 0.0f;
            if ((std::abs(dx) < 0.035f && v > 0.34f && v < 0.7f) || (std::abs(v - 0.46f) < 0.03f && std::abs(dx) < 0.14f)) {
                t.color = t.color * 0.55f;
                t.height -= 0.25f;
            }
            return t;
        };
    }
    if (name == "skull_pile") {
        return [=](float u, float v) {
            Texel t;
            t.alpha = 0.0f;
            t.color = R{0.90f, 0.87f, 0.78f};
            t.roughness = 0.75f;
            static constexpr float kSkulls[4][3] = {{0.3f, 0.82f, 0.13f}, {0.7f, 0.82f, 0.13f}, {0.5f, 0.6f, 0.14f}, {0.5f, 0.86f, 0.11f}};
            for (const auto& s : kSkulls) {
                bool hole = false;
                if (skullMask(u, v, s[0], s[1], s[2], hole)) {
                    t.alpha = 1.0f;
                    t.color = hole ? R{0.08f, 0.07f, 0.07f} : R{0.90f, 0.87f, 0.78f} * (0.9f + fbm(u, v, 8, 2, seed) * 0.2f);
                }
            }
            return t;
        };
    }
    if (name == "elderwood_planks") return [=](float u, float v) { return planksTexel(u, v, seed, R{0.86f, 0.80f, 0.64f}); };
    return {};
}

bool settlementKeepsPattern(const std::string& name) {
    static const char* const kNames[] = {"banner_skull", "banner_purple", "banner_green", "soul_brazier", "soul_brazier_top",
                                         "gravestone", "hide_tent"};
    for (const char* n : kNames) {
        if (name == n) return true;
    }
    return false;
}

// --- Cuevas inmensas (GeneratorCaves.cpp) ------------------------------------------------

// Punta de piedra (estalactita: ancha arriba; estalagmita: ancha abajo) con anillos.
Texel spikeTexel(float u, float v, std::uint32_t seed, Rgb tone, bool down) {
    Texel t;
    t.alpha = 0.0f;
    t.height = 1.0f;
    t.roughness = 0.6f;
    const float along = down ? v : 1.0f - v;  // 0 en la base, 1 en la punta
    const auto spike = [&](float cx, float base_w, float len) {
        if (along > len) return false;
        const float k = along / len;
        const float w = base_w * (1.0f - k) + 0.012f;
        const float wob = fbm(0.0f, v, 4, 2, seed) * 0.025f;
        return std::abs(u - cx - wob) < w;
    };
    if (spike(0.5f, 0.26f, 0.96f) || spike(0.24f, 0.1f, 0.5f) || spike(0.77f, 0.08f, 0.38f)) {
        t.alpha = 1.0f;
        const float rings = std::sin(along * 38.0f + fbm(u, v, 6, 2, seed + 4u) * 2.0f) * 0.06f;
        t.color = tone * (0.82f + fbm01(u, v, 8, 3, seed + 2u) * 0.28f + rings) * (1.0f - along * 0.18f);
        t.height = 0.7f + rings;
    }
    return t;
}

// Enredadera luminosa: tallos que cuelgan con hojas y bayas que brillan.
// LIANAS: `entera` = tramo con otra liana debajo: hebras de borde a borde (sin puntas). La ondulacion es periodica
// en v (2 vueltas por bloque) para que los tramos apilados encajen.
Texel glowVinesTexel(float u, float v, std::uint32_t seed, bool entera = false) {
    Texel t;
    t.alpha = 0.0f;
    t.height = 1.0f;
    t.roughness = 0.7f;
    for (int i = 0; i < 4; ++i) {
        const float x = 0.14f + 0.24f * static_cast<float>(i) + std::sin(v * 12.566371f + static_cast<float>(i) * 2.1f) * 0.035f;
        const float end = entera ? 2.0f : 0.75f + rand01(i, 3, seed) * 0.25f;
        if (v > end) continue;
        if (std::abs(u - x) < 0.022f) {
            t.alpha = 1.0f;
            t.color = Rgb{0.18f, 0.40f, 0.20f} * (0.85f + fbm(u, v, 12, 2, seed) * 0.2f);
        }
        // Hojas alternas.
        const float leaf_v = std::fmod(v * 7.0f + rand01(i, 4, seed), 1.0f);
        const float side = (static_cast<int>(v * 7.0f + rand01(i, 4, seed)) % 2 == 0) ? 1.0f : -1.0f;
        const float lx = (u - x) * side;
        if (lx > 0.0f && lx < 0.08f && std::abs(leaf_v - 0.5f) < 0.16f * (1.0f - lx / 0.08f)) {
            t.alpha = 1.0f;
            t.color = Rgb{0.22f, 0.55f, 0.26f};
        }
        // Bayas luminosas.
        const float bv = std::fmod(v * 4.0f + rand01(i, 5, seed) * 3.0f, 1.0f);
        const float dx = u - x - side * 0.05f, dy = (bv - 0.5f) / 4.0f;
        if (dx * dx + dy * dy < 0.0011f) {
            t.alpha = 1.0f;
            t.color = Rgb{1.0f, 0.78f, 0.32f};
            t.emission = 1.0f;
        }
    }
    return t;
}

Recipe caveRecipe(const std::string& name, std::uint32_t seed) {
    if (name == "deepstone") {
        return [=](float u, float v) {
            Texel t = stoneTexel(u, v, seed, Rgb{0.27f, 0.27f, 0.31f});
            const float layers = std::sin((v + fbm(u, v, 3, 2, seed + 9u) * 0.06f) * kTau * 4.0f);
            t.color = t.color * (0.94f + layers * 0.06f);
            t.height += layers * 0.05f;
            return t;
        };
    }
    if (name == "dripstone") {
        return [=](float u, float v) {
            Texel t = stoneTexel(u, v, seed, Rgb{0.56f, 0.45f, 0.36f});
            const float streaks = fbm(u * 6.0f, v, 2, 3, seed + 5u);
            t.color = t.color * (0.9f + streaks * 0.12f);
            t.height = 0.5f + streaks * 0.25f;
            t.roughness = 0.65f;
            return t;
        };
    }
    if (name == "stalactite") return [=](float u, float v) { return spikeTexel(u, v, seed, Rgb{0.60f, 0.49f, 0.39f}, true); };
    if (name == "stalagmite") return [=](float u, float v) { return spikeTexel(u, v, seed, Rgb{0.58f, 0.47f, 0.37f}, false); };
    if (name == "glow_vines") return [=](float u, float v) { return glowVinesTexel(u, v, seed); };
    if (name == "glow_vines_low") return [=](float u, float v) { return glowVinesTexel(u, v, seed, true); };  // LIANAS
    return {};
}

// --- Oceanos hondos ---------------------------------------------------------------------

// Kelp: un tallo que ondula (se repite arriba y abajo: las columnas se empalman)
// con hojas alargadas alternas y alguna vejiga.
Texel kelpTexel(float u, float v, std::uint32_t seed) {
    Texel t;
    t.alpha = 0.0f;
    t.height = 1.0f;
    t.roughness = 0.55f;
    const float x = 0.5f + std::sin(v * kTau) * 0.06f;
    if (std::abs(u - x) < 0.035f) {
        t.alpha = 1.0f;
        t.color = Rgb{0.30f, 0.38f, 0.12f} * (0.85f + fbm(u, v, 8, 2, seed) * 0.15f);
    }
    for (int i = 0; i < 4; ++i) {
        const float side = (i % 2 == 0) ? 1.0f : -1.0f;
        const float v0 = 0.125f + 0.25f * static_cast<float>(i);
        const float bx = 0.5f + std::sin(v0 * kTau) * 0.06f;
        // Hoja: una elipse inclinada hacia arriba que sale del tallo.
        const float lx = (u - bx) * side, ly = v - v0 + lx * 0.55f;
        if (lx > 0.0f && lx < 0.36f) {
            const float w = 0.075f * std::sin(lx / 0.36f * 3.14159f);
            if (std::abs(ly) < w) {
                t.alpha = 1.0f;
                t.color = Rgb{0.36f, 0.50f, 0.16f} * (0.8f + 0.3f * (1.0f - std::abs(ly) / std::max(w, 1e-4f)));
            }
        }
        const float dx = u - bx - side * 0.05f, dy = v - v0 - 0.02f;
        if (dx * dx + dy * dy < 0.0012f) {
            t.alpha = 1.0f;
            t.color = Rgb{0.55f, 0.52f, 0.20f};
        }
    }
    return t;
}

// Abanico de coral: ramas que salen de la base y se abren hacia arriba.
Texel coralFanTexel(float u, float v, std::uint32_t seed, Rgb tone) {
    Texel t;
    t.alpha = 0.0f;
    t.height = 1.0f;
    t.roughness = 0.6f;
    const float dx = u - 0.5f, dy = 0.97f - v;
    if (dy < 0.0f) return t;
    const float r = std::hypot(dx, dy);
    const float a = std::atan2(dx, dy);  // 0 hacia arriba
    if (r > 0.82f || std::abs(a) > 1.05f) return t;
    const float ribs = std::abs(std::sin(a * 9.0f + fbm(u, v, 4, 2, seed) * 0.6f));
    const float mesh = std::abs(std::sin(r * 38.0f));
    const bool rib = ribs < 0.16f + r * 0.05f || r < 0.08f;
    const bool web = r > 0.2f && mesh < 0.10f && r < 0.8f;
    const float edge = 0.62f + fbm(u, v, 6, 2, seed + 3u) * 0.16f;
    if ((rib && r < edge + 0.12f) || (web && r < edge)) {
        t.alpha = 1.0f;
        t.color = tone * (0.75f + 0.35f * r);
    }
    return t;
}

// Bloque de coral: superficie de poros (celdas) con bordes claros.
Texel coralTexel(float u, float v, std::uint32_t seed, Rgb tone, int cells) {
    const Cell c = voronoi(u, v, cells, seed, 0.8f);
    const float pore = smooth(0.32f, 0.12f, c.f1);
    const float rim = 1.0f - smooth(0.0f, 0.06f, c.f2 - c.f1);
    Texel t;
    t.color = mix(tone * (0.92f + idRand(c.id, 2) * 0.14f), tone * 0.55f, pore) + Rgb{0.06f, 0.05f, 0.05f} * rim;
    t.height = 0.65f - pore * 0.45f + rim * 0.1f;
    t.roughness = 0.8f;
    return t;
}

Recipe oceanRecipe(const std::string& name, std::uint32_t seed) {
    if (name == "kelp") return [=](float u, float v) { return kelpTexel(u, v, seed); };
    if (name == "sea_grass") return [=](float u, float v) { return tallGrassTexel(u, v, seed, Rgb{0.26f, 0.55f, 0.30f}); };
    if (name == "coral_block") return [=](float u, float v) { return coralTexel(u, v, seed, Rgb{0.90f, 0.42f, 0.58f}, 7); };
    if (name == "coral_tube") return [=](float u, float v) { return coralTexel(u, v, seed, Rgb{0.95f, 0.58f, 0.20f}, 4); };
    if (name == "coral_fan") return [=](float u, float v) { return coralFanTexel(u, v, seed, Rgb{0.62f, 0.30f, 0.85f}); };
    if (name == "sea_silt") {
        return [=](float u, float v) {
            Texel t = sandTexel(u, v, seed, Rgb{0.26f, 0.29f, 0.32f});
            t.roughness = 0.95f;
            return t;
        };
    }
    if (name == "sea_glow") {
        return [=](float u, float v) {
            // Anemona: tentaculos cortos con las puntas que brillan.
            Texel t = tallGrassTexel(u, std::clamp(v * 1.6f - 0.6f, 0.0f, 1.0f), seed, Rgb{0.30f, 0.80f, 0.85f});
            if (v < 0.375f) t.alpha = 0.0f;
            if (t.alpha > 0.5f) t.emission = v < 0.6f ? 1.0f : 0.45f;
            return t;
        };
    }
    return {};
}

// --- Rieles y senales de arcanita (ids 211..230) ----------------------------------------------
// Las piezas finas leen la textura por la posicion: las traviesas y los carriles
// son casi lisos (madera oscura, hierro); la arcanita es un mineral turquesa que
// brilla cuando lleva senal.

const Rgb kArcanite{0.24f, 0.92f, 0.84f};

Texel arcaniteDust(float u, float v, std::uint32_t seed, bool lit) {
    Texel t;
    const float grain = fbm(u, v, 24, 2, seed);
    const float spark = rand01(static_cast<int>(u * 64.0f), static_cast<int>(v * 64.0f), seed + 3u);
    if (lit) {
        t.color = mix(kArcanite, Rgb{0.85f, 1.0f, 0.98f}, std::clamp(spark - 0.7f, 0.0f, 0.3f) * 2.0f) * (0.9f + grain * 0.2f);
        t.emission = 0.75f + (spark > 0.85f ? 0.25f : 0.0f);
    } else {
        t.color = Rgb{0.13f, 0.33f, 0.34f} * (0.8f + grain * 0.35f);
        if (spark > 0.9f) t.color = Rgb{0.30f, 0.62f, 0.60f};
    }
    t.roughness = 0.55f;
    t.height = 0.5f + grain * 0.2f;
    return t;
}

Recipe railRecipe(const std::string& name, std::uint32_t seed) {
    if (name == "rail_tie") {
        return [=](float u, float v) {
            Texel t = planksTexel(u, v, seed, Rgb{0.33f, 0.22f, 0.13f});
            t.roughness = 0.9f;
            return t;
        };
    }
    if (name == "rail_iron") {
        return [=](float u, float v) {
            Texel t = ironTexel(u, v, seed, Rgb{0.36f, 0.35f, 0.34f});
            t.color = t.color * (0.85f + 0.3f * fbm(u, v, 12, 2, seed + 1u));
            t.roughness = 0.35f;
            return t;
        };
    }
    if (name == "rail_surge") {  // carril dorado con vetas de arcanita
        return [=](float u, float v) {
            Texel t = ironTexel(u, v, seed, Rgb{0.85f, 0.66f, 0.24f});
            t.metal = 0.9f;
            t.roughness = 0.3f;
            const float vein = std::abs(std::sin((u + v * 0.35f) * kTau * 4.0f + fbm(u, v, 6, 2, seed) * 3.0f));
            if (vein < 0.12f) {
                t.color = kArcanite;
                t.emission = 0.8f;
                t.metal = 0.0f;
            }
            return t;
        };
    }
    if (name == "rail_drag") {  // zapatas de freno: hierro oscuro rayado y oxido
        return [=](float u, float v) {
            Texel t = ironTexel(u, v, seed, Rgb{0.30f, 0.17f, 0.10f});
            t.color = t.color * (0.8f + 0.4f * std::abs(std::sin(v * kTau * 8.0f)));
            t.roughness = 0.85f;
            t.metal = 0.3f;
            return t;
        };
    }
    if (name == "rail_switch") {  // cabeza roja y blanca de la palanca del cambio
        return [=](float u, float v) {
            Texel t = ironTexel(u, v, seed, Rgb{0.80f, 0.12f, 0.10f});
            t.metal = 0.2f;
            t.roughness = 0.5f;
            if (std::fmod(u * 4.0f + v * 4.0f, 2.0f) < 1.0f) t.color = Rgb{0.92f, 0.90f, 0.86f};
            return t;
        };
    }
    if (name == "rail_sensor") {  // placa de piedra runica
        return [=](float u, float v) {
            Texel t = runeStoneTexel(u, v, seed);
            if (t.emission > 0.0f) t.color = kArcanite;
            return t;
        };
    }
    if (name == "buffer_stop") {  // viga del tope: franjas rojas y blancas
        return [=](float u, float v) {
            Texel t = planksTexel(u, v, seed, Rgb{0.70f, 0.14f, 0.10f});
            if (std::fmod((u + v) * 4.0f, 2.0f) < 1.0f) t.color = Rgb{0.90f, 0.88f, 0.82f} * (0.9f + fbm(u, v, 8, 2, seed) * 0.15f);
            return t;
        };
    }
    if (name == "arcanite_ore") return [=](float u, float v) { return oreTexel(u, v, seed, kArcanite, 0.0f, 0.25f, 0.55f); };
    if (name == "arcanite_trace") return [=](float u, float v) { return arcaniteDust(u, v, seed, false); };
    if (name == "arcanite_trace_lit") return [=](float u, float v) { return arcaniteDust(u, v, seed, true); };
    if (name == "rune_lever_base" || name == "null_rune_base" || name == "echo_relay_side") {
        return [=](float u, float v) {
            Texel t = stoneTexel(u, v, seed, Rgb{0.24f, 0.22f, 0.28f});
            if (inFrame(u, v, 0.06f)) t.color = t.color * 0.7f;
            return t;
        };
    }
    if (name == "rune_lever_handle" || name == "rune_lever_handle_on") {
        const bool on = name == "rune_lever_handle_on";
        return [=](float u, float v) {
            // Mango de madera con la punta (filas de arriba) de arcanita.
            if (v < 0.35f) return arcaniteDust(u, v, seed, on);
            return stickTexel(u, v, seed);
        };
    }
    if (name == "rune_button" || name == "rune_button_on") {
        const bool on = name == "rune_button_on";
        return [=](float u, float v) {
            Texel t = stoneTexel(u, v, seed, Rgb{0.30f, 0.28f, 0.34f});
            if (std::hypot(u - 0.5f, v - 0.5f) < 0.22f) {
                t.color = on ? kArcanite : Rgb{0.18f, 0.42f, 0.42f};
                t.emission = on ? 0.9f : 0.0f;
            }
            return t;
        };
    }
    if (name == "trigger_plate") {
        return [=](float u, float v) {
            Texel t = stoneTexel(u, v, seed, Rgb{0.42f, 0.41f, 0.40f});
            if (inFrame(u, v, 0.08f)) t.color = t.color * 0.75f;
            const float r = std::hypot(u - 0.5f, v - 0.5f);
            if (r > 0.18f && r < 0.22f) t.color = Rgb{0.18f, 0.42f, 0.42f};
            return t;
        };
    }
    if (name == "signal_lamp") {
        return [=](float u, float v) {
            Texel t = lampPaneTexel(u, v, seed, Rgb{0.16f, 0.30f, 0.30f});
            t.emission = 0.0f;
            return t;
        };
    }
    if (name == "signal_lamp_lit") return [=](float u, float v) { return lampPaneTexel(u, v, seed, Rgb{0.55f, 1.0f, 0.92f}); };
    if (name == "null_rune_crystal") return [=](float u, float v) { return crystalTexel(u, v, seed, Rgb{0.20f, 0.26f, 0.34f}, 0.0f); };
    if (name == "null_rune_crystal_lit") return [=](float u, float v) { return crystalTexel(u, v, seed, Rgb{0.45f, 0.65f, 1.0f}, 0.9f); };
    if (name == "echo_relay_top") {
        return [=](float u, float v) {
            Texel t = stoneTexel(u, v, seed, Rgb{0.26f, 0.24f, 0.30f});
            // Dos ondas de eco grabadas.
            const float r = std::hypot(u - 0.5f, (v - 0.5f) * 1.6f);
            if (std::abs(std::fmod(r * 8.0f, 1.0f) - 0.5f) < 0.08f && r < 0.45f) t.color = Rgb{0.18f, 0.42f, 0.42f};
            return t;
        };
    }
    if (name == "echo_relay_crystal") return [=](float u, float v) { return crystalTexel(u, v, seed, Rgb{0.16f, 0.36f, 0.34f}, 0.0f); };
    if (name == "echo_relay_crystal_lit") return [=](float u, float v) { return crystalTexel(u, v, seed, kArcanite, 0.9f); };
    return {};
}

// --- Reino Profundo (GeneratorDeep.cpp) -------------------------------------------------

// Ladrillos de los Delvers: sillares grandes oscuros con una veta dorada en las juntas.
Texel delverBricksTexel(float u, float v, std::uint32_t seed, bool cracked) {
    Texel t = bricksTexel(u, v, seed, 2, Rgb{0.22f, 0.21f, 0.27f}, Rgb{0.55f, 0.44f, 0.20f});
    if (cracked) {
        const Cell c = voronoi(u, v, 3, seed + 7u, 1.0f);
        const float crack = 1.0f - smooth(0.0f, 0.03f, c.f2 - c.f1);
        t.color = mix(t.color, Rgb{0.03f, 0.03f, 0.04f}, crack * 0.9f);
        t.height -= crack * 0.35f;
    }
    return t;
}

Recipe deepRecipe(const std::string& name, std::uint32_t seed) {
    if (name == "umbral_stone") {
        return [=](float u, float v) {
            Texel t = stoneTexel(u, v, seed, Rgb{0.20f, 0.21f, 0.27f});
            const float layers = std::sin((v + fbm(u, v, 3, 2, seed + 9u) * 0.08f) * kTau * 3.0f);
            t.color = t.color * (0.93f + layers * 0.07f);
            return t;
        };
    }
    if (name == "delver_bricks") return [=](float u, float v) { return delverBricksTexel(u, v, seed, false); };
    if (name == "cracked_delver_bricks") return [=](float u, float v) { return delverBricksTexel(u, v, seed, true); };
    if (name == "glyph_stone") {
        return [=](float u, float v) {
            Texel t = runeStoneTexel(u, v, seed);
            if (t.emission > 0.0f) t.color = Rgb{0.45f, 1.0f, 0.82f};
            return t;
        };
    }
    if (name == "lore_tablet") {
        return [=](float u, float v) {
            // Losa clara con renglones de runas doradas y marco.
            Texel t = stoneTexel(u, v, seed, Rgb{0.46f, 0.42f, 0.36f});
            if (inFrame(u, v, 0.08f)) {
                t.color = Rgb{0.22f, 0.21f, 0.27f};
                return t;
            }
            const float row = std::fmod(v * 6.0f, 1.0f);
            const int glyph = static_cast<int>(std::floor(u * 9.0f)) + static_cast<int>(std::floor(v * 6.0f)) * 9;
            if (v > 0.15f && v < 0.88f && row > 0.3f && row < 0.75f && rand01(glyph, 3, seed) > 0.25f &&
                std::abs(std::fmod(u * 9.0f, 1.0f) - 0.5f) < 0.3f) {
                t.color = Rgb{0.95f, 0.76f, 0.30f};
                t.emission = 0.7f;
                t.height -= 0.15f;
            }
            return t;
        };
    }
    if (name == "mithril_ore") return [=](float u, float v) { return oreTexel(u, v, seed, Rgb{0.70f, 0.92f, 1.0f}, 0.8f, 0.2f, 0.45f); };
    if (name == "duskroot") {
        return [=](float u, float v) {
            Texel t = glowVinesTexel(u, v, seed);
            if (t.alpha > 0.0f && t.emission > 0.0f) t.color = Rgb{0.55f, 0.75f, 1.0f};
            return t;
        };
    }
    // Capas hondas: adamantita (roja violacea), roca del Hueco (palida) y la raiz viva del Corazon.
    if (name == "adamant_ore") return [=](float u, float v) { return oreTexel(u, v, seed, Rgb{0.95f, 0.30f, 0.55f}, 0.9f, 0.18f, 0.55f); };
    if (name == "hollow_stone") {
        return [=](float u, float v) {
            Texel t = stoneTexel(u, v, seed, Rgb{0.58f, 0.56f, 0.66f});
            const float veins = std::abs(fbm(u, v, 8, 2, seed + 5u) - 0.5f);
            if (veins < 0.025f) t.color = t.color * 0.72f;  // grietas finas
            return t;
        };
    }
    if (name == "heart_root") {
        return [=](float u, float v) {
            Texel t = logBarkTexel(u, v, seed, Rgb{0.36f, 0.12f, 0.10f}, 9.0f);
            const float vein = std::abs(std::sin((u + fbm(u, v, 3, 2, seed + 3u) * 0.3f) * kTau * 3.0f));
            if (vein < 0.12f) {
                t.color = Rgb{1.0f, 0.45f, 0.28f};
                t.emission = 1.0f;
            }
            return t;
        };
    }
    if (name == "heart_root_top") {
        return [=](float u, float v) {
            Texel t = logTopTexel(u, v, seed, Rgb{0.85f, 0.35f, 0.25f}, Rgb{0.36f, 0.12f, 0.10f});
            const float r = std::hypot(u - 0.5f, v - 0.5f);
            if (r < 0.12f) {
                t.color = Rgb{1.0f, 0.62f, 0.35f};
                t.emission = 1.0f;
            }
            return t;
        };
    }
    return {};
}

// --- NOCHE-MUNDO: biomas frondosos y construccion (Blocks.cpp addLushBlocks) -----------------
// (El juego usa sus PNG pixel art de Tools/texturas_mundo.py; esto es lo que sale sin ellos.)

// Cesped de un color propio (sin tinte del bioma): briznas como grass_top recoloreadas.
Texel tonedGrassTop(float u, float v, std::uint32_t seed, Rgb tone) {
    Texel t = grassTopTexel(u, v, seed);
    const float l = (t.color.r + t.color.g + t.color.b) / (0.33f + 0.52f + 0.20f);
    t.color = tone * l;
    return t;
}
// Lado: tierra con el borde de arriba del color de la superficie.
Texel tonedGrassSide(float u, float v, std::uint32_t seed, Rgb tone, float depth) {
    Texel t = dirtTexel(u, v, seed);
    const float edge = depth + fbm(u, 0.0f, 8, 3, seed + 21u) * 0.07f + (rand01(static_cast<int>(u * 48.0f), 0, seed) - 0.5f) * 0.06f;
    if (v < edge) {
        const Texel top = tonedGrassTop(u, v, seed + 5u, tone);
        const float k = smooth(edge, edge - 0.03f, v);
        t.color = mix(t.color, top.color * 0.95f, k);
        t.height = t.height * (1.0f - k) + (top.height * 0.5f + 0.5f) * k;
    }
    return t;
}
// Hojarasca: hojas sueltas de otono sobre la tierra.
Texel litterTexel(float u, float v, std::uint32_t seed) {
    Texel t = dirtTexel(u, v, seed);
    const Cell c = voronoi(u, v, 9, seed + 3u, 1.0f);
    const float leaf = smooth(0.48f, 0.28f, c.f1);
    if (leaf > 0.0f) {
        static constexpr Rgb kTones[4] = {{0.86f, 0.42f, 0.12f}, {0.74f, 0.20f, 0.12f}, {0.92f, 0.70f, 0.20f}, {0.55f, 0.32f, 0.14f}};
        const Rgb tone = kTones[static_cast<int>(idRand(c.id, 1) * 3.999f)];
        t.color = mix(t.color, tone * (0.8f + idRand(c.id, 2) * 0.3f), std::min(1.0f, leaf * 1.6f));
        t.height += leaf * 0.2f;
    }
    return t;
}
// Liana: cuerdas verdes que cuelgan con hojitas.
Texel vinesTexel(float u, float v, std::uint32_t seed, Rgb tone) {
    Texel t;
    t.alpha = 0.0f;
    t.height = 1.0f;
    t.roughness = 0.7f;
    for (int i = 0; i < 4; ++i) {
        const float x = 0.14f + 0.24f * static_cast<float>(i) + std::sin(v * 9.0f + static_cast<float>(i) * 2.0f) * 0.03f;
        const float d = std::abs(u - x);
        if (d < 0.022f) {
            t.alpha = 1.0f;
            t.color = tone * 0.75f;
        } else if (d < 0.08f && std::fmod(v * 7.0f + rand01(i, 0, seed) * 3.0f, 1.0f) < 0.35f) {
            t.alpha = 1.0f;  // hojas a los lados
            t.color = tone * (0.9f + rand01(static_cast<int>(v * 30.0f), i, seed) * 0.3f);
        }
    }
    return t;
}
// Petalos caidos: manchas rosas sueltas cerca del suelo.
Texel petalsTexel(float u, float v, std::uint32_t seed) {
    Texel t;
    t.alpha = 0.0f;
    t.height = 1.0f;
    t.roughness = 0.6f;
    if (v < 0.55f) return t;
    const Cell c = voronoi(u, v, 7, seed, 1.0f);
    if (c.f1 < 0.22f && idRand(c.id, 1) > 0.3f) {
        t.alpha = 1.0f;
        t.color = mix(Rgb{0.98f, 0.70f, 0.82f}, Rgb{1.0f, 0.88f, 0.93f}, idRand(c.id, 2));
    }
    return t;
}
// Nenufar visto desde arriba: hoja redonda con una muesca y a veces una flor.
Texel lilyTexel(float u, float v, std::uint32_t seed) {
    Texel t;
    const float dx = u - 0.5f, dy = v - 0.5f;
    const float r = std::hypot(dx, dy);
    const float a = std::atan2(dy, dx);
    t.alpha = (r < 0.44f && !(a > -0.25f && a < 0.25f && r > 0.05f)) ? 1.0f : 0.0f;
    t.color = Rgb{0.24f, 0.52f, 0.20f} * (0.85f + 0.2f * std::cos(a * 9.0f) * smooth(0.1f, 0.4f, r));
    if (std::hypot(u - 0.36f, v - 0.62f) < 0.08f) t.color = Rgb{0.98f, 0.78f, 0.88f};
    t.height = 0.6f;
    t.roughness = 0.45f;
    (void)seed;
    return t;
}
// Vidriera: emplomado oscuro y vidrios de tonos de un color.
Texel stainedTexel(float u, float v, std::uint32_t seed, Rgb tone) {
    const Cell c = voronoi(u, v, 3, seed, 0.8f);
    const float lead = 1.0f - smooth(0.0f, 0.05f, c.f2 - c.f1);
    const float border = std::min(std::min(u, 1.0f - u), std::min(v, 1.0f - v));
    Texel t;
    const float k = idRand(c.id, 1);
    t.color = mix(tone * (0.75f + k * 0.4f), Rgb{1.0f, 0.95f, 0.75f}, k > 0.85f ? 0.5f : 0.0f);
    if (lead > 0.5f || border < 0.05f) t.color = Rgb{0.12f, 0.12f, 0.14f};
    t.height = lead > 0.5f ? 0.8f : 0.5f;
    t.roughness = 0.1f;
    t.reflectance = 0.9f;
    t.emission = lead > 0.5f ? 0.0f : 0.08f;
    return t;
}

Recipe lushRecipe(const std::string& name, std::uint32_t seed) {
    using R = Rgb;
    if (name == "jungle_grass_top") return [=](float u, float v) { return tonedGrassTop(u, v, seed, R{0.22f, 0.62f, 0.16f}); };
    if (name == "jungle_grass_side") return [=](float u, float v) { return tonedGrassSide(u, v, seed, R{0.22f, 0.62f, 0.16f}, 0.22f); };
    if (name == "blossom_grass_top") {
        return [=](float u, float v) {
            Texel t = tonedGrassTop(u, v, seed, R{0.45f, 0.70f, 0.30f});
            const Cell c = voronoi(u, v, 10, seed + 7u, 1.0f);
            if (c.f1 < 0.16f && idRand(c.id, 1) > 0.55f) t.color = R{0.98f, 0.72f, 0.84f};
            return t;
        };
    }
    if (name == "blossom_grass_side") return [=](float u, float v) { return tonedGrassSide(u, v, seed, R{0.45f, 0.70f, 0.30f}, 0.16f); };
    if (name == "swamp_grass_top") return [=](float u, float v) { return tonedGrassTop(u, v, seed, R{0.34f, 0.42f, 0.18f}); };
    if (name == "swamp_grass_side") return [=](float u, float v) { return tonedGrassSide(u, v, seed, R{0.34f, 0.42f, 0.18f}, 0.18f); };
    if (name == "savanna_grass_top") return [=](float u, float v) { return tonedGrassTop(u, v, seed, R{0.72f, 0.66f, 0.30f}); };
    if (name == "savanna_grass_side") return [=](float u, float v) { return tonedGrassSide(u, v, seed, R{0.72f, 0.66f, 0.30f}, 0.16f); };
    if (name == "leaf_litter_top") return [=](float u, float v) { return litterTexel(u, v, seed); };
    if (name == "leaf_litter_side") {
        return [=](float u, float v) {
            Texel t = dirtTexel(u, v, seed);
            if (v < 0.14f + fbm(u, 0.0f, 8, 2, seed + 3u) * 0.06f) t = litterTexel(u, v, seed + 1u);
            return t;
        };
    }
    if (name == "mud") {
        return [=](float u, float v) {
            Texel t = dirtTexel(u, v, seed);
            t.color = R{t.color.r * 0.62f, t.color.g * 0.60f, t.color.b * 0.58f};
            t.roughness = 0.45f;  // humedo
            return t;
        };
    }
    if (name == "jungle_log") return [=](float u, float v) { return logBarkTexel(u, v, seed, R{0.42f, 0.33f, 0.20f}, 9.0f); };
    if (name == "jungle_log_top") return [=](float u, float v) { return logTopTexel(u, v, seed, R{0.68f, 0.50f, 0.30f}, R{0.40f, 0.31f, 0.19f}); };
    if (name == "cherry_log") return [=](float u, float v) { return logBarkTexel(u, v, seed, R{0.30f, 0.17f, 0.16f}, 14.0f); };
    if (name == "cherry_log_top") return [=](float u, float v) { return logTopTexel(u, v, seed, R{0.86f, 0.60f, 0.56f}, R{0.30f, 0.17f, 0.16f}); };
    if (name == "mangrove_log") return [=](float u, float v) { return logBarkTexel(u, v, seed, R{0.40f, 0.24f, 0.17f}, 10.0f); };
    if (name == "mangrove_log_top") return [=](float u, float v) { return logTopTexel(u, v, seed, R{0.64f, 0.30f, 0.24f}, R{0.40f, 0.24f, 0.17f}); };
    if (name == "acacia_log") return [=](float u, float v) { return logBarkTexel(u, v, seed, R{0.42f, 0.38f, 0.34f}, 8.0f); };
    if (name == "acacia_log_top") return [=](float u, float v) { return logTopTexel(u, v, seed, R{0.80f, 0.42f, 0.22f}, R{0.42f, 0.38f, 0.34f}); };
    if (name == "jungle_leaves") return [=](float u, float v) { return leavesTexel(u, v, seed, R{0.16f, 0.50f, 0.12f}, 0.2f, 6); };
    if (name == "cherry_leaves") return [=](float u, float v) { return leavesTexel(u, v, seed, R{0.98f, 0.66f, 0.80f}, 0.2f, 8); };
    if (name == "mangrove_leaves") return [=](float u, float v) { return leavesTexel(u, v, seed, R{0.26f, 0.44f, 0.16f}, 0.2f, 9); };
    if (name == "acacia_leaves") return [=](float u, float v) { return leavesTexel(u, v, seed, R{0.42f, 0.56f, 0.16f}, 0.15f, 9); };
    if (name == "autumn_leaves_orange") return [=](float u, float v) { return leavesTexel(u, v, seed, R{0.92f, 0.46f, 0.10f}, 0.2f, 7); };
    if (name == "autumn_leaves_red") return [=](float u, float v) { return leavesTexel(u, v, seed, R{0.78f, 0.16f, 0.10f}, 0.2f, 7); };
    if (name == "autumn_leaves_yellow") return [=](float u, float v) { return leavesTexel(u, v, seed, R{0.96f, 0.78f, 0.18f}, 0.2f, 7); };
    if (name == "jungle_vines") return [=](float u, float v) { return vinesTexel(u, v, seed, R{0.20f, 0.52f, 0.14f}); };
    if (name == "blossom_petals") return [=](float u, float v) { return petalsTexel(u, v, seed); };
    if (name == "lily_pad") return [=](float u, float v) { return lilyTexel(u, v, seed); };
    if (name == "jungle_planks") return [=](float u, float v) { return planksTexel(u, v, seed, R{0.66f, 0.46f, 0.30f}); };
    if (name == "cherry_planks") return [=](float u, float v) { return planksTexel(u, v, seed, R{0.88f, 0.62f, 0.58f}); };
    if (name == "acacia_planks") return [=](float u, float v) { return planksTexel(u, v, seed, R{0.78f, 0.42f, 0.22f}); };
    if (name == "mangrove_planks") return [=](float u, float v) { return planksTexel(u, v, seed, R{0.52f, 0.24f, 0.20f}); };
    if (name == "white_bricks") return [=](float u, float v) { return bricksTexel(u, v, seed, 4, R{0.90f, 0.88f, 0.84f}, R{0.66f, 0.64f, 0.62f}); };
    if (name == "sandstone_bricks") return [=](float u, float v) { return bricksTexel(u, v, seed, 3, R{0.86f, 0.75f, 0.52f}, R{0.66f, 0.56f, 0.38f}); };
    if (name == "quartz_tiles") {
        return [=](float u, float v) {
            Texel t = plainTexel(u, v, seed, R{0.94f, 0.92f, 0.90f}, 0.03f);
            const float gu = std::fmod(u * 2.0f, 1.0f), gv = std::fmod(v * 2.0f, 1.0f);
            if (std::min({gu, 1.0f - gu, gv, 1.0f - gv}) < 0.03f) {
                t.color = R{0.70f, 0.68f, 0.66f};
                t.height -= 0.3f;
            }
            t.roughness = 0.25f;
            return t;
        };
    }
    if (name == "copper_block" || name == "oxidized_copper") {
        const bool old = name == "oxidized_copper";
        return [=](float u, float v) {
            Texel t = plainTexel(u, v, seed, old ? R{0.32f, 0.66f, 0.56f} : R{0.86f, 0.50f, 0.30f}, 0.06f);
            const float rivet = std::min(std::min(u, 1.0f - u), std::min(v, 1.0f - v));
            if (rivet < 0.05f) t.color = t.color * 0.7f;
            t.metal = old ? 0.2f : 0.9f;
            t.roughness = old ? 0.7f : 0.35f;
            return t;
        };
    }
    if (name == "polished_stone" || name == "polished_stone_top") {
        const bool top = name == "polished_stone_top";
        return [=](float u, float v) {
            Texel t = plainTexel(u, v, seed, R{0.60f, 0.60f, 0.62f}, 0.04f);
            const float border = std::min(std::min(u, 1.0f - u), std::min(v, 1.0f - v));
            if (border < 0.06f || (!top && std::abs(v - 0.5f) < 0.02f)) {
                t.color = R{0.44f, 0.44f, 0.46f};
                t.height -= 0.25f;
            }
            t.roughness = 0.45f;
            return t;
        };
    }
    if (name == "stained_glass_blue") return [=](float u, float v) { return stainedTexel(u, v, seed, R{0.20f, 0.42f, 0.95f}); };
    if (name == "stained_glass_rose") return [=](float u, float v) { return stainedTexel(u, v, seed, R{0.95f, 0.30f, 0.55f}); };
    return {};
}

Recipe recipeFor(const std::string& name) {
    const std::uint32_t seed = static_cast<std::uint32_t>(std::hash<std::string>{}(name) & 0xFFFFFF);
    if (Recipe r = lushRecipe(name, seed)) return r;  // NOCHE-MUNDO
    if (Recipe r = dimensionRecipe(name, seed)) return r;
    if (Recipe r = structureRecipe(name, seed)) return r;
    if (Recipe r = lightRecipe(name, seed)) return r;
    if (Recipe r = settlementRecipe(name, seed)) return r;
    if (Recipe r = oceanRecipe(name, seed)) return r;
    if (Recipe r = railRecipe(name, seed)) return r;
    if (Recipe r = deepRecipe(name, seed)) return r;
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
    if (name == "copper_ore") return [=](float u, float v) { return oreTexel(u, v, seed, Rgb{0.86f, 0.47f, 0.24f}, 0.6f, 0.4f, 0.0f); };
    if (name == "crystal_ore") {
        return [=](float u, float v) { return oreTexel(u, v, seed, Rgb{0.66f, 0.48f, 1.0f}, 0.0f, 0.1f, 0.3f); };
    }
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
    // Generador Aventura.
    if (name == "fern") return [=](float u, float v) { return fernTexel(u, v, seed); };
    if (name == "bush") return [=](float u, float v) { return bushTexel(u, v, seed); };
    if (name == "blue_flower") return [=](float u, float v) { return flowerTexel(u, v, seed, Rgb{0.25f, 0.42f, 0.92f}, Rgb{0.95f, 0.9f, 0.4f}); };
    if (name == "lavender") return [=](float u, float v) { return flowerTexel(u, v, seed, Rgb{0.62f, 0.42f, 0.86f}, Rgb{0.5f, 0.3f, 0.7f}); };
    if (name == "white_flower") return [=](float u, float v) { return flowerTexel(u, v, seed, Rgb{0.96f, 0.95f, 0.92f}, Rgb{0.98f, 0.8f, 0.2f}); };
    if (name == "red_mushroom") return [=](float u, float v) { return mushroomTexel(u, v, seed, Rgb{0.85f, 0.14f, 0.1f}, true, 0.0f); };
    if (name == "brown_mushroom") return [=](float u, float v) { return mushroomTexel(u, v, seed, Rgb{0.55f, 0.38f, 0.24f}, false, 0.0f); };
    if (name == "glow_mushroom") return [=](float u, float v) { return mushroomTexel(u, v, seed, Rgb{0.3f, 0.85f, 0.9f}, true, 0.9f); };
    if (name == "reeds") return [=](float u, float v) { return reedsTexel(u, v, seed); };
    if (name == "dry_grass") return [=](float u, float v) { return tallGrassTexel(u, v, seed, Rgb{0.74f, 0.64f, 0.36f}); };
    if (name == "moss") return [=](float u, float v) { return sandTexel(u, v, seed, Rgb{0.32f, 0.52f, 0.2f}); };
    if (name == "red_sand") return [=](float u, float v) { return sandTexel(u, v, seed, Rgb{0.80f, 0.48f, 0.28f}); };
    if (name == "terracotta") return [=](float u, float v) { return terracottaTexel(u, v, seed); };
    if (Recipe r = wildRecipe(name, seed)) return r;
    if (Recipe r = caveRecipe(name, seed)) return r;
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

// Estilo de color liso: sin relieve (normal plana, altura y oclusion
// llenas). Los cubos toman el color medio de su textura (las hojas macizas,
// como matas de color); las plantas (cruz) y las menas conservan su dibujo
// (una flor media seria una mancha parda). La variacion por bloque la pone el
// shader (voxel.frag, push.style).
void flatten(const std::string& name, gfx::VoxelTextureLayer& out) {
    bool cross = false, leaves = false;
    for (const BlockDef& d : blocks()) {
        if (d.top != name && d.side != name && d.bottom != name) continue;
        cross = cross || d.shape == BlockShape::Cross;
        leaves = leaves || (d.shape == BlockShape::Cube && d.cutout && d.waves);
    }
    const bool keep_pattern = cross || name.find("_ore") != std::string::npos || name == "torch" || structureKeepsPattern(name) ||
                              lightKeepsPattern(name) || settlementKeepsPattern(name);
    const std::size_t n = out.albedo.size() / 4;
    if (n == 0) return;
    double sum[4] = {0, 0, 0, 0}, material[4] = {0, 0, 0, 0}, weight = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double w = out.albedo[i * 4 + 3] / 255.0;
        for (int c = 0; c < 3; ++c) sum[c] += out.albedo[i * 4 + static_cast<std::size_t>(c)] * w;
        for (int c = 0; c < 4; ++c) material[c] += out.material[i * 4 + static_cast<std::size_t>(c)] * w;
        weight += w;
    }
    if (weight <= 0.0) return;
    // Color liso: con las normales planas, una rugosidad media hace que cada cara brille como si
    // estuviera mojada (sol y reflejos sobre un plano perfecto). Lo mate (tierra, piedra, madera,
    // hierba...) pasa a casi totalmente rugoso; lo que debe brillar (hielo, vidrio, cristal) se queda.
    if (material[0] / weight > 0.45 * 255.0) material[0] = std::max(material[0] / weight, 0.95 * 255.0) * weight;
    for (std::size_t i = 0; i < n; ++i) {
        if (keep_pattern && out.material[i * 4] > 115) out.material[i * 4] = std::max<std::uint8_t>(out.material[i * 4], 242);
        if (!keep_pattern) {
            for (int c = 0; c < 3; ++c) out.albedo[i * 4 + static_cast<std::size_t>(c)] = static_cast<std::uint8_t>(sum[c] / weight + 0.5);
            if (leaves) out.albedo[i * 4 + 3] = 255;
            for (int c = 0; c < 4; ++c) out.material[i * 4 + static_cast<std::size_t>(c)] = static_cast<std::uint8_t>(material[c] / weight + 0.5);
        }
        out.normal[i * 4 + 0] = 128;
        out.normal[i * 4 + 1] = 128;
        out.normal[i * 4 + 2] = 255;
        out.normal[i * 4 + 3] = 255;
    }
}

}  // namespace

// HOJAS: mascara de hojas sueltas (elipses en punta, giradas, que se repiten en los bordes): ~55 % cubierto.
void fluffMask(std::uint32_t size, std::vector<std::uint8_t>& albedo) {
    if (albedo.size() < static_cast<std::size_t>(size) * size * 4) return;
    std::vector<float> cover(static_cast<std::size_t>(size) * size, 0.0f);
    constexpr int kLeaves = 34;
    for (int i = 0; i < kLeaves; ++i) {
        const float cx = rand01(i, 11, 7771u), cy = rand01(i, 12, 7771u);
        const float len = 0.10f + 0.07f * rand01(i, 13, 7771u), wid = len * (0.42f + 0.18f * rand01(i, 14, 7771u));
        const float ang = rand01(i, 15, 7771u) * 6.2831853f;
        const float ca = std::cos(ang), sa = std::sin(ang);
        for (std::uint32_t py = 0; py < size; ++py) {
            for (std::uint32_t px = 0; px < size; ++px) {
                float dx = (static_cast<float>(px) + 0.5f) / static_cast<float>(size) - cx;
                float dy = (static_cast<float>(py) + 0.5f) / static_cast<float>(size) - cy;
                dx -= std::round(dx);
                dy -= std::round(dy);
                const float u = (dx * ca + dy * sa) / len, v = (-dx * sa + dy * ca) / wid;
                // hoja en punta: mas estrecha hacia los extremos
                const float w = 1.0f - u * u;
                if (w <= 0.0f) continue;
                if (v * v < w * w) cover[py * size + px] = 1.0f;
            }
        }
    }
    for (std::size_t i = 0; i < cover.size(); ++i) {
        const std::uint8_t a = albedo[i * 4 + 3];
        albedo[i * 4 + 3] = cover[i] > 0.5f ? a : 0;
    }
}

namespace {

// PAQUETES-TEXTURAS: imagen de un paquete a size x size. Si es mas pequena y cabe un numero entero de veces
// (16, 32, 64... -> 256), se amplia por vecino mas cercano: el pixel art sigue nitido. Si no, como loadResized.
bool loadPackImage(const std::filesystem::path& file, std::uint32_t size, std::vector<std::uint8_t>& out) {
    std::error_code error;
    if (file.empty() || !std::filesystem::exists(file, error)) return false;
    asset::ImageRgba8 image;
    if (!asset::loadImageRgba8(file, image, 4096) || image.width == 0 || image.height == 0) return false;
    if (image.width == image.height && image.width <= size && size % image.width == 0) {
        const std::uint32_t k = size / image.width;
        out.assign(static_cast<std::size_t>(size) * size * 4, 0);
        for (std::uint32_t y = 0; y < size; ++y) {
            for (std::uint32_t x = 0; x < size; ++x) {
                const std::size_t src = (static_cast<std::size_t>(y / k) * image.width + x / k) * 4;
                const std::size_t dst = (static_cast<std::size_t>(y) * size + x) * 4;
                for (std::size_t c = 0; c < 4; ++c) out[dst + c] = image.pixels[src + c];
            }
        }
        return true;
    }
    return loadResized(file, size, out);
}

// Normal (xy), altura (z) y oclusion (w) sacadas del brillo del color: el paquete que solo trae el color no hereda
// el relieve de la textura original (que tendria otro dibujo).
void normalFromAlbedo(std::uint32_t size, const std::vector<std::uint8_t>& albedo, std::vector<std::uint8_t>& normal) {
    const std::size_t n = static_cast<std::size_t>(size) * size;
    if (albedo.size() < n * 4) return;
    std::vector<float> h(n);
    for (std::size_t i = 0; i < n; ++i) {
        h[i] = (0.299f * albedo[i * 4] + 0.587f * albedo[i * 4 + 1] + 0.114f * albedo[i * 4 + 2]) / 255.0f;
    }
    const int s = static_cast<int>(size);
    const auto at = [&](int x, int y) { return h[static_cast<std::size_t>(wrap(y, s)) * size + static_cast<std::size_t>(wrap(x, s))]; };
    const float strength = 2.0f * std::max(1.0f, static_cast<float>(size) / 64.0f);
    normal.assign(n * 4, 0);
    for (int y = 0; y < s; ++y) {
        for (int x = 0; x < s; ++x) {
            float dx = 0.0f, dy = 0.0f;
            for (int k = -1; k <= 1; ++k) {  // Sobel
                dx += (at(x + 1, y + k) - at(x - 1, y + k)) * (k == 0 ? 2.0f : 1.0f);
                dy += (at(x + k, y + 1) - at(x + k, y - 1)) * (k == 0 ? 2.0f : 1.0f);
            }
            float nx = -dx * strength * 0.25f, ny = dy * strength * 0.25f;
            const float len = std::sqrt(nx * nx + ny * ny + 1.0f);
            nx /= len;
            ny /= len;
            const std::size_t i = (static_cast<std::size_t>(y) * size + static_cast<std::size_t>(x)) * 4;
            const float hv = at(x, y);
            normal[i + 0] = byte(nx * 0.5f + 0.5f);
            normal[i + 1] = byte(ny * 0.5f + 0.5f);
            normal[i + 2] = byte(0.72f + 0.28f * hv);
            normal[i + 3] = byte(0.85f + 0.15f * hv);
        }
    }
}

// El material medio (rugosidad, metal, emision, reflectancia) en toda la textura.
void averageMaterial(std::vector<std::uint8_t>& material) {
    const std::size_t n = material.size() / 4;
    if (n == 0) return;
    double sum[4] = {0, 0, 0, 0};
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t c = 0; c < 4; ++c) sum[c] += material[i * 4 + c];
    }
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t c = 0; c < 4; ++c) material[i * 4 + c] = static_cast<std::uint8_t>(sum[c] / static_cast<double>(n) + 0.5);
    }
}

// MENAS-HZ: <mena>_gem = el mineral de la textura de la mena (la que haya: generada, del disco o del paquete). Los
// pixeles del mineral son los que mas se apartan del color medio (la roca es la mayor parte): su color medio, con el
// brillo de cada pixel de la mena para que no quede liso, y su material medio (metal, rugosidad, emision).
void gemFromOre(gfx::VoxelTextureLayer& t) {
    const std::size_t n = t.albedo.size() / 4;
    if (n == 0) return;
    double mean[3] = {0, 0, 0};
    for (std::size_t i = 0; i < n; ++i)
        for (std::size_t c = 0; c < 3; ++c) mean[c] += t.albedo[i * 4 + c];
    for (double& m : mean) m /= static_cast<double>(n);
    std::vector<float> dist(n);
    for (std::size_t i = 0; i < n; ++i) {
        float d = 0.0f;
        for (std::size_t c = 0; c < 3; ++c) {
            const float e = static_cast<float>(t.albedo[i * 4 + c] - mean[c]);
            d += e * e;
        }
        dist[i] = d;
    }
    // Dos grupos (k-medias con k = 2): la roca empieza en el color medio y el mineral en el pixel mas apartado; el
    // mineral es el grupo pequeno (con solo "lo mas apartado" salian pepitas grises en el carbon: la roca clara
    // tambien se aparta del medio).
    std::size_t farthest = 0;
    for (std::size_t i = 1; i < n; ++i)
        if (dist[i] > dist[farthest]) farthest = i;
    double rock[3] = {mean[0], mean[1], mean[2]};
    double ore[3] = {static_cast<double>(t.albedo[farthest * 4]), static_cast<double>(t.albedo[farthest * 4 + 1]),
                     static_cast<double>(t.albedo[farthest * 4 + 2])};
    std::vector<std::uint8_t> is_ore(n, 0);
    for (int it = 0; it < 8; ++it) {
        double sr[3] = {0, 0, 0}, so[3] = {0, 0, 0};
        std::size_t nr = 0, no = 0;
        for (std::size_t i = 0; i < n; ++i) {
            double dr = 0, d_o = 0;
            for (std::size_t c = 0; c < 3; ++c) {
                const double p = t.albedo[i * 4 + c];
                dr += (p - rock[c]) * (p - rock[c]);
                d_o += (p - ore[c]) * (p - ore[c]);
            }
            is_ore[i] = d_o < dr ? 1 : 0;
            for (std::size_t c = 0; c < 3; ++c) (is_ore[i] ? so : sr)[c] += t.albedo[i * 4 + c];
            ++(is_ore[i] ? no : nr);
        }
        if (no == 0 || nr == 0) break;
        for (std::size_t c = 0; c < 3; ++c) {
            rock[c] = sr[c] / static_cast<double>(nr);
            ore[c] = so[c] / static_cast<double>(no);
        }
    }
    std::size_t ore_count = 0;
    for (std::uint8_t v : is_ore) ore_count += v;
    if (ore_count * 2 > n)  // el grupo grande es la roca
        for (std::uint8_t& v : is_ore) v = v != 0 ? 0 : 1;
    double gem[3] = {0, 0, 0}, mat[4] = {0, 0, 0, 0}, lum = 0;
    std::size_t count = 0;
    const bool has_material = t.material.size() == n * 4;
    for (std::size_t i = 0; i < n; ++i) {
        if (is_ore[i] == 0) continue;
        for (std::size_t c = 0; c < 3; ++c) gem[c] += t.albedo[i * 4 + c];
        if (has_material)
            for (std::size_t c = 0; c < 4; ++c) mat[c] += t.material[i * 4 + c];
        lum += t.albedo[i * 4] * 0.299 + t.albedo[i * 4 + 1] * 0.587 + t.albedo[i * 4 + 2] * 0.114;
        ++count;
    }
    if (count == 0) return;
    for (double& g : gem) g /= static_cast<double>(count);
    for (double& m : mat) m /= static_cast<double>(count);
    lum /= static_cast<double>(count);
    for (std::size_t i = 0; i < n; ++i) {
        const double l = t.albedo[i * 4] * 0.299 + t.albedo[i * 4 + 1] * 0.587 + t.albedo[i * 4 + 2] * 0.114;
        // Variacion del brillo de la mena alrededor de 1 (suave: +-25 %).
        const double k = std::clamp(1.0 + (l - mean[0] * 0.299 - mean[1] * 0.587 - mean[2] * 0.114) / 255.0 * 0.8, 0.75, 1.25);
        for (std::size_t c = 0; c < 3; ++c) t.albedo[i * 4 + c] = static_cast<std::uint8_t>(std::clamp(gem[c] * k, 0.0, 255.0));
        t.albedo[i * 4 + 3] = 255;
        if (has_material)
            for (std::size_t c = 0; c < 4; ++c) t.material[i * 4 + c] = static_cast<std::uint8_t>(std::clamp(mat[c] + 0.5, 0.0, 255.0));
    }
    (void)lum;
}

// PLANTAS-ALTAS: desde que altura (0 = arriba) empieza el tramo de abajo de cada planta apilada
// (como Tools/texturas_plantas_altas.py de Horizonz).
float lowCut(const std::string& plant) {
    if (plant == "tall_grass") return 0.42f;
    if (plant == "reeds") return 0.50f;
    if (plant == "fern") return 0.55f;
    return 0.45f;
}

// La parte baja de la imagen (size x size) estirada a todo el alto (vecino mas cercano).
void lowFromFull(std::uint32_t size, float cut, std::vector<std::uint8_t>& image) {
    if (image.size() < static_cast<std::size_t>(size) * size * 4) return;
    const std::vector<std::uint8_t> full = image;
    const float y0 = cut * static_cast<float>(size);
    const float span = static_cast<float>(size) - y0;
    for (std::uint32_t y = 0; y < size; ++y) {
        const auto sy = std::min(size - 1, static_cast<std::uint32_t>(y0 + (static_cast<float>(y) + 0.5f) / static_cast<float>(size) * span));
        std::copy_n(full.begin() + static_cast<std::ptrdiff_t>(static_cast<std::size_t>(sy) * size * 4), static_cast<std::size_t>(size) * 4,
                    image.begin() + static_cast<std::ptrdiff_t>(static_cast<std::size_t>(y) * size * 4));
    }
}

// Pone encima de `out` lo que traiga el paquete para `name`.
void overlayPack(const std::string& name, std::uint32_t size, const std::filesystem::path& pack, gfx::VoxelTextureLayer& out) {
    if (pack.empty()) return;
    std::vector<std::uint8_t> image;
    const bool albedo = loadPackImage(pack / (name + ".png"), size, image);
    if (albedo) out.albedo = std::move(image);
    // "<planta>_low" que no esta en el paquete pero "<planta>" si: se saca de ella.
    static const std::string kLow = "_low";
    if (!albedo && name.size() > kLow.size() && name.compare(name.size() - kLow.size(), kLow.size(), kLow) == 0) {
        const std::string plant = name.substr(0, name.size() - kLow.size());
        if (loadPackImage(pack / (plant + ".png"), size, image)) {
            const float cut = lowCut(plant);
            lowFromFull(size, cut, image);
            out.albedo = std::move(image);
            if (loadPackImage(pack / (plant + "_n.png"), size, image)) {
                lowFromFull(size, cut, image);
                out.normal = std::move(image);
            } else {
                normalFromAlbedo(size, out.albedo, out.normal);
            }
            if (loadPackImage(pack / (plant + "_m.png"), size, image)) {
                lowFromFull(size, cut, image);
                out.material = std::move(image);
            } else {
                averageMaterial(out.material);
            }
            return;
        }
    }
    if (loadPackImage(pack / (name + "_n.png"), size, image)) out.normal = std::move(image);
    else if (albedo) normalFromAlbedo(size, out.albedo, out.normal);
    if (loadPackImage(pack / (name + "_m.png"), size, image)) out.material = std::move(image);
    else if (albedo) averageMaterial(out.material);
}

}  // namespace

void buildTexture(const std::string& name, std::uint32_t size, const std::filesystem::path& folder,
                  gfx::VoxelTextureLayer& out, bool flat_colors) {
    buildTexture(name, size, folder, std::filesystem::path{}, out, flat_colors);
}

void buildTexture(const std::string& name, std::uint32_t size, const std::filesystem::path& folder,
                  const std::filesystem::path& pack, gfx::VoxelTextureLayer& out, bool flat_colors) {
    // HOJAS: <hojas>_fluff = la textura de las hojas (generada, del disco o del paquete) con la mascara de hojas sueltas.
    static const std::string kFluff = "_fluff";
    if (name.size() > kFluff.size() && name.compare(name.size() - kFluff.size(), kFluff.size(), kFluff) == 0) {
        buildTexture(name.substr(0, name.size() - kFluff.size()), size, folder, pack, out, flat_colors);
        fluffMask(size, out.albedo);
        std::vector<std::uint8_t> image;
        if (!folder.empty()) {
            if (loadResized(folder / (name + ".png"), size, image)) out.albedo = std::move(image);
            if (loadResized(folder / (name + "_n.png"), size, image)) out.normal = std::move(image);
            if (loadResized(folder / (name + "_m.png"), size, image)) out.material = std::move(image);
        }
        if (!pack.empty()) {  // PAQUETES-TEXTURAS
            if (loadPackImage(pack / (name + ".png"), size, image)) out.albedo = std::move(image);
            if (loadPackImage(pack / (name + "_n.png"), size, image)) out.normal = std::move(image);
            if (loadPackImage(pack / (name + "_m.png"), size, image)) out.material = std::move(image);
        }
        return;
    }
    // MENAS-HZ: <mena>_gem = el mineral de la textura de la mena (Mesher.cpp: pepitas que sobresalen). Del disco o del
    // paquete si trae una propia.
    static const std::string kGem = "_gem";
    if (name.size() > kGem.size() && name.compare(name.size() - kGem.size(), kGem.size(), kGem) == 0) {
        buildTexture(name.substr(0, name.size() - kGem.size()), size, folder, pack, out, flat_colors);
        gemFromOre(out);
        std::vector<std::uint8_t> image;
        if (!folder.empty()) {
            if (loadResized(folder / (name + ".png"), size, image)) out.albedo = std::move(image);
            if (loadResized(folder / (name + "_n.png"), size, image)) out.normal = std::move(image);
            if (loadResized(folder / (name + "_m.png"), size, image)) out.material = std::move(image);
        }
        if (!pack.empty()) {
            if (loadPackImage(pack / (name + ".png"), size, image)) out.albedo = std::move(image);
            if (loadPackImage(pack / (name + "_n.png"), size, image)) out.normal = std::move(image);
            if (loadPackImage(pack / (name + "_m.png"), size, image)) out.material = std::move(image);
        }
        return;
    }
    generate(name, size, out);
    if (!folder.empty()) {
        // Las que haya en disco mandan (cada una por separado).
        std::vector<std::uint8_t> image;
        if (loadResized(folder / (name + ".png"), size, image)) out.albedo = std::move(image);
        if (loadResized(folder / (name + "_n.png"), size, image)) out.normal = std::move(image);
        if (loadResized(folder / (name + "_m.png"), size, image)) out.material = std::move(image);
    }
    overlayPack(name, size, pack, out);  // PAQUETES-TEXTURAS
    if (flat_colors) flatten(name, out);
}

bool writeTexturePngs(const std::filesystem::path& folder, std::uint32_t size) {
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    bool ok = true;
    for (const std::string& name : textureNames()) {
        // MENAS-HZ: las <mena>_gem salen de la mena al cargar (un PNG gris aqui las taparia).
        if (name.size() > 4 && name.compare(name.size() - 4, 4, "_gem") == 0) continue;
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
        if (def.name.rfind("id_test_", 0) == 0) continue;  // IDS16: los bloques de prueba no llevan icono
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
