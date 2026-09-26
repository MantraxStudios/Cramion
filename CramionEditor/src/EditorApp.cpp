#include "EditorApp.h"

#include "Dialogs.h"
#include "EditorLog.h"

#include <shellapi.h>

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include <ImGuizmo.h>

#include <algorithm>
#include <fstream>
#include <array>
#include <cctype>
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

using CpuClock = std::chrono::steady_clock;

float millisecondsSince(CpuClock::time_point start) {
    return std::chrono::duration<float, std::milli>(CpuClock::now() - start).count();
}

std::wstring widen(const std::string& text) {
    return dialogs::fromUtf8(text).wstring();
}

}  // namespace

EditorApp::EditorApp(dm::Window& window, gfx::VulkanRenderer& renderer, scene::Scene& scene,
                     ImGuiLayer& imgui)
    : window_(window), renderer_(renderer), scene_(scene), imgui_(imgui) {
    // Los componentes de fisica tienen que existir antes de leer escenas.
    physics::registerPhysicsComponents();
    ecs::registerPrefabComponents();
    cinema::registerCinematicComponents();
    terrain::registerTerrainComponents();
    water::registerWaterComponents();
    navigation::registerNavigationComponents();
    voxel::registerVoxelComponents();
    audio::registerAudioComponents();
    scripting::registerScriptComponents();
    ui::registerUiComponents();
    startMcp();  // servidor MCP para IA (solo este PC)
    physics_.addListener([this](const physics::PhysicsEvent& event) { onPhysicsEvent(event); });
    const std::filesystem::path documents = dialogs::documentsFolder();
    new_project_folder_ = dialogs::utf8(documents.empty() ? std::filesystem::current_path()
                                                          : documents / "Cramion Projects");
    updateTitle();
}

EditorApp::~EditorApp() {
    // Cerrar en Play: los scripts terminan (OnDestroy) mientras todo lo que usan
    // sigue vivo (el mundo de bloques se destruye antes que ellos).
    scripts_.stop();
    scripts_.setVoxels(nullptr);
    voxels_.stop();
    cancelExport();
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
    model_previews_.start(project_.libraryFolder() / "Thumbnails");
    scripts_.setAssetsRoot(project_.assetsFolder());
    scripts_.setPrefsFile(project_.libraryFolder() / "Prefs.txt");  // Prefs en Play
    scripts_.setPhysics(&physics_);
    scripts_.setAudio(&audio_);
    scripts_.setNavigation(&nav_);
    scripts_.setVoxels(&voxels_);
    // Input.lockCursor: el raton encerrado en la vista Juego (Escape lo suelta).
    scripts_.setCursorLock([this](bool locked) {
        const RECT area{static_cast<LONG>(game_image_rect_[0]), static_cast<LONG>(game_image_rect_[1]),
                        static_cast<LONG>(game_image_rect_[0] + game_image_rect_[2]),
                        static_cast<LONG>(game_image_rect_[1] + game_image_rect_[3])};
        window_.setCursorCaptured(locked, game_image_rect_[2] > 0.0f ? &area : nullptr);
    });
    audio_.setAssetsRoot(project_.assetsFolder());
    // Mundos de bloques: texturas en Assets, partidas guardadas en Library.
    voxels_.setAssetsRoot(project_.assetsFolder());
    voxels_.setSaveRoot(project_.libraryFolder() / "Worlds");
    voxels_.setPhysics(&physics_);  // los Rigidbody chocan con los bloques
    loadGraphicsSettings();
    sync_ = std::make_unique<ecs::RenderSync>(*asset_manager_);
    sync_->reset(scene_);
    // Terrenos: datos en Assets (compartidos por el render y la fisica).
    terrain_store_.clear();
    terrain_store_.setRoot(project_.assetsFolder());
    sync_->setTerrainStore(&terrain_store_);
    physics_.setTerrainProvider([this](ecs::Entity entity) -> std::shared_ptr<const terrain::TerrainData> {
        const terrain::Terrain* comp = entity.tryGet<terrain::Terrain>();
        return comp != nullptr ? terrain_store_.get(*comp) : nullptr;
    });
    has_project_ = true;
    thumbnail_countdown_ = 240;  // ~4 s: la escena ya dibujada para la miniatura del Hub
    // Fisica: ajustes del proyecto y el mundo fisico (en modo edicion).
    loadPhysicsSettings();
    physics_.setAssetManager(asset_manager_.get());
    physics_.setMeshProvider([this](ecs::Entity entity) -> const asset::ModelData* {
        return sync_ ? sync_->actorModelData(entity, scene_) : nullptr;
    });
    physics_.start(world_);
    // Navegacion: la misma geometria de colision que la fisica.
    nav_.clear();
    nav_.setMeshProvider([this](ecs::Entity entity) -> const asset::ModelData* {
        return sync_ ? sync_->actorModelData(entity, scene_) : nullptr;
    });
    nav_.setTerrainProvider([this](ecs::Entity entity) -> std::shared_ptr<const terrain::TerrainData> {
        const terrain::Terrain* comp = entity.tryGet<terrain::Terrain>();
        return comp != nullptr ? terrain_store_.get(*comp) : nullptr;
    });
    nav_.setPhysics(&physics_);
    loadNavigationSettings();
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
    if (playing()) exitPlay();
    physics_.stop();
    physics_.setMeshProvider({});
    physics_.setAssetManager(nullptr);
    physics_.setTerrainProvider({});
    nav_.clear();
    nav_.setMeshProvider({});
    nav_.setTerrainProvider({});
    stopVoxels();
    voxels_.syncRenderer(renderer_);  // quita sus secciones del renderizador
    model_previews_.stop();
    clip_source_.reset();
    clip_source_uuid_ = {};
    particles_.clear();
    renderer_.setParticles({});
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
    terrain_store_.clear();
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
    nav_.clear();
    stopVoxels();
    const ecs::DVec3 origin_before = world_.origin();
    world_.clear();
    alignOriginAfterLoad(origin_before);
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
    nav_.clear();
    stopVoxels();
    const ecs::DVec3 origin_before = world_.origin();
    if (!ecs::loadScene(world_, path, &error)) {
        std::cerr << "[Editor] No se pudo abrir la escena " << dialogs::utf8(path) << ": " << error
                  << "\n";
        return false;
    }
    alignOriginAfterLoad(origin_before);
    scene_path_ = path;
    clearSelection();
    syncPrefabInstances();  // prefabs que cambiaron con la escena cerrada
    resetUndo();
    dirty_ = false;
    std::cout << "[Editor] Escena abierta: " << dialogs::utf8(path.filename()) << "\n";
    updateTitle();
    return true;
}

bool EditorApp::saveScene() {
    flushCommit();
    if (playing()) {
        std::cerr << "[Editor] Sal del modo Play para guardar (lo que cambia en Play no se guarda)\n";
        return false;
    }
    if (scene_path_.empty()) {
        return saveSceneAs();
    }
    std::string error;
    // Los terrenos guardan sus datos aparte (Assets/Terrains/*.crterrain).
    if (const int terrains = terrain_store_.saveAll(); terrains > 0) {
        std::cout << "[Editor] " << terrains << " terreno(s) guardado(s)\n";
    }
    if (!ecs::saveScene(world_, scene_path_, &error)) {
        std::cerr << "[Editor] No se pudo guardar: " << error << "\n";
        return false;
    }
    dirty_ = false;
    refreshDatabase();
    saveProjectThumbnail();  // el Hub ensena el proyecto como se guardo
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
    if (playing()) exitPlay();
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
    commit_pending_ = false;
    undo_.clear();
    redo_.clear();
    undo_kinds_.clear();
    redo_kinds_.clear();
    terrain_undo_.clear();
    terrain_redo_.clear();
    current_state_ = ecs::serializeWorld(world_);
}

