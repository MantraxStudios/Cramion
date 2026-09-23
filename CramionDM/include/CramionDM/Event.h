#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "CramionDM/KeyCode.h"

namespace cramion::dm {

// Tipos de evento que puede emitir una ventana de CramionDM.
enum class EventType : uint8_t {
    None = 0,

    // Ventana
    WindowClose,
    WindowResize,
    WindowFocus,
    WindowLostFocus,
    WindowMoved,
    WindowMinimized,
    WindowMaximized,
    WindowRestored,
    WindowDpiChanged,   // Cambio de escala DPI (monitor distinto / ajuste del SO)

    // Archivos (arrastrar y soltar)
    FileDropped,

    // Teclado
    KeyPressed,   // Incluye repeticiones (repeat = true si viene de auto-repeat)
    KeyReleased,
    TextInput,    // Carácter Unicode traducido (para escribir texto)

    // Ratón
    MouseButtonPressed,
    MouseButtonReleased,
    MouseMoved,
    MouseScrolled,
    MouseEnter,
    MouseLeave,
};

// Categorías (máscara de bits) para poder filtrar eventos por grupo.
enum class EventCategory : uint8_t {
    None = 0,
    Window = 1 << 0,
    Input = 1 << 1,
    Keyboard = 1 << 2,
    Mouse = 1 << 3,
    File = 1 << 4,
};

inline EventCategory operator|(EventCategory a, EventCategory b) {
    return static_cast<EventCategory>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}

// Evento unificado. Los campos válidos dependen de `type`; cada grupo de campos
// está documentado con los tipos que lo usan.
struct Event {
    EventType type = EventType::None;
    EventCategory category = EventCategory::None;

    // Si un handler lo marca como manejado, se puede detener la propagación.
    bool handled = false;

    // --- WindowResize ---
    uint32_t width = 0;
    uint32_t height = 0;

    // --- WindowMoved ---
    int32_t x = 0;
    int32_t y = 0;

    // --- WindowFocus / WindowLostFocus ---
    bool focused = false;

    // --- KeyPressed / KeyReleased ---
    Key key = Key::Unknown;
    bool repeat = false;   // true si el KeyPressed proviene de auto-repetición
    KeyMods mods = KeyMods::None;

    // --- TextInput ---
    uint32_t codepoint = 0;  // Carácter Unicode (UTF-32)

    // --- MouseButtonPressed / MouseButtonReleased ---
    MouseButton button = MouseButton::Left;

    // --- MouseMoved (mouseX/mouseY = posición; deltaX/deltaY = movimiento relativo) ---
    float mouseX = 0.0f;
    float mouseY = 0.0f;
    float deltaX = 0.0f;
    float deltaY = 0.0f;

    // --- MouseScrolled ---
    float scrollX = 0.0f;
    float scrollY = 0.0f;

    // --- WindowDpiChanged (dpiScale = 1.0 a 96 DPI, 1.5 a 144 DPI, etc.) ---
    float dpiScale = 1.0f;

    // --- FileDropped (rutas soltadas; x/y = posición del cursor al soltar) ---
    std::vector<std::wstring> paths;
};

// Firma del callback que recibe los eventos de una ventana.
using EventCallback = std::function<void(Event&)>;

// Nombre legible del tipo de evento (para depuración / logging).
const char* eventTypeName(EventType type);

}  // namespace cramion::dm
