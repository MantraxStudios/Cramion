#include "LoadingScreen.h"

#include <algorithm>

#if defined(_WIN32)
#include <objidl.h>
namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#include <gdiplus.h>
#include <dbghelp.h>
#include <psapi.h>
#else
#include <CramionFX/asset/ImageFile.h>

#include <cerrno>
#include <cmath>
#include <cwchar>
#include <dlfcn.h>
#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <ucontext.h>
#include <unistd.h>
#endif

#include <array>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cstring>
#include <exception>
#include <mutex>
#include <sstream>
#include <vector>
#include <fstream>
#include <iostream>

#if !defined(_WIN32)
// Xlib al final (sus macros chocan con nombres del motor).
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#undef None
#undef Bool
#undef Status
#undef Success
#undef Always
#endif

namespace cramion::editor {

#if defined(_WIN32)
namespace {

std::wstring widen(const char* text) {
    if (text == nullptr || *text == '\0') return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    std::wstring out(static_cast<std::size_t>(std::max(n - 1, 0)), L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, text, -1, out.data(), n);
    return out;
}

}  // namespace

LoadingScreen::LoadingScreen(HWND window, const std::filesystem::path& image, bool banner_only)
    : window_(window), banner_only_(banner_only) {
    Gdiplus::GdiplusStartupInput input;
    if (Gdiplus::GdiplusStartup(&gdiplus_token_, &input, nullptr) != Gdiplus::Ok) {
        gdiplus_token_ = 0;
        return;
    }
    std::error_code error;
    if (!image.empty() && std::filesystem::exists(image, error)) {
        image_ = std::make_unique<Gdiplus::Bitmap>(image.wstring().c_str());
        if (image_->GetLastStatus() != Gdiplus::Ok) image_.reset();
    }
}

LoadingScreen::~LoadingScreen() {
    image_.reset();
    if (gdiplus_token_ != 0) Gdiplus::GdiplusShutdown(gdiplus_token_);
}

void LoadingScreen::show(float fraction, const char* status) {
    // Mensajes de la ventana (mover, activar...) para que no se congele.
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            PostQuitMessage(static_cast<int>(msg.wParam));
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    const auto now = std::chrono::steady_clock::now();
    if (painted_ && fraction < 1.0f && now - last_paint_ < std::chrono::milliseconds(16)) return;
    if (banner_only_ && painted_) {
        // El banner no cambia con el progreso: solo se repinta si la ventana
        // cambio de tamano (o de vez en cuando, por si algo la tapo).
        RECT client{};
        GetClientRect(window_, &client);
        if (client.right - client.left == painted_width_ && client.bottom - client.top == painted_height_ &&
            now - last_paint_ < std::chrono::milliseconds(500)) {
            return;
        }
    }
    last_paint_ = now;
    painted_ = true;
    fraction = std::clamp(fraction, 0.0f, 1.0f);
    wchar_t text[160];
    std::swprintf(text, 160, L"%ls   %d %%", widen(status).c_str(), static_cast<int>(fraction * 100.0f + 0.5f));
    paint(fraction, text);
}

void LoadingScreen::paint(float fraction, const std::wstring& text) {
    if (gdiplus_token_ == 0 || window_ == nullptr) return;
    RECT client{};
    GetClientRect(window_, &client);
    const int w = client.right - client.left;
    const int h = client.bottom - client.top;
    if (w <= 0 || h <= 0) return;
    painted_width_ = w;
    painted_height_ = h;

    HDC dc = GetDC(window_);
    HDC memory = CreateCompatibleDC(dc);
    HBITMAP bitmap = CreateCompatibleBitmap(dc, w, h);
    HGDIOBJ old = SelectObject(memory, bitmap);
    {
        Gdiplus::Graphics g(memory);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
        if (banner_only_) {
            // Juego exportado: solo el banner, ajustado a la ventana sobre su
            // mismo fondo (como lo pinta luego el juego: no se nota el cambio).
            g.Clear(Gdiplus::Color(255, kBannerBackground[0], kBannerBackground[1], kBannerBackground[2]));
            if (image_) {
                const float iw = static_cast<float>(image_->GetWidth());
                const float ih = static_cast<float>(image_->GetHeight());
                const float scale = std::min(w / iw, h / ih);
                const float dw = iw * scale;
                const float dh = ih * scale;
                g.DrawImage(image_.get(), Gdiplus::RectF((w - dw) * 0.5f, (h - dh) * 0.5f, dw, dh));
            }
        } else {
            g.Clear(Gdiplus::Color(255, 10, 11, 14));

            // Imagen centrada (hasta el 60 % del ancho y el 55 % del alto).
            float bottom = h * 0.5f;
            if (image_) {
                const float iw = static_cast<float>(image_->GetWidth());
                const float ih = static_cast<float>(image_->GetHeight());
                const float scale = std::min({w * 0.6f / iw, h * 0.55f / ih, 1.0f});
                const float dw = iw * scale;
                const float dh = ih * scale;
                const float x = (w - dw) * 0.5f;
                const float y = (h - dh) * 0.45f;
                g.DrawImage(image_.get(), Gdiplus::RectF(x, y, dw, dh));
                bottom = y + dh;
            }

            // Barra de progreso.
            const float bar_w = std::min(w * 0.4f, 560.0f);
            const float bar_h = 6.0f;
            const float bx = (w - bar_w) * 0.5f;
            const float by = bottom + 36.0f;
            Gdiplus::SolidBrush track(Gdiplus::Color(255, 38, 40, 48));
            Gdiplus::SolidBrush fill(Gdiplus::Color(255, 0, 143, 242));
            g.FillRectangle(&track, bx, by, bar_w, bar_h);
            g.FillRectangle(&fill, bx, by, bar_w * fraction, bar_h);

            // Texto: "Compilando shaders   42 %".
            Gdiplus::FontFamily family(L"Segoe UI");
            Gdiplus::Font font(&family, 15.0f, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
            Gdiplus::SolidBrush ink(Gdiplus::Color(255, 170, 174, 186));
            Gdiplus::StringFormat format;
            format.SetAlignment(Gdiplus::StringAlignmentCenter);
            g.DrawString(text.c_str(), -1, &font, Gdiplus::RectF(0.0f, by + 16.0f, static_cast<float>(w), 30.0f), &format, &ink);
        }
    }
    BitBlt(dc, 0, 0, w, h, memory, 0, 0, SRCCOPY);
    SelectObject(memory, old);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(window_, dc);
}
#else
// -----------------------------------------------------------------------------
// Linux: Xlib (la ventana de X de dm::Window, antes de que Vulkan la use)
// -----------------------------------------------------------------------------

namespace {

// UTF-8 a puntos de codigo (wchar_t es UTF-32 en Linux).
std::wstring widen(const char* text) {
    std::wstring out;
    if (text == nullptr) return out;
    const auto* p = reinterpret_cast<const unsigned char*>(text);
    while (*p != 0) {
        char32_t c = *p;
        int extra = 0;
        if (c >= 0xF0) {
            c &= 0x07;
            extra = 3;
        } else if (c >= 0xE0) {
            c &= 0x0F;
            extra = 2;
        } else if (c >= 0xC0) {
            c &= 0x1F;
            extra = 1;
        }
        ++p;
        for (int i = 0; i < extra && (*p & 0xC0) == 0x80; ++i, ++p) c = (c << 6) | (*p & 0x3F);
        out.push_back(static_cast<wchar_t>(c));
    }
    return out;
}

// Desplazamiento del canal (mascara de 8 bits del visual TrueColor).
int maskShift(unsigned long mask) {
    int shift = 0;
    while (mask != 0 && (mask & 1u) == 0) {
        mask >>= 1;
        ++shift;
    }
    return shift;
}

struct PixelFormat {
    int r = 16;
    int g = 8;
    int b = 0;
    std::uint32_t pack(int red, int green, int blue) const {
        return (static_cast<std::uint32_t>(red) << r) | (static_cast<std::uint32_t>(green) << g) |
               (static_cast<std::uint32_t>(blue) << b);
    }
};

}  // namespace

LoadingScreen::LoadingScreen(HWND window, const std::filesystem::path& image, bool banner_only)
    : window_(window), banner_only_(banner_only) {
    std::error_code error;
    if (!image.empty() && std::filesystem::exists(image, error)) {
        asset::ImageRgba8 decoded;
        if (asset::loadImageRgba8(image, decoded)) {
            image_width_ = decoded.width;
            image_height_ = decoded.height;
            image_rgba_ = std::move(decoded.pixels);
        }
    }
    if (window_ != nullptr && window_->display != nullptr && window_->window != 0) {
        Display* display = static_cast<Display*>(window_->display);
        gc_ = XCreateGC(display, static_cast<::Window>(window_->window), 0, nullptr);
        // Fuente del servidor X (iso8859-1: los acentos del castellano); si
        // no hay Helvetica, la "fixed" que siempre esta.
        for (const char* name : {"-*-helvetica-medium-r-normal--15-*-*-*-*-*-iso8859-1",
                                 "-*-dejavu sans-medium-r-normal--15-*-*-*-*-*-iso8859-1", "fixed"}) {
            if (XFontStruct* font = XLoadQueryFont(display, name)) {
                font_ = font;
                break;
            }
        }
    }
}

LoadingScreen::~LoadingScreen() {
    if (window_ == nullptr || window_->display == nullptr) return;
    Display* display = static_cast<Display*>(window_->display);
    if (font_ != nullptr) XFreeFont(display, static_cast<XFontStruct*>(font_));
    if (gc_ != nullptr) XFreeGC(display, static_cast<GC>(gc_));
    XFlush(display);
}

void LoadingScreen::show(float fraction, const char* status) {
    // Los eventos de la ventana los atiende dm::Window (pumpEvents) en el
    // bucle de quien la tiene; aqui solo se pinta.
    const auto now = std::chrono::steady_clock::now();
    if (painted_ && fraction < 1.0f && now - last_paint_ < std::chrono::milliseconds(16)) return;
    if (banner_only_ && painted_ && now - last_paint_ < std::chrono::milliseconds(500)) {
        // El banner no cambia con el progreso: solo si la ventana cambio de
        // tamano (o de vez en cuando, por si algo la tapo).
        if (window_ != nullptr && window_->display != nullptr) {
            XWindowAttributes attributes{};
            if (XGetWindowAttributes(static_cast<Display*>(window_->display), static_cast<::Window>(window_->window),
                                     &attributes) != 0 &&
                attributes.width == painted_width_ && attributes.height == painted_height_) {
                return;
            }
        }
    }
    last_paint_ = now;
    painted_ = true;
    fraction = std::clamp(fraction, 0.0f, 1.0f);
    wchar_t text[160];
    std::swprintf(text, 160, L"%ls   %d %%", widen(status).c_str(), static_cast<int>(fraction * 100.0f + 0.5f));
    paint(fraction, text);
}

void LoadingScreen::paint(float fraction, const std::wstring& text) {
    if (window_ == nullptr || window_->display == nullptr || window_->window == 0 || gc_ == nullptr) return;
    Display* display = static_cast<Display*>(window_->display);
    const auto window = static_cast<::Window>(window_->window);
    XWindowAttributes attributes{};
    if (XGetWindowAttributes(display, window, &attributes) == 0 || attributes.map_state != IsViewable) return;
    const int w = attributes.width;
    const int h = attributes.height;
    if (w <= 0 || h <= 0) return;
    Visual* visual = attributes.visual;
    if (visual == nullptr || visual->c_class != TrueColor || attributes.depth < 24) return;  // solo color verdadero
    PixelFormat format;
    format.r = maskShift(visual->red_mask);
    format.g = maskShift(visual->green_mask);
    format.b = maskShift(visual->blue_mask);

    // Fondo + imagen (se recompone si cambio el tamano).
    if (w != painted_width_ || h != painted_height_ || background_.size() != static_cast<std::size_t>(w) * h) {
        painted_width_ = w;
        painted_height_ = h;
        const unsigned char* bg = banner_only_ ? kBannerBackground : nullptr;
        const int bg_r = bg != nullptr ? bg[0] : 10;
        const int bg_g = bg != nullptr ? bg[1] : 11;
        const int bg_b = bg != nullptr ? bg[2] : 14;
        background_.assign(static_cast<std::size_t>(w) * h, format.pack(bg_r, bg_g, bg_b));
        image_bottom_ = h * 0.5f;
        if (!image_rgba_.empty() && image_width_ > 0 && image_height_ > 0) {
            const float iw = static_cast<float>(image_width_);
            const float ih = static_cast<float>(image_height_);
            // Juego exportado: el banner ajustado a la ventana; editor: hasta
            // el 60 % del ancho y el 55 % del alto.
            const float scale = banner_only_ ? std::min(w / iw, h / ih) : std::min({w * 0.6f / iw, h * 0.55f / ih, 1.0f});
            const float dw = iw * scale;
            const float dh = ih * scale;
            const float x0 = (w - dw) * 0.5f;
            const float y0 = banner_only_ ? (h - dh) * 0.5f : (h - dh) * 0.45f;
            image_bottom_ = y0 + dh;
            const int px0 = std::max(0, static_cast<int>(std::floor(x0)));
            const int py0 = std::max(0, static_cast<int>(std::floor(y0)));
            const int px1 = std::min(w, static_cast<int>(std::ceil(x0 + dw)));
            const int py1 = std::min(h, static_cast<int>(std::ceil(y0 + dh)));
            const auto texel = [&](int x, int y, int c) {
                x = std::clamp(x, 0, static_cast<int>(image_width_) - 1);
                y = std::clamp(y, 0, static_cast<int>(image_height_) - 1);
                return static_cast<float>(image_rgba_[(static_cast<std::size_t>(y) * image_width_ + x) * 4 + c]);
            };
            for (int py = py0; py < py1; ++py) {
                // Muestreo bilineal (centro del pixel en la imagen).
                const float v = (py + 0.5f - y0) / scale - 0.5f;
                const int ty = static_cast<int>(std::floor(v));
                const float fy = v - ty;
                for (int px = px0; px < px1; ++px) {
                    const float u = (px + 0.5f - x0) / scale - 0.5f;
                    const int tx = static_cast<int>(std::floor(u));
                    const float fx = u - tx;
                    float rgba[4];
                    for (int c = 0; c < 4; ++c) {
                        const float top = texel(tx, ty, c) * (1.0f - fx) + texel(tx + 1, ty, c) * fx;
                        const float bottom = texel(tx, ty + 1, c) * (1.0f - fx) + texel(tx + 1, ty + 1, c) * fx;
                        rgba[c] = top * (1.0f - fy) + bottom * fy;
                    }
                    const float a = rgba[3] / 255.0f;
                    const int r = static_cast<int>(rgba[0] * a + bg_r * (1.0f - a) + 0.5f);
                    const int g = static_cast<int>(rgba[1] * a + bg_g * (1.0f - a) + 0.5f);
                    const int b = static_cast<int>(rgba[2] * a + bg_b * (1.0f - a) + 0.5f);
                    background_[static_cast<std::size_t>(py) * w + px] =
                        format.pack(std::clamp(r, 0, 255), std::clamp(g, 0, 255), std::clamp(b, 0, 255));
                }
            }
        }
    }

    std::vector<std::uint32_t> frame = background_;
    int text_y = 0;
    if (!banner_only_) {
        // Barra de progreso.
        const int bar_w = static_cast<int>(std::min(w * 0.4f, 560.0f));
        const int bar_h = 6;
        const int bx = (w - bar_w) / 2;
        const int by = static_cast<int>(image_bottom_ + 36.0f);
        const int filled = static_cast<int>(bar_w * fraction + 0.5f);
        const std::uint32_t track = format.pack(38, 40, 48);
        const std::uint32_t fill = format.pack(0, 143, 242);
        for (int y = std::max(by, 0); y < std::min(by + bar_h, h); ++y) {
            for (int x = std::max(bx, 0); x < std::min(bx + bar_w, w); ++x) {
                frame[static_cast<std::size_t>(y) * w + x] = x - bx < filled ? fill : track;
            }
        }
        text_y = by + 16;
    }

    XImage* image = XCreateImage(display, visual, static_cast<unsigned>(attributes.depth), ZPixmap, 0,
                                 reinterpret_cast<char*>(frame.data()), static_cast<unsigned>(w), static_cast<unsigned>(h), 32,
                                 w * 4);
    if (image == nullptr) return;
    if (image->bits_per_pixel == 32) {
        XPutImage(display, window, static_cast<GC>(gc_), image, 0, 0, 0, 0, static_cast<unsigned>(w), static_cast<unsigned>(h));
    }
    image->data = nullptr;  // el buffer es del vector
    XDestroyImage(image);

    // Texto: "Compilando shaders   42 %" (Latin-1: la fuente es iso8859-1).
    if (!banner_only_ && font_ != nullptr) {
        auto* font = static_cast<XFontStruct*>(font_);
        std::string latin;
        for (const wchar_t c : text) latin.push_back(c < 256 ? static_cast<char>(c) : '?');
        const int text_w = XTextWidth(font, latin.c_str(), static_cast<int>(latin.size()));
        XSetFont(display, static_cast<GC>(gc_), font->fid);
        XSetForeground(display, static_cast<GC>(gc_), format.pack(170, 174, 186));
        XDrawString(display, window, static_cast<GC>(gc_), (w - text_w) / 2, text_y + font->ascent, latin.c_str(),
                    static_cast<int>(latin.size()));
    }
    XFlush(display);
}
#endif

// -----------------------------------------------------------------------------
// Crashes
// -----------------------------------------------------------------------------

std::filesystem::path localDataFolder(const std::filesystem::path& sub) {
#if !defined(_WIN32)
    // Linux: $LOCALAPPDATA si esta (pruebas), si no $XDG_DATA_HOME (o
    // ~/.local/share), como las demas aplicaciones.
    std::filesystem::path base;
    if (const char* local = std::getenv("LOCALAPPDATA"); local != nullptr && *local != '\0') {
        base = local;
    } else if (const char* data = std::getenv("XDG_DATA_HOME"); data != nullptr && *data == '/') {
        base = data;
    } else if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        base = std::filesystem::path(home) / ".local" / "share";
    } else {
        std::error_code error;
        base = std::filesystem::temp_directory_path(error);
    }
    const std::filesystem::path folder = base / "Cramion" / sub;
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    return folder;
#else
    std::filesystem::path base;
    if (const char* local = std::getenv("LOCALAPPDATA"); local != nullptr && *local != '\0') {
        base = local;
    } else {
        std::error_code error;
        base = std::filesystem::temp_directory_path(error);
    }
    const std::filesystem::path folder = base / "Cramion" / sub;
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    return folder;
#endif
}

namespace {

std::string g_app_name = "Cramion";
std::filesystem::path g_crash_folder;

// Ultimas lineas del registro (migas de pan) y datos del contexto (version,
// GPU, escena...). El manejador solo las lee si puede coger el mutex: un
// cierre con el mutex cogido no se queda colgado.
constexpr std::size_t kBreadcrumbs = 80;
std::mutex g_crumb_mutex;
std::array<std::string, kBreadcrumbs> g_crumbs;
std::size_t g_crumb_next = 0;
std::size_t g_crumb_count = 0;
std::mutex g_context_mutex;
std::vector<std::pair<std::string, std::string>> g_context;
std::filesystem::path g_log_file;

#if defined(_WIN32)
std::string hex(std::uint64_t v) {
    char b[32];
    std::snprintf(b, sizeof(b), "0x%llx", static_cast<unsigned long long>(v));
    return b;
}
#endif

#if defined(_WIN32)
const char* exceptionName(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION: return "acceso a memoria no valida (ACCESS_VIOLATION)";
        case EXCEPTION_STACK_OVERFLOW: return "desbordamiento de pila (STACK_OVERFLOW)";
        case EXCEPTION_INT_DIVIDE_BY_ZERO: return "division entera por cero";
        case EXCEPTION_ILLEGAL_INSTRUCTION: return "instruccion ilegal";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "fuera de los limites de un array";
        case EXCEPTION_IN_PAGE_ERROR: return "error de pagina (disco o archivo mapeado)";
        case 0xC0000409: return "fallo rapido (__fastfail / pila corrupta)";
        case 0xE0000001: return "std::terminate (excepcion de C++ sin capturar)";
        case 0xE0000002: return "abort()";
        case 0xE0000003: return "llamada a una funcion virtual pura";
        case 0xE0000004: return "parametro no valido en la biblioteca de C";
        case 0xE06D7363: return "excepcion de C++ sin capturar";
        default: return "excepcion";
    }
}
#endif

std::string tailOfFile(const std::filesystem::path& file, std::size_t max_bytes) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return {};
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    const std::streamoff from = size > static_cast<std::streamoff>(max_bytes) ? size - static_cast<std::streamoff>(max_bytes) : 0;
    in.seekg(from);
    std::string out(static_cast<std::size_t>(size - from), '\0');
    in.read(out.data(), static_cast<std::streamsize>(out.size()));
    return out;
}

