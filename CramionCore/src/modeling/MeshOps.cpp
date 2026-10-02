// Operaciones de topologia de la malla editable (ver EditableMesh.h).
//
// Convenciones: caras antihorarias vistas desde fuera; una arista "dirigida"
// a->b pertenece a la cara que la recorre en ese sentido (su vecina la recorre
// b->a). Las operaciones que crean vertices en una arista los insertan tambien
// en la cara vecina, para que la malla siga cerrada.

#include "CramionCore/modeling/EditableMesh.h"

#include "CramionCore/ecs/MathUtil.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <queue>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace cramion::modeling::ops {

namespace {

std::uint64_t directed(std::uint32_t a, std::uint32_t b) {
    return (static_cast<std::uint64_t>(a) << 32) | b;
}

// Arista sin direccion -> caras que la usan.
std::unordered_map<std::uint64_t, std::vector<int>> edgeFaces(const PolyMesh& m) {
    std::unordered_map<std::uint64_t, std::vector<int>> out;
    for (std::size_t fi = 0; fi < m.faces.size(); ++fi) {
        const Face& f = m.faces[fi];
        for (std::size_t i = 0; i < f.v.size(); ++i) {
            std::vector<int>& list = out[Edge(f.v[i], f.v[(i + 1) % f.v.size()]).key()];
            if (list.empty() || list.back() != static_cast<int>(fi)) list.push_back(static_cast<int>(fi));
        }
    }
    return out;
}

bool hasDirected(const Face& f, std::uint32_t a, std::uint32_t b) {
    for (std::size_t i = 0; i < f.v.size(); ++i) {
        if (f.v[i] == a && f.v[(i + 1) % f.v.size()] == b) return true;
    }
    return false;
}

Vec3 safeNormalize(const Vec3& v, const Vec3& fallback = Vec3{0.0f, 1.0f, 0.0f}) {
    const float len = core::length(v);
    return len > 1e-12f ? v * (1.0f / len) : fallback;
}

Face childFace(const Face& parent, std::vector<std::uint32_t> v) {
    Face f;
    f.v = std::move(v);
    f.material = parent.material;
    f.smoothing = parent.smoothing;
    f.uvs = parent.uvs;
    if (f.uvs.mode == UvMode::Manual) f.uvs.mode = UvMode::Box;
    return f;
}

// Punto hacia dentro de la cara en la esquina v (entre p y n), a `amount` de
// las dos aristas.
Vec3 insetCorner(const Vec3& p, const Vec3& v, const Vec3& n, const Vec3& normal, float amount) {
    const Vec3 dp = safeNormalize(p - v);
    const Vec3 dn = safeNormalize(n - v);
    const Vec3 c = core::cross(dn, dp);
    const float sin_t = core::length(c);
    if (sin_t < 1e-3f) {
        // Recta: perpendicular hacia dentro.
        return v + safeNormalize(core::cross(normal, dn)) * amount;
    }
    Vec3 dir = dp + dn;
    if (core::dot(c, normal) < 0.0f) dir = -dir;  // esquina concava
    return v + dir * (amount / std::max(sin_t, 0.15f));
}

// Inserta `mid` entre a y b (en cualquier sentido) en las caras que tengan esa arista.
void insertOnEdge(PolyMesh& m, std::uint32_t a, std::uint32_t b, const std::vector<std::uint32_t>& points_from_a,
                  int skip_face = -1) {
    for (std::size_t fi = 0; fi < m.faces.size(); ++fi) {
        if (static_cast<int>(fi) == skip_face) continue;
        Face& f = m.faces[fi];
        const std::size_t n = f.v.size();
        for (std::size_t i = 0; i < n; ++i) {
            const std::uint32_t x = f.v[i], y = f.v[(i + 1) % n];
            if (!((x == a && y == b) || (x == b && y == a))) continue;
            std::vector<std::uint32_t> mids = points_from_a;
            if (x == b) std::reverse(mids.begin(), mids.end());
            const bool manual = f.uvs.mode == UvMode::Manual && f.uv.size() == n;
            std::vector<Vec2> uv_mids;
            if (manual) {
                const Vec2 ua = f.uv[i], ub = f.uv[(i + 1) % n];
                for (std::size_t k = 0; k < mids.size(); ++k) {
                    const float t = static_cast<float>(k + 1) / static_cast<float>(mids.size() + 1);
                    uv_mids.push_back(Vec2{ua.x + (ub.x - ua.x) * t, ua.y + (ub.y - ua.y) * t});
                }
                f.uv.insert(f.uv.begin() + static_cast<std::ptrdiff_t>(i + 1), uv_mids.begin(), uv_mids.end());
            }
            f.v.insert(f.v.begin() + static_cast<std::ptrdiff_t>(i + 1), mids.begin(), mids.end());
            break;
        }
    }
}

// Parte una cara por la cuerda entre las esquinas i y j (i < j). La cara
// original se queda con i..j y la nueva (al final) con j..i.
int splitFace(PolyMesh& m, int face, std::size_t i, std::size_t j) {
    Face& f = m.faces[static_cast<std::size_t>(face)];
    const std::size_t n = f.v.size();
    if (i > j) std::swap(i, j);
    if (j - i < 2 || (i == 0 && j == n - 1)) return -1;  // vecinas: no hay corte
    std::vector<std::uint32_t> a(f.v.begin() + static_cast<std::ptrdiff_t>(i), f.v.begin() + static_cast<std::ptrdiff_t>(j + 1));
    std::vector<std::uint32_t> b(f.v.begin() + static_cast<std::ptrdiff_t>(j), f.v.end());
    b.insert(b.end(), f.v.begin(), f.v.begin() + static_cast<std::ptrdiff_t>(i + 1));
    const bool manual = f.uvs.mode == UvMode::Manual && f.uv.size() == n;
    std::vector<Vec2> ua, ub;
    if (manual) {
        ua.assign(f.uv.begin() + static_cast<std::ptrdiff_t>(i), f.uv.begin() + static_cast<std::ptrdiff_t>(j + 1));
        ub.assign(f.uv.begin() + static_cast<std::ptrdiff_t>(j), f.uv.end());
        ub.insert(ub.end(), f.uv.begin(), f.uv.begin() + static_cast<std::ptrdiff_t>(i + 1));
    }
    Face second = f;
    f.v = std::move(a);
    f.uv = std::move(ua);
    second.v = std::move(b);
    second.uv = std::move(ub);
    m.faces.push_back(std::move(second));
    return static_cast<int>(m.faces.size()) - 1;
}

// Bucles de borde (el agujero, recorrido al reves que las caras de alrededor).
std::vector<std::vector<std::uint32_t>> borderLoops(const PolyMesh& m) {
    const auto faces_of = edgeFaces(m);
    std::unordered_multimap<std::uint32_t, std::uint32_t> next;  // b -> a por cada arista de borde a->b
    for (const Face& f : m.faces) {
        for (std::size_t i = 0; i < f.v.size(); ++i) {
            const std::uint32_t a = f.v[i], b = f.v[(i + 1) % f.v.size()];
            const auto it = faces_of.find(Edge(a, b).key());
            if (it != faces_of.end() && it->second.size() == 1) next.emplace(b, a);
        }
    }
    std::vector<std::vector<std::uint32_t>> loops;
    while (!next.empty()) {
        auto it = next.begin();
        const std::uint32_t start = it->first;
        std::vector<std::uint32_t> loop{start};
        std::uint32_t current = it->second;
        next.erase(it);
        std::size_t guard = 0;
        while (current != start && guard++ < m.positions.size() + 4) {
            loop.push_back(current);
            const auto step = next.find(current);
            if (step == next.end()) break;
            const std::uint32_t to = step->second;
            next.erase(step);
            current = to;
        }
        if (current == start && loop.size() >= 3) loops.push_back(std::move(loop));
    }
    return loops;
}

std::vector<std::uint32_t> neighborsOf(const PolyMesh& m, std::uint32_t v) {
    std::set<std::uint32_t> out;
    for (const Face& f : m.faces) {
        const std::size_t n = f.v.size();
        for (std::size_t i = 0; i < n; ++i) {
            if (f.v[i] != v) continue;
            out.insert(f.v[(i + 1) % n]);
            out.insert(f.v[(i + n - 1) % n]);
        }
    }
    return {out.begin(), out.end()};
}

}  // namespace

// -----------------------------------------------------------------------------
// Basicas
// -----------------------------------------------------------------------------

std::vector<std::uint32_t> verticesOfFaces(const PolyMesh& m, const std::vector<int>& faces) {
    std::set<std::uint32_t> out;
    for (const int f : faces) {
        if (f < 0 || static_cast<std::size_t>(f) >= m.faces.size()) continue;
        for (const std::uint32_t v : m.faces[static_cast<std::size_t>(f)].v) out.insert(v);
    }
    return {out.begin(), out.end()};
}

