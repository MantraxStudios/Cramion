#include "CramionCore/anim/Inertialization.h"

#include "CramionCore/ecs/MathUtil.h"

#include <algorithm>
#include <cmath>

namespace cramion::anim {

using core::Mat4;
using core::Quat;
using core::Vec3;

namespace {

void decomposeLocal(const Mat4& m, Vec3& position, Quat& rotation, Vec3& scale) {
    ecs::decomposeMatrix(m, position, rotation, scale);
}

// Giro de `a` a `b` en el hemisferio corto (b = d * a).
Quat difference(const Quat& b, const Quat& a) {
    Quat d = ecs::quatMultiply(b, ecs::quatConjugate(a));
    if (d.w < 0.0f) d = {-d.x, -d.y, -d.z, -d.w};
    return d;
}

}  // namespace

Vec3 quatToScaledAxis(const Quat& input) {
    Quat q = core::normalize(input);
    if (q.w < 0.0f) q = {-q.x, -q.y, -q.z, -q.w};
    const float s = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z);
    if (s < 1e-7f) return Vec3{q.x * 2.0f, q.y * 2.0f, q.z * 2.0f};
    const float angle = 2.0f * std::atan2(s, q.w);
    const float k = angle / s;
    return Vec3{q.x * k, q.y * k, q.z * k};
}

Quat quatFromScaledAxis(const Vec3& v) {
    const float angle = core::length(v);
    if (angle < 1e-7f) return core::normalize(Quat{v.x * 0.5f, v.y * 0.5f, v.z * 0.5f, 1.0f});
    const float s = std::sin(angle * 0.5f) / angle;
    return Quat{v.x * s, v.y * s, v.z * s, std::cos(angle * 0.5f)};
}

void decaySpring(Vec3& x, Vec3& v, float halflife, float delta_seconds) {
    // Holden, "Spring-It-On" (2021): amortiguamiento critico exacto.
    const float y = (4.0f * 0.69314718f) / (std::max(halflife, 1e-4f)) * 0.5f;
    const float e = std::exp(-y * delta_seconds);
    const Vec3 j1 = v + x * y;
    x = (x + j1 * delta_seconds) * e;
    v = (v - j1 * (y * delta_seconds)) * e;
}

void Inertializer::start(const std::vector<Mat4>& fresh, const std::vector<Mat4>& fresh_before, float duration,
                         float delta_seconds) {
    if (frames_ == 0 || fresh.size() != last_position_.size() || duration <= 0.0f) {
        active_ = false;
        return;
    }
    const std::size_t count = fresh.size();
    nodes_.assign(count, Node{});
    const float dt_out = std::max(last_delta_, 1e-4f);
    const float dt_in = std::max(delta_seconds, 1e-4f);
    for (std::size_t i = 0; i < count; ++i) {
        Vec3 p{}, p0{}, s{};
        Quat r{}, r0{};
        decomposeLocal(fresh[i], p, r, s);
        decomposeLocal(i < fresh_before.size() ? fresh_before[i] : fresh[i], p0, r0, s);
        Node& n = nodes_[i];
        // Lo que se ve menos la pose nueva, y lo mismo con las velocidades.
        n.position_offset = last_position_[i] - p;
        n.rotation_offset = quatToScaledAxis(difference(last_rotation_[i], r));
        Vec3 out_velocity{}, out_angular{};
        if (frames_ > 1) {
            out_velocity = (last_position_[i] - previous_position_[i]) * (1.0f / dt_out);
            out_angular = quatToScaledAxis(difference(last_rotation_[i], previous_rotation_[i])) * (1.0f / dt_out);
        }
        const Vec3 in_velocity = (p - p0) * (1.0f / dt_in);
        const Vec3 in_angular = quatToScaledAxis(difference(r, r0)) * (1.0f / dt_in);
        n.position_velocity = out_velocity - in_velocity;
        n.rotation_velocity = out_angular - in_angular;
        // Un hueso que salta (bucle del clip, un teletransporte) no arrastra
        // una velocidad absurda.
        const auto clampLength = [](Vec3& v, float max) {
            const float l = core::length(v);
            if (l > max) v = v * (max / l);
        };
        clampLength(n.position_velocity, 10.0f);
        clampLength(n.rotation_velocity, 20.0f);
    }
    duration_ = duration;
    // Con esta semivida, al acabar la duracion queda ~5% del desfase.
    halflife_ = std::max(duration * 0.3f, 0.01f);
    elapsed_ = 0.0f;
    active_ = true;
}

void Inertializer::apply(std::vector<Mat4>& locals, float delta_seconds) {
    const std::size_t count = locals.size();
    if (last_position_.size() != count) {
        last_position_.assign(count, Vec3{});
        previous_position_.assign(count, Vec3{});
        last_rotation_.assign(count, Quat{});
        previous_rotation_.assign(count, Quat{});
        frames_ = 0;
        active_ = false;
    }
    if (active_ && nodes_.size() == count) {
        elapsed_ += delta_seconds;
        for (std::size_t i = 0; i < count; ++i) {
            Node& n = nodes_[i];
            decaySpring(n.position_offset, n.position_velocity, halflife_, delta_seconds);
            decaySpring(n.rotation_offset, n.rotation_velocity, halflife_, delta_seconds);
            Vec3 p{}, s{};
            Quat r{};
            decomposeLocal(locals[i], p, r, s);
            p = p + n.position_offset;
            r = core::normalize(ecs::quatMultiply(quatFromScaledAxis(n.rotation_offset), r));
            locals[i] = core::composeTrs(p, r, s);
        }
        if (elapsed_ > duration_ * 2.0f) active_ = false;
    }
    for (std::size_t i = 0; i < count; ++i) {
        previous_position_[i] = last_position_[i];
        previous_rotation_[i] = last_rotation_[i];
        Vec3 s{};
        decomposeLocal(locals[i], last_position_[i], last_rotation_[i], s);
    }
    if (delta_seconds > 0.0f) last_delta_ = delta_seconds;
    frames_ = std::min(frames_ + 1, 2);
}

}  // namespace cramion::anim