#if defined(_WIN32)
// Escribe el minidump y el informe; devuelve la ruta del informe.
std::filesystem::path writeReport(EXCEPTION_POINTERS* info, const char* reason) {
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    void* address = info->ExceptionRecord->ExceptionAddress;
    wchar_t module_path[MAX_PATH] = L"?";
    HMODULE module = nullptr;
    std::uintptr_t offset = 0;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCWSTR>(address), &module)) {
        GetModuleFileNameW(module, module_path, MAX_PATH);
        offset = reinterpret_cast<std::uintptr_t>(address) - reinterpret_cast<std::uintptr_t>(module);
    }
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &local);
    const std::filesystem::path base = g_crash_folder / (g_app_name + "_" + stamp);

    // Minidump (se abre con Visual Studio / WinDbg / lldb).
    std::filesystem::path dump = base;
    dump += ".dmp";
    HANDLE file = CreateFileW(dump.wstring().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION exception{};
        exception.ThreadId = GetCurrentThreadId();
        exception.ExceptionPointers = info;
        exception.ClientPointers = FALSE;
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                          static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo |
                                                     MiniDumpWithUnloadedModules),
                          &exception, nullptr, nullptr);
        CloseHandle(file);
    }
    const std::filesystem::path module_name = std::filesystem::path(module_path).filename();

    // Pila con nombres de funcion y lineas (el .pdb junto al .exe; sin el,
    // modulo + desplazamiento: llvm-symbolizer --obj=X.exe 0x140000000+desp).
    std::string stack;
    std::string top_function;
    {
        const HANDLE process = GetCurrentProcess();
        SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS);
        wchar_t exe[MAX_PATH] = L"";
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        const std::string search = std::filesystem::path(exe).parent_path().string();
        SymInitialize(process, search.c_str(), TRUE);
        CONTEXT context = *info->ContextRecord;
        STACKFRAME64 frame{};
        frame.AddrPC.Offset = context.Rip;
        frame.AddrPC.Mode = AddrModeFlat;
        frame.AddrFrame.Offset = context.Rbp;
        frame.AddrFrame.Mode = AddrModeFlat;
        frame.AddrStack.Offset = context.Rsp;
        frame.AddrStack.Mode = AddrModeFlat;
        alignas(SYMBOL_INFO) char symbol_buffer[sizeof(SYMBOL_INFO) + 512];
        for (int i = 0; i < 48; ++i) {
            if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, GetCurrentThread(), &frame, &context, nullptr,
                             SymFunctionTableAccess64, SymGetModuleBase64, nullptr) ||
                frame.AddrPC.Offset == 0) {
                break;
            }
            HMODULE frame_module = nullptr;
            wchar_t frame_path[MAX_PATH] = L"?";
            std::uintptr_t frame_offset = frame.AddrPC.Offset;
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCWSTR>(frame.AddrPC.Offset), &frame_module)) {
                GetModuleFileNameW(frame_module, frame_path, MAX_PATH);
                frame_offset -= reinterpret_cast<std::uintptr_t>(frame_module);
            }
            std::string function;
            auto* symbol = reinterpret_cast<SYMBOL_INFO*>(symbol_buffer);
            std::memset(symbol_buffer, 0, sizeof(symbol_buffer));
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen = 511;
            DWORD64 displacement = 0;
            if (SymFromAddr(process, frame.AddrPC.Offset, &displacement, symbol)) function = symbol->Name;
            std::string where;
            IMAGEHLP_LINE64 line_info{};
            line_info.SizeOfStruct = sizeof(line_info);
            DWORD line_displacement = 0;
            if (SymGetLineFromAddr64(process, frame.AddrPC.Offset, &line_displacement, &line_info) &&
                line_info.FileName != nullptr) {
                where = std::filesystem::path(line_info.FileName).filename().string() + ":" +
                        std::to_string(line_info.LineNumber);
            }
            if (top_function.empty() && !function.empty()) top_function = function + (where.empty() ? "" : " (" + where + ")");
            char line[1024];
            std::snprintf(line, sizeof(line), "  #%-2d %s + 0x%llx  %s%s%s%s\n", i,
                          std::filesystem::path(frame_path).filename().string().c_str(),
                          static_cast<unsigned long long>(frame_offset), function.c_str(), where.empty() ? "" : "  [",
                          where.c_str(), where.empty() ? "" : "]");
            stack += line;
        }
        SymCleanup(process);
    }

    // Sistema: Windows, memoria, procesador.
    std::string system;
    {
        MEMORYSTATUSEX mem{};
        mem.dwLength = sizeof(mem);
        GlobalMemoryStatusEx(&mem);
        SYSTEM_INFO si{};
        GetNativeSystemInfo(&si);
        char line[256];
        std::snprintf(line, sizeof(line), "RAM: %.1f GB libres de %.1f GB (%lu%% en uso)\nNucleos: %lu\n",
                      static_cast<double>(mem.ullAvailPhys) / (1024.0 * 1024.0 * 1024.0),
                      static_cast<double>(mem.ullTotalPhys) / (1024.0 * 1024.0 * 1024.0), mem.dwMemoryLoad,
                      si.dwNumberOfProcessors);
        system = line;
        PROCESS_MEMORY_COUNTERS_EX pmc{};
        pmc.cb = sizeof(pmc);
        if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
            std::snprintf(line, sizeof(line), "Memoria del proceso: %.0f MB (pico %.0f MB)\n",
                          static_cast<double>(pmc.PrivateUsage) / (1024.0 * 1024.0),
                          static_cast<double>(pmc.PeakWorkingSetSize) / (1024.0 * 1024.0));
            system += line;
        }
    }

    std::filesystem::path report = base;
    report += ".txt";
    {
        std::ofstream out(report);
        out << g_app_name << " se cerro por un error\n"
            << "Fecha: " << stamp << "\n"
            << "Motivo: " << (reason != nullptr ? reason : exceptionName(code)) << "\n"
            << "Codigo: " << hex(code) << "\n"
            << "Modulo: " << module_name.string() << " + " << hex(offset) << "\n";
        if (!top_function.empty()) out << "Funcion: " << top_function << "\n";
        if (code == EXCEPTION_ACCESS_VIOLATION && info->ExceptionRecord->NumberParameters >= 2) {
            const ULONG_PTR kind = info->ExceptionRecord->ExceptionInformation[0];
            out << "Acceso: " << (kind == 0 ? "lectura" : kind == 1 ? "escritura" : "ejecucion") << " en "
                << hex(info->ExceptionRecord->ExceptionInformation[1])
                << (info->ExceptionRecord->ExceptionInformation[1] < 0x10000 ? " (puntero nulo)" : "") << "\n";
        }
        out << "Minidump: " << dump.string() << "\n\n";
        out << "--- Contexto ---\n";
        if (g_context_mutex.try_lock()) {
            for (const auto& [k, v] : g_context) out << k << ": " << v << "\n";
            g_context_mutex.unlock();
        }
        out << system << "\n--- Pila ---\n" << stack << "\n--- Ultimas lineas del registro ---\n";
        if (g_crumb_mutex.try_lock()) {
            const std::size_t first = (g_crumb_next + kBreadcrumbs - g_crumb_count) % kBreadcrumbs;
            for (std::size_t i = 0; i < g_crumb_count; ++i) out << g_crumbs[(first + i) % kBreadcrumbs] << "\n";
            g_crumb_mutex.unlock();
        }
        if (g_crumb_count == 0 && !g_log_file.empty()) out << tailOfFile(g_log_file, 6000);
    }
    // Marca para avisar en el siguiente arranque.
    {
        std::ofstream marker(g_crash_folder / (g_app_name + ".pending"));
        marker << report.string();
    }
    std::cerr << "[Crash] " << hex(code) << " en " << module_name.string() << " + " << hex(offset);
    if (!top_function.empty()) std::cerr << " (" << top_function << ")";
    std::cerr << '\n' << stack;
    std::cerr.flush();
    return report;
}