void EditorApp::commit() {
    // En Play los cambios son temporales: sin deshacer (se restaura al parar).
    if (playing()) {
        return;
    }
    // La instantanea (serializar el mundo entero) se hace una vez al final
    // del frame: varias acciones seguidas (pegar, duplicar, crear en serie)
    // cuestan una sola y quedan en un solo paso de deshacer.
    commit_pending_ = true;
}

void EditorApp::flushCommit() {
    if (!commit_pending_) {
        return;
    }
    commit_pending_ = false;
    if (playing()) {
        return;
    }
    recordPrefabOverrides();
    std::string state = ecs::serializeWorld(world_);
    if (state == current_state_) {
        return;
    }
    undo_.push_back(std::move(current_state_));
    current_state_ = std::move(state);
    redo_.clear();
    undo_kinds_.push_back('W');
    redo_kinds_.clear();
    terrain_redo_.clear();

    std::size_t memory = current_state_.size();
    for (const std::string& s : undo_) {
        memory += s.size();
    }
    while (!undo_.empty() && (undo_.size() > kUndoMaxSteps || memory > kUndoMemoryLimit)) {
        memory -= undo_.front().size();
        undo_.pop_front();
        const auto it = std::find(undo_kinds_.begin(), undo_kinds_.end(), 'W');
        if (it != undo_kinds_.end()) undo_kinds_.erase(it);
    }
    dirty_ = true;
    updateTitle();
}

void EditorApp::undo() {
    flushCommit();
    if (playing()) {
        return;
    }
    // Lo ultimo fue un trazo de terreno: se deshace el trazo.
    if (!undo_kinds_.empty() && undo_kinds_.back() == 'T' && !terrain_undo_.empty()) {
        TerrainUndo step = std::move(terrain_undo_.back());
        terrain_undo_.pop_back();
        undo_kinds_.pop_back();
        applyTerrainSnapshot(step.path, *step.before);
        terrain_redo_.push_back(std::move(step));
        redo_kinds_.push_back('T');
        dirty_ = true;
        updateTitle();
        return;
    }
    if (undo_.empty()) {
        return;
    }
    if (!undo_kinds_.empty()) undo_kinds_.pop_back();
    redo_kinds_.push_back('W');
    redo_.push_back(std::move(current_state_));
    current_state_ = std::move(undo_.back());
    undo_.pop_back();
    const ecs::DVec3 origin_before = world_.origin();
    ecs::deserializeWorld(world_, current_state_);
    alignOriginAfterLoad(origin_before);
    dirty_ = true;
    updateTitle();
}

void EditorApp::redo() {
    flushCommit();
    if (playing()) {
        return;
    }
    if (!redo_kinds_.empty() && redo_kinds_.back() == 'T' && !terrain_redo_.empty()) {
        TerrainUndo step = std::move(terrain_redo_.back());
        terrain_redo_.pop_back();
        redo_kinds_.pop_back();
        applyTerrainSnapshot(step.path, *step.after);
        terrain_undo_.push_back(std::move(step));
        undo_kinds_.push_back('T');
        dirty_ = true;
        updateTitle();
        return;
    }
    if (redo_.empty()) {
        return;
    }
    if (!redo_kinds_.empty()) redo_kinds_.pop_back();
    undo_kinds_.push_back('W');
    undo_.push_back(std::move(current_state_));
    current_state_ = std::move(redo_.back());
    redo_.pop_back();
    const ecs::DVec3 origin_before = world_.origin();
    ecs::deserializeWorld(world_, current_state_);
    alignOriginAfterLoad(origin_before);
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
    inspected_material_ = {};
    selection_.clear();
    if (uuid.valid()) {
        selection_.push_back(uuid);
    }
    active_ = uuid;
}

