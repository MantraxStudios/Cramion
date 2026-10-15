// Blueprints de actor (como Unreal): pestana propia y puntos de entrada.
//
// Un Blueprint es un .crprefab con un bloque "blueprint" y un .crgraph (Event
// Graph) al lado (CramionCore/ecs/Blueprint.h). Su pestana carga el prefab en
// un escenario, como la de un prefab, pero con el dock de Unreal:
//
//   Barra       Compilar, Guardar, Explorar, Buscar, Valores por defecto y
//               las pestanas Ventana grafica / Event Graph
//   Izquierda   Componentes (arbol desde la raiz, huesos y sockets) y Mi
//               Blueprint (variables del grafo)
//   Centro      Ventana grafica (la vista 3D de siempre) o Event Graph
//   Derecha     Detalles (Inspector del objeto, nodo/variable o la clase)
//   Abajo       Resultados de compilacion
//
// Guardar = datos de la clase + el grafo + (si cambio el mundo) el .crprefab
// con applyInstance (revision + 1): sus instancias se ponen al dia al volver a
// la escena. Nada se guarda solo.

#include "EditorApp.h"

#include "BlenderUi.h"
#include "Dialogs.h"

#include <CramionCore/ecs/Blueprint.h>
#include <CramionCore/ecs/Reflection.h>
#include <CramionCore/ecs/Rigging.h>
#include <CramionCore/physics/PhysicsComponents.h>
#include <CramionCore/scripting/VisualScript.h>

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <set>
#include <sstream>

