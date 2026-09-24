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

    uint32_t width_ = 0;
    uint32_t height_ = 0;
    bool open_ = false;
    bool trackingMouse_ = false;  // Para generar MouseEnter/MouseLeave

    float lastMouseX_ = 0.0f;
    float lastMouseY_ = 0.0f;
    bool haveLastMouse_ = false;
};

}  // namespace cramion::dm
