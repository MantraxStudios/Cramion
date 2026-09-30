#include "CramionFX/asset/TreeGenerator.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <random>

namespace cramion::asset {

namespace {

using core::Vec3;

constexpr float kPi = 3.14159265f;
constexpr float kGolden = 2.39996323f;  // 137.5 grados (filotaxia)

std::uint32_t rgba(Vec3 c, float a) {
    const auto b = [](float v) { return static_cast<std::uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return b(c.x) | (b(c.y) << 8) | (b(c.z) << 16) | (b(a) << 24);
}

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
    float leaf_size = 0.8f;         // metros (a 11 m de altura)
    float leaves = 2.0f;            // tarjetas por tramo de rama fina
    std::uint32_t leaf_layer = kTreeLayerLeaves;
    std::uint32_t bark_layer = kTreeLayerBark;
    float leaf_start = 0.25f;       // desde que parte de la rama fina hay hojas
};

KindRules rulesFor(TreeKind kind) {
    KindRules r;
    switch (kind) {
        case TreeKind::Pine:
            r.levels = 2; r.trunk_radius = 0.024f; r.crown_start = 0.55f;
            r.branches = {16, 7, 0}; r.angle = {78.0f, 42.0f, 0.0f}; r.length = {0.3f, 0.5f, 0.0f};
            r.droop = {0.12f, 0.15f, 0.0f}; r.up = {0.12f, 0.35f, 0.0f}; r.gnarl = 0.25f; r.shape = 3;
            r.leaf_size = 1.15f; r.leaves = 2.2f; r.leaf_layer = kTreeLayerNeedles; r.leaf_start = 0.2f;
            break;
        case TreeKind::Fir:
            r.levels = 2; r.trunk_radius = 0.022f; r.crown_start = 0.1f;
            r.branches = {46, 6, 0}; r.angle = {96.0f, 48.0f, 0.0f}; r.length = {0.34f, 0.34f, 0.0f};
            r.droop = {0.32f, 0.25f, 0.0f}; r.up = {0.0f, 0.12f, 0.0f}; r.gnarl = 0.12f; r.shape = 0;
            r.leaf_size = 1.0f; r.leaves = 2.2f; r.leaf_layer = kTreeLayerNeedles; r.leaf_start = 0.05f;
            break;
        case TreeKind::Oak:
            r.levels = 3; r.trunk_radius = 0.05f; r.crown_start = 0.28f;
            r.branches = {7, 7, 6}; r.angle = {48.0f, 50.0f, 55.0f}; r.length = {0.62f, 0.55f, 0.48f};
            r.droop = {0.04f, 0.14f, 0.24f}; r.up = {0.22f, 0.14f, 0.06f}; r.gnarl = 0.75f; r.shape = 1;
            r.leaf_size = 0.95f; r.leaves = 2.2f;
            break;
        case TreeKind::Birch:
            r.levels = 3; r.trunk_radius = 0.022f; r.crown_start = 0.34f;
            r.branches = {10, 6, 4}; r.angle = {34.0f, 44.0f, 55.0f}; r.length = {0.42f, 0.5f, 0.5f};
            r.droop = {0.1f, 0.35f, 0.65f}; r.up = {0.3f, 0.08f, 0.0f}; r.gnarl = 0.3f; r.shape = 2;
            r.leaf_size = 0.62f; r.leaves = 2.4f; r.bark_layer = kTreeLayerBirchBark;
            break;
        case TreeKind::Willow:
            r.levels = 3; r.trunk_radius = 0.045f; r.crown_start = 0.3f;
            r.branches = {7, 8, 6}; r.angle = {46.0f, 40.0f, 28.0f}; r.length = {0.55f, 0.62f, 0.75f};
            r.droop = {0.08f, 0.6f, 1.3f}; r.up = {0.3f, 0.0f, 0.0f}; r.gnarl = 0.45f; r.shape = 1;
            r.leaf_size = 0.5f; r.leaves = 3.2f; r.leaf_start = 0.1f;
            break;
        case TreeKind::Palm:
            r.levels = 0; r.trunk_radius = 0.022f; r.crown_start = 1.0f; r.gnarl = 0.0f;
            r.leaf_layer = kTreeLayerFrond;
            break;
    }
    return r;
}

// Forma de la copa: cuanto miden las ramas del tronco a una altura relativa
// t (0 = donde empieza la copa, 1 = la punta).
float crownShape(int shape, float t) {
    switch (shape) {
        case 0: return 0.08f + 0.92f * (1.0f - t);                              // cono (abeto)
        case 2: return 0.35f + 0.65f * std::sin(kPi * std::clamp(0.1f + 0.75f * t, 0.0f, 1.0f));  // llama
        case 3: return 0.45f + 0.55f * std::sin(kPi * std::clamp(0.15f + 0.85f * t, 0.0f, 1.0f)); // sombrilla
        default: return 0.3f + 0.7f * std::sin(kPi * std::clamp(0.12f + 0.88f * t, 0.0f, 1.0f));  // elipsoide
    }
}

// Dos vectores perpendiculares a `d`.
void perpendiculars(const Vec3& d, Vec3& a, Vec3& b) {
    const Vec3 helper = std::abs(d.y) < 0.95f ? Vec3{0.0f, 1.0f, 0.0f} : Vec3{1.0f, 0.0f, 0.0f};
    a = core::normalize(core::cross(helper, d));
    b = core::cross(d, a);
}

struct Branch {
    std::vector<Vec3> points;
    std::vector<float> radii;
    int level = 0;
    float length = 0.0f;
};

struct Builder {
    const TreeSpecies& species;
    KindRules rules;
    int lod;
    std::mt19937 rng;
    TreeMeshData mesh;
    std::vector<Branch> branches;
    Vec3 crown_center{};
    float crown_radius = 3.0f;

