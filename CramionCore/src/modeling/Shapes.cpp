// Formas parametricas de la malla editable. Casi todas salen de dos
// herramientas: el torno (un perfil que gira alrededor de Y: cilindros, conos,
// esferas, capsulas, toros, tubos) y la extrusion de un perfil plano
// (escaleras, rampas, arcos, puertas, prismas). Al final, conformNormals deja
// todas las caras mirando hacia fuera.

#include "CramionCore/modeling/EditableMesh.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace cramion::modeling::shapes {

namespace {

constexpr float kTau = 2.0f * core::kPi;

// Rejilla de quads sobre un rectangulo (origen, dos lados y cortes).
void grid(PolyMesh& m, const Vec3& origin, const Vec3& du, const Vec3& dv, int nu, int nv) {
    nu = std::max(nu, 1);
    nv = std::max(nv, 1);
    const auto base = static_cast<std::uint32_t>(m.positions.size());
    for (int j = 0; j <= nv; ++j) {
        for (int i = 0; i <= nu; ++i) {
            m.positions.push_back(origin + du * (static_cast<float>(i) / static_cast<float>(nu)) +
                                  dv * (static_cast<float>(j) / static_cast<float>(nv)));
        }
    }
    const auto at = [&](int i, int j) { return base + static_cast<std::uint32_t>(j * (nu + 1) + i); };
    for (int j = 0; j < nv; ++j) {
        for (int i = 0; i < nu; ++i) m.addFace({at(i, j), at(i + 1, j), at(i + 1, j + 1), at(i, j + 1)});
    }
}

struct ProfilePoint {
    float radius;  // distancia al eje (0 = polo)
    float y;
};

// Torno: el perfil (radio, altura) gira alrededor de Y. `closed`: el perfil
// es un bucle (toros, tubos). smooth: 0 aristas duras, 1 todo suave, 2 suave
// alrededor del eje pero duro entre tramos del perfil.
PolyMesh lathe(const std::vector<ProfilePoint>& profile, bool closed, int segments, bool caps, int smooth,
               const Vec3& size, float sweep = kTau) {
    PolyMesh m;
    segments = std::max(segments, 3);
    const bool full = std::abs(sweep - kTau) < 1e-4f;
    const int columns = full ? segments : segments + 1;
    const float sx = size.x * 0.5f, sz = size.z * 0.5f, sy = size.y;
    std::vector<std::vector<std::uint32_t>> rings(profile.size());
    for (std::size_t k = 0; k < profile.size(); ++k) {
        const ProfilePoint& p = profile[k];
        if (p.radius <= 1e-6f) {
            rings[k].push_back(static_cast<std::uint32_t>(m.addVertex(Vec3{0.0f, p.y * sy, 0.0f})));
            continue;
        }
        for (int i = 0; i < columns; ++i) {
            const float a = sweep * static_cast<float>(i) / static_cast<float>(segments);
            rings[k].push_back(static_cast<std::uint32_t>(
                m.addVertex(Vec3{p.radius * sx * std::cos(a), p.y * sy, -p.radius * sz * std::sin(a)})));
        }
    }
    const auto at = [&](std::size_t k, int i) {
        const std::vector<std::uint32_t>& ring = rings[k];
        return ring.size() == 1 ? ring[0] : ring[static_cast<std::size_t>(full ? i % segments : i)];
    };
    const std::size_t spans = closed ? profile.size() : profile.size() - 1;
    for (std::size_t k = 0; k < spans; ++k) {
        const std::size_t k2 = (k + 1) % profile.size();
        const int group = smooth == 0 ? 0 : (smooth == 1 ? 1 : static_cast<int>(k) + 1);
        for (int i = 0; i < segments; ++i) {
            std::vector<std::uint32_t> face;
            const std::uint32_t a = at(k, i), b = at(k, i + 1), c = at(k2, i + 1), d = at(k2, i);
            for (const std::uint32_t q : {a, b, c, d}) {
                if (face.empty() || face.back() != q) face.push_back(q);
            }
            if (face.size() > 1 && face.front() == face.back()) face.pop_back();
            if (face.size() < 3) continue;
            const int f = m.addFace(std::move(face));
            m.faces[static_cast<std::size_t>(f)].smoothing = group;
        }
    }
    if (caps && !closed && full) {
        if (rings.front().size() > 1) {
            std::vector<std::uint32_t> bottom(rings.front().rbegin(), rings.front().rend());
            m.addFace(std::move(bottom));
        }
        if (rings.back().size() > 1) m.addFace(rings.back());
    }
    ops::conformNormals(m);
    return m;
}

// Perfil plano (en los ejes u, v desde `origin`) extruido `length` por `dir`.
PolyMesh extrudeProfile(const std::vector<Vec2>& outline, const Vec3& origin, const Vec3& u, const Vec3& v,
                        const Vec3& dir, float length) {
    PolyMesh m;
    const std::size_t n = outline.size();
    if (n < 3) return m;
    for (const float t : {0.0f, 1.0f}) {
        for (const Vec2& p : outline) m.addVertex(origin + u * p.x + v * p.y + dir * (length * t));
    }
    std::vector<std::uint32_t> back, front;
    for (std::size_t i = 0; i < n; ++i) {
        back.push_back(static_cast<std::uint32_t>(i));
        front.push_back(static_cast<std::uint32_t>(n + i));
    }
    std::reverse(back.begin(), back.end());
    m.addFace(std::move(back));
    m.addFace(std::move(front));
    for (std::size_t i = 0; i < n; ++i) {
        const auto a = static_cast<std::uint32_t>(i), b = static_cast<std::uint32_t>((i + 1) % n);
        m.addFace({a, b, static_cast<std::uint32_t>(n + b), static_cast<std::uint32_t>(n + a)});
    }
    ops::conformNormals(m);
    return m;
}

PolyMesh box(const Vec3& size, int sx, int sy, int sz) {
    PolyMesh m;
    const float hx = size.x * 0.5f, hz = size.z * 0.5f, h = size.y;
    const Vec3 X{2.0f * hx, 0.0f, 0.0f}, Y{0.0f, h, 0.0f}, Z{0.0f, 0.0f, 2.0f * hz};
    grid(m, Vec3{-hx, h, hz}, X, -Z, sx, sz);    // arriba
    grid(m, Vec3{-hx, 0.0f, -hz}, X, Z, sx, sz);  // abajo
    grid(m, Vec3{-hx, 0.0f, hz}, X, Y, sx, sy);   // delante (+Z)
    grid(m, Vec3{hx, 0.0f, -hz}, -X, Y, sx, sy);  // detras
    grid(m, Vec3{hx, 0.0f, hz}, -Z, Y, sz, sy);   // +X
    grid(m, Vec3{-hx, 0.0f, -hz}, Z, Y, sz, sy);  // -X
    ops::weldVertices(m, {}, std::max(1e-5f, 1e-4f * std::min({size.x, size.y, size.z})));
    return m;
}

PolyMesh icosphere(const Vec3& size, int subdivisions) {
    PolyMesh m;
    const float t = (1.0f + std::sqrt(5.0f)) * 0.5f;
    const Vec3 base[12] = {{-1, t, 0}, {1, t, 0}, {-1, -t, 0}, {1, -t, 0}, {0, -1, t}, {0, 1, t},
                           {0, -1, -t}, {0, 1, -t}, {t, 0, -1}, {t, 0, 1}, {-t, 0, -1}, {-t, 0, 1}};
    for (const Vec3& p : base) m.addVertex(core::normalize(p));
    const int tris[20][3] = {{0, 11, 5}, {0, 5, 1}, {0, 1, 7}, {0, 7, 10}, {0, 10, 11}, {1, 5, 9}, {5, 11, 4},
                             {11, 10, 2}, {10, 7, 6}, {7, 1, 8}, {3, 9, 4}, {3, 4, 2}, {3, 2, 6}, {3, 6, 8},
                             {3, 8, 9}, {4, 9, 5}, {2, 4, 11}, {6, 2, 10}, {8, 6, 7}, {9, 8, 1}};
    for (const auto& tri : tris) {
        m.addFace({static_cast<std::uint32_t>(tri[0]), static_cast<std::uint32_t>(tri[1]), static_cast<std::uint32_t>(tri[2])});
    }
    for (int s = 0; s < std::clamp(subdivisions, 0, 6); ++s) {
        PolyMesh next;
        next.positions = m.positions;
        std::vector<std::pair<std::uint64_t, std::uint32_t>> mids;
        const auto mid = [&](std::uint32_t a, std::uint32_t b) {
            const std::uint64_t key = Edge(a, b).key();
            for (const auto& [k, i] : mids) {
                if (k == key) return i;
            }
            const auto i = static_cast<std::uint32_t>(next.addVertex(core::normalize(m.positions[a] + m.positions[b])));
            mids.emplace_back(key, i);
            return i;
        };
        for (const Face& f : m.faces) {
            const std::uint32_t a = f.v[0], b = f.v[1], c = f.v[2];
            const std::uint32_t ab = mid(a, b), bc = mid(b, c), ca = mid(c, a);
            next.addFace({a, ab, ca});
            next.addFace({b, bc, ab});
            next.addFace({c, ca, bc});
            next.addFace({ab, bc, ca});
        }
        m = std::move(next);
    }
    for (Vec3& p : m.positions) p = Vec3{p.x * size.x * 0.5f, (p.y + 1.0f) * size.y * 0.5f, p.z * size.z * 0.5f};
    for (Face& f : m.faces) f.smoothing = 1;
    ops::conformNormals(m);
    return m;
}

}  // namespace

