#pragma once

#if defined(__ANDROID__)

// Android (NativeActivity + android_native_app_glue): la "ventana" es la
// ANativeWindow de la actividad. La entrada llega como eventos Touch*,
// Key* (teclado fisico y boton Atras = Escape) y Gamepad*.

#include <cstdint>
#include <functional>
#include <string>

#include "CramionDM/Event.h"

struct android_app;
struct ANativeWindow;

namespace cramion::dm {

struct WindowConfig {
    std::wstring title = L"CramionDM";
    uint32_t width = 0;   // ignorado: toda la pantalla
    uint32_t height = 0;
    bool resizable = true;
    bool maximized = true;
    bool custom_title_bar = false;
};

class Window {
public:
    // La actividad (android_main la recibe); antes de create().
    static void setApp(android_app* app);
    static android_app* app();

    Window() = default;
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    // Espera a que Android de la superficie. false si la app se cierra antes.
    bool create(const WindowConfig& config = {});
    void destroy();

    // Procesa los eventos pendientes. En segundo plano (sin superficie)
    // espera sin gastar bateria hasta que la app vuelve o se cierra.
    void pumpEvents();

    void setEventCallback(EventCallback callback) { callback_ = std::move(callback); }

    bool isOpen() const { return open_; }
    ANativeWindow* handle() const { return window_; }
    // Tamano en pixeles de la imagen del juego (con setMaxShortSide, menor
    // que la pantalla: Android la escala sola al mostrarla).
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    // Densidad (1 = 160 dpi) en pixeles de la imagen: para dimensionar en dp.
    float density() const { return density_ * input_scale_; }
    bool focused() const { return focused_; }
    // Lado corto de la imagen del juego como mucho `pixels` (0 = el de la
    // pantalla). Un movil de 2400x1080 con 720 dibuja 1600x720: 2.25 veces
    // menos pixeles en todo (3D, post-proceso, interfaz) y el escalado lo
    // hace la pantalla sin coste. Los toques llegan en pixeles de la imagen.
    // Si el tamano cambia llega un WindowResize.
    void setMaxShortSide(uint32_t pixels);

    void setTitle(const std::wstring&) {}
    void setCursorCaptured(bool captured) { cursor_captured_ = captured; }
    bool cursorCaptured() const { return cursor_captured_; }
    // Sin cursor en Android: solo se recuerda (la misma llamada que en PC).
    void setCursorConfined(bool confined) { cursor_confined_ = confined; }
    bool cursorConfined() const { return cursor_confined_; }
    void setCursorVisible(bool) {}

    // Vibracion corta (necesita el permiso VIBRATE en el manifiesto).
    void vibrate(int milliseconds);
    // Mandos: aun sin vibracion en Android (la misma llamada que en PC).
    bool setGamepadVibration(int, float, float, float = 0.0f) { return false; }
    void setGamepadsInBackground(bool) {}
    // Orientacion pedida (Activity.setRequestedOrientation): uno de los
    // ActivityInfo.SCREEN_ORIENTATION_* (0 horizontal, 1 vertical, 6/7 con
    // sensor, 10 libre). Al girar llega un WindowResize.
    void setOrientation(int android_orientation);
    // Carpetas de la app: datos internos y (si existe) la del .obb.
    std::string internalDataPath() const;
    std::string externalDataPath() const;
    std::string obbPath() const;

    // Uso interno de los callbacks de la glue.
    void handleCommand(int32_t command);
    int32_t handleInput(const void* event);

private:
    void dispatch(Event& event);
    void refreshSize();

    EventCallback callback_;
    ANativeWindow* window_ = nullptr;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    float density_ = 1.0f;
    uint32_t max_short_side_ = 0;
    float input_scale_ = 1.0f;  // pixeles de la imagen por pixel de la pantalla
    bool open_ = false;
    bool focused_ = true;
    bool resumed_ = true;
    bool cursor_captured_ = false;
    bool cursor_confined_ = false;
    float axes_[8] = {};
    float hat_x_ = 0.0f;
    float hat_y_ = 0.0f;
};

}  // namespace cramion::dm

