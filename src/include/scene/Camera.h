#ifndef CRAMION_SCENE_CAMERA_H
#define CRAMION_SCENE_CAMERA_H

#include "core/Math.h"

namespace cramion::dm {
class Input;
}

namespace cramion::scene {

// Camara libre en primera persona.
//
// Controles:
//   W A S D            avanzar / desplazarse
//   Espacio / Control  subir / bajar
//   Shift              moverse mas rapido
//   Raton derecho      mirar alrededor (arrastrando)
//   Rueda              cambiar la velocidad de movimiento
class Camera {
public:
    void setPosition(const core::Vec3& position) { position_ = position; }
    void setRotation(float yaw_radians, float pitch_radians);

    // Mira hacia un punto desde la posicion actual.
    void lookAt(const core::Vec3& target);

    // Aplica la entrada de este frame. `delta_seconds` viene del reloj.
    void update(const dm::Input& input, float delta_seconds);

    // Relacion de aspecto de la ventana; se aplica a la proyeccion.
    void setAspectRatio(float aspect) { aspect_ = (aspect > 0.0f) ? aspect : 1.0f; }

    core::Mat4 view() const;
    core::Mat4 projection() const;

    const core::Vec3& position() const { return position_; }
    core::Vec3 forward() const;
    core::Vec3 right() const;
    core::Vec3 up() const;

    float fovY() const { return fov_y_; }
    float aspectRatio() const { return aspect_; }
    float nearPlane() const { return near_plane_; }
    float farPlane() const { return far_plane_; }
    float moveSpeed() const { return move_speed_; }

private:
    core::Vec3 position_{0.0f, 40.0f, 0.0f};

    float yaw_ = -core::kPi * 0.5f;  // Mirando hacia -Z.
    float pitch_ = -0.25f;

    float fov_y_ = core::radians(70.0f);
    float aspect_ = 16.0f / 9.0f;
    float near_plane_ = 0.1f;
    float far_plane_ = 500.0f;

    float move_speed_ = 14.0f;
    float mouse_sensitivity_ = 0.0028f;
};

}  // namespace cramion::scene

#endif  // CRAMION_SCENE_CAMERA_H
