#include "CramionCore/xr/XrRig.h"

#include "CramionCore/ecs/Components.h"
#include "CramionCore/ecs/MathUtil.h"
#include "CramionCore/ecs/Reflection.h"

#include <CramionFX/scene/Scene.h>
#include <CramionFX/vk/VulkanRenderer.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace cramion::xr {

using core::Quat;
using core::Vec3;

void XrOrigin::reflect(ecs::PropertyVisitor& v) {
    static constexpr std::array<const char*, 2> kOrigins = {"De pie", "Sentado"};
    ecs::enumField(v, {"tracking", "Origen del seguimiento",
                       "De pie: esta entidad es el suelo de la habitacion (room scale). Sentado: la cabeza empieza aqui (mas la altura)"},
                   tracking, kOrigins);
    if (v.wantsAllFields() || tracking == TrackingOrigin::Eyes) {
        v.field({"camera_y_offset", "Altura de la cabeza", "Sentado: metros de la cabeza sobre el origen"}, camera_y_offset,
                ecs::FloatRange{0.0f, 3.0f, 0.01f, "%.2f m"});
    }
    v.field({"drive_camera", "Mover la camara con el casco", "La camara principal sigue a la cabeza"}, drive_camera);
}

void XrController::reflect(ecs::PropertyVisitor& v) {
    static constexpr std::array<const char*, 2> kHands = {"Izquierda", "Derecha"};
    static constexpr std::array<const char*, 2> kPoses = {"Mano (grip)", "Puntero (aim)"};
    ecs::enumField(v, {"hand", "Mano"}, hand, kHands);
    ecs::enumField(v, {"pose", "Pose", "Mano: donde se agarra el mando. Puntero: hacia donde apunta (rayos)"}, pose, kPoses);
    v.field({"hide_when_untracked", "Ocultar sin mando", "Se desactiva si el mando no se ve o esta apagado"},
            hide_when_untracked);
}

void registerXrComponents() {
    ecs::ComponentRegistry& registry = ecs::ComponentRegistry::instance();
    if (registry.find("XrOrigin") == nullptr) {
        registry.registerComponent<XrOrigin>("XrOrigin", "XR Origin (VR)", "Realidad virtual");
        registry.registerComponent<XrController>("XrController", "XR Controller (mando VR)", "Realidad virtual");
    }
}

namespace {

Quat yawOnly(const Vec3& forward) {
    const float yaw = std::atan2(-forward.x, -forward.z);
    return Quat{0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f)};
}

core::Mat4 poseWorld(const Pose& p) { return core::composeTrs(p.position, p.orientation, Vec3{1.0f, 1.0f, 1.0f}); }

// La camara del juego: como RenderSync::syncCamera.
ecs::Entity mainCamera(ecs::World& world) {
    ecs::Entity main;
    world.forEachDepthFirst([&](ecs::Entity e) {
        const ecs::Camera* camera = e.tryGet<ecs::Camera>();
        if (camera == nullptr || camera->target_texture.valid() || !e.activeInHierarchy()) return;
        if (!main.valid() || (camera->is_main && !main.get<ecs::Camera>().is_main)) main = e;
    });
    return main;
}

}  // namespace

void XrRig::reset() {
    active_ = false;
    origin_entity_ = {};
}

Pose XrRig::toWorld(const Pose& tracked) const {
    Pose out;
    out.position = origin_position_ + rotate(origin_rotation_, tracked.position);
    out.orientation = core::normalize(multiply(origin_rotation_, tracked.orientation));
    out.valid = tracked.valid;
    return out;
}

