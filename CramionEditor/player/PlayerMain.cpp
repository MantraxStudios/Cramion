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
#include "UiRenderer.h"

#include <CramionCore/CramionCore.h>
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
#include <string>

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
        const std::optional<project::ProjectInfo> project = project::openProject(game);
        if (!project) {
            MessageBoxW(nullptr, L"No se encontro el juego (carpeta Game).", L"Cramion", MB_ICONERROR);
            return EXIT_FAILURE;
        }

        dm::Window window;
        if (!window.create({.title = widen(project->name), .width = 1600, .height = 900, .maximized = true})) {
            return EXIT_FAILURE;
        }
        dm::Input input;
        scene::Scene scene;
        scene.initialize();
        scene.camera().setAspectRatio(static_cast<float>(window.width()) / static_cast<float>(std::max(window.height(), 1u)));

        gfx::VulkanRenderer renderer;
        const gfx::EngineInfo engine_info{.app_name = project->name.c_str(), .engine_name = "Cramion Engine", .enable_validation = false};
        renderer.initialize(engine_info, window.handle(), window.width(), window.height());
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

        // --- Banner mientras carga ---
        const std::filesystem::path banner = game / "banner.png";
        constexpr float kBannerSeconds = 3.0f;
        float banner_time = 0.0f;
        bool loaded = false;
        const dm::Input idle_input;
        core::Clock clock;
        while (!quit) {
            const float dt = std::min(clock.tick(), 0.1f);
            window.pumpEvents();
            renderer.applyPendingResize();
            imgui.beginFrame();
            const ImVec2 display = ImGui::GetIO().DisplaySize;

            const bool showing_banner = banner_time < kBannerSeconds;
            if (showing_banner) {
                banner_time += dt;
                ImDrawList* fg = ImGui::GetForegroundDrawList();
                fg->AddRectFilled(ImVec2(0, 0), display, IM_COL32(0, 0, 0, 255));
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
                std::string error;
                if (scene_file.empty() || !ecs::loadScene(world, scene_file, &error)) {
                    std::cerr << "[Juego] No se pudo abrir la escena inicial " << scene_file.string() << " " << error << "\n";
                }
                physics.start(world);
                audio.start(world);
                scripts.start(world);
            }
            const bool running = loaded && !showing_banner;
            if (running) {
                const int steps = physics.update(world, dt, true);
                particles.update(world, dt, &physics);
                scripts.setInput(game_ui.typing() ? nullptr : &input);
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