std::vector<std::uint32_t> verticesOfEdges(const std::vector<Edge>& edges) {
    std::set<std::uint32_t> out;
    for (const Edge& e : edges) {
        out.insert(e.a);
        out.insert(e.b);
    }
    return {out.begin(), out.end()};
}

void transformVertices(PolyMesh& m, const std::vector<std::uint32_t>& vertices, const Mat4& matrix) {
    for (const std::uint32_t v : vertices) {
        if (v < m.positions.size()) m.positions[v] = ecs::transformPoint(matrix, m.positions[v]);
    }
}

void deleteFaces(PolyMesh& m, const std::vector<int>& faces) {
    std::vector<int> sorted = faces;
    std::sort(sorted.begin(), sorted.end());
    sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
    for (auto it = sorted.rbegin(); it != sorted.rend(); ++it) {
        if (*it >= 0 && static_cast<std::size_t>(*it) < m.faces.size()) m.faces.erase(m.faces.begin() + *it);
    }
    m.removeUnusedVertices();
}

void flipFaces(PolyMesh& m, const std::vector<int>& faces) {
    for (const int fi : faces) {
        if (fi < 0 || static_cast<std::size_t>(fi) >= m.faces.size()) continue;
        Face& f = m.faces[static_cast<std::size_t>(fi)];
        std::reverse(f.v.begin(), f.v.end());
        std::reverse(f.uv.begin(), f.uv.end());
    }
}

void conformNormals(PolyMesh& m) {
    const std::size_t count = m.faces.size();
    if (count == 0) return;
    const auto faces_of = edgeFaces(m);
    std::vector<char> visited(count, 0);
    for (std::size_t seed = 0; seed < count; ++seed) {
        if (visited[seed]) continue;
        std::vector<int> component;
        std::queue<int> queue;
        queue.push(static_cast<int>(seed));
        visited[seed] = 1;
        bool closed = true;
        while (!queue.empty()) {
            const int fi = queue.front();
            queue.pop();
            component.push_back(fi);
            const Face f = m.faces[static_cast<std::size_t>(fi)];
            for (std::size_t i = 0; i < f.v.size(); ++i) {
                const std::uint32_t a = f.v[i], b = f.v[(i + 1) % f.v.size()];
                const auto it = faces_of.find(Edge(a, b).key());
                if (it == faces_of.end()) continue;
                if (it->second.size() < 2) closed = false;
                for (const int g : it->second) {
                    if (g == fi || visited[static_cast<std::size_t>(g)]) continue;
                    // La vecina debe recorrer la arista al reves.
                    if (hasDirected(m.faces[static_cast<std::size_t>(g)], a, b)) flipFaces(m, {g});
                    visited[static_cast<std::size_t>(g)] = 1;
                    queue.push(g);
                }
            }
        }
        if (!closed) continue;
        // Cerrada: hacia fuera (volumen con signo positivo).
        double volume = 0.0;
        for (const int fi : component) {
            const Face& f = m.faces[static_cast<std::size_t>(fi)];
            const std::vector<std::uint32_t> tris = m.triangulateFace(fi);
            for (std::size_t t = 0; t + 2 < tris.size(); t += 3) {
                const Vec3& p0 = m.positions[f.v[tris[t]]];
                const Vec3& p1 = m.positions[f.v[tris[t + 1]]];
                const Vec3& p2 = m.positions[f.v[tris[t + 2]]];
                volume += static_cast<double>(core::dot(p0, core::cross(p1, p2)));
            }
        }
        if (volume < 0.0) flipFaces(m, component);
    }
}

std::vector<int> triangulateFaces(PolyMesh& m, const std::vector<int>& faces) {
    std::vector<int> out;
    for (const int fi : faces) {
        if (fi < 0 || static_cast<std::size_t>(fi) >= m.faces.size()) continue;
        const std::vector<std::uint32_t> tris = m.triangulateFace(fi);
        const Face original = m.faces[static_cast<std::size_t>(fi)];
        const bool manual = original.uvs.mode == UvMode::Manual && original.uv.size() == original.v.size();
        for (std::size_t t = 0; t + 2 < tris.size(); t += 3) {
            Face tri = original;
            tri.v = {original.v[tris[t]], original.v[tris[t + 1]], original.v[tris[t + 2]]};
            tri.uv.clear();
            if (manual) tri.uv = {original.uv[tris[t]], original.uv[tris[t + 1]], original.uv[tris[t + 2]]};
            if (t == 0) {
                m.faces[static_cast<std::size_t>(fi)] = std::move(tri);
                out.push_back(fi);
            } else {
                m.faces.push_back(std::move(tri));
                out.push_back(static_cast<int>(m.faces.size()) - 1);
            }
        }
    }
    return out;
}

int weldVertices(PolyMesh& m, const std::vector<std::uint32_t>& vertices, float distance) {
    std::vector<std::uint32_t> candidates = vertices;
    if (candidates.empty()) {
        candidates.resize(m.positions.size());
        for (std::size_t i = 0; i < candidates.size(); ++i) candidates[i] = static_cast<std::uint32_t>(i);
    }
    const float cell = std::max(distance, 1e-6f);
    const auto key = [&](const Vec3& p, int dx, int dy, int dz) {
        const auto c = [&](float x, int d) { return static_cast<std::int64_t>(std::floor(x / cell)) + d; };
        return (c(p.x, dx) * 73856093) ^ (c(p.y, dy) * 19349663) ^ (c(p.z, dz) * 83492791);
    };
    std::unordered_multimap<std::int64_t, std::uint32_t> grid;
    std::vector<std::uint32_t> remap(m.positions.size());
    for (std::size_t i = 0; i < remap.size(); ++i) remap[i] = static_cast<std::uint32_t>(i);
    int merged = 0;
    for (const std::uint32_t v : candidates) {
        if (v >= m.positions.size()) continue;
        const Vec3& p = m.positions[v];
        std::uint32_t target = v;
        for (int dx = -1; dx <= 1 && target == v; ++dx) {
            for (int dy = -1; dy <= 1 && target == v; ++dy) {
                for (int dz = -1; dz <= 1 && target == v; ++dz) {
                    const auto range = grid.equal_range(key(p, dx, dy, dz));
                    for (auto it = range.first; it != range.second; ++it) {
                        if (core::length(m.positions[it->second] - p) <= distance) {
                            target = it->second;
                            break;
                        }
                    }
                }
            }
        }
        if (target != v) {
            remap[v] = target;
            ++merged;
        } else {
            grid.emplace(key(p, 0, 0, 0), v);
        }
    }
    if (merged == 0) return 0;
    for (Face& f : m.faces) {
        for (std::uint32_t& i : f.v) i = remap[i];
    }
    m.cleanup();
    m.removeUnusedVertices();
    return merged;
}

std::uint32_t collapseVertices(PolyMesh& m, const std::vector<std::uint32_t>& vertices) {
    if (vertices.empty()) return 0;
    Vec3 center{};
    for (const std::uint32_t v : vertices) center += m.positions[v];
    center = center * (1.0f / static_cast<float>(vertices.size()));
    const std::uint32_t keep = vertices.front();
    m.positions[keep] = center;
    const std::set<std::uint32_t> set(vertices.begin(), vertices.end());
    for (Face& f : m.faces) {
        for (std::uint32_t& i : f.v) {
            if (set.contains(i)) i = keep;
        }
    }
    m.cleanup();
    return keep;
}

void splitVertices(PolyMesh& m, const std::vector<std::uint32_t>& vertices) {
    const std::set<std::uint32_t> set(vertices.begin(), vertices.end());
    std::set<std::uint32_t> used;
    for (Face& f : m.faces) {
        for (std::uint32_t& i : f.v) {
            if (!set.contains(i)) continue;
            if (used.insert(i).second) continue;  // la primera cara se queda el original
            i = static_cast<std::uint32_t>(m.addVertex(m.positions[i]));
        }
    }
}

std::vector<int> duplicateFaces(PolyMesh& m, const std::vector<int>& faces) {
    std::unordered_map<std::uint32_t, std::uint32_t> copy;
    std::vector<int> out;
    for (const int fi : faces) {
        if (fi < 0 || static_cast<std::size_t>(fi) >= m.faces.size()) continue;
        Face f = m.faces[static_cast<std::size_t>(fi)];
        for (std::uint32_t& i : f.v) {
            const auto it = copy.find(i);
            if (it != copy.end()) {
                i = it->second;
            } else {
                const auto n = static_cast<std::uint32_t>(m.addVertex(m.positions[i]));
                copy[i] = n;
                i = n;
            }
        }
        m.faces.push_back(std::move(f));
        out.push_back(static_cast<int>(m.faces.size()) - 1);
    }
    return out;
}

