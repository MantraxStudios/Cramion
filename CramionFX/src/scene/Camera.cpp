#include "CramionFX/scene/Camera.h"

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

// Las dos vuelven a la orientacion por yaw/pitch: antes, despues de un
// setOrientation (la vista 2D del editor) la camara se quedaba con la
// orientacion libre y ya no giraba con el raton.
void Camera::setRotation(float yaw_radians, float pitch_radians) {
    free_orientation_ = false;
    yaw_ = yaw_radians;
    pitch_ = std::clamp(pitch_radians, -kMaxPitch, kMaxPitch);
}

void Camera::lookAt(const Vec3& target) {
    free_orientation_ = false;
    const Vec3 direction = core::normalize(target - position_);
    yaw_ = std::atan2(direction.z, direction.x);
    pitch_ = std::clamp(std::asin(std::clamp(direction.y, -1.0f, 1.0f)), -kMaxPitch, kMaxPitch);
}

void Camera::setOrientation(const Vec3& forward, const Vec3& up) {
    free_orientation_ = true;
    free_forward_ = core::normalize(forward);
    free_up_ = core::normalize(up);
}

Vec3 Camera::forward() const {
    if (free_orientation_) {
        return free_forward_;
    }
    const float cos_pitch = std::cos(pitch_);
    return core::normalize(
        Vec3{std::cos(yaw_) * cos_pitch, std::sin(pitch_), std::sin(yaw_) * cos_pitch});
}

Vec3 Camera::right() const {
    return core::normalize(
        core::cross(forward(), free_orientation_ ? free_up_ : Vec3{0.0f, 1.0f, 0.0f}));
}

Vec3 Camera::up() const {
    return core::cross(right(), forward());
}

void Camera::update(const dm::Input& input, float delta_seconds) {
    // --- Mirar: solo mientras se mantiene el boton derecho ---
    if (input.isMouseButtonDown(dm::MouseButton::Right)) {
        if (free_orientation_) {
            // Sigue desde donde mira ahora (con yaw/pitch).
            yaw_ = std::atan2(free_forward_.z, free_forward_.x);
            pitch_ = std::clamp(std::asin(std::clamp(free_forward_.y, -1.0f, 1.0f)), -kMaxPitch, kMaxPitch);
            free_orientation_ = false;
        }
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
    return core::lookAt(position_, position_ + forward(),
                        free_orientation_ ? free_up_ : Vec3{0.0f, 1.0f, 0.0f});
}

void Camera::setFovAngles(float left, float right, float up, float down) {
    asymmetric_ = true;
    tan_left_ = std::tan(left);
    tan_right_ = std::tan(right);
    tan_up_ = std::tan(up);
    tan_down_ = std::tan(down);
    // El cono simetrico que lo contiene (cascadas de sombras, LOD).
    const float half_y = std::max(std::fabs(up), std::fabs(down));
    const float half_x = std::max(std::fabs(tan_left_), std::fabs(tan_right_));
    fov_y_ = 2.0f * half_y;
    aspect_ = half_x / std::max(std::tan(half_y), 1e-4f);
}

Mat4 Camera::projection() const {
    if (orthographic_) {
        const float half_width = ortho_size_ * aspect_;
        return core::orthographic(-half_width, half_width, -ortho_size_, ortho_size_, near_plane_, far_plane_);
    }
    if (!asymmetric_) return core::perspective(fov_y_, aspect_, near_plane_, far_plane_);
    // Como perspective() (Vulkan: profundidad [0, 1], Y invertida) con el
    // centro desplazado: tan(izq)..tan(der) -> -1..1.
    const float w = tan_right_ - tan_left_;
    const float h = tan_up_ - tan_down_;
    Mat4 result{};
    result.m[0][0] = 2.0f / w;
    result.m[2][0] = (tan_right_ + tan_left_) / w;
    result.m[1][1] = -2.0f / h;
    result.m[2][1] = -(tan_up_ + tan_down_) / h;
    result.m[2][2] = far_plane_ / (near_plane_ - far_plane_);
    result.m[2][3] = -1.0f;
    result.m[3][2] = (far_plane_ * near_plane_) / (near_plane_ - far_plane_);
    return result;
}

}  // namespace cramion::scene
