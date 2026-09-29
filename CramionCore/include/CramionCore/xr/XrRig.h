#ifndef CRAMION_CORE_XR_RIG_H
#define CRAMION_CORE_XR_RIG_H

// Realidad virtual en la escena (como el XR Origin de Unity):
//
//   XR Origin (entidad vacia con XrOrigin)   el suelo de la habitacion; se
//   |                                          mueve para mover al jugador
//   +- Main Camera                            la cabeza (la pone el casco)
//   +- Mano izquierda (XrController)          el mando (la pone el casco)
//   +- Mano derecha   (XrController)
//
// Sin XR Origin en la escena, la camara principal hace de origen (su
// posicion y su giro horizontal) y el seguimiento empieza en los ojos: la
// cabeza sale donde esta la camara. Asi cualquier escena se ve en VR.
//
// Cada frame, con el casco en marcha:
//   xr.beginFrame(); rig.update(world, xr);     antes de la logica (poses)
//   ... fisica, scripts, RenderSync ...
//   rig.renderEyes(renderer, scene); xr.endFrame();

#include "CramionCore/ecs/World.h"

#include <CramionFX/core/Math.h>
#include <CramionFX/xr/XrSystem.h>

namespace cramion::gfx {
class VulkanRenderer;
}
namespace cramion::scene {
class Scene;
}
namespace cramion::ecs {
class PropertyVisitor;
}

namespace cramion::xr {

// El origen del seguimiento: el suelo de la habitacion (o los ojos, sentado).
struct XrOrigin {
    TrackingOrigin tracking = TrackingOrigin::Floor;
    // Sentado (tracking = Eyes): altura de la cabeza sobre el origen.
    float camera_y_offset = 1.6f;
    // La camara principal sigue al casco (si no, solo se mueven los mandos).
    bool drive_camera = true;

    void reflect(ecs::PropertyVisitor& v);
};

enum class ControllerPose : std::uint8_t { Grip = 0, Aim = 1 };

// La entidad sigue un mando (su posicion y giro en el mundo).
struct XrController {
    Hand hand = Hand::Right;
    ControllerPose pose = ControllerPose::Grip;
    // Sin mando (apagado o fuera de vista) se desactivan los hijos visibles.
    bool hide_when_untracked = true;

    void reflect(ecs::PropertyVisitor& v);
};

void registerXrComponents();

class XrRig {
public:
    // Despues de xr.beginFrame(): elige el origen, lo aplica y mueve la
    // camara principal y las entidades con XrController.
    void update(ecs::World& world, XrSystem& xr);
    // Los dos ojos con la camara de la escena (planos y efectos) movida a
    // cada ojo. Despues deja la camara de la escena en la cabeza (la ventana
    // hace de espejo).
    void renderEyes(gfx::VulkanRenderer& renderer, scene::Scene& scene);
    void reset();

    bool active() const { return active_; }
    // Del espacio del casco al mundo (con el origen de este frame).
    Pose toWorld(const Pose& tracked) const;
    core::Vec3 originPosition() const { return origin_position_; }
    core::Quat originRotation() const { return origin_rotation_; }
    ecs::Entity originEntity() const { return origin_entity_; }

private:
    bool active_ = false;
    XrSystem* xr_ = nullptr;
    core::Vec3 origin_position_{};
    core::Quat origin_rotation_{};
    float near_plane_ = 0.05f;
    float far_plane_ = 2000.0f;
    ecs::Entity origin_entity_;
};

}  // namespace cramion::xr

#endif  // CRAMION_CORE_XR_RIG_H
