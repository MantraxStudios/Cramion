#include "CramionCore/anim/Procedural.h"

#include "CramionCore/ecs/MathUtil.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <unordered_map>

namespace cramion::procedural {

using core::Mat4;
using core::Quat;
using core::Vec3;

namespace {

constexpr Vec3 kUp{0.0f, 1.0f, 0.0f};

Quat axisAngle(const Vec3& axis, float degrees) {
    const float half = degrees * core::kPi / 360.0f;
    const Vec3 a = core::normalize(axis) * std::sin(half);
    return Quat{a.x, a.y, a.z, std::cos(half)};
}

Vec3 horizontal(const Vec3& v) { return Vec3{v.x, 0.0f, v.z}; }

}  // namespace

// --- Huesos con muelle ------------------------------------------------------------

SpringChain makeSpringChain(const std::vector<asset::Node>& nodes, int root, int max_joints) {
    SpringChain chain;
    if (root < 0 || static_cast<std::size_t>(root) >= nodes.size()) return chain;
    std::unordered_map<int, int> index;  // nodo -> indice en la cadena
    std::unordered_map<int, bool> has_child;
    chain.joints.push_back(root);
    chain.parent.push_back(-1);
    chain.leads.push_back(false);
    index[root] = 0;
    // Los padres van antes que los hijos: un recorrido basta.
    for (std::size_t n = static_cast<std::size_t>(root) + 1; n < nodes.size(); ++n) {
        if (static_cast<int>(chain.joints.size()) >= max_joints) break;
        const auto p = index.find(nodes[n].parent);
        if (p == index.end()) continue;
        index[static_cast<int>(n)] = static_cast<int>(chain.joints.size());
        chain.joints.push_back(static_cast<int>(n));
        chain.parent.push_back(p->second);
        chain.leads.push_back(!has_child[p->second]);
        has_child[p->second] = true;
    }
    return chain;
}

void updateSprings(const ik::Pose& pose, const Mat4& world, SpringChain& chain, const SpringSettings& settings,
                   const std::vector<Sphere>& colliders, float dt) {
    const std::size_t count = chain.joints.size();
    if (count < 2) return;
    // Donde pone cada hueso la animacion (mundo).
    std::vector<Vec3> animated(count);
    for (std::size_t i = 0; i < count; ++i) animated[i] = ecs::transformPoint(world, ik::nodePosition(pose, chain.joints[i]));

    // Primera vez o teletransporte: las particulas donde la animacion.
    if (!chain.ready || chain.position.size() != count || core::length(chain.position[0] - animated[0]) > 3.0f) {
        chain.position = animated;
        chain.previous = animated;
        chain.ready = true;
    }
    if (dt > 0.0f) {
        const int steps = std::clamp(static_cast<int>(std::ceil(dt * 60.0f)), 1, 4);
        const float h = dt / static_cast<float>(steps);
        const float keep = 1.0f - std::clamp(settings.damping, 0.0f, 1.0f);
        const Vec3 gravity = kUp * -settings.gravity;
        for (int s = 0; s < steps; ++s) {
            // La raiz va clavada a la animacion.
            chain.position[0] = animated[0];
            chain.previous[0] = animated[0];
            for (std::size_t i = 1; i < count; ++i) {
                const std::size_t p = static_cast<std::size_t>(chain.parent[i]);
                const Vec3 offset = animated[i] - animated[p];
                const float length = core::length(offset);
                // A donde lo llevaria la animacion colgando del padre simulado.
                const Vec3 target = chain.position[p] + offset;
                const Vec3 velocity = (chain.position[i] - chain.previous[i]) * keep;
                const Vec3 acceleration = (target - chain.position[i]) * settings.stiffness + gravity;
                Vec3 next = chain.position[i] + velocity + acceleration * (h * h);
                // Sin estirarse: a su largo del padre.
                const auto constrain = [&](Vec3 v) {
                    const Vec3 d = v - chain.position[p];
                    const float len = core::length(d);
                    return len > 1e-6f ? chain.position[p] + d * (length / len) : target;
                };
                next = constrain(next);
                // Choca con las esferas (el cuerpo).
                for (const Sphere& sphere : colliders) {
                    const Vec3 d = next - sphere.center;
                    const float reach = sphere.radius + settings.radius;
                    const float len = core::length(d);
                    if (len < reach && len > 1e-6f) next = constrain(sphere.center + d * (reach / len));
                }
                chain.previous[i] = chain.position[i];
                chain.position[i] = next;
            }
        }
    }
    // Cada hueso gira para apuntar a su hijo simulado (el primero, si tiene varios).
    const Mat4 to_model = core::inverse(world);
    for (std::size_t i = 1; i < count; ++i) {
        if (!chain.leads[i]) continue;
        const int parent_node = chain.joints[static_cast<std::size_t>(chain.parent[i])];
        const Vec3 from = ik::nodePosition(pose, parent_node);
        const Vec3 now = ik::nodePosition(pose, chain.joints[i]) - from;
        const Vec3 wanted = ecs::transformPoint(to_model, chain.position[i]) - from;
        if (core::length(now) < 1e-6f || core::length(wanted) < 1e-6f) continue;
        ik::rotateGlobal(pose, parent_node, ik::rotationBetween(core::normalize(now), core::normalize(wanted)));
    }
}

// --- Patas ------------------------------------------------------------------------

int commonAncestor(const std::vector<asset::Node>& nodes, const std::vector<int>& of) {
    if (of.empty()) return -1;
    std::vector<int> path;  // de of[0] a la raiz
    for (int n = of[0]; n >= 0; n = nodes[static_cast<std::size_t>(n)].parent) path.push_back(n);
    std::size_t best = 0;  // indice en `path` comun a todos
    for (std::size_t k = 1; k < of.size(); ++k) {
        std::vector<int> ancestors;
        for (int n = of[k]; n >= 0; n = nodes[static_cast<std::size_t>(n)].parent) ancestors.push_back(n);
        while (best < path.size() && std::find(ancestors.begin(), ancestors.end(), path[best]) == ancestors.end()) ++best;
    }
    return best < path.size() ? path[best] : -1;
}

int updateLegs(const ik::Pose& pose, const Mat4& world, std::vector<Leg>& legs, int body, const LegSettings& settings,
               const GroundQuery& ground, const Vec3& velocity, float dt) {
    if (legs.empty()) return 0;
    const Mat4 to_model = core::inverse(world);
    const float base_y = world.m[3][1];
    const auto snap = [&](Vec3 p, float lift) {
        // El pie sobre el suelo que tiene debajo (a su altura de la animacion).
        Vec3 hit{};
        Vec3 normal{};
        const float reach = settings.step_height * 4.0f + 1.0f;
        if (ground && ground(Vec3{p.x, base_y + reach * 0.5f + lift, p.z}, kUp * -1.0f, reach + lift, hit, normal)) {
            p.y = hit.y + lift;
        }
        return p;
    };

    // Donde pondria cada pie la animacion (su "casa"), sobre el suelo.
    std::vector<Vec3> home(legs.size());
    std::vector<Vec3> animated(legs.size());  // sin el suelo: donde lo deja la animacion
    for (std::size_t i = 0; i < legs.size(); ++i) {
        animated[i] = ecs::transformPoint(world, ik::nodePosition(pose, legs[i].end));
        home[i] = snap(animated[i], std::max(animated[i].y - base_y, 0.0f));
        Leg& leg = legs[i];
        if (!leg.ready || core::length(leg.planted - home[i]) > settings.step_distance * 8.0f) {
            leg.planted = home[i];
            leg.stepping = false;
            leg.ready = true;
        }
    }

    int started = 0;
    if (dt > 0.0f) {
        // Pasos en curso.
        for (Leg& leg : legs) {
            if (!leg.stepping) continue;
            leg.t += dt / std::max(settings.step_duration, 0.01f);
            if (leg.t >= 1.0f) {
                leg.planted = leg.to;
                leg.stepping = false;
                leg.t = 0.0f;
            }
        }
        // Pasos nuevos: primero los pies mas alejados de su sitio.
        std::vector<std::size_t> order(legs.size());
        std::iota(order.begin(), order.end(), std::size_t{0});
        std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
            return core::length(horizontal(legs[a].planted - home[a])) > core::length(horizontal(legs[b].planted - home[b]));
        });
        for (const std::size_t i : order) {
            Leg& leg = legs[i];
            if (leg.stepping) continue;
            const float distance = core::length(horizontal(leg.planted - home[i]));
            if (distance <= settings.step_distance) continue;
            // Se turnan: no mientras pisa el otro grupo (salvo que se quede muy atras).
            const bool other_moving = std::any_of(legs.begin(), legs.end(), [&](const Leg& o) {
                return o.stepping && o.group != leg.group;
            });
            if (other_moving && distance < settings.step_distance * 2.0f) continue;
            leg.from = leg.planted;
            const Vec3 ahead = horizontal(velocity) * (settings.step_duration * settings.overshoot);
            leg.to = snap(home[i] + ahead, 0.0f);
            leg.to.y += home[i].y - snap(home[i], 0.0f).y;  // su altura de la animacion
            leg.stepping = true;
            leg.t = 0.0f;
            ++started;
        }
    }

    // Donde esta cada pie ahora (con el arco del paso) y su punto en el suelo.
    std::vector<Vec3> foot(legs.size());
    std::vector<Vec3> grounded(legs.size());
    for (std::size_t i = 0; i < legs.size(); ++i) {
        const Leg& leg = legs[i];
        if (leg.stepping) {
            const float s = leg.t * leg.t * (3.0f - 2.0f * leg.t);
            grounded[i] = leg.from + (leg.to - leg.from) * s;
            foot[i] = grounded[i] + kUp * (std::sin(leg.t * core::kPi) * settings.step_height);
        } else {
            grounded[i] = leg.planted;
            foot[i] = leg.planted;
        }
    }

    // El cuerpo: sube o baja lo que el suelo de los pies se aparta de donde
    // los pone la animacion (que cree que el suelo esta a la altura de la
    // entidad) y se inclina con el plano que forman.
    if (settings.adjust_body && body >= 0 && settings.body_weight > 0.0f) {
        float dy = 0.0f;
        for (std::size_t i = 0; i < legs.size(); ++i) dy += grounded[i].y - animated[i].y;
        dy /= static_cast<float>(legs.size());
        ik::translateGlobal(pose, body, ecs::transformDirection(to_model, kUp * (dy * settings.body_weight)));
        if (legs.size() >= 3) {
            Vec3 center{};
            for (const Vec3& p : grounded) center = center + p;
            center = center * (1.0f / static_cast<float>(legs.size()));
            Vec3 normal{};
            for (std::size_t a = 0; a < legs.size(); ++a) {
                for (std::size_t b = a + 1; b < legs.size(); ++b) {
                    Vec3 c = core::cross(grounded[a] - center, grounded[b] - center);
                    if (c.y < 0.0f) c = c * -1.0f;
                    normal = normal + c;
                }
            }
            if (core::length(normal) > 1e-6f) {
                normal = core::normalize(normal);
                if (normal.y > 0.3f) {
                    const Vec3 up_model = core::normalize(ecs::transformDirection(to_model, kUp));
                    const Vec3 normal_model = core::normalize(ecs::transformDirection(to_model, normal));
                    ik::rotateGlobal(pose, body,
                                     core::slerp(Quat{}, ik::rotationBetween(up_model, normal_model), settings.body_weight));
                }
            }
        }
    }

    // Cada pata a su pie.
    for (std::size_t i = 0; i < legs.size(); ++i) {
        ik::twoBone(pose, legs[i].upper, legs[i].mid, legs[i].end, ecs::transformPoint(to_model, foot[i]), nullptr, 1.0f);
    }
    return started;
}

// --- Capas -------------------------------------------------------------------------

void rotateBone(const ik::Pose& pose, int bone, const Vec3& axis, float degrees) {
    if (bone < 0 || std::abs(degrees) < 1e-5f || core::length(axis) < 1e-6f) return;
    ik::rotateGlobal(pose, bone, axisAngle(axis, degrees));
}

float smoothNoise(float time, float frequency, float seed) {
    const float w = time * frequency * 2.0f * core::kPi;
    return std::sin(w + seed) * 0.5f + std::sin(w * 2.31f + seed * 1.7f) * 0.3f + std::sin(w * 4.13f + seed * 2.9f) * 0.2f;
}

}  // namespace cramion::procedural
