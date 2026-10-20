#include "EditorApp.h"
#include "LoadingScreen.h"

#include <CramionCore/gameplay/Accessibility.h>

#include <CramionCore/physics/Cloth.h>
#include <CramionCore/physics/SoftBody.h>
#include "GraphicsConfig.h"
#include "UiRenderer.h"

#include "Dialogs.h"
#include "EditorLog.h"
#include "EditorBackground.h"

#include "CramionCore/project/DataPack.h"
#include "CramionCore/cvar/CVar.h"
#include "CramionCore/jobs/JobSystem.h"
#include "CramionCore/profiling/Profiler.h"
#include "CramionCore/net/WebSocket.h"
#include <CramionFX/asset/TextureCompression.h>

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
#include <exception>
#include <iostream>
#include <iterator>

namespace cramion::editor {

using core::Vec3;

namespace {

// Tope del historial de deshacer (instantanea por entidad + pasos). Un paso
// solo lleva lo que cambio: caben muchos mas que con el mundo entero por paso.
constexpr std::size_t kUndoMemoryLimit = 256ull * 1024 * 1024;
constexpr std::size_t kUndoMaxSteps = 500;

using CpuClock = std::chrono::steady_clock;

float millisecondsSince(CpuClock::time_point start) {
    return std::chrono::duration<float, std::milli>(CpuClock::now() - start).count();
}

std::wstring widen(const std::string& text) {
    return dialogs::fromUtf8(text).wstring();
}

// Informe de frames lentos en la consola: desde cuantos ms (25 para medir
// tirones del streaming; 150 = solo los parones de verdad).
cvar::CVar<float> g_slow_frame_ms("perf.SlowFrameMs", 150.0f,
                                  "Frames mas lentos que esto (ms) se explican en la consola, parte por parte",
                                  cvar::Saved, 5.0f, 10000.0f);

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
    foliage::registerFoliageComponents();
    fire::registerFireComponents();
    fluid::registerFluidComponents();
    fluid::setActiveSystem(&fluids_);  // la API (Fluid.*) y las consultas
    navigation::registerNavigationComponents();
    voxel::registerVoxelComponents();
    audio::registerAudioComponents();
    video::registerVideoComponents();  // VideoPlayer (EditorVideo.cpp)
    scripting::registerScriptComponents();
    ui::registerUiComponents();
    xr::registerXrComponents();
    startMcp();  // servidor MCP para IA (solo este PC)
    initCrashReporting();  // version y GPU para los informes; el del cierre anterior
    worldpart::registerWorldPartitionComponents();
    worldpart::setActivePartition(&world_partition_);
    ai::setActiveCrowds(&crowds_);
    crowds_.setNavigation(&nav_);
    crowds_.setPrefabResolver([this](const Uuid& uuid) -> std::filesystem::path {
        if (!database_) return {};
        const auto info = database_->find(uuid);
        if (!info) return {};
            return info->path.is_absolute() ? info->path : database_->root() / info->path;
    });
    registerEditorSettings();  // antes del primer frame: ImGui lee su .ini ahi
    physics_.addListener([this](const physics::PhysicsEvent& event) { onPhysicsEvent(event); });
    const std::filesystem::path documents = dialogs::documentsFolder();
    new_project_folder_ = dialogs::utf8(documents.empty() ? std::filesystem::current_path()
                                                          : documents / "Cramion Projects");
    updateTitle();
    // GPU perdida: drawFrame llama a esto (una vez, en este hilo) antes de
    // relanzar. La escena se guarda ya, antes de que el desenrollado toque
    // nada mas (el destructor lo repite: autosaveEmergency va una sola vez).
    gfx::VulkanDevice::setDeviceLostHandler(
        [this](const gfx::VulkanDevice::DeviceLostReport&) { autosave_.writeNow(); });
}

EditorApp::~EditorApp() {
    gfx::VulkanDevice::setDeviceLostHandler({});  // (capturaba this)
    waitCppSourceCheck();  // la tarea de disco lee cpp_scripts_
    // Salida por una excepcion (ErrorDeviceLost en el render, por ejemplo):
    // la escena se autoguarda antes de soltar nada. En un cierre normal ya se
    // decidio (guardar o no guardar): sin emergencia y sin autoguardado.
    if (std::uncaught_exceptions() > 0) {
        autosave_.writeNow();
    } else {
        autosave_.disarm();
        if (!dirty_) discardSceneAutosave();
    }
    // Cerrar en Play: los scripts terminan (OnDestroy) mientras todo lo que usan
    // sigue vivo (el mundo de bloques se destruye antes que ellos).
    scripts_.stop();
    // Video antes que el audio (suelta sus voces de stream) y que la GPU.
    video_.stopPreview();
    video_.stop();
    stopVideoImport();
    video_url_test_.reset();
    scripts_.setVideo(nullptr);  // scripts_ se destruye despues que video_
    setUiRenderTextureResolver({});  // la funcion global captura this
    // WebSocket: cierre limpio (1001) de lo que quede, como mucho 500 ms.
    net::shutdownWebSockets(500);
    // Los scripts de C++ se desenganchan de la fisica mientras sigue viva (se
    // declaran antes que physics_ y se destruirian despues: crash al cerrar).
    cpp_scripts_.stop();
    cpp_scripts_.setPhysics(nullptr);
    scripts_.unmountDataPacks();  // cerrar en Play: lo montado sale del proyecto
    if (datapack_job_ && datapack_job_->thread.joinable()) datapack_job_->thread.join();
    package_manager_.reset();  // cancela y espera descargas/extracciones
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

bool EditorApp::openProject(const std::filesystem::path& path, bool open_scene) {
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
    // Texturas comprimidas a BC7 (como Unity): una vez por imagen, aqui.
    asset::setTextureCacheFolder(project_.libraryFolder() / "Cache" / "Textures");
    model_previews_.start(project_.libraryFolder() / "Thumbnails");
    scripts_.setAssetsRoot(project_.assetsFolder());
    // Visual Script en Play: el grafo ilumina los cables por los que pasa y ensena los valores de los pines.
    scripts_.setVisualScriptLiveDebug(true);
    // DataPacks montados en Play: se apuntan en Library y se quitan al parar;
    // si el editor se cerro en Play, se quitan ahora (el proyecto queda limpio).
    project::cleanupDataPackJournal(project_.libraryFolder() / "DataPacks.journal", project_.assetsFolder());
    scripts_.setDataPackJournal(project_.libraryFolder() / "DataPacks.journal");
    scripts_.setAssetsChangedCallback([this] { refreshDatabase(); });
    scripts_.setPrefsFile(project_.libraryFolder() / "Prefs.txt");  // Prefs en Play
    scripts_.setSaveFolder(project_.libraryFolder() / "Saves");     // Save.save en Play (EditorGameplay.cpp)
    loadLocalization();
    scripts_.setPhysics(&physics_);
    setupCppScripts();
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
    scripts_.setTouchControls(&touch_game_);
    {
        scripting::ScriptSystem::ScreenHost screen;  // la vista Juego (sin orientacion en PC)
        screen.width = [this] { return static_cast<int>(game_image_rect_[2]); };
        screen.height = [this] { return static_cast<int>(game_image_rect_[3]); };
        scripts_.setScreen(screen);
    }
    scripts_.setXr(&renderer_.xr(), &xr_rig_);
    scripts_.setUi(&ui_);  // UI.pointerOverUi, Entity:uiScreenRect...
    audio_.setAssetsRoot(project_.assetsFolder());
    // Oclusion: paredes (colliders) entre el sonido y el oyente.
    audio_.setOcclusionQuery(audio::physicsOcclusionQuery(physics_));
    // Mundos de bloques: texturas en Assets, partidas guardadas en Library.
    voxels_.setAssetsRoot(project_.assetsFolder());
    voxels_.setSaveRoot(project_.libraryFolder() / "Worlds");
    voxels_.setPhysics(&physics_);  // los Rigidbody chocan con los bloques
    loadGraphicsSettings();
    loadViewSettings();
    // Graphics (la API): el editor no cambia su ventana ni guarda desde Play;
    // lo que cambien los scripts se deshace al parar (exitPlay).
    if (!graphics_host_) {
        graphics_host_ = std::make_unique<RendererGraphicsHost>(renderer_, nullptr, [](std::string& error) {
            error = "en el editor no se guarda (el juego exportado si, para el jugador)";
            return false;
        });
    }
    scripts_.setGraphics(graphics_host_.get());
    sync_ = std::make_unique<ecs::RenderSync>(*asset_manager_);
    // Video: sale al renderizador por RenderSync (Render Texture, Material
    // Override, planos de camara) y suena por el AudioSystem.
    video_output_ = video::makeRenderVideoOutput(*sync_, renderer_);
    {
        video::VideoSystem::Host host;
        host.audio = &audio_;
        host.output = video_output_.get();
        host.assets_root = project_.assetsFolder();
        video_.setHost(host);
    }
    scripts_.setVideo(&video_);
    // Una .crrt (camara o video) en una UI Image: su id y tamano en el renderizador.
    setUiRenderTextureResolver([this](const std::string& relative, ImVec2* size) -> std::int32_t {
        if (!sync_) return -1;
        const std::int32_t id = sync_->renderTextureIdForAsset(relative);
        if (id >= 0 && size != nullptr) {
            const vk::Extent2D extent = renderer_.renderTextureExtent(id);
            *size = ImVec2(static_cast<float>(extent.width), static_cast<float>(extent.height));
        }
        return id;
    });
    // Scripts: huesos, IK y ragdoll leen la pose de cada frame del RenderSync.
    scripts_.setSkeletonHost(scripting::ScriptSystem::SkeletonHost{
        [this](ecs::Entity e, const std::string& bone, core::Mat4& m) {
            return sync_ && e.world() != nullptr && (*sync_).boneWorld(*e.world(), e, bone, m);
        },
        [this](ecs::Entity e) {
            std::vector<std::string> names;
            ecs::RenderSync::SkeletonPose pose;
            if (sync_ && e.world() != nullptr && (*sync_).skeletonPose(*e.world(), e, pose)) names = pose.names;
            return names;
        },
        [this](ecs::Entity e, float* scale) -> const asset::ModelData* {
            return sync_ && e.world() != nullptr ? (*sync_).skeletonData(*e.world(), e, scale) : nullptr;
        }});
    sync_->reset(scene_);
    // Terrenos: datos en Assets (compartidos por el render y la fisica).
    terrain_store_.clear();
    terrain_store_.setRoot(project_.assetsFolder());
    sync_->setTerrainStore(&terrain_store_);
    // Liquidos: el terreno y las cajas de las mallas (Mesh Collider).
    fluids_.clear();
    fluids_.setTerrainStore(&terrain_store_);
    setupEffects();  // VFX Graph, 2D y repeticiones (EditorEffects.cpp)
    fluids_.setBoundsProvider([this](ecs::Entity e, core::Vec3& min, core::Vec3& max) {
        if (!sync_) return false;
        const int actor = sync_->actorIndex(e);
        return actor >= 0 && sync_->actorLocalBounds(static_cast<std::uint32_t>(actor), min, max);
    });
    sync_->setGroundQuery([this](const core::Vec3& origin, const core::Vec3& direction, float max_distance, core::Vec3& point,
                            core::Vec3& normal, ecs::Entity self) {
        // Suelo para el IK de los pies: el impacto mas cercano que no sea el
        // propio personaje (su capsula, su modelo, sus armas...).
        return physics::footGroundRaycast(physics_, origin, direction, max_distance, point, normal, self);
    });
    // En el aire (Character Controller): los pies no buscan el suelo.
    sync_->setSupportQuery([this](ecs::Entity self) { return physics::characterSupport(physics_, self); });
    physics_.setTerrainProvider([this](ecs::Entity entity) -> std::shared_ptr<const terrain::TerrainData> {
        const terrain::Terrain* comp = entity.tryGet<terrain::Terrain>();
        return comp != nullptr ? terrain_store_.get(*comp) : nullptr;
    });
    has_project_ = true;
    // Accesibilidad del jugador al probar en el editor (Accessibility.save).
    gameplay::setAccessibilityFile(project_.libraryFolder() / "Accessibility.json");
    gameplay::loadAccessibility();
    thumbnail_countdown_ = 240;  // ~4 s: la escena ya dibujada para la miniatura del Hub
    // Fisica: ajustes del proyecto y el mundo fisico (en modo edicion).
    loadPhysicsSettings();
    physics_.setAssetManager(asset_manager_.get());
    // La malla que se dibuja o, sin actor (oculta), su pieza en la escena o
    // en el AssetManager: un MeshCollider oculto cuya pieza dibuja otro
    // objeto se quedaba sin colision (en el AssetManager estaba vacia).
    physics_.setMeshProvider([this](ecs::Entity entity) -> const asset::ModelData* {
        return sync_ ? sync_->collisionModelData(entity, scene_) : nullptr;
    });
    physics_.start(world_);
    // Navegacion: la misma geometria de colision que la fisica.
    nav_.clear();
    nav_.setMeshProvider([this](ecs::Entity entity) -> const asset::ModelData* {
        return sync_ ? sync_->collisionModelData(entity, scene_) : nullptr;
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

    startTextureImport();  // (en segundo plano; la escena no lo espera)
    startVideoImport();    // datos y miniaturas de los videos (Library/Cache/Video), igual
    if (open_scene) openStartupScene();
    return true;
}

// La escena inicial del proyecto, o una nueva si no hay.
void EditorApp::openStartupScene() {
    bool opened = false;
    if (project_.startup_scene.valid()) {
        if (const auto scene_info = database_->find(project_.startup_scene)) {
            opened = openScene(scene_info->path);
        }
    }
    if (!opened) {
        newScene();
    }
    // Una escena sin guardar que se perdio en un cierre (si la inicial no
    // tenia autoguardado propio).
    checkAutosaveRecovery({});
}

void EditorApp::closeProject() {
    if (!has_project_) {
        return;
    }
    if (sceneLoading()) project_load_.reset();  // la barra de otra escena, fuera
    if (playing()) exitPlay();
    waitCppSourceCheck();
    // Video: vista previa fuera y lecturas paradas (antes de soltar la GPU).
    video_.stopPreview();
    video_.stop();
    video_preview_entity_ = {};
    show_video_preview_ = false;
    stopVideoImport();
    video_url_test_.reset();
    video_info_.clear();
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
    if (fsm_dirty_) saveStateMachineEditor();
    fsm_uuid_ = {};
    fsm_path_.clear();
    fsm_ = {};
    fsm_cache_.clear();
    show_state_machine_ = false;
    renderer_.waitIdle();
    renderer_.setOutlinedActors({});
    world_.clear();
    // La salida de video usa RenderSync: fuera antes que el.
    {
        video::VideoSystem::Host host;
        host.audio = &audio_;
        video_.setHost(host);
    }
    video_output_.reset();
    if (sync_) {
        sync_->reset(scene_);
    }
    sync_.reset();
    stopTextureImport();
    asset_manager_.reset();
    database_.reset();
    asset::setTextureCacheFolder({});
    hot_reload_.reset();  // para el hilo del vigilante (un rescan en marcha solo usa sus copias)
    terrain_streamer_.clear(world_, terrain_store_, false);
    terrain_store_.clear();
    folder_nodes_.clear();
    current_subfolders_.clear();
    current_assets_.clear();
    ++database_version_;
    has_project_ = false;
    scene_path_.clear();
    resetWorkspaces();
    clearSelection();
    resetUndo();
    dirty_ = false;
    updateTitle();
}

void EditorApp::newScene() {
    if (sceneLoading()) project_load_.reset();  // ya no se abre la otra escena
    if (world_workspace_ != 0) returnToSceneWorkspace();
    if (playing()) exitPlay();
    stopVoxels();
    const ecs::DVec3 origin_before = world_.origin();
    // Cambio de escena: la secuencia comun (teselas, celdas, fisica,
    // navegacion, GPU de terrenos y vegetacion, trabajos en otros hilos).
    resetSceneSystems(/*keep_terrain_edits=*/false);
    alignOriginAfterLoad(origin_before);
    world_.setSceneUuid(Uuid::generate());
    world_.setSceneName("Nueva escena");
    ecs::populateDefaultScene(world_);
    scene_path_.clear();
    loadSceneLighting();  // sin escena: fuera las sondas y el lightmap de la anterior
    clearSelection();
    resetUndo();
    dirty_ = false;
    scene_.placeCamera(Vec3{6.0f, 3.0f, 8.0f}, Vec3{0.0f, 1.0f, 0.0f});
    if (has_project_) physics_.start(world_);  // la del modo edicion, con la escena nueva
    updateTitle();
}

bool EditorApp::openScene(const std::filesystem::path& path) {
    if (world_workspace_ != 0) returnToSceneWorkspace();
    std::string error;
    {
        // Antes de vaciar nada: si no se puede leer, la escena abierta sigue.
        std::error_code ec;
        if (!std::filesystem::is_regular_file(path, ec)) {
            std::cerr << "[Editor] No se pudo abrir la escena " << dialogs::utf8(path) << ": no existe\n";
            return false;
        }
    }
    if (playing()) exitPlay();
    stopVoxels();
    const ecs::DVec3 origin_before = world_.origin();
    // Cambio de escena: la secuencia comun. Antes loadScene dejaba las
    // teselas de terreno de la escena anterior en el mundo (desplazadas si el
    // origen cambiaba) y lo que se cargaba en otros hilos para ella podia
    // entrar en la nueva.
    resetSceneSystems(/*keep_terrain_edits=*/false);
    if (!ecs::loadScene(world_, path, &error)) {
        std::cerr << "[Editor] No se pudo abrir la escena " << dialogs::utf8(path) << ": " << error
                  << "\n";
        alignOriginAfterLoad(origin_before);
        // El mundo ya se vacio: queda una escena nueva SIN la ruta de la
        // anterior (si no, Guardar pisaria la escena anterior con un mundo
        // vacio). No se llama a newScene: puede venir de la carga del
        // proyecto (project_load_), que newScene destruiria en marcha.
        world_.setSceneUuid(Uuid::generate());
        world_.setSceneName("Nueva escena");
        ecs::populateDefaultScene(world_);
        scene_path_.clear();
        loadSceneLighting();
        clearSelection();
        resetUndo();
        dirty_ = false;
        if (has_project_) physics_.start(world_);
        updateTitle();
        return false;
    }
    // World Partition con celdas en disco: regiones cargadas (solo las celdas
    // cerca de la camara y las fijadas, en otros hilos). Si se apagaron para
    // esta escena, el editor las carga todas como antes.
    if (!beginEditorRegions(path)) {
        if (const int streamed = worldpart::WorldPartitionSystem::loadExternalCells(world_, path); streamed > 0) {
            std::cout << "[Editor] " << streamed << " objetos de las celdas en disco ("
                      << dialogs::utf8(worldpart::WorldPartitionSystem::cellsFolder(path).filename()) << ")\n";
        }
    }
    alignOriginAfterLoad(origin_before);
    // Los modelos y texturas de la escena anterior, fuera de la GPU y de la
    // RAM (antes se quedaban: las dos escenas a la vez en la VRAM hasta que
    // el streaming los echaba, y sin streaming nunca). Lo que use la nueva
    // vuelve a llegar por el streaming.
    if (sync_) sync_->reset(scene_);
    scene_path_ = path;
    setCrashContext("Escena", dialogs::utf8(path));
    loadSceneLighting();  // iluminacion horneada de la escena (EditorLighting.cpp)
    clearSelection();
    syncPrefabInstances();  // prefabs que cambiaron con la escena cerrada
    resetUndo();
    dirty_ = false;
    if (has_project_) physics_.start(world_);  // la del modo edicion, con la escena nueva
    checkAutosaveRecovery(path);  // si se cerro por un error con cambios sin guardar
    std::cout << "[Editor] Escena abierta: " << dialogs::utf8(path.filename()) << "\n";
    updateTitle();
    return true;
}

bool EditorApp::saveScene() {
    // En un Blueprint, Guardar guarda el Blueprint (grafo + .crprefab).
    if (findWorkspace(world_workspace_) != nullptr && findWorkspace(world_workspace_)->kind == WorkspaceKind::Blueprint) {
        return saveBlueprintWorkspace();
    }
    if (prefabStageRoot().valid() || (findWorkspace(world_workspace_) != nullptr &&
                                      findWorkspace(world_workspace_)->kind == WorkspaceKind::Prefab)) {
        return savePrefabWorkspace();  // en un prefab, Guardar guarda el prefab
    }
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
    // World Partition con "Celdas en disco": los objetos que se descargan van a
    // <escena>.cells/ (un archivo por celda) y no a la escena.
    const worldpart::WorldPartition* external = nullptr;
    for (const entt::entity h : world_.registry().view<worldpart::WorldPartition>()) {
        const worldpart::WorldPartition& wp = world_.registry().get<worldpart::WorldPartition>(h);
        if (wp.enabled && wp.external_cells) external = &wp;
    }
    prepareRegionsForSave(external);  // regiones cargadas sin celdas en disco: todo al mundo antes
    if (external != nullptr) {
        // Con sus rejillas y sus capas (un archivo por celda de cada una),
        // tambien las que el editor tiene descargadas (regiones cargadas).
        const int cells = editor_regions_.saveCells(world_, *external, scene_path_, &error);
        if (cells < 0) {
            std::cerr << "[Editor] No se pudieron guardar las celdas: " << error << "\n";
            return false;
        }
        std::cout << "[Editor] " << cells << " celdas en disco guardadas\n";
    } else {
        worldpart::WorldPartitionSystem::removeExternalCells(scene_path_);  // ya no se usan
    }
    std::function<bool(const ecs::Entity&)> in_cell_file;
    if (external != nullptr) in_cell_file = [](const ecs::Entity& e) { return worldpart::WorldPartitionSystem::streamable(e); };
    if (!ecs::saveScene(world_, scene_path_, &error, in_cell_file)) {
        std::cerr << "[Editor] No se pudo guardar: " << error << "\n";
        return false;
    }
    dirty_ = false;
    discardSceneAutosave();  // lo guardado ya esta en el archivo
    // Recien activadas las celdas en disco: desde aqui, regiones cargadas.
    if (external != nullptr && !editor_regions_.active()) beginEditorRegions(scene_path_);
    refreshDatabase();
    saveProjectThumbnail();  // el Hub ensena el proyecto como se guardo
    std::cout << "[Editor] Escena guardada: " << dialogs::utf8(scene_path_.filename()) << "\n";
    updateTitle();
    return true;
}

bool EditorApp::saveSceneAs() {
    if (world_workspace_ != 0) return saveScene();
    const std::filesystem::path folder = project_.assetsFolder() / "Scenes";
    std::filesystem::create_directories(folder);
    const std::filesystem::path path =
        dialogs::saveFile(window_.handle(), L"Escena de Cramion (*.crscene)\0*.crscene\0",
                          L"crscene", folder, widen(world_.sceneName()));
    if (path.empty()) {
        return false;
    }
    world_.setSceneName(dialogs::utf8(path.stem()));
    // Otro archivo = otra escena: UUID nuevo (con el de la original, el
    // proyecto veria dos escenas con el mismo UUID e ignoraria la copia).
    std::error_code ec;
    if (scene_path_.empty() || !std::filesystem::equivalent(path, scene_path_, ec)) world_.setSceneUuid(Uuid::generate());
    scene_path_ = path;
    return saveScene();
}

// -----------------------------------------------------------------------------
// Autoguardado (Autosave.h): la instantanea de deshacer, cada N minutos y al
// cerrarse por un error; al abrir la escena se ofrece si es mas nueva.
// -----------------------------------------------------------------------------

void EditorApp::updateAutosave() {
    SceneAutosave::Frame frame;
    frame.minutes = safety_prefs_.autosave_minutes;
    // Solo con la escena en world_: en un prefab o Blueprint history_
    // es el de su escenario. Mientras carga, la instantanea aun no es la nueva.
    const Workspace* ws = findWorkspace(world_workspace_);
    const bool scene_world = ws == nullptr || !isPrefabLikeWorkspace(ws->kind);
    if (has_project_ && scene_world && !projectLoading() && !sceneLoading()) {
        if (!autosave_file_valid_ || scene_path_ != autosave_scene_) {
            autosave_scene_ = scene_path_;
            autosave_file_ = SceneAutosave::fileFor(project_.libraryFolder(), project_.assetsFolder(), scene_path_);
            autosave_file_valid_ = true;
        }
        frame.file = &autosave_file_;
        // El texto de la instantanea, armado en otro hilo (unos segundos por
        // detras del ultimo paso: sin serializar nada en este).
        frame.state = history_.backgroundText(frame.minutes > 0 && dirty_);
        frame.dirty = dirty_;
    }
    autosave_.tick(frame);
}

void EditorApp::checkAutosaveRecovery(const std::filesystem::path& scene) {
    if (!has_project_ || autosave_recovery_.pending) return;
    const std::filesystem::path file = SceneAutosave::fileFor(project_.libraryFolder(), project_.assetsFolder(), scene);
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) return;
    if (!SceneAutosave::newerThanScene(file, scene)) {
        autosave_.discard(file);  // la escena se guardo despues: ya no sirve
        return;
    }
    char when[32] = "?";
    if (const auto stamp = std::filesystem::last_write_time(file, ec); !ec) {
        const std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::clock_cast<std::chrono::system_clock>(stamp));
        std::tm local{};
        if (localtime_s(&local, &t) == 0) std::strftime(when, sizeof(when), "%d/%m/%Y %H:%M", &local);
    }
    autosave_recovery_ = AutosaveRecovery{file, scene, when, true};
    autosave_.protect(file);  // no se pisa hasta que se decida
    std::cout << "[Autoguardado] Hay un autoguardado de " << (scene.empty() ? std::string("una escena sin guardar")
                                                                             : dialogs::utf8(scene.filename()))
              << " de " << when << "\n";
}

void EditorApp::applyAutosaveRecovery() {
    const AutosaveRecovery recovery = std::move(autosave_recovery_);
    autosave_recovery_ = {};
    autosave_.protect({});
    if (!recovery.scene.empty() && recovery.scene != scene_path_) {
        std::cerr << "[Autoguardado] La escena abierta ya es otra: el autoguardado sigue en "
                  << dialogs::utf8(recovery.file) << "\n";
        return;
    }
    std::string text;
    {
        std::ifstream in(recovery.file, std::ios::binary);
        text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    if (text.empty()) {
        std::cerr << "[Autoguardado] No se pudo leer " << dialogs::utf8(recovery.file) << "\n";
        return;
    }
    if (playing()) exitPlay();
    if (recovery.scene.empty() && !scene_path_.empty()) newScene();  // era una escena sin guardar
    // Como deshacer: el mundo se rehace en su sitio. Si el texto no es una
    // escena (autoguardado de emergencia a medias), el mundo no se toca.
    const ecs::DVec3 origin_before = world_.origin();
    std::string error;
    if (!ecs::deserializeWorld(world_, text, &error)) {
        std::cerr << "[Autoguardado] El autoguardado no se puede leer (" << error << "): se queda la escena del disco\n";
        return;
    }
    alignOriginAfterLoad(origin_before);
    primeConstructionScripts();
    clearSelection();
    // Un paso de deshacer: Ctrl+Z vuelve a la escena del disco.
    commit();
    dirty_ = true;
    autosave_.discard(recovery.file);  // desde aqui, el autoguardado normal (y el de emergencia)
    std::cout << "[Autoguardado] Escena recuperada del " << recovery.when << " (sin guardar: Ctrl+S para guardarla)\n";
    updateTitle();
}

void EditorApp::discardSceneAutosave() {
    if (!has_project_) return;
    autosave_.discard(autosave_.lastWritten());
    autosave_.discard(SceneAutosave::fileFor(project_.libraryFolder(), project_.assetsFolder(), scene_path_));
}

void EditorApp::drawAutosaveRecovery() {
    if (!autosave_recovery_.pending) return;
    constexpr const char* kTitle = "Recuperar autoguardado";
    if (!ImGui::IsPopupOpen(kTitle)) ImGui::OpenPopup(kTitle);
    if (!ImGui::BeginPopupModal(kTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    const std::string name = autosave_recovery_.scene.empty() ? std::string("una escena sin guardar")
                                                              : "\"" + dialogs::utf8(autosave_recovery_.scene.stem()) + "\"";
    ImGui::Text("El editor se cerró con cambios sin guardar en %s.", name.c_str());
    ImGui::Text("Hay un autoguardado del %s, más nuevo que el archivo.", autosave_recovery_.when.c_str());
    ImGui::TextDisabled("Recuperar lo carga sin guardarlo (Ctrl+Z vuelve a la escena del disco).");
    ImGui::Spacing();
    if (ImGui::Button("Recuperar", ImVec2(120.0f, 0.0f))) {
        ImGui::CloseCurrentPopup();
        applyAutosaveRecovery();
    }
    ImGui::SameLine();
    if (ImGui::Button("Descartar", ImVec2(120.0f, 0.0f))) {
        ImGui::CloseCurrentPopup();
        autosave_.protect({});
        autosave_.discard(autosave_recovery_.file);
        autosave_recovery_ = {};
    }
    ImGui::EndPopup();
}

void EditorApp::drawAutosaveMenu() {
    if (!ImGui::BeginMenu("Autoguardado")) return;
    static constexpr int kMinutes[] = {0, 1, 2, 5, 10};
    for (const int minutes : kMinutes) {
        char label[32];
        if (minutes == 0) {
            std::snprintf(label, sizeof(label), "Desactivado");
        } else {
            std::snprintf(label, sizeof(label), "Cada %d %s", minutes, minutes == 1 ? "minuto" : "minutos");
        }
        if (ImGui::MenuItem(label, nullptr, safety_prefs_.autosave_minutes == minutes)) {
            safety_prefs_.autosave_minutes = minutes;
            saveSafetyPreferences(safety_prefs_);
        }
    }
    ImGui::Separator();
    ImGui::TextDisabled("En Library/Autosave, en segundo plano.");
    ImGui::TextDisabled("También al cerrarse el editor por un error.");
    ImGui::EndMenu();
}

void EditorApp::runOrAskToSave(PendingAction action, const std::filesystem::path& scene) {
    if (playing()) exitPlay();
    if (has_project_) {
        // Al salir, los prefabs abiertos con cambios se guardan (como los
        // scripts: no se pierde nada).
        if (action == PendingAction::Quit || action == PendingAction::BackToHub) {
            std::vector<int> dirty;
            for (const Workspace& ws : workspaces_) {
                if (prefabLikeDirty(ws.id)) dirty.push_back(ws.id);
            }
            for (const int id : dirty) {
                const Workspace* ws = findWorkspace(id);
                const bool blueprint = ws != nullptr && ws->kind == WorkspaceKind::Blueprint;
                loadWorkspaceWorld(id);
                if (blueprint) {
                    saveBlueprintWorkspace();
                } else {
                    savePrefabWorkspace();
                }
            }
            // El editor .crgraph suelto ya no se guarda solo (vsedit): tampoco se pierde.
            if (graphDoc(GraphKind::VisualScript).dirty) saveGraphDoc(GraphKind::VisualScript);
        }
        // Las escenas se abren, crean y guardan en la pestana Escena.
        returnToSceneWorkspace();
    }
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
            beginOpenScene(pending_scene_);  // con la ventana de carga (sin congelar)
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
// Deshacer / rehacer por diferencias (EditorHistory.h): la instantanea por
// entidad y, en cada paso, solo lo que cambio. Cualquier accion (componentes,
// jerarquia, borrar, MCP) se deshace igual: el historial compara el mundo
// entero por huellas, sin que cada sitio tenga que decir que toco.
// -----------------------------------------------------------------------------

void EditorApp::resetUndo() {
    commit_pending_ = false;
    undo_kinds_.clear();
    redo_kinds_.clear();
    terrain_undo_.clear();
    terrain_redo_.clear();
    flatten_prime_ = true;  // escena nueva: su terreno ya esta aplanado bajo los edificios
    history_.reset(world_);
    primeConstructionScripts();  // abrir una escena no pasa los Construction Script (ya estan hechos)
}

void EditorApp::commit() {
    // En Play los cambios son temporales: sin deshacer (se restaura al parar).
    if (playing()) {
        return;
    }
    // El paso se apunta una vez al final del frame: varias acciones seguidas
    // (pegar, duplicar, crear en serie) quedan en un solo paso de deshacer.
    commit_pending_ = true;
    // Una accion puede cambiar un collider sin mover la entidad: la fisica
    // de edicion (incremental) hace una pasada completa en su siguiente sync.
    physics_.touchAllEdit();
}

void EditorApp::flushCommit() {
    if (!commit_pending_) {
        return;
    }
    CR_PROFILE_SCOPE("Deshacer (flushCommit)");
    commit_pending_ = false;
    if (playing()) {
        return;
    }
    // Lo que se coloco o cambio pasa su Construction Script (en el mismo paso de deshacer).
    runConstructionScripts();
    // Los cambios propios de las instancias de prefab tocadas entran en el mismo paso.
    const bool changed = history_.commit(world_, [this](std::span<const ecs::Entity> touched, std::vector<ecs::Entity>& out) {
        recordPrefabOverrides(touched, out);
    });
    // Un paso caro (miles de entidades tocadas a la vez) se explica en la consola.
    if (const UndoHistory::Stats& stats = history_.lastStats(); stats.ms > 16.0) {
        std::cout << "[Deshacer] Paso de " << static_cast<int>(stats.ms) << " ms (" << stats.entities
                  << " entidades comparadas, " << stats.changed << " con otra huella, " << stats.recorded
                  << " en el paso)\n";
    }
    if (!changed) {
        return;
    }
    undo_kinds_.push_back('W');
    redo_kinds_.clear();
    terrain_redo_.clear();
    for (std::size_t dropped = history_.trim(kUndoMaxSteps, kUndoMemoryLimit); dropped > 0; --dropped) {
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
        applyTerrainUndo(step, false);
        terrain_redo_.push_back(std::move(step));
        redo_kinds_.push_back('T');
        dirty_ = true;
        updateTitle();
        return;
    }
    if (!history_.canUndo()) {
        return;
    }
    if (!undo_kinds_.empty()) undo_kinds_.pop_back();
    redo_kinds_.push_back('W');
    {
        CR_PROFILE_SCOPE("Deshacer (Ctrl+Z)");
        // Solo las entidades del paso; el origen flotante no se toca (el
        // historial recoloca las raices si se movio desde entonces).
        history_.undo(world_);
        physics_.touchAllEdit();  // componentes rehechos sin mover la entidad
    }
    primeConstructionScripts();  // el estado deshecho no vuelve a pasar su Construction Script
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
        applyTerrainUndo(step, true);
        terrain_undo_.push_back(std::move(step));
        undo_kinds_.push_back('T');
        dirty_ = true;
        updateTitle();
        return;
    }
    if (!history_.canRedo()) {
        return;
    }
    if (!redo_kinds_.empty()) redo_kinds_.pop_back();
    undo_kinds_.push_back('W');
    {
        CR_PROFILE_SCOPE("Rehacer (Ctrl+Y)");
        history_.redo(world_);
        physics_.touchAllEdit();  // componentes rehechos sin mover la entidad
    }
    primeConstructionScripts();  // el estado deshecho no vuelve a pasar su Construction Script
    dirty_ = true;
    updateTitle();
}

// -----------------------------------------------------------------------------
// Seleccion
// -----------------------------------------------------------------------------

bool EditorApp::isSelected(const Uuid& uuid) const {
    return selection_.contains(uuid);
}

void EditorApp::selectOnly(const Uuid& uuid) {
    if (isStageHelper(uuid)) return;  // luz y cielo del escenario de un prefab
    inspected_material_ = {};
    inspected_render_texture_ = {};
    selection_.clear();
    if (uuid.valid()) {
        selection_.push_back(uuid);
    }
    active_ = uuid;
}

void EditorApp::toggleSelection(const Uuid& uuid) {
    if (isStageHelper(uuid)) return;
    inspected_material_ = {};
    inspected_render_texture_ = {};
    if (selection_.erase(uuid)) {
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
    inspected_render_texture_ = {};
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
    if (all.size() <= 1) return all;
    // Cada una sube por sus padres mirando el conjunto de seleccionadas:
    // O(n · profundidad). Antes cada par (any_of + isAncestorOf): cuadratico.
    std::unordered_set<entt::entity> chosen;
    chosen.reserve(all.size());
    for (const ecs::Entity& e : all) chosen.insert(e.handle());
    std::vector<ecs::Entity> result;
    result.reserve(all.size());
    for (const ecs::Entity& e : all) {
        bool inside = false;
        for (ecs::Entity p = e.parent(); p.valid(); p = p.parent()) {
            if (chosen.contains(p.handle())) {
                inside = true;
                break;
            }
        }
        if (!inside) result.push_back(e);
    }
    return result;
}

std::vector<ecs::Entity> EditorApp::draggedEntities() const {
    if (!drag_single_.valid()) return topLevelSelection();
    const ecs::Entity e = world_.find(drag_single_);
    if (!e.valid()) return {};
    return {e};
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
    if (!parent.valid()) parent = prefabStageRoot();  // en un prefab, todo dentro de su raiz
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
        case 18:
        case 19:
        case 20: {
            // Tela (Jolt soft body): cortina colgada, bandera al viento o una
            // sabana suelta que cae sobre lo que tenga debajo.
            static constexpr const char* kNames[] = {"Cortina", "Bandera", "Sabana"};
            created = world_.create(kNames[kind - 18], parent);
            physics::Cloth& cloth = created.add<physics::Cloth>();
            created.add<ecs::MeshRenderer>();
            if (kind == 19) {
                cloth.width = 3.0f;
                cloth.height = 2.0f;
                cloth.segments_x = 24;
                cloth.segments_y = 16;
                cloth.pin = physics::ClothPin::LeftEdge;
                cloth.wind = Vec3{8.0f, 0.0f, 1.0f};
                cloth.bending = 0.1f;
                cloth.color = Vec3{0.1f, 0.25f, 0.75f};
            } else if (kind == 20) {
                cloth.pin = physics::ClothPin::None;
                cloth.color = Vec3{0.85f, 0.85f, 0.8f};
                cloth.bending = 0.05f;
                created.setLocalEulerDegrees(Vec3{-90.0f, 0.0f, 0.0f});  // tumbada
            }
            break;
        }
        case 21:
        case 22: {
            created = world_.create(kind == 21 ? "Gelatina" : "Pelota blanda", parent);
            physics::SoftBody& soft = created.add<physics::SoftBody>();
            created.add<ecs::MeshRenderer>();
            if (kind == 21) {
                soft.shape = physics::SoftBodyShape::Cube;
                soft.stiffness = 0.3f;
                soft.pressure = 1.5f;
                soft.color = Vec3{0.9f, 0.15f, 0.3f};
            } else {
                soft.stiffness = 0.6f;
                soft.pressure = 3.0f;
                soft.restitution = 0.4f;
                soft.color = Vec3{0.95f, 0.75f, 0.1f};
                soft.roughness = 0.4f;
            }
            break;
        }
        case 23: {
            // Personaje: la entidad en los pies con su Character Controller
            // y una capsula visible de hijo (cambiable por un modelo).
            created = ecs::createEmpty(world_, parent);
            created.setName("Personaje");
            created.add<physics::CharacterController>();
            ecs::Entity body = ecs::createPrimitive(world_, assets::builtin::kCapsule, "Cuerpo", created);
            body.setLocalPosition(Vec3{0.0f, 1.0f, 0.0f});
            body.setLocalScale(Vec3{0.8f, 1.0f, 0.8f});
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
    // Una URL que ya estaba en el portapapeles no se pega como video despues.
    if (const char* text = ImGui::GetClipboardText(); text != nullptr) clipboard_text_at_copy_ = text;
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
    if (!parent.valid()) parent = prefabStageRoot();
    const std::shared_ptr<const assets::ModelAsset> model = asset_manager_->loadModel(uuid);
    if (!model) {
        std::cerr << "[Editor] No se pudo cargar el modelo\n";
        return {};
    }
    ecs::Entity root = ecs::instantiateModel(world_, *model, parent);
    // Con los materiales creados para el modelo (Crear materiales y asignarlos).
    applyModelMaterials(root, uuid);
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

bool EditorApp::selectionBounds(Vec3& low, Vec3& high) {
    const std::vector<ecs::Entity> targets = topLevelSelection();
    if (targets.empty() || !sync_) {
        return false;
    }
    // Caja de todo lo seleccionado (sus actores), o su posicion.
    low = Vec3{1e30f, 1e30f, 1e30f};
    high = Vec3{-1e30f, -1e30f, -1e30f};
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
    return any;
}

void EditorApp::focusSelection() {
    Vec3 low{};
    Vec3 high{};
    if (!selectionBounds(low, high)) {
        return;
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
    syncFlyCursor();  // volar sin tope: cursor capturado mientras se vuela
    imgui_.updateThumbnails();
    pollImports();
    if (has_project_ && thumbnail_countdown_ > 0 && --thumbnail_countdown_ == 0) saveProjectThumbnail();
    {
        // El origen flotante mueve todas las raices: no es un cambio del
        // usuario. La instantanea de deshacer pasa al origen nuevo (sin paso);
        // los pasos guardan su origen y se recolocan al aplicarse.
        const ecs::DVec3 origin_before = world_.origin();
        updateFloatingOrigin();
        if (world_.origin() != origin_before && !playing() && has_project_) {
            flushCommit();
            history_.sync(world_);
        }
    }
    updateTerrainFlatteners();  // TerrainFlatten: rehace al soltar lo que se movio
    // Material pulsado en el navegador: al Inspector solo si se solto sin
    // arrastrar (arrastrarlo a la Jerarquia o al Inspector no cambia nada).
    if (pending_inspect_material_.valid() && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        const ImGuiIO& io = ImGui::GetIO();
        if (io.MouseDragMaxDistanceSqr[ImGuiMouseButton_Left] < io.MouseDragThreshold * io.MouseDragThreshold) {
            const auto picked = database_->find(pending_inspect_material_);
            if (picked && picked->type == assets::AssetType::RenderTexture) {
                inspected_render_texture_ = pending_inspect_material_;
                inspected_material_ = {};
            } else if (picked && picked->type == assets::AssetType::Model) {
                // Sus ajustes de importacion (hasta que se elija otra cosa).
                inspected_model_ = pending_inspect_material_;
                inspected_model_active_ = active_;
                inspected_material_ = {};
                inspected_render_texture_ = {};
            } else {
                inspected_material_ = pending_inspect_material_;
                inspected_render_texture_ = {};
            }
        }
        pending_inspect_material_ = {};
    }
    if (projectLoading()) {
        stepProjectLoad();
        if (projectLoading() || project_load_.stage == ProjectLoad::Stage::Done) {
            drawProjectLoading(delta_seconds);
            if (project_load_.stage == ProjectLoad::Stage::Done) project_load_.stage = ProjectLoad::Stage::Idle;
            // El frame que viene aparecen todas las areas a la vez: como un
            // cambio de pestana (sin llevarse el foco; si no, la ultima que se
            // dibuja se pondria delante de la elegida en el .ini).
            workspace_switch_frame_ = ImGui::GetFrameCount();
            return;
        }
        // Fallo al abrir: vuelve el Hub con el error.
    } else if (sceneLoading()) {
        // Otra escena: el editor sigue a la vista y respondiendo; solo la
        // barra de carga de la barra de estado.
        stepProjectLoad();
        updateSceneLoading(delta_seconds);
    }
    profiler_overlay_.update(delta_seconds, renderer_);
    watchAssets();
    {
        CR_PROFILE_SCOPE("MCP (pollMcp)");
        pollMcp();
    }
    // Lo que hizo el MCP, en su propio paso de deshacer (no mezclado con lo
    // que se haga en la interfaz en este frame).
    flushCommit();
    updateAutosave();
    pollUpdates();
    runSelfTestStep();
    drawCrashReportWindow();  // si la sesion anterior se cerro por un error
    {
        // Accesibilidad (daltonismo, reducir movimiento) en el render.
        const gameplay::AccessibilitySettings& a = gameplay::accessibility();
        renderer_.setAccessibility({a.colorblind_mode, a.colorblind_strength, a.colorblind_correct, a.reduce_motion});
    }
    updatePlatform(delta_seconds);  // Steam, pruebas automaticas, Git (EditorPlatform.cpp)
    frame_tasks_ms_ += millisecondsSince(ui_start);

    if (!has_project_) {
        flying_ = false;
        drawHub();
        drawModals();
        workspace_switch_frame_ = ImGui::GetFrameCount();  // igual al salir del Hub
        return;
    }

    // Pestana pedida el frame anterior (cambia el mundo: antes de todo).
    applyPendingWorkspace();
    // Cambio de tipo de editor de un area pedido el frame anterior: antes del
    // dockspace y de las areas (EditorAreas.cpp).
    applyPendingAreaSwitch();
    // Los dockspaces de las otras pestanas, vivos antes de cualquier ventana.
    keepInactiveDockspacesAlive();

    // La fisica va ANTES que la interfaz: los gizmos, el Inspector y el
    // render ven los objetos donde estan en este frame (si fuera despues, los
    // gizmos irian un frame por detras de lo que se mueve).
    {
        const CpuClock::time_point physics_start = CpuClock::now();
        // Play en pausa (boton, punto de ruptura...): el audio tambien.
        audio_.setPaused(play_state_ == PlayState::Paused);
        video_.setPaused(play_state_ == PlayState::Paused);  // imagen y sonido congelados
        updateVideoEditor(delta_seconds);  // vista previa sin Play y lecturas pendientes
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
        // UI en el mundo (VR): con los objetos donde quedan este frame.
        if (has_project_) updateWorldUi();
        addCpuSample(kCpuPhysics, millisecondsSince(physics_start));
    }

    // Modo cine: solo la imagen de la escena, sin paneles ni gizmos.
    if (cinema_) {
        drawCinemaView();
        drawModals();
        flushCommit();
        render_view_ = kSceneSlot;
        updateViewExtent(delta_seconds);
        addCpuSample(kCpuUi, millisecondsSince(ui_start));
        return;
    }

    // Los paneles vuelven a dibujarse tras el Hub (abrir proyecto, arrancar)
    // o el modo cine: todas las areas aparecen a la vez, como al cambiar de
    // pestana. Sin esto la ultima en dibujarse se llevaba el foco y
    // promoteFocusedArea la ponia delante (perdiendo el editor elegido en
    // cada area, tambien el que vuelve del .ini).
    if (const int ui_frame = ImGui::GetFrameCount(); ui_frame - areas_drawn_frame_ > 1) {
        workspace_switch_frame_ = ui_frame;
        const auto focused = workspace_focus_.find(active_workspace_);
        workspace_restore_focus_ = focused != workspace_focus_.end() ? focused->second : 0;
    }
    areas_drawn_frame_ = ImGui::GetFrameCount();

    drawMenuBar();
    drawWorkspaceBar();
    drawStatusBar();
    const WorkspaceKind workspace = activeWorkspaceKind();
    // C++: respuestas de clangd (IntelliSense) y compilar al guardar, en
    // cualquier pestana (tambien en la del script, donde se escribe).
    updateCppScripts();
    drawEngineSettingsWindow();
    drawCVarsWindow();
    drawPlatformWindows();  // Git, Pruebas y avisos (en cualquier pestana)
    drawInsightsWindow();   // perfilador de CPU (EditorInsights.cpp)
    drawAccessibilityWindow();  // daltonismo, texto, movimiento (EditorAccessibility.cpp)
    drawPackageManagerWindow();  // Cramion Store (EditorPackageManager.cpp)
    drawGameplayWindows();  // Localizacion, Partidas y Dialogos (EditorGameplay.cpp)
    drawEffectsWindows();   // VFX Graph y Repeticiones (EditorEffects.cpp)
    drawGraphEditors();     // Shader Graph, Visual Script, Behavior Tree y 2D
    drawPhysicsToolWindows();  // Fracturar, vehiculo, Motion Matching e Iluminacion
    if (workspace != WorkspaceKind::Scene) {
        const CpuClock::time_point t = CpuClock::now();
        drawWorkspacePanels(delta_seconds);
        addCpuSample(kCpuPanels, millisecondsSince(t));
    } else {
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        // Id nuevo con las areas de Blender (un editor por area): los disenos
        // de antes se rehacen una vez.
        const ImGuiID dockspace_id = ImGui::GetID("CramionDockspaceBlenderAreas");
        scene_dockspace_id_ = dockspace_id;  // para mantenerlo vivo desde otras pestanas
        if (reset_layout_ || ImGui::DockBuilderGetNode(dockspace_id) == nullptr) {
            buildDefaultLayout(dockspace_id);
            reset_layout_ = false;
        }
        // El fondo del dockspace se ve entre las areas: los huecos oscuros de Blender.
        ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::kGap);
        ImGui::DockSpaceOverViewport(dockspace_id, viewport);
        ImGui::PopStyleColor();

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
        // Ventana > Maquina de estados (o un objeto que la pide): su pestana.
        if (show_state_machine_ && std::none_of(workspaces_.begin(), workspaces_.end(), [](const Workspace& ws) {
                return ws.kind == WorkspaceKind::StateMachine;
            })) {
            openStateMachineWorkspace();
        }
        if (show_script_editor_ && script_tabs_.empty()) drawScriptEditor();  // los abiertos: su pestana
        drawMcpWindow();
        drawTerminalWindow();
        drawDataPackWindow();
        drawBuildConfigsWindow();
        drawTouchInterfaceWindow();
        drawInputActionsWindow();
        drawExportProgress();
        drawImportProgress();
        if (show_physics_) drawPhysicsWindow();
        if (show_navigation_window_) drawNavigationWindow();
        if (show_loaded_regions_) drawLoadedRegionsWindow();
        drawPaintWindow();
        drawModelingWindow();
        drawTerrainGeneratorWindow();
        drawHouseGeneratorWindow();
        drawEnvironmentWindow();
        drawVideoPreviewWindow();  // doble clic en un video del Proyecto
        if (show_cinematic_) drawCinematicWindow();
        drawModals();
        addCpuSample(kCpuPanels, millisecondsSince(t));
    }

    // Atajos globales (no mientras se escribe ni se vuela; en un script, el
    // editor de codigo tiene los suyos).
    ImGuiIO& io = ImGui::GetIO();
    if (workspace == WorkspaceKind::Graph && !io.WantTextInput && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) {
        if (const Workspace* ws = findWorkspace(active_workspace_)) saveGraphDoc(ws->graph);  // el grafo, no la escena
    }
    // Pestana Blueprint: con el Event Graph enfocado, Ctrl+Z/Y son del grafo
    // (los atiende su lienzo) y no del mundo; Ctrl+S guarda el Blueprint
    // entero en los dos casos (una vez, al principio del frame que viene).
    BlueprintEditorState* blueprint = workspace == WorkspaceKind::Blueprint ? activeBlueprint() : nullptr;
    const bool blueprint_graph_keys = blueprint != nullptr && (blueprint->graph_focused || blueprint->graph.focused);
    if (blueprint != nullptr && !io.WantTextInput && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) {
        blueprint->save_requested = true;
    }
    if (!io.WantTextInput && !flying_ && workspace != WorkspaceKind::Script && workspace != WorkspaceKind::StateMachine &&
        workspace != WorkspaceKind::Graph) {
        if (blueprint == nullptr && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) {
            if (io.KeyShift) {
                saveSceneAs();
            } else {
                saveScene();
            }
        }
        if (!blueprint_graph_keys && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z)) undo();
        if (!blueprint_graph_keys && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y)) redo();
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
    promoteFocusedArea();
    flushCommit();
    chooseRenderView();
    updateViewExtent(delta_seconds);
    addCpuSample(kCpuUi, millisecondsSince(ui_start));
}

void EditorApp::reportFrame(float total_ms, float window_ms, float imgui_ms, float scene_update_ms) {
    const gfx::VulkanRenderer::FrameTimings r = renderer_.takeFrameTimings();
    const double now = ImGui::GetTime();
    // Frame lento (menos de ~7 FPS): se explica en la consola, como mucho una
    // vez por segundo, con lo que costo cada parte en ESE frame.
    if (total_ms > g_slow_frame_ms.get() && has_project_ && now - last_slow_report_ > 1.0) {
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
        {
            // Streaming: el streamer de teselas mas adoptarlas en la GPU (dentro
            // de sync), lo que hay en vuelo y lo subido en este frame.
            const terrain::TerrainStreamStats& ts = terrain_streamer_.stats();
            const ecs::RenderSync::TerrainGpuStats gs =
                sync_ ? sync_->terrainGpuStats() : ecs::RenderSync::TerrainGpuStats{};
            const jobs::JobStats js = jobs::stats();
            char buffer[256];
            std::snprintf(buffer, sizeof(buffer),
                          "; streaming %.1f ms (teselas: %d leyendo, %d preparando GPU, %d adoptadas %.1f ms, %.1f MB "
                          "subidos; jobs en cola %d)",
                          static_cast<double>(frame_streaming_ms_) + gs.main_ms, ts.tiles_loading, gs.preparing,
                          gs.adopted, gs.main_ms, static_cast<double>(gs.bytes) / (1024.0 * 1024.0), js.queued);
            line += buffer;
        }
        line += " | GPU " + ms(renderer_.gpuProfiler().totalMilliseconds()) + " ms, " +
                std::to_string(scene_.actors().size()) + " actores, " + std::to_string(renderer_.triangleCount()) +
                " triangulos";
        std::cerr << line << std::endl;
    }
    frame_ms_.fill(0.0f);
    frame_tasks_ms_ = 0.0f;
    frame_refresh_ms_ = 0.0f;
    frame_refreshes_ = 0;
    frame_streaming_ms_ = 0.0f;
}

void EditorApp::syncWorld(float delta_seconds, bool secondary) {
    if (!has_project_ || !sync_ || projectLoading()) {
        return;
    }
    const std::uint32_t view = secondary ? (render_view_ == kGameSlot ? kSceneSlot : kGameSlot) : render_view_;
    // (La fisica ya se simulo al empezar el frame, en drawUi.)
    // La vista que se dibuja: Escena (camara del editor, con contorno y
    // gizmos) o Juego (camara real, sin ayudas). Al cambiar de vista o en un
    // corte de camara, sin historias temporales (no se mezclan dos camaras).
    const bool game = view == kGameSlot;
    // La camara con la proporcion de la imagen que se dibuja (la del panel o
    // la elegida), no la de la ventana.
    const vk::Extent2D extent = renderer_.sceneExtent();
    if (extent.width > 0 && extent.height > 0) {
        scene_.camera().setAspectRatio(static_cast<float>(extent.width) / static_cast<float>(extent.height));
    }
    // La camara del editor ve todo el mundo: con terrenos grandes o mar, el
    // plano lejano crece (con 500 m una isla de 2 km se cortaba a la mitad).
    if (!game) {
        float reach = 1000.0f;
        for (const entt::entity h : world_.registry().view<terrain::Terrain>()) {
            const terrain::Terrain& t = world_.registry().get<terrain::Terrain>(h);
            reach = std::max(reach, t.size * 1.6f + t.height);
        }
        for (const entt::entity h : world_.registry().view<water::WaterBody>()) {
            if (world_.registry().get<water::WaterBody>(h).type == water::WaterType::Ocean) reach = std::max(reach, 6000.0f);
        }
        scene::Camera& camera = scene_.camera();
        if (std::abs(camera.farPlane() - reach) > 1.0f) camera.setClipPlanes(camera.nearPlane(), std::min(reach, 40000.0f));
    }
    renderer_.setViewSlot(view);
    renderer_.setEditorHelpersEnabled(!game);
    // Vistas de los lightmaps (SceneDrawMode 4..6) encima del modo de la vista.
    renderer_.setSceneDrawMode(lightmap_debug_view_ > 0 ? static_cast<gfx::SceneDrawMode>(3 + std::clamp(lightmap_debug_view_, 1, 3))
                                                        : static_cast<gfx::SceneDrawMode>(scene_draw_mode_));
    if (!secondary && (view != last_render_view_ || (game && cinematics_.cutThisFrame()))) {
        renderer_.invalidateHistory();
    }
    if (!secondary) last_render_view_ = view;
    if (game) saved_camera_ = scene_.camera();  // afterRender la devuelve

    const CpuClock::time_point t = CpuClock::now();
    ecs::RenderSync::Options options;
    options.apply_main_camera = game;
    options.render_textures = !secondary;
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

// Blender (espacio "Layout"), un editor por area: la Vista 3D grande en el
// centro (con el Juego detras: Escena | Juego en su cabecera); a la derecha
// la Jerarquia (Outliner) y debajo las Propiedades (Objeto, con Render,
// Mundo y Fisica en sus pestanas verticales); abajo, el Proyecto (con
// Estadisticas y Cinematica detras) y la Consola. El orden en cada area es
// el de dibujo: el primero es el que se ve.
void EditorApp::buildDefaultLayout(unsigned int dockspace_id) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::DockBuilderRemoveNode(dockspace_id);
    ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace_id, viewport->WorkSize);

    ImGuiID center = dockspace_id;
    ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.24f, nullptr, &center);
    const ImGuiID outliner = ImGui::DockBuilderSplitNode(right, ImGuiDir_Up, 0.38f, nullptr, &right);
    ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.27f, nullptr, &center);
    const ImGuiID console = ImGui::DockBuilderSplitNode(bottom, ImGuiDir_Right, 0.36f, nullptr, &bottom);

    ImGui::DockBuilderDockWindow(areaTitle(AreaEditor::Scene).c_str(), center);
    ImGui::DockBuilderDockWindow(areaTitle(AreaEditor::Game).c_str(), center);
    ImGui::DockBuilderDockWindow(areaTitle(AreaEditor::Hierarchy).c_str(), outliner);
    ImGui::DockBuilderDockWindow(areaTitle(AreaEditor::Inspector).c_str(), right);
    ImGui::DockBuilderDockWindow(areaTitle(AreaEditor::RenderSettings).c_str(), right);
    ImGui::DockBuilderDockWindow(areaTitle(AreaEditor::Environment).c_str(), right);
    ImGui::DockBuilderDockWindow(areaTitle(AreaEditor::Physics).c_str(), right);
    ImGui::DockBuilderDockWindow(areaTitle(AreaEditor::Project).c_str(), bottom);
    ImGui::DockBuilderDockWindow(areaTitle(AreaEditor::Statistics).c_str(), bottom);
    ImGui::DockBuilderDockWindow(areaTitle(AreaEditor::Cinematic).c_str(), bottom);
    ImGui::DockBuilderDockWindow(areaTitle(AreaEditor::Console).c_str(), console);
    ImGui::DockBuilderFinish(dockspace_id);
    area_front_.clear();
}

// -----------------------------------------------------------------------------
// Menus
// -----------------------------------------------------------------------------

void EditorApp::drawMenuBar() {
    // La barra de menus es tambien la barra de titulo: algo mas alta. Como la
    // barra superior de Blender: menus, espacios de trabajo, Play en el centro
    // y la escena a la derecha.
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, 7.0f));
    ImGui::PushStyleColor(ImGuiCol_MenuBarBg, theme::kBg0);
    const bool menu_open = ImGui::BeginMainMenuBar();
    ImGui::PopStyleColor();
    if (!menu_open) {
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
        if (ImGui::MenuItem("Configuraciones de compilación...")) {
            ensureBuildConfigs();
            build_config_selected_ = build_configs_.active;
            show_build_configs_ = true;
        }
        if (ImGui::MenuItem("Entrada del proyecto (acciones y teclas)...")) show_input_actions_ = true;
        if (ImGui::MenuItem("Controles táctiles (móvil)...")) show_touch_interface_ = true;
        if (ImGui::MenuItem("Exportar juego...")) exportGame(false);
        if (ImGui::MenuItem("Exportar y jugar...")) exportGame(true);
        if (ImGui::MenuItem("Exportar escena como DataPack...")) openDataPackExport();
        if (ImGui::MenuItem("Exportar carpeta como paquete .crwpack...")) {
            show_package_manager_ = true;
            packageOpenExport();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Guardar proyecto como plantilla...")) show_save_template_ = true;
        if (ImGui::MenuItem("Abrir proyecto (Hub)...")) runOrAskToSave(PendingAction::BackToHub);
        ImGui::Separator();
        if (ImGui::MenuItem("Salir")) requestQuit();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Editar")) {
        if (ImGui::MenuItem("Deshacer", "Ctrl+Z", false, history_.canUndo() || !terrain_undo_.empty())) undo();
        if (ImGui::MenuItem("Rehacer", "Ctrl+Y", false, history_.canRedo() || !terrain_redo_.empty())) redo();
        ImGui::Separator();
        const bool any = !selection_.empty();
        if (ImGui::MenuItem("Copiar", "Ctrl+C", false, any)) copySelection();
        if (ImGui::MenuItem("Pegar", "Ctrl+V", false, !clipboard_.empty())) pasteClipboard();
        if (ImGui::MenuItem("Duplicar", "Ctrl+D", false, any)) duplicateSelection();
        if (ImGui::MenuItem("Borrar", "Supr", false, any)) deleteSelection();
        ImGui::Separator();
        if (ImGui::MenuItem("Enfocar selección", "F", false, any)) focusSelection();
        ImGui::Separator();
        ImGui::MenuItem("Configuración del motor...", nullptr, &show_engine_settings_);
        drawAutosaveMenu();
        drawBackgroundMenu();  // ritmo en segundo plano (EditorBackground.cpp)
        ImGui::Separator();
        drawXrMenu();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Añadir")) {
        drawAddMenuItems();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Ventana")) {
        ImGui::MenuItem("Jerarquía", nullptr, &show_hierarchy_);
        ImGui::MenuItem("Inspector", nullptr, &show_inspector_);
        ImGui::MenuItem("Proyecto", nullptr, &show_project_);
        ImGui::MenuItem("Estadísticas", nullptr, &show_statistics_);
        ImGui::MenuItem("Insights (perfilador de CPU)", nullptr, &show_insights_);
        ImGui::MenuItem("Package Manager", nullptr, &show_package_manager_);
        ImGui::MenuItem("Accesibilidad", nullptr, &show_accessibility_);
        ImGui::MenuItem("Consola", nullptr, &show_console_);
        ImGui::MenuItem("Configuración gráfica", nullptr, &show_render_settings_);
        ImGui::MenuItem("Animator", nullptr, &show_animator_);
        if (ImGui::MenuItem("Máquina de estados", nullptr, show_state_machine_)) {
            show_state_machine_ = true;
            openStateMachineWorkspace();
        }
        ImGui::MenuItem("Scripts (C++)", nullptr, &show_script_editor_);
        ImGui::MenuItem("Terminal (IA)", nullptr, &show_terminal_);
        ImGui::MenuItem("MCP (IA)", nullptr, &show_mcp_);
        ImGui::MenuItem("Física", nullptr, &show_physics_);
        ImGui::MenuItem("Navegación", nullptr, &show_navigation_window_);
        ImGui::MenuItem("Regiones cargadas (World Partition)", nullptr, &show_loaded_regions_);
        ImGui::MenuItem("Pintar prefabs", nullptr, &show_paint_window_);
        ImGui::MenuItem("Modelado (ProBuilder)", nullptr, &show_modeling_window_);
        ImGui::MenuItem("Variables (CVars) y memoria", nullptr, &show_cvars_window_);
        drawPlatformWindowMenu();  // Control de versiones (Git) y Pruebas
        drawGameplayWindowMenu();  // Localizacion y Partidas
        drawEffectsWindowMenu();   // Repeticiones y VFX Graph
        draw2DWindowMenu();        // Paleta de tiles y capas de orden
        drawPhysicsToolMenu();     // Iluminacion, Fracturar, Asistente de vehiculo
        ImGui::MenuItem("Generador de terreno", nullptr, &show_terrain_generator_);
        ImGui::MenuItem("Generador de casas y pueblos", nullptr, &show_house_generator_);
        ImGui::MenuItem("Ambiente (clima y hora)", nullptr, &show_environment_window_);
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
        if (ImGui::MenuItem("Scripting en C++")) open(L"https://cramion.mantraxtools.store/manual/primer-script.html");
        if (ImGui::MenuItem("Shaders propios")) open(L"https://cramion.mantraxtools.store/manual/shaders.html");
        ImGui::Separator();
        if (ImGui::MenuItem("Web de Cramion")) open(L"https://cramion.mantraxtools.store");
        if (ImGui::MenuItem("Discord")) open(L"https://discord.gg/zG7rSsUGEz");
        ImGui::Separator();
        if (ImGui::MenuItem(updateAvailable() ? "Actualizar Cramion..." : "Buscar actualizaciones...")) {
            show_update_dialog_ = true;
            if (!updateAvailable()) startUpdateCheck(true);
        }
        ImGui::Separator();
        ImGui::MenuItem("Terminal con IA (Claude Code)...", nullptr, &show_terminal_);
        ImGui::MenuItem("Conectar una IA (MCP)...", nullptr, &show_mcp_);
        ImGui::EndMenu();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F1, false) && !ImGui::GetIO().WantTextInput) {
        ShellExecuteW(nullptr, L"open", L"https://cramion.mantraxtools.store/manual/index.html", nullptr, nullptr, SW_SHOWNORMAL);
    }

    // Hasta aqui (logo y menus) no se arrastra la ventana.
    caption_blockers_.push_back(ImVec4(0.0f, 0.0f, ImGui::GetCursorScreenPos().x, caption_height_));

    // Espacios de trabajo (Escena, prefabs, scripts, grafos) como las
    // pestanas de Blender, hasta donde empieza el Play del centro.
    const float window_width = ImGui::GetWindowWidth();
    const float play_left = ImGui::GetWindowPos().x + window_width * 0.5f - 190.0f;
    {
        ImGui::SameLine(0.0f, 14.0f);
        const float tabs_start = ImGui::GetCursorScreenPos().x;
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 3.0f);
        drawWorkspaceTabs(play_left - 8.0f);
        caption_blockers_.push_back(ImVec4(tabs_start, 0.0f, ImGui::GetCursorScreenPos().x, caption_height_));
    }

    ImGui::SetCursorPosY(0.0f);
    ImGui::BeginGroup();
    drawPlayControls();
    ImGui::EndGroup();
    caption_blockers_.push_back(ImVec4(ImGui::GetItemRectMin().x, 0.0f, ImGui::GetItemRectMax().x, caption_height_));

    // Como la barra superior de Blender: a la derecha, la escena (para
    // cambiar a otra del proyecto).
    {
        const float controls = 46.0f * 3.0f;
        const float combo_width = std::clamp(window_width * 0.14f, 140.0f, 240.0f);
        ImGui::SameLine(window_width - controls - combo_width - 16.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, 4.0f));
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 3.0f);
        const std::string current = world_.sceneName() + (dirty_ ? " *" : "");
        ImGui::SetNextItemWidth(combo_width);
        const float combo_left = ImGui::GetCursorScreenPos().x;
        if (ImGui::BeginCombo("##scene_selector", current.c_str(), ImGuiComboFlags_HeightLarge)) {
            ImGui::TextDisabled("Escenas de %s", project_.name.c_str());
            ImGui::Separator();
            std::vector<assets::AssetInfo> scenes;
            if (database_) {
                for (const assets::AssetInfo& asset : database_->all()) {
                    if (asset.type == assets::AssetType::Scene) scenes.push_back(asset);
                }
            }
            std::sort(scenes.begin(), scenes.end(),
                      [](const assets::AssetInfo& a, const assets::AssetInfo& b) { return a.name < b.name; });
            for (const assets::AssetInfo& scene : scenes) {
                const bool selected = scene.path == scene_path_;
                if (ImGui::Selectable(scene.name.c_str(), selected) && !selected) {
                    runOrAskToSave(PendingAction::OpenScene, scene.path);
                }
            }
            if (scenes.empty()) ImGui::TextDisabled("(ninguna guardada)");
            ImGui::Separator();
            if (ImGui::Selectable("Nueva escena")) runOrAskToSave(PendingAction::NewScene);
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("Escena abierta (%s). Clic: cambiar a otra del proyecto", project_.name.c_str());
        caption_blockers_.push_back(ImVec4(combo_left, 0.0f, combo_left + combo_width, caption_height_));
        ImGui::PopStyleVar();
    }
    drawWindowControls();
    ImGui::EndMainMenuBar();
    ImGui::PopStyleVar();
}

