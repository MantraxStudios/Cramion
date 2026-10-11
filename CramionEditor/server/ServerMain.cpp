// CramionServer: servidor dedicado sin ventana ni GPU (como el dedicated
// server de Unreal o el "Server Build" de Unity).
//
//   CramionServer.exe [--project <carpeta>] [--scene <Assets/...crscene>]
//                     [--port 7777] [--max-players 16] [--tick 30]
//
// Sin --project usa la carpeta Game/ junto al ejecutable (el juego exportado:
// si lleva un .crpack se descomprime en %LOCALAPPDATA%/Cramion/Servers). Carga
// la escena (la de game.ini o la inicial del proyecto), simula la fisica, la
// navegacion y los scripts (Lua y visuales) a un ritmo fijo, y abre la
// partida en el puerto (Network.host). Los scripts saben que son el servidor
// dedicado con Network.isDedicated(); Network.loadScene cambia de escena.
// Ctrl+C lo cierra ordenadamente (avisa a los jugadores).

#include <CramionCore/ai/Crowd.h>
#include <CramionCore/asset/AssetDatabase.h>
#include <CramionCore/asset/AssetManager.h>
#include <CramionCore/audio/Audio.h>
#include <CramionCore/cvar/CVar.h>
#include <CramionCore/ecs/Components.h>
#include <CramionCore/ecs/Prefab.h>
#include <CramionCore/ecs/SceneSerializer.h>
#include <CramionCore/ecs/Tags.h>
#include <CramionCore/ecs/World.h>
#include <CramionCore/fire/Fire.h>
#include <CramionCore/fluid/Fluid.h>
#include <CramionCore/foliage/Foliage.h>
#include <CramionCore/gameplay/Localization.h>
#include <CramionCore/jobs/JobSystem.h>
#include <CramionCore/navigation/Navigation.h>
#include <CramionCore/net/NetworkObject.h>
#include <CramionCore/physics/PhysicsComponents.h>
#include <CramionCore/physics/PhysicsSettings.h>
#include <CramionCore/physics/PhysicsSystem.h>
#include <CramionCore/project/Pack.h>
#include <CramionCore/project/Project.h>
#include <CramionCore/scripting/NativeApi.h>
#include <CramionCore/scripting/Scripting.h>
#include <CramionCore/spline/Spline.h>
#include <CramionCore/terrain/Terrain.h>
#include <CramionCore/ui/UI.h>
#include <CramionCore/voxel/Voxel.h>
#include <CramionCore/water/Water.h>
#include <CramionCore/world/WorldPartition.h>
#include <CramionCore/xr/XrRig.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace cramion;

namespace {

std::atomic<bool> g_quit{false};

std::filesystem::path path8(const std::string& s) { return std::filesystem::path(std::u8string(s.begin(), s.end())); }

#if defined(_WIN32)
BOOL WINAPI onConsoleEvent(DWORD) {
    g_quit = true;
    return TRUE;
}
#endif
void onSignal(int) { g_quit = true; }

std::string readIniValue(const std::filesystem::path& file, const std::string& key) {
    std::ifstream in(file);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind(key + "=", 0) == 0) return line.substr(key.size() + 1);
    }
    return {};
}

std::filesystem::path exeFolder() {
#if defined(_WIN32)
    wchar_t path[MAX_PATH] = L"";
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return std::filesystem::path(path).parent_path();
#else
    std::error_code ec;
    return std::filesystem::canonical("/proc/self/exe", ec).parent_path();
#endif
}

std::filesystem::path dataFolder(const std::string& sub) {
    std::filesystem::path base;
#if defined(_WIN32)
    char* local = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&local, &size, "LOCALAPPDATA") == 0 && local != nullptr) base = local;
    std::free(local);
#else
    if (const char* home = std::getenv("HOME")) base = std::filesystem::path(home) / ".local/share";
#endif
    if (base.empty()) base = std::filesystem::temp_directory_path();
    const std::filesystem::path folder = base / "Cramion" / sub;
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    return folder;
}

// La carpeta del proyecto: la dada, o Game/ (descomprimiendo su .crpack).
std::optional<project::ProjectInfo> openGame(std::filesystem::path folder) {
    std::filesystem::path pack;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(folder, ec); !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
        if (it->path().extension() == ".crpack") pack = it->path();
    }
    if (!pack.empty()) {
        const std::filesystem::path out = dataFolder("Servers") / pack.stem();
        const std::string id = project::packId(pack);
        std::string current;
        std::ifstream(out / ".crpack_id") >> current;
        if (id.empty() || id != current) {
            std::cout << "[Servidor] Descomprimiendo " << pack.filename().string() << "...\n";
            std::filesystem::remove_all(out, ec);
            std::string error;
            if (!project::extractPack(pack, out, nullptr, &error)) {
                std::cerr << "[Servidor] No se pudo descomprimir: " << error << "\n";
                return std::nullopt;
            }
            std::ofstream(out / ".crpack_id") << id;
        }
        folder = out;
    }
    return project::openProject(folder);
}

}  // namespace

