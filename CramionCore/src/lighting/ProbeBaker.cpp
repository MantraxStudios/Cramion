#include "CramionCore/lighting/ProbeBaker.h"

#include "CramionCore/asset/AssetDatabase.h"
#include "CramionCore/asset/AssetManager.h"
#include "CramionCore/asset/MaterialAsset.h"
#include "CramionCore/ecs/Components.h"
#include "CramionCore/ecs/MathUtil.h"
#include "CramionCore/ecs/RuntimeMesh.h"
#include "CramionCore/jobs/JobSystem.h"
#include "CramionCore/physics/PhysicsComponents.h"
#include "CramionCore/terrain/Terrain.h"

#include <CramionFX/asset/Model.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <random>
#include <optional>
#include <thread>
#include <unordered_map>

namespace cramion::lighting {

using core::Vec3;
using core::Vec4;

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kEpsilon = 1e-3f;

Vec3 mulv(const Vec3& a, const Vec3& b) { return Vec3{a.x * b.x, a.y * b.y, a.z * b.z}; }
Vec3 minv(const Vec3& a, const Vec3& b) { return Vec3{std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)}; }
Vec3 maxv(const Vec3& a, const Vec3& b) { return Vec3{std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)}; }
float axis(const Vec3& v, int i) { return i == 0 ? v.x : (i == 1 ? v.y : v.z); }
Vec3 srgbToLinear(const Vec3& c) {
    return Vec3{std::pow(std::max(c.x, 0.0f), 2.2f), std::pow(std::max(c.y, 0.0f), 2.2f), std::pow(std::max(c.z, 0.0f), 2.2f)};
}

// --- BVH --------------------------------------------------------------------------------

class Bvh {
public:
    explicit Bvh(const std::vector<BakeTriangle>& tris) : tris_(tris) {
        order_.resize(tris.size());
        centroids_.resize(tris.size());
        for (std::size_t i = 0; i < tris.size(); ++i) {
            order_[i] = static_cast<int>(i);
            centroids_[i] = (tris[i].a + tris[i].b + tris[i].c) * (1.0f / 3.0f);
        }
        nodes_.reserve(tris.size() * 2 + 1);
        if (!tris.empty()) build(0, static_cast<int>(tris.size()));
    }

    bool empty() const { return nodes_.empty(); }

    // Choque mas cercano. `backface`: lo tocado mira hacia otro lado.
    bool intersect(const Vec3& o, const Vec3& d, float tmax, float& t_out, int& tri_out) const {
        if (nodes_.empty()) return false;
        const Vec3 inv{1.0f / (std::fabs(d.x) > 1e-12f ? d.x : 1e-12f), 1.0f / (std::fabs(d.y) > 1e-12f ? d.y : 1e-12f),
                       1.0f / (std::fabs(d.z) > 1e-12f ? d.z : 1e-12f)};
        int stack[64];
        int sp = 0;
        stack[sp++] = 0;
        float best = tmax;
        int hit = -1;
        while (sp > 0) {
            const Node& n = nodes_[static_cast<std::size_t>(stack[--sp])];
            if (!boxHit(n, o, inv, best)) continue;
            if (n.count > 0) {
                for (int i = n.first; i < n.first + n.count; ++i) {
                    float t = 0.0f;
                    const int tri = order_[static_cast<std::size_t>(i)];
                    if (triangleHit(tris_[static_cast<std::size_t>(tri)], o, d, best, t)) {
                        best = t;
                        hit = tri;
                    }
                }
            } else if (sp < 62) {
                stack[sp++] = n.left;
                stack[sp++] = n.left + 1;
            }
        }
        if (hit < 0) return false;
        t_out = best;
        tri_out = hit;
        return true;
    }

    bool occluded(const Vec3& o, const Vec3& d, float tmax) const {
        if (nodes_.empty()) return false;
        const Vec3 inv{1.0f / (std::fabs(d.x) > 1e-12f ? d.x : 1e-12f), 1.0f / (std::fabs(d.y) > 1e-12f ? d.y : 1e-12f),
                       1.0f / (std::fabs(d.z) > 1e-12f ? d.z : 1e-12f)};
        int stack[64];
        int sp = 0;
        stack[sp++] = 0;
        while (sp > 0) {
            const Node& n = nodes_[static_cast<std::size_t>(stack[--sp])];
            if (!boxHit(n, o, inv, tmax)) continue;
            if (n.count > 0) {
                for (int i = n.first; i < n.first + n.count; ++i) {
                    float t = 0.0f;
                    if (triangleHit(tris_[static_cast<std::size_t>(order_[static_cast<std::size_t>(i)])], o, d, tmax, t)) return true;
                }
            } else if (sp < 62) {
                stack[sp++] = n.left;
                stack[sp++] = n.left + 1;
            }
        }
        return false;
    }

private:
    struct Node {
        Vec3 min, max;
        int left = -1;
        int first = 0;
        int count = 0;
    };

    int build(int first, int count) {
        const int index = static_cast<int>(nodes_.size());
        nodes_.push_back(Node{});
        Vec3 mn{1e30f, 1e30f, 1e30f}, mx{-1e30f, -1e30f, -1e30f};
        Vec3 cmn{1e30f, 1e30f, 1e30f}, cmx{-1e30f, -1e30f, -1e30f};
        for (int i = first; i < first + count; ++i) {
            const BakeTriangle& t = tris_[static_cast<std::size_t>(order_[static_cast<std::size_t>(i)])];
            mn = minv(mn, minv(t.a, minv(t.b, t.c)));
            mx = maxv(mx, maxv(t.a, maxv(t.b, t.c)));
            const Vec3& c = centroids_[static_cast<std::size_t>(order_[static_cast<std::size_t>(i)])];
            cmn = minv(cmn, c);
            cmx = maxv(cmx, c);
        }
        nodes_[static_cast<std::size_t>(index)].min = mn;
        nodes_[static_cast<std::size_t>(index)].max = mx;
        if (count <= 4) {
            nodes_[static_cast<std::size_t>(index)].first = first;
            nodes_[static_cast<std::size_t>(index)].count = count;
            return index;
        }
        const Vec3 extent = cmx - cmn;
        const int split_axis = extent.x > extent.y && extent.x > extent.z ? 0 : (extent.y > extent.z ? 1 : 2);
        const int mid = first + count / 2;
        std::nth_element(order_.begin() + first, order_.begin() + mid, order_.begin() + first + count, [&](int a, int b) {
            return axis(centroids_[static_cast<std::size_t>(a)], split_axis) < axis(centroids_[static_cast<std::size_t>(b)], split_axis);
        });
        // Los dos hijos seguidos (left y left + 1).
        const int left = static_cast<int>(nodes_.size());
        nodes_.push_back(Node{});
        nodes_.push_back(Node{});
        nodes_[static_cast<std::size_t>(index)].left = left;
        buildInto(left, first, mid - first);
        buildInto(left + 1, mid, first + count - mid);
        return index;
    }

    void buildInto(int slot, int first, int count) {
        // Construye el subarbol y lo copia al hueco reservado.
        const int built = build(first, count);
        nodes_[static_cast<std::size_t>(slot)] = nodes_[static_cast<std::size_t>(built)];
        // El nodo construido queda huerfano (no lo apunta nadie): se reutiliza como
        // basura inofensiva; el arbol sigue siendo correcto.
    }

    static bool boxHit(const Node& n, const Vec3& o, const Vec3& inv, float tmax) {
        float t0 = 0.0f, t1 = tmax;
        for (int i = 0; i < 3; ++i) {
            const float origin = axis(o, i);
            const float invd = axis(inv, i);
            float ta = (axis(n.min, i) - origin) * invd;
            float tb = (axis(n.max, i) - origin) * invd;
            if (ta > tb) std::swap(ta, tb);
            t0 = std::max(t0, ta);
            t1 = std::min(t1, tb);
            if (t0 > t1) return false;
        }
        return true;
    }

    static bool triangleHit(const BakeTriangle& tri, const Vec3& o, const Vec3& d, float tmax, float& t) {
        const Vec3 e1 = tri.b - tri.a;
        const Vec3 e2 = tri.c - tri.a;
        const Vec3 p = core::cross(d, e2);
        const float det = core::dot(e1, p);
        if (std::fabs(det) < 1e-10f) return false;
        const float inv_det = 1.0f / det;
        const Vec3 s = o - tri.a;
        const float u = core::dot(s, p) * inv_det;
        if (u < 0.0f || u > 1.0f) return false;
        const Vec3 q = core::cross(s, e1);
        const float v = core::dot(d, q) * inv_det;
        if (v < 0.0f || u + v > 1.0f) return false;
        t = core::dot(e2, q) * inv_det;
        return t > 1e-4f && t < tmax;
    }

    const std::vector<BakeTriangle>& tris_;
    std::vector<Node> nodes_;
    std::vector<int> order_;
    std::vector<Vec3> centroids_;
};

