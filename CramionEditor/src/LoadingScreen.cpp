#include "LoadingScreen.h"

#include <algorithm>

#include <objidl.h>
namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#include <gdiplus.h>
#include <dbghelp.h>

#include <cstdio>
#include <cstdlib>
#include <ctime>
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

LONG WINAPI onCrash(EXCEPTION_POINTERS* info) {
    static volatile LONG entered = 0;
    if (InterlockedExchange(&entered, 1) != 0) return EXCEPTION_CONTINUE_SEARCH;

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
                          static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo),
                          &exception, nullptr, nullptr);
        CloseHandle(file);
    }
    const std::filesystem::path module_name = std::filesystem::path(module_path).filename();
    std::filesystem::path report = base;
    report += ".txt";
    {
        std::ofstream out(report);
        out << g_app_name << " se cerro por un error\n"
            << "Codigo: 0x" << std::hex << code << "\n"
            << "Modulo: " << module_name.string() << " + 0x" << offset << std::dec << "\n"
            << "Minidump: " << dump.string() << "\n";
    }
    std::cerr << "[Crash] 0x" << std::hex << code << " en " << module_name.string() << " + 0x" << offset << std::dec << '\n';
    std::cerr.flush();

    wchar_t message[1024];
    std::swprintf(message, 1024,
                  L"%hs se cerro por un error (0x%08lX en %ls + 0x%llX).\n\nSe guardo un informe en:\n%ls",
                  g_app_name.c_str(), code, module_name.wstring().c_str(), static_cast<unsigned long long>(offset),
                  g_crash_folder.wstring().c_str());
    MessageBoxW(nullptr, message, L"Cramion", MB_ICONERROR | MB_OK | MB_TOPMOST);
    return EXCEPTION_EXECUTE_HANDLER;
}

}  // namespace

void installCrashHandler(const std::string& app_name) {
    g_app_name = app_name.empty() ? std::string("Cramion") : app_name;
    for (char& c : g_app_name) {
        if (c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' || c == '|' || c == '?' || c == '*') c = '_';
    }
    g_crash_folder = localDataFolder("Crashes");
    SetUnhandledExceptionFilter(&onCrash);
}

}  // namespace cramion::editor
