// Fractura de Voronoi: cada celda (los puntos mas cerca de su semilla que de
// las demas) es la malla recortada por los planos bisectores con las otras
// semillas. Cada recorte cierra el hueco con una tapa (las caras de dentro).
// Ver Fracture.h.

#include "CramionCore/physics/Fracture.h"

#include "CramionCore/ecs/RuntimeMesh.h"

#include <CramionFX/asset/Model.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <map>
#include <random>
#include <sstream>
#include <unordered_map>

namespace cramion::physics {

using core::Vec2;
using core::Vec3;
using json = nlohmann::json;

namespace {

// --- Malla de trabajo -------------------------------------------------------

struct Vert {
    Vec3 p{};
    Vec3 n{0.0f, 1.0f, 0.0f};
    Vec2 uv{};
};

struct Tri {
    std::uint32_t a = 0, b = 0, c = 0;
    bool interior = false;
};

struct Work {
    std::vector<Vert> verts;
    std::vector<Tri> tris;
};

Vec3 lerp3(const Vec3& a, const Vec3& b, float t) { return a + (b - a) * t; }
Vec2 lerp2(const Vec2& a, const Vec2& b, float t) { return Vec2{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t}; }

Vec3 safeNormalize(const Vec3& v, const Vec3& fallback = Vec3{0.0f, 1.0f, 0.0f}) {
    const float len = core::length(v);
    return len > 1e-12f ? v * (1.0f / len) : fallback;
}

void bounds(const Work& w, Vec3& lo, Vec3& hi) {
    lo = Vec3{1e30f, 1e30f, 1e30f};
    hi = Vec3{-1e30f, -1e30f, -1e30f};
    for (const Tri& t : w.tris) {
        for (const std::uint32_t i : {t.a, t.b, t.c}) {
            const Vec3& p = w.verts[i].p;
            lo = Vec3{std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
            hi = Vec3{std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
        }
    }
}

// Volumen (con signo; positivo si las caras miran afuera) y centroide.
float volumeOf(const Work& w, Vec3& centroid) {
    double volume = 0.0;
    double cx = 0.0, cy = 0.0, cz = 0.0;
    for (const Tri& t : w.tris) {
        const Vec3& a = w.verts[t.a].p;
        const Vec3& b = w.verts[t.b].p;
        const Vec3& c = w.verts[t.c].p;
        const double v = static_cast<double>(core::dot(a, core::cross(b, c))) / 6.0;
        volume += v;
        cx += v * (a.x + b.x + c.x) / 4.0;
        cy += v * (a.y + b.y + c.y) / 4.0;
        cz += v * (a.z + b.z + c.z) / 4.0;
    }
    if (std::abs(volume) > 1e-12) {
        centroid = Vec3{static_cast<float>(cx / volume), static_cast<float>(cy / volume), static_cast<float>(cz / volume)};
    } else {
        // Plana o abierta: la media de los vertices.
        Vec3 sum{};
        std::size_t count = 0;
        for (const Tri& t : w.tris) {
            for (const std::uint32_t i : {t.a, t.b, t.c}) {
                sum += w.verts[i].p;
                ++count;
            }
        }
        centroid = count > 0 ? sum * (1.0f / static_cast<float>(count)) : Vec3{};
    }
    return static_cast<float>(volume);
}

// Punto dentro de la malla cerrada: rayo y paridad de cortes.
bool inside(const Work& w, const Vec3& p) {
    const Vec3 dir = safeNormalize(Vec3{1.0f, 0.00131f, 0.00071f});
    int hits = 0;
    for (const Tri& t : w.tris) {
        const Vec3& a = w.verts[t.a].p;
        const Vec3 e1 = w.verts[t.b].p - a;
        const Vec3 e2 = w.verts[t.c].p - a;
        const Vec3 h = core::cross(dir, e2);
        const float det = core::dot(e1, h);
        if (std::abs(det) < 1e-12f) continue;
        const float inv = 1.0f / det;
        const Vec3 s = p - a;
        const float u = core::dot(s, h) * inv;
        if (u < 0.0f || u > 1.0f) continue;
        const Vec3 q = core::cross(s, e1);
        const float v = core::dot(dir, q) * inv;
        if (v < 0.0f || u + v > 1.0f) continue;
        if (core::dot(e2, q) * inv > 1e-7f) ++hits;
    }
    return (hits & 1) != 0;
}

// Union de puntos casi iguales (las tapas se encadenan por posicion: los
// vertices de las costuras de UV estan repetidos).
class PointWelder {
public:
    explicit PointWelder(float tolerance) : tol_(std::max(tolerance, 1e-7f)) {}
    int add(const Vec3& p) {
        const std::array<long long, 3> c = cell(p);
        for (long long dx = -1; dx <= 1; ++dx) {
            for (long long dy = -1; dy <= 1; ++dy) {
                for (long long dz = -1; dz <= 1; ++dz) {
                    const auto it = cells_.find(key({c[0] + dx, c[1] + dy, c[2] + dz}));
                    if (it == cells_.end()) continue;
                    for (const int id : it->second) {
                        if (core::length(points_[static_cast<std::size_t>(id)] - p) <= tol_) return id;
                    }
                }
            }
        }
        const int id = static_cast<int>(points_.size());
        points_.push_back(p);
        cells_[key(c)].push_back(id);
        return id;
    }
    const Vec3& point(int id) const { return points_[static_cast<std::size_t>(id)]; }

private:
    std::array<long long, 3> cell(const Vec3& p) const {
        return {static_cast<long long>(std::floor(p.x / tol_)), static_cast<long long>(std::floor(p.y / tol_)),
                static_cast<long long>(std::floor(p.z / tol_))};
    }
    static std::uint64_t key(const std::array<long long, 3>& c) {
        const auto h = [](long long v) { return static_cast<std::uint64_t>(v) * 0x9E3779B97F4A7C15ull; };
        return h(c[0]) ^ (h(c[1]) << 1) ^ (h(c[2]) << 2) ^ (h(c[1]) >> 7);
    }
    float tol_;
    std::vector<Vec3> points_;
    std::unordered_map<std::uint64_t, std::vector<int>> cells_;
};

// Triangulacion por orejas de un poligono simple en 2D (antihorario).
// Devuelve indices locales del poligono.
std::vector<std::uint32_t> earClip(const std::vector<Vec2>& poly) {
    std::vector<std::uint32_t> out;
    const std::size_t n = poly.size();
    if (n < 3) return out;
    std::vector<std::uint32_t> idx(n);
    for (std::size_t i = 0; i < n; ++i) idx[i] = static_cast<std::uint32_t>(i);
    const auto cross2 = [](const Vec2& o, const Vec2& a, const Vec2& b) {
        return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
    };
    const auto inTri = [&](const Vec2& p, const Vec2& a, const Vec2& b, const Vec2& c) {
        return cross2(a, b, p) >= -1e-12f && cross2(b, c, p) >= -1e-12f && cross2(c, a, p) >= -1e-12f;
    };
    std::size_t guard = 0;
    while (idx.size() > 3 && guard++ < n * n + 16) {
        bool clipped = false;
        for (std::size_t i = 0; i < idx.size(); ++i) {
            const std::uint32_t ia = idx[(i + idx.size() - 1) % idx.size()];
            const std::uint32_t ib = idx[i];
            const std::uint32_t ic = idx[(i + 1) % idx.size()];
            const Vec2& a = poly[ia];
            const Vec2& b = poly[ib];
            const Vec2& c = poly[ic];
            if (cross2(a, b, c) <= 1e-14f) continue;  // reflejo o degenerado
            bool ear = true;
            for (const std::uint32_t j : idx) {
                if (j == ia || j == ib || j == ic) continue;
                if (inTri(poly[j], a, b, c)) {
                    ear = false;
                    break;
                }
            }
            if (!ear) continue;
            out.insert(out.end(), {ia, ib, ic});
            idx.erase(idx.begin() + static_cast<std::ptrdiff_t>(i));
            clipped = true;
            break;
        }
        if (!clipped) {
            // Poligono raro (casi degenerado): abanico con lo que queda.
            for (std::size_t i = 1; i + 1 < idx.size(); ++i) out.insert(out.end(), {idx[0], idx[i], idx[i + 1]});
            return out;
        }
    }
    if (idx.size() == 3) out.insert(out.end(), {idx[0], idx[1], idx[2]});
    return out;
}

// Recorta la malla con el plano dot(n, p) = d y se queda con el lado
// dot(n, p) <= d. Tapa el corte con caras interiores (normal +n).
// Devuelve false si no cambio nada (todo estaba ya dentro).
bool clipMesh(const Work& in, const Vec3& n, float d, float uv_scale, float tolerance, Work& out) {
    std::vector<float> dist(in.verts.size());
    bool any_out = false;
    for (std::size_t i = 0; i < in.verts.size(); ++i) dist[i] = core::dot(n, in.verts[i].p) - d;
    for (const Tri& t : in.tris) {
        if (dist[t.a] > tolerance || dist[t.b] > tolerance || dist[t.c] > tolerance) {
            any_out = true;
            break;
        }
    }
    if (!any_out) return false;

    out.verts = in.verts;
    out.tris.clear();
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t> cuts;
    const auto cutVertex = [&](std::uint32_t a, std::uint32_t b) {
        const std::uint32_t lo = std::min(a, b);
        const std::uint32_t hi = std::max(a, b);
        const auto it = cuts.find({lo, hi});
        if (it != cuts.end()) return it->second;
        const float da = dist[lo];
        const float db = dist[hi];
        const float t = std::clamp(da / (da - db), 0.0f, 1.0f);
        Vert v;
        v.p = lerp3(in.verts[lo].p, in.verts[hi].p, t);
        v.n = safeNormalize(lerp3(in.verts[lo].n, in.verts[hi].n, t));
        v.uv = lerp2(in.verts[lo].uv, in.verts[hi].uv, t);
        const std::uint32_t id = static_cast<std::uint32_t>(out.verts.size());
        out.verts.push_back(v);
        cuts.emplace(std::make_pair(lo, hi), id);
        return id;
    };
    std::vector<std::pair<Vec3, Vec3>> segments;
    for (const Tri& t : in.tris) {
        const std::uint32_t v[3] = {t.a, t.b, t.c};
        const bool keep[3] = {dist[t.a] <= tolerance, dist[t.b] <= tolerance, dist[t.c] <= tolerance};
        const int kept = static_cast<int>(keep[0]) + static_cast<int>(keep[1]) + static_cast<int>(keep[2]);
        if (kept == 3) {
            out.tris.push_back(t);
            continue;
        }
        if (kept == 0) continue;
        std::uint32_t poly[4];
        int count = 0;
        std::uint32_t crossing[2];
        int crossings = 0;
        for (int e = 0; e < 3; ++e) {
            const int f = (e + 1) % 3;
            if (keep[e]) poly[count++] = v[e];
            if (keep[e] != keep[f]) {
                const std::uint32_t c = cutVertex(v[e], v[f]);
                poly[count++] = c;
                if (crossings < 2) crossing[crossings++] = c;
            }
        }
        for (int i = 1; i + 1 < count; ++i) out.tris.push_back(Tri{poly[0], poly[i], poly[i + 1], t.interior});
        if (crossings == 2) segments.emplace_back(out.verts[crossing[0]].p, out.verts[crossing[1]].p);
    }

    // Tapas: los segmentos del corte encadenados en lazos.
    PointWelder welder(tolerance * 4.0f);
    std::map<int, std::vector<int>> adjacency;
    for (const auto& [a, b] : segments) {
        const int ia = welder.add(a);
        const int ib = welder.add(b);
        if (ia == ib) continue;
        adjacency[ia].push_back(ib);
        adjacency[ib].push_back(ia);
    }
    // Base del plano con u x v = n (lo antihorario en (u, v) mira a +n).
    const Vec3 helper = std::abs(n.y) < 0.9f ? Vec3{0.0f, 1.0f, 0.0f} : Vec3{1.0f, 0.0f, 0.0f};
    const Vec3 u = safeNormalize(core::cross(helper, n));
    const Vec3 w = core::cross(n, u);
    std::map<std::pair<int, int>, bool> used;
    const auto edgeKey = [](int a, int b) { return std::make_pair(std::min(a, b), std::max(a, b)); };
    for (const auto& [start, neighbors] : adjacency) {
        for (const int first : neighbors) {
            if (used[edgeKey(start, first)]) continue;
            std::vector<int> loop{start};
            used[edgeKey(start, first)] = true;
            int previous = start;
            int current = first;
            std::size_t guard = 0;
            while (current != start && guard++ < segments.size() + 4) {
                loop.push_back(current);
                int next = -1;
                for (const int candidate : adjacency[current]) {
                    if (candidate == previous && adjacency[current].size() > 1) continue;
                    if (used[edgeKey(current, candidate)]) continue;
                    next = candidate;
                    break;
                }
                if (next < 0) break;  // lazo abierto (malla con agujeros): se cierra igual
                used[edgeKey(current, next)] = true;
                previous = current;
                current = next;
            }
            if (loop.size() < 3) continue;
            std::vector<Vec2> poly(loop.size());
            float area = 0.0f;
            for (std::size_t i = 0; i < loop.size(); ++i) {
                const Vec3& p = welder.point(loop[i]);
                poly[i] = Vec2{core::dot(p, u), core::dot(p, w)};
            }
            for (std::size_t i = 0; i < poly.size(); ++i) {
                const Vec2& a = poly[i];
                const Vec2& b = poly[(i + 1) % poly.size()];
                area += a.x * b.y - b.x * a.y;
            }
            if (std::abs(area) < 1e-12f) continue;
            if (area < 0.0f) {
                std::reverse(loop.begin(), loop.end());
                std::reverse(poly.begin(), poly.end());
            }
            const std::uint32_t base = static_cast<std::uint32_t>(out.verts.size());
            for (std::size_t i = 0; i < loop.size(); ++i) {
                Vert v;
                v.p = welder.point(loop[i]);
                v.n = n;
                v.uv = Vec2{poly[i].x * uv_scale, poly[i].y * uv_scale};
                out.verts.push_back(v);
            }
            const std::vector<std::uint32_t> tris = earClip(poly);
            for (std::size_t i = 0; i + 2 < tris.size(); i += 3) {
                out.tris.push_back(Tri{base + tris[i], base + tris[i + 1], base + tris[i + 2], true});
            }
        }
    }
    return true;
}

// Quita los vertices que ya no usa ningun triangulo.
void compact(Work& w) {
    std::vector<std::int64_t> remap(w.verts.size(), -1);
    std::vector<Vert> verts;
    verts.reserve(w.verts.size());
    for (Tri& t : w.tris) {
        for (std::uint32_t* i : {&t.a, &t.b, &t.c}) {
            if (remap[*i] < 0) {
                remap[*i] = static_cast<std::int64_t>(verts.size());
                verts.push_back(w.verts[*i]);
            }
            *i = static_cast<std::uint32_t>(remap[*i]);
        }
    }
    w.verts = std::move(verts);
}

Work workFrom(const FractureSourceMesh& s) {
    Work w;
    w.verts.resize(s.positions.size());
    for (std::size_t i = 0; i < s.positions.size(); ++i) {
        w.verts[i].p = s.positions[i];
        if (i < s.uvs.size()) w.verts[i].uv = s.uvs[i];
    }
    const bool has_normals = s.normals.size() == s.positions.size();
    std::vector<Vec3> face_normals(has_normals ? 0 : s.positions.size(), Vec3{});
    for (std::size_t i = 0; i + 2 < s.indices.size(); i += 3) {
        const std::uint32_t a = s.indices[i], b = s.indices[i + 1], c = s.indices[i + 2];
        if (a >= s.positions.size() || b >= s.positions.size() || c >= s.positions.size()) continue;
        const Vec3 face = core::cross(s.positions[b] - s.positions[a], s.positions[c] - s.positions[a]);
        if (core::length(face) < 1e-14f) continue;  // degenerado
        w.tris.push_back(Tri{a, b, c, false});
        if (!has_normals) {
            face_normals[a] += face;
            face_normals[b] += face;
            face_normals[c] += face;
        }
    }
    for (std::size_t i = 0; i < w.verts.size(); ++i) {
        w.verts[i].n = has_normals ? safeNormalize(s.normals[i]) : safeNormalize(face_normals[i]);
    }
    compact(w);
    return w;
}

// Celdas de Voronoi de `work` con `count` semillas; devuelve las celdas.
std::vector<Work> voronoi(const Work& work, int count, std::mt19937& rng, const FractureSettings& settings, bool cluster,
                          float uv_scale) {
    std::vector<Work> cells;
    Vec3 lo, hi;
    bounds(work, lo, hi);
    const Vec3 size = hi - lo;
    const float diagonal = std::max(core::length(size), 1e-4f);
    const float tolerance = diagonal * 1e-6f;
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    const auto random_in_box = [&]() {
        return Vec3{lo.x + size.x * unit(rng), lo.y + size.y * unit(rng), lo.z + size.z * unit(rng)};
    };
    std::vector<Vec3> seeds;
    count = std::clamp(count, 2, 512);
    int attempts = 0;
    int clustered = cluster ? (count * 3) / 4 : 0;
    while (static_cast<int>(seeds.size()) < count && attempts < count * 200) {
        ++attempts;
        Vec3 p;
        if (clustered > 0) {
            // Bola alrededor del punto de impacto: trozos pequenos alli.
            Vec3 d{unit(rng) * 2.0f - 1.0f, unit(rng) * 2.0f - 1.0f, unit(rng) * 2.0f - 1.0f};
            if (core::length(d) > 1.0f) continue;
            p = settings.cluster_point + d * std::max(settings.cluster_radius, 0.01f);
        } else {
            p = random_in_box();
        }
        // Dentro de la malla (si es cerrada); tras muchos intentos, en la caja.
        if (attempts < count * 100 && !inside(work, p)) continue;
        if (clustered > 0) --clustered;
        seeds.push_back(p);
    }
    if (seeds.size() < 2) return cells;
    for (std::size_t i = 0; i < seeds.size(); ++i) {
        // Las semillas cercanas primero: recortan mas y antes.
        std::vector<std::size_t> order;
        for (std::size_t j = 0; j < seeds.size(); ++j) {
            if (j != i) order.push_back(j);
        }
        std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
            return core::length(seeds[a] - seeds[i]) < core::length(seeds[b] - seeds[i]);
        });
        Work cell = work;
        Work next;
        for (const std::size_t j : order) {
            const Vec3 delta = seeds[j] - seeds[i];
            const float len = core::length(delta);
            if (len < 1e-7f) continue;
            const Vec3 n = delta * (1.0f / len);
            const float d = core::dot(n, (seeds[i] + seeds[j]) * 0.5f);
            if (clipMesh(cell, n, d, uv_scale, tolerance, next)) {
                std::swap(cell, next);
                if (cell.tris.empty()) break;
            }
        }
        if (cell.tris.empty()) continue;
        compact(cell);
        cells.push_back(std::move(cell));
    }
    return cells;
}

void appendPieces(const Work& work, int count, int level, int parent, std::mt19937& rng, const FractureSettings& s,
                  float min_volume, FractureData& out) {
    const bool cluster = s.cluster && level == 0;
    std::vector<Work> cells = voronoi(work, count, rng, s, cluster, std::max(s.interior_uv_scale, 0.001f));
    for (Work& cell : cells) {
        Vec3 centroid;
        const float volume = std::abs(volumeOf(cell, centroid));
        if (volume < min_volume) continue;
        FracturePiece piece;
        piece.center = centroid;
        piece.volume = volume;
        piece.level = level;
        piece.parent = parent;
        piece.positions.reserve(cell.verts.size());
        piece.normals.reserve(cell.verts.size());
        piece.uvs.reserve(cell.verts.size());
        for (const Vert& v : cell.verts) {
            piece.positions.push_back(v.p - centroid);
            piece.normals.push_back(v.n);
            piece.uvs.push_back(v.uv);
        }
        for (const Tri& t : cell.tris) {
            std::vector<std::uint32_t>& list = t.interior ? piece.interior : piece.exterior;
            list.insert(list.end(), {t.a, t.b, t.c});
        }
        const int index = static_cast<int>(out.pieces.size());
        out.pieces.push_back(std::move(piece));
        if (parent >= 0) out.pieces[static_cast<std::size_t>(parent)].children.push_back(index);
        if (level + 1 < std::clamp(s.levels, 1, 3)) {
            appendPieces(cell, std::max(s.sub_pieces, 2), level + 1, index, rng, s, min_volume, out);
        }
    }
}

json vec3Json(const Vec3& v) { return json::array({v.x, v.y, v.z}); }
Vec3 vec3From(const json& j, const Vec3& fallback = {}) {
    if (!j.is_array() || j.size() < 3) return fallback;
    return Vec3{j[0].get<float>(), j[1].get<float>(), j[2].get<float>()};
}

float srgbFromLinear(float c) {
    c = std::clamp(c, 0.0f, 1.0f);
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

}  // namespace

std::vector<int> FractureData::roots() const {
    std::vector<int> out;
    for (std::size_t i = 0; i < pieces.size(); ++i) {
        if (pieces[i].parent < 0) out.push_back(static_cast<int>(i));
    }
    return out;
}

std::size_t FractureData::triangleCount() const {
    std::size_t n = 0;
    for (const FracturePiece& p : pieces) n += (p.exterior.size() + p.interior.size()) / 3;
    return n;
}

FractureSourceMesh sourceFromModel(const asset::ModelData& model) {
    FractureSourceMesh s;
    s.positions.reserve(model.vertices.size());
    s.normals.reserve(model.vertices.size());
    s.uvs.reserve(model.vertices.size());
    for (const asset::SkinnedVertex& v : model.vertices) {
        s.positions.push_back(v.position);
        s.normals.push_back(v.normal);
        s.uvs.push_back(v.uv);
    }
    s.indices = model.indices;
    return s;
}

FractureSourceMesh sourceFromMesh(const ecs::Mesh& mesh) {
    FractureSourceMesh s;
    s.positions = mesh.vertices;
    if (mesh.normals.size() == mesh.vertices.size()) s.normals = mesh.normals;
    if (mesh.uv.size() == mesh.vertices.size()) s.uvs = mesh.uv;
    s.indices = mesh.allTriangles();
    return s;
}

FractureSourceMesh boxSource(const Vec3& size) {
    FractureSourceMesh s;
    const Vec3 h = size * 0.5f;
    // 6 caras con sus vertices (aristas duras), antihorarias vistas desde fuera.
    const Vec3 normals[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (const Vec3& n : normals) {
        const Vec3 up = std::abs(n.y) > 0.5f ? Vec3{0.0f, 0.0f, 1.0f} : Vec3{0.0f, 1.0f, 0.0f};
        const Vec3 right = core::cross(up, n);
        const Vec3 c{n.x * h.x, n.y * h.y, n.z * h.z};
        const Vec3 r{right.x * h.x, right.y * h.y, right.z * h.z};
        const Vec3 u{up.x * h.x, up.y * h.y, up.z * h.z};
        const std::uint32_t base = static_cast<std::uint32_t>(s.positions.size());
        const Vec3 corners[4] = {c - r - u, c + r - u, c + r + u, c - r + u};
        const Vec2 uvs[4] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};
        for (int i = 0; i < 4; ++i) {
            s.positions.push_back(corners[i]);
            s.normals.push_back(n);
            s.uvs.push_back(uvs[i]);
        }
        // right x up = n  =>  (0,1,2) es antihorario visto desde +n.
        s.indices.insert(s.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }
    return s;
}

bool fractureMesh(const FractureSourceMesh& source, const FractureSettings& settings, FractureData& out, std::string* error) {
    Work work = workFrom(source);
    if (work.tris.size() < 4) {
        if (error) *error = "la malla no tiene triangulos suficientes";
        return false;
    }
    Vec3 centroid;
    float total = volumeOf(work, centroid);
    if (total < 0.0f) {
        // Caras al reves (miran adentro): se dan la vuelta.
        for (Tri& t : work.tris) std::swap(t.b, t.c);
        for (Vert& v : work.verts) v.n = -v.n;
        total = -total;
    }
    out.pieces.clear();
    out.settings = settings;
    bounds(work, out.bounds_min, out.bounds_max);
    out.total_volume = total;
    if (!out.uuid.valid()) out.uuid = Uuid::generate();
    std::mt19937 rng(settings.seed == 0 ? 1u : settings.seed);
    const float min_volume = std::max(total, 1e-9f) * std::clamp(settings.min_piece_fraction, 0.0f, 0.2f);
    appendPieces(work, settings.pieces, 0, -1, rng, settings, min_volume, out);
    if (out.roots().empty()) {
        if (error) *error = "no salio ningun trozo (la malla esta abierta o es plana?)";
        return false;
    }
    return true;
}

std::string fractureToText(const FractureData& data) {
    const FractureSettings& s = data.settings;
    json header = {{"uuid", data.uuid.toString()},
                   {"type", "fracture"},
                   {"version", 1},
                   {"pieces", data.pieces.size()},
                   {"source_model", data.source_model.valid() ? data.source_model.toString() : std::string()},
                   {"source_part", data.source_part},
                   {"bounds_min", vec3Json(data.bounds_min)},
                   {"bounds_max", vec3Json(data.bounds_max)},
                   {"total_volume", data.total_volume},
                   {"settings",
                    {{"pieces", s.pieces},
                     {"seed", s.seed},
                     {"levels", s.levels},
                     {"sub_pieces", s.sub_pieces},
                     {"interior_uv_scale", s.interior_uv_scale},
                     {"cluster", s.cluster},
                     {"cluster_point", vec3Json(s.cluster_point)},
                     {"cluster_radius", s.cluster_radius},
                     {"min_piece_fraction", s.min_piece_fraction}}}};
    json pieces = json::array();
    for (const FracturePiece& p : data.pieces) {
        json positions = json::array();
        json normals = json::array();
        json uvs = json::array();
        for (const Vec3& v : p.positions) positions.insert(positions.end(), {v.x, v.y, v.z});
        for (const Vec3& v : p.normals) normals.insert(normals.end(), {v.x, v.y, v.z});
        for (const Vec2& v : p.uvs) uvs.insert(uvs.end(), {v.x, v.y});
        pieces.push_back({{"center", vec3Json(p.center)},
                          {"volume", p.volume},
                          {"level", p.level},
                          {"parent", p.parent},
                          {"children", p.children},
                          {"positions", positions},
                          {"normals", normals},
                          {"uvs", uvs},
                          {"exterior", p.exterior},
                          {"interior", p.interior}});
    }
    json body = {{"pieces", pieces}};
    return header.dump() + "\n" + body.dump() + "\n";
}

bool fractureFromText(const std::string& text, FractureData& out, std::string* error) {
    const std::size_t newline = text.find('\n');
    const json header = json::parse(text.substr(0, newline), nullptr, false);
    if (header.is_discarded() || !header.is_object() || !header.contains("uuid")) {
        if (error) *error = "cabecera no valida";
        return false;
    }
    const json body = newline == std::string::npos ? json() : json::parse(text.substr(newline + 1), nullptr, false);
    if (body.is_discarded() || !body.is_object() || !body.contains("pieces") || !body["pieces"].is_array()) {
        if (error) *error = "trozos no validos";
        return false;
    }
    out = FractureData{};
    out.uuid = Uuid::parse(header.value("uuid", std::string()));
    out.source_model = Uuid::parse(header.value("source_model", std::string()));
    out.source_part = header.value("source_part", 0);
    out.bounds_min = vec3From(header.value("bounds_min", json()));
    out.bounds_max = vec3From(header.value("bounds_max", json()));
    out.total_volume = header.value("total_volume", 0.0f);
    if (header.contains("settings") && header["settings"].is_object()) {
        const json& s = header["settings"];
        FractureSettings& t = out.settings;
        t.pieces = s.value("pieces", t.pieces);
        t.seed = s.value("seed", t.seed);
        t.levels = s.value("levels", t.levels);
        t.sub_pieces = s.value("sub_pieces", t.sub_pieces);
        t.interior_uv_scale = s.value("interior_uv_scale", t.interior_uv_scale);
        t.cluster = s.value("cluster", t.cluster);
        t.cluster_point = vec3From(s.value("cluster_point", json()));
        t.cluster_radius = s.value("cluster_radius", t.cluster_radius);
        t.min_piece_fraction = s.value("min_piece_fraction", t.min_piece_fraction);
    }
    for (const json& j : body["pieces"]) {
        FracturePiece p;
        p.center = vec3From(j.value("center", json()));
        p.volume = j.value("volume", 0.0f);
        p.level = j.value("level", 0);
        p.parent = j.value("parent", -1);
        p.children = j.value("children", std::vector<int>{});
        const std::vector<float> positions = j.value("positions", std::vector<float>{});
        const std::vector<float> normals = j.value("normals", std::vector<float>{});
        const std::vector<float> uvs = j.value("uvs", std::vector<float>{});
        const std::size_t count = positions.size() / 3;
        p.positions.resize(count);
        p.normals.assign(count, Vec3{0.0f, 1.0f, 0.0f});
        p.uvs.assign(count, Vec2{});
        for (std::size_t i = 0; i < count; ++i) {
            p.positions[i] = Vec3{positions[i * 3], positions[i * 3 + 1], positions[i * 3 + 2]};
            if (normals.size() >= (i + 1) * 3) p.normals[i] = Vec3{normals[i * 3], normals[i * 3 + 1], normals[i * 3 + 2]};
            if (uvs.size() >= (i + 1) * 2) p.uvs[i] = Vec2{uvs[i * 2], uvs[i * 2 + 1]};
        }
        p.exterior = j.value("exterior", std::vector<std::uint32_t>{});
        p.interior = j.value("interior", std::vector<std::uint32_t>{});
        const auto valid = [&](const std::vector<std::uint32_t>& list) {
            return list.size() % 3 == 0 && std::all_of(list.begin(), list.end(), [&](std::uint32_t i) { return i < count; });
        };
        if (!valid(p.exterior) || !valid(p.interior)) {
            if (error) *error = "un trozo usa vertices que no existen";
            return false;
        }
        out.pieces.push_back(std::move(p));
    }
    // Los indices de padres e hijos, dentro de rango.
    const int total = static_cast<int>(out.pieces.size());
    for (FracturePiece& p : out.pieces) {
        if (p.parent >= total) p.parent = -1;
        p.children.erase(std::remove_if(p.children.begin(), p.children.end(), [&](int c) { return c < 0 || c >= total; }),
                         p.children.end());
    }
    return true;
}

bool saveFracture(const std::filesystem::path& file, const FractureData& data, std::string* error) {
    std::error_code ec;
    if (file.has_parent_path()) std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream outf(file, std::ios::binary | std::ios::trunc);
    if (!outf) {
        if (error) *error = "no se pudo escribir " + file.string();
        return false;
    }
    outf << fractureToText(data);
    return static_cast<bool>(outf);
}

bool loadFracture(const std::filesystem::path& file, FractureData& out, std::string* error) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        if (error) *error = "no se pudo abrir " + file.string();
        return false;
    }
    std::stringstream buffer;
    buffer << in.rdbuf();
    return fractureFromText(buffer.str(), out, error);
}

std::shared_ptr<ecs::Mesh> pieceMesh(const FracturePiece& piece, const ecs::MeshMaterial& exterior,
                                     const ecs::MeshMaterial& interior) {
    auto mesh = std::make_shared<ecs::Mesh>();
    mesh->name = "Trozo";
    mesh->vertices = piece.positions;
    mesh->normals = piece.normals;
    mesh->uv = piece.uvs;
    mesh->setSubMeshCount(2);
    mesh->setTriangles(piece.exterior, 0);
    mesh->setTriangles(piece.interior, 1);
    mesh->materials = {exterior, interior};
    mesh->recalculateTangents();
    mesh->markModified();
    return mesh;
}

// Usado por Destruction.cpp: el color del material del modelo (lineal) en
// el del Inspector (sRGB).
core::Vec4 fractureSrgbColor(const core::Vec4& linear) {
    return core::Vec4{srgbFromLinear(linear.x), srgbFromLinear(linear.y), srgbFromLinear(linear.z), linear.w};
}

}  // namespace cramion::physics
