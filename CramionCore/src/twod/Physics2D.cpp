#include "CramionCore/twod/Physics2D.h"

#include "CramionCore/ecs/Components.h"
#include "CramionCore/ecs/MathUtil.h"
#include "CramionCore/twod/Tilemap2D.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace cramion::twod {

namespace {

// --- Vector 2D con operadores (core::Vec2 no los tiene) --------------------------

struct V2 {
    float x = 0.0f;
    float y = 0.0f;
    V2() = default;
    constexpr V2(float x_, float y_) : x(x_), y(y_) {}
    V2(const core::Vec2& v) : x(v.x), y(v.y) {}  // NOLINT
    core::Vec2 vec() const { return core::Vec2{x, y}; }
};
inline V2 operator+(V2 a, V2 b) { return V2{a.x + b.x, a.y + b.y}; }
inline V2 operator-(V2 a, V2 b) { return V2{a.x - b.x, a.y - b.y}; }
inline V2 operator-(V2 a) { return V2{-a.x, -a.y}; }
inline V2 operator*(V2 a, float s) { return V2{a.x * s, a.y * s}; }
inline V2 operator*(float s, V2 a) { return V2{a.x * s, a.y * s}; }
inline V2& operator+=(V2& a, V2 b) {
    a.x += b.x;
    a.y += b.y;
    return a;
}
inline V2& operator-=(V2& a, V2 b) {
    a.x -= b.x;
    a.y -= b.y;
    return a;
}
inline float dot(V2 a, V2 b) { return a.x * b.x + a.y * b.y; }
inline float cross(V2 a, V2 b) { return a.x * b.y - a.y * b.x; }
inline V2 cross(V2 v, float s) { return V2{s * v.y, -s * v.x}; }
inline V2 cross(float s, V2 v) { return V2{-s * v.y, s * v.x}; }
inline float len(V2 v) { return std::sqrt(v.x * v.x + v.y * v.y); }
inline V2 norm(V2 v) {
    const float l = len(v);
    return l > 1e-9f ? v * (1.0f / l) : V2{0.0f, 1.0f};
}
inline V2 rotate(float c, float s, V2 v) { return V2{c * v.x - s * v.y, s * v.x + c * v.y}; }

constexpr float kSlop = 0.005f;
constexpr float kBaumgarte = 0.2f;
constexpr float kSleepSpeed = 0.05f;
constexpr float kSleepTime = 0.5f;

std::uint64_t pairKey(entt::entity a, entt::entity b) {
    std::uint32_t x = static_cast<std::uint32_t>(a);
    std::uint32_t y = static_cast<std::uint32_t>(b);
    if (x > y) std::swap(x, y);
    return (static_cast<std::uint64_t>(x) << 32) | y;
}

// Envolvente convexa (antihoraria), monotone chain.
std::vector<V2> convexHull(std::vector<V2> p) {
    if (p.size() < 3) return p;
    std::sort(p.begin(), p.end(), [](V2 a, V2 b) { return a.x < b.x || (a.x == b.x && a.y < b.y); });
    std::vector<V2> h(p.size() * 2);
    std::size_t k = 0;
    for (std::size_t i = 0; i < p.size(); ++i) {
        while (k >= 2 && cross(h[k - 1] - h[k - 2], p[i] - h[k - 2]) <= 1e-9f) --k;
        h[k++] = p[i];
    }
    for (std::size_t i = p.size() - 1, t = k + 1; i > 0; --i) {
        while (k >= t && cross(h[k - 1] - h[k - 2], p[i - 1] - h[k - 2]) <= 1e-9f) --k;
        h[k++] = p[i - 1];
    }
    h.resize(k > 1 ? k - 1 : k);
    return h;
}

float angleFromQuat(const core::Quat& q) {
    return std::atan2(2.0f * (q.w * q.z + q.x * q.y), 1.0f - 2.0f * (q.y * q.y + q.z * q.z));
}

core::Quat quatZ(float angle) {
    return core::Quat{0.0f, 0.0f, std::sin(angle * 0.5f), std::cos(angle * 0.5f)};
}

}  // namespace

// --- Datos ------------------------------------------------------------------------------

struct Physics2DWorld::Impl {
    struct Shape {
        bool circle = false;
        std::vector<V2> verts;    // espacio del cuerpo, antihorarios
        std::vector<V2> normals;
        V2 center{};
        float radius = 0.0f;
        Collider2DMaterial material{};
        // En el mundo (cada paso)
        std::vector<V2> wverts;
        std::vector<V2> wnormals;
        V2 wcenter{};
        float min_x = 0.0f, min_y = 0.0f, max_x = 0.0f, max_y = 0.0f;
    };
    struct Body {
        entt::entity handle = entt::null;
        BodyType2D type = BodyType2D::Static;
        V2 pos{};
        float angle = 0.0f;
        V2 vel{};
        float w = 0.0f;
        float mass = 0.0f;
        float inv_mass = 0.0f;
        float inv_inertia = 0.0f;
        float linear_drag = 0.0f;
        float angular_drag = 0.0f;
        float gravity_scale = 1.0f;
        bool freeze_rotation = false;
        bool freeze_x = false;
        bool freeze_y = false;
        bool allow_sleep = true;
        bool interpolate = true;
        std::vector<Shape> shapes;
        int layer = 0;
        V2 force{};
        float torque = 0.0f;
        bool sleeping = false;
        float still = 0.0f;
        V2 prev_pos{};
        float prev_angle = 0.0f;
        float z = 0.0f;
        core::Vec3 scale{1.0f, 1.0f, 1.0f};
        V2 written_pos{};
        float written_angle = 0.0f;
        bool has_written = false;
        std::uint64_t signature = 0;
        bool seen = false;
        bool dynamic() const { return type == BodyType2D::Dynamic; }
    };
    struct Point {
        V2 p{};
        float separation = 0.0f;
        float pn = 0.0f;
        float pt = 0.0f;
        float mass_n = 0.0f;
        float mass_t = 0.0f;
        float bias = 0.0f;
        V2 ra{};
        V2 rb{};
    };
    struct Manifold {
        int a = 0, b = 0;    // cuerpos
        int sa = 0, sb = 0;  // formas
        V2 normal{};         // de a hacia b
        int count = 0;
        Point points[2];
        float friction = 0.0f;
        float restitution = 0.0f;
        bool trigger = false;
    };
    struct PairInfo {
        entt::entity a = entt::null;
        entt::entity b = entt::null;
        bool trigger = false;
        core::Vec3 point{};
        core::Vec3 normal{};
        core::Vec3 relative_velocity{};
    };

    Physics2DWorld* owner = nullptr;
    TilesetLibrary* tilesets = nullptr;
    ecs::World* world = nullptr;
    bool running = false;
    float accumulator = 0.0f;
    std::vector<Body> bodies;
    std::unordered_map<entt::entity, int> index;
    std::vector<Manifold> manifolds;
    std::map<std::uint64_t, PairInfo> pairs;       // del ultimo update
    std::map<std::uint64_t, PairInfo> step_pairs;  // de los pasos de este update
    std::vector<Physics2DEvent> events;
    std::uint64_t steps = 0;

    // --- Construccion -----------------------------------------------------------

