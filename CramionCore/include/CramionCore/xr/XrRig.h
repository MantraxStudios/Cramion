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
// Jugador en primera persona con fisica: XR Origin + CharacterController
// (Integrado) + XrPlayer en la misma entidad (GameObject > Realidad virtual >
// Jugador VR). El stick izquierdo anda, el derecho gira, A salta; andar por la
// habitacion mueve la capsula (chocando) y la capsula sigue a la cabeza. De
// pie o sentado segun el XrOrigin.
//
// Interaccion (como el XR Interaction Toolkit de Unity): XR Interactor en
// cada mano agarra (boton de agarre) los objetos con XR Grabbable que toca o
// a los que apunta su rayo, y con el rayo usa la UI en el mundo (Canvas en
// modo Mundo; el gatillo es el clic).
//
// Sin XR Origin en la escena, la camara principal hace de origen (su
// posicion y su giro horizontal) y el seguimiento empieza en los ojos: la
// cabeza sale donde esta la camara. Asi cualquier escena se ve en VR.
//
// Cada frame, con el casco en marcha:
//   xr.beginFrame(); rig.update(world, xr);     antes de la logica (poses)
//   ... fisica, scripts, RenderSync ...
//   rig.renderEyes(renderer, scene); xr.endFrame();

#include "CramionCore/asset/AssetTypes.h"
#include "CramionCore/ecs/World.h"
#include "CramionCore/ui/UI.h"

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
namespace cramion::physics {
class PhysicsSystem;
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

enum class XrTurn : std::uint8_t { Snap = 0, Smooth = 1 };
enum class XrMoveDirection : std::uint8_t { Head = 0, LeftHand = 1 };

// Controlador en primera persona con fisica (va con XrOrigin y un
// CharacterController en la misma entidad). La velocidad, el salto, la
// gravedad y los escalones son los del CharacterController.
struct XrPlayer {
    XrMoveDirection direction = XrMoveDirection::Head;  // hacia donde anda el stick
    XrTurn turn = XrTurn::Snap;
    float snap_angle = 45.0f;    // grados por paso
    float turn_speed = 120.0f;   // grados/s (giro suave)
    float dead_zone = 0.15f;
    bool run_with_stick = true;  // pulsar el stick izquierdo: correr
    bool jump_button = true;     // A (mando derecho): saltar
    // Andar por la habitacion choca con las paredes (la camara no las atraviesa).
    bool room_scale_collisions = true;
    // De pie: agachado si la cabeza baja de esta fraccion de la altura de la
    // capsula (0 = nunca). Sentado: stick derecho abajo.
    float crouch_ratio = 0.65f;
    // Los XR Grabbable chocan con la capsula del jugador (empujan o se
    // empujan). Sin esto la atraviesan: lo que se coge no golpea el cuerpo.
    bool collide_with_grabbables = false;

    void reflect(ecs::PropertyVisitor& v);
};

enum class XrGrabMovement : std::uint8_t { Velocity = 0, Instant = 1 };

// Objeto que se agarra con las manos (como XR Grab Interactable de Unity): con
// Rigidbody y un collider. Mientras se sostiene sigue a la mano (Velocidad: por
// la fisica, choca con lo demas; Instantaneo: pegado a ella) y al soltarlo
// sale con la velocidad de la mano.
struct XrGrabbable {
    XrGrabMovement movement = XrGrabMovement::Velocity;
    // A la mano por su punto de agarre (herramientas, pistolas); si no, se
    // coge por donde se toca.
    bool snap_to_hand = false;
    core::Vec3 attach_position{};  // respecto a la mano (m)
    core::Vec3 attach_rotation{};  // grados
    bool distance_grab = true;     // tambien con el rayo (viene a la mano)
    float throw_scale = 1.0f;      // velocidad al soltarlo

    void reflect(ecs::PropertyVisitor& v);
};

// La interaccion de un mando (como XR Direct + Ray Interactor de Unity), en la
// entidad con XR Controller: el boton de agarre coge lo que toca la mano o lo
// que apunta el rayo; el gatillo usa la UI en el mundo.
struct XrInteractor {
    bool direct = true;          // coger lo que toca la mano
    float grab_radius = 0.08f;   // m alrededor de la mano
    bool ray = true;             // rayo: UI en el mundo y coger a distancia
    float ray_length = 8.0f;     // m
    bool show_ray = true;        // el laser
    assets::AssetRef ray_material{{}, assets::AssetType::Material};
    bool haptics = true;         // vibra al coger y al pulsar la UI