// --- Trazado ----------------------------------------------------------------------------

struct Tracer {
    const BakeScene& scene;
    const Bvh& bvh;
    Vec3 sun_dir{};    // hacia el sol
    Vec3 sun_color{};  // lineal * intensidad
    float daylight = 1.0f;

    Vec3 sky(const Vec3& d) const {
        const float k = daylight * scene.settings.sky_intensity;
        if (d.y >= 0.0f) {
            const float t = std::sqrt(std::clamp(d.y, 0.0f, 1.0f));
            return (scene.sky_horizon + (scene.sky_zenith - scene.sky_horizon) * t) * k;
        }
        return scene.sky_horizon * (0.25f * k);
    }

    static float attenuation(float distance, float range) {
        const float ratio = distance / std::max(range, 1e-4f);
        const float window = std::clamp(1.0f - ratio * ratio * ratio * ratio, 0.0f, 1.0f);
        return window * window / (distance * distance + 1.0f);
    }

    Vec3 direct(const Vec3& x, const Vec3& n) const {
        Vec3 result{};
        const Vec3 origin = x + n * kEpsilon;
        const float sc = core::dot(n, sun_dir);
        if (sc > 0.0f && (sun_color.x + sun_color.y + sun_color.z) > 0.0f && !bvh.occluded(origin, sun_dir, 1e6f)) {
            result += sun_color * sc;
        }
        for (const scene::PointLight& p : scene.lights.points) {
            Vec3 v = p.position - x;
            const float dist = core::length(v);
            if (dist < 1e-4f || dist > p.range) continue;
            v = v * (1.0f / dist);
            const float c = core::dot(n, v);
            if (c <= 0.0f) continue;
            if (bvh.occluded(origin, v, dist - kEpsilon * 2.0f)) continue;
            result += srgbToLinear(p.color) * (p.intensity * attenuation(dist, p.range) * c);
        }
        for (const scene::SpotLight& s : scene.lights.spots) {
            Vec3 v = s.position - x;
            const float dist = core::length(v);
            if (dist < 1e-4f || dist > s.range) continue;
            v = v * (1.0f / dist);
            const float c = core::dot(n, v);
            if (c <= 0.0f) continue;
            const float cos_angle = core::dot(v * -1.0f, core::normalize(s.direction));
            const float cos_outer = std::cos(s.outer_angle);
            const float cos_inner = std::cos(s.inner_angle);
            if (cos_angle <= cos_outer) continue;
            const float x01 = std::clamp((cos_angle - cos_outer) / std::max(cos_inner - cos_outer, 1e-4f), 0.0f, 1.0f);
            const float cone = x01 * x01 * (3.0f - 2.0f * x01);
            if (bvh.occluded(origin, v, dist - kEpsilon * 2.0f)) continue;
            result += srgbToLinear(s.color) * (s.intensity * attenuation(dist, s.range) * c * cone);
        }
        return result;
    }

    // Luz DIRECTA de las luces horneadas (modo Horneada) que llega a `x`:
    // llama a f(direccion hacia la luz, irradiancia sobre un plano que la
    // mira de frente, ya con su sombra). La sombra es suave: `samples` rayos
    // repartidos por la bombilla (source_radius) o por el disco del sol; la
    // fraccion que llega es la visibilidad. Con `n`, las luces que quedan
    // detras de la superficie no cuentan. `x` ya va separado de la superficie.
    template <typename F>
    void bakedDirect(const Vec3& x, const Vec3* n, int samples, std::mt19937& rng, F&& f) const {
        samples = std::clamp(samples, 1, 256);
        std::uniform_real_distribution<float> u01(0.0f, 1.0f);
        // Punto al azar en una esfera de radio r (para la bombilla).
        const auto in_sphere = [&](float r) {
            for (int i = 0; i < 8; ++i) {
                const Vec3 p{u01(rng) * 2.0f - 1.0f, u01(rng) * 2.0f - 1.0f, u01(rng) * 2.0f - 1.0f};
                if (core::dot(p, p) <= 1.0f) return p * r;
            }
            return Vec3{};
        };
        // Visibilidad de una luz local desde x (rayos a puntos de la bombilla).
        const auto local_visibility = [&](const Vec3& center, float radius) {
            int lit = 0;
            for (int s = 0; s < samples; ++s) {
                const Vec3 target = center + (samples > 1 && radius > 0.0f ? in_sphere(radius) : Vec3{});
                Vec3 v = target - x;
                const float dist = core::length(v);
                if (dist < 1e-4f) {
                    ++lit;
                    continue;
                }
                v = v * (1.0f / dist);
                if (!bvh.occluded(x, v, dist - kEpsilon * 2.0f)) ++lit;
            }
            return static_cast<float>(lit) / static_cast<float>(samples);
        };
        if (scene.sun_baked && (sun_color.x + sun_color.y + sun_color.z) > 0.0f &&
            (n == nullptr || core::dot(*n, sun_dir) > 0.0f)) {
            const Vec3 helper = std::fabs(sun_dir.y) < 0.99f ? Vec3{0.0f, 1.0f, 0.0f} : Vec3{1.0f, 0.0f, 0.0f};
            const Vec3 t = core::normalize(core::cross(helper, sun_dir));
            const Vec3 b = core::cross(sun_dir, t);
            constexpr float kSunRadius = 0.00465f;  // radio angular del sol (tangente)
            int lit = 0;
            for (int s = 0; s < samples; ++s) {
                const float r = samples > 1 ? std::sqrt(u01(rng)) * kSunRadius : 0.0f;
                const float phi = 2.0f * kPi * u01(rng);
                const Vec3 d = core::normalize(sun_dir + t * (r * std::cos(phi)) + b * (r * std::sin(phi)));
                if (!bvh.occluded(x, d, 1e6f)) ++lit;
            }
            if (lit > 0) f(sun_dir, sun_color * (static_cast<float>(lit) / static_cast<float>(samples)));
        }
        for (std::size_t i = 0; i < scene.lights.points.size(); ++i) {
            if (i >= scene.point_baked.size() || scene.point_baked[i] == 0) continue;
            const scene::PointLight& p = scene.lights.points[i];
            Vec3 v = p.position - x;
            const float dist = core::length(v);
            if (dist < 1e-4f || dist > p.range) continue;
            v = v * (1.0f / dist);
            if (n != nullptr && core::dot(*n, v) <= 0.0f) continue;
            const float vis = local_visibility(p.position, std::min(p.source_radius, dist * 0.5f));
            if (vis <= 0.0f) continue;
            f(v, srgbToLinear(p.color) * (p.intensity * attenuation(dist, p.range) * vis));
        }
        for (std::size_t i = 0; i < scene.lights.spots.size(); ++i) {
            if (i >= scene.spot_baked.size() || scene.spot_baked[i] == 0) continue;
            const scene::SpotLight& s = scene.lights.spots[i];
            Vec3 v = s.position - x;
            const float dist = core::length(v);
            if (dist < 1e-4f || dist > s.range) continue;
            v = v * (1.0f / dist);
            if (n != nullptr && core::dot(*n, v) <= 0.0f) continue;
            const float cos_angle = core::dot(v * -1.0f, core::normalize(s.direction));
            const float cos_outer = std::cos(s.outer_angle);
            const float cos_inner = std::cos(s.inner_angle);
            if (cos_angle <= cos_outer) continue;
            const float x01 = std::clamp((cos_angle - cos_outer) / std::max(cos_inner - cos_outer, 1e-4f), 0.0f, 1.0f);
            const float cone = x01 * x01 * (3.0f - 2.0f * x01);
            const float vis = local_visibility(s.position, std::min(s.source_radius, dist * 0.5f));
            if (vis <= 0.0f) continue;
            f(v, srgbToLinear(s.color) * (s.intensity * attenuation(dist, s.range) * cone * vis));
        }
    }

    static Vec3 cosineSample(const Vec3& n, std::mt19937& rng) {
        std::uniform_real_distribution<float> u01(0.0f, 1.0f);
        const float r1 = u01(rng);
        const float r2 = u01(rng);
        const float r = std::sqrt(r1);
        const float phi = 2.0f * kPi * r2;
        const Vec3 helper = std::fabs(n.y) < 0.99f ? Vec3{0.0f, 1.0f, 0.0f} : Vec3{1.0f, 0.0f, 0.0f};
        const Vec3 t = core::normalize(core::cross(helper, n));
        const Vec3 b = core::cross(n, t);
        return core::normalize(t * (r * std::cos(phi)) + b * (r * std::sin(phi)) + n * std::sqrt(std::max(0.0f, 1.0f - r1)));
    }

