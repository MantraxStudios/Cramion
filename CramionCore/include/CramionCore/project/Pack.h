#ifndef CRAMION_PROJECT_PACK_H
#define CRAMION_PROJECT_PACK_H

// Paquetes .crpack: los assets del juego exportado en un solo archivo binario,
// cada uno comprimido con zstd (con checksum).
//
//   "CRPK" u32 version  u32 archivos  u64 posicion del indice
//   datos: un frame zstd por archivo
//   indice: por archivo  u16 largo + ruta UTF-8 (relativa, con '/'),
//           u64 posicion, u64 comprimido, u64 original
//
// El juego lo descomprime una vez (extractPack) en una carpeta local y lo
// reutiliza mientras el paquete no cambie (packId).

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace cramion::project {

struct PackInput {
    std::filesystem::path source;  // archivo en disco
    std::string path;              // ruta dentro del paquete ("Assets/Models/coche.fbx")
};

struct PackEntry {
    std::string path;
    std::uint64_t offset = 0;
    std::uint64_t compressed = 0;
    std::uint64_t size = 0;
};

// Progreso: bytes originales procesados y archivo actual. Devolver false cancela.
using PackProgress = std::function<bool(std::uint64_t done, const std::string& current)>;

// level: 1 (rapido) .. 19 (pequeno). Escribe a un temporal y lo renombra.
bool writePack(const std::filesystem::path& file, const std::vector<PackInput>& inputs, int level,
               const PackProgress& progress, std::string* error);

bool readPackIndex(const std::filesystem::path& file, std::vector<PackEntry>& entries, std::string* error);

// Descomprime todo en `folder` (verifica los checksums). Rutas con ".." o
// absolutas se rechazan.
bool extractPack(const std::filesystem::path& file, const std::filesystem::path& folder,
                 const PackProgress& progress, std::string* error);

// Identificador del contenido (hash del indice y el tamano): cambia si el
// paquete cambia.
std::string packId(const std::filesystem::path& file);

}  // namespace cramion::project

#endif  // CRAMION_PROJECT_PACK_H
