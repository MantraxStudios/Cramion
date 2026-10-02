#include "CramionFX/asset/TreeGenerator.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <random>
#include <thread>

namespace cramion::asset {

namespace {

using core::Vec3;

constexpr float kPi = 3.14159265f;
constexpr float kGolden = 2.39996323f;  // 137.5 grados (filotaxia)

float clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }
float mixF(float a, float b, float t) { return a + (b - a) * t; }
Vec3 mixV(const Vec3& a, const Vec3& b, float t) { return a + (b - a) * t; }

float smoothstepF(float e0, float e1, float x) {
    const float t = clamp01((x - e0) / (e1 - e0));
    return t * t * (3.0f - 2.0f * t);
}

std::uint32_t pack4(float r, float g, float b, float a) {
    const auto q = [](float v) { return static_cast<std::uint32_t>(clamp01(v) * 255.0f + 0.5f); };
    return q(r) | (q(g) << 8) | (q(b) << 16) | (q(a) << 24);
}

float hashF(int x, int y, std::uint32_t s) {
    std::uint32_t h = static_cast<std::uint32_t>(x) * 0x8da6b343U ^ static_cast<std::uint32_t>(y) * 0xd8163841U ^ s * 0xcb1ab31fU;
    h ^= h >> 16;
    h *= 0x7feb352dU;
    h ^= h >> 15;
    h *= 0x846ca68bU;
    h ^= h >> 16;
    return static_cast<float>(h & 0xFFFFFFU) / 16777215.0f;
}

// Ruido de valor repetible con periodo propio en cada eje (en celdas).
float valueNoise(float x, float y, int period_x, int period_y, std::uint32_t seed) {
    const int x0 = static_cast<int>(std::floor(x));
    const int y0 = static_cast<int>(std::floor(y));
    const float fx = x - static_cast<float>(x0);
    const float fy = y - static_cast<float>(y0);
    const auto wx = [period_x](int v) { return ((v % period_x) + period_x) % period_x; };
    const auto wy = [period_y](int v) { return ((v % period_y) + period_y) % period_y; };
    const float a = hashF(wx(x0), wy(y0), seed);
    const float b = hashF(wx(x0 + 1), wy(y0), seed);
    const float c = hashF(wx(x0), wy(y0 + 1), seed);
    const float d = hashF(wx(x0 + 1), wy(y0 + 1), seed);
    const float u = fx * fx * (3.0f - 2.0f * fx);
    const float v = fy * fy * (3.0f - 2.0f * fy);
    return (a + (b - a) * u) * (1.0f - v) + (c + (d - c) * u) * v;
}

// fBm que se repite en u, v en [0, 1): `cells_x` x `cells_y` celdas en la primera octava.
float fbm(float u, float v, int cells_x, int cells_y, int octaves, std::uint32_t seed) {
    float sum = 0.0f;
    float amp = 0.5f;
    float total = 0.0f;
    for (int o = 0; o < octaves; ++o) {
        const int px = cells_x << o;
        const int py = cells_y << o;
        sum += valueNoise(u * static_cast<float>(px), v * static_cast<float>(py), px, py, seed + static_cast<std::uint32_t>(o) * 31U) * amp;
        total += amp;
        amp *= 0.5f;
    }
    return sum / total;
}

// Voronoi con la distancia al borde de la celda (al bisector con la vecina
// mas cercana) medida en (u, v), no F2 - F1: con celdas estiradas, F2 - F1
// hacia los bordes horizontales muy anchos (bandas oscuras borrosas). `edge`
// en unidades de la textura (la repeticion entera = 1; es cuadrada) y
// `horizontal`: cuanto mira hacia v el borde mas cercano (1 = borde horizontal).
struct VEdge {
    float edge = 1.0f;
    float horizontal = 0.0f;
    int id = 0;
};
VEdge voronoiEdge(float x, float y, int nx, int ny, std::uint32_t seed, float stretch_y) {
    const int cx = static_cast<int>(std::floor(x));
    const int cy = static_cast<int>(std::floor(y));
    const int reach_y = stretch_y < 0.6f ? 3 : 2;
    struct Site {
        float dx;
        float dy;
        int id;
    };
    std::array<Site, 35> sites{};
    int count = 0;
    int best = 0;
    float best_d = 1e9f;
    for (int oy = -reach_y; oy <= reach_y; ++oy) {
        for (int ox = -2; ox <= 2; ++ox) {
            const int gx = cx + ox;
            const int gy = cy + oy;
            const int wx = ((gx % nx) + nx) % nx;
            const int wy = ((gy % ny) + ny) % ny;
            const float qx = static_cast<float>(gx) + 0.1f + 0.8f * hashF(wx, wy, seed);
            const float qy = static_cast<float>(gy) + 0.1f + 0.8f * hashF(wx, wy, seed + 1U);
            const Site site{qx - x, (qy - y) * stretch_y, wy * nx + wx};
            const float d = site.dx * site.dx + site.dy * site.dy;
            if (d < best_d) {
                best_d = d;
                best = count;
            }
            sites[static_cast<std::size_t>(count++)] = site;
        }
    }
    VEdge r;
    const Site& closest = sites[static_cast<std::size_t>(best)];
    r.id = closest.id;
    // Metrica (x, y * stretch) = (u * nx, v * ny * stretch): un bisector con
    // normal n tiene en (u, v) la normal (nx n.x, ny stretch n.y).
    const float sx = static_cast<float>(nx);
    const float sy = static_cast<float>(ny) * stretch_y;
    float edge = 1e9f;
    for (int k = 0; k < count; ++k) {
        if (k == best) continue;
        const Site& o = sites[static_cast<std::size_t>(k)];
        const float ex = o.dx - closest.dx;
        const float ey = o.dy - closest.dy;
        const float len = std::sqrt(ex * ex + ey * ey);
        if (len < 1e-6f) continue;
        const float mx = (o.dx + closest.dx) * 0.5f;
        const float my = (o.dy + closest.dy) * 0.5f;
        const float metric = (mx * ex + my * ey) / len;  // > 0: el punto esta en la celda de `closest`
        const float gx = sx * ex / len;
        const float gy = sy * ey / len;
        const float g = std::sqrt(gx * gx + gy * gy);
        const float d = metric / std::max(g, 1e-6f);
        if (d < edge) {
            edge = d;
            r.horizontal = std::abs(gy) / std::max(g, 1e-6f);
        }
    }
    r.edge = std::max(edge, 0.0f);
    return r;
}

// =============================================================================
// Geometria
// =============================================================================

enum FoliageStyle { kBroadleaf = 0, kPineTufts = 1, kFirSprays = 2, kWillowCurtain = 3 };

// Reglas de cada tipo de arbol.
struct KindRules {
    int levels = 3;                 // niveles de ramas (1..3)
    float trunk_radius = 0.035f;    // radio del tronco / altura
    float crown_start = 0.3f;       // donde empiezan las ramas (fraccion de la altura)
    std::array<int, 3> branches{6, 6, 5};           // ramas por padre en cada nivel
    std::array<float, 3> angle{45.0f, 45.0f, 50.0f}; // inclinacion respecto al padre (grados)
    std::array<float, 3> length{0.6f, 0.55f, 0.5f};  // largo respecto al padre
    std::array<float, 3> droop{0.05f, 0.15f, 0.25f}; // gravedad
    std::array<float, 3> up{0.2f, 0.1f, 0.05f};      // fototropismo
    float gnarl = 0.4f;             // torsion
    int shape = 1;                  // 0 cono, 1 elipsoide, 2 llama, 3 sombrilla
    int whorl = 0;                  // ramas del tronco en verticilos de N (0 = espiral)
    bool planar = false;            // ramitas a los dos lados, en el plano de la rama (abeto)
    int style = kBroadleaf;
    float leaf_size = 0.5f;         // largo de la tarjeta (m, a 11 m de altura)
    float leaf_aspect = 1.0f;       // ancho / largo de la tarjeta
    float leaves = 6.0f;            // tarjetas por metro de ramita
    float leaf_start = 0.25f;       // desde que parte de la ramita hay hojas
    float card_curl = 0.15f;        // cuanto cae la punta de la tarjeta (fraccion del largo)
    float card_fold = 0.15f;        // pliegue en V por el nervio (fraccion del medio ancho)
    std::uint32_t leaf_layer = kTreeLayerLeaves;
    std::uint32_t bark_layer = kTreeLayerBark;
    float bark_tile = 0.8f;         // ancho de una repeticion de la corteza en el tronco (m)
    float flare = 0.6f;             // ensanche del pie
    float buttress = 0.6f;          // contrafuertes
    int roots = 5;                  // raices visibles
    float dead = 0.0f;              // ramas secas por metro de tronco bajo la copa
    float dead_inner = 0.0f;        // probabilidad de rama seca dentro de la copa
    float moss = 0.3f;              // musgo en la corteza
    float translucency = 0.8f;
};

KindRules rulesFor(TreeKind kind) {
    KindRules r;
    switch (kind) {
        case TreeKind::Pine:
            r.levels = 2; r.trunk_radius = 0.024f; r.crown_start = 0.56f;
            r.branches = {30, 9, 0}; r.angle = {80.0f, 48.0f, 0.0f}; r.length = {0.27f, 0.45f, 0.0f};
            r.droop = {0.16f, 0.06f, 0.0f}; r.up = {0.12f, 0.4f, 0.0f}; r.gnarl = 0.45f; r.shape = 3; r.whorl = 5;
            r.style = kPineTufts; r.leaf_size = 0.55f; r.leaf_aspect = 1.0f; r.leaves = 3.8f; r.leaf_start = 0.22f;
            r.card_curl = 0.06f; r.card_fold = 0.0f;
            r.leaf_layer = kTreeLayerNeedles; r.bark_layer = kTreeLayerPineBark; r.bark_tile = 0.75f;
            r.flare = 0.35f; r.buttress = 0.3f; r.roots = 4; r.dead = 1.1f; r.moss = 0.08f; r.translucency = 0.55f;
            break;
        case TreeKind::Fir:
            r.levels = 2; r.trunk_radius = 0.021f; r.crown_start = 0.06f;
            r.branches = {75, 11, 0}; r.angle = {98.0f, 58.0f, 0.0f}; r.length = {0.31f, 0.32f, 0.0f};
            r.droop = {0.28f, 0.12f, 0.0f}; r.up = {0.07f, 0.1f, 0.0f}; r.gnarl = 0.12f; r.shape = 0; r.whorl = 5;
            r.planar = true;
            r.style = kFirSprays; r.leaf_size = 0.58f; r.leaf_aspect = 0.9f; r.leaves = 4.2f; r.leaf_start = 0.04f;
            r.card_curl = 0.12f; r.card_fold = 0.1f;
            r.leaf_layer = kTreeLayerFirNeedles; r.bark_layer = kTreeLayerPineBark; r.bark_tile = 0.6f;
            r.flare = 0.45f; r.buttress = 0.35f; r.roots = 4; r.dead = 0.5f; r.moss = 0.15f; r.translucency = 0.4f;
            break;
        case TreeKind::Oak:
            r.levels = 3; r.trunk_radius = 0.055f; r.crown_start = 0.26f;
            r.branches = {6, 7, 7}; r.angle = {52.0f, 48.0f, 52.0f}; r.length = {0.62f, 0.55f, 0.5f};
            r.droop = {0.04f, 0.12f, 0.2f}; r.up = {0.24f, 0.12f, 0.08f}; r.gnarl = 0.95f; r.shape = 1;
            r.style = kBroadleaf; r.leaf_size = 0.52f; r.leaf_aspect = 1.0f; r.leaves = 7.0f; r.leaf_start = 0.2f;
            r.card_curl = 0.2f; r.card_fold = 0.22f;
            r.leaf_layer = kTreeLayerLeaves; r.bark_layer = kTreeLayerBark; r.bark_tile = 0.95f;
            r.flare = 0.75f; r.buttress = 0.9f; r.roots = 6; r.dead_inner = 0.06f; r.moss = 0.55f; r.translucency = 0.7f;
            break;
        case TreeKind::Birch:
            r.levels = 3; r.trunk_radius = 0.022f; r.crown_start = 0.32f;
            r.branches = {11, 7, 6}; r.angle = {36.0f, 46.0f, 55.0f}; r.length = {0.42f, 0.5f, 0.55f};
            r.droop = {0.1f, 0.4f, 0.9f}; r.up = {0.32f, 0.06f, 0.0f}; r.gnarl = 0.35f; r.shape = 2;
            r.style = kBroadleaf; r.leaf_size = 0.4f; r.leaf_aspect = 0.85f; r.leaves = 8.0f; r.leaf_start = 0.15f;
            r.card_curl = 0.3f; r.card_fold = 0.12f;
            r.leaf_layer = kTreeLayerBirchLeaves; r.bark_layer = kTreeLayerBirchBark; r.bark_tile = 0.55f;
            r.flare = 0.35f; r.buttress = 0.3f; r.roots = 3; r.dead = 0.35f; r.dead_inner = 0.05f; r.moss = 0.1f;
            r.translucency = 0.85f;
            break;
        case TreeKind::Willow:
            r.levels = 2; r.trunk_radius = 0.05f; r.crown_start = 0.28f;
            r.branches = {7, 9, 0}; r.angle = {44.0f, 40.0f, 0.0f}; r.length = {0.58f, 0.62f, 0.0f};
            r.droop = {0.08f, 0.3f, 0.0f}; r.up = {0.3f, 0.12f, 0.0f}; r.gnarl = 0.6f; r.shape = 1;
            r.style = kWillowCurtain; r.leaf_size = 0.95f; r.leaf_aspect = 0.34f; r.leaves = 2.4f; r.leaf_start = 0.12f;
            r.card_curl = 0.0f; r.card_fold = 0.0f;
            r.leaf_layer = kTreeLayerWillowLeaves; r.bark_layer = kTreeLayerBark; r.bark_tile = 0.9f;
            r.flare = 0.7f; r.buttress = 0.7f; r.roots = 5; r.dead_inner = 0.04f; r.moss = 0.45f; r.translucency = 0.8f;
            break;
        case TreeKind::Palm:
            r.levels = 0; r.trunk_radius = 0.022f; r.crown_start = 1.0f; r.gnarl = 0.0f;
            r.leaf_layer = kTreeLayerFrond; r.bark_layer = kTreeLayerPalmBark; r.bark_tile = 0.5f;
            r.flare = 0.5f; r.buttress = 0.0f; r.roots = 0; r.moss = 0.05f; r.translucency = 0.65f;
            break;
    }
    return r;
}

// Forma de la copa: cuanto miden las ramas del tronco a una altura relativa
// t (0 = donde empieza la copa, 1 = la punta).
float crownShape(int shape, float t) {
    switch (shape) {
        case 0: return 0.1f + 0.9f * std::pow(1.0f - t, 0.9f);                                    // cono (abeto)
        case 2: return 0.35f + 0.65f * std::sin(kPi * std::clamp(0.1f + 0.75f * t, 0.0f, 1.0f));  // llama
        case 3: return 0.4f + 0.6f * std::sin(kPi * std::clamp(0.2f + 0.8f * t, 0.0f, 1.0f));     // sombrilla
        default: return 0.3f + 0.7f * std::sin(kPi * std::clamp(0.12f + 0.88f * t, 0.0f, 1.0f)); // elipsoide
    }
}

// Dos vectores perpendiculares a `d`.
void perpendiculars(const Vec3& d, Vec3& a, Vec3& b) {
    const Vec3 helper = std::abs(d.y) < 0.95f ? Vec3{0.0f, 1.0f, 0.0f} : Vec3{1.0f, 0.0f, 0.0f};
    a = core::normalize(core::cross(helper, d));
    b = core::cross(d, a);
}

Vec3 catmull(const Vec3& p0, const Vec3& p1, const Vec3& p2, const Vec3& p3, float t) {
    const float t2 = t * t;
    const float t3 = t2 * t;
    return (p1 * 2.0f + (p2 - p0) * t + (p0 * 2.0f - p1 * 5.0f + p2 * 4.0f - p3) * t2 + (p1 * 3.0f - p0 - p2 * 3.0f + p3) * t3) * 0.5f;
}

struct Branch {
    std::vector<Vec3> points;
    std::vector<float> radii;
    int level = 0;
    float length = 0.0f;
    int pivot = -1;        // rama principal (nivel 1) que la lleva; -1 = el tronco
    float phase = 0.0f;    // del viento (0..1)
    float flex = 0.0f;     // del viento (0..1)
    bool dead = false;     // seca: sin hojas ni hijas
    bool cap = false;      // rota: se cierra con una tapa
    bool root = false;
    std::uint32_t seed = 0;  // para sus hojas
};

// Tarjeta de hojas (antes de elegir el nivel de detalle).
struct Card {
    Vec3 base;       // donde nace (en la ramita)
    Vec3 dir;        // hacia la punta
    Vec3 face;       // normal de la cara (el haz)
    float length = 0.5f;
    float aspect = 1.0f;
    float keep = 0.0f;   // 0..1: las de clave baja se quedan en los niveles lejanos
    Vec3 tint{1.0f, 1.0f, 1.0f};
    float translucency = 0.8f;
    int pivot = -1;
    float leaf_phase = 0.0f;
    std::uint32_t layer = kTreeLayerLeaves;
    float curl = 0.15f;
    float fold = 0.15f;
    int detail = 0;  // 0 en todos los niveles; 1 solo de cerca (nivel 0); 2 solo lejos (1 y 2)
};

struct Builder {
    const TreeSpecies& species;
    KindRules rules;
    int lod;
    std::mt19937 rng;
    TreeMeshData mesh;
    std::vector<Branch> branches;
    std::vector<Card> cards;
    std::vector<float> root_angles;
    std::vector<float> root_weights;
    std::size_t leaf_vertex_start = 0;

    Builder(const TreeSpecies& s, int l) : species(s), rules(rulesFor(s.kind)), lod(l), rng(s.seed * 7919U + 13U) {}

    float random(float lo, float hi) { return std::uniform_real_distribution<float>(lo, hi)(rng); }
    static float random(std::mt19937& r, float lo, float hi) { return std::uniform_real_distribution<float>(lo, hi)(r); }

