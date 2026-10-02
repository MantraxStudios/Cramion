#include "CramionCore/asset/LightmapUv.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <deque>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <numeric>
#include <unordered_map>
#include <utility>

namespace cramion::lighting {

using core::Vec2;
using core::Vec3;

namespace {

constexpr char kMagic[4] = {'C', 'R', 'U', 'V'};
constexpr std::uint32_t kVersion = 1;

float cross2(const Vec2& a, const Vec2& b) { return a.x * b.y - a.y * b.x; }
Vec2 sub2(const Vec2& a, const Vec2& b) { return Vec2{a.x - b.x, a.y - b.y}; }

std::uint64_t fnv(std::uint64_t hash, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < size; ++i) hash = (hash ^ bytes[i]) * 1099511628211ull;
    return hash;
}

// Id comun para los vertices que estan en la misma posicion (los que separan
// las costuras de normales o de UV de la textura): asi las cartas cruzan esas
// costuras si la superficie sigue.
std::vector<std::uint32_t> weldPositions(const std::vector<asset::SkinnedVertex>& vertices) {
    const std::size_t count = vertices.size();
    std::vector<std::uint32_t> canonical(count);
    if (count == 0) return canonical;
    Vec3 lo = vertices[0].position;
    Vec3 hi = lo;
    for (const asset::SkinnedVertex& v : vertices) {
        lo = Vec3{std::min(lo.x, v.position.x), std::min(lo.y, v.position.y), std::min(lo.z, v.position.z)};
        hi = Vec3{std::max(hi.x, v.position.x), std::max(hi.y, v.position.y), std::max(hi.z, v.position.z)};
    }
    const float diagonal = std::max(core::length(hi - lo), 1e-6f);
    const double cell = static_cast<double>(diagonal) * 1e-6;
    struct Key {
        std::int64_t x, y, z;
        std::uint32_t index;
    };
    std::vector<Key> keys(count);
    for (std::size_t i = 0; i < count; ++i) {
        const Vec3& p = vertices[i].position;
        keys[i] = Key{static_cast<std::int64_t>(std::llround(p.x / cell)), static_cast<std::int64_t>(std::llround(p.y / cell)),
                      static_cast<std::int64_t>(std::llround(p.z / cell)), static_cast<std::uint32_t>(i)};
    }
    std::sort(keys.begin(), keys.end(), [](const Key& a, const Key& b) {
        if (a.x != b.x) return a.x < b.x;
        if (a.y != b.y) return a.y < b.y;
        if (a.z != b.z) return a.z < b.z;
        return a.index < b.index;
    });
    std::uint32_t current = keys[0].index;
    for (std::size_t i = 0; i < count; ++i) {
        if (i > 0 && (keys[i].x != keys[i - 1].x || keys[i].y != keys[i - 1].y || keys[i].z != keys[i - 1].z)) {
            current = keys[i].index;
        }
        canonical[keys[i].index] = current;
    }
    return canonical;
}

// Envolvente convexa (cadena monotona de Andrew), en sentido antihorario.
std::vector<Vec2> convexHull(std::vector<Vec2> points) {
    std::sort(points.begin(), points.end(), [](const Vec2& a, const Vec2& b) {
        return a.x < b.x || (a.x == b.x && a.y < b.y);
    });
    points.erase(std::unique(points.begin(), points.end(),
                             [](const Vec2& a, const Vec2& b) { return a.x == b.x && a.y == b.y; }),
                 points.end());
    if (points.size() < 3) return points;
    std::vector<Vec2> hull(points.size() * 2);
    std::size_t k = 0;
    for (const Vec2& p : points) {
        while (k >= 2 && cross2(sub2(hull[k - 1], hull[k - 2]), sub2(p, hull[k - 2])) <= 0.0f) --k;
        hull[k++] = p;
    }
    for (std::size_t i = points.size() - 1, lower = k + 1; i-- > 0;) {
        const Vec2& p = points[i];
        while (k >= lower && cross2(sub2(hull[k - 1], hull[k - 2]), sub2(p, hull[k - 2])) <= 0.0f) --k;
        hull[k++] = p;
    }
    hull.resize(k > 0 ? k - 1 : 0);
    return hull;
}

struct ChartBuild {
    std::vector<std::uint32_t> triangles;
};

// Cartas: crecimiento de regiones desde los triangulos mas grandes. Un vecino
// entra si su cara no se dobla mas de `cos_hard` respecto a la que lo toca y
// no se aparta mas de `cos_max` de la normal media de la carta (asi la
// proyeccion en su plano nunca da la vuelta a un triangulo).
// Solo crece sobre los triangulos con label == -1; los etiqueta con el
// indice de su carta en `charts`.
void growCharts(const std::vector<std::uint32_t>& candidates, const std::vector<std::array<std::int32_t, 3>>& adjacency,
                const std::vector<Vec3>& unit_normals, const std::vector<Vec3>& area_normals,
                const std::vector<bool>& degenerate, float cos_hard, float cos_max, std::vector<std::int32_t>& label,
                std::vector<ChartBuild>& charts) {
    std::vector<std::uint32_t> order;
    order.reserve(candidates.size());
    for (std::uint32_t t : candidates) {
        if (!degenerate[t]) order.push_back(t);
    }
    std::sort(order.begin(), order.end(), [&](std::uint32_t a, std::uint32_t b) {
        const float aa = core::dot(area_normals[a], area_normals[a]);
        const float bb = core::dot(area_normals[b], area_normals[b]);
        return aa != bb ? aa > bb : a < b;
    });
    std::deque<std::uint32_t> queue;
    for (std::uint32_t seed : order) {
        if (label[seed] != -1) continue;
        const auto id = static_cast<std::int32_t>(charts.size());
        charts.emplace_back();
        ChartBuild& chart = charts.back();
        Vec3 sum = area_normals[seed];
        label[seed] = id;
        chart.triangles.push_back(seed);
        queue.clear();
        queue.push_back(seed);
        while (!queue.empty()) {
            const std::uint32_t t = queue.front();
            queue.pop_front();
            const Vec3 average = core::normalize(sum);
            for (int e = 0; e < 3; ++e) {
                const std::int32_t nb = adjacency[t][static_cast<std::size_t>(e)];
                if (nb < 0 || label[static_cast<std::size_t>(nb)] != -1) continue;
                const auto n = static_cast<std::uint32_t>(nb);
                if (degenerate[n]) continue;  // se reparten al final
                if (core::dot(unit_normals[n], unit_normals[t]) < cos_hard) continue;
                if (core::dot(unit_normals[n], average) < cos_max) continue;
                label[n] = id;
                sum += area_normals[n];
                chart.triangles.push_back(n);
                queue.push_back(n);
            }
        }
    }
}

// Coordenadas 2D de cada esquina de la carta proyectada en su plano.
struct ChartPlane {
    Vec3 axis_u{};
    Vec3 axis_v{};
};

ChartPlane chartPlane(const Vec3& normal) {
    const Vec3 n = core::length(normal) > 1e-8f ? core::normalize(normal) : Vec3{0.0f, 1.0f, 0.0f};
    const Vec3 helper = std::abs(n.y) < 0.99f ? Vec3{0.0f, 1.0f, 0.0f} : Vec3{1.0f, 0.0f, 0.0f};
    ChartPlane plane;
    plane.axis_u = core::normalize(core::cross(helper, n));
    plane.axis_v = core::cross(n, plane.axis_u);
    return plane;
}

// true si dos triangulos distintos de la carta (ya en 2D) se pisan: se
// rasteriza en una rejilla y se busca una celda dentro de dos a la vez.
bool chartOverlaps(const std::vector<std::array<Vec2, 3>>& triangles) {
    if (triangles.size() < 3) return false;
    Vec2 lo = triangles[0][0];
    Vec2 hi = lo;
    for (const auto& tri : triangles) {
        for (const Vec2& p : tri) {
            lo = Vec2{std::min(lo.x, p.x), std::min(lo.y, p.y)};
            hi = Vec2{std::max(hi.x, p.x), std::max(hi.y, p.y)};
        }
    }
    const float width = hi.x - lo.x;
    const float height = hi.y - lo.y;
    if (width <= 0.0f || height <= 0.0f) return false;
    const int grid = std::clamp(static_cast<int>(std::sqrt(static_cast<float>(triangles.size())) * 3.0f), 8, 256);
    const float cell = std::max(width, height) / static_cast<float>(grid);
    const int nx = std::max(1, static_cast<int>(std::ceil(width / cell)));
    const int ny = std::max(1, static_cast<int>(std::ceil(height / cell)));
    std::vector<std::int32_t> owner(static_cast<std::size_t>(nx) * static_cast<std::size_t>(ny), -1);
    std::size_t covered = 0;
    std::size_t overlapped = 0;
    for (std::size_t t = 0; t < triangles.size(); ++t) {
        const Vec2& a = triangles[t][0];
        const Vec2& b = triangles[t][1];
        const Vec2& c = triangles[t][2];
        const float area2 = cross2(sub2(b, a), sub2(c, a));
        if (std::abs(area2) < 1e-12f) continue;
        const float sign = area2 > 0.0f ? 1.0f : -1.0f;
        const float epsilon = 1e-5f * std::abs(area2);
        const int x0 = std::clamp(static_cast<int>(std::floor((std::min({a.x, b.x, c.x}) - lo.x) / cell)), 0, nx - 1);
        const int x1 = std::clamp(static_cast<int>(std::floor((std::max({a.x, b.x, c.x}) - lo.x) / cell)), 0, nx - 1);
        const int y0 = std::clamp(static_cast<int>(std::floor((std::min({a.y, b.y, c.y}) - lo.y) / cell)), 0, ny - 1);
        const int y1 = std::clamp(static_cast<int>(std::floor((std::max({a.y, b.y, c.y}) - lo.y) / cell)), 0, ny - 1);
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                const Vec2 q{lo.x + (static_cast<float>(x) + 0.5f) * cell, lo.y + (static_cast<float>(y) + 0.5f) * cell};
                const float w0 = cross2(sub2(b, a), sub2(q, a)) * sign;
                const float w1 = cross2(sub2(c, b), sub2(q, b)) * sign;
                const float w2 = cross2(sub2(a, c), sub2(q, c)) * sign;
                if (w0 <= epsilon || w1 <= epsilon || w2 <= epsilon) continue;
                std::int32_t& slot = owner[static_cast<std::size_t>(y) * static_cast<std::size_t>(nx) + static_cast<std::size_t>(x)];
                if (slot < 0) {
                    slot = static_cast<std::int32_t>(t);
                    ++covered;
                } else if (slot != static_cast<std::int32_t>(t)) {
                    ++overlapped;
                }
            }
        }
    }
    return overlapped > std::max<std::size_t>(1, covered / 200);
}

}  // namespace

