#ifndef CRAMION_CORE_ECS_RIGGING_H
#define CRAMION_CORE_ECS_RIGGING_H

// Configuracion automatica a partir del esqueleto de un modelo (botones del
// Inspector y la API de scripts): huesos del ragdoll, IK de animales (patas al
// suelo, cabeza y cuello) y phys bones (pelo, colas, orejas, faldas).

#include "CramionCore/ecs/Components.h"
#include "CramionCore/ecs/World.h"

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

// --- Huesos como objetos (sin RenderSync) ---

// Esqueleto en pose de reposo sin RenderSync: huesos + sus ancestros, padres antes que hijos.
struct BindBone {
    std::string name;
    int node = -1;     // indice en ModelData::nodes
    int parent = -1;   // indice en el vector devuelto, -1 = raiz
    core::Mat4 global; // relativo a la entidad del modelo
};
std::vector<BindBone> bindSkeleton(const asset::ModelData& data);
// Crea entidades con BoneSocket{bone, Follow, drive_position=true, bone_object=true}.
// `only` vacio = todos; nested = siguen la jerarquia del esqueleto. Salta los que ya existen.
std::vector<Entity> createBoneEntities(World& world, Entity skinned, const asset::ModelData& data,
                                       const std::vector<std::string>& only = {}, bool nested = true);
// Un hueso listo para colgarle componentes. nested=false (por defecto): una sola
// entidad hija del modelo (basta para "Socket aqui"); true: tambien su cadena de ancestros.
Entity materializeBone(World& world, Entity skinned, const asset::ModelData& data, const std::string& bone,
                       bool nested = false);
// Busca en el subarbol la entidad con BoneSocket.bone_object && bone == nombre
// (tambien acepta bone == nombre de la entidad, como los objetos de hueso viejos).
Entity findBoneObject(Entity skinned, const std::string& bone);

}  // namespace cramion::ecs

#endif  // CRAMION_CORE_ECS_RIGGING_H