void EditorApp::toggleSelection(const Uuid& uuid) {
    inspected_material_ = {};
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
    inspected_material_ = {};
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
// direccional, 7 puntual, 8 foco, 9 camara, 10-12 decals, 13 cubo con
// Rigidbody, 14 esfera con Rigidbody, 15 zona trigger, 16 particulas.
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
        case 13:
            created = ecs::createPrimitive(world_, assets::builtin::kCube, "Cubo fisico", parent);
            created.add<physics::Rigidbody>();
            break;
        case 14:
            created = ecs::createPrimitive(world_, assets::builtin::kSphere, "Esfera fisica", parent);
            created.add<physics::Rigidbody>();
            break;
        case 15: {
            created = ecs::createEmpty(world_, parent);
            created.setName("Zona trigger");
            physics::BoxCollider& box = created.add<physics::BoxCollider>();
            box.size = Vec3{3.0f, 2.0f, 3.0f};
            box.material.is_trigger = true;
            break;
        }
        case 16:
            created = ecs::createEmpty(world_, parent);
            created.setName("Particulas");
            created.add<physics::ParticleSystem>();
            break;
        case 17: {
            // Coche como el WheelCollider de Unity: cuerpo con Rigidbody +
            // Vehicle y 4 hijos WheelCollider (el frente es -Z).
            created = ecs::createEmpty(world_, parent);
            created.setName("Vehiculo");
            created.add<physics::BoxCollider>().size = Vec3{1.8f, 0.6f, 4.2f};
            created.add<physics::Rigidbody>().mass = 1200.0f;
            created.add<physics::Vehicle>();
            ecs::Entity body = ecs::createPrimitive(world_, assets::builtin::kCube, "Carroceria", created);
            body.setLocalScale(Vec3{1.8f, 0.6f, 4.2f});
            ecs::Entity cabin = ecs::createPrimitive(world_, assets::builtin::kCube, "Cabina", created);
            cabin.setLocalPosition(Vec3{0.0f, 0.55f, 0.3f});
            cabin.setLocalScale(Vec3{1.6f, 0.5f, 2.0f});
            static constexpr const char* kWheelNames[] = {"Rueda DI", "Rueda DD", "Rueda TI", "Rueda TD"};
            for (int i = 0; i < 4; ++i) {
                const bool front = i < 2;
                ecs::Entity wheel = ecs::createEmpty(world_, created);
                wheel.setName(kWheelNames[i]);
                wheel.setLocalPosition(Vec3{(i % 2 == 0) ? -0.95f : 0.95f, -0.25f, front ? -1.35f : 1.35f});
                physics::WheelCollider& collider = wheel.add<physics::WheelCollider>();
                collider.max_steer_angle = front ? 35.0f : 0.0f;
                collider.max_handbrake_torque = front ? 0.0f : 4000.0f;
                // Visual: el eje de la rueda es X; el cilindro (eje Y) va girado dentro.
                ecs::Entity visual = ecs::createEmpty(world_, wheel);
                visual.setName("Visual");
                ecs::Entity tire = ecs::createPrimitive(world_, assets::builtin::kCylinder, "Neumatico", visual);
                tire.setLocalEulerDegrees(Vec3{0.0f, 0.0f, 90.0f});
                tire.setLocalScale(Vec3{collider.radius * 2.0f, collider.width * 0.5f, collider.radius * 2.0f});
                collider.visual = visual.uuid();
            }
            break;
        }
        default: created = ecs::createEmpty(world_, parent); break;
    }
    // Objetos 3D con su collider, como Unity.
    if (created.valid() && ((kind >= 1 && kind <= 5) || kind == 13 || kind == 14)) {
        physics::addDefaultCollider(created);
    }
    // Sin padre: delante de la camara del editor, como Unity.
    if (created.valid() && !parent.valid() && kind != 6 && (kind < 10 || kind >= 13)) {
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
        ecs::detachCopiedLinks(world_, copy);
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
        ecs::detachCopiedLinks(world_, pasted);
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
    const CpuClock::time_point ui_start = CpuClock::now();
    frame_delta_ = delta_seconds;
    ImGuizmo::BeginFrame();
    imgui_.updateThumbnails();
    pollImports();
    if (has_project_ && thumbnail_countdown_ > 0 && --thumbnail_countdown_ == 0) saveProjectThumbnail();
    updateFloatingOrigin();
    // Material pulsado en el navegador: al Inspector solo si se solto sin
    // arrastrar (arrastrarlo a la Jerarquia o al Inspector no cambia nada).
    if (pending_inspect_material_.valid() && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        const ImGuiIO& io = ImGui::GetIO();
        if (io.MouseDragMaxDistanceSqr[ImGuiMouseButton_Left] < io.MouseDragThreshold * io.MouseDragThreshold) {
            inspected_material_ = pending_inspect_material_;
        }
        pending_inspect_material_ = {};
    }
    profiler_overlay_.update(delta_seconds, renderer_);
    watchAssets();
    pollMcp();
    runSelfTestStep();
    frame_tasks_ms_ += millisecondsSince(ui_start);

    if (!has_project_) {
        flying_ = false;
        drawHub();
        drawModals();
        return;
    }

    // La fisica va ANTES que la interfaz: los gizmos, el Inspector y el
    // render ven los objetos donde estan en este frame (si fuera despues, los
    // gizmos irian un frame por detras de lo que se mueve).
    {
        const CpuClock::time_point physics_start = CpuClock::now();
        updatePhysics(delta_seconds);
        updateNavigation(delta_seconds);
        updateVoxels(delta_seconds);
        if (scripts_.cursorLocked() && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) scripts_.releaseCursor();
        if (quit_play_requested_) {  // Game.quit() en Play
            quit_play_requested_ = false;
            exitPlay();
        }
        // Camaras de cine despues de la fisica (pueden seguir a un cuerpo).
        updateCinematics(delta_seconds);
        addCpuSample(kCpuPhysics, millisecondsSince(physics_start));
    }

    drawMenuBar();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImGuiID dockspace_id = ImGui::GetID("CramionDockspace");
    if (reset_layout_ || ImGui::DockBuilderGetNode(dockspace_id) == nullptr) {
        buildDefaultLayout(dockspace_id);
        reset_layout_ = false;
    }
    ImGui::DockSpaceOverViewport(dockspace_id, viewport);

    CpuClock::time_point t = CpuClock::now();
    drawSceneView();
    drawGameView();
    addCpuSample(kCpuScene, millisecondsSince(t));
    t = CpuClock::now();
    if (show_hierarchy_) drawHierarchy();
    addCpuSample(kCpuHierarchy, millisecondsSince(t));
    t = CpuClock::now();
    if (show_inspector_) drawInspector();
    addCpuSample(kCpuInspector, millisecondsSince(t));
    t = CpuClock::now();
    if (show_project_) drawProject();
    if (show_statistics_) drawStatistics(delta_seconds);
    if (show_console_) drawConsole();
    if (show_render_settings_) drawRenderSettings();
    if (show_animator_) drawAnimatorEditor();
    if (show_script_editor_ || !script_tabs_.empty()) drawScriptEditor();
    drawMcpWindow();
    drawExportProgress();
    drawImportProgress();
    if (show_physics_) drawPhysicsWindow();
    if (show_navigation_window_) drawNavigationWindow();
    if (show_cinematic_) drawCinematicWindow();
    drawModals();
    addCpuSample(kCpuPanels, millisecondsSince(t));

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
        // Play (Ctrl+P) y pausa (Ctrl+Mayus+P), como Unity.
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_P, false)) {
            if (io.KeyShift) {
                togglePause();
            } else if (playing()) {
                exitPlay();
            } else {
                enterPlay();
            }
        }
    }
    flushCommit();
    chooseRenderView();
    addCpuSample(kCpuUi, millisecondsSince(ui_start));
}