std::uint64_t lightmapMeshHash(const asset::ModelData& model) {
    std::uint64_t hash = 1469598103934665603ull;
    hash = fnv(hash, model.indices.data(), model.indices.size() * sizeof(std::uint32_t));
    for (const asset::SkinnedVertex& v : model.vertices) hash = fnv(hash, &v.position, sizeof(Vec3));
    return hash;
}

float meshSurfaceArea(const asset::ModelData& model) {
    double area = 0.0;
    for (std::size_t i = 0; i + 2 < model.indices.size(); i += 3) {
        const std::uint32_t a = model.indices[i];
        const std::uint32_t b = model.indices[i + 1];
        const std::uint32_t c = model.indices[i + 2];
        if (a >= model.vertices.size() || b >= model.vertices.size() || c >= model.vertices.size()) continue;
        const Vec3& pa = model.vertices[a].position;
        area += 0.5 * static_cast<double>(core::length(
                          core::cross(model.vertices[b].position - pa, model.vertices[c].position - pa)));
    }
    return static_cast<float>(area);
}

bool generateLightmapUvs(const asset::ModelData& model, LightmapUvLayout& out, const LightmapUvSettings& settings) {
    out = LightmapUvLayout{};
    const std::size_t vertex_count = model.vertices.size();
    const std::size_t triangle_count = model.indices.size() / 3;
    if (vertex_count == 0 || triangle_count == 0) return false;
    for (std::uint32_t index : model.indices) {
        if (index >= vertex_count) return false;  // malla rota
    }

    out.source_vertices = static_cast<std::uint32_t>(vertex_count);
    out.source_indices = static_cast<std::uint32_t>(model.indices.size());
    out.source_hash = lightmapMeshHash(model);
    out.surface_area = meshSurfaceArea(model);

    // --- Caras: normal, area y vecinas (por aristas con la posicion soldada) ---
    const std::vector<std::uint32_t> canonical = weldPositions(model.vertices);
    std::vector<Vec3> area_normals(triangle_count);
    std::vector<Vec3> unit_normals(triangle_count);
    std::vector<bool> degenerate(triangle_count, false);
    float max_area = 0.0f;
    for (std::size_t t = 0; t < triangle_count; ++t) {
        const Vec3& a = model.vertices[model.indices[t * 3]].position;
        const Vec3& b = model.vertices[model.indices[t * 3 + 1]].position;
        const Vec3& c = model.vertices[model.indices[t * 3 + 2]].position;
        area_normals[t] = core::cross(b - a, c - a) * 0.5f;
        max_area = std::max(max_area, core::length(area_normals[t]));
    }
    for (std::size_t t = 0; t < triangle_count; ++t) {
        const float area = core::length(area_normals[t]);
        degenerate[t] = area <= max_area * 1e-9f || area <= 1e-14f;
        unit_normals[t] = degenerate[t] ? Vec3{} : area_normals[t] * (1.0f / area);
    }

    std::vector<std::array<std::int32_t, 3>> adjacency(triangle_count, {-1, -1, -1});
    {
        std::unordered_map<std::uint64_t, std::uint64_t> edges;  // arista -> triangulo * 4 + lado (+1)
        edges.reserve(triangle_count * 2);
        for (std::size_t t = 0; t < triangle_count; ++t) {
            for (int e = 0; e < 3; ++e) {
                const std::uint32_t a = canonical[model.indices[t * 3 + static_cast<std::size_t>(e)]];
                const std::uint32_t b = canonical[model.indices[t * 3 + static_cast<std::size_t>((e + 1) % 3)]];
                if (a == b) continue;
                const std::uint64_t key = (static_cast<std::uint64_t>(std::min(a, b)) << 32) | std::max(a, b);
                const std::uint64_t mine = static_cast<std::uint64_t>(t) * 4 + static_cast<std::uint64_t>(e) + 1;
                auto [it, inserted] = edges.try_emplace(key, mine);
                if (inserted) continue;
                if (it->second == 0) continue;  // ya tenia dos: no variedad, no se une
                const std::uint64_t other = it->second - 1;
                const std::size_t other_t = static_cast<std::size_t>(other / 4);
                const std::size_t other_e = static_cast<std::size_t>(other % 4);
                if (other_t != t) {
                    adjacency[t][static_cast<std::size_t>(e)] = static_cast<std::int32_t>(other_t);
                    adjacency[other_t][other_e] = static_cast<std::int32_t>(t);
                }
                it->second = 0;
            }
        }
    }

    // --- Cartas ---
    const float cos_hard = std::cos(core::radians(std::clamp(settings.hard_angle, 1.0f, 89.0f)));
    const float cos_max = std::cos(core::radians(75.0f));
    std::vector<std::int32_t> label(triangle_count, -1);
    std::vector<ChartBuild> charts;
    {
        std::vector<std::uint32_t> all(triangle_count);
        std::iota(all.begin(), all.end(), 0u);
        growCharts(all, adjacency, unit_normals, area_normals, degenerate, cos_hard, cos_max, label, charts);
    }
    // Triangulos degenerados: a la carta de un vecino (no ocupan nada).
    for (bool changed = true; changed;) {
        changed = false;
        for (std::size_t t = 0; t < triangle_count; ++t) {
            if (label[t] != -1) continue;
            for (int e = 0; e < 3; ++e) {
                const std::int32_t nb = adjacency[t][static_cast<std::size_t>(e)];
                if (nb >= 0 && label[static_cast<std::size_t>(nb)] >= 0) {
                    label[t] = label[static_cast<std::size_t>(nb)];
                    charts[static_cast<std::size_t>(label[t])].triangles.push_back(static_cast<std::uint32_t>(t));
                    changed = true;
                    break;
                }
            }
        }
    }
    for (std::size_t t = 0; t < triangle_count; ++t) {
        if (label[t] != -1) continue;
        label[t] = static_cast<std::int32_t>(charts.size());
        charts.push_back(ChartBuild{{static_cast<std::uint32_t>(t)}});
    }

    // --- Proyeccion de cada carta; si se pisa a si misma, se parte ---
    struct FinalChart {
        std::vector<std::uint32_t> triangles;
        ChartPlane plane;
    };
    std::vector<FinalChart> finals;
    finals.reserve(charts.size());
    std::vector<std::pair<ChartBuild, int>> work;
    work.reserve(charts.size());
    for (ChartBuild& chart : charts) work.emplace_back(std::move(chart), 0);
    charts.clear();
    while (!work.empty()) {
        auto [chart, depth] = std::move(work.back());
        work.pop_back();
        Vec3 sum{};
        for (std::uint32_t t : chart.triangles) sum += area_normals[t];
        const ChartPlane plane = chartPlane(sum);
        bool split = false;
        if (chart.triangles.size() > 2 && depth < 3) {
            std::vector<std::array<Vec2, 3>> flat;
            flat.reserve(chart.triangles.size());
            for (std::uint32_t t : chart.triangles) {
                std::array<Vec2, 3> tri{};
                for (int c = 0; c < 3; ++c) {
                    const Vec3& p = model.vertices[model.indices[t * 3 + static_cast<std::size_t>(c)]].position;
                    tri[static_cast<std::size_t>(c)] = Vec2{core::dot(p, plane.axis_u), core::dot(p, plane.axis_v)};
                }
                flat.push_back(tri);
            }
            split = chartOverlaps(flat);
        } else if (chart.triangles.size() > 2) {
            split = true;  // ultimo recurso: cada triangulo por su lado
        }
        if (!split) {
            finals.push_back(FinalChart{std::move(chart.triangles), plane});
            continue;
        }
        if (depth >= 3) {
            for (std::uint32_t t : chart.triangles) {
                finals.push_back(FinalChart{{t}, chartPlane(degenerate[t] ? sum : area_normals[t])});
            }
            continue;
        }
        // Otra vez, mas estricto, solo con estos triangulos.
        for (std::uint32_t t : chart.triangles) label[t] = -1;
        std::vector<ChartBuild> parts;
        const float stricter_hard = std::cos(core::radians(std::clamp(settings.hard_angle, 1.0f, 89.0f) /
                                                           static_cast<float>(2 << depth)));
        const float stricter_max = std::cos(core::radians(45.0f / static_cast<float>(depth + 1)));
        growCharts(chart.triangles, adjacency, unit_normals, area_normals, degenerate, stricter_hard, stricter_max,
                   label, parts);
        std::vector<std::uint32_t> left;
        for (std::uint32_t t : chart.triangles) {
            if (label[t] == -1) left.push_back(t);
            label[t] = -2;  // ya no se toca desde fuera
        }
        for (std::uint32_t t : left) parts.push_back(ChartBuild{{t}});
        if (parts.size() <= 1) {
            // No se pudo partir: triangulo a triangulo.
            for (std::uint32_t t : chart.triangles) {
                finals.push_back(FinalChart{{t}, chartPlane(degenerate[t] ? sum : area_normals[t])});
            }
            continue;
        }
        for (ChartBuild& part : parts) work.emplace_back(std::move(part), depth + 1);
    }

    // --- Vertices nuevos (uno por vertice original y carta) en 2D ---
    struct Placed {
        std::uint32_t first_vertex = 0;
        std::uint32_t vertex_count = 0;
        Vec2 size{};
    };
    std::vector<Placed> placed(finals.size());
    std::vector<Vec2> coords;  // 2D de cada vertice nuevo (unidades del modelo)
    out.remap.reserve(vertex_count + vertex_count / 4);
    coords.reserve(vertex_count + vertex_count / 4);
    out.indices.assign(model.indices.size(), 0);
    std::vector<std::uint32_t> stamp(vertex_count, UINT32_MAX);
    std::vector<std::uint32_t> local(vertex_count, 0);
    for (std::size_t c = 0; c < finals.size(); ++c) {
        const FinalChart& chart = finals[c];
        Placed& p = placed[c];
        p.first_vertex = static_cast<std::uint32_t>(out.remap.size());
        for (std::uint32_t t : chart.triangles) {
            for (int k = 0; k < 3; ++k) {
                const std::size_t slot = static_cast<std::size_t>(t) * 3 + static_cast<std::size_t>(k);
                const std::uint32_t v = model.indices[slot];
                if (stamp[v] != static_cast<std::uint32_t>(c)) {
                    stamp[v] = static_cast<std::uint32_t>(c);
                    local[v] = static_cast<std::uint32_t>(out.remap.size());
                    out.remap.push_back(v);
                    const Vec3& pos = model.vertices[v].position;
                    coords.push_back(Vec2{core::dot(pos, chart.plane.axis_u), core::dot(pos, chart.plane.axis_v)});
                }
                out.indices[slot] = local[v];
            }
        }
        p.vertex_count = static_cast<std::uint32_t>(out.remap.size()) - p.first_vertex;

        // Giro de menor caja (un lado de la envolvente convexa va en un eje) y
        // la carta tumbada (mas ancha que alta) para el empaquetado en estantes.
        std::vector<Vec2> points(coords.begin() + p.first_vertex, coords.begin() + p.first_vertex + p.vertex_count);
        const std::vector<Vec2> hull = convexHull(points);
        float best_area = std::numeric_limits<float>::max();
        float best_cos = 1.0f;
        float best_sin = 0.0f;
        for (std::size_t i = 0; i < hull.size(); ++i) {
            const Vec2 edge = sub2(hull[(i + 1) % hull.size()], hull[i]);
            const float length = std::sqrt(edge.x * edge.x + edge.y * edge.y);
            if (length <= 1e-12f) continue;
            const float cs = edge.x / length;
            const float sn = edge.y / length;
            float min_x = std::numeric_limits<float>::max(), max_x = -min_x, min_y = min_x, max_y = -min_x;
            for (const Vec2& h : hull) {
                const float x = h.x * cs + h.y * sn;
                const float y = -h.x * sn + h.y * cs;
                min_x = std::min(min_x, x);
                max_x = std::max(max_x, x);
                min_y = std::min(min_y, y);
                max_y = std::max(max_y, y);
            }
            const float area = (max_x - min_x) * (max_y - min_y);
            if (area < best_area) {
                best_area = area;
                best_cos = cs;
                best_sin = sn;
            }
        }
        Vec2 lo{std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
        Vec2 hi{-lo.x, -lo.y};
        for (std::uint32_t i = p.first_vertex; i < p.first_vertex + p.vertex_count; ++i) {
            const Vec2 q = coords[i];
            coords[i] = Vec2{q.x * best_cos + q.y * best_sin, -q.x * best_sin + q.y * best_cos};
            lo = Vec2{std::min(lo.x, coords[i].x), std::min(lo.y, coords[i].y)};
            hi = Vec2{std::max(hi.x, coords[i].x), std::max(hi.y, coords[i].y)};
        }
        const bool rotate = (hi.y - lo.y) > (hi.x - lo.x);
        for (std::uint32_t i = p.first_vertex; i < p.first_vertex + p.vertex_count; ++i) {
            const Vec2 q{coords[i].x - lo.x, coords[i].y - lo.y};
            coords[i] = rotate ? Vec2{q.y, (hi.x - lo.x) - q.x} : q;
        }
        p.size = rotate ? Vec2{hi.y - lo.y, hi.x - lo.x} : Vec2{hi.x - lo.x, hi.y - lo.y};
    }

    // --- Empaquetado en estantes dentro de [0, 1]^2 ---
    // Las cartas, de mas alta a mas baja; se busca la escala mayor que cabe.
    std::vector<std::uint32_t> order(placed.size());
    std::iota(order.begin(), order.end(), 0u);
    std::sort(order.begin(), order.end(), [&](std::uint32_t a, std::uint32_t b) {
        return placed[a].size.y != placed[b].size.y ? placed[a].size.y > placed[b].size.y : a < b;
    });
    double total_area = 0.0;
    for (const Placed& p : placed) total_area += static_cast<double>(p.size.x) * static_cast<double>(p.size.y);
    std::vector<Vec2> position(placed.size());
    float padding = std::clamp(settings.padding, 0.0f, 0.25f);
    const auto pack = [&](float scale, bool write) {
        float cursor_x = 0.0f;
        float shelf_y = 0.0f;
        float shelf_h = 0.0f;
        for (std::uint32_t c : order) {
            const float w = placed[c].size.x * scale + padding;
            const float h = placed[c].size.y * scale + padding;
            if (w > 1.0f) return false;
            if (cursor_x + w > 1.0f) {
                shelf_y += shelf_h;
                cursor_x = 0.0f;
                shelf_h = 0.0f;
            }
            if (shelf_y + h > 1.0f) return false;
            if (write) position[c] = Vec2{cursor_x + padding * 0.5f, shelf_y + padding * 0.5f};
            cursor_x += w;
            shelf_h = std::max(shelf_h, h);
        }
        return true;
    };
    // Con muchas cartas el hueco solo ya no cabe: se reduce.
    while (padding > 1e-5f && !pack(1e-9f, false)) padding *= 0.7f;
    if (!pack(1e-9f, false)) padding = 0.0f;
    float low = 0.0f;
    float high = total_area > 0.0 ? static_cast<float>(1.0 / std::sqrt(total_area)) : 1.0f;
    // high puede caber (cartas con 0 de area): se sube hasta que no quepa.
    for (int i = 0; i < 40 && pack(high, false); ++i) {
        low = high;
        high *= 2.0f;
    }
    for (int i = 0; i < 24; ++i) {
        const float mid = 0.5f * (low + high);
        if (pack(mid, false)) {
            low = mid;
        } else {
            high = mid;
        }
    }
    const float scale = low;
    pack(scale, true);

    out.uvs.resize(out.remap.size());
    double uv_area = 0.0;
    for (std::size_t c = 0; c < placed.size(); ++c) {
        const Placed& p = placed[c];
        for (std::uint32_t i = p.first_vertex; i < p.first_vertex + p.vertex_count; ++i) {
            out.uvs[i] = Vec2{std::clamp(position[c].x + coords[i].x * scale, 0.0f, 1.0f),
                              std::clamp(position[c].y + coords[i].y * scale, 0.0f, 1.0f)};
        }
        uv_area += static_cast<double>(p.size.x * scale) * static_cast<double>(p.size.y * scale);
    }
    out.charts = static_cast<std::uint32_t>(placed.size());
    out.uv_area = static_cast<float>(std::clamp(uv_area, 0.0, 1.0));
    return true;
}

bool applyLightmapUvs(asset::ModelData& model, const LightmapUvLayout& layout) {
    if (layout.source_vertices != model.vertices.size() || layout.source_indices != model.indices.size() ||
        layout.uvs.size() != layout.remap.size() || layout.indices.size() != model.indices.size() ||
        layout.source_hash != lightmapMeshHash(model)) {
        return false;
    }
    for (std::uint32_t v : layout.remap) {
        if (v >= model.vertices.size()) return false;
    }
    for (std::uint32_t i : layout.indices) {
        if (i >= layout.remap.size()) return false;
    }
    std::vector<asset::SkinnedVertex> vertices(layout.remap.size());
    for (std::size_t i = 0; i < layout.remap.size(); ++i) vertices[i] = model.vertices[layout.remap[i]];
    model.vertices = std::move(vertices);
    model.indices = layout.indices;
    model.lightmap_uvs = layout.uvs;
    // Los LODs apuntaban a los vertices viejos: se rehacen sobre los nuevos
    // (cada esquina con la UV de su carta).
    if (!model.lods.empty() || !model.lod_indices.empty()) asset::generateLods(model);
    return true;
}

bool saveLightmapUvLayout(const std::filesystem::path& file, const LightmapUvLayout& layout) {
    std::error_code error;
    std::filesystem::create_directories(file.parent_path(), error);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    const auto put = [&](const void* data, std::size_t size) { out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size)); };
    put(kMagic, sizeof(kMagic));
    put(&kVersion, sizeof(kVersion));
    put(&layout.source_vertices, sizeof(layout.source_vertices));
    put(&layout.source_indices, sizeof(layout.source_indices));
    put(&layout.source_hash, sizeof(layout.source_hash));
    put(&layout.charts, sizeof(layout.charts));
    put(&layout.uv_area, sizeof(layout.uv_area));
    put(&layout.surface_area, sizeof(layout.surface_area));
    const auto vertices = static_cast<std::uint32_t>(layout.remap.size());
    const auto indices = static_cast<std::uint32_t>(layout.indices.size());
    put(&vertices, sizeof(vertices));
    put(&indices, sizeof(indices));
    put(layout.remap.data(), layout.remap.size() * sizeof(std::uint32_t));
    put(layout.indices.data(), layout.indices.size() * sizeof(std::uint32_t));
    put(layout.uvs.data(), layout.uvs.size() * sizeof(Vec2));
    return static_cast<bool>(out);
}

