// Crear materiales de un modelo y asignarlos solos (clic derecho en un
// modelo del Proyecto). La extraccion esta en CramionCore
// (asset/ModelMaterials.h); aqui el menu, el mapa por modelo y ponerlos en las
// instancias.

#include "EditorApp.h"

#include <CramionCore/asset/ModelMaterials.h>

#include <functional>
#include <iostream>

namespace cramion::editor {

bool EditorApp::applyModelMaterials(ecs::Entity root, const Uuid& model) {
    if (!root.valid() || !asset_manager_) return false;
    const assets::ModelMaterialMap map = assets::loadModelMaterialMap(project_.settingsFolder(), model);
    if (map.empty()) return false;
    const std::shared_ptr<const assets::ModelAsset> asset = asset_manager_->loadModel(model);
    if (!asset) return false;

    bool any = false;
    const std::function<void(ecs::Entity)> visit = [&](ecs::Entity e) {
        if (ecs::MeshRenderer* mr = e.tryGet<ecs::MeshRenderer>(); mr && mr->model.uuid == model && !mr->mesh) {
            const auto part = static_cast<std::size_t>(std::max(mr->part, 0));
            if (part < asset->parts.size()) {
                const auto& materials = asset->parts[part]->materials;
                if (mr->materials.size() < materials.size()) {
                    mr->materials.resize(materials.size(), assets::AssetRef{{}, assets::AssetType::Material});
                }
                for (std::size_t slot = 0; slot < materials.size(); ++slot) {
                    // Solo los huecos vacios: lo que el usuario ya cambio se queda.
                    if (mr->materials[slot].valid()) continue;
                    const auto it = map.find(assets::modelMaterialKey(materials[slot].name, slot));
                    if (it == map.end()) continue;
                    mr->materials[slot] = assets::AssetRef{it->second, assets::AssetType::Material};
                    any = true;
                }
            }
        }
        for (std::size_t i = 0; i < e.childCount(); ++i) visit(e.child(i));
    };
    visit(root);
    return any;
}

void EditorApp::createModelMaterials(const std::vector<Uuid>& models) {
    if (!database_ || models.empty()) return;
    int materials = 0;
    int reused = 0;
    int extracted = 0;
    int found = 0;
    int placed = 0;
    std::vector<std::string> missing;
    for (const Uuid& uuid : models) {
        const auto info = database_->find(uuid);
        if (!info || info->type != assets::AssetType::Model || info->path.empty()) continue;
        const assets::ExtractMaterialsResult result = assets::extractModelMaterials(*info, project_.assetsFolder());
        if (!result.ok) {
            std::cerr << "[Materiales] " << info->name << ": " << result.message << "\n";
            continue;
        }
        // Los mapas de antes del modelo se completan (un material que ya no
        // existe en el archivo no estorba).
        assets::ModelMaterialMap map = assets::loadModelMaterialMap(project_.settingsFolder(), uuid);
        for (const auto& [name, material] : result.map) map[name] = material;
        assets::saveModelMaterialMap(project_.settingsFolder(), uuid, map);

        materials += result.created;
        reused += result.reused;
        extracted += result.textures_extracted;
        found += result.textures_found;
        for (const std::string& name : result.without_color) missing.push_back(info->name + "/" + name);
        std::cout << "[Materiales] " << info->name << ": " << result.created << " creados, " << result.reused
                  << " ya existian, " << result.textures_extracted << " texturas extraidas, " << result.textures_found
                  << " encontradas en el proyecto -> " << assetRelative(result.material_folder) << "\n";
    }
    // Los .crmat y las imagenes nuevas, a la base de datos antes de usarlos.
    refreshDatabase();

    // Instancias que ya estaban en la escena: sus huecos vacios.
    for (const Uuid& uuid : models) {
        std::vector<ecs::Entity> renderers;
        for (const entt::entity h : world_.registry().view<ecs::MeshRenderer>()) {
            ecs::Entity e = world_.wrap(h);
            if (e.get<ecs::MeshRenderer>().model.uuid == uuid) renderers.push_back(e);
        }
        for (ecs::Entity e : renderers) {
            if (applyModelMaterials(e, uuid)) ++placed;
        }
    }
    if (placed > 0) commit();

    std::cout << "[Materiales] " << models.size() << " modelo(s): " << materials << " materiales nuevos, " << reused
              << " reutilizados, " << extracted << " texturas extraidas, " << found
              << " con texturas del proyecto; asignados en " << placed << " pieza(s) de la escena\n";
    if (!missing.empty()) {
        std::string list;
        for (std::size_t i = 0; i < missing.size() && i < 8; ++i) list += (i ? ", " : "") + missing[i];
        if (missing.size() > 8) list += "...";
        std::cout << "[Materiales] Sin textura de color (solo su color): " << list << "\n";
    }
}

}  // namespace cramion::editor
