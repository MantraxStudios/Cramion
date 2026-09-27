// Prueba de las configuraciones de compilacion (consola): guardar y leer
// perfiles, game.ini y el icono escrito dentro de una copia del .exe.
//
//   CramionBuildConfigTests.exe <CramionPlayer.exe> <icono.png>

#include "BuildConfig.h"

#include <windows.h>
#include <shellapi.h>

#include <cstdio>
#include <filesystem>
#include <string>

using namespace cramion::editor;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? "OK" : "FALLO", what);
    if (!ok) ++failures;
}

// Numero de imagenes del grupo de iconos 1 del .exe (-1 si no tiene).
int groupIconCount(const std::filesystem::path& exe) {
    HMODULE module = LoadLibraryExW(exe.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (module == nullptr) return -1;
    int count = -1;
    if (HRSRC res = FindResourceW(module, MAKEINTRESOURCEW(1), RT_GROUP_ICON)) {
        if (HGLOBAL data = LoadResource(module, res)) {
            const auto* bytes = static_cast<const unsigned char*>(LockResource(data));
            if (bytes != nullptr && SizeofResource(module, res) >= 6) count = bytes[4] | (bytes[5] << 8);
        }
    }
    FreeLibrary(module);
    return count;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("uso: CramionBuildConfigTests <CramionPlayer.exe> <icono.png>\n");
        return 2;
    }
    const std::filesystem::path player = argv[1];
    const std::filesystem::path icon = argv[2];
    const std::filesystem::path folder = std::filesystem::temp_directory_path() / "cramion_buildconfig_test";
    std::error_code e;
    std::filesystem::remove_all(folder, e);
    std::filesystem::create_directories(folder, e);

    // Perfiles: ida y vuelta.
    BuildConfigs configs;
    configs.configs.resize(2);
    configs.configs[1].name = "Shipping";
    configs.configs[1].game_name = "Mi Juego";
    configs.configs[1].version = "2.3.1";
    configs.configs[1].version_in_title = true;
    configs.configs[1].icon = "Assets/icono.png";
    configs.configs[1].window_mode = 1;
    configs.configs[1].show_fps = true;
    configs.active = 1;
    const std::filesystem::path file = folder / "ProjectSettings" / "BuildConfigs.json";
    check(saveBuildConfigs(file, configs), "se guardan las configuraciones");
    BuildConfigs loaded = loadBuildConfigs(file);
    check(loaded.configs.size() == 2 && loaded.active == 1, "se leen las dos y la activa");
    const BuildConfig& shipping = loaded.current();
    check(shipping.game_name == "Mi Juego" && shipping.version == "2.3.1" && shipping.icon == "Assets/icono.png" &&
              shipping.window_mode == 1 && shipping.show_fps && shipping.version_in_title,
          "los campos sobreviven");
    check(loadBuildConfigs(folder / "no_existe.json").configs.size() == 1, "sin archivo: una predeterminada");
    const std::string ini = buildConfigIni(shipping, "Mi Juego");
    check(ini.find("title=Mi Juego 2.3.1\n") != std::string::npos && ini.find("window=1\n") != std::string::npos &&
              ini.find("show_fps=1\n") != std::string::npos,
          "game.ini lleva titulo, ventana y FPS");

    // Icono dentro de una copia del reproductor.
    const std::filesystem::path exe = folder / "Mi Juego.exe";
    std::filesystem::copy_file(player, exe, std::filesystem::copy_options::overwrite_existing, e);
    check(!e, "copia del reproductor");
    const int before = groupIconCount(exe);
    std::string error;
    const bool ok = setExeIcon(exe, icon, &error);
    if (!ok) std::printf("  error: %s\n", error.c_str());
    check(ok, "se escribe el icono en el .exe");
    const int after = groupIconCount(exe);
    std::printf("  imagenes del icono: antes %d, despues %d\n", before, after);
    check(after >= 5, "el grupo de iconos 1 tiene los tamanos nuevos (16..256)");
    HICON large = nullptr;
    HICON small = nullptr;
    const UINT extracted = ExtractIconExW(exe.c_str(), 0, &large, &small, 1);
    check(extracted > 0 && large != nullptr, "Windows lee el icono del .exe");
    if (large) DestroyIcon(large);
    if (small) DestroyIcon(small);
    // Otra vez (reexportar): sigue valido.
    check(setExeIcon(exe, icon, &error) && groupIconCount(exe) == after, "reescribir el icono no lo duplica");

    std::filesystem::remove_all(folder, e);
    std::printf(failures == 0 ? "TODO OK\n" : "%d FALLO(S)\n", failures);
    return failures == 0 ? 0 : 1;
}
