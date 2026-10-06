#include "LoadingScreen.h"

#include <algorithm>

#include <objidl.h>
namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#include <gdiplus.h>
#include <dbghelp.h>
#include <psapi.h>

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

namespace cramion::editor {

namespace {

std::wstring widen(const char* text) {
    if (text == nullptr || *text == '\0') return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    std::wstring out(static_cast<std::size_t>(std::max(n - 1, 0)), L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, text, -1, out.data(), n);
    return out;
}

}  // namespace

LoadingScreen::LoadingScreen(HWND window, const std::filesystem::path& image) : window_(window) {
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

    HDC dc = GetDC(window_);
    HDC memory = CreateCompatibleDC(dc);
    HBITMAP bitmap = CreateCompatibleBitmap(dc, w, h);
    HGDIOBJ old = SelectObject(memory, bitmap);
    {
        Gdiplus::Graphics g(memory);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
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
    BitBlt(dc, 0, 0, w, h, memory, 0, 0, SRCCOPY);
    SelectObject(memory, old);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(window_, dc);
}

// -----------------------------------------------------------------------------
// Crashes
// -----------------------------------------------------------------------------

std::filesystem::path localDataFolder(const std::filesystem::path& sub) {
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

std::string hex(std::uint64_t v) {
    char b[32];
    std::snprintf(b, sizeof(b), "0x%llx", static_cast<unsigned long long>(v));
    return b;
}

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

void setCrashLogFile(const std::filesystem::path& file) { g_log_file = file; }

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