// Lo que se puede crear (menu Anadir de la barra superior y de la cabecera
// de la vista 3D, como el Shift+A de Blender).
void EditorApp::drawAddMenuItems() {
    const ecs::Entity parent = world_.find(active_);
    const auto item = [&](const char* label, int kind) {
        if (ImGui::MenuItem(label)) createEntity(kind, {});
    };
    item("Crear vacío", 0);
    if (ImGui::MenuItem("Crear vacío hijo", nullptr, false, parent.valid())) createEntity(0, parent);
    if (ImGui::BeginMenu("Malla editable (modelado)")) {
        for (int k = 0; k < static_cast<int>(modeling::shapes::Kind::Count); ++k) {
            const auto kind = static_cast<modeling::shapes::Kind>(k);
            if (ImGui::MenuItem(modeling::shapes::kindName(kind))) {
                createModelingShape(kind, modeling::shapes::defaults(kind));
                show_modeling_window_ = true;
            }
        }
        ImGui::EndMenu();
    }
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
    if (ImGui::BeginMenu("Volumen de post-procesado")) {
        if (ImGui::MenuItem("Global")) createPostVolume(0, {});
        if (ImGui::MenuItem("Caja")) createPostVolume(1, {});
        if (ImGui::MenuItem("Esfera")) createPostVolume(2, {});
        ImGui::EndMenu();
    }
    ImGui::Separator();
    if (ImGui::BeginMenu("Física")) {
        item("Cubo con Rigidbody", 13);
        item("Esfera con Rigidbody", 14);
        item("Zona trigger", 15);
        item("Vehículo (4 ruedas)", 17);
        item("Personaje (Character Controller)", 23);
        ImGui::Separator();
        item("Tela: cortina", 18);
        item("Tela: bandera", 19);
        item("Tela: sábana que cae", 20);
        item("Cuerpo blando: gelatina (cubo)", 21);
        item("Cuerpo blando: pelota", 22);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Efectos")) {
        item("Sistema de partículas", 16);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Líquidos")) {
        if (ImGui::MenuItem("Grifo de agua (emisor)")) createFluidEntity(0);
        if (ImGui::MenuItem("Bloque de agua (cae y salpica)")) createFluidEntity(1);
        if (ImGui::MenuItem("Chorro de miel")) createFluidEntity(2);
        if (ImGui::MenuItem("Chorro de lava")) createFluidEntity(3);
        ImGui::Separator();
        if (ImGui::MenuItem("Mundo de líquidos (ajustes)")) createFluidEntity(4);
        if (ImGui::MenuItem("Desagüe")) createFluidEntity(5);
        if (ImGui::MenuItem("Tanque de demostración")) createFluidEntity(6);
        ImGui::EndMenu();
    }
    if (ImGui::MenuItem("Terreno")) createTerrainEntity();
    if (ImGui::MenuItem("Terreno grande (teselas)")) createTerrainGridEntity();
    if (ImGui::MenuItem("Vegetación (bosque)")) createFoliageEntity();
    if (ImGui::MenuItem("Mundo de bloques")) createVoxelWorldEntity();
    if (ImGui::MenuItem("Ambiente (clima y hora)")) createEnvironmentEntity();
    if (ImGui::MenuItem("Fuego (incendio)")) createFireEntity();
    if (ImGui::MenuItem("Volumen de niebla")) createFogVolumeEntity();
    if (ImGui::BeginMenu("Spline")) {
        if (ImGui::MenuItem("Carretera")) createSplineObject(0);
        if (ImGui::MenuItem("Camino")) createSplineObject(1);
        if (ImGui::MenuItem("Río (cauce por spline)")) createSplineObject(2);
        if (ImGui::MenuItem("Muro")) createSplineObject(3);
        if (ImGui::MenuItem("Valla")) createSplineObject(4);
        if (ImGui::MenuItem("Tubería")) createSplineObject(5);
        if (ImGui::MenuItem("Raíles")) createSplineObject(6);
        if (ImGui::MenuItem("Cinta")) createSplineObject(7);
        ImGui::Separator();
        if (ImGui::MenuItem("Spline vacía (solo la curva)")) createSplineObject(-1);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Agua")) {
        if (ImGui::MenuItem("Océano / playa")) createWaterEntity(0);
        if (ImGui::MenuItem("Lago")) createWaterEntity(1);
        if (ImGui::MenuItem("Río")) createWaterEntity(2);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Realidad virtual")) {
        if (ImGui::MenuItem("XR Origin (cámara y mandos)")) createXrOrigin(world_.find(active_));
        if (ImGui::MenuItem("Jugador VR (primera persona con física)")) createXrPlayer(world_.find(active_));
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("XR Origin + Character Controller + XR Player: stick izquierdo anda, derecho gira,\n"
                              "A salta; andar por la habitación choca. De pie o sentado en el XR Origin.");
        }
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
}

