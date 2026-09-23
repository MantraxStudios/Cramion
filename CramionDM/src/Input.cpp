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
        case EventType::MouseScrolled: {
            scrollX_ += event.scrollX;
            scrollY_ += event.scrollY;
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

}  // namespace cramion::dm
