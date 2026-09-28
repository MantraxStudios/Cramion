#ifndef CRAMION_CORE_ECS_COMPONENT_PRESETS_H
#define CRAMION_CORE_ECS_COMPONENT_PRESETS_H

// Presets de componentes (como los Presets de Unity o los perfiles de Volume
// de HDRP): un juego de valores guardado con nombre que se aplica a un
// componente de una o varias entidades.
//
//   - De fabrica: el motor trae algunos (Post-procesado: Realista,
//     Cinematografico, Noche, Terror...).
//   - Del proyecto: Assets/Presets/<Componente>/<nombre>.crpreset (JSON con
//     el componente y sus campos), se crean desde el Inspector y viajan con
//     el proyecto.
//
// Al aplicar se vuelve primero a los valores por defecto (no quedan restos del
// preset anterior) y se respetan los campos propios de la entidad: en un
// volumen de post-procesado su forma, tamano, prioridad y peso.

#include "CramionCore/ecs/World.h"

#include <filesystem>
#include <string>
#include <vector>

namespace cramion::ecs {

struct ComponentPreset {
    std::string name;
    std::string component;    // nombre del registro ("PostProcessing")
    std::string fields;       // objeto JSON {"campo": valor}
    std::string description;  // de fabrica: para que sirve
    bool builtin = false;
    std::filesystem::path file;  // del proyecto: su .crpreset
};

inline constexpr const char* kPresetExtension = ".crpreset";

// Presets de fabrica de un componente (vacio si no tiene).
std::vector<ComponentPreset> builtinPresets(const std::string& component);
// Los del proyecto para ese componente (ordenados por nombre).
std::vector<ComponentPreset> projectPresets(const std::filesystem::path& assets_folder, const std::string& component);
// Los de fabrica y los del proyecto.
std::vector<ComponentPreset> allPresets(const std::filesystem::path& assets_folder, const std::string& component);
const ComponentPreset* findPreset(const std::vector<ComponentPreset>& presets, const std::string& name);

// Campos que un preset no toca (los de la propia entidad).
std::vector<std::string> presetExcludedFields(const std::string& component);

// Guarda el componente de la entidad como preset del proyecto. Devuelve el
// archivo (vacio si fallo).
std::filesystem::path savePreset(World& world, Entity entity, const std::string& component, const std::string& name,
                                 const std::filesystem::path& assets_folder, std::string* error = nullptr);
bool readPreset(const std::filesystem::path& file, ComponentPreset& out, std::string* error = nullptr);

// Aplica el preset al componente de la entidad (lo anade si no lo tiene).
bool applyPreset(World& world, Entity entity, const ComponentPreset& preset, std::string* error = nullptr);

}  // namespace cramion::ecs

#endif  // CRAMION_CORE_ECS_COMPONENT_PRESETS_H
