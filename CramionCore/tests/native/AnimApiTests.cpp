// Pruebas del modulo AnimApi: capas, montages, curvas, OnAnimationEvent y root motion.

#include "ApiTest.h"

#include "CramionCore/ecs/Components.h"

#include <cmath>

using namespace cramion;
using namespace cramion::apitest;

namespace {

bool near(float a, float b, float eps = 1e-4f) { return std::abs(a - b) <= eps; }

struct Message {
    std::string method;
    json value;
};

void testLayersAndMontage() {
    std::printf("Capas y montages\n");
    ApiFixture t;
    ecs::Entity e = t.world.create("Heroe");
    const Value self = t.entity(e);
    t.call("setLayerWeight", {Value("Torso"), Value(0.5)}, self);  // sin Animator: nada
    check(t.call("layerWeight", {Value("Torso")}, self).asNumber() == 0.0 && !t.call("isMontagePlaying", {}, self).truthy(),
          "sin Animator: no falla");
    ecs::Animator& a = e.add<ecs::Animator>();
    a.runtime.layer_names = {"Base", "Torso"};
    t.call("setLayerWeight", {Value("Torso"), Value(0.25)}, self);
    check(a.runtime.layers.size() == 1 && near(a.runtime.layers[0].weight, 0.25f), "setLayerWeight por nombre");
    t.call("setLayerWeight", {Value(1), Value(2.0)}, self);
    check(near(a.runtime.layers[0].weight, 1.0f), "por numero y recortado a [0, 1]");
    check(near(static_cast<float>(t.call("layerWeight", {Value("Torso")}, self).asNumber()), 1.0f) &&
              t.call("layerWeight", {Value(0)}, self).asNumber() == 1.0,
          "layerWeight (la base siempre 1)");
    a.runtime.layers[0].state_name = "Apuntar";
    a.runtime.state_name = "Correr";
    check(t.call("layerState", {Value("Torso")}, self).asString() == "Apuntar" &&
              t.call("layerState", {Value("Base")}, self).asString() == "Correr",
          "layerState");
    t.call("crossFadeLayer", {Value("Torso"), Value("Recargar"), Value(0.3)}, self);
    check(a.runtime.layers[0].cross_fade == "Recargar" && near(a.runtime.layers[0].cross_fade_time, 0.3f), "crossFadeLayer");

    Value options = Value::object();
    options.set("slot", Value("Torso"));
    options.set("blendIn", Value(0.05));
    options.set("section", Value("Inicio"));
    const double id = t.call("playMontage", {Value("Golpe"), options}, self).asNumber();
    const ecs::AnimatorMontage& m = a.runtime.montage;
    check(id == 1.0 && m.active && m.just_started && m.clip_name == "Golpe" && m.slot == "Torso" && near(m.blend_in, 0.05f) &&
              near(m.blend_out, 0.2f) && m.start_section == "Inicio",
          "playMontage con opciones");
    check(t.call("isMontagePlaying", {}, self).truthy(), "isMontagePlaying");
    t.call("montageJumpToSection", {Value("Final")}, self);
    check(m.jump_section == "Final", "montageJumpToSection");
    t.call("stopMontage", {Value(0.4)}, self);
    check(m.stopping && near(m.stop_blend, 0.4f), "stopMontage");
    t.call("playMontage", {Value("00000000-0000-0000-0000-000000000000")}, self);
    check(m.serial == 2 && !m.stopping, "otro playMontage reinicia");

    a.runtime.curves["Pisada"] = 0.75f;
    check(near(static_cast<float>(t.call("animatorCurve", {Value("Pisada")}, self).asNumber()), 0.75f) &&
              t.call("animatorCurve", {Value("Otra")}, self).asNumber() == 0.0,
          "animatorCurve");
    t.call("setRootMotion", {Value("character")}, self);
    check(a.root_motion == ecs::RootMotionMode::CharacterController, "setRootMotion por nombre");
    t.call("setRootMotion", {Value(4)}, self);
    check(a.root_motion == ecs::RootMotionMode::Script, "setRootMotion por numero");
}

void testEventsAndRootMotion() {
    std::printf("Eventos y root motion\n");
    ApiFixture t;
    std::vector<std::pair<ecs::Entity, Message>> messages;
    t.scripts.setMessageListener([&](ecs::Entity e, const std::string& method, const std::string& value) {
        messages.push_back({e, Message{method, json::parse(value, nullptr, false)}});
    });
    ecs::Entity e = t.world.create("Heroe");
    const Value self = t.entity(e);
    ecs::Animator& a = e.add<ecs::Animator>();
    a.runtime.pushEvent(ecs::AnimatorFiredEvent{"Golpe", "Ataque", "{\"dano\":10}", 0.4f, 0});
    a.runtime.pushEvent(ecs::AnimatorFiredEvent{"Paso", "Andar", "izquierdo", 0.25f, 1});
    t.frame();
    int events = 0;
    for (const auto& [who, msg] : messages) {
        if (msg.method != "OnAnimationEvent" || !(who == e)) continue;
        ++events;
        if (events == 1) {
            check(msg.value.value("name", "") == "Golpe" && msg.value.value("clip", "") == "Ataque" &&
                      msg.value["params"].is_object() && msg.value["params"].value("dano", 0) == 10,
                  "OnAnimationEvent con parametros JSON");
        } else {
            check(msg.value.value("name", "") == "Paso" && msg.value["params"] == "izquierdo" && msg.value.value("layer", 0) == 1,
                  "parametros de texto y la capa");
        }
    }
    check(events == 2 && a.runtime.events.empty(), "cada evento llega una vez y la cola se vacia");
    messages.clear();
    t.frame();
    check(messages.empty(), "el frame siguiente no repite");

    // Root motion en modo Transform: mueve y gira la entidad.
    a.root_motion = ecs::RootMotionMode::Transform;
    a.runtime.root_delta_position = core::Vec3{0.0f, 0.0f, 1.5f};
    a.runtime.root_delta_yaw = 0.5f;
    t.frame();
    const core::Vec3 p = e.worldPosition();
    check(near(p.z, 1.5f) && near(p.x, 0.0f), "Transform: avanza lo que dio la animacion");
    const core::Quat q = e.localRotation();
    check(near(2.0f * std::atan2(q.y, q.w), 0.5f, 1e-3f), "Transform: gira en Y");
    check(a.runtime.root_delta_position.z == 0.0f && near(a.runtime.last_root_position.z, 1.5f), "se consume (y queda el ultimo)");
    const Value delta = t.call("rootMotionDelta", {}, self);
    check(near(delta["position"].asVec3().z, 1.5f) && near(static_cast<float>(delta["yaw"].asNumber()), 0.5f * 57.2957795f, 1e-2f),
          "rootMotionDelta (yaw en grados)");
    // Modo Script: no mueve nada, llega OnAnimatorMove.
    a.root_motion = ecs::RootMotionMode::Script;
    a.runtime.root_delta_position = core::Vec3{1.0f, 0.0f, 0.0f};
    messages.clear();
    t.frame();
    bool moved = false;
    for (const auto& [who, msg] : messages) {
        if (msg.method == "OnAnimatorMove" && who == e) moved = msg.value.contains("deltaPosition");
    }
    check(moved && near(e.worldPosition().x, 0.0f), "Script: OnAnimatorMove y la entidad quieta");
    // Apagado: se descarta.
    a.root_motion = ecs::RootMotionMode::Off;
    a.runtime.root_delta_position = core::Vec3{5.0f, 0.0f, 0.0f};
    t.frame();
    check(near(e.worldPosition().x, 0.0f) && a.runtime.root_delta_position.x == 0.0f, "apagado: no mueve");
}

void testRegistry() {
    std::printf("Registro\n");
    ApiFixture t(false);
    static const char* const kNames[] = {"setLayerWeight", "layerWeight", "layerState", "crossFadeLayer", "playMontage",
                                         "stopMontage", "montageJumpToSection", "isMontagePlaying", "montageTime",
                                         "montageSection", "animatorCurve", "rootMotionDelta", "setRootMotion"};
    int found = 0;
    bool documented = true;
    for (const char* name : kNames) {
        const scripting::api::Entry* entry = t.api().find(std::string("Entity:") + name);
        if (entry == nullptr) {
            std::printf("  (falta Entity:%s)\n", name);
            continue;
        }
        ++found;
        if (entry->doc.description.empty() || !entry->member()) documented = false;
    }
    check(found == static_cast<int>(std::size(kNames)), "todas las funciones nuevas estan registradas");
    check(documented, "con documentacion y como miembros de Entity");
}

}  // namespace

int main() {
    testLayersAndMontage();
    testEventsAndRootMotion();
    testRegistry();
    return finish();
}
