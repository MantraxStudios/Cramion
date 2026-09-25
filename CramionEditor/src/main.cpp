// -----------------------------------------------------------------------------
// CramionEditor: editor de escenas al estilo de Unity sobre CramionCore (ECS,
// assets, proyectos) y CramionFX, con la interfaz en Dear ImGui (docking).
//
//   CramionEditor.exe                    Hub de proyectos (crear, abrir, recientes)
//   CramionEditor.exe <proyecto.crproj>  abre ese proyecto directamente
// -----------------------------------------------------------------------------

#include "EditorApp.h"
#include "EditorLog.h"
#include "ImGuiLayer.h"

#include <CramionDM/CramionDM.h>
#include <CramionFX/CramionFX.h>

#include <imgui_impl_win32.h>

#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string_view>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wparam,
                                                             LPARAM lparam);

int main(int argc, char** argv) {
    using namespace cramion;
    editor::EditorLog::instance().install();

    try {
        // Nitidez en pantallas con escalado (antes de crear la ventana).
        ImGui_ImplWin32_EnableDpiAwareness();

        dm::Window window;
        // Maximizada desde el principio (Hub y editor).
        if (!window.create({.title = L"Cramion Editor", .width = 1600, .height = 900, .maximized = true, .custom_title_bar = true})) {
            std::cerr << "No se pudo crear la ventana.\n";
            return EXIT_FAILURE;
        }
        dm::Input input;

        // La escena de CramionFX solo la usa el renderizador: la rellena
        // RenderSync con el mundo (ECS) del proyecto.
        scene::Scene scene;
        scene.initialize();
        scene.camera().setAspectRatio(static_cast<float>(window.width()) /
                                      static_cast<float>(window.height()));

        gfx::VulkanRenderer renderer;
        const gfx::EngineInfo engine_info{
            .app_name = "Cramion Editor",
            .engine_name = "Cramion Engine",
#if defined(NDEBUG)
            .enable_validation = false,
#else
            .enable_validation = true,
#endif
        };
        renderer.initialize(engine_info, window.handle(), window.width(), window.height());

        editor::ImGuiLayer imgui;
        imgui.initialize(window.handle(), renderer);
        editor::EditorApp app(window, renderer, scene, imgui);
        // Barra de titulo propia: el editor dice que parte de su barra arrastra.
        window.setCaptionHitTest([&app](int x, int y) { return app.isCaptionDragArea(x, y); });
        app.setInput(&input);  // la entrada del juego (scripts) en Play

        if (argc >= 5 && std::string_view(argv[1]) == "--selftest") {
            app.startSelfTest(argv[2], argv[3], argv[4], argc >= 6 ? std::filesystem::path(argv[5]) : std::filesystem::path{});
        } else if (argc >= 2) {
            app.openProject(std::filesystem::path(argv[1]));
        }

        // Win32 -> ImGui primero; los eventos van a la entrada de la camara,
        // al tamano del render y al editor (archivos soltados, cerrar).
        window.setMessageHook([](HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
            return ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam) != 0;
        });
        window.setEventCallback([&](dm::Event& e) {
            input.onEvent(e);
            switch (e.type) {
                case dm::EventType::WindowResize:
                    renderer.onResize(e.width, e.height);
                    if (e.height > 0) {
                        scene.camera().setAspectRatio(static_cast<float>(e.width) /
                                                      static_cast<float>(e.height));
                    }
                    break;
                case dm::EventType::WindowMinimized:
                    renderer.onResize(0, 0);
                    break;
                case dm::EventType::WindowRestored:
                    renderer.onResize(window.width(), window.height());
                    break;
                case dm::EventType::WindowClose:
                    // La ventana no se destruye aqui: el editor pregunta si
                    // hay cambios sin guardar y decide.
                    app.requestQuit();
                    break;
                case dm::EventType::FileDropped: {
                    std::vector<std::filesystem::path> files;
                    for (const std::wstring& path : e.paths) files.emplace_back(path);
                    app.onFilesDropped(files, e.mouseX, e.mouseY);
                    break;
                }
                default:
                    break;
            }
        });

        const dm::Input idle_input;  // lo que recibe la camara si no se vuela
        core::Clock clock;
        while (!app.quitRequested()) {
            const float delta_seconds = clock.tick();
            window.pumpEvents();

            // Si la ventana cambio de tamano, las imagenes nuevas se crean
            // ANTES de construir la interfaz (la vista apunta a la del render).
            renderer.applyPendingResize();

            imgui.beginFrame();
            app.drawUi(delta_seconds);
            imgui.endFrame();

            scene.update(app.sceneWantsInput() ? input : idle_input, delta_seconds);
            const auto render_start = std::chrono::steady_clock::now();
            // Escena y Juego visibles a la vez: las dos se dibujan cada frame
            // (la otra primero, sin presentar), tambien en Play.
            if (app.wantsSecondaryView()) {
                app.syncWorld(delta_seconds, /*secondary=*/true);
                renderer.drawFrame(scene, /*present=*/false);
                app.afterRender();
            }
            app.syncWorld(delta_seconds);
            renderer.drawFrame(scene);
            app.afterRender();
            app.setRenderCpuTime(std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() -
                                                                            render_start)
                                     .count());
            input.newFrame();
        }

        renderer.waitIdle();
        imgui.shutdown();
        renderer.shutdown();
        window.destroy();
    } catch (const std::exception& error) {
        std::cerr << "Error fatal: " << error.what() << "\n";
        editor::EditorLog::instance().uninstall();
        return EXIT_FAILURE;
    }

    editor::EditorLog::instance().uninstall();
    return EXIT_SUCCESS;
}