    // Luz que llega a `o` desde la direccion `d`.
    Vec3 incoming(const Vec3& o, const Vec3& d, int depth, std::mt19937& rng, bool& escaped, bool& backface) const {
        float t = 0.0f;
        int tri = -1;
        if (!bvh.intersect(o, d, 1e6f, t, tri)) {
            escaped = true;
            return sky(d);
        }
        const BakeTriangle& tr = scene.triangles[static_cast<std::size_t>(tri)];
        Vec3 n = core::normalize(core::cross(tr.b - tr.a, tr.c - tr.a));
        if (core::dot(n, d) > 0.0f) {
            backface = true;
            n = n * -1.0f;
        }
        const Vec3 x = o + d * t;
        Vec3 radiance = tr.emission + mulv(tr.albedo, direct(x, n));
        if (depth > 0) {
            bool e = false;
            bool b = false;
            const Vec3 d2 = cosineSample(n, rng);
            radiance += mulv(tr.albedo, incoming(x + n * kEpsilon, d2, depth - 1, rng, e, b));
        } else {
            // Sin mas rebotes: el cielo que veria (aproximado, sin sombra).
            radiance += mulv(tr.albedo, sky(n) * 0.5f);
        }
        return radiance;
    }
};

// Base SH (la misma que ibl_sh.comp).
void shBasis(const Vec3& d, float y[9]) {
    y[0] = 0.282095f;
    y[1] = 0.488603f * d.y;
    y[2] = 0.488603f * d.z;
    y[3] = 0.488603f * d.x;
    y[4] = 1.092548f * d.x * d.y;
    y[5] = 1.092548f * d.y * d.z;
    y[6] = 0.315392f * (3.0f * d.z * d.z - 1.0f);
    y[7] = 1.092548f * d.x * d.z;
    y[8] = 0.546274f * (d.x * d.x - d.y * d.y);
}

constexpr float kBand[9] = {1.0f, 2.0f / 3.0f, 2.0f / 3.0f, 2.0f / 3.0f, 0.25f, 0.25f, 0.25f, 0.25f, 0.25f};

float probeFloat(const Vec4* p, int index) {
    const Vec4& v = p[index / 4];
    switch (index % 4) {
        case 0: return v.x;
        case 1: return v.y;
        case 2: return v.z;
        default: return v.w;
    }
}

void setProbeFloat(Vec4* p, int index, float value) {
    Vec4& v = p[index / 4];
    switch (index % 4) {
        case 0: v.x = value; break;
        case 1: v.y = value; break;
        case 2: v.z = value; break;
        default: v.w = value; break;
    }
}

}  // namespace

// --- Componente ----------------------------------------------------------------------

void LightProbeVolume::reflect(ecs::PropertyVisitor& v) {
    v.field({"size", "Tamaño", "Caja de sondas, centrada en el objeto (m)"}, size, ecs::Vec3Kind::Scale);
    v.field({"spacing", "Separación", "Metros entre sondas (menos = mas detalle y mas tiempo de horneado)"}, spacing,
            ecs::FloatRange{0.25f, 50.0f, 0.05f, "%.2f m"});
    v.field({"intensity", "Intensidad", "Multiplica la luz rebotada horneada"}, intensity,
            ecs::FloatRange{0.0f, 10.0f, 0.01f, "%.2f"});
}

void registerLightingComponents() {
    ecs::ComponentRegistry& registry = ecs::ComponentRegistry::instance();
    if (registry.find("LightProbeVolume") == nullptr) {
        registry.registerComponent<LightProbeVolume>("LightProbeVolume", "Light Probe Volume", "Iluminacion");
    }
}

// --- Lightmap de superficie --------------------------------------------------------------

// Las celdas que tocan las superficies (dentro de los volumenes, si los hay) y
// en cada una un mini-horneado L1 desde un punto de su superficie, un poco
// hacia fuera. Todo en los nucleos (job system).
static bool bakeSurfaceTexels(const BakeScene& scene, const Tracer& tracer, float texel, gfx::SurfaceLightmap& out,
                              BakeProgress* progress, std::string* error) {
    const float cell = std::clamp(texel, 0.1f, 8.0f);
    const float inv = 1.0f / cell;
    struct CellInfo {
        Vec3 normal{};
        Vec3 point{};
        int samples = 0;
    };
    std::unordered_map<std::uint64_t, CellInfo> cells;
    const auto inside_volumes = [&](const Vec3& p) {
        if (scene.volumes.empty()) return true;
        for (const BakeVolume& v : scene.volumes) {
            if (p.x >= v.min.x && p.y >= v.min.y && p.z >= v.min.z && p.x <= v.min.x + v.size.x &&
                p.y <= v.min.y + v.size.y && p.z <= v.min.z + v.size.z) {
                return true;
            }
        }
        return false;
    };
    if (progress) progress->setStage("Lightmap de superficie: buscando texeles");
    for (const BakeTriangle& t : scene.triangles) {
        Vec3 n = core::cross(t.b - t.a, t.c - t.a);
        const float area2 = core::length(n);
        if (area2 < 1e-8f) continue;
        n = n * (1.0f / area2);
        const float edge = std::max({core::length(t.b - t.a), core::length(t.c - t.a), core::length(t.c - t.b)});
        const int steps = std::clamp(static_cast<int>(std::ceil(edge / (cell * 0.5f))), 1, 512);
        for (int i = 0; i <= steps; ++i) {
            for (int j = 0; i + j <= steps; ++j) {
                const float u = static_cast<float>(i) / static_cast<float>(steps);
                const float w = static_cast<float>(j) / static_cast<float>(steps);
                const Vec3 p = t.a + (t.b - t.a) * u + (t.c - t.a) * w;
                if (!inside_volumes(p)) continue;
                const std::uint64_t key = gfx::surfaceCellKey(static_cast<int>(std::floor(p.x * inv)),
                                                              static_cast<int>(std::floor(p.y * inv)),
                                                              static_cast<int>(std::floor(p.z * inv)));
                CellInfo& c = cells[key];
                c.normal += n;
                if (c.samples == 0) c.point = p;
                ++c.samples;
            }
        }
        if (cells.size() > scene.settings.max_surface_texels) {
            if (error) *error = "demasiados texeles de lightmap (" + std::to_string(cells.size()) + "): sube el tamano del texel";
            return false;
        }
    }
    if (cells.empty()) return true;
    std::vector<std::pair<std::uint64_t, CellInfo>> list(cells.begin(), cells.end());
    std::vector<Vec4> texels(list.size() * gfx::kSurfaceTexelVec4, Vec4{});
    std::vector<char> valid(list.size(), 0);
    const int rays = std::clamp(scene.settings.surface_rays, 8, 4096);
    const int bounces = std::clamp(scene.settings.bounces, 0, 8);
    const float weight = 4.0f * kPi / static_cast<float>(rays);
    std::atomic<std::size_t> done{0};
    if (progress) progress->setStage("Lightmap de superficie: " + std::to_string(list.size()) + " texeles");
    jobs::parallelFor(list.size(), 16, [&](std::size_t begin, std::size_t end) {
        std::mt19937 rng(static_cast<unsigned>(begin * 2654435761u + 7u));
        std::uniform_real_distribution<float> u01(0.0f, 1.0f);
        for (std::size_t i = begin; i < end; ++i) {
            if (progress && progress->cancel.load()) return;
            const CellInfo& c = list[i].second;
            const float nl = core::length(c.normal);
            const Vec3 n = nl > 1e-4f ? c.normal * (1.0f / nl) : Vec3{0.0f, 1.0f, 0.0f};
            // Un poco hacia fuera de la superficie (dos caras: nl pequeno, menos).
            const Vec3 origin = c.point + n * (nl / static_cast<float>(c.samples) > 0.5f ? std::min(cell * 0.25f, 0.1f) : 0.02f);
            Vec3 rgb[4] = {};
            float vis[4] = {};
            int backfaces = 0;
            const float a = u01(rng) * 2.0f * kPi;
            const float golden = kPi * (3.0f - std::sqrt(5.0f));
            for (int r = 0; r < rays; ++r) {
                const float y = 1.0f - (static_cast<float>(r) + 0.5f) / static_cast<float>(rays) * 2.0f;
                const float rad = std::sqrt(std::max(0.0f, 1.0f - y * y));
                const float phi = golden * static_cast<float>(r) + a;
                const Vec3 d{rad * std::cos(phi), y, rad * std::sin(phi)};
                bool escaped = false;
                bool backface = false;
                const Vec3 L = tracer.incoming(origin, d, bounces, rng, escaped, backface);
                if (backface) ++backfaces;
                const float sh[4] = {0.282095f, 0.488603f * d.y, 0.488603f * d.z, 0.488603f * d.x};
                if (escaped) {
                    for (int k = 0; k < 4; ++k) vis[k] += sh[k] * weight;
                } else {
                    for (int k = 0; k < 4; ++k) rgb[k] += L * (sh[k] * weight);
                }
            }
            if (static_cast<float>(backfaces) < static_cast<float>(rays) * 0.4f) {
                const float band[4] = {1.0f, 2.0f / 3.0f, 2.0f / 3.0f, 2.0f / 3.0f};
                // Luces horneadas: su luz directa con sombra suave. Cada luz es
                // una direccion (pi * E * Y(l), el mismo convenio que la luz
                // de los rayos); L1 reparte de mas una luz puntual, asi que se
                // escala para que en la normal del texel de la irradiancia
                // exacta (E * coseno): la luz y la sombra quedan como en
                // tiempo real.
                Vec3 direct_sh[4] = {};
                Vec3 exact{};
                bool any_direct = false;
                tracer.bakedDirect(origin, &n, scene.settings.direct_samples, rng, [&](const Vec3& l, const Vec3& e) {
                    const float yl[4] = {0.282095f, 0.488603f * l.y, 0.488603f * l.z, 0.488603f * l.x};
                    for (int k = 0; k < 4; ++k) direct_sh[k] += e * (kPi * yl[k]);
                    exact += e * std::max(core::dot(n, l), 0.0f);
                    any_direct = true;
                });
                if (any_direct) {
                    const float yn[4] = {0.282095f, 0.488603f * n.y, 0.488603f * n.z, 0.488603f * n.x};
                    Vec3 eval{};
                    for (int k = 0; k < 4; ++k) eval += direct_sh[k] * (band[k] * yn[k]);
                    const Vec3 scale{eval.x > 1e-6f ? exact.x / eval.x : 0.0f, eval.y > 1e-6f ? exact.y / eval.y : 0.0f,
                                     eval.z > 1e-6f ? exact.z / eval.z : 0.0f};
                    for (int k = 0; k < 4; ++k) rgb[k] += mulv(direct_sh[k], scale);
                }
                Vec4* t = &texels[i * gfx::kSurfaceTexelVec4];
                float f[16];
                for (int k = 0; k < 4; ++k) {
                    f[k * 3 + 0] = rgb[k].x * band[k];
                    f[k * 3 + 1] = rgb[k].y * band[k];
                    f[k * 3 + 2] = rgb[k].z * band[k];
                    f[12 + k] = vis[k] * band[k];
                }
                for (int q = 0; q < 4; ++q) t[q] = Vec4{f[q * 4], f[q * 4 + 1], f[q * 4 + 2], f[q * 4 + 3]};
                valid[i] = 1;
            }
            const std::size_t finished = ++done;
            if (progress && (finished & 255u) == 0) {
                progress->fraction.store(static_cast<float>(finished) / static_cast<float>(list.size()));
            }
        }
    });
    if (progress && progress->cancel.load()) {
        if (error) *error = "cancelado";
        return false;
    }
    // Tabla hash (la mitad llena como mucho).
    std::size_t used = 0;
    for (const char v : valid) used += v != 0 ? 1u : 0u;
    std::uint32_t capacity = 16;
    while (capacity < used * 2 + 1) capacity <<= 1;
    out = gfx::SurfaceLightmap{};
    out.cell_size = cell;
    out.capacity = capacity;
    out.keys.assign(static_cast<std::size_t>(capacity) * 2, gfx::kSurfaceEmptyKey);
    out.texels.assign(static_cast<std::size_t>(capacity) * gfx::kSurfaceTexelVec4, Vec4{});
    for (std::size_t i = 0; i < list.size(); ++i) {
        if (!valid[i]) continue;
        const std::uint32_t lo = static_cast<std::uint32_t>(list[i].first & 0xFFFFFFFFull);
        const std::uint32_t hi = static_cast<std::uint32_t>(list[i].first >> 32);
        std::uint32_t slot = gfx::surfaceCellHash(lo, hi) & (capacity - 1);
        while (out.keys[static_cast<std::size_t>(slot) * 2] != gfx::kSurfaceEmptyKey) slot = (slot + 1) & (capacity - 1);
        out.keys[static_cast<std::size_t>(slot) * 2] = lo;
        out.keys[static_cast<std::size_t>(slot) * 2 + 1] = hi;
        for (std::uint32_t q = 0; q < gfx::kSurfaceTexelVec4; ++q) {
            out.texels[static_cast<std::size_t>(slot) * gfx::kSurfaceTexelVec4 + q] = texels[i * gfx::kSurfaceTexelVec4 + q];
        }
        ++out.count;
    }
    return true;
}

