#ifndef CRAMION_EDITOR_LOADING_SCREEN_H
#define CRAMION_EDITOR_LOADING_SCREEN_H

// Pantalla de carga del arranque: en el editor, el logo, una barra y el
// porcentaje de shaders compilados; en el juego exportado (banner_only), solo
// el banner del motor a toda la ventana, igual que lo sigue pintando despues
// el juego con Vulkan (una sola intro, sin saltos). Se pinta con GDI+ porque
// se muestra mientras Vulkan todavia se esta creando; ademas atiende los
// mensajes de la ventana para que Windows no la marque "No responde".

#include "PlatformWindow.h"

#include <chrono>
#include <functional>
#include <filesystem>
#include <memory>
#include <string>

namespace Gdiplus {
class Bitmap;
}

namespace cramion::editor {

// Fondo del banner del motor (player_banner.png): el mismo en la pantalla de
// GDI, en el juego y en el dialogo de carga del editor.
inline constexpr unsigned char kBannerBackground[3] = {12, 14, 17};

class LoadingScreen {
public:
    LoadingScreen(HWND window, const std::filesystem::path& image, bool banner_only = false);
    ~LoadingScreen();

    LoadingScreen(const LoadingScreen&) = delete;
    LoadingScreen& operator=(const LoadingScreen&) = delete;

    // fraction: 0..1. Repinta como mucho ~60 veces por segundo.
    void show(float fraction, const char* status);

private:
    void paint(float fraction, const std::wstring& text);

    HWND window_ = nullptr;
    bool banner_only_ = false;
    int painted_width_ = 0;
    int painted_height_ = 0;
#if defined(_WIN32)
    ULONG_PTR gdiplus_token_ = 0;
    std::unique_ptr<Gdiplus::Bitmap> image_;
#endif
    std::chrono::steady_clock::time_point last_paint_{};
    bool painted_ = false;
};

// Crashes: guarda un minidump y un informe en %LOCALAPPDATA%/Cramion/Crashes
// y avisa con un mensaje (donde fallo y donde quedo el informe).
// Tambien std::terminate, abort(), virtuales puras y parametros no validos.
// El informe lleva la pila con funciones y lineas (si esta el .pdb), el
// contexto (setCrashContext), la memoria y las ultimas lineas del registro.
void installCrashHandler(const std::string& app_name);
// Una linea del registro para el informe (se guardan las ultimas 80).
void crashBreadcrumb(const std::string& line);
// Dato del contexto para el informe (version, GPU, escena, proyecto...).
void setCrashContext(const std::string& key, const std::string& value);
// Archivo de registro cuyo final va al informe si no hay migas de pan.
void setCrashLogFile(const std::filesystem::path& file);
// Informe del cierre anterior de esta aplicacion (vacio si no hubo); lo
// devuelve una sola vez.
std::filesystem::path pendingCrashReport(const std::string& app_name);
std::string readCrashReport(const std::filesystem::path& report);
// Provoca un cierre para probar el informe: 0 acceso no valido, 1 abort,
// 2 terminate.
void testCrash(int kind);

// Carpeta %LOCALAPPDATA%/Cramion/<sub> (creada). En Android, dentro de los
// datos internos de la app.
std::filesystem::path localDataFolder(const std::filesystem::path& sub);

#if !defined(_WIN32)
// Android: la pantalla de carga se pinta con el renderizador y ImGui en
// cuanto existen (antes, show() no hace nada).
void setLoadingPainter(std::function<void(float fraction, const char* status)> painter);
#endif

}  // namespace cramion::editor

#endif  // CRAMION_EDITOR_LOADING_SCREEN_H