void showCrashMessage(DWORD code, const std::filesystem::path& report) {
    wchar_t message[1400];
    std::swprintf(message, 1400,
                  L"%hs se cerro por un error (%hs, 0x%08lX).\n\nSe guardo un informe con la pila y un minidump en:\n%ls\n\n"
                  L"Al volver a abrirlo podras verlo, copiarlo o abrir su carpeta.",
                  g_app_name.c_str(), exceptionName(code), code, report.wstring().c_str());
    MessageBoxW(nullptr, message, L"Cramion", MB_ICONERROR | MB_OK | MB_TOPMOST);
}

std::atomic<LONG> g_entered{0};

LONG WINAPI onCrash(EXCEPTION_POINTERS* info) {
    if (g_entered.exchange(1) != 0) return EXCEPTION_CONTINUE_SEARCH;
    const std::filesystem::path report = writeReport(info, nullptr);
    showCrashMessage(info->ExceptionRecord->ExceptionCode, report);
    return EXCEPTION_EXECUTE_HANDLER;
}

// Cierres que no son excepciones de SEH (terminate, abort, virtual pura,
// parametro no valido): informe con el contexto de aqui y fuera.
[[noreturn]] void crashWithCode(DWORD code, const char* reason) {
    if (g_entered.exchange(1) == 0) {
        CONTEXT context{};
        RtlCaptureContext(&context);
        EXCEPTION_RECORD record{};
        record.ExceptionCode = code;
        record.ExceptionAddress = reinterpret_cast<void*>(context.Rip);
        EXCEPTION_POINTERS pointers{&record, &context};
        const std::filesystem::path report = writeReport(&pointers, reason);
        showCrashMessage(code, report);
    }
    TerminateProcess(GetCurrentProcess(), code);
    std::abort();
}

