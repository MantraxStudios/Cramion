// Booleanas de mallas editables (union, resta, interseccion) con arboles BSP,
// el metodo de csg.js: cada malla parte a los poligonos de la otra por sus
// planos y se quedan los trozos de dentro o de fuera. Los arboles se
// recorren con pilas (sin recursion: mallas grandes no agotan la pila).

#include "CramionCore/modeling/EditableMesh.h"

#include "CramionCore/ecs/MathUtil.h"

#include <algorithm>
#include <vector>

namespace cramion::modeling::ops {

namespace {

constexpr float kEpsilon = 1e-5f;

struct Plane {
    Vec3 normal{};
    float w = 0.0f;
    void flip() {
        normal = -normal;
        w = -w;
    }
};

struct Polygon {
    std::vector<Vec3> points;
    Plane plane;
    int material = 0;
    int smoothing = 0;
    FaceUv uvs{};
    void flip() {
        std::reverse(points.begin(), points.end());
        plane.flip();
    }
};

bool planeOf(const std::vector<Vec3>& points, Plane& out) {
    // Newell (robusto con poligonos casi degenerados).
    Vec3 n{};
    for (std::size_t i = 0; i < points.size(); ++i) {
        const Vec3& a = points[i];
        const Vec3& b = points[(i + 1) % points.size()];
        n.x += (a.y - b.y) * (a.z + b.z);
        n.y += (a.z - b.z) * (a.x + b.x);
        n.z += (a.x - b.x) * (a.y + b.y);
    }
    const float len = core::length(n);
    if (len < 1e-12f) return false;
    out.normal = n * (1.0f / len);
    out.w = core::dot(out.normal, points[0]);
    return true;
}

enum : int { kCoplanar = 0, kFront = 1, kBack = 2, kSpanning = 3 };

void splitPolygon(const Plane& plane, const Polygon& polygon, std::vector<Polygon>& coplanar_front,
                  std::vector<Polygon>& coplanar_back, std::vector<Polygon>& front, std::vector<Polygon>& back) {
    int type = 0;
    std::vector<int> types(polygon.points.size());
    for (std::size_t i = 0; i < polygon.points.size(); ++i) {
        const float t = core::dot(plane.normal, polygon.points[i]) - plane.w;
        const int k = t < -kEpsilon ? kBack : (t > kEpsilon ? kFront : kCoplanar);
        type |= k;
        types[i] = k;
    }
    switch (type) {
        case kCoplanar:
            (core::dot(plane.normal, polygon.plane.normal) > 0.0f ? coplanar_front : coplanar_back).push_back(polygon);
            break;
        case kFront: front.push_back(polygon); break;
        case kBack: back.push_back(polygon); break;
        default: {
            Polygon f = polygon, b = polygon;
            f.points.clear();
            b.points.clear();
            const std::size_t n = polygon.points.size();
            for (std::size_t i = 0; i < n; ++i) {
                const std::size_t j = (i + 1) % n;
                const int ti = types[i], tj = types[j];
                const Vec3& vi = polygon.points[i];
                const Vec3& vj = polygon.points[j];
                if (ti != kBack) f.points.push_back(vi);
                if (ti != kFront) b.points.push_back(vi);
                if ((ti | tj) == kSpanning) {
                    const float t = (plane.w - core::dot(plane.normal, vi)) / core::dot(plane.normal, vj - vi);
                    const Vec3 v = core::lerp(vi, vj, t);
                    f.points.push_back(v);
                    b.points.push_back(v);
                }
            }
            if (f.points.size() >= 3) front.push_back(std::move(f));
            if (b.points.size() >= 3) back.push_back(std::move(b));
        }
    }
}

class Bsp {
public:
    struct Node {
        Plane plane;
        bool has_plane = false;
        int front = -1;
        int back = -1;
        std::vector<Polygon> polygons;
    };

    explicit Bsp(std::vector<Polygon> polygons) {
        nodes_.push_back(Node{});
        build(0, std::move(polygons));
    }