    static void finishPolygon(Shape& s) {
        s.verts = convexHull(s.verts);
        s.normals.resize(s.verts.size());
        for (std::size_t i = 0; i < s.verts.size(); ++i) {
            const V2 e = s.verts[(i + 1) % s.verts.size()] - s.verts[i];
            s.normals[i] = norm(V2{e.y, -e.x});
        }
    }

    static bool hasCollider(const ecs::Entity& e) {
        return e.has<BoxCollider2D>() || e.has<CircleCollider2D>() || e.has<PolygonCollider2D>() ||
               e.has<TilemapCollider2D>();
    }

    std::uint64_t signatureOf(const ecs::Entity& e, const core::Vec3& scale) const {
        // Huella barata de lo que cambia las formas.
        std::uint64_t h = 1469598103934665603ull;
        const auto mix = [&h](const void* data, std::size_t size) {
            const auto* bytes = static_cast<const unsigned char*>(data);
            for (std::size_t i = 0; i < size; ++i) {
                h ^= bytes[i];
                h *= 1099511628211ull;
            }
        };
        const auto mixMat = [&](const Collider2DMaterial& m) {
            mix(&m.is_trigger, sizeof(bool));
            mix(&m.friction, sizeof(float));
            mix(&m.bounciness, sizeof(float));
            mix(&m.one_way, sizeof(bool));
        };
        mix(&scale, sizeof(scale));
        if (const auto* b = e.tryGet<BoxCollider2D>()) {
            mixMat(b->material);
            mix(&b->size, sizeof(b->size));
            mix(&b->offset, sizeof(b->offset));
        }
        if (const auto* c = e.tryGet<CircleCollider2D>()) {
            mixMat(c->material);
            mix(&c->radius, sizeof(float));
            mix(&c->offset, sizeof(c->offset));
        }
        if (const auto* p = e.tryGet<PolygonCollider2D>()) {
            mixMat(p->material);
            for (const core::Vec2& v : p->points) mix(&v, sizeof(v));
            mix(&p->offset, sizeof(p->offset));
        }
        if (const auto* t = e.tryGet<TilemapCollider2D>()) {
            mix(&t->is_trigger, sizeof(bool));
            mix(&t->friction, sizeof(float));
            mix(&t->bounciness, sizeof(float));
            mix(&t->one_way, sizeof(bool));
            if (const auto* map = e.tryGet<Tilemap>()) {
                mix(&map->version, sizeof(map->version));
                mix(&map->cell_size, sizeof(map->cell_size));
                mix(map->tileset.data(), map->tileset.size());
            }
        }
        if (const auto* r = e.tryGet<Rigidbody2D>()) {
            mix(&r->type, sizeof(r->type));
            mix(&r->mass, sizeof(float));
        }
        if (const auto* comp = e.tryGet<CompositeCollider2D>()) mixMat(comp->material);
        return h;
    }

    void buildShapes(const ecs::Entity& e, Body& body) {
        body.shapes.clear();
        const core::Vec3& s = body.scale;
        const V2 sc{s.x, s.y};
        const CompositeCollider2D* composite = e.tryGet<CompositeCollider2D>();
        const auto material = [&](const Collider2DMaterial& m, bool used_by_composite) {
            return composite != nullptr && used_by_composite ? composite->material : m;
        };
        if (const auto* b = e.tryGet<BoxCollider2D>()) {
            Shape shape;
            shape.material = material(b->material, b->used_by_composite);
            const V2 h{std::fabs(b->size.x * sc.x) * 0.5f, std::fabs(b->size.y * sc.y) * 0.5f};
            const V2 o{b->offset.x * sc.x, b->offset.y * sc.y};
            shape.verts = {o + V2{-h.x, -h.y}, o + V2{h.x, -h.y}, o + V2{h.x, h.y}, o + V2{-h.x, h.y}};
            finishPolygon(shape);
            if (shape.verts.size() >= 3) body.shapes.push_back(std::move(shape));
        }
        if (const auto* c = e.tryGet<CircleCollider2D>()) {
            Shape shape;
            shape.circle = true;
            shape.material = c->material;
            shape.center = V2{c->offset.x * sc.x, c->offset.y * sc.y};
            shape.radius = std::max(c->radius * std::max(std::fabs(sc.x), std::fabs(sc.y)), 0.001f);
            body.shapes.push_back(std::move(shape));
        }
        if (const auto* p = e.tryGet<PolygonCollider2D>()) {
            Shape shape;
            shape.material = p->material;
            for (std::size_t i = 0; i < p->points.size() && i < 16; ++i) {
                shape.verts.push_back(V2{(p->points[i].x + p->offset.x) * sc.x, (p->points[i].y + p->offset.y) * sc.y});
            }
            finishPolygon(shape);
            if (shape.verts.size() >= 3) body.shapes.push_back(std::move(shape));
        }
        if (const auto* t = e.tryGet<TilemapCollider2D>()) {
            if (const auto* map = e.tryGet<Tilemap>()) {
                const Tileset* tileset = tilesets != nullptr && !map->tileset.empty() ? tilesets->get(map->tileset) : nullptr;
                Collider2DMaterial m;
                m.is_trigger = t->is_trigger;
                m.friction = t->friction;
                m.bounciness = t->bounciness;
                m.one_way = t->one_way;
                m = material(m, t->used_by_composite);
                const float cw = map->cell_size.x * sc.x;
                const float ch = map->cell_size.y * sc.y;
                // Las plataformas de un sentido no se juntan en vertical (cada
                // fila es una plataforma).
                for (const TileRect& r : solidRects(*map, tileset, !m.one_way)) {
                    Shape shape;
                    shape.material = m;
                    const float x0 = static_cast<float>(r.x) * cw;
                    const float y0 = static_cast<float>(r.y) * ch;
                    const float x1 = static_cast<float>(r.x + r.w) * cw;
                    const float y1 = static_cast<float>(r.y + r.h) * ch;
                    shape.verts = {V2{x0, y0}, V2{x1, y0}, V2{x1, y1}, V2{x0, y1}};
                    finishPolygon(shape);
                    if (shape.verts.size() >= 3) body.shapes.push_back(std::move(shape));
                }
            }
        }
    }

    void computeMass(Body& body, const Rigidbody2D* rb) {
        body.inv_mass = 0.0f;
        body.inv_inertia = 0.0f;
        body.mass = 0.0f;
        if (!body.dynamic()) return;
        // Area e inercia (respecto al origen) de todas las formas.
        float area = 0.0f;
        float inertia_unit = 0.0f;  // por unidad de densidad
        for (const Shape& s : body.shapes) {
            if (s.material.is_trigger) continue;
            if (s.circle) {
                const float a = 3.14159265f * s.radius * s.radius;
                area += a;
                inertia_unit += a * (0.5f * s.radius * s.radius + dot(s.center, s.center));
            } else {
                for (std::size_t i = 0; i < s.verts.size(); ++i) {
                    const V2 p1 = s.verts[i];
                    const V2 p2 = s.verts[(i + 1) % s.verts.size()];
                    const float d = cross(p1, p2);
                    area += 0.5f * d;
                    inertia_unit += d * (dot(p1, p1) + dot(p1, p2) + dot(p2, p2)) / 12.0f;
                }
            }
        }
        const float mass = std::max(rb != nullptr ? rb->mass : 1.0f, 0.001f);
        body.mass = mass;
        body.inv_mass = 1.0f / mass;
        if (area > 1e-6f && !body.freeze_rotation) {
            const float inertia = inertia_unit * (mass / area);
            body.inv_inertia = inertia > 1e-9f ? 1.0f / inertia : 0.0f;
        }
    }