void XrRig::update(ecs::World& world, XrSystem& xr) {
    xr_ = &xr;
    active_ = xr.running();
    if (!active_) return;

    // Origen: el XR Origin activo, o la camara principal.
    origin_entity_ = {};
    const XrOrigin* origin = nullptr;
    world.forEachDepthFirst([&](ecs::Entity e) {
        if (origin != nullptr || !e.activeInHierarchy()) return;
        if (const XrOrigin* o = e.tryGet<XrOrigin>()) {
            origin = o;
            origin_entity_ = e;
        }
    });
    ecs::Entity camera = mainCamera(world);
    if (const ecs::Camera* c = camera.valid() ? camera.tryGet<ecs::Camera>() : nullptr) {
        near_plane_ = std::min(c->near_plane, 0.05f);  // de cerca se ven las manos
        far_plane_ = c->far_plane;
    }
    if (origin != nullptr) {
        xr.setTrackingOrigin(origin->tracking);
        Vec3 position, scale;
        Quat rotation;
        ecs::decomposeMatrix(origin_entity_.worldMatrix(), position, rotation, scale);
        origin_rotation_ = core::normalize(rotation);
        origin_position_ = position;
        if (origin->tracking == TrackingOrigin::Eyes) {
            origin_position_ = origin_position_ + rotate(origin_rotation_, Vec3{0.0f, origin->camera_y_offset, 0.0f});
        }
        if (origin->drive_camera && camera.valid()) {
            const Pose head = toWorld(xr.head());
            if (xr.head().valid) camera.setWorldMatrix(poseWorld(head));
        }
    } else {
        // Sin rig: la cabeza empieza donde esta la camara (sin cabeceo: el
        // arriba/abajo lo pone el casco).
        xr.setTrackingOrigin(TrackingOrigin::Eyes);
        if (camera.valid()) {
            origin_position_ = camera.worldPosition();
            origin_rotation_ = yawOnly(camera.forward());
        } else {
            origin_position_ = Vec3{0.0f, 1.6f, 0.0f};
            origin_rotation_ = Quat{};
        }
    }

    // Mandos.
    world.forEachDepthFirst([&](ecs::Entity e) {
        const XrController* c = e.tryGet<XrController>();
        if (c == nullptr) return;
        const Controller& state = xr.controller(c->hand);
        const Pose& tracked = c->pose == ControllerPose::Aim ? state.aim : state.grip;
        const bool tracking = state.active && tracked.valid;
        if (c->hide_when_untracked && e.activeInHierarchy() != tracking) {
            // Solo si el padre esta activo (si no, no es cosa del mando).
            const ecs::Entity parent = e.parent();
            if (!parent.valid() || parent.activeInHierarchy()) e.setActive(tracking);
        }
        if (tracking) e.setWorldMatrix(poseWorld(toWorld(tracked)));
    });
}

void XrRig::renderEyes(gfx::VulkanRenderer& renderer, scene::Scene& scene) {
    if (!active_ || xr_ == nullptr || !xr_->shouldRender()) return;
    for (int i = 0; i < 2; ++i) {
        const EyeView& view = xr_->eye(i);
        if (!view.pose.valid) {
            renderer.clearXrEye(i);
            continue;
        }
        const Pose w = toWorld(view.pose);
        scene::Camera eye = scene.camera();
        eye.setPosition(w.position);
        eye.setOrientation(rotate(w.orientation, Vec3{0.0f, 0.0f, -1.0f}), rotate(w.orientation, Vec3{0.0f, 1.0f, 0.0f}));
        eye.setFovAngles(view.fov.left, view.fov.right, view.fov.up, view.fov.down);
        eye.setClipPlanes(near_plane_, far_plane_);
        renderer.renderXrEye(scene, eye, i);
    }
    // La ventana (espejo) ve lo que ve la cabeza.
    if (xr_->head().valid) {
        const Pose head = toWorld(xr_->head());
        scene.camera().setPosition(head.position);
        scene.camera().setOrientation(rotate(head.orientation, Vec3{0.0f, 0.0f, -1.0f}),
                                      rotate(head.orientation, Vec3{0.0f, 1.0f, 0.0f}));
    }
}

}  // namespace cramion::xr
