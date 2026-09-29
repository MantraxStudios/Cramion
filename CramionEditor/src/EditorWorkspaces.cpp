// Espacios de trabajo (pestanas debajo del menu), como los editores de
// assets de Unreal:
//
//   Escena   todo lo de siempre (el diseno guardado del editor)
//   Prefab   un escenario solo con el prefab: Jerarquia a la izquierda,
//            vista en el centro, componentes a la derecha, Proyecto y
//            Consola abajo. Guardar escribe el .crprefab y todas sus
//            instancias de la escena se ponen al dia (respetando sus
//            cambios propios).
//   Script   el editor de un .lua / .crshader a toda la ventana
//
// Solo hay un ecs::World (y un renderizador): el mundo de la pestana que no
// se ve se guarda en JSON con su deshacer, su seleccion y su camara, y se
// vuelve a cargar al volver. Asi todos los paneles (Jerarquia, Inspector,
// gizmos, deshacer) funcionan igual dentro del prefab.

#include "EditorApp.h"

#include "Dialogs.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <iostream>

namespace cramion::editor {

using core::Vec3;

namespace {

constexpr ImU32 kPrefabTab = IM_COL32(60, 110, 185, 255);
constexpr ImU32 kPrefabTabActive = IM_COL32(75, 140, 230, 255);
constexpr ImU32 kScriptTab = IM_COL32(70, 120, 80, 255);
constexpr ImU32 kScriptTabActive = IM_COL32(90, 160, 100, 255);

const char* workspaceSuffix(int kind) { return kind == 1 ? "@prefab" : "@script"; }

std::string lowerText(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

bool isCodeFile(const std::string& ext) { return ext == ".lua" || ext == ".crshader"; }

// Icono y color de un archivo del arbol por su extension.
Icon fileIcon(const std::string& ext, ImU32& tint) {
    tint = IM_COL32(200, 200, 210, 255);
    if (isCodeFile(ext)) {
        tint = IM_COL32(120, 210, 130, 255);
        return Icon::AssetBrowser;
    }
    if (ext == ".crprefab") {
        tint = IM_COL32(95, 170, 255, 255);
        return Icon::ColliderBox;
    }
    if (ext == ".crscene") {
        tint = IM_COL32(235, 200, 110, 255);
        return Icon::AssetBrowser;
    }
    if (ext == ".crdata" || ext == ".crmat") return Icon::MeshRenderer;
    if (ext == ".cranimator" || ext == ".cranim") return Icon::SkinnedMesh;
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp") return Icon::Decal;
    if (ext == ".wav" || ext == ".ogg" || ext == ".mp3" || ext == ".flac") return Icon::AudioSource;
    if (ext == ".crterrain") return Icon::Terrain;
    return Icon::AssetBrowser;
}

// Algo de la rama contiene el texto buscado.
template <typename Node>
bool subtreeMatches(const Node& node, const std::string& filter) {
    if (filter.empty() || lowerText(node.name).find(filter) != std::string::npos) return true;
    return std::any_of(node.children.begin(), node.children.end(), [&](const Node& c) { return subtreeMatches(c, filter); });
}

}  // namespace

EditorApp::Workspace* EditorApp::findWorkspace(int id) {
    for (Workspace& ws : workspaces_) {
        if (ws.id == id) return &ws;
    }
    return nullptr;
}

const EditorApp::Workspace* EditorApp::findWorkspace(int id) const {
    for (const Workspace& ws : workspaces_) {
        if (ws.id == id) return &ws;
    }
    return nullptr;
}

void EditorApp::resetWorkspaces() {
    workspaces_.clear();
    Workspace scene;
    scene.id = 0;
    scene.kind = WorkspaceKind::Scene;
    scene.name = "Escena";
    workspaces_.push_back(std::move(scene));
    active_workspace_ = 0;
    world_workspace_ = 0;
    pending_workspace_ = -1;
    pending_play_ = false;
    workspace_close_ask_ = -1;
    workspace_focus_frames_ = 0;
}

EditorApp::WorkspaceKind EditorApp::activeWorkspaceKind() const {
    const Workspace* ws = findWorkspace(active_workspace_);
    return ws != nullptr ? ws->kind : WorkspaceKind::Scene;
}

std::string EditorApp::panelTitle(const char* name) const {
    const WorkspaceKind kind = activeWorkspaceKind();
    if (kind == WorkspaceKind::Scene) return name;
    return std::string(name) + "###" + name + workspaceSuffix(kind == WorkspaceKind::Prefab ? 1 : 2);
}

ecs::Entity EditorApp::prefabStageRoot() {
    const Workspace* ws = findWorkspace(world_workspace_);
    if (ws == nullptr || ws->kind != WorkspaceKind::Prefab) return {};
    return world_.find(ws->root);
}

bool EditorApp::isStageHelper(const Uuid& uuid) const {
    const Workspace* ws = findWorkspace(world_workspace_);
    if (ws == nullptr || ws->kind != WorkspaceKind::Prefab) return false;
    return std::any_of(ws->helpers.begin(), ws->helpers.end(), [&](const Uuid& h) { return h == uuid; });
}

std::filesystem::path EditorApp::activeScriptWorkspace() const {
    const Workspace* ws = findWorkspace(active_workspace_);
    return ws != nullptr && ws->kind == WorkspaceKind::Script ? ws->path : std::filesystem::path{};
}

// -----------------------------------------------------------------------------
// Abrir
// -----------------------------------------------------------------------------

void EditorApp::openPrefabWorkspace(const Uuid& prefab) {
    if (!has_project_) return;
    if (workspaces_.empty()) resetWorkspaces();
    const std::filesystem::path path = prefabPath(prefab);
    if (path.empty()) {
        std::cerr << "[Prefab] El asset del prefab no existe\n";
        return;
    }
    if (playing()) {
        std::cerr << "[Prefab] Sal del modo Play para editar el prefab\n";
        return;
    }
    for (const Workspace& ws : workspaces_) {
        if (ws.kind == WorkspaceKind::Prefab && ws.prefab == prefab) {
            requestWorkspace(ws.id);
            return;
        }
    }
    Workspace ws;
    ws.id = next_workspace_id_++;
    ws.kind = WorkspaceKind::Prefab;
    ws.name = dialogs::utf8(path.stem());
    ws.prefab = prefab;
    ws.path = path;
    workspaces_.push_back(std::move(ws));
    requestWorkspace(workspaces_.back().id);
}

void EditorApp::openScriptWorkspace(const std::filesystem::path& file) {
    if (workspaces_.empty()) resetWorkspaces();
    syncScriptWorkspaces();
    for (const Workspace& ws : workspaces_) {
        if (ws.kind == WorkspaceKind::Script && ws.path == file) {
            activateWorkspace(ws.id);  // sin cambiar de mundo: puede ser ya
            return;
        }
    }
}

// Una pestana por cada script abierto (y ninguna de los que se cerraron).
void EditorApp::syncScriptWorkspaces() {
    for (const ScriptTab& tab : script_tabs_) {
        const bool known = std::any_of(workspaces_.begin(), workspaces_.end(), [&](const Workspace& ws) {
            return ws.kind == WorkspaceKind::Script && ws.path == tab.path;
        });
        if (known) continue;
        Workspace ws;
        ws.id = next_workspace_id_++;
        ws.kind = WorkspaceKind::Script;
        ws.name = dialogs::utf8(tab.path.filename());
        ws.path = tab.path;
        workspaces_.push_back(std::move(ws));
    }
    bool lost_active = false;
    workspaces_.erase(std::remove_if(workspaces_.begin(), workspaces_.end(),
                                     [&](const Workspace& ws) {
                                         if (ws.kind != WorkspaceKind::Script) return false;
                                         const bool open = std::any_of(script_tabs_.begin(), script_tabs_.end(),
                                                                       [&](const ScriptTab& t) { return t.path == ws.path; });
                                         if (!open && ws.id == active_workspace_) lost_active = true;
                                         return !open;
                                     }),
                      workspaces_.end());
    if (lost_active) {
        active_workspace_ = -1;
        activateWorkspace(world_workspace_ >= 0 ? world_workspace_ : 0);
    }
}

// -----------------------------------------------------------------------------
// Cambiar de pestana
// -----------------------------------------------------------------------------

void EditorApp::requestWorkspace(int id) { pending_workspace_ = id; }

void EditorApp::applyPendingWorkspace() {
    if (workspaces_.empty()) resetWorkspaces();
    if (pending_workspace_ >= 0) {
        const int id = pending_workspace_;
        pending_workspace_ = -1;
        activateWorkspace(id);
    }
    if (pending_play_) {
        pending_play_ = false;
        if (active_workspace_ == 0 && world_workspace_ == 0) enterPlay();
    }
}

void EditorApp::activateWorkspace(int id) {
    Workspace* ws = findWorkspace(id);
    if (ws == nullptr || id == active_workspace_) return;
    if (ws->kind == WorkspaceKind::Prefab && playing()) {
        std::cerr << "[Prefab] Sal del modo Play para editar el prefab\n";
        return;
    }
    const WorkspaceKind kind = ws->kind;
    const std::filesystem::path script = ws->path;
    if (kind != WorkspaceKind::Script) loadWorkspaceWorld(id);
    active_workspace_ = id;
    flying_ = false;
    if (kind == WorkspaceKind::Script) {
        for (std::size_t i = 0; i < script_tabs_.size(); ++i) {
            if (script_tabs_[i].path != script) continue;
            active_script_tab_ = static_cast<int>(i);
            script_tabs_[i].select = true;
        }
    }
    updateTitle();
}

void EditorApp::returnToSceneWorkspace() {
    if (workspaces_.empty()) resetWorkspaces();
    pending_workspace_ = -1;
    loadWorkspaceWorld(0);
    activateWorkspace(0);
}

// Pone en world_ el mundo del espacio `id` (Escena o Prefab); el que habia se
// guarda en su pestana.
void EditorApp::loadWorkspaceWorld(int id) {
    if (id == world_workspace_) return;
    Workspace* target = findWorkspace(id);
    if (target == nullptr || target->kind == WorkspaceKind::Script) return;
    flushCommit();

    // El cielo de lo que se ve ahora: el escenario del prefab lo copia (el
    // prefab se ve con la misma luz que en la escena).
    std::optional<ecs::Sky> sky;
    for (const entt::entity h : world_.registry().view<ecs::Sky>()) {
        if (!isStageHelper(world_.wrap(h).uuid())) {
            sky = world_.wrap(h).get<ecs::Sky>();
            break;
        }
    }
    if (!sky) {
        for (const entt::entity h : world_.registry().view<ecs::Sky>()) {
            sky = world_.wrap(h).get<ecs::Sky>();
            break;
        }
    }

    if (Workspace* owner = findWorkspace(world_workspace_)) {
        owner->world = ecs::serializeWorld(world_);
        owner->stashed = true;
        if (owner->kind == WorkspaceKind::Scene) owner->name = world_.sceneName();
        owner->scene_path = scene_path_;
        owner->dirty = dirty_;
        owner->undo = std::move(undo_);
        owner->redo = std::move(redo_);
        owner->current_state = std::move(current_state_);
        owner->undo_kinds = std::move(undo_kinds_);
        owner->redo_kinds = std::move(redo_kinds_);
        owner->terrain_undo = std::move(terrain_undo_);
        owner->terrain_redo = std::move(terrain_redo_);
        owner->selection = selection_;
        owner->active = active_;
        owner->camera = scene_.camera();
    }
    undo_.clear();
    redo_.clear();
    undo_kinds_.clear();
    redo_kinds_.clear();
    terrain_undo_.clear();
    terrain_redo_.clear();
    commit_pending_ = false;
    collider_wire_cache_.clear();
    hierarchy_rows_.clear();
    renaming_ = {};
    reveal_ = {};
    flying_ = false;
    inspected_material_ = {};
    inspected_render_texture_ = {};

    const ecs::DVec3 origin_before = world_.origin();
    if (target->stashed) {
        ecs::deserializeWorld(world_, target->world);
        alignOriginAfterLoad(origin_before);
        target->world.clear();
        target->world.shrink_to_fit();
        target->stashed = false;
        scene_path_ = target->scene_path;
        dirty_ = target->dirty;
        undo_ = std::move(target->undo);
        redo_ = std::move(target->redo);
        current_state_ = std::move(target->current_state);
        undo_kinds_ = std::move(target->undo_kinds);
        redo_kinds_ = std::move(target->redo_kinds);
        terrain_undo_ = std::move(target->terrain_undo);
        terrain_redo_ = std::move(target->terrain_redo);
        selection_ = target->selection;
        active_ = target->active;
        if (target->camera) scene_.camera() = *target->camera;
        world_workspace_ = id;
        // Prefabs guardados en otra pestana: sus instancias se ponen al dia
        // (un paso de deshacer, como Aplicar).
        syncPrefabInstances();
    } else {
        // Prefab abierto por primera vez: su escenario.
        world_.clear();
        alignOriginAfterLoad(origin_before);
        world_.setSceneUuid(Uuid::generate());
        world_.setSceneName("Prefab " + target->name);
        world_workspace_ = id;
        ecs::Entity light = ecs::createLight(world_, ecs::LightType::Directional);
        light.setName("Luz del escenario");
        ecs::Entity environment = world_.create("Cielo del escenario");
        environment.add<ecs::Sky>() = sky.value_or(ecs::Sky{});
        environment.add<ecs::PostProcessing>();
        target->helpers = {light.uuid(), environment.uuid()};
        const std::string text = prefabText(target->prefab);
        ecs::Entity root = text.empty() ? ecs::Entity{} : ecs::instantiatePrefab(world_, text);
        if (!root.valid()) {
            std::cerr << "[Prefab] No se pudo leer " << target->name << " (vacio o danado)\n";
            root = world_.create(target->name);
        }
        root.setLocalPosition(Vec3{0.0f, 0.0f, 0.0f});
        target->root = root.uuid();
        target->built = true;
        scene_path_.clear();
        clearSelection();
        selectOnly(root.uuid());
        revealInHierarchy(root.uuid());
        resetUndo();
        dirty_ = false;
        scene_.placeCamera(Vec3{4.0f, 3.0f, 6.0f}, Vec3{0.0f, 1.0f, 0.0f});
        workspace_focus_frames_ = 3;  // cuando ya tenga actores (su tamano real)
        std::cout << "[Prefab] Editando " << target->name << " (Ctrl+S guarda y actualiza sus instancias)\n";
    }
    renderer_.invalidateHistory();
    updateTitle();
}

// -----------------------------------------------------------------------------
// Guardar y cerrar
// -----------------------------------------------------------------------------

bool EditorApp::savePrefabWorkspace() {
    Workspace* ws = findWorkspace(world_workspace_);
    if (ws == nullptr || ws->kind != WorkspaceKind::Prefab) return false;
    if (playing()) {
        std::cerr << "[Prefab] Sal del modo Play para guardar el prefab\n";
        return false;
    }
    flushCommit();
    ecs::Entity root = world_.find(ws->root);
    if (!root.valid()) {
        std::cerr << "[Prefab] Se borro la raiz del prefab: Ctrl+Z para recuperarla antes de guardar\n";
        return false;
    }
    // Lo que quedo suelto (fuera de la raiz) tambien es del prefab.
    const std::vector<entt::entity> roots = world_.roots();
    int moved = 0;
    for (const entt::entity h : roots) {
        ecs::Entity e = world_.wrap(h);
        if (e == root || isStageHelper(e.uuid())) continue;
        e.setParent(root, true);
        ++moved;
    }
    if (moved > 0) std::cout << "[Prefab] " << moved << " objeto(s) suelto(s) movido(s) dentro de la raiz\n";
    const std::filesystem::path path = prefabPath(ws->prefab);
    if (path.empty()) {
        std::cerr << "[Prefab] El asset del prefab ya no existe\n";
        return false;
    }
    std::string error;
    const std::string text = ecs::applyInstance(world_, root, path, &error);
    if (text.empty()) {
        std::cerr << "[Prefab] No se pudo guardar " << dialogs::utf8(path.filename()) << ": " << error << "\n";
        return false;
    }
    prefab_texts_[ws->prefab.toString()] = text;
    // Los enlaces nuevos no son un cambio del usuario.
    current_state_ = ecs::serializeWorld(world_);
    dirty_ = false;
    std::cout << "[Prefab] " << dialogs::utf8(path.filename()) << " guardado (revision " << ecs::prefabRevision(text)
              << "): sus instancias de la escena se actualizan\n";
    updateTitle();
    return true;
}

void EditorApp::closeWorkspace(int id, bool save) {
    Workspace* ws = findWorkspace(id);
    if (ws == nullptr || ws->kind == WorkspaceKind::Scene) return;
    if (ws->kind == WorkspaceKind::Script) {
        const std::filesystem::path path = ws->path;
        for (std::size_t i = 0; i < script_tabs_.size(); ++i) {
            if (script_tabs_[i].path != path) continue;
            // Como antes: cerrar con cambios los guarda (sin perder nada).
            if (script_tabs_[i].text != script_tabs_[i].saved) saveScript(script_tabs_[i]);
            script_tabs_.erase(script_tabs_.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }
        active_script_tab_ = std::min(active_script_tab_, static_cast<int>(script_tabs_.size()) - 1);
        if (script_tabs_.empty()) show_script_editor_ = false;
        syncScriptWorkspaces();
        return;
    }
    if (save) {
        if (playing()) {
            std::cerr << "[Prefab] Sal del modo Play para guardar el prefab\n";
            return;
        }
        loadWorkspaceWorld(id);
        if (!savePrefabWorkspace()) return;  // no se cierra si no se pudo guardar
    }
    const bool was_active = active_workspace_ == id;
    if (world_workspace_ == id) {
        world_workspace_ = -1;  // su mundo se tira
        loadWorkspaceWorld(0);
    }
    workspaces_.erase(std::remove_if(workspaces_.begin(), workspaces_.end(), [&](const Workspace& w) { return w.id == id; }),
                      workspaces_.end());
    if (was_active || active_workspace_ == id) {
        active_workspace_ = -1;
        activateWorkspace(0);
    }
}

// -----------------------------------------------------------------------------
// Interfaz
// -----------------------------------------------------------------------------

void EditorApp::drawWorkspaceBar() {
    if (workspaces_.empty()) resetWorkspaces();
    syncScriptWorkspaces();
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float height = ImGui::GetFrameHeight() + 6.0f;
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f, 4.0f));
    int clicked = -1;
    int close = -1;
    const bool open_bar = ImGui::BeginViewportSideBar("##workspaces", viewport, ImGuiDir_Up, height, flags);
    ImGui::PopStyleVar();
    if (open_bar && ImGui::BeginTabBar("##workspace_tabs", ImGuiTabBarFlags_FittingPolicyScroll)) {
        for (const Workspace& ws : workspaces_) {
            const bool loaded = ws.id == world_workspace_;
            bool dirty = false;
            std::string name = ws.name;
            const char* prefix = "Escena: ";
            if (ws.kind == WorkspaceKind::Scene) {
                dirty = loaded ? dirty_ : ws.dirty;
                if (loaded) name = world_.sceneName();
            } else if (ws.kind == WorkspaceKind::Prefab) {
                dirty = loaded ? dirty_ : ws.dirty;
                prefix = "Prefab: ";
            } else {
                prefix = "Script: ";
                for (const ScriptTab& tab : script_tabs_) {
                    if (tab.path == ws.path) dirty = tab.text != tab.saved;
                }
            }
            const std::string label = prefix + name + "###workspace" + std::to_string(ws.id);
            ImGuiTabItemFlags tab_flags = dirty ? ImGuiTabItemFlags_UnsavedDocument : 0;
            // La pestana que se ve es siempre la activa: solo cambia con un clic
            // (si ImGui elige otra sola, p. ej. al cerrar una, no cuenta).
            if (ws.id == active_workspace_) tab_flags |= ImGuiTabItemFlags_SetSelected;
            int colors = 0;
            if (ws.kind != WorkspaceKind::Scene) {
                const bool prefab = ws.kind == WorkspaceKind::Prefab;
                ImGui::PushStyleColor(ImGuiCol_Tab, prefab ? kPrefabTab : kScriptTab);
                ImGui::PushStyleColor(ImGuiCol_TabDimmed, prefab ? kPrefabTab : kScriptTab);
                ImGui::PushStyleColor(ImGuiCol_TabHovered, prefab ? kPrefabTabActive : kScriptTabActive);
                ImGui::PushStyleColor(ImGuiCol_TabSelected, prefab ? kPrefabTabActive : kScriptTabActive);
                ImGui::PushStyleColor(ImGuiCol_TabDimmedSelected, prefab ? kPrefabTabActive : kScriptTabActive);
                colors = 5;
            }
            bool open = true;
            const bool selected = ImGui::BeginTabItem(label.c_str(), ws.kind == WorkspaceKind::Scene ? nullptr : &open, tab_flags);
            ImGui::PopStyleColor(colors);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                if (ws.kind == WorkspaceKind::Scene) {
                    ImGui::SetTooltip("La escena abierta (todo el editor)");
                } else {
                    ImGui::SetTooltip("%s", dialogs::utf8(ws.path).c_str());
                }
            }
            const bool pressed = ImGui::IsItemClicked(ImGuiMouseButton_Left);
            if (selected) ImGui::EndTabItem();
            if (!open) {
                close = ws.id;
            } else if (pressed && ws.id != active_workspace_) {
                clicked = ws.id;
            }
        }
        ImGui::EndTabBar();
    }
    ImGui::End();

