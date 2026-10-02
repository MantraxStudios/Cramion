// RenderSync: Motion Matching (anim/MotionMatching.h). Cada frame predice la
// trayectoria del personaje, busca en la base el fotograma que mejor encaja
// (cada "Intervalo de busqueda" o al acabarse un clip sin bucle) y salta alli
// con inercializacion. Ver RenderSync.h.

#include "CramionCore/ecs/RenderSync.h"

#include "CramionCore/anim/IK.h"
#include "CramionCore/ecs/MathUtil.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>

namespace cramion::ecs {

using core::Mat4;
using core::Quat;
using core::Vec3;

namespace {

Vec3 flat(const Vec3& v) { return Vec3{v.x, 0.0f, v.z}; }

float wrapAngle(float a) {
    while (a > core::kPi) a -= 2.0f * core::kPi;
    while (a <= -core::kPi) a += 2.0f * core::kPi;
    return a;
}

Vec3 flatDirection(const Vec3& v, const Vec3& fallback) {
    const Vec3 f = flat(v);
    const float l = core::length(f);
    return l > 1e-5f ? f * (1.0f / l) : fallback;
}

}  // namespace

std::shared_ptr<const anim::MotionDatabaseAsset> RenderSync::motionDatabase(const Uuid& uuid) {
    if (const auto it = motion_databases_.find(uuid); it != motion_databases_.end()) return it->second;
    if (failed_motion_databases_.contains(uuid)) return nullptr;
    const auto info = assets_.database().find(uuid);
    auto database = std::make_shared<anim::MotionDatabaseAsset>();
    std::string error;
    if (!info || info->type != assets::AssetType::MotionDatabase ||
        !anim::loadMotionDatabase(info->path, *database, &error)) {
        failed_motion_databases_.insert(uuid);
        std::cerr << "[RenderSync] No se pudo cargar la base de Motion Matching " << uuid.toString() << " " << error
                  << "\n";
        return nullptr;
    }
    motion_databases_[uuid] = database;
    return database;
}

void RenderSync::reloadMotionDatabase(const Uuid& uuid) {
    motion_databases_.erase(uuid);
    failed_motion_databases_.erase(uuid);
    for (auto it = motion_features_.begin(); it != motion_features_.end();) {
        it = it->first.clip == uuid ? motion_features_.erase(it) : std::next(it);
    }
    // Los que la usan vuelven a buscar desde cero al rehacerse sus rasgos.
}

std::shared_ptr<const anim::MotionFeatures> RenderSync::motionFeaturesOf(Entity e) const {
    if (!e.valid()) return nullptr;
    const anim::MotionMatching* mm = e.tryGet<anim::MotionMatching>();
    if (mm == nullptr || !mm->database.valid()) return nullptr;
    const auto state = animations_.find(e.handle());
    if (state == animations_.end()) return nullptr;
    const auto it = motion_features_.find(ClipKey{state->second.model, mm->database.uuid});
    return it != motion_features_.end() ? it->second : nullptr;
}

RenderSync::MotionStep RenderSync::updateMotionMatching(Entity e, anim::MotionMatching& mm, std::uint32_t model,
                                                        const asset::ModelData& data, AnimationState& state,
                                                        scene::Scene& scene, float delta_seconds) {
    anim::MotionMatchingRuntime& rt = mm.runtime;
    const std::shared_ptr<const anim::MotionDatabaseAsset> db = motionDatabase(mm.database.uuid);
    if (!db) {
        rt.error = "no se pudo leer la base de Motion Matching";
        rt.debug_valid = false;
        return MotionStep::None;
    }

    // Rasgos de esta base con este modelo (la primera vez; sus clips sueltos
    // pueden estar leyendose aun).
    std::shared_ptr<anim::MotionFeatures>& slot = motion_features_[ClipKey{model, mm.database.uuid}];
    if (!slot) {
        std::vector<int> clips(db->clips.size(), -1);
        bool waiting = false;
        for (std::size_t i = 0; i < db->clips.size(); ++i) {
            const anim::MotionClipEntry& entry = db->clips[i];
            if (entry.clip.valid()) {
                clips[i] = externalClip(model, entry.clip.uuid, scene);
                if (clips[i] < 0 && clipPending(model, entry.clip.uuid)) waiting = true;
            } else if (!entry.clip_name.empty()) {
                for (std::size_t c = 0; c < data.animations.size(); ++c) {
                    if (data.animations[c].name == entry.clip_name) {
                        clips[i] = static_cast<int>(c);
                        break;
                    }
                }
            }
        }
        if (waiting) {
            motion_features_.erase(ClipKey{model, mm.database.uuid});
            return MotionStep::Waiting;
        }
        auto built = std::make_shared<anim::MotionFeatures>();
        if (!anim::buildMotionFeatures(data, *db, clips, *built)) {
            std::cerr << "[MotionMatching] " << e.name() << ": " << built->error << "\n";
        } else {
            std::cout << "[MotionMatching] " << e.name() << ": " << built->frames.size() << " fotogramas, "
                      << db->clips.size() << " clips\n";
        }
        slot = built;
        rt.entry = -1;
        rt.frame = -1;
    }
    const std::shared_ptr<anim::MotionFeatures> features = slot;
    if (!features || !features->valid()) {
        rt.error = features ? features->error : std::string("sin rasgos");
        rt.debug_valid = false;
        return MotionStep::None;
    }
    rt.error.clear();
    const anim::MotionFeatures& f = *features;
    const float dt = std::max(delta_seconds, 0.0f);

    // --- Simulacion: lo que hace el objeto y lo que se le pide -----------------
    const Mat4& world_matrix = e.worldMatrix();
    const Vec3 position = e.worldPosition();
    const Vec3 facing = flatDirection(ecs::transformDirection(world_matrix, f.rest_forward), Vec3{0.0f, 0.0f, 1.0f});
    const float yaw_now = anim::yawOfDirection(facing);
    if (!rt.sim_valid) {
        rt.sim_valid = true;
        rt.last_position = position;
        rt.last_yaw = yaw_now;
        rt.measured_velocity = Vec3{};
        rt.sim_velocity = Vec3{};
        rt.sim_acceleration = Vec3{};
        rt.yaw_rate = 0.0f;
    }
    if (dt > 1e-5f) {
        Vec3 v = flat(position - rt.last_position) * (1.0f / dt);
        if (core::length(v) > 60.0f) v = rt.measured_velocity;  // teletransporte: no cuenta
        const float k = 1.0f - std::exp(-dt / 0.08f);
        rt.measured_velocity = core::lerp(rt.measured_velocity, v, k);
        const float rate = wrapAngle(yaw_now - rt.last_yaw) / dt;
        rt.yaw_rate += (std::clamp(rate, -20.0f, 20.0f) - rt.yaw_rate) * k;
    }
    rt.last_position = position;
    rt.last_yaw = yaw_now;

    const bool scripted = mm.input == anim::MotionInputMode::Script;
    const Vec3 goal = scripted ? flat(rt.desired_velocity) : rt.measured_velocity;
    if (dt > 0.0f) anim::springVelocityUpdate(rt.sim_velocity, rt.sim_acceleration, goal, mm.velocity_halflife, dt);
    float goal_yaw = yaw_now;
    if (scripted) {
        const Vec3 wanted = flat(rt.desired_facing);
        if (core::length(wanted) > 1e-3f) {
            goal_yaw = anim::yawOfDirection(wanted);
        } else if (core::length(goal) > 0.2f) {
            goal_yaw = anim::yawOfDirection(goal);
        }
    }

    // Trayectoria pedida (mundo) y en los ejes del modelo.
    const Mat4 inverse_world = core::inverse(world_matrix);
    std::array<float, anim::kMotionFeatureCount> raw{};
    if (rt.frame >= 0 && rt.frame < static_cast<int>(f.frames.size())) {
        const float* current = f.rawFeature(static_cast<std::size_t>(rt.frame));
        std::copy(current, current + anim::kMotionFeatureCount, raw.begin());
    } else {
        std::copy(f.mean.begin(), f.mean.end(), raw.begin());
    }
    const float lift = 0.05f;
    for (int k = 0; k < anim::kTrajectoryPoints; ++k) {
        const float t = f.trajectory_times[static_cast<std::size_t>(k)];
        const Vec3 p = anim::springPredictPosition(position, rt.sim_velocity, rt.sim_acceleration, goal,
                                                   mm.velocity_halflife, t);
        float yaw = 0.0f;
        if (scripted) {
            yaw = anim::springPredictYaw(yaw_now, goal_yaw, mm.facing_halflife, t);
        } else {
            constexpr float kDecay = 0.3f;  // el giro que lleva se va apagando
            yaw = yaw_now + rt.yaw_rate * kDecay * (1.0f - std::exp(-t / kDecay));
        }
        const Vec3 dir_world = anim::directionOfYaw(yaw);
        const Vec3 local = ecs::transformPoint(inverse_world, p);
        const Vec3 local_dir = flatDirection(ecs::transformDirection(inverse_world, dir_world), f.rest_forward);
        raw[static_cast<std::size_t>(anim::kFeatTrajectoryPosition + k * 2)] = local.x;
        raw[static_cast<std::size_t>(anim::kFeatTrajectoryPosition + k * 2 + 1)] = local.z;
        raw[static_cast<std::size_t>(anim::kFeatTrajectoryDirection + k * 2)] = local_dir.x;
        raw[static_cast<std::size_t>(anim::kFeatTrajectoryDirection + k * 2 + 1)] = local_dir.z;
        rt.debug_desired[static_cast<std::size_t>(k)] = Vec3{p.x, position.y + lift, p.z};
        rt.debug_desired_dir[static_cast<std::size_t>(k)] = dir_world;
    }
    std::array<float, anim::kMotionFeatureCount> query{};
    anim::normalizeMotionFeature(f, raw.data(), query.data());

    // --- Avanza lo que suena ------------------------------------------------------
    const float step = rt.replay_control ? 0.0f : dt * std::max(mm.speed, 0.0f);
    bool ended = false;
    if (rt.replay_control) {
        // Repeticion: el fotograma lo pone ella.
        if (rt.entry < 0 || rt.entry >= static_cast<int>(f.entry_count.size()) ||
            f.entry_count[static_cast<std::size_t>(rt.entry)] <= 0) {
            rt.debug_valid = false;
            return MotionStep::None;
        }
    } else if (rt.entry >= 0 && rt.entry < static_cast<int>(f.entry_count.size()) &&
        f.entry_count[static_cast<std::size_t>(rt.entry)] > 0) {
        const float duration = f.entry_duration[static_cast<std::size_t>(rt.entry)];
        rt.time += step;
        if (f.entry_loop[static_cast<std::size_t>(rt.entry)]) {
            if (duration > 1e-4f) rt.time -= std::floor(rt.time / duration) * duration;
        } else if (rt.time >= duration - 1e-4f) {
            rt.time = duration;
            ended = true;
        }
    } else {
        rt.entry = -1;
    }

    // --- Busqueda ---------------------------------------------------------------
    rt.search_timer -= dt;
    const std::uint32_t tags = f.tagMask(mm.required_tags + "," + rt.extra_tags);
    int current = rt.entry >= 0 ? f.frameAt(rt.entry, rt.time) : -1;
    bool force = current < 0 || ended;
    if (current >= 0 && tags != 0 && (f.frames[static_cast<std::size_t>(current)].tags & tags) != tags) force = true;
    bool switched = false;
    if (!rt.replay_control && (force || rt.search_timer <= 0.0f)) {
        rt.search_timer = std::max(mm.search_interval, 0.01f);
        ++rt.searches;
        const float current_cost =
            current >= 0 && !force ? anim::motionCost(f, query.data(), current) : std::numeric_limits<float>::max();
        const anim::MotionSearchResult best =
            anim::searchMotion(f, query.data(), tags, rt.entry, rt.time - 0.2f, rt.time + 0.2f);
        if (best.frame >= 0 && (force || best.cost < current_cost - mm.switch_threshold)) {
            const anim::MotionFrame& frame = f.frames[static_cast<std::size_t>(best.frame)];
            switched = rt.entry >= 0;
            rt.entry = frame.entry;
            rt.time = frame.time;
            rt.last_cost = best.cost;
            ++rt.switches;
            current = best.frame;
        } else if (current >= 0) {
            rt.last_cost = current_cost;
        }
    }
    if (rt.entry < 0) {
        rt.debug_valid = false;
        return MotionStep::None;
    }
    rt.frame = current;

    // --- Pose ---------------------------------------------------------------------
    const std::size_t entry = static_cast<std::size_t>(rt.entry);
    const int clip = f.entry_clip[entry];
    const float duration = f.entry_duration[entry];
    const bool loop = f.entry_loop[entry];
    const auto evaluateAt = [&](float tau) {
        if (loop && duration > 1e-4f) tau -= std::floor(tau / duration) * duration;
        tau = std::clamp(tau, 0.0f, duration);
        state.animator.evaluateBlend({anim::ClipSample{clip, f.entry_start[entry] + tau, 1.0f}});
        if (!db->strip_root_motion || f.hips < 0 || f.hips >= static_cast<int>(data.nodes.size())) return;
        // La pose en el sitio: se quita lo que la cadera avanzo y giro (la raiz
        // medida); lo mueve el script / Character Controller.
        Vec3 root{};
        float root_yaw = 0.0f;
        anim::motionMeasuredRootAt(f, rt.entry, tau, root, root_yaw);
        if (core::length(root) < 1e-5f && std::abs(root_yaw) < 1e-5f) return;
        const float half = -root_yaw * 0.5f;
        const Mat4 undo = core::composeTrs(Vec3{}, Quat{0.0f, std::sin(half), 0.0f, std::cos(half)}, Vec3{1.0f, 1.0f, 1.0f}) *
                          core::translate(Vec3{-root.x, 0.0f, -root.z});
        std::vector<Mat4>& locals = state.animator.locals();
        std::vector<Mat4>& globals = state.animator.globals();
        const std::size_t hips = static_cast<std::size_t>(f.hips);
        const int parent = data.nodes[hips].parent;
        const Mat4 hips_global = undo * globals[hips];
        locals[hips] = parent >= 0 ? core::inverse(globals[static_cast<std::size_t>(parent)]) * hips_global : hips_global;
        ik::recomputeGlobals(ik::Pose{&data.nodes, &locals, &globals}, f.hips);
        state.animator.updateBones();
    };
    if (switched && mm.transition_time > 0.0f && state.inertial.hasHistory() && dt > 0.0f && step > 0.0f) {
        // Salto: la pose nueva y la de un instante antes (su velocidad) para
        // que el desfase con lo que se veia se apague conservando el impulso.
        evaluateAt(rt.time);
        const std::vector<Mat4> now = state.animator.locals();
        evaluateAt(rt.time - std::max(step, 1e-3f));
        const std::vector<Mat4> before = state.animator.locals();
        evaluateAt(rt.time);
        state.inertial.start(now, before, mm.transition_time, dt);
    } else {
        evaluateAt(rt.time);
    }
    // El Animator vuelve a elegir su clip si el Motion Matching se apaga.
    state.clip = -3;
    state.controller_state = -1;
    if (clip >= 0 && clip < static_cast<int>(data.animations.size())) {
        rt.clip_label = data.animations[static_cast<std::size_t>(clip)].name;
    }

    // --- Depuracion -----------------------------------------------------------------
    if (current >= 0) {
        const float* matched = f.rawFeature(static_cast<std::size_t>(current));
        for (int k = 0; k < anim::kTrajectoryPoints; ++k) {
            const Vec3 p{matched[anim::kFeatTrajectoryPosition + k * 2], 0.0f,
                         matched[anim::kFeatTrajectoryPosition + k * 2 + 1]};
            const Vec3 d{matched[anim::kFeatTrajectoryDirection + k * 2], 0.0f,
                         matched[anim::kFeatTrajectoryDirection + k * 2 + 1]};
            const Vec3 w = ecs::transformPoint(world_matrix, p);
            rt.debug_matched[static_cast<std::size_t>(k)] = Vec3{w.x, position.y + lift, w.z};
            rt.debug_matched_dir[static_cast<std::size_t>(k)] =
                flatDirection(ecs::transformDirection(world_matrix, d), facing);
        }
    }
    rt.debug_origin = Vec3{position.x, position.y + lift, position.z};
    rt.debug_valid = true;
    return MotionStep::Posed;
}

}  // namespace cramion::ecs
