// Animacion de juego: capas del Animator, montages, curvas, eventos de los
// clips y root motion.
//
//   entity().setLayerWeight("Torso", 0.5)       peso de una capa (nombre o numero; 0 = base)
//   entity().layerWeight("Torso")               el que tiene ahora
//   entity().layerState("Torso")                estado que suena en esa capa
//   entity().crossFadeLayer("Torso", "Apuntar", 0.2)
//   entity().playMontage("Golpe", {slot = "Torso", blendIn = 0.1, blendOut = 0.2, speed = 1, section = "Inicio"})
//   entity().stopMontage(0.2)   entity().montageJumpToSection("Final")   entity().isMontagePlaying()
//   entity().animatorCurve("Pisada")            valor de una curva de los clips que suenan
//   entity().rootMotionDelta()                  {position, yaw} del ultimo frame (yaw en grados)
//   entity().setRootMotion("transform")         off / transform / character / rigidbody / script
//
// Eventos para los scripts de C++ (mensajes, fase Events):
//   OnAnimationEvent {name, clip, params, time, layer}  (Script::onAnimationEvent)
//   OnAnimatorMove {deltaPosition, deltaRotation}       solo con root motion "script"
//                                                        (Script::onAnimatorMove)

#include "Modules.h"

#include "CramionCore/ecs/MathUtil.h"
#include "CramionCore/physics/PhysicsComponents.h"
#include "CramionCore/physics/PhysicsSystem.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace cramion::scripting::native {
namespace {

using core::Vec3;

constexpr float kRadToDeg = 57.2957795f;

// Indice de capa de un argumento: numero (0 = base) o nombre. -1 = no existe.
int layerIndex(const ecs::Animator& a, const api::Value& v) {
    if (v.isNumber()) return static_cast<int>(v.asNumber());
    const std::string name = v.asString();
    for (std::size_t i = 0; i < a.runtime.layer_names.size(); ++i) {
        if (a.runtime.layer_names[i] == name) return static_cast<int>(i);
    }
    return -1;
}

ecs::AnimatorLayerRuntime* layerRuntime(ecs::Animator& a, int layer) {
    if (layer < 1 || layer > 64) return nullptr;
    if (a.runtime.layers.size() < static_cast<std::size_t>(layer)) a.runtime.layers.resize(static_cast<std::size_t>(layer));
    return &a.runtime.layers[static_cast<std::size_t>(layer - 1)];
}

ecs::RootMotionMode rootMotionMode(const std::string& text) {
    const std::string t = lowerText(text);
    if (t == "transform") return ecs::RootMotionMode::Transform;
    if (t == "character" || t == "charactercontroller" || t == "cc") return ecs::RootMotionMode::CharacterController;
    if (t == "rigidbody" || t == "body") return ecs::RootMotionMode::Rigidbody;
    if (t == "script") return ecs::RootMotionMode::Script;
    return ecs::RootMotionMode::Off;
}

// La entidad o el antepasado mas cercano con el componente T.
template <typename T>
ecs::Entity selfOrAncestorWith(ecs::Entity e) {
    for (int guard = 0; e.valid() && guard < 64; ++guard, e = e.parent()) {
        if (e.has<T>()) return e;
    }
    return {};
}

core::Quat yawQuat(float yaw) { return core::Quat{0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f)}; }

void rotateBy(ecs::Entity e, float yaw) {
    if (yaw == 0.0f) return;
    e.setLocalRotation(core::normalize(ecs::quatMultiply(yawQuat(yaw), e.localRotation())));
}

