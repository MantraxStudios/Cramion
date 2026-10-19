// Pruebas de la interfaz del juego (consola): anclas, escalado, controles,
// eventos hacia los scripts (ScriptSystem::callMethod: llegan como mensajes
// a los scripts de C++ y a los Visual Scripts) y las propiedades de la
// interfaz de la API (Entity:text, value...). Devuelve 0 si todo va.
#include "CramionCore/ecs/SceneSerializer.h"
#include "CramionCore/ecs/World.h"
#include "CramionCore/scripting/NativeApi.h"
#include "CramionCore/scripting/Scripting.h"
#include "CramionCore/ui/UI.h"

#include <CramionDM/Input.h>
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

    // Llamada a una funcion de la API por el puente (lo que hace el SDK).
    const auto bridge = [&](const nlohmann::json& req) { return nlohmann::json::parse(scripts.bridgeCall(req.dump())); };
    const auto apiCall = [&](const char* fn, nlohmann::json args = nlohmann::json::array()) {
        return bridge({{"op", "call"}, {"fn", fn}, {"args", args}});
    };

    std::printf("P1: el clic sobre la interfaz no llega al juego\n");
    scripts.setUi(&system);
    {
        // Raton sobre el boton (ya responde otra vez).
        b.interactable = true;
        system.rectOf(button.handle(), br);
        in = ui::UiInput{};
        in.mouse_x = br.x + br.w * 0.5f; in.mouse_y = br.y + br.h * 0.5f;
        system.update(world, 960, 540, in, true, 0.0f);
        check(system.pointerOverUi() && apiCall("UI.pointerOverUi")["result"] == true, "UI.pointerOverUi sobre el boton");
        dm::Input window;
        dm::Event ev;
        ev.type = dm::EventType::MouseButtonPressed;
        ev.button = dm::MouseButton::Left;
        window.onEvent(ev);
        ev.type = dm::EventType::MouseScrolled;
        ev.scrollY = 1.0f;
        window.onEvent(ev);
        dm::Input game = window;
        system.filterGameInput(game);
        check(window.isMouseButtonPressed(dm::MouseButton::Left) && !game.isMouseButtonPressed(dm::MouseButton::Left) &&
                  !game.isMouseButtonDown(dm::MouseButton::Left) && game.scrollY() == 0.0f,
              "pulsar sobre la UI: ni pulsado, ni mantenido, ni rueda para el juego");
        // Mantenido y soltado fuera de la UI: sigue siendo de la interfaz.
        window.newFrame();
        in.mouse_x = 2.0f; in.mouse_y = 2.0f;
        system.update(world, 960, 540, in, true, 0.0f);
        game = window;
        system.filterGameInput(game);
        check(!system.pointerOverUi() && window.isMouseButtonDown(dm::MouseButton::Left) && !game.isMouseButtonDown(dm::MouseButton::Left),
              "el boton sigue siendo de la UI aunque el raton salga");
        window.newFrame();
        ev.type = dm::EventType::MouseButtonReleased;
        ev.button = dm::MouseButton::Left;
        window.onEvent(ev);
        game = window;
        system.filterGameInput(game);
        check(!game.isMouseButtonReleased(dm::MouseButton::Left), "el soltar tampoco llega al juego");
        // Pulsar fuera: es del juego.
        window.newFrame();
        ev.type = dm::EventType::MouseButtonPressed;
        ev.button = dm::MouseButton::Right;
        window.onEvent(ev);
        game = window;
        system.filterGameInput(game);
        check(game.isMouseButtonPressed(dm::MouseButton::Right) && apiCall("UI.pointerOverUi")["result"] == false,
              "pulsar fuera de la UI llega al juego");
        // Una imagen con raycast_target = false (o transparente) no tapa.
        ecs::Entity deco = world.create("Brujula", canvas);
        deco.add<ui::RectTransform>().size = core::Vec2{1920, 1080};
        deco.add<ui::Image>().raycast_target = false;
        system.update(world, 960, 540, in, true, 0.0f);
        check(!system.pointerOverUi(), "imagen con raycast_target = false: no tapa");
        deco.get<ui::Image>().raycast_target = true;
        system.update(world, 960, 540, in, true, 0.0f);
        check(system.pointerOverUi(), "imagen a pantalla completa (raycast_target): tapa");
        deco.get<ui::Image>().alpha = 0.0f;
        system.update(world, 960, 540, in, true, 0.0f);
        check(!system.pointerOverUi(), "imagen transparente: no tapa");
        world.destroy(deco);
        check(apiCall("UI.isTyping")["result"] == false, "UI.isTyping");
    }

    std::printf("P2: eventos del puntero (UIEventTrigger) y uiScreenRect\n");
    {
        system.takeEvents();
        const auto makeSlot = [&](const char* name, core::Vec2 pos) {
            ecs::Entity slot = world.create(name, canvas);
            ui::RectTransform& rt = slot.add<ui::RectTransform>();
            rt.size = core::Vec2{100, 100};
            rt.position = pos;
            slot.add<ui::Image>();
            ui::EventTrigger& t = slot.add<ui::EventTrigger>();
            t.on_hover = "OnEntrar";
            t.on_exit = "OnSalir";
            t.on_right_click = "OnMenu";
            t.on_double_click = "OnDoble";
            t.on_drag_begin = "OnArrastrar";
            t.on_drag = "OnMoviendo";
            t.on_drag_end = "OnFin";
            t.on_drop = "OnSoltar";
            return slot;
        };
        ecs::Entity slot1 = makeSlot("Casilla1", core::Vec2{-600, 0});
        ecs::Entity slot2 = makeSlot("Casilla2", core::Vec2{-600, 300});
        // El icono (hijo, encima) cuenta como la casilla.
        ecs::Entity icon = world.create("Icono", slot1);
        icon.add<ui::RectTransform>().size = core::Vec2{80, 80};
        icon.add<ui::Image>();
        in = ui::UiInput{};
        system.update(world, 960, 540, in, true, 0.0f);
        ui::UiRect r1, r2;
        check(system.rectOf(slot1.handle(), r1) && system.rectOf(slot2.handle(), r2) && near(r1.w, 50.0f) && near(r1.x, 155.0f),
              "rectangulo de las casillas");
        std::vector<ui::UiEvent> evs;
        const auto step = [&](const ui::UiInput& input) {
            system.update(world, 960, 540, input, true, 0.0f);
            for (ui::UiEvent& e : system.takeEvents()) evs.push_back(std::move(e));
        };
        const auto has = [&](const char* method, ecs::Entity source, ecs::Entity other = {}) {
            for (const ui::UiEvent& e : evs) {
                if (e.kind == ui::UiEvent::Kind::Pointer && e.method == method && e.source == source && e.target == source &&
                    e.other == other) {
                    return true;
                }
            }
            return false;
        };
        const float cx1 = r1.x + r1.w * 0.5f, cy1 = r1.y + r1.h * 0.5f;
        in = ui::UiInput{};
        in.mouse_x = cx1; in.mouse_y = cy1;  // sobre el icono
        step(in);
        check(has("OnEntrar", slot1) && system.pointerOverUi(), "on_hover al entrar (el icono cuenta como la casilla)");
        in.mouse_x = 2.0f; in.mouse_y = 2.0f;
        step(in);
        check(has("OnSalir", slot1), "on_exit al salir");
        evs.clear();
        in.mouse_x = cx1; in.mouse_y = cy1;
        in.right_pressed = true; in.right_down = true;
        step(in);
        in.right_pressed = false; in.right_down = false; in.right_released = true;
        step(in);
        bool right_button = false;
        for (const ui::UiEvent& e : evs) right_button = right_button || (e.method == "OnMenu" && e.button == 1);
        check(has("OnMenu", slot1) && right_button, "on_right_click (button = 1)");
        in = ui::UiInput{};
        in.mouse_x = cx1; in.mouse_y = cy1;
        in.mouse_pressed = true; in.mouse_down = true; in.double_click = true;
        step(in);
        in.mouse_pressed = false; in.double_click = false; in.mouse_down = false; in.mouse_released = true;
        step(in);
        check(has("OnDoble", slot1) && !has("OnArrastrar", slot1), "on_double_click (y un clic no arrastra)");
        // Arrastrar la casilla 1 y soltarla en la 2.
        evs.clear();
        in = ui::UiInput{};
        in.mouse_x = cx1; in.mouse_y = cy1; in.mouse_pressed = true; in.mouse_down = true;
        step(in);
        in.mouse_pressed = false;
        in.mouse_y = cy1 + 3.0f;  // menos que el umbral (6 px)
        step(in);
        check(!has("OnArrastrar", slot1) && system.draggedElement() == entt::entity{entt::null}, "por debajo del umbral no arrastra");
        in.mouse_y = cy1 + 20.0f;
        step(in);
        check(has("OnArrastrar", slot1) && system.draggedElement() == slot1.handle() &&
                  apiCall("UI.draggedElement")["result"] == nlohmann::json{{"$e", static_cast<std::uint64_t>(entt::to_integral(slot1.handle())) + 1}},
              "on_drag_begin y UI.draggedElement");
        in.mouse_y = r2.y + r2.h * 0.5f;
        step(in);
        check(has("OnMoviendo", slot1), "on_drag mientras se mueve");
        in.mouse_down = false; in.mouse_released = true;
        step(in);
        check(has("OnSoltar", slot2, slot1) && has("OnFin", slot1, slot2) && system.draggedElement() == entt::entity{entt::null},
              "on_drop en la casilla 2 (other = la 1) y on_drag_end (other = la 2)");
        // Soltar donde no hay nada: on_drag_end sin other.
        evs.clear();
        in = ui::UiInput{};
        in.mouse_x = cx1; in.mouse_y = cy1; in.mouse_pressed = true; in.mouse_down = true;
        step(in);
        in.mouse_pressed = false; in.mouse_x = 900.0f; in.mouse_y = 10.0f;
        step(in);
        in.mouse_down = false; in.mouse_released = true;
        step(in);
        check(has("OnFin", slot1) && !has("OnSoltar", slot2, slot1), "soltar fuera: on_drag_end sin other");
        // A los scripts (como el juego): Visual Scripts con la entidad.
        messages.clear();
        for (const ui::UiEvent& e : evs) {
            if (e.kind == ui::UiEvent::Kind::Pointer) scripts.callMethod(e.target, e.method, e.source);
        }
        check(!messages.empty() && messages.back().method == "OnFin", "callMethod de los eventos del puntero");
        // Entity:uiScreenRect por el puente.
        const nlohmann::json rect = bridge({{"op", "call"},
                                            {"fn", "uiScreenRect"},
                                            {"self", {{"$e", static_cast<std::uint64_t>(entt::to_integral(slot2.handle())) + 1}}},
                                            {"args", nlohmann::json::array()}})["result"];
        check(rect.is_object() && near(rect["x"].get<float>(), r2.x) && near(rect["width"].get<float>(), 50.0f) &&
                  near(rect["height"].get<float>(), 50.0f),
              "Entity:uiScreenRect");
        world.destroy(slot1);
        world.destroy(slot2);
        system.update(world, 960, 540, ui::UiInput{}, true, 0.0f);
        system.takeEvents();
    }

    std::printf("P3: campo de texto con cursor, seleccion, portapapeles y max_length en caracteres\n");
    {
        const nlohmann::json field_id = {{"$e", static_cast<std::uint64_t>(entt::to_integral(field.handle())) + 1}};
        f.text.clear();
        f.on_change.clear();
        f.max_length = 3;
        check(apiCall("UI.focus", nlohmann::json::array({field_id}))["result"] == true && system.typing(), "UI.focus da el foco");
        std::string clipboard;
        const auto key = [&](auto setup) {
            ui::UiInput k;
            k.set_clipboard = [&](const std::string& t) { clipboard = t; };
            setup(k);
            system.update(world, 960, 540, k, true, 0.0f);
            dispatch();
        };
        messages.clear();
        key([](ui::UiInput& k) { k.enter = true; });  // el Enter que dio el foco no envia
        check(system.typing() && messages.empty(), "el Enter del mismo frame que UI.focus no envia");
        key([](ui::UiInput& k) { k.typed = "a\xC3\xB1oX"; });  // "anoX" con enye: 5 bytes, 4 caracteres
        check(f.text == "a\xC3\xB1o", "max_length = 3 caracteres (la enye cuenta 1, no 2)");
        f.max_length = 0;
        key([](ui::UiInput& k) { k.key_left = true; });
        key([](ui::UiInput& k) { k.key_left = true; });
        check(f.caret == 1, "flechas: el cursor salta la enye entera");
        key([](ui::UiInput& k) { k.typed = "-"; });
        check(f.text == "a-\xC3\xB1o" && f.caret == 2, "escribir en medio");
        key([](ui::UiInput& k) { k.key_home = true; });
        key([](ui::UiInput& k) { k.key_right = true; k.shift = true; });
        key([](ui::UiInput& k) { k.key_right = true; k.shift = true; });
        key([](ui::UiInput& k) { k.copy = true; });
        check(clipboard == "a-" && f.anchor == 0 && f.caret == 2, "Mayus+flechas seleccionan y Ctrl+C copia");
        key([](ui::UiInput& k) { k.paste = true; k.clipboard = "X\nY"; });
        check(f.text == "X Y\xC3\xB1o", "Ctrl+V reemplaza la seleccion (saltos de linea = espacio)");
        key([](ui::UiInput& k) { k.backspace = true; });
        key([](ui::UiInput& k) { k.key_delete = true; });
        check(f.text == "X o", "retroceso y suprimir borran un caracter entero");
        key([](ui::UiInput& k) { k.select_all = true; });
        key([](ui::UiInput& k) { k.cut = true; });
        check(f.text.empty() && clipboard == "X o", "Ctrl+A y Ctrl+X");
        f.password = true;
        key([](ui::UiInput& k) { k.typed = "clave"; });
        clipboard.clear();
        key([](ui::UiInput& k) { k.select_all = true; });
        key([](ui::UiInput& k) { k.copy = true; });
        check(clipboard.empty(), "contrasena: no se copia");
        f.password = false;
        // Clic dentro: el cursor donde cae (sin fuente: medio tamano por letra).
        system.rectOf(field.handle(), fr);
        const float letter = 0.5f * f.font_size * 0.5f;  // escala 0.5
        in = ui::UiInput{};
        in.mouse_x = fr.x + 5.0f + letter * 2.0f + 1.0f; in.mouse_y = fr.y + fr.h * 0.5f;
        in.mouse_pressed = true; in.mouse_down = true;
        system.update(world, 960, 540, in, true, 0.0f);
        in.mouse_pressed = false; in.mouse_x += letter * 2.0f;  // arrastrar: selecciona
        system.update(world, 960, 540, in, true, 0.0f);
        check(f.anchor == 2 && f.caret == 4, "clic coloca el cursor y arrastrar selecciona");
        messages.clear();
        key([](ui::UiInput& k) { k.enter = true; });
        check(!system.typing() && last(menu, "OnEnviar") != nullptr && last(menu, "OnEnviar")->arg == "clave",
              "Enter envia y quita el foco");
        // La lista de dibujo: texto, seleccion recortados al campo.
        apiCall("UI.focus", nlohmann::json::array({field_id}));
        system.update(world, 960, 540, ui::UiInput{}, true, 0.25f);
        bool clipped_text = false, caret_rect = false;
        for (const ui::UiDrawCommand& c : system.drawList()) {
            if (c.entity != field.handle()) continue;
            if (c.type == ui::UiDrawCommand::Type::Text && c.text == "clave" && c.clipped) clipped_text = true;
            if (c.type == ui::UiDrawCommand::Type::Rect && c.rect.w < 2.0f) caret_rect = true;
        }
        check(clipped_text && caret_rect, "dibujo: texto recortado y cursor");
        apiCall("UI.focus", nlohmann::json::array({nullptr}));
        check(!system.typing(), "UI.focus(nil) quita el foco");
    }

    std::printf("P5: solo los Canvas (vista de EnTT), con el mismo orden que antes\n");
    {
        ecs::World w2;
        ui::UiSystem ui2;
        // Mucho mundo sin interfaz y tres Canvas raiz (uno dentro de un objeto).
        for (int i = 0; i < 2000; ++i) w2.create("Roca");
        const auto makeCanvas = [&](const char* name, int order, ecs::Entity parent) {
            ecs::Entity c = w2.create(name, parent);
            c.add<ui::Canvas>().sort_order = order;
            ecs::Entity img = w2.create("Imagen", c);
            img.add<ui::RectTransform>().size = core::Vec2{400, 400};
            img.add<ui::Image>();
            return img;
        };
        ecs::Entity holder = w2.create("Ventanas");
        ecs::Entity a = makeCanvas("A", 0, ecs::Entity{});
        ecs::Entity b = makeCanvas("B", 0, holder);
        ecs::Entity c = makeCanvas("C", -5, ecs::Entity{});
        // Un Canvas dentro de otro no es raiz (se pinta con su padre).
        ecs::Entity nested = w2.create("Anidado", a.parent());
        nested.add<ui::Canvas>();
        nested.add<ui::RectTransform>().size = core::Vec2{0, 0};  // (se coloca como un elemento de A)
        ui2.update(w2, 1920, 1080, ui::UiInput{}, false, 0.0f);
        // Recorrido de la Jerarquia: Ventanas > B, luego A: A encima.
        check(ui2.pick(960, 540) == a.handle(), "empate de sort_order: gana el ultimo de la Jerarquia");
        holder.setSiblingIndex(5000);
        ui2.update(w2, 1920, 1080, ui::UiInput{}, false, 0.0f);
        check(ui2.pick(960, 540) == b.handle(), "al mover Ventanas al final de la Jerarquia, gana B");
        c.parent().get<ui::Canvas>().sort_order = 10;
        ui2.update(w2, 1920, 1080, ui::UiInput{}, false, 0.0f);
        check(ui2.pick(960, 540) == c.handle(), "sort_order mayor, encima");
        c.parent().setActive(false);
        ui2.update(w2, 1920, 1080, ui::UiInput{}, false, 0.0f);
        ui::UiRect tmp;
        check(ui2.pick(960, 540) == b.handle() && !ui2.rectOf(c.handle(), tmp), "un Canvas inactivo no se pinta");
    }

    std::printf("P7: UIImage con uv_rect (atlas) y 9-slice; escenas viejas igual\n");
    {
        ecs::Entity icon = world.create("Icono", canvas);
        icon.add<ui::RectTransform>().size = core::Vec2{64, 64};
        ui::Image& img = icon.add<ui::Image>();
        img.texture = "UI/iconos.png";
        system.update(world, 960, 540, ui::UiInput{}, true, 0.0f);
        const auto command = [&]() -> const ui::UiDrawCommand* {
            for (const ui::UiDrawCommand& c : system.drawList()) {
                if (c.entity == icon.handle() && c.type == ui::UiDrawCommand::Type::Image) return &c;
            }
            return nullptr;
        };
        const ui::UiDrawCommand* c0 = command();
        check(c0 != nullptr && c0->uv0.x == 0.0f && c0->uv1.x == 1.0f && c0->uv1.y == 1.0f && c0->slice.x == 0.0f,
              "por defecto: toda la imagen, sin 9-slice");
        // Atlas de 4x4 iconos: el (2, 1) por la API.
        const nlohmann::json icon_id = {{"$e", static_cast<std::uint64_t>(entt::to_integral(icon.handle())) + 1}};
        bridge({{"op", "call"}, {"fn", "setUvRect"}, {"self", icon_id}, {"args", {0.5, 0.25, 0.25, 0.25}}});
        img.slice_left_top = core::Vec2{12, 10};
        img.slice_right_bottom = core::Vec2{12, 14};
        img.slice_scale = 2.0f;
        system.update(world, 960, 540, ui::UiInput{}, true, 0.0f);
        const ui::UiDrawCommand* c1 = command();
        check(c1 != nullptr && near(c1->uv0.x, 0.5f) && near(c1->uv0.y, 0.25f) && near(c1->uv1.x, 0.75f) && near(c1->uv1.y, 0.5f),
              "Entity:setUvRect -> uv de la lista de dibujo");
        check(c1 != nullptr && near(c1->slice.x, 12.0f) && near(c1->slice.w, 14.0f) && near(c1->slice_scale, 1.0f) && c1->slice_fill,
              "9-slice: bordes en pixeles y escala (2 x 0,5 del Canvas)");
        const nlohmann::json uv = bridge({{"op", "call"}, {"fn", "uvRect"}, {"self", icon_id}, {"args", nlohmann::json::array()}})["result"];
        check(uv.is_object() && near(uv["x"].get<float>(), 0.5f) && near(uv["width"].get<float>(), 0.25f), "Entity:uvRect");
        // Guardar y volver a leer: los campos nuevos se conservan.
        const std::string saved = ecs::serializeWorld(world);
        ecs::World copy;
        check(ecs::deserializeWorld(copy, saved), "(serializar el mundo)");
        const ecs::Entity icon_copy = copy.find(icon.uuid());
        const ui::Image* ic = icon_copy.valid() ? icon_copy.tryGet<ui::Image>() : nullptr;
        check(ic != nullptr && near(ic->uv_rect_pos.x, 0.5f) && near(ic->uv_rect_size.y, 0.25f) && near(ic->slice_left_top.y, 10.0f) &&
                  near(ic->slice_scale, 2.0f) && ic->raycast_target,
              "los campos nuevos se guardan y se leen");
        // Un UIImage de antes (sin los campos nuevos) se lee con los valores por defecto.
        ecs::Entity old = world.create("Viejo", canvas);
        check(ecs::componentFromJson(world, old, "UIImage", R"({"texture": "UI/marco.png", "alpha": 0.5, "corner_radius": 4})"),
              "(UIImage de una escena vieja)");
        const ui::Image& oi = old.get<ui::Image>();
        check(oi.texture == "UI/marco.png" && oi.uv_rect_pos.x == 0.0f && oi.uv_rect_size.x == 1.0f && oi.uv_rect_size.y == 1.0f &&
                  oi.slice_left_top.x == 0.0f && oi.slice_right_bottom.y == 0.0f && oi.slice_scale == 1.0f && oi.raycast_target &&
                  oi.slice_fill_center,
              "UIImage viejo: uv_rect, 9-slice y raycast_target por defecto (se ve igual)");
        check(ecs::componentFromJson(world, old, "UIEventTrigger", "{}") && old.get<ui::EventTrigger>().on_drop.empty() &&
                  near(old.get<ui::EventTrigger>().drag_threshold, 6.0f),
              "UIEventTrigger se crea desde JSON (MCP set_component)");
        world.destroy(old);
        world.destroy(icon);
    }
    scripts.stop();
    std::printf("\n%d fallos\n", failures);
    return failures == 0 ? 0 : 1;
}
