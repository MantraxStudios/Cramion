#include "CramionCore/anim/IK.h"

#include "CramionCore/ecs/MathUtil.h"

#include <algorithm>
#include <cmath>

namespace cramion::ik {

using core::Mat4;
using core::Quat;
using core::Vec3;

namespace {

const Mat4& parentGlobal(const Pose& pose, int node, const Mat4& identity) {
    const int parent = (*pose.nodes)[static_cast<std::size_t>(node)].parent;
    return parent >= 0 ? (*pose.global)[static_cast<std::size_t>(parent)] : identity;
}

// Cambia la global de `node` y deja su local de acuerdo (hijos incluidos).
void setGlobal(const Pose& pose, int node, const Mat4& global) {
    const Mat4 identity = Mat4::identity();
    (*pose.local)[static_cast<std::size_t>(node)] = core::inverse(parentGlobal(pose, node, identity)) * global;
    recomputeGlobals(pose, node);
}

Quat scaledRotation(const Quat& q, float weight) {
    return core::slerp(Quat{}, q, std::clamp(weight, 0.0f, 1.0f));
}

}  // namespace

Quat rotationBetween(const Vec3& a, const Vec3& b) {
    const float d = core::dot(a, b);
    if (d < -0.9999f) {
        Vec3 axis = core::cross(Vec3{1.0f, 0.0f, 0.0f}, a);
        if (core::length(axis) < 1e-4f) axis = core::cross(Vec3{0.0f, 1.0f, 0.0f}, a);
        axis = core::normalize(axis);
        return Quat{axis.x, axis.y, axis.z, 0.0f};
    }
    const Vec3 c = core::cross(a, b);
    return core::normalize(Quat{c.x, c.y, c.z, 1.0f + d});
}

void recomputeGlobals(const Pose& pose, int from) {
    const std::vector<asset::Node>& nodes = *pose.nodes;
    std::vector<Mat4>& local = *pose.local;
    std::vector<Mat4>& global = *pose.global;
    for (std::size_t i = static_cast<std::size_t>(std::max(from, 0)); i < nodes.size(); ++i) {
        const int parent = nodes[i].parent;
        global[i] = parent >= 0 ? global[static_cast<std::size_t>(parent)] * local[i] : local[i];
    }
}

Vec3 nodePosition(const Pose& pose, int node) {
    const Mat4& g = (*pose.global)[static_cast<std::size_t>(node)];
    return Vec3{g.m[3][0], g.m[3][1], g.m[3][2]};
}

Quat nodeRotation(const Pose& pose, int node) {
    Vec3 t{};
    Quat r{};
    Vec3 s{};
    ecs::decomposeMatrix((*pose.global)[static_cast<std::size_t>(node)], t, r, s);
    return r;
}

void rotateGlobal(const Pose& pose, int node, const Quat& delta) {
    const Mat4& g = (*pose.global)[static_cast<std::size_t>(node)];
    const Vec3 p{g.m[3][0], g.m[3][1], g.m[3][2]};
    const Mat4 r = core::composeTrs(Vec3{}, delta, Vec3{1.0f, 1.0f, 1.0f});
    setGlobal(pose, node, core::translate(p) * r * core::translate(p * -1.0f) * g);
}

void translateGlobal(const Pose& pose, int node, const Vec3& offset) {
    setGlobal(pose, node, core::translate(offset) * (*pose.global)[static_cast<std::size_t>(node)]);
}

void setGlobalRotation(const Pose& pose, int node, const Quat& rotation) {
    const Quat current = nodeRotation(pose, node);
    rotateGlobal(pose, node, ecs::quatMultiply(rotation, ecs::quatConjugate(current)));
}

bool twoBone(const Pose& pose, int upper, int mid, int end, const Vec3& target, const Vec3* pole, float weight) {
    const int n = static_cast<int>(pose.nodes->size());
    if (upper < 0 || mid < 0 || end < 0 || upper >= n || mid >= n || end >= n || weight <= 0.0f) return false;
    const Vec3 a = nodePosition(pose, upper);
    const Vec3 b = nodePosition(pose, mid);
    const Vec3 c = nodePosition(pose, end);
    const float l1 = core::length(b - a);
    const float l2 = core::length(c - b);
    if (l1 < 1e-6f || l2 < 1e-6f) return false;
    // Objetivo mezclado con la pose animada.
    const Vec3 goal = c + (target - c) * std::clamp(weight, 0.0f, 1.0f);
    Vec3 to_goal = goal - a;
    float distance = core::length(to_goal);
    if (distance < 1e-6f) return false;
    const Vec3 dir = to_goal * (1.0f / distance);
    distance = std::clamp(distance, std::abs(l1 - l2) + 1e-4f, (l1 + l2) * 0.9999f);

    // Hacia donde se dobla: el pole o, sin el, hacia donde ya se doblaba
    // (el codo respecto a la linea hombro-mano).
    Vec3 bend = pole != nullptr ? *pole - a : b - (a + c) * 0.5f;
    bend = bend - dir * core::dot(bend, dir);
    if (core::length(bend) < 1e-5f) {
        bend = b - a;
        bend = bend - dir * core::dot(bend, dir);
        if (core::length(bend) < 1e-5f) return false;
    }
    bend = core::normalize(bend);

    // Ley del coseno: angulo en el hombro/cadera.
    const float cos_a = std::clamp((l1 * l1 + distance * distance - l2 * l2) / (2.0f * l1 * distance), -1.0f, 1.0f);
    const float sin_a = std::sqrt(std::max(1.0f - cos_a * cos_a, 0.0f));
    const Vec3 new_mid = a + dir * (l1 * cos_a) + bend * (l1 * sin_a);
    const Vec3 new_end = a + dir * distance;

    // 1) el hueso de arriba lleva el codo a su sitio; 2) el del medio, la mano.
    rotateGlobal(pose, upper, rotationBetween(core::normalize(b - a), core::normalize(new_mid - a)));
    const Vec3 mid_now = nodePosition(pose, mid);
    const Vec3 end_now = nodePosition(pose, end);
    rotateGlobal(pose, mid, rotationBetween(core::normalize(end_now - mid_now), core::normalize(new_end - mid_now)));
    return true;
}

std::vector<int> chainTo(const std::vector<asset::Node>& nodes, int end, int bones) {
    std::vector<int> joints;
    if (end < 0 || static_cast<std::size_t>(end) >= nodes.size() || bones < 1) return joints;
    joints.push_back(end);
    for (int i = 0; i < bones; ++i) {
        const int parent = nodes[static_cast<std::size_t>(joints.back())].parent;
        if (parent < 0) return {};
        joints.push_back(parent);
    }
    std::reverse(joints.begin(), joints.end());
    return joints;
}

bool chain(const Pose& pose, const std::vector<int>& joints, const Vec3& target, const Vec3* pole, float weight,
           int iterations) {
    const std::size_t count = joints.size();
    const int n = static_cast<int>(pose.nodes->size());
    if (count < 2 || weight <= 0.0f) return false;
    for (const int j : joints) {
        if (j < 0 || j >= n) return false;
    }
    if (count == 3) return twoBone(pose, joints[0], joints[1], joints[2], target, pole, weight);

    std::vector<Vec3> p(count);
    std::vector<float> length(count - 1);
    float total = 0.0f;
    for (std::size_t i = 0; i < count; ++i) p[i] = nodePosition(pose, joints[i]);
    for (std::size_t i = 0; i + 1 < count; ++i) {
        length[i] = core::length(p[i + 1] - p[i]);
        total += length[i];
    }
    if (total < 1e-6f) return false;
    const Vec3 root = p[0];
    const Vec3 goal = p.back() + (target - p.back()) * std::clamp(weight, 0.0f, 1.0f);

    if (core::length(goal - root) >= total) {
        // No llega: estirada hacia el objetivo.
        const Vec3 dir = core::normalize(goal - root);
        for (std::size_t i = 0; i + 1 < count; ++i) p[i + 1] = p[i] + dir * length[i];
    } else {
        const float tolerance = total * 1e-4f;
        for (int it = 0; it < iterations && core::length(p.back() - goal) > tolerance; ++it) {
            // Hacia atras: el extremo al objetivo.
            p.back() = goal;
            for (std::size_t i = count - 1; i-- > 0;) {
                const Vec3 d = p[i] - p[i + 1];
                const float len = core::length(d);
                p[i] = len > 1e-6f ? p[i + 1] + d * (length[i] / len) : p[i + 1];
            }
            // Hacia delante: la raiz a su sitio.
            p[0] = root;
            for (std::size_t i = 0; i + 1 < count; ++i) {
                const Vec3 d = p[i + 1] - p[i];
                const float len = core::length(d);
                p[i + 1] = len > 1e-6f ? p[i] + d * (length[i] / len) : p[i];
            }
        }
        // Pole: cada articulacion de en medio gira alrededor de la linea
        // entre sus vecinas hasta quedar lo mas cerca posible del pole.
        if (pole != nullptr) {
            for (std::size_t i = 1; i + 1 < count; ++i) {
                const Vec3 axis_vec = p[i + 1] - p[i - 1];
                const float axis_len = core::length(axis_vec);
                if (axis_len < 1e-6f) continue;
                const Vec3 axis = axis_vec * (1.0f / axis_len);
                const auto project = [&](const Vec3& v) {
                    const Vec3 rel = v - p[i - 1];
                    return rel - axis * core::dot(rel, axis);
                };
                const Vec3 joint = project(p[i]);
                const Vec3 wanted = project(*pole);
                if (core::length(joint) < 1e-6f || core::length(wanted) < 1e-6f) continue;
                const Quat turn = rotationBetween(core::normalize(joint), core::normalize(wanted));
                p[i] = p[i - 1] + core::dot(p[i] - p[i - 1], axis) * axis + ecs::quatRotate(turn, joint);
            }
        }
    }
    // Cada hueso apunta a su nueva articulacion (los hijos le siguen).
    for (std::size_t i = 0; i + 1 < count; ++i) {
        const Vec3 from = nodePosition(pose, joints[i]);
        const Vec3 now = nodePosition(pose, joints[i + 1]) - from;
        const Vec3 wanted = p[i + 1] - from;
        if (core::length(now) < 1e-6f || core::length(wanted) < 1e-6f) continue;
        rotateGlobal(pose, joints[i], rotationBetween(core::normalize(now), core::normalize(wanted)));
    }
    return true;
}

Quat groundTilt(const std::vector<Vec3>& feet, const std::vector<float>& heights, const Vec3& forward, float max_degrees) {
    const Vec3 up{0.0f, 1.0f, 0.0f};
    Vec3 f{forward.x, 0.0f, forward.z};
    if (feet.size() < 3 || feet.size() != heights.size() || core::length(f) < 1e-5f) return Quat{};
    f = core::normalize(f);
    const Vec3 right = core::normalize(core::cross(f, up));
    // Pendiente (altura por metro) a lo largo de un eje horizontal.
    const auto slope = [&](const Vec3& axis) {
        float sx = 0.0f, sy = 0.0f, sxx = 0.0f, sxy = 0.0f;
        const float n = static_cast<float>(feet.size());
        for (std::size_t i = 0; i < feet.size(); ++i) {
            const float x = core::dot(feet[i], axis);
            sx += x;
            sy += heights[i];
            sxx += x * x;
            sxy += x * heights[i];
        }
        const float den = n * sxx - sx * sx;
        return std::abs(den) > 1e-6f ? (n * sxy - sx * sy) / den : 0.0f;
    };
    const float limit = max_degrees * core::kPi / 180.0f;
    const float pitch = std::clamp(std::atan(slope(f)), -limit, limit);      // + = sube por delante
    const float roll = std::clamp(std::atan(slope(right)), -limit, limit);   // + = sube por la derecha
    const auto axis_angle = [](const Vec3& axis, float radians) {
        const Vec3 a = axis * std::sin(radians * 0.5f);
        return Quat{a.x, a.y, a.z, std::cos(radians * 0.5f)};
    };
    // Girar +a alrededor de la derecha (f x arriba) lleva delante hacia
    // arriba; girar +a alrededor de delante lleva la derecha hacia abajo.
    return ecs::quatMultiply(axis_angle(right, pitch), axis_angle(f, -roll));
}

void lookChain(const Pose& pose, const std::vector<int>& bones, const Vec3& head_forward, const Vec3& target, float weight,
               float max_angle_degrees) {
    if (bones.empty() || weight <= 0.0f) return;
    const int head = bones.back();
    const Vec3 to = target - nodePosition(pose, head);
    if (core::length(to) < 1e-4f || core::length(head_forward) < 1e-6f) return;
    Quat turn = rotationBetween(core::normalize(head_forward), core::normalize(to));
    const float angle = 2.0f * std::acos(std::clamp(std::abs(turn.w), 0.0f, 1.0f));
    const float max_angle = max_angle_degrees * core::kPi / 180.0f;
    if (angle > max_angle && angle > 1e-5f) turn = scaledRotation(turn, max_angle / angle);
    turn = scaledRotation(turn, weight);
    // Cada hueso hace su parte; los hijos heredan lo de sus padres, asi que la
    // cabeza acaba girada el total.
    const Quat part = scaledRotation(turn, 1.0f / static_cast<float>(bones.size()));
    for (const int bone : bones) rotateGlobal(pose, bone, part);
}

void lookAt(const Pose& pose, int bone, const Vec3& forward, const Vec3& target, float weight, float max_angle_degrees) {
    if (bone < 0 || weight <= 0.0f) return;
    const Vec3 from = nodePosition(pose, bone);
    const Vec3 to = target - from;
    if (core::length(to) < 1e-4f) return;
    const Vec3 dir = core::normalize(to);
    Quat turn = rotationBetween(core::normalize(forward), dir);
    // Angulo maximo.
    const float angle = 2.0f * std::acos(std::clamp(std::abs(turn.w), 0.0f, 1.0f));
    const float max_angle = max_angle_degrees * core::kPi / 180.0f;
    if (angle > max_angle && angle > 1e-5f) turn = scaledRotation(turn, max_angle / angle);
    rotateGlobal(pose, bone, scaledRotation(turn, weight));
}

}  // namespace cramion::ik
