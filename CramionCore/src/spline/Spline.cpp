#include "CramionCore/spline/Spline.h"

#include "CramionCore/ecs/Components.h"
#include "CramionCore/ecs/MathUtil.h"
#include "CramionCore/ecs/RuntimeMesh.h"
#include "CramionCore/ecs/World.h"
#include "CramionCore/physics/PhysicsComponents.h"
#include "CramionCore/terrain/Terrain.h"
#include "CramionCore/terrain/TerrainTools.h"
#include "CramionCore/water/Water.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace cramion::spline {

namespace {

constexpr std::array<const char*, 3> kTypeNames = {"Catmull-Rom (pasa por los puntos)", "Bezier (suave)", "Lineal"};
constexpr std::array<const char*, 8> kShapeNames = {"Carretera", "Camino",  "Rio",     "Muro",
                                                    "Valla",     "Tuberia", "Railes", "Cinta"};
constexpr std::array<const char*, 3> kFollowNames = {"Bucle", "Ida y vuelta", "Una vez"};

const Vec3 kUp{0.0f, 1.0f, 0.0f};

Vec3 rotateAround(const Vec3& v, const Vec3& axis, float degrees) {
    const float a = core::radians(degrees);
    const float c = std::cos(a);
    const float s = std::sin(a);
    return v * c + core::cross(axis, v) * s + axis * (core::dot(axis, v) * (1.0f - c));
}

float smooth01(float t) { return t * t * (3.0f - 2.0f * t); }

// Punto de control i (cerrada: da la vuelta; abierta: refleja los extremos).
Vec3 controlPoint(const Spline& s, int i) {
    const int n = static_cast<int>(s.points.size());
    if (s.closed) return s.points[static_cast<std::size_t>(((i % n) + n) % n)].position;
    if (i < 0) return s.points[0].position * 2.0f - s.points[std::min(1, n - 1)].position;
    if (i >= n) return s.points[static_cast<std::size_t>(n - 1)].position * 2.0f -
                       s.points[static_cast<std::size_t>(std::max(n - 2, 0))].position;
    return s.points[static_cast<std::size_t>(i)].position;
}

const SplinePoint& pointAt(const Spline& s, int i) {
    const int n = static_cast<int>(s.points.size());
    return s.points[static_cast<std::size_t>(s.closed ? ((i % n) + n) % n : std::clamp(i, 0, n - 1))];
}

Vec3 tangentAt(const Spline& s, int i) {
    const Vec3 prev = controlPoint(s, i - 1);
    const Vec3 next = controlPoint(s, i + 1);
    if (s.type == SplineType::Bezier) {
        // Tangente automatica sin pasarse: la direccion de los vecinos con el
        // largo del tramo mas corto.
        const Vec3 p = controlPoint(s, i);
        const float shorter = std::min(core::length(p - prev), core::length(next - p));
        return core::normalize(next - prev) * (shorter * s.tension * 2.0f);
    }
    return (next - prev) * s.tension;
}

std::uint64_t hashFloat(std::uint64_t h, float f) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &f, sizeof(bits));
    return (h ^ bits) * 1099511628211ull;
}

}  // namespace

// -----------------------------------------------------------------------------
// Reflexion
// -----------------------------------------------------------------------------

void Spline::reflect(ecs::PropertyVisitor& v) {
    bool changed = false;
    changed |= ecs::listField(v, {"points", "Puntos", "Puntos de control (locales a la entidad)"}, points,
                              [](SplinePoint& p, ecs::PropertyVisitor& iv) {
                                  iv.field({"position", "Posicion"}, p.position, ecs::Vec3Kind::Position);
                                  iv.field({"width", "Ancho", "Multiplica el ancho de lo extruido"}, p.width,
                                           ecs::FloatRange{0.0f, 20.0f, 0.01f, "%.2f"});
                                  iv.field({"roll", "Giro", "Grados alrededor de la curva (peralte)"}, p.roll,
                                           ecs::FloatRange{-90.0f, 90.0f, 0.5f, "%.1f"});
                              });
    changed |= ecs::enumField(v, {"type", "Tipo"}, type, kTypeNames);
    changed |= v.field({"closed", "Cerrada", "Une el ultimo punto con el primero"}, closed);
    changed |= v.field({"tension", "Tension", "0 = rectas entre puntos, 1 = muy curva"}, tension,
                       ecs::FloatRange{0.0f, 1.0f, 0.01f, "%.2f", true});
    changed |= v.field({"resolution", "Resolucion", "Metros entre muestras de la curva"}, resolution,
                       ecs::FloatRange{0.05f, 20.0f, 0.05f, "%.2f m"});
    v.field({"show_gizmo", "Mostrar en la escena"}, show_gizmo);
    if (changed) markModified();
}

void SplineExtrude::reflect(ecs::PropertyVisitor& v) {
    bool changed = false;
    changed |= ecs::enumField(v, {"shape", "Forma"}, shape, kShapeNames);
    changed |= v.field({"width", "Ancho", "Metros (muro/valla: grosor; tuberia: diametro; railes: ancho de via)"},
                       width, ecs::FloatRange{0.01f, 200.0f, 0.05f, "%.2f m"});
    changed |= v.field({"height", "Alto", "Grosor de la calzada, alto del muro o valla, profundidad del rio"}, height,
                       ecs::FloatRange{0.0f, 50.0f, 0.01f, "%.2f m"});
    changed |= v.field({"uv_meters", "Metros por textura", "Repeticion de la textura a lo largo"}, uv_meters,
                       ecs::FloatRange{0.05f, 500.0f, 0.05f, "%.2f m"});
    changed |= v.field({"conform_to_terrain", "Pegar al terreno"}, conform_to_terrain);
    changed |= v.field({"ground_offset", "Altura sobre el terreno"}, ground_offset,
                       ecs::FloatRange{-10.0f, 10.0f, 0.01f, "%.2f m"});
    changed |= v.field({"post_spacing", "Separacion de postes", "Valla: postes; railes: traviesas"}, post_spacing,
                       ecs::FloatRange{0.1f, 50.0f, 0.05f, "%.2f m"});
    changed |= v.field({"collider", "Collider", "Anade un Mesh Collider con la malla"}, collider);
    changed |= v.field({"color", "Color", "Sin material .crmat en el Mesh Renderer"}, color, ecs::Vec3Kind::Color);
    changed |= v.field({"roughness", "Rugosidad"}, roughness, ecs::FloatRange{0.0f, 1.0f, 0.01f, "%.2f", true});
    changed |= v.field({"terrain_blend", "Talud en el terreno", "Metros de mezcla a cada lado al aplicar al terreno"},
                       terrain_blend, ecs::FloatRange{0.0f, 50.0f, 0.1f, "%.1f m"});
    changed |= v.field({"paint_layer", "Capa pintada", "-2 = automatica (camino/tierra/arena), -1 = no pintar"},
                       paint_layer, -2, terrain::kMaxLayers - 1);
    if (changed) markModified();
}

