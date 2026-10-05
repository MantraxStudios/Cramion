// Ventana de Android: la ANativeWindow de la NativeActivity y su entrada
// (android_native_app_glue). Ver Window.h.

#include "CramionDM/Window.h"

#include <android/configuration.h>
#include <android/input.h>
#include <android/keycodes.h>
#include <android/log.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <android/window.h>
#include <android_native_app_glue.h>
#include <jni.h>

#include <algorithm>
#include <cmath>

namespace cramion::dm {

namespace {

android_app* g_app = nullptr;

void onCommand(android_app* app, int32_t command) {
    if (app->userData != nullptr) static_cast<Window*>(app->userData)->handleCommand(command);
}

int32_t onInput(android_app* app, AInputEvent* event) {
    return app->userData != nullptr ? static_cast<Window*>(app->userData)->handleInput(event) : 0;
}

Key mapKey(int32_t code) {
    if (code >= AKEYCODE_A && code <= AKEYCODE_Z) return static_cast<Key>(static_cast<int>(Key::A) + (code - AKEYCODE_A));
    if (code >= AKEYCODE_0 && code <= AKEYCODE_9) return static_cast<Key>(static_cast<int>(Key::Num0) + (code - AKEYCODE_0));
    if (code >= AKEYCODE_F1 && code <= AKEYCODE_F12) return static_cast<Key>(static_cast<int>(Key::F1) + (code - AKEYCODE_F1));
    if (code >= AKEYCODE_NUMPAD_0 && code <= AKEYCODE_NUMPAD_9) {
        return static_cast<Key>(static_cast<int>(Key::Keypad0) + (code - AKEYCODE_NUMPAD_0));
    }
    switch (code) {
        case AKEYCODE_BACK:
        case AKEYCODE_ESCAPE: return Key::Escape;
        case AKEYCODE_SPACE: return Key::Space;
        case AKEYCODE_ENTER:
        case AKEYCODE_NUMPAD_ENTER: return Key::Enter;
        case AKEYCODE_DEL: return Key::Backspace;
        case AKEYCODE_FORWARD_DEL: return Key::Delete;
        case AKEYCODE_TAB: return Key::Tab;
        case AKEYCODE_DPAD_UP: return Key::Up;
        case AKEYCODE_DPAD_DOWN: return Key::Down;
        case AKEYCODE_DPAD_LEFT: return Key::Left;
        case AKEYCODE_DPAD_RIGHT: return Key::Right;
        case AKEYCODE_SHIFT_LEFT: return Key::LeftShift;
        case AKEYCODE_SHIFT_RIGHT: return Key::RightShift;
        case AKEYCODE_CTRL_LEFT: return Key::LeftControl;
        case AKEYCODE_CTRL_RIGHT: return Key::RightControl;
        case AKEYCODE_ALT_LEFT: return Key::LeftAlt;
        case AKEYCODE_ALT_RIGHT: return Key::RightAlt;
        case AKEYCODE_PAGE_UP: return Key::PageUp;
        case AKEYCODE_PAGE_DOWN: return Key::PageDown;
        case AKEYCODE_MOVE_HOME: return Key::Home;
        case AKEYCODE_MOVE_END: return Key::End;
        case AKEYCODE_INSERT: return Key::Insert;
        case AKEYCODE_COMMA: return Key::Comma;
        case AKEYCODE_PERIOD: return Key::Period;
        case AKEYCODE_MINUS: return Key::Minus;
        case AKEYCODE_EQUALS: return Key::Equal;
        case AKEYCODE_SEMICOLON: return Key::Semicolon;
        case AKEYCODE_SLASH: return Key::Slash;
        case AKEYCODE_BACKSLASH: return Key::Backslash;
        case AKEYCODE_APOSTROPHE: return Key::Apostrophe;
        case AKEYCODE_GRAVE: return Key::GraveAccent;
        case AKEYCODE_LEFT_BRACKET: return Key::LeftBracket;
        case AKEYCODE_RIGHT_BRACKET: return Key::RightBracket;
        case AKEYCODE_CAPS_LOCK: return Key::CapsLock;
        default: return Key::Unknown;
    }
}

bool mapGamepadButton(int32_t code, GamepadButton& out) {
    switch (code) {
        case AKEYCODE_BUTTON_A: out = GamepadButton::A; return true;
        case AKEYCODE_BUTTON_B: out = GamepadButton::B; return true;
        case AKEYCODE_BUTTON_X: out = GamepadButton::X; return true;
        case AKEYCODE_BUTTON_Y: out = GamepadButton::Y; return true;
        case AKEYCODE_BUTTON_L1: out = GamepadButton::LeftShoulder; return true;
        case AKEYCODE_BUTTON_R1: out = GamepadButton::RightShoulder; return true;
        case AKEYCODE_BUTTON_THUMBL: out = GamepadButton::LeftStick; return true;
        case AKEYCODE_BUTTON_THUMBR: out = GamepadButton::RightStick; return true;
        case AKEYCODE_BUTTON_START: out = GamepadButton::Start; return true;
        case AKEYCODE_BUTTON_SELECT: out = GamepadButton::Back; return true;
        default: return false;
    }
}

// JNI: llamar a Java desde el hilo del juego (se engancha a la VM).
struct Jni {
    JNIEnv* env = nullptr;
    JavaVM* vm = nullptr;
    bool attached = false;
    explicit Jni(ANativeActivity* activity) : vm(activity->vm) {
        if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
            attached = vm->AttachCurrentThread(&env, nullptr) == JNI_OK;
            if (!attached) env = nullptr;
        }
    }
    ~Jni() {
        if (attached) vm->DetachCurrentThread();
    }
};

}  // namespace

