#ifndef CRAMION_CORE_ECS_TAGS_H
#define CRAMION_CORE_ECS_TAGS_H

// Tags del proyecto (como el Tag Manager de Unity): la lista de etiquetas que
// se pueden poner a un objeto (EntityInfo::tag), guardada en
// ProjectSettings/Tags.json. Las integradas (Untagged, Respawn, Finish,
// EditorOnly, MainCamera, Player, GameController) no se quitan.
//
// En el codigo: entity.tag(), entity.compareTag("Player"),
// world.findWithTag("Player"), world.findAllWithTag("Enemigo").

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace cramion::ecs {

inline constexpr const char* kUntagged = "Untagged";

std::vector<std::string> defaultTags();
bool isBuiltinTag(std::string_view tag);

// Tags del proyecto abierto (el editor los pone al abrirlo).
const std::vector<std::string>& projectTags();
void setProjectTags(std::vector<std::string> tags);

// Anade el tag si no esta (true si lo anadio).
bool addProjectTag(const std::string& tag);

// Leer/guardar la lista (las integradas siempre van primero).
bool loadTags(const std::filesystem::path& file, std::vector<std::string>& tags);
bool saveTags(const std::filesystem::path& file, const std::vector<std::string>& tags);

}  // namespace cramion::ecs

#endif  // CRAMION_CORE_ECS_TAGS_H