    void build(int root, std::vector<Polygon> polygons) {
        struct Work {
            int node;
            std::vector<Polygon> polygons;
        };
        std::vector<Work> stack;
        stack.push_back(Work{root, std::move(polygons)});
        while (!stack.empty()) {
            Work work = std::move(stack.back());
            stack.pop_back();
            if (work.polygons.empty()) continue;
            if (!nodes_[static_cast<std::size_t>(work.node)].has_plane) {
                nodes_[static_cast<std::size_t>(work.node)].plane = work.polygons.front().plane;
                nodes_[static_cast<std::size_t>(work.node)].has_plane = true;
            }
            const Plane plane = nodes_[static_cast<std::size_t>(work.node)].plane;
            std::vector<Polygon> front, back, coplanar_front, coplanar_back;
            for (const Polygon& p : work.polygons) splitPolygon(plane, p, coplanar_front, coplanar_back, front, back);
            {
                Node& node = nodes_[static_cast<std::size_t>(work.node)];
                for (Polygon& p : coplanar_front) node.polygons.push_back(std::move(p));
                for (Polygon& p : coplanar_back) node.polygons.push_back(std::move(p));
            }
            if (!front.empty()) {
                if (nodes_[static_cast<std::size_t>(work.node)].front < 0) {
                    nodes_.push_back(Node{});
                    nodes_[static_cast<std::size_t>(work.node)].front = static_cast<int>(nodes_.size()) - 1;
                }
                stack.push_back(Work{nodes_[static_cast<std::size_t>(work.node)].front, std::move(front)});
            }
            if (!back.empty()) {
                if (nodes_[static_cast<std::size_t>(work.node)].back < 0) {
                    nodes_.push_back(Node{});
                    nodes_[static_cast<std::size_t>(work.node)].back = static_cast<int>(nodes_.size()) - 1;
                }
                stack.push_back(Work{nodes_[static_cast<std::size_t>(work.node)].back, std::move(back)});
            }
        }
    }

    // Lo de dentro pasa a fuera y al reves.
    void invert() {
        for (Node& node : nodes_) {
            for (Polygon& p : node.polygons) p.flip();
            if (node.has_plane) node.plane.flip();
            std::swap(node.front, node.back);
        }
    }

    // Quita de `polygons` lo que queda dentro de este arbol.
    std::vector<Polygon> clipPolygons(std::vector<Polygon> polygons) const {
        struct Work {
            int node;
            std::vector<Polygon> polygons;
        };
        std::vector<Polygon> result;
        std::vector<Work> stack;
        stack.push_back(Work{0, std::move(polygons)});
        while (!stack.empty()) {
            Work work = std::move(stack.back());
            stack.pop_back();
            const Node& node = nodes_[static_cast<std::size_t>(work.node)];
            if (!node.has_plane) {
                for (Polygon& p : work.polygons) result.push_back(std::move(p));
                continue;
            }
            std::vector<Polygon> front, back;
            for (const Polygon& p : work.polygons) splitPolygon(node.plane, p, front, back, front, back);
            if (node.front >= 0) {
                stack.push_back(Work{node.front, std::move(front)});
            } else {
                for (Polygon& p : front) result.push_back(std::move(p));
            }
            if (node.back >= 0) stack.push_back(Work{node.back, std::move(back)});
            // Sin hijo de atras: lo de atras esta dentro y se descarta.
        }
        return result;
    }

    void clipTo(const Bsp& other) {
        for (Node& node : nodes_) node.polygons = other.clipPolygons(std::move(node.polygons));
    }

    std::vector<Polygon> allPolygons() const {
        std::vector<Polygon> out;
        for (const Node& node : nodes_) out.insert(out.end(), node.polygons.begin(), node.polygons.end());
        return out;
    }

