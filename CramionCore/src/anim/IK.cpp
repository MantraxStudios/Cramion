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
