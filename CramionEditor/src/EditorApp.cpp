#include "EditorApp.h"

#include "Dialogs.h"
#include "EditorLog.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include <ImGuizmo.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <iostream>

namespace cramion::editor {

using core::Vec3;

namespace {

// Tope de memoria de las instantaneas de deshacer.
constexpr std::size_t kUndoMemoryLimit = 256ull * 1024 * 1024;
constexpr std::size_t kUndoMaxSteps = 200;

std::wstring widen(const std::string& text) {
    return dialogs::fromUtf8(text).wstring();
}

}  // namespace

EditorApp::EditorApp(dm::Window& window, gfx::VulkanRenderer& renderer, scene::Scene& scene,
                     ImGuiLayer& imgui)
    : window_(window), renderer_(renderer), scene_(scene), imgui_(imgui) {
    const std::filesystem::path documents = dialogs::documentsFolder();
    new_project_folder_ = dialogs::utf8(documents.empty() ? std::filesystem::current_path()
                                                          : documents / "Cramion Projects");
    updateTitle();
}

EditorApp::~EditorApp() {
    // Las importaciones en curso terminan antes de destruir nada.
    for (ImportJob& job : imports_) {
        if (job.result.valid()) {
            job.result.wait();
        }
    }
}

// -----------------------------------------------------------------------------
// Proyecto
// -----------------------------------------------------------------------------

bool EditorApp::openProject(const std::filesystem::path& path) {
    const std::optional<project::ProjectInfo> info = project::openProject(path);
    if (!info) {
        hub_error_ = "No es un proyecto de Cramion valido: " + dialogs::utf8(path);
        std::cerr << "[Editor] " << hub_error_ << "\n";
        return false;
    }
    closeProject();

    project_ = *info;
    database_ = std::make_unique<assets::AssetDatabase>();
    database_->open(project_.assetsFolder());
    asset_manager_ = std::make_unique<assets::AssetManager>(*database_);
    asset_manager_->setCacheFolder(project_.libraryFolder() / "Cache");
    sync_ = std::make_unique<ecs::RenderSync>(*asset_manager_);
    sync_->reset(scene_);
    has_project_ = true;
    current_folder_ = project_.assetsFolder();
    project::addRecentProject(project_);
    std::cout << "[Editor] Proyecto abierto: " << project_.name << " ("
              << dialogs::utf8(project_.folder) << ")\n";

    // La escena inicial, o una nueva si no hay.
    bool opened = false;
    if (project_.startup_scene.valid()) {
        if (const auto scene_info = database_->find(project_.startup_scene)) {
            opened = openScene(scene_info->path);
        }
    }
    if (!opened) {
        newScene();
    }
    return true;
}

void EditorApp::closeProject() {
    if (!has_project_) {
        return;
    }
    if (animator_dirty_) saveAnimatorEditor();
    animator_uuid_ = {};
    animator_path_.clear();
    animator_ = {};
    show_animator_ = false;
    renderer_.waitIdle();
    renderer_.setOutlinedActors({});
    world_.clear();
    if (sync_) {
        sync_->reset(scene_);
    }
    sync_.reset();
    asset_manager_.reset();
    database_.reset();
    if (assets_watch_ != nullptr) {
        FindCloseChangeNotification(assets_watch_);
        assets_watch_ = nullptr;
    }
    refresh_at_ = -1.0;
    folder_nodes_.clear();
    current_subfolders_.clear();
    current_assets_.clear();
    ++database_version_;
    has_project_ = false;
    scene_path_.clear();
    clearSelection();
    resetUndo();
    dirty_ = false;
    updateTitle();
}

void EditorApp::newScene() {
    world_.clear();
    world_.setSceneUuid(Uuid::generate());
    world_.setSceneName("Nueva escena");
    ecs::populateDefaultScene(world_);
    scene_path_.clear();
    clearSelection();
    resetUndo();
    dirty_ = false;
    scene_.placeCamera(Vec3{6.0f, 3.0f, 8.0f}, Vec3{0.0f, 1.0f, 0.0f});
    updateTitle();
}

bool EditorApp::openScene(const std::filesystem::path& path) {
    std::string error;
    if (!ecs::loadScene(world_, path, &error)) {
        std::cerr << "[Editor] No se pudo abrir la escena " << dialogs::utf8(path) << ": " << error
                  << "\n";
        return false;
    }
    scene_path_ = path;
    clearSelection();
    resetUndo();
    dirty_ = false;
    std::cout << "[Editor] Escena abierta: " << dialogs::utf8(path.filename()) << "\n";
    updateTitle();
    return true;
}

bool EditorApp::saveScene() {
    if (scene_path_.empty()) {
        return saveSceneAs();
    }
    std::string error;
    if (!ecs::saveScene(world_, scene_path_, &error)) {
        std::cerr << "[Editor] No se pudo guardar: " << error << "\n";
        return false;
    }
    dirty_ = false;
    refreshDatabase();
    std::cout << "[Editor] Escena guardada: " << dialogs::utf8(scene_path_.filename()) << "\n";
    updateTitle();
    return true;
}

bool EditorApp::saveSceneAs() {
    const std::filesystem::path folder = project_.assetsFolder() / "Scenes";
    std::filesystem::create_directories(folder);
    const std::filesystem::path path =
        dialogs::saveFile(window_.handle(), L"Escena de Cramion (*.crscene)\0*.crscene\0",
                          L"crscene", folder, widen(world_.sceneName()));
    if (path.empty()) {
        return false;
    }
    world_.setSceneName(dialogs::utf8(path.stem()));
    scene_path_ = path;
    return saveScene();
}

void EditorApp::runOrAskToSave(PendingAction action, const std::filesystem::path& scene) {
    pending_ = action;
    pending_scene_ = scene;
    if (dirty_ && has_project_) {
        ask_save_ = true;
    } else {
        performPending();
    }
}

void EditorApp::performPending() {
    const PendingAction action = pending_;
    pending_ = PendingAction::None;
    switch (action) {
        case PendingAction::NewScene:
            newScene();
            break;
        case PendingAction::OpenScene:
            openScene(pending_scene_);
            break;
        case PendingAction::BackToHub:
            closeProject();
            break;
        case PendingAction::Quit:
            quit_ = true;
            break;
        case PendingAction::None:
            break;
    }
}

void EditorApp::requestQuit() {
    std::cout << "[Editor] Cierre de la ventana solicitado" << std::endl;
    runOrAskToSave(PendingAction::Quit);
}

void EditorApp::updateTitle() {
    std::string title = "Cramion Editor";
    if (has_project_) {
        title += " - " + project_.name + " - " + world_.sceneName() + (dirty_ ? " *" : "");
    } else {
        title += " - Hub";
    }
    window_.setTitle(widen(title));
}

// -----------------------------------------------------------------------------
// Deshacer / rehacer: instantaneas completas del mundo (JSON). Sencillo y
// robusto: cualquier accion (componentes, jerarquia, borrar) se deshace igual.
// -----------------------------------------------------------------------------

void EditorApp::resetUndo() {
    undo_.clear();
    redo_.clear();
    current_state_ = ecs::serializeWorld(world_);
}

void EditorApp::commit() {
    std::string state = ecs::serializeWorld(world_);
    if (state == current_state_) {
        return;
    }
    undo_.push_back(std::move(current_state_));
    current_state_ = std::move(state);
    redo_.clear();

    std::size_t memory = current_state_.size();
    for (const std::string& s : undo_) {
        memory += s.size();
    }
    while (!undo_.empty() && (undo_.size() > kUndoMaxSteps || memory > kUndoMemoryLimit)) {
        memory -= undo_.front().size();
        undo_.pop_front();
    }
    dirty_ = true;
    updateTitle();
}

void EditorApp::undo() {
    if (undo_.empty()) {
        return;
    }
    redo_.push_back(std::move(current_state_));
    current_state_ = std::move(undo_.back());
    undo_.pop_back();
    ecs::deserializeWorld(world_, current_state_);
    dirty_ = true;
    updateTitle();
}

void EditorApp::redo() {
    if (redo_.empty()) {
        return;
    }
    undo_.push_back(std::move(current_state_));
    current_state_ = std::move(redo_.back());
    redo_.pop_back();
    ecs::deserializeWorld(world_, current_state_);
    dirty_ = true;
    updateTitle();
}

// -----------------------------------------------------------------------------
// Seleccion
// -----------------------------------------------------------------------------

bool EditorApp::isSelected(const Uuid& uuid) const {
    return findUuid(selection_, uuid) != selection_.end();
}

void EditorApp::selectOnly(const Uuid& uuid) {
    selection_.clear();
    if (uuid.valid()) {
        selection_.push_back(uuid);
    }
    active_ = uuid;
}

void EditorApp::toggleSelection(const Uuid& uuid) {
    const auto it = findUuid(selection_, uuid);
    if (it != selection_.end()) {
        selection_.erase(it);
        if (active_ == uuid) {
            active_ = selection_.empty() ? Uuid{} : selection_.back();
        }
    } else {
        selection_.push_back(uuid);
        active_ = uuid;
    }
}

void EditorApp::clearSelection() {
    selection_.clear();
    active_ = {};
    renaming_ = {};
}

std::vector<ecs::Entity> EditorApp::selectedEntities() const {
    std::vector<ecs::Entity> result;
    for (const Uuid& uuid : selection_) {
        const ecs::Entity e = world_.find(uuid);
        if (e.valid()) {
            result.push_back(e);
        }
    }
    return result;
}

std::vector<ecs::Entity> EditorApp::topLevelSelection() const {
    const std::vector<ecs::Entity> all = selectedEntities();
    std::vector<ecs::Entity> result;
    for (ecs::Entity e : all) {
        const bool inside = std::any_of(all.begin(), all.end(), [&](const ecs::Entity& other) {
            return !(other == e) && other.isAncestorOf(e);
        });
        if (!inside) {
            result.push_back(e);
        }
    }
    return result;
}

void EditorApp::revealInHierarchy(const Uuid& uuid) {
    reveal_ = uuid;
}

// -----------------------------------------------------------------------------
// Acciones sobre entidades
// -----------------------------------------------------------------------------

// 0 vacio, 1 cubo, 2 esfera, 3 plano, 4 cilindro, 5 capsula, 6 luz
// direccional, 7 puntual, 8 foco, 9 camara.
ecs::Entity EditorApp::createEntity(int kind, ecs::Entity parent) {
    ecs::Entity created;
    switch (kind) {
        case 1: created = ecs::createPrimitive(world_, assets::builtin::kCube, "Cubo", parent); break;
        case 2: created = ecs::createPrimitive(world_, assets::builtin::kSphere, "Esfera", parent); break;
        case 3: created = ecs::createPrimitive(world_, assets::builtin::kPlane, "Plano", parent); break;
        case 4: created = ecs::createPrimitive(world_, assets::builtin::kCylinder, "Cilindro", parent); break;
        case 5: created = ecs::createPrimitive(world_, assets::builtin::kCapsule, "Capsula", parent); break;
        case 6: created = ecs::createLight(world_, ecs::LightType::Directional, parent); break;
        case 7: created = ecs::createLight(world_, ecs::LightType::Point, parent); break;
        case 8: created = ecs::createLight(world_, ecs::LightType::Spot, parent); break;
        case 9: created = ecs::createCamera(world_, parent); break;
        case 10:
        case 11:
        case 12: created = createDecal(kind - 10, parent); break;
        default: created = ecs::createEmpty(world_, parent); break;
    }
    // Sin padre: delante de la camara del editor, como Unity.
    if (created.valid() && !parent.valid() && kind != 6 && kind < 10) {
        const scene::Camera& camera = scene_.camera();
        created.setWorldPosition(camera.position() + camera.forward() * 6.0f);
    }
    selectOnly(created.uuid());
    revealInHierarchy(created.uuid());
    commit();
    return created;
}

void EditorApp::deleteSelection() {
    const std::vector<ecs::Entity> targets = topLevelSelection();
    if (targets.empty()) {
        return;
    }
    for (ecs::Entity e : targets) {
        world_.destroy(e);
    }
    clearSelection();
    commit();
}

void EditorApp::duplicateSelection() {
    const std::vector<ecs::Entity> targets = topLevelSelection();
    if (targets.empty()) {
        return;
    }
    selection_.clear();
    for (ecs::Entity e : targets) {
        const ecs::Entity copy = world_.duplicate(e);
        if (copy.valid()) {
            selection_.push_back(copy.uuid());
            active_ = copy.uuid();
        }
    }
    commit();
}

void EditorApp::copySelection() {
    clipboard_.clear();
    for (ecs::Entity e : topLevelSelection()) {
        clipboard_.push_back(ecs::serializeEntity(world_, e));
    }
}

void EditorApp::pasteClipboard() {
    if (clipboard_.empty()) {
        return;
    }
    // Se pega junto a la seleccion (mismo padre), como Unity.
    ecs::Entity parent;
    if (const ecs::Entity active = world_.find(active_); active.valid()) {
        parent = active.parent();
    }
    selection_.clear();
    for (const std::string& json : clipboard_) {
        const ecs::Entity pasted = ecs::pasteEntities(world_, json, parent);
        if (pasted.valid()) {
            selection_.push_back(pasted.uuid());
            active_ = pasted.uuid();
        }
    }
    commit();
}

ecs::Entity EditorApp::instantiateAsset(const Uuid& uuid, ecs::Entity parent,
                                        const std::optional<Vec3>& world_position) {
    const std::shared_ptr<const assets::ModelAsset> model = asset_manager_->loadModel(uuid);
    if (!model) {
        std::cerr << "[Editor] No se pudo cargar el modelo\n";
        return {};
    }
    ecs::Entity root = ecs::instantiateModel(world_, *model, parent);
    if (root.valid() && world_position) {
        root.setWorldPosition(*world_position);
    }
    selectOnly(root.uuid());
    revealInHierarchy(root.uuid());
    commit();
    return root;
}

// Un cielo HDR soltado en la escena: va al Sky de la escena (se crea uno si
// no hay).
void EditorApp::assignEnvironment(const Uuid& uuid) {
    ecs::Entity target;
    world_.forEachDepthFirst([&](ecs::Entity e) {
        if (!target.valid() && e.has<ecs::Sky>()) {
            target = e;
        }
    });
    if (!target.valid()) {
        target = world_.create("Entorno");
        target.add<ecs::Sky>();
    }
    ecs::Sky& sky = target.get<ecs::Sky>();
    sky.environment = assets::AssetRef{uuid, assets::AssetType::Environment};
    sky.use_hdr = true;
    selectOnly(target.uuid());
    commit();
}

void EditorApp::focusSelection() {
    const std::vector<ecs::Entity> targets = topLevelSelection();
    if (targets.empty() || !sync_) {
        return;
    }
    // Caja de todo lo seleccionado (sus actores), o su posicion.
    Vec3 low{1e30f, 1e30f, 1e30f};
    Vec3 high{-1e30f, -1e30f, -1e30f};
    bool any = false;
    const auto grow = [&](const Vec3& p) {
        low = Vec3{std::min(low.x, p.x), std::min(low.y, p.y), std::min(low.z, p.z)};
        high = Vec3{std::max(high.x, p.x), std::max(high.y, p.y), std::max(high.z, p.z)};
        any = true;
    };
    for (ecs::Entity e : targets) {
        for (const std::uint32_t actor : sync_->actorIndicesInSubtree(e)) {
            Vec3 mn{};
            Vec3 mx{};
            const ecs::Entity owner = sync_->entityForActor(world_, actor);
            if (owner.valid() && sync_->actorLocalBounds(actor, mn, mx)) {
                const core::Aabb box = core::transformAabb(owner.worldMatrix(), core::Aabb{mn, mx});
                grow(box.min);
                grow(box.max);
            }
        }
        if (!any) {
            // Aun sin actores en el render (recien abierta la escena): la
            // posicion de la entidad y de todos sus descendientes.
            std::vector<ecs::Entity> stack{e};
            while (!stack.empty()) {
                const ecs::Entity node = stack.back();
                stack.pop_back();
                grow(node.worldPosition() - Vec3{0.5f, 0.5f, 0.5f});
                grow(node.worldPosition() + Vec3{0.5f, 0.5f, 0.5f});
                for (const entt::entity child : node.children()) stack.push_back(world_.wrap(child));
            }
        }
    }
    const Vec3 center = (low + high) * 0.5f;
    const float radius = std::max(core::length(high - low) * 0.5f, 0.5f);
    scene::Camera& camera = scene_.camera();
    const float distance = radius / std::tan(camera.fovY() * 0.5f) * 1.1f;
    camera.setPosition(center - camera.forward() * distance);
    camera.lookAt(center);
}

// -----------------------------------------------------------------------------
// Frame
// -----------------------------------------------------------------------------

void EditorApp::drawUi(float delta_seconds) {
    ImGuizmo::BeginFrame();
    pollImports();
    watchAssets();
    runSelfTestStep();

    if (!has_project_) {
        flying_ = false;
        drawHub();
        drawModals();
        return;
    }

    drawMenuBar();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImGuiID dockspace_id = ImGui::GetID("CramionDockspace");
    if (reset_layout_ || ImGui::DockBuilderGetNode(dockspace_id) == nullptr) {
        buildDefaultLayout(dockspace_id);
        reset_layout_ = false;
    }
    ImGui::DockSpaceOverViewport(dockspace_id, viewport);

    drawSceneView();
    if (show_hierarchy_) drawHierarchy();
    if (show_inspector_) drawInspector();
    if (show_project_) drawProject();
    if (show_statistics_) drawStatistics(delta_seconds);
    if (show_console_) drawConsole();
    if (show_render_settings_) drawRenderSettings();
    if (show_animator_) drawAnimatorEditor();
    drawModals();

    // Atajos globales (no mientras se escribe ni se vuela).
    ImGuiIO& io = ImGui::GetIO();
    if (!io.WantTextInput && !flying_) {
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) {
            if (io.KeyShift) {
                saveSceneAs();
            } else {
                saveScene();
            }
        }
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z)) undo();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y)) redo();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_N, false)) runOrAskToSave(PendingAction::NewScene);
    }
}