    Vec3 randomUnit() {
        for (int i = 0; i < 16; ++i) {
            const Vec3 v{random(-1.0f, 1.0f), random(-1.0f, 1.0f), random(-1.0f, 1.0f)};
            const float l = core::length(v);
            if (l > 0.05f && l <= 1.0f) return v * (1.0f / l);
        }
        return Vec3{1.0f, 0.0f, 0.0f};
    }

    float windAt(float y) const { return std::pow(clamp01(y / std::max(species.height, 0.1f)), 1.5f); }

    // --- Esqueleto ---
    // Curva suave: la direccion cambia con un "deambular" que tiene inercia
    // (no un zigzag por tramo).
    Branch growBranch(Vec3 origin, Vec3 direction, float length, float radius, int level) {
        Branch b;
        b.level = level;
        b.length = length;
        const int segments = level == 0 ? 22 : (level == 1 ? 12 : (level == 2 ? 8 : 5));
        const float step = length / static_cast<float>(segments);
        Vec3 p = origin;
        Vec3 d = core::normalize(direction);
        const float gnarl = rules.gnarl * species.gnarl;
        const auto li = static_cast<std::size_t>(std::clamp(level - 1, 0, 2));
        Vec3 wander = randomUnit();
        const float k = step / std::max(length, 0.1f) * 3.0f;
        for (int i = 0; i <= segments; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(segments);
            b.points.push_back(p);
            float r = radius * std::pow(std::max(1.0f - 0.9f * t, 0.02f), level == 0 ? 0.85f : 1.05f);
            if (level > 0) r *= 1.0f + 0.3f * std::exp(-t * 12.0f);  // cuello donde nace
            b.radii.push_back(r);
            wander = core::normalize(wander * 0.75f + randomUnit() * 0.5f);
            const Vec3 side = core::normalize(wander - d * core::dot(wander, d));
            if (level == 0) {
                d = core::normalize(d + side * (0.05f * gnarl) + Vec3{0.0f, 0.035f, 0.0f});
            } else {
                d = core::normalize(d + Vec3{0.0f, -rules.droop[li] * (0.4f + t), 0.0f} * k +
                                    Vec3{0.0f, rules.up[li], 0.0f} * k + side * (0.2f * gnarl * std::sqrt(k)));
            }
            p = p + d * step;
        }
        return b;
    }

    void addDeadStub(const Branch& trunk, float y) {
        const float h = species.height;
        const float t = std::clamp(y / std::max(trunk.length, 0.1f), 0.0f, 0.97f);
        const float fi = t * static_cast<float>(trunk.points.size() - 1);
        const auto i0 = static_cast<std::size_t>(fi);
        const std::size_t i1 = std::min(i0 + 1, trunk.points.size() - 1);
        const float f = fi - static_cast<float>(i0);
        const Vec3 at = trunk.points[i0] * (1.0f - f) + trunk.points[i1] * f;
        const float parent_radius = trunk.radii[i0] * (1.0f - f) + trunk.radii[i1] * f;
        const float az = random(0.0f, 2.0f * kPi);
        const Vec3 dir = core::normalize(Vec3{std::cos(az), random(-0.35f, 0.15f), std::sin(az)});
        const float length = random(0.2f, 0.9f) * (h / 12.0f);
        Branch stub = growBranch(at, dir, length, std::max(parent_radius * random(0.12f, 0.25f), 0.012f), 2);
        // Rota: no se afina hasta la punta.
        for (std::size_t i = 0; i < stub.radii.size(); ++i) {
            const float tt = static_cast<float>(i) / static_cast<float>(stub.radii.size() - 1);
            stub.radii[i] = stub.radii[0] * (1.0f - 0.45f * tt) / 1.3f;
        }
        stub.radii[0] *= 1.3f;
        stub.dead = true;
        stub.cap = true;
        stub.seed = static_cast<std::uint32_t>(rng());
        branches.push_back(stub);
    }

    void spawnChildren(int parent_index, int level) {
        if (level > rules.levels) return;
        const auto li = static_cast<std::size_t>(level - 1);
        const Branch parent = branches[static_cast<std::size_t>(parent_index)];
        if (parent.dead) return;
        const float density = std::max(species.branch_density, 0.05f);
        const int count = static_cast<int>(std::round(static_cast<float>(rules.branches[li]) * density));
        if (count <= 0) return;
        const bool from_trunk = parent.level == 0;
        const float start = from_trunk ? rules.crown_start : 0.2f;
        const float azimuth0 = random(0.0f, 2.0f * kPi);
        const int whorl = from_trunk ? rules.whorl : 0;
        const int whorls = whorl > 0 ? std::max(1, count / whorl) : 1;
        for (int i = 0; i < count; ++i) {
            float t;
            float azimuth;
            if (whorl > 0) {
                const int w = std::min(i / whorl, whorls - 1);
                const int k = i % whorl;
                t = start + (1.0f - start) * (static_cast<float>(w) + 0.5f + random(-0.12f, 0.12f)) / static_cast<float>(whorls);
                azimuth = azimuth0 + static_cast<float>(w) * 1.1f + static_cast<float>(k) * 2.0f * kPi / static_cast<float>(whorl) +
                          random(-0.4f, 0.4f);
            } else {
                t = start + (1.0f - start) * (static_cast<float>(i) + random(0.1f, 0.9f)) / static_cast<float>(count);
                azimuth = azimuth0 + static_cast<float>(i) * kGolden + random(-0.3f, 0.3f);
            }
            t = std::clamp(t, 0.0f, 0.97f);
            const float fi = t * static_cast<float>(parent.points.size() - 1);
            const auto i0 = static_cast<std::size_t>(fi);
            const std::size_t i1 = std::min(i0 + 1, parent.points.size() - 1);
            const float f = fi - static_cast<float>(i0);
            const Vec3 at = parent.points[i0] * (1.0f - f) + parent.points[i1] * f;
            const float parent_radius = parent.radii[i0] * (1.0f - f) + parent.radii[i1] * f;
            const Vec3 pd = core::normalize(parent.points[i1] - parent.points[i0]);
            Vec3 around;
            if (rules.planar && !from_trunk) {
                // Abeto: ramitas alternas a los dos lados, en el plano de la rama.
                Vec3 side = core::cross(pd, Vec3{0.0f, 1.0f, 0.0f});
                if (core::length(side) < 1e-3f) side = Vec3{1.0f, 0.0f, 0.0f};
                side = core::normalize(side) * (i % 2 == 0 ? 1.0f : -1.0f) + Vec3{0.0f, random(-0.12f, 0.22f), 0.0f};
                around = core::normalize(side - pd * core::dot(side, pd));
            } else {
                Vec3 a;
                Vec3 b;
                perpendiculars(pd, a, b);
                around = a * std::cos(azimuth) + b * std::sin(azimuth);
            }
            const float incline = (rules.angle[li] + random(-8.0f, 8.0f)) * kPi / 180.0f;
            const Vec3 dir = core::normalize(pd * std::cos(incline) + around * std::sin(incline));
            float length = parent.length * rules.length[li] * random(0.8f, 1.2f);
            if (from_trunk) {
                const float rel = (t - rules.crown_start) / std::max(1.0f - rules.crown_start, 0.05f);
                length = species.height * rules.length[0] * crownShape(rules.shape, rel) * random(0.85f, 1.15f);
            } else {
                length *= 1.0f - 0.5f * t;  // las de la punta del padre, mas cortas
            }
            const bool dead = !from_trunk ? random(0.0f, 1.0f) < rules.dead_inner : false;
            if (dead) length *= random(0.3f, 0.6f);
            const float radius = std::min(parent_radius * 0.72f,
                                          parent_radius * std::pow(length / std::max(parent.length, 0.1f), 0.9f) + 0.004f);
            Branch child = growBranch(at, dir, length, std::max(radius, 0.006f), level);
            child.dead = dead;
            child.cap = dead;
            child.seed = static_cast<std::uint32_t>(rng());
            child.phase = from_trunk ? random(0.0f, 1.0f) : parent.phase;
            child.flex = from_trunk ? std::clamp(length / std::max(species.height, 1.0f) * 1.6f, 0.25f, 1.0f) : parent.flex;
            branches.push_back(std::move(child));
            const int ci = static_cast<int>(branches.size()) - 1;
            branches.back().pivot = from_trunk ? ci : parent.pivot;
            spawnChildren(ci, level + 1);
        }
        if (from_trunk && rules.dead > 0.0f) {
            // Ramas secas rotas en el fuste, bajo la copa.
            const float h = species.height;
            const float y0 = std::min(1.6f, h * 0.15f);
            const float y1 = rules.crown_start * h;
            const int stubs = static_cast<int>(std::round(rules.dead * std::max(y1 - y0, 0.0f) * density));
            for (int s = 0; s < stubs; ++s) addDeadStub(parent, random(y0, y1 + 0.5f));
        }
    }

    // --- Corteza ---
    Vec3 pivotOf(const Branch& b) const {
        if (b.pivot < 0) return Vec3{};
        return branches[static_cast<std::size_t>(b.pivot)].points.front();
    }

    // Tubo organico: curva de Catmull-Rom, seccion irregular, ensanche,
    // contrafuertes en el pie del tronco y UV sin estirar.
    // `stride`: lejos se toma un punto del esqueleto de cada `stride` (la
    // curva con menos tramos; los extremos siempre).
    void tube(const Branch& b, int sides, int subdiv, int stride = 1) {
        std::vector<Vec3> skeleton;
        std::vector<float> skeleton_radii;
        const std::size_t total = b.points.size();
        const auto step = static_cast<std::size_t>(std::max(stride, 1));
        for (std::size_t i = 0; i < total; i += step) {
            skeleton.push_back(b.points[i]);
            skeleton_radii.push_back(b.radii[i]);
        }
        if (total > 0 && (total - 1) % step != 0) {
            skeleton.push_back(b.points.back());
            skeleton_radii.push_back(b.radii.back());
        }
        const std::size_t n = skeleton.size();
        if (n < 2) return;
        std::vector<Vec3> pts;
        std::vector<float> rad;
        for (std::size_t i = 0; i + 1 < n; ++i) {
            const Vec3& p0 = skeleton[i == 0 ? 0 : i - 1];
            const Vec3& p1 = skeleton[i];
            const Vec3& p2 = skeleton[i + 1];
            const Vec3& p3 = skeleton[std::min(i + 2, n - 1)];
            for (int s = 0; s < subdiv; ++s) {
                const float f = static_cast<float>(s) / static_cast<float>(subdiv);
                pts.push_back(catmull(p0, p1, p2, p3, f));
                rad.push_back(mixF(skeleton_radii[i], skeleton_radii[i + 1], f));
            }
        }
        pts.push_back(skeleton.back());
        rad.push_back(skeleton_radii.back());
        const std::size_t rows = pts.size();

        const bool trunk = b.level == 0 && !b.root;
        const float h = species.height;
        const std::uint32_t layer = rules.bark_layer;
        const float base_circ = 2.0f * kPi * std::max(rad[0], 0.004f);
        const int repeats = std::max(1, static_cast<int>(std::round(base_circ / rules.bark_tile)));
        // Fases de la seccion irregular de la semilla de la rama (no del
        // generador comun): iguales en todos los niveles de detalle.
        std::mt19937 pr(b.seed * 2654435761U + 17U);
        const float ph1 = random(pr, 0.0f, 6.28f), ph2 = random(pr, 0.0f, 6.28f), ph3 = random(pr, 0.0f, 6.28f);
        const float irregular = trunk ? 0.07f : (b.level == 1 ? 0.06f : 0.04f);
        const Vec3 pivot = pivotOf(b);
        // Misma fase y flexibilidad que sus hojas: la rama gira entera (coherente).
        const float flex = b.pivot < 0 ? 0.0f : b.flex;

        // Anillos: posiciones con un marco que se transporta sin girar.
        std::vector<Vec3> grid(rows * static_cast<std::size_t>(sides + 1));
        std::vector<float> vcoord(rows, 0.0f);
        Vec3 prev_a{};
        float arc = 0.0f;
        for (std::size_t i = 0; i < rows; ++i) {
            const Vec3 d = core::normalize(i + 1 < rows ? pts[i + 1] - pts[i] : pts[i] - pts[i - 1]);
            Vec3 a;
            Vec3 c;
            perpendiculars(d, a, c);
            if (i > 0) {
                a = core::normalize(prev_a - d * core::dot(prev_a, d));
                c = core::cross(d, a);
            }
            prev_a = a;
            if (i > 0) {
                const float ds = core::length(pts[i] - pts[i - 1]);
                arc += ds;
                // Texel cuadrado: la v avanza como la circunferencia local.
                const float circ = 2.0f * kPi * std::max(rad[i], 0.015f);
                vcoord[i] = vcoord[i - 1] + ds * static_cast<float>(repeats) / std::max(circ, 0.12f);
            }
            const float y = pts[i].y;
            const float s = arc / std::max(rad[0] * 6.0f, 0.05f);
            for (int k = 0; k <= sides; ++k) {
                const float theta = 2.0f * kPi * static_cast<float>(k % sides) / static_cast<float>(sides);
                const Vec3 radial = a * std::cos(theta) + c * std::sin(theta);
                float r = rad[i] * (1.0f + irregular * (0.55f * std::sin(2.0f * theta + ph1 + s * 0.7f) +
                                                        0.3f * std::sin(3.0f * theta + ph2 - s * 1.1f) +
                                                        0.15f * std::sin(5.0f * theta + ph3 + s * 2.3f)));
                if (trunk) {
                    const float low = std::exp(-std::max(y, 0.0f) / std::max(0.065f * h, 0.3f));
                    float butt = 0.0f;
                    for (std::size_t j = 0; j < root_angles.size(); ++j) {
                        const float cd = std::cos(theta - root_angles[j]);
                        butt += root_weights[j] * std::pow(std::max(cd, 0.0f), 7.0f);
                    }
                    r *= 1.0f + rules.flare * low * 0.7f + rules.buttress * butt * low * low * 1.3f;
                }
                grid[i * static_cast<std::size_t>(sides + 1) + static_cast<std::size_t>(k)] = pts[i] + radial * r;
            }
        }
        const auto first = static_cast<std::uint32_t>(mesh.vertices.size());
        const auto row = static_cast<std::size_t>(sides + 1);
        for (std::size_t i = 0; i < rows; ++i) {
            for (int k = 0; k <= sides; ++k) {
                const Vec3& p = grid[i * row + static_cast<std::size_t>(k)];
                const int kp = (k + 1) % sides;
                const int km = (k + sides - 1) % sides;
                const Vec3 dtheta = grid[i * row + static_cast<std::size_t>(kp)] - grid[i * row + static_cast<std::size_t>(km)];
                const std::size_t ip = std::min(i + 1, rows - 1);
                const std::size_t im = i == 0 ? 0 : i - 1;
                const Vec3 dlen = grid[ip * row + static_cast<std::size_t>(k % sides)] - grid[im * row + static_cast<std::size_t>(k % sides)];
                Vec3 nrm = core::normalize(core::cross(dtheta, dlen));
                const Vec3 outward = p - pts[i];
                if (core::dot(nrm, outward) < 0.0f) nrm = -nrm;
                if (core::length(nrm) < 0.5f) nrm = core::normalize(outward);
                // Oclusion base: la union con el padre y el suelo (la de la copa se suma despues).
                const float t = static_cast<float>(i) / static_cast<float>(rows - 1);
                float ao = 1.0f;
                if (!trunk && !b.root) ao *= mixF(0.6f, 1.0f, smoothstepF(0.0f, 0.12f, t));
                ao *= mixF(0.45f, 1.0f, smoothstepF(-0.3f, 0.9f, p.y));
                // Extra: musgo (arriba de las ramas y en el pie) o pie oscuro del abedul.
                float extra;
                if (rules.bark_layer == kTreeLayerBirchBark) {
                    extra = trunk ? 1.0f - smoothstepF(0.2f, 0.11f * h + 0.6f, p.y + 0.4f * (valueNoise(static_cast<float>(k) * 0.6f, p.y * 2.0f, 9999, 9999, 77U) - 0.5f)) : 0.0f;
                } else {
                    const float upf = clamp01(nrm.y * 0.8f + 0.15f);
                    const float foot = std::exp(-std::max(p.y, 0.0f) / 1.3f);
                    extra = rules.moss * clamp01(upf * 0.75f + foot * 0.9f) * (b.level >= 3 ? 0.3f : 1.0f);
                }
                if (b.dead) extra = std::max(extra, 0.2f);
                // Corteza joven (0..1): lisa en las ramas finas; en el pino,
                // ademas, la parte alta del tronco (anaranjada, en laminas).
                // (El abeto comparte la textura del pino pero no se vuelve
                // naranja: el shader distingue el pino por esto.)
                float young = 0.0f;
                if (species.kind == TreeKind::Fir) {
                    young = 0.0f;
                } else if (trunk) {
                    if (species.kind == TreeKind::Pine) {
                        young = smoothstepF(rules.crown_start * h * 0.7f, rules.crown_start * h * 1.05f, p.y);
                    }
                } else if (!b.root && !b.dead) {
                    young = 1.0f - smoothstepF(h * 0.0025f, h * 0.0065f, rad[i]);
                    if (species.kind == TreeKind::Pine) young = std::max(young, 0.6f);
                }
                const float wind = windAt(p.y) * (b.root ? 0.0f : 1.0f);
                const float u = static_cast<float>(k) / static_cast<float>(sides) * static_cast<float>(repeats);
                mesh.vertices.push_back({p.x, p.y, p.z, nrm.x, nrm.y, nrm.z, pack4(species.bark_color.x * 0.5f, species.bark_color.y * 0.5f, species.bark_color.z * 0.5f, ao),
                                         wind, u, vcoord[i], static_cast<float>(layer), 0.0f, pivot.x, pivot.y, pivot.z,
                                         pack4(b.phase, flex * 0.6f, young, extra)});
            }
        }
        for (std::uint32_t i = 0; i + 1 < rows; ++i) {
            for (std::uint32_t k = 0; k < static_cast<std::uint32_t>(sides); ++k) {
                const std::uint32_t p0 = first + i * static_cast<std::uint32_t>(row) + k;
                const std::uint32_t p1 = p0 + static_cast<std::uint32_t>(row);
                mesh.indices.insert(mesh.indices.end(), {p0, p1, p0 + 1, p0 + 1, p1, p1 + 1});
            }
        }
        if (b.cap) {
            // Tapa de la rama rota: un abanico hacia un punto algo salido.
            const Vec3 d = core::normalize(pts[rows - 1] - pts[rows - 2]);
            const Vec3 tip = pts[rows - 1] + d * (rad[rows - 1] * 0.35f);
            const auto center = static_cast<std::uint32_t>(mesh.vertices.size());
            const TreeVertex& ref = mesh.vertices[first + static_cast<std::uint32_t>((rows - 1) * row)];
            TreeVertex cv = ref;
            cv.px = tip.x;
            cv.py = tip.y;
            cv.pz = tip.z;
            cv.nx = d.x;
            cv.ny = d.y;
            cv.nz = d.z;
            cv.u = 0.5f;
            cv.v = ref.v + 0.05f;
            mesh.vertices.push_back(cv);
            const std::uint32_t last = first + static_cast<std::uint32_t>((rows - 1) * row);
            for (std::uint32_t k = 0; k < static_cast<std::uint32_t>(sides); ++k) {
                mesh.indices.insert(mesh.indices.end(), {last + k, center, last + k + 1});
            }
        }
    }

