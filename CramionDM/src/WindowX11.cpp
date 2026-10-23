// Ventana de Linux con Xlib (X11; en Wayland por XWayland). Traduce los
// eventos de X a los de CramionDM: teclado (con texto por XIM), raton, rueda,
// foco, cierre y redimensionado. La captura del cursor (primera persona)
// oculta el cursor, lo encierra (XGrabPointer) y lo devuelve al centro cada
// movimiento, mandando el desplazamiento como MouseRawMoved. Los mandos van
// por evdev (GamepadEvdev.h) y se leen en pumpEvents.

#if defined(__linux__) && !defined(__ANDROID__)

#include "CramionDM/Window.h"

// Antes que Xlib: sus macros (None, Bool...) chocan con los enum de los mandos.
#include "CramionDM/Input.h"
#include "GamepadEvdev.h"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>

#include <algorithm>
#include <chrono>
#include <codecvt>
#include <cstring>
#include <locale>
#include <thread>

namespace cramion::dm {

namespace {

Display* display(const NativeHandle& h) { return static_cast<Display*>(h.display); }

Key keyFromSym(KeySym sym) {
    if (sym >= XK_a && sym <= XK_z) return static_cast<Key>(static_cast<int>(Key::A) + static_cast<int>(sym - XK_a));
    if (sym >= XK_A && sym <= XK_Z) return static_cast<Key>(static_cast<int>(Key::A) + static_cast<int>(sym - XK_A));
    if (sym >= XK_0 && sym <= XK_9) return static_cast<Key>(static_cast<int>(Key::Num0) + static_cast<int>(sym - XK_0));
    if (sym >= XK_F1 && sym <= XK_F12) return static_cast<Key>(static_cast<int>(Key::F1) + static_cast<int>(sym - XK_F1));
    if (sym >= XK_KP_0 && sym <= XK_KP_9) {
        return static_cast<Key>(static_cast<int>(Key::Keypad0) + static_cast<int>(sym - XK_KP_0));
    }
    switch (sym) {
        case XK_BackSpace: return Key::Backspace;
        case XK_Tab: return Key::Tab;
        case XK_Return: case XK_KP_Enter: return Key::Enter;
        case XK_Pause: return Key::Pause;
        case XK_Caps_Lock: return Key::CapsLock;
        case XK_Escape: return Key::Escape;
        case XK_space: return Key::Space;
        case XK_Page_Up: return Key::PageUp;
        case XK_Page_Down: return Key::PageDown;
        case XK_End: return Key::End;
        case XK_Home: return Key::Home;
        case XK_Left: return Key::Left;
        case XK_Up: return Key::Up;
        case XK_Right: return Key::Right;
        case XK_Down: return Key::Down;
        case XK_Print: return Key::PrintScreen;
        case XK_Insert: return Key::Insert;
        case XK_Delete: return Key::Delete;
        case XK_Super_L: return Key::LeftSuper;
        case XK_Super_R: return Key::RightSuper;
        case XK_Menu: return Key::Apps;
        case XK_KP_Multiply: return Key::KeypadMultiply;
        case XK_KP_Add: return Key::KeypadAdd;
        case XK_KP_Subtract: return Key::KeypadSubtract;
        case XK_KP_Decimal: return Key::KeypadDecimal;
        case XK_KP_Divide: return Key::KeypadDivide;
        case XK_Num_Lock: return Key::NumLock;
        case XK_Scroll_Lock: return Key::ScrollLock;
        case XK_Shift_L: return Key::LeftShift;
        case XK_Shift_R: return Key::RightShift;
        case XK_Control_L: return Key::LeftControl;
        case XK_Control_R: return Key::RightControl;
        case XK_Alt_L: return Key::LeftAlt;
        case XK_Alt_R: case XK_ISO_Level3_Shift: return Key::RightAlt;
        case XK_semicolon: return Key::Semicolon;
        case XK_equal: return Key::Equal;
        case XK_comma: return Key::Comma;
        case XK_minus: return Key::Minus;
        case XK_period: return Key::Period;
        case XK_slash: return Key::Slash;
        case XK_grave: return Key::GraveAccent;
        case XK_bracketleft: return Key::LeftBracket;
        case XK_backslash: return Key::Backslash;
        case XK_bracketright: return Key::RightBracket;
        case XK_apostrophe: return Key::Apostrophe;
        default: return Key::Unknown;
    }
}

// Decodifica UTF-8 a puntos de codigo.
std::u32string utf32(const char* text, int length) {
    std::u32string out;
    for (int i = 0; i < length;) {
        const auto c = static_cast<unsigned char>(text[i]);
        char32_t cp = 0;
        int n = 1;
        if (c < 0x80) cp = c;
        else if ((c >> 5) == 0x6) { cp = c & 0x1F; n = 2; }
        else if ((c >> 4) == 0xE) { cp = c & 0x0F; n = 3; }
        else if ((c >> 3) == 0x1E) { cp = c & 0x07; n = 4; }
        for (int k = 1; k < n && i + k < length; ++k) cp = (cp << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3F);
        out.push_back(cp);
        i += n;
    }
    return out;
}

std::string narrow(const std::wstring& w) {
    std::string out;
    for (const wchar_t c : w) {
        const auto cp = static_cast<std::uint32_t>(c);
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    return out;
}

Atom atom(Display* dpy, const char* name) { return XInternAtom(dpy, name, False); }

// _NET_WM_MOVERESIZE (direcciones del estandar EWMH).
constexpr int kMoveResizeTopLeft = 0;
constexpr int kMoveResizeTop = 1;
constexpr int kMoveResizeTopRight = 2;
constexpr int kMoveResizeRight = 3;
constexpr int kMoveResizeBottomRight = 4;
constexpr int kMoveResizeBottom = 5;
constexpr int kMoveResizeBottomLeft = 6;
constexpr int kMoveResizeLeft = 7;
constexpr int kMoveResizeMove = 8;
constexpr int kResizeBorder = 6;  // pixeles del borde para redimensionar

std::wstring widen(const std::string& utf8) {
    std::wstring out;
    for (const char32_t cp : utf32(utf8.data(), static_cast<int>(utf8.size()))) out.push_back(static_cast<wchar_t>(cp));
    return out;
}

// "file:///home/a%20b/x.png" -> "/home/a b/x.png"; vacio si no es un archivo local.
std::string pathFromUri(const std::string& uri) {
    std::string rest;
    if (uri.rfind("file://", 0) == 0) {
        rest = uri.substr(7);
        const std::size_t slash = rest.find('/');
        if (slash == std::string::npos) return {};
        rest = rest.substr(slash);  // sin el host ("localhost" o vacio)
    } else if (!uri.empty() && uri[0] == '/') {
        rest = uri;
    } else {
        return {};
    }
    std::string out;
    for (std::size_t i = 0; i < rest.size(); ++i) {
        if (rest[i] == '%' && i + 2 < rest.size()) {
            out += static_cast<char>(std::stoi(rest.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else {
            out += rest[i];
        }
    }
    return out;
}

}  // namespace

Window::Window() = default;
Window::~Window() { destroy(); }

bool Window::create(const WindowConfig& config) {
    XInitThreads();
    Display* dpy = XOpenDisplay(nullptr);
    if (dpy == nullptr) return false;
    native_.display = dpy;
    const int screen = DefaultScreen(dpy);
    XSetWindowAttributes attributes{};
    attributes.event_mask = KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
                            StructureNotifyMask | FocusChangeMask | EnterWindowMask | LeaveWindowMask | ExposureMask |
                            PropertyChangeMask;
    attributes.background_pixel = BlackPixel(dpy, screen);
    const ::Window win = XCreateWindow(dpy, RootWindow(dpy, screen), 0, 0, config.width, config.height, 0,
                                       CopyFromParent, InputOutput, CopyFromParent, CWEventMask | CWBackPixel, &attributes);
    if (win == 0) {
        XCloseDisplay(dpy);
        native_ = {};
        return false;
    }
    native_.window = win;
    width_ = config.width;
    height_ = config.height;
    custom_title_bar_ = config.custom_title_bar;
    resizable_ = config.resizable;
    Atom delete_atom = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &delete_atom, 1);
    wm_delete_ = delete_atom;
    // Clase de la ventana (el .desktop la asocia por StartupWMClass).
    XClassHint class_hint{};
    std::string res_name = "cramion";
    std::string res_class = "Cramion";
    class_hint.res_name = res_name.data();
    class_hint.res_class = res_class.data();
    XSetClassHint(dpy, win, &class_hint);
    // Sin decoracion del gestor: la barra la dibuja la aplicacion.
    if (custom_title_bar_) {
        struct MotifHints {
            unsigned long flags, functions, decorations;
            long input_mode;
            unsigned long status;
        } hints{2 /* MWM_HINTS_DECORATIONS */, 0, 0, 0, 0};
        const Atom motif = atom(dpy, "_MOTIF_WM_HINTS");
        XChangeProperty(dpy, win, motif, motif, 32, PropModeReplace, reinterpret_cast<unsigned char*>(&hints), 5);
    }
    // Soltar archivos (XDnD version 5).
    const Atom dnd_version = 5;
    XChangeProperty(dpy, win, atom(dpy, "XdndAware"), XA_ATOM, 32, PropModeReplace,
                    reinterpret_cast<const unsigned char*>(&dnd_version), 1);
    if (!config.resizable) {
        XSizeHints hints{};
        hints.flags = PMinSize | PMaxSize;
        hints.min_width = hints.max_width = static_cast<int>(config.width);
        hints.min_height = hints.max_height = static_cast<int>(config.height);
        XSetWMNormalHints(dpy, win, &hints);
    }
    setTitle(config.title);
    // Texto con el metodo de entrada del sistema (acentos, compose).
    XSetLocaleModifiers("");
    XIM im = XOpenIM(dpy, nullptr, nullptr, nullptr);
    if (im != nullptr) {
        input_method_ = im;
        input_context_ = XCreateIC(im, XNInputStyle, XIMPreeditNothing | XIMStatusNothing, XNClientWindow, win,
                                   XNFocusWindow, win, nullptr);
    }
    // Cursor invisible (captura en primera persona).
    char empty[8] = {};
    const Pixmap pixmap = XCreateBitmapFromData(dpy, win, empty, 8, 8);
    XColor black{};
    blank_cursor_ = XCreatePixmapCursor(dpy, pixmap, pixmap, &black, &black, 0, 0);
    XFreePixmap(dpy, pixmap);
    if (config.visible) XMapWindow(dpy, win);
    if (config.maximized) {
        maximized_ = false;
        toggleMaximize();
    }
    XFlush(dpy);
    open_ = true;
    // Las ventanas sin mostrar (herramientas de prueba) no leen mandos.
    if (config.visible) {
        gamepads_ = std::make_unique<EvdevGamepads>();
        if (gamepadVibrationUser() == nullptr) setGamepadVibrationHandler(&Window::vibrationThunk, this);
    }
    return true;
}

void Window::destroy() {
    if (gamepads_) {
        gamepads_->stopAll();  // ningun mando se queda vibrando al cerrar
        gamepads_.reset();
        if (gamepadVibrationUser() == this) setGamepadVibrationHandler(nullptr, nullptr);
    }
    Display* dpy = display(native_);
    if (dpy == nullptr) return;
    if (input_context_ != nullptr) XDestroyIC(static_cast<XIC>(input_context_));
    if (input_method_ != nullptr) XCloseIM(static_cast<XIM>(input_method_));
    if (blank_cursor_ != 0) XFreeCursor(dpy, blank_cursor_);
    for (unsigned long& cursor : shape_cursors_) {
        if (cursor != 0) XFreeCursor(dpy, cursor);
        cursor = 0;
    }
    for (unsigned long& cursor : edge_cursors_) {
        if (cursor != 0) XFreeCursor(dpy, cursor);
        cursor = 0;
    }
    if (native_.window != 0) XDestroyWindow(dpy, native_.window);
    XCloseDisplay(dpy);
    native_ = {};
    input_context_ = input_method_ = nullptr;
    blank_cursor_ = 0;
    open_ = false;
}

void Window::dispatch(Event& event) {
    if (callback_) callback_(event);
}

KeyMods Window::currentMods(unsigned int state) const {
    KeyMods mods{};  // (KeyMods::None choca con la macro None de Xlib)
    if (state & ShiftMask) mods |= KeyMods::Shift;
    if (state & ControlMask) mods |= KeyMods::Control;
    if (state & Mod1Mask) mods |= KeyMods::Alt;
    if (state & Mod4Mask) mods |= KeyMods::Super;
    if (state & LockMask) mods |= KeyMods::CapsLock;
    if (state & Mod2Mask) mods |= KeyMods::NumLock;
    return mods;
}

void Window::pumpEvents() {
    Display* dpy = display(native_);
    if (dpy == nullptr) return;
    while (XPending(dpy) > 0) {
        XEvent xe;
        XNextEvent(dpy, &xe);
        if (XFilterEvent(&xe, 0)) continue;  // lo usa el metodo de entrada
        Event e;
        switch (xe.type) {
            case ClientMessage:
                if (xe.xclient.message_type == atom(dpy, "WM_PROTOCOLS") &&
                    static_cast<unsigned long>(xe.xclient.data.l[0]) == wm_delete_) {
                    e.type = EventType::WindowClose;
                    e.category = EventCategory::Window;
                    dispatch(e);
                } else {
                    handleDndMessage(&xe);
                }
                break;
            case SelectionRequest:
                handleSelectionRequest(&xe);
                break;
            case SelectionClear:
                clipboard_.clear();
                break;
            case SelectionNotify:
                if (xe.xselection.property == atom(dpy, "XdndSelection")) handleDndSelection(&xe);
                break;
            case PropertyNotify:
                if (xe.xproperty.atom == atom(dpy, "_NET_WM_STATE")) readWindowState();
                break;
            case ConfigureNotify:
                if (static_cast<uint32_t>(xe.xconfigure.width) != width_ ||
                    static_cast<uint32_t>(xe.xconfigure.height) != height_) {
                    width_ = static_cast<uint32_t>(xe.xconfigure.width);
                    height_ = static_cast<uint32_t>(xe.xconfigure.height);
                    e.type = EventType::WindowResize;
                    e.category = EventCategory::Window;
                    e.width = width_;
                    e.height = height_;
                    dispatch(e);
                }
                break;
            case FocusIn:
            case FocusOut:
                focused_ = xe.type == FocusIn;
                if (input_context_ != nullptr) {
                    if (focused_) XSetICFocus(static_cast<XIC>(input_context_));
                    else XUnsetICFocus(static_cast<XIC>(input_context_));
                }
                e.type = focused_ ? EventType::WindowFocus : EventType::WindowLostFocus;
                e.category = EventCategory::Window;
                e.focused = focused_;
                dispatch(e);
                if (cursor_captured_ || cursor_confined_) applyPointer();  // sin foco se suelta
                if (gamepads_) gamepads_->refreshRumble(focused_ || gamepads_background_);
                break;
            case KeyPress:
            case KeyRelease: {
                // Auto-repeticion: X manda Release+Press con la misma hora.
                bool repeat = false;
                if (xe.type == KeyRelease && XEventsQueued(dpy, QueuedAfterReading) > 0) {
                    XEvent next;
                    XPeekEvent(dpy, &next);
                    if (next.type == KeyPress && next.xkey.time == xe.xkey.time && next.xkey.keycode == xe.xkey.keycode) {
                        break;  // el Release de una repeticion: se ignora
                    }
                }
                KeySym sym = XLookupKeysym(&xe.xkey, 0);
                e.type = xe.type == KeyPress ? EventType::KeyPressed : EventType::KeyReleased;
                e.category = EventCategory::Keyboard | EventCategory::Input;
                e.key = keyFromSym(sym);
                e.mods = currentMods(xe.xkey.state);
                e.repeat = repeat;
                dispatch(e);
                if (xe.type == KeyPress) {
                    char buffer[64];
                    int length = 0;
                    KeySym text_sym = 0;
                    if (input_context_ != nullptr) {
                        Status status = 0;
                        length = Xutf8LookupString(static_cast<XIC>(input_context_), &xe.xkey, buffer,
                                                   sizeof(buffer) - 1, &text_sym, &status);
                        if (status != XLookupChars && status != XLookupBoth) length = 0;
                    } else {
                        length = XLookupString(&xe.xkey, buffer, sizeof(buffer) - 1, &text_sym, nullptr);
                    }
                    for (const char32_t cp : utf32(buffer, length)) {
                        if (cp < 32 || cp == 127) continue;
                        Event t;
                        t.type = EventType::TextInput;
                        t.category = EventCategory::Keyboard | EventCategory::Input;
                        t.codepoint = static_cast<uint32_t>(cp);
                        dispatch(t);
                    }
                }
                break;
            }
            case ButtonPress:
            case ButtonRelease: {
                const unsigned int b = xe.xbutton.button;
                if (b >= 4 && b <= 7) {  // rueda
                    if (xe.type == ButtonPress) {
                        e.type = EventType::MouseScrolled;
                        e.category = EventCategory::Mouse | EventCategory::Input;
                        e.scrollY = b == 4 ? 1.0f : b == 5 ? -1.0f : 0.0f;
                        e.scrollX = b == 6 ? -1.0f : b == 7 ? 1.0f : 0.0f;
                        e.mouseX = static_cast<float>(xe.xbutton.x);
                        e.mouseY = static_cast<float>(xe.xbutton.y);
                        dispatch(e);
                    }
                    break;
                }
                // Barra propia: arrastrar mueve la ventana, doble clic maximiza
                // y el borde redimensiona (lo hace el gestor de ventanas).
                if (custom_title_bar_ && xe.type == ButtonPress && b == 1 && !cursor_captured_) {
                    const int edge = resizeEdge(xe.xbutton.x, xe.xbutton.y);
                    if (edge >= 0) {
                        startMoveResize(edge, xe.xbutton.x_root, xe.xbutton.y_root, b);
                        break;
                    }
                    if (caption_test_ && caption_test_(xe.xbutton.x, xe.xbutton.y)) {
                        if (xe.xbutton.time - last_click_time_ < 400) {
                            last_click_time_ = 0;
                            toggleMaximize();
                        } else {
                            last_click_time_ = xe.xbutton.time;
                            startMoveResize(kMoveResizeMove, xe.xbutton.x_root, xe.xbutton.y_root, b);
                        }
                        break;
                    }
                }
                if (custom_title_bar_ && xe.type == ButtonPress && b == 3 && caption_test_ &&
                    caption_test_(xe.xbutton.x, xe.xbutton.y)) {
                    showSystemMenu(xe.xbutton.x_root, xe.xbutton.y_root);
                    break;
                }
                e.type = xe.type == ButtonPress ? EventType::MouseButtonPressed : EventType::MouseButtonReleased;
                e.category = EventCategory::Mouse | EventCategory::Input;
                e.button = b == 1 ? MouseButton::Left : b == 3 ? MouseButton::Right : b == 2 ? MouseButton::Middle
                           : b == 8 ? MouseButton::X1 : MouseButton::X2;
                e.mouseX = static_cast<float>(xe.xbutton.x);
                e.mouseY = static_cast<float>(xe.xbutton.y);
                e.mods = currentMods(xe.xbutton.state);
                dispatch(e);
                break;
            }
            case MotionNotify: {
                const float x = static_cast<float>(xe.xmotion.x);
                const float y = static_cast<float>(xe.xmotion.y);
                if (cursor_captured_) {
                    int icx = 0;
                    int icy = 0;
                    captureCenter(icx, icy);
                    const float cx = static_cast<float>(icx);
                    const float cy = static_cast<float>(icy);
                    if (x == cx && y == cy) break;  // el propio warp
                    e.type = EventType::MouseRawMoved;
                    e.category = EventCategory::Mouse | EventCategory::Input;
                    e.deltaX = x - cx;
                    e.deltaY = y - cy;
                    dispatch(e);
                    XWarpPointer(dpy, 0, native_.window, 0, 0, 0, 0, icx, icy);
                    XFlush(dpy);
                    break;
                }
                // Cursor de redimensionar sobre el borde (barra propia).
                if (custom_title_bar_) {
                    const int edge = resizeEdge(xe.xmotion.x, xe.xmotion.y);
                    static constexpr unsigned int kEdgeCursors[] = {XC_top_left_corner, XC_top_side, XC_top_right_corner,
                                                                    XC_right_side, XC_bottom_right_corner, XC_bottom_side,
                                                                    XC_bottom_left_corner, XC_left_side};
                    if (edge >= 0) {
                        if (edge_cursors_[edge] == 0) edge_cursors_[edge] = XCreateFontCursor(dpy, kEdgeCursors[edge]);
                        XDefineCursor(dpy, native_.window, edge_cursors_[edge]);
                        edge_cursor_ = true;
                    } else if (edge_cursor_) {
                        edge_cursor_ = false;
                        const SystemCursor shape = cursor_shape_;
                        cursor_shape_ = SystemCursor::Wait;  // fuerza a volver a ponerlo
                        setCursorShape(shape);
                    }
                }
                e.type = EventType::MouseMoved;
                e.category = EventCategory::Mouse | EventCategory::Input;
                e.mouseX = x;
                e.mouseY = y;
                e.deltaX = have_last_ ? x - last_x_ : 0.0f;
                e.deltaY = have_last_ ? y - last_y_ : 0.0f;
                last_x_ = x;
                last_y_ = y;
                have_last_ = true;
                dispatch(e);
                break;
            }
            case MapNotify:
            case UnmapNotify:
                if ((xe.type == UnmapNotify) != minimized_) {
                    minimized_ = xe.type == UnmapNotify;
                    e.type = minimized_ ? EventType::WindowMinimized : EventType::WindowRestored;
                    e.category = EventCategory::Window;
                    dispatch(e);
                }
                break;
            case EnterNotify:
            case LeaveNotify:
                e.type = xe.type == EnterNotify ? EventType::MouseEnter : EventType::MouseLeave;
                e.category = EventCategory::Mouse;
                dispatch(e);
                have_last_ = false;
                break;
            default:
                break;
        }
    }
    pollGamepads();
}

void Window::pollGamepads() {
    if (!gamepads_) return;
    gamepads_->poll(focused_ || gamepads_background_, [this](Event& e) { dispatch(e); });
}

bool Window::vibrationThunk(void* user, int pad, float low, float high, float seconds) {
    return static_cast<Window*>(user)->setGamepadVibration(pad, low, high, seconds);
}

bool Window::setGamepadVibration(int pad, float low, float high, float seconds) {
    if (!gamepads_) return false;
    return gamepads_->vibrate(pad, std::clamp(low, 0.0f, 1.0f), std::clamp(high, 0.0f, 1.0f), std::max(seconds, 0.0f),
                              focused_ || gamepads_background_);
}

void Window::setTitle(const std::wstring& title) {
    Display* dpy = display(native_);
    if (dpy == nullptr || native_.window == 0) return;
    const std::string utf8 = narrow(title);
    XStoreName(dpy, native_.window, utf8.c_str());
    const Atom net_name = XInternAtom(dpy, "_NET_WM_NAME", False);
    const Atom utf8_string = XInternAtom(dpy, "UTF8_STRING", False);
    XChangeProperty(dpy, native_.window, net_name, utf8_string, 8, PropModeReplace,
                    reinterpret_cast<const unsigned char*>(utf8.c_str()), static_cast<int>(utf8.size()));
}

void Window::minimize() {
    Display* dpy = display(native_);
    if (dpy != nullptr) XIconifyWindow(dpy, native_.window, DefaultScreen(dpy));
}

void Window::toggleMaximize() {
    Display* dpy = display(native_);
    if (dpy == nullptr) return;
    XEvent ev{};
    ev.type = ClientMessage;
    ev.xclient.window = native_.window;
    ev.xclient.message_type = atom(dpy, "_NET_WM_STATE");
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = maximized_ ? 0 : 1;  // quitar / poner
    ev.xclient.data.l[1] = static_cast<long>(atom(dpy, "_NET_WM_STATE_MAXIMIZED_VERT"));
    ev.xclient.data.l[2] = static_cast<long>(atom(dpy, "_NET_WM_STATE_MAXIMIZED_HORZ"));
    ev.xclient.data.l[3] = 1;  // la pide una aplicacion
    XSendEvent(dpy, DefaultRootWindow(dpy), False, SubstructureRedirectMask | SubstructureNotifyMask, &ev);
    XFlush(dpy);
    maximized_ = !maximized_;  // _NET_WM_STATE lo confirma despues (readWindowState)
}

void Window::readWindowState() {
    Display* dpy = display(native_);
    if (dpy == nullptr) return;
    Atom type = 0;
    int format = 0;
    unsigned long count = 0;
    unsigned long after = 0;
    unsigned char* data = nullptr;
    if (XGetWindowProperty(dpy, native_.window, atom(dpy, "_NET_WM_STATE"), 0, 64, False, XA_ATOM, &type, &format,
                           &count, &after, &data) != Success) {
        return;
    }
    bool vert = false;
    bool horz = false;
    bool hidden = false;
    const Atom max_vert = atom(dpy, "_NET_WM_STATE_MAXIMIZED_VERT");
    const Atom max_horz = atom(dpy, "_NET_WM_STATE_MAXIMIZED_HORZ");
    const Atom state_hidden = atom(dpy, "_NET_WM_STATE_HIDDEN");
    const auto* atoms = reinterpret_cast<const Atom*>(data);
    for (unsigned long i = 0; data != nullptr && i < count; ++i) {
        vert |= atoms[i] == max_vert;
        horz |= atoms[i] == max_horz;
        hidden |= atoms[i] == state_hidden;
    }
    if (data != nullptr) XFree(data);
    const bool maximized = vert && horz;
    Event e;
    e.category = EventCategory::Window;
    if (hidden != minimized_) {
        minimized_ = hidden;
        e.type = hidden ? EventType::WindowMinimized : EventType::WindowRestored;
        dispatch(e);
    }
    if (maximized != maximized_) {
        maximized_ = maximized;
        if (maximized) {
            e.type = EventType::WindowMaximized;
            dispatch(e);
        }
    }
}

int Window::resizeEdge(int x, int y) const {
    if (!resizable_ || maximized_) return -1;
    const int w = static_cast<int>(width_);
    const int h = static_cast<int>(height_);
    const bool left = x < kResizeBorder;
    const bool right = x >= w - kResizeBorder;
    const bool top = y < kResizeBorder;
    const bool bottom = y >= h - kResizeBorder;
    if (top && left) return kMoveResizeTopLeft;
    if (top && right) return kMoveResizeTopRight;
    if (bottom && left) return kMoveResizeBottomLeft;
    if (bottom && right) return kMoveResizeBottomRight;
    if (top) return kMoveResizeTop;
    if (bottom) return kMoveResizeBottom;
    if (left) return kMoveResizeLeft;
    if (right) return kMoveResizeRight;
    return -1;
}

void Window::startMoveResize(int direction, int root_x, int root_y, unsigned int button) {
    Display* dpy = display(native_);
    if (dpy == nullptr) return;
    // El gestor necesita el puntero libre para llevarse el arrastre.
    XUngrabPointer(dpy, CurrentTime);
    XEvent ev{};
    ev.type = ClientMessage;
    ev.xclient.window = native_.window;
    ev.xclient.message_type = atom(dpy, "_NET_WM_MOVERESIZE");
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = root_x;
    ev.xclient.data.l[1] = root_y;
    ev.xclient.data.l[2] = direction;
    ev.xclient.data.l[3] = static_cast<long>(button);
    ev.xclient.data.l[4] = 1;
    XSendEvent(dpy, DefaultRootWindow(dpy), False, SubstructureRedirectMask | SubstructureNotifyMask, &ev);
    XFlush(dpy);
}

void Window::showSystemMenu(int screen_x, int screen_y) {
    Display* dpy = display(native_);
    if (dpy == nullptr) return;
    XUngrabPointer(dpy, CurrentTime);
    XEvent ev{};
    ev.type = ClientMessage;
    ev.xclient.window = native_.window;
    ev.xclient.message_type = atom(dpy, "_GTK_SHOW_WINDOW_MENU");
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = 0;  // dispositivo (el puntero principal)
    ev.xclient.data.l[1] = screen_x;
    ev.xclient.data.l[2] = screen_y;
    XSendEvent(dpy, DefaultRootWindow(dpy), False, SubstructureRedirectMask | SubstructureNotifyMask, &ev);
    XFlush(dpy);
}

void Window::clientToScreen(int& x, int& y) const {
    Display* dpy = display(native_);
    if (dpy == nullptr) return;
    ::Window child = 0;
    int sx = 0;
    int sy = 0;
    XTranslateCoordinates(dpy, native_.window, DefaultRootWindow(dpy), x, y, &sx, &sy, &child);
    x = sx;
    y = sy;
}

void Window::requestClose() {
    Event e;
    e.type = EventType::WindowClose;
    e.category = EventCategory::Window;
    dispatch(e);
}

void Window::captureCenter(int& x, int& y) const {
    if (has_capture_rect_) {
        x = static_cast<int>((capture_rect_.left + capture_rect_.right) / 2);
        y = static_cast<int>((capture_rect_.top + capture_rect_.bottom) / 2);
    } else {
        x = static_cast<int>(width_ / 2);
        y = static_cast<int>(height_ / 2);
    }
}

void Window::setCursorCaptured(bool captured, const Rect* client_region) {
    Display* dpy = display(native_);
    const bool was = cursor_captured_;
    cursor_captured_ = captured;
    has_capture_rect_ = captured && client_region != nullptr;
    if (has_capture_rect_) capture_rect_ = *client_region;
    if (dpy == nullptr) return;
    if (captured && !was) {
        // Donde estaba, para devolverlo al soltar.
        ::Window root = 0;
        ::Window child = 0;
        int rx = 0;
        int ry = 0;
        unsigned int mask = 0;
        has_restore_ = XQueryPointer(dpy, native_.window, &root, &child, &rx, &ry, &restore_x_, &restore_y_, &mask);
    }
    if (!captured && was && has_restore_) {
        cursor_captured_ = false;
        applyPointer();
        XWarpPointer(dpy, 0, native_.window, 0, 0, 0, 0, restore_x_, restore_y_);
        has_restore_ = false;
        XFlush(dpy);
        return;
    }
    applyPointer();
    if (captured) {
        int cx = 0;
        int cy = 0;
        captureCenter(cx, cy);
        XWarpPointer(dpy, 0, native_.window, 0, 0, 0, 0, cx, cy);
    }
    XFlush(dpy);
}

void Window::setCursorConfined(bool confined, const Rect*) {
    if (confined == cursor_confined_) return;
    cursor_confined_ = confined;
    applyPointer();
}

void Window::setCursorVisible(bool visible) {
    if (visible == cursor_visible_) return;
    cursor_visible_ = visible;
    applyPointer();
}

void Window::applyPointer() {
    Display* dpy = display(native_);
    if (dpy == nullptr) return;
    const bool grab = focused_ && (cursor_captured_ || cursor_confined_);
    const bool hide = cursor_captured_ || !cursor_visible_;
    XUngrabPointer(dpy, CurrentTime);
    if (hide) {
        XDefineCursor(dpy, native_.window, blank_cursor_);
    } else {
        const SystemCursor shape = cursor_shape_;
        cursor_shape_ = SystemCursor::Wait;
        setCursorShape(shape);
    }
    if (grab) {
        XGrabPointer(dpy, native_.window, True, ButtonPressMask | ButtonReleaseMask | PointerMotionMask, GrabModeAsync,
                     GrabModeAsync, native_.window, hide ? blank_cursor_ : 0, CurrentTime);
    }
    XFlush(dpy);
}

void Window::setCursorShape(SystemCursor shape) {
    Display* dpy = display(native_);
    if (dpy == nullptr || shape == cursor_shape_) return;
    cursor_shape_ = shape;
    if (cursor_captured_ || !cursor_visible_) return;
    static constexpr unsigned int kShapes[] = {XC_left_ptr,  XC_xterm,       XC_fleur,         XC_sb_v_double_arrow,
                                               XC_sb_h_double_arrow, XC_bottom_left_corner, XC_bottom_right_corner,
                                               XC_hand2,     XC_X_cursor,    XC_watch};
    const auto index = static_cast<std::size_t>(shape);
    if (shape_cursors_[index] == 0) shape_cursors_[index] = XCreateFontCursor(dpy, kShapes[index]);
    XDefineCursor(dpy, native_.window, shape_cursors_[index]);
}

// --- Portapapeles -----------------------------------------------------------

void Window::setClipboardText(const std::string& utf8) {
    Display* dpy = display(native_);
    if (dpy == nullptr) return;
    clipboard_ = utf8;
    XSetSelectionOwner(dpy, atom(dpy, "CLIPBOARD"), native_.window, CurrentTime);
    XFlush(dpy);
}

std::string Window::clipboardText() {
    Display* dpy = display(native_);
    if (dpy == nullptr) return {};
    const Atom clipboard = atom(dpy, "CLIPBOARD");
    const ::Window owner = XGetSelectionOwner(dpy, clipboard);
    if (owner == native_.window) return clipboard_;
    if (owner == 0) return {};
    const Atom utf8 = atom(dpy, "UTF8_STRING");
    const Atom property = atom(dpy, "CRAMION_CLIPBOARD");
    XConvertSelection(dpy, clipboard, utf8, property, native_.window, CurrentTime);
    XFlush(dpy);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    XEvent xe;
    while (std::chrono::steady_clock::now() < deadline) {
        if (!XCheckTypedWindowEvent(dpy, native_.window, SelectionNotify, &xe)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        if (xe.xselection.property == 0) return {};
        Atom type = 0;
        int format = 0;
        unsigned long count = 0;
        unsigned long after = 0;
        unsigned char* data = nullptr;
        std::string text;
        if (XGetWindowProperty(dpy, native_.window, property, 0, 1 << 24, True, AnyPropertyType, &type, &format,
                               &count, &after, &data) == Success && data != nullptr) {
            text.assign(reinterpret_cast<const char*>(data), count);
            XFree(data);
        }
        return text;
    }
    return {};
}

void Window::handleSelectionRequest(const void* xevent) {
    Display* dpy = display(native_);
    const XSelectionRequestEvent& req = static_cast<const XEvent*>(xevent)->xselectionrequest;
    XEvent reply{};
    reply.xselection.type = SelectionNotify;
    reply.xselection.requestor = req.requestor;
    reply.xselection.selection = req.selection;
    reply.xselection.target = req.target;
    reply.xselection.time = req.time;
    reply.xselection.property = 0;
    const Atom utf8 = atom(dpy, "UTF8_STRING");
    const Atom targets = atom(dpy, "TARGETS");
    const Atom property = req.property != 0 ? req.property : req.target;
    if (req.target == targets) {
        const Atom supported[] = {targets, utf8, XA_STRING};
        XChangeProperty(dpy, req.requestor, property, XA_ATOM, 32, PropModeReplace,
                        reinterpret_cast<const unsigned char*>(supported), 3);
        reply.xselection.property = property;
    } else if (req.target == utf8 || req.target == XA_STRING || req.target == atom(dpy, "TEXT")) {
        XChangeProperty(dpy, req.requestor, property, req.target == XA_STRING ? XA_STRING : utf8, 8, PropModeReplace,
                        reinterpret_cast<const unsigned char*>(clipboard_.data()), static_cast<int>(clipboard_.size()));
        reply.xselection.property = property;
    }
    XSendEvent(dpy, req.requestor, False, 0, &reply);
    XFlush(dpy);
}

// --- Soltar archivos (XDnD) -------------------------------------------------

void Window::handleDndMessage(const void* xevent) {
    Display* dpy = display(native_);
    const XClientMessageEvent& msg = static_cast<const XEvent*>(xevent)->xclient;
    if (msg.message_type == atom(dpy, "XdndEnter")) {
        dnd_source_ = static_cast<unsigned long>(msg.data.l[0]);
        dnd_version_ = static_cast<int>(static_cast<unsigned long>(msg.data.l[1]) >> 24);
    } else if (msg.message_type == atom(dpy, "XdndPosition")) {
        dnd_source_ = static_cast<unsigned long>(msg.data.l[0]);
        int x = static_cast<int>((msg.data.l[2] >> 16) & 0xFFFF);
        int y = static_cast<int>(msg.data.l[2] & 0xFFFF);
        ::Window child = 0;
        XTranslateCoordinates(dpy, DefaultRootWindow(dpy), native_.window, x, y, &x, &y, &child);
        dnd_x_ = static_cast<float>(x);
        dnd_y_ = static_cast<float>(y);
        // Se acepta (bit 0) con la accion de copiar.
        XEvent ev{};
        ev.type = ClientMessage;
        ev.xclient.window = dnd_source_;
        ev.xclient.message_type = atom(dpy, "XdndStatus");
        ev.xclient.format = 32;
        ev.xclient.data.l[0] = static_cast<long>(native_.window);
        ev.xclient.data.l[1] = 1;
        ev.xclient.data.l[4] = static_cast<long>(atom(dpy, "XdndActionCopy"));
        XSendEvent(dpy, dnd_source_, False, NoEventMask, &ev);
        XFlush(dpy);
    } else if (msg.message_type == atom(dpy, "XdndDrop")) {
        dnd_source_ = static_cast<unsigned long>(msg.data.l[0]);
        const Time time = dnd_version_ >= 1 ? static_cast<Time>(msg.data.l[2]) : CurrentTime;
        XConvertSelection(dpy, atom(dpy, "XdndSelection"), atom(dpy, "text/uri-list"), atom(dpy, "XdndSelection"),
                          native_.window, time);
        XFlush(dpy);
    } else if (msg.message_type == atom(dpy, "XdndLeave")) {
        dnd_source_ = 0;
    }
}

void Window::handleDndSelection(const void*) {
    Display* dpy = display(native_);
    Atom type = 0;
    int format = 0;
    unsigned long count = 0;
    unsigned long after = 0;
    unsigned char* data = nullptr;
    Event e;
    e.type = EventType::FileDropped;
    e.category = EventCategory::File;
    e.mouseX = dnd_x_;
    e.mouseY = dnd_y_;
    if (XGetWindowProperty(dpy, native_.window, atom(dpy, "XdndSelection"), 0, 1 << 24, True, AnyPropertyType, &type,
                           &format, &count, &after, &data) == Success && data != nullptr) {
        const std::string list(reinterpret_cast<const char*>(data), count);
        XFree(data);
        std::size_t begin = 0;
        while (begin < list.size()) {
            std::size_t end = list.find('\n', begin);
            if (end == std::string::npos) end = list.size();
            std::string line = list.substr(begin, end - begin);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty() && line[0] != '#') {
                const std::string path = pathFromUri(line);
                if (!path.empty()) e.paths.push_back(widen(path));
            }
            begin = end + 1;
        }
    }
    if (dnd_source_ != 0) {
        XEvent ev{};
        ev.type = ClientMessage;
        ev.xclient.window = dnd_source_;
        ev.xclient.message_type = atom(dpy, "XdndFinished");
        ev.xclient.format = 32;
        ev.xclient.data.l[0] = static_cast<long>(native_.window);
        ev.xclient.data.l[1] = e.paths.empty() ? 0 : 1;
        ev.xclient.data.l[2] = static_cast<long>(atom(dpy, "XdndActionCopy"));
        XSendEvent(dpy, dnd_source_, False, NoEventMask, &ev);
        XFlush(dpy);
        dnd_source_ = 0;
    }
    if (!e.paths.empty()) dispatch(e);
}

}  // namespace cramion::dm

#endif  // __linux__