void EditorApp::reportFrame(float total_ms, float window_ms, float imgui_ms, float scene_update_ms) {
    const gfx::VulkanRenderer::FrameTimings r = renderer_.takeFrameTimings();
    const double now = ImGui::GetTime();
    // Frame lento (menos de ~7 FPS): se explica en la consola, como mucho una
    // vez por segundo, con lo que costo cada parte en ESE frame.
    if (total_ms > 150.0f && has_project_ && now - last_slow_report_ > 1.0) {
        last_slow_report_ = now;
        const auto ms = [](float v) { return std::to_string(static_cast<int>(v + 0.5f)); };
        const auto s = [&](int section) { return ms(frame_ms_[static_cast<std::size_t>(section)]); };
        std::string line = "[Rendimiento] Frame lento de " + ms(total_ms) + " ms: ";
        line += "tareas " + ms(frame_tasks_ms_);
        if (frame_refreshes_ > 0) line += " (refrescar Assets x" + std::to_string(frame_refreshes_) + " " + ms(frame_refresh_ms_) + ")";
        line += ", fisica " + s(kCpuPhysics) + ", vista escena " + s(kCpuScene) + ", jerarquia " + s(kCpuHierarchy) +
                ", inspector " + s(kCpuInspector) + ", paneles " + s(kCpuPanels) + ", sync " + s(kCpuSync);
        line += ", render " + s(kCpuRender) + " (espera GPU " + ms(r.fence_wait_ms) + ", actores " + ms(r.actors_ms) +
                ", sonda " + ms(r.probe_ms) + ", swapchain " + ms(r.acquire_ms) + ", uniformes " + ms(r.uniforms_ms) +
                ", comandos " + ms(r.record_ms) + ", envio " + ms(r.submit_ms);
        if (r.uploads > 0) line += ", SUBIR MODELOS x" + std::to_string(r.uploads) + " " + ms(r.upload_ms);
        // Los pases que mas tardaron en grabarse.
        std::vector<std::pair<std::string, float>> passes = r.passes;
        std::sort(passes.begin(), passes.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        for (std::size_t i = 0; i < passes.size() && i < 3; ++i) {
            if (passes[i].second >= 1.0f) line += std::string(i == 0 ? "; pases: " : ", ") + passes[i].first + " " + ms(passes[i].second);
        }
        line += "), ventana " + ms(window_ms) + ", imgui " + ms(imgui_ms) + ", camara " + ms(scene_update_ms);
        line += " | GPU " + ms(renderer_.gpuProfiler().totalMilliseconds()) + " ms, " +
                std::to_string(scene_.actors().size()) + " actores, " + std::to_string(renderer_.triangleCount()) +
                " triangulos";
        std::cerr << line << std::endl;
    }
    frame_ms_.fill(0.0f);
    frame_tasks_ms_ = 0.0f;
    frame_refresh_ms_ = 0.0f;
    frame_refreshes_ = 0;
}

void EditorApp::syncWorld(float delta_seconds, bool secondary) {
    if (!has_project_ || !sync_) {
        return;
    }
    const std::uint32_t view = secondary ? (render_view_ == kGameSlot ? kSceneSlot : kGameSlot) : render_view_;
    // (La fisica ya se simulo al empezar el frame, en drawUi.)
    // La vista que se dibuja: Escena (camara del editor, con contorno y
    // gizmos) o Juego (camara real, sin ayudas). Al cambiar de vista o en un
    // corte de camara, sin historias temporales (no se mezclan dos camaras).
    const bool game = view == kGameSlot;
    renderer_.setViewSlot(view);
    renderer_.setEditorHelpersEnabled(!game);
    if (!secondary && (view != last_render_view_ || (game && cinematics_.cutThisFrame()))) {
        renderer_.invalidateHistory();
    }
    if (!secondary) last_render_view_ = view;
    if (game) saved_camera_ = scene_.camera();  // afterRender la devuelve

    const CpuClock::time_point t = CpuClock::now();
    ecs::RenderSync::Options options;
    options.apply_main_camera = game;
    // La segunda vista no avanza animaciones ni agua (ya lo hace la principal).
    sync_->sync(world_, scene_, renderer_, secondary ? 0.0f : delta_seconds, options);
    if (!secondary) voxels_.syncRenderer(renderer_);
    addCpuSample(kCpuSync, millisecondsSince(t));

    if (game) return;  // sin contorno ni gizmos (setEditorHelpersEnabled)
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
    ImGui::DockBuilderDockWindow("Física", bottom);
    ImGui::DockBuilderDockWindow("Escena", center);
    ImGui::DockBuilderDockWindow("Juego", center);
    ImGui::DockBuilderDockWindow("Cinemática", bottom);
    ImGui::DockBuilderDockWindow("Animator", center);
    ImGui::DockBuilderFinish(dockspace_id);
}

// -----------------------------------------------------------------------------
// Menus
// -----------------------------------------------------------------------------

void EditorApp::drawMenuBar() {
    // La barra de menus es tambien la barra de titulo: algo mas alta.
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, 7.0f));
    if (!ImGui::BeginMainMenuBar()) {
        ImGui::PopStyleVar();
        return;
    }
    caption_blockers_.clear();
    caption_height_ = ImGui::GetWindowHeight();
    drawCaptionLogo();
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
        ImGui::Separator();
        if (ImGui::MenuItem("Exportar juego...")) exportGame(false);
        if (ImGui::MenuItem("Exportar y jugar...")) exportGame(true);
        ImGui::Separator();
        if (ImGui::MenuItem("Guardar proyecto como plantilla...")) show_save_template_ = true;
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
        ImGui::Separator();
        if (ImGui::BeginMenu("Física")) {
            item("Cubo con Rigidbody", 13);
            item("Esfera con Rigidbody", 14);
            item("Zona trigger", 15);
            item("Vehículo (4 ruedas)", 17);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Efectos")) {
            item("Sistema de partículas", 16);
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Terreno")) createTerrainEntity();
        if (ImGui::MenuItem("Mundo de bloques")) createVoxelWorldEntity();
        if (ImGui::BeginMenu("Agua")) {
            if (ImGui::MenuItem("Océano / playa")) createWaterEntity(0);
            if (ImGui::MenuItem("Lago")) createWaterEntity(1);
            if (ImGui::MenuItem("Río")) createWaterEntity(2);
            ImGui::EndMenu();
        }
        drawNavigationCreateMenu();
        drawUiCreateMenu();
        if (ImGui::BeginMenu("Cinemática")) {
            if (ImGui::MenuItem("Cámara virtual (desde la vista)")) createCinematic(0);
            if (ImGui::MenuItem("Cámara que sigue a la selección")) createCinematic(1);
            if (ImGui::MenuItem("Riel (Dolly Track)")) createCinematic(2);
            if (ImGui::MenuItem("Cámara en riel (Dolly)")) createCinematic(3);
            if (ImGui::MenuItem("Carro en riel (Dolly Cart)")) createCinematic(4);
            ImGui::Separator();
            if (ImGui::MenuItem("Secuencia cinemática (Timeline)")) createCinematic(5);
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Ventana")) {
        ImGui::MenuItem("Jerarquía", nullptr, &show_hierarchy_);
        ImGui::MenuItem("Inspector", nullptr, &show_inspector_);
        ImGui::MenuItem("Proyecto", nullptr, &show_project_);
        ImGui::MenuItem("Estadísticas", nullptr, &show_statistics_);
        ImGui::MenuItem("Consola", nullptr, &show_console_);
        ImGui::MenuItem("Configuración gráfica", nullptr, &show_render_settings_);
        ImGui::MenuItem("Animator", nullptr, &show_animator_);
        ImGui::MenuItem("Scripts (Lua)", nullptr, &show_script_editor_);
        ImGui::MenuItem("MCP (IA)", nullptr, &show_mcp_);
        ImGui::MenuItem("Física", nullptr, &show_physics_);
        ImGui::MenuItem("Navegación", nullptr, &show_navigation_window_);
        ImGui::MenuItem("Juego", nullptr, &show_game_);
        ImGui::MenuItem("Cinemática", nullptr, &show_cinematic_);
        ImGui::Separator();
        if (ImGui::MenuItem("Restablecer diseño")) {
            reset_layout_ = true;
            show_hierarchy_ = show_inspector_ = show_project_ = show_statistics_ = show_console_ =
                show_render_settings_ = show_physics_ = show_game_ = show_cinematic_ = true;
        }
        ImGui::EndMenu();
    }
    // Ayuda: el manual y la comunidad en el navegador.
    if (ImGui::BeginMenu("Ayuda")) {
        const auto open = [](const wchar_t* url) { ShellExecuteW(nullptr, L"open", url, nullptr, nullptr, SW_SHOWNORMAL); };
        if (ImGui::MenuItem("Documentación", "F1")) open(L"https://cramion.mantraxtools.store/manual/index.html");
        if (ImGui::MenuItem("Scripting en Lua")) open(L"https://cramion.mantraxtools.store/manual/primer-script.html");
        if (ImGui::MenuItem("Shaders propios")) open(L"https://cramion.mantraxtools.store/manual/shaders.html");
        ImGui::Separator();
        if (ImGui::MenuItem("Web de Cramion")) open(L"https://cramion.mantraxtools.store");
        if (ImGui::MenuItem("Discord")) open(L"https://discord.gg/zG7rSsUGEz");
        ImGui::Separator();
        ImGui::MenuItem("Conectar una IA (MCP)...", nullptr, &show_mcp_);
        ImGui::EndMenu();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F1, false) && !ImGui::GetIO().WantTextInput) {
        ShellExecuteW(nullptr, L"open", L"https://cramion.mantraxtools.store/manual/index.html", nullptr, nullptr, SW_SHOWNORMAL);
    }

    // Hasta aqui (logo y menus) no se arrastra la ventana.
    caption_blockers_.push_back(ImVec4(0.0f, 0.0f, ImGui::GetCursorScreenPos().x, caption_height_));

    ImGui::BeginGroup();
    drawPlayControls();
    ImGui::EndGroup();
    caption_blockers_.push_back(ImVec4(ImGui::GetItemRectMin().x, 0.0f, ImGui::GetItemRectMax().x, caption_height_));

    char status[260];
    std::snprintf(status, sizeof(status), "%s - %s%s   |   %.0f FPS   |   GPU %.2f ms", project_.name.c_str(),
                  world_.sceneName().c_str(), dirty_ ? " *" : "", ImGui::GetIO().Framerate,
                  renderer_.gpuProfiler().totalMilliseconds());
    const float controls = 46.0f * 3.0f;
    ImGui::SameLine(ImGui::GetWindowWidth() - controls - ImGui::CalcTextSize(status).x - 16.0f);
    ImGui::TextDisabled("%s", status);
    drawWindowControls();
    ImGui::EndMainMenuBar();
    ImGui::PopStyleVar();
}