    void readTransform(const ecs::Entity& e, V2& pos, float& angle, float& z, core::Vec3& scale) const {
        core::Vec3 p;
        core::Quat q;
        ecs::decomposeMatrix(e.worldMatrix(), p, q, scale);
        pos = V2{p.x, p.y};
        z = p.z;
        angle = angleFromQuat(q);
    }

    void addOrUpdate(const ecs::Entity& e) {
        V2 pos;
        float angle = 0.0f;
        float z = 0.0f;
        core::Vec3 scale;
        readTransform(e, pos, angle, z, scale);
        const std::uint64_t signature = signatureOf(e, scale);
        auto found = index.find(e.handle());
        Body* body = nullptr;
        if (found == index.end()) {
            index[e.handle()] = static_cast<int>(bodies.size());
            bodies.emplace_back();
            body = &bodies.back();
            body->handle = e.handle();
            body->pos = pos;
            body->angle = angle;
            body->prev_pos = pos;
            body->prev_angle = angle;
            body->z = z;
            body->signature = signature + 1;  // fuerza la construccion
            if (const auto* rb = e.tryGet<Rigidbody2D>()) {
                body->vel = rb->initial_velocity;
            }
        } else {
            body = &bodies[static_cast<std::size_t>(found->second)];
        }
        body->seen = true;
        const Rigidbody2D* rb = e.tryGet<Rigidbody2D>();
        body->type = rb != nullptr ? rb->type : BodyType2D::Static;
        if (const auto* info = e.tryGet<ecs::EntityInfo>()) body->layer = info->layer;
        if (rb != nullptr) {
            body->linear_drag = rb->linear_drag;
            body->angular_drag = rb->angular_drag;
            body->gravity_scale = rb->gravity_scale;
            body->freeze_rotation = rb->freeze_rotation;
            body->freeze_x = rb->freeze_x;
            body->freeze_y = rb->freeze_y;
            body->allow_sleep = rb->allow_sleep;
            body->interpolate = rb->interpolate;
        }
        if (body->signature != signature) {
            body->signature = signature;
            body->scale = scale;
            buildShapes(e, *body);
            computeMass(*body, rb);
            body->sleeping = false;
        }
        // Lo que no simula la fisica sigue a su Transform; un dinamico movido
        // por un script (teletransporte) tambien.
        if (!body->dynamic()) {
            body->prev_pos = body->pos;
            body->prev_angle = body->angle;
            body->pos = pos;
            body->angle = angle;
            body->z = z;
        } else if (body->has_written && (len(pos - body->written_pos) > 1e-4f || std::fabs(angle - body->written_angle) > 1e-4f)) {
            body->pos = pos;
            body->angle = angle;
            body->prev_pos = pos;
            body->prev_angle = angle;
            body->z = z;
            body->sleeping = false;
        }
    }

    void sync(ecs::World& w) {
        for (Body& b : bodies) b.seen = false;
        auto& registry = w.registry();
        const auto visit = [&](entt::entity handle) {
            const ecs::Entity e = w.wrap(handle);
            if (!e.activeInHierarchy() || !hasCollider(e)) return;
            addOrUpdate(e);
        };
        for (const entt::entity h : registry.view<BoxCollider2D>()) visit(h);
        for (const entt::entity h : registry.view<CircleCollider2D>()) visit(h);
        for (const entt::entity h : registry.view<PolygonCollider2D>()) visit(h);
        for (const entt::entity h : registry.view<TilemapCollider2D>()) visit(h);
        // Fuera los cuerpos sin entidad (y se rehace el indice).
        bool removed = false;
        for (std::size_t i = 0; i < bodies.size();) {
            if (!bodies[i].seen) {
                bodies[i] = std::move(bodies.back());
                bodies.pop_back();
                removed = true;
            } else {
                ++i;
            }
        }
        if (removed) {
            index.clear();
            for (std::size_t i = 0; i < bodies.size(); ++i) index[bodies[i].handle] = static_cast<int>(i);
        }
    }

    // --- Formas en el mundo -------------------------------------------------------

    static void updateWorldShape(const Body& body, Shape& s) {
        const float c = std::cos(body.angle);
        const float sn = std::sin(body.angle);
        if (s.circle) {
            s.wcenter = body.pos + rotate(c, sn, s.center);
            s.min_x = s.wcenter.x - s.radius;
            s.max_x = s.wcenter.x + s.radius;
            s.min_y = s.wcenter.y - s.radius;
            s.max_y = s.wcenter.y + s.radius;
            return;
        }
        s.wverts.resize(s.verts.size());
        s.wnormals.resize(s.normals.size());
        s.min_x = s.min_y = 1e30f;
        s.max_x = s.max_y = -1e30f;
        for (std::size_t i = 0; i < s.verts.size(); ++i) {
            s.wverts[i] = body.pos + rotate(c, sn, s.verts[i]);
            s.wnormals[i] = rotate(c, sn, s.normals[i]);
            s.min_x = std::min(s.min_x, s.wverts[i].x);
            s.max_x = std::max(s.max_x, s.wverts[i].x);
            s.min_y = std::min(s.min_y, s.wverts[i].y);
            s.max_y = std::max(s.max_y, s.wverts[i].y);
        }
    }

    // --- Colisiones ---------------------------------------------------------------

    struct Contact {
        V2 normal{};  // de A hacia B
        int count = 0;
        V2 points[2];
        float separation[2] = {0.0f, 0.0f};
    };

    static float maxSeparation(const Shape& a, const Shape& b, int& edge) {
        float best = -1e30f;
        edge = 0;
        for (std::size_t i = 0; i < a.wverts.size(); ++i) {
            const V2 n = a.wnormals[i];
            const V2 v = a.wverts[i];
            float lowest = 1e30f;
            for (const V2& p : b.wverts) lowest = std::min(lowest, dot(n, p - v));
            if (lowest > best) {
                best = lowest;
                edge = static_cast<int>(i);
            }
        }
        return best;
    }

    static int clipSegment(V2 out[2], const V2 in[2], V2 normal, float offset) {
        int count = 0;
        const float d0 = dot(normal, in[0]) - offset;
        const float d1 = dot(normal, in[1]) - offset;
        if (d0 <= 0.0f) out[count++] = in[0];
        if (d1 <= 0.0f) out[count++] = in[1];
        if (d0 * d1 < 0.0f && count < 2) {
            const float t = d0 / (d0 - d1);
            out[count++] = in[0] + (in[1] - in[0]) * t;
        }
        return count;
    }

