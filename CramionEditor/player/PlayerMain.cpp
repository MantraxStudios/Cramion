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
#include "UiRenderer.h"

#include <CramionCore/CramionCore.h>
#include <CramionCore/project/Pack.h>
#include <CramionDM/CramionDM.h>
#include <CramionFX/CramionFX.h>

#include <imgui.h>
#include <imgui_impl_win32.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
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
        terrain::registerTerrainComponents();
        water::registerWaterComponents();
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

        // Cambiar de escena: parar todo, cargar y volver a empezar.
        const auto load_scene = [&](const std::filesystem::path& file) {
            scripts.stop();
            audio.stop();
            game_ui.reset();
            physics.stop();
            particles.clear();
            std::string error;
            if (file.empty() || !ecs::loadScene(world, file, &error)) {
                std::cerr << "[Juego] No se pudo abrir la escena " << file.string() << " " << error << "\n";
            }
            renderer.invalidateHistory();
            const std::u8string stem = file.stem().u8string();
            scripts.setSceneName(std::string(stem.begin(), stem.end()));
            physics.start(world);
            audio.start(world);
            scripts.start(world);
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
            const float dt = std::min(clock.tick(), 0.1f);
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
            // Se carga con el banner ya en pantalla (segundo frame).
            if (!loaded && banner_time > 0.05f) {
                loaded = true;
                load_scene(scene_file);
            }
            const bool running = loaded && !showing_banner;
            if (running) {
                const int steps = physics.update(world, dt, true);
                particles.update(world, dt, &physics);
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
                // Oyente: AudioListener o la camara principal.
                core::Vec3 position = scene.camera().position();
                core::Vec3 forward = scene.camera().forward();
                world.forEachDepthFirst([&](ecs::Entity e) {
                    if (const ecs::Camera* c = e.tryGet<ecs::Camera>(); c != nullptr && c->is_main && e.activeInHierarchy()) {
                        position = e.worldPosition();
                        forward = e.forward();
                    }
                });
                audio.update(world, dt, position, forward);
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
                // Scene.load(...) y Game.quit() desde Lua.
                if (const std::filesystem::path next = scripts.takeSceneRequest(); !next.empty()) load_scene(next);
                if (scripts.takeQuitRequest()) quit = true;
            }
            imgui.endFrame();

            scene.update(idle_input, dt);
            if (loaded) {
                ecs::RenderSync::Options options;
                options.apply_main_camera = true;
                sync.sync(world, scene, renderer, running ? dt : 0.0f, options);
                renderer.setParticles(particles.drawList(scene.camera().position()));
            }
            renderer.drawFrame(scene);
            input.newFrame();
        }
        scripts.stop();
        audio.stop();
        physics.stop();
        renderer.waitIdle();
        sync.reset(scene);
        imgui.shutdown();
        renderer.shutdown();
    } catch (const std::exception& e) {
        MessageBoxA(nullptr, e.what(), "Cramion", MB_ICONERROR);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
