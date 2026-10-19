#ifndef CRAMION_EDITOR_GRAPHICS_CONFIG_H
#define CRAMION_EDITOR_GRAPHICS_CONFIG_H

// Configuracion grafica compartida por el editor y el juego exportado:
//
//   - Graphics.ini (clave=valor): escalado, calidad, nitidez, VSync,
//     presupuesto adaptativo, sombras, texturas y los interruptores del
//     render. El del proyecto lo escribe el editor; el juego exportado
//     guarda encima el del jugador (Graphics::save() desde un script).
//   - Las calidades rapidas (Baja, Media, Alta, Ultra).
//   - RendererGraphicsHost: la API Graphics de los scripts sobre el renderizador y
//     la ventana (modo y tamano, solo en el juego exportado).

#include <CramionCore/scripting/Scripting.h>

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "PlatformWindow.h"

namespace cramion::gfx {
class VulkanRenderer;
}

namespace cramion::editor {

// Version de los Graphics.ini que se escriben (version=). La 1 (sin la
// clave) es la de antes: Graphics::save() guardaba tambien lo heredado del
// proyecto, asi que lo que dice no siempre lo eligio el jugador.
inline constexpr int kGraphicsIniVersion = 2;
// De quien es el archivo: el del proyecto (lo escribe el editor) o el del
// jugador (Graphics::save() desde un script del juego exportado).
enum class GraphicsIniSource { Project, Player };
// Lee un Graphics.ini y lo aplica al renderizador (lo que no esta, igual).
// Devuelve su version (1 si no la lleva; 0 si no hay archivo). Del jugador,
// adaptive=0 solo cuenta con version 2 o mas: en uno de antes puede ser el
// del visor del editor heredado del proyecto, no una eleccion del jugador.
int loadGraphicsIni(const std::filesystem::path& file, gfx::VulkanRenderer& renderer,
                    GraphicsIniSource source = GraphicsIniSource::Project);
// Escribe la configuracion actual del renderizador (con version=).
bool saveGraphicsIni(const std::filesystem::path& file, const gfx::VulkanRenderer& renderer);

// Calidades rapidas: 0 Baja, 1 Media, 2 Alta, 3 Ultra.
const std::vector<std::string>& qualityPresetNames();
void applyQualityPreset(gfx::VulkanRenderer& renderer, int level);

// Modo de la ventana del juego (game.ini / Graphics.ini del jugador).
//   Maximized:  ventana con marco, maximizada (el area de trabajo).
//   Fullscreen: sin bordes, del tamano del monitor.
//   Windowed:   con marco, width x height de dentro, centrada en el area de
//               trabajo. Si con el marco no cabe en el area de trabajo pero
//               el tamano si cabe en el monitor (1920x1080 en un monitor de
//               1920x1080), va sin marco y centrada en el monitor: el tamano
//               de dentro es siempre el pedido.
//   Borderless: sin bordes, width x height, centrada en el monitor.
enum class WindowMode : int { Maximized = 0, Fullscreen = 1, Windowed = 2, Borderless = 3 };
inline constexpr int kWindowModeCount = 4;
// Cambia el modo y el tamano (el de dentro, en pixeles) de una ventana
// Win32 ya creada.
void applyWindowMode(HWND hwnd, WindowMode mode, int width, int height);

// El monitor de la ventana (o el principal si no hay ventana), en pixeles:
// su tamano y el mayor tamano de dentro que cabe con el marco de Windows en
// su area de trabajo (sin la barra de tareas). false si no se sabe (Android).
struct MonitorSize {
    int width = 0;
    int height = 0;
    int windowed_max_width = 0;
    int windowed_max_height = 0;
};
bool monitorSize(HWND hwnd, MonitorSize& out);

// Lee window_mode / window_width / window_height de un .ini (si estan).
bool readWindowSettings(const std::filesystem::path& file, WindowMode& mode, int& width, int& height);

// Tope de FPS del juego exportado (Graphics "fps_limit"): 0 = sin tope; si no,
// entre kFpsLimitMin y kFpsLimitMax. Lo aplica el bucle del reproductor
// (espera al final de cada frame); con VSync manda tambien la pantalla.
inline constexpr int kFpsLimitMin = 15;
inline constexpr int kFpsLimitMax = 360;
// 0 (sin tope) o el valor recortado a [kFpsLimitMin, kFpsLimitMax].
int clampFpsLimit(double fps);
// fps_limit de un .ini (0 si no esta).
int readFpsLimit(const std::filesystem::path& file);

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
    // Tope de FPS (0 = sin tope). Solo lo aplica el reproductor.
    int fpsLimit() const { return fps_limit_; }
    void setFpsLimit(int fps) { fps_limit_ = clampFpsLimit(fps); }

private:
    gfx::VulkanRenderer& renderer_;
    HWND window_ = nullptr;
    Saver saver_;
    std::string quality_ = "Personalizada";
    WindowMode window_mode_ = WindowMode::Maximized;
    int window_width_ = 1600;
    int window_height_ = 900;
    int fps_limit_ = 0;
};

}  // namespace cramion::editor

#endif  // CRAMION_EDITOR_GRAPHICS_CONFIG_H