    void reflect(ecs::PropertyVisitor& v);
};

void registerXrComponents();

class XrRig {
public:
    // Despues de xr.beginFrame(): elige el origen, lo aplica y mueve la
    // camara principal y las entidades con XrController.
    void update(ecs::World& world, XrSystem& xr);
    // Para el XrPlayer (CharacterController) y XR Grabbable: sin fisica no se
    // mueve ni se agarra nada.
    void setPhysics(physics::PhysicsSystem* physics) { physics_ = physics; }
    // Cada frame de Play, antes de la fisica (con casco o sin el): la capsula
    // de cada XR Player no choca con los XR Grabbable (si no lo pide).
    void updateCollisions(ecs::World& world);
    // Los rayos de las manos con XR Interactor para la UI en el mundo
    // (UiSystem::updateWorld) y donde la cortaron (el largo del laser).
    const std::array<ui::UiPointer, ui::UiSystem::kMaxPointers>& uiPointers() const { return ui_pointers_; }
    void setUiHits(const std::array<ui::UiPointerHit, ui::UiSystem::kMaxPointers>& hits);
    // Lo que sostiene cada mano (vacia si nada).
    ecs::Entity held(ecs::World& world, Hand hand) const;
    // Los ojos con la camara de la escena (planos y efectos): una camara por
    // ojo (por defecto) o, sin estereo, una sola entre los dos.
    // Despues deja la camara de la escena en la cabeza (la ventana
    // hace de espejo).
    // Antes de dibujar vuelve a leer el origen (la fisica ya movio la capsula
    // en este frame).
    void renderEyes(gfx::VulkanRenderer& renderer, scene::Scene& scene);
    void reset();

    bool active() const { return active_; }
    // Del espacio del casco al mundo (con el origen de este frame).
    Pose toWorld(const Pose& tracked) const;
    core::Vec3 originPosition() const { return origin_position_; }
    core::Quat originRotation() const { return origin_rotation_; }
    ecs::Entity originEntity() const { return origin_entity_; }

private:
    void computeOrigin();
    bool headAboveOrigin(const XrOrigin& origin) const;
    void updatePlayer(ecs::World& world, XrSystem& xr, ecs::Entity entity, const XrOrigin& origin, const XrPlayer& player);
    void updateInteractors(ecs::World& world, XrSystem& xr);
    void updateHand(ecs::World& world, XrSystem& xr, const XrInteractor& interactor, int hand);
    void grab(ecs::World& world, XrSystem& xr, int hand, ecs::Entity target, const Pose& grip, bool by_ray,
              bool haptics);
    void release(ecs::World& world, int hand, bool throw_it);
    void follow(ecs::Entity held, const XrGrabbable& grabbable, const Pose& grip, int hand);
    void showLaser(ecs::World& world, const XrInteractor& interactor, int hand, const Pose& aim, float length,
                   bool visible);

    bool active_ = false;
    XrSystem* xr_ = nullptr;
    physics::PhysicsSystem* physics_ = nullptr;
    // XrPlayer: donde estaba la cabeza en la habitacion (espacio del casco,
    // horizontal) la ultima vez; la capsula va debajo de ella.
    bool player_ = false;
    bool room_valid_ = false;
    core::Vec3 room_offset_{};
    bool snap_armed_ = true;
    bool jump_held_ = false;
    double last_time_ = 0.0;
    float frame_dt_ = 0.0f;
    // Cada mano: lo que sostiene (por Uuid: sobrevive a recargar la escena),
    // como (respecto a la mano), sus poses recientes (velocidad al lanzar) y
    // su laser.
    struct HandState {
        Uuid held;
        core::Vec3 held_position{};
        core::Quat held_rotation{};
        bool grip_was_down = false;
        bool trigger_was_down = false;
        Uuid laser;
        float ui_distance = -1.0f;  // del ultimo frame (-1: el rayo no corta UI)
        bool ui_over_control = false;
        struct Sample {
            core::Vec3 position{};
            core::Quat rotation{};
            double time = 0.0;
        };
        std::array<Sample, 8> history{};
        int samples = 0;
        int next = 0;
    };
    std::array<HandState, 2> hands_{};
    std::array<ui::UiPointer, ui::UiSystem::kMaxPointers> ui_pointers_{};
    double now_ = 0.0;
    core::Vec3 origin_position_{};
    core::Quat origin_rotation_{};
    float near_plane_ = 0.05f;
    float far_plane_ = 2000.0f;
    ecs::Entity origin_entity_;
};

}  // namespace cramion::xr

#endif  // CRAMION_CORE_XR_RIG_H