    Builder(const TreeSpecies& s, int l) : species(s), rules(rulesFor(s.kind)), lod(l), rng(s.seed * 7919U + 13U) {}

    float random(float lo, float hi) { return std::uniform_real_distribution<float>(lo, hi)(rng); }

    float windAt(float y) const { return std::pow(std::clamp(y / std::max(species.height, 0.1f), 0.0f, 1.0f), 1.6f); }

    // --- Esqueleto ---
    Branch growBranch(Vec3 origin, Vec3 direction, float length, float radius, int level) {
        Branch b;
        b.level = level;
        b.length = length;
        const int segments = level == 0 ? 14 : std::max(3, 7 - level * 2);
        const float step = length / static_cast<float>(segments);
        Vec3 p = origin;
        Vec3 d = core::normalize(direction);
        const float gnarl = rules.gnarl * species.gnarl;
        const int li = std::clamp(level - 1, 0, 2);
        for (int i = 0; i <= segments; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(segments);
            b.points.push_back(p);
            // Se afina hacia la punta (un poco mas ancho en la base del tronco).
            const float flare = level == 0 ? 1.0f + 0.5f * std::pow(std::max(0.0f, 1.0f - t * 6.0f), 2.0f) : 1.0f;
            b.radii.push_back(radius * (1.0f - 0.85f * t) * flare);
            if (level == 0) {
                d = core::normalize(d + Vec3{random(-1.0f, 1.0f), 0.0f, random(-1.0f, 1.0f)} * (0.06f * gnarl) +
                                    Vec3{0.0f, 0.05f, 0.0f});
            } else {
                d = core::normalize(d + Vec3{0.0f, -rules.droop[static_cast<std::size_t>(li)] * (0.4f + t), 0.0f} *
                                            (step / std::max(length, 0.1f) * 3.0f) +
                                    Vec3{0.0f, rules.up[static_cast<std::size_t>(li)], 0.0f} * (step / std::max(length, 0.1f) * 3.0f) +
                                    Vec3{random(-1.0f, 1.0f), random(-1.0f, 1.0f), random(-1.0f, 1.0f)} * (0.25f * gnarl));
            }
            p = p + d * step;
        }
        return b;
    }

    void spawnChildren(const Branch& parent, int level) {
        if (level > rules.levels) return;
        const int li = level - 1;
        const float density = std::max(species.branch_density, 0.05f);
        int count = static_cast<int>(std::round(static_cast<float>(rules.branches[static_cast<std::size_t>(li)]) * density));
        // Lejos, menos ramas finas.
        if (lod >= 1 && level == rules.levels && rules.levels > 1) count = 0;
        if (lod >= 2 && level >= 2) count = 0;
        if (count <= 0) return;
        const bool from_trunk = parent.level == 0;
        const float start = from_trunk ? rules.crown_start : 0.25f;
        const float azimuth0 = random(0.0f, 2.0f * kPi);
        for (int i = 0; i < count; ++i) {
            float t = start + (1.0f - start) * (static_cast<float>(i) + random(0.1f, 0.9f)) / static_cast<float>(count);
            t = std::clamp(t, 0.0f, 0.97f);
            const float fi = t * static_cast<float>(parent.points.size() - 1);
            const auto i0 = static_cast<std::size_t>(fi);
            const std::size_t i1 = std::min(i0 + 1, parent.points.size() - 1);
            const float f = fi - static_cast<float>(i0);
            const Vec3 at = parent.points[i0] * (1.0f - f) + parent.points[i1] * f;
            const float parent_radius = parent.radii[i0] * (1.0f - f) + parent.radii[i1] * f;
            const Vec3 pd = core::normalize(parent.points[i1] - parent.points[i0]);
            Vec3 a;
            Vec3 b;
            perpendiculars(pd, a, b);
            const float azimuth = azimuth0 + static_cast<float>(i) * kGolden + random(-0.3f, 0.3f);
            const Vec3 around = a * std::cos(azimuth) + b * std::sin(azimuth);
            const float incline = (rules.angle[static_cast<std::size_t>(li)] + random(-8.0f, 8.0f)) * kPi / 180.0f;
            const Vec3 dir = core::normalize(pd * std::cos(incline) + around * std::sin(incline));
            float length = parent.length * rules.length[static_cast<std::size_t>(li)] * random(0.8f, 1.2f);
            if (from_trunk) {
                const float rel = (t - rules.crown_start) / std::max(1.0f - rules.crown_start, 0.05f);
                length = species.height * rules.length[0] * crownShape(rules.shape, rel) * random(0.85f, 1.15f);
            } else {
                length *= 1.0f - 0.5f * t;  // las de la punta del padre, mas cortas
            }
            const float radius = std::min(parent_radius * 0.72f, parent_radius * std::pow(length / std::max(parent.length, 0.1f), 0.9f) + 0.004f);
            Branch child = growBranch(at, dir, length, std::max(radius, 0.006f), level);
            branches.push_back(child);
            spawnChildren(child, level + 1);
        }
    }