// --- Horneado ----------------------------------------------------------------------------

bool bakeProbes(const BakeScene& scene, gfx::BakedLighting& out, BakeProgress* progress, BakeStats* stats, std::string* error) {
    const auto start = std::chrono::steady_clock::now();
    out = gfx::BakedLighting{};
    // Luces horneadas sin lightmap de superficie: se enciende (las sombras de
    // las sondas solas serian manchas de metros).
    bool any_baked = scene.sun_baked && scene.lights.sun.intensity > 0.0f;
    for (const std::uint8_t b : scene.point_baked) any_baked = any_baked || b != 0;
    for (const std::uint8_t b : scene.spot_baked) any_baked = any_baked || b != 0;
    float surface_texel = scene.settings.surface_texel;
    if (any_baked && surface_texel <= 0.0f && scene.settings.auto_surface_texel > 0.0f) {
        surface_texel = scene.settings.auto_surface_texel;
    }
    if (scene.volumes.empty() && surface_texel <= 0.0f) {
        if (error) *error = "no hay volumenes de sondas";
        return false;
    }
    if (progress) progress->setStage("Construyendo el BVH");
    const Bvh bvh(scene.triangles);

    Tracer tracer{scene, bvh};
    tracer.sun_dir = core::length(scene.lights.sun.direction) > 1e-6f ? core::normalize(scene.lights.sun.direction) * -1.0f
                                                                        : Vec3{0.0f, 1.0f, 0.0f};
    tracer.sun_color = srgbToLinear(scene.lights.sun.color) * std::max(scene.lights.sun.intensity, 0.0f);
    tracer.daylight = std::clamp(scene.lights.sky.daylight, 0.0f, 1.0f);

    // Volumenes del mas pequeno al mas grande (el shader usa el primero que contiene el punto).
    std::vector<BakeVolume> volumes = scene.volumes;
    std::sort(volumes.begin(), volumes.end(), [](const BakeVolume& a, const BakeVolume& b) {
        return a.size.x * a.size.y * a.size.z < b.size.x * b.size.y * b.size.z;
    });
    if (volumes.size() > gfx::kMaxBakedVolumes) volumes.resize(gfx::kMaxBakedVolumes);
    struct ProbeJob {
        Vec3 position;
        std::size_t index;
    };
    std::vector<ProbeJob> jobs;
    std::uint32_t first = 0;
    for (const BakeVolume& v : volumes) {
        gfx::BakedProbeVolume g;
        g.min = v.min;
        g.size = v.size;
        g.nx = std::max(v.nx, 2u);
        g.ny = std::max(v.ny, 2u);
        g.nz = std::max(v.nz, 2u);
        g.first = first;
        g.intensity = v.intensity;
        for (std::uint32_t z = 0; z < g.nz; ++z) {
            for (std::uint32_t y = 0; y < g.ny; ++y) {
                for (std::uint32_t x = 0; x < g.nx; ++x) {
                    const Vec3 f{static_cast<float>(x) / static_cast<float>(g.nx - 1), static_cast<float>(y) / static_cast<float>(g.ny - 1),
                                 static_cast<float>(z) / static_cast<float>(g.nz - 1)};
                    jobs.push_back(ProbeJob{v.min + mulv(v.size, f), first + x + g.nx * (y + g.ny * z)});
                }
            }
        }
        first += g.nx * g.ny * g.nz;
        out.volumes.push_back(g);
        if (static_cast<float>(first) > scene.settings.max_probes) {
            if (error) *error = "demasiadas sondas (" + std::to_string(first) + "): sube la separacion";
            out = gfx::BakedLighting{};
            return false;
        }
    }
    out.probes.assign(static_cast<std::size_t>(first) * gfx::kBakedProbeVec4, Vec4{});

    const int rays = std::clamp(scene.settings.rays, 8, 8192);
    const int bounces = std::clamp(scene.settings.bounces, 0, 8);
    // Direcciones de Fibonacci (uniformes en la esfera); cada sonda las gira al azar.
    std::vector<Vec3> directions(static_cast<std::size_t>(rays));
    const float golden = kPi * (3.0f - std::sqrt(5.0f));
    for (int i = 0; i < rays; ++i) {
        const float y = 1.0f - (static_cast<float>(i) + 0.5f) / static_cast<float>(rays) * 2.0f;
        const float r = std::sqrt(std::max(0.0f, 1.0f - y * y));
        const float phi = golden * static_cast<float>(i);
        directions[static_cast<std::size_t>(i)] = Vec3{r * std::cos(phi), y, r * std::sin(phi)};
    }

    std::atomic<std::size_t> next{0};
    std::atomic<std::size_t> done{0};
    std::atomic<std::size_t> invalid{0};
    const unsigned hardware = std::max(2u, std::thread::hardware_concurrency());
    const int thread_count = scene.settings.threads > 0 ? scene.settings.threads : static_cast<int>(hardware - 1);
    if (progress) progress->setStage("Horneando " + std::to_string(jobs.size()) + " sondas");
    const float weight = 4.0f * kPi / static_cast<float>(rays);
    const auto worker = [&](unsigned seed) {
        std::mt19937 rng(seed * 7919u + 17u);
        std::uniform_real_distribution<float> u01(0.0f, 1.0f);
        for (;;) {
            if (progress && progress->cancel.load()) return;
            const std::size_t j = next.fetch_add(1);
            if (j >= jobs.size()) return;
            const ProbeJob& job = jobs[j];
            // Giro al azar de las direcciones (sin patrones entre sondas).
            const float a = u01(rng) * 2.0f * kPi;
            const float ca = std::cos(a), sa = std::sin(a);
            Vec3 rgb[9] = {};
            float vis[4] = {};
            int backfaces = 0;
            for (int i = 0; i < rays; ++i) {
                const Vec3& d0 = directions[static_cast<std::size_t>(i)];
                const Vec3 d{d0.x * ca - d0.z * sa, d0.y, d0.x * sa + d0.z * ca};
                bool escaped = false;
                bool backface = false;
                const Vec3 L = tracer.incoming(job.position, d, bounces, rng, escaped, backface);
                if (backface) ++backfaces;
                float y[9];
                shBasis(d, y);
                if (escaped) {
                    for (int k = 0; k < 4; ++k) vis[k] += y[k] * weight;
                } else {
                    for (int k = 0; k < 9; ++k) rgb[k] += L * (y[k] * weight);
                }
            }
            Vec4* probe = &out.probes[job.index * gfx::kBakedProbeVec4];
            const bool valid = static_cast<float>(backfaces) < static_cast<float>(rays) * 0.25f;
            if (valid) {
                // Luces horneadas: su luz directa (con sombra desde la sonda),
                // para lo que no tiene lightmap (lo que se mueve, el follaje).
                tracer.bakedDirect(job.position, nullptr, std::max(4, scene.settings.direct_samples / 2), rng,
                                   [&](const Vec3& l, const Vec3& e) {
                                       float yl[9];
                                       shBasis(l, yl);
                                       for (int k = 0; k < 9; ++k) rgb[k] += e * (kPi * yl[k]);
                                   });
                for (int k = 0; k < 9; ++k) {
                    setProbeFloat(probe, k * 3, rgb[k].x * kBand[k]);
                    setProbeFloat(probe, k * 3 + 1, rgb[k].y * kBand[k]);
                    setProbeFloat(probe, k * 3 + 2, rgb[k].z * kBand[k]);
                }
                for (int k = 0; k < 4; ++k) setProbeFloat(probe, 27 + k, vis[k] * kBand[k]);
                setProbeFloat(probe, 31, 1.0f);
            } else {
                ++invalid;
            }
            const std::size_t finished = ++done;
            if (progress) progress->fraction.store(static_cast<float>(finished) / static_cast<float>(jobs.size()));
        }
    };
    std::vector<std::thread> threads;
    for (int t = 0; t < thread_count; ++t) threads.emplace_back(worker, static_cast<unsigned>(t + 1));
    for (std::thread& t : threads) t.join();
    if (progress && progress->cancel.load()) {
        if (error) *error = "cancelado";
        out = gfx::BakedLighting{};
        return false;
    }
    if (surface_texel > 0.0f) {
        if (!bakeSurfaceTexels(scene, tracer, surface_texel, out.surface, progress, error)) {
            out = gfx::BakedLighting{};
            return false;
        }
    }
    // Lo que ya va horneado entero: el renderizador no lo dibuja otra vez.
    out.sun_baked = scene.sun_baked && scene.lights.sun.intensity > 0.0f;
    out.sun_direction = scene.lights.sun.direction;
    out.baked_lights = scene.baked_light_ids;
    if (stats != nullptr) {
        stats->triangles = scene.triangles.size();
        stats->probes = jobs.size();
        stats->invalid_probes = invalid.load();
        stats->surface_texels = out.surface.count;
        stats->baked_lights = out.baked_lights.size() + (out.sun_baked ? 1u : 0u);
        stats->seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    }
    if (progress) progress->fraction.store(1.0f);
    return true;
}

