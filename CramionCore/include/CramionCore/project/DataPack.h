#ifndef CRAMION_PROJECT_DATA_PACK_H
#define CRAMION_PROJECT_DATA_PACK_H

// DataPacks (como los AssetBundles de Unity): una o varias escenas y/o objetos
// (prefabs) con TODO lo que usan (modelos, materiales, texturas, prefabs, scripts, sonidos, cielos,
// shaders...) en un solo archivo .datapack, sin el ejecutable. El juego los
// carga en marcha (Lua: DataPack.load / DataPack.loadScene / DataPack.instantiate):
// niveles descargables, DLC, mods, skins o contenido que se reparte aparte.
//
// Formato: un paquete .crpack (Pack.h: zstd con checksum por archivo) con las
// rutas originales ("Assets/Modelos/casa.crdata") y un manifiesto
// "datapack.json" (nombre, version, escenas, archivos).
//
// Dependencias (collectDependencies): desde las escenas, recursivo, por los
// UUID de assets que aparecen en escenas, prefabs, materiales, animators... y
// por las rutas entre comillas que existen en Assets (componentes: scripts,
// sonidos, texturas de interfaz; scripts Lua: "Sonidos/disparo.wav",
// Scene.load("Nivel2"), Scene.instantiate("Prefabs/Enemigo")...).
//
// Montar (mountDataPack): se extrae en Assets/ con sus mismas rutas (las
// referencias por ruta siguen valiendo) SIN sobrescribir nunca lo que ya
// existe; lo escrito se apunta para poder desmontarlo.

#include "CramionCore/project/Pack.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace cramion::assets {
class AssetDatabase;
}

namespace cramion::project {

inline constexpr const char* kDataPackManifest = "datapack.json";
inline constexpr const char* kDataPackExtension = ".datapack";

struct DataPackManifest {
    std::string name;
    std::string version = "1.0";
    std::string engine;                // version de Cramion que lo creo
    std::string created;               // fecha (UTC, ISO 8601)
    std::vector<std::string> scenes;   // rutas dentro de Assets ("Escenas/Nivel2.crscene")
    std::vector<std::string> objects;  // prefabs para instanciar ("Prefabs/Coche.crprefab")
    std::vector<std::string> files;    // todas, dentro de Assets
    std::uint64_t bytes = 0;           // tamano sin comprimir
};

struct DataPackCollection {
    std::vector<std::filesystem::path> files;  // absolutas, dentro de assets_root (las escenas primero)
    std::vector<std::string> missing;          // referencias que no existen (avisos)
    std::uint64_t bytes = 0;
};

// Todo lo que usan `scenes` (escenas y/o prefabs: rutas absolutas dentro de
// assets_root).
DataPackCollection collectDependencies(const std::vector<std::filesystem::path>& scenes,
                                       const std::filesystem::path& assets_root,
                                       const assets::AssetDatabase& database);

// Escribe el .datapack (level: compresion zstd 1..19). `scenes` son las raices:
// las .crscene van a "scenes" del manifiesto y los .crprefab a "objects".
bool writeDataPack(const std::filesystem::path& file, const std::string& name,
                   const std::vector<std::filesystem::path>& scenes, const DataPackCollection& collection,
                   const std::filesystem::path& assets_root, const std::string& engine_version, int level,
                   const PackProgress& progress, std::string* error, DataPackManifest* written = nullptr);

// Solo el manifiesto (sin extraer nada).
bool readDataPackManifest(const std::filesystem::path& file, DataPackManifest& manifest, std::string* error);

struct DataPackMount {
    std::string name;
    std::filesystem::path file;
    std::vector<std::string> scenes;             // dentro de Assets
    std::vector<std::string> objects;            // prefabs, dentro de Assets
    std::vector<std::filesystem::path> written;  // lo que se escribio (se borra al desmontar)
    std::vector<std::string> kept;               // ya existian: no se tocaron
};

// Extrae el paquete en assets_root sin sobrescribir nada. `journal` (opcional):
// archivo donde se apuntan las rutas escritas, para borrarlas aunque el
// programa se cierre sin desmontar (cleanupDataPackJournal).
bool mountDataPack(const std::filesystem::path& file, const std::filesystem::path& assets_root, DataPackMount& mount,
                   std::string* error, const std::filesystem::path& journal = {});
// Borra lo que escribio el montaje (y las carpetas que queden vacias).
void unmountDataPack(const DataPackMount& mount, const std::filesystem::path& assets_root,
                     const std::filesystem::path& journal = {});
// Borra lo apuntado en el diario (montajes que no se desmontaron) y lo vacia.
void cleanupDataPackJournal(const std::filesystem::path& journal, const std::filesystem::path& assets_root);

}  // namespace cramion::project

#endif  // CRAMION_PROJECT_DATA_PACK_H