// Barra de estado abajo (Blender): la seleccion a la izquierda; el
// rendimiento y la version a la derecha.
void EditorApp::drawStatusBar() {
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float height = ImGui::GetFontSize() + 8.0f;
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
                                   ImGuiWindowFlags_NoNavFocus;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 4.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::kBg0);
    const bool open = ImGui::BeginViewportSideBar("##statusbar", viewport, ImGuiDir_Down, height, flags);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    if (open) {
        ImGui::PushFont(nullptr, theme::smallFontSize());
        // Como Blender: lo que hacen los botones del raton donde esta.
        drawStatusHints();
        ImGui::SameLine(0.0f, 28.0f);
        std::string left;
        if (playing()) left = play_state_ == PlayState::Paused ? "En pausa" : "Jugando";
        const ecs::Entity active = active_ ? world_.find(active_) : ecs::Entity{};
        if (active.valid()) {
            if (!left.empty()) left += "   |   ";
            left += active.name();
            if (selection_.size() > 1) left += "  (+" + std::to_string(selection_.size() - 1) + ")";
        } else if (left.empty()) {
            left = "Nada seleccionado";
        }
        if (const std::string import = textureImportStatus(); !import.empty()) left += "   |   " + import;
        if (const std::string import = videoImportStatus(); !import.empty()) left += "   |   " + import;
        ImGui::TextDisabled("%s", left.c_str());
        if (sceneLoading()) {
            ImGui::SameLine(0.0f, 24.0f);
            drawSceneLoadingBar(std::clamp(ImGui::GetWindowWidth() * 0.16f, 120.0f, 260.0f));
        }
        char right[200];
        std::snprintf(right, sizeof(right), "Objetos %zu   |   %.0f FPS   |   GPU %.2f ms   |   Cramion %s",
                      world_.entityCount(), ImGui::GetIO().Framerate,
                      renderer_.gpuProfiler().totalMilliseconds(), update::currentVersion().str().c_str());
        ImGui::SameLine(std::max(ImGui::GetWindowWidth() - ImGui::CalcTextSize(right).x - 12.0f,
                                 ImGui::GetCursorPosX() + 20.0f));
        ImGui::TextDisabled("%s", right);
        ImGui::PopFont();
    }
    ImGui::End();
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
            const ImU32 bg = i == 2 ? (held ? theme::kRedActive : theme::kRed)
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
    ImGui::PushStyleColor(ImGuiCol_MenuBarBg, theme::kBg0);
    const bool menu_open = ImGui::BeginMainMenuBar();
    ImGui::PopStyleColor();
    if (!menu_open) {
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
    drawNewBlueprintModal();  // "Nuevo Blueprint de actor" (EditorBlueprint.cpp)
    drawSaveTemplateDialog();
    drawUpdateDialog();
    drawUpdateToast();
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
            discardSceneAutosave();  // descartados: no se ofrecen al volver a abrirla
            performPending();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancelar", ImVec2(120.0f, 0.0f))) {
            ImGui::CloseCurrentPopup();
            pending_ = PendingAction::None;
        }
        ImGui::EndPopup();
    }

    drawAutosaveRecovery();

    // Un asset pedido por su UUID (atajos viejos): por su archivo.
    if (pending_delete_asset_.valid()) {
        if (const auto info = database_ ? database_->find(pending_delete_asset_) : std::nullopt; info && !info->path.empty()) {
            requestDelete({info->path});
        }
        pending_delete_asset_ = {};
    }
    drawDeleteModal();
    drawNewCppScriptModal();
}