    // Raices que salen de los contrafuertes y se hunden en el suelo.
    void buildRoots(const Branch& trunk) {
        const float r0 = trunk.radii.front();
        for (std::size_t j = 0; j < root_angles.size(); ++j) {
            const float a = root_angles[j];
            const Vec3 radial{std::cos(a), 0.0f, std::sin(a)};
            const Vec3 start = Vec3{0.0f, 0.22f + 0.2f * root_weights[j], 0.0f} + radial * (r0 * 0.75f);
            const float drop = random(0.32f, 0.55f);
            const Vec3 dir = core::normalize(radial + Vec3{0.0f, -drop, 0.0f});
            const float length = r0 * random(3.2f, 6.0f) * (0.6f + 0.5f * root_weights[j]);
            Branch root = growBranch(start, dir, length, r0 * (0.28f + 0.14f * root_weights[j]), 2);
            // Se hunde: cada tramo un poco mas hacia abajo.
            for (std::size_t i = 1; i < root.points.size(); ++i) {
                const float t = static_cast<float>(i) / static_cast<float>(root.points.size() - 1);
                root.points[i].y -= t * t * length * 0.25f;
                root.radii[i] = root.radii[0] * std::pow(1.0f - 0.85f * t, 1.3f);
            }
            root.root = true;
            root.pivot = -1;
            root.seed = 1000U + static_cast<std::uint32_t>(j) * 77U;
            tube(root, lod == 0 ? 8 : 5, lod == 0 ? 2 : 1);
        }
    }

    // --- Hojas ---
    // Tarjeta curvada: `rows` filas a lo largo (cae por la gravedad) y
    // `cols` columnas (2 = doblada en V por el nervio).
    Vec3 crown_center{};
    Vec3 crown_radii{3.0f, 3.0f, 3.0f};

    void emitCard(const Card& c, float size_scale, int rows, int cols) {
        const float length = c.length * size_scale;
        const float width = length * c.aspect;
        Vec3 side = core::cross(c.face, c.dir);
        if (core::length(side) < 1e-4f) {
            Vec3 x;
            Vec3 y;
            perpendiculars(c.dir, x, y);
            side = x;
        }
        side = core::normalize(side);
        const Vec3 face = core::normalize(core::cross(c.dir, side));
        const auto first = static_cast<std::uint32_t>(mesh.vertices.size());
        const int ncol = cols + 1;
        std::vector<Vec3> pos(static_cast<std::size_t>((rows + 1) * ncol));
        for (int r = 0; r <= rows; ++r) {
            const float s = static_cast<float>(r) / static_cast<float>(rows);
            for (int k = 0; k < ncol; ++k) {
                const float x = static_cast<float>(k) / static_cast<float>(cols) * 2.0f - 1.0f;
                Vec3 p = c.base + c.dir * (s * length) + side * (x * width * 0.5f) + face * (c.fold * std::abs(x) * width * 0.5f);
                p = p + Vec3{0.0f, -1.0f, 0.0f} * (c.curl * length * s * s) + face * (-0.5f * c.curl * length * s * s);
                pos[static_cast<std::size_t>(r * ncol + k)] = p;
            }
        }
        const Vec3 center = c.base + c.dir * (length * 0.5f);
        Vec3 outward = center - crown_center;
        outward = Vec3{outward.x / (crown_radii.x * crown_radii.x), outward.y / (crown_radii.y * crown_radii.y),
                       outward.z / (crown_radii.z * crown_radii.z)};
        outward = core::length(outward) > 1e-6f ? core::normalize(outward) : Vec3{0.0f, 1.0f, 0.0f};
        const Vec3 pivot = c.pivot >= 0 ? branches[static_cast<std::size_t>(c.pivot)].points.front() : Vec3{};
        const float phase = c.pivot >= 0 ? branches[static_cast<std::size_t>(c.pivot)].phase : 0.0f;
        const float flex = c.pivot >= 0 ? branches[static_cast<std::size_t>(c.pivot)].flex : 0.0f;
        for (int r = 0; r <= rows; ++r) {
            const float s = static_cast<float>(r) / static_cast<float>(rows);
            for (int k = 0; k < ncol; ++k) {
                const Vec3& p = pos[static_cast<std::size_t>(r * ncol + k)];
                const Vec3 dr = pos[static_cast<std::size_t>(std::min(r + 1, rows) * ncol + k)] - pos[static_cast<std::size_t>(std::max(r - 1, 0) * ncol + k)];
                const Vec3 dk = pos[static_cast<std::size_t>(r * ncol + std::min(k + 1, cols))] - pos[static_cast<std::size_t>(r * ncol + std::max(k - 1, 0))];
                Vec3 fn = core::normalize(core::cross(dk, dr));
                if (core::dot(fn, face) < 0.0f) fn = -fn;
                // Normal entre la de la tarjeta y la de la copa (volumen).
                const Vec3 n = core::normalize(fn * 0.4f + outward * 0.6f + Vec3{0.0f, 0.08f, 0.0f});
                const float x = static_cast<float>(k) / static_cast<float>(cols) * 2.0f - 1.0f;
                const float flutter = 0.03f + 0.97f * clamp01(s * (0.75f + 0.25f * std::abs(x)));
                mesh.vertices.push_back({p.x, p.y, p.z, n.x, n.y, n.z, pack4(c.tint.x * 0.5f, c.tint.y * 0.5f, c.tint.z * 0.5f, 1.0f),
                                         windAt(p.y), x * 0.5f + 0.5f, 1.0f - s, static_cast<float>(c.layer), flutter,
                                         pivot.x, pivot.y, pivot.z, pack4(phase, flex * 0.6f, c.leaf_phase, c.translucency)});
            }
        }
        for (int r = 0; r < rows; ++r) {
            for (int k = 0; k < cols; ++k) {
                const std::uint32_t p0 = first + static_cast<std::uint32_t>(r * ncol + k);
                const std::uint32_t p1 = p0 + static_cast<std::uint32_t>(ncol);
                mesh.indices.insert(mesh.indices.end(), {p0, p0 + 1, p1, p0 + 1, p1 + 1, p1});
            }
        }
    }

    Card makeCard(std::mt19937& r, const Vec3& base, const Vec3& dir, const Vec3& face, float length, const Branch& owner) {
        Card c;
        c.base = base;
        c.dir = core::normalize(dir);
        c.face = core::normalize(face);
        c.length = length;
        c.aspect = rules.leaf_aspect;
        c.keep = random(r, 0.0f, 1.0f);
        c.pivot = owner.pivot;
        c.leaf_phase = random(r, 0.0f, 1.0f);
        c.layer = rules.leaf_layer;
        c.curl = rules.card_curl * random(r, 0.6f, 1.4f);
        c.fold = rules.card_fold * random(r, 0.6f, 1.4f);
        c.translucency = rules.translucency * random(r, 0.8f, 1.0f);
        // Tono de cada tarjeta: casi igual, alguna mas clara o amarillenta.
        const float tone = random(r, 0.88f, 1.1f);
        Vec3 tint = species.leaf_color * tone;
        if (random(r, 0.0f, 1.0f) < 0.05f && rules.style != kFirSprays) tint = tint * Vec3{1.18f, 1.08f, 0.62f};
        c.tint = tint;
        return c;
    }

    // Borla de pino. Lejos (niveles 1 y 2): dos tarjetas cruzadas con el
    // abanico de brotes (barato; a esa distancia se ve igual). Cerca (nivel
    // 0): los brotes de verdad, cada uno un cepillo de agujas (dos tarjetas
    // estrechas cruzadas) que sale en su direccion desde la punta de la
    // ramita: un volumen, no un abanico plano.
    void pineTuft(std::mt19937& r, const Vec3& base, const Vec3& dir, float size, float keep, const Branch& b) {
        const Vec3 up{0.0f, 1.0f, 0.0f};
        Vec3 x;
        Vec3 y;
        perpendiculars(dir, x, y);
        const float roll = random(r, 0.0f, kPi);
        for (int q = 0; q < 2; ++q) {
            const float rr = roll + static_cast<float>(q) * kPi * 0.5f;
            Card c = makeCard(r, base - dir * (size * 0.08f), dir, x * std::cos(rr) + y * std::sin(rr), size, b);
            c.keep = keep;
            c.detail = 2;
            cards.push_back(c);
        }
        // Brotes cortos y gordos (agujas de 5-7 cm en un brote de ~20 cm: un
        // cilindro esponjoso) que salen a lo largo de la punta de la ramita y
        // en todas direcciones: juntos son una bola de agujas, no una mano.
        const int shoots = 7;
        const float az0 = random(r, 0.0f, 2.0f * kPi);
        for (int k = 0; k < shoots; ++k) {
            Vec3 sdir;
            float length;
            Vec3 start = base;
            if (k == 0) {
                // El brote guia, hacia fuera y algo hacia arriba.
                sdir = core::normalize(dir + up * 0.25f);
                length = size * 0.42f;
                start = base + dir * (size * 0.12f);
            } else {
                // Los demas, alrededor (algunos de lado y alguno algo hacia
                // abajo), curvados hacia la luz.
                const float az = az0 + static_cast<float>(k - 1) * (2.0f * kPi / static_cast<float>(shoots - 1)) + random(r, -0.35f, 0.35f);
                const float tilt = random(r, 0.45f, 1.35f);
                sdir = core::normalize(dir * std::cos(tilt) + (x * std::cos(az) + y * std::sin(az)) * std::sin(tilt) + up * 0.25f);
                length = size * random(r, 0.28f, 0.36f);
                start = base + dir * (size * random(r, -0.06f, 0.18f));
            }
            Vec3 a;
            Vec3 bb;
            perpendiculars(sdir, a, bb);
            const float shoot_roll = random(r, 0.0f, kPi);
            for (int q = 0; q < 2; ++q) {
                const float rr = shoot_roll + static_cast<float>(q) * kPi * 0.5f;
                Card c = makeCard(r, start, sdir, a * std::cos(rr) + bb * std::sin(rr), length, b);
                c.layer = kTreeLayerPineShoot;
                c.aspect = 0.7f;
                c.curl = 0.0f;
                c.fold = 0.0f;
                c.keep = keep;
                c.detail = 1;
                cards.push_back(c);
            }
        }
    }

    // Todas las tarjetas del arbol (las mismas en los tres niveles: lejos se
    // quedan las de clave mas baja, mas grandes).
    void placeLeaves() {
        int deepest = 0;
        for (const Branch& b : branches) {
            if (!b.root && !b.dead) deepest = std::max(deepest, b.level);
        }
        const float base_size = rules.leaf_size * species.leaf_size * std::sqrt(species.height / 11.0f);
        const float density = std::max(species.leaf_density, 0.05f);
        const Vec3 up{0.0f, 1.0f, 0.0f};
        for (const Branch& b : branches) {
            if (b.dead || b.root || b.level == 0) continue;
            const bool twig = b.level == deepest;
            const bool tip_of_parent = b.level == deepest - 1;
            if (!twig && !tip_of_parent) continue;
            std::mt19937 r(b.seed ^ 0x9e3779b9U);
            const std::size_t n = b.points.size();
            const auto pointAt = [&](float t, Vec3& p, Vec3& d) {
                const float fi = std::clamp(t, 0.0f, 1.0f) * static_cast<float>(n - 1);
                const auto i0 = static_cast<std::size_t>(fi);
                const std::size_t i1 = std::min(i0 + 1, n - 1);
                const float f = fi - static_cast<float>(i0);
                p = b.points[i0] * (1.0f - f) + b.points[i1] * f;
                d = core::normalize(b.points[std::max<std::size_t>(i1, 1)] - b.points[std::max<std::size_t>(i1, 1) - 1]);
            };
            const float from = twig ? rules.leaf_start : 0.72f;
            const float span = b.length * (1.0f - from);
            if (rules.style == kWillowCurtain) {
                // Cortinas: cordones que cuelgan de las ramas.
                const int strands = std::max(1, static_cast<int>(std::round(span * rules.leaves * density)));
                for (int s = 0; s < strands; ++s) {
                    Vec3 p;
                    Vec3 d;
                    pointAt(from + (1.0f - from) * (static_cast<float>(s) + random(r, 0.0f, 1.0f)) / static_cast<float>(strands), p, d);
                    Vec3 out{p.x, 0.0f, p.z};
                    out = core::length(out) > 1e-3f ? core::normalize(out) : Vec3{1.0f, 0.0f, 0.0f};
                    const float hang = species.height * random(r, 0.18f, 0.42f) * (0.6f + 0.4f * clamp01(p.y / species.height));
                    const float card = base_size * random(r, 0.85f, 1.15f);
                    const int pieces = std::max(1, static_cast<int>(std::round(hang / card)));
                    Vec3 at = p;
                    const float roll = random(r, 0.0f, 2.0f * kPi);
                    for (int k = 0; k < pieces; ++k) {
                        const Vec3 dir = core::normalize(Vec3{0.0f, -1.0f, 0.0f} + out * (0.25f - 0.06f * static_cast<float>(k)) + d * 0.1f);
                        Vec3 a;
                        Vec3 bb;
                        perpendiculars(dir, a, bb);
                        const float rr = roll + random(r, -0.5f, 0.5f);
                        const Vec3 face = a * std::cos(rr) + bb * std::sin(rr);
                        Card c = makeCard(r, at, dir, face, card, b);
                        c.keep = k == 0 ? c.keep * 0.6f : c.keep;
                        cards.push_back(c);
                        at = at + dir * (card * 0.92f);
                    }
                }
                continue;
            }
            const float spacing = 1.0f / std::max(rules.leaves * density, 0.1f);
            const int count = std::max(1, static_cast<int>(std::round(span / spacing)));
            for (int k = 0; k < count; ++k) {
                const float t = from + (1.0f - from) * std::sqrt((static_cast<float>(k) + random(r, 0.0f, 1.0f)) / static_cast<float>(count));
                Vec3 p;
                Vec3 d;
                pointAt(t, p, d);
                Vec3 a;
                Vec3 bb;
                perpendiculars(d, a, bb);
                const float az = random(r, 0.0f, 2.0f * kPi);
                const Vec3 around = a * std::cos(az) + bb * std::sin(az);
                const float size = base_size * random(r, 0.8f, 1.2f);
                if (rules.style == kPineTufts) {
                    // Borlas que salen de la ramita hacia fuera y hacia arriba.
                    const Vec3 dir = core::normalize(d * 0.7f + up * 0.35f + around * 0.55f);
                    const float keep = random(r, 0.0f, 1.0f);
                    pineTuft(r, p, dir, size, keep, b);
                } else if (rules.style == kFirSprays) {
                    // Capas planas: la ramita aplanada en horizontal, cayendo un poco.
                    Vec3 dir = core::normalize(d + around * 0.35f + Vec3{0.0f, -0.08f, 0.0f});
                    Vec3 side = core::cross(dir, up);
                    side = core::length(side) > 1e-3f ? core::normalize(side) : a;
                    Vec3 face = core::normalize(core::cross(side, dir));
                    if (face.y < 0.0f) face = -face;
                    const float roll = random(r, -0.35f, 0.35f) + (random(r, 0.0f, 1.0f) < 0.25f ? 1.1f : 0.0f);
                    face = core::normalize(face * std::cos(roll) + side * std::sin(roll));
                    cards.push_back(makeCard(r, p - dir * (size * 0.1f), dir, face, size, b));
                } else {
                    // Hojas anchas: racimos que salen de la ramita hacia fuera y
                    // arriba, con el haz hacia la luz.
                    const float droop = rules.droop[2] * 0.35f;
                    const Vec3 dir = core::normalize(d * 0.55f + around * 0.8f + up * (0.2f - droop) + Vec3{random(r, -0.2f, 0.2f), 0.0f, random(r, -0.2f, 0.2f)});
                    Vec3 out = p - crown_center;
                    out = core::length(out) > 1e-3f ? core::normalize(out) : up;
                    Vec3 face = core::normalize(up * 0.6f + out * 0.45f + around * 0.3f + Vec3{random(r, -0.3f, 0.3f), 0.0f, random(r, -0.3f, 0.3f)});
                    cards.push_back(makeCard(r, p, dir, face, size, b));
                }
            }
            if (twig && rules.style != kWillowCurtain) {
                // La punta de la ramita.
                Vec3 p;
                Vec3 d;
                pointAt(1.0f, p, d);
                const float size = base_size * random(r, 0.9f, 1.15f);
                Vec3 face = core::normalize(up * 0.7f + Vec3{random(r, -0.4f, 0.4f), 0.0f, random(r, -0.4f, 0.4f)});
                if (rules.style == kPineTufts) {
                    const float keep = random(r, 0.0f, 0.5f);
                    pineTuft(r, p - d * (size * 0.3f), core::normalize(d + up * 0.3f), size, keep, b);
                } else {
                    Card c = makeCard(r, p - d * (size * 0.15f), d, face, size, b);
                    c.keep *= 0.5f;  // las puntas marcan la silueta: lejos se quedan
                    cards.push_back(c);
                }
            }
        }
    }