// Aplica (o entrega al script) el root motion pendiente de un Animator.
void applyRootMotion(Runtime& rt, ecs::Entity e, ecs::Animator& a, float dt) {
    ecs::AnimatorRuntime& r = a.runtime;
    const Vec3 delta = r.root_delta_position;
    const float yaw = r.root_delta_yaw;
    r.root_delta_position = Vec3{};
    r.root_delta_yaw = 0.0f;
    r.last_root_position = delta;
    r.last_root_yaw = yaw;
    if (a.root_motion == ecs::RootMotionMode::Off || (delta.x == 0.0f && delta.y == 0.0f && delta.z == 0.0f && yaw == 0.0f)) {
        return;
    }
    switch (a.root_motion) {
        case ecs::RootMotionMode::Transform:
            e.setWorldPosition(e.worldPosition() + delta);
            rotateBy(e, yaw);
            break;
        case ecs::RootMotionMode::CharacterController: {
            const ecs::Entity cc = selfOrAncestorWith<physics::CharacterController>(e);
            if (!cc.valid() || rt.physics == nullptr || rt.world == nullptr) break;
            rt.physics->moveCharacter(*rt.world, cc, delta);
            rotateBy(cc, yaw);
            break;
        }
        case ecs::RootMotionMode::Rigidbody: {
            const ecs::Entity body = selfOrAncestorWith<physics::Rigidbody>(e);
            if (!body.valid() || rt.physics == nullptr || dt <= 1e-6f) break;
            // Velocidad en XZ (la caida la sigue llevando la fisica) y giro en Y.
            const Vec3 v = rt.physics->linearVelocity(body);
            rt.physics->setLinearVelocity(body, Vec3{delta.x / dt, v.y, delta.z / dt});
            rt.physics->setAngularVelocity(body, Vec3{0.0f, yaw / dt, 0.0f});
            break;
        }
        case ecs::RootMotionMode::Script: {
            if (!rt.message_listener) break;
            api::Value arg = api::Value::object();
            arg.set("deltaPosition", delta);
            arg.set("deltaRotation", yaw * kRadToDeg);
            rt.message_listener(e, "OnAnimatorMove", rt.native.toJson(arg).dump());
            break;
        }
        case ecs::RootMotionMode::Off: break;
    }
}

void dispatchAnimation(Runtime& rt, float dt) {
    if (rt.world == nullptr) return;
    // Copia de las entidades: un mensaje puede crear o destruir objetos.
    std::vector<entt::entity> handles;
    for (const entt::entity h : rt.world->registry().view<ecs::Animator>()) handles.push_back(h);
    for (const entt::entity h : handles) {
        if (!rt.world->valid(h)) continue;
        const ecs::Entity e = rt.world->wrap(h);
        ecs::Animator* a = e.tryGet<ecs::Animator>();
        if (a == nullptr) continue;
        applyRootMotion(rt, e, *a, dt);
        a = e.tryGet<ecs::Animator>();  // (el script pudo quitarlo)
        if (a == nullptr || a->runtime.events.empty()) continue;
        std::vector<ecs::AnimatorFiredEvent> events;
        events.swap(a->runtime.events);
        if (!rt.message_listener) continue;
        for (const ecs::AnimatorFiredEvent& ev : events) {
            api::Value arg = api::Value::object();
            arg.set("name", ev.name);
            arg.set("clip", ev.clip);
            // Parametros JSON si lo son; si no, el texto tal cual.
            const nlohmann::json parsed = ev.params.empty() ? nlohmann::json() : nlohmann::json::parse(ev.params, nullptr, false);
            arg.set("params", !ev.params.empty() && !parsed.is_discarded() && (parsed.is_object() || parsed.is_array() || parsed.is_number())
                                  ? rt.native.fromJson(parsed)
                                  : api::Value(ev.params));
            arg.set("time", ev.time);
            arg.set("layer", ev.layer);
            rt.message_listener(e, "OnAnimationEvent", rt.native.toJson(arg).dump());
        }
    }
}

}  // namespace

