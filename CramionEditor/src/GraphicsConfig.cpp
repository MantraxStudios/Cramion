#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "GraphicsConfig.h"

#include <CramionCore/vfx/VisualEffect.h>
#include <CramionFX/vk/FrameBudget.h>
#include <CramionFX/vk/GraphicsSettings.h>
#include <CramionFX/vk/VulkanRenderer.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <set>
#include <utility>
#include <vector>

#if defined(__linux__) && !defined(__ANDROID__)
#include <dlfcn.h>

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

using scripting::GraphicsOption;
using scripting::GraphicsValue;

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Nombres de los enums en la API (en el orden del enum).
constexpr std::array<const char*, 5> kUpscalerNames = {"off", "taa", "fsr1", "fsr3", "dlss"};
constexpr std::array<const char*, 6> kResolutionNames = {"native", "quality", "balanced",
                                                         "performance", "ultra_performance", "custom"};
constexpr std::array<const char*, kWindowModeCount> kWindowModeNames = {"maximized", "fullscreen", "windowed",
                                                                        "borderless"};

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
// Filtrado anisotropico por nombre: muestras (0 = segun el PC, 1 = apagado).
constexpr std::array<SizeLevel, 6> kFilteringLevels = {{{"auto", "auto", 0},
                                                        {"off", "no", 1},
                                                        {"2x", "2x", 2},
                                                        {"4x", "4x", 4},
                                                        {"8x", "8x", 8},
                                                        {"16x", "16x", 16}}};

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

int loadGraphicsIni(const std::filesystem::path& file, gfx::VulkanRenderer& renderer, GraphicsIniSource source) {
    std::ifstream in(file);
    if (!in) return 0;
    // Primero todo (la version puede ir en cualquier linea) y despues se aplica.
    std::vector<std::pair<std::string, std::string>> entries;
    int version = 1;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();  // escrito en Windows (CRLF), leido en Android
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        entries.emplace_back(line.substr(0, eq), line.substr(eq + 1));
        if (entries.back().first == "version") version = std::max(std::atoi(entries.back().second.c_str()), 1);
    }
    gfx::GraphicsSettings g = renderer.graphicsSettings();
    for (const auto& [key, text] : entries) {
        const float value = std::strtof(text.c_str(), nullptr);
        if (key == "upscaler") g.upscaler = static_cast<gfx::Upscaler>(std::clamp(static_cast<int>(value), 0, 4));
        if (key == "quality") g.quality = static_cast<gfx::UpscaleQuality>(std::clamp(static_cast<int>(value), 0, 5));
        if (key == "custom_scale") g.custom_scale = std::clamp(value, 0.25f, 1.0f);
        if (key == "sharpness") g.sharpness = std::clamp(value, 0.0f, 1.0f);
        if (key == "vsync") g.vsync = value != 0.0f;
        // Apagar el presupuesto desde el ini del jugador solo si es de los
        // nuevos (version 2): en los de antes iba lo heredado del proyecto.
        if (key == "adaptive" &&
            (value != 0.0f || source == GraphicsIniSource::Project || version >= kGraphicsIniVersion)) {
            g.adaptive = value != 0.0f;
        }
        if (key == "target_fps") g.target_fps = std::clamp(value, 15.0f, 360.0f);
        if (key == "shadow_resolution") g.shadow_resolution = std::clamp(static_cast<int>(value), 0, 8192);
        if (key == "texture_max_size") g.texture_max_size = std::clamp(static_cast<int>(value), 0, 16384);
        if (key == "texture_filtering") g.texture_filtering = std::clamp(static_cast<int>(value), 0, 16);
        if (key == "texture_lod_bias" && std::isfinite(value)) g.texture_lod_bias = std::clamp(value, -1.0f, 2.0f);
        if (key == "vr_render_scale" && std::isfinite(value)) g.vr_render_scale = std::clamp(value, 0.5f, 1.0f);
        if (key == "shadows") renderer.setShadowsEnabled(value != 0.0f);
        if (key == "ray_tracing" && renderer.rayTracingSupported()) renderer.setRayTracingEnabled(value != 0.0f);
        if (key == "reflection_probe") renderer.setReflectionProbeEnabled(value != 0.0f);
        if (key == "sky_occlusion") renderer.setSkyOcclusionEnabled(value != 0.0f);
        if (key == "occlusion_culling") renderer.setOcclusionCullingEnabled(value != 0.0f);
        // Iluminacion: 0 = la decide el presupuesto adaptativo (como antes),
        // 1 = completa, 2 = ligera (sin sombras suaves de contacto del sol,
        // sin oclusion del cielo desde arriba ni sombras de contacto).
        if (key == "lighting_quality") {
            const int q = static_cast<int>(value);
            if (q == 1) renderer.setLiteLightingForced(false);
            else if (q == 2) renderer.setLiteLightingForced(true);
            else renderer.clearLiteLightingForced();
        }
    }
    renderer.setGraphicsSettings(g);
    return version;
}

