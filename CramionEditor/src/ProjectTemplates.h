#ifndef CRAMION_EDITOR_PROJECT_TEMPLATES_H
#define CRAMION_EDITOR_PROJECT_TEMPLATES_H

// Plantillas de proyecto del Hub (como las de Unreal y Unity):
//
//   - Integradas: se generan por codigo (escena, materiales, scripts de Lua,
//     interfaz y ajustes). Vacia, Tercera persona e IA con navegacion.
//   - Del usuario: carpetas en %LOCALAPPDATA%/Cramion/Templates/<nombre>/
//     con template.json, Assets/ y ProjectSettings/ (se copian tal cual al
//     proyecto nuevo). Se crean desde el editor con Archivo > Guardar
//     proyecto como plantilla.

#include <CramionCore/project/Project.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace cramion::editor {

enum class TemplateArt : int { Blank = 0, ThirdPerson = 1, Navigation = 2, User = 3 };

struct ProjectTemplate {
    std::string id;           // "blank", "third_person"... o la carpeta del usuario
    std::string name;
    std::string category;     // "Integradas" / "Mis plantillas"
    std::string description;
    std::vector<std::string> features;
    std::uint32_t accent = 0xFFF28F00;  // RGBA8 (IM_COL32)
    TemplateArt art = TemplateArt::Blank;
    std::filesystem::path folder;  // solo las del usuario
};

// Integradas primero y despues las del usuario.
std::vector<ProjectTemplate> availableTemplates();
std::filesystem::path userTemplatesFolder();

// Crea el proyecto `parent / name` con la plantilla. Lanza std::runtime_error
// si no se puede (la carpeta existe y no esta vacia, plantilla rota...).
project::ProjectInfo createProjectFromTemplate(const ProjectTemplate& t, const std::filesystem::path& parent,
                                               const std::string& name);

// Copia Assets y ProjectSettings del proyecto a una plantilla del usuario.
bool saveProjectAsTemplate(const project::ProjectInfo& project, const std::string& name,
                           const std::string& description, std::string* error = nullptr);

}  // namespace cramion::editor

#endif  // CRAMION_EDITOR_PROJECT_TEMPLATES_H