void EditorApp::syncWorld(float delta_seconds) {
    if (!has_project_ || !sync_) {
        return;
    }
    sync_->sync(world_, scene_, renderer_, delta_seconds);

    // Contorno de la seleccion (la entidad y sus hijos).
    std::vector<std::uint32_t> outlined;
    for (ecs::Entity e : topLevelSelection()) {
        const std::vector<std::uint32_t> actors = sync_->actorIndicesInSubtree(e);
        outlined.insert(outlined.end(), actors.begin(), actors.end());
    }
    renderer_.setOutlinedActors(std::move(outlined));
}

// Unity: Jerarquia a la izquierda; Escena en el centro; Inspector a la
// derecha; Proyecto y Consola abajo.
void EditorApp::buildDefaultLayout(unsigned int dockspace_id) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::DockBuilderRemoveNode(dockspace_id);
    ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace_id, viewport->WorkSize);

    ImGuiID center = dockspace_id;
    const ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.24f, nullptr, &center);
    const ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.30f, nullptr, &center);
    const ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.20f, nullptr, &center);

    ImGui::DockBuilderDockWindow("Jerarquía", left);
    ImGui::DockBuilderDockWindow("Inspector", right);
    ImGui::DockBuilderDockWindow("Ajustes de render", right);
    ImGui::DockBuilderDockWindow("Proyecto", bottom);
    ImGui::DockBuilderDockWindow("Consola", bottom);
    ImGui::DockBuilderDockWindow("Estadísticas", bottom);
    ImGui::DockBuilderDockWindow("Escena", center);
    ImGui::DockBuilderDockWindow("Animator", center);
    ImGui::DockBuilderFinish(dockspace_id);
}

