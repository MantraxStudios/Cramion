#ifndef CRAMION_CORE_ASSET_XR_CONTROLLER_MODELS_H
#define CRAMION_CORE_ASSET_XR_CONTROLLER_MODELS_H

// Modelos integrados de los mandos de VR (como builtin::kCube): los de Meta
// Quest 3 (Touch Plus), en el espacio de la pose Grip. Hijos de una entidad
// con XR Controller (Grip) en 0,0,0 coinciden con el mando de verdad: sirven
// para ver donde esta y ajustar a ojo el offset de las manos.

#include "CramionCore/asset/AssetTypes.h"

namespace cramion::assets::builtin {

inline constexpr Uuid kQuestControllerLeft{0x00000000c0be0000ull, 0x8000000000000010ull};
inline constexpr Uuid kQuestControllerRight{0x00000000c0be0000ull, 0x8000000000000011ull};

// El del mando de esa mano (0 = izquierda, 1 = derecha).
inline constexpr Uuid questController(int hand) { return hand == 0 ? kQuestControllerLeft : kQuestControllerRight; }

}  // namespace cramion::assets::builtin

#endif  // CRAMION_CORE_ASSET_XR_CONTROLLER_MODELS_H
