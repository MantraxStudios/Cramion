#include "CramionCore/xr/XrRig.h"

#include "CramionCore/ecs/Components.h"
#include "CramionCore/ecs/MathUtil.h"
#include "CramionCore/ecs/ModelInstantiation.h"
#include "CramionCore/ecs/Reflection.h"
#include "CramionCore/physics/PhysicsComponents.h"
#include "CramionCore/physics/PhysicsSystem.h"

#include <CramionFX/scene/Scene.h>
#include <CramionFX/vk/VulkanRenderer.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <limits>

namespace cramion::xr {

using core::Quat;
using core::Vec3;

void XrOrigin::reflect(ecs::PropertyVisitor& v) {
    static constexpr std::array<const char*, 2> kOrigins = {"De pie", "Sentado"};
    ecs::enumField(v, {"tracking", "Origen del seguimiento",
                       "De pie: esta entidad es el suelo de la habitacion (room scale). Sentado: la cabeza empieza aqui (mas la altura)"},
                   tracking, kOrigins);
    v.field({"camera_y_offset", "Altura de la cabeza",
             "Sentado: metros de la cabeza sobre el origen. De pie solo se usa si el casco no da el suelo"},
            camera_y_offset, ecs::FloatRange{0.0f, 3.0f, 0.01f, "%.2f m"});
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

void XrPlayer::reflect(ecs::PropertyVisitor& v) {
    static constexpr std::array<const char*, 2> kDirections = {"Cabeza", "Mano izquierda"};
    static constexpr std::array<const char*, 2> kTurns = {"Por pasos", "Suave"};
    ecs::enumField(v, {"direction", "Andar hacia", "El stick izquierdo anda hacia donde mira la cabeza o apunta la mano"},
                   direction, kDirections);
    ecs::enumField(v, {"turn", "Girar", "Stick derecho. Por pasos marea menos"}, turn, kTurns);
    if (v.wantsAllFields() || turn == XrTurn::Snap) {
        v.field({"snap_angle", "Grados por paso"}, snap_angle, ecs::FloatRange{10.0f, 90.0f, 1.0f, "%.0f"});
    }
    if (v.wantsAllFields() || turn == XrTurn::Smooth) {
        v.field({"turn_speed", "Velocidad de giro", "Grados por segundo"}, turn_speed,
                ecs::FloatRange{30.0f, 360.0f, 1.0f, "%.0f"});
    }
    v.field({"dead_zone", "Zona muerta de los sticks"}, dead_zone, ecs::FloatRange{0.0f, 0.6f, 0.01f, "%.2f"});
    v.field({"run_with_stick", "Correr pulsando el stick", "Stick izquierdo pulsado: la velocidad de correr"},
            run_with_stick);
    v.field({"jump_button", "Saltar con A", "Boton A del mando derecho (salto del CharacterController)"}, jump_button);
    v.field({"room_scale_collisions", "Chocar al andar en la habitacion",
             "Andar de verdad por la habitacion mueve la capsula chocando: no se atraviesan paredes"},
            room_scale_collisions);
    v.field({"collide_with_grabbables", "Chocar con lo que se coge",
             "Los XR Grabbable chocan con la capsula del jugador. Desactivado: la atraviesan (no golpean el cuerpo al "
             "cogerlos ni al andar)"},
            collide_with_grabbables);
    v.field({"crouch_ratio", "Agacharse (de pie)",
             "De pie: agachado si la cabeza baja de esta fraccion de la altura de la capsula (0 = nunca). "
             "Sentado: stick derecho abajo"},
            crouch_ratio, ecs::FloatRange{0.0f, 0.95f, 0.01f, "%.2f"});
}

void XrGrabbable::reflect(ecs::PropertyVisitor& v) {
    static constexpr std::array<const char*, 2> kMovements = {"Velocidad (fisica)", "Instantaneo"};
    ecs::enumField(v, {"movement", "Movimiento",
                       "Velocidad: sigue a la mano con la fisica (choca con mesas y paredes). Instantaneo: pegado a la mano"},
                   movement, kMovements);
    v.field({"snap_to_hand", "A la mano por su punto de agarre",
             "Herramientas y pistolas: se colocan en la mano igual siempre. Si no, se cogen por donde se tocan"},
            snap_to_hand);
    if (v.wantsAllFields() || snap_to_hand) {
        v.field({"attach_position", "Punto de agarre", "Posicion respecto a la mano (m)"}, attach_position,
                ecs::Vec3Kind::Position);
        v.field({"attach_rotation", "Giro en la mano", "Grados"}, attach_rotation, ecs::Vec3Kind::Euler);
    }
    v.field({"distance_grab", "Coger a distancia", "Con el rayo de la mano: viene a la mano"}, distance_grab);
    v.field({"throw_scale", "Fuerza al lanzar", "Velocidad al soltarlo (1 = la de la mano)"}, throw_scale,
            ecs::FloatRange{0.0f, 4.0f, 0.05f, "%.2f"});
}

void XrInteractor::reflect(ecs::PropertyVisitor& v) {
    v.field({"direct", "Coger con la mano", "Boton de agarre: lo que toca la mano"}, direct);
    if (v.wantsAllFields() || direct) {
        v.field({"grab_radius", "Radio de la mano"}, grab_radius, ecs::FloatRange{0.01f, 0.5f, 0.005f, "%.3f m"});
    }
    v.field({"ray", "Rayo", "UI en el mundo (gatillo = clic) y coger a distancia (agarre)"}, ray);
    if (v.wantsAllFields() || ray) {
        v.field({"ray_length", "Largo del rayo"}, ray_length, ecs::FloatRange{0.5f, 100.0f, 0.1f, "%.1f m"});
        v.field({"show_ray", "Ver el rayo"}, show_ray);
        v.asset({"ray_material", "Material del rayo", "Material (.crmat) del laser (vacio = el gris)"}, ray_material,
                assets::AssetType::Material);
    }
    v.field({"haptics", "Vibrar", "Al coger algo y al pulsar la UI"}, haptics);
}

void registerXrComponents() {
    ecs::ComponentRegistry& registry = ecs::ComponentRegistry::instance();
    if (registry.find("XrOrigin") == nullptr) {
        registry.registerComponent<XrOrigin>("XrOrigin", "XR Origin (VR)", "Realidad virtual");
        registry.registerComponent<XrController>("XrController", "XR Controller (mando VR)", "Realidad virtual");
        registry.registerComponent<XrPlayer>("XrPlayer", "XR Player (primera persona con fisica)", "Realidad virtual");
        registry.registerComponent<XrInteractor>("XrInteractor", "XR Interactor (coger y rayo)", "Realidad virtual");
        registry.registerComponent<XrGrabbable>("XrGrabbable", "XR Grabbable (se coge con las manos)", "Realidad virtual");
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
    player_ = false;
    room_valid_ = false;
    jump_held_ = false;
    snap_armed_ = true;
    last_time_ = 0.0;
    for (HandState& hs : hands_) {
        hs.held = {};
        hs.grip_was_down = hs.trigger_was_down = false;
        hs.samples = 0;
        hs.ui_distance = -1.0f;
    }
    for (ui::UiPointer& p : ui_pointers_) p = ui::UiPointer{};
}

// Sentado, o de pie sin el suelo del casco (LOCAL: el 0 es la cabeza al
// empezar): la cabeza va a camera_y_offset sobre el origen.
bool XrRig::headAboveOrigin(const XrOrigin& origin) const {
    return origin.tracking == TrackingOrigin::Eyes || xr_ == nullptr || !xr_->floorAvailable();
}

// El origen del mundo del casco: la entidad del XR Origin (y, con XrPlayer,
// corrida para que la capsula quede bajo la cabeza).
void XrRig::computeOrigin() {
    const XrOrigin* origin = origin_entity_.valid() ? origin_entity_.tryGet<XrOrigin>() : nullptr;
    if (origin == nullptr) return;
    Vec3 position, scale;
    Quat rotation;
    ecs::decomposeMatrix(origin_entity_.worldMatrix(), position, rotation, scale);
    origin_rotation_ = core::normalize(rotation);
    origin_position_ = position;
    if (headAboveOrigin(*origin)) {
        origin_position_ = origin_position_ + rotate(origin_rotation_, Vec3{0.0f, origin->camera_y_offset, 0.0f});
    }
    if (player_ && room_valid_) origin_position_ = origin_position_ - rotate(origin_rotation_, room_offset_);
}

void XrRig::updatePlayer(ecs::World& world, XrSystem& xr, ecs::Entity entity, const XrOrigin& origin,
                         const XrPlayer& player) {
    physics::CharacterController& cc = entity.get<physics::CharacterController>();
    // Lo mueve el casco, no el teclado ni su propio giro.
    cc.movement = physics::CharacterMovement::Integrated;
    cc.keyboard = false;
    cc.rotate_to_movement = false;

    const float dt = frame_dt_;

    const Pose& head = xr.head();
    if (!head.valid) {
        physics_->setCharacterInput(entity, Vec3{});
        return;
    }
    const auto entityRotation = [&] {
        Vec3 p, s;
        Quat r;
        ecs::decomposeMatrix(entity.worldMatrix(), p, r, s);
        return core::normalize(r);
    };

    // 1) Andar de verdad por la habitacion: la capsula va debajo de la cabeza.
    const Vec3 room{head.position.x, 0.0f, head.position.z};
    if (!room_valid_) {
        room_offset_ = room;
        room_valid_ = true;
    } else {
        const Vec3 d = room - room_offset_;
        room_offset_ = room;
        const float step = core::length(d);
        // Un salto grande (recentrar, se perdio el seguimiento): sin moverse.
        if (step > 1e-5f && step < 0.5f) {
            const Vec3 world_step = rotate(entityRotation(), d);
            if (player.room_scale_collisions) {
                physics_->moveCharacter(world, entity, world_step);
            } else {
                entity.setWorldPosition(entity.worldPosition() + world_step);
            }
        }
    }

    // 2) Girar con el stick derecho (alrededor de la cabeza: esta sobre la capsula).
    const Controller& right = xr.controller(Hand::Right);
    const Controller& left = xr.controller(Hand::Left);
    float yaw = 0.0f;
    const float sx = right.active ? right.thumbstick.x : 0.0f;
    if (player.turn == XrTurn::Snap) {
        if (snap_armed_ && std::abs(sx) > 0.7f) {
            yaw = (sx > 0.0f ? -1.0f : 1.0f) * player.snap_angle * (core::kPi / 180.0f);
            snap_armed_ = false;
        } else if (std::abs(sx) < 0.3f) {
            snap_armed_ = true;
        }
    } else if (std::abs(sx) > player.dead_zone) {
        yaw = -sx * player.turn_speed * (core::kPi / 180.0f) * dt;
    }
    if (yaw != 0.0f) {
        Vec3 p, s;
        Quat r;
        ecs::decomposeMatrix(entity.worldMatrix(), p, r, s);
        const Quat turn{0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f)};
        entity.setWorldMatrix(core::composeTrs(p, core::normalize(multiply(turn, r)), s));
    }

    // 3) Andar con el stick izquierdo (fisica: gravedad, escalones, paredes).
    Vec3 direction{};
    core::Vec2 stick = left.active ? left.thumbstick : core::Vec2{};
    const float amount = std::sqrt(stick.x * stick.x + stick.y * stick.y);
    if (amount > player.dead_zone) {
        const float scaled = std::min((amount - player.dead_zone) / std::max(1.0f - player.dead_zone, 0.01f), 1.0f);
        const Quat basis = player.direction == XrMoveDirection::LeftHand && left.aim.valid ? left.aim.orientation
                                                                                         : head.orientation;
        Vec3 forward = rotate(multiply(entityRotation(), basis), Vec3{0.0f, 0.0f, -1.0f});
        forward.y = 0.0f;
        if (core::length(forward) > 1e-4f) {
            forward = core::normalize(forward);
            const Vec3 side = core::cross(forward, Vec3{0.0f, 1.0f, 0.0f});
            direction = (side * stick.x + forward * stick.y) * (scaled / amount);
        }
    }
    const bool run = player.run_with_stick && left.buttons[static_cast<std::size_t>(Button::Thumbstick)];
    physics_->setCharacterInput(entity, direction, run);

    // 4) Saltar (A) y agacharse (de pie: con el cuerpo; sentado: stick abajo).
    const bool jump = player.jump_button && right.buttons[static_cast<std::size_t>(Button::Primary)];
    if (jump && !jump_held_) physics_->characterJump(entity);
    jump_held_ = jump;
    bool crouch = false;
    if (origin.tracking == TrackingOrigin::Floor && xr.floorAvailable()) {
        crouch = player.crouch_ratio > 0.0f && head.position.y < cc.height * player.crouch_ratio;
    } else {
        crouch = right.active && right.thumbstick.y < -0.7f;
    }
    physics_->setCharacterCrouch(entity, crouch);
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
    now_ = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    frame_dt_ = last_time_ > 0.0 ? std::clamp(static_cast<float>(now_ - last_time_), 0.0f, 0.1f) : 0.0f;
    last_time_ = now_;
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
        // Jugador con fisica: la capsula se mueve antes de la logica y la
        // fisica de este frame.
        const XrPlayer* player = origin_entity_.tryGet<XrPlayer>();
        const bool can_move = player != nullptr && physics_ != nullptr &&
                              origin_entity_.tryGet<physics::CharacterController>() != nullptr;
        if (can_move != player_) room_valid_ = false;
        player_ = can_move;
        if (player_) updatePlayer(world, xr, origin_entity_, *origin, *player);
        computeOrigin();
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

    updateInteractors(world, xr);
}

// --- Interaccion (XR Interactor y XR Grabbable) ---

namespace {

// La entidad con XR Grabbable de la que forma parte `e` (ella o un padre).
ecs::Entity grabbableOf(ecs::Entity e) {
    for (; e.valid(); e = e.parent()) {
        if (e.has<XrGrabbable>()) return e;
    }
    return {};
}

}  // namespace

void XrRig::updateCollisions(ecs::World& world) {
    if (physics_ == nullptr) return;
    entt::registry& registry = world.registry();
    const auto players = registry.view<XrPlayer, physics::CharacterController>();
    if (players.begin() == players.end()) return;
    for (const entt::entity g : registry.view<XrGrabbable>()) {
        // El cuerpo es el de la entidad con el Rigidbody (ella o un padre).
        ecs::Entity body = world.wrap(g);
        while (body.valid() && !body.has<physics::Rigidbody>()) body = body.parent();
        if (!body.valid()) body = world.wrap(g);
        for (const entt::entity p : players) {
            physics_->ignoreCollision(world.wrap(p), body, !players.get<XrPlayer>(p).collide_with_grabbables);
        }
    }
}

void XrRig::setUiHits(const std::array<ui::UiPointerHit, ui::UiSystem::kMaxPointers>& hits) {
    for (std::size_t h = 0; h < hands_.size() && h < hits.size(); ++h) {
        hands_[h].ui_distance = hits[h].hit ? hits[h].distance : -1.0f;
        hands_[h].ui_over_control = hits[h].hit && hits[h].over_control;
    }
}

ecs::Entity XrRig::held(ecs::World& world, Hand hand) const {
    const Uuid& id = hands_[static_cast<std::size_t>(hand)].held;
    return id.valid() ? world.find(id) : ecs::Entity{};
}

void XrRig::updateInteractors(ecs::World& world, XrSystem& xr) {
    for (ui::UiPointer& p : ui_pointers_) p = ui::UiPointer{};
    std::array<bool, 2> seen{};
    world.forEachDepthFirst([&](ecs::Entity e) {
        const XrInteractor* interactor = e.tryGet<XrInteractor>();
        const XrController* controller = e.tryGet<XrController>();
        if (interactor == nullptr || controller == nullptr || !e.activeInHierarchy()) return;
        const auto h = static_cast<std::size_t>(controller->hand);
        if (h >= seen.size() || seen[h]) return;
        seen[h] = true;
        updateHand(world, xr, *interactor, static_cast<int>(h));
    });
    // Manos sin interactor (o con el mando apagado): sueltan y sin laser.
    for (std::size_t h = 0; h < seen.size(); ++h) {
        if (seen[h]) continue;
        release(world, static_cast<int>(h), false);
        if (ecs::Entity laser = hands_[h].laser.valid() ? world.find(hands_[h].laser) : ecs::Entity{}; laser.valid()) {
            laser.setActive(false);
        }
    }
}

void XrRig::updateHand(ecs::World& world, XrSystem& xr, const XrInteractor& interactor, int hand) {
    HandState& hs = hands_[static_cast<std::size_t>(hand)];
    const Controller& c = xr.controller(static_cast<Hand>(hand));
    if (!c.active || !c.grip.valid) {
        release(world, hand, false);
        showLaser(world, interactor, hand, Pose{}, 0.0f, false);
        return;
    }
    const Pose grip = toWorld(c.grip);
    const Pose aim = c.aim.valid ? toWorld(c.aim) : grip;
    const Vec3 aim_dir = core::normalize(rotate(aim.orientation, Vec3{0.0f, 0.0f, -1.0f}));
    // Poses recientes de la mano (la velocidad al lanzar).
    hs.history[static_cast<std::size_t>(hs.next)] = HandState::Sample{grip.position, grip.orientation, now_};
    hs.next = (hs.next + 1) % static_cast<int>(hs.history.size());
    hs.samples = std::min(hs.samples + 1, static_cast<int>(hs.history.size()));

    const bool grip_down = c.buttons[static_cast<std::size_t>(Button::Grip)];
    const bool trigger_down = c.buttons[static_cast<std::size_t>(Button::Trigger)];
    ecs::Entity held_entity = hs.held.valid() ? world.find(hs.held) : ecs::Entity{};
    if (hs.held.valid() && (!held_entity.valid() || !held_entity.activeInHierarchy() || !held_entity.has<XrGrabbable>())) {
        release(world, hand, false);
        held_entity = {};
    }

    // Lo que corta el rayo (fisica): coger a distancia y el largo del laser.
    float ray_hit = -1.0f;
    ecs::Entity ray_target;
    if (interactor.ray && physics_ != nullptr && !held_entity.valid()) {
        physics::RaycastHit hit;
        physics::QueryFilter filter;
        filter.ignore = origin_entity_;  // la capsula del jugador
        filter.record = false;
        if (physics_->raycast(aim.position, aim_dir, interactor.ray_length, hit, filter)) {
            ray_hit = hit.distance;
            const ecs::Entity g = grabbableOf(hit.entity);
            // La UI va antes si esta delante.
            if (g.valid() && g.get<XrGrabbable>().distance_grab && (hs.ui_distance < 0.0f || hit.distance < hs.ui_distance)) {
                ray_target = g;
            }
        }
    }

    if (held_entity.valid()) {
        if (!grip_down) {
            release(world, hand, true);
        } else {
            follow(held_entity, held_entity.get<XrGrabbable>(), grip, hand);
        }
    } else if (grip_down && !hs.grip_was_down && physics_ != nullptr) {
        // Lo que toca la mano; si nada, lo que apunta el rayo.
        ecs::Entity target;
        if (interactor.direct) {
            physics::QueryFilter filter;
            filter.ignore = origin_entity_;
            filter.record = false;
            float best = std::numeric_limits<float>::max();
            for (const ecs::Entity& e : physics_->overlapSphere(grip.position, interactor.grab_radius, filter)) {
                const ecs::Entity g = grabbableOf(e);
                if (!g.valid()) continue;
                const float d = core::length(g.worldPosition() - grip.position);
                if (d < best) {
                    best = d;
                    target = g;
                }
            }
        }
        const bool by_ray = !target.valid() && ray_target.valid();
        if (by_ray) target = ray_target;
        if (target.valid()) grab(world, xr, hand, target, grip, by_ray, interactor.haptics);
    }
    hs.grip_was_down = grip_down;

    const bool holding = hs.held.valid();
    // El rayo para la UI en el mundo (UiSystem::updateWorld); la vibracion al
    // pulsar un control.
    ui_pointers_[static_cast<std::size_t>(hand)] =
        ui::UiPointer{interactor.ray && !holding, aim.position, aim_dir, trigger_down};
    if (interactor.haptics && trigger_down && !hs.trigger_was_down && hs.ui_over_control && !holding) {
        xr.vibrate(static_cast<Hand>(hand), 0.25f, 0.02f);
    }
    hs.trigger_was_down = trigger_down;

    // El laser: hasta lo primero que corte (UI o un objeto).
    float length = interactor.ray_length;
    if (hs.ui_distance >= 0.0f) length = std::min(length, hs.ui_distance);
    if (ray_hit >= 0.0f) length = std::min(length, ray_hit);
    showLaser(world, interactor, hand, aim, length, interactor.ray && interactor.show_ray && !holding);
}

void XrRig::grab(ecs::World& world, XrSystem& xr, int hand, ecs::Entity target, const Pose& grip, bool by_ray,
                 bool haptics) {
    // Si la otra mano lo tenia, pasa a esta.
    for (std::size_t other = 0; other < hands_.size(); ++other) {
        if (static_cast<int>(other) != hand && hands_[other].held == target.uuid()) {
            release(world, static_cast<int>(other), false);
        }
    }
    HandState& hs = hands_[static_cast<std::size_t>(hand)];
    const XrGrabbable& g = target.get<XrGrabbable>();
    Vec3 position, scale;
    Quat rotation;
    ecs::decomposeMatrix(target.worldMatrix(), position, rotation, scale);
    const Quat inverse_grip = conjugate(grip.orientation);
    if (g.snap_to_hand) {
        hs.held_position = g.attach_position;
        hs.held_rotation = ecs::quatFromEulerDegrees(g.attach_rotation);
    } else {
        // Como se coge; a distancia viene a la mano con el mismo giro.
        hs.held_position = by_ray ? Vec3{} : rotate(inverse_grip, position - grip.position);
        hs.held_rotation = core::normalize(multiply(inverse_grip, core::normalize(rotation)));
    }
    hs.held = target.uuid();
    if (physics_ != nullptr) physics_->wakeUp(target);
    if (haptics) xr.vibrate(static_cast<Hand>(hand), 0.5f, 0.05f);
}

void XrRig::follow(ecs::Entity held, const XrGrabbable& grabbable, const Pose& grip, int hand) {
    const HandState& hs = hands_[static_cast<std::size_t>(hand)];
    const Vec3 target_position = grip.position + rotate(grip.orientation, hs.held_position);
    const Quat target_rotation = core::normalize(multiply(grip.orientation, hs.held_rotation));
    Vec3 position, scale;
    Quat rotation;
    ecs::decomposeMatrix(held.worldMatrix(), position, rotation, scale);
    if (grabbable.movement == XrGrabMovement::Instant || physics_ == nullptr) {
        held.setWorldMatrix(core::composeTrs(target_position, target_rotation, scale));
        if (physics_ != nullptr) {
            physics_->setLinearVelocity(held, Vec3{});
            physics_->setAngularVelocity(held, Vec3{});
        }
        return;
    }
    // Velocidad: llegar a la mano en este frame (la fisica lo mueve chocando).
    const float dt = std::max(frame_dt_, 1.0f / 240.0f);
    Vec3 velocity = (target_position - position) * (1.0f / dt);
    const float speed = core::length(velocity);
    if (speed > 30.0f) velocity = velocity * (30.0f / speed);
    // La gravedad de un paso, ya descontada (si no, cuelga un poco).
    const physics::PhysicsSettings& settings = physics_->settings();
    velocity = velocity - settings.gravity * settings.fixed_step;
    Quat delta = core::normalize(multiply(target_rotation, conjugate(core::normalize(rotation))));
    if (delta.w < 0.0f) delta = Quat{-delta.x, -delta.y, -delta.z, -delta.w};
    const float half = std::acos(std::clamp(delta.w, -1.0f, 1.0f));
    const float s = std::sin(half);
    Vec3 angular{};
    if (s > 1e-5f) {
        angular = Vec3{delta.x / s, delta.y / s, delta.z / s} * (2.0f * half / dt);
        const float w = core::length(angular);
        if (w > 50.0f) angular = angular * (50.0f / w);
    }
    physics_->setLinearVelocity(held, velocity);
    physics_->setAngularVelocity(held, angular);
    physics_->wakeUp(held);
}

void XrRig::release(ecs::World& world, int hand, bool throw_it) {
    HandState& hs = hands_[static_cast<std::size_t>(hand)];
    if (!hs.held.valid()) return;
    const ecs::Entity held_entity = world.find(hs.held);
    hs.held = {};
    if (!throw_it || !held_entity.valid() || physics_ == nullptr || hs.samples < 2) return;
    // Velocidad de la mano en los ultimos ~60 ms (la mas antigua de esas).
    const int count = static_cast<int>(hs.history.size());
    const HandState::Sample& last = hs.history[static_cast<std::size_t>((hs.next - 1 + count) % count)];
    const HandState::Sample* first = &last;
    for (int i = 2; i <= hs.samples; ++i) {
        const HandState::Sample& s = hs.history[static_cast<std::size_t>((hs.next - i + count) % count)];
        first = &s;
        if (last.time - s.time >= 0.06) break;
    }
    const float span = static_cast<float>(last.time - first->time);
    if (span < 1e-4f) return;
    const Vec3 velocity = (last.position - first->position) * (1.0f / span);
    Quat delta = core::normalize(multiply(last.rotation, conjugate(first->rotation)));
    if (delta.w < 0.0f) delta = Quat{-delta.x, -delta.y, -delta.z, -delta.w};
    const float half = std::acos(std::clamp(delta.w, -1.0f, 1.0f));
    const float s = std::sin(half);
    const Vec3 angular = s > 1e-5f ? Vec3{delta.x / s, delta.y / s, delta.z / s} * (2.0f * half / span) : Vec3{};
    const float scale = held_entity.has<XrGrabbable>() ? held_entity.get<XrGrabbable>().throw_scale : 1.0f;
    physics_->setLinearVelocity(held_entity, velocity * scale);
    physics_->setAngularVelocity(held_entity, angular);
    physics_->wakeUp(held_entity);
}

// El laser de la mano: un cubo estirado por el rayo (sin collider ni sombra),
// creado la primera vez.
void XrRig::showLaser(ecs::World& world, const XrInteractor& interactor, int hand, const Pose& aim, float length,
                      bool visible) {
    HandState& hs = hands_[static_cast<std::size_t>(hand)];
    ecs::Entity laser = hs.laser.valid() ? world.find(hs.laser) : ecs::Entity{};
    if (!visible || length <= 0.0f) {
        if (laser.valid()) laser.setActive(false);
        return;
    }
    if (!laser.valid()) {
        laser = ecs::createPrimitive(world, assets::builtin::kCube,
                                     hand == 0 ? "Rayo XR (mano izquierda)" : "Rayo XR (mano derecha)");
        hs.laser = laser.uuid();
    }
    laser.setActive(true);
    ecs::MeshRenderer& renderer = laser.get<ecs::MeshRenderer>();
    renderer.cast_shadows = ecs::ShadowCasting::Off;
    if (interactor.ray_material.valid()) {
        if (renderer.materials.empty() || !(renderer.materials.front() == interactor.ray_material)) {
            renderer.materials = {interactor.ray_material};
        }
    } else if (!renderer.materials.empty()) {
        renderer.materials.clear();
    }
    const Vec3 dir = rotate(aim.orientation, Vec3{0.0f, 0.0f, -1.0f});
    laser.setWorldMatrix(core::composeTrs(aim.position + dir * (length * 0.5f), aim.orientation,
                                          Vec3{0.004f, 0.004f, length}));
}

void XrRig::renderEyes(gfx::VulkanRenderer& renderer, scene::Scene& scene) {
    if (!active_ || xr_ == nullptr || !xr_->shouldRender()) return;
    computeOrigin();  // donde la dejo la fisica este frame
    const auto eyeCamera = [&](const EyeView& view) {
        const Pose w = toWorld(view.pose);
        scene::Camera eye = scene.camera();
        eye.setPosition(w.position);
        eye.setOrientation(rotate(w.orientation, Vec3{0.0f, 0.0f, -1.0f}), rotate(w.orientation, Vec3{0.0f, 1.0f, 0.0f}));
        eye.setFovAngles(view.fov.left, view.fov.right, view.fov.up, view.fov.down);
        eye.setClipPlanes(near_plane_, far_plane_);
        return eye;
    };
    const EyeView center = xr_->centerView();
    if (!center.pose.valid) {
        renderer.clearXrEye(0);
        renderer.clearXrEye(1);
    } else if (xr_->stereo()) {
        // Una camara por ojo (como Unreal): cada ojo desde su sitio y con su
        // campo de vision; las sombras, de la camara que cubre los dos.
        const std::array<scene::Camera, 2> eyes = {eyeCamera(xr_->eye(0)), eyeCamera(xr_->eye(1))};
        renderer.renderXrStereo(scene, eyes, eyeCamera(center));
    } else {
        // Una sola imagen para los dos ojos (mitad de coste, sin profundidad).
        renderer.renderXrMono(scene, eyeCamera(center));
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
