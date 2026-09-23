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
        default: return "None";
    }
}

}  // namespace cramion::dm
