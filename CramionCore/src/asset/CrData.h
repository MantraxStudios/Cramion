#ifndef CRAMION_CORE_SRC_ASSET_CRDATA_H
#define CRAMION_CORE_SRC_ASSET_CRDATA_H

// Formato de los archivos .crdata (interno de CramionAssets).
//
// Todo en little-endian, cadenas como u64 (bytes) + UTF-8 sin terminador:
//
//   Cabecera
//     char[8]  firma "CRDATA\0\1"
//     u32      version del formato (kCrDataVersion)
//     u32      AssetType
//     u64 u64  UUID (high, low)
//     string   nombre para mostrar
//     string   ruta del archivo original (informativa, para reimportar)
//     string   ajustes de importacion (JSON)
//     u64      bytes del contenido que sigue (para saltarlo o validarlo)
//
//   Contenido de un Model
//     u32      numero de nodos; por nodo: string nombre, i32 padre,
//              f32[16] transform local, i32 pieza (-1 = sin malla)
//     u32      1 si es animado
//     u32      numero de animaciones; por cada una: string nombre
//     u32      numero de piezas; por pieza: u64 bytes + el ModelData
//              (asset::writeModelPayload, texturas incrustadas)
//
//   Contenido de un Environment
//     string   extension original (".hdr")
//     u64      bytes + el archivo tal cual
//
// La escritura es atomica: a <archivo>.tmp y renombrar al final.

#include "CramionCore/asset/AssetManager.h"
#include "CramionCore/asset/AssetTypes.h"

#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

namespace cramion::assets::crdata {

inline constexpr char kMagic[8] = {'C', 'R', 'D', 'A', 'T', 'A', '\0', '\1'};
inline constexpr std::uint32_t kVersion = 1;
inline constexpr const char* kExtension = ".crdata";

struct Header {
    AssetType type = AssetType::Unknown;
    Uuid uuid{};
    std::string name;
    std::string source;
    std::string settings_json;
    std::uint64_t payload_bytes = 0;
};

// Solo la cabecera (rapido: no lee el contenido). nullopt si no es un
// .crdata valido de esta version.
std::optional<Header> readHeader(const std::filesystem::path& file);

// --- Modelos ---
struct ModelContent {
    std::vector<ModelNode> nodes;
    bool animated = false;
    std::vector<std::string> animation_names;
    std::vector<asset::ModelData> parts;  // texturas incrustadas, sin decodificar
};

void writeModel(const std::filesystem::path& file, const Header& header,
                const ModelContent& content);
// Lee todo; las piezas quedan tal como se guardaron (sin decodificar).
bool readModel(const std::filesystem::path& file, Header& header, ModelContent& content);

// --- Cielos HDR ---
void writeEnvironment(const std::filesystem::path& file, const Header& header,
                      const std::string& extension, const std::vector<std::uint8_t>& bytes);
bool readEnvironment(const std::filesystem::path& file, Header& header, std::string& extension,
                     std::vector<std::uint8_t>& bytes);

// Ruta de archivo libre en `folder` para `stem` + `extension`: si existe,
// "stem 1", "stem 2"...
std::filesystem::path uniquePath(const std::filesystem::path& folder, const std::string& stem,
                                 const std::string& extension);

std::string utf8(const std::filesystem::path& path);
std::filesystem::path fromUtf8(const std::string& text);

}  // namespace cramion::assets::crdata

#endif  // CRAMION_CORE_SRC_ASSET_CRDATA_H