    if (clicked >= 0) requestWorkspace(clicked);
    if (close >= 0) {
        const Workspace* ws = findWorkspace(close);
        const bool dirty = ws != nullptr && ws->kind == WorkspaceKind::Prefab &&
                           (close == world_workspace_ ? dirty_ : ws->dirty);
        if (dirty) {
            workspace_close_ask_ = close;
            ImGui::OpenPopup("Guardar el prefab##workspace_close");
        } else {
            closeWorkspace(close, false);
        }
    }
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Guardar el prefab##workspace_close", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const Workspace* ws = findWorkspace(workspace_close_ask_);
        if (ws == nullptr) {
            ImGui::CloseCurrentPopup();
        } else {
            ImGui::Text("El prefab \"%s\" tiene cambios sin guardar.", ws->name.c_str());
            ImGui::TextDisabled("Guardar actualiza todas sus instancias de la escena.");
            ImGui::Spacing();
            const int id = workspace_close_ask_;
            if (ImGui::Button("Guardar", ImVec2(110.0f, 0.0f))) {
                ImGui::CloseCurrentPopup();
                workspace_close_ask_ = -1;
                closeWorkspace(id, true);
            }
            ImGui::SameLine();
            if (ImGui::Button("Descartar", ImVec2(110.0f, 0.0f))) {
                ImGui::CloseCurrentPopup();
                workspace_close_ask_ = -1;
                closeWorkspace(id, false);
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancelar", ImVec2(110.0f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                ImGui::CloseCurrentPopup();
                workspace_close_ask_ = -1;
            }
        }
        ImGui::EndPopup();
    }
}

