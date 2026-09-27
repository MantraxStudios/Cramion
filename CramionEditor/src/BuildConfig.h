#ifndef CRAMION_EDITOR_BUILD_CONFIG_H
#define CRAMION_EDITOR_BUILD_CONFIG_H

// Configuraciones de compilacion (Archivo > Configuraciones de compilacion),
// como los Build Profiles de Unity o el Packaging de Unreal: cada una dice
// como sale el juego exportado. Se guardan en ProjectSettings/BuildConfigs.json
// y viajan con el proyecto.
//
//   Nombre del juego   el .exe, la carpeta, el titulo de la ventana y la
//                      pantalla de carga (vacio = el nombre del proyecto)
//   Version            en el titulo de la ventana si se quiere y en game.ini
//   Icono              PNG/JPG/TGA/BMP o .ico: se escribe dentro del .exe
//                      (Explorador, barra de tareas y ventana)
//   Escena inicial, ventana (maximizada, pantalla completa, ventana),
//   static batching y contador de FPS de desarrollo.

#include <filesystem>
#include <string>
#include <vector>

namespace cramion::editor {

struct BuildConfig {
    std::string name = "Predeterminada";
    std::string game_name;      // vacio = el nombre del proyecto
    std::string version = "1.0.0";
    bool version_in_title = false;
    std::string icon;           // relativa al proyecto (o absoluta); vacia = icono del motor
    std::string startup_scene;  // UUID de la escena; vacio = la escena inicial del proyecto
    int window_mode = 0;        // 0 maximizada, 1 pantalla completa, 2 ventana
    int width = 1600;
    int height = 900;
    bool static_batching = true;
    bool show_fps = false;      // desarrollo: FPS/CPU/GPU en una esquina
};

struct BuildConfigs {
    int active = 0;
    std::vector<BuildConfig> configs;

    BuildConfig& current();
};

// Sin archivo (o danado): una configuracion "Predeterminada".
BuildConfigs loadBuildConfigs(const std::filesystem::path& file);
bool saveBuildConfigs(const std::filesystem::path& file, const BuildConfigs& configs);

// Lineas de game.ini para el juego exportado (sin la escena).
std::string buildConfigIni(const BuildConfig& config, const std::string& game_name);

// Icono de un .exe (su grupo de iconos 1, el que usan el Explorador y la
// ventana): desde una imagen (se hacen 16..256 px) o un .ico tal cual.
bool setExeIcon(const std::filesystem::path& exe, const std::filesystem::path& image, std::string* error = nullptr);
bool isIconSource(const std::filesystem::path& file);

}  // namespace cramion::editor

#endif  // CRAMION_EDITOR_BUILD_CONFIG_H
