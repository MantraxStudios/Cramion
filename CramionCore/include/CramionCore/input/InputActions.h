#ifndef CRAMION_CORE_INPUT_ACTIONS_H
#define CRAMION_CORE_INPUT_ACTIONS_H

// Entrada por acciones, como el Enhanced Input de Unreal:
//
//   Accion (Input Action)    "Move" (Vec2), "Jump" (Bool), "Zoom" (Float),
//                            "Fly" (Vec3)... lo que el juego quiere hacer, con
//                            su tipo de valor.
//   Contexto (Mapping        "Default", "Vehiculo", "Menu"... Cada uno asigna
//   Context)                 a las acciones muchas teclas, botones del raton o
//                            del mando (W, S, Stick izquierdo, cruceta...).
//                            Se activan varios a la vez con prioridad: el de
//                            mas prioridad se queda las teclas que comparten.
//   Modificadores            cambian el valor de una tecla: Negate, Swizzle
//                            (YXZ: una tecla 1D mueve el eje Y), Dead Zone,
//                            Scale. Ej.: Move = W (Swizzle), S (Swizzle +
//                            Negate), D, A (Negate), Stick izquierdo.
//   Triggers                 cuando se dispara: Down (por defecto), Pressed,
//                            Released, Hold, Hold And Release, Tap, Pulse,
//                            Chord (otra accion a la vez).
//
// Estados y eventos por frame (como ETriggerEvent): Started, Ongoing,
// Triggered, Completed, Canceled.
//
// Se guarda en el proyecto: ProjectSettings/InputActions.json (Archivo >
// Entrada del proyecto). En Lua: Input.getAction("Move"),
// Input.bindAction("Jump", "triggered", fn), Input.addMappingContext(...),
// Input.rebind(...).

#include <CramionDM/Input.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace cramion::input {

enum class ValueType : std::uint8_t { Bool = 0, Axis1D, Axis2D, Axis3D };

// Valor de una accion: Bool y Axis1D usan x; Axis2D x,y; Axis3D x,y,z.
struct ActionValue {
    float x = 0.0f, y = 0.0f, z = 0.0f;
    float magnitude() const;
    bool actuated(float threshold = 0.5f) const { return magnitude() >= threshold; }
};

// --- Modificadores (se aplican en orden) ---
enum class ModifierType : std::uint8_t { Negate = 0, Swizzle, DeadZone, Scale, Count };
enum class SwizzleOrder : std::uint8_t { YXZ = 0, ZYX, XZY, YZX, ZXY };
struct Modifier {
    ModifierType type = ModifierType::Negate;
    std::array<bool, 3> axes{true, true, true};         // Negate: que ejes
    SwizzleOrder order = SwizzleOrder::YXZ;             // Swizzle
    float lower = 0.2f, upper = 1.0f;                   // DeadZone
    bool radial = true;                                 // DeadZone: por magnitud (sticks) o por eje
    std::array<float, 3> scale{1.0f, 1.0f, 1.0f};       // Scale
};

// --- Triggers ---
enum class TriggerType : std::uint8_t { Down = 0, Pressed, Released, Hold, HoldAndRelease, Tap, Pulse, Chord, Count };
struct Trigger {
    TriggerType type = TriggerType::Down;
    float threshold = 0.5f;   // actuacion (magnitud minima)
    float time = 0.5f;        // Hold/HoldAndRelease: segundos; Tap: maximo; Pulse: intervalo
    bool one_shot = true;     // Hold: dispara una vez (si no, cada frame tras el tiempo)
    bool pulse_at_start = true;
    int pulse_limit = 0;      // Pulse: 0 = sin limite
    std::string chord;        // Chord: la otra accion
};

enum class TriggerState : std::uint8_t { None = 0, Ongoing, Triggered };

struct InputAction {
    std::string name = "Accion";
    ValueType type = ValueType::Bool;
    std::string description;
    // Varias teclas a la vez: Mayor valor absoluto por eje (por defecto, W+S
    // = 1) o Sumar (W+S = 0).
    bool accumulate = false;
    // Con la accion en marcha, las teclas no llegan a contextos de menos
    // prioridad (consume_input, como bConsumeInput).
    bool consume_input = true;
    std::vector<Modifier> modifiers;  // despues de los de cada tecla
    std::vector<Trigger> triggers;
};

// Una tecla/boton/eje asignado a una accion dentro de un contexto.
struct KeyMapping {
    std::string action;
    std::string key;  // "W", "Space", "Mouse Left", "Mouse XY", "Gamepad Left Stick", "Touch Stick", "XR Right Trigger"...
    std::vector<Modifier> modifiers;
    std::vector<Trigger> triggers;
    bool player_mappable = true;  // se puede cambiar en el juego (Input.rebind)
};

struct MappingContext {
    std::string name = "Default";
    std::string description;
    int priority = 0;
    bool active_at_start = true;  // se anade solo al empezar el juego
    std::vector<KeyMapping> mappings;
};

struct InputActionSettings {
    std::vector<InputAction> actions;
    std::vector<MappingContext> contexts;

    const InputAction* findAction(const std::string& name) const;
    InputAction* findAction(const std::string& name);
    const MappingContext* findContext(const std::string& name) const;
    MappingContext* findContext(const std::string& name);
};

// Move/Look/Jump/Sprint/Fire/Aim/Interact/Zoom y el contexto "Default" con
// teclado, raton, mando y joystick tactil. Un proyecto sin archivo usa esto.
InputActionSettings defaultInputActions();

