#ifndef CRAMION_ASSET_HOUSE_GEO_H
#define CRAMION_ASSET_HOUSE_GEO_H

// Geometria compartida de los edificios procedurales (casas, edificios del
// pueblo medieval, murallas, muebles y objetos): caras con las UV en metros
// del material, cajas orientadas, cilindros y conos, troncos, paredes con
// huecos, tejados a dos aguas y un kit de muebles. Interno de CramionFX (lo
// usan HouseGenerator.cpp y MedievalBuildings.cpp).

#include "CramionFX/asset/HouseGenerator.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

namespace cramion::asset::housegeo {

using core::Vec2;
using core::Vec3;
using core::Vec4;

inline constexpr float kPi = 3.14159265f;
inline const Vec3 kUp{0.0f, 1.0f, 0.0f};
inline const Vec3 kX{1.0f, 0.0f, 0.0f};
inline const Vec3 kZ{0.0f, 0.0f, 1.0f};

inline float smoothstep(float a, float b, float x) {
    const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
inline float mix(float a, float b, float t) { return a + (b - a) * t; }
inline Vec3 mix(const Vec3& a, const Vec3& b, float t) { return a + (b - a) * t; }

struct Rng {
    std::mt19937 engine;
    explicit Rng(std::uint32_t seed) : engine(seed * 2654435761U + 12345U) {}
    float range(float a, float b) { return std::uniform_real_distribution<float>(a, b)(engine); }
    bool chance(float p) { return range(0.0f, 1.0f) < p; }
    int pick(int n) { return n <= 1 ? 0 : std::min(static_cast<int>(range(0.0f, static_cast<float>(n))), n - 1); }
};

// ---------------------------------------------------------------------------
// Malla por materiales
// ---------------------------------------------------------------------------

struct Geo {
    std::array<std::vector<SkinnedVertex>, kHouseMaterialCount> vertices;
    std::array<std::vector<std::uint32_t>, kHouseMaterialCount> indices;
    std::vector<Vec3> lights;  // fuegos (hogares, fraguas): donde va la luz

    std::size_t triangles() const {
        std::size_t n = 0;
        for (const auto& i : indices) n += i.size() / 3;
        return n;
    }

    Vec2 uvAt(int m, const Vec3& p, const Vec3& su, const Vec3& sv) const {
        const std::array<float, 2> meters = houseTextureMeters(m);
        return Vec2{core::dot(p, su) / meters[0], core::dot(p, sv) / meters[1]};
    }

    // Poligono convexo (abanico) con una normal por vertice; se orienta para
    // que la cara mire hacia `facing`.
    void polygon(int m, const std::vector<Vec3>& p, const std::vector<Vec3>& n, const std::vector<Vec2>& uv,
                 const Vec3& facing) {
        auto& verts = vertices[static_cast<std::size_t>(m)];
        auto& idx = indices[static_cast<std::size_t>(m)];
        const auto base = static_cast<std::uint32_t>(verts.size());
        for (std::size_t i = 0; i < p.size(); ++i) {
            SkinnedVertex v;
            v.position = p[i];
            v.normal = n[i];
            v.uv = uv[i];
            v.weights[0] = 1.0f;
            verts.push_back(v);
        }
        Vec3 area{};
        for (std::size_t i = 1; i + 1 < p.size(); ++i) area += core::cross(p[i] - p[0], p[i + 1] - p[0]);
        const bool flip = core::dot(area, facing) < 0.0f;
        for (std::uint32_t i = 1; i + 1 < p.size(); ++i) {
            if (flip) {
                idx.insert(idx.end(), {base, base + i + 1, base + i});
            } else {
                idx.insert(idx.end(), {base, base + i, base + i + 1});
            }
        }
    }

    // Cara plana con UV proyectadas en el mundo (su = eje de U, sv = eje de V).
    void flat(int m, const std::vector<Vec3>& p, const Vec3& normal, const Vec3& su, const Vec3& sv,
              Vec2 offset = {}) {
        std::vector<Vec3> n(p.size(), normal);
        std::vector<Vec2> uv;
        uv.reserve(p.size());
        for (const Vec3& q : p) {
            Vec2 t = uvAt(m, q, su, sv);
            uv.push_back(Vec2{t.x + offset.x, t.y + offset.y});
        }
        polygon(m, p, n, uv, normal);
    }

    // Caja orientada: centro, medias medidas y ejes (ortonormales). `rotate`
    // gira la veta 90 grados. `skip`: bits de caras que no se crean
    // (1 -X, 2 +X, 4 -Y, 8 +Y, 16 -Z, 32 +Z).
    void box(int m, const Vec3& c, const Vec3& half, const Vec3& ax, const Vec3& ay, const Vec3& az,
             bool rotate = false, int skip = 0, Vec2 offset = {}) {
        const Vec3 axes[3] = {ax, ay, az};
        const float h[3] = {half.x, half.y, half.z};
        for (int a = 0; a < 3; ++a) {
            for (int s = 0; s < 2; ++s) {
                if (skip & (1 << (a * 2 + s))) continue;
                const Vec3 n = axes[a] * (s == 0 ? -1.0f : 1.0f);
                const Vec3 e1 = axes[(a + 1) % 3] * h[(a + 1) % 3];
                const Vec3 e2 = axes[(a + 2) % 3] * h[(a + 2) % 3];
                const Vec3 fc = c + n * h[a];
                const std::vector<Vec3> p = {fc - e1 - e2, fc + e1 - e2, fc + e1 + e2, fc - e1 + e2};
                // Ejes de las UV: en las caras verticales, U horizontal y V hacia
                // abajo; en las horizontales, los ejes X/Z de la caja.
                Vec3 su;
                Vec3 sv;
                if (std::abs(n.y) > 0.7f) {
                    su = ax;
                    sv = az;
                } else {
                    Vec3 horizontal = core::normalize(core::cross(kUp, n));
                    if (!std::isfinite(horizontal.x)) horizontal = ax;
                    su = horizontal;
                    sv = core::normalize(core::cross(n, su)) * -1.0f;
                }
                if (rotate) std::swap(su, sv);
                flat(m, p, n, su, sv, offset);
            }
        }
    }
    void box(int m, const Vec3& c, const Vec3& half, bool rotate = false, int skip = 0, Vec2 offset = {}) {
        box(m, c, half, kX, kUp, kZ, rotate, skip, offset);
    }
    // Caja entre dos esquinas (alineada con los ejes).
    void boxMinMax(int m, const Vec3& lo, const Vec3& hi, bool rotate = false, int skip = 0) {
        box(m, (lo + hi) * 0.5f, (hi - lo) * 0.5f, rotate, skip);
    }
    // Viga (caja larga) de `a` a `b` con seccion w x h; `up` orienta el canto.
    void beam(int m, const Vec3& a, const Vec3& b, float w, float h, const Vec3& up = kUp) {
        const Vec3 d = b - a;
        const float len = core::length(d);
        if (len < 1e-4f) return;
        const Vec3 az = d * (1.0f / len);
        Vec3 ax = core::cross(up, az);
        if (core::length(ax) < 1e-3f) ax = core::cross(kX, az);
        ax = core::normalize(ax);
        const Vec3 ay = core::cross(az, ax);
        box(m, (a + b) * 0.5f, Vec3{w * 0.5f, h * 0.5f, len * 0.5f}, ax, ay, az, true);
    }

    // Tronco de cono (o cilindro si r0 == r1, cono si r1 == 0) de `a` a `b`.
    // U alrededor (perimetro en metros del material), V a lo largo. `inward`
    // = las caras miran hacia dentro (el interior de un pozo).
    void frustum(int m, const Vec3& a, const Vec3& b, float r0, float r1, int sides, bool cap_a, bool cap_b,
                 bool inward = false, float u_offset = 0.0f, int cap_m = -1, float twist = 0.0f) {
        const Vec3 axis = b - a;
        const float len = core::length(axis);
        if (len < 1e-5f || sides < 3) return;
        const Vec3 d = axis * (1.0f / len);
        Vec3 e1 = core::cross(d, kUp);
        if (core::length(e1) < 1e-3f) e1 = kX;
        e1 = core::normalize(e1);
        const Vec3 e2 = core::cross(e1, d);
        const std::array<float, 2> meters = houseTextureMeters(m);
        const float rmax = std::max(r0, r1);
        const float slope = r0 - r1;
        const float sign = inward ? -1.0f : 1.0f;
        for (int k = 0; k < sides; ++k) {
            const float a0 = twist + 2.0f * kPi * static_cast<float>(k) / static_cast<float>(sides);
            const float a1 = twist + 2.0f * kPi * static_cast<float>(k + 1) / static_cast<float>(sides);
            const Vec3 q0 = e1 * std::cos(a0) + e2 * std::sin(a0);
            const Vec3 q1 = e1 * std::cos(a1) + e2 * std::sin(a1);
            const Vec3 n0 = core::normalize(q0 * len + d * slope) * sign;
            const Vec3 n1 = core::normalize(q1 * len + d * slope) * sign;
            const float u0 = (2.0f * kPi * rmax * static_cast<float>(k) / static_cast<float>(sides)) / meters[0] + u_offset;
            const float u1 = (2.0f * kPi * rmax * static_cast<float>(k + 1) / static_cast<float>(sides)) / meters[0] + u_offset;
            const float v1 = len / meters[1];
            polygon(m, {a + q0 * r0, a + q1 * r0, b + q1 * r1, b + q0 * r1}, {n0, n1, n1, n0},
                    {Vec2{u0, v1}, Vec2{u1, v1}, Vec2{u1, 0.0f}, Vec2{u0, 0.0f}}, (n0 + n1) * 0.5f);
        }
        const int cm = cap_m < 0 ? m : cap_m;
        for (int end = 0; end < 2; ++end) {
            if ((end == 0 && !cap_a) || (end == 1 && !cap_b)) continue;
            const float r = end == 0 ? r0 : r1;
            if (r < 1e-4f) continue;
            const Vec3 center = end == 0 ? a : b;
            const Vec3 n = (end == 0 ? d * -1.0f : d) * sign;
            std::vector<Vec3> p;
            std::vector<Vec2> uv;
            const std::array<float, 2> cmeters = houseTextureMeters(cm);
            for (int k = 0; k < sides; ++k) {
                const float ang = twist + 2.0f * kPi * static_cast<float>(k) / static_cast<float>(sides);
                const Vec3 q = e1 * std::cos(ang) + e2 * std::sin(ang);
                p.push_back(center + q * r);
                if (cm == kHouseLogEnds) {
                    uv.push_back(Vec2{0.5f + 0.48f * std::cos(ang), 0.5f + 0.48f * std::sin(ang)});
                } else {
                    uv.push_back(Vec2{core::dot(q * r, e1) / cmeters[0], core::dot(q * r, e2) / cmeters[1]});
                }
            }
            polygon(cm, p, std::vector<Vec3>(p.size(), n), uv, n);
        }
    }
    void cylinder(int m, const Vec3& a, const Vec3& b, float r, int sides, bool cap_a, bool cap_b, int cap_m = -1) {
        frustum(m, a, b, r, r, sides, cap_a, cap_b, false, 0.0f, cap_m);
    }

    // Tronco: cilindro de `a` a `b` con testas (caps) opcionales.
    void log(const Vec3& a, const Vec3& b, float r, int sides, bool cap_a, bool cap_b, float u_offset,
             float twist) {
        const Vec3 d = core::normalize(b - a);
        Vec3 e1 = core::cross(d, kUp);
        if (core::length(e1) < 1e-3f) e1 = kX;
        e1 = core::normalize(e1);
        const Vec3 e2 = core::cross(e1, d);
        const float len = core::length(b - a);
        const std::array<float, 2> meters = houseTextureMeters(kHouseLogs);
        const float u0 = core::dot(a, d) / meters[0] + u_offset;
        const float u1 = u0 + len / meters[0];
        for (int k = 0; k < sides; ++k) {
            const float a0 = twist + 2.0f * kPi * static_cast<float>(k) / static_cast<float>(sides);
            const float a1 = twist + 2.0f * kPi * static_cast<float>(k + 1) / static_cast<float>(sides);
            const Vec3 n0 = e1 * std::cos(a0) + e2 * std::sin(a0);
            const Vec3 n1 = e1 * std::cos(a1) + e2 * std::sin(a1);
            const float v0 = static_cast<float>(k) / static_cast<float>(sides);
            const float v1 = static_cast<float>(k + 1) / static_cast<float>(sides);
            polygon(kHouseLogs, {a + n0 * r, b + n0 * r, b + n1 * r, a + n1 * r}, {n0, n0, n1, n1},
                    {Vec2{u0, v0}, Vec2{u1, v0}, Vec2{u1, v1}, Vec2{u0, v1}}, (n0 + n1) * 0.5f);
        }
        for (int end = 0; end < 2; ++end) {
            if ((end == 0 && !cap_a) || (end == 1 && !cap_b)) continue;
            const Vec3 center = end == 0 ? a : b;
            const Vec3 n = end == 0 ? d * -1.0f : d;
            std::vector<Vec3> p;
            std::vector<Vec2> uv;
            const float spin = u_offset * 7.0f;
            for (int k = 0; k < sides; ++k) {
                const float ang = twist + 2.0f * kPi * static_cast<float>(k) / static_cast<float>(sides);
                p.push_back(center + (e1 * std::cos(ang) + e2 * std::sin(ang)) * r);
                uv.push_back(Vec2{0.5f + 0.48f * std::cos(ang + spin), 0.5f + 0.48f * std::sin(ang + spin)});
            }
            polygon(kHouseLogEnds, p, std::vector<Vec3>(p.size(), n), uv, n);
        }
    }

    // Copia otra malla desplazada y girada en Y (grados).
    void append(const Geo& other, const Vec3& offset, float yaw_degrees = 0.0f) {
        const float a = yaw_degrees * kPi / 180.0f;
        const float c = std::cos(a);
        const float s = std::sin(a);
        const auto rot = [&](const Vec3& v) { return Vec3{c * v.x + s * v.z, v.y, -s * v.x + c * v.z}; };
        for (int m = 0; m < kHouseMaterialCount; ++m) {
            auto& verts = vertices[static_cast<std::size_t>(m)];
            auto& idx = indices[static_cast<std::size_t>(m)];
            const auto base = static_cast<std::uint32_t>(verts.size());
            for (SkinnedVertex v : other.vertices[static_cast<std::size_t>(m)]) {
                v.position = rot(v.position) + offset;
                v.normal = rot(v.normal);
                verts.push_back(v);
            }
            for (std::uint32_t i : other.indices[static_cast<std::size_t>(m)]) idx.push_back(base + i);
        }
        for (const Vec3& l : other.lights) lights.push_back(rot(l) + offset);
    }

    // Gira toda la malla en Y (grados) alrededor del origen.
    void rotateY(float yaw_degrees) {
        Geo copy = *this;
        for (auto& v : vertices) v.clear();
        for (auto& i : indices) i.clear();
        lights.clear();
        append(copy, Vec3{}, yaw_degrees);
    }

    void finish(ModelData& model, const std::string& name) const {
        model.name = name;
        model.materials.clear();
        for (int m = 0; m < kHouseMaterialCount; ++m) {
            MaterialData mat;
            mat.name = houseMaterialName(m);
            switch (m) {
                case kHouseLogs: mat.base_color = Vec4{0.45f, 0.31f, 0.18f, 1.0f}; mat.roughness = 0.85f; break;
                case kHouseLogEnds: mat.base_color = Vec4{0.55f, 0.42f, 0.27f, 1.0f}; mat.roughness = 0.85f; break;
                case kHousePlanks: mat.base_color = Vec4{0.42f, 0.36f, 0.3f, 1.0f}; mat.roughness = 0.85f; break;
                case kHouseSiding: mat.base_color = Vec4{0.8f, 0.79f, 0.75f, 1.0f}; mat.roughness = 0.6f; break;
                case kHouseStone: mat.base_color = Vec4{0.5f, 0.49f, 0.46f, 1.0f}; mat.roughness = 0.9f; break;
                case kHouseRoof: mat.base_color = Vec4{0.33f, 0.29f, 0.25f, 1.0f}; mat.roughness = 0.9f; break;
                case kHouseTrim: mat.base_color = Vec4{0.85f, 0.84f, 0.8f, 1.0f}; mat.roughness = 0.5f; break;
                case kHouseGlass:
                    mat.base_color = Vec4{0.02f, 0.025f, 0.03f, 1.0f};
                    mat.roughness = 0.04f;
                    mat.reflectance = 0.06f;
                    break;
                case kHouseIron: mat.base_color = Vec4{0.1f, 0.1f, 0.1f, 1.0f}; mat.metallic = 1.0f; mat.roughness = 0.6f; break;
                case kHousePlaster: mat.base_color = Vec4{0.82f, 0.78f, 0.7f, 1.0f}; mat.roughness = 0.92f; break;
                case kHouseThatch: mat.base_color = Vec4{0.55f, 0.46f, 0.3f, 1.0f}; mat.roughness = 0.95f; break;
                case kHouseTile: mat.base_color = Vec4{0.5f, 0.24f, 0.16f, 1.0f}; mat.roughness = 0.8f; break;
                case kHouseCloth: mat.base_color = Vec4{0.6f, 0.2f, 0.16f, 1.0f}; mat.roughness = 0.95f; break;
                case kHouseBeams: mat.base_color = Vec4{0.22f, 0.15f, 0.1f, 1.0f}; mat.roughness = 0.8f; break;
                default: break;
            }
            model.materials.push_back(mat);
        }
        model.vertices.clear();
        model.indices.clear();
        model.submeshes.clear();
        for (int m = 0; m < kHouseMaterialCount; ++m) {
            const auto& verts = vertices[static_cast<std::size_t>(m)];
            const auto& idx = indices[static_cast<std::size_t>(m)];
            if (idx.empty()) continue;
            const auto base = static_cast<std::uint32_t>(model.vertices.size());
            const auto first = static_cast<std::uint32_t>(model.indices.size());
            // Tangentes (mismo convenio que el lector de OBJ: V hacia abajo).
            std::vector<Vec3> tangents(verts.size(), Vec3{});
            std::vector<Vec3> bitangents(verts.size(), Vec3{});
            for (std::size_t i = 0; i + 2 < idx.size(); i += 3) {
                const SkinnedVertex& va = verts[idx[i]];
                const SkinnedVertex& vb = verts[idx[i + 1]];
                const SkinnedVertex& vc = verts[idx[i + 2]];
                const Vec3 e1 = vb.position - va.position;
                const Vec3 e2 = vc.position - va.position;
                const float du1 = vb.uv.x - va.uv.x;
                const float dv1 = vb.uv.y - va.uv.y;
                const float du2 = vc.uv.x - va.uv.x;
                const float dv2 = vc.uv.y - va.uv.y;
                const float det = du1 * dv2 - du2 * dv1;
                if (std::abs(det) < 1e-12f) continue;
                const float r = 1.0f / det;
                const Vec3 t = (e1 * dv2 - e2 * dv1) * r;
                const Vec3 bt = (e2 * du1 - e1 * du2) * r;
                for (std::uint32_t k : {idx[i], idx[i + 1], idx[i + 2]}) {
                    tangents[k] += t;
                    bitangents[k] += bt;
                }
            }
            SubMesh sub;
            sub.first_index = first;
            sub.index_count = static_cast<std::uint32_t>(idx.size());
            sub.material = static_cast<std::uint32_t>(m);
            sub.bounds_min = Vec3{1e9f, 1e9f, 1e9f};
            sub.bounds_max = Vec3{-1e9f, -1e9f, -1e9f};
            for (std::size_t k = 0; k < verts.size(); ++k) {
                SkinnedVertex v = verts[k];
                Vec3 t = tangents[k] - v.normal * core::dot(v.normal, tangents[k]);
                if (core::length(t) < 1e-9f) {
                    v.tangent = Vec4{1.0f, 0.0f, 0.0f, 1.0f};
                } else {
                    t = core::normalize(t);
                    const float w = core::dot(core::cross(v.normal, t), bitangents[k]) < 0.0f ? -1.0f : 1.0f;
                    v.tangent = Vec4{t, w};
                }
                sub.bounds_min = Vec3{std::min(sub.bounds_min.x, v.position.x), std::min(sub.bounds_min.y, v.position.y),
                                      std::min(sub.bounds_min.z, v.position.z)};
                sub.bounds_max = Vec3{std::max(sub.bounds_max.x, v.position.x), std::max(sub.bounds_max.y, v.position.y),
                                      std::max(sub.bounds_max.z, v.position.z)};
                model.vertices.push_back(v);
            }
            for (std::uint32_t i : idx) model.indices.push_back(base + i);
            model.submeshes.push_back(sub);
        }
        model.nodes = {Node{name, -1, core::Mat4::identity()}};
        model.bones = {Bone{name, 0, core::Mat4::identity()}};
    }
};

// Caja envolvente de una malla terminada.
inline void modelBounds(const ModelData& model, Vec3& lo, Vec3& hi) {
    lo = Vec3{1e9f, 1e9f, 1e9f};
    hi = Vec3{-1e9f, -1e9f, -1e9f};
    for (const SkinnedVertex& v : model.vertices) {
        lo = Vec3{std::min(lo.x, v.position.x), std::min(lo.y, v.position.y), std::min(lo.z, v.position.z)};
        hi = Vec3{std::max(hi.x, v.position.x), std::max(hi.y, v.position.y), std::max(hi.z, v.position.z)};
    }
    if (model.vertices.empty()) lo = hi = Vec3{};
}

// ---------------------------------------------------------------------------
// Paredes
// ---------------------------------------------------------------------------

// Hueco en una pared: s = a lo largo (desde el centro de la pared), y = alturas.
struct Opening {
    int wall = 0;
    float s0 = 0.0f;
    float s1 = 0.0f;
    float y0 = 0.0f;
    float y1 = 0.0f;
    bool door = false;
};

// Una pared: centro de su linea, eje a lo largo y normal hacia fuera.
struct Wall {
    Vec3 center{};
    Vec3 along{};
    Vec3 out{};
    float half = 0.0f;  // media longitud de la linea (entre ejes de esquina)
};

inline Vec3 at(const Wall& w, float s, float y, float d) { return w.center + w.along * s + w.out * d + kUp * y; }

// Las cuatro paredes de un rectangulo de W (X) x D (Z) centrado en el origen:
// 0 delante (+Z), 1 atras, 2 derecha (+X), 3 izquierda.
inline std::array<Wall, 4> rectWalls(float W, float D) {
    return {Wall{Vec3{0.0f, 0.0f, D * 0.5f}, kX, kZ, W * 0.5f},
            Wall{Vec3{0.0f, 0.0f, -D * 0.5f}, kX * -1.0f, kZ * -1.0f, W * 0.5f},
            Wall{Vec3{W * 0.5f, 0.0f, 0.0f}, kZ * -1.0f, kX, D * 0.5f},
            Wall{Vec3{-W * 0.5f, 0.0f, 0.0f}, kZ, kX * -1.0f, D * 0.5f}};
}

// Caja en coordenadas de una pared (s, y, d): de lo a hi.
inline void wallBox(Geo& g, int m, const Wall& w, float s0, float s1, float y0, float y1, float d0, float d1,
                    bool rotate = false) {
    const Vec3 c = at(w, (s0 + s1) * 0.5f, (y0 + y1) * 0.5f, (d0 + d1) * 0.5f);
    g.box(m, c, Vec3{std::abs(s1 - s0) * 0.5f, std::abs(y1 - y0) * 0.5f, std::abs(d1 - d0) * 0.5f}, w.along, kUp, w.out,
          rotate);
}

// Pared maciza de grosor `thick` de s0 a s1 y de y0 a y1 con los huecos de
// `openings` que son de la pared `wall` (con sus mochetas). `top`/`ends`:
// crear el remate de arriba y los cantos de los extremos.
inline void wallPanel(Geo& g, int m, const Wall& w, float s0, float s1, float y0, float y1, float thick,
                      const std::vector<Opening>& openings, int wall, bool top = true, bool ends = true,
                      int inner_m = -1) {
    const float h = thick * 0.5f;
    const int im = inner_m < 0 ? m : inner_m;
    std::vector<float> xs = {s0, s1};
    std::vector<float> ys = {y0, y1};
    std::vector<const Opening*> holes;
    for (const Opening& o : openings) {
        if (o.wall != wall || o.s1 <= s0 || o.s0 >= s1 || o.y1 <= y0 || o.y0 >= y1) continue;
        holes.push_back(&o);
        xs.push_back(std::clamp(o.s0, s0, s1));
        xs.push_back(std::clamp(o.s1, s0, s1));
        ys.push_back(std::clamp(o.y0, y0, y1));
        ys.push_back(std::clamp(o.y1, y0, y1));
    }
    std::sort(xs.begin(), xs.end());
    xs.erase(std::unique(xs.begin(), xs.end()), xs.end());
    std::sort(ys.begin(), ys.end());
    ys.erase(std::unique(ys.begin(), ys.end()), ys.end());
    const Vec3 sv = kUp * -1.0f;
    for (std::size_t i = 0; i + 1 < xs.size(); ++i) {
        for (std::size_t j = 0; j + 1 < ys.size(); ++j) {
            const float cx = (xs[i] + xs[i + 1]) * 0.5f;
            const float cy = (ys[j] + ys[j + 1]) * 0.5f;
            bool hole = false;
            for (const Opening* o : holes) hole = hole || (cx > o->s0 && cx < o->s1 && cy > o->y0 && cy < o->y1);
            if (hole) continue;
            for (int side = 0; side < 2; ++side) {
                const float d = side == 0 ? h : -h;
                const Vec3 n = w.out * (side == 0 ? 1.0f : -1.0f);
                g.flat(side == 0 ? m : im,
                       {at(w, xs[i], ys[j], d), at(w, xs[i + 1], ys[j], d), at(w, xs[i + 1], ys[j + 1], d),
                        at(w, xs[i], ys[j + 1], d)},
                       n, w.along, sv);
            }
        }
    }
    // Mochetas de los huecos (el grosor de la pared).
    for (const Opening* o : holes) {
        const float ya = std::max(o->y0, y0);
        const float yb = std::min(o->y1, y1);
        if (yb <= ya) continue;
        if (o->s0 > s0) {
            g.flat(m, {at(w, o->s0, ya, h), at(w, o->s0, ya, -h), at(w, o->s0, yb, -h), at(w, o->s0, yb, h)}, w.along,
                   w.out, sv);
        }
        if (o->s1 < s1) {
            g.flat(m, {at(w, o->s1, ya, h), at(w, o->s1, ya, -h), at(w, o->s1, yb, -h), at(w, o->s1, yb, h)},
                   w.along * -1.0f, w.out, sv);
        }
        const float sa = std::max(o->s0, s0);
        const float sb = std::min(o->s1, s1);
        if (o->y1 < y1) {
            g.flat(m, {at(w, sa, o->y1, h), at(w, sb, o->y1, h), at(w, sb, o->y1, -h), at(w, sa, o->y1, -h)}, kUp * -1.0f,
                   w.along, w.out);
        }
        if (o->y0 > y0 && !o->door) {
            g.flat(m, {at(w, sa, o->y0, h), at(w, sb, o->y0, h), at(w, sb, o->y0, -h), at(w, sa, o->y0, -h)}, kUp, w.along,
                   w.out);
        }
    }
    if (top) {
        g.flat(m, {at(w, s0, y1, h), at(w, s1, y1, h), at(w, s1, y1, -h), at(w, s0, y1, -h)}, kUp, w.along, w.out);
    }
    if (ends) {
        g.flat(m, {at(w, s0, y0, h), at(w, s0, y0, -h), at(w, s0, y1, -h), at(w, s0, y1, h)}, w.along * -1.0f, w.out, sv);
        g.flat(m, {at(w, s1, y0, h), at(w, s1, y0, -h), at(w, s1, y1, -h), at(w, s1, y1, h)}, w.along, w.out, sv);
    }
}

// Hastial: triangulo de pared (dos caras y los cantos inclinados) sobre la
// linea de la pared, de -half a +half en y0 hasta el pico en y1.
inline void gableWall(Geo& g, int m, const Wall& w, float half, float y0, float y1, float thick, int inner_m = -1) {
    const int im = inner_m < 0 ? m : inner_m;
    for (int side = 0; side < 2; ++side) {
        const float d = side == 0 ? thick * 0.5f : -thick * 0.5f;
        const Vec3 n = w.out * (side == 0 ? 1.0f : -1.0f);
        g.flat(side == 0 ? m : im, {at(w, -half, y0, d), at(w, half, y0, d), at(w, 0.0f, y1, d)}, n, w.along, kUp * -1.0f);
    }
}

// Marco de ventana con vidrio y cruceta, alfeizar y dintel (o tapajuntas).
inline void windowFrame(Geo& g, const Wall& w, const Opening& o, float thick, float face, bool stone_lintel,
                        bool shutters, int frame_m = kHouseTrim) {
    const float h = thick * 0.5f;
    const float fw = 0.07f;
    const float fd0 = -h + 0.02f;
    const float fd1 = std::min(-h + 0.2f, h + 0.02f);
    wallBox(g, frame_m, w, o.s0, o.s0 + fw, o.y0, o.y1, fd0, fd1);
    wallBox(g, frame_m, w, o.s1 - fw, o.s1, o.y0, o.y1, fd0, fd1);
    wallBox(g, frame_m, w, o.s0 + fw, o.s1 - fw, o.y1 - fw, o.y1, fd0, fd1);
    wallBox(g, frame_m, w, o.s0 + fw, o.s1 - fw, o.y0, o.y0 + fw, fd0, fd1);
    const float gd = (fd0 + fd1) * 0.5f;
    const float gs0 = o.s0 + fw;
    const float gs1 = o.s1 - fw;
    const float gy0 = o.y0 + fw;
    const float gy1 = o.y1 - fw;
    for (int side = 0; side < 2; ++side) {
        const Vec3 n = w.out * (side == 0 ? 1.0f : -1.0f);
        g.flat(kHouseGlass, {at(w, gs0, gy0, gd), at(w, gs1, gy0, gd), at(w, gs1, gy1, gd), at(w, gs0, gy1, gd)}, n, w.along,
               kUp * -1.0f);
    }
    // Emplomado: cruceta y, en las altas, mas travesanos.
    const float mid_s = (o.s0 + o.s1) * 0.5f;
    wallBox(g, frame_m, w, mid_s - 0.02f, mid_s + 0.02f, gy0, gy1, gd - 0.025f, gd + 0.025f);
    const int bars = std::max(1, static_cast<int>((gy1 - gy0) / 0.45f));
    for (int i = 1; i <= bars; ++i) {
        const float yy = gy0 + (gy1 - gy0) * static_cast<float>(i) / static_cast<float>(bars + 1);
        wallBox(g, frame_m, w, gs0, gs1, yy - 0.018f, yy + 0.018f, gd - 0.022f, gd + 0.022f);
    }
    wallBox(g, frame_m, w, o.s0 - 0.07f, o.s1 + 0.07f, o.y0 - 0.05f, o.y0, fd1 - 0.04f, face + 0.07f);
    if (stone_lintel) {
        wallBox(g, kHouseStone, w, o.s0 - 0.16f, o.s1 + 0.16f, o.y1, o.y1 + 0.2f, h - 0.04f, h + 0.02f);
    } else {
        const float cd0 = face - 0.01f;
        const float cd1 = face + 0.025f;
        wallBox(g, frame_m, w, o.s0 - 0.09f, o.s0, o.y0, o.y1, cd0, cd1);
        wallBox(g, frame_m, w, o.s1, o.s1 + 0.09f, o.y0, o.y1, cd0, cd1);
        wallBox(g, frame_m, w, o.s0 - 0.12f, o.s1 + 0.12f, o.y1, o.y1 + 0.12f, cd0, cd1 + 0.01f);
    }
    if (shutters) {
        const float sw = (o.s1 - o.s0) * 0.5f;
        const float d0 = face + 0.03f;
        wallBox(g, kHousePlanks, w, o.s0 - 0.11f - sw, o.s0 - 0.11f, o.y0 + 0.02f, o.y1 - 0.02f, d0, d0 + 0.035f);
        wallBox(g, kHousePlanks, w, o.s1 + 0.11f, o.s1 + 0.11f + sw, o.y0 + 0.02f, o.y1 - 0.02f, d0, d0 + 0.035f);
    }
}

// Saetera / ventana estrecha sin vidrio (torres, murallas): solo el hueco
// con mochetas lo hace wallPanel; aqui un derrame de piedra alrededor.
inline void slitFrame(Geo& g, const Wall& w, const Opening& o, float thick) {
    const float h = thick * 0.5f;
    wallBox(g, kHouseStone, w, o.s0 - 0.1f, o.s1 + 0.1f, o.y0 - 0.12f, o.y0, h - 0.02f, h + 0.06f);
    wallBox(g, kHouseStone, w, o.s0 - 0.1f, o.s1 + 0.1f, o.y1, o.y1 + 0.14f, h - 0.02f, h + 0.06f);
}

// Hoja de puerta de tablas con la bisagra en su origen (se abre girando en
// Y): travesanos, herrajes y tirador. dw x dh, grosor 0.055.
inline void doorLeaf(Geo& g, float dw, float dh) {
    const float thick = 0.055f;
    g.box(kHousePlanks, Vec3{dw * 0.5f, dh * 0.5f, 0.0f}, Vec3{dw * 0.5f, dh * 0.5f, thick * 0.5f}, kX, kUp, kZ);
    for (float yy : {0.35f, dh - 0.35f}) {
        g.box(kHousePlanks, Vec3{dw * 0.5f, yy, thick * 0.5f + 0.012f}, Vec3{dw * 0.5f - 0.03f, 0.07f, 0.012f}, kX, kUp, kZ,
              true);
        g.box(kHouseIron, Vec3{dw * 0.3f, yy, thick * 0.5f + 0.027f}, Vec3{dw * 0.3f, 0.022f, 0.004f}, kX, kUp, kZ);
    }
    g.box(kHousePlanks, Vec3{dw * 0.5f, dh * 0.5f, thick * 0.5f + 0.012f}, Vec3{0.07f, std::max(dh * 0.5f - 0.42f, 0.05f), 0.012f},
          kX, kUp, kZ);
    for (float side : {1.0f, -1.0f}) {
        const float z = side * (thick * 0.5f + 0.03f);
        g.box(kHouseIron, Vec3{dw - 0.1f, 1.0f, z}, Vec3{0.012f, 0.09f, 0.012f}, kX, kUp, kZ);
        g.box(kHouseIron, Vec3{dw - 0.1f, 1.0f, z * 0.8f}, Vec3{0.03f, 0.03f, 0.008f}, kX, kUp, kZ);
    }
}

// Tejado a dos aguas generico: cumbrera a lo largo de `along` (unitario,
// horizontal) por `center` (x, z), de -half_len a +half_len; los faldones
// bajan hacia +/-`across` hasta `half_span`. La cara de abajo esta a `ridge_y`
// en la cumbrera y baja con `tan` por metro. `th` = grosor.
struct GableRoof {
    Vec3 center{};
    Vec3 along = kX;
    float half_len = 4.0f;
    float half_span = 3.0f;
    float ridge_y = 5.0f;
    float tan = 0.8f;
    float th = 0.16f;
    int top_m = kHouseRoof;
    int under_m = kHousePlanks;
    int trim_m = kHouseTrim;
    bool ridge_cap = true;
    bool barge = true;  // tablas de canto en los hastiales
};

inline void gableRoof(Geo& g, const GableRoof& r) {
    const Vec3 along = core::normalize(Vec3{r.along.x, 0.0f, r.along.z});
    const Vec3 across = core::cross(kUp, along) * -1.0f;  // +across = un faldon
    const Vec3 c{r.center.x, 0.0f, r.center.z};
    const float ye = r.ridge_y - r.half_span * r.tan;
    const std::array<float, 2> rm = houseTextureMeters(r.top_m);
    for (float side : {1.0f, -1.0f}) {
        const Vec3 n = core::normalize(kUp + across * (r.tan * side));
        const Vec3 up = n * r.th;
        const Vec3 r0 = c + along * -r.half_len + kUp * r.ridge_y;
        const Vec3 r1 = c + along * r.half_len + kUp * r.ridge_y;
        const Vec3 e0 = c + along * -r.half_len + across * (r.half_span * side) + kUp * ye;
        const Vec3 e1 = c + along * r.half_len + across * (r.half_span * side) + kUp * ye;
        const Vec3 down = core::normalize(e0 - r0);
        const float slope = core::length(e0 - r0);
        const float u0 = -r.half_len / rm[0];
        const float u1 = r.half_len / rm[0];
        const float vo = side > 0.0f ? 0.0f : 0.37f;
        g.polygon(r.top_m, {r0 + up, r1 + up, e1 + up, e0 + up}, {n, n, n, n},
                  {Vec2{u0, vo}, Vec2{u1, vo}, Vec2{u1, vo + slope / rm[1]}, Vec2{u0, vo + slope / rm[1]}}, n);
        g.flat(r.under_m, {r0, r1, e1, e0}, n * -1.0f, along, down);
        // Frente del alero y cantos.
        g.flat(r.trim_m, {e0, e1, e1 + up, e0 + up}, across * side, along, kUp * -1.0f);
        for (float xs : {-1.0f, 1.0f}) {
            const Vec3 a = c + along * (r.half_len * xs) + kUp * r.ridge_y;
            const Vec3 b = c + along * (r.half_len * xs) + across * (r.half_span * side) + kUp * ye;
            if (r.barge) {
                const Vec3 bc = (a + b) * 0.5f + up * 0.5f - n * 0.05f + along * (xs * 0.02f);
                g.box(r.trim_m, bc, Vec3{0.025f, r.th * 0.5f + 0.06f, core::length(b - a) * 0.5f + 0.02f}, along, n, down, true);
            } else {
                g.flat(r.top_m, {a, b, b + up, a + up}, along * xs, across, kUp * -1.0f);
            }
        }
    }
    if (r.ridge_cap) {
        const float top = r.ridge_y + r.th * std::sqrt(1.0f + r.tan * r.tan);
        const float d = std::max(r.th * 0.8f, 0.13f);
        const Vec3 ay = core::normalize(kUp + across);
        const Vec3 az = core::normalize(across - kUp);
        g.box(r.top_m, c + kUp * (top - d * 0.35f), Vec3{r.half_len + 0.03f, d, d}, along, ay, az, false, 0);
    }
}

// Tejado a cuatro aguas en piramide (torres, campanarios): base cuadrada de
// medio lado `half` a la altura y0, pico a y1. Con su alero y cara de abajo.
inline void pyramidRoof(Geo& g, int m, const Vec3& c, float half_x, float half_z, float y0, float y1, int under_m = kHousePlanks) {
    const Vec3 apex = c + kUp * y1;
    const Vec3 corners[4] = {c + Vec3{-half_x, y0, -half_z}, c + Vec3{half_x, y0, -half_z}, c + Vec3{half_x, y0, half_z},
                             c + Vec3{-half_x, y0, half_z}};
    const std::array<float, 2> rm = houseTextureMeters(m);
    for (int i = 0; i < 4; ++i) {
        const Vec3 a = corners[i];
        const Vec3 b = corners[(i + 1) % 4];
        const Vec3 mid = (a + b) * 0.5f;
        Vec3 out = Vec3{mid.x - c.x, 0.0f, mid.z - c.z};
        out = core::normalize(out);
        const Vec3 n = core::normalize(core::cross(b - a, apex - a)) * (core::dot(core::cross(b - a, apex - a), out) < 0.0f ? -1.0f : 1.0f);
        const float slope = core::length(apex - mid);
        const float w = core::length(b - a);
        g.polygon(m, {a, b, apex}, {n, n, n},
                  {Vec2{-w * 0.5f / rm[0], slope / rm[1]}, Vec2{w * 0.5f / rm[0], slope / rm[1]}, Vec2{0.0f, 0.0f}}, n);
    }
    g.flat(under_m, {corners[0], corners[1], corners[2], corners[3]}, kUp * -1.0f, kX, kZ);
}

// Almenas a lo largo de una linea de pared: parapeto de `thick` en el borde
// de fuera (d de d0 a d0 + thick) de altura `h` con merlones de `merlon`
// metros y huecos de `gap`.
inline void battlements(Geo& g, const Wall& w, float s0, float s1, float y, float h, float d0, float thick,
                        float merlon = 0.7f, float gap = 0.5f, int m = kHouseStone) {
    const float lower = h * 0.45f;
    wallBox(g, m, w, s0, s1, y, y + lower, d0, d0 + thick);
    const float len = s1 - s0;
    const int count = std::max(1, static_cast<int>(std::floor((len + gap) / (merlon + gap))));
    const float used = static_cast<float>(count) * merlon + static_cast<float>(count - 1) * gap;
    float s = s0 + (len - used) * 0.5f;
    for (int i = 0; i < count; ++i) {
        wallBox(g, m, w, s, s + merlon, y + lower, y + h, d0, d0 + thick);
        s += merlon + gap;
    }
}

// ---------------------------------------------------------------------------
// Muebles (kit para los interiores)
// ---------------------------------------------------------------------------

// Marco local de un mueble: origen en el suelo, +Z hacia donde "mira" (el
// lado del que se usa: el frente del armario, la cabecera de la cama atras).
struct Frame {
    Vec3 o{};
    Vec3 x = kX;
    Vec3 z = kZ;
    Vec3 p(float lx, float ly, float lz) const { return o + x * lx + kUp * ly + z * lz; }
};
// facing: 0 = mira a +Z, 1 = +X, 2 = -Z, 3 = -X.
inline Frame frameAt(const Vec3& o, int facing) {
    Frame f;
    f.o = o;
    switch (((facing % 4) + 4) % 4) {
        case 0: f.z = kZ; f.x = kX; break;
        case 1: f.z = kX; f.x = kZ * -1.0f; break;
        case 2: f.z = kZ * -1.0f; f.x = kX * -1.0f; break;
        default: f.z = kX * -1.0f; f.x = kZ; break;
    }
    return f;
}
inline void fbox(Geo& g, int m, const Frame& f, const Vec3& c, const Vec3& half, bool rotate = false, int skip = 0) {
    g.box(m, f.p(c.x, c.y, c.z), half, f.x, kUp, f.z, rotate, skip);
}

inline void table(Geo& g, const Frame& f, float w, float d, float h = 0.76f) {
    fbox(g, kHousePlanks, f, Vec3{0.0f, h - 0.025f, 0.0f}, Vec3{w * 0.5f, 0.025f, d * 0.5f}, false);
    for (float sx : {-1.0f, 1.0f}) {
        for (float sz : {-1.0f, 1.0f}) {
            fbox(g, kHouseBeams, f, Vec3{sx * (w * 0.5f - 0.08f), (h - 0.05f) * 0.5f, sz * (d * 0.5f - 0.08f)},
                 Vec3{0.035f, (h - 0.05f) * 0.5f, 0.035f}, true, 4 | 8);
        }
    }
    fbox(g, kHouseBeams, f, Vec3{0.0f, h - 0.11f, 0.0f}, Vec3{w * 0.5f - 0.1f, 0.04f, 0.02f});
}

inline void bench(Geo& g, const Frame& f, float length, float h = 0.45f) {
    fbox(g, kHousePlanks, f, Vec3{0.0f, h - 0.02f, 0.0f}, Vec3{length * 0.5f, 0.02f, 0.15f});
    for (float sx : {-1.0f, 1.0f}) {
        fbox(g, kHousePlanks, f, Vec3{sx * (length * 0.5f - 0.15f), (h - 0.04f) * 0.5f, 0.0f}, Vec3{0.025f, (h - 0.04f) * 0.5f, 0.13f},
             true, 4);
    }
}

inline void stool(Geo& g, const Frame& f) {
    g.cylinder(kHousePlanks, f.p(0.0f, 0.42f, 0.0f), f.p(0.0f, 0.46f, 0.0f), 0.17f, 10, false, true);
    for (int i = 0; i < 3; ++i) {
        const float a = 2.0f * kPi * static_cast<float>(i) / 3.0f;
        const Vec3 top = f.p(std::cos(a) * 0.1f, 0.42f, std::sin(a) * 0.1f);
        const Vec3 bottom = f.p(std::cos(a) * 0.15f, 0.0f, std::sin(a) * 0.15f);
        g.beam(kHouseBeams, bottom, top, 0.035f, 0.035f);
    }
}

inline void chair(Geo& g, const Frame& f) {
    fbox(g, kHousePlanks, f, Vec3{0.0f, 0.44f, 0.0f}, Vec3{0.21f, 0.02f, 0.21f});
    for (float sx : {-1.0f, 1.0f}) {
        for (float sz : {-1.0f, 1.0f}) {
            const float h = sz < 0.0f ? 0.98f : 0.42f;
            fbox(g, kHouseBeams, f, Vec3{sx * 0.18f, h * 0.5f, sz * 0.18f}, Vec3{0.022f, h * 0.5f, 0.022f}, true, 4);
        }
    }
    fbox(g, kHousePlanks, f, Vec3{0.0f, 0.82f, -0.18f}, Vec3{0.17f, 0.11f, 0.015f});
}

// Cama: cabecera hacia -Z local (contra la pared), w x l.
inline void bed(Geo& g, const Frame& f, float w = 1.0f, float l = 2.0f) {
    const float hl = l * 0.5f;
    const float hw = w * 0.5f;
    // Largueros y patas.
    for (float sx : {-1.0f, 1.0f}) {
        fbox(g, kHousePlanks, f, Vec3{sx * (hw - 0.03f), 0.28f, 0.0f}, Vec3{0.03f, 0.1f, hl}, true);
    }
    fbox(g, kHousePlanks, f, Vec3{0.0f, 0.28f, hl - 0.03f}, Vec3{hw, 0.1f, 0.03f});
    fbox(g, kHouseBeams, f, Vec3{0.0f, 0.5f, -hl + 0.04f}, Vec3{hw + 0.04f, 0.5f, 0.04f});   // cabecero
    fbox(g, kHouseBeams, f, Vec3{0.0f, 0.3f, hl - 0.04f}, Vec3{hw + 0.04f, 0.3f, 0.04f});   // pie
    for (float sx : {-1.0f, 1.0f}) fbox(g, kHouseBeams, f, Vec3{sx * hw, 0.1f, 0.0f}, Vec3{0.04f, 0.1f, 0.04f}, true, 4);
    // Colchon, manta y almohada.
    fbox(g, kHousePlaster, f, Vec3{0.0f, 0.44f, 0.0f}, Vec3{hw - 0.05f, 0.07f, hl - 0.08f}, false, 4);
    fbox(g, kHouseCloth, f, Vec3{0.0f, 0.515f, 0.18f}, Vec3{hw - 0.02f, 0.012f, hl - 0.33f}, false, 4);
    fbox(g, kHouseCloth, f, Vec3{-hw + 0.04f, 0.4f, 0.18f}, Vec3{0.01f, 0.12f, hl - 0.33f}, true, 0);
    fbox(g, kHouseCloth, f, Vec3{hw - 0.04f, 0.4f, 0.18f}, Vec3{0.01f, 0.12f, hl - 0.33f}, true, 0);
    fbox(g, kHousePlaster, f, Vec3{0.0f, 0.55f, -hl + 0.3f}, Vec3{hw - 0.15f, 0.05f, 0.16f}, false, 4);
}

inline void chest(Geo& g, const Frame& f, float w = 0.9f, float d = 0.5f, float h = 0.52f) {
    fbox(g, kHousePlanks, f, Vec3{0.0f, h * 0.45f, 0.0f}, Vec3{w * 0.5f, h * 0.45f, d * 0.5f});
    fbox(g, kHousePlanks, f, Vec3{0.0f, h * 0.95f, 0.0f}, Vec3{w * 0.5f + 0.01f, h * 0.05f + 0.01f, d * 0.5f + 0.01f}, true);
    for (float sx : {-0.7f, 0.7f}) {
        fbox(g, kHouseIron, f, Vec3{sx * w * 0.5f, h * 0.5f, 0.0f}, Vec3{0.025f, h * 0.5f + 0.012f, d * 0.5f + 0.012f});
    }
    fbox(g, kHouseIron, f, Vec3{0.0f, h * 0.78f, d * 0.5f + 0.012f}, Vec3{0.05f, 0.06f, 0.01f});
}

// Barril de duelas con aros: radio de la panza `r`, alto `h`, de pie.
inline void barrel(Geo& g, const Vec3& base, float r = 0.3f, float h = 0.85f, float spin = 0.0f) {
    const float r0 = r * 0.84f;
    const Vec3 a = base;
    const Vec3 m = base + kUp * (h * 0.5f);
    const Vec3 b = base + kUp * h;
    g.frustum(kHousePlanks, a, m, r0, r, 14, true, false, false, spin, kHouseLogEnds);
    g.frustum(kHousePlanks, m, b, r, r0, 14, false, true, false, spin, kHouseLogEnds);
    for (float t : {0.08f, 0.3f, 0.7f, 0.92f}) {
        const float rr = r0 + (r - r0) * (1.0f - std::abs(t - 0.5f) * 2.0f) + 0.008f;
        g.frustum(kHouseIron, base + kUp * (h * t - 0.025f), base + kUp * (h * t + 0.025f), rr, rr, 14, false, false);
    }
}
// Barril tumbado (en una pila, una taberna): eje a lo largo de `dir`.
inline void barrelLying(Geo& g, const Vec3& center, const Vec3& dir, float r = 0.3f, float l = 0.85f) {
    const Vec3 d = core::normalize(dir);
    const float r0 = r * 0.84f;
    const Vec3 a = center - d * (l * 0.5f);
    const Vec3 b = center + d * (l * 0.5f);
    g.frustum(kHousePlanks, a, center, r0, r, 14, true, false, false, 0.0f, kHouseLogEnds);
    g.frustum(kHousePlanks, center, b, r, r0, 14, false, true, false, 0.0f, kHouseLogEnds);
    for (float t : {0.1f, 0.9f}) {
        const float rr = r0 + (r - r0) * (1.0f - std::abs(t - 0.5f) * 2.0f) + 0.008f;
        g.frustum(kHouseIron, a + d * (l * t - 0.025f), a + d * (l * t + 0.025f), rr, rr, 14, false, false);
    }
}

inline void crate(Geo& g, const Frame& f, float s = 0.6f, float y = 0.0f) {
    const float h = s * 0.5f;
    fbox(g, kHousePlanks, f, Vec3{0.0f, y + h, 0.0f}, Vec3{h, h, h});
    // Listones de las aristas.
    for (float sx : {-1.0f, 1.0f}) {
        for (float sz : {-1.0f, 1.0f}) {
            fbox(g, kHouseBeams, f, Vec3{sx * (h - 0.02f), y + h, sz * (h - 0.02f)}, Vec3{0.03f, h + 0.005f, 0.03f}, true);
        }
    }
    fbox(g, kHouseBeams, f, Vec3{0.0f, y + s - 0.02f, 0.0f}, Vec3{h + 0.01f, 0.025f, 0.035f});
}

inline void shelf(Geo& g, const Frame& f, float w, float h, float d, Rng& rng) {
    for (float sx : {-1.0f, 1.0f}) fbox(g, kHousePlanks, f, Vec3{sx * (w * 0.5f - 0.02f), h * 0.5f, 0.0f}, Vec3{0.02f, h * 0.5f, d * 0.5f}, true);
    const int boards = std::max(2, static_cast<int>(h / 0.42f));
    for (int i = 0; i <= boards; ++i) {
        const float y = 0.08f + (h - 0.1f) * static_cast<float>(i) / static_cast<float>(boards);
        fbox(g, kHousePlanks, f, Vec3{0.0f, y, 0.0f}, Vec3{w * 0.5f - 0.04f, 0.015f, d * 0.5f});
        if (i == 0 || i == boards) continue;
        // Cosas en la balda: tarros, cuencos, cajas.
        float x = -w * 0.5f + 0.12f;
        while (x < w * 0.5f - 0.12f) {
            const float kind = rng.range(0.0f, 1.0f);
            const float sz = rng.range(0.06f, 0.11f);
            if (kind < 0.5f) {
                g.frustum(kind < 0.25f ? kHouseStone : kHouseIron, f.p(x, y + 0.015f, 0.0f), f.p(x, y + 0.015f + sz * 2.2f, 0.0f),
                          sz, sz * 0.7f, 8, false, true);
            } else if (kind < 0.8f) {
                fbox(g, kHousePlanks, f, Vec3{x, y + 0.015f + sz, 0.0f}, Vec3{sz, sz, sz * 0.9f});
            }
            x += sz * 2.0f + rng.range(0.05f, 0.18f);
        }
    }
}

inline void cupboard(Geo& g, const Frame& f, float w, float h, float d) {
    fbox(g, kHousePlanks, f, Vec3{0.0f, h * 0.5f, 0.0f}, Vec3{w * 0.5f, h * 0.5f, d * 0.5f});
    fbox(g, kHousePlanks, f, Vec3{0.0f, h + 0.02f, 0.0f}, Vec3{w * 0.5f + 0.03f, 0.02f, d * 0.5f + 0.03f}, true);
    // Dos puertas con sus tiradores.
    for (float sx : {-1.0f, 1.0f}) {
        fbox(g, kHouseBeams, f, Vec3{sx * w * 0.25f, h * 0.52f, d * 0.5f + 0.008f}, Vec3{w * 0.25f - 0.03f, h * 0.42f, 0.008f});
        fbox(g, kHouseIron, f, Vec3{sx * 0.05f, h * 0.55f, d * 0.5f + 0.022f}, Vec3{0.012f, 0.05f, 0.01f});
    }
}

inline void rug(Geo& g, const Frame& f, float w, float l) {
    fbox(g, kHouseCloth, f, Vec3{0.0f, 0.006f, 0.0f}, Vec3{w * 0.5f, 0.006f, l * 0.5f}, false, 4);
}

// Hogar de piedra contra una pared (el frente mira a +Z local): boca con
// lenos, campana hasta `ceiling`. La luz del fuego va en g.lights.
inline void fireplace(Geo& g, const Frame& f, float w, float ceiling, bool logs = true) {
    const float d = 0.65f;
    const float mouth_h = 0.95f;
    // Losa del hogar (sale hacia la sala), mejillas, dintel y fondo.
    fbox(g, kHouseStone, f, Vec3{0.0f, 0.06f, 0.18f}, Vec3{w * 0.5f + 0.15f, 0.06f, d * 0.5f + 0.18f});
    for (float sx : {-1.0f, 1.0f}) {
        fbox(g, kHouseStone, f, Vec3{sx * (w * 0.5f - 0.13f), mouth_h * 0.5f, 0.0f}, Vec3{0.13f, mouth_h * 0.5f, d * 0.5f});
    }
    fbox(g, kHouseBeams, f, Vec3{0.0f, mouth_h + 0.09f, d * 0.5f - 0.1f}, Vec3{w * 0.5f + 0.05f, 0.09f, 0.11f}, false);
    fbox(g, kHouseStone, f, Vec3{0.0f, mouth_h * 0.5f, -d * 0.5f + 0.05f}, Vec3{w * 0.5f - 0.26f, mouth_h * 0.5f, 0.05f});
    // Campana que se estrecha hasta el techo.
    const float hood_top = std::max(ceiling, mouth_h + 0.6f);
    fbox(g, kHouseStone, f, Vec3{0.0f, mouth_h + 0.35f, -0.02f}, Vec3{w * 0.5f, 0.17f, d * 0.5f - 0.02f}, false, 4);
    fbox(g, kHousePlaster, f, Vec3{0.0f, (mouth_h + 0.52f + hood_top) * 0.5f, -0.1f},
         Vec3{w * 0.33f, (hood_top - mouth_h - 0.52f) * 0.5f, d * 0.5f - 0.1f}, false, 4 | 8);
    if (logs) {
        g.log(f.p(-0.28f, 0.2f, 0.02f), f.p(0.25f, 0.2f, 0.1f), 0.06f, 7, true, true, 0.2f, 0.0f);
        g.log(f.p(-0.2f, 0.2f, 0.16f), f.p(0.3f, 0.2f, 0.0f), 0.055f, 7, true, true, 0.6f, 0.4f);
        g.log(f.p(-0.1f, 0.3f, 0.06f), f.p(0.12f, 0.3f, 0.12f), 0.05f, 7, true, true, 0.9f, 0.8f);
        // Morillos de hierro.
        for (float sx : {-0.22f, 0.22f}) fbox(g, kHouseIron, f, Vec3{sx, 0.16f, 0.06f}, Vec3{0.015f, 0.04f, 0.18f});
    }
    g.lights.push_back(f.p(0.0f, 0.45f, 0.35f));
}

// Escalera recta: sube hacia +Z local desde el origen (pie) hasta (0, rise, run).
inline void stairs(Geo& g, const Frame& f, float rise, float run, float width) {
    const int steps = std::max(3, static_cast<int>(std::round(rise / 0.19f)));
    const float sr = rise / static_cast<float>(steps);
    const float sd = run / static_cast<float>(steps);
    for (int i = 0; i < steps; ++i) {
        const float y = sr * static_cast<float>(i + 1);
        const float z = sd * (static_cast<float>(i) + 0.5f);
        fbox(g, kHousePlanks, f, Vec3{0.0f, y - 0.025f, z}, Vec3{width * 0.5f, 0.025f, sd * 0.5f + 0.03f}, true);
        // Tabica.
        fbox(g, kHousePlanks, f, Vec3{0.0f, y - sr * 0.5f, z - sd * 0.5f + 0.012f}, Vec3{width * 0.5f - 0.04f, sr * 0.5f - 0.02f, 0.012f});
    }
    // Zancas inclinadas.
    for (float sx : {-1.0f, 1.0f}) {
        g.beam(kHouseBeams, f.p(sx * width * 0.5f, 0.0f, 0.0f), f.p(sx * width * 0.5f, rise, run), 0.05f, 0.24f,
               core::normalize(f.z * -sd + kUp * sd));
    }
}

// Barra de taberna (frente hacia +Z local), largo `length`.
inline void counter(Geo& g, const Frame& f, float length) {
    fbox(g, kHousePlanks, f, Vec3{0.0f, 0.5f, 0.0f}, Vec3{length * 0.5f, 0.5f, 0.28f});
    fbox(g, kHouseBeams, f, Vec3{0.0f, 1.04f, 0.04f}, Vec3{length * 0.5f + 0.05f, 0.04f, 0.36f}, true);
    for (float x = -length * 0.5f + 0.4f; x < length * 0.5f - 0.2f; x += 0.8f) {
        fbox(g, kHouseBeams, f, Vec3{x, 0.5f, 0.29f}, Vec3{0.04f, 0.48f, 0.02f});
    }
}

// Banco de iglesia con respaldo (mira a +Z local).
inline void pew(Geo& g, const Frame& f, float length) {
    bench(g, f, length, 0.46f);
    fbox(g, kHousePlanks, f, Vec3{0.0f, 0.75f, -0.16f}, Vec3{length * 0.5f, 0.17f, 0.02f});
    for (float sx : {-1.0f, 1.0f}) fbox(g, kHouseBeams, f, Vec3{sx * length * 0.5f, 0.48f, -0.02f}, Vec3{0.03f, 0.48f, 0.2f});
}

inline void anvil(Geo& g, const Frame& f) {
    g.log(f.p(0.0f, 0.0f, 0.0f), f.p(0.0f, 0.5f, 0.0f), 0.22f, 10, false, true, 0.4f, 0.0f);
    fbox(g, kHouseIron, f, Vec3{0.0f, 0.56f, 0.0f}, Vec3{0.1f, 0.06f, 0.12f});
    fbox(g, kHouseIron, f, Vec3{0.0f, 0.72f, 0.0f}, Vec3{0.07f, 0.1f, 0.08f});
    fbox(g, kHouseIron, f, Vec3{0.04f, 0.86f, 0.0f}, Vec3{0.2f, 0.05f, 0.08f});
    g.frustum(kHouseIron, f.p(-0.16f, 0.86f, 0.0f), f.p(-0.42f, 0.88f, 0.0f), 0.05f, 0.01f, 8, false, false);
}

// Fragua de piedra (frente hacia +Z local) con su campana y chimenea hasta
// `chimney_top`.
inline void forge(Geo& g, const Frame& f, float chimney_top) {
    fbox(g, kHouseStone, f, Vec3{0.0f, 0.42f, 0.0f}, Vec3{0.7f, 0.42f, 0.55f});
    fbox(g, kHouseStone, f, Vec3{0.0f, 0.86f, -0.3f}, Vec3{0.7f, 0.02f, 0.25f});
    // Brasas (vidrio oscuro brillante no: hierro) y carbon.
    fbox(g, kHouseIron, f, Vec3{0.0f, 0.86f, 0.15f}, Vec3{0.35f, 0.03f, 0.25f}, false, 4);
    for (float sx : {-1.0f, 1.0f}) fbox(g, kHouseStone, f, Vec3{sx * 0.62f, 1.25f, -0.25f}, Vec3{0.08f, 0.4f, 0.3f});
    fbox(g, kHouseStone, f, Vec3{0.0f, 1.8f, -0.2f}, Vec3{0.72f, 0.2f, 0.38f});
    g.frustum(kHouseStone, f.p(0.0f, 2.0f, -0.25f), f.p(0.0f, 2.7f, -0.3f), 0.5f, 0.3f, 4, false, false, false, 0.0f, -1, kPi * 0.25f);
    fbox(g, kHouseStone, f, Vec3{0.0f, (2.7f + chimney_top) * 0.5f, -0.3f}, Vec3{0.25f, (chimney_top - 2.7f) * 0.5f, 0.25f}, false, 4);
    // Fuelle.
    fbox(g, kHouseBeams, f, Vec3{0.95f, 0.75f, -0.1f}, Vec3{0.18f, 0.06f, 0.35f});
    g.lights.push_back(f.p(0.0f, 1.1f, 0.25f));
}

// Monton de paja / heno.
inline void hay(Geo& g, const Frame& f, float w, float h, float d, float y = 0.0f) {
    fbox(g, kHouseThatch, f, Vec3{0.0f, y + h * 0.5f, 0.0f}, Vec3{w * 0.5f, h * 0.5f, d * 0.5f}, true, y > 0.0f ? 0 : 4);
}

// Valla de madera de `a` a `b` (postes con dos travesanos), y = el suelo en cada punto.
inline void fence(Geo& g, const Vec3& a, const Vec3& b, float post_gap = 2.2f) {
    const Vec3 d{b.x - a.x, 0.0f, b.z - a.z};
    const float len = core::length(d);
    if (len < 0.3f) return;
    const int posts = std::max(1, static_cast<int>(std::ceil(len / post_gap)));
    for (int i = 0; i <= posts; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(posts);
        const Vec3 p = a + (b - a) * t;
        g.beam(kHouseBeams, p - kUp * 0.3f, p + kUp * 1.05f, 0.1f, 0.1f, core::normalize(d));
    }
    for (float y : {0.45f, 0.9f}) g.beam(kHousePlanks, a + kUp * y, b + kUp * y, 0.04f, 0.11f);
}

// Carro de dos ruedas (eje en X local, varas hacia +Z).
inline void cart(Geo& g, const Frame& f) {
    fbox(g, kHousePlanks, f, Vec3{0.0f, 0.78f, -0.2f}, Vec3{0.65f, 0.04f, 1.0f}, true);
    for (float sx : {-1.0f, 1.0f}) fbox(g, kHousePlanks, f, Vec3{sx * 0.63f, 1.0f, -0.2f}, Vec3{0.025f, 0.2f, 1.0f}, true);
    fbox(g, kHousePlanks, f, Vec3{0.0f, 1.0f, -1.18f}, Vec3{0.65f, 0.2f, 0.025f});
    for (float sx : {-1.0f, 1.0f}) {
        g.beam(kHouseBeams, f.p(sx * 0.4f, 0.74f, 0.6f), f.p(sx * 0.45f, 0.35f, 2.2f), 0.06f, 0.06f);
        // Rueda: llanta (hierro), cubo y radios.
        const Vec3 hub = f.p(sx * 0.8f, 0.55f, -0.2f);
        g.frustum(kHouseIron, hub - f.x * 0.05f, hub + f.x * 0.05f, 0.55f, 0.55f, 18, false, false);
        g.frustum(kHousePlanks, hub - f.x * 0.045f, hub + f.x * 0.045f, 0.5f, 0.5f, 18, false, false, true);
        g.cylinder(kHouseBeams, hub - f.x * 0.09f, hub + f.x * 0.09f, 0.09f, 8, true, true);
        for (int k = 0; k < 8; ++k) {
            const float a = 2.0f * kPi * static_cast<float>(k) / 8.0f;
            const Vec3 dir = kUp * std::sin(a) + f.z * std::cos(a);
            g.beam(kHouseBeams, hub + dir * 0.08f, hub + dir * 0.5f, 0.035f, 0.035f, f.x);
        }
    }
    g.beam(kHouseBeams, f.p(-0.85f, 0.55f, -0.2f), f.p(0.85f, 0.55f, -0.2f), 0.07f, 0.07f);
}

// Pila de lena contra algo (frente +Z local).
inline void woodpile(Geo& g, const Frame& f, float w, float h, Rng& rng) {
    const float r = 0.08f;
    for (float y = r; y < h; y += r * 1.75f) {
        for (float x = -w * 0.5f + r; x < w * 0.5f - r * 0.5f; x += r * 2.05f) {
            const float jitter = rng.range(-0.04f, 0.04f);
            g.log(f.p(x, y, -0.25f + jitter), f.p(x, y, 0.25f + jitter), r * rng.range(0.85f, 1.1f), 6, true, true,
                  rng.range(0.0f, 1.0f), rng.range(0.0f, 6.28f));
        }
    }
}

}  // namespace cramion::asset::housegeo

#endif  // CRAMION_ASSET_HOUSE_GEO_H