void SplineFollower::reflect(ecs::PropertyVisitor& v) {
    v.entity({"spline", "Spline", "Entidad con Spline (vacio = la del padre)"}, spline);
    v.field({"speed", "Velocidad"}, speed, ecs::FloatRange{-500.0f, 500.0f, 0.1f, "%.2f m/s"});
    v.field({"distance", "Distancia", "Posicion actual en la curva"}, distance, ecs::FloatRange{0.0f, 0.0f, 0.1f, "%.2f m"});
    ecs::enumField(v, {"mode", "Modo"}, mode, kFollowNames);
    v.field({"orient", "Mirar hacia delante"}, orient);
    v.field({"play_on_start", "Al empezar"}, play_on_start);
    v.field({"offset_side", "Desplazamiento lateral"}, offset_side, ecs::FloatRange{-100.0f, 100.0f, 0.05f, "%.2f m"});
    v.field({"offset_up", "Desplazamiento vertical"}, offset_up, ecs::FloatRange{-100.0f, 100.0f, 0.05f, "%.2f m"});
}

void registerSplineComponents() {
    auto& r = ecs::ComponentRegistry::instance();
    r.registerComponent<Spline>("Spline", "Spline", "Splines");
    r.registerComponent<SplineExtrude>("SplineExtrude", "Extrusion por spline (carretera, rio, muro...)", "Splines");
    r.registerComponent<SplineFollower>("SplineFollower", "Seguir spline", "Splines");
}

// -----------------------------------------------------------------------------
// Curva
// -----------------------------------------------------------------------------

int segmentCount(const Spline& spline) {
    const int n = static_cast<int>(spline.points.size());
    if (n < 2) return 0;
    return spline.closed ? n : n - 1;
}

Vec3 evaluateSegment(const Spline& s, int segment, float t) {
    const Vec3 p0 = controlPoint(s, segment);
    const Vec3 p1 = controlPoint(s, segment + 1);
    if (s.type == SplineType::Linear) return core::lerp(p0, p1, t);
    const Vec3 m0 = tangentAt(s, segment);
    const Vec3 m1 = tangentAt(s, segment + 1);
    const float t2 = t * t;
    const float t3 = t2 * t;
    return p0 * (2.0f * t3 - 3.0f * t2 + 1.0f) + m0 * (t3 - 2.0f * t2 + t) + p1 * (-2.0f * t3 + 3.0f * t2) +
           m1 * (t3 - t2);
}

void SplinePath::build(const Spline& spline, const Mat4& world) {
    samples_.clear();
    closed_ = spline.closed;
    const int segments = segmentCount(spline);
    if (segments == 0) {
        if (!spline.points.empty()) {
            SplineSample s;
            s.position = ecs::transformPoint(world, spline.points[0].position);
            samples_.push_back(s);
        }
        return;
    }
    const float step = std::max(spline.resolution, 0.05f);
    struct Raw {
        Vec3 p;
        float width;
        float roll;
    };
    std::vector<Raw> raw;
    for (int seg = 0; seg < segments; ++seg) {
        const SplinePoint& a = pointAt(spline, seg);
        const SplinePoint& b = pointAt(spline, seg + 1);
        const Vec3 wa = ecs::transformPoint(world, a.position);
        const Vec3 wb = ecs::transformPoint(world, b.position);
        int subdiv = 1;
        if (spline.type != SplineType::Linear) {
            // Largo aproximado (cuerda + algo por la curvatura).
            const Vec3 mid = ecs::transformPoint(world, evaluateSegment(spline, seg, 0.5f));
            const float approx = core::length(mid - wa) + core::length(wb - mid);
            subdiv = std::clamp(static_cast<int>(std::ceil(approx / step)), 1, 4096);
        } else {
            subdiv = std::clamp(static_cast<int>(std::ceil(core::length(wb - wa) / step)), 1, 4096);
        }
        for (int k = 0; k < subdiv; ++k) {
            const float t = static_cast<float>(k) / static_cast<float>(subdiv);
            const float e = smooth01(t);
            raw.push_back({ecs::transformPoint(world, evaluateSegment(spline, seg, t)),
                           a.width + (b.width - a.width) * e, a.roll + (b.roll - a.roll) * e});
        }
    }
    {
        const SplinePoint& last = pointAt(spline, segments);
        raw.push_back({ecs::transformPoint(world, evaluateSegment(spline, segments - 1, 1.0f)), last.width, last.roll});
    }
    samples_.resize(raw.size());
    float distance = 0.0f;
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (i > 0) distance += core::length(raw[i].p - raw[i - 1].p);
        SplineSample& s = samples_[i];
        s.position = raw[i].p;
        s.width = raw[i].width;
        s.distance = distance;
        Vec3 forward;
        if (i == 0) {
            forward = closed_ && raw.size() > 2 ? raw[1].p - raw[raw.size() - 2].p : raw[1].p - raw[0].p;
        } else if (i + 1 == raw.size()) {
            forward = closed_ && raw.size() > 2 ? raw[1].p - raw[i - 1].p : raw[i].p - raw[i - 1].p;
        } else {
            forward = raw[i + 1].p - raw[i - 1].p;
        }
        s.tangent = core::normalize(forward);
        if (core::length(s.tangent) < 0.5f) s.tangent = Vec3{0.0f, 0.0f, 1.0f};
        Vec3 right = core::cross(s.tangent, kUp);
        if (core::length(right) < 1e-4f) right = Vec3{1.0f, 0.0f, 0.0f};  // curva vertical
        right = core::normalize(right);
        Vec3 up = core::normalize(core::cross(right, s.tangent));
        if (std::abs(raw[i].roll) > 1e-3f) {
            right = core::normalize(rotateAround(right, s.tangent, raw[i].roll));
            up = core::normalize(rotateAround(up, s.tangent, raw[i].roll));
        }
        s.right = right;
        s.up = up;
    }
}

