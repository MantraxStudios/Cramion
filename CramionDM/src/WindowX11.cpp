// Ventana de Linux con Xlib (X11; en Wayland por XWayland). Traduce los
// eventos de X a los de CramionDM: teclado (con texto por XIM), raton, rueda,
// foco, cierre y redimensionado. La captura del cursor (primera persona)
// oculta el cursor, lo encierra (XGrabPointer) y lo devuelve al centro cada
// movimiento, mandando el desplazamiento como MouseRawMoved.

#if defined(__linux__) && !defined(__ANDROID__)

#include "CramionDM/Window.h"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include <codecvt>
#include <cstring>
#include <locale>

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

}  // namespace

Window::~Window() { destroy(); }

bool Window::create(const WindowConfig& config) {
    XInitThreads();
    Display* dpy = XOpenDisplay(nullptr);
    if (dpy == nullptr) return false;
    native_.display = dpy;
    const int screen = DefaultScreen(dpy);
    XSetWindowAttributes attributes{};
    attributes.event_mask = KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
                            StructureNotifyMask | FocusChangeMask | EnterWindowMask | LeaveWindowMask | ExposureMask;
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
    Atom delete_atom = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &delete_atom, 1);
    wm_delete_ = delete_atom;
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
    return true;
}

void Window::destroy() {
    Display* dpy = display(native_);
    if (dpy == nullptr) return;
    if (input_context_ != nullptr) XDestroyIC(static_cast<XIC>(input_context_));
    if (input_method_ != nullptr) XCloseIM(static_cast<XIM>(input_method_));
    if (blank_cursor_ != 0) XFreeCursor(dpy, blank_cursor_);
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
    KeyMods mods = KeyMods::None;
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
                if (static_cast<unsigned long>(xe.xclient.data.l[0]) == wm_delete_) {
                    e.type = EventType::WindowClose;
                    e.category = EventCategory::Window;
                    dispatch(e);
                }
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
                if (!focused_ && cursor_captured_) XUngrabPointer(dpy, CurrentTime);
                if (focused_ && cursor_captured_) setCursorCaptured(true);
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
                    const float cx = static_cast<float>(width_ / 2);
                    const float cy = static_cast<float>(height_ / 2);
                    if (x == cx && y == cy) break;  // el propio warp
                    e.type = EventType::MouseRawMoved;
                    e.category = EventCategory::Mouse | EventCategory::Input;
                    e.deltaX = x - cx;
                    e.deltaY = y - cy;
                    dispatch(e);
                    XWarpPointer(dpy, 0, native_.window, 0, 0, 0, 0, static_cast<int>(cx), static_cast<int>(cy));
                    XFlush(dpy);
                    break;
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
    ev.xclient.message_type = XInternAtom(dpy, "_NET_WM_STATE", False);
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = maximized_ ? 0 : 1;  // quitar / poner
    ev.xclient.data.l[1] = static_cast<long>(XInternAtom(dpy, "_NET_WM_STATE_MAXIMIZED_VERT", False));
    ev.xclient.data.l[2] = static_cast<long>(XInternAtom(dpy, "_NET_WM_STATE_MAXIMIZED_HORZ", False));
    XSendEvent(dpy, DefaultRootWindow(dpy), False, SubstructureRedirectMask | SubstructureNotifyMask, &ev);
    maximized_ = !maximized_;
}

void Window::requestClose() {
    Event e;
    e.type = EventType::WindowClose;
    e.category = EventCategory::Window;
    dispatch(e);
}

void Window::setCursorCaptured(bool captured) {
    Display* dpy = display(native_);
    cursor_captured_ = captured;
    if (dpy == nullptr) return;
    if (captured) {
        XDefineCursor(dpy, native_.window, blank_cursor_);
        XGrabPointer(dpy, native_.window, True, ButtonPressMask | ButtonReleaseMask | PointerMotionMask, GrabModeAsync,
                     GrabModeAsync, native_.window, blank_cursor_, CurrentTime);
        XWarpPointer(dpy, 0, native_.window, 0, 0, 0, 0, static_cast<int>(width_ / 2), static_cast<int>(height_ / 2));
    } else {
        XUngrabPointer(dpy, CurrentTime);
        XUndefineCursor(dpy, native_.window);
    }
    XFlush(dpy);
}

}  // namespace cramion::dm

#endif  // __linux__
