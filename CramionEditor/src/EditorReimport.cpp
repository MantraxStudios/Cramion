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

bool EditorApp::startReimport(const Uuid& model, std::optional<assets::ModelImportSettings> settings) {
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
    ImportJob job{info->path, info->path.parent_path(), std::make_shared<assets::ImportProgress>(), {}, model, {}, settings};
    // Los materiales de cada pieza de antes: sus nombres casan los .crmat
    // asignados con las piezas nuevas.
    if (const std::shared_ptr<const assets::ModelAsset> old = asset_manager_->loadModel(model)) {
        for (const auto& part : old->parts) {
            std::vector<std::string> names;
            for (const asset::MaterialData& m : part->materials) names.push_back(m.name);
            job.old_materials.push_back(std::move(names));
        }
    }
    std::cout << "[Editor] Reimportando " << info->name << (settings ? " con los ajustes nuevos" : " (combinando mallas)")
              << "...\n";
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
    if (inspected_model_ == job.reimport) model_import_loaded_ = {};  // el panel relee los ajustes y el alto
    std::cout << "[Editor] Reimportado " << result.message << "; " << rebuilt << " instancia(s) rehecha(s)\n";
}

// --- Ajustes de importacion (Inspector) ---------------------------------------

namespace {

// Alto (m) del modelo tal como esta importado: la caja de sus piezas en
// reposo con las transformaciones de sus nodos.
float importedHeight(const assets::AssetInfo& info) {
    const std::shared_ptr<assets::ModelAsset> model =
        assets::AssetManager::readModel(info.uuid, info.path, info.name, false);
    if (!model) return 0.0f;
    std::vector<core::Mat4> global(model->nodes.size(), core::Mat4::identity());
    float low = 1e30f;
    float high = -1e30f;
    for (std::size_t i = 0; i < model->nodes.size(); ++i) {
        const assets::ModelNode& n = model->nodes[i];
        global[i] = n.parent >= 0 && static_cast<std::size_t>(n.parent) < i ? global[static_cast<std::size_t>(n.parent)] * n.local
                                                                               : n.local;
        if (n.part < 0 || static_cast<std::size_t>(n.part) >= model->parts.size() || !model->parts[n.part]) continue;
        for (const asset::SubMesh& s : model->parts[n.part]->submeshes) {
            for (int c = 0; c < 8; ++c) {
                const core::Vec3 p{(c & 1) ? s.bounds_max.x : s.bounds_min.x, (c & 2) ? s.bounds_max.y : s.bounds_min.y,
                                   (c & 4) ? s.bounds_max.z : s.bounds_min.z};
                const float y = global[i].m[0][1] * p.x + global[i].m[1][1] * p.y + global[i].m[2][1] * p.z + global[i].m[3][1];
                low = std::min(low, y);
                high = std::max(high, y);
            }
        }
    }
    return high > low ? high - low : 0.0f;
}

const char* unitName(float meters) {
    if (std::abs(meters - 0.01f) < 1e-4f) return "centimetros";
    if (std::abs(meters - 0.001f) < 1e-5f) return "milimetros";
    if (std::abs(meters - 1.0f) < 1e-4f) return "metros";
    if (std::abs(meters - 0.0254f) < 1e-4f) return "pulgadas";
    if (std::abs(meters - 0.3048f) < 1e-4f) return "pies";
    return nullptr;
}

}  // namespace