    void computeCrown() {
        // Elipsoide de la copa con las puntas de las ramas.
        Vec3 lo{1e9f, 1e9f, 1e9f};
        Vec3 hi{-1e9f, -1e9f, -1e9f};
        bool any = false;
        for (const Branch& b : branches) {
            if (b.level == 0 || b.root) continue;
            for (const Vec3& p : b.points) {
                lo = Vec3{std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
                hi = Vec3{std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
                any = true;
            }
        }
        if (!any) {
            crown_center = Vec3{0.0f, species.height * 0.7f, 0.0f};
            crown_radii = Vec3{species.height * 0.3f, species.height * 0.3f, species.height * 0.3f};
            return;
        }
        crown_center = Vec3{0.0f, (lo.y + hi.y) * 0.5f, 0.0f};
        const float rxz = std::max(std::max(std::abs(lo.x), std::abs(hi.x)), std::max(std::abs(lo.z), std::abs(hi.z)));
        crown_radii = Vec3{std::max(rxz, 0.5f), std::max((hi.y - lo.y) * 0.5f, 0.5f), std::max(rxz, 0.5f)};
    }

    // Oclusion de la copa: rejilla con la densidad de hojas y rayos hacia el
    // cielo desde cada celda (el interior y la parte de abajo, oscuros).
    void crownOcclusion(const std::vector<std::pair<Vec3, float>>& leaf_area) {
        if (leaf_area.empty() || mesh.vertices.empty()) return;
        Vec3 lo{1e9f, 1e9f, 1e9f};
        Vec3 hi{-1e9f, -1e9f, -1e9f};
        for (const TreeVertex& v : mesh.vertices) {
            lo = Vec3{std::min(lo.x, v.px), std::min(lo.y, v.py), std::min(lo.z, v.pz)};
            hi = Vec3{std::max(hi.x, v.px), std::max(hi.y, v.py), std::max(hi.z, v.pz)};
        }
        const Vec3 ext = hi - lo;
        const float cell = std::max({ext.x, ext.y, ext.z, 1.0f}) / 22.0f;
        const int nx = std::max(1, static_cast<int>(std::ceil(ext.x / cell)) + 1);
        const int ny = std::max(1, static_cast<int>(std::ceil(ext.y / cell)) + 1);
        const int nz = std::max(1, static_cast<int>(std::ceil(ext.z / cell)) + 1);
        const auto idx = [&](int x, int y, int z) { return (static_cast<std::size_t>(z) * ny + y) * nx + x; };
        std::vector<float> tau(static_cast<std::size_t>(nx) * ny * nz, 0.0f);
        for (const auto& [p, area] : leaf_area) {
            const int x = std::clamp(static_cast<int>((p.x - lo.x) / cell), 0, nx - 1);
            const int y = std::clamp(static_cast<int>((p.y - lo.y) / cell), 0, ny - 1);
            const int z = std::clamp(static_cast<int>((p.z - lo.z) / cell), 0, nz - 1);
            // Densidad de area foliar (m2/m3) x 0.5 (orientacion al azar) x
            // 0.5 (huecos del recorte) x el lado de la celda = espesor optico.
            tau[idx(x, y, z)] += area * 0.25f / (cell * cell);
        }
        // Direcciones hacia el cielo (cenit, 45 y 12 grados) con su peso.
        std::vector<std::pair<Vec3, float>> dirs;
        dirs.push_back({Vec3{0.0f, 1.0f, 0.0f}, 1.0f});
        for (int i = 0; i < 6; ++i) {
            const float a = static_cast<float>(i) * kPi / 3.0f;
            dirs.push_back({core::normalize(Vec3{std::cos(a), 1.0f, std::sin(a)}), 0.8f});
            const float b = a + kPi / 6.0f;
            dirs.push_back({core::normalize(Vec3{std::cos(b), 0.22f, std::sin(b)}), 0.45f});
        }
        float wsum = 0.0f;
        for (const auto& d : dirs) wsum += d.second;
        std::vector<float> ao(tau.size(), 1.0f);
        for (int z = 0; z < nz; ++z) {
            for (int y = 0; y < ny; ++y) {
                for (int x = 0; x < nx; ++x) {
                    float sum = 0.0f;
                    for (const auto& [d, w] : dirs) {
                        float depth = tau[idx(x, y, z)] * 0.5f;
                        Vec3 p{static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f, static_cast<float>(z) + 0.5f};
                        for (int s = 0; s < 64; ++s) {
                            p = p + d;
                            const int ix = static_cast<int>(std::floor(p.x));
                            const int iy = static_cast<int>(std::floor(p.y));
                            const int iz = static_cast<int>(std::floor(p.z));
                            if (ix < 0 || iy < 0 || iz < 0 || ix >= nx || iy >= ny || iz >= nz) break;
                            depth += tau[idx(ix, iy, iz)];
                        }
                        sum += w * std::exp(-depth);
                    }
                    ao[idx(x, y, z)] = sum / wsum;
                }
            }
        }
        const auto sample = [&](const Vec3& p) {
            const float fx = std::clamp((p.x - lo.x) / cell - 0.5f, 0.0f, static_cast<float>(nx - 1));
            const float fy = std::clamp((p.y - lo.y) / cell - 0.5f, 0.0f, static_cast<float>(ny - 1));
            const float fz = std::clamp((p.z - lo.z) / cell - 0.5f, 0.0f, static_cast<float>(nz - 1));
            const int x0 = static_cast<int>(fx), y0 = static_cast<int>(fy), z0 = static_cast<int>(fz);
            const int x1 = std::min(x0 + 1, nx - 1), y1 = std::min(y0 + 1, ny - 1), z1 = std::min(z0 + 1, nz - 1);
            const float tx = fx - static_cast<float>(x0), ty = fy - static_cast<float>(y0), tz = fz - static_cast<float>(z0);
            const auto lerp2 = [&](int z) {
                const float a = mixF(ao[idx(x0, y0, z)], ao[idx(x1, y0, z)], tx);
                const float b = mixF(ao[idx(x0, y1, z)], ao[idx(x1, y1, z)], tx);
                return mixF(a, b, ty);
            };
            return mixF(lerp2(z0), lerp2(z1), tz);
        };
        for (TreeVertex& v : mesh.vertices) {
            const float occ = sample(Vec3{v.px, v.py, v.pz});
            const float old = static_cast<float>(v.color >> 24) / 255.0f;
            const float leaf = v.flutter > 0.0f ? 1.0f : 0.0f;
            const float a = old * mixF(0.1f, 1.0f, std::pow(occ, leaf > 0.5f ? 0.85f : 0.7f));
            v.color = (v.color & 0x00FFFFFFU) | (static_cast<std::uint32_t>(clamp01(a) * 255.0f + 0.5f) << 24);
        }
    }

    // --- Palmera ---
    void buildPalm() {
        const float h = species.height;
        Branch trunk;
        trunk.level = 0;
        trunk.length = h;
        trunk.pivot = -1;
        const float lean = random(0.06f, 0.2f) * std::max(species.gnarl, 0.0f);
        const float azimuth = random(0.0f, 2.0f * kPi);
        const Vec3 lean_dir{std::cos(azimuth), 0.0f, std::sin(azimuth)};
        const Vec3 side_dir{-lean_dir.z, 0.0f, lean_dir.x};
        const int segments = 18;
        const float radius = h * rules.trunk_radius;
        for (int i = 0; i <= segments; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(segments);
            const float y = -0.4f + (h + 0.4f) * t;
            // Curva en S suave: se inclina y se endereza arriba.
            trunk.points.push_back(Vec3{0.0f, y, 0.0f} + lean_dir * (lean * h * (t * t * 1.2f - 0.25f * t * t * t)) +
                                   side_dir * (0.03f * h * std::sin(t * kPi * 1.3f)));
            const float bulb = 1.0f + 0.55f * std::exp(-std::max(y, 0.0f) / 0.5f);
            const float shaft = 1.0f + 0.18f * smoothstepF(0.86f, 0.97f, t);
            trunk.radii.push_back(radius * (1.15f - 0.35f * t) * bulb * shaft);
        }
        const int sides = lod == 0 ? 18 : (lod == 1 ? 8 : 5);
        tube(trunk, sides, lod == 0 ? 2 : 1);
        const Vec3 top = trunk.points.back();
        crown_center = top;
        crown_radii = Vec3{h * 0.45f, h * 0.25f, h * 0.45f};
        leaf_vertex_start = mesh.vertices.size();

        const int fronds = static_cast<int>(std::round((lod == 2 ? 8.0f : (lod == 1 ? 13.0f : 18.0f)) * std::max(species.leaf_density, 0.3f)));
        const int steps = lod == 0 ? 14 : (lod == 1 ? 7 : 3);
        const int cols = lod == 0 ? 4 : (lod == 1 ? 2 : 1);
        const float frond_length = h * 0.4f * species.leaf_size;
        std::mt19937 fr(species.seed * 31U + 7U);  // mismas frondas en todos los niveles
        const int dead_fronds = lod == 2 ? 2 : 4;
        for (int f = 0; f < fronds + dead_fronds; ++f) {
            const bool dead = f >= fronds;
            const float a = static_cast<float>(f) * kGolden + random(fr, -0.2f, 0.2f);
            const Vec3 out{std::cos(a), 0.0f, std::sin(a)};
            const float age = dead ? 1.0f : static_cast<float>(f) / static_cast<float>(std::max(fronds - 1, 1));
            // Las jovenes (arriba) suben; las viejas caen. Las secas cuelgan.
            float elev = dead ? -1.25f : mixF(1.05f, -0.05f, age) + random(fr, -0.12f, 0.12f);
            const float bend = dead ? 0.15f : mixF(1.0f, 1.7f, age) * random(fr, 0.85f, 1.15f);
            const float len = frond_length * (dead ? 0.75f : random(fr, 0.85f, 1.1f));
            const Vec3 base = top + Vec3{0.0f, -0.25f * radius - (dead ? 0.6f : 0.0f), 0.0f} + out * (radius * 0.6f);
            const Vec3 side = core::normalize(core::cross(out, Vec3{0.0f, 1.0f, 0.0f}));
            const float twist = random(fr, -0.25f, 0.25f);
            const float phase = random(fr, 0.0f, 1.0f);
            // Fase del temblor: una onda que recorre la fronda (no un valor por
            // vertice, que la rasgaria; ni otro numero del generador, que
            // cambiaria las frondas siguientes segun el nivel de detalle).
            const float leaf_phase = 0.5f * random(fr, 0.0f, 1.0f);
            const Vec3 tint = dead ? Vec3{1.25f, 0.95f, 0.45f} : species.leaf_color * random(fr, 0.9f, 1.08f);
            const auto first = static_cast<std::uint32_t>(mesh.vertices.size());
            Vec3 spine = base;
            std::vector<Vec3> spine_pts;
            std::vector<Vec3> spine_dir;
            for (int s = 0; s <= steps; ++s) {
                const float t = static_cast<float>(s) / static_cast<float>(steps);
                const float ang = elev - bend * t * t * (dead ? 0.2f : 1.0f);
                const Vec3 d = out * std::cos(ang) + Vec3{0.0f, std::sin(ang), 0.0f};
                spine_pts.push_back(spine);
                spine_dir.push_back(d);
                spine = spine + d * (len / static_cast<float>(steps));
            }
            for (int s = 0; s <= steps; ++s) {
                const float t = static_cast<float>(s) / static_cast<float>(steps);
                const Vec3 d = spine_dir[static_cast<std::size_t>(s)];
                Vec3 up_local = core::normalize(core::cross(side, d));
                if (up_local.y < 0.0f) up_local = -up_local;
                const float tw = twist * t;
                const Vec3 sd = core::normalize(side * std::cos(tw) + up_local * std::sin(tw));
                const Vec3 ul = core::normalize(core::cross(sd, d)) * (core::cross(sd, d).y < 0.0f ? -1.0f : 1.0f);
                const float width = len * 0.24f * std::pow(std::max(std::sin(kPi * std::min(t * 0.95f + 0.05f, 1.0f)), 0.0f), 0.7f) * (dead ? 0.55f : 1.0f);
                const float fold = dead ? 0.1f : 0.45f;
                for (int k = 0; k <= cols; ++k) {
                    const float x = static_cast<float>(k) / static_cast<float>(cols) * 2.0f - 1.0f;
                    // Pliegue en V: los foliolos caen desde el nervio.
                    const Vec3 p = spine_pts[static_cast<std::size_t>(s)] + sd * (x * width) - ul * (fold * std::abs(x) * width) +
                                   Vec3{0.0f, -std::abs(x) * width * 0.25f * t, 0.0f};
                    const Vec3 n = core::normalize(ul * 0.5f + (p - crown_center) * (0.5f / std::max(len, 0.5f)) + Vec3{0.0f, 0.25f, 0.0f});
                    const float ao = mixF(0.5f, 1.0f, smoothstepF(0.0f, 0.35f, t)) * (dead ? 0.7f : 1.0f);
                    mesh.vertices.push_back({p.x, p.y, p.z, n.x, n.y, n.z, pack4(tint.x * 0.5f, tint.y * 0.5f, tint.z * 0.5f, ao),
                                             windAt(top.y), x * 0.5f + 0.5f, 1.0f - t, static_cast<float>(kTreeLayerFrond),
                                             0.03f + 0.97f * t * (0.7f + 0.3f * std::abs(x)), base.x, base.y, base.z,
                                             pack4(phase, dead ? 0.15f : 0.55f, leaf_phase + 0.3f * t, dead ? 0.3f : rules.translucency)});
                }
            }
            const auto ncol = static_cast<std::uint32_t>(cols + 1);
            for (int s = 0; s < steps; ++s) {
                for (int k = 0; k < cols; ++k) {
                    const std::uint32_t p0 = first + static_cast<std::uint32_t>(s) * ncol + static_cast<std::uint32_t>(k);
                    const std::uint32_t p1 = p0 + ncol;
                    mesh.indices.insert(mesh.indices.end(), {p0, p0 + 1, p1, p0 + 1, p1 + 1, p1});
                }
            }
        }
    }

    void build() {
        if (species.kind == TreeKind::Palm) {
            buildPalm();
        } else {
            const float h = species.height;
            const float radius = h * rules.trunk_radius;
            // Raices: angulos y pesos (los mismos en todos los niveles).
            for (int j = 0; j < rules.roots; ++j) {
                root_angles.push_back((static_cast<float>(j) + random(-0.3f, 0.3f)) * 2.0f * kPi / static_cast<float>(rules.roots));
                root_weights.push_back(random(0.45f, 1.0f));
            }
            // El tronco empieza bajo el suelo (en pendiente no queda al aire).
            Branch trunk = growBranch(Vec3{0.0f, -0.5f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f}, h + 0.5f, radius, 0);
            trunk.pivot = -1;
            trunk.seed = static_cast<std::uint32_t>(rng());
            branches.push_back(trunk);
            spawnChildren(0, 1);
            computeCrown();
            placeLeaves();

            // Corteza segun el nivel de detalle: lados, tramos y ramas finas.
            // Lo que mide menos de un pixel donde se usa ese nivel no se
            // genera: el nivel 1 se ve desde ~120 m (un pixel ~ 13 cm a
            // 1080p) y el 2 desde ~450 m (~ 48 cm). Antes el pino lejano tenia
            // 3700 triangulos (3100 de ramas invisibles) y el medio 9300: un
            // bosque de 8000 arboles eran 40 millones de triangulos por frame.
            const std::array<std::array<int, 4>, 3> sides = {{{16, 9, 6, 4}, {8, 4, 3, 3}, {5, 3, 3, 3}}};
            const std::array<std::array<int, 4>, 3> subdiv = {{{2, 1, 1, 1}, {1, 1, 1, 1}, {1, 1, 1, 1}}};
            const std::array<std::array<int, 4>, 3> stride = {{{1, 1, 1, 1}, {2, 3, 3, 3}, {3, 4, 4, 4}}};
            // Radio minimo (en la base) de lo que se dibuja, por nivel de detalle.
            const std::array<float, 3> min_radius = {0.0f, h * 0.0035f, h * 0.014f};
            int deepest = 0;
            for (const Branch& b : branches) deepest = std::max(deepest, b.level);
            const auto l = static_cast<std::size_t>(std::clamp(lod, 0, 2));
            for (const Branch& b : branches) {
                if (lod == 1 && b.level >= deepest && b.level > 1) continue;
                if (lod == 1 && b.dead && b.length < h * 0.03f) continue;
                if (lod == 2 && (b.level > 1 || b.dead)) continue;
                if (b.level > 0 && !b.radii.empty() && b.radii.front() < min_radius[l]) continue;
                const auto lv = static_cast<std::size_t>(std::min(b.level, 3));
                tube(b, sides[l][lv], subdiv[l][lv], stride[l][lv]);
            }
            if (lod == 0 && !root_angles.empty()) buildRoots(branches.front());
            leaf_vertex_start = mesh.vertices.size();

            // Hojas: lejos menos tarjetas (las de clave baja) y mas grandes.
            // De cerca, 2 filas (la caida de la tarjeta) en vez de 3; el sauce
            // las conserva (sus cortinas son largas y se curvan).
            const float keep = lod == 0 ? 1.0f : (lod == 1 ? 0.25f : 0.085f);
            const float grow = 1.0f / std::sqrt(keep);
            const int rows = lod == 0 ? (rules.style == kWillowCurtain ? 3 : 2) : 1;
            const int cols = lod == 0 ? 2 : 1;
            std::vector<std::pair<Vec3, float>> area;
            for (const Card& c : cards) {
                if (c.keep > keep) continue;
                // Borlas de pino: los brotes solo de cerca, el abanico solo lejos.
                if ((c.detail == 1 && lod != 0) || (c.detail == 2 && lod == 0)) continue;
                const float scale = rules.style == kWillowCurtain ? std::sqrt(grow) : grow;
                Card cc = c;
                if (rules.style == kWillowCurtain) cc.aspect = std::min(c.aspect * std::sqrt(grow), 1.0f);
                // Los brotes son cortos y rectos: un solo tramo.
                emitCard(cc, scale, c.detail == 1 ? 1 : rows, c.detail == 1 ? 1 : cols);
                const float len = c.length * scale;
                area.push_back({c.base + c.dir * (len * 0.5f), len * len * cc.aspect});
            }
            crownOcclusion(area);
        }
        // Esfera envolvente.
        float top = 0.0f;
        for (const TreeVertex& v : mesh.vertices) top = std::max(top, v.py);
        mesh.height = std::max(top, 0.5f);
        mesh.center_y = mesh.height * 0.5f;
        float radius = 0.5f;
        for (const TreeVertex& v : mesh.vertices) {
            const float dy = v.py - mesh.center_y;
            radius = std::max(radius, std::sqrt(v.px * v.px + dy * dy + v.pz * v.pz));
        }
        mesh.radius = radius;
    }
};

// =============================================================================
// Texturas
// =============================================================================

// Lienzo en floats: color (sRGB), alfa, altura, oclusion y extra (rugosidad
// en la corteza, translucidez en las hojas). `xs`: ancho fisico de un pixel
// en unidades de pixel vertical (tarjetas mas estrechas que altas).
struct Canvas {
    int size;
    float xs = 1.0f;
    std::vector<float> r, g, b, a, h, ao, ex;
    explicit Canvas(int s)
        : size(s), r(static_cast<std::size_t>(s) * s, 0.0f), g(r), b(r), a(r), h(r), ao(static_cast<std::size_t>(s) * s, 1.0f), ex(r) {}
    std::size_t at(int x, int y) const {
        return static_cast<std::size_t>(((y % size) + size) % size) * static_cast<std::size_t>(size) +
               static_cast<std::size_t>(((x % size) + size) % size);
    }
    void put(std::size_t i, const Vec3& c) {
        r[i] = c.x;
        g[i] = c.y;
        b[i] = c.z;
    }
    Vec3 get(std::size_t i) const { return Vec3{r[i], g[i], b[i]}; }
};

// Recorre los pixeles de una caja (coordenadas fisicas) y llama a f(x, y, X, Y, indice).
template <typename F>
void forBox(Canvas& c, float X0, float Y0, float X1, float Y1, F&& f) {
    const int x0 = std::max(0, static_cast<int>(std::floor(X0 / c.xs)) - 1);
    const int x1 = std::min(c.size - 1, static_cast<int>(std::ceil(X1 / c.xs)) + 1);
    const int y0 = std::max(0, static_cast<int>(std::floor(Y0)) - 1);
    const int y1 = std::min(c.size - 1, static_cast<int>(std::ceil(Y1)) + 1);
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            f(x, y, (static_cast<float>(x) + 0.5f) * c.xs, static_cast<float>(y) + 0.5f, c.at(x, y));
        }
    }
}

// Tallo o aguja: polilinea (coordenadas fisicas) con grosor que se afina,
// sombreada como un cilindro.
void drawStem(Canvas& c, const std::vector<std::array<float, 2>>& pts, float th0, float th1, const Vec3& color, float translucency,
              float height_scale = 1.0f, bool needle = false) {
    if (pts.size() < 2) return;
    float total = 0.0f;
    std::vector<float> acc{0.0f};
    for (std::size_t i = 1; i < pts.size(); ++i) {
        total += std::hypot(pts[i][0] - pts[i - 1][0], pts[i][1] - pts[i - 1][1]);
        acc.push_back(total);
    }
    for (std::size_t i = 1; i < pts.size(); ++i) {
        const float ax = pts[i - 1][0], ay = pts[i - 1][1], bx = pts[i][0], by = pts[i][1];
        const float maxth = std::max(th0, th1) + 1.5f;
        const float dx = bx - ax, dy = by - ay;
        const float len2 = std::max(dx * dx + dy * dy, 1e-6f);
        forBox(c, std::min(ax, bx) - maxth, std::min(ay, by) - maxth, std::max(ax, bx) + maxth, std::max(ay, by) + maxth,
               [&](int, int, float X, float Y, std::size_t idx) {
                   const float s = std::clamp(((X - ax) * dx + (Y - ay) * dy) / len2, 0.0f, 1.0f);
                   const float px = ax + dx * s - X, py = ay + dy * s - Y;
                   const float d = std::sqrt(px * px + py * py);
                   const float t = total > 0.0f ? (acc[i - 1] + s * std::sqrt(len2)) / total : 0.0f;
                   float th = mixF(th0, th1, t);
                   if (needle) th *= std::sqrt(std::max(1.0f - t * t * t, 0.0f));
                   if (th <= 0.05f) return;
                   const float cov = clamp01(th - d + 0.5f);
                   if (cov <= 0.0f) return;
                   const float across = clamp01(d / std::max(th, 0.3f));
                   const float round = std::sqrt(std::max(1.0f - across * across, 0.0f));
                   const Vec3 col = color * (0.7f + 0.38f * round) * (needle ? (0.9f + 0.18f * t) : 1.0f);
                   const float keep = c.a[idx];
                   c.put(idx, mixV(c.get(idx), col, cov));
                   c.a[idx] = std::max(keep, cov);
                   c.h[idx] = mixF(c.h[idx], (0.55f + 0.45f * round) * height_scale, cov);
                   c.ao[idx] = mixF(c.ao[idx], 0.85f + 0.15f * round, cov);
                   c.ex[idx] = mixF(c.ex[idx], translucency, cov);
               });
    }
}

// Curva de grosor de cada forma de hoja (0..1 del medio ancho maximo).
// 0 roble (lobulada), 1 abedul (aovada aserrada), 2 sauce (lanceolada),
// 3 foliolo de palmera (lineal).
float leafProfile(int shape, float t, float side, float jitter) {
    switch (shape) {
        case 0: {
            const float env = std::pow(std::max(std::sin(kPi * std::pow(clamp01(t), 0.85f)), 0.0f), 0.75f) * (1.0f - 0.12f * t);
            const float lobes = 4.5f + jitter;
            const float ph = side > 0.0f ? 0.0f : 0.22f;
            const float lobe = std::pow(std::abs(std::sin(kPi * (lobes * t + ph))), 0.55f);
            return env * (0.48f + 0.52f * lobe);
        }
        case 1: {
            float env = t < 0.32f ? std::pow(std::max(std::sin(t / 0.32f * kPi * 0.5f), 0.0f), 0.6f) : std::pow(std::max(1.0f - (t - 0.32f) / 0.68f, 0.0f), 0.85f);
            const float s1 = t * 15.0f + (side > 0.0f ? 0.0f : 0.5f);
            const float s2 = t * 30.0f;
            env *= 1.0f - 0.08f * (s1 - std::floor(s1)) - 0.035f * (s2 - std::floor(s2));
            return env;
        }
        case 2: {
            const float env = std::pow(std::max(std::sin(kPi * clamp01(t)), 0.0f), 0.65f) * (1.0f - 0.3f * t);
            const float s = t * 38.0f;
            return env * (1.0f - 0.05f * (s - std::floor(s)));
        }
        default: {
            return smoothstepF(0.0f, 0.06f, t) * (1.0f - std::pow(clamp01(t), 4.0f)) * (0.85f + 0.15f * std::sin(t * 3.0f));
        }
    }
}

struct LeafPaint {
    float x = 0.0f, y = 0.0f;  // nacimiento del peciolo (fisico)
    float angle = 0.0f;        // hacia la punta (rad; x a la derecha, y hacia abajo)
    float length = 100.0f;     // con el peciolo
    float width = 30.0f;       // medio ancho maximo
    int shape = 0;
    Vec3 color{0.2f, 0.33f, 0.1f};
    Vec3 vein_color{0.4f, 0.48f, 0.18f};
    float petiole = 0.1f;
    float dry = 0.0f;
    float depth_ao = 1.0f;
    float bend = 0.0f;
    float translucency = 0.85f;
    std::uint32_t seed = 1;
};

// Hoja con silueta de su especie, nervio central y nervios secundarios en
// relieve, manchas y bordes secos. `shadow`: solo oscurece lo que ya hay
// debajo (la sombra de esta hoja sobre las anteriores).
void paintLeaf(Canvas& c, const LeafPaint& L, bool shadow = false) {
    const float ca = std::cos(L.angle);
    const float sa = std::sin(L.angle);
    const float off_x = shadow ? L.width * 0.25f + 3.0f : 0.0f;
    const float off_y = shadow ? L.width * 0.35f + 4.0f : 0.0f;
    const float pet = L.length * L.petiole;
    const float blade = std::max(L.length - pet, 1.0f);
    const float jitter = hashF(static_cast<int>(L.seed), 3, 91U) - 0.5f;
    // Mordiscos.
    std::array<std::array<float, 3>, 3> holes{};
    int hole_count = 0;
    if (!shadow && hashF(static_cast<int>(L.seed), 5, 92U) < 0.18f) {
        hole_count = 1 + static_cast<int>(hashF(static_cast<int>(L.seed), 6, 93U) * 2.5f);
        for (int k = 0; k < hole_count; ++k) {
            holes[static_cast<std::size_t>(k)] = {pet + blade * (0.25f + 0.6f * hashF(static_cast<int>(L.seed), 10 + k, 94U)),
                                                  (hashF(static_cast<int>(L.seed), 20 + k, 95U) - 0.5f) * L.width * 1.2f,
                                                  L.width * (0.08f + 0.12f * hashF(static_cast<int>(L.seed), 30 + k, 96U))};
        }
    }
    // Caja de la hoja: de la base a la punta, mas el medio ancho (y la curva)
    // hacia los lados (el lado es (-sa, ca)).
    const float half_w = L.width + std::abs(L.bend) + 2.0f;
    const float x0 = L.x + off_x - ca, y0 = L.y + off_y - sa;
    const float x1 = L.x + off_x + ca * (L.length + 1.0f), y1 = L.y + off_y + sa * (L.length + 1.0f);
    const float side_x = std::abs(sa) * half_w, side_y = std::abs(ca) * half_w;
    forBox(c, std::min(x0, x1) - side_x, std::min(y0, y1) - side_y, std::max(x0, x1) + side_x, std::max(y0, y1) + side_y,
           [&](int px, int py, float X, float Y, std::size_t i) {
               const float dx = X - L.x - off_x;
               const float dy = Y - L.y - off_y;
               const float along = dx * ca + dy * sa;
               float across = -dx * sa + dy * ca;
               if (along < -1.0f || along > L.length + 1.0f) return;
               const float tt = clamp01(along / L.length);
               across -= L.bend * 4.0f * tt * (1.0f - tt);
               if (along < pet) {
                   if (shadow) return;
                   // Peciolo.
                   const float th = 1.1f + 0.6f * (1.0f - along / std::max(pet, 1.0f));
                   const float cov = clamp01(th - std::abs(across) + 0.5f) * clamp01(along + 0.5f);
                   if (cov <= 0.0f) return;
                   c.put(i, mixV(c.get(i), L.vein_color * 0.8f, cov));
                   c.a[i] = std::max(c.a[i], cov);
                   c.h[i] = mixF(c.h[i], 0.6f, cov);
                   c.ao[i] = mixF(c.ao[i], L.depth_ao, cov);
                   c.ex[i] = mixF(c.ex[i], 0.3f, cov);
                   return;
               }
               if (std::abs(across) > L.width + 0.6f) return;
               const float t = (along - pet) / blade;
               const float side = across >= 0.0f ? 1.0f : -1.0f;
               const float w = L.width * leafProfile(L.shape, t, side, jitter);
               const float edge = w - std::abs(across);
               float cov = clamp01(edge + 0.5f);
               if (!(cov > 0.0f)) return;  // (tambien si fuera NaN)
               if (shadow) {
                   if (c.a[i] <= 0.0f) return;
                   const float s = 0.38f * cov;
                   c.ao[i] *= 1.0f - s;
                   c.put(i, c.get(i) * (1.0f - s * 0.55f));
                   return;
               }
               for (int k = 0; k < hole_count; ++k) {
                   const auto& hh = holes[static_cast<std::size_t>(k)];
                   const float hd = std::hypot(along - hh[0], across - hh[1]);
                   if (hd < hh[2]) return;
                   if (hd < hh[2] + 1.5f) cov *= (hd - hh[2]) / 1.5f;
               }
               const float rel = std::abs(across) / std::max(w, 0.5f);
               // Nervio central y secundarios.
               const float mid_w = 0.7f + 1.4f * (1.0f - t) * (L.width / 40.0f);
               const float mid = std::exp(-across * across / std::max(mid_w * mid_w, 0.2f));
               float vein = 0.0f;
               {
                   const float pairs = L.shape == 0 ? 4.5f + jitter : (L.shape == 1 ? 8.0f : (L.shape == 2 ? 14.0f : 0.0f));
                   if (pairs > 0.0f) {
                       const float slope = L.shape == 0 ? 0.9f : (L.shape == 1 ? 1.1f : 1.6f);
                       const float q = t * pairs - rel * slope * (L.shape == 0 ? 0.45f : 0.35f) + (side > 0.0f ? 0.0f : 0.25f);
                       const float fq = std::abs(q - std::round(q));
                       const float px_per_q = blade / pairs;
                       vein = clamp01(1.0f - fq * px_per_q / (0.55f + 0.5f * (1.0f - rel))) * (1.0f - rel * 0.7f) * smoothstepF(0.03f, 0.12f, t);
                   } else {
                       // Foliolo de palmera: nervios paralelos.
                       const float q = across / std::max(L.width * 0.3f, 1.0f);
                       vein = std::pow(std::abs(std::cos(q * kPi)), 18.0f) * 0.6f;
                   }
               }
               // Red fina de nervaduras y moteado.
               const float net = valueNoise(static_cast<float>(px) * 0.35f, static_cast<float>(py) * 0.35f, 99991, 99991, L.seed);
               const float mottle = valueNoise(static_cast<float>(px) * 0.05f, static_cast<float>(py) * 0.05f, 99991, 99991, L.seed + 7U);
               Vec3 col = L.color * (0.9f + 0.18f * mottle) * (1.0f + 0.07f * (1.0f - rel));
               col = mixV(col, L.vein_color, clamp01(mid * 0.85f + vein * 0.45f));
               col = col * (1.0f - 0.05f * net);
               // Seca: borde y manchas pardas.
               float dry = 0.0f;
               if (L.dry > 0.0f) {
                   const float spot = smoothstepF(0.62f, 0.7f, valueNoise(static_cast<float>(px) * 0.09f, static_cast<float>(py) * 0.09f, 99991, 99991, L.seed + 11U));
                   dry = clamp01(L.dry * (smoothstepF(5.0f, 0.0f, edge) + spot));
                   col = mixV(col, Vec3{0.45f, 0.33f, 0.16f} * (0.8f + 0.3f * mottle), dry);
               }
               // Borde un poco mas oscuro (mas fino, mas transparente a la luz).
               col = col * (0.92f + 0.08f * smoothstepF(0.0f, 3.0f, edge));
               const float dome = 1.0f - rel * rel;
               const float height = 0.45f + 0.35f * dome - 0.1f * mid - 0.07f * vein + 0.03f * net;
               c.put(i, mixV(c.get(i), col, cov));
               c.a[i] = std::max(c.a[i], cov);
               c.h[i] = mixF(c.h[i], height, cov);
               c.ao[i] = mixF(c.ao[i], L.depth_ao * (0.9f + 0.1f * dome), cov);
               c.ex[i] = mixF(c.ex[i], L.translucency * (1.0f - 0.35f * mid - 0.2f * vein - 0.5f * dry), cov);
           });
}

// Encoge la hoja hasta que su punta y sus lados caben en el lienzo.
void fitLeaf(const Canvas& c, LeafPaint& L) {
    const float W = static_cast<float>(c.size) * c.xs;
    const float H = static_cast<float>(c.size);
    for (int it = 0; it < 12; ++it) {
        const float tx = L.x + std::cos(L.angle) * L.length;
        const float ty = L.y + std::sin(L.angle) * L.length;
        const float mx = L.x + std::cos(L.angle) * L.length * 0.5f;
        const float my = L.y + std::sin(L.angle) * L.length * 0.5f;
        const float wx = -std::sin(L.angle) * L.width;
        const float wy = std::cos(L.angle) * L.width;
        const float m = 3.0f;
        const auto inside = [&](float x, float y) { return x > m && y > m && x < W - m && y < H - m; };
        if (inside(tx, ty) && inside(mx + wx, my + wy) && inside(mx - wx, my - wy)) return;
        L.length *= 0.85f;
        L.width *= 0.85f;
    }
}

// Ramita curva (coordenadas fisicas) como polilinea.
std::vector<std::array<float, 2>> curve(float x0, float y0, float angle, float length, float bend, int steps = 10) {
    std::vector<std::array<float, 2>> pts;
    float x = x0, y = y0, a = angle;
    pts.push_back({x, y});
    for (int i = 0; i < steps; ++i) {
        a += bend / static_cast<float>(steps);
        x += std::cos(a) * length / static_cast<float>(steps);
        y += std::sin(a) * length / static_cast<float>(steps);
        pts.push_back({x, y});
    }
    return pts;
}

// Las filas de una corteza en varios hilos (cada pixel es independiente y
// escribe solo el suyo). Las cortezas eran lo mas lento de generar.
template <typename F>
void parallelRows(int size, F&& row) {
    const unsigned workers = std::clamp(std::thread::hardware_concurrency() / 3U, 1U, 4U);
    std::vector<std::thread> pool;
    for (unsigned w = 1; w < workers; ++w) {
        pool.emplace_back([&, w]() {
            for (int y = static_cast<int>(w); y < size; y += static_cast<int>(workers)) row(y);
        });
    }
    for (int y = 0; y < size; y += static_cast<int>(workers)) row(y);
    for (std::thread& t : pool) t.join();
}

// --- Cortezas ---

Canvas oakBark(int size) {
    // Roble viejo: crestas largas y estrechas, de anchura desigual, que se
    // separan y se vuelven a juntar; surcos profundos en V; alguna grieta
    // horizontal que parte una cresta. Gris pardo, la cara de las crestas mas
    // gris (curtida) y algun liquen gris verdoso apagado. El musgo lo pone el
    // shader (arriba de las ramas y en el pie), no la textura. Todo periodico:
    // se repite alrededor del tronco y a lo largo.
    Canvas c(size);
    parallelRows(size, [&](int y) {
        for (int x = 0; x < size; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(size);
            const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(size);
            const std::size_t i = c.at(x, y);
            // Los surcos serpentean (curvas largas).
            const float wu = (fbm(u, v, 2, 3, 4, 11U) - 0.5f) * 0.1f;
            const float wv = (fbm(u, v, 3, 3, 3, 12U) - 0.5f) * 0.06f;
            // Crestas: celdas de ~6 x 24 cm (15 x 4 por repeticion de 0.95 m).
            const VEdge ridge = voronoiEdge((u + wu) * 15.0f, (v + wv) * 4.0f, 15, 4, 41U, 0.2f);
            // Surcos en V, anchos como media cresta (de 1.5 a 3.5 cm de lado a lado).
            const float half = 0.008f + 0.011f * hashF(ridge.id, 1, 42U);
            float crest = std::pow(smoothstepF(0.0f, half, ridge.edge), 0.7f);
            // Donde una cresta acaba (borde horizontal) el surco es menos hondo.
            crest = 1.0f - (1.0f - crest) * (1.0f - 0.4f * smoothstepF(0.6f, 0.95f, ridge.horizontal));
            // Grietas horizontales: pocas, de anchura desigual, a distinta altura en cada cresta.
            const float cv = (v + wv) * 6.0f + wu * 5.0f + hashF(ridge.id, 2, 43U) * 4.0f;
            const float cf = cv - std::floor(cv);
            const int crack_row = ((static_cast<int>(std::floor(cv)) % 6) + 6) % 6;
            const float row = hashF(ridge.id, crack_row, 44U);
            const float crack = (1.0f - smoothstepF(0.0f, 0.02f + 0.04f * row, std::abs(cf - 0.5f))) * (row > 0.72f ? 1.0f : 0.0f);
            // Fibras verticales, escamas y poros.
            const float fibers = valueNoise(u * 260.0f, v * 14.0f, 260, 14, 53U);
            const float flakes = fbm(u, v, 16, 8, 3, 54U);
            const float pits = fbm(u, v, 40, 40, 2, 55U);
            const float tone = hashF(ridge.id, 3, 56U);
            float height = crest * (0.62f + 0.26f * flakes) * (1.0f - 0.3f * crack) + 0.07f * fibers * crest + 0.05f * pits;
            height = std::max(height, 0.0f);
            const Vec3 furrow{0.12f, 0.095f, 0.075f};
            const Vec3 wall = Vec3{0.3f, 0.235f, 0.18f} * (0.9f + 0.2f * tone);
            const Vec3 face = Vec3{0.46f, 0.42f, 0.37f} * (0.84f + 0.26f * tone);
            Vec3 col = mixV(furrow, wall, smoothstepF(0.0f, 0.35f, height));
            col = mixV(col, face, smoothstepF(0.45f, 0.8f, height) * (0.7f + 0.3f * flakes));
            col = col * (0.9f + 0.18f * fibers) * (0.94f + 0.12f * pits);
            col = mixV(col, wall * 0.8f, crack * 0.35f);
            // Liquen: manchas pequenas gris verdosas en las caras, poco contraste.
            const float lichen = smoothstepF(0.67f, 0.72f, fbm(u, v, 6, 6, 5, 57U)) * smoothstepF(0.5f, 0.75f, height);
            col = mixV(col, Vec3{0.5f, 0.52f, 0.47f} * (0.85f + 0.3f * pits), lichen * 0.35f);
            c.put(i, col);
            c.a[i] = 1.0f;
            c.h[i] = height + lichen * 0.02f;
            c.ao[i] = 0.25f + 0.75f * smoothstepF(0.0f, 0.6f, height);
            c.ex[i] = 0.92f - 0.06f * lichen - 0.05f * crest;
        }
    });
    return c;
}

Canvas pineBark(int size) {
    // Pino silvestre (parte baja del tronco): placas gruesas e irregulares,
    // mas altas que anchas, separadas por fisuras profundas. La cara de las
    // placas es gris parda (curtida) y se pela en escamas por capas; en las
    // fisuras y donde salta una escama asoma la corteza interior, rojiza.
    Canvas c(size);
    parallelRows(size, [&](int y) {
        for (int x = 0; x < size; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(size);
            const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(size);
            const std::size_t i = c.at(x, y);
            const float wu = (fbm(u, v, 3, 3, 4, 61U) - 0.5f) * 0.09f;
            const float wv = (fbm(u, v, 3, 4, 4, 62U) - 0.5f) * 0.1f;
            // Placas de ~11 x 25 cm (7 x 3 por repeticion de 0.75 m).
            const VEdge plate = voronoiEdge((u + wu) * 7.0f, (v + wv) * 3.0f, 7, 3, 63U, 0.45f);
            const float half = 0.016f + 0.016f * hashF(plate.id, 1, 64U);
            const float fissure = 1.0f - smoothstepF(0.0f, half, plate.edge);  // 1 en el fondo de la fisura
            // Placas que se parten en otras mas pequenas (grietas poco hondas).
            const VEdge sub = voronoiEdge((u + wu) * 12.0f, (v + wv) * 7.0f, 12, 7, 71U, 0.6f);
            const float split = (1.0f - smoothstepF(0.0f, 0.006f, sub.edge)) * (hashF(sub.id, 1, 72U) > 0.6f ? 1.0f : 0.0f);
            // Escamas por capas: terrazas de ruido dentro de cada placa.
            const float flake = fbm(u + hashF(plate.id, 2, 65U), v, 7, 5, 4, 66U);
            const float steps = flake * 3.0f;
            const float fs = steps - std::floor(steps);
            const float terrace = (std::floor(steps) + smoothstepF(0.7f, 1.0f, fs)) / 3.0f;
            const float edge = smoothstepF(0.7f, 0.9f, fs) * (1.0f - smoothstepF(0.97f, 1.0f, fs));  // borde de una escama
            const float fine = valueNoise(u * 180.0f, v * 70.0f, 180, 70, 67U);
            const float fibers = valueNoise(u * 300.0f, v * 20.0f, 300, 20, 68U);
            const float plate_h = 0.5f + 0.35f * terrace + 0.05f * fine;
            float height = mixF(plate_h * (1.0f - 0.15f * split), 0.02f, std::pow(fissure, 0.8f));
            height += 0.03f * fibers;
            const float tone = hashF(plate.id, 3, 69U);
            // Cara curtida: gris parda (unas placas algo mas rojizas que otras).
            Vec3 face = mixV(Vec3{0.41f, 0.35f, 0.31f}, Vec3{0.45f, 0.34f, 0.27f}, tone);
            face = mixV(face, Vec3{0.5f, 0.47f, 0.44f}, smoothstepF(0.55f, 0.85f, terrace) * 0.45f);
            // Corteza interior (rojiza) en las capas recien expuestas y en los bordes.
            const Vec3 inner{0.47f, 0.28f, 0.17f};
            Vec3 col = mixV(face, inner, clamp01((1.0f - terrace) * 0.3f + edge * 0.3f) * 0.5f);
            col = col * (0.88f + 0.2f * fine) * (0.95f + 0.1f * fibers);
            // Fisuras: paredes rojizas y fondo pardo oscuro (no negro).
            const Vec3 bottom{0.13f, 0.08f, 0.06f};
            col = mixV(col, inner * 0.8f, smoothstepF(0.15f, 0.5f, fissure));
            col = mixV(col, bottom, smoothstepF(0.55f, 0.95f, fissure));
            col = mixV(col, inner * 0.8f, split * 0.2f);
            const float lichen = smoothstepF(0.68f, 0.74f, fbm(u, v, 5, 5, 5, 70U)) * (1.0f - fissure) * smoothstepF(0.6f, 0.8f, height);
            col = mixV(col, Vec3{0.52f, 0.54f, 0.48f}, lichen * 0.35f);
            c.put(i, col);
            c.a[i] = 1.0f;
            c.h[i] = height;
            c.ao[i] = 0.22f + 0.78f * smoothstepF(0.0f, 0.55f, height);
            c.ex[i] = 0.84f + 0.1f * fissure - 0.06f * edge;
        }
    });
    return c;
}

Canvas birchBark(int size) {
    Canvas c(size);
    // Lenticelas: rayas horizontales oscuras.
    struct Dash {
        float x, y, len, th;
    };
    std::vector<Dash> dashes;
    std::mt19937 rng(301U);
    std::uniform_real_distribution<float> uni(0.0f, 1.0f);
    for (int k = 0; k < 340; ++k) {
        dashes.push_back({uni(rng), uni(rng), 0.015f + 0.09f * uni(rng) * uni(rng), 0.0016f + 0.0035f * uni(rng)});
    }
    std::vector<float> lenticels(static_cast<std::size_t>(size) * static_cast<std::size_t>(size), 0.0f);
    const float fs = static_cast<float>(size);
    for (const Dash& d : dashes) {
        const int px0 = static_cast<int>(std::floor((d.x - d.len) * fs)) - 1;
        const int px1 = static_cast<int>(std::ceil((d.x + d.len) * fs)) + 1;
        const int py0 = static_cast<int>(std::floor((d.y - d.th * 2.0f) * fs)) - 1;
        const int py1 = static_cast<int>(std::ceil((d.y + d.th * 2.0f) * fs)) + 1;
        for (int py = py0; py <= py1; ++py) {
            for (int px = px0; px <= px1; ++px) {
                const float dx = (static_cast<float>(px) + 0.5f) / fs - d.x;
                const float dy = (static_cast<float>(py) + 0.5f) / fs - d.y;
                if (std::abs(dy) > d.th * 2.0f || std::abs(dx) > d.len) continue;
                const float shape = d.th * (1.0f - (dx / d.len) * (dx / d.len));
                float& l = lenticels[c.at(px, py)];
                l = std::max(l, clamp01((shape - std::abs(dy)) * fs + 0.5f));
            }
        }
    }
    parallelRows(size, [&](int y) {
        for (int x = 0; x < size; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(size);
            const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(size);
            const std::size_t i = c.at(x, y);
            const float n = fbm(u, v, 4, 8, 5, 71U);
            const float band = valueNoise(u * 3.0f, v * 70.0f, 3, 70, 72U);
            Vec3 col = Vec3{0.9f, 0.89f, 0.85f} * (0.9f + 0.1f * n) * (0.96f + 0.06f * band);
            col = mixV(col, Vec3{0.86f, 0.84f, 0.74f}, smoothstepF(0.5f, 0.8f, fbm(u, v, 3, 3, 4, 73U)) * 0.5f);
            float height = 0.6f + 0.05f * band + 0.04f * n;
            float rough = 0.55f;
            // Lenticelas.
            const float lent = lenticels[i];
            col = mixV(col, Vec3{0.22f, 0.18f, 0.16f}, lent * 0.9f);
            height -= lent * 0.2f;
            // Manchas negras rugosas.
            const float dark = smoothstepF(0.64f, 0.69f, fbm(u, v, 3, 7, 5, 74U));
            const float rough_n = fbm(u, v, 20, 20, 3, 75U);
            col = mixV(col, Vec3{0.1f, 0.09f, 0.085f} * (0.8f + 0.5f * rough_n), dark * 0.95f);
            height = mixF(height, 0.35f + 0.4f * rough_n, dark);
            rough = mixF(rough, 0.92f, dark);
            // Tiras que se pelan (rosadas debajo).
            const float peel_band = valueNoise(u * 2.0f, v * 22.0f, 2, 22, 76U);
            const float peel = smoothstepF(0.78f, 0.82f, peel_band) * smoothstepF(0.55f, 0.6f, fbm(u, v, 4, 4, 3, 77U)) * (1.0f - dark);
            col = mixV(col, Vec3{0.86f, 0.71f, 0.62f}, peel * 0.8f);
            height += peel * 0.12f;
            c.put(i, col);
            c.a[i] = 1.0f;
            c.h[i] = height;
            c.ao[i] = 0.6f + 0.4f * smoothstepF(0.2f, 0.65f, height);
            c.ex[i] = rough;
        }
    });
    return c;
}

Canvas palmBark(int size) {
    Canvas c(size);
    parallelRows(size, [&](int y) {
        for (int x = 0; x < size; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(size);
            const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(size);
            const std::size_t i = c.at(x, y);
            // Anillos de las hojas caidas (5 por repeticion), ondulados.
            const float rv = v * 5.0f + (fbm(u, v, 4, 2, 3, 81U) - 0.5f) * 0.35f;
            const float f = rv - std::floor(rv);
            const float ring = smoothstepF(0.0f, 0.06f, f) * (1.0f - smoothstepF(0.06f, 0.2f, f));
            const float crevice = smoothstepF(0.86f, 1.0f, f);
            const float fibers = valueNoise(u * 190.0f, v * 30.0f, 190, 30, 82U);
            const float n = fbm(u, v, 6, 6, 4, 83U);
            float height = 0.45f + 0.35f * ring - 0.35f * crevice + 0.12f * fibers + 0.05f * n;
            Vec3 col = Vec3{0.46f, 0.41f, 0.34f} * (0.85f + 0.25f * n) * (0.9f + 0.15f * fibers);
            col = mixV(col, Vec3{0.3f, 0.26f, 0.21f}, ring * 0.5f);
            col = mixV(col, Vec3{0.12f, 0.1f, 0.08f}, crevice * 0.8f);
            const float lichen = smoothstepF(0.64f, 0.7f, fbm(u, v, 5, 5, 5, 84U));
            col = mixV(col, Vec3{0.62f, 0.62f, 0.56f}, lichen * 0.6f);
            c.put(i, col);
            c.a[i] = 1.0f;
            c.h[i] = height;
            c.ao[i] = 0.35f + 0.65f * smoothstepF(0.05f, 0.6f, height);
            c.ex[i] = 0.85f;
        }
    });
    return c;
}

// --- Hojas (la ramita nace abajo en el centro: v = 1) ---

Vec3 jitterColor(std::mt19937& rng, const Vec3& base, float amount) {
    std::uniform_real_distribution<float> uni(-1.0f, 1.0f);
    const float l = 1.0f + uni(rng) * amount;
    const float hue = uni(rng) * amount;
    return Vec3{base.x * l * (1.0f + hue * 0.6f), base.y * l, base.z * l * (1.0f - hue * 0.5f)};
}

Canvas oakLeaves(int size) {
    Canvas c(size);
    const float s = static_cast<float>(size);
    std::mt19937 rng(77U);
    std::uniform_real_distribution<float> uni(0.0f, 1.0f);
    const Vec3 twig_col{0.3f, 0.24f, 0.17f};
    // Tallo principal y brotes laterales.
    const auto main = curve(s * 0.5f, s * 0.995f, -kPi * 0.5f + 0.06f, s * 0.62f, -0.18f, 12);
    struct Shoot {
        std::vector<std::array<float, 2>> pts;
        float angle;
    };
    std::vector<Shoot> shoots;
    shoots.push_back({main, -kPi * 0.5f - 0.12f});
    const float side_t[4] = {0.3f, 0.48f, 0.64f, 0.8f};
    for (int k = 0; k < 4; ++k) {
        const auto& p = main[static_cast<std::size_t>(side_t[k] * 12.0f)];
        const float dir = (k % 2 == 0 ? -1.0f : 1.0f);
        const float ang = -kPi * 0.5f + dir * (0.75f + 0.25f * uni(rng));
        const float len = s * (0.3f - 0.06f * static_cast<float>(k)) * (0.85f + 0.3f * uni(rng));
        shoots.push_back({curve(p[0], p[1], ang, len, -dir * 0.35f, 8), ang - dir * 0.35f});
    }
    for (std::size_t k = 0; k < shoots.size(); ++k) drawStem(c, shoots[k].pts, k == 0 ? s * 0.0065f : s * 0.0045f, s * 0.0028f, twig_col, 0.0f);
    // Hojas: rosetas en la punta de cada brote y alternas a lo largo.
    std::vector<LeafPaint> leaves;
    std::uint32_t id = 5U;
    const Vec3 base_col{0.17f, 0.29f, 0.075f};
    for (const Shoot& sh : shoots) {
        const auto& tip = sh.pts.back();
        const int rosette = 4 + static_cast<int>(uni(rng) * 2.0f);
        for (int k = 0; k < rosette; ++k) {
            LeafPaint L;
            L.x = tip[0];
            L.y = tip[1];
            L.angle = sh.angle + (static_cast<float>(k) / static_cast<float>(rosette - 1) - 0.5f) * 2.4f + (uni(rng) - 0.5f) * 0.3f;
            L.length = s * (0.17f + 0.07f * uni(rng));
            L.width = L.length * (0.26f + 0.05f * uni(rng));
            L.shape = 0;
            L.color = jitterColor(rng, base_col, 0.12f);
            L.vein_color = Vec3{0.36f, 0.43f, 0.16f};
            L.petiole = 0.06f;
            L.dry = uni(rng) < 0.12f ? 0.4f + 0.5f * uni(rng) : 0.0f;
            L.bend = (uni(rng) - 0.5f) * L.width * 0.4f;
            L.seed = id++;
            leaves.push_back(L);
        }
        for (std::size_t p = 2; p + 2 < sh.pts.size(); p += 3) {
            LeafPaint L;
            L.x = sh.pts[p][0];
            L.y = sh.pts[p][1];
            const float side = (p / 3) % 2 == 0 ? -1.0f : 1.0f;
            L.angle = sh.angle + side * (0.7f + 0.5f * uni(rng));
            L.length = s * (0.13f + 0.06f * uni(rng));
            L.width = L.length * 0.27f;
            L.shape = 0;
            L.color = jitterColor(rng, base_col * 0.92f, 0.12f);
            L.vein_color = Vec3{0.34f, 0.41f, 0.15f};
            L.petiole = 0.07f;
            L.dry = uni(rng) < 0.08f ? 0.5f : 0.0f;
            L.bend = (uni(rng) - 0.5f) * L.width * 0.4f;
            L.seed = id++;
            leaves.push_back(L);
        }
    }
    // Las de abajo primero (mas oscuras), las de encima despues.
    std::shuffle(leaves.begin(), leaves.end(), rng);
    for (std::size_t k = 0; k < leaves.size(); ++k) {
        LeafPaint& L = leaves[k];
        L.depth_ao = 0.62f + 0.38f * static_cast<float>(k) / static_cast<float>(leaves.size());
        fitLeaf(c, L);
        paintLeaf(c, L, true);
        paintLeaf(c, L);
    }
    return c;
}

Canvas birchLeaves(int size) {
    Canvas c(size);
    c.xs = 0.85f;  // tarjeta 0.85 de ancha (xs = ancho fisico de un pixel = aspecto de la tarjeta)
    const float W = static_cast<float>(size) * c.xs;
    const float s = static_cast<float>(size);
    std::mt19937 rng(88U);
    std::uniform_real_distribution<float> uni(0.0f, 1.0f);
    const Vec3 twig_col{0.32f, 0.2f, 0.15f};
    const auto main = curve(W * 0.5f, s * 0.995f, -kPi * 0.5f + 0.1f, s * 0.7f, -0.3f, 14);
    drawStem(c, main, s * 0.005f, s * 0.0022f, twig_col, 0.0f);
    std::vector<std::vector<std::array<float, 2>>> twigs{main};
    for (int k = 0; k < 6; ++k) {
        const auto& p = main[static_cast<std::size_t>(2 + k * 2)];
        const float dir = k % 2 == 0 ? -1.0f : 1.0f;
        const auto tw = curve(p[0], p[1], -kPi * 0.5f + dir * (0.9f + 0.3f * uni(rng)), s * (0.22f - 0.02f * static_cast<float>(k)), dir * 0.9f, 8);
        drawStem(c, tw, s * 0.003f, s * 0.0016f, twig_col, 0.0f);
        twigs.push_back(tw);
    }
    std::vector<LeafPaint> leaves;
    std::uint32_t id = 300U;
    const Vec3 base_col{0.25f, 0.38f, 0.1f};
    for (const auto& tw : twigs) {
        for (std::size_t p = 1; p < tw.size(); p += 2) {
            const float dx = tw[std::min(p + 1, tw.size() - 1)][0] - tw[p - 1][0];
            const float dy = tw[std::min(p + 1, tw.size() - 1)][1] - tw[p - 1][1];
            const float dir = std::atan2(dy, dx);
            LeafPaint L;
            L.x = tw[p][0];
            L.y = tw[p][1];
            const float side = (p / 2) % 2 == 0 ? -1.0f : 1.0f;
            // Cuelgan: entre la ramita y la vertical.
            L.angle = mixF(dir + side * 0.9f, kPi * 0.5f, 0.35f) + (uni(rng) - 0.5f) * 0.4f;
            L.length = s * (0.11f + 0.04f * uni(rng));
            L.width = L.length * 0.36f;
            L.shape = 1;
            L.color = jitterColor(rng, base_col, 0.12f);
            if (uni(rng) < 0.08f) L.color = Vec3{0.55f, 0.52f, 0.14f};
            L.vein_color = Vec3{0.4f, 0.5f, 0.18f};
            L.petiole = 0.18f;
            L.bend = (uni(rng) - 0.5f) * L.width * 0.3f;
            L.seed = id++;
            L.translucency = 0.9f;
            leaves.push_back(L);
        }
    }
    // Amentos (dos).
    for (int k = 0; k < 2; ++k) {
        const auto& p = twigs[static_cast<std::size_t>(1 + k * 2)].back();
        const auto cat = curve(p[0], p[1], kPi * 0.5f + (uni(rng) - 0.5f) * 0.4f, s * 0.08f, 0.2f, 6);
        drawStem(c, cat, s * 0.009f, s * 0.006f, Vec3{0.42f, 0.33f, 0.18f}, 0.2f, 1.2f);
    }
    std::shuffle(leaves.begin(), leaves.end(), rng);
    for (std::size_t k = 0; k < leaves.size(); ++k) {
        LeafPaint& L = leaves[k];
        L.depth_ao = 0.65f + 0.35f * static_cast<float>(k) / static_cast<float>(leaves.size());
        fitLeaf(c, L);
        paintLeaf(c, L, true);
        paintLeaf(c, L);
    }
    return c;
}

Canvas willowLeaves(int size) {
    Canvas c(size);
    c.xs = 0.34f;  // cordon estrecho
    const float W = static_cast<float>(size) * c.xs;
    const float s = static_cast<float>(size);
    std::mt19937 rng(99U);
    std::uniform_real_distribution<float> uni(0.0f, 1.0f);
    const auto strand = curve(W * 0.5f, s * 0.998f, -kPi * 0.5f + 0.02f, s * 0.99f, -0.06f, 20);
    drawStem(c, strand, s * 0.0035f, s * 0.002f, Vec3{0.42f, 0.36f, 0.16f}, 0.1f);
    std::vector<LeafPaint> leaves;
    std::uint32_t id = 500U;
    const Vec3 base_col{0.33f, 0.43f, 0.14f};
    for (int k = 0; k < 64; ++k) {
        const float t = (static_cast<float>(k) + uni(rng) * 0.6f) / 64.0f;
        const auto& p = strand[std::min(static_cast<std::size_t>(t * 20.0f), strand.size() - 1)];
        LeafPaint L;
        L.x = p[0];
        L.y = p[1];
        const float side = k % 2 == 0 ? -1.0f : 1.0f;
        L.angle = -kPi * 0.5f + side * (0.25f + 0.3f * uni(rng));
        L.length = s * (0.12f + 0.05f * uni(rng));
        L.width = L.length * 0.095f;
        L.shape = 2;
        L.color = jitterColor(rng, base_col, 0.14f);
        L.vein_color = Vec3{0.52f, 0.55f, 0.28f};
        L.petiole = 0.05f;
        L.bend = side * L.width * (0.6f + 0.8f * uni(rng));
        L.dry = uni(rng) < 0.05f ? 0.6f : 0.0f;
        L.seed = id++;
        L.translucency = 0.85f;
        leaves.push_back(L);
    }
    for (std::size_t k = 0; k < leaves.size(); ++k) {
        LeafPaint& L = leaves[k];
        L.depth_ao = 0.7f + 0.3f * uni(rng);
        fitLeaf(c, L);
        paintLeaf(c, L, true);
        paintLeaf(c, L);
    }
    return c;
}

Canvas pineNeedles(int size) {
    // Borla de pino: brotes que salen en abanico de la punta de una ramita
    // (abajo en el centro) hacia los lados y hacia arriba; cada brote es un
    // cepillo de agujas por pares (fasciculos), mas cortas y claras en la
    // punta (las nuevas) y mas oscuras hacia dentro. La silueta es redonda y
    // esponjosa (de lejos, una mata de agujas; no una fronda). Las agujas de
    // atras, mas oscuras (profundidad).
    Canvas c(size);
    c.xs = 1.0f;  // tarjeta cuadrada (KindRules del pino)
    const float W = static_cast<float>(size) * c.xs;
    const float s = static_cast<float>(size);
    std::mt19937 rng(91U);
    std::uniform_real_distribution<float> uni(0.0f, 1.0f);
    const Vec3 twig_col{0.42f, 0.29f, 0.18f};
    const float margin = s * 0.12f;  // lo que sobresalen las agujas de la punta del brote
    // Acorta un brote hasta que su punta (con las agujas) cabe en el lienzo.
    const auto fit = [&](float x0, float y0, float angle, float length, float bend, int steps) {
        std::vector<std::array<float, 2>> pts;
        for (int it = 0; it < 16; ++it) {
            pts = curve(x0, y0, angle, length, bend, steps);
            bool inside = true;
            for (const auto& p : pts) {
                if (p[0] < margin || p[0] > W - margin || p[1] < margin) inside = false;
            }
            if (inside) break;
            length *= 0.9f;
        }
        return pts;
    };
    struct Shoot {
        std::vector<std::array<float, 2>> pts;
        float from;  // desde donde tiene agujas (0..1)
    };
    std::vector<Shoot> shoots;
    // La ramita que lleva la borla, y el abanico desde su punta.
    const auto stalk = curve(W * 0.5f, s * 0.995f, -kPi * 0.5f + 0.03f, s * 0.14f, 0.0f, 4);
    drawStem(c, stalk, s * 0.011f, s * 0.008f, twig_col, 0.0f, 1.1f);
    const float hub_x = stalk.back()[0];
    const float hub_y = stalk.back()[1];
    const int fan = 9;
    for (int k = 0; k < fan; ++k) {
        const float f = static_cast<float>(k) / static_cast<float>(fan - 1) * 2.0f - 1.0f;  // -1..1
        const float ang = -kPi * 0.5f + f * 1.25f + (uni(rng) - 0.5f) * 0.16f;
        // El del centro, el mas largo; los de los lados se curvan hacia arriba.
        const float len = s * (0.72f - 0.22f * std::abs(f)) * (0.88f + 0.2f * uni(rng));
        const float bend = -f * (0.25f + 0.15f * uni(rng));
        shoots.push_back({fit(hub_x, hub_y, ang, len, bend, 10), 0.1f});
    }
    for (const Shoot& sh : shoots) drawStem(c, sh.pts, s * 0.0065f, s * 0.0035f, twig_col, 0.0f, 1.1f);
    struct Needle {
        std::vector<std::array<float, 2>> pts;
        Vec3 color;
        float depth;
    };
    std::vector<Needle> needles;
    for (const Shoot& sh : shoots) {
        const auto& pts = sh.pts;
        float total = 0.0f;
        for (std::size_t i = 1; i < pts.size(); ++i) total += std::hypot(pts[i][0] - pts[i - 1][0], pts[i][1] - pts[i - 1][1]);
        const int count = static_cast<int>(total * (1.0f - sh.from) / (s * 0.0045f));
        const float steps = static_cast<float>(pts.size() - 1);
        for (int k = 0; k < count; ++k) {
            const float t = sh.from + (1.0f - sh.from) * (static_cast<float>(k) + uni(rng)) / static_cast<float>(std::max(count, 1));
            const float fi = t * steps;
            const auto i0 = std::min(static_cast<std::size_t>(fi), pts.size() - 2);
            const float f = fi - static_cast<float>(i0);
            const float px = mixF(pts[i0][0], pts[i0 + 1][0], f);
            const float py = mixF(pts[i0][1], pts[i0 + 1][1], f);
            const float dir = std::atan2(pts[i0 + 1][1] - pts[i0][1], pts[i0 + 1][0] - pts[i0][0]);
            for (int q = 0; q < 2; ++q) {
                const float side = uni(rng) < 0.5f ? -1.0f : 1.0f;
                // Hacia delante; en la punta, casi paralelas al brote.
                const float spread = mixF(0.6f + 0.6f * uni(rng), 0.2f + 0.35f * uni(rng), smoothstepF(0.75f, 1.0f, t));
                const float ang = dir + side * spread;
                const float len = s * (0.09f + 0.05f * uni(rng)) * (1.0f - 0.35f * smoothstepF(0.8f, 1.0f, t));
                Needle n;
                n.pts = curve(px, py, ang, len, -side * (0.1f + 0.25f * uni(rng)), 4);
                const float fresh = smoothstepF(0.6f, 1.0f, t);
                n.color = jitterColor(rng, mixV(Vec3{0.16f, 0.24f, 0.14f}, Vec3{0.28f, 0.37f, 0.17f}, fresh), 0.14f);
                if (uni(rng) < 0.012f) n.color = Vec3{0.4f, 0.34f, 0.18f};  // alguna seca
                n.depth = uni(rng);
                needles.push_back(n);
            }
        }
        // Yema terminal.
        const auto& tip = pts.back();
        const auto& before = pts[pts.size() - 2];
        const float tip_dir = std::atan2(tip[1] - before[1], tip[0] - before[0]);
        drawStem(c, curve(tip[0], tip[1], tip_dir, s * 0.022f, 0.0f, 3), s * 0.007f, s * 0.0025f, Vec3{0.5f, 0.38f, 0.25f}, 0.1f, 1.2f);
    }
    std::sort(needles.begin(), needles.end(), [](const Needle& a, const Needle& b) { return a.depth < b.depth; });
    for (const Needle& n : needles) {
        drawStem(c, n.pts, s * 0.0032f, s * 0.0026f, n.color * (0.65f + 0.35f * n.depth), 0.5f, 0.8f + 0.2f * n.depth, true);
    }
    for (std::size_t i = 0; i < c.ao.size(); ++i) c.ao[i] = c.a[i] > 0.0f ? c.ao[i] * (0.75f + 0.25f * clamp01(c.h[i])) : 1.0f;
    return c;
}

Canvas pineShoot(int size) {
    // Un brote de pino visto de lado (las borlas de cerca): el tallo a lo
    // largo y los pares de agujas alrededor. Las de los lados se ven enteras;
    // las de delante y detras, acortadas por la perspectiva y mas claras u
    // oscuras: la tarjeta parece un cilindro de agujas, no un peine. Mas
    // cortas y cerradas hacia la punta (las nuevas), con la yema.
    Canvas c(size);
    c.xs = 0.7f;  // tarjeta 0.7 de ancha (pineTuft)
    const float W = static_cast<float>(size) * c.xs;
    const float s = static_cast<float>(size);
    std::mt19937 rng(97U);
    std::uniform_real_distribution<float> uni(0.0f, 1.0f);
    const auto stem = curve(W * 0.5f, s * 0.995f, -kPi * 0.5f, s * 0.8f, 0.0f, 12);
    drawStem(c, stem, s * 0.022f, s * 0.011f, Vec3{0.42f, 0.29f, 0.18f}, 0.0f, 1.1f);
    struct Needle {
        std::vector<std::array<float, 2>> pts;
        Vec3 color;
        float depth;
    };
    std::vector<Needle> needles;
    const int fascicles = 170;
    const float margin = s * 0.012f;
    for (int k = 0; k < fascicles; ++k) {
        const float t = 0.06f + 0.94f * (static_cast<float>(k) + uni(rng)) / static_cast<float>(fascicles);
        const float fi = t * static_cast<float>(stem.size() - 1);
        const auto i0 = std::min(static_cast<std::size_t>(fi), stem.size() - 2);
        const float f = fi - static_cast<float>(i0);
        const float px = mixF(stem[i0][0], stem[i0 + 1][0], f);
        const float py = mixF(stem[i0][1], stem[i0 + 1][1], f);
        const float axis = std::atan2(stem[i0 + 1][1] - stem[i0][1], stem[i0 + 1][0] - stem[i0][0]);
        const float tip = smoothstepF(0.78f, 1.0f, t);
        for (int q = 0; q < 2; ++q) {
            const float phi = uni(rng) * 2.0f * kPi;  // donde esta alrededor del brote
            const float spread = mixF(0.6f + 0.5f * uni(rng), 0.25f + 0.3f * uni(rng), tip);  // angulo con el brote
            const float lateral = std::sin(spread) * std::sin(phi);
            const float forward = std::cos(spread);
            const float ang = axis + std::atan2(lateral, forward);
            float length = s * (0.24f + 0.08f * uni(rng)) * (1.0f - 0.4f * tip) * std::sqrt(lateral * lateral + forward * forward);
            // Que la punta no se salga por los lados.
            const float reach = std::abs(std::cos(ang)) * length;
            const float room = std::min(px - margin, W - margin - px);
            if (reach > room) length *= room / std::max(reach, 1e-3f);
            Needle n;
            n.pts = curve(px, py, ang, length, (lateral > 0.0f ? -1.0f : 1.0f) * (0.08f + 0.15f * uni(rng)), 4);
            const float fresh = smoothstepF(0.55f, 1.0f, t);
            const float front = 0.5f + 0.5f * std::cos(phi);  // 1 delante, 0 detras
            n.color = jitterColor(rng, mixV(Vec3{0.16f, 0.24f, 0.14f}, Vec3{0.28f, 0.37f, 0.17f}, fresh), 0.14f) * (0.62f + 0.38f * front);
            if (uni(rng) < 0.012f) n.color = Vec3{0.4f, 0.34f, 0.18f};  // alguna seca
            n.depth = front;
            needles.push_back(n);
        }
    }
    // Yema terminal.
    const auto& top = stem.back();
    drawStem(c, curve(top[0], top[1], -kPi * 0.5f, s * 0.04f, 0.0f, 3), s * 0.018f, s * 0.005f, Vec3{0.52f, 0.38f, 0.25f}, 0.1f, 1.2f);
    std::sort(needles.begin(), needles.end(), [](const Needle& a, const Needle& b) { return a.depth < b.depth; });
    for (const Needle& n : needles) {
        drawStem(c, n.pts, s * 0.0068f, s * 0.0055f, n.color, 0.5f, 0.8f + 0.2f * n.depth, true);
    }
    for (std::size_t i = 0; i < c.ao.size(); ++i) c.ao[i] = c.a[i] > 0.0f ? c.ao[i] * (0.75f + 0.25f * clamp01(c.h[i])) : 1.0f;
    return c;
}

Canvas firNeedles(int size) {
    // Rama plana de abeto: un eje y ramitas alternas a los dos lados, largas
    // abajo y cortas arriba (silueta triangular), todas cubiertas de agujas
    // cortas en peine. Llena la tarjeta (antes un peine estrecho y oscuro: la
    // copa del abeto se veia rala, como seca).
    Canvas c(size);
    c.xs = 0.9f;  // tarjeta 0.9 de ancha (KindRules del abeto)
    const float W = static_cast<float>(size) * c.xs;
    const float s = static_cast<float>(size);
    std::mt19937 rng(123U);
    std::uniform_real_distribution<float> uni(0.0f, 1.0f);
    const Vec3 twig_col{0.4f, 0.29f, 0.18f};
    std::vector<std::vector<std::array<float, 2>>> twigs;
    const auto main = curve(W * 0.5f, s * 0.995f, -kPi * 0.5f, s * 0.9f, 0.05f, 18);
    twigs.push_back(main);
    const float margin = s * 0.07f;
    for (int k = 0; k < 14; ++k) {
        const float t = 0.06f + 0.06f * static_cast<float>(k);
        const auto& p = main[static_cast<std::size_t>(t * 18.0f)];
        const float side = k % 2 == 0 ? -1.0f : 1.0f;
        float len = s * 0.5f * (1.0f - 0.78f * t) * (0.88f + 0.2f * uni(rng));
        const float ang = -kPi * 0.5f + side * (0.95f + 0.18f * uni(rng));
        std::vector<std::array<float, 2>> tw;
        for (int it = 0; it < 12; ++it) {
            tw = curve(p[0], p[1], ang, len, -side * 0.22f, 8);
            const auto& e = tw.back();
            if (e[0] > margin && e[0] < W - margin && e[1] > margin) break;
            len *= 0.9f;
        }
        twigs.push_back(tw);
    }
    for (std::size_t k = 0; k < twigs.size(); ++k) drawStem(c, twigs[k], s * (k == 0 ? 0.008f : 0.0045f), s * 0.0022f, twig_col, 0.0f);
    // Agujas cortas en peine a los dos lados de cada ramita.
    struct Needle {
        std::vector<std::array<float, 2>> pts;
        Vec3 color;
        float depth;
    };
    std::vector<Needle> needles;
    for (std::size_t k = 0; k < twigs.size(); ++k) {
        const auto& tw = twigs[k];
        float total = 0.0f;
        for (std::size_t i = 1; i < tw.size(); ++i) total += std::hypot(tw[i][0] - tw[i - 1][0], tw[i][1] - tw[i - 1][1]);
        const int count = static_cast<int>(total / (s * 0.0036f));
        for (int n = 0; n < count; ++n) {
            const float t = (static_cast<float>(n) + uni(rng) * 0.5f) / static_cast<float>(std::max(count, 1));
            const float fi = t * static_cast<float>(tw.size() - 1);
            const auto i0 = std::min(static_cast<std::size_t>(fi), tw.size() - 2);
            const float f = fi - static_cast<float>(i0);
            const float px = mixF(tw[i0][0], tw[i0 + 1][0], f), py = mixF(tw[i0][1], tw[i0 + 1][1], f);
            const float dir = std::atan2(tw[i0 + 1][1] - tw[i0][1], tw[i0 + 1][0] - tw[i0][0] + 1e-6f);
            const float side = n % 2 == 0 ? -1.0f : 1.0f;
            const float ang = dir + side * (1.0f + 0.3f * uni(rng));
            // Mas cortas en la punta de cada ramita (las nuevas).
            const float len = s * (0.04f + 0.02f * uni(rng)) * (k == 0 ? 1.1f : 1.0f) * (1.0f - 0.35f * smoothstepF(0.8f, 1.0f, t));
            const float fresh = smoothstepF(0.7f, 1.0f, t);
            Needle nd;
            nd.pts = curve(px, py, ang, len, side * 0.15f, 3);
            nd.color = jitterColor(rng, mixV(Vec3{0.1f, 0.19f, 0.1f}, Vec3{0.24f, 0.36f, 0.15f}, fresh), 0.12f);
            nd.depth = uni(rng);
            needles.push_back(nd);
        }
    }
    std::sort(needles.begin(), needles.end(), [](const Needle& a, const Needle& b) { return a.depth < b.depth; });
    for (const Needle& n : needles) {
        drawStem(c, n.pts, s * 0.0042f, s * 0.0032f, n.color * (0.7f + 0.3f * n.depth), 0.45f, 0.85f + 0.15f * n.depth, true);
    }
    return c;
}

Canvas palmFrond(int size) {
    Canvas c(size);
    const float s = static_cast<float>(size);
    std::mt19937 rng(131U);
    std::uniform_real_distribution<float> uni(0.0f, 1.0f);
    // Nervio a lo largo de v (la base abajo) y foliolos hacia la punta.
    std::vector<std::array<float, 2>> rachis = {{s * 0.5f, s * 1.0f}, {s * 0.5f, s * 0.0f}};
    std::vector<LeafPaint> leaflets;
    std::uint32_t id = 900U;
    for (int k = 0; k < 64; ++k) {
        const float t = (static_cast<float>(k) + 0.5f) / 64.0f;
        const float y = s * (1.0f - t);
        for (int side = -1; side <= 1; side += 2) {
            LeafPaint L;
            L.x = s * 0.5f;
            L.y = y + (uni(rng) - 0.5f) * s * 0.004f;
            L.angle = -kPi * 0.5f + static_cast<float>(side) * (0.95f + 0.15f * uni(rng));
            L.length = s * 0.5f;
            L.width = s * (0.011f + 0.004f * uni(rng));
            L.shape = 3;
            L.color = jitterColor(rng, Vec3{0.24f, 0.36f, 0.11f}, 0.1f);
            L.vein_color = Vec3{0.42f, 0.46f, 0.2f};
            L.petiole = 0.02f;
            L.dry = uni(rng) < 0.12f ? 0.5f + 0.4f * uni(rng) : 0.0f;
            L.bend = static_cast<float>(side) * s * 0.012f;
            L.seed = id++;
            L.translucency = 0.75f;
            L.depth_ao = 0.8f + 0.2f * uni(rng);
            leaflets.push_back(L);
        }
    }
    for (LeafPaint& L : leaflets) {
        fitLeaf(c, L);
        paintLeaf(c, L, true);
        paintLeaf(c, L);
    }
    drawStem(c, rachis, s * 0.014f, s * 0.006f, Vec3{0.55f, 0.52f, 0.28f}, 0.3f, 1.2f);
    return c;
}

// Canvas -> niveles RGBA8. Color con alfa (las hojas conservan su cobertura
// en los mips) y normal (xy) + oclusion + extra.
void toLevels(Canvas c, bool alpha_test, float normal_strength, std::vector<std::vector<std::uint8_t>>& albedo,
              std::vector<std::vector<std::uint8_t>>& normal, std::uint32_t mips) {
    auto size = static_cast<std::uint32_t>(c.size);
    // Normal de la altura (en el nivel 0, antes de reducir).
    std::vector<float> nx(c.r.size()), ny(c.r.size());
    {
        const int n = c.size;
        const float strength = normal_strength * static_cast<float>(n) / 512.0f;
        parallelRows(n, [&](int y) {
            for (int x = 0; x < n; ++x) {
                const auto hat = [&](int xx, int yy) {
                    if (alpha_test) {
                        xx = std::clamp(xx, 0, n - 1);
                        yy = std::clamp(yy, 0, n - 1);
                    }
                    return c.h[c.at(xx, yy)];
                };
                const float dx = (hat(x + 1, y) - hat(x - 1, y)) * strength / c.xs;
                const float dy = (hat(x, y + 1) - hat(x, y - 1)) * strength;
                const Vec3 nn = core::normalize(Vec3{-dx, -dy, 1.0f});
                const std::size_t i = c.at(x, y);
                nx[i] = nn.x;
                ny[i] = nn.y;
            }
        });
    }
    std::vector<float> r = c.r, g = c.g, b = c.b, a = c.a, ao = c.ao, ex = c.ex;
    // Color de las zonas transparentes: el de las hojas cercanas (sin bordes oscuros).
    if (alpha_test) {
        for (int pass = 0; pass < 7; ++pass) {
            std::vector<float> r2 = r, g2 = g, b2 = b, ao2 = ao, ex2 = ex;
            parallelRows(static_cast<int>(size), [&](int row) {
                const auto y = static_cast<std::uint32_t>(row);
                for (std::uint32_t x = 0; x < size; ++x) {
                    const std::size_t i = static_cast<std::size_t>(y) * size + x;
                    if (a[i] > 0.01f) continue;
                    float sr = 0.0f, sg = 0.0f, sb = 0.0f, sao = 0.0f, sex = 0.0f, w = 0.0f;
                    for (int oy = -1; oy <= 1; ++oy) {
                        for (int ox = -1; ox <= 1; ++ox) {
                            const int xx = static_cast<int>(x) + ox * (1 << pass);
                            const int yy = static_cast<int>(y) + oy * (1 << pass);
                            if (xx < 0 || yy < 0 || xx >= static_cast<int>(size) || yy >= static_cast<int>(size)) continue;
                            const std::size_t j = static_cast<std::size_t>(yy) * size + static_cast<std::size_t>(xx);
                            if (r[j] + g[j] + b[j] <= 0.0f) continue;
                            sr += r[j];
                            sg += g[j];
                            sb += b[j];
                            sao += ao[j];
                            sex += ex[j];
                            w += 1.0f;
                        }
                    }
                    if (w > 0.0f) {
                        r2[i] = sr / w;
                        g2[i] = sg / w;
                        b2[i] = sb / w;
                        ao2[i] = sao / w;
                        ex2[i] = sex / w;
                    }
                }
            });
            r = std::move(r2);
            g = std::move(g2);
            b = std::move(b2);
            ao = std::move(ao2);
            ex = std::move(ex2);
        }
    }
    float coverage0 = 0.0f;
    for (float v : a) coverage0 += v > 0.5f ? 1.0f : 0.0f;
    coverage0 /= static_cast<float>(a.size());

    for (std::uint32_t level = 0; level < mips; ++level) {
        const std::size_t count = static_cast<std::size_t>(size) * size;
        // Cobertura: escala del alfa de este nivel.
        float scale = 1.0f;
        if (alpha_test && level > 0) {
            float lo = 0.5f;
            float hi = 8.0f;
            for (int it = 0; it < 18; ++it) {
                const float mid = 0.5f * (lo + hi);
                float cov = 0.0f;
                for (float v : a) cov += v * mid > 0.5f ? 1.0f : 0.0f;
                cov /= static_cast<float>(count);
                if (cov < coverage0) lo = mid; else hi = mid;
            }
            scale = 0.5f * (lo + hi);
        }
        std::vector<std::uint8_t> out(count * 4);
        std::vector<std::uint8_t> nout(count * 4);
        const auto byte = [](float v) { return static_cast<std::uint8_t>(clamp01(v) * 255.0f + 0.5f); };
        for (std::size_t i = 0; i < count; ++i) {
            // El lienzo esta en sRGB: se guarda tal cual (formato sRGB).
            out[i * 4 + 0] = byte(r[i]);
            out[i * 4 + 1] = byte(g[i]);
            out[i * 4 + 2] = byte(b[i]);
            out[i * 4 + 3] = byte(alpha_test ? a[i] * scale : 1.0f);
            nout[i * 4 + 0] = byte(nx[i] * 0.5f + 0.5f);
            nout[i * 4 + 1] = byte(ny[i] * 0.5f + 0.5f);
            nout[i * 4 + 2] = byte(ao[i]);
            nout[i * 4 + 3] = byte(ex[i]);
        }
        albedo.push_back(std::move(out));
        normal.push_back(std::move(nout));
        if (size == 1) break;
        // Siguiente nivel (media 2x2; el color de las hojas pesado por la cobertura).
        const std::uint32_t next = std::max(size / 2, 1U);
        const std::size_t n2 = static_cast<std::size_t>(next) * next;
        std::vector<float> r2(n2), g2(n2), b2(n2), a2(n2), nx2(n2), ny2(n2), ao2(n2), ex2(n2);
        for (std::uint32_t y = 0; y < next; ++y) {
            for (std::uint32_t x = 0; x < next; ++x) {
                float sr = 0.0f, sg = 0.0f, sb = 0.0f, sa = 0.0f, sx = 0.0f, sy = 0.0f, sao = 0.0f, sex = 0.0f, w = 0.0f;
                for (std::uint32_t oy = 0; oy < 2; ++oy) {
                    for (std::uint32_t ox = 0; ox < 2; ++ox) {
                        const std::size_t j = static_cast<std::size_t>(std::min(y * 2 + oy, size - 1)) * size + std::min(x * 2 + ox, size - 1);
                        const float wj = alpha_test ? 0.15f + a[j] : 1.0f;
                        sr += r[j] * wj;
                        sg += g[j] * wj;
                        sb += b[j] * wj;
                        sx += nx[j] * wj;
                        sy += ny[j] * wj;
                        sao += ao[j] * wj;
                        sex += ex[j] * wj;
                        w += wj;
                        sa += a[j];
                    }
                }
                const std::size_t k = static_cast<std::size_t>(y) * next + x;
                r2[k] = sr / w;
                g2[k] = sg / w;
                b2[k] = sb / w;
                nx2[k] = sx / w;
                ny2[k] = sy / w;
                ao2[k] = sao / w;
                ex2[k] = sex / w;
                a2[k] = sa * 0.25f;
            }
        }
        r = std::move(r2);
        g = std::move(g2);
        b = std::move(b2);
        a = std::move(a2);
        nx = std::move(nx2);
        ny = std::move(ny2);
        ao = std::move(ao2);
        ex = std::move(ex2);
        size = next;
    }
}

}  // namespace