PolyMesh detachFaces(PolyMesh& m, const std::vector<int>& faces, bool remove) {
    PolyMesh out;
    std::unordered_map<std::uint32_t, std::uint32_t> remap;
    for (const int fi : faces) {
        if (fi < 0 || static_cast<std::size_t>(fi) >= m.faces.size()) continue;
        Face f = m.faces[static_cast<std::size_t>(fi)];
        for (std::uint32_t& i : f.v) {
            const auto it = remap.find(i);
            if (it != remap.end()) {
                i = it->second;
            } else {
                const auto n = static_cast<std::uint32_t>(out.addVertex(m.positions[i]));
                remap[i] = n;
                i = n;
            }
        }
        out.faces.push_back(std::move(f));
    }
    if (remove) deleteFaces(m, faces);
    return out;
}

void append(PolyMesh& m, const PolyMesh& other, const Mat4& transform) {
    const auto base = static_cast<std::uint32_t>(m.positions.size());
    for (const Vec3& p : other.positions) m.positions.push_back(ecs::transformPoint(transform, p));
    // Escala negativa: las caras se dan la vuelta.
    const Vec3 x{transform.m[0][0], transform.m[0][1], transform.m[0][2]};
    const Vec3 y{transform.m[1][0], transform.m[1][1], transform.m[1][2]};
    const Vec3 z{transform.m[2][0], transform.m[2][1], transform.m[2][2]};
    const bool mirrored = core::dot(core::cross(x, y), z) < 0.0f;
    for (Face f : other.faces) {
        for (std::uint32_t& i : f.v) i += base;
        if (mirrored) {
            std::reverse(f.v.begin(), f.v.end());
            std::reverse(f.uv.begin(), f.uv.end());
        }
        m.faces.push_back(std::move(f));
    }
}

// -----------------------------------------------------------------------------
// Extruir, inset, bisel
// -----------------------------------------------------------------------------

std::vector<int> extrudeFaces(PolyMesh& m, const std::vector<int>& faces_in, float distance, ExtrudeMode mode) {
    std::vector<int> faces;
    for (const int f : faces_in) {
        if (f >= 0 && static_cast<std::size_t>(f) < m.faces.size()) faces.push_back(f);
    }
    std::sort(faces.begin(), faces.end());
    faces.erase(std::unique(faces.begin(), faces.end()), faces.end());
    if (faces.empty()) return {};
    if (mode == ExtrudeMode::Individual) {
        for (const int fi : faces) {
            const Vec3 normal = m.faceNormal(fi);
            const Face original = m.faces[static_cast<std::size_t>(fi)];
            std::vector<std::uint32_t> top;
            for (const std::uint32_t v : original.v) top.push_back(static_cast<std::uint32_t>(m.addVertex(m.positions[v] + normal * distance)));
            const std::size_t n = original.v.size();
            for (std::size_t i = 0; i < n; ++i) {
                const std::size_t j = (i + 1) % n;
                m.faces.push_back(childFace(original, {original.v[i], original.v[j], top[j], top[i]}));
            }
            m.faces[static_cast<std::size_t>(fi)].v = top;
        }
        return faces;
    }
    // Grupo: los vertices del borde de la region se duplican; todos se mueven
    // por la normal media de sus caras de la region.
    std::unordered_map<std::uint64_t, int> uses;
    for (const int fi : faces) {
        const Face& f = m.faces[static_cast<std::size_t>(fi)];
        for (std::size_t i = 0; i < f.v.size(); ++i) ++uses[Edge(f.v[i], f.v[(i + 1) % f.v.size()]).key()];
    }
    std::map<std::uint32_t, Vec3> normals;
    std::map<std::uint32_t, std::vector<Vec3>> face_normals;
    for (const int fi : faces) {
        const Vec3 n = m.faceNormal(fi);
        for (const std::uint32_t v : m.faces[static_cast<std::size_t>(fi)].v) {
            normals[v] += n;
            face_normals[v].push_back(n);
        }
    }
    std::unordered_map<std::uint32_t, std::uint32_t> top;
    struct Side {
        std::uint32_t a, b;
        int face;
    };
    std::vector<Side> sides;
    for (const int fi : faces) {
        const Face& f = m.faces[static_cast<std::size_t>(fi)];
        for (std::size_t i = 0; i < f.v.size(); ++i) {
            const std::uint32_t a = f.v[i], b = f.v[(i + 1) % f.v.size()];
            if (uses[Edge(a, b).key()] != 1) continue;
            sides.push_back(Side{a, b, fi});
            for (const std::uint32_t v : {a, b}) {
                if (!top.contains(v)) top[v] = static_cast<std::uint32_t>(m.addVertex(m.positions[v]));
            }
        }
    }
    // Mover (los del borde, sus copias; los de dentro, ellos mismos).
    for (const auto& [v, sum] : normals) {
        const Vec3 dir = safeNormalize(sum);
        float min_dot = 1.0f;
        for (const Vec3& n : face_normals[v]) min_dot = std::min(min_dot, core::dot(dir, n));
        const float k = distance / std::max(min_dot, 0.25f);  // grosor uniforme en las esquinas
        const auto it = top.find(v);
        const std::uint32_t target = it != top.end() ? it->second : v;
        m.positions[target] = m.positions[v] + dir * k;
    }
    for (const int fi : faces) {
        for (std::uint32_t& v : m.faces[static_cast<std::size_t>(fi)].v) {
            const auto it = top.find(v);
            if (it != top.end()) v = it->second;
        }
    }
    for (const Side& s : sides) {
        m.faces.push_back(childFace(m.faces[static_cast<std::size_t>(s.face)], {s.a, s.b, top[s.b], top[s.a]}));
    }
    return faces;
}

std::vector<Edge> extrudeEdges(PolyMesh& m, const std::vector<Edge>& edges, float distance, bool along_normal) {
    const auto faces_of = edgeFaces(m);
    struct Item {
        std::uint32_t a, b;  // como la recorre su cara
        int face;
        Vec3 dir;
    };
    std::vector<Item> items;
    std::map<std::uint32_t, Vec3> dirs;
    for (const Edge& e : edges) {
        const auto it = faces_of.find(e.key());
        if (it == faces_of.end() || it->second.size() != 1) continue;  // solo aristas de borde
        const int fi = it->second.front();
        const Face& f = m.faces[static_cast<std::size_t>(fi)];
        const bool forward = hasDirected(f, e.a, e.b);
        const std::uint32_t a = forward ? e.a : e.b, b = forward ? e.b : e.a;
        const Vec3 normal = m.faceNormal(fi);
        const Vec3 out = along_normal ? normal : safeNormalize(core::cross(m.positions[b] - m.positions[a], normal));
        items.push_back(Item{a, b, fi, out});
        dirs[a] += out;
        dirs[b] += out;
    }
    std::map<std::uint32_t, std::uint32_t> moved;
    for (const auto& [v, d] : dirs) moved[v] = static_cast<std::uint32_t>(m.addVertex(m.positions[v] + safeNormalize(d) * distance));
    std::vector<Edge> out;
    for (const Item& it : items) {
        m.faces.push_back(childFace(m.faces[static_cast<std::size_t>(it.face)], {it.b, it.a, moved[it.a], moved[it.b]}));
        out.push_back(Edge(moved[it.a], moved[it.b]));
    }
    return out;
}

std::vector<int> insetFaces(PolyMesh& m, const std::vector<int>& faces_in, float amount, bool individual) {
    std::vector<int> faces;
    for (const int f : faces_in) {
        if (f >= 0 && static_cast<std::size_t>(f) < m.faces.size()) faces.push_back(f);
    }
    std::sort(faces.begin(), faces.end());
    faces.erase(std::unique(faces.begin(), faces.end()), faces.end());
    if (individual) {
        for (const int f : faces) insetFaces(m, {f}, amount, false);
        return faces;
    }
    std::unordered_map<std::uint64_t, int> uses;
    for (const int fi : faces) {
        const Face& f = m.faces[static_cast<std::size_t>(fi)];
        for (std::size_t i = 0; i < f.v.size(); ++i) ++uses[Edge(f.v[i], f.v[(i + 1) % f.v.size()]).key()];
    }
    const auto boundary = [&](std::uint32_t a, std::uint32_t b) { return uses[Edge(a, b).key()] == 1; };
    std::map<std::uint64_t, std::uint32_t> slides;  // (v, otro extremo) -> vertice nuevo
    std::vector<std::vector<std::uint32_t>> corners(faces.size());
    for (std::size_t k = 0; k < faces.size(); ++k) {
        const int fi = faces[k];
        const Face& f = m.faces[static_cast<std::size_t>(fi)];
        const Vec3 normal = m.faceNormal(fi);
        const std::size_t n = f.v.size();
        corners[k].resize(n);
        for (std::size_t i = 0; i < n; ++i) {
            const std::uint32_t p = f.v[(i + n - 1) % n], v = f.v[i], q = f.v[(i + 1) % n];
            const bool bp = boundary(p, v), bn = boundary(v, q);
            const Vec3& P = m.positions[p];
            const Vec3& V = m.positions[v];
            const Vec3& Q = m.positions[q];
            if (bp && bn) {
                corners[k][i] = static_cast<std::uint32_t>(m.addVertex(insetCorner(P, V, Q, normal, amount)));
            } else if (bp || bn) {
                // Se desliza por la arista interior, a `amount` de la del borde.
                const std::uint32_t along = bp ? q : p;
                const Vec3 slide = safeNormalize(m.positions[along] - V);
                const Vec3 edge = safeNormalize(bp ? V - P : Q - V);
                const float sin_t = std::max(core::length(core::cross(slide, edge)), 0.15f);
                const std::uint64_t key = directed(v, along);
                const auto it = slides.find(key);
                if (it != slides.end()) {
                    corners[k][i] = it->second;
                } else {
                    corners[k][i] = static_cast<std::uint32_t>(m.addVertex(V + slide * (amount / sin_t)));
                    slides[key] = corners[k][i];
                }
            } else {
                corners[k][i] = v;
            }
        }
    }
    for (std::size_t k = 0; k < faces.size(); ++k) {
        const int fi = faces[k];
        const Face original = m.faces[static_cast<std::size_t>(fi)];
        const std::size_t n = original.v.size();
        for (std::size_t i = 0; i < n; ++i) {
            const std::size_t j = (i + 1) % n;
            const std::uint32_t a = original.v[i], b = original.v[j];
            if (!boundary(a, b)) continue;
            m.faces.push_back(childFace(original, {a, b, corners[k][j], corners[k][i]}));
        }
        m.faces[static_cast<std::size_t>(fi)].v = corners[k];
    }
    return faces;
}