// -----------------------------------------------------------------------------
// Estadisticas, consola, ajustes de render
// -----------------------------------------------------------------------------

void EditorApp::drawStatistics(float delta_seconds) {
    frame_history_[frame_history_head_] = delta_seconds * 1000.0f;
    frame_history_head_ = (frame_history_head_ + 1) % frame_history_.size();
    if (!beginArea(AreaEditor::Statistics, &show_statistics_)) {
        endArea();
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
    if (const gfx::FoliageStats fs = renderer_.foliageStats(); fs.instances > 0) {
        ImGui::Text("Vegetación: %llu árboles | visibles %u cerca, %u medio, %u lejos | sombras %u | %.1f M triángulos | %.0f MB",
                    static_cast<unsigned long long>(fs.instances), fs.visible[0], fs.visible[1], fs.visible[2], fs.shadow_casters,
                    static_cast<double>(fs.triangles) / 1.0e6, static_cast<double>(fs.memory_bytes) / (1024.0 * 1024.0));
    } else if (sync_ && sync_->foliageGenerating()) {
        ImGui::TextDisabled("Vegetación: sembrando...");
    }
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
    endArea();
}

void EditorApp::drawConsole() {
    if (!beginArea(AreaEditor::Console, &show_console_)) {
        endArea();
        return;
    }
    if (activeWorkspaceKind() == WorkspaceKind::Scene) console_dock_id_ = ImGui::GetWindowDockID();
    EditorLog& log = EditorLog::instance();
    if (ImGui::Button("Limpiar")) log.clear();
    ImGui::SameLine();
    ImGui::Checkbox("Auto-desplazar", &console_autoscroll_);
    // Cuantos avisos (amarillo) y errores (rojo) hay.
    {
        std::size_t warnings = 0;
        std::size_t errors = 0;
        {
            std::lock_guard lock(log.mutex());
            for (const EditorLog::Entry& entry : log.entries()) {
                if (entry.level == EditorLog::Level::Warning) ++warnings;
                if (entry.level == EditorLog::Level::Error) ++errors;
            }
        }
        const std::string error_text = std::to_string(errors) + (errors == 1 ? " error" : " errores");
        const std::string warning_text = std::to_string(warnings) + (warnings == 1 ? " aviso" : " avisos");
        ImGui::SameLine();
        theme::badge(error_text.c_str(), errors > 0 ? theme::kRed : theme::kBg4);
        ImGui::SameLine(0.0f, 4.0f);
        theme::badge(warning_text.c_str(), warnings > 0 ? theme::kYellow : theme::kBg4);
    }
    ImGui::SameLine();
    theme::searchBox("filter", console_filter_, "Filtrar...");
    ImGui::Separator();
    ImGui::BeginChild("log", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None,
                      ImGuiWindowFlags_HorizontalScrollbar);
    {
        std::lock_guard lock(log.mutex());
        const auto line = [](const EditorLog::Entry& entry) {
            // Avisos en amarillo y errores en rojo, con una marca a la izquierda.
            ImU32 color = theme::kLabel;
            ImU32 mark = 0;
            if (entry.level == EditorLog::Level::Warning) color = mark = theme::kYellow;
            if (entry.level == EditorLog::Level::Error) {
                color = theme::kRedText;
                mark = theme::kRed;
            }
            const ImVec2 at = ImGui::GetCursorScreenPos();
            ImGui::SetCursorScreenPos(ImVec2(at.x + 8.0f, at.y));
            ImGui::PushStyleColor(ImGuiCol_Text, color);
            ImGui::TextUnformatted(entry.text.c_str());
            ImGui::PopStyleColor();
            if (mark != 0) {
                ImDrawList* draw = ImGui::GetWindowDrawList();
                const ImVec2 max = ImGui::GetItemRectMax();
                const float right = ImGui::GetWindowPos().x + ImGui::GetWindowWidth();
                draw->AddRectFilled(ImVec2(at.x, at.y), ImVec2(std::max(max.x, right), max.y),
                                    theme::withAlpha(mark, 18));
                draw->AddRectFilled(ImVec2(at.x, at.y), ImVec2(at.x + 3.0f, max.y), mark);
            }
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
    endArea();
}

namespace {

// Una CVar con su etiqueta (la descripcion, de ayuda). Las guardadas se
// escriben solas en ProjectSettings/CVars.json (se mira cada poco).
void cvarField(cvar::CVarBase& c, const char* label) {
    cvar::Registry& reg = cvar::Registry::instance();
    ImGui::PushID(c.name().c_str());
    std::string error;
    switch (c.type()) {
        case cvar::Type::Bool: {
            bool v = c.toString() == "true";
            if (ImGui::Checkbox(label, &v)) reg.set(c.name(), v ? "true" : "false", &error);
            break;
        }
        case cvar::Type::Int:
        case cvar::Type::Float: {
            double v = 0.0;
            cvar::detail::parseNumber(c.toString(), v);
            const bool integer = c.type() == cvar::Type::Int;
            ImGui::SetNextItemWidth(-150.0f);
            bool edited = false;
            if (c.hasRange()) {
                double lo = c.rangeMin(), hi = c.rangeMax();
                edited = ImGui::DragScalar(label, ImGuiDataType_Double, &v, integer ? 1.0f : 0.01f, &lo, &hi,
                                           integer ? "%.0f" : "%.3g", ImGuiSliderFlags_AlwaysClamp);
            } else {
                edited = ImGui::InputDouble(label, &v, 0.0, 0.0, integer ? "%.0f" : "%.4g");
            }
            if (edited) reg.set(c.name(), cvar::detail::formatNumber(v, integer), &error);
            break;
        }
        case cvar::Type::String: {
            std::string v = c.toString();
            ImGui::SetNextItemWidth(-150.0f);
            if (ImGui::InputText(label, &v, ImGuiInputTextFlags_EnterReturnsTrue)) reg.set(c.name(), v, &error);
            break;
        }
    }
    ImGui::SetItemTooltip("%s\n%s\nPor defecto: %s", c.description().c_str(), c.name().c_str(),
                          c.defaultString().c_str());
    if (!error.empty()) std::cerr << "[CVar] " << error << std::endl;
    ImGui::PopID();
}

// Las CVars de una seccion, con etiquetas en castellano; las que empiezan por
// `prefix` y no estan en la lista salen tambien (con su nombre), para que lo
// nuevo aparezca sin tocar el panel.
void cvarSection(const char* title, const char* prefix,
                 std::initializer_list<std::pair<const char*, const char*>> labeled) {
    cvar::Registry& reg = cvar::Registry::instance();
    std::vector<std::pair<cvar::CVarBase*, std::string>> rows;
    for (const auto& [name, label] : labeled) {
        if (cvar::CVarBase* c = reg.find(name)) rows.emplace_back(c, label);
    }
    const std::string pre = prefix;
    for (cvar::CVarBase* c : reg.all()) {
        if (c->name().compare(0, pre.size(), pre) != 0) continue;
        const bool listed = std::any_of(rows.begin(), rows.end(), [&](const auto& r) { return r.first == c; });
        if (!listed) rows.emplace_back(c, c->name().substr(pre.size()));
    }
    if (rows.empty()) return;
    ImGui::SeparatorText(title);
    for (auto& [c, label] : rows) cvarField(*c, label.c_str());
}

}  // namespace

// Lo global del renderizador que no esta en el componente PostProcessing.
void EditorApp::drawRenderSettings() {
    if (!beginArea(AreaEditor::RenderSettings, &show_render_settings_)) {
        endArea();
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
    toggle("Activar sombras", r.shadowsEnabled(), &gfx::VulkanRenderer::setShadowsEnabled);
    toggle("Ver cascadas", r.cascadeDebug(), &gfx::VulkanRenderer::setCascadeDebug);
    ImGui::SeparatorText("Iluminación global");
    if (r.rayTracingSupported()) {
        toggle("Trazado de rayos (RTX)", r.rayTracingEnabled(), &gfx::VulkanRenderer::setRayTracingEnabled);
    } else {
        ImGui::TextDisabled("Trazado de rayos: la GPU no lo admite");
    }
    toggle("Sonda de reflexión", r.reflectionProbeEnabled(),
           &gfx::VulkanRenderer::setReflectionProbeEnabled);
    toggle("Oclusión del cielo (sin RT)", r.skyOcclusionEnabled(), &gfx::VulkanRenderer::setSkyOcclusionEnabled);
    ImGui::SeparatorText("Rendimiento");
    toggle("Occlusion culling (GPU)", r.occlusionCullingEnabled(),
           &gfx::VulkanRenderer::setOcclusionCullingEnabled);
    cvarSection("Streaming (memoria de video)", "render.streaming.",
                {{"render.streaming.GpuResidency", "Sacar de la VRAM lo que no se ve"},
                 {"render.streaming.MinPixels", "Tamaño mínimo (px)"},
                 {"render.streaming.IdleSeconds", "Segundos sin verse"},
                 {"render.streaming.TextureMips", "Streaming de texturas (mips)"},
                 {"render.streaming.TextureBias", "Detalle de texturas (+ = menos)"},
                 {"render.streaming.AsyncModels", "Cargar modelos en segundo plano"},
                 {"render.streaming.UploadMs", "Subida por frame (ms)"}});
    ImGui::TextDisabled("Si hay tirones al acercarte o alejarte de objetos,\n"
                        "prueba a desactivar el streaming de texturas y la VRAM.");
    cvarSection("Sondas dinámicas (DDGI)", "render.gi.",
                {{"render.gi.DynamicProbes", "Sondas dinámicas"}, {"render.gi.ProbesPerFrame", "Sondas por frame"}});
    cvarSection("Animación lejana (LOD)", "anim.lod.",
                {{"anim.lod.Enabled", "Animar menos lo lejano"},
                 {"anim.lod.Near", "Cada frame hasta (m)"},
                 {"anim.lod.Mid", "1 de cada 2 hasta (m)"},
                 {"anim.lod.Far", "1 de cada 4 hasta (m)"},
                 {"anim.lod.Offscreen", "Fuera de pantalla (frames)"}});
    cvarSection("Partículas", "fx.particles.",
                {{"fx.particles.Budget", "Máximo de partículas"},
                 {"fx.particles.CullDistance", "Distancia de dibujo (m)"},
                 {"fx.particles.CollisionDistance", "Distancia de colisión (m)"}});
    // Cualquier otra CVar de render que se anada (sin seccion propia).
    {
        std::vector<cvar::CVarBase*> other;
        for (cvar::CVarBase* c : cvar::Registry::instance().all()) {
            const std::string& n = c->name();
            if (n.rfind("render.", 0) == 0 && n.rfind("render.streaming.", 0) != 0 && n.rfind("render.gi.", 0) != 0) {
                other.push_back(c);
            }
        }
        if (!other.empty()) {
            ImGui::SeparatorText("Otros");
            for (cvar::CVarBase* c : other) cvarField(*c, c->name().c_str() + 7);
        }
    }
    ImGui::Spacing();
    ImGui::TextDisabled("Exposición, tono, bloom, color, viñeta, SSAO, GI,\n"
                        "reflejos y luz volumétrica: componente PostProcessing.");
    endArea();
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
            applyQualityPreset(r, i);
            g = r.graphicsSettings();
            changed = true;
        }
    }

    ImGui::SeparatorText("Escalado y antialiasing");
    static constexpr const char* kUpscalers[] = {"Desactivado (nativa + FXAA)", "TAA (temporal, escala como TAAU)",
                                                 "AMD FSR 1", "AMD FSR 3.1 (INESTABLE)",
                                                 "NVIDIA DLSS 4 (INESTABLE)"};
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo("##upscaler", kUpscalers[static_cast<int>(g.upscaler)])) {
        for (int i = 0; i < 5; ++i) {
            // FSR 3.1 va en cualquier GPU (con su DLL); DLSS 4, en las RTX.
            const bool available = i <= 2 || (i == 3 && r.fsr3Supported()) || (i == 4 && r.dlssSupported());
            if (ImGui::Selectable(kUpscalers[i], static_cast<int>(g.upscaler) == i,
                                  available ? 0 : ImGuiSelectableFlags_Disabled)) {
                g.upscaler = static_cast<gfx::Upscaler>(i);
                // FSR 3 / DLSS a resolucion nativa solo hacen antialiasing
                // (cuestan mas que el TAA): al elegirlos, se escala (Calidad).
                if ((i == 3 || i == 4) && g.quality == gfx::UpscaleQuality::Native) {
                    g.quality = gfx::UpscaleQuality::Quality;
                }
                changed = true;
            }
            if (!available && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip(i == 4 ? "DLSS necesita una GPU NVIDIA RTX con un controlador reciente."
                                         : "Falta amd_fidelityfx_vk.dll junto al editor.");
            }
        }
        ImGui::EndCombo();
    }
    if (!r.upscalerStatus().empty()) {
        const bool fallback = r.activeUpscaler() == gfx::Upscaler::Taa && g.upscaler != gfx::Upscaler::Taa;
        ImGui::TextColored(fallback ? theme::vec(theme::kYellow) : theme::vec(theme::kOk), "%s",
                           r.upscalerStatus().c_str());
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
    if (g.quality == gfx::UpscaleQuality::Native &&
        (g.upscaler == gfx::Upscaler::Fsr3 || g.upscaler == gfx::Upscaler::Dlss)) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::kYellow);
        ImGui::TextWrapped("Nativa = solo antialiasing (DLAA / FSR AA): cuesta FPS. Para ganar FPS usa Calidad o menos.");
        ImGui::PopStyleColor();
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

    ImGui::SeparatorText("Realidad virtual");
    {
        // Se aplica al soltar (cada cambio rehace los destinos de los ojos).
        static float vr_scale_edit = 1.0f;
        static bool vr_scale_active = false;
        if (!vr_scale_active) vr_scale_edit = g.vr_render_scale * 100.0f;
        ImGui::SetNextItemWidth(-90.0f);
        ImGui::SliderFloat("Resolución VR", &vr_scale_edit, 50.0f, 100.0f, "%.0f %%");
        vr_scale_active = ImGui::IsItemActive();
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            g.vr_render_scale = std::clamp(vr_scale_edit / 100.0f, 0.5f, 1.0f);
            changed = true;
        }
        ImGui::SetItemTooltip("Resolución de cada ojo respecto a la que recomienda el casco (100 %% = nativa).\n"
                              "En VR no se usan los presets de calidad de escritorio y el antialiasing es el TAA\n"
                              "por ojo (FSR 1 solo si lo eliges). Con menos de 12 GB de VRAM puede haber un tope.");
    }

    ImGui::SeparatorText("Presupuesto adaptativo");
    {
        const gfx::HardwareProfile& hw = r.hardwareProfile();
        ImGui::TextDisabled("Perfil: %s  |  %s, %llu MB de VRAM%s", gfx::tierName(hw.tier), hw.gpu_name.c_str(),
                            static_cast<unsigned long long>(hw.vram_mb), hw.integrated ? " (integrada)" : "");
        if (ImGui::Checkbox("Optimización adaptativa", &g.adaptive)) changed = true;
        ImGui::SetItemTooltip(
            "Mide cuánto tarda cada pasada de la GPU y, si no llega a los FPS objetivo, baja solo lo que más\n"
            "ahorra y menos se nota (LODs, detalle de sombras, efectos, resolución con FSR 1). Aprende lo que\n"
            "cuesta cada cosa en este PC y vuelve a subir la calidad cuando sobra tiempo.\n"
            "Aquí es la del visor del editor. El juego exportado se adapta a cada PC: siempre la lleva activa,\n"
            "salvo con \"Respetar los ajustes gráficos del proyecto\" en Configuraciones de compilación.");
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
        ImGui::SetItemTooltip(
            "FPS que intenta mantener la optimización adaptativa en el visor del editor.\n"
            "El juego exportado lo limita a los Hz de la pantalla de cada jugador (entre 30 y 144), salvo con\n"
            "\"Respetar los ajustes gráficos del proyecto\" en Configuraciones de compilación.");
        ImGui::EndDisabled();

        static constexpr int kShadowSizes[] = {0, 1024, 1536, 2048, 3072, 4096, 6144};
        char shadow_preview[48];
        if (g.shadow_resolution <= 0) {
            std::snprintf(shadow_preview, sizeof(shadow_preview), "Auto (%u)", gfx::shadowResolutionFor(hw.tier));
        } else {
            std::snprintf(shadow_preview, sizeof(shadow_preview), "%d", g.shadow_resolution);
        }
        ImGui::SetNextItemWidth(-90.0f);
        // ID propio: "Sombras" es tambien el interruptor de la seccion Sombras.
        if (ImGui::BeginCombo("Sombras##resolucion", shadow_preview)) {
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

        std::uint64_t vram_used = 0;
        std::uint64_t vram_budget = 0;
        if (r.device().videoMemory(vram_used, vram_budget)) {
            const float fraction = static_cast<float>(vram_used) / static_cast<float>(std::max<std::uint64_t>(vram_budget, 1));
            char vram_text[64];
            std::snprintf(vram_text, sizeof(vram_text), "VRAM %.2f / %.2f GB", vram_used / 1073741824.0,
                          vram_budget / 1073741824.0);
            // Gris normal, amarillo al acercarse al limite y rojo casi lleno (tema).
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, fraction > 0.9f    ? theme::kRed
                                                          : fraction > 0.75f ? theme::kYellowDeep
                                                                             : theme::kTextFaint);
            ImGui::ProgressBar(std::min(fraction, 1.0f), ImVec2(-1.0f, 0.0f), vram_text);
            ImGui::PopStyleColor();
            if (fraction > 0.9f) {
                ImGui::TextColored(theme::vec(theme::kRedText),
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
                        ImGui::TextColored(theme::vec(theme::kOk), "máxima");
                    } else {
                        ImGui::TextColored(theme::vec(theme::kYellow), "-%d de %d", level, max);
                    }
                }
                ImGui::EndTable();
            }
            if (!budget.lastAction().empty()) ImGui::TextDisabled("Último cambio: %s", budget.lastAction().c_str());
        }
        ImGui::TextDisabled("Mapa de sombras en uso: %u x %u x 4 cascadas", r.shadowResolution(), r.shadowResolution());
    }

    // Texturas: calidad (lado maximo, al cargar), filtrado anisotropico y
    // sesgo de los mips (al momento). Lo mismo que Graphics::set("texture_*").
    ImGui::SeparatorText("Texturas");
    {
        const gfx::HardwareProfile& hw = r.hardwareProfile();
        struct TextureLevel {
            const char* name;
            int size;
        };
        static constexpr TextureLevel kTextureLevels[] = {{"Auto", 0},      {"Baja", 1024},  {"Media", 2048},
                                                          {"Alta", 4096},   {"Ultra", 8192}, {"Máxima", 16384}};
        const std::uint32_t auto_textures = gfx::textureSizeFor(hw.tier);
        char texture_preview[64];
        std::snprintf(texture_preview, sizeof(texture_preview), "Personalizada (%d)", g.texture_max_size);
        for (const TextureLevel& level : kTextureLevels) {
            if (level.size != g.texture_max_size) continue;
            if (level.size == 0 && auto_textures == 0) {
                std::snprintf(texture_preview, sizeof(texture_preview), "Auto (sin límite)");
            } else if (level.size == 0) {
                std::snprintf(texture_preview, sizeof(texture_preview), "Auto (%u)", auto_textures);
            } else {
                std::snprintf(texture_preview, sizeof(texture_preview), "%s (%d)", level.name, level.size);
            }
        }
        ImGui::SetNextItemWidth(-90.0f);
        if (ImGui::BeginCombo("Calidad##texturas", texture_preview)) {
            for (const TextureLevel& level : kTextureLevels) {
                char label[80];
                if (level.size == 0) {
                    std::snprintf(label, sizeof(label), "Auto (según el perfil)");
                } else {
                    std::snprintf(label, sizeof(label), "%s: máximo %d  (%.0f MB por textura)", level.name, level.size,
                                  level.size * static_cast<double>(level.size) * 4.0 * 1.33 / (1024.0 * 1024.0));
                }
                if (ImGui::Selectable(label, g.texture_max_size == level.size)) {
                    g.texture_max_size = level.size;
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("Lado máximo de las texturas de los modelos. Una de 16K sin comprimir ocupa 1,3 GB de VRAM.\n"
                              "Se aplica a los modelos que se cargan después (reabre la escena).");

        // Filtrado anisotropico: suelos, carreteras y paredes vistos en angulo.
        const std::uint32_t max_aniso = r.maxTextureAnisotropy();
        const std::uint32_t auto_aniso = std::min(r.autoTextureAnisotropy(), std::max(max_aniso, 1u));
        static constexpr int kFiltering[] = {0, 1, 2, 4, 8, 16};
        const auto filtering_label = [&](int samples, char* out, std::size_t size) {
            if (samples == 0) std::snprintf(out, size, "Auto (%ux según el perfil)", auto_aniso);
            else if (samples == 1) std::snprintf(out, size, "Desactivado (trilineal)");
            else std::snprintf(out, size, "Anisotrópico %dx", samples);
        };
        char filtering_preview[64];
        filtering_label(g.texture_filtering, filtering_preview, sizeof(filtering_preview));
        ImGui::SetNextItemWidth(-90.0f);
        if (ImGui::BeginCombo("Filtrado##texturas", filtering_preview)) {
            for (const int samples : kFiltering) {
                char label[64];
                filtering_label(samples, label, sizeof(label));
                const bool available = samples <= 1 || static_cast<std::uint32_t>(samples) <= max_aniso;
                if (ImGui::Selectable(label, g.texture_filtering == samples,
                                      available ? 0 : ImGuiSelectableFlags_Disabled)) {
                    g.texture_filtering = samples;
                    changed = true;
                }
                if (!available && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                    ImGui::SetTooltip("Esta GPU llega hasta %ux.", max_aniso);
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("Filtrado anisotrópico: los suelos y las paredes vistos en ángulo se ven nítidos en vez de\n"
                              "borrosos. En una GPU dedicada 16x apenas cuesta FPS. Se aplica al momento.");

        // Sesgo de los mips: se aplica al soltar (cada cambio rehace los samplers).
        static float lod_bias_edit = 0.0f;
        static bool lod_bias_active = false;
        if (!lod_bias_active) lod_bias_edit = g.texture_lod_bias;
        ImGui::SetNextItemWidth(-90.0f);
        ImGui::SliderFloat("Sesgo de mips", &lod_bias_edit, -1.0f, 2.0f, "%+.2f");
        lod_bias_active = ImGui::IsItemActive();
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            g.texture_lod_bias = std::clamp(lod_bias_edit, -1.0f, 2.0f);
            changed = true;
        }
        ImGui::SetItemTooltip("Negativo: texturas más nítidas (y más parpadeo de lejos). Positivo: más suaves y\n"
                              "menos ancho de banda. 0 = normal. Ctrl+clic para escribir el valor.");
        if (g.texture_lod_bias != 0.0f) {
            ImGui::SameLine();
            if (ImGui::SmallButton("0##sesgo_mips")) {
                g.texture_lod_bias = 0.0f;
                changed = true;
            }
            ImGui::SetItemTooltip("Volver a 0 (normal).");
        }
        const gfx::TextureFiltering active = r.textureFiltering();
        ImGui::TextDisabled("En uso: %s, sesgo %+.2f  |  máximo de la GPU %ux",
                            active.anisotropy > 1.0f ? (std::to_string(static_cast<int>(active.anisotropy)) + "x").c_str()
                                                     : "sin anisotrópico",
                            active.lod_bias, max_aniso);
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

// ProjectSettings/Graphics.ini (GraphicsConfig.h; tambien lo lee el juego).
void EditorApp::saveGraphicsSettings() const {
    if (!has_project_) return;
    saveGraphicsIni(project_.settingsFolder() / "Graphics.ini", renderer_);
}

void EditorApp::loadGraphicsSettings() { loadGraphicsIni(project_.settingsFolder() / "Graphics.ini", renderer_); }

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
        if (isDecalImage(file) || audio::isAudioFile(file)) {
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
            job.result = std::async(std::launch::async, [info, progress = job.progress, settings = job.settings] {
                if (!info) return assets::ImportResult{false, {}, "[Assets] El modelo ya no existe"};
                return assets::reimportModel(*info, settings ? *settings : assets::modelImportSettings(info->path),
                                             progress.get());
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
    ImGui::TextColored(theme::vec(theme::kYellow), "Importando archivo %zu de %zu", current, imports_total_);
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
        ImGui::TextColored(theme::vec(theme::kRedText), "%zu fallaron (ver consola)", imports_failed_);
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
