// PolyMesh, su paso a ecs::Mesh (triangulos, normales con suavizado y UV
// proyectadas por cara) y el componente EditableMesh.

#include "CramionCore/modeling/EditableMesh.h"

#include "CramionCore/ecs/Components.h"
#include "CramionCore/ecs/MathUtil.h"
#include "CramionCore/ecs/RuntimeMesh.h"
#include "CramionCore/ecs/World.h"
#include "CramionCore/physics/PhysicsComponents.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

namespace cramion::modeling {

namespace {

Vec3 newell(const PolyMesh& m, const Face& f) {
    Vec3 n{};
    const std::size_t count = f.v.size();
    for (std::size_t i = 0; i < count; ++i) {
        const Vec3& a = m.positions[f.v[i]];
        const Vec3& b = m.positions[f.v[(i + 1) % count]];
        n.x += (a.y - b.y) * (a.z + b.z);
        n.y += (a.z - b.z) * (a.x + b.x);
        n.z += (a.x - b.x) * (a.y + b.y);
    }
    return n;
}

float cross2(const Vec2& o, const Vec2& a, const Vec2& b) {
    return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
}

bool insideTriangle(const Vec2& p, const Vec2& a, const Vec2& b, const Vec2& c) {
    const float d1 = cross2(a, b, p);
    const float d2 = cross2(b, c, p);
    const float d3 = cross2(c, a, p);
    return d1 >= -1e-9f && d2 >= -1e-9f && d3 >= -1e-9f;
}

// Base de la proyeccion de UV (u a la derecha, v hacia arriba vistos desde fuera).
void boxBasis(const Vec3& n, Vec3& u, Vec3& v) {
    const float ax = std::abs(n.x), ay = std::abs(n.y), az = std::abs(n.z);
    if (ay >= ax && ay >= az) {
        u = Vec3{1.0f, 0.0f, 0.0f};
        v = n.y >= 0.0f ? Vec3{0.0f, 0.0f, -1.0f} : Vec3{0.0f, 0.0f, 1.0f};
    } else if (ax >= az) {
        u = n.x >= 0.0f ? Vec3{0.0f, 0.0f, -1.0f} : Vec3{0.0f, 0.0f, 1.0f};
        v = Vec3{0.0f, 1.0f, 0.0f};
    } else {
        u = n.z >= 0.0f ? Vec3{1.0f, 0.0f, 0.0f} : Vec3{-1.0f, 0.0f, 0.0f};
        v = Vec3{0.0f, 1.0f, 0.0f};
    }
}

void planarBasis(const Vec3& n, Vec3& u, Vec3& v) {
    const Vec3 up{0.0f, 1.0f, 0.0f};
    const Vec3 projected = up - n * core::dot(up, n);
    if (core::length(projected) < 0.2f) {
        boxBasis(n, u, v);
        return;
    }
    v = core::normalize(projected);
    u = core::normalize(core::cross(v, n));
}

// --- Texto ---
void putFloat(std::string& out, float f) {
    char buffer[32];
    if (!std::isfinite(f)) f = 0.0f;
    const int n = std::snprintf(buffer, sizeof(buffer), "%.6g ", static_cast<double>(f));
    for (int i = 0; i < n; ++i) {
        out.push_back(buffer[i] == ',' ? '.' : buffer[i]);  // sin depender del locale
    }
}
void putInt(std::string& out, long long v) {
    out += std::to_string(v);
    out.push_back(' ');
}

struct Reader {
    const char* p;
    const char* end;
    bool ok = true;
    void skip() {
        while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) ++p;
    }
    long long integer() {
        skip();
        bool neg = false;
        if (p < end && (*p == '-' || *p == '+')) neg = *p++ == '-';
        if (p >= end || *p < '0' || *p > '9') {
            ok = false;
            return 0;
        }
        long long v = 0;
        while (p < end && *p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
        return neg ? -v : v;
    }
    // Sin strtod: no depende del locale (coma o punto).
    float real() {
        skip();
        bool neg = false;
        if (p < end && (*p == '-' || *p == '+')) neg = *p++ == '-';
        double v = 0.0;
        bool digits = false;
        while (p < end && *p >= '0' && *p <= '9') {
            v = v * 10.0 + (*p++ - '0');
            digits = true;
        }
        if (p < end && (*p == '.' || *p == ',')) {
            ++p;
            double scale = 0.1;
            while (p < end && *p >= '0' && *p <= '9') {
                v += (*p++ - '0') * scale;
                scale *= 0.1;
                digits = true;
            }
        }
        if (p < end && (*p == 'e' || *p == 'E')) {
            ++p;
            bool eneg = false;
            if (p < end && (*p == '-' || *p == '+')) eneg = *p++ == '-';
            int e = 0;
            while (p < end && *p >= '0' && *p <= '9') e = e * 10 + (*p++ - '0');
            v *= std::pow(10.0, eneg ? -e : e);
        }
        if (!digits) ok = false;
        return static_cast<float>(neg ? -v : v);
    }
};

}  // namespace

// -----------------------------------------------------------------------------
// PolyMesh
// -----------------------------------------------------------------------------

void PolyMesh::clear() {
    positions.clear();
    faces.clear();
}

int PolyMesh::addVertex(const Vec3& p) {
    positions.push_back(p);
    return static_cast<int>(positions.size()) - 1;
}

int PolyMesh::addFace(std::vector<std::uint32_t> indices, int material) {
    Face f;
    f.v = std::move(indices);
    f.material = material;
    faces.push_back(std::move(f));
    return static_cast<int>(faces.size()) - 1;
}

Vec3 PolyMesh::faceNormal(int face) const {
    const Vec3 n = newell(*this, faces[static_cast<std::size_t>(face)]);
    const float len = core::length(n);
    return len > 1e-12f ? n * (1.0f / len) : Vec3{0.0f, 1.0f, 0.0f};
}

Vec3 PolyMesh::faceCenter(int face) const {
    const Face& f = faces[static_cast<std::size_t>(face)];
    Vec3 c{};
    for (const std::uint32_t i : f.v) c += positions[i];
    return f.v.empty() ? c : c * (1.0f / static_cast<float>(f.v.size()));
}

float PolyMesh::faceArea(int face) const {
    return core::length(newell(*this, faces[static_cast<std::size_t>(face)])) * 0.5f;
}

std::vector<Edge> PolyMesh::edges() const {
    std::vector<Edge> out;
    std::unordered_set<std::uint64_t> seen;
    for (const Face& f : faces) {
        for (std::size_t i = 0; i < f.v.size(); ++i) {
            const Edge e(f.v[i], f.v[(i + 1) % f.v.size()]);
            if (seen.insert(e.key()).second) out.push_back(e);
        }
    }
    return out;
}

std::vector<int> PolyMesh::facesOfEdge(const Edge& e) const {
    std::vector<int> out;
    for (std::size_t fi = 0; fi < faces.size(); ++fi) {
        const Face& f = faces[fi];
        for (std::size_t i = 0; i < f.v.size(); ++i) {
            if (Edge(f.v[i], f.v[(i + 1) % f.v.size()]) == e) {
                out.push_back(static_cast<int>(fi));
                break;
            }
        }
    }
    return out;
}

std::vector<Edge> PolyMesh::borderEdges() const {
    std::unordered_map<std::uint64_t, int> count;
    for (const Face& f : faces) {
        for (std::size_t i = 0; i < f.v.size(); ++i) ++count[Edge(f.v[i], f.v[(i + 1) % f.v.size()]).key()];
    }
    std::vector<Edge> out;
    for (const Edge& e : edges()) {
        if (count[e.key()] == 1) out.push_back(e);
    }
    return out;
}

std::vector<std::vector<int>> PolyMesh::vertexFaces() const {
    std::vector<std::vector<int>> out(positions.size());
    for (std::size_t fi = 0; fi < faces.size(); ++fi) {
        for (const std::uint32_t i : faces[fi].v) {
            if (i < out.size() && (out[i].empty() || out[i].back() != static_cast<int>(fi))) out[i].push_back(static_cast<int>(fi));
        }
    }
    return out;
}

void PolyMesh::bounds(Vec3& min, Vec3& max) const {
    if (positions.empty()) {
        min = max = Vec3{};
        return;
    }
    min = max = positions.front();
    for (const Vec3& p : positions) {
        min = Vec3{std::min(min.x, p.x), std::min(min.y, p.y), std::min(min.z, p.z)};
        max = Vec3{std::max(max.x, p.x), std::max(max.y, p.y), std::max(max.z, p.z)};
    }
}

void PolyMesh::removeUnusedVertices() {
    std::vector<std::int64_t> remap(positions.size(), -1);
    std::vector<Vec3> kept;
    kept.reserve(positions.size());
    for (Face& f : faces) {
        for (std::uint32_t& i : f.v) {
            if (remap[i] < 0) {
                remap[i] = static_cast<std::int64_t>(kept.size());
                kept.push_back(positions[i]);
            }
            i = static_cast<std::uint32_t>(remap[i]);
        }
    }
    positions = std::move(kept);
}

void PolyMesh::cleanup() {
    for (Face& f : faces) {
        // Vertices repetidos seguidos (tras soldar), con sus UV.
        std::vector<std::uint32_t> v;
        std::vector<Vec2> uv;
        const bool has_uv = f.uv.size() == f.v.size();
        for (std::size_t i = 0; i < f.v.size(); ++i) {
            if (!v.empty() && v.back() == f.v[i]) continue;
            v.push_back(f.v[i]);
            if (has_uv) uv.push_back(f.uv[i]);
        }
        while (v.size() > 1 && v.front() == v.back()) {
            v.pop_back();
            if (has_uv) uv.pop_back();
        }
        f.v = std::move(v);
        f.uv = has_uv ? std::move(uv) : std::vector<Vec2>{};
        if (!has_uv && f.uvs.mode == UvMode::Manual) f.uvs.mode = UvMode::Box;
    }
    faces.erase(std::remove_if(faces.begin(), faces.end(),
                               [](const Face& f) {
                                   std::vector<std::uint32_t> sorted = f.v;
                                   std::sort(sorted.begin(), sorted.end());
                                   return std::unique(sorted.begin(), sorted.end()) - sorted.begin() < 3;
                               }),
                faces.end());
}

std::string PolyMesh::validate() const {
    for (std::size_t fi = 0; fi < faces.size(); ++fi) {
        const Face& f = faces[fi];
        if (f.v.size() < 3) return "la cara " + std::to_string(fi) + " tiene menos de 3 vertices";
        for (const std::uint32_t i : f.v) {
            if (i >= positions.size()) return "la cara " + std::to_string(fi) + " usa un vertice que no existe";
        }
        if (f.uvs.mode == UvMode::Manual && f.uv.size() != f.v.size()) {
            return "la cara " + std::to_string(fi) + " tiene UV manuales incompletas";
        }
    }
    for (const Vec3& p : positions) {
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) return "posicion no valida (NaN)";
    }
    return {};
}