void Window::setApp(android_app* app) { g_app = app; }
android_app* Window::app() { return g_app; }

Window::~Window() { destroy(); }

bool Window::create(const WindowConfig&) {
    if (g_app == nullptr) return false;
    g_app->userData = this;
    g_app->onAppCmd = onCommand;
    g_app->onInputEvent = onInput;
    open_ = true;
    if (g_app->config != nullptr) {
        const int32_t dpi = AConfiguration_getDensity(g_app->config);
        if (dpi > 0 && dpi != ACONFIGURATION_DENSITY_NONE) density_ = static_cast<float>(dpi) / 160.0f;
    }
    // Pantalla siempre encendida y a pantalla completa mientras se juega.
    ANativeActivity_setWindowFlags(g_app->activity, AWINDOW_FLAG_KEEP_SCREEN_ON | AWINDOW_FLAG_FULLSCREEN, 0);
    // La superficie llega con APP_CMD_INIT_WINDOW.
    while (window_ == nullptr && open_) pumpEvents();
    return window_ != nullptr;
}

void Window::destroy() {
    if (g_app != nullptr && g_app->userData == this) {
        g_app->userData = nullptr;
        g_app->onAppCmd = nullptr;
        g_app->onInputEvent = nullptr;
    }
    window_ = nullptr;
    open_ = false;
}

void Window::pumpEvents() {
    if (g_app == nullptr) return;
    while (true) {
        // Sin superficie (segundo plano) se espera; si no, solo lo pendiente.
        const bool idle = window_ != nullptr || g_app->destroyRequested != 0 || !open_;
        int events = 0;
        android_poll_source* source = nullptr;
        const int result = ALooper_pollOnce(idle ? 0 : -1, nullptr, &events, reinterpret_cast<void**>(&source));
        if (result < 0 && result != ALOOPER_POLL_CALLBACK) break;
        if (source != nullptr) source->process(g_app, source);
        if (g_app->destroyRequested != 0 && open_) {
            open_ = false;
            Event e;
            e.type = EventType::WindowClose;
            e.category = EventCategory::Window;
            dispatch(e);
        }
    }
}

void Window::dispatch(Event& event) {
    if (callback_) callback_(event);
}

void Window::setMaxShortSide(uint32_t pixels) {
    max_short_side_ = pixels;
    refreshSize();
}

