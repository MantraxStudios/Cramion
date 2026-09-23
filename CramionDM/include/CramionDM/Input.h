#pragma once

#include <array>
#include <cstddef>

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
};

}  // namespace cramion::dm