std::vector<int> bevelEdges(PolyMesh& m, const std::vector<Edge>& edges, float amount) {
    const auto faces_of = edgeFaces(m);
    std::unordered_set<std::uint64_t> selected;
    for (const Edge& e : edges) {
        const auto it = faces_of.find(e.key());
        if (it != faces_of.end() && it->second.size() == 2) selected.insert(e.key());
    }
    if (selected.empty() || amount <= 0.0f) return {};
    // Que no se pase de las aristas vecinas.
    float limit = 1e30f;
    for (const Face& f : m.faces) {
        const std::size_t n = f.v.size();
        for (std::size_t i = 0; i < n; ++i) {
            const std::uint32_t a = f.v[i], b = f.v[(i + 1) % n];
            if (selected.contains(Edge(a, b).key())) continue;
            const bool touches = selected.contains(Edge(f.v[(i + n - 1) % n], a).key()) ||
                                 selected.contains(Edge(b, f.v[(i + 2) % n]).key());
            if (touches) limit = std::min(limit, core::length(m.positions[a] - m.positions[b]) * 0.49f);
        }
    }
    amount = std::min(amount, limit);
    const auto is_sel = [&](std::uint32_t a, std::uint32_t b) { return selected.contains(Edge(a, b).key()); };
    const std::size_t first_new = m.positions.size();

    // 1) Vertices que se deslizan por aristas no elegidas (compartidos por las dos caras de esa arista).
    std::map<std::uint64_t, std::uint32_t> slides;
    const auto slide = [&](std::uint32_t v, std::uint32_t toward) {
        const std::uint64_t key = directed(v, toward);
        const auto it = slides.find(key);
        if (it != slides.end()) return it->second;
        const Vec3 d = safeNormalize(m.positions[toward] - m.positions[v]);
        const auto n = static_cast<std::uint32_t>(m.addVertex(m.positions[v] + d * amount));
        slides[key] = n;
        return n;
    };
    const std::size_t face_count = m.faces.size();
    std::vector<std::vector<std::uint32_t>> replace(face_count);
    // Esquina nueva de cada cara para cada vertice de una arista elegida.
    std::vector<std::map<std::uint32_t, std::uint32_t>> corner_of(face_count);
    for (std::size_t fi = 0; fi < face_count; ++fi) {
        const Face& f = m.faces[fi];
        const std::size_t n = f.v.size();
        for (std::size_t i = 0; i < n; ++i) {
            const std::uint32_t p = f.v[(i + n - 1) % n], v = f.v[i], q = f.v[(i + 1) % n];
            const bool sp = is_sel(p, v), sn = is_sel(v, q);
            if (sp && sn) {
                corner_of[fi][v] = static_cast<std::uint32_t>(
                    m.addVertex(insetCorner(m.positions[p], m.positions[v], m.positions[q], m.faceNormal(static_cast<int>(fi)), amount)));
            } else if (sn) {
                corner_of[fi][v] = slide(v, p);
            } else if (sp) {
                corner_of[fi][v] = slide(v, q);
            }
        }
    }
    // 2) Caras con sus esquinas nuevas.
    for (std::size_t fi = 0; fi < face_count; ++fi) {
        const Face& f = m.faces[fi];
        const std::size_t n = f.v.size();
        std::vector<std::uint32_t> out;
        for (std::size_t i = 0; i < n; ++i) {
            const std::uint32_t p = f.v[(i + n - 1) % n], v = f.v[i], q = f.v[(i + 1) % n];
            const auto own = corner_of[fi].find(v);
            if (own != corner_of[fi].end()) {
                out.push_back(own->second);
                continue;
            }
            const auto before = slides.find(directed(v, p));
            const auto after = slides.find(directed(v, q));
            if (before != slides.end()) out.push_back(before->second);
            if (before == slides.end() || after == slides.end()) out.push_back(v);
            if (after != slides.end()) out.push_back(after->second);
        }
        replace[fi] = std::move(out);
    }
    // 3) La cara del bisel por cada arista: [b1, a1, a2, b2].
    std::vector<Face> bevel_faces;
    for (const std::uint64_t key : selected) {
        const Edge e(static_cast<std::uint32_t>(key >> 32), static_cast<std::uint32_t>(key & 0xffffffffu));
        const std::vector<int>& two = faces_of.at(key);
        int f1 = two[0], f2 = two[1];
        std::uint32_t a = e.a, b = e.b;
        if (!hasDirected(m.faces[static_cast<std::size_t>(f1)], a, b)) std::swap(a, b);
        if (!hasDirected(m.faces[static_cast<std::size_t>(f1)], a, b)) std::swap(f1, f2);
        if (!hasDirected(m.faces[static_cast<std::size_t>(f1)], a, b)) continue;
        const std::uint32_t a1 = corner_of[static_cast<std::size_t>(f1)][a], b1 = corner_of[static_cast<std::size_t>(f1)][b];
        const std::uint32_t a2 = corner_of[static_cast<std::size_t>(f2)][a], b2 = corner_of[static_cast<std::size_t>(f2)][b];
        Face bevel = childFace(m.faces[static_cast<std::size_t>(f1)], {b1, a1, a2, b2});
        bevel.smoothing = 0;
        bevel_faces.push_back(std::move(bevel));
    }
    for (std::size_t fi = 0; fi < face_count; ++fi) {
        m.faces[fi].v = std::move(replace[fi]);
        if (m.faces[fi].uvs.mode == UvMode::Manual) {
            m.faces[fi].uv.clear();
            m.faces[fi].uvs.mode = UvMode::Box;
        }
    }
    std::vector<int> out;
    for (Face& f : bevel_faces) {
        m.faces.push_back(std::move(f));
        out.push_back(static_cast<int>(m.faces.size()) - 1);
    }
    m.cleanup();
    // 4) Esquinas donde se juntan tres o mas biseles: el agujero de vertices nuevos se tapa.
    for (const std::vector<std::uint32_t>& loop : borderLoops(m)) {
        bool all_new = true;
        for (const std::uint32_t v : loop) all_new = all_new && v >= first_new;
        if (!all_new) continue;
        Face corner;
        corner.v = loop;
        m.faces.push_back(std::move(corner));
        out.push_back(static_cast<int>(m.faces.size()) - 1);
    }
    // Indices de caras: cleanup pudo quitar degeneradas antes de las nuevas.
    m.removeUnusedVertices();
    std::vector<int> valid;
    for (const int f : out) {
        if (static_cast<std::size_t>(f) < m.faces.size()) valid.push_back(f);
    }
    return valid;
}

// -----------------------------------------------------------------------------
// Subdividir y conectar
// -----------------------------------------------------------------------------

