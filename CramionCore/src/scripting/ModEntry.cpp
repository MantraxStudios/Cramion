#include "CramionCore/scripting/ModEntry.h"

#include "CramionCore/ecs/Prefab.h"
#include "CramionCore/scripting/VisualScript.h"

namespace cramion::project {

ecs::Entity startModEntry(ecs::World& world, const std::filesystem::path& assets_root, const ModMount& mount, std::string* error) {
    if (mount.entry_asset.empty()) return {};
    const std::filesystem::path file = assets_root / std::filesystem::path(std::u8string(mount.entry_asset.begin(), mount.entry_asset.end()));
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) {
        if (error != nullptr) *error = "no existe " + mount.entry_asset;
        return {};
    }
    if (file.extension() == ecs::kPrefabExtension) {
        ecs::Entity e = ecs::instantiatePrefab(world, file);
        if (!e && error != nullptr) *error = "no se pudo instanciar " + mount.entry_asset;
        return e;
    }
    if (file.extension() == vscript::kGraphExtension) {
        ecs::Entity e = world.create("Mod " + mount.id);
        e.add<vscript::VisualScript>().graph = mount.entry_asset;
        return e;
    }
    if (error != nullptr) *error = mount.entry_asset + ": la entrada es un .crgraph o un .crprefab";
    return {};
}

}  // namespace cramion::project