SplineSample SplinePath::atDistance(float distance) const {
    if (samples_.empty()) return {};
    if (samples_.size() == 1) return samples_[0];
    const float len = length();
    if (len <= 1e-6f) return samples_[0];
    if (closed_) {
        distance = std::fmod(distance, len);
        if (distance < 0.0f) distance += len;
    } else {
        distance = std::clamp(distance, 0.0f, len);
    }
    const auto it = std::lower_bound(samples_.begin(), samples_.end(), distance,
                                     [](const SplineSample& s, float d) { return s.distance < d; });
    if (it == samples_.begin()) return samples_.front();
    if (it == samples_.end()) return samples_.back();
    const SplineSample& b = *it;
    const SplineSample& a = *(it - 1);
    const float span = b.distance - a.distance;
    const float t = span > 1e-6f ? (distance - a.distance) / span : 0.0f;
    SplineSample r;
    r.position = core::lerp(a.position, b.position, t);
    r.tangent = core::normalize(core::lerp(a.tangent, b.tangent, t));
    r.right = core::normalize(core::lerp(a.right, b.right, t));
    r.up = core::normalize(core::lerp(a.up, b.up, t));
    r.width = a.width + (b.width - a.width) * t;
    r.distance = distance;
    return r;
}

float SplinePath::closestDistance(const Vec3& point, Vec3* closest) const {
    if (samples_.empty()) return 0.0f;
    float best = 1e30f;
    float best_distance = 0.0f;
    Vec3 best_point = samples_[0].position;
    for (std::size_t i = 0; i + 1 < samples_.size(); ++i) {
        const Vec3 a = samples_[i].position;
        const Vec3 ab = samples_[i + 1].position - a;
        const float len2 = core::dot(ab, ab);
        const float t = len2 > 1e-9f ? std::clamp(core::dot(point - a, ab) / len2, 0.0f, 1.0f) : 0.0f;
        const Vec3 p = a + ab * t;
        const Vec3 d = point - p;
        const float dist2 = core::dot(d, d);
        if (dist2 < best) {
            best = dist2;
            best_point = p;
            best_distance = samples_[i].distance + (samples_[i + 1].distance - samples_[i].distance) * t;
        }
    }
    if (samples_.size() == 1) best_point = samples_[0].position;
    if (closest != nullptr) *closest = best_point;
    return best_distance;
}

// -----------------------------------------------------------------------------
// Mallas
// -----------------------------------------------------------------------------

namespace {

struct Builder {
    std::vector<Vec3> vertices;
    std::vector<core::Vec2> uv;
    std::vector<std::vector<std::uint32_t>> tris;

    explicit Builder(int submeshes) : tris(static_cast<std::size_t>(submeshes)) {}

    std::uint32_t add(const Vec3& p, float u, float v) {
        vertices.push_back(p);
        uv.push_back({u, v});
        return static_cast<std::uint32_t>(vertices.size() - 1);
    }
    void tri(int sub, std::uint32_t a, std::uint32_t b, std::uint32_t c) {
        auto& t = tris[static_cast<std::size_t>(sub)];
        t.push_back(a);
        t.push_back(b);
        t.push_back(c);
    }

    // Caja orientada (ax, ay y az = cross(ax, ay) unitarios).
    void box(int sub, const Vec3& c, const Vec3& ax, const Vec3& ay, const Vec3& half, float uv_scale = 1.0f) {
        const Vec3 az = core::cross(ax, ay);
        struct F {
            Vec3 n, u, v;
            float hn, hu, hv;
        };
        const F faces[6] = {{ax, ay, az, half.x, half.y, half.z},    {ax * -1.0f, az, ay, half.x, half.z, half.y},
                            {ay, az, ax, half.y, half.z, half.x},    {ay * -1.0f, ax, az, half.y, half.x, half.z},
                            {az, ax, ay, half.z, half.x, half.y},    {az * -1.0f, ay, ax, half.z, half.y, half.x}};
        for (const F& f : faces) {
            const Vec3 o = c + f.n * f.hn;
            const float su = 2.0f * f.hu * uv_scale;
            const float sv = 2.0f * f.hv * uv_scale;
            const std::uint32_t i0 = add(o - f.u * f.hu - f.v * f.hv, 0.0f, 0.0f);
            const std::uint32_t i1 = add(o + f.u * f.hu - f.v * f.hv, su, 0.0f);
            const std::uint32_t i2 = add(o + f.u * f.hu + f.v * f.hv, su, sv);
            const std::uint32_t i3 = add(o - f.u * f.hu + f.v * f.hv, 0.0f, sv);
            tri(sub, i0, i1, i2);
            tri(sub, i0, i2, i3);
        }
    }