// -----------------------------------------------------------------------------
// Menus
// -----------------------------------------------------------------------------

void EditorApp::drawMenuBar() {
    if (!ImGui::BeginMainMenuBar()) {
        return;
    }
    if (ImGui::BeginMenu("Archivo")) {
        if (ImGui::MenuItem("Nueva escena", "Ctrl+N")) runOrAskToSave(PendingAction::NewScene);
        if (ImGui::MenuItem("Abrir escena...")) {
            const std::filesystem::path path = dialogs::openFile(
                window_.handle(), L"Escena de Cramion (*.crscene)\0*.crscene\0",
                project_.assetsFolder() / "Scenes");
            if (!path.empty()) runOrAskToSave(PendingAction::OpenScene, path);
        }
        if (ImGui::MenuItem("Guardar", "Ctrl+S")) saveScene();
        if (ImGui::MenuItem("Guardar como...", "Ctrl+Shift+S")) saveSceneAs();
        ImGui::Separator();
        if (ImGui::MenuItem("Importar assets...")) {
            const auto files = dialogs::openFiles(
                window_.handle(),
                L"Modelos y cielos (*.obj;*.fbx;*.gltf;*.glb;*.dae;*.hdr)\0*.obj;*.fbx;*.gltf;*.glb;*.dae;*.hdr\0Todos\0*.*\0");
            startImport(files, current_folder_);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Usar como escena inicial", nullptr, false, !scene_path_.empty())) {
            project_.startup_scene = world_.sceneUuid();
            project::saveProject(project_);
            std::cout << "[Editor] Escena inicial del proyecto: " << world_.sceneName() << "\n";
        }
        if (ImGui::MenuItem("Abrir proyecto (Hub)...")) runOrAskToSave(PendingAction::BackToHub);
        ImGui::Separator();
        if (ImGui::MenuItem("Salir")) requestQuit();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Editar")) {
        if (ImGui::MenuItem("Deshacer", "Ctrl+Z", false, !undo_.empty())) undo();
        if (ImGui::MenuItem("Rehacer", "Ctrl+Y", false, !redo_.empty())) redo();
        ImGui::Separator();
        const bool any = !selection_.empty();
        if (ImGui::MenuItem("Copiar", "Ctrl+C", false, any)) copySelection();
        if (ImGui::MenuItem("Pegar", "Ctrl+V", false, !clipboard_.empty())) pasteClipboard();
        if (ImGui::MenuItem("Duplicar", "Ctrl+D", false, any)) duplicateSelection();
        if (ImGui::MenuItem("Borrar", "Supr", false, any)) deleteSelection();
        ImGui::Separator();
        if (ImGui::MenuItem("Enfocar selección", "F", false, any)) focusSelection();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("GameObject")) {
        const ecs::Entity parent = world_.find(active_);
        const auto item = [&](const char* label, int kind) {
            if (ImGui::MenuItem(label)) createEntity(kind, {});
        };
        item("Crear vacío", 0);
        if (ImGui::MenuItem("Crear vacío hijo", nullptr, false, parent.valid())) createEntity(0, parent);
        if (ImGui::BeginMenu("Objeto 3D")) {
            item("Cubo", 1);
            item("Esfera", 2);
            item("Plano", 3);
            item("Cilindro", 4);
            item("Cápsula", 5);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Luz")) {
            item("Luz direccional", 6);
            item("Luz puntual", 7);
            item("Foco", 8);
            ImGui::EndMenu();
        }
        item("Cámara", 9);
        ImGui::Separator();
        item("Decal (estampa)", 10);
        item("Charco", 11);
        item("Humedad", 12);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Ventana")) {
        ImGui::MenuItem("Jerarquía", nullptr, &show_hierarchy_);
        ImGui::MenuItem("Inspector", nullptr, &show_inspector_);
        ImGui::MenuItem("Proyecto", nullptr, &show_project_);
        ImGui::MenuItem("Estadísticas", nullptr, &show_statistics_);
        ImGui::MenuItem("Consola", nullptr, &show_console_);
        ImGui::MenuItem("Ajustes de render", nullptr, &show_render_settings_);
        ImGui::MenuItem("Animator", nullptr, &show_animator_);
        ImGui::Separator();
        if (ImGui::MenuItem("Restablecer diseño")) {
            reset_layout_ = true;
            show_hierarchy_ = show_inspector_ = show_project_ = show_statistics_ = show_console_ =
                show_render_settings_ = true;
        }
        ImGui::EndMenu();
    }

    char status[200];
    std::snprintf(status, sizeof(status), "%s   |   %.0f FPS   |   GPU %.2f ms",
                  project_.name.c_str(), ImGui::GetIO().Framerate,
                  renderer_.gpuProfiler().totalMilliseconds());
    ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::CalcTextSize(status).x - 16.0f);
    ImGui::TextDisabled("%s", status);
    ImGui::EndMainMenuBar();
}

