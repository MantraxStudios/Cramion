#pragma once

// CramionCore: la capa de juego sobre CramionFX. Un solo include:
//
//   #include <CramionCore/CramionCore.h>
//
//   cramion::ecs::World world;                         // ECS (EnTT)
//   auto coche = world.create("Coche");
//   coche.add<cramion::ecs::MeshRenderer>().model = ...; // AssetRef por UUID
//   cramion::ecs::saveScene(world, "Assets/Scenes/Demo.crscene");
//
//   cramion::ecs::RenderSync sync(asset_manager);      // mundo -> renderizador
//   sync.reset(scene);
//   ... cada frame: scene.update(); sync.sync(world, scene, renderer, dt); renderer.drawFrame(scene);

#include "CramionCore/Uuid.h"
#include "CramionCore/asset/AssetDatabase.h"
#include "CramionCore/cinematics/Cinematics.h"
#include "CramionCore/asset/AssetManager.h"
#include "CramionCore/asset/AssetTypes.h"
#include "CramionCore/asset/Importer.h"
#include "CramionCore/ecs/Components.h"
#include "CramionCore/ecs/MathUtil.h"
#include "CramionCore/ecs/ModelInstantiation.h"
#include "CramionCore/ecs/Reflection.h"
#include "CramionCore/ecs/RenderSync.h"
#include "CramionCore/ecs/SceneSerializer.h"
#include "CramionCore/ecs/Tags.h"
#include "CramionCore/ecs/World.h"
#include "CramionCore/physics/Particles.h"
#include "CramionCore/physics/PhysicsComponents.h"
#include "CramionCore/physics/PhysicsSettings.h"
#include "CramionCore/physics/PhysicsSystem.h"
#include "CramionCore/project/Project.h"
