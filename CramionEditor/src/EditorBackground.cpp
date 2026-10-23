#include "EditorBackground.h"

#include "LoadingScreen.h"  // localDataFolder

#if defined(_WIN32)
#include <windows.h>
#include <dwmapi.h>
#else
#include "PlatformWindow.h"

#include <ctime>
#endif

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>
#include <vector>
#if !defined(_WIN32)
#include <cerrno>
#include <unistd.h>
#endif

#if defined(_WIN32)
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#else
// Xlib al final (sus macros chocan con nombres del motor).
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#undef None
#undef Bool
#undef Status
#undef Success
#undef Always
#endif

namespace cramion::editor {

namespace {

// Ahorro: la escena se dibuja como mucho una vez cada 2 s en segundo plano
// (lo de antes). Dibujar la vista a 10 fps gastaba la GPU de otro programa:
// con el juego exportado delante, el editor en una escena con nubes y luz
// volumetrica pedia ~30 ms de GPU por frame y el juego tenia tirones de 50 ms
// (p95).
constexpr float kSaverRedrawMs = 2000.0f;

std::filesystem::path preferencesFile() { return localDataFolder("Settings") / "EditorBackground.ini"; }

BackgroundPreferences loadPreferences() {
    BackgroundPreferences prefs;
    std::ifstream in(preferencesFile());
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("mode=", 0) == 0) {
            prefs.mode = static_cast<BackgroundMode>(std::clamp(std::atoi(line.c_str() + 5), 0, 2));
        }
        if (line.rfind("fps=", 0) == 0) prefs.fps = std::clamp(std::atoi(line.c_str() + 4), 1, 60);
    }
    return prefs;
}

#if defined(_WIN32)
// Rectangulo visible de una ventana (sin la sombra invisible de Windows 10/11,
// que GetWindowRect incluye).
bool visibleRect(HWND window, RECT& rect) {
    if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &rect, sizeof(rect)))) return true;
    return GetWindowRect(window, &rect) != FALSE;
}
#else
// Propiedad de ventana (XA_WINDOW / XA_ATOM...) como lista de longs.
std::vector<unsigned long> windowProperty(Display* display, ::Window window, const char* name, Atom type) {
    std::vector<unsigned long> out;
    const Atom property = XInternAtom(display, name, True);
    if (property == 0L) return out;
    Atom actual = 0;
    int format = 0;
    unsigned long count = 0;
    unsigned long remaining = 0;
    unsigned char* data = nullptr;
    if (XGetWindowProperty(display, window, property, 0, 1024, False, type, &actual, &format, &count, &remaining, &data) ==
            0 &&
        data != nullptr && format == 32) {
        const auto* values = reinterpret_cast<const unsigned long*>(data);  // formato 32 = long en Xlib
        out.assign(values, values + count);
    }
    if (data != nullptr) XFree(data);
    return out;
}

// La ventana de nivel superior (hija de la raiz) que contiene `window`: la
// que mueve el gestor de ventanas (con su marco).
::Window topLevel(Display* display, ::Window window) {
    ::Window current = window;
    for (int i = 0; i < 32 && current != 0; ++i) {
        ::Window root = 0;
        ::Window parent = 0;
        ::Window* children = nullptr;
        unsigned int count = 0;
        if (XQueryTree(display, current, &root, &parent, &children, &count) == 0) break;
        if (children != nullptr) XFree(children);
        if (parent == 0 || parent == root) return current;
        current = parent;
    }
    return window;
}

// Rectangulo en pantalla de la ventana de nivel superior.
bool screenRect(Display* display, ::Window window, RECT& rect) {
    XWindowAttributes attributes{};
    if (XGetWindowAttributes(display, window, &attributes) == 0) return false;
    int x = 0;
    int y = 0;
    ::Window child = 0;
    XTranslateCoordinates(display, window, attributes.root, 0, 0, &x, &y, &child);
    rect.left = x;
    rect.top = y;
    rect.right = x + attributes.width;
    rect.bottom = y + attributes.height;
    return attributes.map_state == IsViewable;
}

bool hasState(Display* display, ::Window window, const char* state) {
    const Atom wanted = XInternAtom(display, state, True);
    if (wanted == 0L) return false;
    for (const unsigned long atom : windowProperty(display, window, "_NET_WM_STATE", XA_ATOM)) {
        if (atom == wanted) return true;
    }
    return false;
}

