#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>
#include <functional>
#include <string>

#include "CramionDM/Event.h"

namespace cramion::dm {

// Parámetros de creación de la ventana.
struct WindowConfig {
    std::wstring title = L"CramionDM";
    uint32_t width = 1280;
    uint32_t height = 720;
    bool resizable = true;
    bool maximized = false;  // abrir maximizada (width/height = tamano al restaurar)
    // Sin la barra de titulo de Windows: la dibuja la aplicacion (ver
    // setCaptionHitTest). Se conservan el borde para redimensionar, el snap,
    // las animaciones y el menu de sistema.
    bool custom_title_bar = false;
};

// Ventana Win32 que traduce los mensajes del sistema a eventos de CramionDM.
//
// Uso típico:
//   Window window;
//   window.create({.title = L"Demo"});
//   window.setEventCallback([](Event& e){ ... });
//   while (window.isOpen()) {
//       window.pumpEvents();   // procesa la cola de mensajes -> emite eventos
//       // render...
//   }
class Window {
public:
    Window() = default;
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    // Crea la ventana. Devuelve false si falla el registro o la creación.
    bool create(const WindowConfig& config = {});

    // Cierra y destruye la ventana.
    void destroy();

    // Procesa todos los mensajes pendientes de la cola (no bloquea) y despacha
    // los eventos correspondientes al callback registrado.
    void pumpEvents();

    // Registra el callback que recibirá los eventos.
    void setEventCallback(EventCallback callback) { callback_ = std::move(callback); }

    // Recibe cada mensaje de Win32 antes que la ventana (p. ej. para Dear
    // ImGui: ImGui_ImplWin32_WndProcHandler). Si devuelve true el mensaje se
    // da por atendido y no se procesa ni se convierte en evento.
    using MessageHook = std::function<bool(HWND, UINT, WPARAM, LPARAM)>;
    void setMessageHook(MessageHook hook) { message_hook_ = std::move(hook); }

    bool isOpen() const { return open_; }
    HWND handle() const { return hwnd_; }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }

    // Cambia el título de la ventana.
    void setTitle(const std::wstring& title);

    // --- Barra de titulo propia (custom_title_bar) ---
    // Devuelve true si el punto (pixeles del area cliente) es zona de
    // arrastre de la barra: mover la ventana, doble clic = maximizar.
    using CaptionHitTest = std::function<bool(int x, int y)>;
    void setCaptionHitTest(CaptionHitTest test) { caption_test_ = std::move(test); }
    bool customTitleBar() const { return custom_title_bar_; }
    void minimize();
    void toggleMaximize();
    bool isMaximized() const;
    // Como pulsar la X: llega el evento WindowClose.
    void requestClose();
    // Menu de sistema de Windows (Alt+Espacio) en ese punto de la pantalla.
    void showSystemMenu(int screen_x, int screen_y);

    // --- Cursor capturado (juegos en primera persona) ---
    // Oculta el cursor, lo encierra en la ventana (o en `client_region`, en
    // pixeles del area cliente: la vista Juego del editor) y manda el
    // movimiento como MouseRawMoved (raw input, sin tope en los bordes).
    // Al perder el foco se suelta solo y al recuperarlo se vuelve a encerrar.
    void setCursorCaptured(bool captured, const RECT* client_region = nullptr);
    bool cursorCaptured() const { return cursor_captured_; }

private:
    // WndProc estático que redirige al método de instancia.
    static LRESULT CALLBACK wndProcThunk(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT handleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    // Emite un evento hacia el callback (si hay uno registrado).
    void dispatch(Event& event);

    // Calcula los modificadores activos en este momento.
    static KeyMods currentMods();

    HWND hwnd_ = nullptr;
    HINSTANCE hinstance_ = nullptr;
    EventCallback callback_;
    MessageHook message_hook_;
    CaptionHitTest caption_test_;
    bool custom_title_bar_ = false;
    LRESULT hitTest(LPARAM lParam) const;

    uint32_t width_ = 0;
    uint32_t height_ = 0;
    bool open_ = false;
    bool trackingMouse_ = false;  // Para generar MouseEnter/MouseLeave

    float lastMouseX_ = 0.0f;
    float lastMouseY_ = 0.0f;
    bool haveLastMouse_ = false;

    void applyCursorClip();
    bool cursor_captured_ = false;
    bool cursor_hidden_ = false;
    bool raw_input_registered_ = false;
    bool has_capture_rect_ = false;
    RECT capture_rect_{};
};

}  // namespace cramion::dm