    // Barre un perfil (x = derecha, y = arriba; en sentido horario visto con
    // x a la derecha e y arriba -> caras hacia fuera) a lo largo de las
    // muestras. `smooth`: vertices compartidos entre tramos del perfil.
    void sweep(int sub, const std::vector<SplineSample>& samples, const std::vector<core::Vec2>& profile,
               bool closed_profile, bool smooth, float uv_meters, bool scale_with_width, float u_scale = -1.0f,
               float side = 0.0f) {
        if (samples.size() < 2 || profile.size() < 2) return;
        const std::size_t pn = profile.size();
        const std::size_t segs = closed_profile ? pn : pn - 1;
        std::vector<float> u_at(pn + 1, 0.0f);
        for (std::size_t k = 1; k <= pn; ++k) {
            const core::Vec2 a = profile[k - 1];
            const core::Vec2 b = profile[k % pn];
            u_at[k] = u_at[k - 1] + std::hypot(b.x - a.x, b.y - a.y);
        }
        const float u_norm = u_scale > 0.0f ? u_scale : 1.0f;
        const float inv_v = 1.0f / std::max(uv_meters, 0.01f);
        const auto place = [&](const SplineSample& s, core::Vec2 p) {
            const float w = scale_with_width ? s.width : 1.0f;
            return s.position + s.right * (p.x * w + side) + s.up * p.y;
        };
        if (smooth) {
            const std::size_t ring = pn + (closed_profile ? 1 : 0);
            const std::uint32_t base = static_cast<std::uint32_t>(vertices.size());
            for (const SplineSample& s : samples) {
                for (std::size_t k = 0; k < ring; ++k) add(place(s, profile[k % pn]), u_at[k] / u_norm, s.distance * inv_v);
            }
            for (std::size_t i = 0; i + 1 < samples.size(); ++i) {
                for (std::size_t k = 0; k < segs; ++k) {
                    const std::uint32_t a0 = base + static_cast<std::uint32_t>(i * ring + k);
                    const std::uint32_t b0 = a0 + 1;
                    const std::uint32_t a1 = a0 + static_cast<std::uint32_t>(ring);
                    const std::uint32_t b1 = a1 + 1;
                    tri(sub, a0, b0, a1);
                    tri(sub, b0, b1, a1);
                }
            }
            return;
        }
        for (std::size_t k = 0; k < segs; ++k) {
            const core::Vec2 pa = profile[k];
            const core::Vec2 pb = profile[(k + 1) % pn];
            const std::uint32_t base = static_cast<std::uint32_t>(vertices.size());
            for (const SplineSample& s : samples) {
                add(place(s, pa), u_at[k] / u_norm, s.distance * inv_v);
                add(place(s, pb), u_at[k + 1] / u_norm, s.distance * inv_v);
            }
            for (std::size_t i = 0; i + 1 < samples.size(); ++i) {
                const std::uint32_t a0 = base + static_cast<std::uint32_t>(i * 2);
                const std::uint32_t b0 = a0 + 1;
                const std::uint32_t a1 = a0 + 2;
                const std::uint32_t b1 = a0 + 3;
                tri(sub, a0, b0, a1);
                tri(sub, b0, b1, a1);
            }
        }
    }
};

ecs::MeshMaterial material(const Vec3& c, float roughness, float metallic = 0.0f) {
    ecs::MeshMaterial m;
    m.color = core::Vec4{c, 1.0f};
    m.roughness = roughness;
    m.metallic = metallic;
    return m;
}

// Muestras cada `spacing` metros (postes, traviesas).
std::vector<SplineSample> every(const std::vector<SplineSample>& samples, float spacing, bool closed) {
    std::vector<SplineSample> out;
    if (samples.size() < 2) return out;
    const float len = samples.back().distance;
    spacing = std::max(spacing, 0.05f);
    const int count = std::max(1, static_cast<int>(std::floor(len / spacing)));
    const float step = len / static_cast<float>(count);
    std::size_t j = 0;
    for (int i = 0; i <= count - (closed ? 1 : 0); ++i) {
        const float d = step * static_cast<float>(i);
        while (j + 2 < samples.size() && samples[j + 1].distance < d) ++j;
        const SplineSample& a = samples[j];
        const SplineSample& b = samples[j + 1];
        const float span = b.distance - a.distance;
        const float t = span > 1e-6f ? std::clamp((d - a.distance) / span, 0.0f, 1.0f) : 0.0f;
        SplineSample s = a;
        s.position = core::lerp(a.position, b.position, t);
        s.tangent = core::normalize(core::lerp(a.tangent, b.tangent, t));
        s.right = core::normalize(core::lerp(a.right, b.right, t));
        s.up = core::normalize(core::lerp(a.up, b.up, t));
        s.width = a.width + (b.width - a.width) * t;
        s.distance = d;
        out.push_back(s);
    }
    return out;
}

}  // namespace