// Unreal: el arbol a la izquierda, la vista en el centro y los detalles a la
// derecha; abajo el Proyecto (para soltar modelos y prefabs) y la Consola.
void EditorApp::buildPrefabLayout(unsigned int dockspace_id) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::DockBuilderRemoveNode(dockspace_id);
    ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace_id, viewport->WorkSize);
    ImGuiID center = dockspace_id;
    const ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.26f, nullptr, &center);
    const ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.24f, nullptr, &center);
    const ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.28f, nullptr, &center);
    ImGui::DockBuilderDockWindow(panelTitle("Jerarquía").c_str(), left);
    ImGui::DockBuilderDockWindow(panelTitle("Inspector").c_str(), right);
    ImGui::DockBuilderDockWindow(panelTitle("Proyecto").c_str(), bottom);
    ImGui::DockBuilderDockWindow(panelTitle("Consola").c_str(), bottom);
    ImGui::DockBuilderDockWindow(panelTitle("Escena").c_str(), center);
    ImGui::DockBuilderFinish(dockspace_id);
}

void EditorApp::drawWorkspacePanels(float delta_seconds) {
    (void)delta_seconds;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    game_view_visible_ = false;
    if (activeWorkspaceKind() == WorkspaceKind::Prefab) {
        const ImGuiID dock = ImHashStr("CramionPrefabDockspace");
        if (ImGui::DockBuilderGetNode(dock) == nullptr) buildPrefabLayout(dock);
        ImGui::DockSpaceOverViewport(dock, viewport);
        if (workspace_focus_frames_ > 0 && --workspace_focus_frames_ == 0) {
            if (const ecs::Entity root = prefabStageRoot(); root.valid()) {
                selectOnly(root.uuid());
                focusSelection();
            }
        }
        drawSceneView();
        if (show_hierarchy_) drawHierarchy();
        if (show_inspector_) drawInspector();
        if (show_project_) drawProject();
        if (show_console_) drawConsole();
    } else {
        const ImGuiID dock = ImHashStr("CramionScriptDockspace");
        const ImGuiDockNode* node = ImGui::DockBuilderGetNode(dock);
        if (node == nullptr || node->IsLeafNode()) buildScriptLayout(dock);
        ImGui::DockSpaceOverViewport(dock, viewport, ImGuiDockNodeFlags_AutoHideTabBar);
        const ImGuiDockNode* central = ImGui::DockBuilderGetCentralNode(dock);
        script_workspace_dock_ = central != nullptr ? central->ID : dock;
        scene_view_visible_ = false;
        flying_ = false;
        drawScriptEditor();
        drawScriptFileTree();
    }
    drawMcpWindow();
    drawBuildConfigsWindow();
    drawTouchInterfaceWindow();
    drawInputActionsWindow();
    drawExportProgress();
    drawImportProgress();
    drawModals();
}