void EditorApp::drawModelImportSettings(const Uuid& uuid) {
    const std::optional<assets::AssetInfo> info = database_ ? database_->find(uuid) : std::nullopt;
    if (!info) return;
    if (model_import_loaded_ != uuid) {
        model_import_loaded_ = uuid;
        model_import_saved_ = assets::modelImportSettings(info->path);
        model_import_edit_ = model_import_saved_;
        model_import_height_ = importedHeight(*info);
        model_import_source_ = assets::modelImportSource(info->path);
    }
    assets::ModelImportSettings& s = model_import_edit_;
    ImGui::SetWindowFontScale(1.15f);
    ImGui::TextUnformatted(info->name.c_str());
    ImGui::SetWindowFontScale(1.0f);
    ImGui::TextDisabled("Modelo  -  ajustes de importacion");
    ImGui::Separator();

    const float factor = s.scale * (s.convert_units ? s.file_unit_scale : 1.0f);
    const float saved_factor =
        model_import_saved_.scale * (model_import_saved_.convert_units ? model_import_saved_.file_unit_scale : 1.0f);
    ImGui::SeparatorText("Escala");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::DragFloat("##scale_factor", &s.scale, std::max(s.scale * 0.01f, 0.0001f), 0.0001f, 10000.0f,
                     "Scale Factor  %.4g", ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip("Multiplica el tamano del modelo al importarlo (malla, huesos y animaciones),\n"
                          "como el Scale Factor de Unity. 0.01 pasa de centimetros a metros.");
    for (const float preset : {0.01f, 0.1f, 1.0f, 10.0f, 100.0f}) {
        char label[32];
        std::snprintf(label, sizeof(label), "x%g", static_cast<double>(preset));
        if (ImGui::SmallButton(label)) s.scale = preset;
        ImGui::SameLine();
    }
    ImGui::NewLine();
    ImGui::Checkbox("Convert Units", &s.convert_units);
    ImGui::SameLine();
    if (const char* unit = unitName(s.file_unit_scale)) {
        ImGui::TextDisabled("(el archivo va en %s: x%g)", unit, static_cast<double>(s.file_unit_scale));
    } else {
        ImGui::TextDisabled("(1 unidad del archivo = %g m)", static_cast<double>(s.file_unit_scale));
    }
    ImGui::SetItemTooltip("Usa la unidad que dice el propio archivo. Un FBX de Mixamo, Maya o 3ds Max\n"
                          "suele ir en centimetros y sin esto sale 100 veces mas grande.");
    ImGui::Text("Escala final: x%g", static_cast<double>(factor));
    if (model_import_height_ > 0.0f) {
        const float height = model_import_height_ * (saved_factor > 0.0f ? factor / saved_factor : 1.0f);
        ImGui::Text("Alto del modelo: %.3g m", static_cast<double>(height));
        if (height > 50.0f) {
            ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.3f, 1.0f), "Muy grande: prueba Convert Units o x0.01.");
        } else if (height < 0.02f) {
            ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.3f, 1.0f), "Muy pequeno: prueba x100.");
        }
    }

    ImGui::SeparatorText("Modelo");
    ImGui::Checkbox("Personaje animado (una pieza con esqueleto)", &s.animated);
    ImGui::Checkbox("Normal maps de DirectX (+Y abajo)", &s.directx_normals);
    ImGui::Checkbox("Combinar mallas", &s.combine_meshes);
    if (s.combine_meshes) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90.0f);
        ImGui::DragInt("piezas como mucho", &s.combine_above_parts, 1.0f, 1, 4096);
    }
    ImGui::Dummy(ImVec2(0.0f, 6.0f));
    std::error_code error;
    const bool has_source = !model_import_source_.empty() &&
                            std::filesystem::is_regular_file(dialogs::fromUtf8(model_import_source_), error);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("Original: %s", model_import_source_.empty() ? "(desconocido)" : model_import_source_.c_str());
    if (!has_source) {
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.35f, 1.0f),
                           "No esta el archivo original: para cambiar estos ajustes vuelve a importarlo.");
    }
    ImGui::PopTextWrapPos();
    const bool changed = s.scale != model_import_saved_.scale || s.convert_units != model_import_saved_.convert_units ||
                         s.animated != model_import_saved_.animated || s.directx_normals != model_import_saved_.directx_normals ||
                         s.combine_meshes != model_import_saved_.combine_meshes ||
                         s.combine_above_parts != model_import_saved_.combine_above_parts;
    bool busy = false;
    for (const ImportJob& job : imports_) busy = busy || job.reimport == uuid;
    ImGui::BeginDisabled(!changed || !has_source || busy);
    if (ImGui::Button("Aplicar", ImVec2(120.0f, 0.0f)) && startReimport(uuid, s)) {
        std::cout << "[Editor] " << info->name << ": escala x" << factor << "\n";
    }
    ImGui::SameLine();
    if (ImGui::Button("Revertir", ImVec2(120.0f, 0.0f))) s = model_import_saved_;
    ImGui::EndDisabled();
    if (busy) {
        ImGui::SameLine();
        ImGui::TextDisabled("Reimportando...");
    }
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
