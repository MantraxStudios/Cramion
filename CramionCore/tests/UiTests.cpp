// Pruebas de la interfaz del juego (consola): anclas, escalado, controles,
// eventos hacia los scripts (ScriptSystem::callMethod: llegan como mensajes
// a los scripts de C++ y a los Visual Scripts) y las propiedades de la
// interfaz de la API (Entity:text, value...). Devuelve 0 si todo va.
#include "CramionCore/ecs/World.h"
#include "CramionCore/scripting/NativeApi.h"
#include "CramionCore/scripting/Scripting.h"
#include "CramionCore/ui/UI.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace cramion;
using scripting::api::Value;

int failures = 0;
void check(bool ok, const char* what) {
    std::printf("  %s %s\n", ok ? "OK   " : "FALLO", what);
    if (!ok) ++failures;
}
bool near(float a, float b) { return std::abs(a - b) < 0.01f; }

// Un mensaje que llego a los scripts del objeto (setMessageListener).
struct Message {
    ecs::Entity target;
    std::string method;
    nlohmann::json arg;
};

int main() {
    std::printf("Anclas\n");
    const ui::UiRect parent{0, 0, 1000, 500};
    ui::RectTransform centered;
    centered.size = core::Vec2{200, 100};
    ui::UiRect r = ui::layoutRect(centered, parent);
    check(near(r.x, 400) && near(r.y, 200) && near(r.w, 200), "centrado en el padre");
    ui::RectTransform stretch;
    stretch.anchor_min = core::Vec2{0, 0};
    stretch.anchor_max = core::Vec2{1, 1};
    stretch.size = core::Vec2{-40, -40};
    r = ui::layoutRect(stretch, parent);
    check(near(r.x, 20) && near(r.w, 960) && near(r.h, 460), "estirado con margen de 20");
    ui::RectTransform corner;
    corner.anchor_min = corner.anchor_max = core::Vec2{1, 0};
    corner.pivot = core::Vec2{1, 0};
    corner.position = core::Vec2{-10, 10};
    corner.size = core::Vec2{100, 50};
    r = ui::layoutRect(corner, parent);
    check(near(r.x, 890) && near(r.y, 10), "anclado arriba a la derecha");

    std::printf("Controles y eventos\n");
    ui::registerUiComponents();
    scripting::registerScriptComponents();
    ecs::World world;
    ecs::Entity canvas = world.create("Canvas");
    canvas.add<ui::Canvas>().reference = core::Vec2{1920, 1080};
    ecs::Entity menu = world.create("Menu");
    ecs::Entity button = world.create("Jugar", canvas);
    button.add<ui::RectTransform>().size = core::Vec2{400, 100};
    ui::Button& b = button.add<ui::Button>();
    b.target = menu.uuid();
    b.on_click = "OnJugar";
    ecs::Entity slider = world.create("Volumen", canvas);
    ui::RectTransform& srt = slider.add<ui::RectTransform>();
    srt.size = core::Vec2{400, 40};
    srt.position = core::Vec2{0, 200};
    ui::Slider& s = slider.add<ui::Slider>();
    s.target = menu.uuid();
    s.on_change = "OnVolumen";
    ecs::Entity field = world.create("Nombre", canvas);
    ui::RectTransform& frt = field.add<ui::RectTransform>();
    frt.size = core::Vec2{400, 60};
    frt.position = core::Vec2{0, -200};
    ui::InputField& f = field.add<ui::InputField>();
    f.target = menu.uuid();
    f.on_submit = "OnEnviar";
    ecs::Entity toggle = world.create("Musica", canvas);
    ui::RectTransform& trt = toggle.add<ui::RectTransform>();
    trt.size = core::Vec2{100, 100};
    trt.position = core::Vec2{600, 0};
    ui::Toggle& tg = toggle.add<ui::Toggle>();
    tg.on_change = "OnMusica";  // sin destino: a su propio objeto

    scripting::ScriptSystem scripts;
    std::vector<Message> messages;
    scripts.setMessageListener([&](ecs::Entity target, const std::string& method, const std::string& arg) {
        messages.push_back(Message{target, method, nlohmann::json::parse(arg, nullptr, false)});
    });
    scripts.start(world);
    ui::UiSystem system;
    const auto dispatch = [&] {
        for (const ui::UiEvent& e : system.takeEvents()) {
            if (e.kind == ui::UiEvent::Kind::Click) scripts.callMethod(e.target, e.method, e.source);
            if (e.kind == ui::UiEvent::Kind::Number) scripts.callMethod(e.target, e.method, e.number);
            if (e.kind == ui::UiEvent::Kind::Text) scripts.callMethod(e.target, e.method, e.text);
            if (e.kind == ui::UiEvent::Kind::Bool) scripts.callMethod(e.target, e.method, e.flag);
        }
    };
    // El ultimo mensaje si es `method` para `target`.
    const auto last = [&](ecs::Entity target, const char* method) -> const Message* {
        if (messages.empty() || messages.back().target != target || messages.back().method != method) return nullptr;
        return &messages.back();
    };
    // Vista a la mitad de la referencia: todo escala 0.5.
    ui::UiInput in;
    system.update(world, 960, 540, in, true, 0.0f);
    ui::UiRect br;
    check(system.rectOf(button.handle(), br) && near(br.w, 200) && near(br.x, 380), "escala con la pantalla (960x540)");
    in.mouse_x = 480; in.mouse_y = 270; in.mouse_pressed = true; in.mouse_down = true;
    system.update(world, 960, 540, in, true, 0.0f);
    in.mouse_pressed = false; in.mouse_down = false; in.mouse_released = true;
    system.update(world, 960, 540, in, true, 0.0f);
    dispatch();
    const std::uint64_t button_id = static_cast<std::uint64_t>(entt::to_integral(button.handle())) + 1;
    const Message* m = last(menu, "OnJugar");
    check(m != nullptr && m->arg == nlohmann::json{{"$e", button_id}}, "clic en el boton: OnJugar(boton) al Menu");
    // Slider: arrastrar al 75 %.
    ui::UiRect sr;
    system.rectOf(slider.handle(), sr);
    in = ui::UiInput{};
    in.mouse_x = sr.x + sr.w * 0.75f; in.mouse_y = sr.y + sr.h * 0.5f; in.mouse_pressed = true; in.mouse_down = true;
    system.update(world, 960, 540, in, true, 0.0f);
    dispatch();
    m = last(menu, "OnVolumen");
    check(near(s.value, 0.75f) && m != nullptr && m->arg.is_number() && near(m->arg.get<float>(), 0.75f),
          "slider: valor y OnVolumen(valor)");
    // Campo de texto: clic, escribir, Enter.
    ui::UiRect fr;
    system.rectOf(field.handle(), fr);
    in = ui::UiInput{};
    in.mouse_x = fr.x + 5; in.mouse_y = fr.y + 5; in.mouse_pressed = true; in.mouse_released = true;
    system.update(world, 960, 540, in, true, 0.0f);
    in = ui::UiInput{};
    in.typed = "Hola";
    system.update(world, 960, 540, in, true, 0.0f);
    in = ui::UiInput{};
    in.enter = true;
    system.update(world, 960, 540, in, true, 0.0f);
    dispatch();
    m = last(menu, "OnEnviar");
    check(f.text == "Hola" && m != nullptr && m->arg == "Hola", "campo de texto: escribir y OnEnviar(texto)");
    // Casilla sin destino: el mensaje va a su propio objeto.
    ui::UiRect tr;
    system.rectOf(toggle.handle(), tr);
    in = ui::UiInput{};
    in.mouse_x = tr.x + tr.w * 0.5f; in.mouse_y = tr.y + tr.h * 0.5f; in.mouse_pressed = true; in.mouse_down = true;
    system.update(world, 960, 540, in, true, 0.0f);
    in.mouse_pressed = false; in.mouse_down = false; in.mouse_released = true;
    system.update(world, 960, 540, in, true, 0.0f);
    dispatch();
    m = last(toggle, "OnMusica");
    check(tg.on && m != nullptr && m->arg == true, "casilla: OnMusica(true) a su objeto");
    // Un objeto que ya no existe no recibe nada.
    const std::size_t before = messages.size();
    scripts.callMethod(ecs::Entity{}, "OnNada", 1.0f);
    check(messages.size() == before, "callMethod sin objeto no envia nada");

    std::printf("API de la interfaz\n");
    scripting::api::NativeApi& api = scripts.nativeApi();
    for (const char* name : {"Entity:text", "Entity:value", "Entity:interactable", "Entity:color", "Entity:texture",
                             "Entity:alpha", "Entity:uiPosition", "Entity:uiSize"}) {
        const scripting::api::Entry* e = api.find(name);
        if (e == nullptr || e->kind != scripting::api::Entry::Kind::Property || !e->assign) {
            std::printf("  (falta %s)\n", name);
            check(false, "propiedad de la interfaz registrada");
        }
    }
    const Value vslider = Value::entity(slider.handle());
    api.set("Entity:value", Value(0.2), vslider);
    check(near(s.value, 0.2f) && near(static_cast<float>(api.get("Entity:value", vslider).asNumber()), 0.2f),
          "Entity:value del slider");
    api.set("value", Value(0), Value::entity(toggle.handle()));
    check(!tg.on, "Entity:value de la casilla");
    api.set("text", Value("Ana"), Value::entity(field.handle()));
    check(f.text == "Ana" && api.get("text", Value::entity(field.handle())).asString() == "Ana", "Entity:text del campo");
    api.set("interactable", Value(false), Value::entity(button.handle()));
    check(!b.interactable && !api.get("interactable", Value::entity(button.handle())).truthy(), "Entity:interactable del boton");
    api.set("uiPosition", Value(core::Vec3{50, 60, 0}), Value::entity(button.handle()));
    check(near(button.get<ui::RectTransform>().position.x, 50) && near(button.get<ui::RectTransform>().position.y, 60),
          "Entity:uiPosition");
    // Un boton que no responde no envia el clic.
    messages.clear();
    system.update(world, 960, 540, ui::UiInput{}, true, 0.0f);
    system.rectOf(button.handle(), br);
    in = ui::UiInput{};
    in.mouse_x = br.x + br.w * 0.5f; in.mouse_y = br.y + br.h * 0.5f; in.mouse_pressed = true; in.mouse_down = true;
    system.update(world, 960, 540, in, true, 0.0f);
    in.mouse_pressed = false; in.mouse_down = false; in.mouse_released = true;
    system.update(world, 960, 540, in, true, 0.0f);
    dispatch();
    check(messages.empty(), "interactable = false: sin OnJugar");
    // Por el puente de los scripts de C++ (el mismo JSON que el SDK).
    const nlohmann::json request = {{"op", "get"},
                                    {"self", {{"$e", static_cast<std::uint64_t>(entt::to_integral(field.handle())) + 1}}},
                                    {"key", "text"}};
    const nlohmann::json reply = nlohmann::json::parse(scripts.bridgeCall(request.dump()));
    check(reply["ok"] == true && reply["result"] == "Ana", "puente: Entity:text");
    scripts.stop();
    std::printf("\n%d fallos\n", failures);
    return failures == 0 ? 0 : 1;
}