    static bool polyPoly(const Shape& a, const Shape& b, Contact& c) {
        int edge_a = 0;
        const float sep_a = maxSeparation(a, b, edge_a);
        if (sep_a > 0.0f) return false;
        int edge_b = 0;
        const float sep_b = maxSeparation(b, a, edge_b);
        if (sep_b > 0.0f) return false;
        const Shape* ref = &a;
        const Shape* inc = &b;
        int edge = edge_a;
        bool flip = false;
        if (sep_b > sep_a + 0.0005f) {
            ref = &b;
            inc = &a;
            edge = edge_b;
            flip = true;
        }
        const std::size_t n = ref->wverts.size();
        const V2 v1 = ref->wverts[static_cast<std::size_t>(edge)];
        const V2 v2 = ref->wverts[(static_cast<std::size_t>(edge) + 1) % n];
        const V2 ref_normal = ref->wnormals[static_cast<std::size_t>(edge)];
        // Arista incidente: la mas opuesta a la normal de referencia.
        std::size_t inc_edge = 0;
        float lowest = 1e30f;
        for (std::size_t i = 0; i < inc->wnormals.size(); ++i) {
            const float d = dot(ref_normal, inc->wnormals[i]);
            if (d < lowest) {
                lowest = d;
                inc_edge = i;
            }
        }
        const V2 incident[2] = {inc->wverts[inc_edge], inc->wverts[(inc_edge + 1) % inc->wverts.size()]};
        const V2 tangent = norm(v2 - v1);
        V2 clip1[2];
        if (clipSegment(clip1, incident, -tangent, -dot(tangent, v1)) < 2) return false;
        V2 clip2[2];
        if (clipSegment(clip2, clip1, tangent, dot(tangent, v2)) < 2) return false;
        const float front = dot(ref_normal, v1);
        c.count = 0;
        for (const V2& p : clip2) {
            const float sep = dot(ref_normal, p) - front;
            if (sep <= 0.0f) {
                c.points[c.count] = p - ref_normal * (sep * 0.5f);
                c.separation[c.count] = sep;
                ++c.count;
            }
        }
        c.normal = flip ? -ref_normal : ref_normal;
        return c.count > 0;
    }

    // Poligono A contra circulo B (normal de A hacia B).
    static bool polyCircle(const Shape& a, const Shape& b, Contact& c) {
        const V2 center = b.wcenter;
        const float r = b.radius;
        float best = -1e30f;
        std::size_t face = 0;
        for (std::size_t i = 0; i < a.wverts.size(); ++i) {
            const float s = dot(a.wnormals[i], center - a.wverts[i]);
            if (s > r) return false;
            if (s > best) {
                best = s;
                face = i;
            }
        }
        const V2 v1 = a.wverts[face];
        const V2 v2 = a.wverts[(face + 1) % a.wverts.size()];
        c.count = 1;
        if (best < 1e-6f) {  // centro dentro
            c.normal = a.wnormals[face];
            c.points[0] = center - c.normal * r;
            c.separation[0] = best - r;
            return true;
        }
        const float u1 = dot(center - v1, v2 - v1);
        const float u2 = dot(center - v2, v1 - v2);
        if (u1 <= 0.0f || u2 <= 0.0f) {
            const V2 v = u1 <= 0.0f ? v1 : v2;
            const V2 d = center - v;
            const float dist = len(d);
            if (dist > r) return false;
            c.normal = norm(d);
            c.points[0] = v;
            c.separation[0] = dist - r;
            return true;
        }
        c.normal = a.wnormals[face];
        c.points[0] = center - c.normal * r;
        c.separation[0] = best - r;
        return true;
    }

    static bool circleCircle(const Shape& a, const Shape& b, Contact& c) {
        const V2 d = b.wcenter - a.wcenter;
        const float dist = len(d);
        if (dist > a.radius + b.radius) return false;
        c.normal = dist > 1e-6f ? d * (1.0f / dist) : V2{0.0f, 1.0f};
        c.count = 1;
        c.points[0] = a.wcenter + c.normal * a.radius;
        c.separation[0] = dist - a.radius - b.radius;
        return true;
    }

    static bool collide(const Shape& a, const Shape& b, Contact& c) {
        if (!a.circle && !b.circle) return polyPoly(a, b, c);
        if (!a.circle && b.circle) return polyCircle(a, b, c);
        if (a.circle && b.circle) return circleCircle(a, b, c);
        if (polyCircle(b, a, c)) {
            c.normal = -c.normal;
            return true;
        }
        return false;
    }

    // --- Paso ---------------------------------------------------------------------

    void findContacts(float dt) {
        manifolds.clear();
        struct Item {
            int body;
            int shape;
            float min_x;
        };
        std::vector<Item> items;
        for (std::size_t bi = 0; bi < bodies.size(); ++bi) {
            Body& body = bodies[bi];
            for (std::size_t si = 0; si < body.shapes.size(); ++si) {
                updateWorldShape(body, body.shapes[si]);
                items.push_back(Item{static_cast<int>(bi), static_cast<int>(si), body.shapes[si].min_x});
            }
        }
        std::sort(items.begin(), items.end(), [](const Item& x, const Item& y) { return x.min_x < y.min_x; });
        for (std::size_t i = 0; i < items.size(); ++i) {
            const Shape& sa = bodies[static_cast<std::size_t>(items[i].body)].shapes[static_cast<std::size_t>(items[i].shape)];
            for (std::size_t j = i + 1; j < items.size(); ++j) {
                if (items[j].min_x > sa.max_x) break;
                if (items[i].body == items[j].body) continue;
                int ia = items[i].body;
                int ib = items[j].body;
                int ja = items[i].shape;
                int jb = items[j].shape;
                Body* A = &bodies[static_cast<std::size_t>(ia)];
                Body* B = &bodies[static_cast<std::size_t>(ib)];
                const Shape* SA = &A->shapes[static_cast<std::size_t>(ja)];
                const Shape* SB = &B->shapes[static_cast<std::size_t>(jb)];
                if (SA->max_y < SB->min_y || SB->max_y < SA->min_y) continue;
                const bool trigger = SA->material.is_trigger || SB->material.is_trigger;
                // Al menos uno tiene que moverse (Unity: un Rigidbody2D).
                const bool a_moves = A->type != BodyType2D::Static;
                const bool b_moves = B->type != BodyType2D::Static;
                if (!a_moves && !b_moves) continue;
                if (!trigger && !A->dynamic() && !B->dynamic()) continue;
                Contact c;
                if (!collide(*SA, *SB, c)) continue;
                // Plataformas de un sentido: solo desde su +Y y sin subir.
                const auto oneWayBlocks = [&](const Body& platform, const Shape& ps, const Body& other, V2 normal_from_platform) {
                    if (!ps.material.one_way) return true;
                    const V2 up = rotate(std::cos(platform.angle), std::sin(platform.angle), V2{0.0f, 1.0f});
                    if (dot(normal_from_platform, up) < 0.7f) return false;
                    if (dot(other.vel - platform.vel, up) > 0.5f) return false;
                    // Los que ya estaban mas abajo que la superficie la atraviesan.
                    const float depth = c.count > 0 ? -c.separation[0] : 0.0f;
                    return depth < 0.25f;
                };
                if (!oneWayBlocks(*A, *SA, *B, c.normal) || !oneWayBlocks(*B, *SB, *A, -c.normal)) continue;
                if (A->sleeping && B->dynamic() && !B->sleeping) A->sleeping = false;
                if (B->sleeping && A->dynamic() && !A->sleeping) B->sleeping = false;

                Manifold m;
                m.a = ia;
                m.b = ib;
                m.sa = ja;
                m.sb = jb;
                m.normal = c.normal;
                m.count = c.count;
                m.trigger = trigger;
                m.friction = std::sqrt(std::max(SA->material.friction, 0.0f) * std::max(SB->material.friction, 0.0f));
                m.restitution = std::max(SA->material.bounciness, SB->material.bounciness);
                for (int k = 0; k < c.count; ++k) {
                    m.points[k].p = c.points[k];
                    m.points[k].separation = c.separation[k];
                }
                // Evento (una vez por par de entidades y paso).
                PairInfo info;
                info.a = A->handle;
                info.b = B->handle;
                info.trigger = trigger;
                info.point = core::Vec3{c.points[0].x, c.points[0].y, A->z};
                info.normal = core::Vec3{c.normal.x, c.normal.y, 0.0f};
                const V2 rv = B->vel - A->vel;
                info.relative_velocity = core::Vec3{rv.x, rv.y, 0.0f};
                const std::uint64_t key = pairKey(A->handle, B->handle);
                auto existing = step_pairs.find(key);
                if (existing == step_pairs.end()) step_pairs.emplace(key, info);
                else if (!trigger) existing->second.trigger = false;
                if (!trigger) manifolds.push_back(m);
            }
        }
        (void)dt;
    }