std::vector<std::uint32_t> PolyMesh::triangulateFace(int face) const {
    const Face& f = faces[static_cast<std::size_t>(face)];
    const std::size_t n = f.v.size();
    std::vector<std::uint32_t> out;
    if (n < 3) return out;
    if (n == 3) return {0, 1, 2};
    // Al plano de la cara (base con su normal).
    const Vec3 normal = faceNormal(face);
    Vec3 u{}, v{};
    boxBasis(normal, u, v);
    u = core::normalize(u - normal * core::dot(u, normal));
    v = core::cross(normal, u);
    std::vector<Vec2> p(n);
    for (std::size_t i = 0; i < n; ++i) {
        const Vec3& q = positions[f.v[i]];
        p[i] = Vec2{core::dot(q, u), core::dot(q, v)};
    }
    // cross(normal, u) = v: los antihorarios en 3D lo son en (u, v).
    std::vector<std::uint32_t> idx(n);
    for (std::size_t i = 0; i < n; ++i) idx[i] = static_cast<std::uint32_t>(i);
    float area = 0.0f;
    for (std::size_t i = 0; i < n; ++i) {
        const Vec2& a = p[i];
        const Vec2& b = p[(i + 1) % n];
        area += a.x * b.y - b.x * a.y;
    }
    if (area < 0.0f) std::reverse(idx.begin(), idx.end());
    std::size_t guard = 0;
    while (idx.size() > 3 && guard++ < n * n) {
        bool clipped = false;
        const std::size_t m = idx.size();
        for (std::size_t i = 0; i < m; ++i) {
            const std::uint32_t a = idx[(i + m - 1) % m], b = idx[i], c = idx[(i + 1) % m];
            if (cross2(p[a], p[b], p[c]) <= 1e-12f) continue;  // reflejo o degenerado
            bool ear = true;
            for (std::size_t k = 0; k < m && ear; ++k) {
                const std::uint32_t q = idx[k];
                if (q == a || q == b || q == c) continue;
                if (insideTriangle(p[q], p[a], p[b], p[c])) ear = false;
            }
            if (!ear) continue;
            out.insert(out.end(), {a, b, c});
            idx.erase(idx.begin() + static_cast<std::ptrdiff_t>(i));
            clipped = true;
            break;
        }
        if (!clipped) break;  // casi degenerado: abanico con lo que queda
    }
    if (idx.size() == 3) {
        out.insert(out.end(), {idx[0], idx[1], idx[2]});
    } else {
        for (std::size_t i = 1; i + 1 < idx.size(); ++i) out.insert(out.end(), {idx[0], idx[i], idx[i + 1]});
    }
    // Si se dio la vuelta, los triangulos van al reves de la cara: corregir.
    if (area < 0.0f) {
        for (std::size_t t = 0; t + 2 < out.size(); t += 3) std::swap(out[t + 1], out[t + 2]);
    }
    return out;
}