bool saveGraphicsIni(const std::filesystem::path& file, const gfx::VulkanRenderer& renderer) {
    std::error_code error;
    std::filesystem::create_directories(file.parent_path(), error);
    std::ofstream out(file, std::ios::trunc);
    if (!out) return false;
    const gfx::GraphicsSettings& g = renderer.graphicsSettings();
    out << "version=" << kGraphicsIniVersion << "\n";
    out << "upscaler=" << static_cast<int>(g.upscaler) << "\n";
    out << "quality=" << static_cast<int>(g.quality) << "\n";
    out << "custom_scale=" << g.custom_scale << "\n";
    out << "sharpness=" << g.sharpness << "\n";
    out << "vsync=" << (g.vsync ? 1 : 0) << "\n";
    out << "adaptive=" << (g.adaptive ? 1 : 0) << "\n";
    out << "target_fps=" << g.target_fps << "\n";
    out << "shadow_resolution=" << g.shadow_resolution << "\n";
    out << "texture_max_size=" << g.texture_max_size << "\n";
    out << "texture_filtering=" << g.texture_filtering << "\n";
    out << "texture_lod_bias=" << g.texture_lod_bias << "\n";
    out << "vr_render_scale=" << g.vr_render_scale << "\n";
    out << "shadows=" << (renderer.shadowsEnabled() ? 1 : 0) << "\n";
    out << "ray_tracing=" << (renderer.rayTracingEnabled() ? 1 : 0) << "\n";
    out << "reflection_probe=" << (renderer.reflectionProbeEnabled() ? 1 : 0) << "\n";
    out << "sky_occlusion=" << (renderer.skyOcclusionEnabled() ? 1 : 0) << "\n";
    out << "occlusion_culling=" << (renderer.occlusionCullingEnabled() ? 1 : 0) << "\n";
    out << "lighting_quality=" << (renderer.liteLightingForced() < 0 ? 0 : renderer.liteLightingForced() == 0 ? 1 : 2)
        << "\n";
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
            mode = static_cast<WindowMode>(std::clamp(value, 0, kWindowModeCount - 1));
            found = true;
        }
        if (key == "window_width" && value >= 320) width = std::min(value, 16384);
        if (key == "window_height" && value >= 240) height = std::min(value, 16384);
    }
    return found;
}

int clampFpsLimit(double fps) {
    if (!std::isfinite(fps) || fps < 1.0) return 0;
    return std::clamp(static_cast<int>(std::lround(fps)), kFpsLimitMin, kFpsLimitMax);
}

int readFpsLimit(const std::filesystem::path& file) {
    std::ifstream in(file);
    if (!in) return 0;
    int limit = 0;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("fps_limit=", 0) == 0) limit = clampFpsLimit(std::strtod(line.c_str() + 10, nullptr));
    }
    return limit;
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
    // VFX Graph: la mezcla Distorsion, apagada en Baja (vfx::vfxDistortionForQuality).
    renderer.vfx().setDistortionQuality(level >= 1);
    renderer.setGraphicsSettings(g);
    // Efectos: la tabla de escalabilidad por tipo de esa calidad (vfx.quality manda si es >= 0).
    vfx::setQualityPreset(level);
}

// -----------------------------------------------------------------------------
// Ventana
// -----------------------------------------------------------------------------

#if defined(_WIN32)
namespace {

// Lo que anade el marco de Windows (WS_OVERLAPPEDWINDOW) al tamano de dentro,
// con el DPI de la ventana (el proceso es "per monitor").
SIZE windowFrameExtra(HWND hwnd) {
    RECT r{0, 0, 0, 0};
    const UINT dpi = hwnd != nullptr ? GetDpiForWindow(hwnd) : 0;
    if (dpi > 0) {
        AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
    } else {
        AdjustWindowRectEx(&r, WS_OVERLAPPEDWINDOW, FALSE, 0);
    }
    return SIZE{r.right - r.left, r.bottom - r.top};
}

bool monitorInfo(HWND hwnd, MONITORINFOEXW& info) {
    info = MONITORINFOEXW{};
    info.cbSize = sizeof(info);
    const HMONITOR monitor = hwnd != nullptr ? MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST)
                                             : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    return monitor != nullptr && GetMonitorInfoW(monitor, &info) != FALSE;
}