std::shared_ptr<ecs::Mesh> buildExtrudeMesh(const Spline& spline, const SplineExtrude& ex,
                                            const std::vector<SplineSample>& samples) {
    auto mesh = std::make_shared<ecs::Mesh>();
    mesh->name = "Spline";
    if (samples.size() < 2 || ex.shape == ExtrudeShape::River) return mesh;
    const float w = std::max(ex.width, 0.01f);
    const float h = std::max(ex.height, 0.0f);
    Builder b(2);
    std::vector<ecs::MeshMaterial> mats;
    switch (ex.shape) {
        case ExtrudeShape::Road: {
            const float half = w * 0.5f;
            const float curb = std::max(0.3f, h * 2.0f);
            const float depth = std::max(h, 0.02f);
            b.sweep(0, samples, {{-half - curb, -depth}, {-half, 0.0f}, {half, 0.0f}, {half + curb, -depth}}, false,
                    false, ex.uv_meters, true, w);
            // Linea central discontinua (3 m si, 3 m no) y lineas de borde.
            std::vector<SplineSample> dash;
            for (std::size_t i = 0; i + 1 < samples.size(); ++i) {
                const bool on = static_cast<int>(std::floor(samples[i].distance / 3.0f)) % 2 == 0;
                if (on) {
                    if (dash.empty()) dash.push_back(samples[i]);
                    dash.push_back(samples[i + 1]);
                } else if (!dash.empty()) {
                    b.sweep(1, dash, {{-0.07f, 0.012f}, {0.07f, 0.012f}}, false, false, 3.0f, false);
                    dash.clear();
                }
            }
            if (dash.size() >= 2) b.sweep(1, dash, {{-0.07f, 0.012f}, {0.07f, 0.012f}}, false, false, 3.0f, false);
            b.sweep(1, samples, {{-0.06f, 0.012f}, {0.06f, 0.012f}}, false, false, 3.0f, false, -1.0f, -(half - 0.3f));
            b.sweep(1, samples, {{-0.06f, 0.012f}, {0.06f, 0.012f}}, false, false, 3.0f, false, -1.0f, half - 0.3f);
            mats = {material(ex.color, ex.roughness), material({0.92f, 0.92f, 0.88f}, 0.6f)};
            break;
        }
        case ExtrudeShape::Path: {
            const float half = w * 0.5f;
            b.sweep(0, samples,
                    {{-half - 0.4f, -0.08f}, {-half, 0.0f}, {-half * 0.3f, 0.015f}, {half * 0.3f, 0.015f},
                     {half, 0.0f}, {half + 0.4f, -0.08f}},
                    false, true, ex.uv_meters, true, w);
            mats = {material(ex.color, ex.roughness)};
            break;
        }
        case ExtrudeShape::Wall: {
            const float half = w * 0.5f;
            const float cap = std::min(0.12f, h * 0.1f);
            b.sweep(0, samples,
                    {{-half, -0.3f}, {-half, h - cap}, {-half - 0.06f, h - cap}, {-half - 0.06f, h}, {half + 0.06f, h},
                     {half + 0.06f, h - cap}, {half, h - cap}, {half, -0.3f}},
                    false, false, ex.uv_meters, true, ex.uv_meters);
            if (!spline.closed) {
                // Tapas de los extremos.
                for (int end = 0; end < 2; ++end) {
                    const SplineSample& s = end == 0 ? samples.front() : samples.back();
                    const Vec3 fwd = end == 0 ? s.tangent * -1.0f : s.tangent;
                    const Vec3 c = s.position + s.up * ((h - 0.3f) * 0.5f) + fwd * 0.005f;
                    b.box(0, c, s.right, s.up, Vec3{half * s.width, (h + 0.3f) * 0.5f, 0.005f}, 1.0f / ex.uv_meters);
                }
            }
            mats = {material(ex.color, ex.roughness)};
            break;
        }
        case ExtrudeShape::Fence: {
            const float post = std::max(0.06f, w);
            for (const SplineSample& s : every(samples, ex.post_spacing, spline.closed)) {
                b.box(0, s.position + s.up * (h * 0.5f - 0.15f), s.right, s.up,
                      Vec3{post * 0.5f, (h + 0.3f) * 0.5f, post * 0.5f});
            }
            const float rail_h = post * 0.45f;
            const float rail_d = post * 0.3f;
            for (const float y : {h * 0.38f, h * 0.82f}) {
                b.sweep(1, samples,
                        {{-rail_d, y - rail_h}, {-rail_d, y + rail_h}, {rail_d, y + rail_h}, {rail_d, y - rail_h}}, true,
                        false, ex.uv_meters, false, 0.2f, 0.0f);
            }
            mats = {material(ex.color, ex.roughness), material(ex.color * 0.85f, ex.roughness)};
            break;
        }
        case ExtrudeShape::Pipe: {
            const float r = w * 0.5f;
            std::vector<core::Vec2> circle;
            constexpr int kSides = 14;
            for (int k = 0; k < kSides; ++k) {
                const float a = core::kPi * 0.5f - static_cast<float>(k) / kSides * 2.0f * core::kPi;  // horario
                circle.push_back({std::cos(a) * r, r + std::sin(a) * r});
            }
            b.sweep(0, samples, circle, true, true, ex.uv_meters, true, 2.0f * core::kPi * r);
            mats = {material(ex.color, ex.roughness, 0.6f)};
            break;
        }
        case ExtrudeShape::Rails: {
            const float gauge = w * 0.5f;
            for (const float side : {-gauge, gauge}) {
                b.sweep(0, samples,
                        {{-0.04f, 0.14f}, {-0.04f, 0.3f}, {0.04f, 0.3f}, {0.04f, 0.14f}}, true, false, ex.uv_meters,
                        false, 0.32f, side);
            }
            for (const SplineSample& s : every(samples, std::max(ex.post_spacing, 0.3f), spline.closed)) {
                b.box(1, s.position + s.up * 0.07f, s.right, s.up, Vec3{gauge + 0.35f, 0.07f, 0.12f});
            }
            mats = {material({0.42f, 0.4f, 0.38f}, 0.35f, 0.9f), material(ex.color, ex.roughness)};
            break;
        }
        case ExtrudeShape::Ribbon: {
            const float half = w * 0.5f;
            b.sweep(0, samples, {{-half, 0.0f}, {half, 0.0f}}, false, false, ex.uv_meters, true, w);
            b.sweep(0, samples, {{half, 0.0f}, {-half, 0.0f}}, false, false, ex.uv_meters, true, w);  // por detras
            mats = {material(ex.color, ex.roughness)};
            break;
        }
        case ExtrudeShape::River:
            break;
    }
    // Quita las submallas vacias del final.
    int used = static_cast<int>(mats.size());
    while (used > 1 && b.tris[static_cast<std::size_t>(used - 1)].empty()) --used;
    mesh->vertices = std::move(b.vertices);
    mesh->uv = std::move(b.uv);
    mesh->setSubMeshCount(used);
    for (int i = 0; i < used; ++i) mesh->setTriangles(std::move(b.tris[static_cast<std::size_t>(i)]), i);
    mats.resize(static_cast<std::size_t>(used));
    mesh->materials = std::move(mats);
    mesh->recalculateNormals();
    mesh->recalculateTangents();
    mesh->recalculateBounds();
    mesh->markModified();
    return mesh;
}

// -----------------------------------------------------------------------------
// Terreno
// -----------------------------------------------------------------------------