// Arriba de la Jerarquia y en el Inspector de la raiz.
void EditorApp::drawPrefabStageBanner() {
    const Workspace* ws = findWorkspace(world_workspace_);
    if (ws == nullptr || ws->kind != WorkspaceKind::Prefab) return;
    ImGui::PushID("prefab_stage");
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->ChannelsSplit(2);
    draw->ChannelsSetCurrent(1);
    ImGui::Dummy(ImVec2(0.0f, 2.0f));
    ImGui::Indent(6.0f);
    ImGui::TextColored(ImVec4(0.45f, 0.72f, 1.0f, 1.0f), "Editando prefab");
    ImGui::SameLine();
    ImGui::TextUnformatted(ws->name.c_str());
    ImGui::BeginDisabled(playing());
    if (ImGui::SmallButton(dirty_ ? "Guardar *" : "Guardar")) savePrefabWorkspace();
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Ctrl+S: escribe el .crprefab y actualiza todas sus instancias");
    ImGui::SameLine();
    if (ImGui::SmallButton("Volver a la escena")) requestWorkspace(0);
    ImGui::Unindent(6.0f);
    ImGui::Dummy(ImVec2(0.0f, 2.0f));
    draw->ChannelsSetCurrent(0);
    const ImVec2 end(start.x + width, ImGui::GetCursorScreenPos().y);
    draw->AddRectFilled(start, end, IM_COL32(75, 140, 230, 40), 4.0f);
    draw->AddRect(start, end, IM_COL32(75, 140, 230, 140), 4.0f);
    draw->ChannelsMerge();
    ImGui::PopID();
}