#elif defined(__linux__)

// Linux (X11 / Xlib; en Wayland va por XWayland): ventana con su teclado,
// raton (con captura en primera persona por XInput2 relativo... aqui con
// warp al centro) y redimensionado. La superficie de Vulkan sale de handle().

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "CramionDM/Event.h"

namespace cramion::dm {

struct WindowConfig {
    std::wstring title = L"CramionDM";
    uint32_t width = 1280;
    uint32_t height = 720;
    bool resizable = true;
    bool maximized = false;
    bool custom_title_bar = false;  // ignorado en Linux
    bool visible = true;
};

class EvdevGamepads;

// Lo que necesita Vulkan (VK_KHR_xlib_surface): la conexion y la ventana.
struct NativeHandle {
    void* display = nullptr;    // Display*
    unsigned long window = 0;   // ::Window
};

class Window {
public:
    Window();  // en el .cpp: los mandos son un tipo incompleto aqui
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    bool create(const WindowConfig& config = {});
    void destroy();
    void pumpEvents();
    void setEventCallback(EventCallback callback) { callback_ = std::move(callback); }

    bool isOpen() const { return open_; }
    const NativeHandle* handle() const { return &native_; }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    float density() const { return 1.0f; }
    bool focused() const { return focused_; }

    void setTitle(const std::wstring& title);
    void minimize();
    void toggleMaximize();
    bool isMaximized() const { return maximized_; }
    void requestClose();

    void setCursorCaptured(bool captured);
    bool cursorCaptured() const { return cursor_captured_; }
    // Cursor confinado (visible, sin salir de la ventana: XGrabPointer con
    // confine_to). Sin foco se suelta y al volver se encierra otra vez.
    void setCursorConfined(bool confined);
    bool cursorConfined() const { return cursor_confined_; }
    // Ocultar el cursor sobre la ventana (sin capturarlo).
    void setCursorVisible(bool visible);

    // --- Mandos (evdev: /dev/input/event*, hasta 4) ---
    // Se leen en pumpEvents sin bloquear y llegan como eventos Gamepad*
    // (con gamepadIndex). Los nuevos se ven al momento (inotify) o en ~1 s.
    // Sin foco no se leen y se sueltan los botones, salvo con
    // setGamepadsInBackground(true). Hace falta leer /dev/input/event* (udev
    // lo da al usuario de la sesion; si no, el grupo "input").
    void setGamepadsInBackground(bool enabled) { gamepads_background_ = enabled; }
    // Vibracion por force feedback (FF_RUMBLE); ver dm::setGamepadVibration.
    bool setGamepadVibration(int pad, float low, float high, float seconds = 0.0f);

private:
    void dispatch(Event& event);
    KeyMods currentMods(unsigned int state) const;
    void pollGamepads();
    static bool vibrationThunk(void* user, int pad, float low, float high, float seconds);
    void applyPointer();  // cursor oculto y agarre del puntero segun el estado

    std::unique_ptr<EvdevGamepads> gamepads_;
    bool gamepads_background_ = false;

    EventCallback callback_;
    NativeHandle native_{};
    unsigned long wm_delete_ = 0;   // Atom WM_DELETE_WINDOW
    void* input_method_ = nullptr;  // XIM
    void* input_context_ = nullptr; // XIC
    unsigned long blank_cursor_ = 0;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    bool open_ = false;
    bool focused_ = true;
    bool maximized_ = false;
    bool cursor_captured_ = false;
    bool cursor_confined_ = false;
    bool cursor_visible_ = true;
    float last_x_ = 0.0f;
    float last_y_ = 0.0f;
    bool have_last_ = false;
    bool warping_ = false;
};

}  // namespace cramion::dm

