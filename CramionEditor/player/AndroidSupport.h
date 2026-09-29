#ifndef CRAMION_PLAYER_ANDROID_SUPPORT_H
#define CRAMION_PLAYER_ANDROID_SUPPORT_H

// Lo que el player necesita de Android ademas de la ventana (CramionDM):
// leer los assets del APK, la carpeta de datos, los logs a logcat y los
// crashes. Solo se compila en Android.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

struct AAssetManager;

namespace cramion::android {

// Carpeta de datos internos de la app (localDataFolder, partidas, caches).
void setDataRoot(const std::filesystem::path& folder);
const std::filesystem::path& dataRoot();

// cout/cerr a logcat (etiqueta "Cramion") y a un archivo.
void redirectLogs(const std::filesystem::path& file);

// Assets del APK (carpeta assets/). Rutas con '/', sin "assets/".
void setAssetManager(AAssetManager* manager);
bool assetExists(const std::string& name);
std::uint64_t assetSize(const std::string& name);
// Los archivos directos de una carpeta (sin subcarpetas: el APK no las lista).
std::vector<std::string> assetFolder(const std::string& folder);
using CopyProgress = std::function<void(std::uint64_t done, std::uint64_t total)>;
bool extractAsset(const std::string& name, const std::filesystem::path& to, const CopyProgress& progress,
                  std::string* error);

// Un asset guardado sin comprimir en el APK como trozo de un archivo que se
// puede abrir (/proc/self/fd/N desde `offset`, `length` bytes): se lee sin
// copiarlo. false si esta comprimido.
struct AssetRange {
    std::filesystem::path path;
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
    int fd = -1;
};
bool openAssetRange(const std::string& name, AssetRange& range);
void closeAssetRange(AssetRange& range);

}  // namespace cramion::android

#endif  // CRAMION_PLAYER_ANDROID_SUPPORT_H
