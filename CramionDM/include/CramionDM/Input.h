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

    // Quita de esta copia del estado un boton del raton (mantenido, pulsado y
    // soltado) o la rueda: la entrada que llega a los scripts cuando ese clic
    // es de la interfaz del juego (ui::UiSystem::filterGameInput).
    void suppressMouseButton(MouseButton button);
    void suppressScroll() { scrollX_ = scrollY_ = 0.0f; }

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
    // Hasta 4 mandos (Event::gamepadIndex). Sin indice, "cualquiera": un
    // boton cuenta si esta pulsado en alguno y los ejes son los del ultimo
    // mando que se uso (el de un jugador, aunque haya otro conectado).
    static constexpr int kMaxGamepads = 4;
    bool isGamepadButtonDown(GamepadButton button) const;
    bool isGamepadButtonPressed(GamepadButton button) const;
    bool isGamepadButtonReleased(GamepadButton button) const;
    // Sticks en [-1, 1] con zona muerta radial aplicada; gatillos en [0, 1].
    float gamepadAxis(GamepadAxis axis) const;
    bool gamepadConnected() const { return connected_mask_ != 0; }
    // Un mando concreto (0..kMaxGamepads-1; fuera de rango, false / 0).
    bool isGamepadButtonDown(int pad, GamepadButton button) const;
    bool isGamepadButtonPressed(int pad, GamepadButton button) const;
    bool isGamepadButtonReleased(int pad, GamepadButton button) const;
    float gamepadAxis(int pad, GamepadAxis axis) const;
    bool gamepadConnected(int pad) const;
    GamepadKind gamepadKind(int pad) const;
    int gamepadCount() const;
    // El ultimo mando que se uso (el que dan las consultas sin indice).
    int activeGamepad() const { return active_pad_; }
    // Zonas muertas: stick radial (0.15 por defecto; XInput recomienda
    // 0.24) y gatillo (0 por defecto). Se reescala lo de fuera a [0, 1].
    void setGamepadDeadZones(float stick, float trigger);
    float gamepadStickDeadZone() const { return stick_dead_zone_; }
    float gamepadTriggerDeadZone() const { return trigger_dead_zone_; }

    // --- Dispositivo activo (iconos de los botones en la interfaz) ---
    // Cambia con la ultima entrada clara: tecla o clic (o raton de mas de
    // unos pixeles), boton o eje del mando pasada la mitad, o un dedo.
    enum class Device : uint8_t { KeyboardMouse = 0, Gamepad, Touch };
    Device activeDevice() const { return active_device_; }
    // true solo el frame en que activeDevice() cambio.
    bool activeDeviceChanged() const { return device_changed_; }

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
    struct Pad {
        std::array<bool, kGamepadButtonCount> down{};
        std::array<bool, kGamepadButtonCount> pressed{};
        std::array<bool, kGamepadButtonCount> released{};
        std::array<float, kGamepadAxisCount> axes{};  // en bruto
        GamepadKind kind = GamepadKind::Unknown;
    };
    std::array<Pad, kMaxGamepads> pads_{};
    // "Cualquier mando": OR de los botones de todos, con sus flancos propios.
    std::array<bool, kGamepadButtonCount> padDown_{};
    std::array<bool, kGamepadButtonCount> padPressed_{};
    std::array<bool, kGamepadButtonCount> padReleased_{};
    uint8_t connected_mask_ = 0;  // bit por hueco
    int active_pad_ = 0;
    float stick_dead_zone_ = 0.15f;
    float trigger_dead_zone_ = 0.0f;
    void markPadConnected(int pad, GamepadKind kind);
    void setPadButton(int pad, size_t button, bool down);
    void releasePad(int pad);

    Device active_device_ = Device::KeyboardMouse;
    bool device_changed_ = false;
    void useDevice(Device device);

    float virtualStickX_ = 0.0f;
    float virtualStickY_ = 0.0f;

    static constexpr size_t kXrButtonCount = static_cast<size_t>(XrButton::Count);
    std::array<bool, kXrButtonCount> xrDown_{};
    std::array<bool, kXrButtonCount> xrPressed_{};
    std::array<bool, kXrButtonCount> xrReleased_{};
    std::array<float, static_cast<size_t>(XrAxis::Count)> xrAxes_{};
};

// --- Vibracion de los mandos ---
// pad 0..3 (-1 = todos). low = motor grave (el grande, izquierdo), high = el
// agudo (derecho), en [0, 1]; con seconds > 0 se para sola. 0, 0 la para.
// La atiende la ventana que lee los mandos (Windows: XInput y DualShock 4 /
// DualSense por USB o Bluetooth; Linux: evdev con force feedback). Sin
// ventana que la atienda (Android, pruebas), devuelve false.
bool setGamepadVibration(int pad, float low, float high, float seconds = 0.0f);
// Uso interno de Window: quien atiende setGamepadVibration (nullptr = nadie).
using GamepadVibrationHandler = bool (*)(void* user, int pad, float low, float high, float seconds);
void setGamepadVibrationHandler(GamepadVibrationHandler handler, void* user);
void* gamepadVibrationUser();

}  // namespace cramion::dm