// --- Informe ------------------------------------------------------------------------------

const char* bakeSkipLabel(BakeSkip reason) {
    switch (reason) {
        case BakeSkip::Inactive: return "desactivadas";
        case BakeSkip::Hidden: return "ocultas (MeshRenderer invisible)";
        case BakeSkip::DynamicBody: return "con Rigidbody Dynamic (se mueven)";
        case BakeSkip::AnimatorDriven: return "movidas por un Animator";
        case BakeSkip::NoModel: return "con un modelo que no se pudo cargar";
        case BakeSkip::BadPart: return "con una pieza que no existe en el modelo";
        case BakeSkip::NotStatic: return "sin marcar Static";
        default: return "?";
    }
}

void BakeGatherReport::skip(BakeSkip reason, const std::string& name) {
    const int i = static_cast<int>(reason);
    if (i < 0 || i >= static_cast<int>(BakeSkip::Count)) return;
    ++skipped[i];
    if (examples[i].size() < 3) examples[i].push_back(name);
}

std::string BakeGatherReport::summary() const {
    std::string out;
    const auto line = [&](const std::string& text) {
        if (!out.empty()) out += "\n";
        out += text;
    };
    const auto examples_of = [&](int i) {
        std::string text;
        for (std::size_t k = 0; k < examples[i].size(); ++k) text += (k ? ", " : " (") + examples[i][k];
        if (!text.empty()) text += skipped[i] > examples[i].size() ? "...)" : ")";
        return text;
    };
    if (triangles == 0) {
        std::string reasons;
        for (int i = 0; i < static_cast<int>(BakeSkip::Count); ++i) {
            if (skipped[i] == 0) continue;
            if (!reasons.empty()) reasons += ", ";
            reasons += std::to_string(skipped[i]) + " " + bakeSkipLabel(static_cast<BakeSkip>(i)) + examples_of(i);
        }
        line("Nada que hornear: 0 de " + std::to_string(meshes_seen) + " mallas entran" +
             (reasons.empty() ? std::string(" (la escena no tiene mallas ni terreno)") : ". Motivos: " + reasons) +
             ". Marca los objetos (o sus padres) como Static o revisa los filtros.");
    } else {
        line(std::to_string(meshes_baked) + " mallas" +
             (terrains > 0 ? " y " + std::to_string(terrains) + " terreno(s)" : std::string()) + ": " +
             std::to_string(triangles) + " triángulos");
        if (used_all_non_moving) {
            line("Ningún objeto marcado Static: se hornean las " + std::to_string(meshes_baked) + " mallas que no se mueven");
        }
        for (int i = 0; i < static_cast<int>(BakeSkip::Count); ++i) {
            if (skipped[i] == 0 || static_cast<BakeSkip>(i) == BakeSkip::Inactive || static_cast<BakeSkip>(i) == BakeSkip::Hidden) {
                continue;
            }
            line(std::to_string(skipped[i]) + " omitidas: " + bakeSkipLabel(static_cast<BakeSkip>(i)) + examples_of(i));
        }
    }
    if (terrain_triangles > 0) line("Terreno: " + std::to_string(terrain_triangles) + " triángulos desde el mapa de alturas");
    if (static_with_dynamic_rb > 0) {
        line(std::to_string(static_with_dynamic_rb) +
             " Static con Rigidbody Dynamic: se hornean igual (pon el Rigidbody en Kinematic o quita Static)");
    }
    if (static_with_animator > 0) {
        line(std::to_string(static_with_animator) +
             " Static con un Animator que reproduce: se hornean en su pose de reposo (para el Animator o quita Static)");
    }
    if (skinned_rest_pose > 0) line(std::to_string(skinned_rest_pose) + " mallas con huesos: se hornean en su pose de reposo");
    if (lights_mixed + lights_baked + lights_realtime > 0 || sun_baked || sun_realtime_only) {
        line("Luces: " + std::to_string(lights_baked) + " horneadas, " + std::to_string(lights_mixed) + " mixtas, " +
             std::to_string(lights_realtime) + " solo tiempo real; sol " +
             (sun_baked ? "horneado" : (sun_realtime_only ? "solo tiempo real" : "mixto")));
    }
    return out;
}

// --- Escena ------------------------------------------------------------------------------

bool staticInHierarchy(ecs::Entity entity) {
    for (ecs::Entity e = entity; e.valid(); e = e.parent()) {
        const ecs::EntityInfo* info = e.tryGet<ecs::EntityInfo>();
        if (info != nullptr && info->is_static) return true;
    }
    return false;
}