void registerAnimApi(Runtime& rt) {
    api::NativeApi& api = rt.native;
    const auto animatorOf = [&rt](api::Call& c) -> ecs::Animator* { return rt.selfEntity(c).tryGet<ecs::Animator>(); };

    api.method("Entity", "setLayerWeight", [animatorOf](api::Call& c) {
        ecs::Animator* a = animatorOf(c);
        if (a == nullptr) return api::Value{};
        const int layer = layerIndex(*a, c.arg(0));
        if (ecs::AnimatorLayerRuntime* l = layerRuntime(*a, layer)) l->weight = std::clamp(static_cast<float>(c.number(1)), 0.0f, 1.0f);
        return api::Value{};
    }, {"\"Torso\", 0.5", "peso de una capa del Animator (nombre o numero; 1 = la primera de encima)"});
    api.method("Entity", "layerWeight", [animatorOf](api::Call& c) {
        ecs::Animator* a = animatorOf(c);
        if (a == nullptr) return api::Value(0.0f);
        const int layer = layerIndex(*a, c.arg(0));
        if (layer == 0) return api::Value(1.0f);
        if (layer < 1 || static_cast<std::size_t>(layer) > a->runtime.layers.size()) return api::Value(0.0f);
        const ecs::AnimatorLayerRuntime& l = a->runtime.layers[static_cast<std::size_t>(layer - 1)];
        return api::Value(l.weight >= 0.0f ? l.weight : l.effective_weight);
    }, {"\"Torso\"", "peso de una capa del Animator", "numero"});
    api.method("Entity", "layerState", [animatorOf](api::Call& c) {
        ecs::Animator* a = animatorOf(c);
        if (a == nullptr) return api::Value(std::string());
        const int layer = layerIndex(*a, c.arg(0));
        if (layer == 0) return api::Value(a->runtime.state_name);
        if (layer < 1 || static_cast<std::size_t>(layer) > a->runtime.layers.size()) return api::Value(std::string());
        return api::Value(a->runtime.layers[static_cast<std::size_t>(layer - 1)].state_name);
    }, {"\"Torso\"", "estado que suena en una capa del Animator", "texto"});
    api.method("Entity", "crossFadeLayer", [animatorOf](api::Call& c) {
        ecs::Animator* a = animatorOf(c);
        if (a == nullptr) return api::Value{};
        const int layer = layerIndex(*a, c.arg(0));
        const std::string state = c.string(1);
        const auto seconds = static_cast<float>(c.number(2, 0.15));
        if (layer == 0) {
            a->runtime.cross_fade = state;
            a->runtime.cross_fade_time = seconds;
            a->runtime.cross_fade_inertial = false;
        } else if (ecs::AnimatorLayerRuntime* l = layerRuntime(*a, layer)) {
            l->cross_fade = state;
            l->cross_fade_time = seconds;
        }
        a->playing = true;
        return api::Value{};
    }, {"\"Torso\", \"Apuntar\", 0.2", "salta a un estado de una capa con fundido"});

    api.method("Entity", "playMontage", [animatorOf](api::Call& c) {
        ecs::Animator* a = animatorOf(c);
        if (a == nullptr) return api::Value(0);
        ecs::AnimatorMontage& m = a->runtime.montage;
        const std::string clip = c.string(0);
        const Uuid uuid = Uuid::parse(clip);
        m.clip = assets::AssetRef{uuid, assets::AssetType::AnimationClip};
        m.clip_name = uuid.valid() ? std::string{} : clip;
        const api::Value& o = c.arg(1);
        m.slot = o.isObject() && o["slot"].isString() ? o["slot"].asString() : std::string{};
        m.blend_in = o.isObject() && o["blendIn"].isNumber() ? static_cast<float>(o["blendIn"].asNumber()) : 0.15f;
        m.blend_out = o.isObject() && o["blendOut"].isNumber() ? static_cast<float>(o["blendOut"].asNumber()) : 0.2f;
        m.speed = o.isObject() && o["speed"].isNumber() ? static_cast<float>(o["speed"].asNumber()) : 1.0f;
        m.start_section = o.isObject() && o["section"].isString() ? o["section"].asString() : std::string{};
        ecs::startMontage(m);
        a->playing = true;
        return api::Value(static_cast<double>(m.serial));
    }, {"\"Golpe\", {slot = \"Torso\", blendIn = 0.1, blendOut = 0.2, speed = 1, section = \"Inicio\"}",
        "reproduce un clip puntual sobre el grafo (cuerpo entero o la capa del slot) y vuelve solo", "numero (id)"});
    api.method("Entity", "stopMontage", [animatorOf](api::Call& c) {
        ecs::Animator* a = animatorOf(c);
        if (a == nullptr || !a->runtime.montage.active) return api::Value{};
        a->runtime.montage.stopping = true;
        a->runtime.montage.stop_blend = std::max(static_cast<float>(c.number(0, a->runtime.montage.blend_out)), 0.0f);
        a->runtime.montage.stop_elapsed = 0.0f;
        return api::Value{};
    }, {"0.2", "apaga el montage en esos segundos"});
    api.method("Entity", "montageJumpToSection", [animatorOf](api::Call& c) {
        ecs::Animator* a = animatorOf(c);
        if (a != nullptr && a->runtime.montage.active) a->runtime.montage.jump_section = c.string(0);
        return api::Value{};
    }, {"\"Final\"", "salta a una seccion del clip del montage"});
    api.method("Entity", "isMontagePlaying", [animatorOf](api::Call& c) {
        ecs::Animator* a = animatorOf(c);
        return api::Value(a != nullptr && a->runtime.montage.active);
    }, {"", "suena un montage?", "bool"});
    api.method("Entity", "montageTime", [animatorOf](api::Call& c) {
        ecs::Animator* a = animatorOf(c);
        return api::Value(a != nullptr && a->runtime.montage.active ? a->runtime.montage.time : 0.0f);
    }, {"", "segundo del clip del montage", "numero"});
    api.method("Entity", "montageSection", [animatorOf](api::Call& c) {
        ecs::Animator* a = animatorOf(c);
        return api::Value(a != nullptr && a->runtime.montage.active ? a->runtime.montage.section : std::string());
    }, {"", "seccion del montage en la que va", "texto"});

    api.method("Entity", "animatorCurve", [animatorOf](api::Call& c) {
        ecs::Animator* a = animatorOf(c);
        if (a == nullptr) return api::Value(0.0f);
        const auto it = a->runtime.curves.find(c.string(0));
        return api::Value(it != a->runtime.curves.end() ? it->second : 0.0f);
    }, {"\"Pisada\"", "valor de una curva de los clips que suenan (Animator.GetFloat de una curva)", "numero"});
    api.method("Entity", "rootMotionDelta", [animatorOf](api::Call& c) {
        api::Value out = api::Value::object();
        const ecs::Animator* a = animatorOf(c);
        out.set("position", a != nullptr ? a->runtime.last_root_position : Vec3{});
        out.set("yaw", a != nullptr ? a->runtime.last_root_yaw * kRadToDeg : 0.0f);
        return out;
    }, {"", "root motion del ultimo frame (mundo; yaw en grados)", "{position, yaw}"});
    api.method("Entity", "setRootMotion", [animatorOf](api::Call& c) {
        if (ecs::Animator* a = animatorOf(c)) {
            a->root_motion = c.arg(0).isNumber() ? static_cast<ecs::RootMotionMode>(std::clamp(static_cast<int>(c.number(0)), 0, 4))
                                                 : rootMotionMode(c.string(0));
        }
        return api::Value{};
    }, {"\"transform\"", "root motion del Animator: off, transform, character, rigidbody o script"});

    rt.onStart([&rt] {
        // Lo que se acumulo fuera de Play no cuenta.
        if (rt.world == nullptr) return;
        for (const entt::entity h : rt.world->registry().view<ecs::Animator>()) {
            ecs::AnimatorRuntime& r = rt.world->registry().get<ecs::Animator>(h).runtime;
            r.root_delta_position = Vec3{};
            r.root_delta_yaw = 0.0f;
            r.events.clear();
        }
    });
    rt.onFrame(Phase::Events, [&rt](float dt) { dispatchAnimation(rt, dt); });
}

}  // namespace cramion::scripting::native
