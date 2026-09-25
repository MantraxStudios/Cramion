#ifndef CRAMION_EDITOR_LOADING_SCREEN_H
#define CRAMION_EDITOR_LOADING_SCREEN_H

// Pantalla de carga del arranque (editor y juego exportado): la imagen del
// motor, una barra y el porcentaje de shaders compilados. Se pinta con GDI+
// porque se muestra mientras Vulkan todavia se esta creando; ademas atiende
// los mensajes de la ventana para que Windows no la marque "No responde".

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>

namespace Gdiplus {
class Bitmap;
}

namespace cramion::editor {

class LoadingScreen {
public:
    LoadingScreen(HWND window, const std::filesystem::path& image);
    ~LoadingScreen();

    LoadingScreen(const LoadingScreen&) = delete;
    LoadingScreen& operator=(const LoadingScreen&) = delete;

    // fraction: 0..1. Repinta como mucho ~60 veces por segundo.
    void show(float fraction, const char* status);

private:
    void paint(float fraction, const std::wstring& text);

    HWND window_ = nullptr;
    ULONG_PTR gdiplus_token_ = 0;
    std::unique_ptr<Gdiplus::Bitmap> image_;
    std::chrono::steady_clock::time_point last_paint_{};
    bool painted_ = false;
};

// Crashes: guarda un minidump y un informe en %LOCALAPPDATA%/Cramion/Crashes
// y avisa con un mensaje (donde fallo y donde quedo el informe).
void installCrashHandler(const std::string& app_name);

// Carpeta %LOCALAPPDATA%/Cramion/<sub> (creada).
std::filesystem::path localDataFolder(const std::filesystem::path& sub);

}  // namespace cramion::editor

#endif  // CRAMION_EDITOR_LOADING_SCREEN_H
