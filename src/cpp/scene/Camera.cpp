#include "scene/Camera.h"

#include <CramionDM/Input.h>

#include <algorithm>
#include <cmath>

namespace cramion::scene {

using core::Mat4;
using core::Vec3;

namespace {
// Limite del cabeceo para que la camara no se de la vuelta en los polos.
constexpr float kMaxPitch = core::kPi * 0.5f - 0.01f;
}  // namespace

void Camera::setRotation(float yaw_radians, float pitch_radians) {
    yaw_ = yaw_radians;
    pitch_ = std::clamp(pitch_radians, -kMaxPitch, kMaxPitch);
}

void Camera::lookAt(const Vec3& target) {
    const Vec3 direction = core::normalize(target - position_);
    yaw_ = std::atan2(direction.z, direction.x);
    pitch_ = std::clamp(std::asin(direction.y), -kMaxPitch, kMaxPitch);
}

Vec3 Camera::forward() const {
    const float cos_pitch = std::cos(pitch_);
    return core::normalize(
        Vec3{std::cos(yaw_) * cos_pitch, std::sin(pitch_), std::sin(yaw_) * cos_pitch});
}

Vec3 Camera::right() const {
    return core::normalize(core::cross(forward(), Vec3{0.0f, 1.0f, 0.0f}));
}

Vec3 Camera::up() const {
    return core::cross(right(), forward());
}

void Camera::update(const dm::Input& input, float delta_seconds) {
    // --- Mirar: solo mientras se mantiene el boton derecho ---
    if (input.isMouseButtonDown(dm::MouseButton::Right)) {
        yaw_ += input.mouseDeltaX() * mouse_sensitivity_;
        pitch_ -= input.mouseDeltaY() * mouse_sensitivity_;
        pitch_ = std::clamp(pitch_, -kMaxPitch, kMaxPitch);
    }

    // --- Velocidad con la rueda ---
    if (input.scrollY() != 0.0f) {
        move_speed_ = std::clamp(move_speed_ * std::pow(1.15f, input.scrollY()), 1.0f, 200.0f);
    }

    // --- Desplazamiento ---
    Vec3 direction{};
    const Vec3 fwd = forward();
    const Vec3 rgt = right();

    if (input.isKeyDown(dm::Key::W)) direction += fwd;
    if (input.isKeyDown(dm::Key::S)) direction -= fwd;
    if (input.isKeyDown(dm::Key::D)) direction += rgt;
    if (input.isKeyDown(dm::Key::A)) direction -= rgt;
    if (input.isKeyDown(dm::Key::Space)) direction += Vec3{0.0f, 1.0f, 0.0f};
    if (input.isKeyDown(dm::Key::LeftControl)) direction -= Vec3{0.0f, 1.0f, 0.0f};

    if (core::dot(direction, direction) > 0.0f) {
        const float speed =
            move_speed_ * (input.isKeyDown(dm::Key::LeftShift) ? 4.0f : 1.0f);
        position_ += core::normalize(direction) * (speed * delta_seconds);
    }
}

Mat4 Camera::view() const {
    return core::lookAt(position_, position_ + forward(), Vec3{0.0f, 1.0f, 0.0f});
}

Mat4 Camera::projection() const {
    return core::perspective(fov_y_, aspect_, near_plane_, far_plane_);
}

}  // namespace cramion::scene
