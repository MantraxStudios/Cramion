#ifndef CRAMION_EDITOR_PLATFORM_WINDOW_H
#define CRAMION_EDITOR_PLATFORM_WINDOW_H

// La ventana del sistema para lo que comparten el editor y el juego
// exportado: HWND en Windows; en Android (el player) la ANativeWindow, con el
// mismo nombre para no llenar de #if las firmas.

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
struct ANativeWindow;
using HWND = ANativeWindow*;
#endif

#endif  // CRAMION_EDITOR_PLATFORM_WINDOW_H