#else

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>
#include <functional>
#include <memory>
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
    // false: no se muestra ni quita el foco (herramientas de prueba del
    // render: la swapchain existe igual).
    bool visible = true;
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
    Window();  // en el .cpp: los mandos son un tipo incompleto aqui
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
    // La ventana tiene el foco (como en Android y Linux).
    bool focused() const { return hwnd_ != nullptr && GetForegroundWindow() == hwnd_; }
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
    // Al soltarlo, el cursor vuelve a donde estaba al capturarlo (camara orbital con
    // el clic derecho: el puntero no salta al centro de la ventana).
    void setCursorCaptured(bool captured, const RECT* client_region = nullptr);
    bool cursorCaptured() const { return cursor_captured_; }

    // --- Cursor confinado (Confined de Unity: estrategia, MOBA...) ---
    // El cursor sigue visible y se mueve normal, pero no sale de la ventana
    // (o de `client_region`, en pixeles del area cliente: la vista Juego del
    // editor). Se vuelve a calcular al mover o redimensionar la ventana; sin
    // foco se suelta y al recuperarlo se encierra otra vez. La captura manda
    // si estan las dos. Llamarlo cada frame con el mismo rectangulo no hace nada.
    void setCursorConfined(bool confined, const RECT* client_region = nullptr);
    bool cursorConfined() const { return cursor_confined_; }
    // Oculta el cursor mientras la ventana tiene el foco (Cursor.visible =
    // false de Unity). La captura lo oculta siempre.
    void setCursorVisible(bool visible);
    bool cursorVisible() const { return cursor_visible_; }

    // --- Mandos (hasta 4) ---
    // XInput (xinput1_4.dll cargada al vuelo: sin ella no hay mandos XInput,
    // pero la ventana funciona) y DualShock 4 / DualSense por HID (raw
    // input; CRAMION_GAMEPAD_HID=0 lo apaga). Se leen en pumpEvents sin
    // bloquear y llegan como eventos Gamepad* (con gamepadIndex); un hueco
    // XInput vacio se vuelve a mirar cada ~1 s (uno por frame) o al momento
    // si Windows avisa de un dispositivo nuevo. Sin foco no se leen, se
    // sueltan los botones y se para la vibracion, salvo con
    // setGamepadsInBackground(true). Las ventanas sin mostrar no los leen.
    void setGamepadsInBackground(bool enabled) { gamepads_background_ = enabled; }
    // Vibracion de un mando (-1 = todos); ver dm::setGamepadVibration.
    bool setGamepadVibration(int pad, float low, float high, float seconds = 0.0f);

private:
    // El rectangulo (en pantalla) en el que se encierra el cursor confinado.
    bool confineScreenRect(RECT& out) const;
    struct Gamepads;
    std::unique_ptr<Gamepads> gamepads_;
    bool gamepads_background_ = false;
    bool has_focus_ = false;
    void createGamepads();
    void destroyGamepads();
    void pollGamepads();
    void refreshRumble();
    void handleGamepadInput(const RAWINPUT& raw);
    void removeGamepadDevice(HANDLE device);
    void applyRumble(int slot);
    static bool vibrationThunk(void* user, int pad, float low, float high, float seconds);

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
    void updateCursorShown();  // ShowCursor segun captura, visible y foco
    bool cursor_captured_ = false;
    bool cursor_confined_ = false;
    bool cursor_visible_ = true;
    bool has_confine_rect_ = false;
    RECT confine_rect_{};
    bool cursor_hidden_ = false;
    bool raw_input_registered_ = false;
    bool has_capture_rect_ = false;
    RECT capture_rect_{};
    POINT cursor_before_capture_{};  // donde estaba el cursor al capturarlo
    bool has_cursor_before_capture_ = false;
};

}  // namespace cramion::dm

#endif  // __ANDROID__ / __linux__ / Windows