    void stepOnce(float dt, core::Vec2 gravity, int iterations) {
        // Fuerzas
        for (Body& b : bodies) {
            b.prev_pos = b.dynamic() ? b.pos : b.prev_pos;
            b.prev_angle = b.dynamic() ? b.angle : b.prev_angle;
            if (!b.dynamic() || b.sleeping) continue;
            b.vel += (V2{gravity} * b.gravity_scale + b.force * b.inv_mass) * dt;
            b.w += b.torque * b.inv_inertia * dt;
            b.vel = b.vel * (1.0f / (1.0f + dt * std::max(b.linear_drag, 0.0f)));
            b.w *= 1.0f / (1.0f + dt * std::max(b.angular_drag, 0.0f));
            if (b.freeze_x) b.vel.x = 0.0f;
            if (b.freeze_y) b.vel.y = 0.0f;
            if (b.freeze_rotation) b.w = 0.0f;
        }
        findContacts(dt);
        // Preparacion de los contactos
        for (Manifold& m : manifolds) {
            Body& A = bodies[static_cast<std::size_t>(m.a)];
            Body& B = bodies[static_cast<std::size_t>(m.b)];
            const float ima = A.dynamic() && !A.sleeping ? A.inv_mass : 0.0f;
            const float imb = B.dynamic() && !B.sleeping ? B.inv_mass : 0.0f;
            const float iia = A.dynamic() && !A.sleeping ? A.inv_inertia : 0.0f;
            const float iib = B.dynamic() && !B.sleeping ? B.inv_inertia : 0.0f;
            const V2 t{m.normal.y, -m.normal.x};
            for (int k = 0; k < m.count; ++k) {
                Point& p = m.points[k];
                p.ra = p.p - A.pos;
                p.rb = p.p - B.pos;
                const float rna = cross(p.ra, m.normal);
                const float rnb = cross(p.rb, m.normal);
                const float kn = ima + imb + iia * rna * rna + iib * rnb * rnb;
                p.mass_n = kn > 0.0f ? 1.0f / kn : 0.0f;
                const float rta = cross(p.ra, t);
                const float rtb = cross(p.rb, t);
                const float kt = ima + imb + iia * rta * rta + iib * rtb * rtb;
                p.mass_t = kt > 0.0f ? 1.0f / kt : 0.0f;
                p.bias = -kBaumgarte / dt * std::min(0.0f, p.separation + kSlop);
                const V2 dv = B.vel + cross(B.w, p.rb) - A.vel - cross(A.w, p.ra);
                const float vn = dot(dv, m.normal);
                if (vn < -1.0f) p.bias = std::max(p.bias, -m.restitution * vn);
                p.pn = 0.0f;
                p.pt = 0.0f;
            }
        }
        // Impulsos
        for (int it = 0; it < iterations; ++it) {
            for (Manifold& m : manifolds) {
                Body& A = bodies[static_cast<std::size_t>(m.a)];
                Body& B = bodies[static_cast<std::size_t>(m.b)];
                const float ima = A.dynamic() && !A.sleeping ? A.inv_mass : 0.0f;
                const float imb = B.dynamic() && !B.sleeping ? B.inv_mass : 0.0f;
                const float iia = A.dynamic() && !A.sleeping ? A.inv_inertia : 0.0f;
                const float iib = B.dynamic() && !B.sleeping ? B.inv_inertia : 0.0f;
                const V2 t{m.normal.y, -m.normal.x};
                for (int k = 0; k < m.count; ++k) {
                    Point& p = m.points[k];
                    V2 dv = B.vel + cross(B.w, p.rb) - A.vel - cross(A.w, p.ra);
                    const float vn = dot(dv, m.normal);
                    float dpn = p.mass_n * (-vn + p.bias);
                    const float pn0 = p.pn;
                    p.pn = std::max(pn0 + dpn, 0.0f);
                    dpn = p.pn - pn0;
                    V2 impulse = m.normal * dpn;
                    A.vel -= impulse * ima;
                    A.w -= iia * cross(p.ra, impulse);
                    B.vel += impulse * imb;
                    B.w += iib * cross(p.rb, impulse);

                    dv = B.vel + cross(B.w, p.rb) - A.vel - cross(A.w, p.ra);
                    const float vt = dot(dv, t);
                    float dpt = p.mass_t * (-vt);
                    const float max_pt = m.friction * p.pn;
                    const float pt0 = p.pt;
                    p.pt = std::clamp(pt0 + dpt, -max_pt, max_pt);
                    dpt = p.pt - pt0;
                    impulse = t * dpt;
                    A.vel -= impulse * ima;
                    A.w -= iia * cross(p.ra, impulse);
                    B.vel += impulse * imb;
                    B.w += iib * cross(p.rb, impulse);
                }
            }
        }
        // Integracion y sueno
        for (Body& b : bodies) {
            if (!b.dynamic() || b.sleeping) continue;
            if (b.freeze_x) b.vel.x = 0.0f;
            if (b.freeze_y) b.vel.y = 0.0f;
            if (b.freeze_rotation) b.w = 0.0f;
            b.pos += b.vel * dt;
            b.angle += b.w * dt;
            b.force = V2{};
            b.torque = 0.0f;
            if (b.allow_sleep && len(b.vel) < kSleepSpeed && std::fabs(b.w) < kSleepSpeed) {
                b.still += dt;
                if (b.still > kSleepTime) {
                    b.sleeping = true;
                    b.vel = V2{};
                    b.w = 0.0f;
                }
            } else {
                b.still = 0.0f;
            }
        }
        ++steps;
    }

    void writeBack(ecs::World& w, float alpha) {
        for (Body& b : bodies) {
            if (!b.dynamic()) continue;
            if (!w.registry().valid(b.handle)) continue;
            ecs::Entity e = w.wrap(b.handle);
            const float t = b.interpolate ? std::clamp(alpha, 0.0f, 1.0f) : 1.0f;
            const V2 pos = b.prev_pos + (b.pos - b.prev_pos) * t;
            const float angle = b.prev_angle + (b.angle - b.prev_angle) * t;
            // Dormido o quieto: lo mismo que ya escribio (sync() detecta si
            // otro lo movio). Reescribirlo marcaba la jerarquia como sucia y
            // le cambiaba la version al render en cada frame.
            if (b.has_written && pos.x == b.written_pos.x && pos.y == b.written_pos.y && angle == b.written_angle) {
                continue;
            }
            e.setWorldMatrix(core::composeTrs(core::Vec3{pos.x, pos.y, b.z}, quatZ(angle), b.scale));
            b.written_pos = pos;
            b.written_angle = angle;
            b.has_written = true;
        }
    }