namespace {

struct Ground {
    std::shared_ptr<terrain::TerrainData> data;
    terrain::Terrain terrain;
    Vec3 origin;
};

std::vector<Ground> grounds(ecs::World& world, terrain::TerrainStore* store) {
    std::vector<Ground> out;
    if (store == nullptr) return out;
    for (const entt::entity handle : world.registry().view<terrain::Terrain>()) {
        const ecs::Entity e = world.wrap(handle);
        if (!e.activeInHierarchy()) continue;
        const terrain::Terrain& t = e.get<terrain::Terrain>();
        std::shared_ptr<terrain::TerrainData> data = store->get(t);
        if (!data) continue;
        out.push_back({std::move(data), t, e.worldPosition()});
    }
    return out;
}

bool groundHeight(const std::vector<Ground>& gs, float x, float z, float& height) {
    for (const Ground& g : gs) {
        if (x < g.origin.x || z < g.origin.z || x > g.origin.x + g.terrain.size || z > g.origin.z + g.terrain.size) continue;
        height = terrain::heightAt(*g.data, g.terrain, g.origin, x, z);
        return true;
    }
    return false;
}

// Pega las muestras (mundo) al terreno y suaviza la altura (las carreteras no
// siguen cada bache).
void conform(std::vector<SplineSample>& samples, const std::vector<Ground>& gs, float offset, float smooth_meters) {
    if (gs.empty() || samples.empty()) return;
    std::vector<float> y(samples.size());
    std::vector<char> hit(samples.size(), 0);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        float hgt = 0.0f;
        hit[i] = groundHeight(gs, samples[i].position.x, samples[i].position.z, hgt) ? 1 : 0;
        y[i] = hit[i] ? hgt : samples[i].position.y;
    }
    if (smooth_meters > 0.0f && samples.size() > 2) {
        std::vector<float> s(y.size());
        for (std::size_t i = 0; i < y.size(); ++i) {
            float sum = 0.0f, wsum = 0.0f;
            for (std::size_t j = i; j-- > 0 && samples[i].distance - samples[j].distance <= smooth_meters;) {
                sum += y[j];
                wsum += 1.0f;
            }
            for (std::size_t j = i; j < y.size() && samples[j].distance - samples[i].distance <= smooth_meters; ++j) {
                sum += y[j];
                wsum += 1.0f;
            }
            s[i] = wsum > 0.0f ? sum / wsum : y[i];
        }
        y = std::move(s);
    }
    for (std::size_t i = 0; i < samples.size(); ++i) {
        if (hit[i]) samples[i].position.y = y[i] + offset;
    }
    // Rehace las tangentes y los ejes con la nueva altura.
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const std::size_t a = i > 0 ? i - 1 : i;
        const std::size_t b = i + 1 < samples.size() ? i + 1 : i;
        const Vec3 t = core::normalize(samples[b].position - samples[a].position);
        if (core::length(t) < 0.5f) continue;
        SplineSample& s = samples[i];
        // Conserva el peralte: angulo entre right y la horizontal.
        const Vec3 flat_right = core::normalize(core::cross(s.tangent, kUp));
        const float roll = std::atan2(core::dot(s.right, kUp), core::dot(s.right, flat_right));
        s.tangent = t;
        Vec3 right = core::normalize(core::cross(t, kUp));
        if (core::length(right) < 0.5f) continue;
        Vec3 up = core::normalize(core::cross(right, t));
        if (std::abs(roll) > 1e-4f) {
            const float deg = roll * 180.0f / core::kPi;
            right = core::normalize(rotateAround(right, t, deg));
            up = core::normalize(rotateAround(up, t, deg));
        }
        s.right = right;
        s.up = up;
    }
    float d = 0.0f;
    for (std::size_t i = 0; i < samples.size(); ++i) {
        if (i > 0) d += core::length(samples[i].position - samples[i - 1].position);
        samples[i].distance = d;
    }
}

std::string lowerCopy(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

int autoLayer(const terrain::Terrain& t, ExtrudeShape shape) {
    std::vector<const char*> names;
    if (shape == ExtrudeShape::River) names = {"arena", "sand", "grava", "gravel", "tierra", "dirt"};
    else if (shape == ExtrudeShape::Road) names = {"camino", "grava", "gravel", "road", "tierra", "dirt"};
    else names = {"tierra", "dirt", "camino", "grava"};
    for (const char* n : names) {
        for (std::size_t i = 0; i < t.layers.size(); ++i) {
            if (lowerCopy(t.layers[i].name).find(n) != std::string::npos) return static_cast<int>(i);
        }
    }
    return -1;
}

float maxWidth(const Spline& s) {
    float m = 0.0f;
    for (const SplinePoint& p : s.points) m = std::max(m, p.width);
    return m;
}

}  // namespace

SplinePath worldPath(const ecs::Entity& entity, terrain::TerrainStore* terrains) {
    SplinePath path;
    const Spline* s = entity.tryGet<Spline>();
    if (s == nullptr) return path;
    path.build(*s, entity.worldMatrix());
    const SplineExtrude* ex = entity.tryGet<SplineExtrude>();
    if (ex != nullptr && ex->conform_to_terrain && terrains != nullptr && entity.world() != nullptr) {
        const std::vector<Ground> gs = grounds(*entity.world(), terrains);
        std::vector<SplineSample>& samples = path.mutableSamples();
        const float smooth = ex->shape == ExtrudeShape::Road || ex->shape == ExtrudeShape::Rails ? ex->width * 1.5f
                             : ex->shape == ExtrudeShape::River                                ? ex->width
                                                                                                : 0.0f;
        conform(samples, gs, ex->ground_offset, smooth);
    }
    return path;
}

