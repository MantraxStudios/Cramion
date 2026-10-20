#include "CramionDM/Input.h"

#include <algorithm>
#include <cmath>

namespace cramion::dm {

namespace {

// Quien atiende setGamepadVibration (la ventana que lee los mandos).
GamepadVibrationHandler g_vibration_handler = nullptr;
void* g_vibration_user = nullptr;

bool isTrigger(size_t axis) {
    return axis == static_cast<size_t>(GamepadAxis::LeftTrigger) || axis == static_cast<size_t>(GamepadAxis::RightTrigger);
}

}  // namespace

bool setGamepadVibration(int pad, float low, float high, float seconds) {
    if (g_vibration_handler == nullptr) return false;
    return g_vibration_handler(g_vibration_user, pad, std::clamp(low, 0.0f, 1.0f), std::clamp(high, 0.0f, 1.0f),
                               std::max(seconds, 0.0f));
}

void setGamepadVibrationHandler(GamepadVibrationHandler handler, void* user) {
    g_vibration_handler = handler;
    g_vibration_user = handler != nullptr ? user : nullptr;
}

void* gamepadVibrationUser() { return g_vibration_user; }

void Input::reset() {
    keyDown_.fill(false);
    keyPressed_.fill(false);
    keyReleased_.fill(false);
    buttonDown_.fill(false);
    buttonPressed_.fill(false);
    buttonReleased_.fill(false);
    mouseX_ = mouseY_ = 0.0f;
    mouseDeltaX_ = mouseDeltaY_ = 0.0f;
    scrollX_ = scrollY_ = 0.0f;
    mods_ = KeyMods::None;
    touches_.clear();
    pads_ = {};
    padDown_.fill(false);
    padPressed_.fill(false);
    padReleased_.fill(false);
    connected_mask_ = 0;
    active_pad_ = 0;
    active_device_ = Device::KeyboardMouse;
    device_changed_ = false;
    virtualStickX_ = virtualStickY_ = 0.0f;
    xrDown_.fill(false);
    xrPressed_.fill(false);
    xrReleased_.fill(false);
    xrAxes_.fill(0.0f);
}

void Input::suppressMouseButton(MouseButton button) {
    const auto idx = static_cast<size_t>(button);
    if (idx >= kButtonCount) return;
    buttonDown_[idx] = false;
    buttonPressed_[idx] = false;
    buttonReleased_[idx] = false;
}

void Input::onEvent(const Event& event) {
    switch (event.type) {
        case EventType::KeyPressed: {
            const auto idx = static_cast<size_t>(event.key);
            if (idx < kKeyCount) {
                if (!keyDown_[idx]) {
                    keyPressed_[idx] = true;  // Solo en el flanco, no en auto-repeat.
                }
                keyDown_[idx] = true;
            }
            mods_ = event.mods;
            if (!event.repeat) useDevice(Device::KeyboardMouse);
            break;
        }
        case EventType::KeyReleased: {
            const auto idx = static_cast<size_t>(event.key);
            if (idx < kKeyCount) {
                keyDown_[idx] = false;
                keyReleased_[idx] = true;
            }
            mods_ = event.mods;
            break;
        }
        case EventType::MouseButtonPressed: {
            const auto idx = static_cast<size_t>(event.button);
            if (idx < kButtonCount) {
                if (!buttonDown_[idx]) {
                    buttonPressed_[idx] = true;
                }
                buttonDown_[idx] = true;
            }
            useDevice(Device::KeyboardMouse);
            break;
        }
        case EventType::MouseButtonReleased: {
            const auto idx = static_cast<size_t>(event.button);
            if (idx < kButtonCount) {
                buttonDown_[idx] = false;
                buttonReleased_[idx] = true;
            }
            break;
        }
        case EventType::MouseMoved: {
            mouseX_ = event.mouseX;
            mouseY_ = event.mouseY;
            mouseDeltaX_ += event.deltaX;
            mouseDeltaY_ += event.deltaY;
            // Unos pixeles de temblor (o el salto al encerrar el cursor) no
            // quitan los iconos del mando.
            if (std::fabs(event.deltaX) + std::fabs(event.deltaY) > 3.0f) useDevice(Device::KeyboardMouse);
            break;
        }
        case EventType::MouseRawMoved: {
            mouseDeltaX_ += event.deltaX;
            mouseDeltaY_ += event.deltaY;
            if (std::fabs(event.deltaX) + std::fabs(event.deltaY) > 3.0f) useDevice(Device::KeyboardMouse);
            break;
        }
        case EventType::MouseScrolled: {
            scrollX_ += event.scrollX;
            scrollY_ += event.scrollY;
            useDevice(Device::KeyboardMouse);
            break;
        }
        case EventType::TouchBegan: {
            touch_screen_ = true;
            useDevice(Device::Touch);
            Touch t;
            t.id = event.touchId;
            t.x = t.startX = event.mouseX;
            t.y = t.startY = event.mouseY;
            t.phase = TouchPhase::Began;
            // Un id reutilizado en el mismo frame: el dedo viejo ya termino.
            for (Touch& old : touches_) {
                if (old.id == t.id) old.id = -1 - old.id;
            }
            touches_.push_back(t);
            break;
        }
        case EventType::TouchMoved:
        case EventType::TouchEnded: {
            for (Touch& t : touches_) {
                if (t.id != event.touchId || t.phase == TouchPhase::Ended) continue;
                t.deltaX += event.mouseX - t.x;
                t.deltaY += event.mouseY - t.y;
                t.x = event.mouseX;
                t.y = event.mouseY;
                if (event.type == EventType::TouchEnded) {
                    t.phase = TouchPhase::Ended;
                } else if (t.phase != TouchPhase::Began) {
                    t.phase = TouchPhase::Moved;
                }
                break;
            }
            break;
        }
        case EventType::GamepadButtonPressed:
        case EventType::GamepadButtonReleased: {
            const auto idx = static_cast<size_t>(event.gamepadButton);
            const int pad = event.gamepadIndex;
            if (idx >= kGamepadButtonCount || pad < 0 || pad >= kMaxGamepads) break;
            // Android no manda GamepadConnected: el primer boton lo conecta.
            markPadConnected(pad, GamepadKind::Unknown);
            const bool down = event.type == EventType::GamepadButtonPressed;
            setPadButton(pad, idx, down);
            if (down) {
                active_pad_ = pad;
                useDevice(Device::Gamepad);
            }
            break;
        }
        case EventType::GamepadAxisMoved: {
            const auto idx = static_cast<size_t>(event.gamepadAxis);
            const int pad = event.gamepadIndex;
            if (idx >= kGamepadAxisCount || pad < 0 || pad >= kMaxGamepads) break;
            markPadConnected(pad, GamepadKind::Unknown);
            pads_[static_cast<size_t>(pad)].axes[idx] = event.value;
            // Pasada la mitad es una intencion clara (no el temblor de un
            // stick suelto): ese mando pasa a ser el activo.
            const float used = isTrigger(idx) ? event.value : std::fabs(event.value);
            if (used > 0.5f) {
                active_pad_ = pad;
                useDevice(Device::Gamepad);
            }
            break;
        }
        case EventType::GamepadConnected: {
            const int pad = event.gamepadIndex;
            if (pad < 0 || pad >= kMaxGamepads) break;
            markPadConnected(pad, event.gamepadKind);
            if (event.gamepadKind != GamepadKind::Unknown) pads_[static_cast<size_t>(pad)].kind = event.gamepadKind;
            break;
        }
        case EventType::GamepadDisconnected: {
            const int pad = event.gamepadIndex;
            if (pad < 0 || pad >= kMaxGamepads) break;
            releasePad(pad);
            connected_mask_ = static_cast<uint8_t>(connected_mask_ & ~(1u << pad));
            pads_[static_cast<size_t>(pad)].kind = GamepadKind::Unknown;
            if (active_pad_ == pad) {
                // El activo pasa al primer mando que quede (o al 0).
                active_pad_ = 0;
                for (int i = 0; i < kMaxGamepads; ++i) {
                    if ((connected_mask_ & (1u << i)) != 0) {
                        active_pad_ = i;
                        break;
                    }
                }
            }
            break;
        }
        default:
            break;
    }
}

void Input::newFrame() {
    keyPressed_.fill(false);
    keyReleased_.fill(false);
    buttonPressed_.fill(false);
    buttonReleased_.fill(false);
    mouseDeltaX_ = mouseDeltaY_ = 0.0f;
    scrollX_ = scrollY_ = 0.0f;
    padPressed_.fill(false);
    padReleased_.fill(false);
    for (Pad& p : pads_) {
        p.pressed.fill(false);
        p.released.fill(false);
    }
    device_changed_ = false;
    xrPressed_.fill(false);
    xrReleased_.fill(false);
    // Los dedos levantados se van; los demas quedan quietos hasta que se muevan.
    std::vector<Touch> alive;
    alive.reserve(touches_.size());
    for (Touch& t : touches_) {
        if (t.phase == TouchPhase::Ended || t.id < 0) continue;
        t.phase = TouchPhase::Stationary;
        t.deltaX = t.deltaY = 0.0f;
        alive.push_back(t);
    }
    touches_ = std::move(alive);
}

void Input::useDevice(Device device) {
    if (active_device_ == device) return;
    active_device_ = device;
    device_changed_ = true;
}

void Input::markPadConnected(int pad, GamepadKind kind) {
    connected_mask_ = static_cast<uint8_t>(connected_mask_ | (1u << pad));
    Pad& p = pads_[static_cast<size_t>(pad)];
    if (p.kind == GamepadKind::Unknown) p.kind = kind;
}

void Input::setPadButton(int pad, size_t button, bool down) {
    Pad& p = pads_[static_cast<size_t>(pad)];
    if (down && !p.down[button]) p.pressed[button] = true;
    if (!down && p.down[button]) p.released[button] = true;
    p.down[button] = down;
    // "Cualquier mando": pulsado mientras alguno lo tenga pulsado.
    bool any = false;
    for (const Pad& other : pads_) any = any || other.down[button];
    if (any && !padDown_[button]) padPressed_[button] = true;
    if (!any && padDown_[button]) padReleased_[button] = true;
    padDown_[button] = any;
}

void Input::releasePad(int pad) {
    Pad& p = pads_[static_cast<size_t>(pad)];
    for (size_t i = 0; i < kGamepadButtonCount; ++i) {
        if (p.down[i]) setPadButton(pad, i, false);
    }
    p.axes.fill(0.0f);
}

void Input::setGamepadDeadZones(float stick, float trigger) {
    stick_dead_zone_ = std::clamp(stick, 0.0f, 0.95f);
    trigger_dead_zone_ = std::clamp(trigger, 0.0f, 0.95f);
}

bool Input::isGamepadButtonDown(GamepadButton button) const {
    const auto idx = static_cast<size_t>(button);
    return idx < kGamepadButtonCount && padDown_[idx];
}

bool Input::isGamepadButtonPressed(GamepadButton button) const {
    const auto idx = static_cast<size_t>(button);
    return idx < kGamepadButtonCount && padPressed_[idx];
}

bool Input::isGamepadButtonReleased(GamepadButton button) const {
    const auto idx = static_cast<size_t>(button);
    return idx < kGamepadButtonCount && padReleased_[idx];
}

float Input::gamepadAxis(GamepadAxis axis) const { return gamepadAxis(active_pad_, axis); }

bool Input::isGamepadButtonDown(int pad, GamepadButton button) const {
    const auto idx = static_cast<size_t>(button);
    return pad >= 0 && pad < kMaxGamepads && idx < kGamepadButtonCount && pads_[static_cast<size_t>(pad)].down[idx];
}

bool Input::isGamepadButtonPressed(int pad, GamepadButton button) const {
    const auto idx = static_cast<size_t>(button);
    return pad >= 0 && pad < kMaxGamepads && idx < kGamepadButtonCount && pads_[static_cast<size_t>(pad)].pressed[idx];
}

bool Input::isGamepadButtonReleased(int pad, GamepadButton button) const {
    const auto idx = static_cast<size_t>(button);
    return pad >= 0 && pad < kMaxGamepads && idx < kGamepadButtonCount && pads_[static_cast<size_t>(pad)].released[idx];
}

float Input::gamepadAxis(int pad, GamepadAxis axis) const {
    const auto idx = static_cast<size_t>(axis);
    if (pad < 0 || pad >= kMaxGamepads || idx >= kGamepadAxisCount) return 0.0f;
    const auto& axes = pads_[static_cast<size_t>(pad)].axes;
    if (isTrigger(idx)) {
        const float v = std::clamp(axes[idx], 0.0f, 1.0f);
        if (v <= trigger_dead_zone_) return 0.0f;
        return (v - trigger_dead_zone_) / (1.0f - trigger_dead_zone_);
    }
    // Zona muerta radial: se mira el stick entero (x, y), no cada eje por
    // separado (eso hace una cruz muerta y diagonales pegadas a los ejes).
    // Lo de fuera se reescala para que el recorrido empiece en 0.
    const size_t first = idx & ~size_t{1};  // LeftX/LeftY = 0/1, RightX/RightY = 2/3
    const float x = axes[first];
    const float y = axes[first + 1];
    const float length = std::sqrt(x * x + y * y);
    if (length <= stick_dead_zone_ || length <= 0.0f) return 0.0f;
    const float scaled = std::min(1.0f, (length - stick_dead_zone_) / (1.0f - stick_dead_zone_));
    return std::clamp(axes[idx] / length * scaled, -1.0f, 1.0f);
}

bool Input::gamepadConnected(int pad) const {
    return pad >= 0 && pad < kMaxGamepads && (connected_mask_ & (1u << pad)) != 0;
}

GamepadKind Input::gamepadKind(int pad) const {
    return gamepadConnected(pad) ? pads_[static_cast<size_t>(pad)].kind : GamepadKind::Unknown;
}

int Input::gamepadCount() const {
    int n = 0;
    for (int i = 0; i < kMaxGamepads; ++i) n += (connected_mask_ >> i) & 1;
    return n;
}

bool Input::isKeyDown(Key key) const {
    const auto idx = static_cast<size_t>(key);
    return idx < kKeyCount && keyDown_[idx];
}

bool Input::isKeyPressed(Key key) const {
    const auto idx = static_cast<size_t>(key);
    return idx < kKeyCount && keyPressed_[idx];
}

bool Input::isKeyReleased(Key key) const {
    const auto idx = static_cast<size_t>(key);
    return idx < kKeyCount && keyReleased_[idx];
}

bool Input::isMouseButtonDown(MouseButton button) const {
    const auto idx = static_cast<size_t>(button);
    return idx < kButtonCount && buttonDown_[idx];
}

bool Input::isMouseButtonPressed(MouseButton button) const {
    const auto idx = static_cast<size_t>(button);
    return idx < kButtonCount && buttonPressed_[idx];
}

bool Input::isMouseButtonReleased(MouseButton button) const {
    const auto idx = static_cast<size_t>(button);
    return idx < kButtonCount && buttonReleased_[idx];
}

void Input::setXrButton(XrButton button, bool down) {
    const auto idx = static_cast<size_t>(button);
    if (idx >= kXrButtonCount) return;
    if (down && !xrDown_[idx]) xrPressed_[idx] = true;
    if (!down && xrDown_[idx]) xrReleased_[idx] = true;
    xrDown_[idx] = down;
}

void Input::setXrAxis(XrAxis axis, float value) {
    const auto idx = static_cast<size_t>(axis);
    if (idx < xrAxes_.size()) xrAxes_[idx] = value;
}

bool Input::isXrButtonDown(XrButton button) const {
    const auto idx = static_cast<size_t>(button);
    return idx < kXrButtonCount && xrDown_[idx];
}

bool Input::isXrButtonPressed(XrButton button) const {
    const auto idx = static_cast<size_t>(button);
    return idx < kXrButtonCount && xrPressed_[idx];
}

bool Input::isXrButtonReleased(XrButton button) const {
    const auto idx = static_cast<size_t>(button);
    return idx < kXrButtonCount && xrReleased_[idx];
}

float Input::xrAxis(XrAxis axis) const {
    const auto idx = static_cast<size_t>(axis);
    return idx < xrAxes_.size() ? xrAxes_[idx] : 0.0f;
}

}  // namespace cramion::dm
