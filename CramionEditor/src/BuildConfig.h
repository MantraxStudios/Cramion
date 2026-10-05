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
//
//   Plataforma         Windows (.exe) o Android (APK / AAB, con el OBB
//                      aparte si se quiere): paquete, version, orientacion
//                      inicial, permisos, clave de firma y perfil movil
//                      (AndroidBuildSettings). Los controles tactiles son del
//                      proyecto (ProjectSettings/TouchInterface.json).

#include <filesystem>
#include <cstdint>
#include <string>
#include <vector>

namespace cramion::editor {

// Linux: se compila en Linux o WSL (CMakePresets "linux-release"); el editor
// solo guarda la configuracion y explica como hacerlo.
enum class BuildPlatform : int { Windows = 0, Android = 1, Linux = 2 };

// Steam (platform/Steam.h): el juego carga steam_api64.dll si esta.
struct SteamBuildSettings {
    bool enabled = false;
    std::uint32_t app_id = 480;   // 480 = Spacewar (la de pruebas de Valve)
    bool ship_appid_file = true;  // steam_appid.txt junto al .exe (solo para probar fuera de Steam)
    bool restart_if_necessary = false;  // relanzar desde Steam si se abre a mano
    std::string dll;              // ruta de steam_api64.dll del Steamworks SDK (se copia al exportar)
};

struct AndroidBuildSettings {
    std::string package;          // vacio = com.cramion.<juego>
    int version_code = 1;         // sube en cada version que se publique
    int min_sdk = 26;             // Android 8.0 (Vulkan 1.0+: el modo compatible del renderizador)
    int target_sdk = 35;          // Android 15 (lo que pide Google Play)
    int orientation = 0;          // al abrir: 0 horizontal, 1 vertical, 2 libre, 3 horizontal fija, 4 vertical fija
                                  // (despues, Screen.setOrientation en Lua)
    bool make_apk = true;
    bool make_aab = false;        // Google Play
    bool split_obb = false;       // los assets en main.<version>.<paquete>.obb
    bool x86_64 = false;          // tambien para emuladores (si esta compilado)
    bool armeabi_v7a = true;      // moviles de 32 bits (Android Go, gama baja; si esta compilado)
    bool internet = true;
    bool vibrate = true;
    bool record_audio = false;
    std::string icon;             // vacio = el de la configuracion (o el del motor)
    // Firma: vacio = clave de depuracion (valida para probar, no para Play).
    // Las contrasenas no se guardan en el proyecto.
    std::string keystore;
    std::string key_alias;
    std::string store_password;   // solo en memoria
    std::string key_password;     // solo en memoria
    // Perfil movil: calidad inicial (0 Baja..3 Ultra) y FPS objetivo.
    int quality = 0;
    int target_fps = 30;
    // Resolucion de la imagen (lado corto en pixeles; la pantalla la escala):
    // 0 = segun la calidad (720 Baja, 900 Media, 1080 Alta, nativa en Ultra),
    // -1 = la nativa de la pantalla.
    int resolution = 0;
};

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
    bool vr = false;            // realidad virtual (OpenXR) si hay casco; solo Windows
    BuildPlatform platform = BuildPlatform::Windows;
    AndroidBuildSettings android;
    SteamBuildSettings steam;
};

struct BuildConfigs {
    int active = 0;
    std::vector<BuildConfig> configs;

    BuildConfig& current();
};

// Sin archivo (o danado): una configuracion "Predeterminada".
BuildConfigs loadBuildConfigs(const std::filesystem::path& file);
bool saveBuildConfigs(const std::filesystem::path& file, const BuildConfigs& configs);

// Lineas de game.ini para el juego exportado (sin la escena). En Android
// ademas el perfil movil y los controles tactiles.
std::string buildConfigIni(const BuildConfig& config, const std::string& game_name);

// Icono de un .exe (su grupo de iconos 1, el que usan el Explorador y la
// ventana): desde una imagen (se hacen 16..256 px) o un .ico tal cual.
bool setExeIcon(const std::filesystem::path& exe, const std::filesystem::path& image, std::string* error = nullptr);
bool isIconSource(const std::filesystem::path& file);

}  // namespace cramion::editor

#endif  // CRAMION_EDITOR_BUILD_CONFIG_H
