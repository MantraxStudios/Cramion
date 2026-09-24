#ifndef CRAMION_CORE_ECS_MODEL_INSTANTIATION_H
#define CRAMION_CORE_ECS_MODEL_INSTANTIATION_H

// Poner un modelo importado en la escena, como arrastrar un FBX en Unity: una
// entidad por nodo del modelo (con su Transform local) y un MeshRenderer en
// los nodos que tienen malla. Los animados llevan ademas un Animator.

#include "CramionCore/asset/AssetManager.h"
#include "CramionCore/ecs/World.h"

namespace cramion::ecs {

// Crea la jerarquia bajo `parent` (Entity{} = raiz) y devuelve la entidad
// raiz (la del nodo 0). La raiz se llama como el asset.
Entity instantiateModel(World& world, const assets::ModelAsset& model, Entity parent = {});

// Una primitiva integrada (assets::builtin::kCube, kSphere...): una sola
// entidad con su MeshRenderer.
Entity createPrimitive(World& world, const Uuid& builtin, std::string name, Entity parent = {});

// Plantillas de GameObject > Crear (como en Unity).
Entity createEmpty(World& world, Entity parent = {});
Entity createLight(World& world, LightType type, Entity parent = {});
Entity createCamera(World& world, Entity parent = {});
// Escena nueva: camara, luz direccional, cielo y post-procesado global.
void populateDefaultScene(World& world);

}  // namespace cramion::ecs

#endif  // CRAMION_CORE_ECS_MODEL_INSTANTIATION_H