std::filesystem::path inputActionsFile(const std::filesystem::path& settings_folder);
InputActionSettings loadInputActions(const std::filesystem::path& file);  // sin archivo: defaultInputActions()
bool saveInputActions(const std::filesystem::path& file, const InputActionSettings& settings);
std::string inputActionsToJson(const InputActionSettings& settings);
bool inputActionsFromJson(const std::string& text, InputActionSettings& out);

// --- Fuentes (teclas y ejes) por nombre ---
enum class SourceKind : std::uint8_t {
    None = 0,
    Key,
    MouseButton,
    MouseAxis,
    GamepadButton,
    GamepadAxis,
    GamepadStick,
    TouchStick,
    XrButton,  // mandos de VR (OpenXR): "XR Right Primary"...
    XrAxis,    // "XR Right Trigger", "XR Left Grip", "XR Left Stick X"...
    XrStick,   // "XR Left Stick", "XR Right Stick"
};
struct Source {
    SourceKind kind = SourceKind::None;
    int code = 0;
    ValueType dimension = ValueType::Bool;  // lo que da: Bool (botones), Axis1D, Axis2D
};
Source parseSource(const std::string& name);  // "w", "Space", "Gamepad A"... sin mayusculas
// Todos los nombres, en grupos (para las listas del editor).
struct SourceGroup {
    const char* title;
    std::vector<std::string> names;
};
const std::vector<SourceGroup>& sourceGroups();
// Lo que se pulso este frame ("W", "Mouse Left", "Gamepad A"...) o vacio:
// para menus de "pulsa una tecla" (Input.anyKeyPressed / el editor).
std::string pressedSourceName(const dm::Input& input);
ActionValue readSource(const Source& source, const dm::Input& input);

const char* valueTypeName(ValueType type);          // "Bool", "Axis1D (float)"...
const char* valueTypeId(ValueType type);            // "bool", "axis1d", "axis2d", "axis3d"
const char* modifierTypeName(ModifierType type);    // "Negate"...
const char* triggerTypeName(TriggerType type);      // "Down"...
const char* swizzleName(SwizzleOrder order);        // "YXZ"...
const char* triggerStateName(TriggerState state);   // "none", "ongoing", "triggered"

ActionValue applyModifier(const Modifier& modifier, ActionValue value, ValueType type);

// --- Evaluacion en el juego ---
enum TriggerEvent : std::uint8_t {
    EventNone = 0,
    EventStarted = 1 << 0,
    EventOngoing = 1 << 1,
    EventTriggered = 1 << 2,
    EventCompleted = 1 << 3,
    EventCanceled = 1 << 4,
};

struct ActionState {
    ActionValue value;           // con los modificadores, recortado a su tipo
    TriggerState state = TriggerState::None;
    std::uint8_t events = EventNone;  // los de este frame (TriggerEvent)
    float elapsed = 0.0f;        // segundos desde Started
    float triggered_time = 0.0f; // segundos en Triggered
};

class InputMapper {
public:
    void setSettings(const InputActionSettings& settings);
    const InputActionSettings& settings() const { return settings_; }
    InputActionSettings& editableSettings() { return settings_; }

    // Contextos activos (reinicia a los "active_at_start").
    void resetContexts();
    bool addContext(const std::string& name, int priority);  // false si no existe
    bool addContext(const std::string& name);                // con su prioridad
    void removeContext(const std::string& name);
    void clearContexts();
    bool hasContext(const std::string& name) const;
    std::vector<std::string> activeContexts() const;

    // Una vez por frame. input nullptr: todo suelto (escribiendo en un campo).
    void update(const dm::Input* input, float delta_seconds);

    const ActionState* state(const std::string& action) const;
    const std::vector<ActionState>& states() const { return states_; }

    // Cambiar una tecla en el juego. index = cual de las teclas de esa accion
    // en el contexto (0 = la primera). false si no existe o no es mappable.
    bool rebind(const std::string& context, const std::string& action, int index, const std::string& key);
    std::vector<std::pair<std::string, std::string>> bindings(const std::string& action) const;  // (contexto, tecla)
    // Teclas cambiadas por el jugador (para guardarlas) y cargarlas.
    std::string overridesJson() const;
    void applyOverridesJson(const std::string& text);
    void clearOverrides();

private:
    struct MappingRuntime {
        Source source;
        std::vector<float> held;        // por trigger: segundos actuado
        std::vector<int> pulses;
        std::vector<bool> was_actuated;
    };
    struct ActionRuntime {
        std::vector<float> held;
        std::vector<int> pulses;
        std::vector<bool> was_actuated;
    };
    struct ActiveContext {
        std::string name;
        int priority = 0;
        std::uint64_t order = 0;
    };

    void rebuild();
    TriggerState evalTriggers(const std::vector<Trigger>& triggers, const ActionValue& value, std::vector<float>& held,
                              std::vector<int>& pulses, std::vector<bool>& was_actuated, float dt,
                              float implicit_threshold) const;

    InputActionSettings base_;      // sin cambios del jugador
    InputActionSettings settings_;  // con ellos
    struct Override {
        std::string context, action;
        int index = 0;
        std::string key;
    };
    std::vector<Override> overrides_;
    std::vector<ActiveContext> active_;
    std::uint64_t next_order_ = 0;
    std::vector<ActionState> states_;             // por accion (indice en settings_.actions)
    std::vector<ActionRuntime> action_runtime_;
    std::vector<std::vector<MappingRuntime>> mapping_runtime_;  // [contexto][mapping]
};

}  // namespace cramion::input

#endif  // CRAMION_CORE_INPUT_ACTIONS_H