void Window::refreshSize() {
    if (window_ == nullptr) return;
    // El de la pantalla (la swapchain fija el de sus imagenes, no este).
    const uint32_t native_w = static_cast<uint32_t>(std::max(ANativeWindow_getWidth(window_), 0));
    const uint32_t native_h = static_cast<uint32_t>(std::max(ANativeWindow_getHeight(window_), 0));
    uint32_t w = native_w;
    uint32_t h = native_h;
    const uint32_t short_side = std::min(native_w, native_h);
    if (max_short_side_ > 0 && short_side > max_short_side_) {
        const double scale = static_cast<double>(max_short_side_) / static_cast<double>(short_side);
        // Pares: algunos escaladores de la pantalla los prefieren.
        w = std::max<uint32_t>(2u, static_cast<uint32_t>(std::lround(native_w * scale)) & ~1u);
        h = std::max<uint32_t>(2u, static_cast<uint32_t>(std::lround(native_h * scale)) & ~1u);
    }
    input_scale_ = native_w > 0 ? static_cast<float>(w) / static_cast<float>(native_w) : 1.0f;
    if (w == width_ && h == height_) return;
    width_ = w;
    height_ = h;
    Event e;
    e.type = EventType::WindowResize;
    e.category = EventCategory::Window;
    e.width = w;
    e.height = h;
    dispatch(e);
}

void Window::handleCommand(int32_t command) {
    switch (command) {
        case APP_CMD_INIT_WINDOW: {
            const bool again = window_ == nullptr && width_ != 0;
            window_ = g_app->window;
            refreshSize();
            if (again) {
                Event e;
                e.type = EventType::WindowSurfaceCreated;
                e.category = EventCategory::Window;
                e.width = width_;
                e.height = height_;
                dispatch(e);
            }
            break;
        }
        case APP_CMD_TERM_WINDOW: {
            // Se suelta la swapchain ahora: al volver de aqui la ventana ya no existe.
            Event e;
            e.type = EventType::WindowSurfaceLost;
            e.category = EventCategory::Window;
            dispatch(e);
            window_ = nullptr;
            break;
        }
        case APP_CMD_WINDOW_RESIZED:
        case APP_CMD_CONFIG_CHANGED:
        case APP_CMD_CONTENT_RECT_CHANGED: refreshSize(); break;
        case APP_CMD_GAINED_FOCUS:
        case APP_CMD_LOST_FOCUS: {
            focused_ = command == APP_CMD_GAINED_FOCUS;
            Event e;
            e.type = focused_ ? EventType::WindowFocus : EventType::WindowLostFocus;
            e.category = EventCategory::Window;
            e.focused = focused_;
            dispatch(e);
            break;
        }
        case APP_CMD_PAUSE: resumed_ = false; break;
        case APP_CMD_RESUME: resumed_ = true; break;
        case APP_CMD_DESTROY: {
            if (!open_) break;
            open_ = false;
            Event e;
            e.type = EventType::WindowClose;
            e.category = EventCategory::Window;
            dispatch(e);
            break;
        }
        default: break;
    }
}

