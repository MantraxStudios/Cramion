#pragma once

#include <cstdint>

namespace cramion::dm {

// Códigos de tecla de CramionDM.
//
// Los valores coinciden con los Virtual-Key Codes de Windows, de modo que el
// wParam de los mensajes WM_KEYDOWN / WM_KEYUP se puede convertir directamente
// a Key. Cubre teclas alfanuméricas, función, navegación, teclado numérico y
// modificadores.
enum class Key : uint16_t {
    Unknown = 0x00,

    // --- Control / edición ---
    Backspace = 0x08,
    Tab = 0x09,
    Enter = 0x0D,
    Pause = 0x13,
    CapsLock = 0x14,
    Escape = 0x1B,
    Space = 0x20,

    // --- Navegación ---
    PageUp = 0x21,
    PageDown = 0x22,
    End = 0x23,
    Home = 0x24,
    Left = 0x25,
    Up = 0x26,
    Right = 0x27,
    Down = 0x28,
    PrintScreen = 0x2C,
    Insert = 0x2D,
    Delete = 0x2E,

    // --- Números (fila superior) ---
    Num0 = 0x30,
    Num1 = 0x31,
    Num2 = 0x32,
    Num3 = 0x33,
    Num4 = 0x34,
    Num5 = 0x35,
    Num6 = 0x36,
    Num7 = 0x37,
    Num8 = 0x38,
    Num9 = 0x39,

    // --- Letras ---
    A = 0x41,
    B = 0x42,
    C = 0x43,
    D = 0x44,
    E = 0x45,
    F = 0x46,
    G = 0x47,
    H = 0x48,
    I = 0x49,
    J = 0x4A,
    K = 0x4B,
    L = 0x4C,
    M = 0x4D,
    N = 0x4E,
    O = 0x4F,
    P = 0x50,
    Q = 0x51,
    R = 0x52,
    S = 0x53,
    T = 0x54,
    U = 0x55,
    V = 0x56,
    W = 0x57,
    X = 0x58,
    Y = 0x59,
    Z = 0x5A,

    // --- Teclas de Windows / menú ---
    LeftSuper = 0x5B,   // Tecla Windows izquierda
    RightSuper = 0x5C,  // Tecla Windows derecha
    Apps = 0x5D,        // Tecla de menú contextual

    // --- Teclado numérico ---
    Keypad0 = 0x60,
    Keypad1 = 0x61,
    Keypad2 = 0x62,
    Keypad3 = 0x63,
    Keypad4 = 0x64,
    Keypad5 = 0x65,
    Keypad6 = 0x66,
    Keypad7 = 0x67,
    Keypad8 = 0x68,
    Keypad9 = 0x69,
    KeypadMultiply = 0x6A,
    KeypadAdd = 0x6B,
    KeypadSubtract = 0x6D,
    KeypadDecimal = 0x6E,
    KeypadDivide = 0x6F,

    // --- Teclas de función ---
    F1 = 0x70,
    F2 = 0x71,
    F3 = 0x72,
    F4 = 0x73,
    F5 = 0x74,
    F6 = 0x75,
    F7 = 0x76,
    F8 = 0x77,
    F9 = 0x78,
    F10 = 0x79,
    F11 = 0x7A,
    F12 = 0x7B,

    NumLock = 0x90,
    ScrollLock = 0x91,

    // --- Modificadores (izquierda / derecha) ---
    LeftShift = 0xA0,
    RightShift = 0xA1,
    LeftControl = 0xA2,
    RightControl = 0xA3,
    LeftAlt = 0xA4,
    RightAlt = 0xA5,

    // --- Puntuación / OEM (distribución US) ---
    Semicolon = 0xBA,     // ;:
    Equal = 0xBB,         // =+
    Comma = 0xBC,         // ,<
    Minus = 0xBD,         // -_
    Period = 0xBE,        // .>
    Slash = 0xBF,         // /?
    GraveAccent = 0xC0,   // `~
    LeftBracket = 0xDB,   // [{
    Backslash = 0xDC,     // \|
    RightBracket = 0xDD,  // ]}
    Apostrophe = 0xDE,    // '"

    // Cantidad de valores posibles (para dimensionar arrays de estado).
    Count = 0x100
};

// Botones del ratón.
enum class MouseButton : uint8_t {
    Left = 0,
    Right = 1,
    Middle = 2,
    X1 = 3,  // Botón lateral 1
    X2 = 4,  // Botón lateral 2

    Count = 5
};

// Máscara de bits de los modificadores activos durante un evento.
enum class KeyMods : uint8_t {
    None = 0,
    Shift = 1 << 0,
    Control = 1 << 1,
    Alt = 1 << 2,
    Super = 1 << 3,   // Tecla Windows
    CapsLock = 1 << 4,
    NumLock = 1 << 5,
};

inline KeyMods operator|(KeyMods a, KeyMods b) {
    return static_cast<KeyMods>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}
inline KeyMods operator&(KeyMods a, KeyMods b) {
    return static_cast<KeyMods>(static_cast<uint8_t>(a) & static_cast<uint8_t>(b));
}
inline KeyMods& operator|=(KeyMods& a, KeyMods b) {
    a = a | b;
    return a;
}
inline bool hasMod(KeyMods value, KeyMods flag) {
    return (static_cast<uint8_t>(value) & static_cast<uint8_t>(flag)) != 0;
}

// Devuelve un nombre legible de la tecla (útil para depuración / logging).
const char* keyName(Key key);

}  // namespace cramion::dm