    // --- Mallas ---
    void tube(const Branch& b, int sides) {
        const auto first = static_cast<std::uint32_t>(mesh.vertices.size());
        const std::uint32_t layer = rules.bark_layer;
        const Vec3 bark = species.bark_color;
        float v = 0.0f;
        Vec3 prev_a{};
        for (std::size_t i = 0; i < b.points.size(); ++i) {
            const Vec3 d = core::normalize(i + 1 < b.points.size() ? b.points[i + 1] - b.points[i]
                                                                   : b.points[i] - b.points[i - 1]);
            Vec3 a;
            Vec3 c;
            perpendiculars(d, a, c);
            // Sin giros bruscos del anillo de un tramo al siguiente.
            if (i > 0) {
                a = core::normalize(prev_a - d * core::dot(prev_a, d));
                c = core::cross(d, a);
            }
            prev_a = a;
            if (i > 0) v += core::length(b.points[i] - b.points[i - 1]) / std::max(2.0f * kPi * b.radii[0], 0.05f) * 0.35f;
            const float ao = b.level == 0 ? std::clamp(0.45f + b.points[i].y / species.height, 0.45f, 1.0f) : 0.8f;
            for (int k = 0; k <= sides; ++k) {
                const float angle = 2.0f * kPi * static_cast<float>(k) / static_cast<float>(sides);
                const Vec3 n = a * std::cos(angle) + c * std::sin(angle);
                const Vec3 p = b.points[i] + n * b.radii[i];
                const float wind = windAt(p.y) * (b.level == 0 ? 0.25f : 0.6f + 0.2f * static_cast<float>(b.level));
                mesh.vertices.push_back({p.x, p.y, p.z, n.x, n.y, n.z, rgba(bark, ao), wind,
                                         static_cast<float>(k) / static_cast<float>(sides) * (b.level == 0 ? 2.0f : 1.0f), v,
                                         static_cast<float>(layer), 0.0f});
            }
        }
        const auto row = static_cast<std::uint32_t>(sides + 1);
        for (std::uint32_t i = 0; i + 1 < b.points.size(); ++i) {
            for (std::uint32_t k = 0; k < static_cast<std::uint32_t>(sides); ++k) {
                const std::uint32_t p0 = first + i * row + k;
                const std::uint32_t p1 = p0 + row;
                mesh.indices.insert(mesh.indices.end(), {p0, p1, p0 + 1, p0 + 1, p1, p1 + 1});
            }
        }
    }

    // Tarjeta de hojas centrada en `center`, con el lado largo por `along`.
    void leafCard(const Vec3& center, const Vec3& along_in, float size, float aspect) {
        Vec3 outward = center - crown_center;
        outward = core::length(outward) > 1e-3f ? core::normalize(outward) : Vec3{0.0f, 1.0f, 0.0f};
        // Orientacion al azar alrededor de la rama, algo hacia fuera y arriba.
        Vec3 normal = core::normalize(outward * 0.6f + Vec3{random(-1.0f, 1.0f), random(-0.3f, 1.0f), random(-1.0f, 1.0f)});
        Vec3 along = core::normalize(along_in - normal * core::dot(along_in, normal));
        if (!std::isfinite(along.x) || core::length(along) < 1e-3f) {
            Vec3 x;
            Vec3 y;
            perpendiculars(normal, x, y);
            along = x;
        }
        const Vec3 side = core::cross(normal, along);
        const float half_along = size * 0.5f * aspect;
        const float half_side = size * 0.5f;
        // Normales esfericas: la copa se ilumina como un volumen.
        const Vec3 lit = core::normalize(normal * 0.4f + outward * 0.6f + Vec3{0.0f, 0.1f, 0.0f});
        // Oclusion: el interior de la copa y su parte de abajo, mas oscuros.
        const float depth = std::clamp(core::length(center - crown_center) / std::max(crown_radius, 0.5f), 0.0f, 1.0f);
        const float below = std::clamp(0.5f - 0.5f * outward.y, 0.0f, 1.0f);
        const float ao = std::clamp((0.2f + 0.8f * depth * depth) * (1.0f - 0.35f * below), 0.15f, 1.0f);
        const float tone = random(0.85f, 1.12f);
        const Vec3 color = species.leaf_color * tone;
        const auto first = static_cast<std::uint32_t>(mesh.vertices.size());
        const float wind = windAt(center.y) * 0.9f + 0.1f;
        const float layer = static_cast<float>(rules.leaf_layer);
        const std::array<std::array<float, 2>, 4> corners = {{{-1.0f, -1.0f}, {1.0f, -1.0f}, {1.0f, 1.0f}, {-1.0f, 1.0f}}};
        for (const auto& c : corners) {
            const Vec3 p = center + along * (c[0] * half_along) + side * (c[1] * half_side);
            mesh.vertices.push_back({p.x, p.y, p.z, lit.x, lit.y, lit.z, rgba(color, ao), wind, c[0] * 0.5f + 0.5f,
                                     c[1] * 0.5f + 0.5f, layer, 1.0f});
        }
        mesh.indices.insert(mesh.indices.end(), {first, first + 1, first + 2, first, first + 2, first + 3});
    }

    void leavesOn(const Branch& b, float count_scale, float size_scale) {
        const float base = rules.leaf_size * species.leaf_size * (species.height / 11.0f) * size_scale;
        const bool needles = rules.leaf_layer == kTreeLayerNeedles;
        for (std::size_t i = 1; i < b.points.size(); ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(b.points.size() - 1);
            if (t < rules.leaf_start) continue;
            const Vec3 d = core::normalize(b.points[i] - b.points[i - 1]);
            float cards = rules.leaves * species.leaf_density * count_scale;
            int n = static_cast<int>(cards);
            if (random(0.0f, 1.0f) < cards - static_cast<float>(n)) ++n;
            for (int k = 0; k < n; ++k) {
                const float f = random(0.0f, 1.0f);
                Vec3 at = b.points[i - 1] * (1.0f - f) + b.points[i] * f;
                const float spread = base * (needles ? 0.2f : 0.35f);
                at = at + Vec3{random(-spread, spread), random(-spread, spread), random(-spread, spread)};
                const float size = base * random(0.75f, 1.25f);
                leafCard(at, needles ? d : Vec3{random(-1.0f, 1.0f), random(-0.5f, 0.5f), random(-1.0f, 1.0f)}, size,
                         needles ? 1.6f : 1.0f);
            }
        }
    }

