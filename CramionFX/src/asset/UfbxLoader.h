#ifndef CRAMION_ASSET_UFBX_LOADER_H
#define CRAMION_ASSET_UFBX_LOADER_H

// Cargador de FBX con ufbx (privado de CramionFX). Da lo mismo que el camino
// de assimp de ModelLoader.cpp: jerarquia, mallas partidas por material,
// materiales y texturas (tambien incrustadas), color de vertice, esqueleto y
// pesos, tomas, unidades y formas de mezcla. Lo elige ModelLoader segun
// fbxLoader() (CVar asset.fbx.Loader).

#include "CramionFX/asset/Model.h"

#include <filesystem>
#include <string>
#include <vector>

namespace cramion::asset::detail {

// Como importWithAssimp: `force_static` hornea los nodos en los vertices;
// `keep_hierarchy` deja cada malla en el espacio de su nodo (sin
// animaciones ni clusteres). Lanza std::runtime_error si no se puede leer.
ModelData importFbxWithUfbx(const std::filesystem::path& path, bool force_static, bool keep_hierarchy,
                            const ImportProgressCallback& on_progress);

// Metros por unidad del archivo (UnitScaleFactor / 100); 1 si no se puede leer.
float ufbxFileUnitScale(const std::filesystem::path& path);

// Como loadAnimationClips: solo las tomas, asignadas por nombre a `target`.
std::vector<AnimationClip> ufbxLoadAnimationClips(const std::filesystem::path& path, const std::vector<Node>& target,
                                                  std::string* error, float scale, bool convert_units);

}  // namespace cramion::asset::detail

#endif  // CRAMION_ASSET_UFBX_LOADER_H