void onTerminate() {
    std::string reason = "std::terminate (excepcion de C++ sin capturar)";
    if (const std::exception_ptr current = std::current_exception()) {
        try {
            std::rethrow_exception(current);
        } catch (const std::exception& e) {
            reason += ": ";
            reason += e.what();
        } catch (...) {
        }
    }
    static std::string kept;
    kept = reason;
    crashWithCode(0xE0000001, kept.c_str());
}

void onAbortSignal(int) { crashWithCode(0xE0000002, nullptr); }
void onPureCall() { crashWithCode(0xE0000003, nullptr); }
void onInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t) {
    crashWithCode(0xE0000004, nullptr);
}

}  // namespace

void installCrashHandler(const std::string& app_name) {
    g_app_name = app_name.empty() ? std::string("Cramion") : app_name;
    for (char& c : g_app_name) {
        if (c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' || c == '|' || c == '?' || c == '*') c = '_';
    }
    g_crash_folder = localDataFolder("Crashes");
    SetUnhandledExceptionFilter(&onCrash);
    std::set_terminate(&onTerminate);
    std::signal(SIGABRT, &onAbortSignal);
    _set_purecall_handler(&onPureCall);
    _set_invalid_parameter_handler(&onInvalidParameter);
    // Reserva pila para el manejador si la pila se desborda.
    ULONG guarantee = 64 * 1024;
    SetThreadStackGuarantee(&guarantee);
    setCrashContext("Aplicacion", g_app_name);
}
#else
}  // namespace

