#include "CramionCore/ecs/MathUtil.h"

#include <algorithm>
#include <cmath>

namespace cramion::ecs {

using core::Mat4;
using core::Quat;
using core::Vec3;

namespace {
constexpr float kDegToRad = core::kPi / 180.0f;
constexpr float kRadToDeg = 180.0f / core::kPi;

Quat axisAngle(float radians, int axis) {
    const float half = radians * 0.5f;
    Quat q{0.0f, 0.0f, 0.0f, std::cos(half)};
    (axis == 0 ? q.x : (axis == 1 ? q.y : q.z)) = std::sin(half);
    return q;
}
}  // namespace

Quat quatMultiply(const Quat& a, const Quat& b) {
    return Quat{a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
                a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

Quat quatConjugate(const Quat& q) {
    return Quat{-q.x, -q.y, -q.z, q.w};
}

Vec3 quatRotate(const Quat& q, const Vec3& v) {
    // v' = v + 2 * cross(q.xyz, cross(q.xyz, v) + w * v)
    const Vec3 u{q.x, q.y, q.z};
    const Vec3 t = core::cross(u, v) * 2.0f;
    return v + t * q.w + core::cross(u, t);
}

Quat quatFromEulerDegrees(const Vec3& degrees) {
    const Quat qy = axisAngle(degrees.y * kDegToRad, 1);
    const Quat qx = axisAngle(degrees.x * kDegToRad, 0);
    const Quat qz = axisAngle(degrees.z * kDegToRad, 2);
    return core::normalize(quatMultiply(quatMultiply(qy, qx), qz));
}

// Para R = Ry Rx Rz (R(fila, col)): R(1,2) = -sx, R(0,2) = sy cx, R(2,2) = cy cx,
// R(1,0) = cx sz, R(1,1) = cx cz. Con cx ~ 0 (bloqueo de cardan) se fija z = 0.
Vec3 quatToEulerDegrees(const Quat& in) {
    const Quat q = core::normalize(in);
    const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    const float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    const float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    const float r12 = 2.0f * (yz - wx);
    const float r02 = 2.0f * (xz + wy);
    const float r22 = 1.0f - 2.0f * (xx + yy);
    const float r10 = 2.0f * (xy + wz);
    const float r11 = 1.0f - 2.0f * (xx + zz);
    const float x = std::asin(std::clamp(-r12, -1.0f, 1.0f));
    float y = 0.0f;
    float z = 0.0f;
    if (std::abs(r12) < 0.9999f) {
        y = std::atan2(r02, r22);
        z = std::atan2(r10, r11);
    } else {
        const float r00 = 1.0f - 2.0f * (yy + zz);
        const float r20 = 2.0f * (xz - wy);
        y = std::atan2(-r20, r00);
    }
    return Vec3{x * kRadToDeg, y * kRadToDeg, z * kRadToDeg};
}

Quat quatFromRotationMatrix(const Mat4& m) {
    // R(fila, col) = m[col][fila].
    const float r00 = m.m[0][0], r11 = m.m[1][1], r22 = m.m[2][2];
    const float trace = r00 + r11 + r22;
    Quat q{};
    if (trace > 0.0f) {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;
        q.w = 0.25f * s;
        q.x = (m.m[1][2] - m.m[2][1]) / s;
        q.y = (m.m[2][0] - m.m[0][2]) / s;
        q.z = (m.m[0][1] - m.m[1][0]) / s;
    } else if (r00 > r11 && r00 > r22) {
        const float s = std::sqrt(1.0f + r00 - r11 - r22) * 2.0f;
        q.w = (m.m[1][2] - m.m[2][1]) / s;
        q.x = 0.25f * s;
        q.y = (m.m[1][0] + m.m[0][1]) / s;
        q.z = (m.m[2][0] + m.m[0][2]) / s;
    } else if (r11 > r22) {
        const float s = std::sqrt(1.0f + r11 - r00 - r22) * 2.0f;
        q.w = (m.m[2][0] - m.m[0][2]) / s;
        q.x = (m.m[1][0] + m.m[0][1]) / s;
        q.y = 0.25f * s;
        q.z = (m.m[2][1] + m.m[1][2]) / s;
    } else {
        const float s = std::sqrt(1.0f + r22 - r00 - r11) * 2.0f;
        q.w = (m.m[0][1] - m.m[1][0]) / s;
        q.x = (m.m[2][0] + m.m[0][2]) / s;
        q.y = (m.m[2][1] + m.m[1][2]) / s;
        q.z = 0.25f * s;
    }
    return core::normalize(q);
}

void decomposeMatrix(const Mat4& m, Vec3& position, Quat& rotation, Vec3& scale) {
    position = Vec3{m.m[3][0], m.m[3][1], m.m[3][2]};
    Vec3 axes[3];
    float lengths[3];
    for (int c = 0; c < 3; ++c) {
        axes[c] = Vec3{m.m[c][0], m.m[c][1], m.m[c][2]};
        lengths[c] = core::length(axes[c]);
    }
    // Determinante negativo: un eje reflejado; se lleva el signo la X.
    const float det = core::dot(axes[0], core::cross(axes[1], axes[2]));
    if (det < 0.0f) {
        lengths[0] = -lengths[0];
    }
    scale = Vec3{lengths[0], lengths[1], lengths[2]};
    Mat4 r = Mat4::identity();
    for (int c = 0; c < 3; ++c) {
        const float s = std::abs(lengths[c]) > 1e-8f ? lengths[c] : 1.0f;
        r.m[c][0] = m.m[c][0] / s;
        r.m[c][1] = m.m[c][1] / s;
        r.m[c][2] = m.m[c][2] / s;
    }
    rotation = quatFromRotationMatrix(r);
}

Vec3 transformPoint(const Mat4& m, const Vec3& p) {
    const core::Vec4 r = m * core::Vec4{p.x, p.y, p.z, 1.0f};
    return Vec3{r.x, r.y, r.z};
}

Vec3 transformDirection(const Mat4& m, const Vec3& d) {
    const core::Vec4 r = m * core::Vec4{d.x, d.y, d.z, 0.0f};
    return Vec3{r.x, r.y, r.z};
}

float maxAxisScale(const Mat4& m) {
    float result = 0.0f;
    for (int c = 0; c < 3; ++c) {
        result = std::max(result, core::length(Vec3{m.m[c][0], m.m[c][1], m.m[c][2]}));
    }
    return result;
}

}  // namespace cramion::ecs
