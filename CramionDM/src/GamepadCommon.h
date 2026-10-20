#pragma once

// Parte comun de los mandos de escritorio: Window.cpp (XInput y DualShock 4 /
// DualSense por HID) y WindowX11.cpp (evdev). Cada lector deja el estado de
// su mando en un State; Slots reparte los 4 huecos (Event::gamepadIndex) y
// manda solo lo que cambio con los mismos eventos que WindowAndroid.cpp
// (GamepadButtonPressed/Released, GamepadAxisMoved, GamepadConnected y
// GamepadDisconnected). Los ejes van en bruto: la zona muerta la pone Input.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "CramionDM/Event.h"
#include "CramionDM/Input.h"

namespace cramion::dm::gamepad {

inline constexpr int kSlots = Input::kMaxGamepads;
inline constexpr std::size_t kButtons = static_cast<std::size_t>(GamepadButton::Count);
inline constexpr std::size_t kAxes = static_cast<std::size_t>(GamepadAxis::Count);
// Cambio minimo de un eje para mandar otro GamepadAxisMoved (~1/512 del
// recorrido: por debajo es ruido del sensor y solo llenaria la cola).
inline constexpr float kAxisEpsilon = 1.0f / 512.0f;
// Cada cuanto se vuelve a buscar un mando desconectado (XInput de un hueco
// vacio cuesta hasta milisegundos; evdev, abrir la carpeta).
inline constexpr double kProbeSeconds = 1.0;

// Lo leido de un mando en un momento. Sticks en [-1, 1] con la Y hacia
// arriba; gatillos en [0, 1].
struct State {
    std::array<bool, kButtons> buttons{};
    std::array<float, kAxes> axes{};

    void set(GamepadButton b, bool down) { buttons[static_cast<std::size_t>(b)] = down; }
    void set(GamepadAxis a, float v) { axes[static_cast<std::size_t>(a)] = v; }
    // Cruceta como hat (-1/0/1 por eje; y = -1 arriba, como evdev).
    void setHat(int x, int y) {
        set(GamepadButton::DpadLeft, x < 0);
        set(GamepadButton::DpadRight, x > 0);
        set(GamepadButton::DpadUp, y < 0);
        set(GamepadButton::DpadDown, y > 0);
    }
};

inline double nowSeconds() {
    using Clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
}

// Vibracion pedida para un hueco: motores en [0, 1] y cuando se para sola
// (until = 0: hasta que la cambien).
struct Rumble {
    float low = 0.0f;
    float high = 0.0f;
    double until = 0.0;
};

// Los huecos de mando: quien los ocupa y lo ultimo que se emitio de cada uno.
class Slots {
public:
    bool used(int slot) const { return valid(slot) && slots_[static_cast<std::size_t>(slot)].used; }
    GamepadKind kind(int slot) const { return used(slot) ? slots_[static_cast<std::size_t>(slot)].kind : GamepadKind::Unknown; }

    // Ocupa el primer hueco libre y emite GamepadConnected. -1 si estan los 4.
    template <typename Emit>
    int connect(GamepadKind kind, Emit&& emit) {
        for (int i = 0; i < kSlots; ++i) {
            Slot& s = slots_[static_cast<std::size_t>(i)];
            if (s.used) continue;
            s = Slot{};
            s.used = true;
            s.kind = kind;
            Event e = make(i, EventType::GamepadConnected);
            e.gamepadKind = kind;
            emit(e);
            return i;
        }
        return -1;
    }

    // Suelta lo pulsado (para que Input vea los flancos) y libera el hueco.
    template <typename Emit>
    void disconnect(int slot, Emit&& emit) {
        if (!used(slot)) return;
        update(slot, State{}, emit);
        slots_[static_cast<std::size_t>(slot)].used = false;
        Event e = make(slot, EventType::GamepadDisconnected);
        emit(e);
    }

    // Emite solo lo que cambio desde lo ultimo mandado de ese hueco.
    template <typename Emit>
    void update(int slot, const State& now, Emit&& emit) {
        if (!used(slot)) return;
        State& sent = slots_[static_cast<std::size_t>(slot)].sent;
        for (std::size_t b = 0; b < kButtons; ++b) {
            if (now.buttons[b] == sent.buttons[b]) continue;
            sent.buttons[b] = now.buttons[b];
            Event e = make(slot, now.buttons[b] ? EventType::GamepadButtonPressed : EventType::GamepadButtonReleased);
            e.gamepadButton = static_cast<GamepadButton>(b);
            emit(e);
        }
        for (std::size_t a = 0; a < kAxes; ++a) {
            const float v = now.axes[a];
            const float before = sent.axes[a];
            // Siempre el reposo exacto y los topes, aunque el paso sea pequeno.
            const bool edge = (v == 0.0f || std::fabs(v) >= 1.0f) && v != before;
            if (!edge && std::fabs(v - before) < kAxisEpsilon) continue;
            sent.axes[a] = v;
            Event e = make(slot, EventType::GamepadAxisMoved);
            e.gamepadAxis = static_cast<GamepadAxis>(a);
            e.value = v;
            emit(e);
        }
    }

    // Sin foco: todo suelto y en reposo, pero los mandos siguen conectados.
    template <typename Emit>
    void releaseAll(Emit&& emit) {
        for (int i = 0; i < kSlots; ++i) update(i, State{}, emit);
    }

private:
    struct Slot {
        bool used = false;
        GamepadKind kind = GamepadKind::Unknown;
        State sent;
    };

    static bool valid(int slot) { return slot >= 0 && slot < kSlots; }
    static Event make(int slot, EventType type) {
        Event e;
        e.type = type;
        e.category = EventCategory::Input;
        e.gamepadIndex = slot;
        return e;
    }

    std::array<Slot, kSlots> slots_{};
};

// Motor en [0, 1] a 8 y 16 bits (informes HID, XInput y evdev).
inline std::uint8_t motor8(float v) { return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); }
inline std::uint16_t motor16(float v) {
    return static_cast<std::uint16_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 65535.0f));
}

}  // namespace cramion::dm::gamepad