const char* treeKindName(TreeKind kind) {
    switch (kind) {
        case TreeKind::Pine: return "Pino";
        case TreeKind::Fir: return "Abeto";
        case TreeKind::Oak: return "Roble";
        case TreeKind::Birch: return "Abedul";
        case TreeKind::Palm: return "Palmera";
        case TreeKind::Willow: return "Sauce";
    }
    return "Roble";
}

bool TreeSpecies::operator==(const TreeSpecies& o) const {
    return kind == o.kind && seed == o.seed && height == o.height && leaf_density == o.leaf_density &&
           leaf_size == o.leaf_size && branch_density == o.branch_density && gnarl == o.gnarl &&
           leaf_color.x == o.leaf_color.x && leaf_color.y == o.leaf_color.y && leaf_color.z == o.leaf_color.z &&
           bark_color.x == o.bark_color.x && bark_color.y == o.bark_color.y && bark_color.z == o.bark_color.z;
}

TreeSpecies treePreset(TreeKind kind) {
    TreeSpecies s;
    s.kind = kind;
    switch (kind) {
        case TreeKind::Pine:
            s.height = 16.0f; s.leaf_color = {1.0f, 1.0f, 1.0f}; s.bark_color = {1.0f, 1.0f, 1.0f};
            break;
        case TreeKind::Fir:
            s.height = 15.0f; s.leaf_color = {0.95f, 1.0f, 1.05f}; s.bark_color = {0.78f, 0.8f, 0.85f};
            break;
        case TreeKind::Oak:
            s.height = 12.0f; s.leaf_color = {1.0f, 1.0f, 1.0f}; s.bark_color = {1.0f, 1.0f, 1.0f};
            break;
        case TreeKind::Birch:
            s.height = 13.0f; s.leaf_color = {1.0f, 1.0f, 1.0f}; s.bark_color = {1.0f, 1.0f, 1.0f};
            break;
        case TreeKind::Palm:
            s.height = 10.0f; s.leaf_color = {1.0f, 1.0f, 1.0f}; s.bark_color = {1.0f, 1.0f, 1.0f};
            break;
        case TreeKind::Willow:
            s.height = 10.0f; s.leaf_color = {1.0f, 1.0f, 1.0f}; s.bark_color = {0.85f, 0.85f, 0.82f};
            break;
    }
    return s;
}

