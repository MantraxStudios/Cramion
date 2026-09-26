// CramionPlayer: el juego exportado desde el editor (Archivo > Exportar juego).
//
//   <Juego>/
//     <Juego>.exe      este programa
//     shaders/         SPIR-V del motor
//     Game/            el proyecto (Assets, ProjectSettings, .crproj),
//                      game.ini (escena inicial) y banner.png
//
// Al abrir muestra el banner del motor mientras carga la escena inicial y
// despues juega: render, fisica (Jolt), scripts Lua, audio, cinematicas,
// particulas e interfaz (Canvas).

#include "ImGuiLayer.h"
#include "LoadingScreen.h"
#include "ProfilerOverlay.h"
#include "UiRenderer.h"

#include <CramionCore/CramionCore.h>
#include <CramionCore/ecs/FloatingOrigin.h>
#include <CramionCore/project/Pack.h>
#include <CramionDM/CramionDM.h>
#include <CramionFX/CramionFX.h>

#include <imgui.h>
#include <imgui_impl_win32.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

namespace {

using namespace cramion;

std::filesystem::path exeFolder() {
    wchar_t buffer[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    return std::filesystem::path(buffer).parent_path();
}

std::string readIniValue(const std::filesystem::path& file, const std::string& key) {
    std::ifstream in(file);
    std::string line;
    while (std::getline(in, line)) {
        const std::size_t eq = line.find('=');
        if (eq != std::string::npos && line.substr(0, eq) == key) return line.substr(eq + 1);
    }
    return {};
}

std::wstring widen(const std::string& text) {
    if (text.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n);
    return out;
}

gfx::GraphicsSettings loadGraphics(const std::filesystem::path& file, gfx::VulkanRenderer& renderer) {
    gfx::GraphicsSettings g = renderer.graphicsSettings();
    std::ifstream in(file);
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
        if (key == "shadows") renderer.setShadowsEnabled(value != 0.0f);
        if (key == "ray_tracing" && renderer.rayTracingSupported()) renderer.setRayTracingEnabled(value != 0.0f);
        if (key == "reflection_probe") renderer.setReflectionProbeEnabled(value != 0.0f);
    }
    return g;
}

}  // namespace

