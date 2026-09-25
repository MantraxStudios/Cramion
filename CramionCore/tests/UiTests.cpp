// Pruebas de la interfaz del juego (consola): anclas, escalado, controles y
// eventos hacia los scripts Lua. Devuelve 0 si todo va.
#include "CramionCore/ecs/World.h"
#include "CramionCore/scripting/Scripting.h"
#include "CramionCore/ui/UI.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>

using namespace cramion;

int failures = 0;
void check(bool ok, const char* what) {
    std::printf("  %s %s\n", ok ? "OK   " : "FALLO", what);
    if (!ok) ++failures;
}
bool near(float a, float b) { return std::abs(a - b) < 0.01f; }

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
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "cramion_ui_assets";
    std::filesystem::create_directories(root / "Scripts");
    std::ofstream(root / "Scripts" / "Menu.lua") << R"(
local M = {}
function M:OnJugar(boton) self.entity.name = "Pulsado:" .. boton.name end
function M:OnVolumen(v) self.volumen = v end
function M:OnEnviar(t) self.enviado = t end
return M
)";
    ui::registerUiComponents();
    scripting::registerScriptComponents();
    ecs::World world;
    ecs::Entity canvas = world.create("Canvas");
    canvas.add<ui::Canvas>().reference = core::Vec2{1920, 1080};
    ecs::Entity menu = world.create("Menu");
    menu.add<scripting::Script>().file = "Scripts/Menu.lua";
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

    scripting::ScriptSystem scripts;
    scripts.setAssetsRoot(root);
    scripts.start(world);
    ui::UiSystem system;
    const auto dispatch = [&] {
        for (const ui::UiEvent& e : system.takeEvents()) {
            if (e.kind == ui::UiEvent::Kind::Click) scripts.callMethod(e.target, e.method, e.source);
            if (e.kind == ui::UiEvent::Kind::Number) scripts.callMethod(e.target, e.method, e.number);
            if (e.kind == ui::UiEvent::Kind::Text) scripts.callMethod(e.target, e.method, e.text);
        }
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
    check(menu.name() == "Pulsado:Jugar", "clic en el boton llama a Menu:OnJugar(boton)");
    // Slider: arrastrar al 75 %.
    ui::UiRect sr;
    system.rectOf(slider.handle(), sr);
    in = ui::UiInput{};
    in.mouse_x = sr.x + sr.w * 0.75f; in.mouse_y = sr.y + sr.h * 0.5f; in.mouse_pressed = true; in.mouse_down = true;
    system.update(world, 960, 540, in, true, 0.0f);
    dispatch();
    std::string out;
    check(near(s.value, 0.75f) && scripts.run("return Scene.find('Pulsado:Jugar'):getScript().volumen", &out) &&
              std::abs(std::stof(out) - 0.75f) < 0.01f, "slider: valor y OnVolumen(valor)");
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
    check(f.text == "Hola" && scripts.run("return Scene.find('Pulsado:Jugar'):getScript().enviado", &out) && out == "Hola",
          "campo de texto: escribir y OnEnviar(texto)");
    check(scripts.run("local e = Scene.find('Volumen'); e.value = 0.2; return e.value", &out) && std::abs(std::stof(out) - 0.2f) < 0.01f,
          "Lua: entity.value del slider");
    scripts.stop();
    std::filesystem::remove_all(root);
    std::printf("\n%d fallos\n", failures);
    return failures == 0 ? 0 : 1;
}