int32_t Window::handleInput(const void* raw) {
    const AInputEvent* event = static_cast<const AInputEvent*>(raw);
    const int32_t type = AInputEvent_getType(event);
    const int32_t source = AInputEvent_getSource(event);

    if (type == AINPUT_EVENT_TYPE_KEY) {
        const int32_t code = AKeyEvent_getKeyCode(event);
        const int32_t action = AKeyEvent_getAction(event);
        if (code == AKEYCODE_VOLUME_UP || code == AKEYCODE_VOLUME_DOWN || code == AKEYCODE_VOLUME_MUTE) return 0;
        if (action != AKEY_EVENT_ACTION_DOWN && action != AKEY_EVENT_ACTION_UP) return 1;
        const bool down = action == AKEY_EVENT_ACTION_DOWN;
        GamepadButton pad{};
        if (mapGamepadButton(code, pad) ||
            ((source & AINPUT_SOURCE_GAMEPAD) == AINPUT_SOURCE_GAMEPAD && code == AKEYCODE_BACK &&
             (pad = GamepadButton::Back, true))) {
            Event e;
            e.type = down ? EventType::GamepadButtonPressed : EventType::GamepadButtonReleased;
            e.category = EventCategory::Input;
            e.gamepadButton = pad;
            dispatch(e);
            return 1;
        }
        const Key key = mapKey(code);
        if (key == Key::Unknown) return 0;
        const int32_t meta = AKeyEvent_getMetaState(event);
        KeyMods mods = KeyMods::None;
        if (meta & AMETA_SHIFT_ON) mods |= KeyMods::Shift;
        if (meta & AMETA_CTRL_ON) mods |= KeyMods::Control;
        if (meta & AMETA_ALT_ON) mods |= KeyMods::Alt;
        Event e;
        e.type = down ? EventType::KeyPressed : EventType::KeyReleased;
        e.category = EventCategory::Input | EventCategory::Keyboard;
        e.key = key;
        e.mods = mods;
        e.repeat = down && AKeyEvent_getRepeatCount(event) > 0;
        dispatch(e);
        // Texto basico del teclado fisico (letras, numeros y espacio).
        if (down) {
            uint32_t c = 0;
            if (key >= Key::A && key <= Key::Z) {
                c = static_cast<uint32_t>((hasMod(mods, KeyMods::Shift) ? 'A' : 'a') + (static_cast<int>(key) - static_cast<int>(Key::A)));
            } else if (key >= Key::Num0 && key <= Key::Num9) {
                c = static_cast<uint32_t>('0' + (static_cast<int>(key) - static_cast<int>(Key::Num0)));
            } else if (key == Key::Space) {
                c = ' ';
            }
            if (c != 0) {
                Event t;
                t.type = EventType::TextInput;
                t.category = EventCategory::Input | EventCategory::Keyboard;
                t.codepoint = c;
                dispatch(t);
            }
        }
        return 1;
    }

    if (type != AINPUT_EVENT_TYPE_MOTION) return 0;
    const int32_t action = AMotionEvent_getAction(event);
    const int32_t masked = action & AMOTION_EVENT_ACTION_MASK;

    // Mando: sticks, gatillos y cruceta (HAT).
    if ((source & AINPUT_SOURCE_JOYSTICK) == AINPUT_SOURCE_JOYSTICK && masked == AMOTION_EVENT_ACTION_MOVE) {
        const auto axis = [&](int32_t a) { return AMotionEvent_getAxisValue(event, a, 0); };
        const float values[6] = {
            axis(AMOTION_EVENT_AXIS_X),
            -axis(AMOTION_EVENT_AXIS_Y),
            axis(AMOTION_EVENT_AXIS_Z),
            -axis(AMOTION_EVENT_AXIS_RZ),
            std::max(axis(AMOTION_EVENT_AXIS_LTRIGGER), axis(AMOTION_EVENT_AXIS_BRAKE)),
            std::max(axis(AMOTION_EVENT_AXIS_RTRIGGER), axis(AMOTION_EVENT_AXIS_GAS)),
        };
        for (int i = 0; i < 6; ++i) {
            if (std::fabs(values[i] - axes_[i]) < 0.001f) continue;
            axes_[i] = values[i];
            Event e;
            e.type = EventType::GamepadAxisMoved;
            e.category = EventCategory::Input;
            e.gamepadAxis = static_cast<GamepadAxis>(i);
            e.value = values[i];
            dispatch(e);
        }
        const float hx = axis(AMOTION_EVENT_AXIS_HAT_X);
        const float hy = axis(AMOTION_EVENT_AXIS_HAT_Y);
        const auto hat = [&](float before, float now, GamepadButton negative, GamepadButton positive) {
            const int b = before < -0.5f ? -1 : (before > 0.5f ? 1 : 0);
            const int n = now < -0.5f ? -1 : (now > 0.5f ? 1 : 0);
            if (b == n) return;
            Event e;
            e.category = EventCategory::Input;
            if (b != 0) {
                e.type = EventType::GamepadButtonReleased;
                e.gamepadButton = b < 0 ? negative : positive;
                dispatch(e);
            }
            if (n != 0) {
                e.type = EventType::GamepadButtonPressed;
                e.gamepadButton = n < 0 ? negative : positive;
                dispatch(e);
            }
        };
        hat(hat_x_, hx, GamepadButton::DpadLeft, GamepadButton::DpadRight);
        hat(hat_y_, hy, GamepadButton::DpadUp, GamepadButton::DpadDown);
        hat_x_ = hx;
        hat_y_ = hy;
        return 1;
    }

    // Rueda del raton (Chromebook, DeX).
    if (masked == AMOTION_EVENT_ACTION_SCROLL) {
        Event e;
        e.type = EventType::MouseScrolled;
        e.category = EventCategory::Input | EventCategory::Mouse;
        e.scrollX = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_HSCROLL, 0);
        e.scrollY = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_VSCROLL, 0);
        dispatch(e);
        return 1;
    }

    // Dedos (y el raton, como un dedo): un evento por puntero.
    const auto touch = [&](EventType kind, size_t index) {
        Event e;
        e.type = kind;
        e.category = EventCategory::Input;
        e.touchId = AMotionEvent_getPointerId(event, index);
        // De pixeles de la pantalla a pixeles de la imagen (setMaxShortSide).
        e.mouseX = AMotionEvent_getX(event, index) * input_scale_;
        e.mouseY = AMotionEvent_getY(event, index) * input_scale_;
        dispatch(e);
    };
    const size_t index = static_cast<size_t>((action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >>
                                             AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
    const size_t count = AMotionEvent_getPointerCount(event);
    switch (masked) {
        case AMOTION_EVENT_ACTION_DOWN:
        case AMOTION_EVENT_ACTION_POINTER_DOWN: touch(EventType::TouchBegan, index); break;
        case AMOTION_EVENT_ACTION_UP:
        case AMOTION_EVENT_ACTION_POINTER_UP: touch(EventType::TouchEnded, index); break;
        case AMOTION_EVENT_ACTION_MOVE:
            for (size_t i = 0; i < count; ++i) touch(EventType::TouchMoved, i);
            break;
        case AMOTION_EVENT_ACTION_CANCEL:
            for (size_t i = 0; i < count; ++i) touch(EventType::TouchEnded, i);
            break;
        default: return 0;
    }
    return 1;
}

void Window::vibrate(int milliseconds) {
    if (g_app == nullptr || milliseconds <= 0) return;
    Jni jni(g_app->activity);
    JNIEnv* env = jni.env;
    if (env == nullptr) return;
    jobject activity = g_app->activity->clazz;
    jclass context = env->FindClass("android/content/Context");
    jfieldID field = env->GetStaticFieldID(context, "VIBRATOR_SERVICE", "Ljava/lang/String;");
    jobject name = env->GetStaticObjectField(context, field);
    jmethodID get_service = env->GetMethodID(context, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
    jobject vibrator = env->CallObjectMethod(activity, get_service, name);
    if (vibrator != nullptr) {
        jclass vibrator_class = env->GetObjectClass(vibrator);
        jmethodID vibrate = env->GetMethodID(vibrator_class, "vibrate", "(J)V");
        if (vibrate != nullptr) env->CallVoidMethod(vibrator, vibrate, static_cast<jlong>(milliseconds));
        env->DeleteLocalRef(vibrator_class);
        env->DeleteLocalRef(vibrator);
    }
    if (env->ExceptionCheck()) env->ExceptionClear();  // sin permiso VIBRATE
    env->DeleteLocalRef(name);
    env->DeleteLocalRef(context);
}

void Window::setOrientation(int android_orientation) {
    if (g_app == nullptr) return;
    Jni jni(g_app->activity);
    JNIEnv* env = jni.env;
    if (env == nullptr) return;
    jobject activity = g_app->activity->clazz;
    jclass cls = env->GetObjectClass(activity);
    jmethodID set = env->GetMethodID(cls, "setRequestedOrientation", "(I)V");
    if (set != nullptr) env->CallVoidMethod(activity, set, static_cast<jint>(android_orientation));
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->DeleteLocalRef(cls);
}

std::string Window::internalDataPath() const {
    return g_app != nullptr && g_app->activity->internalDataPath != nullptr ? g_app->activity->internalDataPath : "";
}

std::string Window::externalDataPath() const {
    return g_app != nullptr && g_app->activity->externalDataPath != nullptr ? g_app->activity->externalDataPath : "";
}

std::string Window::obbPath() const {
    return g_app != nullptr && g_app->activity->obbPath != nullptr ? g_app->activity->obbPath : "";
}

}  // namespace cramion::dm
