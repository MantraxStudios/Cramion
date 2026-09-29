#include "CramionCore/input/InputActions.h"

#include <CramionDM/TouchControls.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace cramion::input {

using json = nlohmann::json;

namespace {

// "Gamepad Left Stick" -> "gamepadleftstick" (sin mayusculas, espacios ni _).
std::string canonical(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (const char c : name) {
        if (c == ' ' || c == '_') continue;
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

struct SourceTable {
    std::vector<SourceGroup> groups;
    std::unordered_map<std::string, Source> by_name;
    std::vector<std::pair<Source, std::string>> display;  // la fuente -> su nombre bonito
};

const SourceTable& table() {
    static const SourceTable t = [] {
        SourceTable s;
        const auto add = [&](SourceGroup& g, const std::string& name, Source src) {
            g.names.push_back(name);
            s.by_name[canonical(name)] = src;
            s.display.emplace_back(src, name);
        };
        SourceGroup keys{"Teclado", {}};
        for (int k = 1; k < static_cast<int>(dm::Key::Count); ++k) {
            const char* name = dm::keyName(static_cast<dm::Key>(k));
            if (std::string(name) == "Unknown") continue;
            add(keys, name, Source{SourceKind::Key, k, ValueType::Bool});
        }
        SourceGroup mouse{"Raton", {}};
        add(mouse, "Mouse Left", {SourceKind::MouseButton, 0, ValueType::Bool});
        add(mouse, "Mouse Right", {SourceKind::MouseButton, 1, ValueType::Bool});
        add(mouse, "Mouse Middle", {SourceKind::MouseButton, 2, ValueType::Bool});
        add(mouse, "Mouse X1", {SourceKind::MouseButton, 3, ValueType::Bool});
        add(mouse, "Mouse X2", {SourceKind::MouseButton, 4, ValueType::Bool});
        add(mouse, "Mouse X", {SourceKind::MouseAxis, 0, ValueType::Axis1D});
        add(mouse, "Mouse Y", {SourceKind::MouseAxis, 1, ValueType::Axis1D});
        add(mouse, "Mouse XY", {SourceKind::MouseAxis, 2, ValueType::Axis2D});
        add(mouse, "Mouse Wheel", {SourceKind::MouseAxis, 3, ValueType::Axis1D});
        SourceGroup pad{"Mando", {}};
        static constexpr const char* kButtons[] = {"Gamepad A",  "Gamepad B",     "Gamepad X",          "Gamepad Y",
                                                   "Gamepad LB", "Gamepad RB",    "Gamepad LS",         "Gamepad RS",
                                                   "Gamepad Start", "Gamepad Back", "Gamepad DPad Up", "Gamepad DPad Down",
                                                   "Gamepad DPad Left", "Gamepad DPad Right"};
        for (int i = 0; i < static_cast<int>(dm::GamepadButton::Count); ++i) {
            add(pad, kButtons[i], {SourceKind::GamepadButton, i, ValueType::Bool});
        }
        add(pad, "Gamepad Left Stick", {SourceKind::GamepadStick, 0, ValueType::Axis2D});
        add(pad, "Gamepad Right Stick", {SourceKind::GamepadStick, 1, ValueType::Axis2D});
        static constexpr const char* kAxes[] = {"Gamepad Left X", "Gamepad Left Y", "Gamepad Right X",
                                                "Gamepad Right Y", "Gamepad LT", "Gamepad RT"};
        for (int i = 0; i < static_cast<int>(dm::GamepadAxis::Count); ++i) {
            add(pad, kAxes[i], {SourceKind::GamepadAxis, i, ValueType::Axis1D});
        }
        SourceGroup touch{"Tactil", {}};
        add(touch, "Touch Stick", {SourceKind::TouchStick, 0, ValueType::Axis2D});
        s.groups = {keys, mouse, pad, touch};
        // Alias (no salen en las listas).
        s.by_name["mouse0"] = {SourceKind::MouseButton, 0, ValueType::Bool};
        s.by_name["mouse1"] = {SourceKind::MouseButton, 1, ValueType::Bool};
        s.by_name["mouse2"] = {SourceKind::MouseButton, 2, ValueType::Bool};
        s.by_name["mousescrollwheel"] = {SourceKind::MouseAxis, 3, ValueType::Axis1D};
        s.by_name["gamepadlt"] = {SourceKind::GamepadAxis, 4, ValueType::Axis1D};
        s.by_name["gamepadrt"] = {SourceKind::GamepadAxis, 5, ValueType::Axis1D};
        return s;
    }();
    return t;
}

int sourceId(const Source& s) { return (static_cast<int>(s.kind) << 16) | s.code; }

ActionValue truncate(ActionValue v, ValueType type) {
    switch (type) {
        case ValueType::Bool:
        case ValueType::Axis1D: return {v.x, 0.0f, 0.0f};
        case ValueType::Axis2D: return {v.x, v.y, 0.0f};
        case ValueType::Axis3D: return v;
    }
    return v;
}

int rank(TriggerState s) { return static_cast<int>(s); }

// --- JSON ---
const char* modifierId(ModifierType t) {
    switch (t) {
        case ModifierType::Negate: return "negate";
        case ModifierType::Swizzle: return "swizzle";
        case ModifierType::DeadZone: return "deadzone";
        case ModifierType::Scale: return "scale";
        default: return "negate";
    }
}

const char* triggerId(TriggerType t) {
    switch (t) {
        case TriggerType::Down: return "down";
        case TriggerType::Pressed: return "pressed";
        case TriggerType::Released: return "released";
        case TriggerType::Hold: return "hold";
        case TriggerType::HoldAndRelease: return "holdandrelease";
        case TriggerType::Tap: return "tap";
        case TriggerType::Pulse: return "pulse";
        case TriggerType::Chord: return "chord";
        default: return "down";
    }
}

json modifiersJson(const std::vector<Modifier>& mods) {
    json out = json::array();
    for (const Modifier& m : mods) {
        json j = {{"type", modifierId(m.type)}};
        switch (m.type) {
            case ModifierType::Negate: j["axes"] = {m.axes[0], m.axes[1], m.axes[2]}; break;
            case ModifierType::Swizzle: j["order"] = swizzleName(m.order); break;
            case ModifierType::DeadZone:
                j["lower"] = m.lower;
                j["upper"] = m.upper;
                j["radial"] = m.radial;
                break;
            case ModifierType::Scale: j["scale"] = {m.scale[0], m.scale[1], m.scale[2]}; break;
            default: break;
        }
        out.push_back(j);
    }
    return out;
}

std::vector<Modifier> modifiersFrom(const json& list) {
    std::vector<Modifier> out;
    if (!list.is_array()) return out;
    for (const json& j : list) {
        if (!j.is_object()) continue;
        Modifier m;
        const std::string id = lower(j.value("type", std::string("negate")));
        for (int t = 0; t < static_cast<int>(ModifierType::Count); ++t) {
            if (id == modifierId(static_cast<ModifierType>(t))) m.type = static_cast<ModifierType>(t);
        }
        if (const auto a = j.find("axes"); a != j.end() && a->is_array() && a->size() == 3) {
            for (int i = 0; i < 3; ++i) m.axes[i] = (*a)[i].is_boolean() && (*a)[i].get<bool>();
        }
        const std::string order = j.value("order", std::string("YXZ"));
        for (int o = 0; o <= static_cast<int>(SwizzleOrder::ZXY); ++o) {
            if (order == swizzleName(static_cast<SwizzleOrder>(o))) m.order = static_cast<SwizzleOrder>(o);
        }
        m.lower = std::clamp(j.value("lower", m.lower), 0.0f, 1.0f);
        m.upper = std::clamp(j.value("upper", m.upper), m.lower + 0.001f, 10.0f);
        m.radial = j.value("radial", m.radial);
        if (const auto s = j.find("scale"); s != j.end() && s->is_array() && s->size() == 3) {
            for (int i = 0; i < 3; ++i) m.scale[i] = (*s)[i].is_number() ? (*s)[i].get<float>() : 1.0f;
        }
        out.push_back(m);
    }
    return out;
}

json triggersJson(const std::vector<Trigger>& triggers) {
    json out = json::array();
    for (const Trigger& t : triggers) {
        json j = {{"type", triggerId(t.type)}, {"threshold", t.threshold}};
        switch (t.type) {
            case TriggerType::Hold:
                j["time"] = t.time;
                j["one_shot"] = t.one_shot;
                break;
            case TriggerType::HoldAndRelease:
            case TriggerType::Tap: j["time"] = t.time; break;
            case TriggerType::Pulse:
                j["time"] = t.time;
                j["at_start"] = t.pulse_at_start;
                j["limit"] = t.pulse_limit;
                break;
            case TriggerType::Chord: j["action"] = t.chord; break;
            default: break;
        }
        out.push_back(j);
    }
    return out;
}

std::vector<Trigger> triggersFrom(const json& list) {
    std::vector<Trigger> out;
    if (!list.is_array()) return out;
    for (const json& j : list) {
        if (!j.is_object()) continue;
        Trigger t;
        const std::string id = lower(j.value("type", std::string("down")));
        for (int k = 0; k < static_cast<int>(TriggerType::Count); ++k) {
            if (id == triggerId(static_cast<TriggerType>(k))) t.type = static_cast<TriggerType>(k);
        }
        t.threshold = std::clamp(j.value("threshold", t.threshold), 0.0f, 10.0f);
        t.time = std::clamp(j.value("time", t.time), 0.0f, 600.0f);
        t.one_shot = j.value("one_shot", t.one_shot);
        t.pulse_at_start = j.value("at_start", t.pulse_at_start);
        t.pulse_limit = std::max(0, j.value("limit", t.pulse_limit));
        t.chord = j.value("action", t.chord);
        out.push_back(t);
    }
    return out;
}

ValueType valueTypeFromId(const std::string& id) {
    const std::string n = lower(id);
    for (int t = 0; t <= static_cast<int>(ValueType::Axis3D); ++t) {
        if (n == valueTypeId(static_cast<ValueType>(t))) return static_cast<ValueType>(t);
    }
    if (n == "float" || n == "axis1") return ValueType::Axis1D;
    if (n == "vec2" || n == "vector2" || n == "axis2") return ValueType::Axis2D;
    if (n == "vec3" || n == "vector3" || n == "axis3") return ValueType::Axis3D;
    return ValueType::Bool;
}

Modifier negate(bool x = true, bool y = true, bool z = true) {
    Modifier m;
    m.type = ModifierType::Negate;
    m.axes = {x, y, z};
    return m;
}
Modifier swizzle(SwizzleOrder order) {
    Modifier m;
    m.type = ModifierType::Swizzle;
    m.order = order;
    return m;
}
Modifier deadZone(float lower_bound = 0.2f) {
    Modifier m;
    m.type = ModifierType::DeadZone;
    m.lower = lower_bound;
    return m;
}
Modifier scale(float x, float y, float z) {
    Modifier m;
    m.type = ModifierType::Scale;
    m.scale = {x, y, z};
    return m;
}

}  // namespace

float ActionValue::magnitude() const { return std::sqrt(x * x + y * y + z * z); }

const InputAction* InputActionSettings::findAction(const std::string& name) const {
    const std::string n = lower(name);
    for (const InputAction& a : actions) {
        if (lower(a.name) == n) return &a;
    }
    return nullptr;
}
InputAction* InputActionSettings::findAction(const std::string& name) {
    return const_cast<InputAction*>(static_cast<const InputActionSettings*>(this)->findAction(name));
}
const MappingContext* InputActionSettings::findContext(const std::string& name) const {
    const std::string n = lower(name);
    for (const MappingContext& c : contexts) {
        if (lower(c.name) == n) return &c;
    }
    return nullptr;
}
MappingContext* InputActionSettings::findContext(const std::string& name) {
    return const_cast<MappingContext*>(static_cast<const InputActionSettings*>(this)->findContext(name));
}

const char* valueTypeName(ValueType type) {
    switch (type) {
        case ValueType::Bool: return "Bool (digital)";
        case ValueType::Axis1D: return "Axis1D (float)";
        case ValueType::Axis2D: return "Axis2D (Vec2)";
        case ValueType::Axis3D: return "Axis3D (Vec3)";
    }
    return "Bool";
}
const char* valueTypeId(ValueType type) {
    switch (type) {
        case ValueType::Bool: return "bool";
        case ValueType::Axis1D: return "axis1d";
        case ValueType::Axis2D: return "axis2d";
        case ValueType::Axis3D: return "axis3d";
    }
    return "bool";
}
const char* modifierTypeName(ModifierType type) {
    switch (type) {
        case ModifierType::Negate: return "Negate";
        case ModifierType::Swizzle: return "Swizzle";
        case ModifierType::DeadZone: return "Dead Zone";
        case ModifierType::Scale: return "Scale";
        default: return "?";
    }
}
const char* triggerTypeName(TriggerType type) {
    switch (type) {
        case TriggerType::Down: return "Down";
        case TriggerType::Pressed: return "Pressed";
        case TriggerType::Released: return "Released";
        case TriggerType::Hold: return "Hold";
        case TriggerType::HoldAndRelease: return "Hold And Release";
        case TriggerType::Tap: return "Tap";
        case TriggerType::Pulse: return "Pulse";
        case TriggerType::Chord: return "Chord";
        default: return "?";
    }
}
const char* swizzleName(SwizzleOrder order) {
    switch (order) {
        case SwizzleOrder::YXZ: return "YXZ";
        case SwizzleOrder::ZYX: return "ZYX";
        case SwizzleOrder::XZY: return "XZY";
        case SwizzleOrder::YZX: return "YZX";
        case SwizzleOrder::ZXY: return "ZXY";
    }
    return "YXZ";
}
const char* triggerStateName(TriggerState state) {
    switch (state) {
        case TriggerState::None: return "none";
        case TriggerState::Ongoing: return "ongoing";
        case TriggerState::Triggered: return "triggered";
    }
    return "none";
}

// -----------------------------------------------------------------------------
// Fuentes
// -----------------------------------------------------------------------------

Source parseSource(const std::string& name) {
    const SourceTable& t = table();
    const auto it = t.by_name.find(canonical(name));
    if (it != t.by_name.end()) return it->second;
    // Teclas con alias ("Shift", "Ctrl", "Esc"...).
    const dm::Key k = dm::keyFromName(name);
    if (k != dm::Key::Unknown) return Source{SourceKind::Key, static_cast<int>(k), ValueType::Bool};
    return {};
}

const std::vector<SourceGroup>& sourceGroups() { return table().groups; }

std::string pressedSourceName(const dm::Input& in) {
    for (const auto& [src, name] : table().display) {
        switch (src.kind) {
            case SourceKind::Key:
                if (in.isKeyPressed(static_cast<dm::Key>(src.code))) return name;
                break;
            case SourceKind::MouseButton:
                if (in.isMouseButtonPressed(static_cast<dm::MouseButton>(src.code))) return name;
                break;
            case SourceKind::GamepadButton:
                if (in.isGamepadButtonPressed(static_cast<dm::GamepadButton>(src.code))) return name;
                break;
            case SourceKind::GamepadAxis:
                // Gatillos a fondo.
                if (src.code >= 4 && in.gamepadAxis(static_cast<dm::GamepadAxis>(src.code)) > 0.8f) return name;
                break;
            default: break;
        }
    }
    return {};
}

ActionValue readSource(const Source& s, const dm::Input& in) {
    switch (s.kind) {
        case SourceKind::Key: return {in.isKeyDown(static_cast<dm::Key>(s.code)) ? 1.0f : 0.0f, 0.0f, 0.0f};
        case SourceKind::MouseButton:
            return {in.isMouseButtonDown(static_cast<dm::MouseButton>(s.code)) ? 1.0f : 0.0f, 0.0f, 0.0f};
        case SourceKind::MouseAxis:
            // Pixeles este frame; Y positiva hacia arriba (como en Unreal).
            switch (s.code) {
                case 0: return {in.mouseDeltaX(), 0.0f, 0.0f};
                case 1: return {-in.mouseDeltaY(), 0.0f, 0.0f};
                case 2: return {in.mouseDeltaX(), -in.mouseDeltaY(), 0.0f};
                default: return {in.scrollY(), 0.0f, 0.0f};
            }
        case SourceKind::GamepadButton:
            return {in.isGamepadButtonDown(static_cast<dm::GamepadButton>(s.code)) ? 1.0f : 0.0f, 0.0f, 0.0f};
        case SourceKind::GamepadAxis: return {in.gamepadAxis(static_cast<dm::GamepadAxis>(s.code)), 0.0f, 0.0f};
        case SourceKind::GamepadStick:
            if (s.code == 0) return {in.gamepadAxis(dm::GamepadAxis::LeftX), in.gamepadAxis(dm::GamepadAxis::LeftY), 0.0f};
            return {in.gamepadAxis(dm::GamepadAxis::RightX), in.gamepadAxis(dm::GamepadAxis::RightY), 0.0f};
        case SourceKind::TouchStick: return {in.virtualStickX(), in.virtualStickY(), 0.0f};
        case SourceKind::None: break;
    }
    return {};
}

ActionValue applyModifier(const Modifier& m, ActionValue v, ValueType type) {
    switch (m.type) {
        case ModifierType::Negate:
            if (m.axes[0]) v.x = -v.x;
            if (m.axes[1]) v.y = -v.y;
            if (m.axes[2]) v.z = -v.z;
            break;
        case ModifierType::Swizzle: {
            const ActionValue i = v;
            switch (m.order) {
                case SwizzleOrder::YXZ: v = {i.y, i.x, i.z}; break;
                case SwizzleOrder::ZYX: v = {i.z, i.y, i.x}; break;
                case SwizzleOrder::XZY: v = {i.x, i.z, i.y}; break;
                case SwizzleOrder::YZX: v = {i.y, i.z, i.x}; break;
                case SwizzleOrder::ZXY: v = {i.z, i.x, i.y}; break;
            }
            break;
        }
        case ModifierType::DeadZone: {
            const float range = std::max(m.upper - m.lower, 1e-4f);
            const auto remap = [&](float a) { return std::clamp((a - m.lower) / range, 0.0f, 1.0f); };
            if (m.radial) {
                const float mag = v.magnitude();
                if (mag <= 1e-6f) return {};
                const float k = remap(mag) / mag;
                v = {v.x * k, v.y * k, v.z * k};
            } else {
                const auto axis = [&](float a) { return std::copysign(remap(std::fabs(a)), a); };
                v = {axis(v.x), axis(v.y), axis(v.z)};
            }
            break;
        }
        case ModifierType::Scale:
            v.x *= m.scale[0];
            v.y *= m.scale[1];
            v.z *= m.scale[2];
            break;
        default: break;
    }
    return truncate(v, type);
}

// -----------------------------------------------------------------------------
// Por defecto, archivo
// -----------------------------------------------------------------------------

InputActionSettings defaultInputActions() {
    InputActionSettings s;
    const auto action = [&](const char* name, ValueType type, const char* description) {
        InputAction a;
        a.name = name;
        a.type = type;
        a.description = description;
        s.actions.push_back(a);
    };
    action("Move", ValueType::Axis2D, "Andar: x derecha, y adelante");
    action("Look", ValueType::Axis2D, "Mirar: x girar, y arriba");
    action("Jump", ValueType::Bool, "Saltar");
    action("Sprint", ValueType::Bool, "Correr");
    action("Fire", ValueType::Bool, "Disparar / atacar");
    action("Aim", ValueType::Bool, "Apuntar");
    action("Interact", ValueType::Bool, "Usar (pulsar)");
    {
        Trigger pressed;
        pressed.type = TriggerType::Pressed;
        s.actions.back().triggers.push_back(pressed);
    }
    action("Zoom", ValueType::Axis1D, "Acercar / alejar");
    action("Fly", ValueType::Axis3D, "Volar: x derecha, y arriba, z adelante (contexto Vuelo)");

    MappingContext def;
    def.name = "Default";
    def.description = "A pie: teclado + raton, mando y joystick tactil";
    const auto map = [](MappingContext& c, const char* action_name, const char* key, std::vector<Modifier> mods = {}) {
        KeyMapping m;
        m.action = action_name;
        m.key = key;
        m.modifiers = std::move(mods);
        c.mappings.push_back(std::move(m));
    };
    // Move (Vec2): cada tecla 1D va a un eje y un sentido.
    map(def, "Move", "W", {swizzle(SwizzleOrder::YXZ)});
    map(def, "Move", "S", {swizzle(SwizzleOrder::YXZ), negate()});
    map(def, "Move", "D");
    map(def, "Move", "A", {negate()});
    map(def, "Move", "Up", {swizzle(SwizzleOrder::YXZ)});
    map(def, "Move", "Down", {swizzle(SwizzleOrder::YXZ), negate()});
    map(def, "Move", "Right");
    map(def, "Move", "Left", {negate()});
    map(def, "Move", "Gamepad Left Stick", {deadZone()});
    map(def, "Move", "Gamepad DPad Up", {swizzle(SwizzleOrder::YXZ)});
    map(def, "Move", "Gamepad DPad Down", {swizzle(SwizzleOrder::YXZ), negate()});
    map(def, "Move", "Gamepad DPad Right");
    map(def, "Move", "Gamepad DPad Left", {negate()});
    map(def, "Move", "Touch Stick");
    // Look (Vec2): raton (o arrastrar el dedo) y stick derecho, como getAxis("Mouse X").
    map(def, "Look", "Mouse XY", {scale(0.1f, 0.1f, 1.0f)});
    map(def, "Look", "Gamepad Right Stick", {deadZone(), scale(0.6f, 0.6f, 1.0f)});
    map(def, "Jump", "Space");
    map(def, "Jump", "Gamepad A");
    map(def, "Sprint", "LeftShift");
    map(def, "Sprint", "Gamepad LS");
    map(def, "Fire", "Mouse Left");
    map(def, "Fire", "Gamepad RT");
    map(def, "Aim", "Mouse Right");
    map(def, "Aim", "Gamepad LT");
    map(def, "Interact", "E");
    map(def, "Interact", "Gamepad X");
    map(def, "Zoom", "Mouse Wheel");
    map(def, "Zoom", "Gamepad RB");
    map(def, "Zoom", "Gamepad LB", {negate()});
    s.contexts.push_back(def);

    // Vuelo (Vec3): encima de Default (se queda W/A/S/D/E); se activa desde Lua.
    MappingContext fly;
    fly.name = "Vuelo";
    fly.description = "Volar libre (Input.addMappingContext(\"Vuelo\"))";
    fly.priority = 1;
    fly.active_at_start = false;
    map(fly, "Fly", "W", {swizzle(SwizzleOrder::ZYX)});
    map(fly, "Fly", "S", {swizzle(SwizzleOrder::ZYX), negate()});
    map(fly, "Fly", "D");
    map(fly, "Fly", "A", {negate()});
    map(fly, "Fly", "E", {swizzle(SwizzleOrder::YXZ)});
    map(fly, "Fly", "Q", {swizzle(SwizzleOrder::YXZ), negate()});
    map(fly, "Fly", "Gamepad Left Stick", {deadZone(), swizzle(SwizzleOrder::XZY)});
    map(fly, "Fly", "Gamepad RB", {swizzle(SwizzleOrder::YXZ)});
    map(fly, "Fly", "Gamepad LB", {swizzle(SwizzleOrder::YXZ), negate()});
    s.contexts.push_back(fly);
    return s;
}

std::filesystem::path inputActionsFile(const std::filesystem::path& settings_folder) {
    return settings_folder / "InputActions.json";
}

std::string inputActionsToJson(const InputActionSettings& s) {
    json actions = json::array();
    for (const InputAction& a : s.actions) {
        json j = {{"name", a.name}, {"type", valueTypeId(a.type)}, {"description", a.description},
                  {"accumulate", a.accumulate}, {"consume_input", a.consume_input}};
        if (!a.modifiers.empty()) j["modifiers"] = modifiersJson(a.modifiers);
        if (!a.triggers.empty()) j["triggers"] = triggersJson(a.triggers);
        actions.push_back(j);
    }
    json contexts = json::array();
    for (const MappingContext& c : s.contexts) {
        json mappings = json::array();
        for (const KeyMapping& m : c.mappings) {
            json j = {{"action", m.action}, {"key", m.key}};
            if (!m.modifiers.empty()) j["modifiers"] = modifiersJson(m.modifiers);
            if (!m.triggers.empty()) j["triggers"] = triggersJson(m.triggers);
            if (!m.player_mappable) j["player_mappable"] = false;
            mappings.push_back(j);
        }
        contexts.push_back({{"name", c.name},
                            {"description", c.description},
                            {"priority", c.priority},
                            {"active_at_start", c.active_at_start},
                            {"mappings", mappings}});
    }
    const json root = {{"format", "CramionInputActions"}, {"version", 1}, {"actions", actions}, {"contexts", contexts}};
    return root.dump(2);
}

bool inputActionsFromJson(const std::string& text, InputActionSettings& out) {
    try {
        const json root = json::parse(text);
        if (!root.is_object()) return false;
        InputActionSettings s;
        if (const auto list = root.find("actions"); list != root.end() && list->is_array()) {
            for (const json& j : *list) {
                if (!j.is_object()) continue;
                InputAction a;
                a.name = j.value("name", a.name);
                a.type = valueTypeFromId(j.value("type", std::string("bool")));
                a.description = j.value("description", std::string());
                a.accumulate = j.value("accumulate", a.accumulate);
                a.consume_input = j.value("consume_input", a.consume_input);
                if (const auto m = j.find("modifiers"); m != j.end()) a.modifiers = modifiersFrom(*m);
                if (const auto t = j.find("triggers"); t != j.end()) a.triggers = triggersFrom(*t);
                s.actions.push_back(std::move(a));
            }
        }
        if (const auto list = root.find("contexts"); list != root.end() && list->is_array()) {
            for (const json& j : *list) {
                if (!j.is_object()) continue;
                MappingContext c;
                c.name = j.value("name", c.name);
                c.description = j.value("description", std::string());
                c.priority = j.value("priority", c.priority);
                c.active_at_start = j.value("active_at_start", c.active_at_start);
                if (const auto ml = j.find("mappings"); ml != j.end() && ml->is_array()) {
                    for (const json& mj : *ml) {
                        if (!mj.is_object()) continue;
                        KeyMapping m;
                        m.action = mj.value("action", std::string());
                        m.key = mj.value("key", std::string());
                        if (const auto mm = mj.find("modifiers"); mm != mj.end()) m.modifiers = modifiersFrom(*mm);
                        if (const auto mt = mj.find("triggers"); mt != mj.end()) m.triggers = triggersFrom(*mt);
                        m.player_mappable = mj.value("player_mappable", true);
                        c.mappings.push_back(std::move(m));
                    }
                }
                s.contexts.push_back(std::move(c));
            }
        }
        out = std::move(s);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

InputActionSettings loadInputActions(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return defaultInputActions();
    std::stringstream text;
    text << in.rdbuf();
    InputActionSettings s;
    if (!inputActionsFromJson(text.str(), s)) return defaultInputActions();
    return s;
}

bool saveInputActions(const std::filesystem::path& file, const InputActionSettings& settings) {
    std::error_code e;
    std::filesystem::create_directories(file.parent_path(), e);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << inputActionsToJson(settings);
    return static_cast<bool>(out);
}

// -----------------------------------------------------------------------------
// InputMapper
// -----------------------------------------------------------------------------

void InputMapper::setSettings(const InputActionSettings& settings) {
    base_ = settings;
    settings_ = settings;
    const std::vector<Override> overrides = std::move(overrides_);
    overrides_.clear();
    for (const Override& o : overrides) rebind(o.context, o.action, o.index, o.key);
    states_.assign(settings_.actions.size(), ActionState{});
    // Los contextos activos que ya no existen se quitan.
    std::erase_if(active_, [&](const ActiveContext& a) { return settings_.findContext(a.name) == nullptr; });
    rebuild();
}

void InputMapper::rebuild() {
    if (states_.size() != settings_.actions.size()) states_.assign(settings_.actions.size(), ActionState{});
    action_runtime_.assign(settings_.actions.size(), ActionRuntime{});
    for (std::size_t i = 0; i < settings_.actions.size(); ++i) {
        const std::size_t n = settings_.actions[i].triggers.size();
        action_runtime_[i] = ActionRuntime{std::vector<float>(n, 0.0f), std::vector<int>(n, 0), std::vector<bool>(n, false)};
    }
    mapping_runtime_.assign(settings_.contexts.size(), {});
    for (std::size_t c = 0; c < settings_.contexts.size(); ++c) {
        for (const KeyMapping& m : settings_.contexts[c].mappings) {
            const std::size_t n = m.triggers.size();
            mapping_runtime_[c].push_back(MappingRuntime{parseSource(m.key), std::vector<float>(n, 0.0f), std::vector<int>(n, 0),
                                                         std::vector<bool>(n, false)});
        }
    }
}

void InputMapper::resetContexts() {
    active_.clear();
    for (const MappingContext& c : settings_.contexts) {
        if (c.active_at_start) active_.push_back({c.name, c.priority, next_order_++});
    }
    for (ActionState& s : states_) s = ActionState{};
}

bool InputMapper::addContext(const std::string& name, int priority) {
    const MappingContext* c = settings_.findContext(name);
    if (c == nullptr) return false;
    removeContext(name);
    active_.push_back({c->name, priority, next_order_++});
    return true;
}

bool InputMapper::addContext(const std::string& name) {
    const MappingContext* c = settings_.findContext(name);
    return c != nullptr && addContext(name, c->priority);
}

void InputMapper::removeContext(const std::string& name) {
    const std::string n = lower(name);
    std::erase_if(active_, [&](const ActiveContext& a) { return lower(a.name) == n; });
}

void InputMapper::clearContexts() { active_.clear(); }

bool InputMapper::hasContext(const std::string& name) const {
    const std::string n = lower(name);
    return std::any_of(active_.begin(), active_.end(), [&](const ActiveContext& a) { return lower(a.name) == n; });
}

std::vector<std::string> InputMapper::activeContexts() const {
    std::vector<ActiveContext> sorted = active_;
    std::stable_sort(sorted.begin(), sorted.end(), [](const ActiveContext& a, const ActiveContext& b) {
        return a.priority != b.priority ? a.priority > b.priority : a.order > b.order;
    });
    std::vector<std::string> out;
    for (const ActiveContext& a : sorted) out.push_back(a.name);
    return out;
}

const ActionState* InputMapper::state(const std::string& action) const {
    const std::string n = lower(action);
    for (std::size_t i = 0; i < settings_.actions.size() && i < states_.size(); ++i) {
        if (lower(settings_.actions[i].name) == n) return &states_[i];
    }
    return nullptr;
}

TriggerState InputMapper::evalTriggers(const std::vector<Trigger>& triggers, const ActionValue& value, std::vector<float>& held,
                                       std::vector<int>& pulses, std::vector<bool>& was_actuated, float dt,
                                       float implicit_threshold) const {
    // Sin triggers: disparada mientras hay valor (Bool: a partir de 0.5).
    if (triggers.empty()) return value.magnitude() >= implicit_threshold ? TriggerState::Triggered : TriggerState::None;
    bool any_explicit = false;
    bool implicit_ok = true;
    TriggerState explicit_max = TriggerState::None;
    const float mag = value.magnitude();
    for (std::size_t i = 0; i < triggers.size(); ++i) {
        const Trigger& t = triggers[i];
        const bool on = mag >= std::max(t.threshold, 1e-4f);
        const bool was = was_actuated[i];
        TriggerState s = TriggerState::None;
        switch (t.type) {
            case TriggerType::Down: s = on ? TriggerState::Triggered : TriggerState::None; break;
            case TriggerType::Pressed: s = on && !was ? TriggerState::Triggered : TriggerState::None; break;
            case TriggerType::Released: s = on ? TriggerState::Ongoing : (was ? TriggerState::Triggered : TriggerState::None); break;
            case TriggerType::Hold:
                if (on) {
                    held[i] += dt;
                    if (held[i] >= t.time) {
                        const bool first = pulses[i] == 0;
                        pulses[i] = 1;
                        s = first || !t.one_shot ? TriggerState::Triggered : TriggerState::None;
                    } else {
                        s = TriggerState::Ongoing;
                    }
                } else {
                    held[i] = 0.0f;
                    pulses[i] = 0;
                }
                break;
            case TriggerType::HoldAndRelease:
                if (on) {
                    held[i] += dt;
                    s = TriggerState::Ongoing;
                } else {
                    s = was && held[i] >= t.time ? TriggerState::Triggered : TriggerState::None;
                    held[i] = 0.0f;
                }
                break;
            case TriggerType::Tap:
                if (on) {
                    held[i] += dt;
                    s = held[i] <= t.time ? TriggerState::Ongoing : TriggerState::None;
                } else {
                    s = was && held[i] <= t.time ? TriggerState::Triggered : TriggerState::None;
                    held[i] = 0.0f;
                }
                break;
            case TriggerType::Pulse:
                if (on) {
                    const bool can = t.pulse_limit <= 0 || pulses[i] < t.pulse_limit;
                    if (!was) {
                        held[i] = 0.0f;
                        if (t.pulse_at_start && can) {
                            ++pulses[i];
                            s = TriggerState::Triggered;
                            break;
                        }
                    } else {
                        held[i] += dt;
                    }
                    if (!can) {
                        s = TriggerState::None;
                    } else if (held[i] >= std::max(t.time, 1e-3f)) {
                        held[i] -= std::max(t.time, 1e-3f);
                        ++pulses[i];
                        s = TriggerState::Triggered;
                    } else {
                        s = TriggerState::Ongoing;
                    }
                } else {
                    held[i] = 0.0f;
                    pulses[i] = 0;
                }
                break;
            case TriggerType::Chord: {
                const ActionState* other = state(t.chord);
                if (other == nullptr || other->state != TriggerState::Triggered) implicit_ok = false;
                was_actuated[i] = on;
                continue;
            }
            default: break;
        }
        was_actuated[i] = on;
        any_explicit = true;
        if (rank(s) > rank(explicit_max)) explicit_max = s;
    }
    if (!implicit_ok) return any_explicit && explicit_max != TriggerState::None ? TriggerState::Ongoing : TriggerState::None;
    return any_explicit ? explicit_max : TriggerState::Triggered;
}

void InputMapper::update(const dm::Input* in, float dt) {
    const std::size_t count = settings_.actions.size();
    if (states_.size() != count || action_runtime_.size() != count || mapping_runtime_.size() != settings_.contexts.size()) {
        rebuild();
    }
    // Por accion: el mayor valor positivo y negativo por eje (W+S = 0) o la suma.
    struct Accum {
        ActionValue pos, neg, sum;
        bool any = false;
        TriggerState explicit_state = TriggerState::None;
        bool has_explicit = false;
        TriggerState implicit_state = TriggerState::None;
    };
    std::vector<Accum> acc(count);

    // Contextos por prioridad (el ultimo anadido gana a igual prioridad).
    std::vector<ActiveContext> order = active_;
    std::stable_sort(order.begin(), order.end(), [](const ActiveContext& a, const ActiveContext& b) {
        return a.priority != b.priority ? a.priority > b.priority : a.order > b.order;
    });
    std::vector<int> consumed;  // teclas de contextos de mas prioridad
    const auto actionIndex = [&](const std::string& name) -> int {
        const std::string n = lower(name);
        for (std::size_t i = 0; i < count; ++i) {
            if (lower(settings_.actions[i].name) == n) return static_cast<int>(i);
        }
        return -1;
    };
    for (const ActiveContext& active : order) {
        int ci = -1;
        for (std::size_t c = 0; c < settings_.contexts.size(); ++c) {
            if (lower(settings_.contexts[c].name) == lower(active.name)) ci = static_cast<int>(c);
        }
        if (ci < 0) continue;
        const MappingContext& ctx = settings_.contexts[static_cast<std::size_t>(ci)];
        std::vector<int> used;
        for (std::size_t mi = 0; mi < ctx.mappings.size(); ++mi) {
            const KeyMapping& m = ctx.mappings[mi];
            MappingRuntime& rt = mapping_runtime_[static_cast<std::size_t>(ci)][mi];
            const int ai = actionIndex(m.action);
            if (ai < 0 || rt.source.kind == SourceKind::None) continue;
            const InputAction& action = settings_.actions[static_cast<std::size_t>(ai)];
            const int id = sourceId(rt.source);
            const bool blocked = std::find(consumed.begin(), consumed.end(), id) != consumed.end();
            ActionValue v;
            if (in != nullptr && !blocked) {
                v = truncate(readSource(rt.source, *in), action.type);
                for (const Modifier& mod : m.modifiers) v = applyModifier(mod, v, action.type);
            }
            const TriggerState s = evalTriggers(m.triggers, v, rt.held, rt.pulses, rt.was_actuated, dt,
                                                action.type == ValueType::Bool ? 0.5f : 1e-4f);
            if (action.consume_input) used.push_back(id);
            Accum& a = acc[static_cast<std::size_t>(ai)];
            if (m.triggers.empty()) {
                if (rank(s) > rank(a.implicit_state)) a.implicit_state = s;
            } else {
                a.has_explicit = true;
                if (rank(s) > rank(a.explicit_state)) a.explicit_state = s;
            }
            if (s == TriggerState::None && !m.triggers.empty()) continue;
            a.any = true;
            const auto add = [](float value, float& pos, float& neg, float& sum) {
                pos = std::max(pos, value);
                neg = std::min(neg, value);
                sum += value;
            };
            add(v.x, a.pos.x, a.neg.x, a.sum.x);
            add(v.y, a.pos.y, a.neg.y, a.sum.y);
            add(v.z, a.pos.z, a.neg.z, a.sum.z);
        }
        consumed.insert(consumed.end(), used.begin(), used.end());
    }

    for (std::size_t i = 0; i < count; ++i) {
        const InputAction& action = settings_.actions[i];
        const Accum& a = acc[i];
        ActionValue v = action.accumulate ? a.sum : ActionValue{a.pos.x + a.neg.x, a.pos.y + a.neg.y, a.pos.z + a.neg.z};
        v = truncate(v, action.type);
        for (const Modifier& mod : action.modifiers) v = applyModifier(mod, v, action.type);

        TriggerState mapped = a.implicit_state;
        if (a.has_explicit && rank(a.explicit_state) > rank(mapped)) mapped = a.explicit_state;
        TriggerState next = mapped;
        if (!action.triggers.empty()) {
            ActionRuntime& rt = action_runtime_[i];
            const TriggerState own = evalTriggers(action.triggers, v, rt.held, rt.pulses, rt.was_actuated, dt, 1e-4f);
            next = a.has_explicit ? static_cast<TriggerState>(std::min(rank(own), rank(a.explicit_state))) : own;
        }

        ActionState& st = states_[i];
        const TriggerState prev = st.state;
        std::uint8_t ev = EventNone;
        if (prev == TriggerState::None && next != TriggerState::None) ev |= EventStarted;
        if (next == TriggerState::Ongoing) ev |= EventOngoing;
        if (next == TriggerState::Triggered) ev |= EventTriggered;
        if (next == TriggerState::None && prev == TriggerState::Triggered) ev |= EventCompleted;
        if (next == TriggerState::None && prev == TriggerState::Ongoing) ev |= EventCanceled;
        if (ev & EventStarted) {
            st.elapsed = 0.0f;
            st.triggered_time = 0.0f;
        } else if (next != TriggerState::None) {
            st.elapsed += dt;
        }
        if (next == TriggerState::Triggered) st.triggered_time += (ev & EventStarted) ? 0.0f : dt;
        if (next == TriggerState::None) {
            st.elapsed = 0.0f;
            st.triggered_time = 0.0f;
        }
        if (action.type == ValueType::Bool) v = {v.magnitude() >= 0.5f || next == TriggerState::Triggered ? 1.0f : 0.0f, 0.0f, 0.0f};
        st.value = v;
        st.state = next;
        st.events = ev;
    }
}

bool InputMapper::rebind(const std::string& context, const std::string& action, int index, const std::string& key) {
    MappingContext* c = settings_.findContext(context);
    if (c == nullptr || index < 0) return false;
    if (parseSource(key).kind == SourceKind::None) return false;
    const std::string n = lower(action);
    int k = 0;
    for (KeyMapping& m : c->mappings) {
        if (lower(m.action) != n) continue;
        if (k++ != index) continue;
        if (!m.player_mappable) return false;
        m.key = key;
        std::erase_if(overrides_, [&](const Override& o) {
            return lower(o.context) == lower(context) && lower(o.action) == n && o.index == index;
        });
        overrides_.push_back({c->name, m.action, index, key});
        rebuild();
        return true;
    }
    return false;
}

std::vector<std::pair<std::string, std::string>> InputMapper::bindings(const std::string& action) const {
    std::vector<std::pair<std::string, std::string>> out;
    const std::string n = lower(action);
    for (const MappingContext& c : settings_.contexts) {
        for (const KeyMapping& m : c.mappings) {
            if (lower(m.action) == n) out.emplace_back(c.name, m.key);
        }
    }
    return out;
}

std::string InputMapper::overridesJson() const {
    json list = json::array();
    for (const Override& o : overrides_) {
        list.push_back({{"context", o.context}, {"action", o.action}, {"index", o.index}, {"key", o.key}});
    }
    return list.dump();
}

void InputMapper::applyOverridesJson(const std::string& text) {
    try {
        const json list = json::parse(text);
        if (!list.is_array()) return;
        for (const json& o : list) {
            if (!o.is_object()) continue;
            rebind(o.value("context", std::string()), o.value("action", std::string()), o.value("index", 0),
                   o.value("key", std::string()));
        }
    } catch (const std::exception&) {
    }
}

void InputMapper::clearOverrides() {
    overrides_.clear();
    settings_ = base_;
    rebuild();
}

}  // namespace cramion::input