    // --- Palmera ---
    void buildPalm() {
        const float h = species.height;
        Branch trunk;
        trunk.level = 0;
        trunk.length = h;
        const float lean = random(0.05f, 0.18f) * species.gnarl;
        const float azimuth = random(0.0f, 2.0f * kPi);
        const Vec3 lean_dir{std::cos(azimuth), 0.0f, std::sin(azimuth)};
        const int segments = lod == 0 ? 16 : 8;
        const float radius = h * rules.trunk_radius;
        for (int i = 0; i <= segments; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(segments);
            trunk.points.push_back(Vec3{0.0f, h * t, 0.0f} + lean_dir * (lean * h * t * t));
            trunk.radii.push_back(radius * (1.25f - 0.45f * t) * (1.0f + 0.06f * std::sin(t * 60.0f)));
        }
        tube(trunk, lod == 0 ? 10 : (lod == 1 ? 6 : 4));
        const Vec3 top = trunk.points.back();
        crown_center = top;
        crown_radius = h * 0.45f;
        const int fronds = static_cast<int>(std::round((lod == 2 ? 7.0f : 12.0f) * std::max(species.leaf_density, 0.3f)));
        const int steps = lod == 0 ? 10 : (lod == 1 ? 6 : 3);
        const float frond_length = h * 0.42f * species.leaf_size;
        for (int f = 0; f < fronds; ++f) {
            const float a = static_cast<float>(f) * kGolden + random(-0.2f, 0.2f);
            const Vec3 out{std::cos(a), 0.0f, std::sin(a)};
            const float rise = random(0.2f, 0.9f);
            const auto first = static_cast<std::uint32_t>(mesh.vertices.size());
            for (int s = 0; s <= steps; ++s) {
                const float t = static_cast<float>(s) / static_cast<float>(steps);
                // Arco: sube y cae hacia la punta.
                const Vec3 spine = top + out * (frond_length * t) + Vec3{0.0f, frond_length * (rise * t - 0.9f * t * t), 0.0f};
                const float width = frond_length * 0.22f * std::sin(kPi * std::min(t * 1.1f + 0.05f, 1.0f));
                const Vec3 side = core::normalize(core::cross(out, Vec3{0.0f, 1.0f, 0.0f}));
                const Vec3 n = core::normalize(Vec3{0.0f, 1.0f, 0.0f} + out * 0.3f);
                const float ao = 0.55f + 0.45f * t;
                const float wind = 0.4f + 0.6f * t;
                for (int e = -1; e <= 1; e += 2) {
                    const Vec3 p = spine + side * (width * static_cast<float>(e)) + Vec3{0.0f, -width * 0.25f, 0.0f};
                    mesh.vertices.push_back({p.x, p.y, p.z, n.x, n.y, n.z, rgba(species.leaf_color, ao), wind,
                                             e < 0 ? 0.0f : 1.0f, t, static_cast<float>(kTreeLayerFrond), 1.0f});
                }
            }
            for (int s = 0; s < steps; ++s) {
                const std::uint32_t p0 = first + static_cast<std::uint32_t>(s) * 2;
                mesh.indices.insert(mesh.indices.end(), {p0, p0 + 2, p0 + 1, p0 + 1, p0 + 2, p0 + 3});
            }
        }
    }

