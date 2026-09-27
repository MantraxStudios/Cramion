#include "CramionCore/anim/PhysBones.h"

#include "CramionCore/ecs/MathUtil.h"

#include <algorithm>
#include <cmath>

namespace cramion::physbone {

using core::Mat4;
using core::Quat;
using core::Vec3;

namespace {

constexpr Vec3 kDown{0.0f, -1.0f, 0.0f};
constexpr float kGravity = 9.81f;

Vec3 closestOnSegment(const Vec3& a, const Vec3& b, const Vec3& p) {
    const Vec3 ab = b - a;
    const float len2 = core::dot(ab, ab);
    const float t = len2 > 1e-12f ? std::clamp(core::dot(p - a, ab) / len2, 0.0f, 1.0f) : 0.0f;
    return a + ab * t;
}

// Saca (o mete) el punto `p` de radio `r` del collider.
Vec3 collide(const Collider& c, Vec3 p, float r) {
    switch (c.shape) {
        case Collider::Shape::Plane: {
            const Vec3 n = core::normalize(c.normal);
            const float d = core::dot(p - c.a, n);
            if (d < r) p = p + n * (r - d);
            return p;
        }
        case Collider::Shape::Sphere:
        case Collider::Shape::Capsule: {
            const Vec3 center = c.shape == Collider::Shape::Sphere ? c.a : closestOnSegment(c.a, c.b, p);
            const Vec3 d = p - center;
            const float len = core::length(d);
            if (c.inside) {
                const float limit = std::max(c.radius - r, 0.0f);
                if (len > limit && len > 1e-6f) p = center + d * (limit / len);
            } else {
                const float reach = c.radius + r;
                if (len < reach) p = len > 1e-6f ? center + d * (reach / len) : center + Vec3{0.0f, reach, 0.0f};
            }
            return p;
        }
    }
    return p;
}

}  // namespace

Chain makeChain(const std::vector<asset::Node>& nodes, int root, const std::vector<int>& ignore, bool tips,
                int max_joints) {
    Chain chain;
    if (root < 0 || static_cast<std::size_t>(root) >= nodes.size()) return chain;
    std::vector<int> index(nodes.size(), -1);  // nodo -> particula
    std::vector<bool> has_child;
    chain.nodes.push_back(root);
    chain.parent.push_back(-1);
    chain.leads.push_back(false);
    has_child.push_back(false);
    index[static_cast<std::size_t>(root)] = 0;
    for (std::size_t n = static_cast<std::size_t>(root) + 1; n < nodes.size(); ++n) {
        if (static_cast<int>(chain.nodes.size()) >= max_joints) break;
        const int parent = nodes[n].parent;
        if (parent < 0 || index[static_cast<std::size_t>(parent)] < 0) continue;
        if (std::find(ignore.begin(), ignore.end(), static_cast<int>(n)) != ignore.end()) continue;
        const int p = index[static_cast<std::size_t>(parent)];
        index[n] = static_cast<int>(chain.nodes.size());
        chain.nodes.push_back(static_cast<int>(n));
        chain.parent.push_back(p);
        chain.leads.push_back(!has_child[static_cast<std::size_t>(p)]);
        has_child.push_back(false);
        has_child[static_cast<std::size_t>(p)] = true;
    }
    // Puntas extra: el ultimo hueso de cada rama tambien gira.
    if (tips) {
        const std::size_t real = chain.nodes.size();
        for (std::size_t i = 1; i < real; ++i) {
            if (has_child[i]) continue;
            chain.nodes.push_back(-1);
            chain.parent.push_back(static_cast<int>(i));
            chain.leads.push_back(true);
            has_child[i] = true;
        }
    }
    // Profundidad 0..1 (para afinar el radio hacia la punta).
    std::vector<int> steps(chain.nodes.size(), 0);
    int deepest = 1;
    for (std::size_t i = 1; i < chain.nodes.size(); ++i) {
        steps[i] = steps[static_cast<std::size_t>(chain.parent[i])] + 1;
        deepest = std::max(deepest, steps[i]);
    }
    chain.depth.resize(chain.nodes.size());
    for (std::size_t i = 0; i < chain.nodes.size(); ++i) chain.depth[i] = static_cast<float>(steps[i]) / static_cast<float>(deepest);
    return chain;
}

void update(const ik::Pose& pose, const Mat4& world, Chain& chain, const Settings& settings,
            const std::vector<Collider>& colliders, float dt) {
    const std::size_t count = chain.nodes.size();
    if (count < 2) return;

    // Donde pone cada particula la animacion (mundo). Las puntas extra
    // siguen la direccion del ultimo hueso.
    std::vector<Vec3> animated(count);
    for (std::size_t i = 0; i < count; ++i) {
        if (chain.nodes[i] >= 0) {
            animated[i] = ecs::transformPoint(world, ik::nodePosition(pose, chain.nodes[i]));
        } else {
            const std::size_t p = static_cast<std::size_t>(chain.parent[i]);
            const int gp = chain.parent[p];
            const Vec3 dir = gp >= 0 ? animated[p] - animated[static_cast<std::size_t>(gp)] : Vec3{0.0f, -0.05f, 0.0f};
            animated[i] = animated[p] + dir * std::max(settings.end_length, 0.0f);
        }
    }
    const bool teleported = chain.ready && core::length(animated[0] - chain.last_root) > 5.0f;
    if (!chain.ready || chain.position.size() != count || teleported) {
        chain.position = animated;
        chain.previous = animated;
        chain.last_root = animated[0];
        chain.ready = true;
    }

    if (dt > 0.0f) {
        const int substeps = std::clamp(static_cast<int>(std::ceil(dt * 60.0f)), 1, 4);
        const float h = dt / static_cast<float>(substeps);
        // Pull 1 = muelle de 400 1/s². Spring: cuanta velocidad conserva cada
        // paso de 1/60 s (0 = 80 %, frena pronto; 1 = 99 %, rebota mucho).
        const float pull = std::clamp(settings.pull, 0.0f, 1.0f) * 400.0f;
        const float keep = std::pow(0.8f + 0.19f * std::clamp(settings.spring, 0.0f, 1.0f), h * 60.0f);
        const float stiffness = std::clamp(settings.stiffness, 0.0f, 1.0f);
        const float immobile = std::clamp(settings.immobile, 0.0f, 1.0f);
        const float max_angle = settings.max_angle * core::kPi / 180.0f;
        const float radius_tip = settings.radius_tip < 0.0f ? settings.radius : settings.radius_tip;
        // El movimiento del personaje (la raiz), repartido entre los pasos.
        const Vec3 root_motion = (animated[0] - chain.last_root) * (1.0f / static_cast<float>(substeps));
        for (int s = 0; s < substeps; ++s) {
            if (immobile > 0.0f) {
                for (std::size_t i = 1; i < count; ++i) {
                    chain.position[i] = chain.position[i] + root_motion * immobile;
                    chain.previous[i] = chain.previous[i] + root_motion * immobile;
                }
            }
            chain.position[0] = animated[0];
            chain.previous[0] = animated[0];
            for (std::size_t i = 1; i < count; ++i) {
                const std::size_t p = static_cast<std::size_t>(chain.parent[i]);
                const Vec3 rest_offset = animated[i] - animated[p];
                const float length = core::length(rest_offset);
                if (length < 1e-6f) {
                    chain.previous[i] = chain.position[i];
                    chain.position[i] = chain.position[p];
                    continue;
                }
                const Vec3 rest_dir = rest_offset * (1.0f / length);
                const Vec3 target = chain.position[p] + rest_offset;
                const Vec3 velocity = (chain.position[i] - chain.previous[i]) * keep;
                const float hanging = std::max(core::dot(rest_dir, kDown), 0.0f);
                const float gravity = settings.gravity * kGravity * (1.0f - std::clamp(settings.gravity_falloff, 0.0f, 1.0f) * hanging);
                const Vec3 acceleration = (target - chain.position[i]) * pull + kDown * gravity;
                Vec3 next = chain.position[i] + velocity + acceleration * (h * h);
                next = core::lerp(next, target, stiffness * 0.5f);

                // Largo fijo y angulo maximo respecto a la animacion.
                const auto constrain = [&](const Vec3& v) {
                    Vec3 d = v - chain.position[p];
                    float len = core::length(d);
                    Vec3 dir = len > 1e-6f ? d * (1.0f / len) : rest_dir;
                    if (max_angle > 0.0f) {
                        const float angle = std::acos(std::clamp(core::dot(dir, rest_dir), -1.0f, 1.0f));
                        if (angle > max_angle) {
                            const Quat turn = ik::rotationBetween(rest_dir, dir);
                            dir = ecs::quatRotate(core::slerp(Quat{}, turn, max_angle / angle), rest_dir);
                        }
                    }
                    return chain.position[p] + dir * length;
                };
                next = constrain(next);
                if (settings.collide && !colliders.empty()) {
                    const float r = settings.radius + (radius_tip - settings.radius) * chain.depth[i];
                    // Choque, largo y otra vez choque: si el largo la vuelve a
                    // meter, gana el choque (estirarse un poco no se ve; atravesar si).
                    for (const Collider& c : colliders) next = collide(c, next, r);
                    next = constrain(next);
                    for (const Collider& c : colliders) next = collide(c, next, r);
                }
                chain.previous[i] = chain.position[i];
                chain.position[i] = next;
            }
        }
        chain.last_root = animated[0];
    }

    // Cada hueso apunta a su hijo simulado (el primero, si tiene varios). Las
    // puntas extra giran el ultimo hueso.
    const Mat4 to_model = core::inverse(world);
    for (std::size_t i = 1; i < count; ++i) {
        if (!chain.leads[i]) continue;
        const int parent_node = chain.nodes[static_cast<std::size_t>(chain.parent[i])];
        if (parent_node < 0) continue;
        const Vec3 from = ik::nodePosition(pose, parent_node);
        Vec3 now{};
        if (chain.nodes[i] >= 0) {
            now = ik::nodePosition(pose, chain.nodes[i]) - from;
        } else {
            // La punta extra, donde la llevaria ahora el hueso (su direccion).
            const std::size_t p = static_cast<std::size_t>(chain.parent[i]);
            const int gp = chain.parent[p];
            if (gp < 0 || chain.nodes[static_cast<std::size_t>(gp)] < 0) continue;
            now = from - ik::nodePosition(pose, chain.nodes[static_cast<std::size_t>(gp)]);
        }
        const Vec3 wanted = ecs::transformPoint(to_model, chain.position[i]) - from;
        if (core::length(now) < 1e-7f || core::length(wanted) < 1e-7f) continue;
        ik::rotateGlobal(pose, parent_node, ik::rotationBetween(core::normalize(now), core::normalize(wanted)));
    }
}

}  // namespace cramion::physbone
