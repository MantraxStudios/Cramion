#pragma once

// Modo del cursor de los juegos, como Cursor.lockState y Cursor.visible de
// Unity:
//   None      libre (como siempre)
//   Locked    oculto, fijo en el centro y movimiento en bruto (primera persona)
//   Confined  visible pero sin salir de la ventana del juego (o de la vista
//             Juego del editor): estrategia, MOBA, camara por los bordes...
// Lo que pide el juego (request / setVisible) y lo que se aplica de verdad
// (effective / hidden) son cosas distintas: Escape en el editor, perder el
// foco o pasar a otra vista lo sueltan sin que el juego cambie su modo, y un
// clic en la vista (o volver el foco, en el juego exportado) lo vuelve a
// aplicar. Solo logica: quien tiene la ventana aplica el resultado.

#include <cctype>
#include <string>
#include <string_view>

namespace cramion::scripting {

// Los numeros son los de CursorLockMode de Unity.
enum class CursorLockMode : int { None = 0, Locked = 1, Confined = 2 };

inline const char* cursorLockModeName(CursorLockMode mode) {
    switch (mode) {
        case CursorLockMode::Locked: return "locked";
        case CursorLockMode::Confined: return "confined";
        default: return "none";
    }
}

// "none" / "locked" / "confined" (sin distinguir mayusculas; tambien "free",
// "lock", "confine") o "0" / "1" / "2". false si no es un modo.
inline bool parseCursorLockMode(std::string_view text, CursorLockMode& out) {
    std::string t;
    for (const char c : text) {
        if (!std::isspace(static_cast<unsigned char>(c))) t += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (t == "none" || t == "free" || t == "0" || t.empty()) {
        out = CursorLockMode::None;
    } else if (t == "locked" || t == "lock" || t == "1") {
        out = CursorLockMode::Locked;
    } else if (t == "confined" || t == "confine" || t == "2") {
        out = CursorLockMode::Confined;
    } else {
        return false;
    }
    return true;
}

class CursorLockState {
public:
    // resume_on_focus: al volver el foco se aplica solo (juego exportado). Si
    // es false hace falta un clic en la vista (editor, como Unity).
    explicit CursorLockState(bool resume_on_focus = true) : resume_on_focus_(resume_on_focus) {}

    void setResumeOnFocus(bool on) { resume_on_focus_ = on; }

    // --- Lo que pide el juego ---
    // Pedir el mismo modo otra vez no hace nada (un script que lo pide cada
    // frame no anula el Escape); otro modo se aplica al momento.
    void request(CursorLockMode mode) {
        if (mode == requested_) return;
        requested_ = mode;
        suspended_ = false;
    }
    void setVisible(bool visible) { visible_ = visible; }

    // --- Lo que pasa en el programa ---
    // Escape (editor) o el programa lo suelta: hasta el proximo clic.
    void release() {
        if (wantsControl()) suspended_ = true;
    }
    // La ventana gana o pierde el foco del sistema (Alt+Tab).
    void focus(bool focused) {
        if (focused == focused_) return;
        focused_ = focused;
        if (!focused && !resume_on_focus_ && wantsControl()) suspended_ = true;
    }
    // La vista del juego tiene el foco (editor: la pestana Juego). Sin ella
    // no se aplica nada, pero al volver se aplica sin clic.
    void setViewActive(bool active) { view_active_ = active; }
    // Clic en la vista del juego: vuelve lo que pide el juego.
    void click() {
        if (focused_ && view_active_) suspended_ = false;
    }
    // Al parar el juego: libre y visible.
    void reset() {
        requested_ = CursorLockMode::None;
        visible_ = true;
        suspended_ = false;
    }

    CursorLockMode requested() const { return requested_; }
    bool visible() const { return visible_; }
    bool suspended() const { return suspended_; }
    bool focused() const { return focused_; }
    // El juego quiere algo distinto del cursor normal.
    bool wantsControl() const { return requested_ != CursorLockMode::None || !visible_; }
    bool active() const { return focused_ && view_active_ && !suspended_; }

    // Lo que se aplica ahora mismo.
    CursorLockMode effective() const { return active() ? requested_ : CursorLockMode::None; }
    // El cursor oculto sobre el juego (Locked siempre lo oculta).
    bool hidden() const { return active() && (requested_ == CursorLockMode::Locked || !visible_); }

private:
    CursorLockMode requested_ = CursorLockMode::None;
    bool visible_ = true;
    bool suspended_ = false;
    bool focused_ = true;
    bool view_active_ = true;
    bool resume_on_focus_ = true;
};

}  // namespace cramion::scripting