// Linux: manejador de senales (SIGSEGV, SIGABRT, SIGFPE, SIGILL, SIGBUS). En
// la senal solo se usa lo que se puede llamar ahi (open/write/backtrace con
// todo preparado al instalar); las migas y el contexto, si se coge el mutex.
// Sin minidump: el informe lleva la pila (backtrace_symbols_fd; con -rdynamic
// o el binario con simbolos, addr2line -e <exe> <direccion> da la linea).
namespace {

constexpr int kCrashSignals[] = {SIGSEGV, SIGABRT, SIGFPE, SIGILL, SIGBUS};
struct sigaction g_previous[64] = {};
char g_crash_dir[2048] = "";  // g_crash_folder ya en char (sin memoria en la senal)
char g_log_path[2048] = "";
char g_terminate_reason[512] = "";
std::atomic<int> g_entered{0};

// Escritura sin buffer (de la senal).
void put(int fd, const char* text) {
    if (fd < 0 || text == nullptr) return;
    std::size_t size = std::strlen(text);
    while (size > 0) {
        const ssize_t done = ::write(fd, text, size);
        if (done < 0 && errno == EINTR) continue;
        if (done <= 0) return;
        text += done;
        size -= static_cast<std::size_t>(done);
    }
}

void putHex(int fd, std::uintptr_t v) {
    char b[2 + 16 + 1];
    char* p = b + sizeof(b) - 1;
    *p = '\0';
    do {
        *--p = "0123456789abcdef"[v & 0xF];
        v >>= 4;
    } while (v != 0);
    *--p = 'x';
    *--p = '0';
    put(fd, p);
}

void putNumber(int fd, long long v) {
    char b[24];
    char* p = b + sizeof(b) - 1;
    *p = '\0';
    const bool negative = v < 0;
    unsigned long long u = negative ? 0ull - static_cast<unsigned long long>(v) : static_cast<unsigned long long>(v);
    do {
        *--p = static_cast<char>('0' + u % 10);
        u /= 10;
    } while (u != 0);
    if (negative) *--p = '-';
    put(fd, p);
}

// Copia las lineas de un archivo de /proc que empiezan por alguno de los prefijos.
void putProcLines(int fd, const char* file, std::initializer_list<const char*> prefixes) {
    const int in = ::open(file, O_RDONLY | O_CLOEXEC);
    if (in < 0) return;
    char buffer[4096];
    const ssize_t n = ::read(in, buffer, sizeof(buffer) - 1);
    ::close(in);
    if (n <= 0) return;
    buffer[n] = '\0';
    char* line = buffer;
    while (*line != '\0') {
        char* end = std::strchr(line, '\n');
        if (end != nullptr) *end = '\0';
        for (const char* prefix : prefixes) {
            if (std::strncmp(line, prefix, std::strlen(prefix)) == 0) {
                put(fd, line);
                put(fd, "\n");
            }
        }
        if (end == nullptr) break;
        line = end + 1;
    }
}

const char* signalName(int signal, int code) {
    switch (signal) {
        case SIGSEGV: return "acceso a memoria no valida (SIGSEGV)";
        case SIGBUS: return "error de bus (SIGBUS: archivo mapeado o acceso desalineado)";
        case SIGILL: return "instruccion ilegal (SIGILL)";
        case SIGFPE: return code == FPE_INTDIV ? "division entera por cero (SIGFPE)" : "error aritmetico (SIGFPE)";
        case SIGABRT: return "abort() (SIGABRT)";
        default: return "senal";
    }
}

// Cuadro de error del escritorio (zenity, kdialog o xmessage) en otro
// proceso; se espera a que se cierre, como el MessageBox de Windows.
void showCrashMessage(const char* reason, const char* report) {
    if (std::getenv("DISPLAY") == nullptr && std::getenv("WAYLAND_DISPLAY") == nullptr) return;
    char message[3072];
    std::snprintf(message, sizeof(message),
                  "%s se cerro por un error (%s).\n\nSe guardo un informe con la pila en:\n%s\n\n"
                  "Al volver a abrirlo podras verlo, copiarlo o abrir su carpeta.",
                  g_app_name.c_str(), reason, report);
    const pid_t child = fork();
    if (child == 0) {
        execlp("zenity", "zenity", "--error", "--title=Cramion", "--no-markup", "--text", message, static_cast<char*>(nullptr));
        execlp("kdialog", "kdialog", "--title", "Cramion", "--error", message, static_cast<char*>(nullptr));
        execlp("xmessage", "xmessage", "-center", message, static_cast<char*>(nullptr));
        _exit(127);
    }
    if (child > 0) {
        int status = 0;
        while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
        }
    }
}