    void emitEvents(ecs::World& w) {
        // Enter/Stay de los pares de este update, Exit de los que ya no estan.
        for (const auto& [key, info] : step_pairs) {
            const ecs::Entity a = w.wrap(info.a);
            const ecs::Entity b = w.wrap(info.b);
            const bool existed = pairs.contains(key);
            Physics2DEvent ev;
            if (info.trigger) ev.type = existed ? Physics2DEventType::TriggerStay : Physics2DEventType::TriggerEnter;
            else ev.type = existed ? Physics2DEventType::CollisionStay : Physics2DEventType::CollisionEnter;
            ev.a = a;
            ev.b = b;
            ev.point = info.point;
            ev.normal = info.normal;
            ev.relative_velocity = info.relative_velocity;
            events.push_back(ev);
        }
        for (const auto& [key, info] : pairs) {
            if (step_pairs.contains(key)) continue;
            Physics2DEvent ev;
            ev.type = info.trigger ? Physics2DEventType::TriggerExit : Physics2DEventType::CollisionExit;
            ev.a = w.registry().valid(info.a) ? w.wrap(info.a) : ecs::Entity{};
            ev.b = w.registry().valid(info.b) ? w.wrap(info.b) : ecs::Entity{};
            ev.point = info.point;
            ev.normal = info.normal;
            events.push_back(ev);
        }
        pairs = std::move(step_pairs);
        step_pairs.clear();
    }

    // --- Consultas ----------------------------------------------------------------

    bool layerOk(const Body& b, std::uint32_t mask) const {
        const int layer = std::clamp(b.layer, 0, 31);
        return (mask & (1u << layer)) != 0;
    }

    static bool rayShape(const Shape& s, V2 origin, V2 dir, float max_t, float& t_hit, V2& normal) {
        if (s.circle) {
            const V2 m = origin - s.wcenter;
            const float b = dot(m, dir);
            const float c = dot(m, m) - s.radius * s.radius;
            if (c > 0.0f && b > 0.0f) return false;
            const float disc = b * b - c;
            if (disc < 0.0f) return false;
            float t = -b - std::sqrt(disc);
            if (t < 0.0f) t = 0.0f;
            if (t > max_t) return false;
            t_hit = t;
            normal = norm(origin + dir * t - s.wcenter);
            return true;
        }
        float lower = 0.0f;
        float upper = max_t;
        int index_hit = -1;
        for (std::size_t i = 0; i < s.wverts.size(); ++i) {
            const float numerator = dot(s.wnormals[i], s.wverts[i] - origin);
            const float denominator = dot(s.wnormals[i], dir);
            if (std::fabs(denominator) < 1e-9f) {
                if (numerator < 0.0f) return false;
            } else if (denominator < 0.0f && numerator < lower * denominator) {
                lower = numerator / denominator;
                index_hit = static_cast<int>(i);
            } else if (denominator > 0.0f && numerator < upper * denominator) {
                upper = numerator / denominator;
            }
            if (upper < lower) return false;
        }
        t_hit = lower;
        normal = index_hit >= 0 ? s.wnormals[static_cast<std::size_t>(index_hit)] : -dir;
        return true;
    }

    void refreshWorldShapes() {
        for (Body& b : bodies) {
            for (Shape& s : b.shapes) updateWorldShape(b, s);
        }
    }

    std::vector<ecs::Entity> overlap(const Shape& probe, std::uint32_t mask, bool hit_triggers) {
        std::vector<ecs::Entity> out;
        if (world == nullptr) return out;
        refreshWorldShapes();
        for (const Body& b : bodies) {
            if (!layerOk(b, mask)) continue;
            for (const Shape& s : b.shapes) {
                if (s.material.is_trigger && !hit_triggers) continue;
                if (probe.max_x < s.min_x || s.max_x < probe.min_x || probe.max_y < s.min_y || s.max_y < probe.min_y) continue;
                Contact c;
                if (collide(probe, s, c)) {
                    out.push_back(world->wrap(b.handle));
                    break;
                }
            }
        }
        return out;
    }
};

// --- Interfaz ---------------------------------------------------------------------------

Physics2DWorld::Physics2DWorld() : impl_(std::make_unique<Impl>()) { impl_->owner = this; }
Physics2DWorld::~Physics2DWorld() = default;

void Physics2DWorld::setTilesetLibrary(TilesetLibrary* tilesets) { impl_->tilesets = tilesets; }

void Physics2DWorld::start(ecs::World& world) {
    stop();
    impl_->world = &world;
    impl_->running = true;
    impl_->sync(world);
}

void Physics2DWorld::stop() {
    Impl& d = *impl_;
    d.running = false;
    d.world = nullptr;
    d.bodies.clear();
    d.index.clear();
    d.manifolds.clear();
    d.pairs.clear();
    d.step_pairs.clear();
    d.events.clear();
    d.accumulator = 0.0f;
}

bool Physics2DWorld::running() const { return impl_->running; }

int Physics2DWorld::update(ecs::World& world, float dt) {
    Impl& d = *impl_;
    if (!d.running) return 0;
    d.world = &world;
    d.sync(world);
    const float step = std::max(fixed_step, 1.0f / 1000.0f);
    d.accumulator += std::max(dt, 0.0f);
    int steps = 0;
    while (d.accumulator >= step && steps < std::max(max_substeps, 1)) {
        d.stepOnce(step, gravity, std::max(velocity_iterations, 1));
        d.accumulator -= step;
        ++steps;
    }
    if (steps >= std::max(max_substeps, 1)) d.accumulator = std::min(d.accumulator, step);
    if (steps > 0) d.emitEvents(world);
    d.writeBack(world, d.accumulator / step);
    return steps;
}

void Physics2DWorld::step(ecs::World& world) {
    Impl& d = *impl_;
    if (!d.running) return;
    d.world = &world;
    d.sync(world);
    d.stepOnce(std::max(fixed_step, 1.0f / 1000.0f), gravity, std::max(velocity_iterations, 1));
    d.emitEvents(world);
    d.writeBack(world, 1.0f);
}

std::vector<Physics2DEvent> Physics2DWorld::takeEvents() {
    std::vector<Physics2DEvent> out;
    out.swap(impl_->events);
    return out;
}

namespace {
template <typename Fn>
auto withBody(Physics2DWorld::Impl& d, const ecs::Entity& e, Fn&& fn) {
    const auto it = d.index.find(e.handle());
    using Body = Physics2DWorld::Impl::Body;
    Body* body = it != d.index.end() ? &d.bodies[static_cast<std::size_t>(it->second)] : nullptr;
    return fn(body);
}
}  // namespace

bool Physics2DWorld::hasBody(ecs::Entity e) const { return e.valid() && impl_->index.contains(e.handle()); }

core::Vec2 Physics2DWorld::velocity(ecs::Entity e) const {
    return withBody(*impl_, e, [](Impl::Body* b) { return b != nullptr ? b->vel.vec() : core::Vec2{}; });
}

void Physics2DWorld::setVelocity(ecs::Entity e, core::Vec2 v) {
    withBody(*impl_, e, [&](Impl::Body* b) {
        if (b == nullptr || !b->dynamic()) return 0;
        b->vel = v;
        b->sleeping = false;
        b->still = 0.0f;
        return 0;
    });
}

