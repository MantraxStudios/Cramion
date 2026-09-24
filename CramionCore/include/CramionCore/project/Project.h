#ifndef CRAMION_CORE_PROJECT_H
#define CRAMION_CORE_PROJECT_H

// CONTRATO COMPARTIDO. Implementacion: CramionCore/src/project/ (target
// CramionAssets).

#include "CramionCore/Uuid.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace cramion::project {

// Un proyecto de Cramion, como los de Unity:
//
//   <Carpeta>/
//     <Nombre>.crproj        JSON: nombre, version del motor, escena inicial
//     Assets/                todo lo del proyecto (.crdata, .crscene) y sus carpetas
//       Scenes/
//     Library/               derivado y regenerable (caches); se puede borrar
//     ProjectSettings/       ajustes del proyecto (JSON)
struct ProjectInfo {
    std::string name;
    std::filesystem::path folder;        // carpeta del proyecto
    std::filesystem::path file;          // el .crproj
    std::string engine_version = "0.1";
    Uuid startup_scene{};                // escena que se abre al cargar (invalida = ninguna)

    std::filesystem::path assetsFolder() const { return folder / "Assets"; }
    std::filesystem::path libraryFolder() const { return folder / "Library"; }
    std::filesystem::path settingsFolder() const { return folder / "ProjectSettings"; }
};

// Crea la estructura de carpetas y el .crproj en `parent / name`. Lanza
// std::runtime_error si la carpeta ya existe y no esta vacia.
ProjectInfo createProject(const std::filesystem::path& parent, const std::string& name);

// Lee un .crproj (o la carpeta que lo contiene). nullopt si no es valido.
std::optional<ProjectInfo> openProject(const std::filesystem::path& path);

// Guarda el .crproj (p. ej. al cambiar la escena inicial).
void saveProject(const ProjectInfo& project);

// Proyectos recientes del hub (en %APPDATA%/Cramion/hub.json), el ultimo
// abierto primero.
struct RecentProject {
    std::string name;
    std::filesystem::path file;
    std::int64_t last_opened = 0;  // segundos desde 1970
};
std::vector<RecentProject> recentProjects();
void addRecentProject(const ProjectInfo& project);
void removeRecentProject(const std::filesystem::path& file);

}  // namespace cramion::project

#endif  // CRAMION_CORE_PROJECT_H
