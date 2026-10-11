// Input (teclado, raton, mando, tactil y acciones), Screen y XR.

#include "Modules.h"

#include "CramionCore/xr/XrRig.h"

#include <CramionDM/Input.h>
#include <CramionDM/TouchControls.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace cramion::scripting::native {
namespace {

using core::Vec3;

// Input.bindAction: la funcion del script que se llama en los eventos de una accion.
struct ActionBinding {
    int id = 0;
    std::string action;
    std::uint8_t events = 0;  // input::TriggerEvent
    api::Value fn;
};

struct InputState {
    std::vector<ActionBinding> bindings;
    int next_binding = 1;
};

std::uint8_t triggerEvent(const std::string& name) {
    const std::string n = lowerText(name);
    if (n == "started") return input::EventStarted;
    if (n == "ongoing") return input::EventOngoing;
    if (n == "triggered") return input::EventTriggered;
    if (n == "completed") return input::EventCompleted;
    if (n == "canceled" || n == "cancelled") return input::EventCanceled;
    return 0;
}

const input::ActionState* actionState(Runtime& rt, const std::string& name, const char* where) {
    const input::ActionState* s = rt.actions.state(name);
    if (s == nullptr) rt.write(1, std::string(where) + ": no hay una accion \"" + name + "\" (Archivo > Entrada del proyecto)");
    return s;
}

// Bool -> true/false, Axis1D -> numero, Axis2D/3D -> Vec3.
api::Value actionValue(const Runtime& rt, const std::string& name, const input::ActionState& s) {
    const input::InputAction* a = rt.actions.settings().findAction(name);
    const input::ValueType type = a != nullptr ? a->type : input::ValueType::Axis3D;
    switch (type) {
        case input::ValueType::Bool: return api::Value(s.value.x > 0.5f);
        case input::ValueType::Axis1D: return api::Value(s.value.x);
        default: return api::Value(Vec3{s.value.x, s.value.y, s.value.z});
    }
}

api::Value textList(const std::vector<std::string>& items) {
    api::Value::Array out;
    out.reserve(items.size());
    for (const std::string& s : items) out.emplace_back(s);
    return api::Value(std::move(out));
}

// --- XR ---

bool xrOn(const Runtime& rt) { return rt.xr_system != nullptr && rt.xr_system->running(); }

bool xrHand(Runtime& rt, const std::string& name, xr::Hand& out) {
    const std::string n = lowerText(name);
    if (n == "left" || n == "l" || n == "izquierda") {
        out = xr::Hand::Left;
    } else if (n == "right" || n == "r" || n == "derecha") {
        out = xr::Hand::Right;
    } else {
        rt.write(1, "XR: '" + name + "' no es una mano (left, right)");
        return false;
    }
    return true;
}

bool xrButton(Runtime& rt, const std::string& hand, const std::string& name, dm::XrButton& out) {
    xr::Hand h{};
    if (!xrHand(rt, hand, h)) return false;
    std::string n = lowerText(name);
    if (n == "a" || n == "x") n = "primary";
    if (n == "b" || n == "y") n = "secondary";
    if (n == "stick") n = "thumbstick";
    if (n == "squeeze") n = "grip";
    for (int b = 0; b < static_cast<int>(xr::Button::Count); ++b) {
        if (n == xr::buttonName(static_cast<xr::Button>(b))) {
            out = static_cast<dm::XrButton>(static_cast<int>(h) * 6 + b);
            return true;
        }
    }
    rt.write(1, "XR: '" + name + "' no es un boton (trigger, grip, thumbstick, primary, secondary, menu)");
    return false;
}

xr::Pose xrWorld(const Runtime& rt, const xr::Pose& p) { return rt.xr_rig != nullptr ? rt.xr_rig->toWorld(p) : p; }

// pose: "grip" (la mano, por defecto) o "aim" (el puntero).
bool controllerPose(Runtime& rt, const std::string& hand, const std::string& pose, xr::Pose& out) {
    xr::Hand h{};
    if (!xrOn(rt) || !xrHand(rt, hand, h)) return false;
    const xr::Controller& c = rt.xr_system->controller(h);
    out = lowerText(pose) == "aim" ? c.aim : c.grip;
    return c.active && out.valid;
}

// --- Teclado, raton, mando y pantalla tactil ---

void registerDevices(Runtime& rt) {
    api::NativeApi& n = rt.native;
    n.function("Input.getKey", [&rt](api::Call& c) { return api::Value(rt.keyDown(c.string(0))); },
               {"\"W\"", "tecla mantenida", "bool"});
    n.function(
        "Input.getKeyDown",
        [&rt](api::Call& c) {
            const dm::Key k = rt.key(c.string(0));
            return api::Value(rt.input != nullptr && k != dm::Key::Unknown && rt.input->isKeyPressed(k));
        },
        {"\"Space\"", "tecla pulsada este frame", "bool"});
    n.function(
        "Input.getKeyUp",
        [&rt](api::Call& c) {
            const dm::Key k = rt.key(c.string(0));
            return api::Value(rt.input != nullptr && k != dm::Key::Unknown && rt.input->isKeyReleased(k));
        },
        {"\"E\"", "tecla soltada este frame", "bool"});

    const auto button = [](const api::Call& c) {
        return static_cast<dm::MouseButton>(std::clamp<long long>(c.integer(0), 0, 4));
    };
    n.function(
        "Input.getMouseButton",
        [&rt, button](api::Call& c) {
            const dm::MouseButton b = button(c);
            return api::Value(rt.input != nullptr && rt.input->isMouseButtonDown(b));
        },
        {"0", "boton del raton mantenido (0 izq, 1 der, 2 medio)", "bool"});
    n.function(
        "Input.getMouseButtonDown",
        [&rt, button](api::Call& c) {
            const dm::MouseButton b = button(c);
            return api::Value(rt.input != nullptr && rt.input->isMouseButtonPressed(b));
        },
        {"0", "boton pulsado este frame", "bool"});
    n.function(
        "Input.getMouseButtonUp",
        [&rt, button](api::Call& c) {
            const dm::MouseButton b = button(c);
            return api::Value(rt.input != nullptr && rt.input->isMouseButtonReleased(b));
        },
        {"0", "boton soltado", "bool"});
    n.function(
        "Input.mousePosition",
        [&rt](api::Call&) { return api::Value(rt.input != nullptr ? Vec3{rt.input->mouseX(), rt.input->mouseY(), 0.0f} : Vec3{}); },
        {"", "Vec3 con la posicion del raton", "Vec3"});
    n.function(
        "Input.mouseDelta",
        [&rt](api::Call&) {
            return api::Value(rt.input != nullptr ? Vec3{rt.input->mouseDeltaX(), rt.input->mouseDeltaY(), 0.0f} : Vec3{});
        },
        {"", "Vec3 con el movimiento", "Vec3"});
    n.function("Input.getAxis", [&rt](api::Call& c) { return api::Value(rt.axis(c.string(0))); },
               {"\"Horizontal\"", "-1..1: Horizontal (A/D), Vertical (W/S), Mouse X, Mouse Y", "numero"});
    // Raton capturado (primera persona): oculto, sin salir de la ventana y
    // mouseDelta sin tope en los bordes.
    n.function(
        "Input.lockCursor",
        [&rt](api::Call& c) {
            rt.lockCursor(c.boolean(0, true));
            return api::Value{};
        },
        {"true", "captura el raton (primera persona); false lo suelta"});
    n.function("Input.isCursorLocked", [&rt](api::Call&) { return api::Value(rt.cursor_locked); },
               {"", "esta capturado?", "bool"});

    // Pantalla tactil: los dedos de este frame (1..touchCount).
    n.function(
        "Input.touchCount",
        [&rt](api::Call&) { return api::Value(rt.input != nullptr ? static_cast<int>(rt.input->touches().size()) : 0); },
        {"", "dedos en la pantalla", "numero"});
    n.function(
        "Input.getTouch",
        [&rt](api::Call& c) -> api::Value {
            const long long index = c.integer(0);
            if (rt.input == nullptr || index < 1 || index > static_cast<long long>(rt.input->touches().size())) return {};
            const dm::Input::Touch& t = rt.input->touches()[static_cast<std::size_t>(index - 1)];
            static constexpr const char* kPhases[] = {"began", "moved", "stationary", "ended"};
            api::Value out = api::Value::object();
            out.set("id", api::Value(t.id));
            out.set("position", api::Value(Vec3{t.x, t.y, 0.0f}));
            out.set("delta", api::Value(Vec3{t.deltaX, t.deltaY, 0.0f}));
            out.set("start", api::Value(Vec3{t.startX, t.startY, 0.0f}));
            out.set("phase", api::Value(kPhases[static_cast<int>(t.phase)]));
            return out;
        },
        {"1", "{id, position, delta, start, phase} de un dedo (1..touchCount)", "touch o nil"});
    // El juego corre en un movil (Android) o se toco la pantalla.
    n.function(
        "Input.isMobile",
        [&rt](api::Call&) {
#if defined(__ANDROID__)
            (void)rt;
            return api::Value(true);
#else
            return api::Value(rt.input != nullptr && rt.input->touchScreen());
#endif
        },
        {"", "movil o pantalla tactil", "bool"});
    n.function(
        "Input.vibrate",
        [&rt](api::Call& c) {
            const int ms = static_cast<int>(std::clamp<long long>(c.integer(0, 60), 1, 5000));
            if (rt.vibrate) rt.vibrate(ms);
            return api::Value{};
        },
        {"60", "vibra el movil (ms)"});

    // Mando: botones por nombre (a, b, x, y, lb, rb, ls, rs, start, back,
    // up, down, left, right) y ejes (leftx, lefty, rightx, righty, lt, rt).
    n.function(
        "Input.getGamepadButton",
        [&rt](api::Call& c) {
            const std::string name = c.string(0);
            dm::GamepadButton b{};
            return api::Value(rt.input != nullptr && Runtime::gamepadButton(name, b) && rt.input->isGamepadButtonDown(b));
        },
        {"\"a\"", "boton del mando mantenido: a, b, x, y, lb, rb, ls, rs, start, back, up, down, left, right", "bool"});
    n.function(
        "Input.getGamepadButtonDown",
        [&rt](api::Call& c) {
            const std::string name = c.string(0);
            dm::GamepadButton b{};
            return api::Value(rt.input != nullptr && Runtime::gamepadButton(name, b) && rt.input->isGamepadButtonPressed(b));
        },
        {"\"a\"", "pulsado este frame", "bool"});
    n.function(
        "Input.getGamepadButtonUp",
        [&rt](api::Call& c) {
            const std::string name = c.string(0);
            dm::GamepadButton b{};
            return api::Value(rt.input != nullptr && Runtime::gamepadButton(name, b) && rt.input->isGamepadButtonReleased(b));
        },
        {"\"a\"", "soltado este frame", "bool"});
    n.function(
        "Input.getGamepadAxis",
        [&rt](api::Call& c) {
            const std::string name = lowerText(c.string(0));
            if (rt.input == nullptr) return api::Value(0.0f);
            for (int i = 0; i < static_cast<int>(dm::GamepadAxis::Count); ++i) {
                const auto a = static_cast<dm::GamepadAxis>(i);
                if (name == dm::gamepadAxisName(a)) return api::Value(rt.input->gamepadAxis(a));
            }
            return api::Value(0.0f);
        },
        {"\"leftx\"", "eje del mando: leftx, lefty, rightx, righty, lt, rt", "numero"});
    n.function("Input.isGamepadConnected",
               [&rt](api::Call&) { return api::Value(rt.input != nullptr && rt.input->gamepadConnected()); },
               {"", "hay un mando?", "bool"});

    // La tecla/boton pulsado este frame ("W", "Mouse Left", "Gamepad A") o nil.
    n.function(
        "Input.anyKeyPressed",
        [&rt](api::Call&) -> api::Value {
            if (rt.input == nullptr) return {};
            std::string name = input::pressedSourceName(*rt.input);
            if (name.empty()) return {};
            return api::Value(std::move(name));
        },
        {"", "tecla/boton pulsado este frame (\"W\", \"Gamepad A\") o nil", "texto o nil"});

    // Controles tactiles en pantalla (se disenan en el editor: Archivo >
    // Controles tactiles). Mostrar/ocultar todo, el joystick, la zona de
    // mirar o un boton por su texto.
    n.function(
        "Input.setTouchControls",
        [&rt](api::Call& c) {
            if (rt.touch_controls != nullptr) rt.touch_controls->setEnabled(c.boolean(0));
            return api::Value{};
        },
        {"true", "muestra u oculta los controles tactiles"});
    n.function(
        "Input.touchControlsEnabled",
        [&rt](api::Call&) { return api::Value(rt.touch_controls != nullptr && rt.touch_controls->layout().enabled); },
        {"", "estan visibles?", "bool"});
    n.function(
        "Input.setTouchJoystick",
        [&rt](api::Call& c) {
            if (rt.touch_controls != nullptr) rt.touch_controls->setJoystick(c.boolean(0));
            return api::Value{};
        },
        {"true", "joystick tactil"});
    n.function(
        "Input.setTouchLook",
        [&rt](api::Call& c) {
            if (rt.touch_controls != nullptr) rt.touch_controls->setLook(c.boolean(0));
            return api::Value{};
        },
        {"true", "zona para mirar arrastrando"});
    n.function(
        "Input.setTouchButton",
        [&rt](api::Call& c) {
            const std::string label = c.string(0);
            const bool visible = c.boolean(1);
            if (rt.touch_controls == nullptr) return api::Value(false);
            if (!rt.touch_controls->setButtonVisible(label, visible)) {
                rt.write(1, "Input.setTouchButton: no hay un boton tactil \"" + label + "\"");
                return api::Value(false);
            }
            return api::Value(true);
        },
        {"\"Saltar\", true", "muestra u oculta un boton tactil por su texto", "bool"});
}

// --- Acciones y contextos (Enhanced Input; Archivo > Entrada del proyecto) ---

void registerActions(Runtime& rt, const std::shared_ptr<InputState>& state) {
    api::NativeApi& n = rt.native;
    // Valor segun su tipo: Bool -> true/false, Axis1D -> numero, Axis2D/3D -> Vec3.
    n.function(
        "Input.getAction",
        [&rt](api::Call& c) -> api::Value {
            const std::string name = c.string(0);
            const input::ActionState* s = actionState(rt, name, "Input.getAction");
            return s != nullptr ? actionValue(rt, name, *s) : api::Value{};
        },
        {"\"Move\"", "valor de una accion: Bool -> true/false, Axis1D -> numero, Axis2D/3D -> Vec3",
         "bool, numero o Vec3"});
    n.function(
        "Input.getActionValue",
        [&rt](api::Call& c) {
            const input::ActionState* s = actionState(rt, c.string(0), "Input.getActionValue");
            return api::Value(s != nullptr ? Vec3{s->value.x, s->value.y, s->value.z} : Vec3{});
        },
        {"\"Move\"", "Vec3 con el valor de la accion", "Vec3"});
    n.function(
        "Input.getActionState",
        [&rt](api::Call& c) {
            const input::ActionState* s = actionState(rt, c.string(0), "Input.getActionState");
            return api::Value(input::triggerStateName(s != nullptr ? s->state : input::TriggerState::None));
        },
        {"\"Jump\"", "\"none\", \"ongoing\" o \"triggered\"", "texto"});
    struct EventQuery {
        const char* name;
        std::uint8_t event;
        const char* args;
        const char* description;
    };
    static constexpr EventQuery kEvents[] = {
        {"isActionTriggered", input::EventTriggered, "\"Fire\"", "disparada este frame (segun sus triggers)"},
        {"isActionOngoing", input::EventOngoing, "\"Fire\"", "en curso (Hold todavia sin completar...)"},
        {"wasActionStarted", input::EventStarted, "\"Jump\"", "empezo este frame"},
        {"wasActionCompleted", input::EventCompleted, "\"Jump\"", "termino este frame (se solto tras dispararse)"},
        {"wasActionCanceled", input::EventCanceled, "\"Jump\"", "se solto sin llegar a dispararse (Hold corto...)"},
    };
    for (const EventQuery& q : kEvents) {
        const std::string where = std::string("Input.") + q.name;
        const std::uint8_t ev = q.event;
        n.function(
            where,
            [&rt, where, ev](api::Call& c) {
                const input::ActionState* s = actionState(rt, c.string(0), where.c_str());
                return api::Value(s != nullptr && (s->events & ev) != 0);
            },
            {q.args, q.description, "bool"});
    }
    n.function(
        "Input.getActionElapsed",
        [&rt](api::Call& c) {
            const input::ActionState* s = actionState(rt, c.string(0), "Input.getActionElapsed");
            return api::Value(s != nullptr ? s->elapsed : 0.0f);
        },
        {"\"Fire\"", "segundos desde que empezo", "numero"});

    // Input.bindAction("Jump", "triggered", fn) -> id. Eventos: started,
    // ongoing, triggered, completed, canceled. fn(valor, segundos) llega al
    // script en la fase Input del frame en que pasa (ver abajo).
    n.function(
        "Input.bindAction",
        [&rt, state](api::Call& c) {
            const std::string name = c.string(0);
            const std::string event = c.string(1);
            const api::Value& fn = c.function(2);
            if (fn.isNil()) throw api::Error("argumento 3: se esperaba una funcion (llego nil)");
            const std::uint8_t ev = triggerEvent(event);
            if (ev == 0) {
                rt.write(1, "Input.bindAction: '" + event + "' no es un evento (started, ongoing, triggered, completed, canceled)");
                return api::Value(0);
            }
            if (actionState(rt, name, "Input.bindAction") == nullptr) return api::Value(0);
            const int id = state->next_binding++;
            state->bindings.push_back({id, name, ev, fn});
            return api::Value(id);
        },
        {"\"Jump\", \"triggered\", function(valor, t) end", "llama a la funcion en ese evento; devuelve un id", "numero"});
    n.function(
        "Input.unbindAction",
        [state](api::Call& c) {
            const long long id = c.integer(0);
            std::erase_if(state->bindings, [id](const ActionBinding& b) { return b.id == id; });
            return api::Value{};
        },
        {"id", "quita un bindAction"});
    n.function(
        "Input.getActions",
        [&rt](api::Call&) {
            std::vector<std::string> names;
            for (const input::InputAction& a : rt.actions.settings().actions) names.push_back(a.name);
            return textList(names);
        },
        {"", "nombres de todas las acciones del proyecto", "lista de texto"});

    // Contextos: varios activos a la vez; el de mas prioridad se queda las teclas.
    n.function(
        "Input.addMappingContext",
        [&rt](api::Call& c) {
            const std::string name = c.string(0);
            const bool ok = c.has(1) ? rt.actions.addContext(name, static_cast<int>(c.integer(1))) : rt.actions.addContext(name);
            if (!ok) rt.write(1, "Input.addMappingContext: no hay un contexto \"" + name + "\"");
            return api::Value(ok);
        },
        {"\"Vuelo\", 1", "activa un contexto (prioridad opcional)", "bool"});
    n.function(
        "Input.removeMappingContext",
        [&rt](api::Call& c) {
            rt.actions.removeContext(c.string(0));
            return api::Value{};
        },
        {"\"Vuelo\"", "desactiva un contexto"});
    n.function("Input.hasMappingContext", [&rt](api::Call& c) { return api::Value(rt.actions.hasContext(c.string(0))); },
               {"\"Vuelo\"", "esta activo?", "bool"});
    n.function(
        "Input.clearMappingContexts",
        [&rt](api::Call&) {
            rt.actions.clearContexts();
            return api::Value{};
        },
        {"", "desactiva todos los contextos"});
    n.function("Input.getMappingContexts", [&rt](api::Call&) { return textList(rt.actions.activeContexts()); },
               {"", "lista de contextos activos", "lista de texto"});

    // Reasignar teclas (menu de opciones): {{context=, key=}, ...}.
    n.function(
        "Input.getBindings",
        [&rt](api::Call& c) {
            api::Value::Array out;
            for (const auto& [context, key] : rt.actions.bindings(c.string(0))) {
                api::Value b = api::Value::object();
                b.set("context", api::Value(context));
                b.set("key", api::Value(key));
                out.push_back(std::move(b));
            }
            return api::Value(std::move(out));
        },
        {"\"Jump\"", "lista {context, key} de las teclas de una accion", "lista de {context, key}"});
    // index: 1 = la primera tecla de esa accion en ese contexto.
    n.function(
        "Input.rebind",
        [&rt](api::Call& c) {
            const std::string context = c.string(0);
            const std::string action = c.string(1);
            const long long index = c.integer(2);
            const std::string key = c.string(3);
            const bool ok = rt.actions.rebind(context, action, static_cast<int>(index - 1), key);
            if (!ok) {
                rt.write(1, "Input.rebind: no se pudo (" + context + " / " + action + " #" + std::to_string(index) + " -> " +
                                key + ")");
            }
            return api::Value(ok);
        },
        {"\"Default\", \"Jump\", 1, \"F\"", "cambia una tecla (1 = la primera de esa accion)", "bool"});
    // Las teclas cambiadas se guardan con Prefs (entre partidas).
    n.function(
        "Input.saveBindings",
        [&rt](api::Call&) {
            rt.prefs[Runtime::kBindingsPref] = rt.actions.overridesJson();
            rt.savePrefs();
            return api::Value{};
        },
        {"", "guarda las teclas cambiadas (entre partidas)"});
    n.function(
        "Input.resetBindings",
        [&rt](api::Call&) {
            rt.actions.clearOverrides();
            rt.prefs.erase(Runtime::kBindingsPref);
            rt.savePrefs();
            return api::Value{};
        },
        {"", "vuelve a las teclas del proyecto"});

    // Los enlaces de bindAction: despues de leer las acciones del frame (el
    // nucleo hace actions.update al principio de la fase Input), cada uno
    // cuyo evento paso recibe (valor, segundos desde que empezo).
    rt.onFrame(Phase::Input, [&rt, state](float) {
        if (state->bindings.empty()) return;
        // Copia: una funcion (Visual Scripts) puede anadir o quitar enlaces.
        const std::vector<ActionBinding> bindings = state->bindings;
        for (const ActionBinding& b : bindings) {
            const input::ActionState* s = rt.actions.state(b.action);
            if (s == nullptr || (s->events & b.events) == 0) continue;
            rt.native.invoke(b.fn, {actionValue(rt, b.action, *s), api::Value(s->elapsed)});
        }
    });
    // Sus funciones son de los scripts que se van.
    rt.onStop([state](bool) { state->bindings.clear(); });
}

// --- Screen: tamano y orientacion ---

void registerScreen(Runtime& rt) {
    api::NativeApi& n = rt.native;
    n.function("Screen.width", [&rt](api::Call&) { return api::Value(rt.screen.width ? rt.screen.width() : 0); },
               {"", "ancho en pixeles", "numero"});
    n.function("Screen.height", [&rt](api::Call&) { return api::Value(rt.screen.height ? rt.screen.height() : 0); },
               {"", "alto en pixeles", "numero"});
    // "landscape" o "portrait" segun el tamano actual.
    n.function(
        "Screen.orientation",
        [&rt](api::Call&) {
            const int w = rt.screen.width ? rt.screen.width() : 0;
            const int h = rt.screen.height ? rt.screen.height() : 0;
            return api::Value(w >= h ? "landscape" : "portrait");
        },
        {"", "\"landscape\" o \"portrait\" segun el tamano", "texto"});
    // auto (gira libre), landscape / portrait (gira solo entre las dos
    // horizontales o verticales), landscape_fixed / portrait_fixed (fija).
    n.function(
        "Screen.setOrientation",
        [&rt](api::Call& c) {
            const std::string mode = c.string(0);
            if (!rt.screen.set_orientation) return api::Value(false);  // PC: no hace nada
            if (!rt.screen.set_orientation(lowerText(mode))) {
                rt.write(1, "Screen.setOrientation: '" + mode +
                                "' no es un modo (auto, landscape, portrait, landscape_fixed, portrait_fixed)");
                return api::Value(false);
            }
            return api::Value(true);
        },
        {"\"landscape\"", "auto, landscape, portrait, landscape_fixed, portrait_fixed (moviles)", "bool"});
    n.function(
        "Screen.orientationMode",
        [&rt](api::Call&) { return api::Value(rt.screen.orientation_mode ? rt.screen.orientation_mode() : std::string("auto")); },
        {"", "el ultimo modo pedido", "texto"});
}

// --- XR: realidad virtual (OpenXR) ---
// Manos "left"/"right"; botones trigger, grip, thumbstick, primary (A/X),
// secondary (B/Y), menu.

void registerXr(Runtime& rt) {
    api::NativeApi& n = rt.native;
    n.function("XR.isAvailable", [&rt](api::Call&) { return api::Value(rt.xr_system != nullptr && rt.xr_system->available()); },
               {"", "hay casco y sesion de VR", "bool"});
    n.function("XR.isRunning", [&rt](api::Call&) { return api::Value(xrOn(rt)); },
               {"", "el casco esta mostrando el juego", "bool"});
    n.function("XR.isFocused", [&rt](api::Call&) { return api::Value(rt.xr_system != nullptr && rt.xr_system->focused()); },
               {"", "el juego tiene los mandos (sin el menu del sistema encima)", "bool"});
    n.function("XR.getSystemName",
               [&rt](api::Call&) { return api::Value(rt.xr_system != nullptr ? rt.xr_system->systemName() : std::string()); },
               {"", "nombre del casco", "texto"});
    n.function("XR.getRuntimeName",
               [&rt](api::Call&) { return api::Value(rt.xr_system != nullptr ? rt.xr_system->runtimeName() : std::string()); },
               {"", "runtime de OpenXR (SteamVR, Oculus...)", "texto"});
    n.function(
        "XR.getHeadPosition",
        [&rt](api::Call&) -> api::Value {
            if (!xrOn(rt) || !rt.xr_system->head().valid) return {};
            return api::Value(xrWorld(rt, rt.xr_system->head()).position);
        },
        {"", "Vec3 de la cabeza en el mundo (o nil)", "Vec3 o nil"});
    n.function(
        "XR.getHeadRotation",
        [&rt](api::Call&) -> api::Value {
            if (!xrOn(rt) || !rt.xr_system->head().valid) return {};
            return api::Value(xrWorld(rt, rt.xr_system->head()).orientation);
        },
        {"", "Quat de la cabeza en el mundo (o nil)", "Quat o nil"});
    // La cabeza dentro de la habitacion (metros desde el origen, sin el rig).
    n.function(
        "XR.getHeadLocalPosition",
        [&rt](api::Call&) -> api::Value {
            if (!xrOn(rt) || !rt.xr_system->head().valid) return {};
            return api::Value(rt.xr_system->head().position);
        },
        {"", "Vec3 de la cabeza dentro de la habitacion", "Vec3 o nil"});
    n.function(
        "XR.isControllerActive",
        [&rt](api::Call& c) {
            const std::string hand = c.string(0);
            xr::Hand h{};
            return api::Value(xrOn(rt) && xrHand(rt, hand, h) && rt.xr_system->controller(h).active);
        },
        {"\"right\"", "el mando esta encendido y se sigue", "bool"});
    n.function(
        "XR.getControllerPosition",
        [&rt](api::Call& c) -> api::Value {
            xr::Pose p;
            if (!controllerPose(rt, c.string(0), c.string(1, "grip"), p)) return {};
            return api::Value(xrWorld(rt, p).position);
        },
        {"\"right\", \"grip\"", "Vec3 de la mano (grip) o del puntero (aim), o nil", "Vec3 o nil"});
    n.function(
        "XR.getControllerRotation",
        [&rt](api::Call& c) -> api::Value {
            xr::Pose p;
            if (!controllerPose(rt, c.string(0), c.string(1, "grip"), p)) return {};
            return api::Value(xrWorld(rt, p).orientation);
        },
        {"\"right\", \"grip\"", "Quat de la mano o del puntero, o nil", "Quat o nil"});
    // Rayo del puntero: origen y direccion en el mundo (para Physics.raycast).
    // Dos resultados: [origen, direccion] ([nil, nil] sin mando).
    n.function(
        "XR.getAimRay",
        [&rt](api::Call& c) {
            xr::Pose p;
            if (!controllerPose(rt, c.string(0), "aim", p)) return api::Value(api::Value::Array{api::Value{}, api::Value{}});
            const xr::Pose w = xrWorld(rt, p);
            return api::Value(api::Value::Array{api::Value(w.position),
                                                api::Value(xr::rotate(w.orientation, Vec3{0.0f, 0.0f, -1.0f}))});
        },
        {"\"right\"", "origen, direccion del puntero (para Physics.raycast)", "lista [Vec3, Vec3]"});
    n.function(
        "XR.getTrigger",
        [&rt](api::Call& c) {
            const std::string hand = c.string(0);
            xr::Hand h{};
            return api::Value(xrOn(rt) && xrHand(rt, hand, h) ? rt.xr_system->controller(h).trigger : 0.0f);
        },
        {"\"right\"", "gatillo 0..1", "numero"});
    n.function(
        "XR.getGrip",
        [&rt](api::Call& c) {
            const std::string hand = c.string(0);
            xr::Hand h{};
            return api::Value(xrOn(rt) && xrHand(rt, hand, h) ? rt.xr_system->controller(h).grip_value : 0.0f);
        },
        {"\"right\"", "agarre 0..1", "numero"});
    // Stick (o trackpad) como Vec3(x, y, 0), en [-1, 1] con Y arriba.
    n.function(
        "XR.getThumbstick",
        [&rt](api::Call& c) {
            const std::string hand = c.string(0);
            xr::Hand h{};
            if (!xrOn(rt) || !xrHand(rt, hand, h)) return api::Value(Vec3{});
            const core::Vec2 v = rt.xr_system->controller(h).thumbstick;
            return api::Value(Vec3{v.x, v.y, 0.0f});
        },
        {"\"left\"", "Vec3(x, y, 0) del stick, -1..1", "Vec3"});
    n.function(
        "XR.getButton",
        [&rt](api::Call& c) {
            const std::string hand = c.string(0);
            const std::string name = c.string(1);
            dm::XrButton b{};
            return api::Value(rt.input != nullptr && xrButton(rt, hand, name, b) && rt.input->isXrButtonDown(b));
        },
        {"\"right\", \"a\"", "boton mantenido: trigger, grip, thumbstick, primary (a/x), secondary (b/y), menu", "bool"});
    n.function(
        "XR.getButtonDown",
        [&rt](api::Call& c) {
            const std::string hand = c.string(0);
            const std::string name = c.string(1);
            dm::XrButton b{};
            return api::Value(rt.input != nullptr && xrButton(rt, hand, name, b) && rt.input->isXrButtonPressed(b));
        },
        {"\"right\", \"trigger\"", "boton pulsado este frame", "bool"});
    n.function(
        "XR.getButtonUp",
        [&rt](api::Call& c) {
            const std::string hand = c.string(0);
            const std::string name = c.string(1);
            dm::XrButton b{};
            return api::Value(rt.input != nullptr && xrButton(rt, hand, name, b) && rt.input->isXrButtonReleased(b));
        },
        {"\"right\", \"trigger\"", "boton soltado este frame", "bool"});
    // Vibracion: intensidad 0..1, segundos y frecuencia en Hz (opcional).
    n.function(
        "XR.vibrate",
        [&rt](api::Call& c) {
            const std::string hand = c.string(0);
            const float amplitude = static_cast<float>(c.number(1, 0.5));
            const float seconds = static_cast<float>(c.number(2, 0.1));
            const float frequency = static_cast<float>(c.number(3, 0.0));
            xr::Hand h{};
            if (rt.xr_system != nullptr && xrHand(rt, hand, h)) rt.xr_system->vibrate(h, amplitude, seconds, frequency);
            return api::Value{};
        },
        {"\"right\", 0.5, 0.1", "vibracion: intensidad 0..1, segundos, hz"});
    // "floor" (de pie) o "eyes" (sentado). Con XR Origin manda su componente.
    n.function(
        "XR.setTrackingOrigin",
        [&rt](api::Call& c) {
            const std::string mode = c.string(0);
            if (rt.xr_system == nullptr) return api::Value(false);
            const std::string m = lowerText(mode);
            if (m != "floor" && m != "eyes") {
                rt.write(1, "XR.setTrackingOrigin: '" + mode + "' no es un origen (floor, eyes)");
                return api::Value(false);
            }
            rt.xr_system->setTrackingOrigin(m == "floor" ? xr::TrackingOrigin::Floor : xr::TrackingOrigin::Eyes);
            return api::Value(true);
        },
        {"\"floor\"", "floor (de pie) o eyes (sentado); con XR Origin manda el componente", "bool"});
    n.function(
        "XR.getTrackingOrigin",
        [&rt](api::Call&) {
            return api::Value(rt.xr_system != nullptr && rt.xr_system->trackingOrigin() == xr::TrackingOrigin::Eyes ? "eyes"
                                                                                                                   : "floor");
        },
        {"", "\"floor\" o \"eyes\"", "texto"});
    // Origen del rig en el mundo (la entidad con XR Origin, o la camara).
    n.function("XR.getOriginPosition",
               [&rt](api::Call&) { return api::Value(rt.xr_rig != nullptr ? rt.xr_rig->originPosition() : Vec3{}); },
               {"", "Vec3 del rig (XR Origin o la camara) en el mundo", "Vec3"});
}

}  // namespace

void registerInputApi(Runtime& rt) {
    const auto state = std::make_shared<InputState>();
    registerDevices(rt);
    registerActions(rt, state);
    registerScreen(rt);
    registerXr(rt);
}

}  // namespace cramion::scripting::native