TreeMeshData buildTree(const TreeSpecies& species, int lod) {
    Builder builder(species, std::clamp(lod, 0, 2));
    builder.build();
    return std::move(builder.mesh);
}

TreeTextures generateTreeTextures(std::uint32_t size) {
    TreeTextures t;
    t.size = size;
    std::uint32_t mips = 1;
    for (std::uint32_t s = size; s > 1; s /= 2) ++mips;
    t.mips = mips;
    t.albedo.resize(kTreeLayerCount);
    t.normal.resize(kTreeLayerCount);
    const int n = static_cast<int>(size);
    // Cada capa en su hilo (son independientes).
    struct Job {
        std::uint32_t layer;
        bool leaf;
        float strength;
    };
    const std::array<Job, kTreeLayerCount> jobs = {{
        {kTreeLayerBark, false, 4.0f},
        {kTreeLayerLeaves, true, 1.6f},
        {kTreeLayerNeedles, true, 1.2f},
        {kTreeLayerFrond, true, 1.2f},
        {kTreeLayerBirchBark, false, 2.0f},
        {kTreeLayerPineBark, false, 4.0f},
        {kTreeLayerBirchLeaves, true, 1.5f},
        {kTreeLayerWillowLeaves, true, 1.3f},
        {kTreeLayerFirNeedles, true, 1.2f},
        {kTreeLayerPalmBark, false, 3.0f},
        {kTreeLayerPineShoot, true, 1.2f},
    }};
    const auto run = [&](const Job& job) {
        Canvas c = [&]() {
            switch (job.layer) {
                case kTreeLayerBark: return oakBark(n);
                case kTreeLayerLeaves: return oakLeaves(n);
                case kTreeLayerNeedles: return pineNeedles(n);
                case kTreeLayerFrond: return palmFrond(n);
                case kTreeLayerBirchBark: return birchBark(n);
                case kTreeLayerPineBark: return pineBark(n);
                case kTreeLayerBirchLeaves: return birchLeaves(n);
                case kTreeLayerWillowLeaves: return willowLeaves(n);
                case kTreeLayerFirNeedles: return firNeedles(n);
                case kTreeLayerPineShoot: return pineShoot(n);
                default: return palmBark(n);
            }
        }();
        toLevels(std::move(c), job.leaf, job.strength, t.albedo[job.layer], t.normal[job.layer], mips);
    };
    const unsigned threads = std::clamp(std::thread::hardware_concurrency(), 1u, static_cast<unsigned>(jobs.size()));
    std::vector<std::thread> pool;
    for (unsigned w = 0; w < threads; ++w) {
        pool.emplace_back([&, w]() {
            for (std::size_t j = w; j < jobs.size(); j += threads) run(jobs[j]);
        });
    }
    for (std::thread& th : pool) th.join();
    return t;
}

}  // namespace cramion::asset
