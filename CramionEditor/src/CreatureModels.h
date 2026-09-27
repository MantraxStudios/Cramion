#ifndef CRAMION_EDITOR_CREATURE_MODELS_H
#define CRAMION_EDITOR_CREATURE_MODELS_H

// Modelos con esqueleto generados por codigo para la plantilla "Criaturas"
// (y las pruebas): un perro (4 patas de 3 huesos, cuello, cabeza, orejas y
// cola) y un maniqui humanoide (nombres de Mixamo, con coleta). Mallas de
// piezas rigidas por hueso, materiales y animaciones (caminar, quieto).
// Miran a -Z (el "delante" del motor), Y arriba, en metros.

#include <CramionCore/Uuid.h>
#include <CramionFX/asset/Model.h>

#include <filesystem>
#include <string>

namespace cramion::editor {

asset::ModelData makeDogModel();
asset::ModelData makeDummyModel();

// Escribe el modelo como asset animado (.crdata) con ese UUID.
bool writeCreatureModel(const std::filesystem::path& file, const Uuid& uuid, const std::string& name,
                        const asset::ModelData& model, std::string* error = nullptr);

}  // namespace cramion::editor

#endif  // CRAMION_EDITOR_CREATURE_MODELS_H