const char* kindName(Kind kind) {
    switch (kind) {
        case Kind::Cube: return "Cubo";
        case Kind::Plane: return "Plano";
        case Kind::Cylinder: return "Cilindro";
        case Kind::Cone: return "Cono";
        case Kind::Sphere: return "Esfera";
        case Kind::Icosphere: return "Icoesfera";
        case Kind::Torus: return "Toro";
        case Kind::Pipe: return "Tubo";
        case Kind::Prism: return "Prisma";
        case Kind::Wedge: return "Rampa (cuna)";
        case Kind::Stairs: return "Escalera";
        case Kind::CurvedStairs: return "Escalera curva";
        case Kind::Arch: return "Arco";
        case Kind::Door: return "Pared con puerta";
        case Kind::Room: return "Habitacion (interior)";
        case Kind::Capsule: return "Capsula";
        case Kind::Count: break;
    }
    return "?";
}

const char* kindKey(Kind kind) {
    static constexpr const char* kKeys[] = {"cube", "plane", "cylinder", "cone", "sphere", "icosphere",
                                            "torus", "pipe", "prism", "wedge", "stairs", "curved_stairs",
                                            "arch", "door", "room", "capsule"};
    const int i = static_cast<int>(kind);
    return i >= 0 && i < static_cast<int>(Kind::Count) ? kKeys[i] : "?";
}

