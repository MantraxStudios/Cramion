// Ventana de Android: la ANativeWindow de la NativeActivity y su entrada
// (android_native_app_glue). Ver Window.h.

#include "CramionDM/Window.h"

#include <android/configuration.h>
#include <android/input.h>
#include <android/keycodes.h>
#include <android/looper.h>
#include <android/log.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <android/window.h>
#include <sys/system_properties.h>
#include <android_native_app_glue.h>
#include <jni.h>

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <string>
#include <vector>

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

void Window::setApp(android_app* app) {
    g_app = app;
    installUiHooks();  // teclado en pantalla: ordenes al hilo de la interfaz
}
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
    // Pruebas automaticas por adb: `adb shell setprop debug.cramion.showlocked 1`
    // abre el juego encima de la pantalla de bloqueo (y enciende la pantalla).
    {
        char value[PROP_VALUE_MAX] = {};
        if (__system_property_get("debug.cramion.showlocked", value) > 0 && value[0] == '1') {
            ANativeActivity_setWindowFlags(g_app->activity, AWINDOW_FLAG_SHOW_WHEN_LOCKED | AWINDOW_FLAG_TURN_SCREEN_ON, 0);
        }
    }
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
    pumpSoftKeyboard();
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
        const int32_t meta = AKeyEvent_getMetaState(event);
        KeyMods mods = KeyMods::None;
        if (meta & AMETA_SHIFT_ON) mods |= KeyMods::Shift;
        if (meta & AMETA_CTRL_ON) mods |= KeyMods::Control;
        if (meta & AMETA_ALT_ON) mods |= KeyMods::Alt;
        // Texto de la tecla (teclado fisico o el de pantalla, que manda cada
        // letra como una tecla con su meta: KeyCharacterMap del dispositivo).
        // Sin Ctrl (atajos) ni caracteres de control (Intro, Borrar...).
        const auto text = [&](uint32_t c, int times) {
            if (c < 32 || c == 127 || hasMod(mods, KeyMods::Control)) return;
            for (int i = 0; i < times; ++i) {
                Event t;
                t.type = EventType::TextInput;
                t.category = EventCategory::Input | EventCategory::Keyboard;
                t.codepoint = c;
                dispatch(t);
            }
        };
        if (action == AKEY_EVENT_ACTION_MULTIPLE) {
            // La misma tecla varias veces (algunos IME). Una cadena de varias
            // letras (KEYCODE_UNKNOWN + getCharacters) no llega al NDK.
            if (code == AKEYCODE_UNKNOWN) return 1;
            const int times = std::clamp(AKeyEvent_getRepeatCount(event), 1, 64);
            const Key key = mapKey(code);
            for (int i = 0; i < times && key != Key::Unknown; ++i) {
                Event e;
                e.category = EventCategory::Input | EventCategory::Keyboard;
                e.key = key;
                e.mods = mods;
                e.type = EventType::KeyPressed;
                dispatch(e);
                e.type = EventType::KeyReleased;
                dispatch(e);
            }
            text(unicodeChar(event), times);
            return 1;
        }
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
        const uint32_t c = down ? unicodeChar(event) : 0;
        if (key == Key::Unknown && c == 0) return 0;
        if (key != Key::Unknown) {
            Event e;
            e.type = down ? EventType::KeyPressed : EventType::KeyReleased;
            e.category = EventCategory::Input | EventCategory::Keyboard;
            e.key = key;
            e.mods = mods;
            e.repeat = down && AKeyEvent_getRepeatCount(event) > 0;
            dispatch(e);
        }
        if (down) text(c, 1);
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

// --- Teclado en pantalla ---