    void add(std::vector<Polygon> polygons) { build(0, std::move(polygons)); }

private:
    std::vector<Node> nodes_;
};

bool convex(const std::vector<Vec3>& points, const Vec3& normal) {
    const std::size_t n = points.size();
    for (std::size_t i = 0; i < n; ++i) {
        const Vec3& a = points[i];
        const Vec3& b = points[(i + 1) % n];
        const Vec3& c = points[(i + 2) % n];
        if (core::dot(core::cross(b - a, c - b), normal) < -1e-6f) return false;
    }
    return true;
}

std::vector<Polygon> toPolygons(const PolyMesh& m, const Mat4& world) {
    std::vector<Polygon> out;
    const Vec3 x{world.m[0][0], world.m[0][1], world.m[0][2]};
    const Vec3 y{world.m[1][0], world.m[1][1], world.m[1][2]};
    const Vec3 z{world.m[2][0], world.m[2][1], world.m[2][2]};
    const bool mirrored = core::dot(core::cross(x, y), z) < 0.0f;
    for (std::size_t fi = 0; fi < m.faces.size(); ++fi) {
        const Face& f = m.faces[fi];
        std::vector<Vec3> points;
        for (const std::uint32_t v : f.v) points.push_back(ecs::transformPoint(world, m.positions[v]));
        if (mirrored) std::reverse(points.begin(), points.end());
        Polygon base;
        base.material = f.material;
        base.smoothing = f.smoothing;
        base.uvs = f.uvs;
        if (base.uvs.mode == UvMode::Manual) base.uvs.mode = UvMode::Box;
        Plane plane;
        if (!planeOf(points, plane)) continue;
        if (convex(points, plane.normal)) {
            base.points = std::move(points);
            base.plane = plane;
            out.push_back(std::move(base));
            continue;
        }
        // Concava: en triangulos (BSP necesita poligonos convexos).
        const std::vector<std::uint32_t> tris = m.triangulateFace(static_cast<int>(fi));
        for (std::size_t t = 0; t + 2 < tris.size(); t += 3) {
            Polygon tri = base;
            tri.points = {points[tris[t]], points[tris[t + 1]], points[tris[t + 2]]};
            if (mirrored) tri.points = {points[points.size() - 1 - tris[t]], points[points.size() - 1 - tris[t + 2]],
                                        points[points.size() - 1 - tris[t + 1]]};
            if (planeOf(tri.points, tri.plane)) out.push_back(std::move(tri));
        }
    }
    return out;
}

}  // namespace

PolyMesh boolean(const PolyMesh& a_mesh, const Mat4& a_world, const PolyMesh& b_mesh, const Mat4& b_world, BoolOp op) {
    Bsp a(toPolygons(a_mesh, a_world));
    Bsp b(toPolygons(b_mesh, b_world));
    switch (op) {
        case BoolOp::Union:
            a.clipTo(b);
            b.clipTo(a);
            b.invert();
            b.clipTo(a);
            b.invert();
            a.add(b.allPolygons());
            break;
        case BoolOp::Subtract:
            a.invert();
            a.clipTo(b);
            b.clipTo(a);
            b.invert();
            b.clipTo(a);
            b.invert();
            a.add(b.allPolygons());
            a.invert();
            break;
        case BoolOp::Intersect:
            a.invert();
            b.clipTo(a);
            b.invert();
            a.clipTo(b);
            b.clipTo(a);
            a.add(b.allPolygons());
            a.invert();
            break;
    }
    // Al espacio de a, con los vertices iguales soldados.
    const Mat4 to_local = core::inverse(a_world);
    PolyMesh out;
    for (const Polygon& p : a.allPolygons()) {
        if (p.points.size() < 3) continue;
        Face f;
        f.material = p.material;
        f.smoothing = p.smoothing;
        f.uvs = p.uvs;
        for (const Vec3& q : p.points) f.v.push_back(static_cast<std::uint32_t>(out.addVertex(ecs::transformPoint(to_local, q))));
        out.faces.push_back(std::move(f));
    }
    Vec3 lo{}, hi{};
    out.bounds(lo, hi);
    const float size = std::max({hi.x - lo.x, hi.y - lo.y, hi.z - lo.z, 1e-3f});
    weldVertices(out, {}, size * 1e-5f);
    out.cleanup();
    out.removeUnusedVertices();
    return out;
}

}  // namespace cramion::modeling::ops
