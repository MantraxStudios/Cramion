#ifndef CRAMION_CORE_ECS_BLUEPRINT_H
#define CRAMION_CORE_ECS_BLUEPRINT_H

// Blueprint de actor (como Unreal): un .crprefab normal con un bloque
// "blueprint" arriba y un .crgraph (Event Graph) al lado. La raiz lleva
// VisualScript{graph = graph_path}. Las instancias se actualizan con el
// sistema de prefabs (revision + syncInstance).

#include "CramionCore/asset/AssetManager.h"
#include "CramionCore/ecs/Prefab.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace cramion::ecs {

inline constexpr const char* kBlueprintKey = "blueprint";

struct BlueprintInfo {
    std::string parent = "Actor";
    std::string graph_path;       // relativo a Assets/ con '/': "Blueprints/BP_Caballero.crgraph"
    Uuid graph{};                 // uuid del .crgraph
    Uuid source{};                // asset de origen (modelo/prefab); invalido = vacio
    std::string description;
    std::string category;
    bool tick = true;
};

bool isBlueprint(const std::string& prefab_text);
bool blueprintInfo(const std::string& prefab_text, BlueprintInfo& out);
// Devuelve el texto con el bloque "blueprint" puesto/reemplazado (resto intacto).
std::string withBlueprintInfo(const std::string& prefab_text, const BlueprintInfo& info);
bool writeBlueprintInfo(const std::filesystem::path& prefab_file, const BlueprintInfo& info,
                        std::string* error = nullptr);

enum class BoneObjects : std::uint8_t { None, All };

struct BlueprintCreateOptions {
    std::string name;                        // "BP_Caballero" (sin extension)
    std::filesystem::path assets_root;       // .../Assets
    std::filesystem::path folder = "Blueprints";  // relativo a assets_root (se crea)
    BoneObjects bones = BoneObjects::None;
    bool add_collider = false;               // BoxCollider ajustado a los limites
    // Se llama con el mundo temporal antes de guardar (p. ej. materiales del editor).
    std::function<void(World&, Entity root)> before_save;
};

struct BlueprintCreateResult {
    std::filesystem::path prefab_file;  // absoluto, .crprefab
    std::filesystem::path graph_file;   // absoluto, .crgraph
    std::string graph_rel;              // relativo a Assets/
    Uuid prefab{};
};

// Nombres unicos: "BP_X.crprefab", "BP_X 1.crprefab"...
std::filesystem::path uniqueAssetPath(const std::filesystem::path& folder, const std::string& stem,
                                      const std::string& extension);

bool createBlueprintFromModel(const assets::ModelAsset& model, const Uuid& model_uuid,
                              const BlueprintCreateOptions& options, BlueprintCreateResult& out,
                              std::string* error = nullptr);
bool createBlueprintFromPrefab(const std::string& prefab_text, const Uuid& prefab_uuid,
                               const BlueprintCreateOptions& options, BlueprintCreateResult& out,
                               std::string* error = nullptr);
bool createEmptyBlueprint(const BlueprintCreateOptions& options, BlueprintCreateResult& out,
                          std::string* error = nullptr);
// Mismo archivo y uuid: anade el bloque y el .crgraph (si la raiz ya tiene
// VisualScript, reutiliza su grafo).
bool convertPrefabToBlueprint(const std::filesystem::path& prefab_file, const std::filesystem::path& assets_root,
                              BlueprintCreateResult& out, std::string* error = nullptr);

}  // namespace cramion::ecs

#endif  // CRAMION_CORE_ECS_BLUEPRINT_H