int applyToTerrain(ecs::World& world, const ecs::Entity& entity, terrain::TerrainStore& terrains) {
    Spline* s = entity.tryGet<Spline>();
    SplineExtrude* ex = entity.tryGet<SplineExtrude>();
    if (s == nullptr || ex == nullptr || s->points.size() < 2) return 0;
    const std::vector<Ground> gs = grounds(world, &terrains);
    if (gs.empty()) return 0;
    const Mat4 world_m = entity.worldMatrix();
    const Mat4 inv = core::inverse(world_m);
    // 1) Fija la altura de los puntos de control donde la curva pegada al
    // terreno los pone, y deja de pegarse (el terreno ahora sigue a la curva).
    if (ex->conform_to_terrain) {
        const SplinePath conformed = worldPath(entity, &terrains);
        for (SplinePoint& p : s->points) {
            const Vec3 wp = ecs::transformPoint(world_m, p.position);
            Vec3 on_curve;
            conformed.closestDistance(wp, &on_curve);
            const Vec3 target{wp.x, on_curve.y - ex->ground_offset, wp.z};
            p.position = ecs::transformPoint(inv, target);
        }
        ex->conform_to_terrain = false;
        ex->ground_offset = 0.0f;
        s->markModified();
        ex->markModified();
    }
    SplinePath path;
    path.build(*s, world_m);
    std::vector<Vec3> pts;
    pts.reserve(path.samples().size());
    const bool river = ex->shape == ExtrudeShape::River;
    const float bed = river ? std::max(ex->height, 0.3f) : (ex->shape == ExtrudeShape::Road ? 0.02f : 0.0f);
    for (const SplineSample& smp : path.samples()) pts.push_back(smp.position - Vec3{0.0f, bed, 0.0f});
    float width = ex->width * std::max(maxWidth(*s), 0.1f);
    if (ex->shape == ExtrudeShape::Road) width += 1.0f;
    if (ex->shape == ExtrudeShape::Fence || ex->shape == ExtrudeShape::Wall) width += 0.8f;
    if (ex->shape == ExtrudeShape::Rails) width += 1.6f;
    int changed = 0;
    for (const Ground& g : gs) {
        bool any = terrain::flattenPath(*g.data, g.terrain, g.origin, pts, width, ex->terrain_blend);
        const int layer = ex->paint_layer == -2 ? autoLayer(g.terrain, ex->shape) : ex->paint_layer;
        if (layer >= 0 && layer < static_cast<int>(g.terrain.layers.size())) {
            any |= terrain::paintPath(*g.data, g.terrain, g.origin, pts, width + (river ? 2.0f : 0.0f), layer,
                                      std::max(ex->terrain_blend * 0.5f, 0.5f));
        }
        if (any) {
            g.data->commitCollision();  // la fisica choca con el terreno nuevo
            ++changed;
        }
    }
    return changed;
}

// -----------------------------------------------------------------------------
// Sistemas
// -----------------------------------------------------------------------------

namespace {

struct Built {
    std::uint64_t key = 0;
    std::weak_ptr<ecs::Mesh> mesh;
};

std::uint64_t buildKey(const Spline& s, const SplineExtrude& ex, const ecs::Entity& e,
                       const std::vector<Ground>& gs) {
    std::uint64_t h = 1469598103934665603ull;
    h = (h ^ s.revision) * 1099511628211ull;
    h = (h ^ ex.revision) * 1099511628211ull;
    h = (h ^ static_cast<std::uint64_t>(s.points.size())) * 1099511628211ull;
    if (ex.conform_to_terrain) {
        const Mat4& m = e.worldMatrix();
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r) h = hashFloat(h, m.m[c][r]);
        for (const Ground& g : gs) {
            h = (h ^ g.data->version()) * 1099511628211ull;
            h = hashFloat(h, g.origin.x);
            h = hashFloat(h, g.origin.y);
            h = hashFloat(h, g.origin.z);
        }
    }
    return h;
}

}  // namespace

void updateSplineMeshes(ecs::World& world, terrain::TerrainStore* terrains) {
    static std::unordered_map<const ecs::World*, std::unordered_map<entt::entity, Built>> caches;
    auto& cache = caches[&world];
    auto& registry = world.registry();
    auto view = registry.view<Spline, SplineExtrude>();
    if (view.begin() == view.end()) {
        cache.clear();
        return;
    }
    std::vector<Ground> gs;
    bool gs_ready = false;
    for (const entt::entity h : view) {
        const Spline& s = view.get<Spline>(h);
        SplineExtrude& ex = view.get<SplineExtrude>(h);
        ecs::Entity e = world.wrap(h);
        if (ex.conform_to_terrain && !gs_ready) {
            gs = grounds(world, terrains);
            gs_ready = true;
        }
        const std::uint64_t key = buildKey(s, ex, e, ex.conform_to_terrain ? gs : std::vector<Ground>{});
        Built& b = cache[h];
        if (ex.shape == ExtrudeShape::River) {
            if (b.key == key) continue;
            b.key = key;
            water::WaterBody* body = e.tryGet<water::WaterBody>();
            if (body == nullptr) {
                body = &e.add<water::WaterBody>();
                *body = water::riverPreset();
            }
            body->type = water::WaterType::River;
            SplinePath path = worldPath(e, terrains);
            const Mat4 inv = core::inverse(e.worldMatrix());
            body->points.clear();
            const float step = std::max(4.0f, ex.width * 0.5f);
            const std::vector<SplineSample> pts = every(path.samples(), step, false);
            for (const SplineSample& smp : pts) {
                water::RiverPoint rp;
                rp.position = ecs::transformPoint(inv, smp.position);
                rp.width = ex.width * smp.width;
                body->points.push_back(rp);
            }
            if (ecs::MeshRenderer* r = e.tryGet<ecs::MeshRenderer>()) {
                if (r->mesh) r->mesh.reset();
            }
            continue;
        }
        ecs::MeshRenderer* renderer = e.tryGet<ecs::MeshRenderer>();
        if (renderer == nullptr) renderer = &e.add<ecs::MeshRenderer>();
        const std::shared_ptr<ecs::Mesh> current = b.mesh.lock();
        if (b.key == key && current && renderer->mesh == current) continue;
        // Muestras en local (la malla va en local a la entidad).
        SplinePath path = worldPath(e, terrains);
        std::vector<SplineSample> local = path.samples();
        const Mat4 inv = core::inverse(e.worldMatrix());
        for (SplineSample& smp : local) {
            smp.position = ecs::transformPoint(inv, smp.position);
            smp.tangent = core::normalize(ecs::transformDirection(inv, smp.tangent));
            smp.right = core::normalize(ecs::transformDirection(inv, smp.right));
            smp.up = core::normalize(ecs::transformDirection(inv, smp.up));
        }
        std::shared_ptr<ecs::Mesh> mesh = buildExtrudeMesh(s, ex, local);
        renderer->mesh = mesh;
        b.key = key;
        b.mesh = mesh;
        if (ex.collider && !e.has<physics::MeshCollider>() && !e.has<physics::BoxCollider>()) {
            e.add<physics::MeshCollider>();
        }
    }
    for (auto it = cache.begin(); it != cache.end();) {
        it = registry.valid(it->first) && registry.all_of<Spline, SplineExtrude>(it->first) ? std::next(it)
                                                                                           : cache.erase(it);
    }
}

