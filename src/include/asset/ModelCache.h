#ifndef CRAMION_ASSET_MODEL_CACHE_H
#define CRAMION_ASSET_MODEL_CACHE_H

#include "asset/Model.h"

#include <filesystem>

namespace cramion::asset {

// Cache binaria de un modelo ya convertido al formato del motor.
//
// Importar un escenario grande con assimp (San Miguel: un OBJ de 1.1 GB,
// ~10 millones de triangulos) tarda minutos: hay que parsear texto, unir
// vertices repetidos y calcular tangentes. El resultado se guarda tal cual
// (vertices, indices, materiales, jerarquia y las texturas aun comprimidas)
// y los arranques siguientes lo leen en segundos.
//
// La cache se invalida sola si el archivo original cambia de tamano o de
// fecha, o si cambia kModelCacheVersion (formato de ModelData).

// Lee la cache si existe y corresponde a `source`. Devuelve false si no.
bool readModelCache(const std::filesystem::path& cache, const std::filesystem::path& source,
                    ModelData& out);

// Escribe la cache. Lanza std::runtime_error si no puede.
void writeModelCache(const std::filesystem::path& cache, const std::filesystem::path& source,
                     const ModelData& model);

}  // namespace cramion::asset

#endif  // CRAMION_ASSET_MODEL_CACHE_H
