#include "CramionDM/Window.h"

#include <dbt.h>       // DBT_DEVNODES_CHANGED (mando enchufado o quitado)
#include <shellapi.h>  // DragAcceptFiles / DragQueryFile (drag & drop)
#include <windowsx.h>  // GET_X_LPARAM / GET_Y_LPARAM
#include <xinput.h>    // solo los tipos: las funciones se cargan de la DLL

#include <algorithm>
#include <array>
#include <vector>

#include "CramionDM/Input.h"
#include "GamepadCommon.h"

namespace cramion::dm {

namespace {

constexpr wchar_t kWindowClassName[] = L"CramionDMWindowClass";

// Traduce el wParam/lParam de un mensaje de teclado a un Key de CramionDM,
// resolviendo la variante izquierda/derecha de Shift/Control/Alt.
Key translateKey(WPARAM wParam, LPARAM lParam) {
    const UINT scancode = (lParam >> 16) & 0xFF;
    const bool extended = (lParam & (1 << 24)) != 0;

    switch (wParam) {
        case VK_SHIFT:
            // MapVirtualKey distingue LShift (0x2A) de RShift.
            return (MapVirtualKey(scancode, MAPVK_VSC_TO_VK_EX) == VK_RSHIFT)
                       ? Key::RightShift
                       : Key::LeftShift;
        case VK_CONTROL:
            return extended ? Key::RightControl : Key::LeftControl;
        case VK_MENU:
            return extended ? Key::RightAlt : Key::LeftAlt;
        default:
            return static_cast<Key>(wParam);
    }
}

}  // namespace

Window::Window() = default;

Window::~Window() {
    destroy();
}

KeyMods Window::currentMods() {
    KeyMods mods = KeyMods::None;
    if (GetKeyState(VK_SHIFT) & 0x8000) mods |= KeyMods::Shift;
    if (GetKeyState(VK_CONTROL) & 0x8000) mods |= KeyMods::Control;
    if (GetKeyState(VK_MENU) & 0x8000) mods |= KeyMods::Alt;
    if ((GetKeyState(VK_LWIN) & 0x8000) || (GetKeyState(VK_RWIN) & 0x8000)) mods |= KeyMods::Super;
    if (GetKeyState(VK_CAPITAL) & 0x0001) mods |= KeyMods::CapsLock;
    if (GetKeyState(VK_NUMLOCK) & 0x0001) mods |= KeyMods::NumLock;
    return mods;
}

bool Window::create(const WindowConfig& config) {
    hinstance_ = GetModuleHandleW(nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wc.lpfnWndProc = &Window::wndProcThunk;
    wc.hInstance = hinstance_;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    // Icono del ejecutable (recurso 1, assets/cramion.rc); sin el, el de Windows.
    wc.hIcon = static_cast<HICON>(LoadImageW(hinstance_, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                             GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0));
    wc.hIconSm = static_cast<HICON>(LoadImageW(hinstance_, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                               GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
    wc.hbrBackground = nullptr;  // No borrar el fondo: lo pinta el renderizador.
    wc.lpszClassName = kWindowClassName;

    // Registrar la clase una sola vez (ignora "ya registrada").
    if (!RegisterClassExW(&wc)) {
        const DWORD err = GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS) {
            return false;
        }
    }

    DWORD style = WS_OVERLAPPEDWINDOW;
    if (!config.resizable) {
        style &= ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
    }

    // Ajustar el tamaño para que el área cliente sea width x height.
    RECT rect{0, 0, static_cast<LONG>(config.width), static_cast<LONG>(config.height)};
    AdjustWindowRect(&rect, style, FALSE);

    hwnd_ = CreateWindowExW(0, kWindowClassName, config.title.c_str(), style, CW_USEDEFAULT,
                            CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top, nullptr,
                            nullptr, hinstance_, this);
    if (!hwnd_) {
        return false;
    }

    width_ = config.width;
    height_ = config.height;
    open_ = true;
    has_focus_ = GetFocus() == hwnd_;

    // Aceptar archivos arrastrados desde el explorador -> genera WM_DROPFILES.
    DragAcceptFiles(hwnd_, TRUE);

    // Sin barra de Windows: WM_NCCALCSIZE deja el area cliente en toda la
    // ventana (se aplica con SWP_FRAMECHANGED).
    custom_title_bar_ = config.custom_title_bar;
    if (custom_title_bar_) {
        SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                     SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    if (!config.visible) {
        // Sin mostrarla no llega WM_SIZE: el tamano, del area cliente.
        RECT client{};
        GetClientRect(hwnd_, &client);
        width_ = static_cast<uint32_t>(client.right - client.left);
        height_ = static_cast<uint32_t>(client.bottom - client.top);
        return true;
    }
    // Maximizada: WM_SIZE (dentro de ShowWindow) ya deja width_/height_ con el
    // tamano real antes de que nadie cree la swapchain.
    ShowWindow(hwnd_, config.maximized && config.resizable ? SW_SHOWMAXIMIZED : SW_SHOW);
    UpdateWindow(hwnd_);
    has_focus_ = GetFocus() == hwnd_;
    createGamepads();
    return true;
}

void Window::destroy() {
    destroyGamepads();
    if (hwnd_) {
        if (cursor_captured_) setCursorCaptured(false);
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    open_ = false;
}

void Window::setTitle(const std::wstring& title) {
    if (hwnd_) {
        SetWindowTextW(hwnd_, title.c_str());
    }
}

void Window::minimize() {
    if (hwnd_) ShowWindow(hwnd_, SW_MINIMIZE);
}

void Window::toggleMaximize() {
    if (hwnd_) ShowWindow(hwnd_, IsZoomed(hwnd_) ? SW_RESTORE : SW_MAXIMIZE);
}

bool Window::isMaximized() const {
    return hwnd_ != nullptr && IsZoomed(hwnd_) != FALSE;
}

void Window::requestClose() {
    if (hwnd_) PostMessageW(hwnd_, WM_CLOSE, 0, 0);
}

void Window::showSystemMenu(int screen_x, int screen_y) {
    if (!hwnd_) return;
    HMENU menu = GetSystemMenu(hwnd_, FALSE);
    if (!menu) return;
    const bool maximized = isMaximized();
    EnableMenuItem(menu, SC_RESTORE, MF_BYCOMMAND | (maximized ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(menu, SC_MAXIMIZE, MF_BYCOMMAND | (maximized ? MF_GRAYED : MF_ENABLED));
    EnableMenuItem(menu, SC_MOVE, MF_BYCOMMAND | (maximized ? MF_GRAYED : MF_ENABLED));
    EnableMenuItem(menu, SC_SIZE, MF_BYCOMMAND | (maximized ? MF_GRAYED : MF_ENABLED));
    const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen_x, screen_y, 0, hwnd_, nullptr);
    if (command != 0) PostMessageW(hwnd_, WM_SYSCOMMAND, command, 0);
}

// Bordes para redimensionar (la ventana no tiene marco visible) y la zona de
// arrastre de la barra que dibuja la aplicacion.
LRESULT Window::hitTest(LPARAM lParam) const {
    POINT point{static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam))};
    ScreenToClient(hwnd_, &point);
    RECT client{};
    GetClientRect(hwnd_, &client);
    if (!IsZoomed(hwnd_)) {
        const UINT dpi = GetDpiForWindow(hwnd_);
        const int border = MulDiv(GetSystemMetrics(SM_CXFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER), dpi > 0 ? static_cast<int>(dpi) : 96, 96);
        const bool left = point.x < border;
        const bool right = point.x >= client.right - border;
        const bool top = point.y < border;
        const bool bottom = point.y >= client.bottom - border;
        if (top && left) return HTTOPLEFT;
        if (top && right) return HTTOPRIGHT;
        if (bottom && left) return HTBOTTOMLEFT;
        if (bottom && right) return HTBOTTOMRIGHT;
        if (left) return HTLEFT;
        if (right) return HTRIGHT;
        if (top) return HTTOP;
        if (bottom) return HTBOTTOM;
    }
    if (caption_test_ && caption_test_(point.x, point.y)) return HTCAPTION;
    return HTCLIENT;
}

void Window::pumpEvents() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    // Despues de los mensajes (los informes HID ya llegaron por WM_INPUT).
    pollGamepads();
}

void Window::dispatch(Event& event) {
    if (callback_) {
        callback_(event);
    }
}

// -----------------------------------------------------------------------------
// Cursor capturado
// -----------------------------------------------------------------------------

void Window::setCursorCaptured(bool captured, const RECT* client_region) {
    if (hwnd_ == nullptr) return;
    has_capture_rect_ = client_region != nullptr;
    if (client_region != nullptr) capture_rect_ = *client_region;
    if (captured && !raw_input_registered_) {
        RAWINPUTDEVICE device{};
        device.usUsagePage = 0x01;  // escritorio generico
        device.usUsage = 0x02;      // raton
        device.dwFlags = 0;
        device.hwndTarget = hwnd_;
        raw_input_registered_ = RegisterRawInputDevices(&device, 1, sizeof(device)) != FALSE;
    }
    const bool was_captured = cursor_captured_;
    cursor_captured_ = captured;
    if (captured) {
        // Recuerda donde estaba el cursor para devolverlo ahi al soltarlo.
        if (!was_captured) has_cursor_before_capture_ = GetCursorPos(&cursor_before_capture_) != FALSE;
        applyCursorClip();
        if (!cursor_hidden_) {
            while (ShowCursor(FALSE) >= 0) {
            }
            cursor_hidden_ = true;
        }
    } else {
        ClipCursor(nullptr);
        if (was_captured && has_cursor_before_capture_) SetCursorPos(cursor_before_capture_.x, cursor_before_capture_.y);
        has_cursor_before_capture_ = false;
        if (cursor_hidden_) {
            while (ShowCursor(TRUE) < 0) {
            }
            cursor_hidden_ = false;
        }
    }
}

void Window::applyCursorClip() {
    RECT client{};
    GetClientRect(hwnd_, &client);
    RECT area = has_capture_rect_ ? capture_rect_ : client;
    POINT corners[2] = {{area.left, area.top}, {area.right, area.bottom}};
    ClientToScreen(hwnd_, &corners[0]);
    ClientToScreen(hwnd_, &corners[1]);
    // Un rectangulo pequeno en el centro: el cursor oculto no puede salir a
    // otra ventana ni pulsar fuera (el movimiento real llega por raw input).
    const LONG cx = (corners[0].x + corners[1].x) / 2;
    const LONG cy = (corners[0].y + corners[1].y) / 2;
    const RECT clip{cx - 2, cy - 2, cx + 2, cy + 2};
    SetCursorPos(cx, cy);
    ClipCursor(&clip);
}

// -----------------------------------------------------------------------------
// Mandos: XInput y DualShock 4 / DualSense por HID (raw input)
// -----------------------------------------------------------------------------

namespace {

using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
using XInputSetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_VIBRATION*);

constexpr USHORT kSonyVendor = 0x054C;

// Mandos de Sony que se leen por HID: 0 si no es uno de ellos, 4 (DualShock 4,
// tambien su receptor USB) o 5 (DualSense y DualSense Edge).
int sonyModel(DWORD vendor, DWORD product) {
    if (vendor != kSonyVendor) return 0;
    switch (product) {
        case 0x05C4:
        case 0x09CC:
        case 0x0BA0: return 4;
        case 0x0CE6:
        case 0x0DF2: return 5;
        default: return 0;
    }
}

float stickByte(BYTE v) { return std::clamp((static_cast<float>(v) - 128.0f) / 127.0f, -1.0f, 1.0f); }

// Botones de Sony (el mismo orden en DS4 y DualSense): cruceta como hat en el
// nibble bajo (0 = arriba, en el sentido del reloj, 8 = suelta) y los de la
// cara en el alto; luego L1, R1, L2, R2, Share/Create, Options, L3, R3.
void sonyButtons(BYTE face, BYTE shoulders, gamepad::State& out) {
    const int hat = face & 0x0F;
    out.set(GamepadButton::DpadUp, hat == 0 || hat == 1 || hat == 7);
    out.set(GamepadButton::DpadRight, hat == 1 || hat == 2 || hat == 3);
    out.set(GamepadButton::DpadDown, hat == 3 || hat == 4 || hat == 5);
    out.set(GamepadButton::DpadLeft, hat == 5 || hat == 6 || hat == 7);
    out.set(GamepadButton::X, (face & 0x10) != 0);  // cuadrado
    out.set(GamepadButton::A, (face & 0x20) != 0);  // cruz
    out.set(GamepadButton::B, (face & 0x40) != 0);  // circulo
    out.set(GamepadButton::Y, (face & 0x80) != 0);  // triangulo
    out.set(GamepadButton::LeftShoulder, (shoulders & 0x01) != 0);
    out.set(GamepadButton::RightShoulder, (shoulders & 0x02) != 0);
    out.set(GamepadButton::Back, (shoulders & 0x10) != 0);
    out.set(GamepadButton::Start, (shoulders & 0x20) != 0);
    out.set(GamepadButton::LeftStick, (shoulders & 0x40) != 0);
    out.set(GamepadButton::RightStick, (shoulders & 0x80) != 0);
}

// Informe de entrada de un DS4 o DualSense (con su id en el primer byte).
// Dos disposiciones a partir de `base` (el byte del stick izquierdo X):
//  A (DS4 USB 0x01 y BT 0x11; DualSense BT simple 0x01): sticks, botones x3, L2, R2.
//  B (DualSense USB 0x01 y BT 0x31): sticks, L2, R2, contador, botones x3.
// `bluetooth` sale del informe (por USB son de 64 bytes; el simple de BT, 10).
bool parseSonyReport(const BYTE* d, DWORD size, int model, gamepad::State& out, bool& bluetooth) {
    if (size < 10) return false;
    const BYTE id = d[0];
    bool layout_b = false;
    DWORD base = 1;
    if (id == 0x01) {
        bluetooth = size < 64;
        layout_b = model == 5 && !bluetooth;
    } else if (id == 0x11 && model == 4) {
        bluetooth = true;
        base = 3;
    } else if (id == 0x31 && model == 5) {
        bluetooth = true;
        layout_b = true;
        base = 2;
    } else {
        return false;  // otros informes (sensores, audio...)
    }
    if (base + 10 > size) return false;
    const BYTE* p = d + base;
    out.set(GamepadAxis::LeftX, stickByte(p[0]));
    out.set(GamepadAxis::LeftY, -stickByte(p[1]));  // 0 = arriba
    out.set(GamepadAxis::RightX, stickByte(p[2]));
    out.set(GamepadAxis::RightY, -stickByte(p[3]));
    if (layout_b) {
        out.set(GamepadAxis::LeftTrigger, p[4] / 255.0f);
        out.set(GamepadAxis::RightTrigger, p[5] / 255.0f);
        sonyButtons(p[7], p[8], out);
    } else {
        sonyButtons(p[4], p[5], out);
        out.set(GamepadAxis::LeftTrigger, p[7] / 255.0f);
        out.set(GamepadAxis::RightTrigger, p[8] / 255.0f);
    }
    return true;
}

// CRC-32 de los informes de salida por Bluetooth (semilla 0xA2: la cabecera
// de transaccion HID de un informe de salida). Sin tabla: 78 bytes de vez en cuando.
uint32_t sonyCrc32(const BYTE* data, size_t size) {
    uint32_t crc = 0xFFFFFFFFu;
    const auto feed = [&crc](BYTE b) {
        crc ^= b;
        for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    };
    feed(0xA2);
    for (size_t i = 0; i < size; ++i) feed(data[i]);
    return ~crc;
}

}  // namespace

struct Window::Gamepads {
    // XInput (cargado al vuelo: no depende de la DLL para arrancar).
    HMODULE xinput = nullptr;
    XInputGetStateFn get_state = nullptr;
    XInputSetStateFn set_state = nullptr;
    struct XPad {
        int slot = -1;  // -1 = desconectado
        DWORD packet = 0;
        bool fresh = true;  // mandar el estado aunque el paquete no cambie
        double next_probe = 0.0;
    };
    std::array<XPad, XUSER_MAX_COUNT> xpads{};
    bool probe_all = true;  // al empezar y cuando Windows avisa de un dispositivo

    // DS4 / DualSense por raw input. La vibracion abre el dispositivo (solo
    // la primera vez que se pide) y escribe sin esperar (OVERLAPPED).
    struct Sony {
        HANDLE device = nullptr;  // el de raw input
        int slot = -1;
        int model = 4;
        bool bluetooth = false;
        HANDLE file = INVALID_HANDLE_VALUE;
        HANDLE done = nullptr;  // evento del OVERLAPPED
        OVERLAPPED overlapped{};
        std::array<BYTE, 78> report{};
        bool writing = false;
        bool pending = false;  // otra vibracion esperando a que acabe la escritura
        bool rumble_failed = false;
    };
    // Por puntero: el OVERLAPPED y el informe no se pueden mover mientras escribe.
    std::vector<std::unique_ptr<Sony>> sony;
    std::vector<HANDLE> ignored;  // HID que no son de Sony (ya preguntados)
    bool hid = false;             // raw input de mandos registrado
    std::vector<BYTE> raw;        // WM_INPUT de HID (crece una vez)
    gamepad::State hid_state;

    gamepad::Slots slots;
    std::array<gamepad::Rumble, gamepad::kSlots> rumble{};
    bool released = false;  // sin foco: ya se solto todo

    Sony* sonyFor(HANDLE device) {
        for (const auto& s : sony) {
            if (s->device == device) return s.get();
        }
        return nullptr;
    }

    static void closeSony(Sony& s) {
        if (s.file != INVALID_HANDLE_VALUE) {
            if (s.writing) {
                // Una escritura en curso tiene que acabar antes de soltar el buffer.
                DWORD written = 0;
                CancelIoEx(s.file, &s.overlapped);
                GetOverlappedResult(s.file, &s.overlapped, &written, TRUE);
            }
            CloseHandle(s.file);
        }
        if (s.done != nullptr) CloseHandle(s.done);
        s.file = INVALID_HANDLE_VALUE;
        s.done = nullptr;
        s.writing = false;
    }

    // Escribe el informe de vibracion de un DS4/DualSense. Si hay otra
    // escritura en curso, queda pendiente y la manda pollGamepads al acabar.
    void writeSony(Sony& s, float low, float high) {
        if (s.rumble_failed) return;
        if (s.writing) {
            DWORD written = 0;
            if (!GetOverlappedResult(s.file, &s.overlapped, &written, FALSE)) {
                if (GetLastError() == ERROR_IO_INCOMPLETE) {
                    s.pending = true;
                    return;
                }
            }
            s.writing = false;
        }
        if (s.file == INVALID_HANDLE_VALUE) {
            UINT chars = 0;
            GetRawInputDeviceInfoW(s.device, RIDI_DEVICENAME, nullptr, &chars);
            std::wstring name(chars, L'\0');
            if (chars == 0 || GetRawInputDeviceInfoW(s.device, RIDI_DEVICENAME, name.data(), &chars) == static_cast<UINT>(-1)) {
                s.rumble_failed = true;
                return;
            }
            s.file = CreateFileW(name.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                 FILE_FLAG_OVERLAPPED, nullptr);
            s.done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (s.file == INVALID_HANDLE_VALUE || s.done == nullptr) {
                // Otro programa lo tiene en exclusiva (DS4Windows, Steam...).
                closeSony(s);
                s.rumble_failed = true;
                return;
            }
        }
        // Solo el bit de vibracion: la barra de luz y lo demas, como estaban.
        s.report.fill(0);
        BYTE* r = s.report.data();
        DWORD size = 0;
        if (s.model == 4 && !s.bluetooth) {
            r[0] = 0x05;
            r[1] = 0x01;
            r[4] = gamepad::motor8(high);  // motor pequeno (derecho)
            r[5] = gamepad::motor8(low);   // motor grande (izquierdo)
            size = 32;
        } else if (s.model == 4) {
            r[0] = 0x11;
            r[1] = 0xC0 | 0x04;  // HID + CRC, informes de entrada cada 4 ms
            r[3] = 0x01;
            r[6] = gamepad::motor8(high);
            r[7] = gamepad::motor8(low);
            size = 78;
        } else if (!s.bluetooth) {
            r[0] = 0x02;
            r[1] = 0x03;  // vibracion "compatible" (motores clasicos, no hapticos)
            r[3] = gamepad::motor8(high);
            r[4] = gamepad::motor8(low);
            size = 48;
        } else {
            r[0] = 0x31;
            r[1] = 0x02;
            r[2] = 0x03;
            r[4] = gamepad::motor8(high);
            r[5] = gamepad::motor8(low);
            size = 78;
        }
        if (s.bluetooth) {
            const uint32_t crc = sonyCrc32(r, 74);
            r[74] = static_cast<BYTE>(crc);
            r[75] = static_cast<BYTE>(crc >> 8);
            r[76] = static_cast<BYTE>(crc >> 16);
            r[77] = static_cast<BYTE>(crc >> 24);
        }
        s.overlapped = OVERLAPPED{};
        s.overlapped.hEvent = s.done;
        ResetEvent(s.done);
        s.pending = false;
        if (WriteFile(s.file, r, size, nullptr, &s.overlapped) || GetLastError() == ERROR_IO_PENDING) {
            s.writing = true;
        } else {
            s.rumble_failed = true;
        }
    }
};

void Window::createGamepads() {
    gamepads_ = std::make_unique<Gamepads>();
    Gamepads& g = *gamepads_;
    // xinput1_4 (Windows 8+); la 9.1.0 de Windows 7 no vibra con algunos
    // mandos pero lee igual. Solo de System32 (sin DLL plantadas al lado).
    for (const wchar_t* dll : {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"}) {
        g.xinput = LoadLibraryExW(dll, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (g.xinput != nullptr) break;
    }
    if (g.xinput != nullptr) {
        g.get_state = reinterpret_cast<XInputGetStateFn>(reinterpret_cast<void*>(GetProcAddress(g.xinput, "XInputGetState")));
        g.set_state = reinterpret_cast<XInputSetStateFn>(reinterpret_cast<void*>(GetProcAddress(g.xinput, "XInputSetState")));
    }
    // DS4 / DualSense: raw input de mandos (uso 5) y joysticks (4), con aviso
    // de alta y baja (WM_INPUT_DEVICE_CHANGE).
    char hid_env[8] = {};
    const bool hid_off = GetEnvironmentVariableA("CRAMION_GAMEPAD_HID", hid_env, sizeof(hid_env)) > 0 && hid_env[0] == '0';
    if (!hid_off) {
        RAWINPUTDEVICE devices[2]{};
        for (int i = 0; i < 2; ++i) {
            devices[i].usUsagePage = 0x01;
            devices[i].usUsage = i == 0 ? 0x05 : 0x04;
            devices[i].dwFlags = RIDEV_DEVNOTIFY;
            devices[i].hwndTarget = hwnd_;
        }
        g.hid = RegisterRawInputDevices(devices, 2, sizeof(RAWINPUTDEVICE)) != FALSE;
    }
    // La primera ventana con mandos atiende dm::setGamepadVibration.
    if (gamepadVibrationUser() == nullptr) setGamepadVibrationHandler(&Window::vibrationThunk, this);
}

void Window::destroyGamepads() {
    if (!gamepads_) return;
    Gamepads& g = *gamepads_;
    // Ningun mando se queda vibrando al cerrar.
    for (int i = 0; i < gamepad::kSlots; ++i) {
        if (g.rumble[static_cast<size_t>(i)].low > 0.0f || g.rumble[static_cast<size_t>(i)].high > 0.0f) {
            g.rumble[static_cast<size_t>(i)] = {};
            applyRumble(i);
        }
    }
    for (const auto& s : g.sony) Gamepads::closeSony(*s);
    if (g.hid) {
        RAWINPUTDEVICE devices[2]{};
        for (int i = 0; i < 2; ++i) {
            devices[i].usUsagePage = 0x01;
            devices[i].usUsage = i == 0 ? 0x05 : 0x04;
            devices[i].dwFlags = RIDEV_REMOVE;
        }
        RegisterRawInputDevices(devices, 2, sizeof(RAWINPUTDEVICE));
    }
    if (g.xinput != nullptr) FreeLibrary(g.xinput);
    if (gamepadVibrationUser() == this) setGamepadVibrationHandler(nullptr, nullptr);
    gamepads_.reset();
}

bool Window::vibrationThunk(void* user, int pad, float low, float high, float seconds) {
    return static_cast<Window*>(user)->setGamepadVibration(pad, low, high, seconds);
}

bool Window::setGamepadVibration(int pad, float low, float high, float seconds) {
    if (!gamepads_) return false;
    Gamepads& g = *gamepads_;
    const double until = seconds > 0.0f ? gamepad::nowSeconds() + seconds : 0.0;
    bool any = false;
    for (int i = 0; i < gamepad::kSlots; ++i) {
        if ((pad >= 0 && pad != i) || !g.slots.used(i)) continue;
        g.rumble[static_cast<size_t>(i)] = {std::clamp(low, 0.0f, 1.0f), std::clamp(high, 0.0f, 1.0f), until};
        applyRumble(i);
        any = true;
    }
    return any;
}

// Manda al mando de ese hueco su vibracion (sin foco, parada).
void Window::applyRumble(int slot) {
    Gamepads& g = *gamepads_;
    const gamepad::Rumble& r = g.rumble[static_cast<size_t>(slot)];
    const bool active = has_focus_ || gamepads_background_;
    const float low = active ? r.low : 0.0f;
    const float high = active ? r.high : 0.0f;
    for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
        if (g.xpads[i].slot != slot) continue;
        if (g.set_state != nullptr) {
            XINPUT_VIBRATION v{};
            v.wLeftMotorSpeed = gamepad::motor16(low);
            v.wRightMotorSpeed = gamepad::motor16(high);
            g.set_state(i, &v);
        }
        return;
    }
    for (const auto& s : g.sony) {
        if (s->slot == slot) {
            g.writeSony(*s, low, high);
            return;
        }
    }
}

void Window::pollGamepads() {
    if (!gamepads_) return;
    Gamepads& g = *gamepads_;
    const auto emit = [this](Event& e) { dispatch(e); };
    const double now = gamepad::nowSeconds();

    // Vibraciones con tiempo que ya acabaron; escrituras HID que esperaban.
    for (int i = 0; i < gamepad::kSlots; ++i) {
        gamepad::Rumble& r = g.rumble[static_cast<size_t>(i)];
        if (r.until > 0.0 && now >= r.until) {
            r = {};
            applyRumble(i);
        }
    }
    for (const auto& s : g.sony) {
        if (s->pending && s->slot >= 0) applyRumble(s->slot);
    }

    if (!has_focus_ && !gamepads_background_) {
        // Sin foco: una vez, todo suelto. Al volver se manda el estado real.
        if (!g.released) {
            g.slots.releaseAll(emit);
            for (Gamepads::XPad& x : g.xpads) x.fresh = true;
            g.released = true;
        }
        return;
    }
    g.released = false;

    if (g.get_state != nullptr) {
        // Los conectados, cada frame (barato). De los vacios, como mucho uno
        // por frame y cada ~1 s: XInputGetState de un hueco vacio es lento.
        bool probed = false;
        for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
            Gamepads::XPad& x = g.xpads[i];
            if (x.slot < 0) {
                if (!g.probe_all && (probed || now < x.next_probe)) continue;
                probed = true;
            }
            XINPUT_STATE state{};
            if (g.get_state(i, &state) != ERROR_SUCCESS) {
                if (x.slot >= 0) {
                    g.rumble[static_cast<size_t>(x.slot)] = {};
                    g.slots.disconnect(x.slot, emit);
                    x.slot = -1;
                }
                x.next_probe = now + gamepad::kProbeSeconds;
                continue;
            }
            if (x.slot < 0) {
                x.slot = g.slots.connect(GamepadKind::Xbox, emit);
                if (x.slot < 0) {  // ya hay 4 mandos
                    x.next_probe = now + gamepad::kProbeSeconds;
                    continue;
                }
                x.fresh = true;
            }
            if (!x.fresh && state.dwPacketNumber == x.packet) continue;
            x.packet = state.dwPacketNumber;
            x.fresh = false;
            const XINPUT_GAMEPAD& p = state.Gamepad;
            const auto stick = [](SHORT v) { return std::max(-1.0f, static_cast<float>(v) / 32767.0f); };
            gamepad::State s;
            s.set(GamepadAxis::LeftX, stick(p.sThumbLX));
            s.set(GamepadAxis::LeftY, stick(p.sThumbLY));  // XInput ya tiene la Y hacia arriba
            s.set(GamepadAxis::RightX, stick(p.sThumbRX));
            s.set(GamepadAxis::RightY, stick(p.sThumbRY));
            s.set(GamepadAxis::LeftTrigger, p.bLeftTrigger / 255.0f);
            s.set(GamepadAxis::RightTrigger, p.bRightTrigger / 255.0f);
            const WORD b = p.wButtons;
            s.set(GamepadButton::A, (b & XINPUT_GAMEPAD_A) != 0);
            s.set(GamepadButton::B, (b & XINPUT_GAMEPAD_B) != 0);
            s.set(GamepadButton::X, (b & XINPUT_GAMEPAD_X) != 0);
            s.set(GamepadButton::Y, (b & XINPUT_GAMEPAD_Y) != 0);
            s.set(GamepadButton::LeftShoulder, (b & XINPUT_GAMEPAD_LEFT_SHOULDER) != 0);
            s.set(GamepadButton::RightShoulder, (b & XINPUT_GAMEPAD_RIGHT_SHOULDER) != 0);
            s.set(GamepadButton::LeftStick, (b & XINPUT_GAMEPAD_LEFT_THUMB) != 0);
            s.set(GamepadButton::RightStick, (b & XINPUT_GAMEPAD_RIGHT_THUMB) != 0);
            s.set(GamepadButton::Start, (b & XINPUT_GAMEPAD_START) != 0);
            s.set(GamepadButton::Back, (b & XINPUT_GAMEPAD_BACK) != 0);
            s.set(GamepadButton::DpadUp, (b & XINPUT_GAMEPAD_DPAD_UP) != 0);
            s.set(GamepadButton::DpadDown, (b & XINPUT_GAMEPAD_DPAD_DOWN) != 0);
            s.set(GamepadButton::DpadLeft, (b & XINPUT_GAMEPAD_DPAD_LEFT) != 0);
            s.set(GamepadButton::DpadRight, (b & XINPUT_GAMEPAD_DPAD_RIGHT) != 0);
            g.slots.update(x.slot, s, emit);
        }
        g.probe_all = false;
    }
}

// Al cambiar el foco: sin el, ningun mando se queda vibrando (XInput no lo
// para solo); al volver, sigue la vibracion que estaba pedida.
void Window::refreshRumble() {
    if (!gamepads_) return;
    for (int i = 0; i < gamepad::kSlots; ++i) {
        const gamepad::Rumble& r = gamepads_->rumble[static_cast<size_t>(i)];
        if (r.low > 0.0f || r.high > 0.0f) applyRumble(i);
    }
}

// Un informe HID (WM_INPUT). El primero de un dispositivo dice si es un DS4 o
// un DualSense; los demas HID se apuntan para no volver a preguntar.
void Window::handleGamepadInput(const RAWINPUT& raw) {
    if (!gamepads_ || raw.header.dwType != RIM_TYPEHID) return;
    Gamepads& g = *gamepads_;
    if (!has_focus_ && !gamepads_background_) return;
    HANDLE device = raw.header.hDevice;
    Gamepads::Sony* s = g.sonyFor(device);
    if (s == nullptr) {
        if (std::find(g.ignored.begin(), g.ignored.end(), device) != g.ignored.end()) return;
        RID_DEVICE_INFO info{};
        info.cbSize = sizeof(info);
        UINT size = sizeof(info);
        const int model = GetRawInputDeviceInfoW(device, RIDI_DEVICEINFO, &info, &size) != static_cast<UINT>(-1) &&
                                  info.dwType == RIM_TYPEHID
                              ? sonyModel(info.hid.dwVendorId, info.hid.dwProductId)
                              : 0;
        if (model == 0) {
            g.ignored.push_back(device);
            return;
        }
        const int slot = g.slots.connect(GamepadKind::PlayStation, [this](Event& e) { dispatch(e); });
        if (slot < 0) return;  // ya hay 4: se vuelve a intentar con el siguiente informe
        auto added = std::make_unique<Gamepads::Sony>();
        added->device = device;
        added->slot = slot;
        added->model = model;
        s = added.get();
        g.sony.push_back(std::move(added));
    }
    const DWORD count = raw.data.hid.dwCount;
    const DWORD size = raw.data.hid.dwSizeHid;
    if (count == 0 || size == 0) return;
    // Varios informes juntos: vale el ultimo.
    const BYTE* last = raw.data.hid.bRawData + static_cast<size_t>(count - 1) * size;
    gamepad::State& state = g.hid_state;
    state = {};
    if (!parseSonyReport(last, size, s->model, state, s->bluetooth)) return;
    g.slots.update(s->slot, state, [this](Event& e) { dispatch(e); });
}

void Window::removeGamepadDevice(HANDLE device) {
    if (!gamepads_) return;
    Gamepads& g = *gamepads_;
    g.ignored.erase(std::remove(g.ignored.begin(), g.ignored.end(), device), g.ignored.end());
    for (size_t i = 0; i < g.sony.size(); ++i) {
        Gamepads::Sony& s = *g.sony[i];
        if (s.device != device) continue;
        Gamepads::closeSony(s);
        if (s.slot >= 0) {
            g.rumble[static_cast<size_t>(s.slot)] = {};
            g.slots.disconnect(s.slot, [this](Event& e) { dispatch(e); });
        }
        g.sony.erase(g.sony.begin() + static_cast<std::ptrdiff_t>(i));
        return;
    }
}

LRESULT CALLBACK Window::wndProcThunk(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        // Guardar el puntero de instancia pasado en CreateWindowEx.
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        auto* self = static_cast<Window*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    }

    auto* self = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self) {
        return self->handleMessage(msg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT Window::handleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    if (message_hook_ && message_hook_(hwnd_, msg, wParam, lParam)) {
        return 1;
    }

    // Barra de titulo propia: sin marco ni barra de Windows.
    if (custom_title_bar_) {
        if (msg == WM_NCCALCSIZE && wParam == TRUE) {
            // Maximizada, Windows la agranda un marco por cada lado: se
            // recorta para no salirse de la pantalla.
            if (IsZoomed(hwnd_)) {
                auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);
                const UINT dpi = GetDpiForWindow(hwnd_);
                const int d = dpi > 0 ? static_cast<int>(dpi) : 96;
                const int fx = MulDiv(GetSystemMetrics(SM_CXFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER), d, 96);
                const int fy = MulDiv(GetSystemMetrics(SM_CYFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER), d, 96);
                params->rgrc[0].left += fx;
                params->rgrc[0].right -= fx;
                params->rgrc[0].top += fy;
                params->rgrc[0].bottom -= fy;
            }
            return 0;
        }
        if (msg == WM_NCHITTEST) {
            return hitTest(lParam);
        }
        // Sin tema no clientes: que Windows no repinte la barra al activar.
        if (msg == WM_NCACTIVATE) {
            return DefWindowProcW(hwnd_, msg, wParam, -1);
        }
    }

    switch (msg) {
        // ---------------- Ventana ----------------
        case WM_CLOSE: {
            Event e;
            e.type = EventType::WindowClose;
            e.category = EventCategory::Window;
            dispatch(e);
            open_ = false;
            return 0;
        }
        case WM_DESTROY:
            open_ = false;
            return 0;

        case WM_SIZE: {
            // Estado de la ventana (minimizada / maximizada / restaurada).
            if (wParam == SIZE_MINIMIZED) {
                Event e;
                e.type = EventType::WindowMinimized;
                e.category = EventCategory::Window;
                dispatch(e);
                return 0;  // No emitir resize con tamaño 0x0.
            }
            if (wParam == SIZE_MAXIMIZED) {
                Event e;
                e.type = EventType::WindowMaximized;
                e.category = EventCategory::Window;
                dispatch(e);
            } else if (wParam == SIZE_RESTORED) {
                Event e;
                e.type = EventType::WindowRestored;
                e.category = EventCategory::Window;
                dispatch(e);
            }

            width_ = LOWORD(lParam);
            height_ = HIWORD(lParam);
            Event e;
            e.type = EventType::WindowResize;
            e.category = EventCategory::Window;
            e.width = width_;
            e.height = height_;
            dispatch(e);
            return 0;
        }
        case WM_MOVE: {
            Event e;
            e.type = EventType::WindowMoved;
            e.category = EventCategory::Window;
            e.x = static_cast<int32_t>(GET_X_LPARAM(lParam));
            e.y = static_cast<int32_t>(GET_Y_LPARAM(lParam));
            dispatch(e);
            return 0;
        }
        case WM_SETFOCUS: {
            if (cursor_captured_) applyCursorClip();
            has_focus_ = true;
            refreshRumble();
            Event e;
            e.type = EventType::WindowFocus;
            e.category = EventCategory::Window;
            e.focused = true;
            dispatch(e);
            return 0;
        }
        case WM_KILLFOCUS: {
            if (cursor_captured_) ClipCursor(nullptr);  // sin foco, el raton es del sistema
            has_focus_ = false;
            refreshRumble();
            Event e;
            e.type = EventType::WindowLostFocus;
            e.category = EventCategory::Window;
            e.focused = false;
            dispatch(e);
            return 0;
        }
        case WM_DPICHANGED: {
            // LOWORD(wParam) = nuevo DPI. Windows sugiere un nuevo rectángulo.
            const UINT newDpi = LOWORD(wParam);
            Event e;
            e.type = EventType::WindowDpiChanged;
            e.category = EventCategory::Window;
            e.dpiScale = static_cast<float>(newDpi) / 96.0f;
            dispatch(e);

            // Reposicionar/redimensionar según lo sugerido por el sistema.
            auto* suggested = reinterpret_cast<RECT*>(lParam);
            SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left, suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }

        // ---------------- Arrastrar y soltar archivos ----------------
        case WM_DROPFILES: {
            auto hDrop = reinterpret_cast<HDROP>(wParam);

            Event e;
            e.type = EventType::FileDropped;
            e.category = EventCategory::File;

            // Posición del cursor (coordenadas cliente) al soltar.
            POINT pt{};
            if (DragQueryPoint(hDrop, &pt)) {
                e.x = pt.x;
                e.y = pt.y;
                e.mouseX = static_cast<float>(pt.x);
                e.mouseY = static_cast<float>(pt.y);
            }

            // Recuperar cada ruta soltada.
            const UINT count = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
            e.paths.reserve(count);
            for (UINT i = 0; i < count; ++i) {
                const UINT len = DragQueryFileW(hDrop, i, nullptr, 0);
                std::wstring path(len, L'\0');
                DragQueryFileW(hDrop, i, path.data(), len + 1);
                e.paths.push_back(std::move(path));
            }

            DragFinish(hDrop);
            dispatch(e);
            return 0;
        }

        // ---------------- Teclado ----------------
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            Event e;
            e.type = EventType::KeyPressed;
            e.category = EventCategory::Keyboard | EventCategory::Input;
            e.key = translateKey(wParam, lParam);
            e.repeat = (lParam & (1 << 30)) != 0;  // Bit 30 = tecla ya estaba pulsada.
            e.mods = currentMods();
            dispatch(e);
            // Permitir que Alt+F4 y atajos del sistema sigan funcionando.
            if (msg == WM_SYSKEYDOWN) {
                return DefWindowProcW(hwnd_, msg, wParam, lParam);
            }
            return 0;
        }
        case WM_KEYUP:
        case WM_SYSKEYUP: {
            Event e;
            e.type = EventType::KeyReleased;
            e.category = EventCategory::Keyboard | EventCategory::Input;
            e.key = translateKey(wParam, lParam);
            e.mods = currentMods();
            dispatch(e);
            if (msg == WM_SYSKEYUP) {
                return DefWindowProcW(hwnd_, msg, wParam, lParam);
            }
            return 0;
        }
        case WM_CHAR: {
            // Carácter Unicode traducido (para entrada de texto).
            Event e;
            e.type = EventType::TextInput;
            e.category = EventCategory::Keyboard | EventCategory::Input;
            e.codepoint = static_cast<uint32_t>(wParam);
            dispatch(e);
            return 0;
        }

        // ---------------- Ratón: botones ----------------
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
        case WM_XBUTTONDOWN: {
            SetCapture(hwnd_);
            Event e;
            e.type = EventType::MouseButtonPressed;
            e.category = EventCategory::Mouse | EventCategory::Input;
            if (msg == WM_LBUTTONDOWN) e.button = MouseButton::Left;
            else if (msg == WM_RBUTTONDOWN) e.button = MouseButton::Right;
            else if (msg == WM_MBUTTONDOWN) e.button = MouseButton::Middle;
            else e.button = (GET_XBUTTON_WPARAM(wParam) == XBUTTON1) ? MouseButton::X1
                                                                     : MouseButton::X2;
            e.mouseX = static_cast<float>(GET_X_LPARAM(lParam));
            e.mouseY = static_cast<float>(GET_Y_LPARAM(lParam));
            dispatch(e);
            return (msg == WM_XBUTTONDOWN) ? TRUE : 0;
        }
        case WM_LBUTTONUP:
        case WM_RBUTTONUP:
        case WM_MBUTTONUP:
        case WM_XBUTTONUP: {
            ReleaseCapture();
            Event e;
            e.type = EventType::MouseButtonReleased;
            e.category = EventCategory::Mouse | EventCategory::Input;
            if (msg == WM_LBUTTONUP) e.button = MouseButton::Left;
            else if (msg == WM_RBUTTONUP) e.button = MouseButton::Right;
            else if (msg == WM_MBUTTONUP) e.button = MouseButton::Middle;
            else e.button = (GET_XBUTTON_WPARAM(wParam) == XBUTTON1) ? MouseButton::X1
                                                                     : MouseButton::X2;
            e.mouseX = static_cast<float>(GET_X_LPARAM(lParam));
            e.mouseY = static_cast<float>(GET_Y_LPARAM(lParam));
            dispatch(e);
            return (msg == WM_XBUTTONUP) ? TRUE : 0;
        }

        // ---------------- Ratón: movimiento en bruto (capturado) ----------------
        case WM_INPUT: {
            const auto handle = reinterpret_cast<HRAWINPUT>(lParam);
            UINT size = 0;
            GetRawInputData(handle, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER));
            RAWINPUT stack_raw{};
            RAWINPUT* raw = &stack_raw;
            if (size > sizeof(stack_raw)) {
                // Informes HID (mandos): mas grandes que un RAWINPUT de raton.
                if (!gamepads_) return DefWindowProcW(hwnd_, msg, wParam, lParam);
                if (gamepads_->raw.size() < size) gamepads_->raw.resize(size);
                raw = reinterpret_cast<RAWINPUT*>(gamepads_->raw.data());
            } else {
                size = sizeof(stack_raw);
            }
            if (GetRawInputData(handle, RID_INPUT, raw, &size, sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1)) {
                return DefWindowProcW(hwnd_, msg, wParam, lParam);
            }
            if (raw->header.dwType == RIM_TYPEHID) {
                handleGamepadInput(*raw);
            } else if (cursor_captured_ && GetFocus() == hwnd_ && raw->header.dwType == RIM_TYPEMOUSE &&
                       (raw->data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) == 0) {
                Event e;
                e.type = EventType::MouseRawMoved;
                e.category = EventCategory::Mouse | EventCategory::Input;
                e.deltaX = static_cast<float>(raw->data.mouse.lLastX);
                e.deltaY = static_cast<float>(raw->data.mouse.lLastY);
                if (e.deltaX != 0.0f || e.deltaY != 0.0f) dispatch(e);
            }
            return DefWindowProcW(hwnd_, msg, wParam, lParam);
        }

