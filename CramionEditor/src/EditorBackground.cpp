#include "EditorBackground.h"

#include "LoadingScreen.h"  // localDataFolder

#include <windows.h>
#include <dwmapi.h>

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
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

// Rectangulo visible de una ventana (sin la sombra invisible de Windows 10/11,
// que GetWindowRect incluye).
bool visibleRect(HWND window, RECT& rect) {
    if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &rect, sizeof(rect)))) return true;
    return GetWindowRect(window, &rect) != FALSE;
}

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

WindowPresence windowPresence(void* main_window) {
    const HWND self = static_cast<HWND>(main_window);
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