namespace cramion::editor {

namespace {

namespace bl = blender;

std::string lowerText(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// Un nombre de archivo valido: sin los caracteres que Windows no acepta.
std::string cleanAssetName(const std::string& name) {
    std::string out;
    for (const char c : name) out += std::strchr("<>:\"/\\|?*", c) != nullptr || static_cast<unsigned char>(c) < 32 ? '_' : c;
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
    while (!out.empty() && out.front() == ' ') out.erase(out.begin());
    return out;
}

// Lee el bloque "blueprint" y abre su Event Graph (si falta el .crgraph, lo
// crea con el grafo de ejemplo para no dejar la pestana sin grafo).
bool loadBlueprintState(BlueprintEditorState& bp, const Uuid& prefab, const std::filesystem::path& prefab_path,
                        const std::string& text, const std::filesystem::path& assets) {
    bp.prefab = prefab;
    bp.prefab_path = prefab_path;
    if (!ecs::blueprintInfo(text, bp.info)) {
        std::cerr << "[Blueprint] " << dialogs::utf8(prefab_path.filename()) << " no es un Blueprint de actor\n";
        return false;
    }
    if (bp.info.graph_path.empty()) {
        std::cerr << "[Blueprint] " << dialogs::utf8(prefab_path.filename()) << " no tiene Event Graph (graph_path)\n";
        return true;
    }
    const std::filesystem::path graph_file = assets / dialogs::fromUtf8(bp.info.graph_path);
    std::error_code ec;
    if (!std::filesystem::exists(graph_file, ec)) {
        std::cerr << "[Blueprint] Falta " << bp.info.graph_path << ": se crea uno nuevo (¿se renombro o movio a mano?)\n";
        std::filesystem::create_directories(graph_file.parent_path(), ec);
        vscript::saveGraph(vscript::exampleGraph(), graph_file);
    }
    std::string error;
    if (!vsedit::open(bp.graph, graph_file, assets, &error)) {
        std::cerr << "[Blueprint] No se pudo abrir " << bp.info.graph_path << ": " << error << "\n";
    }
    return true;
}

// La primera de su nodo: la que se ve si el nodo no tiene pestanas.
void bringToFront(const char* window_name) {
    ImGuiWindow* window = ImGui::FindWindowByName(window_name);
    if (window == nullptr || window->DockNode == nullptr) return;
    ImVector<ImGuiWindow*>& list = window->DockNode->Windows;
    ImGuiWindow** it = list.find(window);
    if (it == list.end() || it == list.begin()) return;
    list.erase(it);
    list.push_front(window);
}

// Tipo de un objeto del panel Componentes (la columna de la derecha).
const char* componentKind(const ecs::Entity& e, bool root) {
    if (root) return "Self";
    if (const ecs::BoneSocket* s = e.tryGet<ecs::BoneSocket>()) {
        return s->bone_object || s->bone == e.name() ? "Hueso" : "Socket";
    }
    if (e.has<ecs::MeshRenderer>()) return e.has<ecs::Animator>() ? "Malla esqueletal" : "Malla";
    if (e.has<ecs::Light>()) return "Luz";
    if (e.has<ecs::Camera>()) return "Cámara";
    if (e.has<vscript::VisualScript>()) return "Visual Script";
    return e.childCount() > 0 ? "Grupo" : "Objeto";
}

// Esqueleto en pose de reposo de un modelo (bindSkeleton), una vez por modelo.
const std::vector<ecs::BindBone>& cachedSkeleton(const asset::ModelData& data) {
    static const asset::ModelData* cached = nullptr;
    static std::size_t cached_nodes = 0;
    static std::vector<ecs::BindBone> bones;
    if (cached != &data || cached_nodes != data.nodes.size()) {
        cached = &data;
        cached_nodes = data.nodes.size();
        bones = ecs::bindSkeleton(data);
    }
    return bones;
}

}  // namespace

// -----------------------------------------------------------------------------
// Estado y ayudas
// -----------------------------------------------------------------------------

BlueprintEditorState* EditorApp::activeBlueprint() {
    const Workspace* ws = findWorkspace(active_workspace_);
    if (ws == nullptr || ws->kind != WorkspaceKind::Blueprint) return nullptr;
    const auto it = blueprint_editors_.find(ws->id);
    return it != blueprint_editors_.end() ? &it->second : nullptr;
}

bool EditorApp::blueprintDirty(int workspace_id) const {
    const Workspace* ws = findWorkspace(workspace_id);
    if (ws == nullptr || ws->kind != WorkspaceKind::Blueprint) return false;
    const bool world = workspace_id == world_workspace_ ? dirty_ : ws->dirty;
    const auto it = blueprint_editors_.find(workspace_id);
    return world || (it != blueprint_editors_.end() && (it->second.graph.dirty || it->second.info_dirty));
}

bool EditorApp::prefabLikeDirty(int workspace_id) const {
    const Workspace* ws = findWorkspace(workspace_id);
    if (ws == nullptr) return false;
    if (ws->kind == WorkspaceKind::Blueprint) return blueprintDirty(workspace_id);
    if (ws->kind == WorkspaceKind::Prefab) return workspace_id == world_workspace_ ? dirty_ : ws->dirty;
    return false;
}

vsedit::Host EditorApp::blueprintHost() {
    vsedit::Host host;
    host.assets_root = has_project_ ? project_.assetsFolder() : std::filesystem::path{};
    host.debug = [this](const std::string& relative, scripting::ScriptSystem::VisualScriptDebug& out) {
        if (!playing()) return false;
        const std::vector<entt::entity> objects = scripts_.visualScriptObjects(relative);
        if (objects.empty()) return false;
        return scripts_.visualScriptDebug(world_.wrap(objects.front()), out);
    };
    host.reload = [this](const std::string& relative) { scripts_.reloadFile(relative); };
    host.breakpoints = [this](const std::string& file, const std::vector<int>& nodes) { scripts_.setVisualScriptBreakpoints(file, nodes); };
    host.notify = [this](const std::string& toast_title, const std::string& text) { pushToast(toast_title, text, 2); };
    host.entity_names = [this] {
        // "Self" primero (el editor escribe "" = este objeto) y los objetos del actor.
        std::vector<std::string> names{"Self"};
        const ecs::Entity root = prefabStageRoot();
        if (!root.valid()) return names;
        std::vector<ecs::Entity> stack;
        for (const entt::entity child : root.children()) stack.push_back(world_.wrap(child));
        while (!stack.empty()) {
            const ecs::Entity e = stack.back();
            stack.pop_back();
            if (std::find(names.begin(), names.end(), e.name()) == names.end()) names.push_back(e.name());
            for (const entt::entity child : e.children()) stack.push_back(world_.wrap(child));
        }
        return names;
    };
    // Arrastrar un objeto (Componentes o Jerarquia) al Event Graph: la raiz es
    // Self y lo de debajo, su ruta desde la raiz (nunca su Uuid, que cambia en
    // cada instancia).
    host.entity_drop = [this](const Uuid& uuid) {
        vsedit::EntityDrop drop;
        const ecs::Entity e = world_.find(uuid);
        if (!e.valid()) {
            drop.error = "El objeto ya no existe";
            return drop;
        }
        drop.name = e.name();
        const ecs::Entity root = prefabStageRoot();
        if (!root.valid() || (e.handle() != root.handle() && !root.isAncestorOf(e))) {
            drop.error = "\"" + e.name() + "\" no está dentro de este Blueprint";
            return drop;
        }
        drop.ok = true;
        drop.root = e.handle() == root.handle();
        if (!drop.root) drop.path = vscript::entityChildPath(root, e);
        for (const ecs::ComponentType& type : ecs::ComponentRegistry::instance().types()) {
            if (type.has && type.has(world_, e.handle())) drop.components.emplace_back(type.name, type.label);
        }
        return drop;
    };
    return host;
}

std::shared_ptr<const assets::ModelAsset> EditorApp::fullModel(const Uuid& uuid, bool wait) {
    if (sync_) return sync_->completeModel(uuid, scene_, wait);
    if (!asset_manager_) return nullptr;
    return wait ? asset_manager_->loadModel(uuid) : asset_manager_->requestModel(uuid);
}

const asset::ModelData* EditorApp::skinnedModelData(ecs::Entity e, ecs::Entity& skinned,
                                                    std::shared_ptr<const assets::ModelAsset>& keep, bool wait) {
    skinned = {};
    keep.reset();
    if (!e.valid() || !asset_manager_) return nullptr;
    std::vector<ecs::Entity> stack{e};
    while (!stack.empty()) {
        const ecs::Entity x = stack.back();
        stack.pop_back();
        if (const ecs::MeshRenderer* mr = x.tryGet<ecs::MeshRenderer>(); mr != nullptr && !mr->mesh && mr->model.valid()) {
            // Solo su pieza (se llama cada frame con el menu de la Jerarquia
            // o el panel del Blueprint abiertos: fullModel copiaria el modelo
            // entero por objeto). Ya dibujada, la de la escena (en el
            // AssetManager queda vacia y el esqueleto no aparecia).
            const int part = std::max(mr->part, 0);
            const asset::ModelData* data = sync_ ? sync_->partModelData(mr->model.uuid, part, scene_) : nullptr;
            std::shared_ptr<const assets::ModelAsset> model;
            if (data == nullptr) {
                model = wait ? asset_manager_->loadModel(mr->model.uuid) : asset_manager_->requestModel(mr->model.uuid);
                if (model && static_cast<std::size_t>(part) < model->parts.size()) data = model->parts[static_cast<std::size_t>(part)].get();
            }
            if (data != nullptr && !data->bones.empty()) {
                skinned = x;
                keep = std::move(model);
                return data;
            }
        }
        const std::vector<entt::entity>& children = x.children();
        for (auto it = children.rbegin(); it != children.rend(); ++it) stack.push_back(world_.wrap(*it));
    }
    return nullptr;
}

// -----------------------------------------------------------------------------
// Abrir, crear y guardar
// -----------------------------------------------------------------------------

void EditorApp::openBlueprintWorkspace(const Uuid& prefab) {
    if (!has_project_) return;
    if (workspaces_.empty()) resetWorkspaces();
    const std::filesystem::path path = prefabPath(prefab);
    if (path.empty()) {
        std::cerr << "[Blueprint] El asset del Blueprint no existe\n";
        return;
    }
    if (playing()) {
        std::cerr << "[Blueprint] Sal del modo Play para editar el Blueprint\n";
        return;
    }
    for (const Workspace& ws : workspaces_) {
        if (ws.kind == WorkspaceKind::Blueprint && ws.prefab == prefab) {
            requestWorkspace(ws.id);
            return;
        }
    }
    // El mismo prefab abierto como prefab normal (se convirtio despues): esa
    // pestana se cierra si no tiene cambios.
    for (const Workspace& ws : workspaces_) {
        if (ws.kind == WorkspaceKind::Prefab && ws.prefab == prefab) {
            if (prefabLikeDirty(ws.id)) {
                std::cerr << "[Blueprint] Guarda o cierra antes la pestana del prefab " << ws.name << "\n";
                return;
            }
            closeWorkspace(ws.id, false);
            break;
        }
    }
    const int id = next_workspace_id_++;
    // En su sitio del mapa (el documento del grafo no se mueve despues).
    if (!loadBlueprintState(blueprint_editors_[id], prefab, path, prefabText(prefab), project_.assetsFolder())) {
        blueprint_editors_.erase(id);
        return;
    }
    Workspace ws;
    ws.id = id;
    ws.kind = WorkspaceKind::Blueprint;
    ws.name = dialogs::utf8(path.stem());
    ws.prefab = prefab;
    ws.path = path;
    workspaces_.push_back(std::move(ws));
    requestWorkspace(id);
    std::cout << "[Blueprint] Editando " << dialogs::utf8(path.stem())
              << " (Compilar comprueba el Event Graph; Ctrl+S guarda y actualiza sus instancias)\n";
}

bool EditorApp::saveBlueprintWorkspace() {
    Workspace* ws = findWorkspace(world_workspace_);
    if (ws == nullptr || ws->kind != WorkspaceKind::Blueprint) return false;
    if (playing()) {
        std::cerr << "[Blueprint] Sal del modo Play para guardar el Blueprint\n";
        return false;
    }
    const auto it = blueprint_editors_.find(ws->id);
    if (it == blueprint_editors_.end()) return savePrefabWorkspace();
    BlueprintEditorState& bp = it->second;
    const std::filesystem::path path = prefabPath(ws->prefab);
    if (path.empty()) {
        std::cerr << "[Blueprint] El asset del Blueprint ya no existe\n";
        return false;
    }
    bool ok = true;
    // 1. Los datos de la clase, sin subir la revision (applyInstance conserva
    //    el bloque). Primero, para que la cache del prefab no quede vieja.
    if (bp.info_dirty) {
        std::string error;
        if (ecs::writeBlueprintInfo(path, bp.info, &error)) {
            bp.info_dirty = false;
            prefab_texts_.erase(ws->prefab.toString());
        } else {
            std::cerr << "[Blueprint] No se pudieron guardar los datos de la clase: " << error << "\n";
            ok = false;
        }
    }
    // 2. El Event Graph (sin tocar el prefab ni sus instancias).
    if (bp.graph.dirty && !bp.graph.path.empty()) {
        std::string error;
        if (vsedit::save(bp.graph, &error)) {
            scripts_.reloadFile(bp.graph.relative);
        } else {
            std::cerr << "[Blueprint] No se pudo guardar " << bp.graph.relative << ": " << error << "\n";
            ok = false;
        }
    }
    // 3. Los componentes: solo si cambio el mundo (cada guardado del prefab
    //    sube la revision y resincroniza todas las instancias). Lo hecho en
    //    este frame (commit pendiente) cuenta.
    flushCommit();
    if (dirty_) ok = savePrefabWorkspace() && ok;
    if (ok) std::cout << "[Blueprint] " << dialogs::utf8(path.stem()) << " guardado\n";
    updateTitle();
    return ok;
}

void EditorApp::saveBlueprintsBeforePlay() {
    if (blueprint_editors_.empty() || playing()) return;
    flushCommit();  // lo hecho en este frame cuenta como cambio
    const vsedit::Host host = blueprintHost();
    std::vector<int> worlds;
    std::set<int> broken;  // grafos que no compilan: no se guardan
    for (auto& [id, bp] : blueprint_editors_) {
        const Workspace* ws = findWorkspace(id);
        if (ws == nullptr) continue;
        if (bp.graph.dirty && !bp.graph.path.empty()) {
            // Compilar guarda el grafo si no tiene errores; con errores se
            // juega la version guardada (como Unreal, avisando).
            if (!vsedit::compile(bp.graph, host)) {
                broken.insert(id);
                std::cerr << "[Blueprint] " << ws->name
                          << " tiene errores de compilacion: se juega su ultima version guardada\n";
            }
        }
        if (bp.info_dirty || (id == world_workspace_ ? dirty_ : ws->dirty)) worlds.push_back(id);
    }
    // Una vez por Play: enterPlay vuelve a llamar el frame que viene (desde la
    // pestana de la escena) y, si un guardado fallo, cargar otra vez su mundo
    // lo dejaria saltando entre pestanas sin entrar nunca en Play.
    static int last_frame = -100;
    const int frame = ImGui::GetFrameCount();
    if (worlds.empty() || frame - last_frame <= 3) return;
    last_frame = frame;
    for (const int id : worlds) {
        loadWorkspaceWorld(id);
        // saveBlueprintWorkspace guardaria tambien el grafo roto: se aparta.
        BlueprintEditorState& bp = blueprint_editors_[id];
        const bool graph_dirty = bp.graph.dirty;
        if (broken.count(id) != 0) bp.graph.dirty = false;
        saveBlueprintWorkspace();
        if (broken.count(id) != 0) bp.graph.dirty = graph_dirty;
    }
}

bool EditorApp::createActorBlueprint(const Uuid& source, assets::AssetType type, const std::string& name, int bones,
                                     bool collider) {
    return createActorBlueprintIn(source, type, name, bones, collider, {});
}

bool EditorApp::createActorBlueprintIn(const Uuid& source, assets::AssetType type, const std::string& name, int bones,
                                       bool collider, const std::filesystem::path& folder) {
    if (!has_project_) return false;
    if (playing()) {
        std::cerr << "[Blueprint] Sal del modo Play para crear Blueprints\n";
        return false;
    }
    ecs::BlueprintCreateOptions options;
    options.name = cleanAssetName(name);
    if (options.name.empty()) options.name = "BP_Actor";
    options.assets_root = project_.assetsFolder();
    if (!folder.empty()) options.folder = folder;
    options.bones = bones == 1 ? ecs::BoneObjects::All : ecs::BoneObjects::None;
    options.add_collider = collider;
    ecs::BlueprintCreateResult result;
    std::string error;
    bool ok = false;
    if (!source.valid()) {
        ok = ecs::createEmptyBlueprint(options, result, &error);
    } else if (type == assets::AssetType::Model) {
        // Con todas sus mallas (las piezas ya dibujadas estan en la escena).
        const std::shared_ptr<const assets::ModelAsset> model = asset_manager_ ? fullModel(source) : nullptr;
        if (!model) {
            std::cerr << "[Blueprint] No se pudo cargar el modelo\n";
            return false;
        }
        // Con los materiales creados para el modelo (Crear materiales y asignarlos).
        options.before_save = [this, source](ecs::World&, ecs::Entity root) { applyModelMaterials(root, source); };
        ok = ecs::createBlueprintFromModel(*model, source, options, result, &error);
    } else if (type == assets::AssetType::Prefab) {
        const std::string text = prefabText(source);  // copia: refreshDatabase vacia la cache
        if (text.empty()) {
            std::cerr << "[Blueprint] No se pudo leer el prefab\n";
            return false;
        }
        ok = ecs::createBlueprintFromPrefab(text, source, options, result, &error);
    } else if (type == assets::AssetType::Material) {
        // Un cubo con ese material (como arrastrar un material a un cubo); su
        // collider lo pone el cubo (no la raiz).
        options.add_collider = false;
        options.before_save = [source, collider](ecs::World& world, ecs::Entity root) {
            ecs::Entity cube = world.create("Malla", root);
            ecs::MeshRenderer& mr = cube.add<ecs::MeshRenderer>();
            mr.model = assets::AssetRef{assets::builtin::kCube, assets::AssetType::Model};
            mr.materials = {assets::AssetRef{source, assets::AssetType::Material}};
            if (collider) physics::addDefaultCollider(cube);
        };
        ok = ecs::createEmptyBlueprint(options, result, &error);
    } else {
        ok = ecs::createEmptyBlueprint(options, result, &error);
    }
    if (!ok) {
        std::cerr << "[Blueprint] No se pudo crear " << options.name << ": " << error << "\n";
        return false;
    }
    refreshDatabase();
    std::cout << "[Blueprint] Creado " << assetRelative(result.prefab_file) << " (Event Graph: " << result.graph_rel << ")\n";
    openBlueprintWorkspace(result.prefab);
    return true;
}

void EditorApp::convertPrefabToBlueprintAsset(const Uuid& prefab) {
    if (!has_project_) return;
    if (playing()) {
        std::cerr << "[Blueprint] Sal del modo Play para convertir el prefab\n";
        return;
    }
    const std::filesystem::path path = prefabPath(prefab);
    if (path.empty()) return;
    for (const Workspace& ws : workspaces_) {
        if (ws.kind == WorkspaceKind::Prefab && ws.prefab == prefab && prefabLikeDirty(ws.id)) {
            std::cerr << "[Blueprint] Guarda o cierra antes la pestana del prefab " << ws.name << "\n";
            return;
        }
    }
    const std::string text = prefabText(prefab);
    if (ecs::isBlueprint(text)) {
        openBlueprintWorkspace(prefab);
        return;
    }
    // Su raiz ya tenia Visual Script: el Blueprint usa ese grafo (puede que
    // lo compartan otros objetos o prefabs).
    const bool had_graph = text.find("\"VisualScript\"") != std::string::npos;
    ecs::BlueprintCreateResult result;
    std::string error;
    if (!ecs::convertPrefabToBlueprint(path, project_.assetsFolder(), result, &error)) {
        std::cerr << "[Blueprint] No se pudo convertir " << dialogs::utf8(path.filename()) << ": " << error << "\n";
        return;
    }
    if (had_graph) {
        std::cout << "[Blueprint] Aviso: el prefab ya tenia Visual Script; su Event Graph es " << result.graph_rel
                  << " (si otros objetos usan ese grafo, cambiarlo les afecta tambien)\n";
    }
    prefab_texts_.erase(prefab.toString());
    refreshDatabase();
    std::cout << "[Blueprint] " << dialogs::utf8(path.filename()) << " es ahora un Blueprint de actor\n";
    openBlueprintWorkspace(prefab);
}

void EditorApp::requestNewBlueprint(const Uuid& source, assets::AssetType type, const std::string& source_name,
                                    const std::filesystem::path& folder) {
    new_blueprint_ = NewBlueprintRequest{};
    new_blueprint_.open = true;
    new_blueprint_.source = source;
    new_blueprint_.source_type = source.valid() ? type : assets::AssetType::Unknown;
    new_blueprint_.source_name = source_name;
    new_blueprint_.folder = folder;
    std::string base = cleanAssetName(source_name);
    if (base.rfind("BP_", 0) == 0) base.erase(0, 3);
    const std::string name = "BP_" + (base.empty() ? std::string("Actor") : base);
    std::snprintf(new_blueprint_.name, sizeof(new_blueprint_.name), "%s", name.c_str());
}

void EditorApp::drawNewBlueprintModal() {
    NewBlueprintRequest& req = new_blueprint_;
    const char* popup = "Nuevo Blueprint de actor##bp_new";
    if (req.open) {
        ImGui::OpenPopup(popup);
        req.open = false;
        req.focus_name = true;
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(popup, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    const bool from_model = req.source.valid() && req.source_type == assets::AssetType::Model;
    if (req.source.valid()) {
        ImGui::Text("A partir de: %s", req.source_name.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", assets::assetTypeName(req.source_type));
    } else {
        ImGui::TextDisabled("Actor vacío: la raíz (Self) y su Event Graph.");
    }
    ImGui::Spacing();
    if (req.focus_name) {
        ImGui::SetKeyboardFocusHere();
        req.focus_name = false;
    }
    ImGui::SetNextItemWidth(320.0f);
    const bool enter = ImGui::InputText("Nombre", req.name, sizeof(req.name), ImGuiInputTextFlags_EnterReturnsTrue);
    const std::string folder = req.folder.empty() ? std::string("Blueprints") : dialogs::utf8(req.folder);
    ImGui::TextDisabled("Se guarda en Assets/%s (.crprefab y su .crgraph)", folder.c_str());
    if (from_model) {
        ImGui::Spacing();
        ImGui::TextUnformatted("Huesos:");
        ImGui::SameLine();
        ImGui::RadioButton("Ninguno", &req.bones, 0);
        ImGui::SameLine();
        ImGui::RadioButton("Todos", &req.bones, 1);
        ImGui::SetItemTooltip("Un objeto por hueso (siguen la animacion). Con muchos huesos, mejor convertir\n"
                              "solo los que hagan falta desde el panel Componentes (Esqueleto > Convertir en objeto).");
    }
    if (req.source.valid()) {
        ImGui::Checkbox("Colisión (caja ajustada a la malla)", &req.collider);
    }
    ImGui::Separator();
    if (ImGui::Button("Crear", ImVec2(120.0f, 0.0f)) || enter) {
        const NewBlueprintRequest copy = req;
        ImGui::CloseCurrentPopup();
        createActorBlueprintIn(copy.source, copy.source_type, copy.name, copy.bones, copy.collider, copy.folder);
    }
    if (from_model) {
        ImGui::SameLine();
        if (ImGui::Button("Solo ponerlo en la escena")) {
            const Uuid source = req.source;
            ImGui::CloseCurrentPopup();
            instantiateAsset(source, {}, std::nullopt);
        }
        ImGui::SetItemTooltip("Lo de siempre: el modelo en la escena, sin Blueprint.");
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancelar", ImVec2(110.0f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// -----------------------------------------------------------------------------
// La pestana
// -----------------------------------------------------------------------------

void EditorApp::buildBlueprintLayout(unsigned int dockspace_id) {
    const ImVec2 size = ImGui::GetContentRegionAvail();
    ImGui::DockBuilderRemoveNode(dockspace_id);
    ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace_id, ImVec2(std::max(size.x, 400.0f), std::max(size.y, 300.0f)));
    ImGuiID center = dockspace_id;
    ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.2f, nullptr, &center);
    const ImGuiID left_bottom = ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.45f, nullptr, &left);
    const ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.28f, nullptr, &center);
    const ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.2f, nullptr, &center);
    ImGui::DockBuilderDockWindow("Componentes###bpc", left);
    ImGui::DockBuilderDockWindow("Mi Blueprint###bpm", left_bottom);
    ImGui::DockBuilderDockWindow("Detalles###bpd", right);
    ImGui::DockBuilderDockWindow("Resultados de compilación###bpr", bottom);
    // Ventana grafica y Event Graph en el mismo sitio (se elige en la barra).
    ImGui::DockBuilderDockWindow(panelTitle("Escena").c_str(), center);
    ImGui::DockBuilderDockWindow("Event Graph###bpg", center);
    ImGui::DockBuilderFinish(dockspace_id);
}

void EditorApp::drawBlueprintWorkspace() {
    Workspace* ws = findWorkspace(active_workspace_);
    if (ws == nullptr || ws->kind != WorkspaceKind::Blueprint) return;
    auto it = blueprint_editors_.find(ws->id);
    if (it == blueprint_editors_.end()) {
        // Pestana sin estado (no deberia pasar): se vuelve a leer.
        it = blueprint_editors_.try_emplace(ws->id).first;
        if (!loadBlueprintState(it->second, ws->prefab, prefabPath(ws->prefab), prefabText(ws->prefab), project_.assetsFolder())) {
            blueprint_editors_.erase(it);
            return;
        }
    }
    BlueprintEditorState& bp = it->second;
    if (world_workspace_ != ws->id) return;  // su mundo llega el frame que viene
    if (bp.save_requested) {
        bp.save_requested = false;
        saveBlueprintWorkspace();
    }
    const vsedit::Host host = blueprintHost();
    scene_view_visible_ = false;

    // Ventana a toda la pantalla: barra arriba y el dock debajo.
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::kGap);
    const ImGuiWindowFlags host_flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking |
                                        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
                                        ImGuiWindowFlags_NoSavedSettings;
    ImGui::Begin("Blueprint##bp_host", nullptr, host_flags);
    ImGui::PopStyleVar(3);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::kBg0);
    ImGui::BeginChild("##bp_toolbar", ImVec2(0.0f, 30.0f), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    drawBlueprintToolbar(bp);
    ImGui::EndChild();
    ImGui::PopStyleColor();
    const ImGuiID dock = ImHashStr("CramionBlueprintDock");
    if (ImGui::DockBuilderGetNode(dock) == nullptr) buildBlueprintLayout(dock);
    bp.dock = dock;
    bp.layout_built = true;
    ImGui::DockSpace(dock, ImVec2(0.0f, 0.0f));
    if (const ImGuiDockNode* central = ImGui::DockBuilderGetCentralNode(dock)) bp.center_dock = central->ID;
    ImGui::End();
    ImGui::PopStyleColor();

    // Recien abierto: la camara enfoca al actor cuando ya tiene su tamano.
    if (workspace_focus_frames_ > 0 && --workspace_focus_frames_ == 0) {
        if (const ecs::Entity root = prefabStageRoot(); root.valid()) {
            selectOnly(root.uuid());
            focusSelection();
        }
    }

    // Centro: la Ventana grafica (la vista 3D de siempre) o el Event Graph.
    // Solo se envia una de las dos: el nodo del centro ensena la que hay.
    bp.graph_focused = false;
    if (!bp.show_graph) {
        bp.graph.focused = false;  // oculto: Ctrl+Z/Y vuelven al mundo
        drawSceneView();
    } else {
        flying_ = false;
        ImGuiWindowClass graph_class;
        graph_class.DockNodeFlagsOverrideSet = ImGuiDockNodeFlags_NoTabBar;
        ImGui::SetNextWindowClass(&graph_class);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        const bool open = ImGui::Begin("Event Graph###bpg", nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleVar();
        if (open) {
            bringToFront("Event Graph###bpg");
            const vsedit::DrawResult result = vsedit::drawGraph(bp.graph, host, ImGui::GetContentRegionAvail());
            bp.graph_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
            if (result.save) bp.save_requested = true;
            if (result.compile) vsedit::compile(bp.graph, host);
        } else {
            bp.graph.focused = false;
        }
        ImGui::End();
    }

    if (ImGui::Begin("Componentes###bpc")) drawBlueprintComponents(bp);
    ImGui::End();
    if (ImGui::Begin("Mi Blueprint###bpm")) {
        vsedit::drawMyBlueprint(bp.graph, host);
    }
    ImGui::End();
    if (ImGui::Begin("Detalles###bpd")) drawBlueprintDetails(bp);
    ImGui::End();
    if (ImGui::Begin("Resultados de compilación###bpr")) {
        const int before = bp.graph.details_node;
        vsedit::drawCompilerResults(bp.graph);
        // Un clic en un error lleva a su nodo: el Event Graph delante.
        if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && ImGui::IsMouseReleased(ImGuiMouseButton_Left) &&
            (bp.graph.details_node != before || ImGui::IsAnyItemHovered())) {
            bp.show_graph = true;
        }
    }
    ImGui::End();
}

void EditorApp::drawBlueprintToolbar(BlueprintEditorState& bp) {
    const vsedit::Host host = blueprintHost();
    const float frame = ImGui::GetFrameHeight();
    ImGui::SetCursorPos(ImVec2(8.0f, std::max((30.0f - frame) * 0.5f, 0.0f)));
    {
        // Icono y nombre del Blueprint.
        const ImVec2 p = ImGui::GetCursorScreenPos();
        bl::drawGlyph(ImGui::GetWindowDrawList(), bl::Glyph::Blueprint, ImVec2(p.x + frame * 0.5f, p.y + frame * 0.5f),
                      frame * 0.8f, IM_COL32(80, 150, 255, 255));
        ImGui::Dummy(ImVec2(frame, frame));
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(dialogs::utf8(bp.prefab_path.stem()).c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", bp.info.parent.c_str());
        ImGui::SameLine(0.0f, 16.0f);
    }
    vsedit::drawCompileButton(bp.graph, host);
    ImGui::SameLine();
    const bool dirty = blueprintDirty(active_workspace_);
    ImGui::BeginDisabled(playing());
    if (ImGui::Button(dirty ? "Guardar *" : "Guardar")) bp.save_requested = true;
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Ctrl+S: el Event Graph, los datos de la clase y, si cambiaron los componentes,\n"
                          "el .crprefab (sus instancias de la escena se actualizan al volver).");
    ImGui::SameLine();
    if (ImGui::Button("Explorar")) revealInProject(bp.prefab_path);
    ImGui::SetItemTooltip("Buscar el Blueprint en el panel Proyecto");
    ImGui::SameLine();
    if (ImGui::Button("Buscar")) {
        bp.find_filter.clear();
        ImGui::OpenPopup("##bp_find");
    }
    ImGui::SetItemTooltip("Buscar un nodo del Event Graph");
    if (ImGui::BeginPopup("##bp_find")) {
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(280.0f);
        ImGui::InputTextWithHint("##filtro", "Nombre del nodo...", &bp.find_filter);
        const std::string needle = lowerText(bp.find_filter);
        ImGui::BeginChild("##bp_find_list", ImVec2(280.0f, 240.0f));
        int shown = 0;
        for (const vscript::Node& node : bp.graph.graph.nodes) {
            const std::string title = vscript::nodeTitle(node);
            const std::string haystack = lowerText(title + " " + node.kind + " " + node.fn + " " + node.comment);
            if (!needle.empty() && haystack.find(needle) == std::string::npos) continue;
            ImGui::PushID(node.id);
            if (ImGui::Selectable(title.c_str())) {
                bp.show_graph = true;
                bp.graph.details_node = node.id;
                bp.graph.canvas.frameNode(node.id);
                ImGui::CloseCurrentPopup();
            }
            ImGui::PopID();
            ++shown;
        }
        if (shown == 0) ImGui::TextDisabled("Ningún nodo");
        ImGui::EndChild();
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Valores por defecto")) bp.details = BlueprintEditorState::Details::Class;
    ImGui::SetItemTooltip("Detalles de la clase: descripcion, categoria y valores de las variables");

    // Pestanas del centro.
    ImGui::SameLine(0.0f, 24.0f);
    if (bl::workspaceTab("##bp_view", "Ventana gráfica", !bp.show_graph, false, IM_COL32(80, 150, 255, 255), nullptr)) {
        bp.show_graph = false;
    }
    ImGui::SameLine();
    if (bl::workspaceTab("##bp_graph", "Event Graph", bp.show_graph, bp.graph.dirty, IM_COL32(80, 150, 255, 255), nullptr)) {
        bp.show_graph = true;
    }

    // A la derecha: volver a la escena.
    const char* back = "Volver a la escena";
    const float back_w = ImGui::CalcTextSize(back).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    const float after_tabs = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x + 12.0f;
    ImGui::SameLine(std::max(ImGui::GetWindowWidth() - back_w - 8.0f, after_tabs));
    if (ImGui::Button(back)) requestWorkspace(0);
}

void EditorApp::drawBlueprintComponents(BlueprintEditorState& bp) {
    const ecs::Entity root = prefabStageRoot();
    std::shared_ptr<const assets::ModelAsset> model_keep;
    ecs::Entity skinned;
    const asset::ModelData* skeleton = root.valid() ? skinnedModelData(root, skinned, model_keep, false) : nullptr;

    // Cabecera: + Anadir y buscar.
    ImGui::PushStyleColor(ImGuiCol_Button, theme::kAccent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::kAccentHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme::kAccentActive);
    if (ImGui::Button("+ Añadir")) ImGui::OpenPopup("##bp_add");
    ImGui::PopStyleColor(3);
    ImGui::SetItemTooltip("Objetos (dentro del elegido), componentes y sockets en los huesos");
    ImGui::SameLine();
    theme::searchBox("bp_components_filter", bp.component_filter, "Buscar...",
                     std::max(ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight() - 6.0f, 60.0f));
    ImGui::SameLine(0.0f, 4.0f);
    if (bl::iconButton("##bp_show_bones", bl::Glyph::Bone, bp.show_bones, "Ver los objetos de hueso")) {
        bp.show_bones = !bp.show_bones;
    }

    bool open_add_component = false;
    if (ImGui::BeginPopup("##bp_add")) {
        ecs::Entity parent = world_.find(active_);
        if (!parent.valid() || !root.valid() || (parent != root && !root.isAncestorOf(parent))) parent = root;
        const auto item = [&](const char* label, int kind) {
            if (ImGui::MenuItem(label)) createEntity(kind, parent);
        };
        ImGui::SeparatorText("Objeto");
        item("Vacío", 0);
        item("Cubo", 1);
        item("Esfera", 2);
        item("Cápsula", 5);
        item("Luz puntual", 7);
        item("Foco", 8);
        item("Cámara", 9);
        ImGui::SeparatorText("Componente");
        const ecs::Entity target = world_.find(active_);
        if (ImGui::MenuItem("Componente en el objeto elegido...", nullptr, false, target.valid())) open_add_component = true;
        if (ImGui::BeginMenu("Socket en hueso...", skeleton != nullptr)) {
            const std::vector<ecs::BindBone>& bones = cachedSkeleton(*skeleton);
            // Id por indice: los esqueletos importados pueden repetir (o dejar vacios) nombres de hueso.
            for (std::size_t i = 0; i < bones.size(); ++i) {
                const ecs::BindBone& b = bones[i];
                ImGui::PushID(static_cast<int>(i));
                const bool picked = ImGui::MenuItem(b.name.c_str());
                ImGui::PopID();
                if (picked) {
                    ecs::Entity socket = world_.create("Socket " + b.name, skinned);
                    socket.add<ecs::BoneSocket>().bone = b.name;
                    selectOnly(socket.uuid());
                    bp.details = BlueprintEditorState::Details::Entity;
                    commit();
                }
            }
            ImGui::EndMenu();
        }
        if (skeleton == nullptr) ImGui::SetItemTooltip("El actor no tiene un modelo con esqueleto");
        ImGui::EndPopup();
    }
    if (open_add_component) ImGui::OpenPopup("add_component");
    if (const ecs::Entity target = world_.find(active_); target.valid()) {
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(std::clamp(viewport->WorkSize.x * 0.6f, 560.0f, 1100.0f),
                                        std::clamp(viewport->WorkSize.y * 0.72f, 420.0f, 860.0f)),
                                 ImGuiCond_Appearing);
        drawAddComponent(target);
    }
    ImGui::Separator();

    ImGui::BeginChild("##bp_tree");
    if (!root.valid()) {
        ImGui::TextDisabled("Se borró la raíz del actor: Ctrl+Z para recuperarla.");
        ImGui::EndChild();
        return;
    }
    const std::string filter = lowerText(bp.component_filter);
    const std::function<bool(const ecs::Entity&)> matches = [&](const ecs::Entity& e) {
        if (filter.empty() || lowerText(e.name()).find(filter) != std::string::npos) return true;
        for (const entt::entity child : e.children()) {
            if (matches(world_.wrap(child))) return true;
        }
        return false;
    };
    const float icon_size = ImGui::GetTextLineHeight();
    // Soltar objetos sobre `target`: pasan a ser sus hijos.
    const auto drop_on = [&](ecs::Entity target) {
        if (!ImGui::BeginDragDropTarget()) return;
        if (ImGui::AcceptDragDropPayload(kEntityPayload) != nullptr) {
            bool moved = false;
            for (ecs::Entity e : draggedEntities()) {
                if (e == target || e == root || e.isAncestorOf(target)) continue;
                moved = e.setParent(target, true) || moved;
            }
            if (moved) commit();
        }
        ImGui::EndDragDropTarget();
    };
    bool changed = false;  // el mundo cambio: no seguir con filas viejas
    std::function<void(ecs::Entity, int)> draw_row = [&](ecs::Entity e, int depth) {
        if (changed || !e.valid() || !matches(e)) return;
        const bool is_root = e == root;
        const ecs::BoneSocket* socket = e.tryGet<ecs::BoneSocket>();
        const bool is_bone = socket != nullptr && (socket->bone_object || socket->bone == e.name());
        if (is_bone && !bp.show_bones) {
            // Oculto, pero sus hijos (sockets, colliders) siguen a la vista.
            for (const entt::entity child : std::vector<entt::entity>(e.children())) draw_row(world_.wrap(child), depth);
            return;
        }
        const Uuid uuid = e.uuid();
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                   ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_FramePadding;
        if (e.childCount() == 0) flags |= ImGuiTreeNodeFlags_Leaf;
        if (isSelected(uuid)) flags |= ImGuiTreeNodeFlags_Selected;
        if (is_root) flags |= ImGuiTreeNodeFlags_DefaultOpen;
        if (!filter.empty()) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
        const std::string label = "      " + (is_root ? e.name() + " (Self)" : e.name());
        const bool open =
            ImGui::TreeNodeEx(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(entt::to_integral(e.handle())) + 1),
                              flags, "%s", label.c_str());
        const ImVec2 min = ImGui::GetItemRectMin();
        const ImVec2 max = ImGui::GetItemRectMax();
        const ImVec2 icon_at(min.x + ImGui::GetTreeNodeToLabelSpacing(), min.y + (max.y - min.y - icon_size) * 0.5f);
        ImDrawList* draw = ImGui::GetWindowDrawList();
        if (is_root) {
            bl::drawGlyph(draw, bl::Glyph::Blueprint, ImVec2(icon_at.x + icon_size * 0.5f, icon_at.y + icon_size * 0.5f),
                          icon_size, IM_COL32(80, 150, 255, 255));
        } else if (is_bone) {
            bl::drawGlyph(draw, bl::Glyph::Bone, ImVec2(icon_at.x + icon_size * 0.5f, icon_at.y + icon_size * 0.5f), icon_size,
                          theme::kTextDim);
        } else if (socket != nullptr) {
            bl::drawGlyph(draw, bl::Glyph::Pivot, ImVec2(icon_at.x + icon_size * 0.5f, icon_at.y + icon_size * 0.5f), icon_size,
                          theme::kYellow);
        } else if (e.has<ecs::MeshRenderer>()) {
            bl::drawGlyph(draw, bl::Glyph::Mesh, ImVec2(icon_at.x + icon_size * 0.5f, icon_at.y + icon_size * 0.5f), icon_size,
                          theme::kLabel);
        } else {
            ImU32 tint = theme::kText;
            const Icon icon = entityIcon(e, tint);
            imgui_.drawIcon(draw, icon, icon_at, icon_size, tint);
        }
        // El tipo, a la derecha y en gris.
        {
            const char* kind = componentKind(e, is_root);
            const ImVec2 ts = ImGui::CalcTextSize(kind);
            const float x = max.x - ts.x - 6.0f;
            if (x > icon_at.x + ImGui::CalcTextSize(label.c_str()).x + 12.0f) {
                draw->AddText(ImVec2(x, min.y + (max.y - min.y - ts.y) * 0.5f), theme::kTextFaint, kind);
            }
        }
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen()) {
            if (ImGui::GetIO().KeyCtrl) {
                toggleSelection(uuid);
            } else {
                selectOnly(uuid);
            }
            bp.details = BlueprintEditorState::Details::Entity;
        }
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) focusSelection();
        // La raiz tambien se arrastra (al Event Graph = Self); soltarla en el
        // arbol no la mueve (drop_on la salta).
        if (ImGui::BeginDragDropSource()) {
            drag_single_ = isSelected(uuid) && !is_root ? Uuid{} : uuid;
            ImGui::SetDragDropPayload(kEntityPayload, &uuid, sizeof(Uuid));
            ImGui::TextUnformatted(e.name().c_str());
            ImGui::EndDragDropSource();
        }
        drop_on(e);
        // Un id por fila: los hermanos comparten la pila de ids del arbol.
        ImGui::PushID(static_cast<int>(entt::to_integral(e.handle())));
        if (ImGui::BeginPopupContextItem("##bp_row_menu")) {
            if (!isSelected(uuid)) selectOnly(uuid);
            if (ImGui::MenuItem("Enfocar", "F")) focusSelection();
            if (!is_root && ImGui::MenuItem("Duplicar", "Ctrl+D")) {
                duplicateSelection();
                changed = true;
            }
            ecs::Entity bone_skinned;
            std::shared_ptr<const assets::ModelAsset> keep;
            if (skinnedModelData(e, bone_skinned, keep, false) != nullptr && ImGui::MenuItem("Huesos como objetos (todos)")) {
                createBoneObjects(e);
                changed = true;
            }
            if (!is_root) {
                ImGui::Separator();
                if (ImGui::MenuItem("Borrar", "Supr")) {
                    deleteSelection();
                    changed = true;
                }
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
        if (open) {
            if (!changed) {
                for (const entt::entity child : std::vector<entt::entity>(e.children())) {
                    if (changed) break;
                    draw_row(world_.wrap(child), depth + 1);
                }
            }
            ImGui::TreePop();
        }
    };
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f, 1.0f));
    theme::pushSelectionColors();
    draw_row(root, 0);

    // Esqueleto: los huesos que aun no son objeto (filas virtuales).
    if (!changed && skeleton != nullptr && skinned.valid()) {
        std::set<std::string> existing;
        std::vector<ecs::Entity> stack{skinned};
        while (!stack.empty()) {
            const ecs::Entity x = stack.back();
            stack.pop_back();
            if (const ecs::BoneSocket* s = x.tryGet<ecs::BoneSocket>(); s != nullptr && (s->bone_object || s->bone == x.name())) {
                existing.insert(s->bone);
            }
            for (const entt::entity child : x.children()) stack.push_back(world_.wrap(child));
        }
        std::vector<const ecs::BindBone*> virtual_bones;
        const std::vector<ecs::BindBone>& skeleton_bones = cachedSkeleton(*skeleton);
        for (const ecs::BindBone& b : skeleton_bones) {
            if (existing.count(b.name) != 0) continue;
            if (!filter.empty() && lowerText(b.name).find(filter) == std::string::npos) continue;
            virtual_bones.push_back(&b);
        }
        if (!virtual_bones.empty()) {
            if (!filter.empty()) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
            const std::string group = "      Esqueleto (" + std::to_string(virtual_bones.size()) + ")";
            const bool open = ImGui::TreeNodeEx("##bp_skeleton", ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_FramePadding,
                                                "%s", group.c_str());
            const ImVec2 min = ImGui::GetItemRectMin();
            const ImVec2 max = ImGui::GetItemRectMax();
            bl::drawGlyph(ImGui::GetWindowDrawList(), bl::Glyph::Bone,
                          ImVec2(min.x + ImGui::GetTreeNodeToLabelSpacing() + icon_size * 0.5f, (min.y + max.y) * 0.5f), icon_size,
                          theme::kTextDim);
            ImGui::SetItemTooltip("Huesos de %s que aun no son objetos. Clic derecho en uno: Convertir en objeto\n"
                                  "o Socket aqui. Soltar un objeto sobre un hueso lo convierte y lo emparenta.",
                                  skinned.name().c_str());
            if (open) {
                for (const ecs::BindBone* b : virtual_bones) {
                    if (changed) break;
                    // Id por indice en el esqueleto: el nombre del hueso puede repetirse.
                    ImGui::PushID(static_cast<int>(b - skeleton_bones.data()));
                    const std::string row = "      " + b->name;
                    ImGui::TreeNodeEx("##bone", ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                    ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_FramePadding,
                                      "%s", row.c_str());
                    const ImVec2 rmin = ImGui::GetItemRectMin();
                    const ImVec2 rmax = ImGui::GetItemRectMax();
                    bl::drawGlyph(ImGui::GetWindowDrawList(), bl::Glyph::Bone,
                                  ImVec2(rmin.x + ImGui::GetTreeNodeToLabelSpacing() + icon_size * 0.5f, (rmin.y + rmax.y) * 0.5f),
                                  icon_size * 0.85f, theme::kTextFaint);
                    const auto materialize = [&]() {
                        const ecs::Entity bone = ecs::materializeBone(world_, skinned, *skeleton, b->name, true);
                        if (bone.valid()) {
                            selectOnly(bone.uuid());
                            bp.details = BlueprintEditorState::Details::Entity;
                        }
                        return bone;
                    };
                    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        if (materialize().valid()) commit();
                        changed = true;
                    }
                    if (!changed && ImGui::BeginDragDropTarget()) {
                        if (ImGui::AcceptDragDropPayload(kEntityPayload) != nullptr) {
                            const std::vector<ecs::Entity> dragged = draggedEntities();
                            // La raiz sola (se arrastra para el Event Graph) no crea el hueso.
                            const bool movable = std::any_of(dragged.begin(), dragged.end(),
                                                             [&](const ecs::Entity& d) { return d != root; });
                            if (const ecs::Entity bone = movable ? materialize() : ecs::Entity{}; bone.valid()) {
                                for (ecs::Entity e : dragged) {
                                    if (e == root || e == bone || e.isAncestorOf(bone)) continue;
                                    e.setParent(bone, true);
                                }
                                commit();
                            }
                            changed = true;
                        }
                        ImGui::EndDragDropTarget();
                    }
                    if (!changed && ImGui::BeginPopupContextItem("##bone_menu")) {
                        ImGui::TextDisabled("%s", b->name.c_str());
                        ImGui::Separator();
                        if (ImGui::MenuItem("Convertir en objeto")) {
                            if (materialize().valid()) commit();
                            changed = true;
                        }
                        ImGui::SetItemTooltip("Un objeto que sigue a este hueso (y su cadena de huesos padre):\n"
                                              "se le pueden anadir componentes (luz, collider...).");
                        if (ImGui::MenuItem("Socket aquí")) {
                            ecs::Entity s = world_.create("Socket " + b->name, skinned);
                            s.add<ecs::BoneSocket>().bone = b->name;
                            selectOnly(s.uuid());
                            bp.details = BlueprintEditorState::Details::Entity;
                            commit();
                            changed = true;
                        }
                        ImGui::SetItemTooltip("Un objeto hijo de la malla que sigue a este hueso (para colgar un arma o un efecto).");
                        ImGui::EndPopup();
                    }
                    ImGui::PopID();
                }
                ImGui::TreePop();
            }
        }
    }
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar();

    // Espacio vacio: soltar = hijo de la raiz; clic = deseleccionar.
    const ImVec2 rest = ImGui::GetContentRegionAvail();
    ImGui::InvisibleButton("##bp_empty", ImVec2(std::max(rest.x, 1.0f), std::max(rest.y, 30.0f)));
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) clearSelection();
    if (!changed) drop_on(root);
    ImGui::EndChild();

    // Atajos del panel (con el foco en el).
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) && !ImGui::GetIO().WantTextInput) {
        const ImGuiIO& io = ImGui::GetIO();
        const bool root_selected = isSelected(root.uuid());
        if (!root_selected && ImGui::IsKeyPressed(ImGuiKey_Delete)) deleteSelection();
        if (!root_selected && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D, false)) duplicateSelection();
        if (ImGui::IsKeyPressed(ImGuiKey_F, false)) focusSelection();
    }
}