bool loadLightmapUvLayout(const std::filesystem::path& file, LightmapUvLayout& out) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    const auto get = [&](void* data, std::size_t size) {
        in.read(static_cast<char*>(data), static_cast<std::streamsize>(size));
        return static_cast<bool>(in);
    };
    char magic[4] = {};
    std::uint32_t version = 0;
    if (!get(magic, sizeof(magic)) || std::memcmp(magic, kMagic, sizeof(kMagic)) != 0) return false;
    if (!get(&version, sizeof(version)) || version != kVersion) return false;
    LightmapUvLayout layout;
    std::uint32_t vertices = 0;
    std::uint32_t indices = 0;
    if (!get(&layout.source_vertices, sizeof(layout.source_vertices)) ||
        !get(&layout.source_indices, sizeof(layout.source_indices)) ||
        !get(&layout.source_hash, sizeof(layout.source_hash)) || !get(&layout.charts, sizeof(layout.charts)) ||
        !get(&layout.uv_area, sizeof(layout.uv_area)) || !get(&layout.surface_area, sizeof(layout.surface_area)) ||
        !get(&vertices, sizeof(vertices)) || !get(&indices, sizeof(indices))) {
        return false;
    }
    // Limites de cordura (un archivo danado no reserva gigas).
    if (vertices > (1u << 28) || indices > (1u << 30)) return false;
    layout.remap.resize(vertices);
    layout.indices.resize(indices);
    layout.uvs.resize(vertices);
    if (!get(layout.remap.data(), layout.remap.size() * sizeof(std::uint32_t)) ||
        !get(layout.indices.data(), layout.indices.size() * sizeof(std::uint32_t)) ||
        !get(layout.uvs.data(), layout.uvs.size() * sizeof(Vec2))) {
        return false;
    }
    out = std::move(layout);
    return true;
}

