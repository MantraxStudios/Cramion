// CramionPlayer: el juego exportado desde el editor (Archivo > Exportar juego).
//
//   <Juego>/
//     <Juego>.exe      este programa
//     shaders/         SPIR-V del motor
//     Game/            el proyecto (Assets, ProjectSettings, .crproj),
//                      game.ini (escena inicial) y banner.png
//
// Al abrir muestra el banner del motor mientras carga la escena inicial y
// despues juega: render, fisica (Jolt), scripts Lua, audio, cinematicas,
// particulas e interfaz (Canvas).
//
// Android (libmain.so en un APK, NativeActivity): el mismo juego. El APK
// lleva assets/shaders, assets/Game (game.ini, banner.png) y el .crpack
// (o va aparte en un .obb). Se copian a los datos internos de la app la
// primera vez (o cuando cambia la compilacion) y el tactil se convierte en
// raton, teclas y ejes con los controles en pantalla (dm::TouchControls).

#include <CramionCore/profiling/Profiler.h>
#include "GraphicsConfig.h"
#include "ImGuiLayer.h"
#include "LoadingScreen.h"
#include "ProfilerOverlay.h"
#include "UiRenderer.h"

#include <CramionCore/cvar/CVar.h>
#include <CramionCore/scripting/CppScripts.h>
#include <CramionCore/CramionCore.h>
#include <CramionCore/ecs/FloatingOrigin.h>
#include <CramionCore/project/DataPack.h>
#include <CramionCore/project/Pack.h>
#include <CramionCore/project/TouchInterface.h>
#include <CramionCore/input/InputActions.h>
#include <CramionCore/gameplay/Localization.h>
#include <CramionCore/gameplay/SaveGame.h>
#include <CramionCore/xr/XrRig.h>
#include <CramionCore/fluid/Fluid.h>
#include <CramionCore/platform/Steam.h>
#include <CramionCore/replay/Replay.h>
#include <CramionCore/lighting/ProbeBaker.h>
#include <CramionCore/twod/System2D.h>
#include <CramionCore/vfx/VisualEffect.h>
#include <CramionDM/CramionDM.h>
#include <CramionFX/CramionFX.h>

#include <imgui.h>
#include <nlohmann/json.hpp>
#if defined(_WIN32)
#include <imgui_impl_win32.h>
#else
#include "AndroidSupport.h"

#include <CramionCore/net/Http.h>
#include <android_native_app_glue.h>

#include <cstdio>
#include <cstring>
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif
#include <cmath>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

#if defined(_WIN32)
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
#endif

namespace {

using namespace cramion;

#if defined(_WIN32)
std::filesystem::path exeFolder() {
    wchar_t buffer[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    return std::filesystem::path(buffer).parent_path();
}
#endif

std::string readIniValue(const std::filesystem::path& file, const std::string& key) {
    std::ifstream in(file);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();  // escrito en Windows (CRLF), leido en Android
        const std::size_t eq = line.find('=');
        if (eq != std::string::npos && line.substr(0, eq) == key) return line.substr(eq + 1);
    }
    return {};
}

#if defined(_WIN32)
std::wstring widen(const std::string& text) {
    if (text.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n);
    return out;
}

void showError(const std::string& message) { MessageBoxA(nullptr, message.c_str(), "Cramion", MB_ICONERROR); }
#else
std::wstring widen(const std::string& text) { return std::wstring(text.begin(), text.end()); }

void showError(const std::string& message) { std::cerr << "[Juego] Error: " << message << std::endl; }

// Copia del APK a los datos internos lo que el juego lee de disco: los
// shaders del motor y Game/ (game.ini, banner.png). El .crpack no (es
// grande: se descomprime directo mas tarde). Solo si cambio la compilacion
// (build= de game.ini) o falta algo.
bool installFromApk(const std::filesystem::path& root, std::string* error) {
    const std::filesystem::path stamp = root / ".apk_build";
    std::string apk_ini;
    {
        const std::filesystem::path probe = root / ".game_ini_apk";
        if (!android::extractAsset("Game/game.ini", probe, nullptr, error)) return false;
        apk_ini = readIniValue(probe, "build");
        std::error_code e;
        std::filesystem::remove(probe, e);
    }
    std::string installed;
    std::ifstream(stamp) >> installed;
    std::error_code e;
    if (!apk_ini.empty() && installed == apk_ini && std::filesystem::exists(root / "shaders", e) &&
        std::filesystem::exists(root / "Game" / "game.ini", e)) {
        return true;
    }
    std::filesystem::remove_all(root / "shaders", e);
    for (const std::string& name : android::assetFolder("shaders")) {
        if (!android::extractAsset("shaders/" + name, root / "shaders" / name, nullptr, error)) return false;
    }
    // Fuentes de los shaders de superficie (shaders/source).
    for (const std::string& name : android::assetFolder("shaders/source")) {
        if (!android::extractAsset("shaders/source/" + name, root / "shaders" / "source" / name, nullptr, error)) return false;
    }
    for (const std::string& name : android::assetFolder("Game")) {
        if (name.size() > 7 && name.substr(name.size() - 7) == ".crpack") continue;
        if (!android::extractAsset("Game/" + name, root / "Game" / name, nullptr, error)) return false;
    }
    std::ofstream(stamp) << apk_ini;
    return true;
}
#endif

}  // namespace

