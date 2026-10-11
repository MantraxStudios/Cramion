// Pruebas de native/InputApi.cpp: Input (teclado, raton, mando, tactil,
// acciones y bindAction), Screen y XR (sin casco).

#include "ApiTest.h"

#include "CramionCore/xr/XrRig.h"

#include <CramionDM/Input.h>
#include <CramionDM/TouchControls.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace cramion;
using namespace cramion::apitest;

namespace {

bool near(double a, double b) { return std::fabs(a - b) < 1e-3; }

bool nearVec(const Value& v, float x, float y, float z) {
    if (!v.isVec3()) return false;
    const core::Vec3 p = v.asVec3();
    return near(p.x, x) && near(p.y, y) && near(p.z, z);
}

void send(dm::Input& in, dm::Event e) { in.onEvent(e); }

void key(dm::Input& in, dm::Key k, bool down) {
    dm::Event e;
    e.type = down ? dm::EventType::KeyPressed : dm::EventType::KeyReleased;
    e.category = dm::EventCategory::Keyboard;
    e.key = k;
    send(in, e);
}

std::string readFile(const std::filesystem::path& file) {
    std::ifstream in(file);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void testKeysAndMouse() {
    std::printf("Teclado, raton y ejes\n");
    bool locked = false;
    int lock_calls = 0;
    int vibrated = 0;
    {
        ApiFixture t;  // sin entrada
        check(t.call("Input.getKey", {Value("W")}).truthy() == false, "sin Input: getKey es false");
        check(nearVec(t.call("Input.mousePosition"), 0, 0, 0), "sin Input: mousePosition es (0, 0, 0)");
        check(t.call("Input.touchCount").asNumber() == 0 && t.call("Input.getTouch", {Value(1)}).isNil(),
              "sin Input: no hay dedos");
        check(t.call("Input.anyKeyPressed").isNil(), "sin Input: anyKeyPressed es nil");
    }
    dm::Input in;
    ApiFixture t;
    t.scripts.setInput(&in);
    key(in, dm::Key::W, true);
    key(in, dm::Key::D, true);
    check(t.call("Input.getKey", {Value("w")}).truthy(), "getKey (sin mayusculas)");
    check(t.call("Input.getKeyDown", {Value("W")}).truthy(), "getKeyDown el frame de la pulsacion");
    check(!t.call("Input.getKeyUp", {Value("W")}).truthy(), "getKeyUp false mientras se mantiene");
    check(!t.call("Input.getKey", {Value("noesunatecla")}).truthy(), "tecla desconocida: false");
    check(near(t.call("Input.getAxis", {Value("Horizontal")}).asNumber(), 1.0) &&
              near(t.call("Input.getAxis", {Value("vertical")}).asNumber(), 1.0),
          "getAxis Horizontal / Vertical con D y W");
    check(t.call("Input.anyKeyPressed").isString(), "anyKeyPressed: la tecla de este frame");
    in.newFrame();
    key(in, dm::Key::W, false);
    check(t.call("Input.getKeyUp", {Value("w")}).truthy() && !t.call("Input.getKey", {Value("w")}).truthy(),
          "getKeyUp al soltar");
    check(t.call("Input.anyKeyPressed").isNil(), "anyKeyPressed: nil si no se pulso nada");

    dm::Event move;
    move.type = dm::EventType::MouseMoved;
    move.mouseX = 10.0f;
    move.mouseY = 20.0f;
    move.deltaX = 3.0f;
    move.deltaY = -4.0f;
    send(in, move);
    check(nearVec(t.call("Input.mousePosition"), 10, 20, 0), "mousePosition");
    check(nearVec(t.call("Input.mouseDelta"), 3, -4, 0), "mouseDelta");
    const json r = t.bridge({{"fn", "Input.mousePosition"}});
    check(r["ok"] == true && r["result"]["$v"][1] == 20.0, "mousePosition por el puente: Vec3");
    dm::Event click;
    click.type = dm::EventType::MouseButtonPressed;
    click.button = dm::MouseButton::Right;
    send(in, click);
    check(t.call("Input.getMouseButton", {Value(1)}).truthy() && t.call("Input.getMouseButtonDown", {Value(1)}).truthy(),
          "getMouseButton / Down (1 = derecho)");
    check(!t.call("Input.getMouseButton", {Value(0)}).truthy() && !t.call("Input.getMouseButton", {Value(99)}).truthy(),
          "otro boton y uno fuera de rango (se recorta a 4): false");
    const json bad = t.bridge({{"fn", "Input.getMouseButton"}, {"args", {"izq"}}});
    check(bad["ok"] == false, "getMouseButton con texto: error");

    t.scripts.setCursorLock([&](bool on) {
        locked = on;
        ++lock_calls;
    });
    t.call("Input.lockCursor");
    check(locked && t.call("Input.isCursorLocked").truthy(), "lockCursor() captura el raton");
    t.call("Input.lockCursor", {Value(false)});
    check(!locked && !t.call("Input.isCursorLocked").truthy() && lock_calls == 2, "lockCursor(false) lo suelta");

    t.scripts.setVibrate([&](int ms) { vibrated = ms; });
    t.call("Input.vibrate");
    check(vibrated == 60, "vibrate() son 60 ms");
    t.call("Input.vibrate", {Value(99999)});
    check(vibrated == 5000, "vibrate se recorta a 5000 ms");
}

void testTouchAndGamepad() {
    std::printf("Tactil y mando\n");
    dm::TouchControls tc;
    dm::Input in;
    ApiFixture t;
    t.scripts.setInput(&in);
    check(!t.call("Input.isMobile").truthy(), "isMobile false sin tocar la pantalla");
    dm::Event touch;
    touch.type = dm::EventType::TouchBegan;
    touch.touchId = 7;
    touch.mouseX = 100.0f;
    touch.mouseY = 200.0f;
    send(in, touch);
    check(t.call("Input.touchCount").asNumber() == 1, "touchCount");
    const Value f = t.call("Input.getTouch", {Value(1)});
    check(f.isObject() && f["id"].asNumber() == 7 && nearVec(f["position"], 100, 200, 0) && nearVec(f["start"], 100, 200, 0) &&
              f["phase"].asString() == "began",
          "getTouch(1): id, position, start y phase");
    check(t.call("Input.getTouch", {Value(0)}).isNil() && t.call("Input.getTouch", {Value(2)}).isNil(),
          "getTouch fuera de 1..touchCount: nil");
    check(t.call("Input.isMobile").truthy(), "isMobile tras tocar la pantalla");
    const json r = t.bridge({{"fn", "Input.getTouch"}, {"args", {1}}});
    check(r["ok"] == true && r["result"]["phase"] == "began" && r["result"]["delta"]["$v"][0] == 0.0,
          "getTouch por el puente (objeto)");

    dm::Event pad;
    pad.type = dm::EventType::GamepadButtonPressed;
    pad.gamepadButton = dm::GamepadButton::A;
    send(in, pad);
    check(t.call("Input.getGamepadButton", {Value("a")}).truthy() && t.call("Input.getGamepadButtonDown", {Value("A")}).truthy(),
          "getGamepadButton / Down");
    check(!t.call("Input.getGamepadButtonUp", {Value("a")}).truthy() && !t.call("Input.getGamepadButton", {Value("zz")}).truthy(),
          "getGamepadButtonUp false y boton desconocido false");
    check(t.call("Input.isGamepadConnected").truthy(), "isGamepadConnected");
    dm::Event axis;
    axis.type = dm::EventType::GamepadAxisMoved;
    axis.gamepadAxis = dm::GamepadAxis::LeftTrigger;
    axis.value = 0.7f;
    send(in, axis);
    check(near(t.call("Input.getGamepadAxis", {Value("LT")}).asNumber(), 0.7) &&
              t.call("Input.getGamepadAxis", {Value("nada")}).asNumber() == 0.0,
          "getGamepadAxis por nombre");

    // Controles tactiles en pantalla.
    check(!t.call("Input.touchControlsEnabled").truthy() && !t.call("Input.setTouchButton", {Value("Saltar"), Value(false)}).truthy(),
          "sin controles tactiles: no hacen nada");
    dm::TouchLayout layout;
    layout.buttons.push_back(dm::TouchButton{});  // "Saltar"
    tc.setLayout(layout);
    t.scripts.setTouchControls(&tc);
    check(t.call("Input.touchControlsEnabled").truthy(), "touchControlsEnabled");
    t.call("Input.setTouchControls", {Value(false)});
    t.call("Input.setTouchJoystick", {Value(false)});
    t.call("Input.setTouchLook", {Value(false)});
    check(!tc.layout().enabled && !tc.layout().joystick && !tc.layout().look, "setTouchControls / Joystick / Look");
    check(t.call("Input.setTouchButton", {Value("saltar"), Value(false)}).truthy() && !tc.layout().buttons[0].visible,
          "setTouchButton por su texto");
    check(!t.call("Input.setTouchButton", {Value("Nada"), Value(true)}).truthy() && t.logged("no hay un boton tactil"),
          "setTouchButton de un boton que no existe: false y aviso");
    t.scripts.setTouchControls(nullptr);
}

void testActions() {
    std::printf("Acciones y contextos\n");
    dm::Input in;
    ApiFixture t;
    t.scripts.setInput(&in);
    key(in, dm::Key::Space, true);
    key(in, dm::Key::D, true);
    t.frame();
    check(t.call("Input.getAction", {Value("Jump")}).isBool() && t.call("Input.getAction", {Value("Jump")}).truthy(),
          "getAction de un Bool: true");
    check(nearVec(t.call("Input.getAction", {Value("Move")}), 1, 0, 0), "getAction de un Vec2: Vec3 (1, 0, 0)");
    check(t.call("Input.getAction", {Value("Zoom")}).isNumber(), "getAction de un Axis1D: numero");
    check(nearVec(t.call("Input.getActionValue", {Value("Jump")}), 1, 0, 0), "getActionValue: Vec3");
    check(t.call("Input.getActionState", {Value("Jump")}).asString() == "triggered", "getActionState triggered");
    check(t.call("Input.isActionTriggered", {Value("Jump")}).truthy() && t.call("Input.wasActionStarted", {Value("Jump")}).truthy(),
          "isActionTriggered y wasActionStarted");
    check(!t.call("Input.isActionOngoing", {Value("Jump")}).truthy() && !t.call("Input.wasActionCanceled", {Value("Jump")}).truthy(),
          "isActionOngoing y wasActionCanceled false");
    check(t.call("Input.getActionElapsed", {Value("Jump")}).asNumber() >= 0.0, "getActionElapsed");
    check(t.call("Input.getAction", {Value("NoExiste")}).isNil() && t.logged("Input.getAction: no hay una accion \"NoExiste\""),
          "accion desconocida: nil y aviso");
    check(t.call("Input.getActionState", {Value("NoExiste")}).asString() == "none", "getActionState desconocida: none");
    in.newFrame();
    key(in, dm::Key::Space, false);
    t.frame();
    in.newFrame();
    check(t.call("Input.wasActionCompleted", {Value("Jump")}).truthy(), "wasActionCompleted al soltar");

    const Value actions = t.call("Input.getActions");
    bool jump = false;
    for (const Value& a : actions.items()) jump = jump || a.asString() == "Jump";
    check(actions.isArray() && actions.size() == 9 && jump, "getActions: las 9 acciones por defecto");

    check(t.call("Input.getMappingContexts").size() == 1 && t.call("Input.getMappingContexts")[0].asString() == "Default",
          "getMappingContexts: Default al empezar");
    check(t.call("Input.addMappingContext", {Value("Vuelo")}).truthy() && t.call("Input.hasMappingContext", {Value("Vuelo")}).truthy(),
          "addMappingContext / hasMappingContext");
    check(t.call("Input.getMappingContexts").size() == 2, "dos contextos activos");
    check(!t.call("Input.addMappingContext", {Value("Nada"), Value(3)}).truthy() && t.logged("no hay un contexto \"Nada\""),
          "addMappingContext de uno que no existe: false y aviso");
    t.call("Input.removeMappingContext", {Value("Vuelo")});
    check(!t.call("Input.hasMappingContext", {Value("Vuelo")}).truthy(), "removeMappingContext");
    t.call("Input.clearMappingContexts");
    const json r = t.bridge({{"fn", "Input.getMappingContexts"}});
    check(r["ok"] == true && r["result"].is_array() && r["result"].empty(), "clearMappingContexts: lista vacia por el puente");
}

void testBindAction() {
    std::printf("bindAction\n");
    std::vector<std::pair<std::uint64_t, json>> calls;
    dm::Input in;
    ApiFixture t;
    t.scripts.setInput(&in);
    t.scripts.setBridgeCallbackSink(
        [&calls](std::uint64_t id, const std::string& args) { calls.emplace_back(id, json::parse(args)); });
    const json r = t.bridge({{"fn", "Input.bindAction"}, {"args", {"Jump", "started", json::object({{"$f", 5}})}}});
    check(r["ok"] == true && r["result"].is_number() && r["result"].get<double>() >= 1.0, "bindAction devuelve un id");
    const Value move_id = t.call("Input.bindAction", {Value("Move"), Value("Triggered"), Value::function(6)});
    check(move_id.asNumber() > r["result"].get<double>(), "otro enlace: otro id");
    check(t.call("Input.bindAction", {Value("Jump"), Value("pulsado"), Value::function(7)}).asNumber() == 0 &&
              t.logged("'pulsado' no es un evento"),
          "evento desconocido: 0 y aviso");
    check(t.call("Input.bindAction", {Value("Nada"), Value("started"), Value::function(7)}).asNumber() == 0 &&
              t.logged("Input.bindAction: no hay una accion"),
          "accion desconocida: 0 y aviso");
    check(t.bridge({{"fn", "Input.bindAction"}, {"args", {"Jump", "started"}}})["ok"] == false, "sin funcion: error");

    t.frame();
    check(calls.empty(), "nada pulsado: no se llama a nadie");
    key(in, dm::Key::Space, true);
    key(in, dm::Key::D, true);
    t.frame();
    in.newFrame();
    bool jumped = false, moved = false;
    for (const auto& [id, args] : calls) {
        if (id == 5) jumped = args.is_array() && args.size() == 2 && args[0] == true && args[1].is_number();
        if (id == 6) moved = args.is_array() && args[0].contains("$v") && args[0]["$v"][0] == 1.0;
    }
    check(calls.size() == 2 && jumped, "started de Jump: fn(true, segundos)");
    check(moved, "triggered de Move: fn(Vec3, segundos)");
    calls.clear();
    t.frame();
    in.newFrame();
    check(calls.size() == 1 && calls[0].first == 6, "manteniendo: Move sigue (triggered), Jump no (started)");
    t.call("Input.unbindAction", {move_id});
    calls.clear();
    t.frame();
    check(calls.empty(), "unbindAction quita el enlace");

    // Al parar el juego se olvidan (son de los scripts que se van).
    t.scripts.stop();
    t.scripts.start(t.world);
    key(in, dm::Key::Space, false);
    t.frame();
    in.newFrame();
    key(in, dm::Key::Space, true);
    t.frame();
    check(calls.empty(), "tras stop/start no queda ningun enlace");
}

void testRebind() {
    std::printf("Reasignar teclas y guardarlas\n");
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "cramion_input_api";
    std::filesystem::create_directories(dir);
    const std::filesystem::path prefs = dir / "prefs.txt";
    std::filesystem::remove(prefs);
    {
        ApiFixture t(false);
        t.scripts.setPrefsFile(prefs);
        t.scripts.start(t.world);
        const Value b = t.call("Input.getBindings", {Value("Jump")});
        check(b.isArray() && b.size() == 3 && b[0]["context"].asString() == "Default" && b[0]["key"].asString() == "Space",
              "getBindings: {context, key}");
        check(t.call("Input.rebind", {Value("Default"), Value("Jump"), Value(1), Value("F")}).truthy(), "rebind (1 = la primera)");
        check(t.call("Input.getBindings", {Value("Jump")})[0]["key"].asString() == "F", "Jump con F");
        check(!t.call("Input.rebind", {Value("Default"), Value("Jump"), Value(1), Value("NoEsTecla")}).truthy() &&
                  t.logged("Input.rebind: no se pudo"),
              "rebind a una tecla invalida: false y aviso");
        t.call("Input.saveBindings");
        check(readFile(prefs).find("__input_bindings=") != std::string::npos, "saveBindings lo guarda en Prefs");
    }
    {
        ApiFixture t(false);
        t.scripts.setPrefsFile(prefs);
        t.scripts.start(t.world);
        check(t.call("Input.getBindings", {Value("Jump")})[0]["key"].asString() == "F", "la partida siguiente empieza con F");
        t.call("Input.resetBindings");
        check(t.call("Input.getBindings", {Value("Jump")})[0]["key"].asString() == "Space", "resetBindings vuelve a Space");
        check(readFile(prefs).find("__input_bindings") == std::string::npos, "resetBindings lo borra de Prefs");
    }
}

void testScreen() {
    std::printf("Screen\n");
    std::string mode = "auto";
    ApiFixture t;
    check(t.call("Screen.width").asNumber() == 0 && t.call("Screen.height").asNumber() == 0, "sin pantalla: 0 x 0");
    check(t.call("Screen.orientation").asString() == "landscape" && t.call("Screen.orientationMode").asString() == "auto",
          "sin pantalla: landscape y auto");
    check(!t.call("Screen.setOrientation", {Value("portrait")}).truthy(), "en PC setOrientation no hace nada");
    scripting::ScriptSystem::ScreenHost host;
    host.width = [] { return 1080; };
    host.height = [] { return 1920; };
    host.set_orientation = [&mode](const std::string& m) {
        if (m != "auto" && m != "landscape" && m != "portrait") return false;
        mode = m;
        return true;
    };
    host.orientation_mode = [&mode] { return mode; };
    t.scripts.setScreen(host);
    check(t.call("Screen.width").asNumber() == 1080 && t.call("Screen.height").asNumber() == 1920, "width / height");
    check(t.call("Screen.orientation").asString() == "portrait", "orientation segun el tamano");
    check(t.call("Screen.setOrientation", {Value("Landscape")}).truthy() && mode == "landscape" &&
              t.call("Screen.orientationMode").asString() == "landscape",
          "setOrientation (sin mayusculas) y orientationMode");
    check(!t.call("Screen.setOrientation", {Value("de lado")}).truthy() && t.logged("no es un modo"),
          "modo invalido: false y aviso");
    t.scripts.setScreen({});
}

void testXr() {
    std::printf("XR (sin casco)\n");
    xr::XrSystem system;
    xr::XrRig rig;
    dm::Input in;
    ApiFixture t;
    t.scripts.setInput(&in);
    check(!t.call("XR.isAvailable").truthy() && !t.call("XR.isRunning").truthy() && !t.call("XR.isFocused").truthy(),
          "sin XR: no disponible");
    check(t.call("XR.getSystemName").asString().empty() && t.call("XR.getTrackingOrigin").asString() == "floor",
          "sin XR: sin nombre y floor");
    check(t.call("XR.getHeadPosition").isNil() && t.call("XR.getHeadRotation").isNil() && t.call("XR.getHeadLocalPosition").isNil(),
          "sin XR: la cabeza es nil");
    check(t.call("XR.getControllerPosition", {Value("right")}).isNil() && !t.call("XR.isControllerActive", {Value("left")}).truthy(),
          "sin XR: los mandos tampoco");
    const json ray = t.bridge({{"fn", "XR.getAimRay"}, {"args", {"right"}}});
    check(ray["ok"] == true && ray["result"].is_array() && ray["result"].size() == 2 && ray["result"][0].is_null() &&
              ray["result"][1].is_null(),
          "getAimRay sin mando: [nil, nil]");
    check(!t.call("XR.setTrackingOrigin", {Value("eyes")}).truthy(), "setTrackingOrigin sin XR: false");
    check(nearVec(t.call("XR.getOriginPosition"), 0, 0, 0), "getOriginPosition sin rig: (0, 0, 0)");

    // Los botones de los mandos van por Input (como los rellena el casco).
    in.setXrButton(dm::XrButton::RightPrimary, true);
    in.setXrButton(dm::XrButton::RightGrip, true);
    check(t.call("XR.getButton", {Value("right"), Value("a")}).truthy() &&
              t.call("XR.getButtonDown", {Value("R"), Value("primary")}).truthy(),
          "getButton (a = primary) y getButtonDown");
    check(t.call("XR.getButton", {Value("derecha"), Value("squeeze")}).truthy(), "squeeze = grip, derecha = right");
    check(!t.call("XR.getButton", {Value("left"), Value("a")}).truthy() && !t.call("XR.getButtonUp", {Value("right"), Value("a")}).truthy(),
          "la otra mano y getButtonUp: false");
    check(!t.call("XR.getButton", {Value("medio"), Value("a")}).truthy() && t.logged("'medio' no es una mano"),
          "mano invalida: false y aviso");
    check(!t.call("XR.getButton", {Value("right"), Value("zz")}).truthy() && t.logged("'zz' no es un boton"),
          "boton invalido: false y aviso");

    // Con el sistema de XR pero sin sesion (no hay casco).
    t.scripts.setXr(&system, &rig);
    check(!t.call("XR.isAvailable").truthy() && !t.call("XR.isRunning").truthy(), "XrSystem sin sesion: no disponible");
    check(t.call("XR.setTrackingOrigin", {Value("Eyes")}).truthy() && system.trackingOrigin() == xr::TrackingOrigin::Eyes &&
              t.call("XR.getTrackingOrigin").asString() == "eyes",
          "setTrackingOrigin / getTrackingOrigin");
    check(!t.call("XR.setTrackingOrigin", {Value("techo")}).truthy() && t.logged("no es un origen"),
          "origen invalido: false y aviso");
    t.call("XR.vibrate", {Value("left")});
    check(t.call("XR.getTrigger", {Value("right")}).asNumber() == 0.0 && nearVec(t.call("XR.getThumbstick", {Value("left")}), 0, 0, 0),
          "sin sesion: gatillo 0 y stick (0, 0, 0)");
    check(nearVec(t.call("XR.getOriginPosition"), 0, 0, 0), "getOriginPosition del rig");
    t.scripts.setXr(nullptr, nullptr);
}

void testDocs() {
    std::printf("Documentacion\n");
    ApiFixture t(false);
    int missing = 0;
    int count = 0;
    for (const auto& e : t.api().entries()) {
        if (e.owner != "Input" && e.owner != "Screen" && e.owner != "XR") continue;
        ++count;
        if (e.doc.description.empty()) ++missing;
    }
    check(count == 47 + 5 + 22, "47 de Input, 5 de Screen y 22 de XR (como la API de Lua)");
    check(missing == 0, "todas con descripcion");
}

}  // namespace

int main() {
    testKeysAndMouse();
    testTouchAndGamepad();
    testActions();
    testBindAction();
    testRebind();
    testScreen();
    testXr();
    testDocs();
    return finish();
}