int main(int argc, char** argv) {
    std::filesystem::path project_folder;
    std::string scene_arg;
    int port = 7777;
    int max_players = 16;
    double tick = 30.0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        if (a == "--project") project_folder = path8(next());
        else if (a == "--scene") scene_arg = next();
        else if (a == "--port") port = std::atoi(next().c_str());
        else if (a == "--max-players") max_players = std::atoi(next().c_str());
        else if (a == "--tick") tick = std::atof(next().c_str());
        else if (a == "--help" || a == "-h") {
            std::cout << "CramionServer [--project <carpeta>] [--scene <Assets/...>] [--port 7777] [--max-players 16] [--tick 30]\n";
            return 0;
        }
    }
    tick = std::clamp(tick, 5.0, 240.0);
    port = std::clamp(port, 1, 65535);
    max_players = std::clamp(max_players, 2, 4096);
#if defined(_WIN32)
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCtrlHandler(&onConsoleEvent, TRUE);
#endif
    std::signal(SIGINT, &onSignal);
    std::signal(SIGTERM, &onSignal);

    if (project_folder.empty()) project_folder = exeFolder() / "Game";
    const std::optional<project::ProjectInfo> project = openGame(project_folder);
    if (!project) {
        std::cerr << "[Servidor] No hay proyecto en " << project_folder.string() << " (usa --project <carpeta>)\n";
        return 1;
    }
    std::cout << "[Servidor] " << project->name << " | puerto " << port << " | hasta " << max_players << " jugadores | "
              << tick << " ticks/s | " << jobs::workerCount() << " hilos de trabajo\n";

    // --- Componentes (los mismos que el juego) ---
    physics::registerPhysicsComponents();
    ecs::registerPrefabComponents();
    terrain::registerTerrainComponents();
    water::registerWaterComponents();
    foliage::registerFoliageComponents();
    fire::registerFireComponents();
    fluid::registerFluidComponents();
    navigation::registerNavigationComponents();
    voxel::registerVoxelComponents();
    audio::registerAudioComponents();
    scripting::registerScriptComponents();
    ui::registerUiComponents();
    xr::registerXrComponents();
    net::registerNetworkComponents();

    assets::AssetDatabase database;
    database.open(project->assetsFolder());
    assets::AssetManager asset_manager(database);
    asset_manager.setCacheFolder(dataFolder("Servers") / "Cache" / project->name);
    terrain::TerrainStore terrains;
    terrains.setRoot(project->assetsFolder());

    physics::PhysicsSettings physics_settings;
    physics::loadPhysicsSettings(project->settingsFolder() / "Physics.json", physics_settings);
    physics::setProjectPhysicsSettings(physics_settings);
    std::vector<std::string> tags = ecs::defaultTags();
    ecs::loadTags(project->settingsFolder() / "Tags.json", tags);
    ecs::setProjectTags(tags);
    cvar::Registry::instance().setCheatsAllowed(false);
    cvar::Registry::instance().load(project->settingsFolder() / "CVars.json");

    // Mallas para los colliders: del modelo (sin GPU, sin RenderSync).
    const auto model_data = [&](ecs::Entity entity) -> const asset::ModelData* {
        const ecs::MeshRenderer* mr = entity.tryGet<ecs::MeshRenderer>();
        if (mr == nullptr || !mr->model.valid()) return nullptr;
        const std::shared_ptr<const assets::ModelAsset> model = asset_manager.loadModel(mr->model.uuid);
        if (!model || model->parts.empty()) return nullptr;
        const std::size_t part = static_cast<std::size_t>(std::clamp(mr->part, 0, static_cast<int>(model->parts.size()) - 1));
        return model->parts[part].get();
    };
    physics::PhysicsSystem physics;
    physics.setSettings(physics_settings);
    physics.setAssetManager(&asset_manager);
    physics.setTerrainProvider([&](ecs::Entity entity) -> std::shared_ptr<const terrain::TerrainData> {
        const terrain::Terrain* comp = entity.tryGet<terrain::Terrain>();
        return comp != nullptr ? terrains.get(*comp) : nullptr;
    });
    physics.setMeshProvider(model_data);

    navigation::NavigationSettings nav_settings;
    navigation::loadNavigationSettings(project->settingsFolder() / "Navigation.json", nav_settings);
    navigation::NavigationSystem nav;
    nav.setSettings(nav_settings);
    nav.setMeshProvider(model_data);
    nav.setTerrainProvider([&](ecs::Entity entity) -> std::shared_ptr<const terrain::TerrainData> {
        const terrain::Terrain* comp = entity.tryGet<terrain::Terrain>();
        return comp != nullptr ? terrains.get(*comp) : nullptr;
    });
    nav.setPhysics(&physics);

    scripting::ScriptSystem scripts;
    scripts.setAssetsRoot(project->assetsFolder());
    scripts.setPhysics(&physics);
    scripts.setNavigation(&nav);
    scripts.setPrefsFile(dataFolder("Servers") / (project->name + ".prefs"));
    scripts.setLog([](int level, const std::string& text) {
        (level >= 2 ? std::cerr : std::cout) << (level >= 2 ? "[Lua ERROR] " : level == 1 ? "[Lua aviso] " : "[Lua] ") << text
                                             << "\n";
    });
    {
        std::string loc_error;
        gameplay::localization().load(project->settingsFolder() / "Localization", &loc_error);
    }
    worldpart::WorldPartitionSystem partition;
    worldpart::setActivePartition(&partition);
    ai::CrowdSystem crowds;
    crowds.setNavigation(&nav);
    crowds.setPrefabResolver([&database](const Uuid& uuid) -> std::filesystem::path {
        const auto info = database.find(uuid);
        if (!info) return {};
            return info->path.is_absolute() ? info->path : database.root() / info->path;
    });
    ai::setActiveCrowds(&crowds);

    // Escena: la pedida, la de game.ini o la inicial del proyecto.
    std::filesystem::path scene_file;
    if (!scene_arg.empty()) scene_file = project->assetsFolder() / path8(scene_arg);
    if (scene_file.empty()) {
        const std::string s = readIniValue(project_folder / "game.ini", "scene");
        if (!s.empty()) scene_file = project->assetsFolder() / path8(s);
    }
    if (scene_file.empty() && project->startup_scene.valid()) {
        if (const auto info = database.find(project->startup_scene)) scene_file = info->path;
    }
    if (scene_file.empty()) {
        std::cerr << "[Servidor] El proyecto no tiene escena inicial (usa --scene)\n";
        return 1;
    }

    ecs::World world;
    bool hosted = false;
    const auto load = [&](const std::filesystem::path& file) -> bool {
        if (scripts.running()) scripts.stop();
        physics.stop();
        partition.end(world, false);
        std::string error;
        if (!ecs::loadScene(world, file, &error)) {
            std::cerr << "[Servidor] No se pudo abrir " << file.string() << ": " << error << "\n";
            return false;
        }
        const std::u8string stem = file.stem().u8string();
        scripts.setSceneName(std::string(stem.begin(), stem.end()));
        partition.begin(world, core::Vec3{});
        physics.start(world);
        nav.waitForBuild(world, 30.0f);
        crowds.begin(world);
        scripts.start(world);
        // Marca de servidor dedicado y la partida abierta (si el script no la abrio).
        scripting::api::NativeApi& api = scripts.nativeApi();
        api.set("Network.dedicated", scripting::api::Value(true));
        if (!hosted) {
            if (!api.call("Network.isServer").truthy())
                api.call("Network.host", {scripting::api::Value(port), scripting::api::Value(max_players)});
            hosted = true;
        }
        std::cout << "[Servidor] Escena " << file.filename().string() << " lista (" << world.entityCount() << " objetos)\n";
        return true;
    };
    if (!load(scene_file)) return 1;

    // --- Bucle a ritmo fijo ---
    using Clock = std::chrono::steady_clock;
    const auto step = std::chrono::duration<double>(1.0 / tick);
    auto next_tick = Clock::now();
    auto last = Clock::now();
    auto last_status = Clock::now();
    std::uint64_t ticks = 0;
    double worst_ms = 0.0;
    while (!g_quit) {
        const auto now = Clock::now();
        const float dt = static_cast<float>(std::min(std::chrono::duration<double>(now - last).count(), 0.25));
        last = now;
        const auto work_start = Clock::now();
        const int steps = physics.update(world, dt, true);
        nav.update(world, dt, true, nav_settings.runtime_generation);
        fire::updateFires(world, dt, fire::FireMode::Play, &terrains);
        spline::updateSplineFollowers(world, dt);
        partition.update(world, core::Vec3{});
        crowds.update(world, dt, core::Vec3{});
        scripts.fixedUpdate(world, physics_settings.fixed_step, steps);
        scripts.update(world, dt);
        if (const std::filesystem::path next = scripts.takeSceneRequest(); !next.empty()) load(next);
        if (scripts.takeQuitRequest()) break;
        ++ticks;
        worst_ms = std::max(worst_ms, std::chrono::duration<double, std::milli>(Clock::now() - work_start).count());
        if (now - last_status > std::chrono::seconds(30)) {
            std::cout << "[Servidor] " << scripts.networkStatus() << " | tick peor " << worst_ms << " ms | " << ticks
                      << " ticks\n";
            worst_ms = 0.0;
            last_status = now;
        }
        next_tick += std::chrono::duration_cast<Clock::duration>(step);
        if (next_tick < Clock::now()) next_tick = Clock::now();  // se retraso: sin acumular
        std::this_thread::sleep_until(next_tick);
    }
    std::cout << "[Servidor] Cerrando...\n";
    scripts.shutdownNetwork();
    scripts.stop();
    physics.stop();
    jobs::shutdown();
    return 0;
}
