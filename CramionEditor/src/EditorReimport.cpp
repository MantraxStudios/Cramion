// Reimportar modelos: volver a leer el FBX/OBJ original y, si tiene muchas
// piezas, juntarlas por material (una palmera con 60 hojas sueltas pasa a
// tronco + hojas). Cada pieza es un objeto de la escena con su coste de CPU
// (sincronizar, sombras en cada cascada, gizmos), asi que 5 palmeras
// importadas por nodo eran 1600 objetos y el editor iba a 16 FPS.
//
// El .crdata conserva su UUID y su archivo: lo que lo usa sigue apuntando a
// el. Como sus piezas cambian, las instancias de la escena abierta se
// rehacen (rebuildModelInstances).

#include "EditorApp.h"

#include "Dialogs.h"

#include <CramionCore/ecs/ModelInstantiation.h>

#include <iostream>
#include <map>
#include <unordered_set>

namespace cramion::editor {

bool EditorApp::startReimport(const Uuid& model) {
    if (!has_project_ || !database_ || !asset_manager_) return false;
    const std::optional<assets::AssetInfo> info = database_->find(model);
    if (!info || info->type != assets::AssetType::Model || info->path.empty()) return false;
    for (const ImportJob& job : imports_) {
        if (job.reimport == model) return false;  // ya en marcha
    }
    // Copia de seguridad del .crdata de antes (el reimportado lo sustituye).
    {
        std::error_code e;
        const std::filesystem::path backups = project_.libraryFolder() / "ReimportBackups";
        std::filesystem::create_directories(backups, e);
        std::filesystem::copy_file(info->path, backups / (info->path.stem().wstring() + L"_" +
                                                          dialogs::fromUtf8(model.toString()).wstring() + L".crdata"),
                                   std::filesystem::copy_options::overwrite_existing, e);
        if (e) {
            std::cerr << "[Editor] No se pudo guardar la copia de seguridad de " << info->name << ": " << e.message()
                      << " (no se reimporta)\n";
            return false;
        }
    }
    ImportJob job{info->path, info->path.parent_path(), std::make_shared<assets::ImportProgress>(), {}, model, {}};
    // Los materiales de cada pieza de antes: sus nombres casan los .crmat
    // asignados con las piezas nuevas.
    if (const std::shared_ptr<const assets::ModelAsset> old = asset_manager_->loadModel(model)) {
        for (const auto& part : old->parts) {
            std::vector<std::string> names;
            for (const asset::MaterialData& m : part->materials) names.push_back(m.name);
            job.old_materials.push_back(std::move(names));
        }
    }
    std::cout << "[Editor] Reimportando " << info->name << " (combinando mallas)...\n";
    imports_.push_back(std::move(job));
    ++imports_total_;
    pollImports();
    return true;
}

void EditorApp::finishReimport(const ImportJob& job, const assets::ImportResult& result) {
    if (!result.ok) {
        std::cerr << "[Editor] No se pudo reimportar " << dialogs::utf8(job.source.filename()) << ": " << result.message
                  << "\n";
        return;
    }
    // Todo lo cargado del modelo viejo fuera: el renderizador vuelve a subir
    // la escena y las miniaturas se regeneran.
    if (sync_) sync_->reset(scene_);
    if (asset_manager_) asset_manager_->unload(job.reimport);
    model_previews_.invalidate();
    const int rebuilt = rebuildModelInstances(job.reimport, job.old_materials);
    std::cout << "[Editor] Reimportado " << result.message << "; " << rebuilt << " instancia(s) rehecha(s)\n";
}

namespace {

// Entidad "de modelo": solo nodos del modelo (Transform y, si acaso, su
// MeshRenderer del mismo modelo). Si tiene algo mas es del usuario.
bool isModelNode(ecs::World& world, ecs::Entity e, const Uuid& model) {
    const ecs::MeshRenderer* mr = e.tryGet<ecs::MeshRenderer>();
    if (mr != nullptr && mr->model.uuid != model) return false;
    int components = 0;
    for (const ecs::ComponentType& type : ecs::ComponentRegistry::instance().types()) {
        if (type.name == "Transform" || type.name == "MeshRenderer") continue;
        if (type.has(world, e.handle())) ++components;
    }
    return components == 0;
}

// Todo el subarbol son nodos del modelo.
bool subtreeIsModel(ecs::World& world, ecs::Entity e, const Uuid& model) {
    if (!isModelNode(world, e, model)) return false;
    for (std::size_t i = 0; i < e.childCount(); ++i) {
        if (!subtreeIsModel(world, e.child(i), model)) return false;
    }
    return true;
}

}  // namespace

int EditorApp::rebuildModelInstances(const Uuid& model, const std::vector<std::vector<std::string>>& old_materials) {
    const std::shared_ptr<const assets::ModelAsset> asset = asset_manager_ ? asset_manager_->loadModel(model) : nullptr;
    if (!asset) return 0;

    // Raices de instancia: se sube desde cada MeshRenderer del modelo
    // mientras el padre siga siendo "del modelo" (sin componentes del
    // usuario en ningun sitio de su rama).
    std::unordered_set<entt::entity> roots;
    for (const entt::entity h : world_.registry().view<ecs::MeshRenderer>()) {
        ecs::Entity e = world_.wrap(h);
        if (e.get<ecs::MeshRenderer>().model.uuid != model || e.get<ecs::MeshRenderer>().mesh) continue;
        ecs::Entity root = e;
        for (ecs::Entity p = e.parent(); p.valid() && subtreeIsModel(world_, p, model) &&
                                         p.get<ecs::NameComponent>().name == asset->name;
             p = p.parent()) {
            root = p;
        }
        // Una raiz con hijos que no son del modelo (se anadieron a mano): se
        // sube hasta ella solo si todo lo de debajo es del modelo.
        roots.insert(root.handle());
    }
    // Una raiz dentro de otra (pasa si la pieza se llama como el modelo):
    // solo la de arriba.
    std::vector<ecs::Entity> list;
    for (const entt::entity h : roots) {
        bool nested = false;
        for (ecs::Entity p = world_.wrap(h).parent(); p.valid() && !nested; p = p.parent()) {
            nested = roots.count(p.handle()) != 0;
        }
        if (!nested) list.push_back(world_.wrap(h));
    }

    int rebuilt = 0;
    int dropped_components = 0;
    for (ecs::Entity root : list) {
        // .crmat por nombre de material, y Static, de todo el subarbol viejo.
        std::map<std::string, assets::AssetRef> overrides;
        bool any_static = root.get<ecs::EntityInfo>().is_static;
        ecs::ShadowCasting shadows = ecs::ShadowCasting::On;
        const std::function<void(ecs::Entity)> collect = [&](ecs::Entity e) {
            any_static = any_static || e.get<ecs::EntityInfo>().is_static;
            if (const ecs::MeshRenderer* mr = e.tryGet<ecs::MeshRenderer>(); mr && mr->model.uuid == model) {
                shadows = mr->cast_shadows;
                const auto part = static_cast<std::size_t>(std::max(mr->part, 0));
                for (std::size_t slot = 0; slot < mr->materials.size(); ++slot) {
                    if (!mr->materials[slot].valid()) continue;
                    const std::string name = part < old_materials.size() && slot < old_materials[part].size()
                                                 ? old_materials[part][slot]
                                                 : std::string{};
                    overrides[name] = mr->materials[slot];
                }
            }
            for (std::size_t i = 0; i < e.childCount(); ++i) collect(e.child(i));
        };
        collect(root);

        // Hijos viejos fuera (lo de usuario en ellos se pierde: se cuenta).
        std::vector<ecs::Entity> children;
        for (std::size_t i = 0; i < root.childCount(); ++i) children.push_back(root.child(i));
        for (ecs::Entity child : children) {
            if (!subtreeIsModel(world_, child, model)) ++dropped_components;
            world_.destroy(child);
        }
        if (root.has<ecs::MeshRenderer>()) root.remove<ecs::MeshRenderer>();

        // Jerarquia nueva debajo de una copia temporal y se pasa a la raiz
        // (la raiz se queda: UUID, Transform, nombre, padre y lo demas).
        ecs::Entity fresh = ecs::instantiateModel(world_, *asset, {});
        if (const ecs::MeshRenderer* mr = fresh.tryGet<ecs::MeshRenderer>()) root.add<ecs::MeshRenderer>(*mr);
        std::vector<ecs::Entity> fresh_children;
        for (std::size_t i = 0; i < fresh.childCount(); ++i) fresh_children.push_back(fresh.child(i));
        for (ecs::Entity c : fresh_children) c.setParent(root, /*keep_world=*/false);
        world_.destroy(fresh);

        // Materiales, sombras y Static en las piezas nuevas.
        const std::function<void(ecs::Entity)> apply = [&](ecs::Entity e) {
            if (any_static) e.get<ecs::EntityInfo>().is_static = true;
            if (ecs::MeshRenderer* mr = e.tryGet<ecs::MeshRenderer>(); mr && mr->model.uuid == model) {
                mr->cast_shadows = shadows;
                const auto part = static_cast<std::size_t>(std::max(mr->part, 0));
                if (part < asset->parts.size() && !overrides.empty()) {
                    const auto& materials = asset->parts[part]->materials;
                    mr->materials.assign(materials.size(), assets::AssetRef{{}, assets::AssetType::Material});
                    bool any = false;
                    for (std::size_t slot = 0; slot < materials.size(); ++slot) {
                        auto it = overrides.find(materials[slot].name);
                        if (it == overrides.end() && overrides.size() == 1) it = overrides.begin();
                        if (it != overrides.end()) {
                            mr->materials[slot] = it->second;
                            any = true;
                        }
                    }
                    if (!any) mr->materials.clear();
                }
            }
            for (std::size_t i = 0; i < e.childCount(); ++i) apply(e.child(i));
        };
        apply(root);
        ++rebuilt;
    }
    if (dropped_components > 0) {
        std::cerr << "[Editor] Reimportar: " << dropped_components
                  << " pieza(s) tenian componentes anadidos a mano y se han quitado\n";
    }
    if (rebuilt > 0) {
        clearSelection();
        commit();
    }
    return rebuilt;
}

}  // namespace cramion::editor
