#ifndef CRAMION_EDITOR_PLATFORM_WINDOW_H
#define CRAMION_EDITOR_PLATFORM_WINDOW_H

// La ventana del sistema para lo que comparten el editor y el juego
// exportado: HWND en Windows; en Android (el player) la ANativeWindow y en
// Linux la conexion + ventana de X (dm::NativeHandle, lo que devuelve
// dm::Window::handle()), con el mismo nombre para no llenar de #if las firmas.

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__ANDROID__)
struct ANativeWindow;
using HWND = ANativeWindow*;
#else
#include <CramionDM/Window.h>
#include <CramionFX/vk/VulkanCommon.h>

using HWND = const cramion::dm::NativeHandle*;

// Los tipos de Win32 que usan las firmas del editor: rectangulo (regiones
// del cursor y de la vista) y punto en pixeles.
using LONG = long;
using RECT = cramion::dm::Rect;
struct POINT {
    long x = 0;
    long y = 0;
};

namespace cramion::editor {
// La ventana como la recibe el renderizador (gfx::XlibWindow: el mismo
// formato que dm::NativeHandle).
inline gfx::NativeWindow renderWindow(HWND window) {
    static_assert(sizeof(gfx::XlibWindow) == sizeof(dm::NativeHandle));
    return reinterpret_cast<gfx::NativeWindow>(window);
}
}  // namespace cramion::editor
#endif

#if !defined(__linux__) || defined(__ANDROID__)
namespace cramion::editor {
inline HWND renderWindow(HWND window) { return window; }
}  // namespace cramion::editor
#endif

#endif  // CRAMION_EDITOR_PLATFORM_WINDOW_H
