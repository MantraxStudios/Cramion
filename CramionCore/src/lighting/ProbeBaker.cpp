#include "CramionCore/lighting/ProbeBaker.h"

#include "CramionCore/asset/AssetManager.h"
#include "CramionCore/ecs/Components.h"
#include "CramionCore/ecs/MathUtil.h"
#include "CramionCore/ecs/RuntimeMesh.h"
#include "CramionCore/physics/PhysicsComponents.h"

#include <CramionFX/asset/Model.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <random>
#include <thread>

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

// --- Horneado ----------------------------------------------------------------------------

bool bakeProbes(const BakeScene& scene, gfx::BakedLighting& out, BakeProgress* progress, BakeStats* stats, std::string* error) {
    const auto start = std::chrono::steady_clock::now();
    out = gfx::BakedLighting{};
    if (scene.volumes.empty()) {
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
    if (stats != nullptr) {
        stats->triangles = scene.triangles.size();
        stats->probes = jobs.size();
        stats->invalid_probes = invalid.load();
        stats->seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    }
    if (progress) progress->fraction.store(1.0f);
    return true;
}

// --- Escena ------------------------------------------------------------------------------

void gatherBakeScene(ecs::World& world, assets::AssetManager& assets, BakeScene& scene, const ExtraGeometry& extra) {
    auto& registry = world.registry();
    // Solo los "Static" si hay alguno marcado (como Unity con Contribute GI).
    bool any_static = false;
    for (const entt::entity h : registry.view<ecs::MeshRenderer>()) {
        const ecs::EntityInfo* info = registry.try_get<ecs::EntityInfo>(h);
        if (info != nullptr && info->is_static) {
            any_static = true;
            break;
        }
    }
    for (const entt::entity h : registry.view<ecs::MeshRenderer>()) {
        const ecs::Entity e = world.wrap(h);
        if (!e.activeInHierarchy()) continue;
        const ecs::MeshRenderer& r = e.get<ecs::MeshRenderer>();
        if (!r.visible) continue;
        const ecs::EntityInfo* info = e.tryGet<ecs::EntityInfo>();
        if (any_static && (info == nullptr || !info->is_static)) continue;
        if (const physics::Rigidbody* rb = e.tryGet<physics::Rigidbody>(); rb != nullptr && rb->type == physics::BodyType::Dynamic) continue;
        const core::Mat4& m = e.worldMatrix();
        const auto tp = [&](const Vec3& p) { return ecs::transformPoint(m, p); };
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
            continue;
        }
        if (!r.model.valid()) continue;
        const std::shared_ptr<const assets::ModelAsset> model = assets.loadModel(r.model.uuid);
        if (!model || model->animated) continue;
        if (r.part < 0 || r.part >= static_cast<int>(model->parts.size()) || !model->parts[static_cast<std::size_t>(r.part)]) continue;
        const asset::ModelData& data = *model->parts[static_cast<std::size_t>(r.part)];
        for (const asset::SubMesh& sm : data.submeshes) {
            Vec3 albedo{0.5f, 0.5f, 0.5f};
            Vec3 emission{};
            if (sm.material < data.materials.size()) {
                const asset::MaterialData& md = data.materials[sm.material];
                albedo = Vec3{md.base_color.x, md.base_color.y, md.base_color.z} * (md.albedo_texture >= 0 ? 0.6f : 1.0f);
                emission = md.emissive;
            }
            for (std::uint32_t t = sm.first_index; t + 2 < sm.first_index + sm.index_count && t + 2 < data.indices.size(); t += 3) {
                const std::uint32_t i0 = data.indices[t], i1 = data.indices[t + 1], i2 = data.indices[t + 2];
                if (i0 >= data.vertices.size() || i1 >= data.vertices.size() || i2 >= data.vertices.size()) continue;
                scene.triangles.push_back(BakeTriangle{tp(data.vertices[i0].position), tp(data.vertices[i1].position),
                                                       tp(data.vertices[i2].position), albedo, emission});
            }
        }
    }
    if (extra) extra(scene.triangles);
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
constexpr std::uint32_t kVersion = 1;
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
    if (!in || std::memcmp(magic, kMagic, 4) != 0 || header[0] != kVersion || header[2] > 1024) {
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