// -----------------------------------------------------------------------------
// Pestana de script: arbol de carpetas y assets a la derecha
// -----------------------------------------------------------------------------

void EditorApp::buildScriptLayout(unsigned int dockspace_id) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::DockBuilderRemoveNode(dockspace_id);
    ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace_id, viewport->WorkSize);
    ImGuiID center = dockspace_id;
    const ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.22f, nullptr, &center);
    ImGui::DockBuilderDockWindow(panelTitle("Archivos").c_str(), right);
    ImGui::DockBuilderFinish(dockspace_id);
}

void EditorApp::rebuildFileTree(FileTreeNode& node, int depth) {
    node.children.clear();
    if (depth > 24) return;
    std::error_code e;
    for (std::filesystem::directory_iterator it(node.path, std::filesystem::directory_options::skip_permission_denied, e);
         !e && it != std::filesystem::directory_iterator(); it.increment(e)) {
        FileTreeNode child;
        child.path = it->path();
        child.name = dialogs::utf8(child.path.filename());
        if (child.name.empty() || child.name[0] == '.' || lowerText(dialogs::utf8(child.path.extension())) == ".meta") continue;
        std::error_code fe;
        child.folder = it->is_directory(fe);
        if (child.folder) rebuildFileTree(child, depth + 1);
        node.children.push_back(std::move(child));
    }
    // Carpetas primero, luego por nombre (como el Explorador).
    std::sort(node.children.begin(), node.children.end(), [](const FileTreeNode& a, const FileTreeNode& b) {
        if (a.folder != b.folder) return a.folder;
        return lowerText(a.name) < lowerText(b.name);
    });
}