// Sin marco, w x h (como mucho el monitor), centrada en el monitor.
void showBorderless(HWND hwnd, const RECT& monitor, int width, int height) {
    const int w = std::min<int>(width, monitor.right - monitor.left);
    const int h = std::min<int>(height, monitor.bottom - monitor.top);
    const int x = monitor.left + (monitor.right - monitor.left - w) / 2;
    const int y = monitor.top + (monitor.bottom - monitor.top - h) / 2;
    ShowWindow(hwnd, SW_RESTORE);
    SetWindowLongPtrW(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
    SetWindowPos(hwnd, HWND_TOP, x, y, w, h, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
}

}  // namespace
#endif

#if defined(__linux__) && !defined(__ANDROID__)
namespace {

// Linux (X11): los monitores con XRandR 1.5 (XRRGetMonitors, cargado con
// dlopen para no enlazar libXrandr); sin el, la pantalla entera de X.
struct XrrMonitorInfo {  // XRRMonitorInfo de <X11/extensions/Xrandr.h>
    Atom name;
    int primary;
    int automatic;
    int noutput;
    int x;
    int y;
    int width;
    int height;
    int mwidth;
    int mheight;
    unsigned long* outputs;
};
using GetMonitorsFn = XrrMonitorInfo* (*)(Display*, ::Window, int, int*);
using FreeMonitorsFn = void (*)(XrrMonitorInfo*);

struct XRect {
    long left = 0;
    long top = 0;
    long right = 0;
    long bottom = 0;
    long width() const { return right - left; }
    long height() const { return bottom - top; }
};

Display* displayOf(HWND hwnd) { return hwnd != nullptr ? static_cast<Display*>(hwnd->display) : nullptr; }
::Window windowOf(HWND hwnd) { return hwnd != nullptr ? static_cast<::Window>(hwnd->window) : 0; }

std::vector<long> longProperty(Display* display, ::Window window, const char* name, Atom type) {
    std::vector<long> out;
    const Atom property = XInternAtom(display, name, True);
    if (property == 0L) return out;
    Atom actual = 0;
    int format = 0;
    unsigned long count = 0;
    unsigned long remaining = 0;
    unsigned char* data = nullptr;
    if (XGetWindowProperty(display, window, property, 0, 64, False, type, &actual, &format, &count, &remaining, &data) == 0 &&
        data != nullptr && format == 32) {
        const auto* values = reinterpret_cast<const long*>(data);  // formato 32 = long en Xlib
        out.assign(values, values + count);
    }
    if (data != nullptr) XFree(data);
    return out;
}

// Rectangulo del area de dentro de la ventana en la pantalla.
bool clientRect(Display* display, ::Window window, XRect& rect) {
    XWindowAttributes attributes{};
    if (XGetWindowAttributes(display, window, &attributes) == 0) return false;
    int x = 0;
    int y = 0;
    ::Window child = 0;
    XTranslateCoordinates(display, window, attributes.root, 0, 0, &x, &y, &child);
    rect = {x, y, x + attributes.width, y + attributes.height};
    return true;
}

// El monitor con el centro de la ventana (sin ventana, el principal).
bool monitorRect(Display* display, ::Window window, XRect& out) {
    const ::Window root = DefaultRootWindow(display);
    XRect client{};
    const bool has_window = window != 0 && clientRect(display, window, client);
    const long cx = (client.left + client.right) / 2;
    const long cy = (client.top + client.bottom) / 2;
    static void* xrandr = dlopen("libXrandr.so.2", RTLD_LAZY | RTLD_LOCAL);
    static const auto get_monitors = xrandr != nullptr ? reinterpret_cast<GetMonitorsFn>(dlsym(xrandr, "XRRGetMonitors")) : nullptr;
    static const auto free_monitors =
        xrandr != nullptr ? reinterpret_cast<FreeMonitorsFn>(dlsym(xrandr, "XRRFreeMonitors")) : nullptr;
    if (get_monitors != nullptr && free_monitors != nullptr) {
        int count = 0;
        if (XrrMonitorInfo* monitors = get_monitors(display, root, True, &count)) {
            int best = -1;
            for (int i = 0; i < count; ++i) {
                const XrrMonitorInfo& m = monitors[i];
                const bool inside = cx >= m.x && cx < m.x + m.width && cy >= m.y && cy < m.y + m.height;
                if ((has_window && inside) || (!has_window && m.primary != 0)) {
                    best = i;
                    break;
                }
                if (best < 0 && (m.primary != 0 || i == 0)) best = i;
            }
            if (best >= 0) out = {monitors[best].x, monitors[best].y, monitors[best].x + monitors[best].width,
                                  monitors[best].y + monitors[best].height};
            free_monitors(monitors);
            if (best >= 0) return true;
        }
    }
    const int screen = DefaultScreen(display);
    out = {0, 0, DisplayWidth(display, screen), DisplayHeight(display, screen)};
    return out.width() > 0 && out.height() > 0;
}

// Area de trabajo (sin paneles: _NET_WORKAREA) dentro del monitor.
XRect workArea(Display* display, const XRect& monitor) {
    XRect work = monitor;
    const std::vector<long> area = longProperty(display, DefaultRootWindow(display), "_NET_WORKAREA", XA_CARDINAL);
    if (area.size() >= 4) {
        work.left = std::max(monitor.left, area[0]);
        work.top = std::max(monitor.top, area[1]);
        work.right = std::min(monitor.right, area[0] + area[2]);
        work.bottom = std::min(monitor.bottom, area[1] + area[3]);
        if (work.width() <= 0 || work.height() <= 0) work = monitor;
    }
    return work;
}

// Lo que anade el marco del gestor de ventanas (_NET_FRAME_EXTENTS:
// izquierda, derecha, arriba, abajo). Sin saberlo, un marco tipico.
void frameExtents(Display* display, ::Window window, long& horizontal, long& vertical) {
    const std::vector<long> extents =
        window != 0 ? longProperty(display, window, "_NET_FRAME_EXTENTS", XA_CARDINAL) : std::vector<long>{};
    if (extents.size() >= 4 && extents[0] + extents[1] + extents[2] + extents[3] > 0) {
        horizontal = extents[0] + extents[1];
        vertical = extents[2] + extents[3];
    } else {
        horizontal = 2;
        vertical = 37;
    }
}

// _NET_WM_STATE: anade (1) o quita (0) hasta dos estados (mensaje al gestor).
void setWmState(Display* display, ::Window window, bool add, const char* first, const char* second = nullptr) {
    XEvent event{};
    event.xclient.type = ClientMessage;
    event.xclient.window = window;
    event.xclient.message_type = XInternAtom(display, "_NET_WM_STATE", False);
    event.xclient.format = 32;
    event.xclient.data.l[0] = add ? 1 : 0;
    event.xclient.data.l[1] = static_cast<long>(XInternAtom(display, first, False));
    event.xclient.data.l[2] = second != nullptr ? static_cast<long>(XInternAtom(display, second, False)) : 0;
    event.xclient.data.l[3] = 1;  // aplicacion normal
    XSendEvent(display, DefaultRootWindow(display), False, SubstructureRedirectMask | SubstructureNotifyMask, &event);
}

// Con o sin la decoracion del gestor (_MOTIF_WM_HINTS).
void setDecorated(Display* display, ::Window window, bool decorated) {
    const Atom hints = XInternAtom(display, "_MOTIF_WM_HINTS", False);
    const long value[5] = {2 /* MWM_HINTS_DECORATIONS */, 0, decorated ? 1 : 0, 0, 0};
    XChangeProperty(display, window, hints, hints, 32, PropModeReplace, reinterpret_cast<const unsigned char*>(value), 5);
}

bool decorated(Display* display, ::Window window) {
    const Atom hints = XInternAtom(display, "_MOTIF_WM_HINTS", True);
    if (hints == 0L) return true;
    const std::vector<long> value = longProperty(display, window, "_MOTIF_WM_HINTS", hints);
    return value.size() < 3 || (value[0] & 2) == 0 || value[2] != 0;
}

bool hasWmState(Display* display, ::Window window, const char* state) {
    const Atom wanted = XInternAtom(display, state, True);
    if (wanted == 0L) return false;
    for (const long atom : longProperty(display, window, "_NET_WM_STATE", XA_ATOM)) {
        if (static_cast<Atom>(atom) == wanted) return true;
    }
    return false;
}

// Sin marco, w x h (como mucho el monitor), centrada en el monitor.
void showBorderless(Display* display, ::Window window, const XRect& monitor, long width, long height) {
    setWmState(display, window, false, "_NET_WM_STATE_FULLSCREEN");
    setWmState(display, window, false, "_NET_WM_STATE_MAXIMIZED_VERT", "_NET_WM_STATE_MAXIMIZED_HORZ");
    setDecorated(display, window, false);
    const long w = std::min(width, monitor.width());
    const long h = std::min(height, monitor.height());
    XMoveResizeWindow(display, window, static_cast<int>(monitor.left + (monitor.width() - w) / 2),
                      static_cast<int>(monitor.top + (monitor.height() - h) / 2), static_cast<unsigned>(w),
                      static_cast<unsigned>(h));
}

}  // namespace
#endif

bool monitorSize(HWND hwnd, MonitorSize& out) {
#if defined(__linux__) && !defined(__ANDROID__)
    out = MonitorSize{};
    Display* display = displayOf(hwnd);
    XRect monitor{};
    if (display == nullptr || !monitorRect(display, windowOf(hwnd), monitor)) return false;
    const XRect work = workArea(display, monitor);
    long frame_w = 0;
    long frame_h = 0;
    frameExtents(display, windowOf(hwnd), frame_w, frame_h);
    out.width = static_cast<int>(monitor.width());
    out.height = static_cast<int>(monitor.height());
    out.windowed_max_width = std::max(0, static_cast<int>(work.width() - frame_w));
    out.windowed_max_height = std::max(0, static_cast<int>(work.height() - frame_h));
    return true;
#elif !defined(_WIN32)
    (void)hwnd;
    out = MonitorSize{};
    return false;
#else
    MONITORINFOEXW info{};
    if (!monitorInfo(hwnd, info)) return false;
    const SIZE frame = windowFrameExtra(hwnd);
    out.width = static_cast<int>(info.rcMonitor.right - info.rcMonitor.left);
    out.height = static_cast<int>(info.rcMonitor.bottom - info.rcMonitor.top);
    out.windowed_max_width = std::max(0, static_cast<int>(info.rcWork.right - info.rcWork.left - frame.cx));
    out.windowed_max_height = std::max(0, static_cast<int>(info.rcWork.bottom - info.rcWork.top - frame.cy));
    return true;
#endif
}

void applyWindowMode(HWND hwnd, WindowMode mode, int width, int height) {
#if defined(__linux__) && !defined(__ANDROID__)
    // Linux: lo mismo con las peticiones de EWMH al gestor de ventanas
    // (_NET_WM_STATE) y _MOTIF_WM_HINTS para quitar el marco. El tamano nuevo
    // llega luego como WindowResize (ConfigureNotify).
    Display* display = displayOf(hwnd);
    const ::Window window = windowOf(hwnd);
    if (display == nullptr || window == 0) return;
    XRect screen{};
    if (!monitorRect(display, window, screen)) return;
    if (mode == WindowMode::Fullscreen) {
        // Pantalla completa del gestor (sin bordes, del tamano del monitor).
        setWmState(display, window, false, "_NET_WM_STATE_MAXIMIZED_VERT", "_NET_WM_STATE_MAXIMIZED_HORZ");
        setDecorated(display, window, false);
        XMoveResizeWindow(display, window, static_cast<int>(screen.left), static_cast<int>(screen.top),
                          static_cast<unsigned>(screen.width()), static_cast<unsigned>(screen.height()));
        setWmState(display, window, true, "_NET_WM_STATE_FULLSCREEN");
        XFlush(display);
        return;
    }
    width = std::max(width, 320);
    height = std::max(height, 240);
    if (mode == WindowMode::Borderless) {
        showBorderless(display, window, screen, width, height);
        XFlush(display);
        return;
    }
    const XRect work = workArea(display, screen);
    long frame_w = 0;
    long frame_h = 0;
    frameExtents(display, window, frame_w, frame_h);
    const long framed_w = width + frame_w;
    const long framed_h = height + frame_h;
    const bool fits_framed = framed_w <= work.width() && framed_h <= work.height();
    const bool fits_monitor = width <= screen.width() && height <= screen.height();
    if (mode == WindowMode::Windowed && !fits_framed && fits_monitor) {
        // Con el marco no cabe: sin marco, el tamano pedido.
        showBorderless(display, window, screen, width, height);
        XFlush(display);
        return;
    }
    setWmState(display, window, false, "_NET_WM_STATE_FULLSCREEN");
    setDecorated(display, window, true);
    if (mode == WindowMode::Maximized) {
        setWmState(display, window, true, "_NET_WM_STATE_MAXIMIZED_VERT", "_NET_WM_STATE_MAXIMIZED_HORZ");
        XFlush(display);
        return;
    }
    // Ventana con ese tamano de dentro, centrada en el area de trabajo.
    setWmState(display, window, false, "_NET_WM_STATE_MAXIMIZED_VERT", "_NET_WM_STATE_MAXIMIZED_HORZ");
    const long w = std::min(framed_w, work.width());
    const long h = std::min(framed_h, work.height());
    XMoveResizeWindow(display, window, static_cast<int>(work.left + (work.width() - w) / 2),
                      static_cast<int>(work.top + (work.height() - h) / 2), static_cast<unsigned>(w - frame_w),
                      static_cast<unsigned>(h - frame_h));
    XFlush(display);
}
#elif !defined(_WIN32)
    // Android: siempre a pantalla completa.
    (void)hwnd;
    (void)mode;
    (void)width;
    (void)height;
}
#else
    if (hwnd == nullptr) return;
    MONITORINFOEXW monitor{};
    if (!monitorInfo(hwnd, monitor)) return;
    const RECT& screen = monitor.rcMonitor;
    if (mode == WindowMode::Fullscreen) {
        // Sin bordes, del tamano del monitor (como el "Borderless" de los juegos).
        showBorderless(hwnd, screen, screen.right - screen.left, screen.bottom - screen.top);
        return;
    }
    width = std::max(width, 320);
    height = std::max(height, 240);
    if (mode == WindowMode::Borderless) {
        showBorderless(hwnd, screen, width, height);
        return;
    }
    const RECT& work = monitor.rcWork;
    const SIZE frame = windowFrameExtra(hwnd);
    const int framed_w = width + frame.cx;
    const int framed_h = height + frame.cy;
    const bool fits_framed = framed_w <= work.right - work.left && framed_h <= work.bottom - work.top;
    const bool fits_monitor = width <= screen.right - screen.left && height <= screen.bottom - screen.top;
    if (mode == WindowMode::Windowed && !fits_framed && fits_monitor) {
        // Con el marco no cabe (Windows la recortaria: 1920x1080 quedaba en
        // 1920x1061 en un monitor de 1920x1080). Sin marco, el tamano pedido.
        showBorderless(hwnd, screen, width, height);
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
    const int w = std::min<int>(framed_w, work.right - work.left);
    const int h = std::min<int>(framed_h, work.bottom - work.top);
    const int x = work.left + (work.right - work.left - w) / 2;
    const int y = work.top + (work.bottom - work.top - h) / 2;
    SetWindowPos(hwnd, HWND_NOTOPMOST, x, y, w, h, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
}
#endif

// -----------------------------------------------------------------------------
// Graphics (API de los scripts)
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
    add("vr_render_scale", number(g.vr_render_scale),
        "VR: resolucion de cada ojo respecto a la recomendada por el casco (0.5..1). En VR sustituye a "
        "resolution/resolution_scale y el antialiasing es el TAA por ojo (fsr1 solo si se elige)");
    // Presentacion y rendimiento
    add("vsync", g.vsync, "Sincronizar con la pantalla");
    add("adaptive", g.adaptive, "Presupuesto adaptativo: baja la calidad sola para llegar a target_fps");
    add("target_fps", number(g.target_fps), "FPS objetivo del presupuesto adaptativo (15..360)");
    add("fps_limit", number(fps_limit_),
        "Tope de FPS del juego exportado: 0 = sin tope, 15..360 (espera al final de cada frame; con vsync manda "
        "tambien la pantalla). Se guarda con Graphics.save(). En el editor no se aplica");
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
    add("texture_filtering", levelOf(kFilteringLevels, g.texture_filtering),
        "Filtrado anisotropico de las texturas (suelos y paredes en angulo nitidos): auto (segun el PC), off, "
        "2x, 4x, 8x o 16x. Tambien vale un numero (1..16). Se aplica al momento",
        levelNames(kFilteringLevels));
    add("texture_filtering_active", number(renderer_.textureFiltering().anisotropy),
        "Anisotropico en uso (1 = apagado; con auto, el del perfil del PC)", {}, false);
    add("texture_filtering_max", number(renderer_.maxTextureAnisotropy()),
        "Maximo anisotropico que admite la GPU (lectura)", {}, false);
    add("texture_lod_bias", number(g.texture_lod_bias),
        "Sesgo de los mipmaps (-1..2): negativo = texturas mas nitidas (y mas parpadeo de lejos), positivo = "
        "mas suaves y menos ancho de banda. 0 = normal. Se aplica al momento");
    // Iluminacion y rendimiento del render
    add("ray_tracing", renderer_.rayTracingEnabled(), "Trazado de rayos por hardware (GI, reflejos y sombras de las luces locales)");
    add("reflection_probe", renderer_.reflectionProbeEnabled(), "Sonda de reflexion del entorno");
    add("sky_occlusion", renderer_.skyOcclusionEnabled(),
        "Oclusion del cielo vista desde arriba (sin trazado de rayos): bajo los arboles y los tejados llega menos cielo");
    add("occlusion_culling", renderer_.occlusionCullingEnabled(), "No dibujar lo que queda tapado (GPU)");
    add("lighting_quality",
        std::string(renderer_.liteLightingForced() < 0 ? "auto" : renderer_.liteLightingForced() == 0 ? "full" : "lite"),
        "Iluminacion: auto (la decide el presupuesto adaptativo), full (completa) o lite (ligera: sin sombras suaves "
        "de contacto del sol, sin oclusion del cielo desde arriba ni sombras de contacto; mas barata en GPU "
        "integradas). Se aplica al momento",
        {"auto", "full", "lite"});
    add("cascade_debug", renderer_.cascadeDebug(), "Colorear las cascadas de sombra (depurar)");
    // Ventana (solo en el juego exportado)
    const bool game_window = window_ != nullptr;
    add("window_mode", std::string(kWindowModeNames[static_cast<std::size_t>(window_mode_)]),
        "Ventana: maximized, fullscreen (pantalla completa sin bordes), windowed (window_width x window_height con "
        "marco; sin marco si con el no cabe en la pantalla) o borderless (window_width x window_height sin marco, "
        "centrada)",
        names(kWindowModeNames), game_window);
    add("window_width", number(window_width_), "Ancho de la ventana en modo windowed o borderless", {}, game_window);
    add("window_height", number(window_height_), "Alto de la ventana en modo windowed o borderless", {}, game_window);
    add("window_size", std::to_string(window_width_) + "x" + std::to_string(window_height_),
        "Ancho y alto a la vez (\"1280x720\"): un solo cambio de tamano. Desde maximized o fullscreen pasa a windowed",
        {}, game_window);
    {
        MonitorSize monitor;
        monitorSize(window_, monitor);
        add("monitor_width", number(monitor.width), "Ancho del monitor de la ventana (0 si no se sabe)", {}, false);
        add("monitor_height", number(monitor.height), "Alto del monitor de la ventana (0 si no se sabe)", {}, false);
        add("windowed_max_width", number(monitor.windowed_max_width),
            "Mayor ancho de dentro que cabe con el marco de Windows en el area de trabajo", {}, false);
        add("windowed_max_height", number(monitor.windowed_max_height),
            "Mayor alto de dentro que cabe con el marco de Windows en el area de trabajo", {}, false);
#if defined(_WIN32)
        const bool framed = window_ != nullptr && (GetWindowLongPtrW(window_, GWL_STYLE) & WS_CAPTION) == WS_CAPTION;
#elif defined(__linux__) && !defined(__ANDROID__)
        const bool framed = displayOf(window_) != nullptr && decorated(displayOf(window_), windowOf(window_)) &&
                            !hasWmState(displayOf(window_), windowOf(window_), "_NET_WM_STATE_FULLSCREEN");
#else
        const bool framed = false;
#endif
        add("window_framed", framed, "La ventana tiene ahora el marco de Windows (lectura)", {}, false);
    }
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
    // Streaming de la memoria de video (render.streaming.*).
    add("models_resident", number(static_cast<double>(renderer_.uploadedModelCount() - renderer_.evictedModelCount())),
        "Modelos en la memoria de video", {}, false);
    add("models_evicted", number(static_cast<double>(renderer_.evictedModelCount())),
        "Modelos fuera de la memoria de video porque no se ven (vuelven al acercarse)", {}, false);
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
    } else if (key == "vr_render_scale") {
        if (!need_number(0.5, 1.0)) return false;
        g.vr_render_scale = static_cast<float>(n);
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
    } else if (key == "fps_limit") {
        // No es del renderizador: lo aplica el bucle del reproductor.
        if (!need_number(0.0, static_cast<double>(kFpsLimitMax))) return false;
        fps_limit_ = clampFpsLimit(n);
        return true;
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
    } else if (key == "texture_filtering") {
        // "16x", "off", "auto" o un numero (1 = apagado, 0 = auto).
        int samples = 0;
        if (std::holds_alternative<double>(value) && asNumber(value, n)) {
            samples = static_cast<int>(std::clamp(n, 0.0, 16.0));
        } else if (!need_choice(levelNames(kFilteringLevels))) {
            return false;
        } else if (!sizeOfLevel(kFilteringLevels, text, samples)) {
            return bad_choice(levelNames(kFilteringLevels));
        }
        // Solo potencias de dos (lo que dan las GPU): 3 -> 2, 12 -> 8.
        if (samples > 1) {
            int p = 1;
            while (p * 2 <= samples) p *= 2;
            samples = p;
        }
        g.texture_filtering = samples;
        settings_changed = true;
    } else if (key == "texture_lod_bias") {
        if (!need_number(-1.0, 2.0)) return false;
        g.texture_lod_bias = static_cast<float>(n);
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
    } else if (key == "sky_occlusion") {
        if (!need_bool()) return false;
        renderer_.setSkyOcclusionEnabled(flag);
    } else if (key == "lighting_quality") {
        if (!need_choice({"auto", "full", "lite"})) return false;
        if (text == "auto") renderer_.clearLiteLightingForced();
        else if (text == "full") renderer_.setLiteLightingForced(false);
        else if (text == "lite") renderer_.setLiteLightingForced(true);
        else return bad_choice({"auto", "full", "lite"});
    } else if (key == "occlusion_culling") {
        if (!need_bool()) return false;
        renderer_.setOcclusionCullingEnabled(flag);
    } else if (key == "cascade_debug") {
        if (!need_bool()) return false;
        renderer_.setCascadeDebug(flag);
    } else if (key == "window_mode" || key == "window_width" || key == "window_height" || key == "window_size") {
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
            if (key == "window_size") {
                // "1280x720": los dos lados a la vez (un solo cambio de tamano).
                if (!asText(value, text)) {
                    error = "debe ser un texto como \"1280x720\"";
                    return false;
                }
                const std::size_t x = lower(text).find('x');
                const int w = x == std::string::npos ? 0 : std::atoi(text.c_str());
                const int h = x == std::string::npos ? 0 : std::atoi(text.c_str() + x + 1);
                if (w < 320 || h < 240) {
                    error = "debe ser un texto como \"1280x720\" (minimo 320x240)";
                    return false;
                }
                window_width_ = std::min(w, 16384);
                window_height_ = std::min(h, 16384);
            } else {
                if (!need_number(key == "window_width" ? 320.0 : 240.0, 16384.0)) return false;
                (key == "window_width" ? window_width_ : window_height_) = static_cast<int>(n);
            }
            // Dar un tamano pasa a ventana con ese tamano (como Screen.SetResolution);
            // sin bordes sigue sin bordes.
            if (window_mode_ != WindowMode::Borderless) window_mode_ = WindowMode::Windowed;
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
    // Los modos del monitor en el que esta la ventana (sin ventana, el principal).
    MONITORINFOEXW monitor{};
    const wchar_t* device = monitorInfo(window_, monitor) ? monitor.szDevice : nullptr;
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    for (DWORD i = 0; EnumDisplaySettingsW(device, i, &mode) != 0; ++i) {
        if (mode.dmPelsWidth >= 640 && mode.dmPelsHeight >= 480) {
            sizes.insert({static_cast<int>(mode.dmPelsWidth), static_cast<int>(mode.dmPelsHeight)});
        }
    }
#elif defined(__linux__) && !defined(__ANDROID__)
    // Linux: el tamano del monitor y los habituales que caben en el (sin
    // XRandR enlazado no hay lista de modos; la ventana no cambia el modo).
    MonitorSize monitor;
    if (monitorSize(window_, monitor) && monitor.width > 0) {
        sizes.insert({monitor.width, monitor.height});
        static constexpr std::pair<int, int> kCommon[] = {{640, 480},   {800, 600},   {1024, 768},  {1280, 720},
                                                           {1280, 800},  {1366, 768},  {1440, 900},  {1600, 900},
                                                           {1680, 1050}, {1920, 1080}, {1920, 1200}, {2560, 1440},
                                                           {2560, 1600}, {3440, 1440}, {3840, 2160}};
        for (const auto& [w, h] : kCommon) {
            if (w <= monitor.width && h <= monitor.height) sizes.insert({w, h});
        }
    } else {
        sizes.insert({window_width_, window_height_});
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
