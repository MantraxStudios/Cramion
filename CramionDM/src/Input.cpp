#include "CramionDM/Input.h"

namespace cramion::dm {

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
    padDown_.fill(false);
    padPressed_.fill(false);
    padReleased_.fill(false);
    padAxes_.fill(0.0f);
    virtualStickX_ = virtualStickY_ = 0.0f;
    xrDown_.fill(false);
    xrPressed_.fill(false);
    xrReleased_.fill(false);
    xrAxes_.fill(0.0f);
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
            break;
        }
        case EventType::MouseRawMoved: {
            mouseDeltaX_ += event.deltaX;
            mouseDeltaY_ += event.deltaY;
            break;
        }
        case EventType::MouseScrolled: {
            scrollX_ += event.scrollX;
            scrollY_ += event.scrollY;
            break;
        }
        case EventType::TouchBegan: {
            touch_screen_ = true;
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
            if (idx >= kGamepadButtonCount) break;
            gamepad_connected_ = true;
            const bool down = event.type == EventType::GamepadButtonPressed;
            if (down && !padDown_[idx]) padPressed_[idx] = true;
            if (!down && padDown_[idx]) padReleased_[idx] = true;
            padDown_[idx] = down;
            break;
        }
        case EventType::GamepadAxisMoved: {
            const auto idx = static_cast<size_t>(event.gamepadAxis);
            if (idx < kGamepadAxisCount) padAxes_[idx] = event.value;
            gamepad_connected_ = true;
            break;
        }
        case EventType::GamepadConnected: gamepad_connected_ = true; break;
        case EventType::GamepadDisconnected: {
            gamepad_connected_ = false;
            for (size_t i = 0; i < kGamepadButtonCount; ++i) {
                if (padDown_[i]) padReleased_[i] = true;
            }
            padDown_.fill(false);
            padAxes_.fill(0.0f);
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

float Input::gamepadAxis(GamepadAxis axis) const {
    const auto idx = static_cast<size_t>(axis);
    if (idx >= kGamepadAxisCount) return 0.0f;
    const float v = padAxes_[idx];
    // Zona muerta de los sticks (los gatillos no la necesitan).
    if (axis != GamepadAxis::LeftTrigger && axis != GamepadAxis::RightTrigger) {
        constexpr float kDead = 0.15f;
        if (v > -kDead && v < kDead) return 0.0f;
        return (v - (v > 0.0f ? kDead : -kDead)) / (1.0f - kDead);
    }
    return v;
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
