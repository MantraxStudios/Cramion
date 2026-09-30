#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "GraphicsConfig.h"

#include <CramionFX/vk/FrameBudget.h>
#include <CramionFX/vk/GraphicsSettings.h>
#include <CramionFX/vk/VulkanRenderer.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <fstream>
#include <set>

namespace cramion::editor {

namespace {

using scripting::GraphicsOption;
using scripting::GraphicsValue;

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Nombres de Lua de los enums (en el orden del enum).
constexpr std::array<const char*, 5> kUpscalerNames = {"off", "taa", "fsr1", "fsr3", "dlss"};
constexpr std::array<const char*, 6> kResolutionNames = {"native", "quality", "balanced",
                                                         "performance", "ultra_performance", "custom"};
constexpr std::array<const char*, 3> kWindowModeNames = {"maximized", "fullscreen", "windowed"};

// Calidad de texturas y sombras por nombre: tamano (0 = segun el PC).
struct SizeLevel {
    const char* name;
    const char* spanish;
    int size;
};
constexpr std::array<SizeLevel, 6> kTextureLevels = {{{"auto", "auto", 0},
                                                      {"low", "baja", 1024},
                                                      {"medium", "media", 2048},
                                                      {"high", "alta", 4096},
                                                      {"ultra", "ultra", 8192},
                                                      {"max", "maxima", 16384}}};
constexpr std::array<SizeLevel, 5> kShadowLevels = {{{"auto", "auto", 0},
                                                     {"low", "baja", 1024},
                                                     {"medium", "media", 2048},
                                                     {"high", "alta", 4096},
                                                     {"ultra", "ultra", 6144}}};

template <std::size_t N>
std::vector<std::string> names(const std::array<const char*, N>& list) {
    return std::vector<std::string>(list.begin(), list.end());
}

template <std::size_t N>
std::vector<std::string> levelNames(const std::array<SizeLevel, N>& list) {
    std::vector<std::string> out;
    for (const SizeLevel& l : list) out.emplace_back(l.name);
    return out;
}

template <std::size_t N>
int indexOf(const std::array<const char*, N>& list, const std::string& value) {
    const std::string v = lower(value);
    for (std::size_t i = 0; i < N; ++i) {
        if (v == list[i]) return static_cast<int>(i);
    }
    return -1;
}

// El nivel cuyo tamano es exactamente `size`, o "custom".
template <std::size_t N>
std::string levelOf(const std::array<SizeLevel, N>& list, int size) {
    for (const SizeLevel& l : list) {
        if (l.size == size) return l.name;
    }
    return "custom";
}

template <std::size_t N>
bool sizeOfLevel(const std::array<SizeLevel, N>& list, const std::string& value, int& size) {
    const std::string v = lower(value);
    for (const SizeLevel& l : list) {
        if (v == l.name || v == l.spanish) {
            size = l.size;
            return true;
        }
    }
    return false;
}

bool asBool(const GraphicsValue& v, bool& out) {
    if (const bool* b = std::get_if<bool>(&v)) {
        out = *b;
        return true;
    }
    if (const double* d = std::get_if<double>(&v)) {
        out = *d != 0.0;
        return true;
    }
    return false;
}

bool asNumber(const GraphicsValue& v, double& out) {
    if (const double* d = std::get_if<double>(&v)) {
        out = *d;
        return std::isfinite(out);
    }
    if (const bool* b = std::get_if<bool>(&v)) {
        out = *b ? 1.0 : 0.0;
        return true;
    }
    return false;
}

bool asText(const GraphicsValue& v, std::string& out) {
    if (const std::string* s = std::get_if<std::string>(&v)) {
        out = *s;
        return true;
    }
    return false;
}

}  // namespace

// -----------------------------------------------------------------------------
// Graphics.ini
// -----------------------------------------------------------------------------

void loadGraphicsIni(const std::filesystem::path& file, gfx::VulkanRenderer& renderer) {
    std::ifstream in(file);
    if (!in) return;
    gfx::GraphicsSettings g = renderer.graphicsSettings();
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();  // escrito en Windows (CRLF), leido en Android
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq);
        const float value = std::strtof(line.c_str() + eq + 1, nullptr);
        if (key == "upscaler") g.upscaler = static_cast<gfx::Upscaler>(std::clamp(static_cast<int>(value), 0, 4));
        if (key == "quality") g.quality = static_cast<gfx::UpscaleQuality>(std::clamp(static_cast<int>(value), 0, 5));
        if (key == "custom_scale") g.custom_scale = std::clamp(value, 0.25f, 1.0f);
        if (key == "sharpness") g.sharpness = std::clamp(value, 0.0f, 1.0f);
        if (key == "vsync") g.vsync = value != 0.0f;
        if (key == "adaptive") g.adaptive = value != 0.0f;
        if (key == "target_fps") g.target_fps = std::clamp(value, 15.0f, 360.0f);
        if (key == "shadow_resolution") g.shadow_resolution = std::clamp(static_cast<int>(value), 0, 8192);
        if (key == "texture_max_size") g.texture_max_size = std::clamp(static_cast<int>(value), 0, 16384);
        if (key == "shadows") renderer.setShadowsEnabled(value != 0.0f);
        if (key == "ray_tracing" && renderer.rayTracingSupported()) renderer.setRayTracingEnabled(value != 0.0f);
        if (key == "reflection_probe") renderer.setReflectionProbeEnabled(value != 0.0f);
        if (key == "occlusion_culling") renderer.setOcclusionCullingEnabled(value != 0.0f);
    }
    renderer.setGraphicsSettings(g);
}