void EditorApp::drawBlueprintDetails(BlueprintEditorState& bp) {
    using Details = BlueprintEditorState::Details;
    // Lo ultimo que se eligio manda: un nodo o variable del grafo, o un objeto.
    if (bp.graph.details_node != bp.last_details_node || bp.graph.details_variable != bp.last_details_variable) {
        if (bp.graph.details_node != 0 || !bp.graph.details_variable.empty()) bp.details = Details::Graph;
        bp.last_details_node = bp.graph.details_node;
        bp.last_details_variable = bp.graph.details_variable;
    }
    if (active_ != bp.last_active) {
        bp.last_active = active_;
        if (active_.valid()) bp.details = Details::Entity;
    }
    static const char* const kLabels[] = {"Objeto", "Grafo", "Clase"};
    static const char* const kTips[] = {"El objeto elegido en Componentes o en la Ventana grafica",
                                        "El nodo o la variable elegidos en el Event Graph", "Los datos del Blueprint"};
    const int picked = bl::segmented("##bp_details", kLabels, nullptr, 3, static_cast<int>(bp.details), kTips);
    if (picked >= 0) bp.details = static_cast<Details>(picked);
    ImGui::Separator();
    ImGui::BeginChild("##bp_details_body");
    switch (bp.details) {
        case Details::Entity: drawInspectorContents(); break;
        case Details::Graph: {
            const vsedit::Host host = blueprintHost();
            vsedit::drawDetails(bp.graph, host);
            if (bp.graph.details_node == 0 && bp.graph.details_variable.empty()) {
                ImGui::TextDisabled("Elige un nodo del Event Graph o una variable de Mi Blueprint.");
            }
            break;
        }
        case Details::Class: {
            ImGui::SeparatorText("Clase");
            ImGui::TextDisabled("Clase padre");
            ImGui::SameLine(120.0f);
            ImGui::TextUnformatted(bp.info.parent.c_str());
            ImGui::TextDisabled("Event Graph");
            ImGui::SameLine(120.0f);
            ImGui::TextUnformatted(bp.info.graph_path.c_str());
            if (bp.info.source.valid() && database_) {
                const auto source = database_->find(bp.info.source);
                ImGui::TextDisabled("Creado desde");
                ImGui::SameLine(120.0f);
                ImGui::TextUnformatted(source ? source->name.c_str() : "(ya no existe)");
            }
            ImGui::Spacing();
            ImGui::TextDisabled("Descripción");
            if (ImGui::InputTextMultiline("##bp_description", &bp.info.description, ImVec2(-1.0f, 70.0f))) bp.info_dirty = true;
            ImGui::TextDisabled("Categoría");
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::InputText("##bp_category", &bp.info.category)) bp.info_dirty = true;
            ImGui::SeparatorText("Variables (valores por defecto)");
            if (bp.graph.graph.variables.empty()) {
                ImGui::TextDisabled("Sin variables: se crean en Mi Blueprint.");
            }
            for (const vscript::Variable& v : bp.graph.graph.variables) {
                ImGui::PushID(v.name.c_str());
                if (ImGui::Selectable(v.name.c_str(), false, ImGuiSelectableFlags_AllowOverlap)) {
                    bp.graph.details_variable = v.name;
                    bp.graph.details_node = 0;
                }
                ImGui::SameLine(140.0f);
                ImGui::TextDisabled("%s", vscript::pinTypeLabel(v.type));
                ImGui::SameLine(240.0f);
                ImGui::TextUnformatted(v.value.empty() ? "-" : v.value.c_str());
                if (v.exposed) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("(Inspector)");
                }
                ImGui::PopID();
            }
            break;
        }
    }
    ImGui::EndChild();
}