// -----------------------------------------------------------------------------
// Hub
// -----------------------------------------------------------------------------

void EditorApp::drawHub() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::Begin("Cramion Hub", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);

    // Columna izquierda: marca y acciones.
    ImGui::BeginChild("hub_side", ImVec2(260.0f, 0.0f), ImGuiChildFlags_Borders);
    ImGui::SetWindowFontScale(1.6f);
    ImGui::TextUnformatted("Cramion");
    ImGui::SetWindowFontScale(1.0f);
    ImGui::TextDisabled("Hub de proyectos");
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    if (ImGui::Button("Nuevo proyecto", ImVec2(-1.0f, 34.0f))) {
        show_new_project_ = true;
        hub_error_.clear();
    }
    ImGui::Spacing();
    if (ImGui::Button("Abrir proyecto...", ImVec2(-1.0f, 34.0f))) {
        const std::filesystem::path path = dialogs::openFile(
            window_.handle(), L"Proyecto de Cramion (*.crproj)\0*.crproj\0");
        if (!path.empty()) {
            openProject(path);
        }
    }
    ImGui::Spacing();
    if (ImGui::Button("Salir", ImVec2(-1.0f, 28.0f))) {
        quit_ = true;
    }
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("hub_main", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);
    ImGui::SetWindowFontScale(1.25f);
    ImGui::TextUnformatted("Proyectos");
    ImGui::SetWindowFontScale(1.0f);
    ImGui::Spacing();
    if (!hub_error_.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", hub_error_.c_str());
    }

    const std::vector<project::RecentProject> recent = project::recentProjects();
    if (recent.empty()) {
        ImGui::TextDisabled("Todavía no hay proyectos. Crea uno nuevo o abre uno existente.");
    } else if (ImGui::BeginTable("recent", 4,
                                 ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                     ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX)) {
        ImGui::TableSetupColumn("Nombre", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Ruta", ImGuiTableColumnFlags_WidthStretch, 2.2f);
        ImGui::TableSetupColumn("Última apertura", ImGuiTableColumnFlags_WidthFixed, 150.0f);
        ImGui::TableSetupColumn("##acciones", ImGuiTableColumnFlags_WidthFixed, 150.0f);
        ImGui::TableHeadersRow();
        std::optional<std::filesystem::path> to_open;
        std::optional<std::filesystem::path> to_remove;
        for (const project::RecentProject& p : recent) {
            ImGui::PushID(dialogs::utf8(p.file).c_str());
            ImGui::TableNextRow(0, 36.0f);
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            if (ImGui::Selectable(p.name.c_str(), false,
                                  ImGuiSelectableFlags_SpanAllColumns |
                                      ImGuiSelectableFlags_AllowOverlap,
                                  ImVec2(0.0f, 30.0f)) &&
                ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                to_open = p.file;
            }
            ImGui::TableNextColumn();
            const bool exists = std::filesystem::exists(p.file);
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s%s", dialogs::utf8(p.file.parent_path()).c_str(),
                                exists ? "" : "  (no encontrado)");
            ImGui::TableNextColumn();
            char date[64] = "-";
            if (p.last_opened > 0) {
                const std::time_t t = static_cast<std::time_t>(p.last_opened);
                std::tm local{};
                localtime_s(&local, &t);
                std::strftime(date, sizeof(date), "%d/%m/%Y %H:%M", &local);
            }
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(date);
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(!exists);
            if (ImGui::Button("Abrir")) to_open = p.file;
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Quitar")) to_remove = p.file;
            ImGui::PopID();
        }
        ImGui::EndTable();
        if (to_remove) project::removeRecentProject(*to_remove);
        if (to_open) openProject(*to_open);
    }
    ImGui::EndChild();
    ImGui::End();

    // --- Nuevo proyecto ---
    if (show_new_project_) {
        ImGui::OpenPopup("Nuevo proyecto");
        show_new_project_ = false;
    }
    ImGui::SetNextWindowSize(ImVec2(560.0f, 0.0f), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Nuevo proyecto", nullptr, ImGuiWindowFlags_NoResize)) {
        ImGui::TextUnformatted("Nombre");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputText("##name", &new_project_name_);
        ImGui::TextUnformatted("Ubicación");
        ImGui::SetNextItemWidth(-40.0f);
        ImGui::InputText("##folder", &new_project_folder_);
        ImGui::SameLine();
        if (ImGui::Button("...", ImVec2(32.0f, 0.0f))) {
            const std::filesystem::path folder =
                dialogs::pickFolder(window_.handle(), dialogs::fromUtf8(new_project_folder_));
            if (!folder.empty()) new_project_folder_ = dialogs::utf8(folder);
        }
        ImGui::TextDisabled("Se creará: %s",
                            dialogs::utf8(dialogs::fromUtf8(new_project_folder_) /
                                          dialogs::fromUtf8(new_project_name_))
                                .c_str());
        if (!hub_error_.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", hub_error_.c_str());
        }
        ImGui::Spacing();
        if (ImGui::Button("Crear", ImVec2(120.0f, 0.0f)) && !new_project_name_.empty()) {
            try {
                std::filesystem::create_directories(dialogs::fromUtf8(new_project_folder_));
                project::ProjectInfo info = project::createProject(
                    dialogs::fromUtf8(new_project_folder_), new_project_name_);
                // Escena por defecto guardada como inicial.
                ecs::World world;
                world.setSceneName("Main");
                ecs::populateDefaultScene(world);
                const std::filesystem::path scene = info.assetsFolder() / "Scenes" / "Main.crscene";
                std::filesystem::create_directories(scene.parent_path());
                ecs::saveScene(world, scene);
                info.startup_scene = world.sceneUuid();
                project::saveProject(info);
                ImGui::CloseCurrentPopup();
                hub_error_.clear();
                openProject(info.file);
            } catch (const std::exception& error) {
                hub_error_ = error.what();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancelar", ImVec2(120.0f, 0.0f))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

// -----------------------------------------------------------------------------
// Dialogos modales
// -----------------------------------------------------------------------------

void EditorApp::drawModals() {
    if (ask_save_) {
        ImGui::OpenPopup("¿Guardar cambios?");
        ask_save_ = false;
    }
    if (ImGui::BeginPopupModal("¿Guardar cambios?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("La escena \"%s\" tiene cambios sin guardar.", world_.sceneName().c_str());
        ImGui::Spacing();
        if (ImGui::Button("Guardar", ImVec2(120.0f, 0.0f))) {
            ImGui::CloseCurrentPopup();
            if (saveScene()) performPending();
            else pending_ = PendingAction::None;
        }
        ImGui::SameLine();
        if (ImGui::Button("No guardar", ImVec2(120.0f, 0.0f))) {
            ImGui::CloseCurrentPopup();
            dirty_ = false;
            performPending();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancelar", ImVec2(120.0f, 0.0f))) {
            ImGui::CloseCurrentPopup();
            pending_ = PendingAction::None;
        }
        ImGui::EndPopup();
    }

    if (pending_delete_asset_.valid()) {
        ImGui::OpenPopup("Borrar asset");
    }
    if (ImGui::BeginPopupModal("Borrar asset", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const auto info = database_ ? database_->find(pending_delete_asset_) : std::nullopt;
        ImGui::Text("¿Borrar \"%s\"? No se puede deshacer.", info ? info->name.c_str() : "?");
        if (ImGui::Button("Borrar", ImVec2(120.0f, 0.0f))) {
            if (database_) {
                if (asset_manager_) asset_manager_->unload(pending_delete_asset_);
                database_->remove(pending_delete_asset_);
            }
            pending_delete_asset_ = {};
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancelar", ImVec2(120.0f, 0.0f))) {
            pending_delete_asset_ = {};
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

// -----------------------------------------------------------------------------
// Estadisticas, consola, ajustes de render
// -----------------------------------------------------------------------------

void EditorApp::drawStatistics(float delta_seconds) {
    frame_history_[frame_history_head_] = delta_seconds * 1000.0f;
    frame_history_head_ = (frame_history_head_ + 1) % frame_history_.size();
    if (!ImGui::Begin("Estadísticas", &show_statistics_)) {
        ImGui::End();
        return;
    }
    const ImGuiIO& io = ImGui::GetIO();
    const gfx::GpuProfiler& profiler = renderer_.gpuProfiler();
    ImGui::Text("%.0f FPS  |  frame %.2f ms  |  GPU %.2f ms  |  %zu entidades", io.Framerate,
                1000.0f / std::max(io.Framerate, 1.0f), profiler.totalMilliseconds(),
                world_.entityCount());
    char overlay[32];
    std::snprintf(overlay, sizeof(overlay), "%.2f ms", 1000.0f / std::max(io.Framerate, 1.0f));
    ImGui::PlotLines("##frames", frame_history_.data(), static_cast<int>(frame_history_.size()),
                     static_cast<int>(frame_history_head_), overlay, 0.0f, 33.3f, ImVec2(-1.0f, 50.0f));
    ImGui::Text("Triángulos %llu  |  submallas visibles %u / %u (tapadas %u, en sombras %u)",
                static_cast<unsigned long long>(renderer_.triangleCount()),
                renderer_.visibleSubmeshes(), renderer_.totalSubmeshes(),
                renderer_.occludedSubmeshes(), renderer_.shadowSubmeshes());
    if (profiler.supported() &&
        ImGui::BeginTable("gpu", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Pasada de GPU", ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn("ms", ImGuiTableColumnFlags_WidthFixed, 56.0f);
        ImGui::TableSetupColumn("%", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableHeadersRow();
        const float total = std::max(profiler.totalMilliseconds(), 0.001f);
        for (const gfx::GpuTiming& timing : profiler.timings()) {
            if (timing.milliseconds < 0.005f) continue;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(timing.name.c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%.2f", timing.milliseconds);
            ImGui::TableNextColumn();
            ImGui::ProgressBar(timing.milliseconds / total, ImVec2(-1.0f, 0.0f), "");
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

void EditorApp::drawConsole() {
    if (!ImGui::Begin("Consola", &show_console_)) {
        ImGui::End();
        return;
    }
    EditorLog& log = EditorLog::instance();
    if (ImGui::Button("Limpiar")) log.clear();
    ImGui::SameLine();
    ImGui::Checkbox("Auto-desplazar", &console_autoscroll_);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##filter", "Filtrar...", &console_filter_);
    ImGui::Separator();
    ImGui::BeginChild("log", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None,
                      ImGuiWindowFlags_HorizontalScrollbar);
    {
        std::lock_guard lock(log.mutex());
        const auto line = [](const EditorLog::Entry& entry) {
            ImVec4 color{0.85f, 0.85f, 0.85f, 1.0f};
            if (entry.level == EditorLog::Level::Warning) color = ImVec4{1.0f, 0.8f, 0.35f, 1.0f};
            if (entry.level == EditorLog::Level::Error) color = ImVec4{1.0f, 0.45f, 0.4f, 1.0f};
            ImGui::PushStyleColor(ImGuiCol_Text, color);
            ImGui::TextUnformatted(entry.text.c_str());
            ImGui::PopStyleColor();
        };
        const auto& entries = log.entries();
        if (console_filter_.empty()) {
            // Solo se dibujan las lineas visibles (miles de lineas no cuestan).
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(entries.size()));
            while (clipper.Step()) {
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) line(entries[i]);
            }
        } else {
            for (const EditorLog::Entry& entry : entries) {
                if (entry.text.find(console_filter_) != std::string::npos) line(entry);
            }
        }
    }
    if (console_autoscroll_ && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f) {
        ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
    ImGui::End();
}

// Lo global del renderizador que no esta en el componente PostProcessing.
void EditorApp::drawRenderSettings() {
    if (!ImGui::Begin("Ajustes de render", &show_render_settings_)) {
        ImGui::End();
        return;
    }
    gfx::VulkanRenderer& r = renderer_;
    const auto toggle = [&](const char* label, bool value, void (gfx::VulkanRenderer::*setter)(bool)) {
        bool v = value;
        if (ImGui::Checkbox(label, &v)) (r.*setter)(v);
    };
    ImGui::SeparatorText("Sombras");
    toggle("Sombras", r.shadowsEnabled(), &gfx::VulkanRenderer::setShadowsEnabled);
    toggle("Ver cascadas", r.cascadeDebug(), &gfx::VulkanRenderer::setCascadeDebug);
    ImGui::SeparatorText("Iluminación global");
    if (r.rayTracingSupported()) {
        toggle("Trazado de rayos (RTX)", r.rayTracingEnabled(), &gfx::VulkanRenderer::setRayTracingEnabled);
    } else {
        ImGui::TextDisabled("Trazado de rayos: la GPU no lo admite");
    }
    toggle("Sonda de reflexión", r.reflectionProbeEnabled(),
           &gfx::VulkanRenderer::setReflectionProbeEnabled);
    ImGui::SeparatorText("Rendimiento");
    toggle("Occlusion culling (GPU)", r.occlusionCullingEnabled(),
           &gfx::VulkanRenderer::setOcclusionCullingEnabled);
    ImGui::Spacing();
    ImGui::TextDisabled("Exposición, tono, bloom, color, viñeta, SSAO, GI,\n"
                        "reflejos y luz volumétrica: componente PostProcessing.");
    ImGui::End();
}

// -----------------------------------------------------------------------------
// Importacion en segundo plano: el importador puede tardar (un FBX grande,
// casi un minuto); la interfaz sigue viva y el asset aparece al terminar.
// -----------------------------------------------------------------------------

void EditorApp::startImport(const std::vector<std::filesystem::path>& files,
                            const std::filesystem::path& folder) {
    if (!has_project_ || files.empty()) {
        return;
    }
    for (const std::filesystem::path& file : files) {
        // Imagenes (texturas de decals): se copian tal cual a Assets/.
        if (isDecalImage(file)) {
            std::error_code error;
            std::filesystem::path target = folder / file.filename();
            if (!std::filesystem::exists(target)) std::filesystem::copy_file(file, target, error);
            if (error) std::cerr << "[Editor] No se pudo copiar " << dialogs::utf8(file.filename()) << "\n";
            else std::cout << "[Editor] Imagen copiada: " << dialogs::utf8(file.filename()) << "\n";
            refreshDatabase();
            continue;
        }
        std::cout << "[Editor] Importando " << dialogs::utf8(file.filename()) << "...\n";
        imports_.push_back(ImportJob{file, std::async(std::launch::async, [file, folder] {
                                         return assets::importAny(file, folder);
                                     })});
    }
}

void EditorApp::pollImports() {
    bool finished = false;
    for (auto it = imports_.begin(); it != imports_.end();) {
        if (it->result.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            const assets::ImportResult result = it->result.get();
            if (result.ok) {
                std::cout << "[Editor] Importado: " << result.info.name << " ("
                          << assets::assetTypeName(result.info.type) << ")\n";
            } else {
                std::cerr << "[Editor] Error al importar " << dialogs::utf8(it->source.filename())
                          << ": " << result.message << "\n";
            }
            it = imports_.erase(it);
            finished = true;
        } else {
            ++it;
        }
    }
    if (finished && database_) {
        refreshDatabase();
    }
}

void EditorApp::onFilesDropped(const std::vector<std::filesystem::path>& files) {
    if (has_project_) {
        startImport(files, current_folder_);
    } else {
        // En el Hub: un .crproj soltado se abre.
        for (const std::filesystem::path& file : files) {
            if (file.extension() == ".crproj" && openProject(file)) {
                break;
            }
        }
    }
}

}  // namespace cramion::editor