void onCrashSignal(int signal, siginfo_t* info, void* context) {
    if (g_entered.exchange(1) == 0) {
        void* frames[64];
        const int count = backtrace(frames, 64);
        // Donde fallo: el contador de programa del contexto (si no, el
        // primer marco de la pila).
        void* pc = nullptr;
#if defined(__x86_64__)
        if (context != nullptr) pc = reinterpret_cast<void*>(static_cast<ucontext_t*>(context)->uc_mcontext.gregs[REG_RIP]);
#elif defined(__aarch64__)
        if (context != nullptr) pc = reinterpret_cast<void*>(static_cast<ucontext_t*>(context)->uc_mcontext.pc);
#else
        (void)context;
#endif
        if (pc == nullptr && count > 0) pc = frames[0];
        Dl_info where{};
        const bool has_module = pc != nullptr && dladdr(pc, &where) != 0;

        const std::time_t now = std::time(nullptr);
        std::tm local{};
        localtime_r(&now, &local);
        char stamp[32];
        std::strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &local);
        char report[2600];
        std::snprintf(report, sizeof(report), "%s/%s_%s.txt", g_crash_dir, g_app_name.c_str(), stamp);
        const char* reason = signal == SIGABRT && g_terminate_reason[0] != '\0'
                                 ? g_terminate_reason
                                 : signalName(signal, info != nullptr ? info->si_code : 0);
        const int fd = ::open(report, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd >= 0) {
            put(fd, g_app_name.c_str());
            put(fd, " se cerro por un error\nFecha: ");
            put(fd, stamp);
            put(fd, "\nMotivo: ");
            put(fd, reason);
            put(fd, "\nCodigo: senal ");
            putNumber(fd, signal);
            put(fd, "\nModulo: ");
            if (has_module) {
                put(fd, where.dli_fname != nullptr ? where.dli_fname : "?");
                put(fd, " + ");
                putHex(fd, reinterpret_cast<std::uintptr_t>(pc) - reinterpret_cast<std::uintptr_t>(where.dli_fbase));
                if (where.dli_sname != nullptr) {
                    put(fd, "\nFuncion: ");
                    put(fd, where.dli_sname);
                }
            } else {
                putHex(fd, reinterpret_cast<std::uintptr_t>(pc));
            }
            if ((signal == SIGSEGV || signal == SIGBUS) && info != nullptr) {
                const auto address = reinterpret_cast<std::uintptr_t>(info->si_addr);
                put(fd, "\nAcceso: ");
                putHex(fd, address);
                if (address < 0x10000) put(fd, " (puntero nulo)");
            }
            put(fd, "\n\n--- Contexto ---\n");
            if (g_context_mutex.try_lock()) {
                for (const auto& [k, v] : g_context) {
                    put(fd, k.c_str());
                    put(fd, ": ");
                    put(fd, v.c_str());
                    put(fd, "\n");
                }
                g_context_mutex.unlock();
            }
            putProcLines(fd, "/proc/meminfo", {"MemTotal:", "MemAvailable:"});
            putProcLines(fd, "/proc/self/status", {"VmRSS:", "VmHWM:", "Threads:"});
            put(fd, "\n--- Pila ---\n");
            backtrace_symbols_fd(frames, count, fd);
            put(fd, "\n--- Ultimas lineas del registro ---\n");
            bool crumbs = false;
            if (g_crumb_mutex.try_lock()) {
                const std::size_t first = (g_crumb_next + kBreadcrumbs - g_crumb_count) % kBreadcrumbs;
                for (std::size_t i = 0; i < g_crumb_count; ++i) {
                    put(fd, g_crumbs[(first + i) % kBreadcrumbs].c_str());
                    put(fd, "\n");
                }
                crumbs = g_crumb_count > 0;
                g_crumb_mutex.unlock();
            }
            if (!crumbs && g_log_path[0] != '\0') {
                // El final del registro (6000 bytes).
                const int log = ::open(g_log_path, O_RDONLY | O_CLOEXEC);
                if (log >= 0) {
                    const off_t size = ::lseek(log, 0, SEEK_END);
                    ::lseek(log, size > 6000 ? size - 6000 : 0, SEEK_SET);
                    char buffer[6001];
                    const ssize_t n = ::read(log, buffer, 6000);
                    if (n > 0) {
                        buffer[n] = '\0';
                        put(fd, buffer);
                    }
                    ::close(log);
                }
            }
            ::close(fd);
            // Marca para avisar en el siguiente arranque.
            char marker[2600];
            std::snprintf(marker, sizeof(marker), "%s/%s.pending", g_crash_dir, g_app_name.c_str());
            const int mfd = ::open(marker, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
            if (mfd >= 0) {
                put(mfd, report);
                ::close(mfd);
            }
        }
        put(2, "[Crash] ");
        put(2, reason);
        if (has_module && where.dli_sname != nullptr) {
            put(2, " (");
            put(2, where.dli_sname);
            put(2, ")");
        }
        put(2, "\n");
        backtrace_symbols_fd(frames, count, 2);
        showCrashMessage(reason, report);
    }
    // El del sistema (volcado de memoria si esta activado) con la misma senal.
    struct sigaction fallback {};
    fallback.sa_handler = SIG_DFL;
    sigemptyset(&fallback.sa_mask);
    sigaction(signal, &fallback, nullptr);
    raise(signal);
}