// Devuelve true si se pidio cambiar algo que invalida el arbol (no seguir dibujando).
bool EditorApp::drawFileTreeNode(const FileTreeNode& node, const std::filesystem::path& active, const std::string& filter) {
    if (!subtreeMatches(node, filter)) return false;
    const std::string ext = lowerText(dialogs::utf8(node.path.extension()));
    const float icon_size = ImGui::GetTextLineHeight();
    ImGui::PushID(node.name.c_str());
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth;
    Icon icon = Icon::FolderClosed;
    ImU32 tint = IM_COL32(230, 190, 90, 255);
    if (node.folder) {
        flags |= ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick;
        // Abiertas: buscando, o si dentro esta el script que se edita.
        const std::string folder = dialogs::utf8(node.path);
        const std::string inside = dialogs::utf8(active);
        if (!filter.empty() || (!inside.empty() && inside.rfind(folder, 0) == 0)) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
    } else {
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
        if (node.path == active) flags |= ImGuiTreeNodeFlags_Selected;
        icon = fileIcon(ext, tint);
    }
    // Hueco delante del nombre para el icono.
    const std::string label = "      " + node.name;
    const bool open = ImGui::TreeNodeEx("##node", flags, "%s", label.c_str());
    if (node.folder && open) icon = Icon::FolderOpen;
    const ImVec2 min = ImGui::GetItemRectMin();
    imgui_.drawIcon(ImGui::GetWindowDrawList(), icon,
                    ImVec2(min.x + ImGui::GetTreeNodeToLabelSpacing(),
                           min.y + (ImGui::GetItemRectSize().y - icon_size) * 0.5f),
                    icon_size, tint);
    bool changed = false;
    const std::string relative = assetRelative(node.path);
    if (!node.folder) {
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", relative.c_str());
        // Un clic en un script lo abre; doble clic en un prefab o escena, tambien.
        if (isCodeFile(ext) && ImGui::IsItemClicked(ImGuiMouseButton_Left) && node.path != active) {
            openScript(node.path);
            changed = true;
        } else if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            if (ext == ".crprefab" && database_) {
                for (const assets::AssetInfo& info : database_->all()) {
                    if (info.path == node.path) openPrefabWorkspace(info.uuid);
                }
            } else if (ext == ".crscene") {
                runOrAskToSave(PendingAction::OpenScene, node.path);
                changed = true;
            } else if (!isCodeFile(ext)) {
                current_folder_ = node.path.parent_path();
                show_project_ = true;
                requestWorkspace(0);
            }
        }
        // Arrastrar la ruta (p. ej. a un campo del Inspector que acepte scripts).
        if (isCodeFile(ext) && ImGui::BeginDragDropSource()) {
            const std::string path = dialogs::utf8(node.path);
            ImGui::SetDragDropPayload(kScriptPayload, path.c_str(), path.size() + 1);
            ImGui::TextUnformatted(node.name.c_str());
            ImGui::EndDragDropSource();
        }
    }
    if (ImGui::BeginPopupContextItem("##menu")) {
        if (node.folder && ImGui::MenuItem("Nuevo script aqui")) {
            createScriptAsset(node.path, {});
            file_tree_time_ = -10.0;
            changed = true;
        }
        if (!node.folder && isCodeFile(ext) && ImGui::MenuItem("Abrir")) {
            openScript(node.path);
            changed = true;
        }
        if (ImGui::MenuItem("Copiar ruta (dentro de Assets)")) ImGui::SetClipboardText(relative.c_str());
        if (ImGui::MenuItem("Mostrar en el Proyecto")) {
            current_folder_ = node.folder ? node.path : node.path.parent_path();
            show_project_ = true;
            requestWorkspace(0);
        }
        ImGui::EndPopup();
    }
    if (node.folder && open) {
        for (const FileTreeNode& child : node.children) {
            if (changed) break;
            changed = drawFileTreeNode(child, active, filter);
        }
        ImGui::TreePop();
    }
    ImGui::PopID();
    return changed;
}

void EditorApp::drawScriptFileTree() {
    if (!ImGui::Begin(panelTitle("Archivos").c_str())) {
        ImGui::End();
        return;
    }
    // El arbol se rehace si cambia la base de datos o cada pocos segundos
    // (archivos creados fuera del editor).
    const double now = ImGui::GetTime();
    if (file_tree_version_ != database_version_ || now - file_tree_time_ > 3.0 || file_tree_.path != project_.assetsFolder()) {
        file_tree_version_ = database_version_;
        file_tree_time_ = now;
        file_tree_.path = project_.assetsFolder();
        file_tree_.name = "Assets";
        file_tree_.folder = true;
        rebuildFileTree(file_tree_, 0);
    }
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##filtro", "Buscar archivo...", &file_tree_filter_);
    ImGui::Separator();
    ImGui::BeginChild("##tree");
    const std::string filter = lowerText(file_tree_filter_);
    ImGui::SetNextItemOpen(true, ImGuiCond_Once);
    drawFileTreeNode(file_tree_, activeScriptWorkspace(), filter);
    ImGui::EndChild();
    ImGui::End();
}

}  // namespace cramion::editor