namespace {

// Posiciones de los vertices en la pose de reposo (como los dibuja el editor
// sin Animator). Sin huesos, las del modelo tal cual.
std::vector<Vec3> restPosePositions(const asset::ModelData& data) {
    std::vector<Vec3> positions(data.vertices.size());
    if (data.bones.empty() || data.nodes.empty()) {
        for (std::size_t i = 0; i < data.vertices.size(); ++i) positions[i] = data.vertices[i].position;
        return positions;
    }
    std::vector<core::Mat4> global(data.nodes.size(), core::Mat4::identity());
    for (std::size_t i = 0; i < data.nodes.size(); ++i) {
        const asset::Node& node = data.nodes[i];
        global[i] = node.parent >= 0 && static_cast<std::size_t>(node.parent) < i ? global[static_cast<std::size_t>(node.parent)] * node.local
                                                                                 : node.local;
    }
    std::vector<core::Mat4> skin(data.bones.size(), core::Mat4::identity());
    for (std::size_t b = 0; b < data.bones.size(); ++b) {
        const asset::Bone& bone = data.bones[b];
        skin[b] = bone.node >= 0 && static_cast<std::size_t>(bone.node) < global.size()
                      ? global[static_cast<std::size_t>(bone.node)] * bone.offset
                      : bone.offset;
    }
    for (std::size_t i = 0; i < data.vertices.size(); ++i) {
        const asset::SkinnedVertex& v = data.vertices[i];
        const Vec4 p{v.position, 1.0f};
        Vec3 sum{};
        float weight = 0.0f;
        for (std::uint32_t k = 0; k < asset::kMaxBoneInfluences; ++k) {
            if (v.weights[k] <= 0.0f || v.joints[k] >= skin.size()) continue;
            const Vec4 q = skin[v.joints[k]] * p;
            sum += Vec3{q.x, q.y, q.z} * v.weights[k];
            weight += v.weights[k];
        }
        positions[i] = weight > 1e-4f ? sum * (1.0f / weight) : v.position;
    }
    return positions;
}

// El terreno desde su mapa de alturas (como el collider: esquina en el
// origen del objeto, `size` x `size` m, alturas 0..1 por `height`). Como
// mucho ~400k triangulos (se salta vertices si hace falta).
std::size_t appendTerrain(ecs::Entity e, const terrain::Terrain& t, const terrain::TerrainData& data,
                          std::vector<BakeTriangle>& out) {
    const std::uint32_t res = data.resolution();
    if (res < 2 || data.heights().size() < static_cast<std::size_t>(res) * res) return 0;
    const double cells = static_cast<double>(res - 1) * static_cast<double>(res - 1);
    const std::uint32_t step = std::max<std::uint32_t>(1u, static_cast<std::uint32_t>(std::ceil(std::sqrt(cells * 2.0 / 400000.0))));
    std::vector<std::uint32_t> samples;
    for (std::uint32_t i = 0; i < res - 1; i += step) samples.push_back(i);
    samples.push_back(res - 1);
    const std::size_t n = samples.size();
    if (n < 2) return 0;
    const core::Mat4& m = e.worldMatrix();
    const float inv = 1.0f / static_cast<float>(res - 1);
    std::vector<Vec3> grid(n * n);
    for (std::size_t z = 0; z < n; ++z) {
        for (std::size_t x = 0; x < n; ++x) {
            const float u = static_cast<float>(samples[x]) * inv;
            const float v = static_cast<float>(samples[z]) * inv;
            grid[z * n + x] = ecs::transformPoint(m, Vec3{u * t.size, data.sample(u, v) * t.height, v * t.size});
        }
    }
    Vec3 albedo{0.18f, 0.2f, 0.12f};
    if (!t.layers.empty() && t.layers.front().albedo.empty()) albedo = srgbToLinear(t.layers.front().tint) * 0.8f;
    const std::size_t before = out.size();
    out.reserve(out.size() + (n - 1) * (n - 1) * 2);
    for (std::size_t z = 0; z + 1 < n; ++z) {
        for (std::size_t x = 0; x + 1 < n; ++x) {
            const Vec3& p00 = grid[z * n + x];
            const Vec3& p10 = grid[z * n + x + 1];
            const Vec3& p01 = grid[(z + 1) * n + x];
            const Vec3& p11 = grid[(z + 1) * n + x + 1];
            // Cara de arriba hacia +Y.
            out.push_back(BakeTriangle{p00, p01, p10, albedo, {}});
            out.push_back(BakeTriangle{p10, p01, p11, albedo, {}});
        }
    }
    return out.size() - before;
}

}  // namespace