namespace {

// Limpia una excepcion de Java pendiente (metodo que no existe en esta
// version de Android, permiso...). true si la habia.
bool clearException(JNIEnv* env) {
    if (!env->ExceptionCheck()) return false;
    env->ExceptionClear();
    return true;
}

// La DecorView de la actividad (referencia local) o nullptr.
jobject decorView(JNIEnv* env, jobject activity) {
    jclass cls = env->GetObjectClass(activity);
    jmethodID get_window = env->GetMethodID(cls, "getWindow", "()Landroid/view/Window;");
    env->DeleteLocalRef(cls);
    if (clearException(env) || get_window == nullptr) return nullptr;
    jobject window = env->CallObjectMethod(activity, get_window);
    if (clearException(env) || window == nullptr) return nullptr;
    jclass window_cls = env->GetObjectClass(window);
    jmethodID get_decor = env->GetMethodID(window_cls, "getDecorView", "()Landroid/view/View;");
    jobject decor = get_decor != nullptr ? env->CallObjectMethod(window, get_decor) : nullptr;
    if (clearException(env)) decor = nullptr;
    env->DeleteLocalRef(window_cls);
    env->DeleteLocalRef(window);
    return decor;
}

// El InputMethodManager (referencia local) o nullptr.
jobject inputMethodManager(JNIEnv* env, jobject activity) {
    jclass context = env->FindClass("android/content/Context");
    if (clearException(env) || context == nullptr) return nullptr;
    jfieldID field = env->GetStaticFieldID(context, "INPUT_METHOD_SERVICE", "Ljava/lang/String;");
    jobject name = field != nullptr ? env->GetStaticObjectField(context, field) : nullptr;
    jmethodID get_service = env->GetMethodID(context, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
    jobject imm = name != nullptr && get_service != nullptr ? env->CallObjectMethod(activity, get_service, name) : nullptr;
    if (clearException(env)) imm = nullptr;
    if (name != nullptr) env->DeleteLocalRef(name);
    env->DeleteLocalRef(context);
    return imm;
}

}  // namespace

uint32_t Window::unicodeChar(const void* raw) {
    const AInputEvent* event = static_cast<const AInputEvent*>(raw);
    const int32_t code = AKeyEvent_getKeyCode(event);
    const int32_t meta = AKeyEvent_getMetaState(event);
    // Sin JNI: lo basico (letras, numeros y espacio), como antes.
    const auto basic = [&]() -> uint32_t {
        if (code >= AKEYCODE_A && code <= AKEYCODE_Z) {
            return static_cast<uint32_t>(((meta & AMETA_SHIFT_ON) != 0 ? 'A' : 'a') + (code - AKEYCODE_A));
        }
        if (code >= AKEYCODE_0 && code <= AKEYCODE_9) return static_cast<uint32_t>('0' + (code - AKEYCODE_0));
        if (code == AKEYCODE_SPACE) return ' ';
        return 0;
    };
    if (g_app == nullptr) return basic();
    Jni jni(g_app->activity);
    JNIEnv* env = jni.env;
    if (env == nullptr) return basic();
    // Clase y metodos de KeyEvent: una vez (la clase como referencia global).
    static jclass key_class = nullptr;
    static jmethodID ctor = nullptr;
    static jmethodID get_unicode = nullptr;
    if (key_class == nullptr) {
        jclass local = env->FindClass("android/view/KeyEvent");
        if (clearException(env) || local == nullptr) return basic();
        key_class = static_cast<jclass>(env->NewGlobalRef(local));
        env->DeleteLocalRef(local);
        // KeyEvent(downTime, eventTime, action, code, repeat, metaState, deviceId, scancode)
        ctor = env->GetMethodID(key_class, "<init>", "(JJIIIIII)V");
        get_unicode = env->GetMethodID(key_class, "getUnicodeChar", "(I)I");
        if (clearException(env)) ctor = get_unicode = nullptr;
    }
    if (ctor == nullptr || get_unicode == nullptr) return basic();
    jobject key = env->NewObject(key_class, ctor, static_cast<jlong>(0), static_cast<jlong>(0), static_cast<jint>(0),
                                 static_cast<jint>(code), static_cast<jint>(0), static_cast<jint>(meta),
                                 static_cast<jint>(AInputEvent_getDeviceId(event)),
                                 static_cast<jint>(AKeyEvent_getScanCode(event)));
    if (clearException(env) || key == nullptr) return basic();
    const jint c = env->CallIntMethod(key, get_unicode, static_cast<jint>(meta));
    env->DeleteLocalRef(key);
    if (clearException(env)) return basic();
    // COMBINING_ACCENT (tecla muerta): no es un caracter todavia.
    if ((static_cast<uint32_t>(c) & 0x80000000u) != 0) return 0;
    return static_cast<uint32_t>(c);
}

// --- Teclado en pantalla: un EditText invisible en el hilo de la interfaz ---
//
// Android solo abre el IME para una vista que es un editor de texto (las
// demas "no se sirven": showSoftInput se ignora), y la NativeContentView de la
// NativeActivity no lo es. Sin codigo Java en el APK, el motor crea por JNI un
// EditText de 1x1 (no se ve: la superficie es del juego) y le da el foco. El
// IME escribe en su texto; aqui se compara con lo anterior y salen Backspace
// (lo borrado), los caracteres nuevos (TextInput) e Intro ('\n' = Enter).
//
// Las vistas solo se tocan en el hilo de la interfaz: el juego le manda
// ordenes por una tuberia que escucha el ALooper de ese hilo (se engancha la
// primera vez que la glue llama a un callback de la actividad en ese hilo).

namespace {

constexpr char kOrdenMostrar = 'S';
constexpr char kOrdenMostrarClave = 'P';
constexpr char kOrdenOcultar = 'H';
constexpr char kOrdenLeer = 'R';
// Texto de relleno: el IME siempre tiene algo que borrar (Borrar con el campo
// "vacio" no llegaria). Sin espacios (el doble espacio pone un punto).
const char16_t kRelleno[] = u"................................";
constexpr std::size_t kRellenoLen = 32;
constexpr uint32_t kImeBackspace = 0xFFFF0008u;
constexpr uint32_t kImeEnter = 0xFFFF000Du;

ALooper* g_ui_looper = nullptr;
int g_ui_pipe[2] = {-1, -1};
jobject g_edit = nullptr;            // EditText (referencia global; hilo de la interfaz)
std::u16string g_edit_last;          // su texto la ultima vez (hilo de la interfaz)
std::mutex g_ime_mutex;
std::vector<uint32_t> g_ime_out;     // caracteres / kImeBackspace / kImeEnter para el juego
ANativeActivityCallbacks g_glue{};   // los callbacks originales de la glue

JNIEnv* uiEnv(ANativeActivity* activity) {
    JNIEnv* env = nullptr;
    if (activity->vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) return nullptr;
    return env;
}

jmethodID method(JNIEnv* env, jobject object, const char* name, const char* signature) {
    jclass cls = env->GetObjectClass(object);
    jmethodID m = env->GetMethodID(cls, name, signature);
    env->DeleteLocalRef(cls);
    if (clearException(env)) return nullptr;
    return m;
}

void setEditText(JNIEnv* env, const char16_t* text, std::size_t length) {
    jstring s = env->NewString(reinterpret_cast<const jchar*>(text), static_cast<jsize>(length));
    if (jmethodID m = method(env, g_edit, "setText", "(Ljava/lang/CharSequence;)V")) env->CallVoidMethod(g_edit, m, s);
    clearException(env);
    if (jmethodID m = method(env, g_edit, "setSelection", "(I)V")) env->CallVoidMethod(g_edit, m, static_cast<jint>(length));
    clearException(env);
    env->DeleteLocalRef(s);
    g_edit_last.assign(text, length);
}

bool ensureEdit(JNIEnv* env, ANativeActivity* activity) {
    if (g_edit != nullptr) return true;
    jclass cls = env->FindClass("android/widget/EditText");
    if (clearException(env) || cls == nullptr) return false;
    jmethodID ctor = env->GetMethodID(cls, "<init>", "(Landroid/content/Context;)V");
    jobject edit = ctor != nullptr ? env->NewObject(cls, ctor, activity->clazz) : nullptr;
    env->DeleteLocalRef(cls);
    if (clearException(env) || edit == nullptr) return false;
    // 1x1 en la vista de contenido (no se dibuja: la superficie es del juego).
    jclass lp_cls = env->FindClass("android/view/ViewGroup$LayoutParams");
    jmethodID lp_ctor = lp_cls != nullptr ? env->GetMethodID(lp_cls, "<init>", "(II)V") : nullptr;
    jobject lp = lp_ctor != nullptr ? env->NewObject(lp_cls, lp_ctor, static_cast<jint>(1), static_cast<jint>(1)) : nullptr;
    if (clearException(env) || lp == nullptr) {
        env->DeleteLocalRef(edit);
        return false;
    }
    if (jmethodID add = method(env, activity->clazz, "addContentView",
                               "(Landroid/view/View;Landroid/view/ViewGroup$LayoutParams;)V")) {
        env->CallVoidMethod(activity->clazz, add, edit, lp);
    }
    env->DeleteLocalRef(lp);
    env->DeleteLocalRef(lp_cls);
    if (clearException(env)) {
        env->DeleteLocalRef(edit);
        return false;
    }
    // Sin pantalla completa del IME en horizontal (EXTRACT_UI) y con Intro
    // que escribe '\n' en vez de una accion (NO_ENTER_ACTION).
    if (jmethodID m = method(env, edit, "setImeOptions", "(I)V")) {
        env->CallVoidMethod(edit, m, static_cast<jint>(0x10000000 | 0x02000000 | 0x40000000));
    }
    if (jmethodID m = method(env, edit, "setAlpha", "(F)V")) env->CallVoidMethod(edit, m, 0.0f);
    clearException(env);
    g_edit = env->NewGlobalRef(edit);
    env->DeleteLocalRef(edit);
    return true;
}

void uiShow(JNIEnv* env, ANativeActivity* activity, bool password) {
    if (!ensureEdit(env, activity)) return;
    // Texto en varias lineas (Intro = '\n'), sin sugerencias; o contrasena.
    const jint type = password ? (0x00000001 | 0x00000080) : (0x00000001 | 0x00020000 | 0x00080000);
    if (jmethodID m = method(env, g_edit, "setInputType", "(I)V")) env->CallVoidMethod(g_edit, m, type);
    clearException(env);
    setEditText(env, kRelleno, kRellenoLen);
    if (jmethodID m = method(env, g_edit, "setFocusable", "(Z)V")) env->CallVoidMethod(g_edit, m, JNI_TRUE);
    if (jmethodID m = method(env, g_edit, "setFocusableInTouchMode", "(Z)V")) env->CallVoidMethod(g_edit, m, JNI_TRUE);
    if (jmethodID m = method(env, g_edit, "requestFocus", "()Z")) env->CallBooleanMethod(g_edit, m);
    clearException(env);
    jobject imm = inputMethodManager(env, activity->clazz);
    if (imm != nullptr) {
        if (jmethodID m = method(env, imm, "restartInput", "(Landroid/view/View;)V")) env->CallVoidMethod(imm, m, g_edit);
        clearException(env);
        if (jmethodID m = method(env, imm, "showSoftInput", "(Landroid/view/View;I)Z")) {
            env->CallBooleanMethod(imm, m, g_edit, static_cast<jint>(0));
        }
        clearException(env);
        env->DeleteLocalRef(imm);
    }
}

void uiHide(JNIEnv* env, ANativeActivity* activity) {
    if (g_edit == nullptr) return;
    jobject imm = inputMethodManager(env, activity->clazz);
    if (imm != nullptr) {
        jobject token = nullptr;
        if (jmethodID m = method(env, g_edit, "getWindowToken", "()Landroid/os/IBinder;")) token = env->CallObjectMethod(g_edit, m);
        if (clearException(env)) token = nullptr;
        if (token != nullptr) {
            if (jmethodID m = method(env, imm, "hideSoftInputFromWindow", "(Landroid/os/IBinder;I)Z")) {
                env->CallBooleanMethod(imm, m, token, static_cast<jint>(0));
            }
            clearException(env);
            env->DeleteLocalRef(token);
        }
        env->DeleteLocalRef(imm);
    }
    if (jmethodID m = method(env, g_edit, "clearFocus", "()V")) env->CallVoidMethod(g_edit, m);
    if (jmethodID m = method(env, g_edit, "setFocusable", "(Z)V")) env->CallVoidMethod(g_edit, m, JNI_FALSE);
    clearException(env);
}

// Lo que cambio el IME desde la ultima vez: borrados y caracteres nuevos.
void uiRead(JNIEnv* env) {
    if (g_edit == nullptr) return;
    jobject editable = nullptr;
    if (jmethodID m = method(env, g_edit, "getText", "()Landroid/text/Editable;")) editable = env->CallObjectMethod(g_edit, m);
    if (clearException(env) || editable == nullptr) return;
    jstring s = nullptr;
    if (jmethodID m = method(env, editable, "toString", "()Ljava/lang/String;")) {
        s = static_cast<jstring>(env->CallObjectMethod(editable, m));
    }
    env->DeleteLocalRef(editable);
    if (clearException(env) || s == nullptr) return;
    const jsize n = env->GetStringLength(s);
    std::u16string now(static_cast<std::size_t>(n), u'\0');
    if (n > 0) env->GetStringRegion(s, 0, n, reinterpret_cast<jchar*>(now.data()));
    env->DeleteLocalRef(s);
    if (now == g_edit_last) return;
    std::size_t same = 0;
    while (same < now.size() && same < g_edit_last.size() && now[same] == g_edit_last[same]) ++same;
    std::vector<uint32_t> out;
    for (std::size_t i = same; i < g_edit_last.size(); ++i) out.push_back(kImeBackspace);
    for (std::size_t i = same; i < now.size(); ++i) {
        uint32_t c = now[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < now.size()) {  // par sustituto (emoji...)
            c = 0x10000 + ((c - 0xD800) << 10) + (static_cast<uint32_t>(now[i + 1]) - 0xDC00);
            ++i;
        }
        out.push_back(c == '\n' ? kImeEnter : c);
    }
    g_edit_last = now;
    // Casi sin relleno (o mucho texto): se vuelve a poner.
    if (g_edit_last.size() < kRellenoLen / 2 || g_edit_last.size() > 256 || g_edit_last.find(u'\n') != std::u16string::npos) {
        setEditText(env, kRelleno, kRellenoLen);
    }
    std::lock_guard<std::mutex> lock(g_ime_mutex);
    g_ime_out.insert(g_ime_out.end(), out.begin(), out.end());
}

int onUiOrder(int fd, int, void*) {
    if (g_app == nullptr) return 1;
    ANativeActivity* activity = g_app->activity;
    JNIEnv* env = uiEnv(activity);
    char orders[64];
    ssize_t n;
    while ((n = read(fd, orders, sizeof(orders))) > 0) {
        if (env == nullptr) continue;
        for (ssize_t i = 0; i < n; ++i) {
            switch (orders[i]) {
                case kOrdenMostrar: uiShow(env, activity, false); break;
                case kOrdenMostrarClave: uiShow(env, activity, true); break;
                case kOrdenOcultar: uiHide(env, activity); break;
                case kOrdenLeer: uiRead(env); break;
                default: break;
            }
        }
    }
    return 1;  // seguir escuchando
}

// En el hilo de la interfaz (el de los callbacks de la actividad).
void hookUiThread() {
    if (g_ui_looper != nullptr) return;
    ALooper* looper = ALooper_forThread();
    if (looper == nullptr || pipe(g_ui_pipe) != 0) return;
    fcntl(g_ui_pipe[0], F_SETFL, O_NONBLOCK);
    fcntl(g_ui_pipe[1], F_SETFL, O_NONBLOCK);
    ALooper_acquire(looper);
    ALooper_addFd(looper, g_ui_pipe[0], ALOOPER_POLL_CALLBACK, ALOOPER_EVENT_INPUT, onUiOrder, nullptr);
    g_ui_looper = looper;
}

void sendUiOrder(char order) {
    if (g_ui_looper == nullptr || g_ui_pipe[1] < 0) return;
    const ssize_t ignored = write(g_ui_pipe[1], &order, 1);
    (void)ignored;
}

// Los callbacks de la glue con el enganche delante (el primero que llegue).
void hookFocus(ANativeActivity* a, int focused) {
    hookUiThread();
    if (g_glue.onWindowFocusChanged != nullptr) g_glue.onWindowFocusChanged(a, focused);
}
void hookResume(ANativeActivity* a) {
    hookUiThread();
    if (g_glue.onResume != nullptr) g_glue.onResume(a);
}
void hookContentRect(ANativeActivity* a, const ARect* r) {
    hookUiThread();
    if (g_glue.onContentRectChanged != nullptr) g_glue.onContentRectChanged(a, r);
}
void hookWindowCreated(ANativeActivity* a, ANativeWindow* w) {
    hookUiThread();
    if (g_glue.onNativeWindowCreated != nullptr) g_glue.onNativeWindowCreated(a, w);
}

}  // namespace

void Window::installUiHooks() {
    if (g_app == nullptr || g_app->activity == nullptr || g_app->activity->callbacks == nullptr) return;
    ANativeActivityCallbacks* cb = g_app->activity->callbacks;
    if (cb->onWindowFocusChanged == hookFocus) return;
    g_glue = *cb;
    cb->onWindowFocusChanged = hookFocus;
    cb->onResume = hookResume;
    cb->onContentRectChanged = hookContentRect;
    cb->onNativeWindowCreated = hookWindowCreated;
}

void Window::showSoftKeyboard(bool show, bool password) {
    if (g_app == nullptr) return;
    keyboard_wanted_ = show;
    if (g_ui_looper != nullptr) {
        sendUiOrder(show ? (password ? kOrdenMostrarClave : kOrdenMostrar) : kOrdenOcultar);
        return;
    }
    // Sin el hilo de la interfaz enganchado (no deberia pasar): lo de la NativeActivity.
    if (show) ANativeActivity_showSoftInput(g_app->activity, ANATIVEACTIVITY_SHOW_SOFT_INPUT_FORCED);
    else ANativeActivity_hideSoftInput(g_app->activity, 0);
}

void Window::pumpSoftKeyboard() {
    if (keyboard_wanted_) sendUiOrder(kOrdenLeer);  // se lee en el hilo de la interfaz; llega el frame siguiente
    std::vector<uint32_t> got;
    {
        std::lock_guard<std::mutex> lock(g_ime_mutex);
        got.swap(g_ime_out);
    }
    for (const uint32_t c : got) {
        if (c == kImeBackspace || c == kImeEnter) {
            Event e;
            e.category = EventCategory::Input | EventCategory::Keyboard;
            e.key = c == kImeBackspace ? Key::Backspace : Key::Enter;
            e.type = EventType::KeyPressed;
            dispatch(e);
            e.type = EventType::KeyReleased;
            dispatch(e);
            continue;
        }
        Event t;
        t.type = EventType::TextInput;
        t.category = EventCategory::Input | EventCategory::Keyboard;
        t.codepoint = c;
        dispatch(t);
    }
}

float Window::softKeyboardHeight() {
    if (g_app == nullptr) return 0.0f;
    Jni jni(g_app->activity);
    JNIEnv* env = jni.env;
    if (env == nullptr) return 0.0f;
    jobject decor = decorView(env, g_app->activity->clazz);
    if (decor == nullptr) return 0.0f;
    float pixels = -1.0f;
    jclass view_cls = env->GetObjectClass(decor);
    // Android 11+: el inset del IME (exacto, con la ventana a pantalla completa).
    jclass type_cls = env->FindClass("android/view/WindowInsets$Type");
    if (clearException(env)) type_cls = nullptr;
    if (type_cls != nullptr) {
        jmethodID ime = env->GetStaticMethodID(type_cls, "ime", "()I");
        if (clearException(env)) ime = nullptr;
        jmethodID root = env->GetMethodID(view_cls, "getRootWindowInsets", "()Landroid/view/WindowInsets;");
        if (clearException(env)) root = nullptr;
        if (ime != nullptr && root != nullptr) {
            const jint mask = env->CallStaticIntMethod(type_cls, ime);
            jobject insets = env->CallObjectMethod(decor, root);
            if (clearException(env)) insets = nullptr;
            if (insets != nullptr) {
                jclass insets_cls = env->GetObjectClass(insets);
                jmethodID get = env->GetMethodID(insets_cls, "getInsets", "(I)Landroid/graphics/Insets;");
                if (clearException(env)) get = nullptr;
                jobject value = get != nullptr ? env->CallObjectMethod(insets, get, mask) : nullptr;
                if (clearException(env)) value = nullptr;
                if (value != nullptr) {
                    jclass value_cls = env->GetObjectClass(value);
                    jfieldID bottom = env->GetFieldID(value_cls, "bottom", "I");
                    if (clearException(env)) bottom = nullptr;
                    if (bottom != nullptr) pixels = static_cast<float>(env->GetIntField(value, bottom));
                    env->DeleteLocalRef(value_cls);
                    env->DeleteLocalRef(value);
                }
                env->DeleteLocalRef(insets_cls);
                env->DeleteLocalRef(insets);
            }
        }
        env->DeleteLocalRef(type_cls);
    }
    if (pixels < 0.0f) {
        // Antes de Android 11: lo que falta de la zona visible de la ventana.
        jclass rect_cls = env->FindClass("android/graphics/Rect");
        if (clearException(env)) rect_cls = nullptr;
        if (rect_cls != nullptr) {
            jmethodID rect_ctor = env->GetMethodID(rect_cls, "<init>", "()V");
            jobject rect = rect_ctor != nullptr ? env->NewObject(rect_cls, rect_ctor) : nullptr;
            jmethodID frame = env->GetMethodID(view_cls, "getWindowVisibleDisplayFrame", "(Landroid/graphics/Rect;)V");
            jmethodID height = env->GetMethodID(view_cls, "getHeight", "()I");
            jfieldID bottom = env->GetFieldID(rect_cls, "bottom", "I");
            if (!clearException(env) && rect != nullptr && frame != nullptr && height != nullptr && bottom != nullptr) {
                env->CallVoidMethod(decor, frame, rect);
                const jint h = env->CallIntMethod(decor, height);
                if (!clearException(env)) {
                    const float missing = static_cast<float>(h - env->GetIntField(rect, bottom));
                    // Menos de un 15 %: la barra de navegacion, no el teclado.
                    pixels = missing > static_cast<float>(h) * 0.15f ? missing : 0.0f;
                }
            }
            clearException(env);
            if (rect != nullptr) env->DeleteLocalRef(rect);
            env->DeleteLocalRef(rect_cls);
        }
    }
    env->DeleteLocalRef(view_cls);
    env->DeleteLocalRef(decor);
    return std::max(pixels, 0.0f) * input_scale_;
}

}  // namespace cramion::dm