unsigned long windowPid(Display* display, ::Window window) {
    const std::vector<unsigned long> pid = windowProperty(display, window, "_NET_WM_PID", XA_CARDINAL);
    return pid.empty() ? 0 : pid.front();
}
#endif

}  // namespace

BackgroundPreferences& backgroundPreferences() {
    static BackgroundPreferences prefs = loadPreferences();
    return prefs;
}

void saveBackgroundPreferences() {
    const BackgroundPreferences& prefs = backgroundPreferences();
    std::ofstream out(preferencesFile(), std::ios::trunc);
    out << "mode=" << static_cast<int>(prefs.mode) << "\n";
    out << "fps=" << prefs.fps << "\n";
}

#if defined(_WIN32)
WindowPresence windowPresence(const void* main_window) {
    const HWND self = static_cast<HWND>(const_cast<void*>(main_window));
    if (self == nullptr) return WindowPresence::Foreground;
    if (IsIconic(self)) return WindowPresence::Hidden;
    // En otro escritorio virtual (o tapada por DWM): «cloaked».
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(self, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0) {
        return WindowPresence::Hidden;
    }
    const HWND foreground = GetForegroundWindow();
    if (foreground == nullptr) return WindowPresence::Background;
    DWORD process = 0;
    GetWindowThreadProcessId(foreground, &process);
    // Las ventanas del propio editor (dialogos, ventanas sueltas de ImGui)
    // cuentan como primer plano.
    if (process == GetCurrentProcessId()) return WindowPresence::Foreground;
    // Tapada entera por la ventana de delante (el juego a pantalla completa,
    // otra aplicacion maximizada en el mismo monitor): no se ve nada.
    RECT mine{};
    RECT front{};
    if (!IsIconic(foreground) && visibleRect(self, mine) && visibleRect(foreground, front) &&
        front.left <= mine.left && front.top <= mine.top && front.right >= mine.right && front.bottom >= mine.bottom) {
        return WindowPresence::Hidden;
    }
    return WindowPresence::Background;
}
#else
WindowPresence windowPresence(const void* main_window) {
    const auto* handle = static_cast<const dm::NativeHandle*>(main_window);
    if (handle == nullptr || handle->display == nullptr || handle->window == 0) return WindowPresence::Foreground;
    Display* display = static_cast<Display*>(handle->display);
    const ::Window self = static_cast<::Window>(handle->window);
    XWindowAttributes attributes{};
    if (XGetWindowAttributes(display, self, &attributes) == 0) return WindowPresence::Foreground;
    // Minimizada (sin mapear) o en otro escritorio virtual (_NET_WM_STATE_HIDDEN).
    if (attributes.map_state != IsViewable || hasState(display, self, "_NET_WM_STATE_HIDDEN")) return WindowPresence::Hidden;
    // La ventana activa segun el gestor de ventanas; sin el, el foco de X.
    const ::Window root = DefaultRootWindow(display);
    ::Window active = 0;
    if (const std::vector<unsigned long> value = windowProperty(display, root, "_NET_ACTIVE_WINDOW", XA_WINDOW);
        !value.empty()) {
        active = static_cast<::Window>(value.front());
    } else {
        int revert = 0;
        XGetInputFocus(display, &active, &revert);
    }
    if (active == 0 || active == PointerRoot) return WindowPresence::Background;
    // La propia ventana (o una hija suya) o las del propio editor (mismo
    // proceso: dialogos) cuentan como primer plano.
    if (active == self || topLevel(display, active) == topLevel(display, self)) return WindowPresence::Foreground;
    const unsigned long pid = windowPid(display, active);
    if (pid != 0 && pid == static_cast<unsigned long>(getpid())) return WindowPresence::Foreground;
    // Tapada entera por la ventana de delante (el juego a pantalla completa,
    // otra aplicacion maximizada en el mismo monitor): no se ve nada.
    RECT mine{};
    RECT front{};
    if (screenRect(display, self, mine) && screenRect(display, active, front) &&
        !hasState(display, active, "_NET_WM_STATE_HIDDEN") && front.left <= mine.left && front.top <= mine.top &&
        front.right >= mine.right && front.bottom >= mine.bottom) {
        return WindowPresence::Hidden;
    }
    return WindowPresence::Background;
}
#endif