std::vector<int> subdivideFaces(PolyMesh& m, const std::vector<int>& faces_in) {
    std::set<int> faces;
    for (const int f : faces_in) {
        if (f >= 0 && static_cast<std::size_t>(f) < m.faces.size()) faces.insert(f);
    }
    std::unordered_map<std::uint64_t, std::uint32_t> mids;
    for (const int fi : faces) {
        const Face& f = m.faces[static_cast<std::size_t>(fi)];
        for (std::size_t i = 0; i < f.v.size(); ++i) {
            const Edge e(f.v[i], f.v[(i + 1) % f.v.size()]);
            if (!mids.contains(e.key())) mids[e.key()] = static_cast<std::uint32_t>(m.addVertex((m.positions[e.a] + m.positions[e.b]) * 0.5f));
        }
    }
    std::vector<int> out;
    const std::size_t count = m.faces.size();
    for (std::size_t fi = 0; fi < count; ++fi) {
        Face& f = m.faces[fi];
        const std::size_t n = f.v.size();
        const bool manual = f.uvs.mode == UvMode::Manual && f.uv.size() == n;
        if (!faces.contains(static_cast<int>(fi))) {
            // Vecina: recibe los puntos medios de sus aristas.
            std::vector<std::uint32_t> v;
            std::vector<Vec2> uv;
            for (std::size_t i = 0; i < n; ++i) {
                v.push_back(f.v[i]);
                if (manual) uv.push_back(f.uv[i]);
                const auto it = mids.find(Edge(f.v[i], f.v[(i + 1) % n]).key());
                if (it != mids.end()) {
                    v.push_back(it->second);
                    if (manual) {
                        const Vec2 a = f.uv[i], b = f.uv[(i + 1) % n];
                        uv.push_back(Vec2{(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f});
                    }
                }
            }
            f.v = std::move(v);
            f.uv = std::move(uv);
            continue;
        }
        const Face original = f;
        const auto center = static_cast<std::uint32_t>(m.addVertex(m.faceCenter(static_cast<int>(fi))));
        Vec2 uv_center{};
        if (manual) {
            for (const Vec2& t : original.uv) uv_center = Vec2{uv_center.x + t.x / static_cast<float>(n), uv_center.y + t.y / static_cast<float>(n)};
        }
        for (std::size_t i = 0; i < n; ++i) {
            const std::size_t prev = (i + n - 1) % n, next = (i + 1) % n;
            Face child = original;
            child.v = {original.v[i], mids[Edge(original.v[i], original.v[next]).key()], center,
                       mids[Edge(original.v[prev], original.v[i]).key()]};
            child.uv.clear();
            if (manual) {
                const Vec2 a = original.uv[i], b = original.uv[next], c = original.uv[prev];
                child.uv = {a, Vec2{(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f}, uv_center, Vec2{(a.x + c.x) * 0.5f, (a.y + c.y) * 0.5f}};
            }
            if (i == 0) {
                m.faces[fi] = std::move(child);
                out.push_back(static_cast<int>(fi));
            } else {
                m.faces.push_back(std::move(child));
                out.push_back(static_cast<int>(m.faces.size()) - 1);
            }
        }
    }
    return out;
}

std::vector<std::uint32_t> subdivideEdges(PolyMesh& m, const std::vector<Edge>& edges, int cuts) {
    cuts = std::clamp(cuts, 1, 64);
    std::vector<std::uint32_t> out;
    std::set<Edge> done;
    for (const Edge& e : edges) {
        if (!done.insert(e).second || e.a >= m.positions.size() || e.b >= m.positions.size()) continue;
        std::vector<std::uint32_t> points;
        for (int k = 1; k <= cuts; ++k) {
            const float t = static_cast<float>(k) / static_cast<float>(cuts + 1);
            points.push_back(static_cast<std::uint32_t>(m.addVertex(core::lerp(m.positions[e.a], m.positions[e.b], t))));
        }
        insertOnEdge(m, e.a, e.b, points);
        out.insert(out.end(), points.begin(), points.end());
    }
    return out;
}

std::vector<Edge> connectVertices(PolyMesh& m, const std::vector<std::uint32_t>& vertices) {
    const std::set<std::uint32_t> set(vertices.begin(), vertices.end());
    std::vector<Edge> out;
    const std::size_t count = m.faces.size();
    for (std::size_t fi = 0; fi < count; ++fi) {
        const Face f = m.faces[fi];
        std::vector<std::size_t> corners;
        for (std::size_t i = 0; i < f.v.size(); ++i) {
            if (set.contains(f.v[i])) corners.push_back(i);
        }
        if (corners.size() < 2) continue;
        if (corners.size() == 2) {
            if (splitFace(m, static_cast<int>(fi), corners[0], corners[1]) >= 0) out.push_back(Edge(f.v[corners[0]], f.v[corners[1]]));
            continue;
        }
        // Tres o mas: poligono de dentro con las elegidas y los trozos de fuera.
        const std::size_t n = f.v.size();
        std::vector<std::uint32_t> inner;
        for (const std::size_t c : corners) inner.push_back(f.v[c]);
        std::vector<Face> pieces;
        for (std::size_t k = 0; k < corners.size(); ++k) {
            const std::size_t a = corners[k], b = corners[(k + 1) % corners.size()];
            std::vector<std::uint32_t> piece;
            for (std::size_t i = a;; i = (i + 1) % n) {
                piece.push_back(f.v[i]);
                if (i == b) break;
            }
            if (piece.size() >= 3) {
                pieces.push_back(childFace(f, piece));
                out.push_back(Edge(f.v[a], f.v[b]));
            }
        }
        m.faces[fi] = childFace(f, inner);
        for (Face& p : pieces) m.faces.push_back(std::move(p));
    }
    return out;
}

std::vector<Edge> connectEdges(PolyMesh& m, const std::vector<Edge>& edges) {
    std::vector<std::uint32_t> mids;
    std::set<Edge> done;
    for (const Edge& e : edges) {
        if (!done.insert(e).second) continue;
        const auto mid = static_cast<std::uint32_t>(m.addVertex((m.positions[e.a] + m.positions[e.b]) * 0.5f));
        insertOnEdge(m, e.a, e.b, {mid});
        mids.push_back(mid);
    }
    // Caras con dos puntos medios: corte; con mas: un centro.
    const std::set<std::uint32_t> set(mids.begin(), mids.end());
    std::vector<Edge> out;
    const std::size_t count = m.faces.size();
    for (std::size_t fi = 0; fi < count; ++fi) {
        const Face f = m.faces[fi];
        std::vector<std::size_t> corners;
        for (std::size_t i = 0; i < f.v.size(); ++i) {
            if (set.contains(f.v[i])) corners.push_back(i);
        }
        if (corners.size() == 2) {
            if (splitFace(m, static_cast<int>(fi), corners[0], corners[1]) >= 0) out.push_back(Edge(f.v[corners[0]], f.v[corners[1]]));
        } else if (corners.size() > 2) {
            const auto center = static_cast<std::uint32_t>(m.addVertex(m.faceCenter(static_cast<int>(fi))));
            const std::size_t n = f.v.size();
            std::vector<Face> pieces;
            for (std::size_t k = 0; k < corners.size(); ++k) {
                const std::size_t a = corners[k], b = corners[(k + 1) % corners.size()];
                std::vector<std::uint32_t> piece;
                for (std::size_t i = a;; i = (i + 1) % n) {
                    piece.push_back(f.v[i]);
                    if (i == b) break;
                }
                piece.push_back(center);
                pieces.push_back(childFace(f, piece));
                out.push_back(Edge(f.v[a], center));
            }
            m.faces[fi] = std::move(pieces.front());
            for (std::size_t k = 1; k < pieces.size(); ++k) m.faces.push_back(std::move(pieces[k]));
        }
    }
    return out;
}

std::vector<Edge> edgeRing(const PolyMesh& m, const Edge& start) {
    const auto faces_of = edgeFaces(m);
    std::vector<Edge> ring{start};
    std::set<Edge> seen{start};
    const auto first = faces_of.find(start.key());
    if (first == faces_of.end()) return ring;
    for (const int initial : first->second) {
        Edge current = start;
        int face = initial;
        while (true) {
            const Face& f = m.faces[static_cast<std::size_t>(face)];
            if (f.v.size() != 4) break;
            std::size_t i = 0;
            for (; i < 4; ++i) {
                if (Edge(f.v[i], f.v[(i + 1) % 4]) == current) break;
            }
            if (i == 4) break;
            const Edge opposite(f.v[(i + 2) % 4], f.v[(i + 3) % 4]);
            if (!seen.insert(opposite).second) break;
            ring.push_back(opposite);
            const std::vector<int>& next = faces_of.at(opposite.key());
            int other = -1;
            for (const int g : next) {
                if (g != face) other = g;
            }
            if (other < 0 || next.size() != 2) break;
            current = opposite;
            face = other;
        }
    }
    return ring;
}

std::vector<Edge> edgeLoop(const PolyMesh& m, const Edge& start) {
    const auto faces_of = edgeFaces(m);
    std::vector<Edge> loop{start};
    std::set<Edge> seen{start};
    for (const bool forward : {true, false}) {
        Edge current = start;
        std::uint32_t pivot = forward ? start.b : start.a;
        while (true) {
            const std::vector<std::uint32_t> around = neighborsOf(m, pivot);
            if (around.size() != 4) break;  // solo por vertices de 4 aristas (rejilla de quads)
            const auto cur = faces_of.find(current.key());
            if (cur == faces_of.end()) break;
            const std::set<int> current_faces(cur->second.begin(), cur->second.end());
            Edge next{};
            bool found = false;
            for (const std::uint32_t q : around) {
                const Edge candidate(pivot, q);
                if (candidate == current) continue;
                const auto it = faces_of.find(candidate.key());
                if (it == faces_of.end()) continue;
                bool shares = false;
                for (const int g : it->second) shares = shares || current_faces.contains(g);
                if (!shares) {
                    next = candidate;
                    found = true;
                    break;
                }
            }
            if (!found || !seen.insert(next).second) break;
            loop.push_back(next);
            pivot = next.a == pivot ? next.b : next.a;
            current = next;
        }
    }
    return loop;
}

std::vector<Edge> insertEdgeLoop(PolyMesh& m, const Edge& start, float t) {
    // El anillo con sentido: al cruzar un quad a,b,c,d de (a,b) a la opuesta,
    // la paralela en el mismo sentido es (d,c).
    const auto faces_of = edgeFaces(m);
    struct Step {
        std::uint32_t a, b;  // arista orientada
    };
    std::vector<Step> ring{{start.a, start.b}};
    std::set<Edge> seen{start};
    std::vector<int> quads;
    const auto first = faces_of.find(start.key());
    if (first == faces_of.end()) return {};
    bool reversed_side = false;
    for (const int initial : first->second) {
        std::uint32_t ca = start.a, cb = start.b;
        int face = initial;
        std::vector<Step> side;
        std::vector<int> side_quads;
        while (true) {
            const Face& f = m.faces[static_cast<std::size_t>(face)];
            if (f.v.size() != 4) break;
            std::size_t i = 0;
            for (; i < 4; ++i) {
                const std::uint32_t x = f.v[i], y = f.v[(i + 1) % 4];
                if ((x == ca && y == cb) || (x == cb && y == ca)) break;
            }
            if (i == 4) break;
            const bool same = f.v[i] == ca;
            const std::uint32_t c = f.v[(i + 2) % 4], d = f.v[(i + 3) % 4];
            // a->b en el quad (x=f.v[i], y=f.v[i+1]); la paralela es d->c.
            const std::uint32_t na = same ? d : c, nb = same ? c : d;
            side_quads.push_back(face);
            const Edge opposite(na, nb);
            if (!seen.insert(opposite).second) break;  // anillo cerrado
            side.push_back({na, nb});
            const auto next = faces_of.find(opposite.key());
            int other = -1;
            if (next != faces_of.end() && next->second.size() == 2) {
                for (const int g : next->second) {
                    if (g != face) other = g;
                }
            }
            if (other < 0) break;
            ca = na;
            cb = nb;
            face = other;
        }
        if (!reversed_side) {
            ring.insert(ring.end(), side.begin(), side.end());
        } else {
            ring.insert(ring.begin(), side.rbegin(), side.rend());
        }
        quads.insert(quads.end(), side_quads.begin(), side_quads.end());
        reversed_side = true;
    }
    std::sort(quads.begin(), quads.end());
    quads.erase(std::unique(quads.begin(), quads.end()), quads.end());
    std::set<std::uint32_t> points;
    for (const Step& s : ring) {
        const auto p = static_cast<std::uint32_t>(m.addVertex(core::lerp(m.positions[s.a], m.positions[s.b], std::clamp(t, 0.01f, 0.99f))));
        insertOnEdge(m, s.a, s.b, {p});
        points.insert(p);
    }
    std::vector<Edge> out;
    for (const int q : quads) {
        const Face f = m.faces[static_cast<std::size_t>(q)];
        std::vector<std::size_t> corners;
        for (std::size_t i = 0; i < f.v.size(); ++i) {
            if (points.contains(f.v[i])) corners.push_back(i);
        }
        if (corners.size() == 2 && splitFace(m, q, corners[0], corners[1]) >= 0) {
            out.push_back(Edge(f.v[corners[0]], f.v[corners[1]]));
        }
    }
    return out;
}

int bridgeEdges(PolyMesh& m, const Edge& a, const Edge& b) {
    const auto faces_of = edgeFaces(m);
    const auto fa = faces_of.find(a.key());
    const auto fb = faces_of.find(b.key());
    if (fa == faces_of.end() || fb == faces_of.end() || fa->second.size() != 1 || fb->second.size() != 1 || a == b) return -1;
    const Face& face_a = m.faces[static_cast<std::size_t>(fa->second.front())];
    const Face& face_b = m.faces[static_cast<std::size_t>(fb->second.front())];
    const std::uint32_t x = hasDirected(face_a, a.a, a.b) ? a.a : a.b, y = x == a.a ? a.b : a.a;
    const std::uint32_t z = hasDirected(face_b, b.a, b.b) ? b.a : b.b, w = z == b.a ? b.b : b.a;
    std::vector<std::uint32_t> v{y, x};
    if (w != x) v.push_back(w);
    if (z != y && z != w) v.push_back(z);
    if (v.size() < 3) return -1;
    Face f = childFace(face_a, v);
    m.faces.push_back(std::move(f));
    return static_cast<int>(m.faces.size()) - 1;
}

std::vector<int> fillHoles(PolyMesh& m, const std::vector<Edge>& edges) {
    std::set<Edge> wanted(edges.begin(), edges.end());
    std::vector<int> out;
    for (const std::vector<std::uint32_t>& loop : borderLoops(m)) {
        if (!wanted.empty()) {
            bool touches = false;
            for (std::size_t i = 0; i < loop.size() && !touches; ++i) touches = wanted.contains(Edge(loop[i], loop[(i + 1) % loop.size()]));
            if (!touches) continue;
        }
        Face f;
        f.v = loop;
        m.faces.push_back(std::move(f));
        out.push_back(static_cast<int>(m.faces.size()) - 1);
    }
    return out;
}

int mergeFaces(PolyMesh& m, const std::vector<int>& faces_in) {
    std::vector<int> faces;
    for (const int f : faces_in) {
        if (f >= 0 && static_cast<std::size_t>(f) < m.faces.size()) faces.push_back(f);
    }
    std::sort(faces.begin(), faces.end());
    faces.erase(std::unique(faces.begin(), faces.end()), faces.end());
    if (faces.size() < 2) return faces.empty() ? -1 : faces.front();
    std::unordered_map<std::uint64_t, int> uses;
    for (const int fi : faces) {
        const Face& f = m.faces[static_cast<std::size_t>(fi)];
        for (std::size_t i = 0; i < f.v.size(); ++i) ++uses[Edge(f.v[i], f.v[(i + 1) % f.v.size()]).key()];
    }
    std::unordered_map<std::uint32_t, std::uint32_t> next;
    std::size_t boundary = 0;
    for (const int fi : faces) {
        const Face& f = m.faces[static_cast<std::size_t>(fi)];
        for (std::size_t i = 0; i < f.v.size(); ++i) {
            const std::uint32_t a = f.v[i], b = f.v[(i + 1) % f.v.size()];
            if (uses[Edge(a, b).key()] != 1) continue;
            if (next.contains(a)) return -1;  // contorno que se toca a si mismo
            next[a] = b;
            ++boundary;
        }
    }
    if (boundary < 3) return -1;
    std::vector<std::uint32_t> loop;
    const std::uint32_t start = next.begin()->first;
    std::uint32_t current = start;
    do {
        loop.push_back(current);
        const auto it = next.find(current);
        if (it == next.end()) return -1;
        current = it->second;
    } while (current != start && loop.size() <= boundary);
    if (loop.size() != boundary) return -1;  // varios bucles (agujeros): no se une
    Face merged = childFace(m.faces[static_cast<std::size_t>(faces.front())], loop);
    merged.uvs = m.faces[static_cast<std::size_t>(faces.front())].uvs;
    if (merged.uvs.mode == UvMode::Manual) merged.uvs.mode = UvMode::Box;
    for (auto it = faces.rbegin(); it != faces.rend(); ++it) m.faces.erase(m.faces.begin() + *it);
    m.faces.push_back(std::move(merged));
    m.removeUnusedVertices();
    return static_cast<int>(m.faces.size()) - 1;
}

// -----------------------------------------------------------------------------
// Suavizar, relajar, espejo, pivote...
// -----------------------------------------------------------------------------

void subdivideSmooth(PolyMesh& m, int levels) {
    for (int level = 0; level < std::clamp(levels, 0, 4); ++level) {
        const auto faces_of = edgeFaces(m);
        const std::size_t nv = m.positions.size();
        std::vector<Vec3> face_points(m.faces.size());
        for (std::size_t fi = 0; fi < m.faces.size(); ++fi) face_points[fi] = m.faceCenter(static_cast<int>(fi));
        PolyMesh out;
        out.positions.resize(nv);
        // Vertices originales.
        std::vector<Vec3> face_sum(nv), edge_sum(nv);
        std::vector<int> face_n(nv, 0), edge_n(nv, 0);
        std::vector<std::vector<std::uint32_t>> border_neighbors(nv);
        for (std::size_t fi = 0; fi < m.faces.size(); ++fi) {
            for (const std::uint32_t v : m.faces[fi].v) {
                face_sum[v] += face_points[fi];
                ++face_n[v];
            }
        }
        for (const auto& [key, list] : faces_of) {
            const auto a = static_cast<std::uint32_t>(key >> 32), b = static_cast<std::uint32_t>(key & 0xffffffffu);
            const Vec3 mid = (m.positions[a] + m.positions[b]) * 0.5f;
            edge_sum[a] += mid;
            edge_sum[b] += mid;
            ++edge_n[a];
            ++edge_n[b];
            if (list.size() == 1) {
                border_neighbors[a].push_back(b);
                border_neighbors[b].push_back(a);
            }
        }
        for (std::size_t v = 0; v < nv; ++v) {
            const Vec3& p = m.positions[v];
            if (!border_neighbors[v].empty()) {
                // Borde: solo con sus vecinos del borde (las esquinas se quedan).
                if (border_neighbors[v].size() == 2) {
                    out.positions[v] = (p * 6.0f + m.positions[border_neighbors[v][0]] + m.positions[border_neighbors[v][1]]) * 0.125f;
                } else {
                    out.positions[v] = p;
                }
                continue;
            }
            if (face_n[v] == 0 || edge_n[v] < 3) {
                out.positions[v] = p;
                continue;
            }
            const float n = static_cast<float>(edge_n[v]);
            const Vec3 q = face_sum[v] * (1.0f / static_cast<float>(face_n[v]));
            const Vec3 r = edge_sum[v] * (1.0f / n);
            out.positions[v] = (q + r * 2.0f + p * (n - 3.0f)) * (1.0f / n);
        }
        // Puntos de arista y de cara.
        std::unordered_map<std::uint64_t, std::uint32_t> edge_points;
        for (const auto& [key, list] : faces_of) {
            const auto a = static_cast<std::uint32_t>(key >> 32), b = static_cast<std::uint32_t>(key & 0xffffffffu);
            Vec3 p = (m.positions[a] + m.positions[b]) * 0.5f;
            if (list.size() == 2) {
                p = (m.positions[a] + m.positions[b] + face_points[static_cast<std::size_t>(list[0])] +
                     face_points[static_cast<std::size_t>(list[1])]) * 0.25f;
            }
            edge_points[key] = static_cast<std::uint32_t>(out.addVertex(p));
        }
        for (std::size_t fi = 0; fi < m.faces.size(); ++fi) {
            const Face& f = m.faces[fi];
            const std::size_t n = f.v.size();
            const auto center = static_cast<std::uint32_t>(out.addVertex(face_points[fi]));
            const bool manual = f.uvs.mode == UvMode::Manual && f.uv.size() == n;
            Vec2 uv_center{};
            if (manual) {
                for (const Vec2& t : f.uv) uv_center = Vec2{uv_center.x + t.x / static_cast<float>(n), uv_center.y + t.y / static_cast<float>(n)};
            }
            for (std::size_t i = 0; i < n; ++i) {
                const std::size_t prev = (i + n - 1) % n, next = (i + 1) % n;
                Face child = f;
                child.v = {f.v[i], edge_points[Edge(f.v[i], f.v[next]).key()], center, edge_points[Edge(f.v[prev], f.v[i]).key()]};
                child.uv.clear();
                if (manual) {
                    const Vec2 a = f.uv[i], b = f.uv[next], c = f.uv[prev];
                    child.uv = {a, Vec2{(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f}, uv_center, Vec2{(a.x + c.x) * 0.5f, (a.y + c.y) * 0.5f}};
                }
                out.faces.push_back(std::move(child));
            }
        }
        m = std::move(out);
    }
}

void relax(PolyMesh& m, const std::vector<std::uint32_t>& vertices_in, float amount, int iterations, bool keep_border) {
    std::vector<std::uint32_t> vertices = vertices_in;
    if (vertices.empty()) {
        vertices.resize(m.positions.size());
        for (std::size_t i = 0; i < vertices.size(); ++i) vertices[i] = static_cast<std::uint32_t>(i);
    }
    std::set<std::uint32_t> border;
    if (keep_border) {
        for (const Edge& e : m.borderEdges()) {
            border.insert(e.a);
            border.insert(e.b);
        }
    }
    std::vector<std::vector<std::uint32_t>> neighbors(m.positions.size());
    for (const Edge& e : m.edges()) {
        neighbors[e.a].push_back(e.b);
        neighbors[e.b].push_back(e.a);
    }
    for (int it = 0; it < std::clamp(iterations, 1, 100); ++it) {
        std::vector<Vec3> next = m.positions;
        for (const std::uint32_t v : vertices) {
            if (v >= m.positions.size() || border.contains(v) || neighbors[v].empty()) continue;
            Vec3 avg{};
            for (const std::uint32_t q : neighbors[v]) avg += m.positions[q];
            avg = avg * (1.0f / static_cast<float>(neighbors[v].size()));
            next[v] = core::lerp(m.positions[v], avg, std::clamp(amount, 0.0f, 1.0f));
        }
        m.positions = std::move(next);
    }
}

void mirror(PolyMesh& m, int axis, bool keep, float plane) {
    axis = std::clamp(axis, 0, 2);
    const auto reflect = [&](Vec3 p) {
        float& c = axis == 0 ? p.x : (axis == 1 ? p.y : p.z);
        c = 2.0f * plane - c;
        return p;
    };
    std::vector<int> all(m.faces.size());
    for (std::size_t i = 0; i < all.size(); ++i) all[i] = static_cast<int>(i);
    if (!keep) {
        for (Vec3& p : m.positions) p = reflect(p);
        flipFaces(m, all);
        return;
    }
    PolyMesh copy = m;
    for (Vec3& p : copy.positions) p = reflect(p);
    flipFaces(copy, all);
    append(m, copy);
    // Costura: los del plano se sueldan con su reflejo.
    std::vector<std::uint32_t> seam;
    for (std::size_t i = 0; i < m.positions.size(); ++i) {
        const Vec3& p = m.positions[i];
        const float c = axis == 0 ? p.x : (axis == 1 ? p.y : p.z);
        if (std::abs(c - plane) < 1e-4f) seam.push_back(static_cast<std::uint32_t>(i));
    }
    if (!seam.empty()) weldVertices(m, seam, 1e-4f);
    // Caras del plano de simetria (dos iguales al reves): fuera.
    std::map<std::vector<std::uint32_t>, std::vector<int>> by_set;
    for (std::size_t fi = 0; fi < m.faces.size(); ++fi) {
        std::vector<std::uint32_t> key = m.faces[fi].v;
        std::sort(key.begin(), key.end());
        by_set[key].push_back(static_cast<int>(fi));
    }
    std::vector<int> doubled;
    for (const auto& [key, list] : by_set) {
        if (list.size() >= 2) doubled.insert(doubled.end(), list.begin(), list.end());
    }
    if (!doubled.empty()) deleteFaces(m, doubled);
}

Vec3 centerPivot(PolyMesh& m, bool bottom) {
    Vec3 lo{}, hi{};
    m.bounds(lo, hi);
    Vec3 center = (lo + hi) * 0.5f;
    if (bottom) center.y = lo.y;
    for (Vec3& p : m.positions) p -= center;
    return center;
}

void snapToGrid(PolyMesh& m, const std::vector<std::uint32_t>& vertices, float grid) {
    if (grid <= 0.0f) return;
    const auto snap = [&](float x) { return std::round(x / grid) * grid; };
    for (const std::uint32_t v : vertices) {
        if (v >= m.positions.size()) continue;
        Vec3& p = m.positions[v];
        p = Vec3{snap(p.x), snap(p.y), snap(p.z)};
    }
}

void randomize(PolyMesh& m, const std::vector<std::uint32_t>& vertices_in, float amount, std::uint32_t seed) {
    std::vector<std::uint32_t> vertices = vertices_in;
    if (vertices.empty()) {
        vertices.resize(m.positions.size());
        for (std::size_t i = 0; i < vertices.size(); ++i) vertices[i] = static_cast<std::uint32_t>(i);
    }
    std::vector<Vec3> normals(m.positions.size());
    for (std::size_t fi = 0; fi < m.faces.size(); ++fi) {
        const Vec3 n = m.faceNormal(static_cast<int>(fi));
        for (const std::uint32_t v : m.faces[fi].v) normals[v] += n;
    }
    std::uint32_t state = seed * 747796405u + 2891336453u;
    const auto random = [&]() {
        state = state * 747796405u + 2891336453u;
        std::uint32_t word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
        word = (word >> 22u) ^ word;
        return static_cast<float>(word) / 4294967295.0f * 2.0f - 1.0f;
    };
    for (const std::uint32_t v : vertices) {
        if (v < m.positions.size()) m.positions[v] += safeNormalize(normals[v]) * (random() * amount);
    }
}

// -----------------------------------------------------------------------------
// Seleccion
// -----------------------------------------------------------------------------

std::vector<int> growFaces(const PolyMesh& m, const std::vector<int>& faces) {
    const auto faces_of = edgeFaces(m);
    std::set<int> out(faces.begin(), faces.end());
    for (const int fi : faces) {
        if (fi < 0 || static_cast<std::size_t>(fi) >= m.faces.size()) continue;
        const Face& f = m.faces[static_cast<std::size_t>(fi)];
        for (std::size_t i = 0; i < f.v.size(); ++i) {
            for (const int g : faces_of.at(Edge(f.v[i], f.v[(i + 1) % f.v.size()]).key())) out.insert(g);
        }
    }
    return {out.begin(), out.end()};
}

std::vector<int> shrinkFaces(const PolyMesh& m, const std::vector<int>& faces) {
    const auto faces_of = edgeFaces(m);
    const std::set<int> set(faces.begin(), faces.end());
    std::vector<int> out;
    for (const int fi : set) {
        if (fi < 0 || static_cast<std::size_t>(fi) >= m.faces.size()) continue;
        const Face& f = m.faces[static_cast<std::size_t>(fi)];
        bool inner = true;
        for (std::size_t i = 0; i < f.v.size() && inner; ++i) {
            const std::vector<int>& list = faces_of.at(Edge(f.v[i], f.v[(i + 1) % f.v.size()]).key());
            for (const int g : list) inner = inner && set.contains(g);
            inner = inner && list.size() == 2;
        }
        if (inner) out.push_back(fi);
    }
    return out;
}

std::vector<int> linkedFaces(const PolyMesh& m, const std::vector<int>& faces) {
    const auto faces_of = edgeFaces(m);
    std::set<int> out;
    std::queue<int> queue;
    for (const int f : faces) {
        if (f >= 0 && static_cast<std::size_t>(f) < m.faces.size() && out.insert(f).second) queue.push(f);
    }
    while (!queue.empty()) {
        const int fi = queue.front();
        queue.pop();
        const Face& f = m.faces[static_cast<std::size_t>(fi)];
        for (std::size_t i = 0; i < f.v.size(); ++i) {
            for (const int g : faces_of.at(Edge(f.v[i], f.v[(i + 1) % f.v.size()]).key())) {
                if (out.insert(g).second) queue.push(g);
            }
        }
    }
    return {out.begin(), out.end()};
}

std::vector<int> facesByAngle(const PolyMesh& m, int seed, float max_angle) {
    if (seed < 0 || static_cast<std::size_t>(seed) >= m.faces.size()) return {};
    const auto faces_of = edgeFaces(m);
    const Vec3 reference = m.faceNormal(seed);
    const float limit = std::cos(core::radians(std::clamp(max_angle, 0.0f, 180.0f))) - 1e-5f;
    std::set<int> out{seed};
    std::queue<int> queue;
    queue.push(seed);
    while (!queue.empty()) {
        const int fi = queue.front();
        queue.pop();
        const Face& f = m.faces[static_cast<std::size_t>(fi)];
        for (std::size_t i = 0; i < f.v.size(); ++i) {
            for (const int g : faces_of.at(Edge(f.v[i], f.v[(i + 1) % f.v.size()]).key())) {
                if (out.contains(g) || core::dot(m.faceNormal(g), reference) < limit) continue;
                out.insert(g);
                queue.push(g);
            }
        }
    }
    return {out.begin(), out.end()};
}

std::vector<int> facesWithMaterial(const PolyMesh& m, int material) {
    std::vector<int> out;
    for (std::size_t fi = 0; fi < m.faces.size(); ++fi) {
        if (m.faces[fi].material == material) out.push_back(static_cast<int>(fi));
    }
    return out;
}

// -----------------------------------------------------------------------------
// Importar y exportar
// -----------------------------------------------------------------------------

PolyMesh fromTriangles(const std::vector<Vec3>& positions, const std::vector<std::uint32_t>& indices,
                       const std::vector<int>& materials, float quad_angle) {
    PolyMesh m;
    m.positions = positions;
    for (std::size_t t = 0; t + 2 < indices.size(); t += 3) {
        if (indices[t] >= positions.size() || indices[t + 1] >= positions.size() || indices[t + 2] >= positions.size()) continue;
        const int material = t / 3 < materials.size() ? materials[t / 3] : 0;
        m.addFace({indices[t], indices[t + 1], indices[t + 2]}, material);
    }
    Vec3 lo{}, hi{};
    m.bounds(lo, hi);
    const float size = std::max({hi.x - lo.x, hi.y - lo.y, hi.z - lo.z, 1e-3f});
    weldVertices(m, {}, size * 1e-6f);
    m.cleanup();
    if (quad_angle <= 0.0f) return m;
    // Parejas de triangulos coplanares -> quads convexos.
    const float limit = std::cos(core::radians(quad_angle));
    const auto faces_of = edgeFaces(m);
    std::vector<char> used(m.faces.size(), 0);
    std::vector<Face> out;
    for (std::size_t fi = 0; fi < m.faces.size(); ++fi) {
        if (used[fi]) continue;
        const Face& f = m.faces[fi];
        bool merged = false;
        if (f.v.size() == 3) {
            const Vec3 n = m.faceNormal(static_cast<int>(fi));
            // La arista mas larga primero (la diagonal del quad).
            std::size_t order[3] = {0, 1, 2};
            std::sort(order, order + 3, [&](std::size_t x, std::size_t y) {
                return core::length(m.positions[f.v[(x + 1) % 3]] - m.positions[f.v[x]]) >
                       core::length(m.positions[f.v[(y + 1) % 3]] - m.positions[f.v[y]]);
            });
            for (const std::size_t i : order) {
                const std::uint32_t a = f.v[i], b = f.v[(i + 1) % 3], c = f.v[(i + 2) % 3];
                const auto it = faces_of.find(Edge(a, b).key());
                if (it == faces_of.end() || it->second.size() != 2) continue;
                const int g = it->second[0] == static_cast<int>(fi) ? it->second[1] : it->second[0];
                const Face& other = m.faces[static_cast<std::size_t>(g)];
                if (used[static_cast<std::size_t>(g)] || other.v.size() != 3 || other.material != f.material) continue;
                if (core::dot(n, m.faceNormal(g)) < limit) continue;
                std::uint32_t d = other.v[0];
                for (const std::uint32_t q : other.v) {
                    if (q != a && q != b) d = q;
                }
                // a,b,c antihorario; la otra recorre b,a,d: el quad es a,d,b,c.
                const std::vector<std::uint32_t> quad{a, d, b, c};
                bool convex = true;
                for (std::size_t k = 0; k < 4 && convex; ++k) {
                    const Vec3& p0 = m.positions[quad[k]];
                    const Vec3& p1 = m.positions[quad[(k + 1) % 4]];
                    const Vec3& p2 = m.positions[quad[(k + 2) % 4]];
                    convex = core::dot(core::cross(p1 - p0, p2 - p1), n) > 1e-9f;
                }
                if (!convex) continue;
                Face q = f;
                q.v = quad;
                out.push_back(std::move(q));
                used[fi] = used[static_cast<std::size_t>(g)] = 1;
                merged = true;
                break;
            }
        }
        if (!merged) {
            used[fi] = 1;
            out.push_back(f);
        }
    }
    m.faces = std::move(out);
    return m;
}

std::string toObj(const PolyMesh& m, const std::string& name) {
    std::string out = "# Cramion: malla editable\no " + name + "\n";
    char line[160];
    for (const Vec3& p : m.positions) {
        std::snprintf(line, sizeof(line), "v %.6f %.6f %.6f\n", static_cast<double>(p.x), static_cast<double>(p.y),
                      static_cast<double>(p.z));
        out += line;
    }
    std::size_t vt = 1;
    std::string faces;
    int material = -1;
    for (std::size_t fi = 0; fi < m.faces.size(); ++fi) {
        const Face& f = m.faces[fi];
        const Vec3 n = m.faceNormal(static_cast<int>(fi));
        std::snprintf(line, sizeof(line), "vn %.5f %.5f %.5f\n", static_cast<double>(n.x), static_cast<double>(n.y),
                      static_cast<double>(n.z));
        out += line;
        for (std::size_t c = 0; c < f.v.size(); ++c) {
            const Vec2 uv = faceCornerUv(m, static_cast<int>(fi), static_cast<int>(c), Mat4::identity());
            std::snprintf(line, sizeof(line), "vt %.6f %.6f\n", static_cast<double>(uv.x), static_cast<double>(-uv.y));
            out += line;
        }
        if (f.material != material) {
            material = f.material;
            faces += "usemtl hueco" + std::to_string(material) + "\n";
        }
        faces += "f";
        for (std::size_t c = 0; c < f.v.size(); ++c) {
            faces += " " + std::to_string(f.v[c] + 1) + "/" + std::to_string(vt + c) + "/" + std::to_string(fi + 1);
        }
        faces += "\n";
        vt += f.v.size();
    }
    for (char& ch : out) {
        if (ch == ',') ch = '.';  // por si el locale usa coma decimal
    }
    return out + faces;
}

}  // namespace cramion::modeling::ops