// Formato: "PM1 <vertices> <caras>", las posiciones (x y z) y por cara:
// <n> <indices...> <material> <suavizado> <modo> <relleno> <su> <sv> <ou> <ov>
// <giro> <banderas> [<u v> x n si es manual].
std::string PolyMesh::serialize() const {
    std::string out;
    out.reserve(positions.size() * 30 + faces.size() * 60);
    out += "PM1 ";
    putInt(out, static_cast<long long>(positions.size()));
    putInt(out, static_cast<long long>(faces.size()));
    for (const Vec3& p : positions) {
        putFloat(out, p.x);
        putFloat(out, p.y);
        putFloat(out, p.z);
    }
    for (const Face& f : faces) {
        putInt(out, static_cast<long long>(f.v.size()));
        for (const std::uint32_t i : f.v) putInt(out, i);
        putInt(out, f.material);
        putInt(out, f.smoothing);
        const bool manual = f.uvs.mode == UvMode::Manual && f.uv.size() == f.v.size();
        putInt(out, manual ? 2 : (f.uvs.mode == UvMode::Manual ? 0 : static_cast<int>(f.uvs.mode)));
        putInt(out, static_cast<int>(f.uvs.fill));
        putFloat(out, f.uvs.scale.x);
        putFloat(out, f.uvs.scale.y);
        putFloat(out, f.uvs.offset.x);
        putFloat(out, f.uvs.offset.y);
        putFloat(out, f.uvs.rotation);
        putInt(out, (f.uvs.flip_u ? 1 : 0) | (f.uvs.flip_v ? 2 : 0) | (f.uvs.swap_uv ? 4 : 0) | (f.uvs.world_space ? 8 : 0));
        if (manual) {
            for (const Vec2& uv : f.uv) {
                putFloat(out, uv.x);
                putFloat(out, uv.y);
            }
        }
    }
    if (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

bool PolyMesh::deserialize(const std::string& text) {
    Reader r{text.data(), text.data() + text.size()};
    r.skip();
    if (text.size() < 3 || std::strncmp(r.p, "PM1", 3) != 0) return false;
    r.p += 3;
    const long long nv = r.integer();
    const long long nf = r.integer();
    if (!r.ok || nv < 0 || nf < 0 || nv > 50000000 || nf > 50000000) return false;
    PolyMesh m;
    m.positions.resize(static_cast<std::size_t>(nv));
    for (Vec3& p : m.positions) {
        p.x = r.real();
        p.y = r.real();
        p.z = r.real();
    }
    m.faces.resize(static_cast<std::size_t>(nf));
    for (Face& f : m.faces) {
        const long long n = r.integer();
        if (!r.ok || n < 0 || n > 100000) return false;
        f.v.resize(static_cast<std::size_t>(n));
        for (std::uint32_t& i : f.v) {
            const long long x = r.integer();
            if (x < 0 || x >= nv) return false;
            i = static_cast<std::uint32_t>(x);
        }
        f.material = static_cast<int>(r.integer());
        f.smoothing = static_cast<int>(r.integer());
        f.uvs.mode = static_cast<UvMode>(std::clamp(static_cast<int>(r.integer()), 0, 2));
        f.uvs.fill = static_cast<UvFill>(std::clamp(static_cast<int>(r.integer()), 0, 2));
        f.uvs.scale.x = r.real();
        f.uvs.scale.y = r.real();
        f.uvs.offset.x = r.real();
        f.uvs.offset.y = r.real();
        f.uvs.rotation = r.real();
        const long long flags = r.integer();
        f.uvs.flip_u = (flags & 1) != 0;
        f.uvs.flip_v = (flags & 2) != 0;
        f.uvs.swap_uv = (flags & 4) != 0;
        f.uvs.world_space = (flags & 8) != 0;
        if (f.uvs.mode == UvMode::Manual) {
            f.uv.resize(f.v.size());
            for (Vec2& uv : f.uv) {
                uv.x = r.real();
                uv.y = r.real();
            }
        }
        if (!r.ok) return false;
    }
    if (!r.ok) return false;
    *this = std::move(m);
    return true;
}

// -----------------------------------------------------------------------------
// UV y malla del renderizador
// -----------------------------------------------------------------------------

namespace {

// UV de todas las esquinas de una cara.
void faceUvs(const PolyMesh& mesh, int face, const Mat4& world, float uv_scale, std::vector<Vec2>& out) {
    const Face& f = mesh.faces[static_cast<std::size_t>(face)];
    const std::size_t n = f.v.size();
    out.resize(n);
    const float global = std::max(uv_scale, 1e-4f);
    if (f.uvs.mode == UvMode::Manual && f.uv.size() == n) {
        for (std::size_t i = 0; i < n; ++i) out[i] = f.uv[i];
        return;
    }
    Vec3 normal = mesh.faceNormal(face);
    if (f.uvs.world_space) normal = core::normalize(ecs::transformDirection(world, normal));
    Vec3 u{}, v{};
    if (f.uvs.mode == UvMode::Planar) {
        planarBasis(normal, u, v);
    } else {
        boxBasis(normal, u, v);
    }
    Vec2 lo{1e30f, 1e30f}, hi{-1e30f, -1e30f};
    for (std::size_t i = 0; i < n; ++i) {
        Vec3 p = mesh.positions[f.v[i]];
        if (f.uvs.world_space) p = ecs::transformPoint(world, p);
        out[i] = Vec2{core::dot(p, u), core::dot(p, v)};
        lo = Vec2{std::min(lo.x, out[i].x), std::min(lo.y, out[i].y)};
        hi = Vec2{std::max(hi.x, out[i].x), std::max(hi.y, out[i].y)};
    }
    const Vec2 scale{std::abs(f.uvs.scale.x) > 1e-5f ? f.uvs.scale.x : 1.0f,
                     std::abs(f.uvs.scale.y) > 1e-5f ? f.uvs.scale.y : 1.0f};
    const float angle = core::radians(f.uvs.rotation);
    const float cs = std::cos(angle), sn = std::sin(angle);
    const Vec2 extent{std::max(hi.x - lo.x, 1e-6f), std::max(hi.y - lo.y, 1e-6f)};
    for (Vec2& t : out) {
        Vec2 w = t;
        Vec2 pivot{0.0f, 0.0f};
        if (f.uvs.fill == UvFill::Tile) {
            // Metros: v hacia arriba en la cara = hacia arriba en la imagen.
            w = Vec2{w.x / (scale.x * global), -w.y / (scale.y * global)};
        } else {
            Vec2 k = f.uvs.fill == UvFill::Stretch ? extent : Vec2{std::max(extent.x, extent.y), std::max(extent.x, extent.y)};
            w = Vec2{(w.x - lo.x) / k.x, 1.0f - (w.y - lo.y) / k.y};
            w = Vec2{w.x / scale.x, w.y / scale.y};
            pivot = Vec2{0.5f / scale.x, 0.5f / scale.y};
        }
        if (f.uvs.swap_uv) std::swap(w.x, w.y);
        if (f.uvs.flip_u) w.x = -w.x + 2.0f * pivot.x;
        if (f.uvs.flip_v) w.y = -w.y + 2.0f * pivot.y;
        if (angle != 0.0f) {
            const Vec2 d{w.x - pivot.x, w.y - pivot.y};
            w = Vec2{pivot.x + d.x * cs - d.y * sn, pivot.y + d.x * sn + d.y * cs};
        }
        t = Vec2{w.x + f.uvs.offset.x, w.y + f.uvs.offset.y};
    }
}

}  // namespace

Vec2 faceCornerUv(const PolyMesh& mesh, int face, int corner, const Mat4& world, float uv_scale) {
    std::vector<Vec2> uvs;
    faceUvs(mesh, face, world, uv_scale, uvs);
    return corner >= 0 && static_cast<std::size_t>(corner) < uvs.size() ? uvs[static_cast<std::size_t>(corner)] : Vec2{};
}

std::shared_ptr<ecs::Mesh> buildMesh(const PolyMesh& mesh, const BuildOptions& options) {
    auto out = std::make_shared<ecs::Mesh>();
    out->name = "Malla editable";
    const std::size_t face_count = mesh.faces.size();
    int max_material = 0;
    for (const Face& f : mesh.faces) max_material = std::max(max_material, std::max(f.material, 0));
    const int submeshes = std::max(max_material + 1, std::max(static_cast<int>(options.slots.size()), 1));

    // Normales de cara (Newell: su longitud es el doble del area, el peso).
    std::vector<Vec3> weighted(face_count), unit(face_count);
    for (std::size_t fi = 0; fi < face_count; ++fi) {
        weighted[fi] = newell(mesh, mesh.faces[fi]);
        const float len = core::length(weighted[fi]);
        unit[fi] = len > 1e-12f ? weighted[fi] * (1.0f / len) : Vec3{0.0f, 1.0f, 0.0f};
    }
    const std::vector<std::vector<int>> vertex_faces = mesh.vertexFaces();
    const float auto_cos = options.auto_smooth_angle > 0.0f
                               ? std::cos(core::radians(std::min(options.auto_smooth_angle, 180.0f))) - 1e-5f
                               : 2.0f;

    std::vector<std::vector<std::uint32_t>> triangles(static_cast<std::size_t>(submeshes));
    std::vector<Vec2> uvs;
    for (std::size_t fi = 0; fi < face_count; ++fi) {
        const Face& f = mesh.faces[fi];
        if (f.v.size() < 3) continue;
        const auto base = static_cast<std::uint32_t>(out->vertices.size());
        faceUvs(mesh, static_cast<int>(fi), options.world, options.uv_scale, uvs);
        for (std::size_t c = 0; c < f.v.size(); ++c) {
            const std::uint32_t vi = f.v[c];
            Vec3 n = unit[fi];
            const bool smooth_group = f.smoothing > 0;
            if (smooth_group || auto_cos <= 1.0f) {
                Vec3 sum{};
                for (const int g : vertex_faces[vi]) {
                    const auto gi = static_cast<std::size_t>(g);
                    const bool same = gi == fi || (smooth_group && mesh.faces[gi].smoothing == f.smoothing) ||
                                      core::dot(unit[fi], unit[gi]) >= auto_cos;
                    if (same) sum += weighted[gi];
                }
                const float len = core::length(sum);
                if (len > 1e-12f) n = sum * (1.0f / len);
            }
            out->vertices.push_back(mesh.positions[vi]);
            out->normals.push_back(n);
            out->uv.push_back(uvs[c]);
        }
        std::vector<std::uint32_t>& list = triangles[static_cast<std::size_t>(std::clamp(f.material, 0, submeshes - 1))];
        for (const std::uint32_t t : mesh.triangulateFace(static_cast<int>(fi))) list.push_back(base + t);
    }
    out->setSubMeshCount(submeshes);
    for (int s = 0; s < submeshes; ++s) out->setTriangles(std::move(triangles[static_cast<std::size_t>(s)]), s);
    out->materials.resize(static_cast<std::size_t>(submeshes));
    for (int s = 0; s < submeshes; ++s) {
        const SlotMaterial slot = static_cast<std::size_t>(s) < options.slots.size() ? options.slots[static_cast<std::size_t>(s)]
                                                                                    : SlotMaterial{};
        ecs::MeshMaterial& m = out->materials[static_cast<std::size_t>(s)];
        m.color = core::Vec4{slot.color.x, slot.color.y, slot.color.z, 1.0f};
        m.metallic = slot.metallic;
        m.roughness = slot.roughness;
    }
    if (!out->vertices.empty()) out->recalculateTangents();
    out->markModified();
    return out;
}

// -----------------------------------------------------------------------------
// Componente
// -----------------------------------------------------------------------------

void EditableMesh::reflect(ecs::PropertyVisitor& v) {
    if (v.field({"auto_smooth_angle", "Suavizado automatico",
                 "Grados: las caras vecinas con menos angulo se ven suaves (0 = solo los grupos de suavizado)"},
                auto_smooth_angle, ecs::FloatRange{0.0f, 180.0f, 0.5f, "%.1f°", true})) {
        markModified();
    }
    if (v.field({"uv_scale", "Escala de UV", "Metros por repeticion de la textura en todas las caras"}, uv_scale,
                ecs::FloatRange{0.01f, 100.0f, 0.01f, "%.2f m"})) {
        markModified();
    }
    if (ecs::listField(v, {"slots", "Huecos de material",
                           "Color de cada hueco cuando el Mesh Renderer no le pone un .crmat (Materiales)"},
                       slots, [](SlotMaterial& s, ecs::PropertyVisitor& iv) {
                           iv.field({"color", "Color"}, s.color, ecs::Vec3Kind::Color);
                           iv.field({"metallic", "Metalico"}, s.metallic, ecs::FloatRange{0.0f, 1.0f, 0.01f, "%.2f", true});
                           iv.field({"roughness", "Rugosidad"}, s.roughness, ecs::FloatRange{0.0f, 1.0f, 0.01f, "%.2f", true});
                       })) {
        markModified();
    }
    // La malla solo al guardar/leer (en el Inspector se edita con la ventana Modelado).
    if (v.wantsAllFields()) {
        std::string data = mesh.serialize();
        if (v.field({"data", "Malla"}, data)) {
            if (mesh.deserialize(data)) markModified();
        }
    }
}

void registerModelingComponents() {
    ecs::ComponentRegistry::instance().registerComponent<EditableMesh>("EditableMesh", "Malla editable (modelado)",
                                                                       "Renderizado");
}

namespace {

// Lo ya generado por entidad: si el componente, sus ajustes o (con UV en el
// mundo) la matriz cambian, se rehace.
struct Built {
    std::uint64_t revision = 0;
    std::uint64_t settings = 0;
    const EditableMesh* component = nullptr;
    std::weak_ptr<ecs::Mesh> mesh;
};

std::uint64_t settingsHash(const EditableMesh& em, const ecs::Entity& e, bool world_uv) {
    std::uint64_t h = 1469598103934665603ull;
    const auto mix = [&](float f) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &f, sizeof(bits));
        h = (h ^ bits) * 1099511628211ull;
    };
    mix(em.auto_smooth_angle);
    mix(em.uv_scale);
    for (const SlotMaterial& s : em.slots) {
        mix(s.color.x);
        mix(s.color.y);
        mix(s.color.z);
        mix(s.metallic);
        mix(s.roughness);
    }
    if (world_uv) {
        const Mat4& m = e.worldMatrix();
        for (int i = 0; i < 16; ++i) mix(m.m[i / 4][i % 4]);
    }
    return h;
}

}  // namespace

