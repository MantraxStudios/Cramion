#ifndef CRAMION_CORE_ASSET_TYPES_H
#define CRAMION_CORE_ASSET_TYPES_H

// CONTRATO COMPARTIDO (lo usan CramionAssets, el ECS de CramionCore y el
// editor). No cambiarlo sin coordinar.

#include "CramionCore/Uuid.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace cramion::assets {

// Tipos de asset de un proyecto. Cada uno vive en un archivo de Assets/:
//   Model        .crdata  modelo importado (mallas, materiales, texturas
//                         incrustadas, esqueleto, animaciones, jerarquia)
//   Environment  .crdata  cielo HDR importado (el .hdr original incrustado)
//   Scene        .crscene escena (JSON): entidades y componentes
//   AnimatorController .cranimator maquina de estados de animacion (JSON)
//   AnimationClip      .cranim     clip extraido de un modelo (pistas por
//                                  nombre de nodo; 1a linea = cabecera)
// Los archivos originales (FBX, OBJ, glTF, HDR) no se copian al proyecto: el
// importador los convierte a .crdata.
enum class AssetType : std::uint32_t {
    Unknown = 0,
    Model = 1,
    Environment = 2,
    Scene = 3,
    AnimatorController = 4,
    AnimationClip = 5,
};

inline const char* assetTypeName(AssetType type) {
    switch (type) {
        case AssetType::Model: return "Modelo";
        case AssetType::Environment: return "Cielo HDR";
        case AssetType::Scene: return "Escena";
        case AssetType::AnimatorController: return "Animator";
        case AssetType::AnimationClip: return "Clip de animacion";
        default: return "Desconocido";
    }
}

// Referencia a un asset desde un componente o desde otro asset: solo el
// UUID (y el tipo esperado). Se resuelve con AssetDatabase / AssetManager.
struct AssetRef {
    Uuid uuid{};
    AssetType type = AssetType::Unknown;

    bool valid() const { return uuid.valid(); }
    friend bool operator==(const AssetRef&, const AssetRef&) = default;
};

// UUIDs fijos de los assets integrados (no tienen archivo): primitivas que el
// AssetManager genera por codigo, como las de Unity.
namespace builtin {
inline constexpr Uuid kCube{0x00000000c0be0000ull, 0x8000000000000001ull};
inline constexpr Uuid kSphere{0x00000000c0be0000ull, 0x8000000000000002ull};
inline constexpr Uuid kPlane{0x00000000c0be0000ull, 0x8000000000000003ull};
inline constexpr Uuid kCylinder{0x00000000c0be0000ull, 0x8000000000000004ull};
inline constexpr Uuid kCapsule{0x00000000c0be0000ull, 0x8000000000000005ull};
}  // namespace builtin

// Lo que sabe la base de datos de cada asset (sin cargarlo).
struct AssetInfo {
    Uuid uuid{};
    AssetType type = AssetType::Unknown;
    std::string name;                   // nombre para mostrar
    std::filesystem::path path;         // archivo dentro de Assets/ (vacio si es integrado)
    std::filesystem::path source;       // archivo original del que se importo (informativo)
    std::uint64_t size_bytes = 0;
};

}  // namespace cramion::assets

#endif  // CRAMION_CORE_ASSET_TYPES_H