bool kindFromKey(const std::string& key, Kind& kind) {
    for (int i = 0; i < static_cast<int>(Kind::Count); ++i) {
        if (key == kindKey(static_cast<Kind>(i))) {
            kind = static_cast<Kind>(i);
            return true;
        }
    }
    return false;
}

Params defaults(Kind kind) {
    Params p;
    switch (kind) {
        case Kind::Plane: p.size = Vec3{4.0f, 0.0f, 4.0f}; p.subdivisions_x = p.subdivisions_z = 4; break;
        case Kind::Cylinder: p.size = Vec3{1.0f, 2.0f, 1.0f}; p.segments = 24; break;
        case Kind::Cone: p.size = Vec3{1.0f, 1.5f, 1.0f}; p.segments = 24; break;
        case Kind::Sphere: p.segments = 24; p.rings = 12; break;
        case Kind::Icosphere: p.rings = 2; break;
        case Kind::Torus: p.size = Vec3{2.0f, 0.5f, 2.0f}; p.segments = 32; p.rings = 12; p.thickness = 0.25f; break;
        case Kind::Pipe: p.size = Vec3{1.0f, 2.0f, 1.0f}; p.segments = 24; p.thickness = 0.15f; break;
        case Kind::Wedge: p.size = Vec3{2.0f, 1.0f, 3.0f}; break;
        case Kind::Stairs: p.size = Vec3{2.0f, 2.0f, 3.0f}; p.steps = 10; break;
        case Kind::CurvedStairs: p.size = Vec3{6.0f, 3.0f, 6.0f}; p.steps = 16; p.inner_radius = 1.0f; p.angle = 180.0f; break;
        case Kind::Arch: p.size = Vec3{4.0f, 2.0f, 1.0f}; p.segments = 16; p.thickness = 0.5f; p.angle = 180.0f; break;
        case Kind::Door: p.size = Vec3{4.0f, 3.0f, 0.25f}; p.door = Vec2{0.3f, 0.75f}; break;
        case Kind::Room: p.size = Vec3{8.0f, 3.0f, 8.0f}; break;
        case Kind::Capsule: p.size = Vec3{1.0f, 2.0f, 1.0f}; p.segments = 24; p.rings = 6; break;
        default: break;
    }
    return p;
}