void updateSplineFollowers(ecs::World& world, float dt) {
    auto view = world.registry().view<SplineFollower>();
    std::unordered_map<entt::entity, SplinePath> paths;
    for (const entt::entity h : view) {
        SplineFollower& f = view.get<SplineFollower>(h);
        ecs::Entity e = world.wrap(h);
        if (!f.started) {
            f.started = true;
            f.playing = f.play_on_start;
        }
        if (!f.playing || !e.activeInHierarchy()) continue;
        ecs::Entity target = f.spline.valid() ? world.find(f.spline) : e.parent();
        if (!target.valid() || !target.has<Spline>()) continue;
        auto it = paths.find(target.handle());
        if (it == paths.end()) {
            SplinePath p;
            p.build(target.get<Spline>(), target.worldMatrix());
            it = paths.emplace(target.handle(), std::move(p)).first;
        }
        const SplinePath& path = it->second;
        if (path.empty()) continue;
        const float len = path.length();
        f.distance += f.speed * dt * static_cast<float>(f.direction == 0 ? 1 : f.direction);
        switch (f.mode) {
            case FollowMode::Loop:
                f.distance = std::fmod(f.distance, len);
                if (f.distance < 0.0f) f.distance += len;
                break;
            case FollowMode::PingPong:
                if (f.distance > len) {
                    f.distance = 2.0f * len - f.distance;
                    f.direction = -1;
                } else if (f.distance < 0.0f) {
                    f.distance = -f.distance;
                    f.direction = 1;
                }
                f.distance = std::clamp(f.distance, 0.0f, len);
                break;
            case FollowMode::Once:
                if (f.distance >= len || f.distance <= 0.0f) {
                    f.distance = std::clamp(f.distance, 0.0f, len);
                    f.playing = false;
                }
                break;
        }
        const SplineSample s = path.atDistance(f.distance);
        const Vec3 pos = s.position + s.right * f.offset_side + s.up * f.offset_up;
        if (!f.orient) {
            e.setWorldPosition(pos);
            continue;
        }
        const Mat4& current = e.worldMatrix();
        const auto col = [&](int c) { return core::length(Vec3{current.m[c][0], current.m[c][1], current.m[c][2]}); };
        const Vec3 scale{col(0), col(1), col(2)};
        Vec3 forward = s.tangent * (f.direction < 0 && f.mode == FollowMode::PingPong ? -1.0f : 1.0f);
        if (f.speed < 0.0f) forward = forward * -1.0f;
        // El adelante del motor es -Z: columnas (derecha, arriba, -adelante).
        Vec3 right = core::normalize(core::cross(forward, kUp));
        if (core::length(right) < 0.5f) right = s.right;
        const Vec3 up = core::normalize(core::cross(right, forward));
        Mat4 m = Mat4::identity();
        const Vec3 cols[3] = {right * scale.x, up * scale.y, forward * -scale.z};
        for (int c = 0; c < 3; ++c) {
            m.m[c][0] = cols[c].x;
            m.m[c][1] = cols[c].y;
            m.m[c][2] = cols[c].z;
        }
        m.m[3][0] = pos.x;
        m.m[3][1] = pos.y;
        m.m[3][2] = pos.z;
        e.setWorldMatrix(m);
    }
}

ecs::Entity createSplineEntity(ecs::World& world, const std::vector<Vec3>& world_points, int shape,
                               const std::string& name, bool closed) {
    registerSplineComponents();
    Vec3 center{};
    for (const Vec3& p : world_points) center += p;
    if (!world_points.empty()) center = center * (1.0f / static_cast<float>(world_points.size()));
    ecs::Entity e = world.create(name);
    e.setWorldPosition(center);
    Spline& s = e.add<Spline>();
    s.closed = closed;
    s.points.clear();
    for (const Vec3& p : world_points) s.points.push_back(SplinePoint{p - center});
    if (s.points.size() < 2) s.points = Spline{}.points;
    s.markModified();
    if (shape >= 0) {
        SplineExtrude& ex = e.add<SplineExtrude>();
        ex.shape = static_cast<ExtrudeShape>(std::clamp(shape, 0, 7));
        switch (ex.shape) {
            case ExtrudeShape::Road: break;
            case ExtrudeShape::Path:
                ex.width = 2.5f;
                ex.color = {0.42f, 0.33f, 0.22f};
                ex.roughness = 0.95f;
                ex.collider = false;
                break;
            case ExtrudeShape::River:
                ex.width = 10.0f;
                ex.height = 2.0f;
                ex.ground_offset = 0.0f;
                ex.collider = false;
                break;
            case ExtrudeShape::Wall:
                ex.width = 0.6f;
                ex.height = 2.5f;
                ex.color = {0.55f, 0.52f, 0.47f};
                ex.ground_offset = 0.0f;
                ex.uv_meters = 2.0f;
                break;
            case ExtrudeShape::Fence:
                ex.width = 0.12f;
                ex.height = 1.2f;
                ex.color = {0.45f, 0.32f, 0.2f};
                ex.ground_offset = 0.0f;
                break;
            case ExtrudeShape::Pipe:
                ex.width = 0.6f;
                ex.color = {0.5f, 0.52f, 0.55f};
                ex.roughness = 0.4f;
                ex.conform_to_terrain = false;
                break;
            case ExtrudeShape::Rails:
                ex.width = 1.435f;
                ex.post_spacing = 0.65f;
                ex.color = {0.3f, 0.22f, 0.15f};
                break;
            case ExtrudeShape::Ribbon:
                ex.width = 1.0f;
                ex.color = {0.8f, 0.1f, 0.1f};
                ex.conform_to_terrain = false;
                ex.collider = false;
                break;
        }
        ex.markModified();
    }
    return e;
}

}  // namespace cramion::spline
