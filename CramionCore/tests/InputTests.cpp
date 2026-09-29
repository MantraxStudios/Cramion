// Pruebas de la entrada por acciones (Enhanced Input): valores, modificadores,
// triggers, contextos con prioridad, reasignar teclas y el JSON. Tambien la
// tabla Input de Lua. Devuelve 0 si todo va.

#include "CramionCore/ecs/World.h"
#include "CramionCore/input/InputActions.h"
#include "CramionCore/scripting/Scripting.h"
#include "CramionCore/xr/XrRig.h"

// windows.h (por Vulkan) define near/far.
#undef near
#undef far

#include <CramionDM/Input.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

using namespace cramion;

namespace {

int failures = 0;
int checks = 0;

void check(bool condition, const char* what) {
    ++checks;
    std::printf("  %s %s\n", condition ? "OK   " : "FALLO", what);
    if (!condition) ++failures;
}

bool near(float a, float b) { return std::fabs(a - b) < 1e-3f; }

void key(dm::Input& in, dm::Key k, bool down) {
    dm::Event e;
    e.type = down ? dm::EventType::KeyPressed : dm::EventType::KeyReleased;
    e.category = dm::EventCategory::Keyboard;
    e.key = k;
    in.onEvent(e);
}

// Un frame: la entrada ya tiene los eventos; update y newFrame.
void frame(input::InputMapper& m, dm::Input& in, float dt = 1.0f / 60.0f) {
    m.update(&in, dt);
    in.newFrame();
}

void testValues() {
    std::printf("Valores y modificadores\n");
    input::InputMapper m;
    m.setSettings(input::defaultInputActions());
    m.resetContexts();
    dm::Input in;
    key(in, dm::Key::W, true);
    frame(m, in);
    const input::ActionState* move = m.state("Move");
    check(move != nullptr && near(move->value.x, 0.0f) && near(move->value.y, 1.0f), "W -> Move (0, 1) con Swizzle");
    check(move->state == input::TriggerState::Triggered && (move->events & input::EventStarted), "Move empieza y se dispara");
    key(in, dm::Key::D, true);
    frame(m, in);
    check(near(move->value.x, 1.0f) && near(move->value.y, 1.0f), "W + D -> (1, 1)");
    key(in, dm::Key::S, true);
    frame(m, in);
    check(near(move->value.y, 0.0f), "W + S se anulan (Negate)");
    key(in, dm::Key::Up, true);
    frame(m, in);
    check(near(move->value.y, 0.0f), "W + Flecha arriba + S: mayor por sentido (no 2)");
    key(in, dm::Key::W, false);
    key(in, dm::Key::S, false);
    key(in, dm::Key::D, false);
    key(in, dm::Key::Up, false);
    frame(m, in);
    check(move->state == input::TriggerState::None && (move->events & input::EventCompleted), "al soltar: Completed");

    input::Modifier dz;
    dz.type = input::ModifierType::DeadZone;
    dz.lower = 0.2f;
    const input::ActionValue small = input::applyModifier(dz, {0.1f, 0.1f, 0.0f}, input::ValueType::Axis2D);
    check(near(small.magnitude(), 0.0f), "Dead Zone radial corta 0.14");
    input::Modifier sw;
    sw.type = input::ModifierType::Swizzle;
    sw.order = input::SwizzleOrder::XZY;
    const input::ActionValue v = input::applyModifier(sw, {0.5f, 1.0f, 0.0f}, input::ValueType::Axis3D);
    check(near(v.x, 0.5f) && near(v.y, 0.0f) && near(v.z, 1.0f), "Swizzle XZY: la Y del stick va a Z");
}

void testTriggers() {
    std::printf("Triggers\n");
    input::InputActionSettings s;
    const auto add = [&](const char* name, input::TriggerType type, float time) {
        input::InputAction a;
        a.name = name;
        input::Trigger t;
        t.type = type;
        t.time = time;
        a.triggers.push_back(t);
        s.actions.push_back(a);
    };
    add("Hold", input::TriggerType::Hold, 0.5f);
    add("Tap", input::TriggerType::Tap, 0.2f);
    add("Pressed", input::TriggerType::Pressed, 0.0f);
    add("Released", input::TriggerType::Released, 0.0f);
    add("Pulse", input::TriggerType::Pulse, 0.25f);
    input::MappingContext c;
    for (const char* n : {"Hold", "Tap", "Pressed", "Released", "Pulse"}) c.mappings.push_back({n, "Space", {}, {}, true});
    s.contexts.push_back(c);
    input::InputMapper m;
    m.setSettings(s);
    m.resetContexts();
    dm::Input in;
    key(in, dm::Key::Space, true);
    frame(m, in, 0.1f);
    check(m.state("Pressed")->state == input::TriggerState::Triggered, "Pressed: el primer frame");
    check(m.state("Hold")->state == input::TriggerState::Ongoing, "Hold: en curso");
    check(m.state("Tap")->state == input::TriggerState::Ongoing, "Tap: en curso");
    check(m.state("Released")->state == input::TriggerState::Ongoing, "Released: en curso mientras se mantiene");
    check(m.state("Pulse")->state == input::TriggerState::Triggered, "Pulse: al empezar");
    int pulses = 1;
    for (int i = 0; i < 6; ++i) {
        frame(m, in, 0.1f);
        if (m.state("Pulse")->state == input::TriggerState::Triggered) ++pulses;
    }
    check(m.state("Pressed")->state == input::TriggerState::None, "Pressed: no se repite");
    check(m.state("Hold")->events & input::EventCompleted || m.state("Hold")->state != input::TriggerState::Ongoing,
          "Hold: se disparo pasados 0.5 s");
    check(m.state("Tap")->events & input::EventCanceled || m.state("Tap")->state == input::TriggerState::None,
          "Tap: se cancela si se mantiene");
    check(pulses == 3, "Pulse: cada 0.25 s (3 en 0.7 s)");
    key(in, dm::Key::Space, false);
    frame(m, in, 0.1f);
    check(m.state("Released")->state == input::TriggerState::Triggered, "Released: al soltar");

    // Tap corto.
    key(in, dm::Key::Space, true);
    frame(m, in, 0.05f);
    key(in, dm::Key::Space, false);
    frame(m, in, 0.05f);
    check(m.state("Tap")->state == input::TriggerState::Triggered, "Tap: pulsar y soltar rapido");
    check(m.state("Hold")->events & input::EventCanceled, "Hold corto: Canceled");
}

void testContexts() {
    std::printf("Contextos, prioridad y reasignar\n");
    input::InputMapper m;
    m.setSettings(input::defaultInputActions());
    m.resetContexts();
    check(m.hasContext("Default") && !m.hasContext("Vuelo"), "Default activo, Vuelo no");
    check(m.addContext("Vuelo"), "anadir Vuelo");
    dm::Input in;
    key(in, dm::Key::W, true);
    key(in, dm::Key::E, true);
    frame(m, in);
    const input::ActionState* fly = m.state("Fly");
    check(near(fly->value.z, 1.0f) && near(fly->value.y, 1.0f), "Fly (Vec3): W -> z, E -> y");
    check(near(m.state("Move")->value.y, 0.0f), "Vuelo (prioridad 1) se queda la W: Move no la recibe");
    check(m.state("Interact")->state == input::TriggerState::None, "E tambien consumida por Vuelo");
    m.removeContext("Vuelo");
    frame(m, in);
    check(near(m.state("Move")->value.y, 1.0f), "sin Vuelo, W vuelve a Move");

    check(m.rebind("Default", "Jump", 0, "F"), "rebind Jump -> F");
    key(in, dm::Key::F, true);
    frame(m, in);
    check(m.state("Jump")->value.x > 0.5f, "Jump con F");
    const std::string saved = m.overridesJson();
    input::InputMapper other;
    other.setSettings(input::defaultInputActions());
    other.applyOverridesJson(saved);
    check(other.bindings("Jump").front().second == "F", "las teclas cambiadas se guardan y cargan");
    m.clearOverrides();
    check(m.bindings("Jump").front().second == "Space", "resetBindings vuelve a Space");
    check(!m.rebind("Default", "Jump", 0, "NoEsTecla"), "tecla invalida: false");
}

void testJson() {
    std::printf("JSON\n");
    const input::InputActionSettings a = input::defaultInputActions();
    const std::filesystem::path file = std::filesystem::temp_directory_path() / "cramion_input_test" / "InputActions.json";
    check(input::saveInputActions(file, a), "se guarda");
    const input::InputActionSettings b = input::loadInputActions(file);
    check(b.actions.size() == a.actions.size() && b.contexts.size() == a.contexts.size(), "mismas acciones y contextos");
    check(input::inputActionsToJson(a) == input::inputActionsToJson(b), "ida y vuelta identica");
    check(b.findAction("Fly")->type == input::ValueType::Axis3D && b.findAction("Zoom")->type == input::ValueType::Axis1D,
          "tipos Vec3 y float");
    input::InputActionSettings bad;
    check(!input::inputActionsFromJson("{ roto", bad), "JSON roto: false");
    check(input::loadInputActions(file.parent_path() / "no_existe.json").actions.size() == a.actions.size(),
          "sin archivo: por defecto");
    check(input::parseSource("gamepad left stick").kind == input::SourceKind::GamepadStick, "nombres sin mayusculas");
}

void testLua() {
    std::printf("Lua\n");
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "cramion_input_lua";
    std::filesystem::create_directories(root / "Scripts");
    std::ofstream(root / "Scripts" / "Jugador.lua", std::ios::binary) << R"(
local J = {}
function J:Awake()
    self.saltos = 0
    Input.bindAction("Jump", "started", function(v) self.saltos = self.saltos + 1 end)
end
function J:Update(dt)
    local mover = Input.getAction("Move")
    self.entity.name = string.format("%.0f,%.0f,%s,%s,%d", mover.x, mover.y, tostring(Input.getAction("Jump")),
        Input.getActionState("Jump"), self.saltos)
    if Input.getKeyDown("V") then Input.addMappingContext("Vuelo") end
end
return J
)";
    ecs::World world;
    scripting::registerScriptComponents();
    ecs::Entity e = world.create("x");
    e.add<scripting::Script>().file = "Scripts/Jugador.lua";
    scripting::ScriptSystem scripts;
    scripts.setAssetsRoot(root);
    dm::Input in;
    scripts.setInput(&in);
    scripts.start(world);
    key(in, dm::Key::D, true);
    key(in, dm::Key::Space, true);
    scripts.update(world, 0.016f);
    in.newFrame();
    check(e.name() == "1,0,true,triggered,1", "getAction Vec2/Bool, getActionState y bindAction");
    key(in, dm::Key::V, true);
    scripts.update(world, 0.016f);
    in.newFrame();
    check(scripts.inputMapper().hasContext("Vuelo"), "Input.addMappingContext desde Lua");
    scripts.stop();
}