bool saveGraphicsIni(const std::filesystem::path& file, const gfx::VulkanRenderer& renderer) {
    std::error_code error;
    std::filesystem::create_directories(file.parent_path(), error);
    std::ofstream out(file, std::ios::trunc);
    if (!out) return false;
    const gfx::GraphicsSettings& g = renderer.graphicsSettings();
    out << "upscaler=" << static_cast<int>(g.upscaler) << "\n";
    out << "quality=" << static_cast<int>(g.quality) << "\n";
    out << "custom_scale=" << g.custom_scale << "\n";
    out << "sharpness=" << g.sharpness << "\n";
    out << "vsync=" << (g.vsync ? 1 : 0) << "\n";
    out << "adaptive=" << (g.adaptive ? 1 : 0) << "\n";
    out << "target_fps=" << g.target_fps << "\n";
    out << "shadow_resolution=" << g.shadow_resolution << "\n";
    out << "texture_max_size=" << g.texture_max_size << "\n";
    out << "shadows=" << (renderer.shadowsEnabled() ? 1 : 0) << "\n";
    out << "ray_tracing=" << (renderer.rayTracingEnabled() ? 1 : 0) << "\n";
    out << "reflection_probe=" << (renderer.reflectionProbeEnabled() ? 1 : 0) << "\n";
    out << "occlusion_culling=" << (renderer.occlusionCullingEnabled() ? 1 : 0) << "\n";
    return static_cast<bool>(out);
}

bool readWindowSettings(const std::filesystem::path& file, WindowMode& mode, int& width, int& height) {
    std::ifstream in(file);
    if (!in) return false;
    bool found = false;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();  // escrito en Windows (CRLF), leido en Android
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq);
        const int value = std::atoi(line.c_str() + eq + 1);
        if (key == "window_mode") {
            mode = static_cast<WindowMode>(std::clamp(value, 0, 2));
            found = true;
        }
        if (key == "window_width" && value >= 320) width = std::min(value, 16384);
        if (key == "window_height" && value >= 240) height = std::min(value, 16384);
    }
    return found;
}

// -----------------------------------------------------------------------------
// Calidades rapidas
// -----------------------------------------------------------------------------

const std::vector<std::string>& qualityPresetNames() {
    static const std::vector<std::string> kNames = {"Baja", "Media", "Alta", "Ultra"};
    return kNames;
}