void updateEditableMeshes(ecs::World& world) {
    static std::unordered_map<const ecs::World*, std::unordered_map<entt::entity, Built>> caches;
    auto& cache = caches[&world];
    auto& registry = world.registry();
    auto view = registry.view<EditableMesh>();
    if (view.begin() == view.end()) {
        cache.clear();
        return;
    }
    for (const entt::entity h : view) {
        EditableMesh& em = view.get<EditableMesh>(h);
        ecs::Entity e = world.wrap(h);
        bool world_uv = false;
        for (const Face& f : em.mesh.faces) {
            if (f.uvs.world_space) {
                world_uv = true;
                break;
            }
        }
        ecs::MeshRenderer* renderer = e.tryGet<ecs::MeshRenderer>();
        if (renderer == nullptr) renderer = &e.add<ecs::MeshRenderer>();
        Built& b = cache[h];
        const std::uint64_t settings = settingsHash(em, e, world_uv);
        const std::shared_ptr<ecs::Mesh> current = b.mesh.lock();
        const bool fresh = b.component == &em && b.revision == em.revision && b.settings == settings && current &&
                           renderer->mesh == current;
        if (fresh) continue;
        if (em.mesh.faces.empty()) {
            renderer->mesh.reset();
            b = Built{em.revision, settings, &em, {}};
            continue;
        }
        BuildOptions options;
        options.auto_smooth_angle = em.auto_smooth_angle;
        options.uv_scale = em.uv_scale;
        options.slots = em.slots;
        if (world_uv) options.world = e.worldMatrix();
        std::shared_ptr<ecs::Mesh> mesh = buildMesh(em.mesh, options);
        renderer->mesh = mesh;
        b = Built{em.revision, settings, &em, mesh};
    }
    // Las entidades que ya no tienen el componente.
    for (auto it = cache.begin(); it != cache.end();) {
        it = registry.valid(it->first) && registry.all_of<EditableMesh>(it->first) ? std::next(it) : cache.erase(it);
    }
}

ecs::Entity createEditableEntity(ecs::World& world, PolyMesh mesh, const std::string& name, ecs::Entity parent,
                                 bool collider) {
    registerModelingComponents();
    ecs::Entity e = world.create(name, parent);
    EditableMesh& em = e.add<EditableMesh>();
    em.mesh = std::move(mesh);
    em.markModified();
    e.add<ecs::MeshRenderer>();
    if (collider) e.add<physics::MeshCollider>();
    updateEditableMeshes(world);
    return e;
}

}  // namespace cramion::modeling