// Mandos de VR (OpenXR): fuentes "XR ...", las acciones por defecto y la
// tabla XR de Lua sin casco.
void testXr() {
    std::printf("Mandos de VR\n");
    check(input::parseSource("XR Right Trigger").kind == input::SourceKind::XrAxis, "XR Right Trigger es un eje");
    check(input::parseSource("xr left stick").kind == input::SourceKind::XrStick &&
              input::parseSource("xr left stick").dimension == input::ValueType::Axis2D,
          "XR Left Stick es Vec2");
    check(input::parseSource("XR Right Primary").kind == input::SourceKind::XrButton, "XR Right Primary es un boton");
    input::InputMapper m;
    m.setSettings(input::defaultInputActions());
    m.resetContexts();
    dm::Input in;
    in.setXrAxis(dm::XrAxis::LeftStickX, 1.0f);
    in.setXrAxis(dm::XrAxis::RightTrigger, 1.0f);
    in.setXrButton(dm::XrButton::RightPrimary, true);
    check(input::pressedSourceName(in) == "XR Right Trigger" || input::pressedSourceName(in) == "XR Right Primary",
          "pulsar un mando VR sale en pressedSourceName");
    frame(m, in);
    const input::ActionState* move = m.state("Move");
    check(move != nullptr && move->value.x > 0.9f && near(move->value.y, 0.0f), "stick izquierdo -> Move");
    check(m.state("Fire") != nullptr && m.state("Fire")->state == input::TriggerState::Triggered, "gatillo derecho -> Fire");
    check(m.state("Jump") != nullptr && (m.state("Jump")->events & input::EventStarted), "A -> Jump");

    const std::filesystem::path root = std::filesystem::temp_directory_path() / "cramion_input_xr";
    std::filesystem::create_directories(root / "Scripts");
    std::ofstream(root / "Scripts" / "Vr.lua", std::ios::binary) << R"(
local V = {}
function V:Update(dt)
    self.entity.name = string.format("%s,%s,%s,%s", tostring(XR.isAvailable()), tostring(XR.getButton("right", "a")),
        tostring(XR.getButtonDown("right", "primary")), tostring(XR.getHeadPosition()))
end
return V
)";
    ecs::World world;
    scripting::registerScriptComponents();
    ecs::Entity e = world.create("x");
    e.add<scripting::Script>().file = "Scripts/Vr.lua";
    scripting::ScriptSystem scripts;
    scripts.setAssetsRoot(root);
    dm::Input lua_in;
    scripts.setInput(&lua_in);
    scripts.start(world);
    lua_in.setXrButton(dm::XrButton::RightPrimary, true);
    scripts.update(world, 0.016f);
    check(e.name() == "false,true,true,nil", "XR en Lua sin casco: botones por Input, sin poses");
    scripts.stop();

    // XR Origin desde Lua: de pie <-> sentado con getField/setField.
    xr::registerXrComponents();
    ecs::Entity rig = world.create("XR Origin");
    rig.add<xr::XrOrigin>();
    std::string out;
    scripts.run("local o = Scene.find('XR Origin')\n"
                "local antes = o:getField('XrOrigin', 'tracking')\n"
                "o:setField('XrOrigin', 'tracking', 'Sentado')\n"
                "o:setField('XrOrigin', 'camera_y_offset', 1.2)\n"
                "print(antes .. ',' .. o:getField('XrOrigin', 'tracking'))",
                &out, &world);
    const xr::XrOrigin& o = rig.get<xr::XrOrigin>();
    check(o.tracking == xr::TrackingOrigin::Eyes, "XrOrigin: setField('tracking', 'Sentado')");
    check(near(o.camera_y_offset, 1.2f), "XrOrigin: setField('camera_y_offset')");
}

}  // namespace

int main() {
    testValues();
    testTriggers();
    testContexts();
    testJson();
    testLua();
    testXr();
    std::printf("\n%d/%d comprobaciones correctas\n", checks - failures, checks);
    return failures == 0 ? 0 : 1;
}