float Physics2DWorld::angularVelocity(ecs::Entity e) const {
    return withBody(*impl_, e, [](Impl::Body* b) { return b != nullptr ? b->w : 0.0f; });
}

void Physics2DWorld::setAngularVelocity(ecs::Entity e, float w) {
    withBody(*impl_, e, [&](Impl::Body* b) {
        if (b == nullptr || !b->dynamic()) return 0;
        b->w = w;
        b->sleeping = false;
        return 0;
    });
}

void Physics2DWorld::addForce(ecs::Entity e, core::Vec2 force, ForceMode2D mode) {
    withBody(*impl_, e, [&](Impl::Body* b) {
        if (b == nullptr || !b->dynamic()) return 0;
        b->sleeping = false;
        b->still = 0.0f;
        const V2 f{force};
        if (mode == ForceMode2D::Force) b->force += f;
        else if (mode == ForceMode2D::Impulse) b->vel += f * b->inv_mass;
        else b->vel += f;
        return 0;
    });
}

void Physics2DWorld::addForceAtPosition(ecs::Entity e, core::Vec2 force, core::Vec2 point, ForceMode2D mode) {
    withBody(*impl_, e, [&](Impl::Body* b) {
        if (b == nullptr || !b->dynamic()) return 0;
        b->sleeping = false;
        b->still = 0.0f;
        const V2 f{force};
        const float torque = cross(V2{point} - b->pos, f);
        if (mode == ForceMode2D::Force) {
            b->force += f;
            b->torque += torque;
        } else if (mode == ForceMode2D::Impulse) {
            b->vel += f * b->inv_mass;
            b->w += torque * b->inv_inertia;
        } else {
            b->vel += f;
            b->w += torque * (b->inv_inertia > 0.0f && b->inv_mass > 0.0f ? b->inv_inertia / b->inv_mass : 0.0f);
        }
        return 0;
    });
}

void Physics2DWorld::addTorque(ecs::Entity e, float torque, ForceMode2D mode) {
    withBody(*impl_, e, [&](Impl::Body* b) {
        if (b == nullptr || !b->dynamic()) return 0;
        b->sleeping = false;
        b->still = 0.0f;
        if (mode == ForceMode2D::Force) b->torque += torque;
        else if (mode == ForceMode2D::Impulse) b->w += torque * b->inv_inertia;
        else b->w += torque;
        return 0;
    });
}

void Physics2DWorld::setPosition(ecs::Entity e, core::Vec2 position) {
    withBody(*impl_, e, [&](Impl::Body* b) {
        if (b == nullptr) return 0;
        b->pos = position;
        b->prev_pos = position;
        b->sleeping = false;
        if (impl_->world != nullptr && impl_->world->registry().valid(b->handle)) {
            ecs::Entity entity = impl_->world->wrap(b->handle);
            entity.setWorldMatrix(core::composeTrs(core::Vec3{position.x, position.y, b->z}, quatZ(b->angle), b->scale));
            b->written_pos = position;
            b->written_angle = b->angle;
            b->has_written = true;
        }
        return 0;
    });
}

bool Physics2DWorld::isSleeping(ecs::Entity e) const {
    return withBody(*impl_, e, [](Impl::Body* b) { return b != nullptr && b->sleeping; });
}

void Physics2DWorld::wakeUp(ecs::Entity e) {
    withBody(*impl_, e, [](Impl::Body* b) {
        if (b != nullptr) {
            b->sleeping = false;
            b->still = 0.0f;
        }
        return 0;
    });
}

void Physics2DWorld::refresh(ecs::Entity e) {
    withBody(*impl_, e, [](Impl::Body* b) {
        if (b != nullptr) b->signature ^= 0x9E3779B97F4A7C15ull;  // se rehace en el proximo update
        return 0;
    });
}

bool Physics2DWorld::raycast(core::Vec2 origin, core::Vec2 direction, float distance, RaycastHit2D& hit,
                             std::uint32_t mask, bool hit_triggers) const {
    const std::vector<RaycastHit2D> all = raycastAll(origin, direction, distance, mask, hit_triggers);
    if (all.empty()) return false;
    hit = all.front();
    return true;
}

std::vector<RaycastHit2D> Physics2DWorld::raycastAll(core::Vec2 origin, core::Vec2 direction, float distance,
                                                     std::uint32_t mask, bool hit_triggers) const {
    std::vector<RaycastHit2D> out;
    Impl& d = *impl_;
    if (d.world == nullptr) return out;
    const V2 o{origin};
    const V2 dir = norm(V2{direction});
    if (len(V2{direction}) < 1e-9f) return out;
    const float max_t = std::max(distance, 0.0f);
    d.refreshWorldShapes();
    for (const Impl::Body& b : d.bodies) {
        if (!d.layerOk(b, mask)) continue;
        float best = 1e30f;
        V2 best_normal{};
        for (const Impl::Shape& s : b.shapes) {
            if (s.material.is_trigger && !hit_triggers) continue;
            float t = 0.0f;
            V2 n{};
            if (Impl::rayShape(s, o, dir, max_t, t, n) && t < best) {
                best = t;
                best_normal = n;
            }
        }
        if (best <= max_t) {
            RaycastHit2D h;
            h.entity = d.world->wrap(b.handle);
            const V2 p = o + dir * best;
            h.point = p.vec();
            h.normal = best_normal.vec();
            h.distance = best;
            h.fraction = max_t > 0.0f ? best / max_t : 0.0f;
            out.push_back(h);
        }
    }
    std::sort(out.begin(), out.end(), [](const RaycastHit2D& a, const RaycastHit2D& b) { return a.distance < b.distance; });
    return out;
}

std::vector<ecs::Entity> Physics2DWorld::overlapCircle(core::Vec2 center, float radius, std::uint32_t mask,
                                                       bool hit_triggers) const {
    Impl::Shape probe;
    probe.circle = true;
    probe.wcenter = V2{center};
    probe.radius = std::max(radius, 0.0f);
    probe.min_x = center.x - radius;
    probe.max_x = center.x + radius;
    probe.min_y = center.y - radius;
    probe.max_y = center.y + radius;
    return impl_->overlap(probe, mask, hit_triggers);
}

std::vector<ecs::Entity> Physics2DWorld::overlapBox(core::Vec2 center, core::Vec2 size, float angle_degrees,
                                                    std::uint32_t mask, bool hit_triggers) const {
    Impl::Shape probe;
    const V2 h{std::fabs(size.x) * 0.5f, std::fabs(size.y) * 0.5f};
    probe.verts = {V2{-h.x, -h.y}, V2{h.x, -h.y}, V2{h.x, h.y}, V2{-h.x, h.y}};
    Impl::finishPolygon(probe);
    Impl::Body body;
    body.pos = V2{center};
    body.angle = angle_degrees * 3.14159265f / 180.0f;
    Impl::updateWorldShape(body, probe);
    return impl_->overlap(probe, mask, hit_triggers);
}

std::vector<ecs::Entity> Physics2DWorld::overlapPoint(core::Vec2 point, std::uint32_t mask, bool hit_triggers) const {
    return overlapCircle(point, 1e-4f, mask, hit_triggers);
}