BackgroundDecision decideBackground(WindowPresence presence, bool allowed, bool busy, float ms_since_scene) {
    BackgroundDecision d;
    const BackgroundPreferences& prefs = backgroundPreferences();
    if (presence == WindowPresence::Foreground || !allowed || prefs.mode == BackgroundMode::Unlimited) return d;
    d.throttle = true;
    d.frame_ms = 1000.0f / static_cast<float>(std::clamp(prefs.fps, 1, 60));
    if (busy) return d;  // cargando o con una IA por MCP: la escena cada frame (limitado)
    if (presence == WindowPresence::Hidden) {
        d.draw_scene = false;  // no se ve
    } else if (prefs.mode == BackgroundMode::Saver) {
        d.draw_scene = ms_since_scene >= kSaverRedrawMs;
    }
    return d;
}

void waitUntil(std::chrono::steady_clock::time_point deadline) {
    using Clock = std::chrono::steady_clock;
#if !defined(_WIN32)
    // Linux: clock_nanosleep con hora absoluta en CLOCK_MONOTONIC (el reloj
    // de steady_clock en libstdc++/libc++), precisa a decenas de microsegundos.
    const auto spin = std::chrono::microseconds(200);
    if (deadline - Clock::now() > spin) {
        const auto target = std::chrono::duration_cast<std::chrono::nanoseconds>((deadline - spin).time_since_epoch()).count();
        timespec ts{};
        ts.tv_sec = static_cast<time_t>(target / 1000000000);
        ts.tv_nsec = static_cast<long>(target % 1000000000);
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr) == EINTR) {
        }
    }
    while (Clock::now() < deadline) std::this_thread::yield();
#else
    // Un temporizador por proceso (solo lo usa el hilo principal); vive hasta
    // el final del programa.
    static HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    const auto spin = std::chrono::microseconds(500);
    const auto now = Clock::now();
    if (deadline - now > spin) {
        const auto d = deadline - now - spin;
        bool waited = false;
        if (timer != nullptr) {
            LARGE_INTEGER due{};
            due.QuadPart = -static_cast<LONGLONG>(std::chrono::duration_cast<std::chrono::nanoseconds>(d).count() / 100);
            if (due.QuadPart < 0 && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
                WaitForSingleObject(timer, INFINITE);
                waited = true;
            }
        }
        if (!waited) std::this_thread::sleep_for(d);  // Windows anterior a 10 1803
    }
    while (Clock::now() < deadline) std::this_thread::yield();
#endif
}

void drawBackgroundMenu() {
    if (!ImGui::BeginMenu("Segundo plano")) return;
    BackgroundPreferences& prefs = backgroundPreferences();
    bool changed = false;
    struct Mode {
        BackgroundMode mode;
        const char* label;
        const char* tip;
    };
    static constexpr Mode kModes[] = {
        {BackgroundMode::Saver, "Ahorro", "La interfaz a los fps de abajo y la escena cada 2 s"},
        {BackgroundMode::Live, "Vista en vivo", "Interfaz y escena a los fps de abajo"},
        {BackgroundMode::Unlimited, "Sin límite", "Como en primer plano (gasta GPU aunque no lo uses)"},
    };
    for (const Mode& m : kModes) {
        if (ImGui::MenuItem(m.label, nullptr, prefs.mode == m.mode)) {
            prefs.mode = m.mode;
            changed = true;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", m.tip);
    }
    ImGui::Separator();
    ImGui::BeginDisabled(prefs.mode == BackgroundMode::Unlimited);
    static constexpr int kFps[] = {5, 10, 15, 30, 60};
    for (const int fps : kFps) {
        char label[24];
        std::snprintf(label, sizeof(label), "%d fps", fps);
        if (ImGui::MenuItem(label, nullptr, prefs.fps == fps)) {
            prefs.fps = fps;
            changed = true;
        }
    }
    ImGui::EndDisabled();
    ImGui::Separator();
    ImGui::TextDisabled("Minimizado o tapado por otra ventana no dibuja la escena.");
    ImGui::TextDisabled("Sin limitar en Play, con el casco, cargando y en pruebas.");
    ImGui::TextDisabled("Con una IA por MCP la escena se dibuja en cada frame.");
    ImGui::EndMenu();
    if (changed) saveBackgroundPreferences();
}

}  // namespace cramion::editor