int runPlayer() {
    using namespace cramion;
    try {
#if defined(_WIN32)
        ImGui_ImplWin32_EnableDpiAwareness();
        const std::filesystem::path root = exeFolder();
        const std::filesystem::path game = root / "Game";
        wchar_t exe_path[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
        const std::filesystem::path exe_stem = std::filesystem::path(exe_path).stem();
        const std::u8string stem_u8 = exe_stem.u8string();
        const std::string game_name(stem_u8.begin(), stem_u8.end());

        // Sin consola: la salida y los crashes van a %LOCALAPPDATA%/Cramion.
        editor::installCrashHandler(game_name);
        static std::ofstream log_file(editor::localDataFolder("Logs") / std::filesystem::path(exe_stem).concat(".log"));
        if (log_file) {
            std::cout.rdbuf(log_file.rdbuf());
            std::cerr.rdbuf(log_file.rdbuf());
        }
#else
        // Android: todo en los datos internos de la app; los logs a logcat.
        const std::filesystem::path root = android::dataRoot();
        const std::filesystem::path game = root / "Game";
        const std::filesystem::path exe_stem = "Game";
        const std::string game_name = "Game";
        android::redirectLogs(editor::localDataFolder("Logs") / "Game.log");
        editor::installCrashHandler(game_name);
        {
            std::string error;
            if (!installFromApk(root, &error)) {
                showError("No se pudieron preparar los archivos del juego: " + error);
                return EXIT_FAILURE;
            }
        }
        gfx::shaders::setDirectory(root / "shaders");
#endif

        // Configuracion de compilacion (game.ini): titulo, ventana y FPS.
        const std::filesystem::path ini = game / "game.ini";
        std::string title = readIniValue(ini, "title");
        if (title.empty()) title = game_name;
        const std::string window_setting = readIniValue(ini, "window");
        int window_mode = window_setting.empty() ? 0 : std::clamp(std::atoi(window_setting.c_str()), 0, 2);
        int window_width = std::clamp(std::atoi(readIniValue(ini, "width").c_str()), 0, 16384);
        int window_height = std::clamp(std::atoi(readIniValue(ini, "height").c_str()), 0, 16384);
        // Lo que el jugador eligio y guardo desde el juego (Graphics.save()
        // en Lua) manda sobre la configuracion de compilacion.
        const std::filesystem::path player_graphics =
            editor::localDataFolder("Saves") / std::filesystem::path(exe_stem).concat(".graphics.ini");
        {
            editor::WindowMode saved_mode = static_cast<editor::WindowMode>(window_mode);
            if (editor::readWindowSettings(player_graphics, saved_mode, window_width, window_height)) {
                window_mode = static_cast<int>(saved_mode);
            }
        }
        const bool show_fps = readIniValue(ini, "show_fps") == "1";
        // Realidad virtual (OpenXR): si hay casco, el juego se ve en el; la
        // ventana queda de espejo. Sin runtime o sin casco, sigue sin VR.
        const bool vr_requested = readIniValue(ini, "vr") == "1";
        // vr_runtime=auto|steamvr|meta|system (sin la linea: automatico, que
        // prueba SteamVR si esta abierto, el activo de Windows y Meta).
        const xr::RuntimeChoice vr_runtime = xr::runtimeChoiceFromKey(readIniValue(ini, "vr_runtime"));
        // Una camara por ojo (estereo). vr_mono=1: una sola imagen para los dos
        // ojos (la mitad de coste, sin profundidad).
        const bool vr_stereo = readIniValue(ini, "vr_mono") != "1";

        dm::Window window;
        if (!window.create({.title = widen(title),
                            .width = static_cast<std::uint32_t>(window_width >= 320 ? window_width : 1600),
                            .height = static_cast<std::uint32_t>(window_height >= 240 ? window_height : 900),
                            .maximized = window_mode == 0})) {
            showError("No se pudo crear la ventana.");
            return EXIT_FAILURE;
        }
#if defined(_WIN32)
        if (window_mode == 1) {
            // Pantalla completa sin bordes: la ventana ocupa todo el monitor.
            HWND hwnd = window.handle();
            MONITORINFO monitor{};
            monitor.cbSize = sizeof(monitor);
            GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor);
            SetWindowLongPtrW(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
            const RECT& r = monitor.rcMonitor;
            SetWindowPos(hwnd, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
            window.pumpEvents();
        }
#endif
        // El banner del motor desde el primer momento: descomprimir los
        // assets y compilar los shaders llevan su porcentaje.
        auto loading = std::make_unique<editor::LoadingScreen>(window.handle(), game / "banner.png");
        loading->show(0.0f, "Cargando");

        // Assets: Game/<Juego>.crpack se descomprime una vez en
        // %LOCALAPPDATA%/Cramion/Games/<Juego> (se reutiliza si no cambio).
        // Sin paquete (exportaciones antiguas), la carpeta Game tal cual.
        // Android: el paquete es el .obb (si se exporto aparte) o va dentro
        // del APK (se copia fuera, se descomprime y se borra la copia).
        const auto prepare_project = [&]() -> std::optional<project::ProjectInfo> {
        std::filesystem::path project_folder = game;
        {
            std::filesystem::path pack;
            std::error_code e;
            for (std::filesystem::directory_iterator it(game, e); !e && it != std::filesystem::directory_iterator(); it.increment(e)) {
                if (it->path().extension() == ".crpack") pack = it->path();
            }
            std::string apk_pack;  // ruta dentro del APK
#if !defined(_WIN32)
            if (const std::string obb = window.obbPath(); !obb.empty()) {
                std::error_code oe;
                for (std::filesystem::directory_iterator it(obb, oe); !oe && it != std::filesystem::directory_iterator(); it.increment(oe)) {
                    const std::string name = it->path().filename().string();
                    if (name.rfind("main.", 0) == 0 && it->path().extension() == ".obb") pack = it->path();
                }
                if (!pack.empty()) std::cout << "[Juego] Assets del OBB: " << pack.string() << "\n";
            }
            if (pack.empty()) {
                for (const std::string& name : android::assetFolder("Game")) {
                    if (name.size() > 7 && name.substr(name.size() - 7) == ".crpack") apk_pack = "Game/" + name;
                }
            }
#endif
            if (!pack.empty() || !apk_pack.empty()) {
                // Dentro del APK no se puede leer el indice sin sacarlo: vale
                // la compilacion (build= de game.ini) y el tamano.
                const std::string id = apk_pack.empty() ? project::packId(pack)
                                                        : "apk-" + readIniValue(ini, "build") + "-" +
#if defined(_WIN32)
                                                              std::string();
#else
                                                              std::to_string(android::assetSize(apk_pack));
#endif
                const std::filesystem::path data = editor::localDataFolder("Games") / exe_stem;
                std::string current;
                std::ifstream(data / ".crpack_id") >> current;
                if (id.empty() || current != id) {
                    // Dentro del APK: se lee de alli mismo (va sin comprimir en
                    // el zip, asi que es un trozo del archivo del APK).
                    std::uint64_t pack_base = 0;
                    std::uint64_t pack_length = 0;
                    bool pack_copied = false;
#if !defined(_WIN32)
                    android::AssetRange range;
                    if (!apk_pack.empty()) {
                        if (android::openAssetRange(apk_pack, range)) {
                            pack = range.path;
                            pack_base = range.offset;
                            pack_length = range.length;
                        } else {
                            // Comprimido dentro del APK (no deberia): se copia fuera.
                            pack = root / "pack.crpack";
                            std::string copy_error;
                            const bool copied = android::extractAsset(apk_pack, pack, [&](std::uint64_t done, std::uint64_t total) {
                                loading->show(total > 0 ? static_cast<float>(static_cast<double>(done) / static_cast<double>(total)) : 1.0f,
                                              "Preparando los assets");
                            }, &copy_error);
                            if (!copied) throw std::runtime_error("No se pudieron copiar los assets del APK: " + copy_error);
                            pack_copied = true;
                        }
                    }
#endif
                    std::vector<project::PackEntry> entries;
                    std::string error;
                    if (!project::readPackIndex(pack, entries, &error, pack_base, pack_length)) throw std::runtime_error(error);
                    std::uint64_t total = 0;
                    for (const project::PackEntry& entry : entries) total += entry.size;
                    std::filesystem::remove_all(data, e);
                    std::filesystem::create_directories(data, e);
                    const auto unpack_start = std::chrono::steady_clock::now();
                    const bool ok = project::extractPack(pack, data, [&](std::uint64_t done, const std::string&) {
                        loading->show(total > 0 ? static_cast<float>(static_cast<double>(done) / static_cast<double>(total)) : 1.0f,
                                      "Descomprimiendo assets");
                        return true;
                    }, &error, pack_base, pack_length);
                    if (!ok) throw std::runtime_error("No se pudieron descomprimir los assets: " + error);
                    std::cout << "[Juego] Assets descomprimidos: " << (total >> 20) << " MB en "
                              << std::chrono::duration<double>(std::chrono::steady_clock::now() - unpack_start).count()
                              << " s\n";
                    std::ofstream(data / ".crpack_id") << id;
                    if (pack_copied) std::filesystem::remove(pack, e);  // la copia del APK ya no hace falta
#if !defined(_WIN32)
                    android::closeAssetRange(range);
#endif
                }
                project_folder = data;
            }
        }
        return project::openProject(project_folder);
        };
        std::optional<project::ProjectInfo> project;
#if defined(_WIN32)
        project = prepare_project();
        if (!project) {
            showError("No se encontro el juego (carpeta Game).");
            return EXIT_FAILURE;
        }
        SetWindowTextW(window.handle(), widen(title).c_str());
#endif

        dm::Input input;
        scene::Scene scene;
        scene.initialize();
        scene.camera().setAspectRatio(static_cast<float>(window.width()) / static_cast<float>(std::max(window.height(), 1u)));

        gfx::VulkanRenderer renderer;
        const std::string app_name = project ? project->name : title;
        const gfx::EngineInfo engine_info{.app_name = app_name.c_str(),
                                          .engine_name = "Cramion Engine",
                                          .enable_validation = false,
                                          .enable_xr = vr_requested,
                                          .xr_runtime = static_cast<int>(vr_runtime)};
        renderer.setLoadingCallback([&](float fraction, const char* what) { loading->show(fraction, what); });
        renderer.initialize(engine_info, window.handle(), window.width(), window.height());
#if defined(_WIN32)
        loading.reset();
        editor::loadGraphicsIni(project->settingsFolder() / "Graphics.ini", renderer);
        editor::loadGraphicsIni(player_graphics, renderer);  // la del jugador, encima
#endif
        renderer.setEditorHelpersEnabled(false);
        const bool vr = renderer.xrAvailable();
        if (vr) {
            // El casco marca el ritmo (xrWaitFrame): la ventana sin vsync.
            gfx::GraphicsSettings g = renderer.graphicsSettings();
            g.vsync = false;
            renderer.setGraphicsSettings(g);
            renderer.xr().setStereo(vr_stereo);
            std::cout << "[VR] " << renderer.xr().systemName() << " (" << renderer.xr().runtimeName() << "), "
                      << renderer.xr().eyeExtent().width << "x" << renderer.xr().eyeExtent().height << " por ojo\n";
        } else if (vr_requested) {
            std::cout << "[VR] Sin realidad virtual: " << renderer.xr().error() << "\n";
        }
        xr::XrRig xr_rig;

        editor::ImGuiLayer imgui;
#if !defined(_WIN32)
        // La interfaz a la escala de la pantalla (un poco menos que la
        // densidad: los moviles tienen mucha y la pantalla es pequena).
        imgui.setContentScale(std::max(window.density() * 0.8f, 1.0f));
        imgui.setDisplaySize(window.width(), window.height());
#endif
        imgui.initialize(window.handle(), renderer);

        // Pantalla de carga (encima de todo).
        const auto draw_loading = [&](const ImVec2& display, float fraction, const std::string& text) {
            ImDrawList* fg = ImGui::GetForegroundDrawList();
            fg->AddRectFilled(ImVec2(0, 0), display, IM_COL32(13, 15, 20, 255));
            ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 2.0f);
            const ImVec2 title_size = ImGui::CalcTextSize(title.c_str());
            fg->AddText(ImVec2((display.x - title_size.x) * 0.5f, display.y * 0.5f - 70.0f), IM_COL32(235, 238, 245, 255),
                        title.c_str());
            ImGui::PopFont();
            const float width = std::min(520.0f, display.x - 64.0f);
            const ImVec2 a{(display.x - width) * 0.5f, display.y * 0.5f};
            const ImVec2 b{a.x + width, a.y + 8.0f};
            fg->AddRectFilled(a, b, IM_COL32(40, 44, 54, 255), 4.0f);
            fg->AddRectFilled(a, ImVec2(a.x + width * std::clamp(fraction, 0.0f, 1.0f), b.y), IM_COL32(90, 150, 255, 255), 4.0f);
            char percent[16];
            std::snprintf(percent, sizeof(percent), "%.0f %%", fraction * 100.0f);
            const ImVec2 percent_size = ImGui::CalcTextSize(percent);
            fg->AddText(ImVec2(b.x - percent_size.x, b.y + 10.0f), IM_COL32(150, 160, 180, 255), percent);
            fg->AddText(ImVec2(a.x, b.y + 10.0f), IM_COL32(150, 160, 180, 255), text.c_str());
        };

        bool quit = false;
        // Controles tactiles (moviles): la interfaz tactil del proyecto
        // (ProjectSettings/TouchInterface.json, se disena en el editor).
        dm::TouchControls touch;
        {
            if (project) touch.setLayout(project::loadTouchInterface(project::touchInterfaceFile(project->settingsFolder())));
            touch.setScreen(static_cast<float>(window.width()), static_cast<float>(window.height()),
#if defined(_WIN32)
                            1.0f);
#else
                            window.density());
#endif
        }
        // Lo que sale de los controles tactiles va a Input y a la interfaz
        // (ImGui: en Android no tiene backend de plataforma).
        std::vector<dm::Event> synthesized;
        const auto feed = [&](dm::Event& e) {
            input.onEvent(e);
#if !defined(_WIN32)
            ImGuiIO& io = ImGui::GetIO();
            switch (e.type) {
                case dm::EventType::MouseMoved:
                    io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
                    io.AddMousePosEvent(e.mouseX, e.mouseY);
                    break;
                case dm::EventType::MouseButtonPressed:
                case dm::EventType::MouseButtonReleased:
                    io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
                    io.AddMouseButtonEvent(static_cast<int>(e.button), e.type == dm::EventType::MouseButtonPressed);
                    break;
                case dm::EventType::MouseScrolled: io.AddMouseWheelEvent(e.scrollX, e.scrollY); break;
                case dm::EventType::TextInput: io.AddInputCharacter(e.codepoint); break;
                case dm::EventType::KeyPressed:
                case dm::EventType::KeyReleased:
                    if (e.key == dm::Key::Backspace) io.AddKeyEvent(ImGuiKey_Backspace, e.type == dm::EventType::KeyPressed);
                    if (e.key == dm::Key::Enter) io.AddKeyEvent(ImGuiKey_Enter, e.type == dm::EventType::KeyPressed);
                    break;
                default: break;
            }
#endif
        };
#if defined(_WIN32)
        window.setMessageHook([](HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
            return ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam) != 0;
        });
#endif
        core::Clock touch_clock;
        double touch_time = 0.0;
        window.setEventCallback([&](dm::Event& e) {
            switch (e.type) {
                case dm::EventType::TouchBegan:
                case dm::EventType::TouchMoved:
                case dm::EventType::TouchEnded:
                    input.onEvent(e);  // Input.getTouch en Lua
                    synthesized.clear();
                    touch.onEvent(e, touch_time, synthesized);
                    for (dm::Event& s : synthesized) feed(s);
                    return;
                default: break;
            }
            feed(e);
            switch (e.type) {
                case dm::EventType::WindowResize:
                    renderer.onResize(e.width, e.height);
                    if (e.height > 0) scene.camera().setAspectRatio(static_cast<float>(e.width) / static_cast<float>(e.height));
#if !defined(_WIN32)
                    imgui.setDisplaySize(e.width, e.height);
                    touch.setScreen(static_cast<float>(e.width), static_cast<float>(e.height), window.density());
#endif
                    break;
                case dm::EventType::WindowMinimized: renderer.onResize(0, 0); break;
                case dm::EventType::WindowRestored: renderer.onResize(window.width(), window.height()); break;
                case dm::EventType::WindowClose: quit = true; break;
                // Android: segundo plano y vuelta (la swapchain va y viene).
                case dm::EventType::WindowSurfaceLost: renderer.releaseSurface(); break;
                case dm::EventType::WindowSurfaceCreated:
                    renderer.replaceWindow(window.handle(), e.width, e.height);
                    break;
                case dm::EventType::WindowLostFocus:
                    synthesized.clear();
                    touch.releaseAll(synthesized);
                    for (dm::Event& s : synthesized) feed(s);
                    break;
                default: break;
            }
        });

#if !defined(_WIN32)
        // Con el renderizador listo, la pantalla de carga se pinta con el.
        editor::setLoadingPainter([&](float fraction, const char* status) {
            window.pumpEvents();
            renderer.applyPendingResize();
            imgui.beginFrame();
            draw_loading(ImGui::GetIO().DisplaySize, fraction, status);
            imgui.endFrame();
            renderer.drawFrame(scene);
        });
        loading->show(0.0f, "Cargando");
        project = prepare_project();
        loading.reset();
        editor::setLoadingPainter(nullptr);
        if (!project) {
            showError("No se encontro el juego en el APK.");
            return EXIT_FAILURE;
        }
        touch.setLayout(project::loadTouchInterface(project::touchInterfaceFile(project->settingsFolder())));
        // Perfil movil: la calidad de la configuracion de compilacion (Baja
        // por defecto), sin trazado de rayos y con el presupuesto adaptativo
        // a los FPS objetivo. Encima, lo que el jugador guardo.
        editor::loadGraphicsIni(project->settingsFolder() / "Graphics.ini", renderer);
        {
            const std::string quality = readIniValue(ini, "android_quality");
            const int level = quality.empty() ? 0 : std::clamp(std::atoi(quality.c_str()), 0, 3);
            editor::applyQualityPreset(renderer, level);
            renderer.setRayTracingEnabled(false);
            renderer.setMobileProfile(level);
            // Las nubes volumetricas cuestan mucho en un movil: desde Alta.
            if (level < 2) renderer.setCloudsEnabled(false);
            gfx::GraphicsSettings g = renderer.graphicsSettings();
            const int fps = std::atoi(readIniValue(ini, "android_fps").c_str());
            g.target_fps = static_cast<float>(fps >= 15 ? fps : 30);
            g.adaptive = true;
            g.vsync = true;
            renderer.setGraphicsSettings(g);
        }
        editor::loadGraphicsIni(player_graphics, renderer);
        std::cout << "[Juego] " << project->name << " en " << renderer.device().name() << " (" << window.width() << "x"
                  << window.height() << ", densidad " << window.density() << ")\n";
#endif

        // --- Sistemas del juego ---
        ecs::registerPrefabComponents();
        terrain::registerTerrainComponents();
        water::registerWaterComponents();
        foliage::registerFoliageComponents();
        fire::registerFireComponents();
        fluid::registerFluidComponents();
        navigation::registerNavigationComponents();
        voxel::registerVoxelComponents();
        audio::registerAudioComponents();
        scripting::registerScriptComponents();
        ui::registerUiComponents();
        xr::registerXrComponents();

        assets::AssetDatabase database;
        database.open(project->assetsFolder());
        assets::AssetManager asset_manager(database);
        asset_manager.setCacheFolder(std::filesystem::temp_directory_path() / "Cramion" / project->name);
        ecs::RenderSync sync(asset_manager);
        sync.reset(scene);
        terrain::TerrainStore terrains;
        terrains.setRoot(project->assetsFolder());
        sync.setTerrainStore(&terrains);

        physics::PhysicsSettings physics_settings;
        physics::loadPhysicsSettings(project->settingsFolder() / "Physics.json", physics_settings);
        physics::setProjectPhysicsSettings(physics_settings);
        std::vector<std::string> tags = ecs::defaultTags();
        ecs::loadTags(project->settingsFolder() / "Tags.json", tags);
        ecs::setProjectTags(tags);
        physics::PhysicsSystem physics;
        xr_rig.setPhysics(&physics);  // XrPlayer: jugador VR con CharacterController
        physics.setSettings(physics_settings);
        physics.setAssetManager(&asset_manager);
        physics.setTerrainProvider([&](ecs::Entity entity) -> std::shared_ptr<const terrain::TerrainData> {
            const terrain::Terrain* comp = entity.tryGet<terrain::Terrain>();
            return comp != nullptr ? terrains.get(*comp) : nullptr;
        });
        physics.setMeshProvider([&](ecs::Entity entity) -> const asset::ModelData* { return sync.actorModelData(entity, scene); });
        // Suelo para el IK de los pies (el propio personaje no cuenta).
        sync.setGroundQuery([&](const core::Vec3& origin, const core::Vec3& direction, float max_distance, core::Vec3& point,
                                core::Vec3& normal, ecs::Entity self) {
            return physics::footGroundRaycast(physics, origin, direction, max_distance, point, normal, self);
        });
        // En el aire (Character Controller): los pies no buscan el suelo.
        sync.setSupportQuery([&](ecs::Entity self) { return physics::characterSupport(physics, self); });

        audio::AudioSystem audio;
        audio.setAssetsRoot(project->assetsFolder());
        audio.setOcclusionQuery(audio::physicsOcclusionQuery(physics));
        scripting::ScriptSystem scripts;
        scripts.setAssetsRoot(project->assetsFolder());
        // DataPack.load: lo montado entra en la base de assets (sus modelos,
        // prefabs y escenas se encuentran por UUID).
        scripts.setAssetsChangedCallback([&database] { database.refresh(); });
        // Se apuntan para limpiarlos si el juego se cerro de golpe con alguno montado.
        project::cleanupDataPackJournal(project->libraryFolder() / "DataPacks.journal", project->assetsFolder());
        scripts.setDataPackJournal(project->libraryFolder() / "DataPacks.journal");
        scripts.setPhysics(&physics);
        scripts.setAudio(&audio);
        // CVars del proyecto (sin trucos en el juego) y los scripts de C++:
        // la DLL compilada al exportar, en scripts/ junto al ejecutable.
        cvar::Registry::instance().setCheatsAllowed(false);
        cvar::Registry::instance().load(project->settingsFolder() / "CVars.json");
        scripting::CppScriptSystem cpp_scripts;
        cpp_scripts.setAssetsRoot(project->assetsFolder());
        cpp_scripts.setPhysics(&physics);
        cpp_scripts.setLuaBridge(&scripts);  // toda la API de Lua desde C++
        cpp_scripts.setAssetPathResolver([&database, &project](const Uuid& uuid) -> std::string {
            const std::optional<assets::AssetInfo> info = database.find(uuid);
            if (!info || info->path.empty()) return {};
            const std::filesystem::path rel =
                info->path.is_absolute() ? info->path.lexically_relative(project->assetsFolder()) : info->path;
            const std::u8string text = rel.generic_u8string();
            return std::string(text.begin(), text.end());
        });
#if defined(_WIN32)
        {
            wchar_t exe_buffer[MAX_PATH];
            GetModuleFileNameW(nullptr, exe_buffer, MAX_PATH);
            const std::filesystem::path game_dll = std::filesystem::path(exe_buffer).parent_path() / "scripts" / "game_scripts.dll";
            std::error_code dll_error;
            if (std::filesystem::exists(game_dll, dll_error)) cpp_scripts.usePrebuilt(game_dll);
        }
#endif
        // Navegacion: ajustes del proyecto y la misma geometria que la fisica.
        navigation::NavigationSettings nav_settings;
        navigation::loadNavigationSettings(project->settingsFolder() / "Navigation.json", nav_settings);
        navigation::NavigationSystem nav;
        nav.setSettings(nav_settings);
        nav.setMeshProvider([&](ecs::Entity entity) -> const asset::ModelData* { return sync.actorModelData(entity, scene); });
        nav.setTerrainProvider([&](ecs::Entity entity) -> std::shared_ptr<const terrain::TerrainData> {
            const terrain::Terrain* comp = entity.tryGet<terrain::Terrain>();
            return comp != nullptr ? terrains.get(*comp) : nullptr;
        });
        nav.setPhysics(&physics);
        scripts.setNavigation(&nav);
        scripts.setCursorLock([&](bool locked) { window.setCursorCaptured(locked); });
        scripts.setTouchControls(&touch);
        // Acciones y contextos de entrada del proyecto (Input.getAction...).
        scripts.setInputActions(input::loadInputActions(input::inputActionsFile(project->settingsFolder())));
        std::string orientation_mode = "auto";
        scripting::ScriptSystem::ScreenHost screen_host;
        screen_host.width = [&] { return static_cast<int>(window.width()); };
        screen_host.height = [&] { return static_cast<int>(window.height()); };
        screen_host.orientation_mode = [&] { return orientation_mode; };
#if !defined(_WIN32)
        scripts.setVibrate([&](int ms) { window.vibrate(ms); });
        screen_host.set_orientation = [&](const std::string& mode) {
            // ActivityInfo.SCREEN_ORIENTATION_*
            static const std::pair<const char*, int> kModes[] = {{"auto", 10},          {"landscape", 6},
                                                                 {"portrait", 7},       {"landscape_fixed", 0},
                                                                 {"portrait_fixed", 1}};
            for (const auto& [name, value] : kModes) {
                if (mode == name) {
                    window.setOrientation(value);
                    orientation_mode = mode;
                    return true;
                }
            }
            return false;
        };
#endif
        scripts.setScreen(screen_host);
        scripts.setXr(&renderer.xr(), &xr_rig);
        // Mundos de bloques: texturas del juego y partidas en Saves/<juego>/Worlds.
        voxel::VoxelSystem voxels;
        voxels.setAssetsRoot(project->assetsFolder());
        voxels.setSaveRoot(editor::localDataFolder("Saves") / std::filesystem::path(exe_stem) / "Worlds");
        scripts.setVoxels(&voxels);
        // Lua: huesos, IK y ragdoll leen la pose de cada frame del RenderSync.
        scripts.setSkeletonHost(scripting::ScriptSystem::SkeletonHost{
        [&sync](ecs::Entity e, const std::string& bone, core::Mat4& m) {
            return e.world() != nullptr && sync.boneWorld(*e.world(), e, bone, m);
        },
        [&sync](ecs::Entity e) {
            std::vector<std::string> names;
            ecs::RenderSync::SkeletonPose pose;
            if (e.world() != nullptr && sync.skeletonPose(*e.world(), e, pose)) names = pose.names;
            return names;
        },
        [&sync](ecs::Entity e, float* scale) -> const asset::ModelData* {
            return e.world() != nullptr ? sync.skeletonData(*e.world(), e, scale) : nullptr;
        }});
        // Graphics (Lua): menu de opciones del juego. Graphics.save() lo
        // guarda para el jugador (se lee al volver a abrir el juego).
        editor::RendererGraphicsHost graphics_host(renderer, window.handle(), nullptr);
        graphics_host.setWindowState(static_cast<editor::WindowMode>(window_mode), window_width, window_height);
        graphics_host.setSaver([&](std::string& error) {
            if (!editor::saveGraphicsIni(player_graphics, renderer)) {
                error = "no se pudo escribir " + player_graphics.string();
                return false;
            }
            std::ofstream out(player_graphics, std::ios::app);
            out << "window_mode=" << static_cast<int>(graphics_host.windowMode()) << "\n";
            out << "window_width=" << graphics_host.windowWidth() << "\n";
            out << "window_height=" << graphics_host.windowHeight() << "\n";
            return static_cast<bool>(out);
        });
        scripts.setGraphics(&graphics_host);
        voxels.setPhysics(&physics);
        cinema::CinematicSystem cinematics;
        physics::ParticleWorld particles;
        // Liquidos (fluid::FluidSystem): Lua los usa con Fluid.*.
        fluid::FluidSystem fluids;
        fluid::setActiveSystem(&fluids);
        fluids.setTerrainStore(&terrains);
        fluids.setBoundsProvider([&sync](ecs::Entity e, core::Vec3& min, core::Vec3& max) {
            const int actor = sync.actorIndex(e);
            return actor >= 0 && sync.actorLocalBounds(static_cast<std::uint32_t>(actor), min, max);
        });
        // VFX Graph (particulas en la GPU), 2D y repeticiones.
        vfx::VfxSystem vfx_system;
        vfx::setActiveSystem(&vfx_system);
        vfx_system.setAssetsRoot(project->assetsFolder());
        vfx_system.setGraphResolver([&database](const Uuid& uuid) -> std::filesystem::path {
            const auto info = database.find(uuid);
            return info ? info->path : std::filesystem::path{};
        });
        vfx_system.setModelProvider([&asset_manager](const Uuid& uuid) { return asset_manager.loadModel(uuid); });
        twod::System2D twod_system;
        twod_system.setAssetsRoot(project->assetsFolder());
        twod::loadSortingLayers(project->settingsFolder() / "SortingLayers.json");
        replay::ReplaySystem replays;
        replay::setActiveSystem(&replays);
        replays.setFolder(editor::localDataFolder("Saves") / exe_stem / "replays");
        replays.setAudio(&audio);
        replays.setInput(&input);
        audio::setOneShotListener([&replays](const std::string& clip, const core::Vec3& position, float volume, bool spatial) {
            if (replays.recording()) replays.notifyOneShot(clip, position, volume, spatial);
        });
        // Steam (Configuraciones de compilacion > Steam): sin Steam el juego sigue igual.
        {
            const std::string app = readIniValue(game / "game.ini", "steam_app_id");
            if (!app.empty()) {
                const std::uint32_t app_id = static_cast<std::uint32_t>(std::strtoul(app.c_str(), nullptr, 10));
                if (readIniValue(game / "game.ini", "steam_restart") == "1" &&
                    platform::Steam::restartAppIfNecessary(app_id, root)) {
                    return 0;  // Steam abre el juego de nuevo
                }
                if (!platform::Steam::instance().init(app_id, root)) {
                    std::cerr << "[Steam] " << platform::Steam::instance().error() << "\n";
                }
            }
        }
        ui::UiSystem game_ui;
        ecs::World world;
        // Un dedo sobre un boton de la interfaz la pulsa aunque caiga en el joystick.
        touch.setUiHitTest([&](float x, float y) { return game_ui.interactiveAt(world, x, y); });

        // Escena inicial: la de game.ini, o la inicial del proyecto.
        std::filesystem::path scene_file;
        const std::string scene_setting = readIniValue(game / "game.ini", "scene");
        if (!scene_setting.empty()) scene_file = project->assetsFolder() / std::filesystem::path(std::u8string(scene_setting.begin(), scene_setting.end()));
        if (scene_file.empty() && project->startup_scene.valid()) {
            if (const auto info = database.find(project->startup_scene)) scene_file = info->path;
        }

        // Datos guardados de los scripts (Prefs), por juego.
        scripts.setPrefsFile(editor::localDataFolder("Saves") / std::filesystem::path(exe_stem).concat(".prefs"));
        // Partidas (Save.save): %APPDATA%/<juego>/saves en Windows.
        {
            const std::u8string game_u8 = exe_stem.u8string();
            scripts.setSaveFolder(gameplay::SaveSystem::defaultFolder(
                std::string(game_u8.begin(), game_u8.end()), editor::localDataFolder("Saves") / exe_stem / "saves"));
        }
        // Textos en varios idiomas (ProjectSettings/Localization).
        {
            std::string loc_error;
            if (!gameplay::localization().load(project->settingsFolder() / "Localization", &loc_error)) {
                std::cerr << "[Localizacion] " << loc_error << "\n";
            }
        }

        const auto main_camera = [&](core::Vec3& position, core::Vec3& forward) {
            position = scene.camera().position();
            forward = scene.camera().forward();
            world.forEachDepthFirst([&](ecs::Entity e) {
                if (const ecs::Camera* c = e.tryGet<ecs::Camera>(); c != nullptr && c->is_main && e.activeInHierarchy()) {
                    position = e.worldPosition();
                    forward = e.forward();
                }
            });
        };

        struct OrbitView {
            bool fitted = false;
            core::Vec3 target{};
            float distance = 10.0f;
            float min_distance = 1.0f;
            float max_distance = 1000.0f;
            float yaw = 0.6f;     // radianes
            float pitch = 0.45f;  // radianes, por encima del horizonte
            float idle = 0.0f;    // segundos desde que el jugador la movio
            float hint = 0.0f;    // segundos que lleva activa (el aviso se desvanece)
        };
        OrbitView orbit;
        editor::ProfilerOverlay profiler_overlay;

        // --- Carga de escenas por etapas ---
        // La ventana sigue viva (cada frame se dibuja la pantalla de carga con
        // la etapa y el porcentaje) y lo pesado, leer los modelos y
        // descomprimir sus texturas, va en otro hilo. Etapas:
        //   Scene    leer el .crscene (rapido) y lanzar el hilo de los modelos
        //   Models   el hilo lee cada modelo que se va a dibujar
        //   Upload   subirlos a la GPU (un frame con el texto antes: bloquea)
        //   Systems  fisica, navegacion, bloques, audio y scripts
        struct SceneLoad {
            enum class Stage { Idle, Scene, Models, Upload, Systems, Done };
            Stage stage = Stage::Idle;
            std::filesystem::path file;
            std::vector<Uuid> models;
            std::future<void> worker;
            std::atomic<std::size_t> models_done{0};
            std::mutex mutex;
            std::string current;  // modelo que se esta leyendo
            int frames = 0;       // frames en la etapa (para dibujar su texto antes de bloquear)
        };
        SceneLoad load;
        const auto begin_load = [&](const std::filesystem::path& file) {
            load.stage = SceneLoad::Stage::Scene;
            load.file = file;
            load.models.clear();
            load.models_done = 0;
            load.frames = 0;
            orbit.fitted = false;
        };
        const auto scene_loading = [&] { return load.stage != SceneLoad::Stage::Idle && load.stage != SceneLoad::Stage::Done; };
        ecs::RenderSync::Options sync_options;
        sync_options.apply_main_camera = true;

        // Un paso por frame. Devuelve la fraccion y el texto para la pantalla.
        const auto step_load = [&](float& fraction, std::string& text) {
            using Stage = SceneLoad::Stage;
            switch (load.stage) {
                case Stage::Idle:
                case Stage::Done: break;
                case Stage::Scene: {
                    scripts.stop();
                    cpp_scripts.stop();
                    audio.stop();
                    game_ui.reset();
                    physics.stop();
                    particles.clear();
                    fluids.clear();
                    vfx_system.clear();
                    twod_system.stop();
                    if (replays.playing()) replays.stop(world);
                    if (replays.recording()) replays.stopRecording();
                    nav.clear();
                    voxels.stop();
                    std::string error;
                    if (load.file.empty() || !ecs::loadScene(world, load.file, &error)) {
                        std::cerr << "[Juego] No se pudo abrir la escena " << load.file.string() << " " << error << "\n";
                    }
                    // Iluminacion horneada de la escena (<escena>.crbake).
                    {
                        gfx::BakedLighting baked;
                        gfx::LightingMode mode = gfx::LightingMode::Realtime;
                        const std::filesystem::path bake_file = lighting::bakedLightingFile(load.file);
                        if (!load.file.empty() && std::filesystem::exists(bake_file)) {
                            std::string bake_error;
                            if (!lighting::loadBakedLighting(bake_file, baked, mode, &bake_error)) std::cerr << "[Juego] " << bake_error << "\n";
                        }
                        renderer.setBakedLighting(std::move(baked));
                        renderer.setLightingMode(mode);
                    }
                    // Instancias de prefab guardadas con una revision vieja: al dia.
                    ecs::syncOutdatedInstances(world, [&](const Uuid& id) {
                        const auto info = database.find(id);
                        return info && info->type == assets::AssetType::Prefab ? ecs::readPrefabFile(info->path) : std::string{};
                    });
                    renderer.invalidateHistory();
                    // La escena puede estar guardada lejos del origen.
                    renderer.setWorldOrigin(world.origin().x, world.origin().y, world.origin().z);
                    const std::u8string stem = load.file.stem().u8string();
                    scripts.setSceneName(std::string(stem.begin(), stem.end()));
                    // Los modelos que se van a dibujar, una vez cada uno.
                    std::unordered_set<Uuid> seen;
                    world.forEachDepthFirst([&](ecs::Entity e) {
                        const ecs::MeshRenderer* r = e.tryGet<ecs::MeshRenderer>();
                        if (r == nullptr || r->mesh || !r->model.valid() || !e.activeInHierarchy()) return;
                        if (const ecs::EntityInfo* info = e.tryGet<ecs::EntityInfo>(); info != nullptr && info->static_batched) return;
                        if (seen.insert(r->model.uuid).second) load.models.push_back(r->model.uuid);
                    });
                    std::cout << "[Juego] Cargando " << std::string(stem.begin(), stem.end()) << ": " << load.models.size()
                              << " modelos\n";
                    // Mientras el hilo lee, nadie mas toca el AssetManager: la
                    // fisica y los scripts estan parados y no se sincroniza el render.
                    load.worker = std::async(std::launch::async, [&] {
                        for (const Uuid& id : load.models) {
                            {
                                const auto info = database.find(id);
                                std::lock_guard lock(load.mutex);
                                load.current = info ? info->name : std::string{};
                            }
                            asset_manager.loadModel(id);
                            ++load.models_done;
                        }
                    });
                    load.stage = Stage::Models;
                    break;
                }
                case Stage::Models:
                    if (load.worker.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                        load.worker.get();  // relanza si el hilo fallo
                        load.stage = Stage::Upload;
                        load.frames = 0;
                    }
                    break;
                case Stage::Upload: {
                    // Primero un frame con "Subiendo a la GPU" en pantalla.
                    if (load.frames++ == 0) break;
                    // Con el streaming cada sincronizacion sube unos milisegundos:
                    // la pantalla de carga sigue (en tandas de 250 ms) hasta que
                    // todo este en la GPU; despues, lo que se instancie en el
                    // juego llega poco a poco sin tirones.
                    const auto upload_start = std::chrono::steady_clock::now();
                    do {
                        sync.sync(world, scene, renderer, 0.0f, sync_options);
                    } while (renderer.uploadedModelCount() < scene.models().size() &&
                             std::chrono::steady_clock::now() - upload_start < std::chrono::milliseconds(250));
                    if (renderer.uploadedModelCount() < scene.models().size()) break;
                    load.stage = Stage::Systems;
                    load.frames = 0;
                    break;
                }
                case Stage::Systems: {
                    if (load.frames++ == 0) break;
                    physics.start(world);
                    twod_system.start(world);  // fisica 2D del nivel
                    // La malla lista antes de que empiecen los scripts.
                    nav.waitForBuild(world, 20.0f);
                    // Los bloques de alrededor listos antes de empezar (no se cae del mundo).
                    voxels.start(world);
                    if (voxels.active()) {
                        core::Vec3 position, forward;
                        main_camera(position, forward);
                        voxels.waitUntilReady(position, 2, 20.0f);
                    }
                    audio.start(world);
                    scripts.start(world);
                    if (!cpp_scripts.dll().empty()) cpp_scripts.start(world);
                    load.stage = Stage::Done;
                    std::cout << "[Juego] Escena lista\n";
                    break;
                }
            }
            switch (load.stage) {
                case Stage::Scene: fraction = 0.02f; text = "Leyendo la escena"; break;
                case Stage::Models: {
                    const std::size_t total = std::max<std::size_t>(load.models.size(), 1);
                    const std::size_t done = std::min(load.models_done.load(), total);
                    fraction = 0.05f + 0.7f * static_cast<float>(done) / static_cast<float>(total);
                    std::lock_guard lock(load.mutex);
                    text = "Cargando modelos (" + std::to_string(std::min(done + 1, total)) + "/" + std::to_string(total) + ")";
                    if (!load.current.empty()) text += ": " + load.current;
                    break;
                }
                case Stage::Upload: fraction = 0.8f; text = "Subiendo modelos y texturas a la GPU"; break;
                case Stage::Systems: fraction = 0.9f; text = "Preparando fisica, navegacion y scripts"; break;
                default: fraction = 1.0f; text.clear(); break;
            }
        };

        // --- Camara orbital por defecto (como el DefaultPawn de Unreal) ---
        // Si la escena no tiene ninguna Camera activa, el juego no se queda
        // mirando al vacio desde el origen: una vista orbita alrededor de todo
        // lo que se dibuja. Gira sola; arrastrar con el raton la mueve (y la
        // deja quieta un rato) y la rueda acerca. En cuanto un script crea una
        // Camera, manda esa.
        const auto has_camera = [&] {
            bool found = false;
            world.forEachDepthFirst([&](ecs::Entity e) {
                if (!found && e.has<ecs::Camera>() && e.activeInHierarchy()) found = true;
            });
            return found;
        };
        // Encuadra lo que se dibuja: caja de las esferas de los actores.
        const auto fit_orbit = [&] {
            core::Vec3 low{1e30f, 1e30f, 1e30f};
            core::Vec3 high{-1e30f, -1e30f, -1e30f};
            for (const scene::Actor& actor : scene.actors()) {
                if (actor.shadows_only || actor.bounds_radius <= 0.0f || !std::isfinite(actor.bounds_radius)) continue;
                const core::Vec3& c = actor.bounds_center;
                const float r = actor.bounds_radius;
                low = core::Vec3{std::min(low.x, c.x - r), std::min(low.y, c.y - r), std::min(low.z, c.z - r)};
                high = core::Vec3{std::max(high.x, c.x + r), std::max(high.y, c.y + r), std::max(high.z, c.z + r)};
            }
            float radius = 5.0f;
            orbit.target = core::Vec3{0.0f, 0.0f, 0.0f};
            if (low.x <= high.x) {
                orbit.target = (low + high) * 0.5f;
                radius = std::max(core::length(high - low) * 0.5f, 0.5f);
            }
            const float half_fov = std::max(scene.camera().fovY() * 0.5f, 0.2f);
            orbit.distance = std::clamp(radius / std::sin(half_fov) * 1.05f, 1.5f, 20000.0f);
            orbit.min_distance = std::max(radius * 0.05f, 0.3f);
            orbit.max_distance = orbit.distance * 4.0f;
            orbit.idle = 1e9f;
            orbit.hint = 0.0f;
            orbit.fitted = true;
        };
        const auto update_orbit = [&](float dt, const ImVec2& display) {
            if (!orbit.fitted) fit_orbit();
            const ImGuiIO& io = ImGui::GetIO();
            const bool mouse_free = !io.WantCaptureMouse;
            orbit.idle += dt;
            orbit.hint += dt;
            if (mouse_free && (ImGui::IsMouseDown(ImGuiMouseButton_Left) || ImGui::IsMouseDown(ImGuiMouseButton_Right))) {
                orbit.yaw -= io.MouseDelta.x * 0.006f;
                orbit.pitch = std::clamp(orbit.pitch + io.MouseDelta.y * 0.006f, -1.2f, 1.45f);
                if (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f) orbit.idle = 0.0f;
            }
            if (mouse_free && io.MouseWheel != 0.0f) {
                orbit.distance = std::clamp(orbit.distance * std::pow(0.88f, io.MouseWheel), orbit.min_distance, orbit.max_distance);
                orbit.idle = 0.0f;
            }
            // Gira sola si nadie la toca (arranca suave tras soltarla).
            if (orbit.idle > 3.0f) orbit.yaw += dt * 0.25f * std::clamp(orbit.idle - 3.0f, 0.0f, 1.0f);
            const float c = std::cos(orbit.pitch);
            const core::Vec3 offset{std::sin(orbit.yaw) * c, std::sin(orbit.pitch), std::cos(orbit.yaw) * c};
            scene.placeCamera(orbit.target + offset * orbit.distance, orbit.target);

            // Aviso los primeros segundos.
            if (orbit.hint < 6.0f) {
                const float alpha = std::clamp(6.0f - orbit.hint, 0.0f, 1.0f);
                const char* hint = "Escena sin camara: vista orbital (arrastra para girar, rueda para acercar)";
                const ImVec2 size = ImGui::CalcTextSize(hint);
                const ImVec2 at{(display.x - size.x) * 0.5f, display.y - size.y - 24.0f};
                ImDrawList* bg = ImGui::GetForegroundDrawList();
                bg->AddRectFilled(ImVec2(at.x - 12.0f, at.y - 6.0f), ImVec2(at.x + size.x + 12.0f, at.y + size.y + 6.0f),
                                  IM_COL32(13, 15, 20, static_cast<int>(170 * alpha)), 6.0f);
                bg->AddText(at, IM_COL32(220, 225, 235, static_cast<int>(255 * alpha)), hint);
            }
        };

        // --- Controles tactiles en pantalla (joystick y botones) ---
        const bool touch_platform =
#if defined(_WIN32)
            false;
#else
            true;
#endif
        const auto draw_touch = [&] {
            const dm::TouchLayout& layout = touch.layout();
            if (!layout.enabled || (!touch_platform && !input.touchScreen())) return;
            ImDrawList* fg = ImGui::GetForegroundDrawList();
            const auto alpha = [&](float k) { return static_cast<int>(std::clamp(layout.opacity * k, 0.0f, 1.0f) * 255.0f); };
            if (layout.joystick) {
                const dm::TouchControls::StickView s = touch.stick();
                const ImVec2 base{s.base_x, s.base_y};
                fg->AddCircleFilled(base, s.radius, IM_COL32(15, 17, 22, alpha(0.45f)), 48);
                fg->AddCircle(base, s.radius, IM_COL32(255, 255, 255, alpha(0.55f)), 48, 2.0f);
                fg->AddCircleFilled(ImVec2(s.knob_x, s.knob_y), s.radius * 0.42f,
                                    IM_COL32(255, 255, 255, alpha(s.active ? 0.95f : 0.6f)), 32);
            }
            for (std::size_t i = 0; i < layout.buttons.size(); ++i) {
                const dm::TouchButton& b = layout.buttons[i];
                if (!b.visible) continue;
                const ImVec2 c{touch.buttonCenterX(b), touch.buttonCenterY(b)};
                const float r = touch.buttonRadius(b);
                const bool down = touch.buttonDown(i);
                fg->AddCircleFilled(c, r, down ? IM_COL32(90, 150, 255, alpha(0.9f)) : IM_COL32(15, 17, 22, alpha(0.5f)), 40);
                fg->AddCircle(c, r, IM_COL32(255, 255, 255, alpha(0.7f)), 40, 2.0f);
                const ImVec2 size = ImGui::CalcTextSize(b.label.c_str());
                fg->AddText(ImVec2(c.x - size.x * 0.5f, c.y - size.y * 0.5f), IM_COL32(255, 255, 255, alpha(1.4f)), b.label.c_str());
            }
        };

        // --- Banner mientras carga ---
        const std::filesystem::path banner = game / "banner.png";
        constexpr float kBannerSeconds = 3.0f;
        float banner_time = 0.0f;
        float banner_wait = 0.0f;
        bool loaded = false;
        const dm::Input idle_input;
        core::Clock clock;
        float perf_seconds = 0.0f;
        int perf_frames = 0;
        prof::setLog([](const std::string& line) { std::cout << line << std::endl; });
        while (!quit) {
            prof::beginFrame();
            const float clock_dt = clock.tick();  // sin recortar (el Profiler mide los frames reales)
            const float dt = std::min(clock_dt, 0.1f);
            touch_time += clock_dt;
            window.pumpEvents();
            // Lo que Lua apago con un dedo encima se suelta.
            synthesized.clear();
            touch.update(synthesized);
            for (dm::Event& s : synthesized) feed(s);
            touch.apply(input);  // el joystick virtual a los ejes
            // VR: espera al casco, poses de la cabeza y los mandos, y sus
            // botones a Input (Input Actions "XR Right Trigger"...).
            if (vr) {
                renderer.xr().beginFrame();
                renderer.xr().applyToInput(input);
                if (renderer.xr().exitRequested()) quit = true;
            }
            renderer.applyPendingResize();
            imgui.beginFrame();
            imgui.updateThumbnails();  // sube las imagenes ya decodificadas (banner, UI)
            const ImVec2 display = ImGui::GetIO().DisplaySize;

            const bool showing_banner = banner_time < kBannerSeconds;
            if (showing_banner) {
                ImVec2 probe{};
                if (imgui.image(banner, &probe) != 0 || banner_wait > 1.0f) {
                    banner_time += dt;
                } else {
                    banner_wait += dt;
                }
                ImDrawList* fg = ImGui::GetForegroundDrawList();
                fg->AddRectFilled(ImVec2(0, 0), display, IM_COL32(13, 15, 20, 255));  // el fondo del banner
                ImVec2 image_size{};
                if (const ImTextureID texture = imgui.image(banner, &image_size); texture != 0 && image_size.y > 0) {
                    const float fade_in = std::clamp(banner_time / 0.5f, 0.0f, 1.0f);
                    const float fade_out = std::clamp((kBannerSeconds - banner_time) / 0.6f, 0.0f, 1.0f);
                    const float alpha = std::min(fade_in, fade_out);
                    const float aspect = image_size.x / image_size.y;
                    ImVec2 fit = display;
                    if (display.x / std::max(display.y, 1.0f) > aspect) fit.x = display.y * aspect;
                    else fit.y = display.x / aspect;
                    const ImVec2 a{(display.x - fit.x) * 0.5f, (display.y - fit.y) * 0.5f};
                    fg->AddImage(texture, a, ImVec2(a.x + fit.x, a.y + fit.y), ImVec2(0, 0), ImVec2(1, 1),
                                 IM_COL32(255, 255, 255, static_cast<int>(alpha * 255.0f)));
                }
            }
            // Se empieza a cargar con el banner ya en pantalla (segundo frame).
            if (!loaded && banner_time > 0.05f) {
                loaded = true;
                begin_load(scene_file);
            }
            float load_fraction = 1.0f;
            std::string load_text;
            if (scene_loading()) step_load(load_fraction, load_text);
            if (scene_loading() && !showing_banner) draw_loading(display, load_fraction, load_text);
            const bool running = loaded && !scene_loading() && !showing_banner;
            if (running && vr) {
                xr_rig.update(world, renderer.xr());  // camara y mandos antes de la logica
            } else {
                xr_rig.reset();
            }
            if (running) {
                int steps = 0;
                {
                    CR_PROFILE_SCOPE("Fisica");
                    xr_rig.updateCollisions(world);  // el Jugador VR no choca con lo que se coge
                    steps = physics.update(world, dt, true);
                }
                particles.setViewer(scene.camera().position(), scene.camera().forward());
                particles.update(world, dt, &physics);
                fluids.update(world, dt, true, &physics, renderer);
                twod_system.update(world, dt, twod::System2D::Mode::Play);
                fire::updateFires(world, dt, fire::FireMode::Play, &terrains);
                {
                    CR_PROFILE_SCOPE("Navegacion");
                    nav.update(world, dt, true, nav_settings.runtime_generation);
                }
                scripts.setInput(game_ui.typing() ? nullptr : &input);
                if (!game_ui.typing()) {
                    const auto down = [&](dm::Key a, dm::Key b) { return input.isKeyDown(a) || input.isKeyDown(b); };
                    physics.driveVehiclesWithKeyboard(world, down(dm::Key::W, dm::Key::Up), down(dm::Key::S, dm::Key::Down),
                                                      down(dm::Key::A, dm::Key::Left), down(dm::Key::D, dm::Key::Right),
                                                      input.isKeyDown(dm::Key::Space));
                    physics::PhysicsSystem::CharacterKeys keys;
                    keys.forward = down(dm::Key::W, dm::Key::Up);
                    keys.back = down(dm::Key::S, dm::Key::Down);
                    keys.left = down(dm::Key::A, dm::Key::Left);
                    keys.right = down(dm::Key::D, dm::Key::Right);
                    keys.run = down(dm::Key::LeftShift, dm::Key::RightShift);
                    keys.jump = input.isKeyDown(dm::Key::Space);
                    keys.crouch = down(dm::Key::C, dm::Key::LeftControl);
                    physics.driveCharactersWithKeyboard(world, keys);
                }
                {
                    CR_PROFILE_SCOPE("Scripts Lua");
                    scripts.fixedUpdate(world, physics_settings.fixed_step, steps);
                    scripts.update(world, dt);
                }
                cpp_scripts.setInput(game_ui.typing() ? nullptr : &input);
                {
                    CR_PROFILE_SCOPE("Scripts C++");
                    cpp_scripts.fixedUpdate(world, physics_settings.fixed_step, steps);
                    cpp_scripts.update(world, dt);
                }
                cinematics.update(world, dt, true);
                replays.update(world, dt);  // despues de los scripts
                if (platform::Steam::instance().available()) platform::Steam::instance().update();
                // Origen flotante: si la camara se alejo mucho del (0,0,0), todo
                // se desplaza para que vuelva cerca (nada tiembla a 100 km).
                {
                    core::Vec3 focus, look;
                    main_camera(focus, look);
                    if (const std::optional<core::Vec3> offset = ecs::updateFloatingOrigin(world, focus)) {
                        physics.shiftOrigin(*offset);
                        particles.shiftOrigin(*offset);
                        fluids.shiftOrigin(*offset);
                        vfx_system.shiftOrigin(*offset);
                        nav.shiftOrigin(*offset);
                        voxels.shiftOrigin(*offset);
                        cinematics.shiftOrigin(*offset);
                        audio.shiftOrigin(*offset);
                        scripts.shiftOrigin(*offset);
                        cpp_scripts.shiftOrigin(*offset);
                        renderer.shiftOrigin(*offset);
                        orbit.target = orbit.target - *offset;
                        std::cout << "[Juego] Origen del mundo desplazado; origen = (" << world.origin().x << ", "
                                  << world.origin().y << ", " << world.origin().z << ")\n";
                    }
                }
                // Oyente: AudioListener o la camara principal.
                core::Vec3 position, forward;
                main_camera(position, forward);
                audio.update(world, dt, position, forward);
                voxels.update(dt, position);
                // Interfaz: toda la ventana.
                const ui::UiInput ui_input = editor::uiInputFromImGui(ImVec2(0, 0), display, true, game_ui.typing());
                game_ui.update(world, display.x, display.y, ui_input, true, static_cast<float>(ImGui::GetTime()));
                // UI en el mundo (Canvas en modo Mundo): con los rayos de las
                // manos en VR; sus paneles los dibuja el render.
                game_ui.updateWorld(world, xr_rig.uiPointers(), true, static_cast<float>(ImGui::GetTime()));
                xr_rig.setUiHits(game_ui.pointerHits());
                for (const ui::UiEvent& e : game_ui.takeEvents()) {
                    switch (e.kind) {
                        case ui::UiEvent::Kind::Click: scripts.callMethod(e.target, e.method, e.source); break;
                        case ui::UiEvent::Kind::Number: scripts.callMethod(e.target, e.method, e.number); break;
                        case ui::UiEvent::Kind::Text: scripts.callMethod(e.target, e.method, e.text); break;
                        case ui::UiEvent::Kind::Bool: scripts.callMethod(e.target, e.method, e.flag); break;
                    }
                    // Y al script de C++ del objeto (Script::on("OnJugar", ...) / onMessage).
                    switch (e.kind) {
                        case ui::UiEvent::Kind::Click: cpp_scripts.sendMessage(e.target, e.method, cpp_scripts.entityJson(e.source)); break;
                        case ui::UiEvent::Kind::Number: cpp_scripts.sendMessage(e.target, e.method, nlohmann::json(e.number).dump()); break;
                        case ui::UiEvent::Kind::Text: cpp_scripts.sendMessage(e.target, e.method, nlohmann::json(e.text).dump()); break;
                        case ui::UiEvent::Kind::Bool: cpp_scripts.sendMessage(e.target, e.method, e.flag ? "true" : "false"); break;
                    }
                }
                renderer.setWorldUi(editor::buildWorldUi(game_ui.worldCanvases(), imgui, project->assetsFolder()));
                editor::drawUiList(ImGui::GetBackgroundDrawList(), ImVec2(0, 0), game_ui.drawList(), imgui, project->assetsFolder());
                // Sin Camera en la escena: vista orbital por defecto.
                if (!has_camera()) update_orbit(dt, display);
                draw_touch();
                // Componente Profiler: FPS, CPU, GPU y memoria en una esquina.
                profiler_overlay.update(clock_dt, renderer);
                static const ecs::Profiler kDevelopmentProfiler{};  // "Mostrar FPS" de la configuracion
                const ecs::Profiler* p = editor::findProfiler(world);
                if (p == nullptr && show_fps) p = &kDevelopmentProfiler;
                if (p != nullptr) {
                    profiler_overlay.draw(ImGui::GetForegroundDrawList(), ImVec2(0, 0), display, *p,
                                          renderer.device().name());
                }
                // Scene.load(...) y Game.quit() desde Lua.
                if (const std::filesystem::path next = scripts.takeSceneRequest(); !next.empty()) begin_load(next);
                if (scripts.takeQuitRequest()) quit = true;
            }
            imgui.endFrame();

            scene.update(idle_input, dt);
            // Mientras carga no se sincroniza (el hilo usa el AssetManager):
            // se sigue viendo lo anterior, tapado por la pantalla de carga.
            if (loaded && !scene_loading()) {
                sync.sync(world, scene, renderer, running ? dt : 0.0f, sync_options);
                renderer.setParticles(particles.drawList(scene.camera().position()));
                vfx_system.update(world, running ? dt : 0.0f, true, {}, renderer);
                renderer.setSprites(twod_system.drawList(world, scene.camera().position(), scene.camera().forward()));
                voxels.syncRenderer(renderer);
            }
            // VR: los dos ojos al casco; la ventana ve lo de la cabeza.
            if (xr_rig.active()) xr_rig.renderEyes(renderer, scene);
            {
                CR_PROFILE_SCOPE("Render (CPU)");
                renderer.drawFrame(scene);
            }
            if (vr) renderer.xr().endFrame();
            input.newFrame();

            // Cada 5 s una linea de rendimiento en el log (se vuelca ya: si el
            // juego se cierra mal, queda escrita). Sirve para ver en que se va
            // el frame en el PC de un jugador.
            if (loaded && !scene_loading()) {
                perf_seconds += clock_dt;
                ++perf_frames;
                if (perf_seconds >= 5.0f) {
                    const gfx::FrameBudget& budget = renderer.frameBudget();
                    const vk::Extent2D internal = renderer.renderExtent();
                    const vk::Extent2D output = renderer.sceneExtent();
                    std::cout << "[Rendimiento] " << static_cast<int>(std::lround(perf_frames / perf_seconds))
                              << " FPS | GPU " << renderer.gpuProfiler().totalMilliseconds() << " ms (media "
                              << budget.smoothedGpuMs() << ", objetivo " << budget.targetMilliseconds() << ") | "
                              << internal.width << "x" << internal.height << " -> " << output.width << "x"
                              << output.height << " | " << scene.actors().size() << " actores, "
                              << renderer.lodActors() << " con LOD, " << renderer.culledSmallActors()
                              << " diminutos | sombras " << renderer.shadowResolution() << " (" << renderer.shadowDrawCalls()
                              << " llamadas)";
                    std::uint64_t vram_used = 0;
                    std::uint64_t vram_budget = 0;
                    if (renderer.device().videoMemory(vram_used, vram_budget)) {
                        std::cout << " | VRAM " << (vram_used >> 20) << " / " << (vram_budget >> 20) << " MB";
                    }
                    std::cout << " | pases:";
                    for (const gfx::GpuTiming& t : renderer.gpuProfiler().timings()) {
                        if (t.milliseconds >= 0.3f) std::cout << " " << t.name << " " << t.milliseconds;
                    }
                    if (budget.enabled()) {
                        std::cout << " | palancas:";
                        for (std::size_t i = 0; i < gfx::kLeverCount; ++i) {
                            std::cout << " " << static_cast<int>(budget.level(static_cast<gfx::Lever>(i)));
                        }
                    }
                    const prof::FrameSummary fs = prof::summary(300);
                    std::cout << " | frame p50 " << fs.p50_ms << " / p95 " << fs.p95_ms << " / p99 " << fs.p99_ms
                              << " ms, tirones " << fs.hitches;
                    std::cout << std::endl;
                    perf_seconds = 0.0f;
                    perf_frames = 0;
                }
            }
            prof::endFrame();
        }
        // Cierre: cada paso queda en el log con lo que tardo y, si algo se
        // atasca, un vigilante cierra el proceso (un juego siempre debe cerrarse).
        std::cout << "[Juego] Cerrando..." << std::endl;
        std::thread([] {
            std::this_thread::sleep_for(std::chrono::seconds(8));
            std::cout << "[Juego] El cierre tardo mas de 8 s: se termina el proceso" << std::endl;
            std::cerr.flush();
#ifdef _WIN32
            TerminateProcess(GetCurrentProcess(), 0);
#else
            std::_Exit(0);
#endif
        }).detach();
        const auto paso = [](const char* nombre, auto&& fn) {
            if (std::getenv("CRAMION_TRACE_SHUTDOWN") != nullptr) std::cout << "[Juego] Cierre: " << nombre << "..." << std::endl;
            const auto t0 = std::chrono::steady_clock::now();
            fn();
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            if (ms > 50.0) std::cout << "[Juego] Cierre: " << nombre << " " << static_cast<int>(ms) << " ms" << std::endl;
        };
        // Cerrado a media carga: el hilo de los modelos termina antes de nada.
        paso("carga", [&] { if (load.worker.valid()) load.worker.wait(); });
        paso("Lua", [&] { scripts.stop(); });
        paso("C++", [&] { cpp_scripts.stop(); });
        paso("datapacks", [&] { scripts.unmountDataPacks(); });  // lo montado con DataPack.load sale de los assets del juego
        paso("red", [&] { scripts.shutdownNetwork(); });  // avisa a los demas jugadores
        paso("audio", [&] { audio.stop(); });
        paso("fisica", [&] { physics.stop(); });
        paso("voxeles", [&] { voxels.stop(); });  // guarda el mundo con nombre
        paso("GPU", [&] { renderer.waitIdle(); });
        paso("escena", [&] { sync.reset(scene); });
        paso("interfaz", [&] { imgui.shutdown(); });
        paso("render", [&] { renderer.shutdown(); });
        std::cout << "[Juego] Cerrado" << std::endl;
    } catch (const std::exception& e) {
        std::string message = e.what();
        std::cerr << "[Juego] Error: " << message << "\n";
        // Sin memoria de video: se explica en vez del codigo de Vulkan.
        if (message.find("OutOfDeviceMemory") != std::string::npos || message.find("OutOfHostMemory") != std::string::npos) {
            message = "La tarjeta grafica se quedo sin memoria de video al cargar la escena.\n\n"
                      "La escena tiene demasiados modelos o texturas para esta GPU. Prueba a bajar la resolucion "
                      "de las texturas, usar menos modelos de alta resolucion (escaneos) o partir la escena.\n\n"
                      "Detalle: " + std::string(e.what());
        }
        showError(message);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

#if defined(_WIN32)
int main() { return runPlayer(); }
#else
// NativeActivity: la glue llama aqui en su propio hilo.
void android_main(android_app* app) {
    using namespace cramion;
    dm::Window::setApp(app);
    android::setAssetManager(app->activity->assetManager);
    net::setJavaVM(app->activity->vm);
    const std::filesystem::path data = app->activity->internalDataPath != nullptr ? app->activity->internalDataPath : "/data/local/tmp";
    android::setDataRoot(data);
    // temp_directory_path() (caches de modelos) dentro de la app.
    std::error_code e;
    std::filesystem::create_directories(data / "tmp", e);
    setenv("TMPDIR", (data / "tmp").c_str(), 1);
    runPlayer();
    std::cout << std::endl;
    // El proceso puede seguir vivo con la actividad cerrada: sin esto la
    // proxima vez arrancaria con el estado estatico de esta. _exit y no exit:
    // los destructores estaticos con hilos del driver aun vivos (Mali)
    // abortaban el proceso al salir.
    std::fflush(nullptr);
    _exit(0);
}
#endif