// std::terminate: el motivo (la excepcion) y abort(), que entra por SIGABRT.
void onTerminate() {
    std::string reason = "std::terminate (excepcion de C++ sin capturar)";
    if (const std::exception_ptr current = std::current_exception()) {
        try {
            std::rethrow_exception(current);
        } catch (const std::exception& e) {
            reason += ": ";
            reason += e.what();
        } catch (...) {
        }
    }
    std::snprintf(g_terminate_reason, sizeof(g_terminate_reason), "%s", reason.c_str());
    std::abort();
}

}  // namespace

void installCrashHandler(const std::string& app_name) {
    g_app_name = app_name.empty() ? std::string("Cramion") : app_name;
    for (char& c : g_app_name) {
        if (c == '/' || c == '\\' || c == '\0') c = '_';
    }
    g_crash_folder = localDataFolder("Crashes");
    std::snprintf(g_crash_dir, sizeof(g_crash_dir), "%s", g_crash_folder.c_str());
    // backtrace() carga libgcc la primera vez (con memoria): ahora, no en la senal.
    void* warm[4];
    backtrace(warm, 4);
    // Pila aparte para el manejador: un desbordamiento de pila tambien llega.
    static std::vector<char> alternate(256 * 1024);
    stack_t stack{};
    stack.ss_sp = alternate.data();
    stack.ss_size = alternate.size();
    sigaltstack(&stack, nullptr);
    std::set_terminate(&onTerminate);
    struct sigaction action {};
    action.sa_sigaction = &onCrashSignal;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    for (const int signal : kCrashSignals) sigaction(signal, &action, &g_previous[signal]);
    setCrashContext("Aplicacion", g_app_name);
}
#endif

