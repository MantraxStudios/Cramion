#include "CramionDM/Event.h"

namespace cramion::dm {

const char* eventTypeName(EventType type) {
    switch (type) {
        case EventType::WindowClose: return "WindowClose";
        case EventType::WindowResize: return "WindowResize";
        case EventType::WindowFocus: return "WindowFocus";
        case EventType::WindowLostFocus: return "WindowLostFocus";
        case EventType::WindowMoved: return "WindowMoved";
        case EventType::WindowMinimized: return "WindowMinimized";
        case EventType::WindowMaximized: return "WindowMaximized";
        case EventType::WindowRestored: return "WindowRestored";
        case EventType::WindowDpiChanged: return "WindowDpiChanged";
        case EventType::FileDropped: return "FileDropped";
        case EventType::KeyPressed: return "KeyPressed";
        case EventType::KeyReleased: return "KeyReleased";
        case EventType::TextInput: return "TextInput";
        case EventType::MouseButtonPressed: return "MouseButtonPressed";
        case EventType::MouseButtonReleased: return "MouseButtonReleased";
        case EventType::MouseMoved: return "MouseMoved";
        case EventType::MouseScrolled: return "MouseScrolled";
        case EventType::MouseEnter: return "MouseEnter";
        case EventType::MouseLeave: return "MouseLeave";
        case EventType::MouseRawMoved: return "MouseRawMoved";
        case EventType::TouchBegan: return "TouchBegan";
        case EventType::TouchMoved: return "TouchMoved";
        case EventType::TouchEnded: return "TouchEnded";
        case EventType::GamepadButtonPressed: return "GamepadButtonPressed";
        case EventType::GamepadButtonReleased: return "GamepadButtonReleased";
        case EventType::GamepadAxisMoved: return "GamepadAxisMoved";
        case EventType::GamepadConnected: return "GamepadConnected";
        case EventType::GamepadDisconnected: return "GamepadDisconnected";
        case EventType::WindowSurfaceLost: return "WindowSurfaceLost";
        case EventType::WindowSurfaceCreated: return "WindowSurfaceCreated";
        default: return "None";
    }
}

}  // namespace cramion::dm