// --- Registro -------------------------------------------------------------------

namespace {

std::mutex& registryMutex() {
    static std::mutex mutex;
    return mutex;
}

std::map<std::pair<Uuid, int>, std::shared_ptr<const LightmapUvLayout>>& registry() {
    static std::map<std::pair<Uuid, int>, std::shared_ptr<const LightmapUvLayout>> map;
    return map;
}

}  // namespace

void registerLightmapUvs(const Uuid& model, int part, std::shared_ptr<const LightmapUvLayout> layout) {
    std::lock_guard lock(registryMutex());
    if (layout) {
        registry()[{model, part}] = std::move(layout);
    } else {
        registry().erase({model, part});
    }
}

void clearLightmapUvRegistry() {
    std::lock_guard lock(registryMutex());
    registry().clear();
}

std::shared_ptr<const LightmapUvLayout> registeredLightmapUvs(const Uuid& model, int part) {
    std::lock_guard lock(registryMutex());
    const auto it = registry().find({model, part});
    return it != registry().end() ? it->second : nullptr;
}

bool applyRegisteredLightmapUvs(const Uuid& model, int part, asset::ModelData& data) {
    if (!data.lightmap_uvs.empty()) return true;  // ya las tiene
    const std::shared_ptr<const LightmapUvLayout> layout = registeredLightmapUvs(model, part);
    if (!layout) return false;
    if (!applyLightmapUvs(data, *layout)) {
        std::cerr << "[Lightmap] Las UV de lightmap de " << data.name
                  << " son de otra version de la malla: vuelve a hornear la iluminacion\n";
        return false;
    }
    return true;
}

}  // namespace cramion::lighting
