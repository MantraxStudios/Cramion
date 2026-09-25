#include "CramionDM/Window.h"

#include <shellapi.h>  // DragAcceptFiles / DragQueryFile (drag & drop)
#include <windowsx.h>  // GET_X_LPARAM / GET_Y_LPARAM

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

    // Aceptar archivos arrastrados desde el explorador -> genera WM_DROPFILES.
    DragAcceptFiles(hwnd_, TRUE);

    // Sin barra de Windows: WM_NCCALCSIZE deja el area cliente en toda la
    // ventana (se aplica con SWP_FRAMECHANGED).
    custom_title_bar_ = config.custom_title_bar;
    if (custom_title_bar_) {
        SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                     SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    // Maximizada: WM_SIZE (dentro de ShowWindow) ya deja width_/height_ con el
    // tamano real antes de que nadie cree la swapchain.
    ShowWindow(hwnd_, config.maximized && config.resizable ? SW_SHOWMAXIMIZED : SW_SHOW);
    UpdateWindow(hwnd_);
    return true;
}

void Window::destroy() {
    if (hwnd_) {
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
}

void Window::dispatch(Event& event) {
    if (callback_) {
        callback_(event);
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
            Event e;
            e.type = EventType::WindowFocus;
            e.category = EventCategory::Window;
            e.focused = true;
            dispatch(e);
            return 0;
        }
        case WM_KILLFOCUS: {
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
            if (haveLastMouse_) {
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