int main() {
    using namespace cramion;
    try {
        ImGui_ImplWin32_EnableDpiAwareness();
        const std::filesystem::path root = exeFolder();
        const std::filesystem::path game = root / "Game";
        wchar_t exe_path[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
        const std::filesystem::path exe_stem = std::filesystem::path(exe_path).stem();
        const std::u8string stem_u8 = exe_stem.u8string();
        const std::string game_name(stem_u8.begin(), stem_u8.end());

        // Sin consola: la salida y los crashes van a %LOCALAPPDATA%/Cramion.
        editor::installCrashHandler(game_name);
        static std::ofstream log_file(editor::localDataFolder("Logs") / std::filesystem::path(exe_stem).concat(".log"));
        if (log_file) {
            std::cout.rdbuf(log_file.rdbuf());
            std::cerr.rdbuf(log_file.rdbuf());
        }

        dm::Window window;
        if (!window.create({.title = exe_stem.wstring(), .width = 1600, .height = 900, .maximized = true})) {
            MessageBoxW(nullptr, L"No se pudo crear la ventana.", L"Cramion", MB_ICONERROR);
            return EXIT_FAILURE;
        }
        // El banner del motor desde el primer momento: descomprimir los
        // assets y compilar los shaders llevan su porcentaje.
        auto loading = std::make_unique<editor::LoadingScreen>(window.handle(), game / "banner.png");
        loading->show(0.0f, "Cargando");

        // Assets: Game/<Juego>.crpack se descomprime una vez en
        // %LOCALAPPDATA%/Cramion/Games/<Juego> (se reutiliza si no cambio).
        // Sin paquete (exportaciones antiguas), la carpeta Game tal cual.
        std::filesystem::path project_folder = game;
        {
            std::filesystem::path pack;
            std::error_code e;
            for (std::filesystem::directory_iterator it(game, e); !e && it != std::filesystem::directory_iterator(); it.increment(e)) {
                if (it->path().extension() == ".crpack") pack = it->path();
            }
            if (!pack.empty()) {
                const std::string id = project::packId(pack);
                const std::filesystem::path data = editor::localDataFolder("Games") / exe_stem;
                std::string current;
                std::ifstream(data / ".crpack_id") >> current;
                if (id.empty() || current != id) {
                    std::vector<project::PackEntry> entries;
                    std::string error;
                    if (!project::readPackIndex(pack, entries, &error)) throw std::runtime_error(error);
                    std::uint64_t total = 0;
                    for (const project::PackEntry& entry : entries) total += entry.size;
                    std::filesystem::remove_all(data, e);
                    std::filesystem::create_directories(data, e);
                    const bool ok = project::extractPack(pack, data, [&](std::uint64_t done, const std::string&) {
                        loading->show(total > 0 ? static_cast<float>(static_cast<double>(done) / static_cast<double>(total)) : 1.0f,
                                      "Descomprimiendo assets");
                        return true;
                    }, &error);
                    if (!ok) throw std::runtime_error("No se pudieron descomprimir los assets: " + error);
                    std::ofstream(data / ".crpack_id") << id;
                }
                project_folder = data;
            }
        }
        const std::optional<project::ProjectInfo> project = project::openProject(project_folder);
        if (!project) {
            MessageBoxW(nullptr, L"No se encontro el juego (carpeta Game).", L"Cramion", MB_ICONERROR);
            return EXIT_FAILURE;
        }
        SetWindowTextW(window.handle(), widen(project->name).c_str());

        dm::Input input;
        scene::Scene scene;
        scene.initialize();
        scene.camera().setAspectRatio(static_cast<float>(window.width()) / static_cast<float>(std::max(window.height(), 1u)));

        gfx::VulkanRenderer renderer;
        const gfx::EngineInfo engine_info{.app_name = project->name.c_str(), .engine_name = "Cramion Engine", .enable_validation = false};
        renderer.setLoadingCallback([&](float fraction, const char* what) { loading->show(fraction, what); });
        renderer.initialize(engine_info, window.handle(), window.width(), window.height());
        loading.reset();
        renderer.setGraphicsSettings(loadGraphics(project->settingsFolder() / "Graphics.ini", renderer));
        renderer.setEditorHelpersEnabled(false);

        editor::ImGuiLayer imgui;
        imgui.initialize(window.handle(), renderer);

        bool quit = false;
        window.setMessageHook([](HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
            return ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam) != 0;
        });
        window.setEventCallback([&](dm::Event& e) {
            input.onEvent(e);
            switch (e.type) {
                case dm::EventType::WindowResize:
                    renderer.onResize(e.width, e.height);
                    if (e.height > 0) scene.camera().setAspectRatio(static_cast<float>(e.width) / static_cast<float>(e.height));
                    break;
                case dm::EventType::WindowMinimized: renderer.onResize(0, 0); break;
                case dm::EventType::WindowRestored: renderer.onResize(window.width(), window.height()); break;
                case dm::EventType::WindowClose: quit = true; break;
                default: break;
            }
        });

        // --- Sistemas del juego ---
        ecs::registerPrefabComponents();
        terrain::registerTerrainComponents();
        water::registerWaterComponents();
        navigation::registerNavigationComponents();
        voxel::registerVoxelComponents();
        audio::registerAudioComponents();
        scripting::registerScriptComponents();
        ui::registerUiComponents();

        assets::AssetDatabase database;
        database.open(project->assetsFolder());
        assets::AssetManager asset_manager(database);
        asset_manager.setCacheFolder(std::filesystem::temp_directory_path() / "Cramion" / project->name);
        ecs::RenderSync sync(asset_manager);
        sync.reset(scene);
        terrain::TerrainStore terrains;
        terrains.setRoot(project->assetsFolder());
        sync.setTerrainStore(&terrains);

        physics::PhysicsSettings physics_settings;
        physics::loadPhysicsSettings(project->settingsFolder() / "Physics.json", physics_settings);
        physics::setProjectPhysicsSettings(physics_settings);
        std::vector<std::string> tags = ecs::defaultTags();
        ecs::loadTags(project->settingsFolder() / "Tags.json", tags);
        ecs::setProjectTags(tags);
        physics::PhysicsSystem physics;
        physics.setSettings(physics_settings);
        physics.setAssetManager(&asset_manager);
        physics.setTerrainProvider([&](ecs::Entity entity) -> std::shared_ptr<const terrain::TerrainData> {
            const terrain::Terrain* comp = entity.tryGet<terrain::Terrain>();
            return comp != nullptr ? terrains.get(*comp) : nullptr;
        });
        physics.setMeshProvider([&](ecs::Entity entity) -> const asset::ModelData* { return sync.actorModelData(entity, scene); });

        audio::AudioSystem audio;
        audio.setAssetsRoot(project->assetsFolder());
        scripting::ScriptSystem scripts;
        scripts.setAssetsRoot(project->assetsFolder());
        scripts.setPhysics(&physics);
        scripts.setAudio(&audio);
        // Navegacion: ajustes del proyecto y la misma geometria que la fisica.
        navigation::NavigationSettings nav_settings;
        navigation::loadNavigationSettings(project->settingsFolder() / "Navigation.json", nav_settings);
        navigation::NavigationSystem nav;
        nav.setSettings(nav_settings);
        nav.setMeshProvider([&](ecs::Entity entity) -> const asset::ModelData* { return sync.actorModelData(entity, scene); });
        nav.setTerrainProvider([&](ecs::Entity entity) -> std::shared_ptr<const terrain::TerrainData> {
            const terrain::Terrain* comp = entity.tryGet<terrain::Terrain>();
            return comp != nullptr ? terrains.get(*comp) : nullptr;
        });
        nav.setPhysics(&physics);
        scripts.setNavigation(&nav);
        scripts.setCursorLock([&](bool locked) { window.setCursorCaptured(locked); });
        // Mundos de bloques: texturas del juego y partidas en Saves/<juego>/Worlds.
        voxel::VoxelSystem voxels;
        voxels.setAssetsRoot(project->assetsFolder());
        voxels.setSaveRoot(editor::localDataFolder("Saves") / std::filesystem::path(exe_stem) / "Worlds");
        scripts.setVoxels(&voxels);
        voxels.setPhysics(&physics);
        cinema::CinematicSystem cinematics;
        physics::ParticleWorld particles;
        ui::UiSystem game_ui;
        ecs::World world;

        // Escena inicial: la de game.ini, o la inicial del proyecto.
        std::filesystem::path scene_file;
        const std::string scene_setting = readIniValue(game / "game.ini", "scene");
        if (!scene_setting.empty()) scene_file = project->assetsFolder() / std::filesystem::path(std::u8string(scene_setting.begin(), scene_setting.end()));
        if (scene_file.empty() && project->startup_scene.valid()) {
            if (const auto info = database.find(project->startup_scene)) scene_file = info->path;
        }

        // Datos guardados de los scripts (Prefs), por juego.
        scripts.setPrefsFile(editor::localDataFolder("Saves") / std::filesystem::path(exe_stem).concat(".prefs"));

        const auto main_camera = [&](core::Vec3& position, core::Vec3& forward) {
            position = scene.camera().position();
            forward = scene.camera().forward();
            world.forEachDepthFirst([&](ecs::Entity e) {
                if (const ecs::Camera* c = e.tryGet<ecs::Camera>(); c != nullptr && c->is_main && e.activeInHierarchy()) {
                    position = e.worldPosition();
                    forward = e.forward();
                }
            });
        };

        struct OrbitView {
            bool fitted = false;
            core::Vec3 target{};
            float distance = 10.0f;
            float min_distance = 1.0f;
            float max_distance = 1000.0f;
            float yaw = 0.6f;     // radianes
            float pitch = 0.45f;  // radianes, por encima del horizonte
            float idle = 0.0f;    // segundos desde que el jugador la movio
            float hint = 0.0f;    // segundos que lleva activa (el aviso se desvanece)
        };
        OrbitView orbit;
        editor::ProfilerOverlay profiler_overlay;

        // --- Carga de escenas por etapas ---
        // La ventana sigue viva (cada frame se dibuja la pantalla de carga con
        // la etapa y el porcentaje) y lo pesado, leer los modelos y
        // descomprimir sus texturas, va en otro hilo. Etapas:
        //   Scene    leer el .crscene (rapido) y lanzar el hilo de los modelos
        //   Models   el hilo lee cada modelo que se va a dibujar
        //   Upload   subirlos a la GPU (un frame con el texto antes: bloquea)
        //   Systems  fisica, navegacion, bloques, audio y scripts
        struct SceneLoad {
            enum class Stage { Idle, Scene, Models, Upload, Systems, Done };
            Stage stage = Stage::Idle;
            std::filesystem::path file;
            std::vector<Uuid> models;
            std::future<void> worker;
            std::atomic<std::size_t> models_done{0};
            std::mutex mutex;
            std::string current;  // modelo que se esta leyendo
            int frames = 0;       // frames en la etapa (para dibujar su texto antes de bloquear)
        };
        SceneLoad load;
        const auto begin_load = [&](const std::filesystem::path& file) {
            load.stage = SceneLoad::Stage::Scene;
            load.file = file;
            load.models.clear();
            load.models_done = 0;
            load.frames = 0;
            orbit.fitted = false;
        };
        const auto scene_loading = [&] { return load.stage != SceneLoad::Stage::Idle && load.stage != SceneLoad::Stage::Done; };
        ecs::RenderSync::Options sync_options;
        sync_options.apply_main_camera = true;

        // Un paso por frame. Devuelve la fraccion y el texto para la pantalla.
        const auto step_load = [&](float& fraction, std::string& text) {
            using Stage = SceneLoad::Stage;
            switch (load.stage) {
                case Stage::Idle:
                case Stage::Done: break;
                case Stage::Scene: {
                    scripts.stop();
                    audio.stop();
                    game_ui.reset();
                    physics.stop();
                    particles.clear();
                    nav.clear();
                    voxels.stop();
                    std::string error;
                    if (load.file.empty() || !ecs::loadScene(world, load.file, &error)) {
                        std::cerr << "[Juego] No se pudo abrir la escena " << load.file.string() << " " << error << "\n";
                    }
                    // Instancias de prefab guardadas con una revision vieja: al dia.
                    ecs::syncOutdatedInstances(world, [&](const Uuid& id) {
                        const auto info = database.find(id);
                        return info && info->type == assets::AssetType::Prefab ? ecs::readPrefabFile(info->path) : std::string{};
                    });
                    renderer.invalidateHistory();
                    // La escena puede estar guardada lejos del origen.
                    renderer.setWorldOrigin(world.origin().x, world.origin().y, world.origin().z);
                    const std::u8string stem = load.file.stem().u8string();
                    scripts.setSceneName(std::string(stem.begin(), stem.end()));
                    // Los modelos que se van a dibujar, una vez cada uno.
                    std::unordered_set<Uuid> seen;
                    world.forEachDepthFirst([&](ecs::Entity e) {
                        const ecs::MeshRenderer* r = e.tryGet<ecs::MeshRenderer>();
                        if (r == nullptr || r->mesh || !r->model.valid() || !e.activeInHierarchy()) return;
                        if (const ecs::EntityInfo* info = e.tryGet<ecs::EntityInfo>(); info != nullptr && info->static_batched) return;
                        if (seen.insert(r->model.uuid).second) load.models.push_back(r->model.uuid);
                    });
                    std::cout << "[Juego] Cargando " << std::string(stem.begin(), stem.end()) << ": " << load.models.size()
                              << " modelos\n";
                    // Mientras el hilo lee, nadie mas toca el AssetManager: la
                    // fisica y los scripts estan parados y no se sincroniza el render.
                    load.worker = std::async(std::launch::async, [&] {
                        for (const Uuid& id : load.models) {
                            {
                                const auto info = database.find(id);
                                std::lock_guard lock(load.mutex);
                                load.current = info ? info->name : std::string{};
                            }
                            asset_manager.loadModel(id);
                            ++load.models_done;
                        }
                    });
                    load.stage = Stage::Models;
                    break;
                }
                case Stage::Models:
                    if (load.worker.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                        load.worker.get();  // relanza si el hilo fallo
                        load.stage = Stage::Upload;
                        load.frames = 0;
                    }
                    break;
                case Stage::Upload:
                    // Primero un frame con "Subiendo a la GPU" en pantalla.
                    if (load.frames++ == 0) break;
                    sync.sync(world, scene, renderer, 0.0f, sync_options);
                    load.stage = Stage::Systems;
                    load.frames = 0;
                    break;
                case Stage::Systems: {
                    if (load.frames++ == 0) break;
                    physics.start(world);
                    // La malla lista antes de que empiecen los scripts.
                    nav.waitForBuild(world, 20.0f);
                    // Los bloques de alrededor listos antes de empezar (no se cae del mundo).
                    voxels.start(world);
                    if (voxels.active()) {
                        core::Vec3 position, forward;
                        main_camera(position, forward);
                        voxels.waitUntilReady(position, 2, 20.0f);
                    }
                    audio.start(world);
                    scripts.start(world);
                    load.stage = Stage::Done;
                    std::cout << "[Juego] Escena lista\n";
                    break;
                }
            }
            switch (load.stage) {
                case Stage::Scene: fraction = 0.02f; text = "Leyendo la escena"; break;
                case Stage::Models: {
                    const std::size_t total = std::max<std::size_t>(load.models.size(), 1);
                    const std::size_t done = std::min(load.models_done.load(), total);
                    fraction = 0.05f + 0.7f * static_cast<float>(done) / static_cast<float>(total);
                    std::lock_guard lock(load.mutex);
                    text = "Cargando modelos (" + std::to_string(std::min(done + 1, total)) + "/" + std::to_string(total) + ")";
                    if (!load.current.empty()) text += ": " + load.current;
                    break;
                }
                case Stage::Upload: fraction = 0.8f; text = "Subiendo modelos y texturas a la GPU"; break;
                case Stage::Systems: fraction = 0.9f; text = "Preparando fisica, navegacion y scripts"; break;
                default: fraction = 1.0f; text.clear(); break;
            }
        };

        // Pantalla de carga (encima de todo).
        const auto draw_loading = [&](const ImVec2& display, float fraction, const std::string& text) {
            ImDrawList* fg = ImGui::GetForegroundDrawList();
            fg->AddRectFilled(ImVec2(0, 0), display, IM_COL32(13, 15, 20, 255));
            const std::string title = project->name;
            ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 2.0f);
            const ImVec2 title_size = ImGui::CalcTextSize(title.c_str());
            fg->AddText(ImVec2((display.x - title_size.x) * 0.5f, display.y * 0.5f - 70.0f), IM_COL32(235, 238, 245, 255),
                        title.c_str());
            ImGui::PopFont();
            const float width = std::min(520.0f, display.x - 64.0f);
            const ImVec2 a{(display.x - width) * 0.5f, display.y * 0.5f};
            const ImVec2 b{a.x + width, a.y + 8.0f};
            fg->AddRectFilled(a, b, IM_COL32(40, 44, 54, 255), 4.0f);
            fg->AddRectFilled(a, ImVec2(a.x + width * std::clamp(fraction, 0.0f, 1.0f), b.y), IM_COL32(90, 150, 255, 255), 4.0f);
            char percent[16];
            std::snprintf(percent, sizeof(percent), "%.0f %%", fraction * 100.0f);
            const ImVec2 percent_size = ImGui::CalcTextSize(percent);
            fg->AddText(ImVec2(b.x - percent_size.x, b.y + 10.0f), IM_COL32(150, 160, 180, 255), percent);
            fg->AddText(ImVec2(a.x, b.y + 10.0f), IM_COL32(150, 160, 180, 255), text.c_str());
        };

        // --- Camara orbital por defecto (como el DefaultPawn de Unreal) ---
        // Si la escena no tiene ninguna Camera activa, el juego no se queda
        // mirando al vacio desde el origen: una vista orbita alrededor de todo
        // lo que se dibuja. Gira sola; arrastrar con el raton la mueve (y la
        // deja quieta un rato) y la rueda acerca. En cuanto un script crea una
        // Camera, manda esa.
        const auto has_camera = [&] {
            bool found = false;
            world.forEachDepthFirst([&](ecs::Entity e) {
                if (!found && e.has<ecs::Camera>() && e.activeInHierarchy()) found = true;
            });
            return found;
        };
        // Encuadra lo que se dibuja: caja de las esferas de los actores.
        const auto fit_orbit = [&] {
            core::Vec3 low{1e30f, 1e30f, 1e30f};
            core::Vec3 high{-1e30f, -1e30f, -1e30f};
            for (const scene::Actor& actor : scene.actors()) {
                if (actor.shadows_only || actor.bounds_radius <= 0.0f || !std::isfinite(actor.bounds_radius)) continue;
                const core::Vec3& c = actor.bounds_center;
                const float r = actor.bounds_radius;
                low = core::Vec3{std::min(low.x, c.x - r), std::min(low.y, c.y - r), std::min(low.z, c.z - r)};
                high = core::Vec3{std::max(high.x, c.x + r), std::max(high.y, c.y + r), std::max(high.z, c.z + r)};
            }
            float radius = 5.0f;
            orbit.target = core::Vec3{0.0f, 0.0f, 0.0f};
            if (low.x <= high.x) {
                orbit.target = (low + high) * 0.5f;
                radius = std::max(core::length(high - low) * 0.5f, 0.5f);
            }
            const float half_fov = std::max(scene.camera().fovY() * 0.5f, 0.2f);
            orbit.distance = std::clamp(radius / std::sin(half_fov) * 1.05f, 1.5f, 20000.0f);
            orbit.min_distance = std::max(radius * 0.05f, 0.3f);
            orbit.max_distance = orbit.distance * 4.0f;
            orbit.idle = 1e9f;
            orbit.hint = 0.0f;
            orbit.fitted = true;
        };
        const auto update_orbit = [&](float dt, const ImVec2& display) {
            if (!orbit.fitted) fit_orbit();
            const ImGuiIO& io = ImGui::GetIO();
            const bool mouse_free = !io.WantCaptureMouse;
            orbit.idle += dt;
            orbit.hint += dt;
            if (mouse_free && (ImGui::IsMouseDown(ImGuiMouseButton_Left) || ImGui::IsMouseDown(ImGuiMouseButton_Right))) {
                orbit.yaw -= io.MouseDelta.x * 0.006f;
                orbit.pitch = std::clamp(orbit.pitch + io.MouseDelta.y * 0.006f, -1.2f, 1.45f);
                if (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f) orbit.idle = 0.0f;
            }
            if (mouse_free && io.MouseWheel != 0.0f) {
                orbit.distance = std::clamp(orbit.distance * std::pow(0.88f, io.MouseWheel), orbit.min_distance, orbit.max_distance);
                orbit.idle = 0.0f;
            }
            // Gira sola si nadie la toca (arranca suave tras soltarla).
            if (orbit.idle > 3.0f) orbit.yaw += dt * 0.25f * std::clamp(orbit.idle - 3.0f, 0.0f, 1.0f);
            const float c = std::cos(orbit.pitch);
            const core::Vec3 offset{std::sin(orbit.yaw) * c, std::sin(orbit.pitch), std::cos(orbit.yaw) * c};
            scene.placeCamera(orbit.target + offset * orbit.distance, orbit.target);

            // Aviso los primeros segundos.
            if (orbit.hint < 6.0f) {
                const float alpha = std::clamp(6.0f - orbit.hint, 0.0f, 1.0f);
                const char* hint = "Escena sin camara: vista orbital (arrastra para girar, rueda para acercar)";
                const ImVec2 size = ImGui::CalcTextSize(hint);
                const ImVec2 at{(display.x - size.x) * 0.5f, display.y - size.y - 24.0f};
                ImDrawList* bg = ImGui::GetForegroundDrawList();
                bg->AddRectFilled(ImVec2(at.x - 12.0f, at.y - 6.0f), ImVec2(at.x + size.x + 12.0f, at.y + size.y + 6.0f),
                                  IM_COL32(13, 15, 20, static_cast<int>(170 * alpha)), 6.0f);
                bg->AddText(at, IM_COL32(220, 225, 235, static_cast<int>(255 * alpha)), hint);
            }
        };

        // --- Banner mientras carga ---
        const std::filesystem::path banner = game / "banner.png";
        constexpr float kBannerSeconds = 3.0f;
        float banner_time = 0.0f;
        float banner_wait = 0.0f;
        bool loaded = false;
        const dm::Input idle_input;
        core::Clock clock;
        while (!quit) {
            const float clock_dt = clock.tick();  // sin recortar (el Profiler mide los frames reales)
            const float dt = std::min(clock_dt, 0.1f);
            window.pumpEvents();
            renderer.applyPendingResize();
            imgui.beginFrame();
            imgui.updateThumbnails();  // sube las imagenes ya decodificadas (banner, UI)
            const ImVec2 display = ImGui::GetIO().DisplaySize;

            const bool showing_banner = banner_time < kBannerSeconds;
            if (showing_banner) {
                ImVec2 probe{};
                if (imgui.image(banner, &probe) != 0 || banner_wait > 1.0f) {
                    banner_time += dt;
                } else {
                    banner_wait += dt;
                }
                ImDrawList* fg = ImGui::GetForegroundDrawList();
                fg->AddRectFilled(ImVec2(0, 0), display, IM_COL32(13, 15, 20, 255));  // el fondo del banner
                ImVec2 image_size{};
                if (const ImTextureID texture = imgui.image(banner, &image_size); texture != 0 && image_size.y > 0) {
                    const float fade_in = std::clamp(banner_time / 0.5f, 0.0f, 1.0f);
                    const float fade_out = std::clamp((kBannerSeconds - banner_time) / 0.6f, 0.0f, 1.0f);
                    const float alpha = std::min(fade_in, fade_out);
                    const float aspect = image_size.x / image_size.y;
                    ImVec2 fit = display;
                    if (display.x / std::max(display.y, 1.0f) > aspect) fit.x = display.y * aspect;
                    else fit.y = display.x / aspect;
                    const ImVec2 a{(display.x - fit.x) * 0.5f, (display.y - fit.y) * 0.5f};
                    fg->AddImage(texture, a, ImVec2(a.x + fit.x, a.y + fit.y), ImVec2(0, 0), ImVec2(1, 1),
                                 IM_COL32(255, 255, 255, static_cast<int>(alpha * 255.0f)));
                }
            }
            // Se empieza a cargar con el banner ya en pantalla (segundo frame).
            if (!loaded && banner_time > 0.05f) {
                loaded = true;
                begin_load(scene_file);
            }
            float load_fraction = 1.0f;
            std::string load_text;
            if (scene_loading()) step_load(load_fraction, load_text);
            if (scene_loading() && !showing_banner) draw_loading(display, load_fraction, load_text);
            const bool running = loaded && !scene_loading() && !showing_banner;
            if (running) {
                const int steps = physics.update(world, dt, true);
                particles.update(world, dt, &physics);
                nav.update(world, dt, true, nav_settings.runtime_generation);
                scripts.setInput(game_ui.typing() ? nullptr : &input);
                if (!game_ui.typing()) {
                    const auto down = [&](dm::Key a, dm::Key b) { return input.isKeyDown(a) || input.isKeyDown(b); };
                    physics.driveVehiclesWithKeyboard(world, down(dm::Key::W, dm::Key::Up), down(dm::Key::S, dm::Key::Down),
                                                      down(dm::Key::A, dm::Key::Left), down(dm::Key::D, dm::Key::Right),
                                                      input.isKeyDown(dm::Key::Space));
                }
                scripts.fixedUpdate(world, physics_settings.fixed_step, steps);
                scripts.update(world, dt);
                cinematics.update(world, dt, true);
                // Origen flotante: si la camara se alejo mucho del (0,0,0), todo
                // se desplaza para que vuelva cerca (nada tiembla a 100 km).
                {
                    core::Vec3 focus, look;
                    main_camera(focus, look);
                    if (const std::optional<core::Vec3> offset = ecs::updateFloatingOrigin(world, focus)) {
                        physics.shiftOrigin(*offset);
                        particles.shiftOrigin(*offset);
                        nav.shiftOrigin(*offset);
                        voxels.shiftOrigin(*offset);
                        cinematics.shiftOrigin(*offset);
                        audio.shiftOrigin(*offset);
                        scripts.shiftOrigin(*offset);
                        renderer.shiftOrigin(*offset);
                        orbit.target = orbit.target - *offset;
                        std::cout << "[Juego] Origen del mundo desplazado; origen = (" << world.origin().x << ", "
                                  << world.origin().y << ", " << world.origin().z << ")\n";
                    }
                }
                // Oyente: AudioListener o la camara principal.
                core::Vec3 position, forward;
                main_camera(position, forward);
                audio.update(world, dt, position, forward);
                voxels.update(dt, position);
                // Interfaz: toda la ventana.
                const ui::UiInput ui_input = editor::uiInputFromImGui(ImVec2(0, 0), display, true, game_ui.typing());
                game_ui.update(world, display.x, display.y, ui_input, true, static_cast<float>(ImGui::GetTime()));
                for (const ui::UiEvent& e : game_ui.takeEvents()) {
                    switch (e.kind) {
                        case ui::UiEvent::Kind::Click: scripts.callMethod(e.target, e.method, e.source); break;
                        case ui::UiEvent::Kind::Number: scripts.callMethod(e.target, e.method, e.number); break;
                        case ui::UiEvent::Kind::Text: scripts.callMethod(e.target, e.method, e.text); break;
                        case ui::UiEvent::Kind::Bool: scripts.callMethod(e.target, e.method, e.flag); break;
                    }
                }
                editor::drawUiList(ImGui::GetBackgroundDrawList(), ImVec2(0, 0), game_ui.drawList(), imgui, project->assetsFolder());
                // Sin Camera en la escena: vista orbital por defecto.
                if (!has_camera()) update_orbit(dt, display);
                // Componente Profiler: FPS, CPU, GPU y memoria en una esquina.
                profiler_overlay.update(clock_dt, renderer);
                if (const ecs::Profiler* p = editor::findProfiler(world)) {
                    profiler_overlay.draw(ImGui::GetForegroundDrawList(), ImVec2(0, 0), display, *p,
                                          renderer.device().name());
                }
                // Scene.load(...) y Game.quit() desde Lua.
                if (const std::filesystem::path next = scripts.takeSceneRequest(); !next.empty()) begin_load(next);
                if (scripts.takeQuitRequest()) quit = true;
            }
            imgui.endFrame();

            scene.update(idle_input, dt);
            // Mientras carga no se sincroniza (el hilo usa el AssetManager):
            // se sigue viendo lo anterior, tapado por la pantalla de carga.
            if (loaded && !scene_loading()) {
                sync.sync(world, scene, renderer, running ? dt : 0.0f, sync_options);
                renderer.setParticles(particles.drawList(scene.camera().position()));
                voxels.syncRenderer(renderer);
            }
            renderer.drawFrame(scene);
            input.newFrame();
        }
        // Cerrado a media carga: el hilo de los modelos termina antes de nada.
        if (load.worker.valid()) load.worker.wait();
        scripts.stop();
        audio.stop();
        physics.stop();
        voxels.stop();  // guarda el mundo con nombre
        renderer.waitIdle();
        sync.reset(scene);
        imgui.shutdown();
        renderer.shutdown();
    } catch (const std::exception& e) {
        std::string message = e.what();
        std::cerr << "[Juego] Error: " << message << "\n";
        // Sin memoria de video: se explica en vez del codigo de Vulkan.
        if (message.find("OutOfDeviceMemory") != std::string::npos || message.find("OutOfHostMemory") != std::string::npos) {
            message = "La tarjeta grafica se quedo sin memoria de video al cargar la escena.\n\n"
                      "La escena tiene demasiados modelos o texturas para esta GPU. Prueba a bajar la resolucion "
                      "de las texturas, usar menos modelos de alta resolucion (escaneos) o partir la escena.\n\n"
                      "Detalle: " + std::string(e.what());
        }
        MessageBoxA(nullptr, message.c_str(), "Cramion", MB_ICONERROR);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
