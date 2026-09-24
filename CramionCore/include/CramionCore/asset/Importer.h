#ifndef CRAMION_CORE_ASSET_IMPORTER_H
#define CRAMION_CORE_ASSET_IMPORTER_H

// CONTRATO COMPARTIDO. Implementacion: CramionCore/src/asset/ (target
// CramionAssets).

#include "CramionCore/asset/AssetTypes.h"

#include <filesystem>
#include <string>

namespace cramion::assets {

struct ModelImportSettings {
    // Personaje animado: una sola pieza con esqueleto y animaciones (no se
    // parte en nodos). Si es false, cada malla del archivo es una pieza con
    // su nodo (hijos en la jerarquia al instanciarlo, como Unity).
    bool animated = false;
    // Escala uniforme aplicada al importar (unidades del archivo -> metros).
    float scale = 1.0f;
    // Normal maps de convenio DirectX (+Y hacia abajo).
    bool directx_normals = false;
};

struct ImportResult {
    bool ok = false;
    AssetInfo info;       // el asset creado (uuid, ruta del .crdata...)
    std::string message;  // error o resumen para la consola
};

// Convierte un archivo original en un .crdata dentro de `destination_folder`
// (una carpeta de Assets/). El original NO se copia: el .crdata lo contiene
// todo (texturas incluidas) y lleva un UUID nuevo.
ImportResult importModel(const std::filesystem::path& source,
                         const std::filesystem::path& destination_folder,
                         const ModelImportSettings& settings = {});

// Cielo HDR (.hdr equirectangular): el archivo va incrustado tal cual.
ImportResult importEnvironment(const std::filesystem::path& source,
                               const std::filesystem::path& destination_folder);

// Segun la extension: .obj .fbx .gltf .glb .dae -> modelo, .hdr -> cielo.
ImportResult importAny(const std::filesystem::path& source,
                       const std::filesystem::path& destination_folder);

}  // namespace cramion::assets

#endif  // CRAMION_CORE_ASSET_IMPORTER_H
