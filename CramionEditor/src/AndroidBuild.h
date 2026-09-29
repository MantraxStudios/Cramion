#ifndef CRAMION_EDITOR_ANDROID_BUILD_H
#define CRAMION_EDITOR_ANDROID_BUILD_H

// Empaquetado del juego para Android (Archivo > Exportar juego con una
// configuracion de plataforma Android), sin Gradle ni Android Studio: solo
// las herramientas del SDK.
//
//   1. AndroidManifest.xml (paquete, nombre, version, orientacion, permisos,
//      NativeActivity con libmain.so) e iconos por densidad (mipmap-*).
//   2. aapt2 compile + link -> recursos y manifiesto binarios.
//   3. Se anaden lib/<abi>/libmain.so (sin comprimir) y assets/ (shaders,
//      Game/game.ini, banner y el .crpack si no va aparte en un .obb).
//   4. zipalign (16 KB para las .so) y apksigner (v2/v3) con la clave.
//   AAB (Google Play): aapt2 --proto-format + bundletool build-bundle y
//   jarsigner. OBB: el .crpack tal cual como main.<version>.<paquete>.obb.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace cramion::editor {

// Donde estan las herramientas (se buscan solas: ANDROID_HOME,
// %LOCALAPPDATA%/Android/Sdk, el JDK de Android Studio o JAVA_HOME).
struct AndroidToolchain {
    std::filesystem::path sdk;
    std::filesystem::path build_tools;   // la version mas nueva
    std::filesystem::path android_jar;   // platforms/android-N/android.jar
    int platform = 0;                    // N de android.jar
    std::filesystem::path java_bin;      // carpeta con java/keytool/jarsigner
    std::filesystem::path adb;
    std::filesystem::path bundletool;    // bundletool.jar (solo AAB)
    std::string error;                   // que falta (vacio = todo bien para APK)

    bool ok() const { return error.empty(); }
};

// `editor_folder`: donde buscar android/bundletool.jar junto al editor.
AndroidToolchain findAndroidToolchain(const std::filesystem::path& editor_folder, int target_sdk);

struct AndroidKey {
    std::filesystem::path keystore;  // vacio = clave de depuracion (se crea sola)
    std::string store_password;
    std::string alias;
    std::string key_password;
};

struct AndroidPackageInput {
    std::string package;       // com.estudio.juego
    std::string label;         // nombre bajo el icono
    std::string version_name = "1.0.0";
    int version_code = 1;
    int min_sdk = 29;
    int target_sdk = 35;
    int orientation = 0;       // 0 horizontal, 1 vertical, 2 libre, 3 horizontal fija, 4 vertical fija
    bool internet = true;
    bool vibrate = true;
    bool record_audio = false;
    std::filesystem::path icon;  // PNG/JPG (cuadrado mejor); vacio = el del motor
    std::filesystem::path fallback_icon;

    // (abi, libmain.so): "arm64-v8a", "x86_64"...
    std::vector<std::pair<std::string, std::filesystem::path>> libraries;
    // (ruta dentro de assets/, archivo): "shaders/pbr.frag.spv"...
    std::vector<std::pair<std::string, std::filesystem::path>> assets;
    std::filesystem::path pack;  // el .crpack del juego
    std::string pack_name;       // "Juego.crpack" (dentro de assets/Game/)
    bool split_obb = false;      // el .crpack como .obb aparte (solo APK)

    bool make_apk = true;
    bool make_aab = false;
    AndroidKey key;

    std::filesystem::path output_folder;
    std::string file_stem;       // "MiJuego" -> MiJuego.apk / MiJuego.aab
    std::filesystem::path work_folder;  // temporales (se borran al terminar)
};

struct AndroidPackageResult {
    std::filesystem::path apk;
    std::filesystem::path aab;
    std::filesystem::path obb;
    std::string log;  // salida de las herramientas (para la consola)
};

using AndroidProgress = std::function<void(float fraction, const std::string& step)>;

// Nombre de paquete valido (a.b.c, letras/numeros/_, cada parte empieza por letra).
bool validAndroidPackage(const std::string& package);
// "Mi Juego!" -> "com.cramion.mijuego".
std::string defaultAndroidPackage(const std::string& game_name);

// El manifiesto (se usa tambien en las pruebas).
std::string androidManifest(const AndroidPackageInput& input);

bool buildAndroidPackage(const AndroidToolchain& tools, const AndroidPackageInput& input, const AndroidProgress& progress,
                         const std::atomic<bool>* cancel, AndroidPackageResult& result, std::string* error);

// Dispositivos conectados por adb ("serial  modelo").
std::vector<std::string> androidDevices(const AndroidToolchain& tools);
// Instala el APK (y sube el .obb si hay) y abre el juego en el dispositivo.
bool installAndroidPackage(const AndroidToolchain& tools, const std::string& device, const std::filesystem::path& apk,
                           const std::filesystem::path& obb, const std::string& package, std::string& log,
                           std::string* error);

}  // namespace cramion::editor

#endif  // CRAMION_EDITOR_ANDROID_BUILD_H