void gatherBakeScene(ecs::World& world, assets::AssetManager& assets, BakeScene& scene, const ExtraGeometry& extra,
                     const TerrainProvider& terrains, BakeGatherReport* report) {
    auto& registry = world.registry();
    BakeGatherReport local_report;
    BakeGatherReport& rep = report != nullptr ? *report : local_report;
    // Materiales (.crmat) que sustituyen a los del modelo: se leen una vez.
    struct MaterialColor {
        Vec3 albedo;
        Vec3 emission;
    };
    std::unordered_map<std::uint64_t, std::optional<MaterialColor>> material_cache;
    const auto override_material = [&](const assets::AssetRef& ref) -> std::optional<MaterialColor> {
        if (!ref.valid()) return std::nullopt;
        const std::uint64_t key = ref.uuid.high ^ (ref.uuid.low * 0x9E3779B97F4A7C15ull);
        if (const auto it = material_cache.find(key); it != material_cache.end()) return it->second;
        std::optional<MaterialColor> result;
        const std::optional<assets::AssetInfo> info = assets.database().find(ref.uuid);
        assets::MaterialAsset material;
        if (info && info->type == assets::AssetType::Material && !info->path.empty() &&
            assets::loadMaterial(info->path, material, nullptr)) {
            result = MaterialColor{Vec3{material.base_color.x, material.base_color.y, material.base_color.z} *
                                       (material.albedo.empty() ? 1.0f : 0.6f),
                                   material.emissive * material.emissive_intensity};
        }
        material_cache[key] = result;
        return result;
    };

    // 1) Las mallas que pueden entrar (activas, visibles, que no se mueven).
    struct Candidate {
        entt::entity handle;
        bool is_static;
        std::shared_ptr<const assets::ModelAsset> model;  // nulo = malla de codigo
    };
    std::vector<Candidate> candidates;
    for (const entt::entity h : registry.view<ecs::MeshRenderer>()) {
        const ecs::Entity e = world.wrap(h);
        ++rep.meshes_seen;
        if (!e.activeInHierarchy()) {
            rep.skip(BakeSkip::Inactive, e.name());
            continue;
        }
        const ecs::MeshRenderer& r = e.get<ecs::MeshRenderer>();
        if (!r.visible) {
            rep.skip(BakeSkip::Hidden, e.name());
            continue;
        }
        const bool is_static = staticInHierarchy(e);
        if (const physics::Rigidbody* rb = e.tryGet<physics::Rigidbody>(); rb != nullptr && rb->type == physics::BodyType::Dynamic) {
            // Static manda (como en Unity): se hornea, con un aviso.
            if (!is_static) {
                rep.skip(BakeSkip::DynamicBody, e.name());
                continue;
            }
            ++rep.static_with_dynamic_rb;
        }
        std::shared_ptr<const assets::ModelAsset> model;
        if (!r.mesh) {
            if (!r.model.valid()) {
                rep.skip(BakeSkip::NoModel, e.name());
                continue;
            }
            model = assets.loadModel(r.model.uuid);
            if (!model) {
                rep.skip(BakeSkip::NoModel, e.name());
                continue;
            }
            if (r.part < 0 || r.part >= static_cast<int>(model->parts.size()) || !model->parts[static_cast<std::size_t>(r.part)]) {
                rep.skip(BakeSkip::BadPart, e.name());
                continue;
            }
            // Un Animator que reproduce la mueve: no es geometria fija. Sin
            // Animator (o parado) se dibuja en reposo y asi se hornea. Al
            // instanciar un modelo con animaciones (casi todo .glb/.fbx de
            // Blender) se le pone un Animator que reproduce: si esta marcada
            // Static manda Static (como con el Rigidbody) y va en reposo.
            if (model->animated) {
                const ecs::Animator* animator = e.tryGet<ecs::Animator>();
                if (animator != nullptr && animator->playing) {
                    if (!is_static) {
                        rep.skip(BakeSkip::AnimatorDriven, e.name());
                        continue;
                    }
                    ++rep.static_with_animator;
                }
            }
        }
        candidates.push_back(Candidate{h, is_static, std::move(model)});
    }
    // 2) Solo las Static si hay alguna (como Contribute GI de Unity); si no,
    // todas las que no se mueven.
    bool any_static = false;
    for (const Candidate& c : candidates) any_static = any_static || c.is_static;
    rep.used_all_non_moving = !any_static && !candidates.empty();

    for (const Candidate& c : candidates) {
        const ecs::Entity e = world.wrap(c.handle);
        if (any_static && !c.is_static) {
            rep.skip(BakeSkip::NotStatic, e.name());
            continue;
        }
        if (c.is_static) ++rep.static_marked;
        const ecs::MeshRenderer& r = e.get<ecs::MeshRenderer>();
        const core::Mat4& m = e.worldMatrix();
        const auto tp = [&](const Vec3& p) { return ecs::transformPoint(m, p); };
        const std::size_t before = scene.triangles.size();
        if (r.mesh) {
            const ecs::Mesh& mesh = *r.mesh;
            for (int s = 0; s < mesh.subMeshCount(); ++s) {
                Vec3 albedo{0.5f, 0.5f, 0.5f};
                Vec3 emission{};
                if (s < static_cast<int>(mesh.materials.size())) {
                    const ecs::MeshMaterial& mm = mesh.materials[static_cast<std::size_t>(s)];
                    albedo = srgbToLinear(Vec3{mm.color.x, mm.color.y, mm.color.z}) * (mm.texture.empty() ? 1.0f : 0.6f);
                    emission = srgbToLinear(mm.emission) * mm.emission_intensity;
                }
                const std::vector<std::uint32_t>& idx = mesh.triangles(s);
                for (std::size_t t = 0; t + 2 < idx.size(); t += 3) {
                    if (idx[t] >= mesh.vertices.size() || idx[t + 1] >= mesh.vertices.size() || idx[t + 2] >= mesh.vertices.size()) continue;
                    scene.triangles.push_back(BakeTriangle{tp(mesh.vertices[idx[t]]), tp(mesh.vertices[idx[t + 1]]),
                                                           tp(mesh.vertices[idx[t + 2]]), albedo, emission});
                }
            }
        } else {
            const asset::ModelData& data = *c.model->parts[static_cast<std::size_t>(r.part)];
            if (data.bones.size() > 1) ++rep.skinned_rest_pose;
            const std::vector<Vec3> positions = restPosePositions(data);
            for (const asset::SubMesh& sm : data.submeshes) {
                Vec3 albedo{0.5f, 0.5f, 0.5f};
                Vec3 emission{};
                if (sm.material < data.materials.size()) {
                    const asset::MaterialData& md = data.materials[sm.material];
                    albedo = Vec3{md.base_color.x, md.base_color.y, md.base_color.z} * (md.albedo_texture >= 0 ? 0.6f : 1.0f);
                    emission = md.emissive;
                }
                if (sm.material < r.materials.size()) {
                    if (const std::optional<MaterialColor> over = override_material(r.materials[sm.material])) {
                        albedo = over->albedo;
                        emission = over->emission;
                    }
                }
                for (std::uint32_t t = sm.first_index; t + 2 < sm.first_index + sm.index_count && t + 2 < data.indices.size(); t += 3) {
                    const std::uint32_t i0 = data.indices[t], i1 = data.indices[t + 1], i2 = data.indices[t + 2];
                    if (i0 >= positions.size() || i1 >= positions.size() || i2 >= positions.size()) continue;
                    scene.triangles.push_back(BakeTriangle{tp(positions[i0]), tp(positions[i1]), tp(positions[i2]), albedo, emission});
                }
            }
        }
        if (scene.triangles.size() > before) ++rep.meshes_baked;
    }
    // 3) El terreno (tambien las teselas de un TerrainGrid), desde sus alturas.
    if (terrains) {
        for (const entt::entity h : registry.view<terrain::Terrain>()) {
            const ecs::Entity e = world.wrap(h);
            if (!e.activeInHierarchy()) continue;
            const std::shared_ptr<const terrain::TerrainData> data = terrains(e);
            if (!data) continue;
            const std::size_t added = appendTerrain(e, e.get<terrain::Terrain>(), *data, scene.triangles);
            if (added > 0) {
                ++rep.terrains;
                rep.terrain_triangles += added;
            }
        }
    }
    if (extra) extra(scene.triangles);
    rep.triangles = scene.triangles.size();
    for (const entt::entity h : registry.view<LightProbeVolume>()) {
        const ecs::Entity e = world.wrap(h);
        if (!e.activeInHierarchy()) continue;
        const LightProbeVolume& v = e.get<LightProbeVolume>();
        BakeVolume b;
        const Vec3 size{std::max(std::fabs(v.size.x), 0.5f), std::max(std::fabs(v.size.y), 0.5f), std::max(std::fabs(v.size.z), 0.5f)};
        b.min = e.worldPosition() - size * 0.5f;
        b.size = size;
        const float spacing = std::max(v.spacing, 0.1f);
        b.nx = std::max(2u, static_cast<std::uint32_t>(std::lround(size.x / spacing)) + 1u);
        b.ny = std::max(2u, static_cast<std::uint32_t>(std::lround(size.y / spacing)) + 1u);
        b.nz = std::max(2u, static_cast<std::uint32_t>(std::lround(size.z / spacing)) + 1u);
        b.intensity = v.intensity;
        scene.volumes.push_back(b);
    }
}

void gatherBakeLights(ecs::World& world, const scene::LightSet& sun, BakeScene& scene, ecs::LightBakeMode sun_mode,
                      BakeGatherReport* report) {
    constexpr float kDegToRad = kPi / 180.0f;
    scene.lights.sun = sun.sun;
    scene.lights.sky = sun.sky;
    scene.lights.ambient = sun.ambient;
    scene.lights.points.clear();
    scene.lights.spots.clear();
    scene.point_baked.clear();
    scene.spot_baked.clear();
    scene.baked_light_ids.clear();
    bool directional = false;
    for (const entt::entity h : world.registry().view<ecs::Light>()) {
        const ecs::Entity e = world.wrap(h);
        if (!e.activeInHierarchy()) continue;
        const ecs::Light& light = e.get<ecs::Light>();
        if (light.type == ecs::LightType::Directional) {
            // El primero manda (como el renderizador).
            if (!directional) {
                directional = true;
                sun_mode = light.bake_mode;
            }
            continue;
        }
        if (light.intensity <= 0.0f) continue;
        if (report != nullptr) {
            if (light.bake_mode == ecs::LightBakeMode::Baked) ++report->lights_baked;
            else if (light.bake_mode == ecs::LightBakeMode::Realtime) ++report->lights_realtime;
            else ++report->lights_mixed;
        }
        if (light.bake_mode == ecs::LightBakeMode::Realtime) continue;
        const std::uint8_t baked = light.bake_mode == ecs::LightBakeMode::Baked ? 1 : 0;
        if (light.type == ecs::LightType::Point) {
            scene::PointLight p{};
            p.position = e.worldPosition();
            p.color = light.color;
            p.intensity = light.intensity;
            p.range = light.range;
            p.cast_shadows = light.cast_shadows;
            p.shadow_strength = std::clamp(light.shadow_strength, 0.0f, 1.0f);
            p.source_radius = std::clamp(light.source_radius, 0.0f, 2.0f);
            scene.lights.points.push_back(p);
            scene.point_baked.push_back(baked);
        } else {
            scene::SpotLight s{};
            s.position = e.worldPosition();
            s.direction = e.forward();
            s.color = light.color;
            s.intensity = light.intensity;
            s.range = light.range;
            const float inner = std::min(light.inner_angle, light.outer_angle - 0.5f);
            s.inner_angle = std::max(inner, 0.5f) * kDegToRad;
            s.outer_angle = light.outer_angle * kDegToRad;
            s.enabled = true;
            s.cast_shadows = light.cast_shadows;
            s.shadow_strength = std::clamp(light.shadow_strength, 0.0f, 1.0f);
            s.source_radius = std::clamp(light.source_radius, 0.0f, 2.0f);
            scene.lights.spots.push_back(s);
            scene.spot_baked.push_back(baked);
        }
        if (baked != 0) scene.baked_light_ids.push_back({e.uuid().high, e.uuid().low});
    }
    if (sun_mode == ecs::LightBakeMode::Realtime) scene.lights.sun.intensity = 0.0f;
    scene.sun_baked = sun_mode == ecs::LightBakeMode::Baked && scene.lights.sun.intensity > 0.0f;
    if (report != nullptr) {
        report->sun_baked = scene.sun_baked;
        report->sun_realtime_only = sun_mode == ecs::LightBakeMode::Realtime;
    }
}

BakeVolume volumeAround(const std::vector<BakeTriangle>& triangles, float spacing) {
    Vec3 mn{1e30f, 1e30f, 1e30f}, mx{-1e30f, -1e30f, -1e30f};
    for (const BakeTriangle& t : triangles) {
        mn = minv(mn, minv(t.a, minv(t.b, t.c)));
        mx = maxv(mx, maxv(t.a, maxv(t.b, t.c)));
    }
    BakeVolume v;
    if (triangles.empty()) {
        v.min = Vec3{-10.0f, 0.0f, -10.0f};
        v.size = Vec3{20.0f, 6.0f, 20.0f};
    } else {
        // Un poco mas grande y como mucho 400 x 60 x 400 m.
        mn = mn - Vec3{1.0f, 0.5f, 1.0f};
        mx = mx + Vec3{1.0f, 2.0f, 1.0f};
        Vec3 size = mx - mn;
        size = Vec3{std::min(size.x, 400.0f), std::min(size.y, 60.0f), std::min(size.z, 400.0f)};
        v.min = (mn + mx) * 0.5f - size * 0.5f;
        v.size = size;
    }
    spacing = std::max(spacing, 0.25f);
    v.nx = std::max(2u, static_cast<std::uint32_t>(std::lround(v.size.x / spacing)) + 1u);
    v.ny = std::max(2u, static_cast<std::uint32_t>(std::lround(v.size.y / spacing)) + 1u);
    v.nz = std::max(2u, static_cast<std::uint32_t>(std::lround(v.size.z / spacing)) + 1u);
    return v;
}

