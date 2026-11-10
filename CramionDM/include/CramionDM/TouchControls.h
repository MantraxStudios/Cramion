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
    bool visible = true; // al empezar (Input::setTouchButton(etiqueta, visible))
    // Colocado en dp desde una esquina (Input.placeTouchButton): el mismo
    // tamano fisico en un movil y en una tableta, pegado al borde que toca.
    // Si no, x/y son fracciones de la pantalla y size multiplica 34 dp.
    bool anchored = false;
    float anchor_x = 1.0f;   // 0 izquierda, 1 derecha
    float anchor_y = 1.0f;   // 0 arriba, 1 abajo
    float offset_x = 0.0f;   // dp desde el ancla (negativo = hacia la izquierda)
    float offset_y = 0.0f;   // dp desde el ancla (negativo = hacia arriba)
    float radius_dp = 0.0f;  // radio en dp (0 = 34 * size)
    // Aspecto: 0 normal, 1 principal (atacar), 2 menu (pequeno, plano),
    // 3 contextual (resaltado: usar, montar...).
    int style = 0;
    std::string text;  // lo que se dibuja (vacio = label)
    // Aspecto (Input.setTouchButtonIcon / setTouchButtonState): imagen de
    // Assets ("UI/Pixel/Habilidades/slash.png"), recarga que falta (0..1, un
    // barrido oscuro), texto encima (segundos, cantidad), apagado (sin
    // recurso) y color del aro (RGBA 0xRRGGBBAA, 0 = el del estilo).
    std::string icon;
    float cooldown = 0.0f;
    std::string badge;
    bool dim = false;
    std::uint32_t ring = 0;
};

struct TouchLayout {
    bool enabled = true;
    bool joystick = true;
    bool look = true;
    float look_sensitivity = 1.0f;
    float scale = 1.0f;    // tamano de stick y botones
    float opacity = 0.55f;
    bool tap_clicks = true;  // toque corto en la zona de mirar = clic izquierdo
    float stick_radius_dp = 0.0f;  // 0 = 64 dp
    // Empujar el stick mas alla del aro mantiene esta tecla ("LeftShift" =
    // correr); vacio = no.
    std::string stick_sprint_action;
    // Donde se dibuja el stick flotante en reposo: centro en dp desde abajo a
    // la izquierda (0, 0 = el sitio por defecto). Lo mueve el editor.
    float stick_rest_x = 0.0f;
    float stick_rest_y = 0.0f;
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

    // Cambios en el juego (scripts). Lo que se apaga con un dedo encima se suelta
    // en el siguiente update().
    void setEnabled(bool enabled);
    void setJoystick(bool enabled);
    void setLook(bool enabled);
    // Por etiqueta ("Saltar"); false si no existe.
    bool setButtonVisible(const std::string& label, bool visible);
    // Coloca (o crea) un boton en dp desde una esquina (ver TouchButton).
    void placeButton(const TouchButton& button);
    bool setButtonText(const std::string& label, const std::string& text);
    bool setButtonIcon(const std::string& label, const std::string& icon);
    bool setButtonState(const std::string& label, float cooldown, const std::string& badge, bool dim, std::uint32_t ring);
    void setStick(float radius_dp, const std::string& sprint_action);
    void setLookSensitivity(float sensitivity);
    void setOpacity(float opacity) { layout_.opacity = opacity < 0.05f ? 0.05f : (opacity > 1.0f ? 1.0f : opacity); }

    // --- Editor de la disposicion (el jugador mueve y cambia de tamano) ---
    // Con el editor abierto los dedos no pulsan nada: un dedo sobre un boton
    // visible colocado en dp (o sobre el stick) lo arrastra, un segundo dedo lo
    // cambia de tamano pellizcando, y la barra de arriba tiene -, +, Reset y
    // Done. Al soltar un boton se ancla a la esquina mas cercana (asi sirve en
    // otras pantallas). Reset y Done se avisan con takeEditEvent().
    void setEditMode(bool on);
    bool editMode() const { return edit_; }
    std::string takeEditEvent();  // "reset", "done" o "" (una sola vez)
    // Lo colocado en dp, en texto (para guardarlo) y vuelta. setLayoutString
    // solo cambia los botones que ya existen.
    std::string layoutString() const;
    void setLayoutString(const std::string& text);
    struct EditItem {
        std::string label;
        float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    };
    std::vector<EditItem> editPanel() const;
    int editSelected() const { return edit_selected_; }  // indice del boton, -2 el stick, -1 nada
    static constexpr int kEditStick = -2;
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
        bool sprint = false;  // empujado mas alla del aro (corre)
        bool can_sprint = false;
        float sprint_radius = 0.0f;
    };
    StickView stick() const;
    float buttonRadius(const TouchButton& button) const;
    float buttonCenterX(const TouchButton& button) const;
    float buttonCenterY(const TouchButton& button) const;
    // Pixeles por dp de los controles colocados (densidad, escala y un ajuste
    // suave al alto de la pantalla: un movil apaisado tiene ~360-430 dp).
    float unit() const;
    bool buttonDown(std::size_t index) const;
    float stickX() const { return stick_x_; }
    float stickY() const { return stick_y_; }

private:
    enum class Role : std::uint8_t { Stick, Look, Button, Mouse, Edit, EditPinch };
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
        bool looking = false;  // el dedo que gira la camara (solo uno a la vez)
        bool dragged = false;  // un boton arrastrado: tambien gira la camara
        float grab_x = 0.0f;   // editor: del dedo al centro de lo que arrastra
        float grab_y = 0.0f;
    };

    float stickRadius() const;
    void pressAction(const std::string& action, bool down, std::vector<Event>& out) const;
    void updateStick(const Finger& finger, std::vector<Event>& out);
    void setSprint(bool on, std::vector<Event>& out);
    Finger* find(std::int32_t id);

    bool fingerStillValid(const Finger& finger) const;
    bool lookBusy(const Finger* except) const;
    void emitLook(float dx, float dy, std::vector<Event>& out) const;
    void onEditEvent(const Event& event);
    int editHit(float x, float y) const;
    void editMoveTo(int item, float x, float y);
    float editRadiusDp(int item) const;
    void editSetRadiusDp(int item, float r);

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
    bool sprint_ = false;
    bool edit_ = false;
    int edit_selected_ = -1;
    std::string edit_event_;
    float pinch_start_dist_ = 0.0f;
    float pinch_start_radius_ = 0.0f;
};

// touchId de los eventos de raton/teclado que salen de los botones tactiles
// (no de un dedo sobre la interfaz): el reproductor no se los pasa a la
// interfaz (un "Atacar" no hace clic donde se toco por ultima vez).
constexpr std::int32_t kTouchButtonEventId = -0x7A11;

}  // namespace cramion::dm
