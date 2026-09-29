#pragma once

// Controles tactiles en pantalla para moviles (el juego exportado a Android):
//
//   Joystick   mitad izquierda: donde se apoya el dedo aparece el stick y
//              mueve los ejes Horizontal/Vertical (como WASD).
//   Mirar      el resto de la pantalla: arrastrar mueve la camara (el delta
//              del raton, como mirar con el raton capturado); un toque corto
//              es un clic izquierdo.
//   Botones    circulos que pulsan una tecla o un boton del raton mientras
//              se mantienen ("Saltar" = Espacio, "Disparar" = Mouse0...).
//
// No dibuja: convierte los eventos Touch* en eventos de teclado y raton que
// van a dm::Input (y a la interfaz), y expone el estado para que el player
// lo pinte. Es independiente de la plataforma (se prueba en Windows).

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "CramionDM/Event.h"
#include "CramionDM/Input.h"
#include "CramionDM/KeyCode.h"

namespace cramion::dm {

struct TouchButton {
    std::string label = "Saltar";
    // Tecla ("Space", "E", "LeftShift"...) o "Mouse0".."Mouse2".
    std::string action = "Space";
    float x = 0.88f;   // centro, fraccion del ancho de la pantalla
    float y = 0.72f;   // centro, fraccion del alto
    float size = 1.0f; // multiplica el radio
    bool visible = true; // al empezar (Lua: Input.setTouchButton(etiqueta, visible))
};

struct TouchLayout {
    bool enabled = true;
    bool joystick = true;
    bool look = true;
    float look_sensitivity = 1.0f;
    float scale = 1.0f;    // tamano de stick y botones
    float opacity = 0.55f;
    bool tap_clicks = true;  // toque corto en la zona de mirar = clic izquierdo
    std::vector<TouchButton> buttons;
};

// Botones por defecto de un juego en tercera/primera persona.
TouchLayout defaultTouchLayout();

// game.ini: lineas touch_*=... (una touch_button por boton:
// "etiqueta|accion|x|y|tamano"). Parse lee una clave; devuelve false si no es
// de los controles tactiles.
std::string touchLayoutIni(const TouchLayout& layout);
bool parseTouchLayoutLine(const std::string& key, const std::string& value, TouchLayout& layout);

// "Space" -> Key::Space ("W", "LeftShift", "Enter", "1"...); Unknown si no existe.
Key keyFromName(const std::string& name);

class TouchControls {
public:
    void setLayout(const TouchLayout& layout);
    const TouchLayout& layout() const { return layout_; }

    // Cambios en el juego (Lua). Lo que se apaga con un dedo encima se suelta
    // en el siguiente update().
    void setEnabled(bool enabled);
    void setJoystick(bool enabled);
    void setLook(bool enabled);
    // Por etiqueta ("Saltar"); false si no existe.
    bool setButtonVisible(const std::string& label, bool visible);
    // Suelta los dedos de lo que se apago (teclas arriba, stick al centro).
    void update(std::vector<Event>& out);
    // La interfaz del juego tiene prioridad: si hay un control (boton,
    // deslizador...) donde se apoya el dedo, ese dedo es el raton aunque
    // caiga en la zona del joystick o de mirar.
    void setUiHitTest(std::function<bool(float x, float y)> test) { ui_hit_ = std::move(test); }

    // Tamano de la pantalla en pixeles y densidad (1 = 160 dpi en Android).
    void setScreen(float width, float height, float density);

    // Un evento Touch* (los demas se ignoran). `out` recibe los eventos de
    // teclado/raton equivalentes para Input y la interfaz. `time` en segundos.
    void onEvent(const Event& event, double time, std::vector<Event>& out);

    // Suelta todo (la app pierde el foco): teclas arriba, stick al centro.
    void releaseAll(std::vector<Event>& out);

    // Cada frame, despues de los eventos: el stick a los ejes.
    void apply(Input& input) const { input.setVirtualStick(stick_x_, stick_y_); }

    // --- Para dibujar ---
    struct StickView {
        bool active = false;
        float base_x = 0.0f;
        float base_y = 0.0f;
        float knob_x = 0.0f;
        float knob_y = 0.0f;
        float radius = 0.0f;
    };
    StickView stick() const;
    float buttonRadius(const TouchButton& button) const;
    float buttonCenterX(const TouchButton& button) const { return button.x * width_; }
    float buttonCenterY(const TouchButton& button) const { return button.y * height_; }
    bool buttonDown(std::size_t index) const;
    float stickX() const { return stick_x_; }
    float stickY() const { return stick_y_; }

private:
    enum class Role : std::uint8_t { Stick, Look, Button, Mouse };
    struct Finger {
        std::int32_t id = 0;
        Role role = Role::Look;
        std::size_t button = 0;
        float x = 0.0f;
        float y = 0.0f;
        float start_x = 0.0f;
        float start_y = 0.0f;
        double start_time = 0.0;
        float travel = 0.0f;  // pixeles recorridos (un toque apenas se mueve)
    };

    float stickRadius() const;
    void pressAction(const std::string& action, bool down, std::vector<Event>& out) const;
    void updateStick(const Finger& finger);
    Finger* find(std::int32_t id);

    bool fingerStillValid(const Finger& finger) const;

    std::function<bool(float, float)> ui_hit_;
    TouchLayout layout_ = defaultTouchLayout();
    float width_ = 1280.0f;
    float height_ = 720.0f;
    float density_ = 1.0f;
    std::vector<Finger> fingers_;
    float stick_base_x_ = 0.0f;
    float stick_base_y_ = 0.0f;
    float stick_x_ = 0.0f;
    float stick_y_ = 0.0f;
};

}  // namespace cramion::dm
