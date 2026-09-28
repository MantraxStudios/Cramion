#ifndef CRAMION_CORE_ASSET_MODEL_MATERIALS_H
#define CRAMION_CORE_ASSET_MODEL_MATERIALS_H

// Materiales de un modelo como archivos del proyecto (como "Extract
// Materials" + el remapeo del importador de modelos de Unity).
//
// extractModelMaterials() crea un .crmat por cada material distinto de las
// piezas del modelo, con sus factores y sus texturas:
//   - las incrustadas en el .crdata se sacan a imagenes de Assets (los canales
//     empaquetados, metal/rugosidad de glTF y oclusion, en mapas sueltos);
//   - si un material no trae textura de color (un FBX con las rutas rotas, un
//     pack con las texturas aparte), se busca en el proyecto por el nombre del
//     material, de la malla o del modelo con los sufijos habituales
//     (_BaseColor, _Albedo, _Normal, _Roughness...) y se completa el resto del
//     juego de mapas con materialFromImage().
//
// El resultado es un mapa nombre de material -> .crmat que el editor guarda
// por modelo (ProjectSettings/ModelMaterials.json) y aplica a cada instancia:
// al ponerlo en la escena ya sale con sus materiales.

#include "CramionCore/Uuid.h"
#include "CramionCore/asset/AssetTypes.h"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace cramion::assets {

// Nombre del material (el de ModelData::materials) -> .crmat.
using ModelMaterialMap = std::map<std::string, Uuid>;

struct ExtractMaterialsResult {
    bool ok = false;
    ModelMaterialMap map;
    int created = 0;             // .crmat nuevos
    int reused = 0;              // ya existian (no se sobrescriben: pueden estar editados)
    int textures_extracted = 0;  // imagenes sacadas del .crdata
    int textures_found = 0;      // materiales completados con texturas del proyecto
    std::vector<std::string> without_color;  // materiales sin textura de color
    std::filesystem::path material_folder;
    std::string message;
};

// Nombre de hueco que se usa en el mapa para un material sin nombre.
std::string modelMaterialKey(const std::string& material_name, std::size_t index);

ExtractMaterialsResult extractModelMaterials(const AssetInfo& model, const std::filesystem::path& assets_root);

// Mapas guardados en <settings_folder>/ModelMaterials.json, por UUID del modelo.
ModelMaterialMap loadModelMaterialMap(const std::filesystem::path& settings_folder, const Uuid& model);
bool saveModelMaterialMap(const std::filesystem::path& settings_folder, const Uuid& model, const ModelMaterialMap& map);

}  // namespace cramion::assets

#endif  // CRAMION_CORE_ASSET_MODEL_MATERIALS_H
