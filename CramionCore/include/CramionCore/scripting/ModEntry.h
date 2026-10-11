#ifndef CRAMION_CORE_SCRIPTING_MOD_ENTRY_H
#define CRAMION_CORE_SCRIPTING_MOD_ENTRY_H

// La entrada de un mod en la escena (los mods: project/Mods.h). Va aparte
// porque necesita el ECS, los prefabs y los Visual Scripts, y Mods.cpp esta
// en CramionAssets.

#include "CramionCore/ecs/World.h"
#include "CramionCore/project/Mods.h"

#include <filesystem>
#include <string>

namespace cramion::project {

// Pone en la escena la entrada de un mod montado (assets_root: la carpeta
// Assets del juego): un objeto "Mod <id>" con el Visual Script o la
// instancia del prefab. Entidad vacia si no tiene entrada o falla (error).
ecs::Entity startModEntry(ecs::World& world, const std::filesystem::path& assets_root, const ModMount& mount,
                          std::string* error = nullptr);

}  // namespace cramion::project

#endif  // CRAMION_CORE_SCRIPTING_MOD_ENTRY_H