// Logo del motor al principio de la barra (como Unity/Unreal). Clic: el menu
// de la ventana de Windows (mover, tamano, cerrar).
void EditorApp::drawCaptionLogo() {
    const ImTextureID logo = imgui_.logo();
    if (logo == 0) return;
    const float size = ImGui::GetFrameHeight() - 6.0f;
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 3.0f);
    ImGui::Image(logo, ImVec2(size, size));
    if (ImGui::IsItemClicked()) {
        POINT p{static_cast<LONG>(ImGui::GetItemRectMin().x), static_cast<LONG>(ImGui::GetItemRectMax().y)};
        ClientToScreen(window_.handle(), &p);
        window_.showSystemMenu(p.x, p.y);
    }
    ImGui::SetItemTooltip("Cramion Engine");
    caption_blockers_.push_back(ImVec4(ImGui::GetItemRectMin().x, 0.0f, ImGui::GetItemRectMax().x, caption_height_));
}

// Minimizar, maximizar/restaurar y cerrar, dibujados como los de Windows 11.
void EditorApp::drawWindowControls() {
    const float width = 46.0f;
    const float height = ImGui::GetWindowHeight();
    const ImVec2 window_pos = ImGui::GetWindowPos();
    const float x0 = window_pos.x + ImGui::GetWindowWidth() - width * 3.0f;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->PushClipRect(ImVec2(x0, window_pos.y), ImVec2(x0 + width * 3.0f, window_pos.y + height), false);
    const ImU32 glyph = IM_COL32(225, 225, 230, 255);
    const float g = std::round(height * 0.17f);
    for (int i = 0; i < 3; ++i) {
        const ImVec2 min(x0 + width * static_cast<float>(i), window_pos.y);
        const ImVec2 max(min.x + width, min.y + height);
        ImGui::SetCursorScreenPos(min);
        ImGui::PushID(i);
        const bool pressed = ImGui::InvisibleButton("##window_control", ImVec2(width, height));
        const bool hovered = ImGui::IsItemHovered();
        const bool held = ImGui::IsItemActive();
        ImGui::PopID();
        if (hovered || held) {
            const ImU32 bg = i == 2 ? (held ? IM_COL32(200, 30, 40, 255) : IM_COL32(232, 17, 35, 255))
                                    : (held ? IM_COL32(255, 255, 255, 45) : IM_COL32(255, 255, 255, 28));
            draw->AddRectFilled(min, max, bg);
        }
        const ImVec2 c(std::round((min.x + max.x) * 0.5f) + 0.5f, std::round((min.y + max.y) * 0.5f) + 0.5f);
        if (i == 0) {
            draw->AddLine(ImVec2(c.x - g, c.y), ImVec2(c.x + g, c.y), glyph, 1.0f);
            ImGui::SetItemTooltip("Minimizar");
        } else if (i == 1) {
            if (window_.isMaximized()) {
                draw->AddRect(ImVec2(c.x - g, c.y - g + 2.0f), ImVec2(c.x + g - 2.0f, c.y + g), glyph, 0.0f, 0, 1.0f);
                draw->AddLine(ImVec2(c.x - g + 2.0f, c.y - g), ImVec2(c.x + g, c.y - g), glyph, 1.0f);
                draw->AddLine(ImVec2(c.x + g, c.y - g), ImVec2(c.x + g, c.y + g - 2.0f), glyph, 1.0f);
            } else {
                draw->AddRect(ImVec2(c.x - g, c.y - g), ImVec2(c.x + g, c.y + g), glyph, 0.0f, 0, 1.0f);
            }
            ImGui::SetItemTooltip(window_.isMaximized() ? "Restaurar" : "Maximizar");
        } else {
            draw->AddLine(ImVec2(c.x - g, c.y - g), ImVec2(c.x + g, c.y + g), glyph, 1.0f);
            draw->AddLine(ImVec2(c.x + g, c.y - g), ImVec2(c.x - g, c.y + g), glyph, 1.0f);
            ImGui::SetItemTooltip("Cerrar");
        }
        if (pressed) {
            if (i == 0) window_.minimize();
            if (i == 1) window_.toggleMaximize();
            if (i == 2) requestQuit();
        }
    }
    draw->PopClipRect();
    caption_blockers_.push_back(ImVec4(x0, 0.0f, x0 + width * 3.0f, height));
}

bool EditorApp::isCaptionDragArea(int x, int y) const {
    const float fx = static_cast<float>(x);
    const float fy = static_cast<float>(y);
    if (fy < 0.0f || fy >= caption_height_) return false;
    for (const ImVec4& r : caption_blockers_) {
        if (fx >= r.x && fx < r.z && fy >= r.y && fy < r.w) return false;
    }
    return true;
}

// En el Hub (sin menus): logo, titulo y los botones de la ventana.
void EditorApp::drawHubTitleBar() {
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, 7.0f));
    if (!ImGui::BeginMainMenuBar()) {
        ImGui::PopStyleVar();
        return;
    }
    caption_blockers_.clear();
    caption_height_ = ImGui::GetWindowHeight();
    drawCaptionLogo();
    ImGui::TextUnformatted("Cramion Hub");
    drawWindowControls();
    ImGui::EndMainMenuBar();
    ImGui::PopStyleVar();
}

// -----------------------------------------------------------------------------
// Dialogos modales
// -----------------------------------------------------------------------------