void crashBreadcrumb(const std::string& line) {
    std::lock_guard<std::mutex> lock(g_crumb_mutex);
    g_crumbs[g_crumb_next] = line.size() > 400 ? line.substr(0, 400) + "..." : line;
    g_crumb_next = (g_crumb_next + 1) % kBreadcrumbs;
    g_crumb_count = std::min(g_crumb_count + 1, kBreadcrumbs);
}

void setCrashContext(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lock(g_context_mutex);
    for (auto& [k, v] : g_context) {
        if (k == key) {
            v = value;
            return;
        }
    }
    g_context.emplace_back(key, value);
}

void setCrashLogFile(const std::filesystem::path& file) {
    g_log_file = file;
#if !defined(_WIN32)
    std::snprintf(g_log_path, sizeof(g_log_path), "%s", file.c_str());  // lo lee la senal
#endif
}

std::filesystem::path pendingCrashReport(const std::string& app_name) {
    std::string name = app_name.empty() ? g_app_name : app_name;
    const std::filesystem::path marker = localDataFolder("Crashes") / (name + ".pending");
    std::ifstream in(marker);
    if (!in) return {};
    std::string line;
    std::getline(in, line);
    in.close();
    std::error_code ec;
    std::filesystem::remove(marker, ec);
    const std::filesystem::path report = std::filesystem::path(std::u8string(line.begin(), line.end()));
    return std::filesystem::exists(report, ec) ? report : std::filesystem::path{};
}

std::string readCrashReport(const std::filesystem::path& report) { return tailOfFile(report, 64 * 1024); }

void testCrash(int kind) {
    switch (kind) {
        case 1: std::abort();
        case 2: std::terminate();
        default: {
            volatile int* p = nullptr;
            *p = 42;  // acceso no valido
            break;
        }
    }
}

}  // namespace cramion::editor