PolyMesh make(Kind kind, const Params& p) {
    const Vec3 size{std::max(p.size.x, 1e-3f), std::max(p.size.y, 0.0f), std::max(p.size.z, 1e-3f)};
    const int segments = std::clamp(p.segments, 3, 256);
    const int smooth = p.smooth ? 2 : 0;
    switch (kind) {
        case Kind::Cube: return box(Vec3{size.x, std::max(size.y, 1e-3f), size.z}, p.subdivisions_x, p.subdivisions_y, p.subdivisions_z);
        case Kind::Plane: {
            PolyMesh m;
            grid(m, Vec3{-size.x * 0.5f, 0.0f, size.z * 0.5f}, Vec3{size.x, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, -size.z},
                 p.subdivisions_x, p.subdivisions_z);
            return m;
        }
        case Kind::Cylinder: {
            std::vector<ProfilePoint> profile;
            const int rows = std::max(p.subdivisions_y, 1);
            for (int k = 0; k <= rows; ++k) profile.push_back({1.0f, static_cast<float>(k) / static_cast<float>(rows)});
            return lathe(profile, false, segments, p.caps, p.smooth ? 1 : 0, size);
        }
        case Kind::Cone: return lathe({{1.0f, 0.0f}, {0.0f, 1.0f}}, false, segments, p.caps, smooth, size);
        case Kind::Sphere: {
            std::vector<ProfilePoint> profile;
            const int rings = std::clamp(p.rings, 2, 128);
            for (int k = 0; k <= rings; ++k) {
                const float phi = core::kPi * static_cast<float>(k) / static_cast<float>(rings);
                profile.push_back({k == 0 || k == rings ? 0.0f : std::sin(phi), (1.0f - std::cos(phi)) * 0.5f});
            }
            return lathe(profile, false, segments, false, p.smooth ? 1 : 0, size);
        }
        case Kind::Icosphere: return icosphere(Vec3{size.x, std::max(size.y, 1e-3f), size.z}, p.rings);
        case Kind::Torus: {
            // size.x = diametro exterior; thickness = radio del tubo.
            const float outer = size.x * 0.5f;
            const float tube = std::clamp(p.thickness, 1e-3f, outer * 0.95f);
            const float major = outer - tube;
            std::vector<ProfilePoint> profile;
            const int rings = std::clamp(p.rings, 3, 128);
            for (int j = 0; j < rings; ++j) {
                const float b = kTau * static_cast<float>(j) / static_cast<float>(rings);
                profile.push_back({(major + tube * std::cos(b)) / outer, tube + tube * std::sin(b)});
            }
            return lathe(profile, true, segments, false, p.smooth ? 1 : 0, Vec3{size.x, 1.0f, size.z});
        }
        case Kind::Pipe: {
            const float outer = size.x * 0.5f;
            const float inner = std::clamp(outer - p.thickness, 1e-3f, outer * 0.98f) / outer;
            return lathe({{1.0f, 0.0f}, {1.0f, 1.0f}, {inner, 1.0f}, {inner, 0.0f}}, true, segments, false, smooth, size);
        }
        case Kind::Prism: {
            const float hx = size.x * 0.5f;
            return extrudeProfile({{-hx, 0.0f}, {hx, 0.0f}, {0.0f, std::max(size.y, 1e-3f)}}, Vec3{0.0f, 0.0f, -size.z * 0.5f},
                                  Vec3{1.0f, 0.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f}, Vec3{0.0f, 0.0f, 1.0f}, size.z);
        }
        case Kind::Wedge: {
            // Sube hacia -Z: perfil en (z, y).
            const float hz = size.z * 0.5f;
            return extrudeProfile({{hz, 0.0f}, {-hz, 0.0f}, {-hz, std::max(size.y, 1e-3f)}}, Vec3{-size.x * 0.5f, 0.0f, 0.0f},
                                  Vec3{0.0f, 0.0f, 1.0f}, Vec3{0.0f, 1.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f}, size.x);
        }
        case Kind::Stairs: {
            // Perfil de dientes en (z, y), sube hacia -Z. Con laterales: maciza
            // hasta el suelo; sin ellos, una losa de `thickness` por debajo.
            const int steps = std::clamp(p.steps, 1, 256);
            const float hz = size.z * 0.5f, h = std::max(size.y, 1e-3f);
            const float step_d = size.z / static_cast<float>(steps), step_h = h / static_cast<float>(steps);
            std::vector<Vec2> outline;
            outline.push_back({hz, 0.0f});
            for (int i = 0; i < steps; ++i) {
                outline.push_back({hz - step_d * static_cast<float>(i), step_h * static_cast<float>(i + 1)});
                outline.push_back({hz - step_d * static_cast<float>(i + 1), step_h * static_cast<float>(i + 1)});
            }
            if (p.sides) {
                outline.push_back({-hz, 0.0f});
            } else {
                // Losa: baja por la pendiente, paralela a los bordes de los peldanos.
                const float t = std::max(p.thickness, 0.02f);
                outline.push_back({-hz, h - t});
                outline.push_back({hz - step_d, std::max(step_h - t, 0.0f)});
            }
            return extrudeProfile(outline, Vec3{-size.x * 0.5f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, 1.0f}, Vec3{0.0f, 1.0f, 0.0f},
                                  Vec3{1.0f, 0.0f, 0.0f}, size.x);
        }
        case Kind::CurvedStairs: {
            // Un bloque por peldano alrededor de Y (radio interior y exterior).
            PolyMesh m;
            const int steps = std::clamp(p.steps, 1, 256);
            const float outer = size.x * 0.5f;
            const float inner = std::clamp(p.inner_radius, 0.0f, outer * 0.95f);
            const float sweep = core::radians(std::clamp(p.angle, 1.0f, 3600.0f));
            const float h = std::max(size.y, 1e-3f);
            for (int i = 0; i < steps; ++i) {
                const float a0 = sweep * static_cast<float>(i) / static_cast<float>(steps);
                const float a1 = sweep * static_cast<float>(i + 1) / static_cast<float>(steps);
                const float top = h * static_cast<float>(i + 1) / static_cast<float>(steps);
                const float bottom = p.sides ? 0.0f : std::max(top - h / static_cast<float>(steps) - p.thickness, 0.0f);
                const auto at = [&](float r, float a, float y) {
                    return Vec3{r * std::cos(a), y, -r * std::sin(a)};
                };
                const auto base = static_cast<std::uint32_t>(m.positions.size());
                for (const float y : {bottom, top}) {
                    m.addVertex(at(inner, a0, y));
                    m.addVertex(at(outer, a0, y));
                    m.addVertex(at(outer, a1, y));
                    m.addVertex(at(inner, a1, y));
                }
                const auto q = [&](int a, int b, int c, int d) {
                    m.addFace({base + static_cast<std::uint32_t>(a), base + static_cast<std::uint32_t>(b),
                               base + static_cast<std::uint32_t>(c), base + static_cast<std::uint32_t>(d)});
                };
                q(0, 1, 2, 3);
                q(4, 7, 6, 5);
                q(0, 4, 5, 1);
                q(1, 5, 6, 2);
                q(2, 6, 7, 3);
                q(3, 7, 4, 0);
            }
            ops::conformNormals(m);
            return m;
        }
        case Kind::Arch: {
            // Perfil en (x, y): arco exterior y vuelta por el interior; fondo en Z.
            const float outer = size.x * 0.5f;
            const float inner = std::clamp(outer - p.thickness, 1e-3f, outer * 0.98f);
            const float sweep = core::radians(std::clamp(p.angle, 1.0f, 359.0f));
            const float start = (core::kPi - sweep) * 0.5f;
            const float ky = size.y > 0.0f ? size.y / outer : 1.0f;  // alto del arco (eliptico)
            std::vector<Vec2> outline;
            for (int i = 0; i <= segments; ++i) {
                const float a = start + sweep * static_cast<float>(i) / static_cast<float>(segments);
                outline.push_back({outer * std::cos(a), outer * std::sin(a) * ky});
            }
            for (int i = segments; i >= 0; --i) {
                const float a = start + sweep * static_cast<float>(i) / static_cast<float>(segments);
                outline.push_back({inner * std::cos(a), inner * std::sin(a) * ky});
            }
            PolyMesh m = extrudeProfile(outline, Vec3{0.0f, 0.0f, -size.z * 0.5f}, Vec3{1.0f, 0.0f, 0.0f},
                                        Vec3{0.0f, 1.0f, 0.0f}, Vec3{0.0f, 0.0f, 1.0f}, size.z);
            // El arco de un solo poligono: mejor en quads por segmento.
            return m;
        }
        case Kind::Door: {
            const float hx = size.x * 0.5f, h = std::max(size.y, 1e-3f);
            const float dw = std::clamp(p.door.x, 0.02f, 0.98f) * size.x * 0.5f;
            const float dh = std::clamp(p.door.y, 0.02f, 0.98f) * h;
            return extrudeProfile({{-hx, 0.0f}, {-dw, 0.0f}, {-dw, dh}, {dw, dh}, {dw, 0.0f}, {hx, 0.0f}, {hx, h}, {-hx, h}},
                                  Vec3{0.0f, 0.0f, -size.z * 0.5f}, Vec3{1.0f, 0.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f},
                                  Vec3{0.0f, 0.0f, 1.0f}, size.z);
        }
        case Kind::Room: {
            PolyMesh m = box(Vec3{size.x, std::max(size.y, 1e-3f), size.z}, p.subdivisions_x, p.subdivisions_y, p.subdivisions_z);
            std::vector<int> all(m.faces.size());
            for (std::size_t i = 0; i < all.size(); ++i) all[i] = static_cast<int>(i);
            ops::flipFaces(m, all);
            return m;
        }
        case Kind::Capsule: {
            const float h = std::max(size.y, size.x);
            const float r = size.x * 0.5f;
            const int rings = std::clamp(p.rings, 2, 64);
            std::vector<ProfilePoint> profile;
            for (int k = 0; k <= rings; ++k) {
                const float phi = 0.5f * core::kPi * static_cast<float>(k) / static_cast<float>(rings);
                profile.push_back({k == 0 ? 0.0f : std::sin(phi), (r - r * std::cos(phi)) / h});
            }
            // Si no hay cilindro en medio (alto = diametro), el ecuador no se repite.
            for (int k = h > 2.0f * r + 1e-4f ? rings : rings - 1; k >= 0; --k) {
                const float phi = 0.5f * core::kPi * static_cast<float>(k) / static_cast<float>(rings);
                profile.push_back({k == 0 ? 0.0f : std::sin(phi), (h - r + r * std::cos(phi)) / h});
            }
            return lathe(profile, false, segments, false, p.smooth ? 1 : 0, Vec3{size.x, h, size.z});
        }
        case Kind::Count: break;
    }
    return box(Vec3{1.0f, 1.0f, 1.0f}, 1, 1, 1);
}

PolyMesh extrudePolygon(const std::vector<Vec2>& outline, float height, bool flip) {
    PolyMesh m = extrudeProfile(outline, Vec3{}, Vec3{1.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, 1.0f}, Vec3{0.0f, 1.0f, 0.0f},
                                std::max(height, 1e-4f));
    if (flip) {
        std::vector<int> all(m.faces.size());
        for (std::size_t i = 0; i < all.size(); ++i) all[i] = static_cast<int>(i);
        ops::flipFaces(m, all);
    }
    return m;
}

}  // namespace cramion::modeling::shapes