Physics2DWorld::Stats Physics2DWorld::stats() const {
    Stats s;
    s.bodies = static_cast<int>(impl_->bodies.size());
    for (const Impl::Body& b : impl_->bodies) s.shapes += static_cast<int>(b.shapes.size());
    s.contacts = static_cast<int>(impl_->manifolds.size());
    s.steps = impl_->steps;
    return s;
}

std::vector<Physics2DWorld::DebugShape> Physics2DWorld::debugShapes() const {
    std::vector<DebugShape> out;
    impl_->refreshWorldShapes();
    for (const Impl::Body& b : impl_->bodies) {
        for (const Impl::Shape& s : b.shapes) {
            DebugShape d;
            d.trigger = s.material.is_trigger;
            d.sleeping = b.sleeping;
            d.dynamic = b.dynamic();
            if (s.circle) {
                for (int i = 0; i < 20; ++i) {
                    const float a = static_cast<float>(i) / 20.0f * 6.2831853f;
                    d.points.push_back(core::Vec2{s.wcenter.x + std::cos(a) * s.radius, s.wcenter.y + std::sin(a) * s.radius});
                }
            } else {
                for (const V2& v : s.wverts) d.points.push_back(v.vec());
            }
            out.push_back(std::move(d));
        }
    }
    return out;
}

// --- Contornos para el editor --------------------------------------------------------------

std::vector<ColliderOutline> colliderOutlines(ecs::Entity e) {
    std::vector<ColliderOutline> out;
    if (!e.valid()) return out;
    const core::Mat4& m = e.worldMatrix();
    const auto toWorld = [&](float x, float y) {
        const core::Vec3 p = ecs::transformPoint(m, core::Vec3{x, y, 0.0f});
        return core::Vec2{p.x, p.y};
    };
    if (const auto* b = e.tryGet<BoxCollider2D>()) {
        ColliderOutline o;
        o.trigger = b->material.is_trigger;
        const float hx = b->size.x * 0.5f;
        const float hy = b->size.y * 0.5f;
        o.points = {toWorld(b->offset.x - hx, b->offset.y - hy), toWorld(b->offset.x + hx, b->offset.y - hy),
                    toWorld(b->offset.x + hx, b->offset.y + hy), toWorld(b->offset.x - hx, b->offset.y + hy)};
        out.push_back(std::move(o));
    }
    if (const auto* c = e.tryGet<CircleCollider2D>()) {
        ColliderOutline o;
        o.trigger = c->material.is_trigger;
        for (int i = 0; i < 24; ++i) {
            const float a = static_cast<float>(i) / 24.0f * 6.2831853f;
            o.points.push_back(toWorld(c->offset.x + std::cos(a) * c->radius, c->offset.y + std::sin(a) * c->radius));
        }
        out.push_back(std::move(o));
    }
    if (const auto* p = e.tryGet<PolygonCollider2D>()) {
        ColliderOutline o;
        o.trigger = p->material.is_trigger;
        std::vector<V2> pts;
        for (const core::Vec2& v : p->points) pts.push_back(V2{v.x + p->offset.x, v.y + p->offset.y});
        for (const V2& v : convexHull(pts)) o.points.push_back(toWorld(v.x, v.y));
        out.push_back(std::move(o));
    }
    if (const auto* t = e.tryGet<TilemapCollider2D>()) {
        if (const auto* map = e.tryGet<Tilemap>()) {
            for (const TileRect& r : solidRects(*map, nullptr, !t->one_way)) {
                ColliderOutline o;
                o.trigger = t->is_trigger;
                const float x0 = static_cast<float>(r.x) * map->cell_size.x;
                const float y0 = static_cast<float>(r.y) * map->cell_size.y;
                const float x1 = static_cast<float>(r.x + r.w) * map->cell_size.x;
                const float y1 = static_cast<float>(r.y + r.h) * map->cell_size.y;
                o.points = {toWorld(x0, y0), toWorld(x1, y0), toWorld(x1, y1), toWorld(x0, y1)};
                out.push_back(std::move(o));
            }
        }
    }
    return out;
}

// --- Reflexion ----------------------------------------------------------------------------

namespace {
void reflectMaterial(ecs::PropertyVisitor& v, Collider2DMaterial& m) {
    v.field({"is_trigger", "Es trigger", "Detecta (OnTriggerEnter2D) pero no choca"}, m.is_trigger);
    v.field({"friction", "Fricción"}, m.friction, ecs::FloatRange{0.0f, 2.0f, 0.01f, "%.2f"});
    v.field({"bounciness", "Rebote"}, m.bounciness, ecs::FloatRange{0.0f, 1.0f, 0.01f, "%.2f", true});
    v.field({"one_way", "Un sentido", "Plataforma que se atraviesa saltando desde abajo"}, m.one_way);
}
}  // namespace

void Rigidbody2D::reflect(ecs::PropertyVisitor& v) {
    static constexpr const char* kTypes[] = {"Dinámico", "Cinemático", "Estático"};
    ecs::enumField(v, {"type", "Tipo", "Dinamico: lo mueve la fisica. Cinematico: su Transform o un script"}, type, kTypes);
    v.field({"mass", "Masa"}, mass, ecs::FloatRange{0.001f, 100000.0f, 0.05f, "%.3f kg"});
    v.field({"linear_drag", "Rozamiento lineal"}, linear_drag, ecs::FloatRange{0.0f, 100.0f, 0.01f, "%.2f"});
    v.field({"angular_drag", "Rozamiento angular"}, angular_drag, ecs::FloatRange{0.0f, 100.0f, 0.01f, "%.2f"});
    v.field({"gravity_scale", "Escala de gravedad"}, gravity_scale, ecs::FloatRange{-10.0f, 10.0f, 0.01f, "%.2f"});
    v.field({"freeze_rotation", "Congelar giro"}, freeze_rotation);
    v.field({"freeze_x", "Congelar X"}, freeze_x);
    v.field({"freeze_y", "Congelar Y"}, freeze_y);
    v.field({"initial_velocity", "Velocidad inicial"}, initial_velocity);
    v.field({"interpolate", "Interpolar", "Movimiento suave entre pasos de fisica"}, interpolate);
    v.field({"allow_sleep", "Puede dormir"}, allow_sleep);
}

void BoxCollider2D::reflect(ecs::PropertyVisitor& v) {
    v.field({"size", "Tamaño"}, size);
    v.field({"offset", "Desplazamiento"}, offset);
    reflectMaterial(v, material);
    v.field({"used_by_composite", "Usado por el Composite"}, used_by_composite);
}

void CircleCollider2D::reflect(ecs::PropertyVisitor& v) {
    v.field({"radius", "Radio"}, radius, ecs::FloatRange{0.001f, 1000.0f, 0.01f, "%.3f"});
    v.field({"offset", "Desplazamiento"}, offset);
    reflectMaterial(v, material);
}

void PolygonCollider2D::reflect(ecs::PropertyVisitor& v) {
    ecs::listField(v, {"points", "Puntos", "Convexo (si no, su envolvente convexa); hasta 16"}, points,
                   [](core::Vec2& p, ecs::PropertyVisitor& pv) { pv.field({"p", "Punto"}, p); });
    v.field({"offset", "Desplazamiento"}, offset);
    reflectMaterial(v, material);
}

void CompositeCollider2D::reflect(ecs::PropertyVisitor& v) { reflectMaterial(v, material); }

}  // namespace cramion::twod