        // ---------------- Mandos: altas y bajas ----------------
        case WM_INPUT_DEVICE_CHANGE: {
            if (wParam == GIDC_REMOVAL) removeGamepadDevice(reinterpret_cast<HANDLE>(lParam));
            if (gamepads_) gamepads_->probe_all = true;  // quiza era un mando XInput
            return 0;
        }
        case WM_DEVICECHANGE: {
            if (wParam == DBT_DEVNODES_CHANGED && gamepads_) gamepads_->probe_all = true;
            return TRUE;
        }

        // ---------------- Ratón: movimiento ----------------
        case WM_MOUSEMOVE: {
            const float x = static_cast<float>(GET_X_LPARAM(lParam));
            const float y = static_cast<float>(GET_Y_LPARAM(lParam));

            if (!trackingMouse_) {
                // Registrarse para recibir WM_MOUSELEAVE.
                TRACKMOUSEEVENT tme{};
                tme.cbSize = sizeof(tme);
                tme.dwFlags = TME_LEAVE;
                tme.hwndTrack = hwnd_;
                TrackMouseEvent(&tme);
                trackingMouse_ = true;

                Event enter;
                enter.type = EventType::MouseEnter;
                enter.category = EventCategory::Mouse | EventCategory::Input;
                dispatch(enter);
            }

            Event e;
            e.type = EventType::MouseMoved;
            e.category = EventCategory::Mouse | EventCategory::Input;
            e.mouseX = x;
            e.mouseY = y;
            // Capturado: el movimiento llega por WM_INPUT (no se cuenta dos veces).
            if (haveLastMouse_ && !cursor_captured_) {
                e.deltaX = x - lastMouseX_;
                e.deltaY = y - lastMouseY_;
            }
            lastMouseX_ = x;
            lastMouseY_ = y;
            haveLastMouse_ = true;
            dispatch(e);
            return 0;
        }
        case WM_MOUSELEAVE: {
            trackingMouse_ = false;
            haveLastMouse_ = false;
            Event e;
            e.type = EventType::MouseLeave;
            e.category = EventCategory::Mouse | EventCategory::Input;
            dispatch(e);
            return 0;
        }

        // ---------------- Ratón: rueda ----------------
        case WM_MOUSEWHEEL: {
            Event e;
            e.type = EventType::MouseScrolled;
            e.category = EventCategory::Mouse | EventCategory::Input;
            e.scrollY = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / WHEEL_DELTA;
            dispatch(e);
            return 0;
        }
        case WM_MOUSEHWHEEL: {
            Event e;
            e.type = EventType::MouseScrolled;
            e.category = EventCategory::Mouse | EventCategory::Input;
            e.scrollX = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / WHEEL_DELTA;
            dispatch(e);
            return 0;
        }

        default:
            break;
    }
    return DefWindowProcW(hwnd_, msg, wParam, lParam);
}

}  // namespace cramion::dm
