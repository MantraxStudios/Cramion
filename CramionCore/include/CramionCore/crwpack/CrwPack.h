#ifndef CRAMION_CRWPACK_CRWPACK_H
#define CRAMION_CRWPACK_CRWPACK_H

// Paquetes .crwpack: los paquetes de assets de la Cramion Store (y del
// Package Manager del editor), como los .unitypackage / AssetBundles de Unity.
//
// El contenido de todos los archivos se concatena en un solo flujo y se corta
// en bloques fijos (4 MiB por defecto). Cada bloque se comprime solo (zstd, o
// guardado tal cual si no gana), asi que comprimir y descomprimir usan todos
// los nucleos y los archivos pequenos comparten bloque (mejor compresion que
// uno por archivo). Cada bloque lleva su CRC32.
//
//   cabecera (64 bytes, little endian)
//     "CRWPACK\0"  u32 formato(1)  u32 flags  u32 tamano de bloque
//     u32 archivos  u64 bloques  u64 bytes originales
//     u64 posicion del manifiesto  u32 tamano del manifiesto
//     u32 tamano del indice  u64 posicion del indice
//   manifiesto: JSON sin comprimir (name, version, displayName, description,
//               author, engine, dependencies {nombre: version}, changelog...)
//               - lo lee el servidor sin descomprimir nada.
//   bloques comprimidos
//   indice: "CWIX" u32 archivos
//           por archivo: u16 largo + ruta UTF-8 (relativa, con '/'),
//                        u64 posicion en el flujo, u64 tamano
//           u64 bloques
//           por bloque: u64 posicion en el .crwpack, u32 comprimido,
//                       u32 original, u32 crc32 (del original), u32 metodo
//           u32 crc32 de todo lo anterior del indice

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace cramion::crwpack {

constexpr std::uint32_t kFormatVersion = 1;
constexpr std::uint32_t kDefaultBlockSize = 4u << 20;
constexpr const char* kExtension = ".crwpack";

enum class Method : std::uint32_t { Stored = 0, Zstd = 1 };

struct FileEntry {
    std::string path;          // relativa, con '/'
    std::uint64_t offset = 0;  // en el flujo concatenado
    std::uint64_t size = 0;
};

struct BlockEntry {
    std::uint64_t offset = 0;  // en el .crwpack
    std::uint32_t compressed = 0;
    std::uint32_t size = 0;
    std::uint32_t crc = 0;
    Method method = Method::Zstd;
};

struct Info {
    std::uint32_t format = 0;
    std::uint32_t block_size = 0;
    std::uint64_t raw_size = 0;     // suma de los archivos
    std::uint64_t packed_size = 0;  // el .crwpack entero
    nlohmann::json manifest;
    std::vector<FileEntry> files;
    std::vector<BlockEntry> blocks;
};

// Niveles de zstd de los preajustes (como Unity: LZ4 rapido o LZMA pequeno).
enum class Level { Fast = 3, Normal = 9, Max = 19 };

struct PackOptions {
    int level = static_cast<int>(Level::Normal);  // 1..22
    std::uint32_t block_size = kDefaultBlockSize; // 64 KiB .. 64 MiB
    int threads = 0;                              // 0 = todos los nucleos
};

// Bytes originales procesados y total. Se llama desde el hilo que llama a la
// funcion. Devolver false cancela.
using Progress = std::function<bool(std::uint64_t done, std::uint64_t total)>;

// Comprime todos los archivos de `folder` (recursivo, en orden de ruta) en
// `file`. Escribe a un temporal y lo renombra. Al manifiesto se le anaden
// "format", "files", "rawSize" y "created".
bool pack(const std::filesystem::path& folder, const std::filesystem::path& file, nlohmann::json manifest,
          const PackOptions& options, const Progress& progress, std::string* error);

// Igual, con la lista de archivos explicita: (archivo en disco, ruta dentro).
bool packFiles(const std::vector<std::pair<std::filesystem::path, std::string>>& inputs,
               const std::filesystem::path& file, nlohmann::json manifest, const PackOptions& options,
               const Progress& progress, std::string* error);

// Cabecera, manifiesto e indice (no descomprime nada).
bool readInfo(const std::filesystem::path& file, Info& info, std::string* error);

// Solo el manifiesto.
bool readManifest(const std::filesystem::path& file, nlohmann::json& manifest, std::string* error);

// Descomprime todo en `folder` (la crea). Rutas con "..", absolutas o con
// ':' se rechazan. `threads` 0 = todos los nucleos. Verifica los CRC.
bool unpack(const std::filesystem::path& file, const std::filesystem::path& folder, int threads,
            const Progress& progress, std::string* error);

// Descomprime en memoria y comprueba los CRC sin escribir nada.
bool verify(const std::filesystem::path& file, int threads, const Progress& progress, std::string* error);

// Ruta valida dentro de un paquete (relativa, sin "..", sin ':' ni '\').
bool safeRelativePath(const std::string& path);

// Versiones semver ("1.2.3", "1.2.3-beta.1"): <0, 0 o >0.
int compareVersions(const std::string& a, const std::string& b);
bool validVersion(const std::string& version);
bool validName(const std::string& name);  // "com.autor.paquete"

std::uint32_t crc32(const void* data, std::size_t size, std::uint32_t crc = 0);

}  // namespace cramion::crwpack

#endif  // CRAMION_CRWPACK_CRWPACK_H