// -----------------------------------------------------------------------------
// Inspector: variables expuestas del Visual Script de un objeto
// -----------------------------------------------------------------------------

void EditorApp::drawVisualScriptVariables(ecs::Entity entity) {
    vscript::VisualScript* vs = entity.tryGet<vscript::VisualScript>();
    if (vs == nullptr || vs->graph.empty() || !has_project_) return;
    // El grafo se lee una vez (y otra si cambia el archivo).
    struct Cached {
        std::string graph;
        std::filesystem::file_time_type time{};
        std::vector<scripting::ScriptProperty> props;
    };
    static Cached cache;
    const std::filesystem::path file = project_.assetsFolder() / dialogs::fromUtf8(vs->graph);
    std::error_code ec;
    const std::filesystem::file_time_type time = std::filesystem::last_write_time(file, ec);
    if (ec) return;
    if (cache.graph != vs->graph || cache.time != time) {
        cache.graph = vs->graph;
        cache.time = time;
        cache.props.clear();
        vscript::Graph graph;
        if (vscript::loadGraph(file, graph)) cache.props = vscript::exposedProperties(graph);
    }
    if (cache.props.empty()) return;
    ImGui::SeparatorText("Variables del grafo");
    for (const scripting::ScriptProperty& def : cache.props) {
        ImGui::PushID(def.name.c_str());
        auto own = std::find_if(vs->properties.begin(), vs->properties.end(),
                                [&](const scripting::ScriptProperty& p) { return p.name == def.name; });
        const bool overridden = own != vs->properties.end();
        std::string value = overridden ? own->value : def.value;
        bool changed = false;
        const float reset_w = overridden ? ImGui::CalcTextSize("Por defecto").x + ImGui::GetStyle().FramePadding.x * 2.0f + 6.0f : 0.0f;
        ImGui::SetNextItemWidth(std::max(ImGui::GetContentRegionAvail().x * 0.6f - reset_w, 60.0f));
        switch (def.type) {
            case scripting::PropertyType::Bool: {
                bool b = value == "true" || value == "1";
                if (ImGui::Checkbox(def.name.c_str(), &b)) {
                    value = b ? "true" : "false";
                    changed = true;
                }
                break;
            }
            case scripting::PropertyType::Text:
                changed = ImGui::InputText(def.name.c_str(), &value);
                break;
            case scripting::PropertyType::Vector: {
                float v[3] = {0.0f, 0.0f, 0.0f};
                std::istringstream in(value);
                in >> v[0] >> v[1] >> v[2];
                if (ImGui::DragFloat3(def.name.c_str(), v, 0.05f)) {
                    char text[96];
                    std::snprintf(text, sizeof(text), "%g %g %g", v[0], v[1], v[2]);
                    value = text;
                    changed = true;
                }
                break;
            }
            case scripting::PropertyType::Number:
            default: {
                float f = static_cast<float>(std::atof(value.c_str()));
                if (ImGui::DragFloat(def.name.c_str(), &f, 0.1f)) {
                    char text[48];
                    std::snprintf(text, sizeof(text), "%g", f);
                    value = text;
                    changed = true;
                }
                break;
            }
        }
        const bool finished = ImGui::IsItemDeactivatedAfterEdit();
        if (overridden) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Por defecto")) {
                vs->properties.erase(own);
                dirty_ = true;
                commit();
                ImGui::PopID();
                break;  // la lista cambio: el resto, el frame que viene
            }
            ImGui::SetItemTooltip("Volver al valor del grafo (%s)", def.value.c_str());
        }
        if (changed) {
            if (overridden) {
                own->value = value;
                own->type = def.type;
            } else {
                vs->properties.push_back(scripting::ScriptProperty{def.name, def.type, value});
            }
            dirty_ = true;
        }
        if (finished || (changed && def.type == scripting::PropertyType::Bool)) commit();
        ImGui::PopID();
    }
}

}  // namespace cramion::editor
