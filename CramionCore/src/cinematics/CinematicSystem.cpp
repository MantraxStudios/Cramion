// CinematicSystem: carros sobre rieles, camaras virtuales (cuerpo, apuntado,
// amortiguacion, ruido), secuencias y el Camera Brain que mezcla la camara
// virtual activa en la camara real.

#include "CramionCore/cinematics/Cinematics.h"

#include "CramionCore/ecs/MathUtil.h"

#include <algorithm>
#include <cmath>

namespace cramion::cinema {

using core::Quat;
using core::Vec3;

namespace {

constexpr float kDegToRad = core::kPi / 180.0f;

// Rotacion que mira hacia `forward` (el forward del motor es -Z local).
Quat lookRotation(const Vec3& forward, const Vec3& world_up = Vec3{0.0f, 1.0f, 0.0f}) {
    const float length = core::length(forward);
    if (length < 1e-6f) return Quat{};
    const Vec3 f = forward * (1.0f / length);
    Vec3 right = core::cross(f, world_up);
    if (core::length(right) < 1e-5f) right = core::cross(f, Vec3{0.0f, 0.0f, 1.0f});
    right = core::normalize(right);
    const Vec3 up = core::cross(right, f);
    core::Mat4 m = core::Mat4::identity();
    m.m[0][0] = right.x; m.m[0][1] = right.y; m.m[0][2] = right.z;
    m.m[1][0] = up.x;    m.m[1][1] = up.y;    m.m[1][2] = up.z;
    m.m[2][0] = -f.x;    m.m[2][1] = -f.y;    m.m[2][2] = -f.z;
    return core::normalize(ecs::quatFromRotationMatrix(m));
}

Quat axisAngle(const Vec3& axis, float radians) {
    const float s = std::sin(radians * 0.5f);
    return Quat{axis.x * s, axis.y * s, axis.z * s, std::cos(radians * 0.5f)};
}

// Fraccion que avanza una amortiguacion de `seconds` en `dt` (llega al 99 %
// en `seconds`). 0 segundos = rigido.
float damp(float seconds, float dt) {
    if (seconds <= 1e-4f) return 1.0f;
    return 1.0f - std::exp(-4.6f * dt / seconds);
}

// Ruido suave (suma de senos desfasados): sin saltos, sin repetirse pronto.
float smoothNoise(float t, float seed) {
    return 0.55f * std::sin(t * 6.2831f + seed) + 0.3f * std::sin(t * 2.3f * 6.2831f + seed * 1.7f) +
           0.15f * std::sin(t * 5.1f * 6.2831f + seed * 3.1f);
}

void setPose(ecs::Entity entity, const Vec3& position, const Quat& rotation) {
    Vec3 p{};
    Quat q{};
    Vec3 scale{};
    ecs::decomposeMatrix(entity.worldMatrix(), p, q, scale);
    entity.setWorldMatrix(core::composeTrs(position, rotation, scale));
}

Quat worldRotation(ecs::Entity entity) {
    Vec3 p{};
    Quat q{};
    Vec3 scale{};
    ecs::decomposeMatrix(entity.worldMatrix(), p, q, scale);
    return core::normalize(q);
}

}  // namespace

void CinematicSystem::shiftOrigin(const core::Vec3& offset) {
    for (auto& [handle, state] : vcams_) {
        state.position = state.position - offset;
        state.output.position = state.output.position - offset;
    }
    blend_from_.position = blend_from_.position - offset;
    output_.position = output_.position - offset;
}

void CinematicSystem::reset() {
    vcams_.clear();
    sequences_.clear();
    preview_ = {};
    solo_ = {};
    live_ = {};
    blend_from_camera_ = {};
    brain_ = {};
    has_output_ = false;
    blend_time_ = blend_duration_ = 0.0f;
    live_shot_ = -1;
    live_sequence_ = {};
    cut_ = false;
    clock_ = 0.0f;
}

void CinematicSystem::update(ecs::World& world, float delta_seconds, bool playing) {
    const float dt = std::clamp(delta_seconds, 0.0f, 0.25f);
    ++frame_;
    clock_ += dt;
    cut_ = false;
    updateCarts(world, dt, playing);
    updateSequences(world, dt, playing);

    // Camaras virtuales (despues de los carros: pueden seguir a uno).
    for (const entt::entity handle : world.registry().view<VirtualCamera>()) {
        const ecs::Entity entity = world.wrap(handle);
        if (!entity.activeInHierarchy()) continue;
        updateCamera(world, entity, entity.get<VirtualCamera>(), dt, playing);
    }
    for (auto it = vcams_.begin(); it != vcams_.end();) {
        it = it->second.frame == frame_ ? std::next(it) : vcams_.erase(it);
    }
    updateBrain(world, dt, playing);
}

// -----------------------------------------------------------------------------
// Carros
// -----------------------------------------------------------------------------

void CinematicSystem::updateCarts(ecs::World& world, float dt, bool playing) {
    for (const entt::entity handle : world.registry().view<DollyCart>()) {
        ecs::Entity entity = world.wrap(handle);
        if (!entity.activeInHierarchy()) continue;
        DollyCart& cart = entity.get<DollyCart>();
        const ecs::Entity track_entity = world.find(cart.track);
        const DollyTrack* track = track_entity.valid() ? track_entity.tryGet<DollyTrack>() : nullptr;
        if (track == nullptr) continue;
        const DollyPath path(*track, track_entity.worldMatrix());
        if (!path.valid()) continue;
        if (playing || cart.play_in_editor) {
            cart.position += cart.speed * dt;
            // Normalizar la posicion guardada (en bucle da la vuelta).
            cart.position = path.fromPathUnits(path.toPathUnits(cart.position, cart.units), cart.units);
        }
        const float u = path.toPathUnits(cart.position, cart.units);
        const Quat rotation = cart.orient_to_path ? core::normalize(ecs::quatMultiply(
                                                        lookRotation(path.tangent(u)),
                                                        axisAngle(Vec3{0.0f, 0.0f, 1.0f}, -path.roll(u) * kDegToRad)))
                                                  : worldRotation(entity);
        setPose(entity, path.point(u), rotation);
    }
}

// -----------------------------------------------------------------------------
// Secuencias
// -----------------------------------------------------------------------------

void CinematicSystem::play(ecs::Entity sequence, float from) {
    SequenceState& state = sequences_[sequence.handle()];
    state.time = from;
    state.playing = true;
    state.started = true;
}

void CinematicSystem::stop(ecs::Entity sequence) {
    const auto it = sequences_.find(sequence.handle());
    if (it != sequences_.end()) it->second.playing = false;
}

bool CinematicSystem::isPlaying(ecs::Entity sequence) const {
    if (preview_ == sequence && preview_.valid()) return preview_advance_;
    const auto it = sequences_.find(sequence.handle());
    return it != sequences_.end() && it->second.playing;
}

float CinematicSystem::time(ecs::Entity sequence) const {
    if (preview_ == sequence && preview_.valid()) return preview_time_;
    const auto it = sequences_.find(sequence.handle());
    return it != sequences_.end() ? it->second.time : 0.0f;
}

void CinematicSystem::setPreview(ecs::Entity sequence, float time, bool advance) {
    preview_ = sequence;
    preview_time_ = time;
    preview_advance_ = advance;
}

void CinematicSystem::clearPreview() {
    preview_ = {};
    preview_advance_ = false;
}

void CinematicSystem::updateSequences(ecs::World& world, float dt, bool playing) {
    // Vista previa del editor.
    if (preview_.valid()) {
        const CinematicSequence* seq = world.valid(preview_.handle()) ? preview_.tryGet<CinematicSequence>() : nullptr;
        if (seq == nullptr) {
            clearPreview();
        } else if (preview_advance_) {
            preview_time_ += dt * seq->speed;
            const float total = seq->duration();
            if (preview_time_ >= total) {
                if (seq->loop && total > 0.0f) preview_time_ = std::fmod(preview_time_, total);
                else {
                    preview_time_ = total;
                    preview_advance_ = false;
                }
            }
        }
    }
    if (!playing) return;

    for (const entt::entity handle : world.registry().view<CinematicSequence>()) {
        const ecs::Entity entity = world.wrap(handle);
        if (!entity.activeInHierarchy()) continue;
        const CinematicSequence& seq = entity.get<CinematicSequence>();
        SequenceState& state = sequences_[handle];
        if (!state.started) {
            state.started = true;
            state.playing = seq.play_on_start;
            state.time = 0.0f;
        }
        if (!state.playing) continue;
        state.time += dt * seq.speed;
        const float total = seq.duration();
        if (state.time >= total) {
            if (seq.loop && total > 0.0f) {
                state.time = std::fmod(state.time, total);
            } else {
                state.time = total;
                state.playing = false;
            }
        }
        // Objetos activos solo en su tramo (solo en Play: fuera de Play
        // cambiaria la escena que se guarda).
        for (const CinematicActivation& a : seq.activations) {
            ecs::Entity target = world.find(a.entity);
            if (!target.valid()) continue;
            const bool on = state.playing && state.time >= a.start && state.time < a.end;
            if (target.activeSelf() != on) target.setActive(on);
        }
    }
}

// -----------------------------------------------------------------------------
// Camaras virtuales
// -----------------------------------------------------------------------------

void CinematicSystem::updateCamera(ecs::World& world, ecs::Entity entity, VirtualCamera& vcam, float dt,
                                   bool playing) {
    VcamState& state = vcams_[entity.handle()];
    state.frame = frame_;
    const bool smooth = playing && state.initialized;
    const ecs::Entity follow = world.find(vcam.follow);
    const ecs::Entity look_at = world.find(vcam.look_at);
    const bool has_follow = follow.valid() && follow.activeInHierarchy();
    const bool has_look = look_at.valid() && look_at.activeInHierarchy();

    Vec3 position = entity.worldPosition();
    Quat rotation = worldRotation(entity);
    if (!state.initialized) {
        state.position = position;
        state.rotation = rotation;
        state.orbit = vcam.orbit_angle;
    }

    // --- Cuerpo ---
    bool moved = false;
    Vec3 desired = position;
    switch (vcam.body) {
        case BodyMode::DoNothing: break;
        case BodyMode::Transposer:
            if (has_follow) {
                const Vec3 offset = vcam.binding == BindingMode::TargetLocal
                                        ? ecs::quatRotate(worldRotation(follow), vcam.follow_offset)
                                        : vcam.follow_offset;
                desired = follow.worldPosition() + offset;
                moved = true;
            }
            break;
        case BodyMode::HardLockToTarget:
            if (has_follow) {
                desired = follow.worldPosition();
                moved = true;
            }
            break;
        case BodyMode::Orbital:
            if (has_follow) {
                state.orbit = playing ? state.orbit + vcam.orbit_speed * dt : vcam.orbit_angle;
                const float a = state.orbit * kDegToRad;
                desired = follow.worldPosition() +
                          Vec3{std::sin(a) * vcam.orbit_radius, vcam.orbit_height, std::cos(a) * vcam.orbit_radius};
                moved = true;
            }
            break;
        case BodyMode::TrackedDolly: {
            const ecs::Entity track_entity = world.find(vcam.track);
            const DollyTrack* track = track_entity.valid() ? track_entity.tryGet<DollyTrack>() : nullptr;
            if (track == nullptr) break;
            const DollyPath path(*track, track_entity.worldMatrix());
            if (!path.valid()) break;
            float target = vcam.auto_dolly && has_follow
                               ? path.closest(follow.worldPosition(),
                                              state.initialized ? state.dolly - vcam.auto_dolly_offset : -1.0f) +
                                     vcam.auto_dolly_offset
                               : path.toPathUnits(vcam.path_position, vcam.path_units);
            target = path.wrap(target);
            if (smooth) {
                float delta = target - state.dolly;
                // En bucle: el camino corto (no dar la vuelta entera).
                if (path.looped()) {
                    const float total = path.segments();
                    if (delta > total * 0.5f) delta -= total;
                    if (delta < -total * 0.5f) delta += total;
                }
                state.dolly = path.wrap(state.dolly + delta * damp(vcam.dolly_damping, dt));
            } else {
                state.dolly = target;
            }
            desired = path.point(state.dolly);
            // Sin objetivo al que mirar, la camara mira por el riel.
            if (vcam.aim == AimMode::DoNothing) {
                rotation = core::normalize(ecs::quatMultiply(lookRotation(path.tangent(state.dolly)),
                                                             axisAngle(Vec3{0.0f, 0.0f, 1.0f},
                                                                       -path.roll(state.dolly) * kDegToRad)));
            }
            state.position = desired;  // la amortiguacion ya va en el riel
            position = desired;
            moved = true;
            break;
        }
    }
    if (moved && vcam.body != BodyMode::TrackedDolly) {
        if (smooth) {
            state.position = Vec3{state.position.x + (desired.x - state.position.x) * damp(vcam.damping.x, dt),
                                  state.position.y + (desired.y - state.position.y) * damp(vcam.damping.y, dt),
                                  state.position.z + (desired.z - state.position.z) * damp(vcam.damping.z, dt)};
        } else {
            state.position = desired;
        }
        position = state.position;
    } else if (!moved) {
        state.position = position;
    }

    // --- Apuntar ---
    bool aimed = false;
    switch (vcam.aim) {
        case AimMode::DoNothing: break;
        case AimMode::Composer:
        case AimMode::HardLookAt:
            if (has_look) {
                const Quat target = lookRotation(look_at.worldPosition() + vcam.look_offset - position);
                rotation = vcam.aim == AimMode::Composer && smooth
                               ? core::slerp(state.rotation, target, damp(vcam.aim_damping, dt))
                               : target;
                aimed = true;
            }
            break;
        case AimMode::SameAsFollowTarget:
            if (has_follow) {
                rotation = worldRotation(follow);
                aimed = true;
            }
            break;
    }
    if (aimed && std::abs(vcam.dutch) > 1e-3f) {
        rotation = core::normalize(ecs::quatMultiply(rotation, axisAngle(Vec3{0.0f, 0.0f, 1.0f}, -vcam.dutch * kDegToRad)));
    }
    state.rotation = rotation;
    state.initialized = true;

    // La camara virtual se mueve de verdad (como en Cinemachine): su
    // Transform es su pose. Sin cuerpo ni apuntado, manda su Transform.
    if (moved || aimed || vcam.body == BodyMode::TrackedDolly) {
        setPose(entity, position, rotation);
    }

    // Salida: con el dutch de las que no apuntan y el ruido (solo en la
    // salida: no se acumula en el Transform).
    Quat output = rotation;
    if (!aimed && std::abs(vcam.dutch) > 1e-3f) {
        output = core::normalize(ecs::quatMultiply(output, axisAngle(Vec3{0.0f, 0.0f, 1.0f}, -vcam.dutch * kDegToRad)));
    }
    if (vcam.noise_amplitude > 0.0f && vcam.noise_frequency > 0.0f) {
        const float t = clock_ * vcam.noise_frequency;
        const float seed = static_cast<float>(entt::to_integral(entity.handle()) % 97u);
        const float yaw = smoothNoise(t, seed) * vcam.noise_amplitude * kDegToRad;
        const float pitch = smoothNoise(t * 1.13f, seed + 11.0f) * vcam.noise_amplitude * 0.7f * kDegToRad;
        const float roll = smoothNoise(t * 0.71f, seed + 23.0f) * vcam.noise_amplitude * 0.35f * kDegToRad;
        output = core::normalize(ecs::quatMultiply(
            output, ecs::quatMultiply(axisAngle(Vec3{0.0f, 1.0f, 0.0f}, yaw),
                                      ecs::quatMultiply(axisAngle(Vec3{1.0f, 0.0f, 0.0f}, pitch),
                                                        axisAngle(Vec3{0.0f, 0.0f, 1.0f}, roll)))));
    }
    state.output = CameraPose{position, output, vcam.fov};
}

bool CinematicSystem::poseOf(ecs::Entity camera, CameraPose& pose) const {
    const auto it = vcams_.find(camera.handle());
    if (it == vcams_.end()) return false;
    pose = it->second.output;
    return true;
}

float CinematicSystem::blendProgress() const {
    if (blend_duration_ <= 0.0f) return 1.0f;
    return std::clamp(blend_time_ / blend_duration_, 0.0f, 1.0f);
}

// -----------------------------------------------------------------------------
// Camera Brain
// -----------------------------------------------------------------------------

void CinematicSystem::updateBrain(ecs::World& world, float dt, bool playing) {
    // La camara real: la primera con Camera + CameraBrain (la principal si hay varias).
    ecs::Entity brain_entity;
    for (const entt::entity handle : world.registry().view<CameraBrain, ecs::Camera>()) {
        const ecs::Entity e = world.wrap(handle);
        if (!e.activeInHierarchy()) continue;
        if (!brain_entity.valid() || (e.get<ecs::Camera>().is_main && !brain_entity.get<ecs::Camera>().is_main)) {
            brain_entity = e;
        }
    }
    brain_ = brain_entity;
    if (!brain_entity.valid()) {
        live_ = {};
        has_output_ = false;
        return;
    }
    const CameraBrain& brain = brain_entity.get<CameraBrain>();

    // Quien manda: una secuencia sonando (o en vista previa); si no, la de
    // mayor prioridad (a igualdad, la ultima de la Jerarquia).
    ecs::Entity desired;
    float blend = brain.default_blend;
    BlendCurve curve = brain.curve;
    int shot_index = -1;
    ecs::Entity sequence_entity;
    const auto from_sequence = [&](ecs::Entity seq_entity, float t) {
        const CinematicSequence* seq = seq_entity.tryGet<CinematicSequence>();
        if (seq == nullptr) return false;
        const int index = seq->shotAt(t);
        if (index < 0) return false;
        const CinematicShot& shot = seq->shots[static_cast<std::size_t>(index)];
        const ecs::Entity camera = world.find(shot.camera);
        if (!camera.valid() || !camera.has<VirtualCamera>() || vcams_.count(camera.handle()) == 0) return false;
        desired = camera;
        blend = shot.blend_in;
        curve = shot.curve;
        shot_index = index;
        sequence_entity = seq_entity;
        return true;
    };
    bool sequenced = false;
    if (solo_.valid() && world.valid(solo_.handle()) && vcams_.count(solo_.handle()) != 0) {
        desired = solo_;
        sequenced = true;  // manda sobre todo (sin mezcla de secuencia)
        shot_index = -1;
    } else if (preview_.valid()) {
        sequenced = from_sequence(preview_, preview_time_);
    }
    if (!sequenced && playing) {
        for (const auto& [handle, state] : sequences_) {
            if (state.playing && world.valid(handle) && from_sequence(world.wrap(handle), state.time)) {
                sequenced = true;
                break;
            }
        }
    }
    if (!sequenced) {
        int best = -1000000;
        world.forEachDepthFirst([&](ecs::Entity e) {
            const VirtualCamera* vcam = e.tryGet<VirtualCamera>();
            if (vcam == nullptr || vcams_.count(e.handle()) == 0) return;
            if (vcam->priority >= best) {
                best = vcam->priority;
                desired = e;
            }
        });
    }
    if (!desired.valid()) {
        live_ = {};
        return;  // sin camaras virtuales: la camara real se queda como este
    }
    if (!playing && !brain.update_in_editor && !preview_.valid()) return;

    // Cambio de camara (o de plano con la misma camara): mezcla o corte.
    const bool shot_changed = sequenced && (shot_index != live_shot_ || !(sequence_entity == live_sequence_));
    if (!(desired == live_) || shot_changed) {
        const bool can_blend = has_output_ && live_.valid() && blend > 1e-3f && curve != BlendCurve::Cut;
        blend_from_camera_ = live_;
        blend_from_ = output_;
        live_ = desired;
        blend_time_ = 0.0f;
        blend_duration_ = can_blend ? blend : 0.0f;
        blend_curve_ = curve;
        cut_ = !can_blend && has_output_;
    }
    live_shot_ = sequenced ? shot_index : -1;
    live_sequence_ = sequence_entity;

    CameraPose pose = vcams_[live_.handle()].output;
    if (blend_duration_ > 0.0f && blend_time_ < blend_duration_) {
        blend_time_ += dt;
        const float t = applyBlendCurve(blend_curve_, blend_time_ / blend_duration_);
        pose.position = core::lerp(blend_from_.position, pose.position, t);
        pose.rotation = core::slerp(blend_from_.rotation, pose.rotation, t);
        pose.fov = blend_from_.fov + (pose.fov - blend_from_.fov) * t;
    }
    output_ = pose;
    has_output_ = true;

    setPose(brain_entity, pose.position, pose.rotation);
    brain_entity.get<ecs::Camera>().fov = pose.fov;
}

}  // namespace cramion::cinema