void EditorApp::drawModals() {
    drawSaveTemplateDialog();
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
    ImGui::Text("Con LOD: %llu triángulos de escenarios  |  %u objetos simplificados",
                static_cast<unsigned long long>(renderer_.lodTriangles()), renderer_.lodActors());
    ImGui::Text("Lotes de material (llamadas de escenario) %u  |  llamadas de sombras %u  |  modelos con materiales propios %zu",
                renderer_.batchCount(), renderer_.shadowDrawCalls(),
                sync_ ? sync_->variantCount() : std::size_t{0});
    // CPU por partes (media movil): donde se va el frame.
    static constexpr const char* kCpuNames[kCpuSectionCount] = {
        "Interfaz (total)", "  Jerarquía", "  Inspector", "  Escena + gizmos", "  Otros paneles",
        "Física + partículas", "Sync del mundo", "Render (CPU)"};
    if (ImGui::BeginTable("cpu", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("CPU", ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn("ms", ImGuiTableColumnFlags_WidthFixed, 56.0f);
        ImGui::TableHeadersRow();
        for (int i = 0; i < kCpuSectionCount; ++i) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(kCpuNames[i]);
            ImGui::TableNextColumn();
            ImGui::Text("%.2f", cpu_ms_[static_cast<std::size_t>(i)]);
        }
        ImGui::EndTable();
    }
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
    console_dock_id_ = ImGui::GetWindowDockID();
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
    drawGraphicsSettings();
    const auto toggle = [&](const char* label, bool value, void (gfx::VulkanRenderer::*setter)(bool)) {
        bool v = value;
        if (ImGui::Checkbox(label, &v)) {
            (r.*setter)(v);
            saveGraphicsSettings();
        }
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

// Escalado (TAA, FSR, DLSS), calidad, nitidez, vsync y calidades rapidas.
void EditorApp::drawGraphicsSettings() {
    gfx::VulkanRenderer& r = renderer_;
    gfx::GraphicsSettings g = r.graphicsSettings();
    bool changed = false;

    ImGui::SeparatorText("Calidad rápida");
    static constexpr const char* kPresets[] = {"Baja", "Media", "Alta", "Ultra"};
    const float preset_width = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 3.0f) / 4.0f;
    for (int i = 0; i < 4; ++i) {
        if (i > 0) ImGui::SameLine();
        if (ImGui::Button(kPresets[i], ImVec2(preset_width, 0.0f))) {
            // Baja/Media: escalado a menos resolucion; Alta/Ultra: nativa con TAA.
            static constexpr gfx::UpscaleQuality kQuality[] = {gfx::UpscaleQuality::Performance,
                                                               gfx::UpscaleQuality::Balanced,
                                                               gfx::UpscaleQuality::Quality,
                                                               gfx::UpscaleQuality::Native};
            if (g.upscaler == gfx::Upscaler::Off) g.upscaler = gfx::Upscaler::Taa;
            g.quality = kQuality[i];
            g.sharpness = i < 2 ? 0.45f : 0.25f;
            r.setShadowsEnabled(true);
            r.setReflectionProbeEnabled(i >= 1);
            r.setOcclusionCullingEnabled(true);
            if (r.rayTracingSupported()) r.setRayTracingEnabled(i == 3);
            changed = true;
        }
    }

    ImGui::SeparatorText("Escalado y antialiasing");
    static constexpr const char* kUpscalers[] = {"Desactivado (nativa + FXAA)", "TAA (temporal, escala como TAAU)",
                                                 "AMD FSR 1", "AMD FSR 3 (fase 2)", "NVIDIA DLSS (fase 2)"};
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo("##upscaler", kUpscalers[static_cast<int>(g.upscaler)])) {
        for (int i = 0; i < 5; ++i) {
            // FSR 3 y DLSS llegan con sus SDK (siguiente fase).
            const bool available = i <= 2;
            if (ImGui::Selectable(kUpscalers[i], static_cast<int>(g.upscaler) == i,
                                  available ? 0 : ImGuiSelectableFlags_Disabled)) {
                g.upscaler = static_cast<gfx::Upscaler>(i);
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::BeginDisabled(g.upscaler == gfx::Upscaler::Off);
    static constexpr const char* kQualities[] = {"Nativa (100 %, DLAA)", "Calidad (67 %)", "Equilibrado (58 %)",
                                                 "Rendimiento (50 %)", "Ultra rendimiento (33 %)", "Personalizada"};
    int quality = static_cast<int>(g.quality);
    ImGui::SetNextItemWidth(-90.0f);
    if (ImGui::Combo("Resolución", &quality, kQualities, 6)) {
        g.quality = static_cast<gfx::UpscaleQuality>(quality);
        changed = true;
    }
    if (g.quality == gfx::UpscaleQuality::Custom) {
        float percent = g.custom_scale * 100.0f;
        ImGui::SetNextItemWidth(-90.0f);
        if (ImGui::SliderFloat("Escala", &percent, 25.0f, 100.0f, "%.0f %%")) {
            g.custom_scale = percent / 100.0f;
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
    }
    float sharpness = g.sharpness;
    ImGui::SetNextItemWidth(-90.0f);
    if (ImGui::SliderFloat("Nitidez", &sharpness, 0.0f, 1.0f, "%.2f")) {
        g.sharpness = sharpness;
        changed = true;
    }
    ImGui::SetItemTooltip("AMD RCAS tras el escalado (0 = sin nitidez).");
    ImGui::EndDisabled();
    const vk::Extent2D render = r.renderExtent();
    const vk::Extent2D output = r.sceneExtent();
    ImGui::TextDisabled("Interna %ux%u  ->  pantalla %ux%u", render.width, render.height, output.width, output.height);

    ImGui::BeginDisabled(true);
    bool frame_generation = g.frame_generation;
    ImGui::Checkbox("Generación de frames (DLSS 3 / FSR 3, fase 3)", &frame_generation);
    ImGui::EndDisabled();

    ImGui::SeparatorText("Presupuesto adaptativo");
    {
        const gfx::HardwareProfile& hw = r.hardwareProfile();
        ImGui::TextDisabled("Perfil: %s  |  %s, %llu MB de VRAM%s", gfx::tierName(hw.tier), hw.gpu_name.c_str(),
                            static_cast<unsigned long long>(hw.vram_mb), hw.integrated ? " (integrada)" : "");
        if (ImGui::Checkbox("Optimización adaptativa", &g.adaptive)) changed = true;
        ImGui::SetItemTooltip(
            "Mide cuánto tarda cada pasada de la GPU y, si no llega a los FPS objetivo, baja solo lo que más\n"
            "ahorra y menos se nota (LODs, detalle de sombras, efectos, resolución con FSR 1). Aprende lo que\n"
            "cuesta cada cosa en este PC y vuelve a subir la calidad cuando sobra tiempo.");
        ImGui::BeginDisabled(!g.adaptive);
        static constexpr int kTargets[] = {30, 45, 60, 75, 90, 120, 144, 165, 240};
        char preview[16];
        std::snprintf(preview, sizeof(preview), "%.0f FPS", g.target_fps);
        ImGui::SetNextItemWidth(-90.0f);
        if (ImGui::BeginCombo("Objetivo", preview)) {
            for (const int fps : kTargets) {
                char label[16];
                std::snprintf(label, sizeof(label), "%d FPS", fps);
                if (ImGui::Selectable(label, static_cast<int>(g.target_fps) == fps)) {
                    g.target_fps = static_cast<float>(fps);
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();

        static constexpr int kShadowSizes[] = {0, 1024, 1536, 2048, 3072, 4096, 6144};
        char shadow_preview[48];
        if (g.shadow_resolution <= 0) {
            std::snprintf(shadow_preview, sizeof(shadow_preview), "Auto (%u)", gfx::shadowResolutionFor(hw.tier));
        } else {
            std::snprintf(shadow_preview, sizeof(shadow_preview), "%d", g.shadow_resolution);
        }
        ImGui::SetNextItemWidth(-90.0f);
        if (ImGui::BeginCombo("Sombras", shadow_preview)) {
            for (const int size : kShadowSizes) {
                char label[48];
                if (size == 0) {
                    std::snprintf(label, sizeof(label), "Auto (%u, según el perfil)", gfx::shadowResolutionFor(hw.tier));
                } else {
                    std::snprintf(label, sizeof(label), "%d  (%.0f MB)", size,
                                  4.0 * size * size * 4.0 / (1024.0 * 1024.0));
                }
                if (ImGui::Selectable(label, g.shadow_resolution == size)) {
                    g.shadow_resolution = size;
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("Resolución por cascada del mapa de sombras del sol (4 cascadas).");

        static constexpr int kTextureSizes[] = {0, 1024, 2048, 4096, 8192, 16384};
        const std::uint32_t auto_textures = gfx::textureSizeFor(hw.tier);
        char texture_preview[48];
        if (g.texture_max_size <= 0) {
            if (auto_textures == 0) std::snprintf(texture_preview, sizeof(texture_preview), "Auto (sin límite)");
            else std::snprintf(texture_preview, sizeof(texture_preview), "Auto (%u)", auto_textures);
        } else {
            std::snprintf(texture_preview, sizeof(texture_preview), "%d", g.texture_max_size);
        }
        ImGui::SetNextItemWidth(-90.0f);
        if (ImGui::BeginCombo("Texturas", texture_preview)) {
            for (const int size : kTextureSizes) {
                char label[64];
                if (size == 0) std::snprintf(label, sizeof(label), "Auto (según el perfil)");
                else std::snprintf(label, sizeof(label), "Máximo %d  (%.0f MB por textura)", size,
                                   size * static_cast<double>(size) * 4.0 * 1.33 / (1024.0 * 1024.0));
                if (ImGui::Selectable(label, g.texture_max_size == size)) {
                    g.texture_max_size = size;
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("Lado máximo de las texturas de los modelos. Una de 16K sin comprimir ocupa 1,3 GB de VRAM.\n"
                              "Se aplica a los modelos que se cargan después (reabre la escena).");
        std::uint64_t vram_used = 0;
        std::uint64_t vram_budget = 0;
        if (r.device().videoMemory(vram_used, vram_budget)) {
            const float fraction = static_cast<float>(vram_used) / static_cast<float>(std::max<std::uint64_t>(vram_budget, 1));
            char vram_text[64];
            std::snprintf(vram_text, sizeof(vram_text), "VRAM %.2f / %.2f GB", vram_used / 1073741824.0,
                          vram_budget / 1073741824.0);
            if (fraction > 0.9f) ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.9f, 0.3f, 0.25f, 1.0f));
            ImGui::ProgressBar(std::min(fraction, 1.0f), ImVec2(-1.0f, 0.0f), vram_text);
            if (fraction > 0.9f) {
                ImGui::PopStyleColor();
                ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f),
                                   "Casi sin VRAM: la GPU usará la RAM y habrá caídas de FPS. Baja las texturas.");
            }
        }

        const gfx::FrameBudget& budget = r.frameBudget();
        if (g.adaptive) {
            const float gpu = budget.smoothedGpuMs();
            const float target = budget.targetMilliseconds();
            char overlay[64];
            std::snprintf(overlay, sizeof(overlay), "GPU %.1f ms / %.1f ms", gpu, target);
            ImGui::ProgressBar(std::min(gpu / std::max(target, 0.1f), 1.0f), ImVec2(-1.0f, 0.0f), overlay);
            if (ImGui::BeginTable("levers", 2, ImGuiTableFlags_SizingStretchProp)) {
                for (std::size_t i = 0; i < gfx::kLeverCount; ++i) {
                    const auto lever = static_cast<gfx::Lever>(i);
                    const int level = budget.level(lever);
                    const int max = budget.maxLevel(lever);
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(gfx::FrameBudget::leverName(lever));
                    ImGui::TableNextColumn();
                    if (level == 0) {
                        ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1.0f), "máxima");
                    } else {
                        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "-%d de %d", level, max);
                    }
                }
                ImGui::EndTable();
            }
            if (!budget.lastAction().empty()) ImGui::TextDisabled("Último cambio: %s", budget.lastAction().c_str());
        }
        ImGui::TextDisabled("Mapa de sombras en uso: %u x %u x 4 cascadas", r.shadowResolution(), r.shadowResolution());
    }

    ImGui::SeparatorText("Presentación");
    if (ImGui::Checkbox("VSync", &g.vsync)) changed = true;
    ImGui::SetItemTooltip("Sin VSync: mailbox (sin cortes de imagen, sin tope de FPS).");
    ImGui::TextDisabled("FXAA: componente PostProcessing (con TAA/FSR sobra).");

    if (changed) {
        r.setGraphicsSettings(g);
        saveGraphicsSettings();
    }
}

// Formato: clave=valor por linea (sin dependencias).
void EditorApp::saveGraphicsSettings() const {
    if (!has_project_) return;
    const gfx::GraphicsSettings& g = renderer_.graphicsSettings();
    std::ofstream out(project_.settingsFolder() / "Graphics.ini", std::ios::trunc);
    if (!out) return;
    out << "upscaler=" << static_cast<int>(g.upscaler) << "\n";
    out << "quality=" << static_cast<int>(g.quality) << "\n";
    out << "custom_scale=" << g.custom_scale << "\n";
    out << "sharpness=" << g.sharpness << "\n";
    out << "vsync=" << (g.vsync ? 1 : 0) << "\n";
    out << "adaptive=" << (g.adaptive ? 1 : 0) << "\n";
    out << "target_fps=" << g.target_fps << "\n";
    out << "shadow_resolution=" << g.shadow_resolution << "\n";
    out << "texture_max_size=" << g.texture_max_size << "\n";
    out << "shadows=" << (renderer_.shadowsEnabled() ? 1 : 0) << "\n";
    out << "ray_tracing=" << (renderer_.rayTracingEnabled() ? 1 : 0) << "\n";
    out << "reflection_probe=" << (renderer_.reflectionProbeEnabled() ? 1 : 0) << "\n";
    out << "occlusion_culling=" << (renderer_.occlusionCullingEnabled() ? 1 : 0) << "\n";
}

void EditorApp::loadGraphicsSettings() {
    std::ifstream in(project_.settingsFolder() / "Graphics.ini");
    if (!in) return;
    gfx::GraphicsSettings g = renderer_.graphicsSettings();
    std::string line;
    while (std::getline(in, line)) {
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq);
        const float value = std::strtof(line.c_str() + eq + 1, nullptr);
        if (key == "upscaler") g.upscaler = static_cast<gfx::Upscaler>(std::clamp(static_cast<int>(value), 0, 2));
        if (key == "quality") g.quality = static_cast<gfx::UpscaleQuality>(std::clamp(static_cast<int>(value), 0, 5));
        if (key == "custom_scale") g.custom_scale = std::clamp(value, 0.25f, 1.0f);
        if (key == "sharpness") g.sharpness = std::clamp(value, 0.0f, 1.0f);
        if (key == "vsync") g.vsync = value != 0.0f;
        if (key == "adaptive") g.adaptive = value != 0.0f;
        if (key == "target_fps") g.target_fps = std::clamp(value, 15.0f, 360.0f);
        if (key == "shadow_resolution") g.shadow_resolution = std::clamp(static_cast<int>(value), 0, 8192);
        if (key == "texture_max_size") g.texture_max_size = std::clamp(static_cast<int>(value), 0, 16384);
        if (key == "shadows") renderer_.setShadowsEnabled(value != 0.0f);
        if (key == "ray_tracing" && renderer_.rayTracingSupported()) renderer_.setRayTracingEnabled(value != 0.0f);
        if (key == "reflection_probe") renderer_.setReflectionProbeEnabled(value != 0.0f);
        if (key == "occlusion_culling") renderer_.setOcclusionCullingEnabled(value != 0.0f);
    }
    renderer_.setGraphicsSettings(g);
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
        // Carpeta: toda su estructura, con lo que tenga dentro.
        std::error_code dir_error;
        if (std::filesystem::is_directory(file, dir_error)) {
            importFolder(file, folder / file.filename());
            continue;
        }
        // Imagenes (texturas de decals): se copian tal cual a Assets/.
        std::string import_ext = file.extension().string();
        std::transform(import_ext.begin(), import_ext.end(), import_ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (isDecalImage(file) || import_ext == ".lua" || audio::isAudioFile(file)) {
            std::error_code error;
            std::filesystem::path target = folder / file.filename();
            if (!std::filesystem::exists(target)) std::filesystem::copy_file(file, target, error);
            if (error) std::cerr << "[Editor] No se pudo copiar " << dialogs::utf8(file.filename()) << "\n";
            else std::cout << "[Editor] Imagen copiada: " << dialogs::utf8(file.filename()) << "\n";
            refreshDatabase();
            continue;
        }
        std::cout << "[Editor] En cola para importar: " << dialogs::utf8(file.filename()) << "\n";
        imports_.push_back(ImportJob{file, folder, std::make_shared<assets::ImportProgress>(), {}});
        ++imports_total_;
    }
    pollImports();  // arranca los primeros sin esperar al siguiente frame
}

void EditorApp::pollImports() {
    bool finished = false;
    for (auto it = imports_.begin(); it != imports_.end();) {
        if (!it->result.valid()) {
            ++it;  // en cola
        } else if (it->result.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            const assets::ImportResult result = it->result.get();
            if (it->reimport.valid()) {
                finishReimport(*it, result);
                if (!result.ok) ++imports_failed_;
            } else if (result.ok) {
                std::cout << "[Editor] Importado: " << result.info.name << " ("
                          << assets::assetTypeName(result.info.type) << ")\n";
            } else {
                std::cerr << "[Editor] Error al importar " << dialogs::utf8(it->source.filename())
                          << ": " << result.message << "\n";
                ++imports_failed_;
            }
            ++imports_done_;
            it = imports_.erase(it);
            finished = true;
        } else {
            ++it;
        }
    }
    if (finished && database_) {
        refreshDatabase();
    }

    // Arranca los que esperan en la cola, en orden, hasta el tope.
    std::size_t running = 0;
    for (const ImportJob& job : imports_) running += job.result.valid() ? 1 : 0;
    for (ImportJob& job : imports_) {
        if (running >= kMaxParallelImports) break;
        if (job.result.valid()) continue;
        std::cout << "[Editor] Importando " << dialogs::utf8(job.source.filename()) << "...\n";
        if (job.reimport.valid()) {
            const std::optional<assets::AssetInfo> info = database_ ? database_->find(job.reimport) : std::nullopt;
            job.result = std::async(std::launch::async, [info, progress = job.progress] {
                if (!info) return assets::ImportResult{false, {}, "[Assets] El modelo ya no existe"};
                return assets::reimportModel(*info, assets::modelImportSettings(info->path), progress.get());
            });
        } else {
            job.result = std::async(std::launch::async, [file = job.source, folder = job.folder,
                                                         progress = job.progress] {
                return assets::importAny(file, folder, progress.get());
            });
        }
        ++running;
    }
    if (imports_.empty()) {
        if (imports_failed_ > 0) {
            std::cerr << "[Editor] Importacion terminada: " << imports_failed_ << " de " << imports_total_
                      << " archivo(s) fallaron\n";
        }
        imports_total_ = imports_done_ = imports_failed_ = 0;
    }
}

float EditorApp::importFraction() const {
    float in_flight = 0.0f;
    for (const ImportJob& job : imports_) {
        if (job.result.valid()) in_flight += job.progress->fraction();
    }
    const float total = static_cast<float>(std::max<std::size_t>(imports_total_, 1));
    return std::min((static_cast<float>(imports_done_) + in_flight) / total, 1.0f);
}

void EditorApp::drawImportProgress() {
    if (imports_.empty()) return;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float margin = 16.0f;
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - margin,
                                   viewport->WorkPos.y + viewport->WorkSize.y - margin),
                            ImGuiCond_Always, ImVec2(1.0f, 1.0f));
    ImGui::SetNextWindowSize(ImVec2(420.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.94f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                   ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoNav;
    if (!ImGui::Begin("##import_progress", nullptr, flags)) {
        ImGui::End();
        return;
    }

    const float overall = importFraction();
    const std::size_t current = std::min(imports_done_ + 1, imports_total_);
    ImGui::TextColored(ImVec4(0.55f, 0.75f, 1.0f, 1.0f), "Importando archivo %zu de %zu", current, imports_total_);
    char overlay[48];
    std::snprintf(overlay, sizeof(overlay), "%.0f %%", overall * 100.0f);
    ImGui::ProgressBar(overall, ImVec2(-1.0f, 0.0f), overlay);

    std::size_t queued = 0;
    for (const ImportJob& job : imports_) {
        if (!job.result.valid()) {
            ++queued;  // los de la cola van resumidos abajo
            continue;
        }
        ImGui::Separator();
        const std::string name = dialogs::utf8(job.source.filename());
        ImGui::TextUnformatted(name.c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", dialogs::utf8(job.source).c_str());
        const float fraction = job.progress->fraction();
        std::snprintf(overlay, sizeof(overlay), "%.0f %%", fraction * 100.0f);
        ImGui::ProgressBar(fraction, ImVec2(-1.0f, ImGui::GetTextLineHeight() + 4.0f), overlay);
        ImGui::TextDisabled("%s", job.progress->stage().c_str());
    }
    if (queued > 0) {
        ImGui::Separator();
        ImGui::TextDisabled("%zu en cola:", queued);
        std::size_t shown = 0;
        for (const ImportJob& job : imports_) {
            if (job.result.valid()) continue;
            if (shown == 4) {
                ImGui::TextDisabled("  ... y %zu mas", queued - shown);
                break;
            }
            ImGui::TextDisabled("  %s", dialogs::utf8(job.source.filename()).c_str());
            ++shown;
        }
    }
    if (imports_failed_ > 0) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "%zu fallaron (ver consola)", imports_failed_);
    }
    ImGui::End();
}

void EditorApp::importFolder(const std::filesystem::path& source, const std::filesystem::path& target) {
    std::error_code error;
    // No meter Assets dentro de si mismo.
    const std::filesystem::path assets = std::filesystem::weakly_canonical(project_.assetsFolder(), error);
    const std::filesystem::path from = std::filesystem::weakly_canonical(source, error);
    if (!error && std::mismatch(from.begin(), from.end(), assets.begin(), assets.end()).first == from.end()) {
        std::cerr << "[Editor] No se puede importar una carpeta que contiene Assets\n";
        return;
    }
    std::filesystem::create_directories(target, error);
    std::cout << "[Editor] Importando la carpeta " << dialogs::utf8(source.filename()) << "...\n";
    static const std::array<std::string, 6> kImportable = {".obj", ".fbx", ".gltf", ".glb", ".dae", ".hdr"};
    std::size_t copied = 0;
    for (auto it = std::filesystem::recursive_directory_iterator(
             source, std::filesystem::directory_options::skip_permission_denied, error);
         !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
        const std::filesystem::path relative = std::filesystem::relative(it->path(), source, error);
        const std::filesystem::path destination = target / relative;
        if (it->is_directory(error)) {
            std::filesystem::create_directories(destination, error);
            continue;
        }
        if (!it->is_regular_file(error)) continue;
        std::string ext = dialogs::utf8(it->path().extension());
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (std::find(kImportable.begin(), kImportable.end(), ext) != kImportable.end()) {
            startImport({it->path()}, destination.parent_path());
        } else if (!std::filesystem::exists(destination)) {
            // Lo demas (imagenes, materiales, sonidos...) se copia tal cual.
            std::filesystem::copy_file(it->path(), destination, error);
            if (!error) ++copied;
            error.clear();
        }
    }
    if (copied > 0) std::cout << "[Editor] " << copied << " archivo(s) copiados\n";
    refreshDatabase();
}

std::filesystem::path EditorApp::createFolderIn(const std::filesystem::path& parent) {
    std::filesystem::path folder = parent / "Nueva carpeta";
    for (int i = 2; std::filesystem::exists(folder); ++i) {
        folder = parent / ("Nueva carpeta " + std::to_string(i));
    }
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    refreshDatabase();
    // Directamente a ponerle nombre (como Unity).
    renaming_folder_ = folder;
    folder_rename_buffer_ = dialogs::utf8(folder.filename());
    return folder;
}

void EditorApp::onFilesDropped(const std::vector<std::filesystem::path>& files, float x, float y) {
    if (has_project_) {
        // Encima de una carpeta del navegador: dentro de ella.
        std::filesystem::path target = current_folder_.empty() ? project_.assetsFolder() : current_folder_;
        for (const FolderDropZone& zone : folder_drop_zones_) {
            if (x >= zone.x0 && x <= zone.x1 && y >= zone.y0 && y <= zone.y1) target = zone.path;
        }
        startImport(files, target);
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
