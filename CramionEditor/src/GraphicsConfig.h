#ifndef CRAMION_EDITOR_GRAPHICS_CONFIG_H
#define CRAMION_EDITOR_GRAPHICS_CONFIG_H

// Configuracion grafica compartida por el editor y el juego exportado:
//
//   - Graphics.ini (clave=valor): escalado, calidad, nitidez, VSync,
//     presupuesto adaptativo, sombras, texturas y los interruptores del
//     render. El del proyecto lo escribe el editor; el juego exportado
//     guarda encima el del jugador (Graphics.save() desde Lua).
//   - Las calidades rapidas (Baja, Media, Alta, Ultra).
//   - RendererGraphicsHost: la tabla Graphics de Lua sobre el renderizador y
//     la ventana (modo y tamano, solo en el juego exportado).

#include <CramionCore/scripting/Scripting.h>

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace cramion::gfx {
class VulkanRenderer;
}

namespace cramion::editor {

// Lee un Graphics.ini y lo aplica al renderizador (lo que no esta, igual).
void loadGraphicsIni(const std::filesystem::path& file, gfx::VulkanRenderer& renderer);
// Escribe la configuracion actual del renderizador.
bool saveGraphicsIni(const std::filesystem::path& file, const gfx::VulkanRenderer& renderer);

// Calidades rapidas: 0 Baja, 1 Media, 2 Alta, 3 Ultra.
const std::vector<std::string>& qualityPresetNames();
void applyQualityPreset(gfx::VulkanRenderer& renderer, int level);

// Modo de la ventana del juego (game.ini / Graphics.ini del jugador).
enum class WindowMode : int { Maximized = 0, Fullscreen = 1, Windowed = 2 };
// Cambia el modo y el tamano (el de dentro, en pixeles) de una ventana
// Win32 ya creada. Pantalla completa = sin bordes, del tamano del monitor.
void applyWindowMode(HWND hwnd, WindowMode mode, int width, int height);

// Lee window_mode / window_width / window_height de un .ini (si estan).
bool readWindowSettings(const std::filesystem::path& file, WindowMode& mode, int& width, int& height);

class RendererGraphicsHost final : public scripting::GraphicsHost {
public:
    // `window`: nullptr si la ventana no es del juego (el editor): entonces el
    // modo y el tamano se leen pero no se cambian.
    // `saver`: que hace Graphics.save() (el editor no guarda nada en Play).
    using Saver = std::function<bool(std::string& error)>;
    RendererGraphicsHost(gfx::VulkanRenderer& renderer, HWND window, Saver saver);

    std::vector<scripting::GraphicsOption> options() const override;
    bool set(const std::string& key, const scripting::GraphicsValue& value, std::string& error) override;
    std::vector<std::string> qualityLevels() const override;
    bool setQuality(const std::string& level, std::string& error) override;
    std::string quality() const override { return quality_; }
    std::vector<std::pair<int, int>> resolutions() const override;
    bool save(std::string& error) override;

    WindowMode windowMode() const { return window_mode_; }
    int windowWidth() const { return window_width_; }
    int windowHeight() const { return window_height_; }
    // El modo con el que se abrio la ventana (game.ini o el guardado).
    void setWindowState(WindowMode mode, int width, int height);
    void setSaver(Saver saver) { saver_ = std::move(saver); }

private:
    gfx::VulkanRenderer& renderer_;
    HWND window_ = nullptr;
    Saver saver_;
    std::string quality_ = "Personalizada";
    WindowMode window_mode_ = WindowMode::Maximized;
    int window_width_ = 1600;
    int window_height_ = 900;
};

}  // namespace cramion::editor

#endif  // CRAMION_EDITOR_GRAPHICS_CONFIG_H