void applyQualityPreset(gfx::VulkanRenderer& renderer, int level) {
    level = std::clamp(level, 0, 3);
    gfx::GraphicsSettings g = renderer.graphicsSettings();
    // Baja/Media: escalado a menos resolucion; Alta/Ultra: casi nativa con TAA.
    static constexpr gfx::UpscaleQuality kQuality[] = {gfx::UpscaleQuality::Performance, gfx::UpscaleQuality::Balanced,
                                                       gfx::UpscaleQuality::Quality, gfx::UpscaleQuality::Native};
    if (g.upscaler == gfx::Upscaler::Off) g.upscaler = gfx::Upscaler::Taa;
    g.quality = kQuality[level];
    g.sharpness = level < 2 ? 0.45f : 0.25f;
    renderer.setShadowsEnabled(true);
    renderer.setReflectionProbeEnabled(level >= 1);
    renderer.setOcclusionCullingEnabled(true);
    if (renderer.rayTracingSupported()) renderer.setRayTracingEnabled(level == 3);
    renderer.setGraphicsSettings(g);
}

// -----------------------------------------------------------------------------
// Ventana
// -----------------------------------------------------------------------------

void applyWindowMode(HWND hwnd, WindowMode mode, int width, int height) {
#if !defined(_WIN32)
    // Android: siempre a pantalla completa.
    (void)hwnd;
    (void)mode;
    (void)width;
    (void)height;
}
#else
    if (hwnd == nullptr) return;
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor);
    if (mode == WindowMode::Fullscreen) {
        // Sin bordes, del tamano del monitor (como el "Borderless" de los juegos).
        ShowWindow(hwnd, SW_RESTORE);
        SetWindowLongPtrW(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        const RECT& r = monitor.rcMonitor;
        SetWindowPos(hwnd, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        return;
    }
    SetWindowLongPtrW(hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
    if (mode == WindowMode::Maximized) {
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
        ShowWindow(hwnd, SW_MAXIMIZE);
        return;
    }
    // Ventana con ese tamano de dentro, centrada en el area de trabajo.
    ShowWindow(hwnd, SW_RESTORE);
    RECT rect{0, 0, std::max(width, 320), std::max(height, 240)};
    AdjustWindowRectEx(&rect, WS_OVERLAPPEDWINDOW, FALSE, 0);
    const RECT& work = monitor.rcWork;
    const int w = std::min<int>(rect.right - rect.left, work.right - work.left);
    const int h = std::min<int>(rect.bottom - rect.top, work.bottom - work.top);
    const int x = work.left + (work.right - work.left - w) / 2;
    const int y = work.top + (work.bottom - work.top - h) / 2;
    SetWindowPos(hwnd, HWND_NOTOPMOST, x, y, w, h, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
}
#endif

// -----------------------------------------------------------------------------
// Graphics (Lua)
// -----------------------------------------------------------------------------

RendererGraphicsHost::RendererGraphicsHost(gfx::VulkanRenderer& renderer, HWND window, Saver saver)
    : renderer_(renderer), window_(window), saver_(std::move(saver)) {}

void RendererGraphicsHost::setWindowState(WindowMode mode, int width, int height) {
    window_mode_ = mode;
    if (width >= 320) window_width_ = width;
    if (height >= 240) window_height_ = height;
}

std::vector<GraphicsOption> RendererGraphicsHost::options() const {
    const gfx::GraphicsSettings& g = renderer_.graphicsSettings();
    const gfx::HardwareProfile& hw = renderer_.hardwareProfile();
    std::vector<GraphicsOption> o;
    const auto add = [&](const char* key, GraphicsValue value, const char* description,
                         std::vector<std::string> choices = {}, bool writable = true) {
        o.push_back(GraphicsOption{key, std::move(value), writable, description, std::move(choices)});
    };
    const auto number = [](double v) { return GraphicsValue{v}; };

    // Escalado y antialiasing
    add("upscaler", std::string(kUpscalerNames[std::min<std::size_t>(static_cast<std::size_t>(g.upscaler), 4)]),
        "Escalado y antialiasing: off (nativa + FXAA), taa, fsr1, fsr3 (AMD FSR 3.1, INESTABLE) o dlss (NVIDIA DLSS 4, RTX, INESTABLE)",
        names(kUpscalerNames));
    add("upscaler_active", std::string(kUpscalerNames[static_cast<std::size_t>(renderer_.activeUpscaler())]),
        "El que escala de verdad (fsr3 o dlss vuelven a taa si el equipo no los tiene)", {}, false);
    add("fsr3_supported", renderer_.fsr3Supported(), "Hay AMD FSR 3.1 (lectura)", {}, false);
    add("dlss_supported", renderer_.dlssSupported(), "Hay NVIDIA DLSS 4: GPU RTX (lectura)", {}, false);
    add("resolution", std::string(kResolutionNames[static_cast<std::size_t>(g.quality)]),
        "Resolucion interna con taa/fsr1: native 100%, quality 67%, balanced 58%, performance 50%, "
        "ultra_performance 33% o custom (resolution_scale)",
        names(kResolutionNames));
    add("resolution_scale", number(gfx::renderScale(g)),
        "Escala de la resolucion interna (0.25..1). Cambiarla pasa a resolution = custom (y a taa si no habia escalado)");
    add("sharpness", number(g.sharpness), "Nitidez AMD RCAS tras el escalado (0..1)");
    // Presentacion y rendimiento
    add("vsync", g.vsync, "Sincronizar con la pantalla");
    add("adaptive", g.adaptive, "Presupuesto adaptativo: baja la calidad sola para llegar a target_fps");
    add("target_fps", number(g.target_fps), "FPS objetivo del presupuesto adaptativo (15..360)");
    // Sombras y texturas
    add("shadows", renderer_.shadowsEnabled(), "Sombras");
    add("shadow_resolution", number(g.shadow_resolution),
        "Resolucion por cascada del mapa de sombras del sol (0 = segun el PC, 512..8192)");
    add("shadow_quality", levelOf(kShadowLevels, g.shadow_resolution),
        "Calidad de las sombras: auto, low (1024), medium (2048), high (4096) o ultra (6144)", levelNames(kShadowLevels));
    add("texture_max_size", number(g.texture_max_size),
        "Lado maximo de las texturas (0 = segun el PC). Se aplica a los modelos que se cargan despues");
    add("texture_quality", levelOf(kTextureLevels, g.texture_max_size),
        "Calidad de las texturas: auto, low (1024), medium (2048), high (4096), ultra (8192) o max (16384). "
        "Se aplica a lo que se carga despues (Scene.load)",
        levelNames(kTextureLevels));
    // Iluminacion y rendimiento del render
    add("ray_tracing", renderer_.rayTracingEnabled(), "Trazado de rayos por hardware (GI, reflejos y sombras de las luces locales)");
    add("reflection_probe", renderer_.reflectionProbeEnabled(), "Sonda de reflexion del entorno");
    add("occlusion_culling", renderer_.occlusionCullingEnabled(), "No dibujar lo que queda tapado (GPU)");
    add("cascade_debug", renderer_.cascadeDebug(), "Colorear las cascadas de sombra (depurar)");
    // Ventana (solo en el juego exportado)
    const bool game_window = window_ != nullptr;
    add("window_mode", std::string(kWindowModeNames[static_cast<std::size_t>(window_mode_)]),
        "Ventana: maximized, fullscreen (pantalla completa sin bordes) o windowed (window_width x window_height)",
        names(kWindowModeNames), game_window);
    add("window_width", number(window_width_), "Ancho de la ventana en modo windowed", {}, game_window);
    add("window_height", number(window_height_), "Alto de la ventana en modo windowed", {}, game_window);
    // Solo lectura: el PC y lo que se esta dibujando
    const vk::Extent2D render = renderer_.renderExtent();
    const vk::Extent2D screen = renderer_.sceneExtent();
    add("render_width", number(render.width), "Resolucion interna (ancho)", {}, false);
    add("render_height", number(render.height), "Resolucion interna (alto)", {}, false);
    add("screen_width", number(screen.width), "Resolucion de salida (ancho)", {}, false);
    add("screen_height", number(screen.height), "Resolucion de salida (alto)", {}, false);
    add("gpu", hw.gpu_name, "Nombre de la GPU", {}, false);
    add("hardware_tier", std::string(gfx::tierName(hw.tier)), "Perfil del PC que eligio el motor", {}, false);
    add("vram_mb", number(static_cast<double>(hw.vram_mb)), "Memoria de video (MB)", {}, false);
    std::uint64_t used = 0;
    std::uint64_t budget = 0;
    if (renderer_.device().videoMemory(used, budget)) {
        add("vram_used_mb", number(static_cast<double>(used) / (1024.0 * 1024.0)), "VRAM en uso (MB)", {}, false);
    }
    add("ray_tracing_supported", renderer_.rayTracingSupported(), "La GPU admite trazado de rayos", {}, false);
    add("gpu_ms", number(renderer_.frameBudget().smoothedGpuMs()), "Milisegundos de GPU por frame (suavizado)", {}, false);
    // Vegetacion instanciada (componente Foliage): para medir el rendimiento.
    const gfx::FoliageStats fs = renderer_.foliageStats();
    add("foliage_trees", number(static_cast<double>(fs.instances)), "Arboles de la vegetacion en la GPU", {}, false);
    add("foliage_visible", number(static_cast<double>(fs.visible[0] + fs.visible[1] + fs.visible[2])),
        "Arboles dibujados el ultimo frame", {}, false);
    add("foliage_near", number(static_cast<double>(fs.visible[0])), "Arboles con todo el detalle (cerca)", {}, false);
    add("foliage_triangles", number(static_cast<double>(fs.triangles)), "Triangulos de la vegetacion el ultimo frame", {}, false);
    return o;
}

bool RendererGraphicsHost::set(const std::string& key, const GraphicsValue& value, std::string& error) {
    gfx::GraphicsSettings g = renderer_.graphicsSettings();
    bool flag = false;
    double n = 0.0;
    std::string text;
    const auto need_bool = [&]() {
        if (asBool(value, flag)) return true;
        error = "debe ser true o false";
        return false;
    };
    const auto need_number = [&](double lo, double hi) {
        if (!asNumber(value, n)) {
            error = "debe ser un numero";
            return false;
        }
        n = std::clamp(n, lo, hi);
        return true;
    };
    const auto need_choice = [&](const std::vector<std::string>& choices) {
        if (!asText(value, text)) {
            error = "debe ser un texto";
            return false;
        }
        (void)choices;
        return true;
    };
    const auto bad_choice = [&](const std::vector<std::string>& choices) {
        error = "'" + text + "' no vale; usa:";
        for (const std::string& c : choices) error += " " + c;
        return false;
    };

    bool settings_changed = false;
    if (key == "upscaler") {
        if (!need_choice(names(kUpscalerNames))) return false;
        const int i = indexOf(kUpscalerNames, text);
        if (i < 0) return bad_choice(names(kUpscalerNames));
        g.upscaler = static_cast<gfx::Upscaler>(i);
        settings_changed = true;
    } else if (key == "resolution") {
        if (!need_choice(names(kResolutionNames))) return false;
        const int i = indexOf(kResolutionNames, text);
        if (i < 0) return bad_choice(names(kResolutionNames));
        g.quality = static_cast<gfx::UpscaleQuality>(i);
        settings_changed = true;
    } else if (key == "resolution_scale") {
        if (!need_number(0.25, 1.0)) return false;
        g.quality = gfx::UpscaleQuality::Custom;
        g.custom_scale = static_cast<float>(n);
        if (g.upscaler == gfx::Upscaler::Off) g.upscaler = gfx::Upscaler::Taa;
        settings_changed = true;
    } else if (key == "sharpness") {
        if (!need_number(0.0, 1.0)) return false;
        g.sharpness = static_cast<float>(n);
        settings_changed = true;
    } else if (key == "vsync") {
        if (!need_bool()) return false;
        g.vsync = flag;
        settings_changed = true;
    } else if (key == "adaptive") {
        if (!need_bool()) return false;
        g.adaptive = flag;
        settings_changed = true;
    } else if (key == "target_fps") {
        if (!need_number(15.0, 360.0)) return false;
        g.target_fps = static_cast<float>(n);
        settings_changed = true;
    } else if (key == "shadow_resolution") {
        if (!need_number(0.0, 8192.0)) return false;
        const int size = static_cast<int>(n);
        g.shadow_resolution = size == 0 ? 0 : std::max(size, 512);
        settings_changed = true;
    } else if (key == "shadow_quality") {
        if (!need_choice(levelNames(kShadowLevels))) return false;
        int size = 0;
        if (!sizeOfLevel(kShadowLevels, text, size)) return bad_choice(levelNames(kShadowLevels));
        g.shadow_resolution = size;
        settings_changed = true;
    } else if (key == "texture_max_size") {
        if (!need_number(0.0, 16384.0)) return false;
        const int size = static_cast<int>(n);
        g.texture_max_size = size == 0 ? 0 : std::max(size, 64);
        settings_changed = true;
    } else if (key == "texture_quality") {
        if (!need_choice(levelNames(kTextureLevels))) return false;
        int size = 0;
        if (!sizeOfLevel(kTextureLevels, text, size)) return bad_choice(levelNames(kTextureLevels));
        g.texture_max_size = size;
        settings_changed = true;
    } else if (key == "shadows") {
        if (!need_bool()) return false;
        renderer_.setShadowsEnabled(flag);
    } else if (key == "ray_tracing") {
        if (!need_bool()) return false;
        if (flag && !renderer_.rayTracingSupported()) {
            error = "esta GPU no admite trazado de rayos (mira ray_tracing_supported)";
            return false;
        }
        renderer_.setRayTracingEnabled(flag);
    } else if (key == "reflection_probe") {
        if (!need_bool()) return false;
        renderer_.setReflectionProbeEnabled(flag);
    } else if (key == "occlusion_culling") {
        if (!need_bool()) return false;
        renderer_.setOcclusionCullingEnabled(flag);
    } else if (key == "cascade_debug") {
        if (!need_bool()) return false;
        renderer_.setCascadeDebug(flag);
    } else if (key == "window_mode" || key == "window_width" || key == "window_height") {
        if (window_ == nullptr) {
            error = "la ventana solo se cambia en el juego exportado (en el editor es la del editor)";
            return false;
        }
        if (key == "window_mode") {
            if (!need_choice(names(kWindowModeNames))) return false;
            const int i = indexOf(kWindowModeNames, text);
            if (i < 0) return bad_choice(names(kWindowModeNames));
            window_mode_ = static_cast<WindowMode>(i);
        } else {
            if (!need_number(key == "window_width" ? 320.0 : 240.0, 16384.0)) return false;
            (key == "window_width" ? window_width_ : window_height_) = static_cast<int>(n);
            // Dar un tamano pasa a ventana con ese tamano (como Screen.SetResolution).
            window_mode_ = WindowMode::Windowed;
        }
        applyWindowMode(window_, window_mode_, window_width_, window_height_);
        return true;
    } else {
        bool known = false;
        for (const GraphicsOption& option : options()) {
            if (option.key == key) {
                known = true;
                break;
            }
        }
        error = known ? "es de solo lectura" : "no existe (mira Graphics.options())";
        return false;
    }
    if (settings_changed) renderer_.setGraphicsSettings(g);
    quality_ = "Personalizada";
    return true;
}

std::vector<std::string> RendererGraphicsHost::qualityLevels() const { return qualityPresetNames(); }

bool RendererGraphicsHost::setQuality(const std::string& level, std::string& error) {
    const std::vector<std::string>& levels = qualityPresetNames();
    static constexpr std::array<const char*, 4> kEnglish = {"low", "medium", "high", "ultra"};
    const std::string wanted = lower(level);
    for (std::size_t i = 0; i < levels.size(); ++i) {
        if (wanted == lower(levels[i]) || wanted == kEnglish[i]) {
            applyQualityPreset(renderer_, static_cast<int>(i));
            quality_ = levels[i];
            return true;
        }
    }
    error = "'" + level + "' no es una calidad: Baja, Media, Alta o Ultra";
    return false;
}

std::vector<std::pair<int, int>> RendererGraphicsHost::resolutions() const {
    std::set<std::pair<int, int>, std::greater<>> sizes;
#if defined(_WIN32)
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    for (DWORD i = 0; EnumDisplaySettingsW(nullptr, i, &mode) != 0; ++i) {
        if (mode.dmPelsWidth >= 640 && mode.dmPelsHeight >= 480) {
            sizes.insert({static_cast<int>(mode.dmPelsWidth), static_cast<int>(mode.dmPelsHeight)});
        }
    }
#else
    // Android: la pantalla (la resolucion de dibujo se baja con resolution_scale).
    sizes.insert({window_width_, window_height_});
#endif
    return {sizes.begin(), sizes.end()};
}

bool RendererGraphicsHost::save(std::string& error) {
    if (!saver_) {
        error = "este programa no guarda la configuracion";
        return false;
    }
    return saver_(error);
}

}  // namespace cramion::editor
