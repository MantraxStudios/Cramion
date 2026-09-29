#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "CramionDM/Event.h"
#include "CramionDM/KeyCode.h"

namespace cramion::dm {

// Estado de entrada consultable ("polling") además del sistema de eventos.
//
// El sistema de eventos (callbacks) es útil para reaccionar a acciones puntuales
// (pulsar una tecla, hacer clic). Input mantiene además el estado actual para
// consultas por frame típicas de un motor:
//
//   if (input.isKeyDown(Key::W)) mover();
//   if (input.isKeyPressed(Key::Space)) saltar();  // solo el frame de la pulsación
//
// Uso:
//   1) Alimentar Input con los eventos de la ventana:  input.onEvent(ev);
//   2) Al final de cada frame llamar a  input.newFrame();  para actualizar los
//      estados "pressed/released" de un solo frame y los deltas del ratón.
class Input {
public:
    Input() { reset(); }

    // Procesa un evento de la ventana y actualiza el estado interno.
    void onEvent(const Event& event);

    // Debe llamarse una vez por frame, DESPUÉS de procesar los eventos, para
    // limpiar los estados "de un frame" (justPressed / justReleased / scroll /
    // deltas del ratón).
    void newFrame();

    // Reinicia todo el estado a cero.
    void reset();

    // --- Teclado ---
    // ¿La tecla está mantenida pulsada en este momento?
    bool isKeyDown(Key key) const;
    // ¿La tecla se pulsó justamente en este frame? (flanco de bajada)
    bool isKeyPressed(Key key) const;
    // ¿La tecla se soltó justamente en este frame? (flanco de subida)
    bool isKeyReleased(Key key) const;

    // --- Ratón ---
    bool isMouseButtonDown(MouseButton button) const;
    bool isMouseButtonPressed(MouseButton button) const;
    bool isMouseButtonReleased(MouseButton button) const;

    float mouseX() const { return mouseX_; }
    float mouseY() const { return mouseY_; }
    float mouseDeltaX() const { return mouseDeltaX_; }
    float mouseDeltaY() const { return mouseDeltaY_; }
    float scrollX() const { return scrollX_; }
    float scrollY() const { return scrollY_; }

    // Modificadores activos en el último evento de teclado.
    KeyMods mods() const { return mods_; }

    // --- Pantalla tactil ---
    // Los dedos de este frame (los que se levantaron siguen un frame con
    // phase = Ended). Posiciones en pixeles de la ventana.
    struct Touch {
        int32_t id = 0;
        float x = 0.0f;
        float y = 0.0f;
        float deltaX = 0.0f;
        float deltaY = 0.0f;
        float startX = 0.0f;
        float startY = 0.0f;
        TouchPhase phase = TouchPhase::Began;
    };
    const std::vector<Touch>& touches() const { return touches_; }
    // Se toco la pantalla alguna vez (movil o PC tactil).
    bool touchScreen() const { return touch_screen_; }

    // --- Mando ---
    bool isGamepadButtonDown(GamepadButton button) const;
    bool isGamepadButtonPressed(GamepadButton button) const;
    bool isGamepadButtonReleased(GamepadButton button) const;
    // Sticks en [-1, 1] con zona muerta aplicada; gatillos en [0, 1].
    float gamepadAxis(GamepadAxis axis) const;
    bool gamepadConnected() const { return gamepad_connected_; }

    // --- Mandos de realidad virtual (los rellena el sistema OpenXR cada frame) ---
    void setXrButton(XrButton button, bool down);
    void setXrAxis(XrAxis axis, float value);
    bool isXrButtonDown(XrButton button) const;
    bool isXrButtonPressed(XrButton button) const;
    bool isXrButtonReleased(XrButton button) const;
    float xrAxis(XrAxis axis) const;

    // --- Joystick virtual (controles tactiles en pantalla) ---
    // Se suma a los ejes Horizontal/Vertical de los scripts. Y hacia arriba.
    void setVirtualStick(float x, float y) {
        virtualStickX_ = x;
        virtualStickY_ = y;
    }
    float virtualStickX() const { return virtualStickX_; }
    float virtualStickY() const { return virtualStickY_; }

private:
    static constexpr size_t kKeyCount = static_cast<size_t>(Key::Count);
    static constexpr size_t kButtonCount = static_cast<size_t>(MouseButton::Count);

    std::array<bool, kKeyCount> keyDown_{};
    std::array<bool, kKeyCount> keyPressed_{};
    std::array<bool, kKeyCount> keyReleased_{};

    std::array<bool, kButtonCount> buttonDown_{};
    std::array<bool, kButtonCount> buttonPressed_{};
    std::array<bool, kButtonCount> buttonReleased_{};

    float mouseX_ = 0.0f;
    float mouseY_ = 0.0f;
    float mouseDeltaX_ = 0.0f;
    float mouseDeltaY_ = 0.0f;
    float scrollX_ = 0.0f;
    float scrollY_ = 0.0f;

    KeyMods mods_ = KeyMods::None;

    std::vector<Touch> touches_;
    bool touch_screen_ = false;

    static constexpr size_t kGamepadButtonCount = static_cast<size_t>(GamepadButton::Count);
    static constexpr size_t kGamepadAxisCount = static_cast<size_t>(GamepadAxis::Count);
    std::array<bool, kGamepadButtonCount> padDown_{};
    std::array<bool, kGamepadButtonCount> padPressed_{};
    std::array<bool, kGamepadButtonCount> padReleased_{};
    std::array<float, kGamepadAxisCount> padAxes_{};
    bool gamepad_connected_ = false;

    float virtualStickX_ = 0.0f;
    float virtualStickY_ = 0.0f;

    static constexpr size_t kXrButtonCount = static_cast<size_t>(XrButton::Count);
    std::array<bool, kXrButtonCount> xrDown_{};
    std::array<bool, kXrButtonCount> xrPressed_{};
    std::array<bool, kXrButtonCount> xrReleased_{};
    std::array<float, static_cast<size_t>(XrAxis::Count)> xrAxes_{};
};

}  // namespace cramion::dm
