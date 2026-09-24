#ifndef CRAMION_CORE_ECS_SCENE_SERIALIZER_H
#define CRAMION_CORE_ECS_SCENE_SERIALIZER_H

// Escenas .crscene (JSON legible, apto para control de versiones):
//
// {
//   "format": "CramionScene", "version": 1,
//   "uuid": "...", "name": "Escena",
//   "entities": [                       // en profundidad: padres antes que hijos,
//     { "uuid": "...", "name": "Coche", //   hermanos en su orden
//       "parent": "..." | null, "active": true, "tag": "", "layer": 0,
//       "components": { "Transform": {...}, "MeshRenderer": {...} } } ]
// }
//
// Al leer se ignoran campos y componentes desconocidos (con aviso) y los que
// faltan se quedan con su valor por defecto: una escena vieja o de una
// version nueva se abre igual.

#include "CramionCore/ecs/World.h"

#include <filesystem>
#include <string>

namespace cramion::ecs {

inline constexpr int kSceneFormatVersion = 1;

// Todo el mundo a texto JSON / de texto JSON (sustituye el contenido).
std::string serializeWorld(const World& world);
bool deserializeWorld(World& world, const std::string& json, std::string* error = nullptr);

bool saveScene(const World& world, const std::filesystem::path& path, std::string* error = nullptr);
bool loadScene(World& world, const std::filesystem::path& path, std::string* error = nullptr);

// Una entidad y sus hijos (copiar/pegar, prefabs). pasteEntities crea copias
// con UUIDs nuevos bajo `parent` y devuelve la raiz.
std::string serializeEntity(const World& world, Entity entity);
Entity pasteEntities(World& world, const std::string& json, Entity parent = {});

// Lee solo el UUID de un .crscene (para la base de datos de assets).
Uuid readSceneUuid(const std::filesystem::path& path);

}  // namespace cramion::ecs

#endif  // CRAMION_CORE_ECS_SCENE_SERIALIZER_H