// --- Archivo -----------------------------------------------------------------------------

namespace {
constexpr char kMagic[4] = {'C', 'R', 'B', 'K'};
constexpr std::uint32_t kVersion = 3;  // 2: + lightmap de superficie; 3: + luces horneadas (directa)
}  // namespace

std::filesystem::path bakedLightingFile(const std::filesystem::path& scene_file) {
    std::filesystem::path p = scene_file;
    p.replace_extension(kBakedLightingExtension);
    return p;
}

bool saveBakedLighting(const std::filesystem::path& file, const gfx::BakedLighting& data, gfx::LightingMode mode, std::string* error) {
    const std::filesystem::path temporary = file.string() + ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) {
            if (error) *error = "no se pudo escribir " + file.string();
            return false;
        }
        out.write(kMagic, 4);
        const std::uint32_t header[3] = {kVersion, static_cast<std::uint32_t>(mode), static_cast<std::uint32_t>(data.volumes.size())};
        out.write(reinterpret_cast<const char*>(header), sizeof(header));
        for (const gfx::BakedProbeVolume& v : data.volumes) {
            const float f[7] = {v.min.x, v.min.y, v.min.z, v.size.x, v.size.y, v.size.z, v.intensity};
            const std::uint32_t u[4] = {v.nx, v.ny, v.nz, v.first};
            out.write(reinterpret_cast<const char*>(f), sizeof(f));
            out.write(reinterpret_cast<const char*>(u), sizeof(u));
        }
        const std::uint64_t count = data.probes.size();
        out.write(reinterpret_cast<const char*>(&count), sizeof(count));
        if (count > 0) out.write(reinterpret_cast<const char*>(data.probes.data()), static_cast<std::streamsize>(count * sizeof(Vec4)));
        // Lightmap de superficie (version 2).
        const gfx::SurfaceLightmap& sl = data.surface;
        const std::uint32_t surface_header[3] = {sl.capacity, sl.count, 0};
        out.write(reinterpret_cast<const char*>(&sl.cell_size), sizeof(float));
        out.write(reinterpret_cast<const char*>(surface_header), sizeof(surface_header));
        if (sl.capacity > 0) {
            out.write(reinterpret_cast<const char*>(sl.keys.data()), static_cast<std::streamsize>(sl.keys.size() * sizeof(std::uint32_t)));
            out.write(reinterpret_cast<const char*>(sl.texels.data()), static_cast<std::streamsize>(sl.texels.size() * sizeof(Vec4)));
        }
        // Luces horneadas (version 3): el sol y los UUID de las locales.
        const std::uint32_t light_header[2] = {data.sun_baked ? 1u : 0u, static_cast<std::uint32_t>(data.baked_lights.size())};
        const float sun_direction[3] = {data.sun_direction.x, data.sun_direction.y, data.sun_direction.z};
        out.write(reinterpret_cast<const char*>(light_header), sizeof(light_header));
        out.write(reinterpret_cast<const char*>(sun_direction), sizeof(sun_direction));
        for (const auto& id : data.baked_lights) out.write(reinterpret_cast<const char*>(id.data()), sizeof(std::uint64_t) * 2);
        if (!out) {
            if (error) *error = "error escribiendo " + file.string();
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(temporary, file, ec);
    if (ec) {
        if (error) *error = ec.message();
        return false;
    }
    return true;
}

bool loadBakedLighting(const std::filesystem::path& file, gfx::BakedLighting& data, gfx::LightingMode& mode, std::string* error) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        if (error) *error = "no existe " + file.string();
        return false;
    }
    char magic[4] = {};
    in.read(magic, 4);
    std::uint32_t header[3] = {};
    in.read(reinterpret_cast<char*>(header), sizeof(header));
    if (!in || std::memcmp(magic, kMagic, 4) != 0 || header[0] < 1 || header[0] > kVersion || header[2] > 1024) {
        if (error) *error = "archivo de iluminacion danado: " + file.string();
        return false;
    }
    gfx::BakedLighting result;
    mode = header[1] == 1 ? gfx::LightingMode::Baked : gfx::LightingMode::Realtime;
    for (std::uint32_t i = 0; i < header[2]; ++i) {
        float f[7] = {};
        std::uint32_t u[4] = {};
        in.read(reinterpret_cast<char*>(f), sizeof(f));
        in.read(reinterpret_cast<char*>(u), sizeof(u));
        gfx::BakedProbeVolume v;
        v.min = Vec3{f[0], f[1], f[2]};
        v.size = Vec3{f[3], f[4], f[5]};
        v.intensity = f[6];
        v.nx = u[0];
        v.ny = u[1];
        v.nz = u[2];
        v.first = u[3];
        result.volumes.push_back(v);
    }
    std::uint64_t count = 0;
    in.read(reinterpret_cast<char*>(&count), sizeof(count));
    if (!in || count > (1ull << 28)) {
        if (error) *error = "archivo de iluminacion danado: " + file.string();
        return false;
    }
    result.probes.resize(static_cast<std::size_t>(count));
    if (count > 0) in.read(reinterpret_cast<char*>(result.probes.data()), static_cast<std::streamsize>(count * sizeof(Vec4)));
    if (!in) {
        if (error) *error = "archivo de iluminacion incompleto: " + file.string();
        return false;
    }
    if (header[0] >= 2) {
        gfx::SurfaceLightmap& sl = result.surface;
        std::uint32_t surface_header[3] = {};
        in.read(reinterpret_cast<char*>(&sl.cell_size), sizeof(float));
        in.read(reinterpret_cast<char*>(surface_header), sizeof(surface_header));
        if (!in || surface_header[0] > (1u << 26) || (surface_header[0] & (surface_header[0] - 1)) != 0) {
            if (error) *error = "archivo de iluminacion danado: " + file.string();
            return false;
        }
        sl.capacity = surface_header[0];
        sl.count = surface_header[1];
        if (sl.capacity > 0) {
            sl.keys.resize(static_cast<std::size_t>(sl.capacity) * 2);
            sl.texels.resize(static_cast<std::size_t>(sl.capacity) * gfx::kSurfaceTexelVec4);
            in.read(reinterpret_cast<char*>(sl.keys.data()), static_cast<std::streamsize>(sl.keys.size() * sizeof(std::uint32_t)));
            in.read(reinterpret_cast<char*>(sl.texels.data()), static_cast<std::streamsize>(sl.texels.size() * sizeof(Vec4)));
            if (!in) {
                if (error) *error = "archivo de iluminacion incompleto: " + file.string();
                return false;
            }
        }
    }
    if (header[0] >= 3) {
        std::uint32_t light_header[2] = {};
        float sun_direction[3] = {};
        in.read(reinterpret_cast<char*>(light_header), sizeof(light_header));
        in.read(reinterpret_cast<char*>(sun_direction), sizeof(sun_direction));
        if (!in || light_header[1] > (1u << 20)) {
            if (error) *error = "archivo de iluminacion danado: " + file.string();
            return false;
        }
        result.sun_baked = light_header[0] != 0;
        result.sun_direction = Vec3{sun_direction[0], sun_direction[1], sun_direction[2]};
        result.baked_lights.resize(light_header[1]);
        for (auto& id : result.baked_lights) in.read(reinterpret_cast<char*>(id.data()), sizeof(std::uint64_t) * 2);
        if (!in) {
            if (error) *error = "archivo de iluminacion incompleto: " + file.string();
            return false;
        }
    }
    data = std::move(result);
    return true;
}

Vec3 probeIrradiance(const Vec4* probe, const Vec3& n) {
    float y[9];
    shBasis(n, y);
    Vec3 r{};
    for (int k = 0; k < 9; ++k) {
        r += Vec3{probeFloat(probe, k * 3), probeFloat(probe, k * 3 + 1), probeFloat(probe, k * 3 + 2)} * y[k];
    }
    return Vec3{std::max(r.x, 0.0f), std::max(r.y, 0.0f), std::max(r.z, 0.0f)};
}

float probeSkyVisibility(const Vec4* probe, const Vec3& n) {
    float y[9];
    shBasis(n, y);
    float v = 0.0f;
    for (int k = 0; k < 4; ++k) v += probeFloat(probe, 27 + k) * y[k];
    return std::clamp(v, 0.0f, 1.0f);
}

}  // namespace cramion::lighting