    void build() {
        if (species.kind == TreeKind::Palm) {
            buildPalm();
        } else {
            const float h = species.height;
            const float radius = h * rules.trunk_radius;
            Branch trunk = growBranch(Vec3{}, Vec3{0.0f, 1.0f, 0.0f}, h, radius, 0);
            branches.push_back(trunk);
            // La copa: para las normales esfericas y la oclusion.
            crown_center = Vec3{0.0f, h * (rules.crown_start + (1.0f - rules.crown_start) * 0.5f), 0.0f};
            crown_radius = std::max(h * (1.0f - rules.crown_start) * 0.55f, h * rules.length[0] * 0.8f);
            spawnChildren(trunk, 1);

            // Lados de los tubos por nivel de detalle.
            const std::array<std::array<int, 4>, 3> sides = {{{10, 6, 4, 3}, {6, 4, 3, 3}, {4, 3, 3, 3}}};
            int deepest = 0;
            for (const Branch& b : branches) deepest = std::max(deepest, b.level);
            for (const Branch& b : branches) {
                tube(b, sides[static_cast<std::size_t>(std::clamp(lod, 0, 2))][static_cast<std::size_t>(std::min(b.level, 3))]);
            }
            // Hojas en las ramas mas finas que hay en este nivel de detalle;
            // lejos, menos y mas grandes (cubren lo mismo).
            const float count_scale = lod == 0 ? 1.0f : (lod == 1 ? 0.5f : 0.22f);
            const float size_scale = lod == 0 ? 1.0f : (lod == 1 ? 1.45f : 2.2f);
            for (const Branch& b : branches) {
                if (b.level == deepest && b.level > 0) leavesOn(b, count_scale, size_scale);
            }
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

// -----------------------------------------------------------------------------
// Texturas
// -----------------------------------------------------------------------------

float hashF(int x, int y, std::uint32_t s) {
    std::uint32_t h = static_cast<std::uint32_t>(x) * 0x8da6b343U ^ static_cast<std::uint32_t>(y) * 0xd8163841U ^ s * 0xcb1ab31fU;
    h ^= h >> 16;
    h *= 0x7feb352dU;
    h ^= h >> 15;
    h *= 0x846ca68bU;
    h ^= h >> 16;
    return static_cast<float>(h & 0xFFFFFFU) / 16777215.0f;
}

// Ruido de valor repetible (periodo en celdas).
float valueNoise(float x, float y, int period, std::uint32_t seed) {
    const int x0 = static_cast<int>(std::floor(x));
    const int y0 = static_cast<int>(std::floor(y));
    const float fx = x - static_cast<float>(x0);
    const float fy = y - static_cast<float>(y0);
    const auto w = [period](int v) { return ((v % period) + period) % period; };
    const float a = hashF(w(x0), w(y0), seed);
    const float b = hashF(w(x0 + 1), w(y0), seed);
    const float c = hashF(w(x0), w(y0 + 1), seed);
    const float d = hashF(w(x0 + 1), w(y0 + 1), seed);
    const float u = fx * fx * (3.0f - 2.0f * fx);
    const float v = fy * fy * (3.0f - 2.0f * fy);
    return (a + (b - a) * u) * (1.0f - v) + (c + (d - c) * u) * v;
}

float fbmTile(float u, float v, int cells_x, int cells_y, int octaves, std::uint32_t seed) {
    float sum = 0.0f;
    float amp = 0.5f;
    float total = 0.0f;
    for (int o = 0; o < octaves; ++o) {
        const int px = cells_x << o;
        const int py = cells_y << o;
        // Periodo por eje: se usa el mayor como rejilla y se escala.
        sum += valueNoise(u * static_cast<float>(px), v * static_cast<float>(py), std::max(px, py), seed + static_cast<std::uint32_t>(o) * 31U) * amp;
        total += amp;
        amp *= 0.5f;
    }
    return sum / total;
}

struct Canvas {
    std::uint32_t size;
    std::vector<float> r, g, b, a, h;
    explicit Canvas(std::uint32_t s) : size(s), r(s * s, 0.0f), g(s * s, 0.0f), b(s * s, 0.0f), a(s * s, 0.0f), h(s * s, 0.0f) {}
    std::size_t at(int x, int y) const {
        const int n = static_cast<int>(size);
        return static_cast<std::size_t>(((y % n) + n) % n) * size + static_cast<std::size_t>(((x % n) + n) % n);
    }
};

// Hoja: elipse apuntada, nervio central y laterales, un poco de degradado.
void drawLeaf(Canvas& c, float cx, float cy, float angle, float length, float width, float tone, std::uint32_t seed,
              float hue = 0.0f) {
    const float ca = std::cos(angle);
    const float sa = std::sin(angle);
    const int reach = static_cast<int>(length) + 2;
    for (int y = static_cast<int>(cy) - reach; y <= static_cast<int>(cy) + reach; ++y) {
        for (int x = static_cast<int>(cx) - reach; x <= static_cast<int>(cx) + reach; ++x) {
            if (x < 0 || y < 0 || x >= static_cast<int>(c.size) || y >= static_cast<int>(c.size)) continue;
            const float dx = static_cast<float>(x) - cx;
            const float dy = static_cast<float>(y) - cy;
            const float along = dx * ca + dy * sa;       // 0 en el tallo, length en la punta
            const float across = -dx * sa + dy * ca;
            if (along < 0.0f || along > length) continue;
            const float t = along / length;
            const float half = width * std::pow(std::sin(kPi * std::min(t * 0.95f + 0.02f, 1.0f)), 0.75f) *
                               (1.0f - 0.25f * t);
            const float edge = half - std::abs(across);
            if (edge <= 0.0f) continue;
            const std::size_t i = c.at(x, y);
            const float coverage = std::clamp(edge, 0.0f, 1.0f);
            const float rib = std::exp(-across * across / 1.2f);
            // Nervios laterales apenas marcados y la mitad de la hoja que mira a
            // la luz algo mas clara.
            const float veins = std::pow(std::abs(std::sin((along - std::abs(across) * 0.8f) * (6.0f / std::max(length * 0.1f, 1.0f)))), 16.0f);
            const float noise = (hashF(x, y, seed) - 0.5f) * 0.05f;
            const float lit = across > 0.0f ? 1.04f : 0.94f;
            const float shade = tone * lit * (0.85f + 0.15f * (1.0f - t)) * (1.0f + 0.12f * rib) * (1.0f - veins * 0.06f) *
                                (0.9f + 0.1f * std::clamp(edge / std::max(half, 0.5f) * 2.0f, 0.0f, 1.0f)) + noise;
            c.r[i] = shade * (0.92f + hue * 0.25f);
            c.g[i] = shade;
            c.b[i] = shade * (0.8f - hue * 0.2f);
            c.a[i] = std::max(c.a[i], coverage);
            c.h[i] = 0.6f + 0.4f * (edge / std::max(half, 0.5f)) - rib * 0.2f;
        }
    }
}

void drawLine(Canvas& c, float x0, float y0, float x1, float y1, float thickness, float tone) {
    const float len = std::sqrt((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0));
    const int steps = std::max(1, static_cast<int>(len * 2.0f));
    for (int s = 0; s <= steps; ++s) {
        const float t = static_cast<float>(s) / static_cast<float>(steps);
        const float px = x0 + (x1 - x0) * t;
        const float py = y0 + (y1 - y0) * t;
        const float th = thickness * (1.0f - 0.6f * t);
        const int r = static_cast<int>(std::ceil(th)) + 1;
        for (int y = static_cast<int>(py) - r; y <= static_cast<int>(py) + r; ++y) {
            for (int x = static_cast<int>(px) - r; x <= static_cast<int>(px) + r; ++x) {
                if (x < 0 || y < 0 || x >= static_cast<int>(c.size) || y >= static_cast<int>(c.size)) continue;
                const float d = std::sqrt((static_cast<float>(x) - px) * (static_cast<float>(x) - px) +
                                          (static_cast<float>(y) - py) * (static_cast<float>(y) - py));
                const float cov = std::clamp(th - d + 0.5f, 0.0f, 1.0f);
                if (cov <= 0.0f) continue;
                const std::size_t i = c.at(x, y);
                const float shade = tone * (0.75f + 0.25f * t);
                c.r[i] = shade * 0.9f;
                c.g[i] = shade;
                c.b[i] = shade * 0.78f;
                c.a[i] = std::max(c.a[i], cov);
                c.h[i] = 0.7f;
            }
        }
    }
}

float smoothstepF(float e0, float e1, float x) {
    const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

Canvas barkCanvas(std::uint32_t size, bool birch) {
    Canvas c(size);
    for (std::uint32_t y = 0; y < size; ++y) {
        for (std::uint32_t x = 0; x < size; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(size);
            const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(size);
            const std::size_t i = c.at(static_cast<int>(x), static_cast<int>(y));
            float r;
            float g;
            float b;
            float height;
            if (!birch) {
                // Corteza de placas: celdas de Voronoi estiradas en vertical
                // (placas largas) con los bordes deformados; los bordes son los
                // surcos. Cada placa con su tono, fibras finas y algo de musgo.
                const float warp_u = (fbmTile(u, v, 4, 4, 3, 11U) - 0.5f) * 0.05f;
                const float warp_v = (fbmTile(u, v, 4, 4, 3, 13U) - 0.5f) * 0.12f;
                constexpr int kPlatesX = 9;
                constexpr int kPlatesY = 3;
                const float px = (u + warp_u) * kPlatesX;
                const float py = (v + warp_v) * kPlatesY;
                const int cx = static_cast<int>(std::floor(px));
                const int cy = static_cast<int>(std::floor(py));
                float f1 = 9.0f;
                float f2 = 9.0f;
                int cell = 0;
                for (int oy = -3; oy <= 3; ++oy) {  // la vertical cuenta 1/3: mas filas
                    for (int ox = -2; ox <= 2; ++ox) {
                        const int gx = cx + ox;
                        const int gy = cy + oy;
                        const int wx = ((gx % kPlatesX) + kPlatesX) % kPlatesX;
                        const int wy = ((gy % kPlatesY) + kPlatesY) % kPlatesY;
                        const float qx = static_cast<float>(gx) + 0.1f + 0.8f * hashF(wx, wy, 41U);
                        const float qy = static_cast<float>(gy) + 0.1f + 0.8f * hashF(wx, wy, 42U);
                        // Distancia con la vertical encogida: placas ~3 veces mas altas que anchas.
                        const float dx = px - qx;
                        const float dy = (py - qy) * 0.33f;
                        const float d = std::sqrt(dx * dx + dy * dy);
                        if (d < f1) {
                            f2 = f1;
                            f1 = d;
                            cell = wy * kPlatesX + wx;
                        } else if (d < f2) {
                            f2 = d;
                        }
                    }
                }
                const float gap = f2 - f1;
                const float furrow = 1.0f - smoothstepF(0.02f, 0.22f + 0.1f * hashF(cell, 3, 43U), gap);
                const float breaks = 0.0f;
                const float plate = 0.35f + 0.4f * hashF(cell, 1, 44U) + 0.25f * fbmTile(u, v, 16, 4, 3, 15U);
                const float fine = valueNoise(u * 160.0f, v * 12.0f, 160, 16U);  // fibras verticales
                const float gray = smoothstepF(0.45f, 0.75f, fbmTile(u, v, 4, 3, 3, 17U));
                const float moss = smoothstepF(0.66f, 0.8f, fbmTile(u, v, 3, 2, 4, 18U)) * (1.0f - furrow);
                const float top = 0.62f + 0.3f * plate + (fine - 0.5f) * 0.12f;
                const float depth = 1.0f - furrow * 0.75f - breaks * 0.5f;
                // Color (lo multiplica el de la especie): pardo en los surcos,
                // gris en las placas curtidas.
                r = top * depth * (1.02f - gray * 0.08f);
                g = top * depth * (0.96f + gray * 0.02f);
                b = top * depth * (0.9f + gray * 0.08f);
                r = r * (1.0f - moss * 0.35f);
                g = g * (1.0f - moss * 0.05f);
                b = b * (1.0f - moss * 0.45f);
                height = (1.0f - furrow) * 0.75f + plate * 0.2f + fine * 0.05f - breaks * 0.35f;
            } else {
                // Abedul: blanco con lenticelas horizontales y manchas oscuras.
                const float marks = fbmTile(u, v, 3, 6, 4, 21U);
                const float lenticel = (valueNoise(u * 6.0f, v * 90.0f, 90, 22U) > 0.78f) ? 1.0f : 0.0f;
                const float dark = smoothstepF(0.62f, 0.72f, marks);
                const float tone = 0.92f - 0.75f * dark - 0.45f * lenticel * (1.0f - dark) - fbmTile(u, v, 16, 16, 2, 23U) * 0.08f;
                r = g = b = tone;
                height = 1.0f - dark * 0.6f - lenticel * 0.3f;
            }
            c.r[i] = std::clamp(r, 0.03f, 1.0f);
            c.g[i] = std::clamp(g, 0.03f, 1.0f);
            c.b[i] = std::clamp(b, 0.03f, 1.0f);
            c.a[i] = 1.0f;
            c.h[i] = height;
        }
    }
    return c;
}

// Racimo de hojas: una ramita con ramitas laterales y muchas hojas pequenas
// (unos 6-9 cm en una tarjeta de ~1 m), solapadas y cada una con su tono. Una
// sola hoja grande por tarjeta se ve de juguete.
Canvas leavesCanvas(std::uint32_t size) {
    Canvas c(size);
    std::mt19937 rng(77U);
    std::uniform_real_distribution<float> uni(0.0f, 1.0f);
    const float s = static_cast<float>(size);
    struct Twig {
        float x0, y0, x1, y1;
    };
    std::vector<Twig> twigs;
    // Tallo principal (de abajo hacia arriba) y 7 ramitas.
    const float bx = s * 0.5f;
    twigs.push_back({bx, s * 0.99f, bx + s * 0.03f, s * 0.12f});
    for (int k = 0; k < 7; ++k) {
        const float t = 0.15f + 0.7f * static_cast<float>(k) / 6.0f;
        const float side = (k % 2 == 0) ? -1.0f : 1.0f;
        const float x0 = bx + s * 0.03f * t;
        const float y0 = s * (0.99f - 0.87f * t);
        const float ang = -kPi * 0.5f + side * (0.7f + uni(rng) * 0.35f);
        const float len = s * (0.3f - 0.14f * t) * (0.8f + 0.3f * uni(rng));
        twigs.push_back({x0, y0, x0 + std::cos(ang) * len, y0 + std::sin(ang) * len});
    }
    for (const Twig& tw : twigs) drawLine(c, tw.x0, tw.y0, tw.x1, tw.y1, s * 0.0045f, 0.4f);
    // Hojas alternas a lo largo de cada ramita, apuntando hacia fuera.
    std::uint32_t id = 5U;
    for (const Twig& tw : twigs) {
        const float dx = tw.x1 - tw.x0;
        const float dy = tw.y1 - tw.y0;
        const float len = std::sqrt(dx * dx + dy * dy);
        const float dir = std::atan2(dy, dx);
        const int count = std::max(4, static_cast<int>(len / (s * 0.035f)));
        for (int k = 0; k < count; ++k) {
            const float t = 0.12f + 0.88f * (static_cast<float>(k) + uni(rng) * 0.5f) / static_cast<float>(count);
            const float px = tw.x0 + dx * t;
            const float py = tw.y0 + dy * t;
            const float side = (k % 2 == 0) ? -1.0f : 1.0f;
            const float angle = dir + side * (0.5f + uni(rng) * 0.6f);
            const float leaf = s * (0.065f + 0.035f * uni(rng));
            const float tone = 0.78f + 0.3f * uni(rng);
            const float hue = uni(rng) * 0.3f - 0.08f + (uni(rng) < 0.08f ? 0.35f : 0.0f);  // alguna amarillea
            drawLeaf(c, px, py, angle, leaf, leaf * 0.36f, tone, id++, hue);
        }
        drawLeaf(c, tw.x1, tw.y1, dir, s * 0.07f, s * 0.025f, 0.95f, id++, 0.0f);  // la de la punta
    }
    return c;
}

Canvas needlesCanvas(std::uint32_t size) {
    Canvas c(size);
    std::mt19937 rng(91U);
    std::uniform_real_distribution<float> uni(0.0f, 1.0f);
    const float s = static_cast<float>(size);
    // Ramita a lo largo de u y agujas a los dos lados, hacia la punta.
    drawLine(c, s * 0.02f, s * 0.5f, s * 0.98f, s * 0.5f, s * 0.012f, 0.4f);
    for (int k = 0; k < 150; ++k) {
        const float x = s * (0.04f + 0.9f * uni(rng));
        const float side = uni(rng) < 0.5f ? -1.0f : 1.0f;
        const float angle = side * (0.55f + uni(rng) * 0.45f);
        const float len = s * (0.16f + 0.1f * uni(rng)) * (1.0f - 0.4f * (x / s));
        const float x1 = x + std::cos(angle) * len;
        const float y1 = s * 0.5f + std::sin(angle) * len;
        drawLine(c, x, s * 0.5f, x1, y1, s * 0.0045f, 0.7f + 0.35f * uni(rng));
    }
    return c;
}

Canvas frondCanvas(std::uint32_t size) {
    Canvas c(size);
    std::mt19937 rng(123U);
    std::uniform_real_distribution<float> uni(0.0f, 1.0f);
    const float s = static_cast<float>(size);
    // Nervio a lo largo de v (u = 0.5) y foliolos hacia los lados.
    drawLine(c, s * 0.5f, s * 0.0f, s * 0.5f, s * 1.0f, s * 0.01f, 0.55f);
    for (int k = 0; k < 44; ++k) {
        const float t = static_cast<float>(k) / 43.0f;
        const float y = s * (0.02f + 0.96f * t);
        for (int side = -1; side <= 1; side += 2) {
            const float len = s * 0.48f * std::sin(kPi * std::min(t * 1.05f + 0.03f, 1.0f));
            const float angle = (side < 0 ? kPi : 0.0f) + static_cast<float>(side) * 0.35f;
            drawLeaf(c, s * 0.5f, y, angle, len, s * 0.022f, 0.8f + 0.2f * uni(rng), static_cast<std::uint32_t>(k * 2 + side + 7));
        }
    }
    return c;
}

// Canvas -> niveles RGBA8 (color y normal). Las transparentes conservan su
// cobertura en los mips: el alfa de cada nivel se escala para que la
// fraccion por encima de 0.5 sea la del nivel 0.
void toLevels(const Canvas& c, bool alpha_test, float normal_strength, std::vector<std::vector<std::uint8_t>>& albedo,
              std::vector<std::vector<std::uint8_t>>& normal, std::uint32_t mips) {
    std::uint32_t size = c.size;
    std::vector<float> r = c.r, g = c.g, b = c.b, a = c.a, h = c.h;
    // Color de las zonas transparentes: el de las hojas cercanas (sin bordes oscuros).
    if (alpha_test) {
        for (int pass = 0; pass < 6; ++pass) {
            std::vector<float> r2 = r, g2 = g, b2 = b;
            for (std::uint32_t y = 0; y < size; ++y) {
                for (std::uint32_t x = 0; x < size; ++x) {
                    const std::size_t i = static_cast<std::size_t>(y) * size + x;
                    if (a[i] > 0.01f) continue;
                    float sr = 0.0f, sg = 0.0f, sb = 0.0f, w = 0.0f;
                    for (int oy = -2; oy <= 2; ++oy) {
                        for (int ox = -2; ox <= 2; ++ox) {
                            const int xx = static_cast<int>(x) + ox * (1 << pass);
                            const int yy = static_cast<int>(y) + oy * (1 << pass);
                            if (xx < 0 || yy < 0 || xx >= static_cast<int>(size) || yy >= static_cast<int>(size)) continue;
                            const std::size_t j = static_cast<std::size_t>(yy) * size + static_cast<std::size_t>(xx);
                            if (r[j] + g[j] + b[j] <= 0.0f) continue;
                            sr += r[j];
                            sg += g[j];
                            sb += b[j];
                            w += 1.0f;
                        }
                    }
                    if (w > 0.0f) {
                        r2[i] = sr / w;
                        g2[i] = sg / w;
                        b2[i] = sb / w;
                    }
                }
            }
            r = r2;
            g = g2;
            b = b2;
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
            float hi = 6.0f;
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
        const auto byte = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
        for (std::uint32_t y = 0; y < size; ++y) {
            for (std::uint32_t x = 0; x < size; ++x) {
                const std::size_t i = static_cast<std::size_t>(y) * size + x;
                out[i * 4 + 0] = byte(r[i]);
                out[i * 4 + 1] = byte(g[i]);
                out[i * 4 + 2] = byte(b[i]);
                out[i * 4 + 3] = byte(alpha_test ? a[i] * scale : 1.0f);
                const auto hat = [&](int xx, int yy) {
                    xx = (xx + static_cast<int>(size)) % static_cast<int>(size);
                    yy = (yy + static_cast<int>(size)) % static_cast<int>(size);
                    return h[static_cast<std::size_t>(yy) * size + static_cast<std::size_t>(xx)];
                };
                const float strength = normal_strength * static_cast<float>(size) / 512.0f;
                const float dx = (hat(static_cast<int>(x) + 1, static_cast<int>(y)) - hat(static_cast<int>(x) - 1, static_cast<int>(y))) * strength;
                const float dy = (hat(static_cast<int>(x), static_cast<int>(y) + 1) - hat(static_cast<int>(x), static_cast<int>(y) - 1)) * strength;
                Vec3 n = core::normalize(Vec3{-dx, dy, 1.0f});
                nout[i * 4 + 0] = byte(n.x * 0.5f + 0.5f);
                nout[i * 4 + 1] = byte(n.y * 0.5f + 0.5f);
                nout[i * 4 + 2] = byte(n.z * 0.5f + 0.5f);
                nout[i * 4 + 3] = 255;
            }
        }
        albedo.push_back(std::move(out));
        normal.push_back(std::move(nout));
        if (size == 1) break;
        // Siguiente nivel (media 2x2; el color pesado por el alfa).
        const std::uint32_t next = std::max(size / 2, 1U);
        std::vector<float> r2(next * next), g2(next * next), b2(next * next), a2(next * next), h2(next * next);
        for (std::uint32_t y = 0; y < next; ++y) {
            for (std::uint32_t x = 0; x < next; ++x) {
                float sr = 0.0f, sg = 0.0f, sb = 0.0f, sa = 0.0f, sh = 0.0f;
                for (std::uint32_t oy = 0; oy < 2; ++oy) {
                    for (std::uint32_t ox = 0; ox < 2; ++ox) {
                        const std::size_t j = static_cast<std::size_t>(std::min(y * 2 + oy, size - 1)) * size + std::min(x * 2 + ox, size - 1);
                        sr += r[j];
                        sg += g[j];
                        sb += b[j];
                        sa += a[j];
                        sh += h[j];
                    }
                }
                const std::size_t k = static_cast<std::size_t>(y) * next + x;
                r2[k] = sr * 0.25f;
                g2[k] = sg * 0.25f;
                b2[k] = sb * 0.25f;
                a2[k] = sa * 0.25f;
                h2[k] = sh * 0.25f;
            }
        }
        r = std::move(r2);
        g = std::move(g2);
        b = std::move(b2);
        a = std::move(a2);
        h = std::move(h2);
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
            s.height = 14.0f; s.leaf_color = {0.2f, 0.32f, 0.16f}; s.bark_color = {0.45f, 0.32f, 0.24f};
            break;
        case TreeKind::Fir:
            s.height = 13.0f; s.leaf_color = {0.14f, 0.26f, 0.15f}; s.bark_color = {0.35f, 0.27f, 0.22f};
            break;
        case TreeKind::Oak:
            s.height = 11.0f; s.leaf_color = {0.23f, 0.35f, 0.13f}; s.bark_color = {0.38f, 0.33f, 0.27f};
            break;
        case TreeKind::Birch:
            s.height = 12.0f; s.leaf_color = {0.3f, 0.42f, 0.15f}; s.bark_color = {0.88f, 0.86f, 0.82f};
            break;
        case TreeKind::Palm:
            s.height = 10.0f; s.leaf_color = {0.3f, 0.45f, 0.14f}; s.bark_color = {0.5f, 0.42f, 0.32f};
            break;
        case TreeKind::Willow:
            s.height = 10.0f; s.leaf_color = {0.36f, 0.48f, 0.16f}; s.bark_color = {0.33f, 0.29f, 0.24f};
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
    toLevels(barkCanvas(size, false), false, 3.0f, t.albedo[kTreeLayerBark], t.normal[kTreeLayerBark], mips);
    toLevels(leavesCanvas(size), true, 1.5f, t.albedo[kTreeLayerLeaves], t.normal[kTreeLayerLeaves], mips);
    toLevels(needlesCanvas(size), true, 1.0f, t.albedo[kTreeLayerNeedles], t.normal[kTreeLayerNeedles], mips);
    toLevels(frondCanvas(size), true, 1.0f, t.albedo[kTreeLayerFrond], t.normal[kTreeLayerFrond], mips);
    toLevels(barkCanvas(size, true), false, 2.0f, t.albedo[kTreeLayerBirchBark], t.normal[kTreeLayerBirchBark], mips);
    return t;
}

}  // namespace cramion::asset
