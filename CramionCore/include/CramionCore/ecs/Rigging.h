#ifndef CRAMION_CORE_ECS_RIGGING_H
#define CRAMION_CORE_ECS_RIGGING_H

// Configuracion automatica a partir del esqueleto de un modelo (botones del
// Inspector y funciones de Lua): huesos del ragdoll, IK de animales (patas al
// suelo, cabeza y cuello) y phys bones (pelo, colas, orejas, faldas).

#include "CramionCore/ecs/Components.h"

#include <CramionFX/asset/Model.h>

#include <string>
#include <vector>

namespace cramion::ecs {

// Nodo del esqueleto por su nombre, con o sin el prefijo del programa
// ("mixamorig:Head" vale como "Head"). -1 si no esta.
int findBone(const asset::ModelData& data, const std::string& name);

// `scale`: metros por unidad del modelo (la escala de la entidad).
std::vector<RagdollBoneSetting> suggestRagdollBones(const asset::ModelData& data, float scale);

// Rellena el IK para el animal (o humano) del modelo: una cadena al suelo por
// pata, el hueso que mira y su cuello. false si no reconoce el esqueleto.
bool suggestCreatureIK(const asset::ModelData& data, InverseKinematics& ik);

// Cadenas de phys bones por el nombre de los huesos: pelo, coletas, colas,
// orejas, faldas, capas, cintas, antenas, cadenas.
std::vector<PhysBoneChain> suggestPhysBones(const asset::ModelData& data);

}  // namespace cramion::ecs

#endif  // CRAMION_CORE_ECS_RIGGING_H
