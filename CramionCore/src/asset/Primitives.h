#ifndef CRAMION_CORE_SRC_ASSET_PRIMITIVES_H
#define CRAMION_CORE_SRC_ASSET_PRIMITIVES_H

// Primitivas integradas (interno de CramionAssets): los assets con UUID fijo
// (builtin::k...) que no tienen archivo. Mismas medidas que las de Unity:
// cubo de 1 m, esfera y capsula de 0.5 m de radio, cilindro y capsula de 2 m
// de alto, plano de 10 x 10 m. Material gris PBR por defecto.

#include "CramionCore/asset/AssetManager.h"

#include <memory>
#include <vector>

namespace cramion::assets::primitives {

const std::vector<AssetInfo>& builtinInfos();
bool isBuiltin(const Uuid& uuid);

// nullptr si el UUID no es de una primitiva.
std::shared_ptr<ModelAsset> make(const Uuid& uuid);

}  // namespace cramion::assets::primitives

#endif  // CRAMION_CORE_SRC_ASSET_PRIMITIVES_H
