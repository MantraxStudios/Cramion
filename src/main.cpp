// -----------------------------------------------------------------------------
// Cramion - punto de entrada
//
// Orden de arranque:
//   1. Dispositivo DirectX 12 de CramionDM (capa de plataforma).
//   2. Ventana Win32 + callback de eventos + estado de entrada.
//   3. Renderizador Vulkan diferido sobre el HWND de esa ventana.
//   4. Escena: el escenario San Miguel, la camara y una luz direccional.
//   5. Bucle principal: tiempo -> eventos -> actualizacion -> dibujado.
//   6. Apagado ordenado (el renderizador espera a la GPU y libera al reves).
//
// Controles:
//   W A S D            moverse            Raton derecho  mirar
//   Espacio / Control  subir / bajar      Rueda          velocidad
//   Shift              correr
//   T                  ciclo dia/noche    G              sombras on/off
//   C                  ver cascadas       X              antialiasing (FXAA)
//   N                  saltar 12 horas (dia <-> noche)
//   B                  bloom on/off       O              SSAO on/off
//   E                  auto-exposicion    L              rayos de luz on/off
//   I                  luz rebotada (GI)  K              tonemapper Neutral / ACES
//   R                  reflejos (SSR)     P  sonda de reflexion on/off
//   V                  nubes volumetricas H  occlusion culling on/off
//   Y                  trazado de rayos (RTX) on/off
//   U                  cielo HDR de la escena / cielo fisico
//   J                  lluvia: calle mojada y charcos on/off
//   M                  agua: zona inundada on/off
//   RePag / AvPag      compensacion de exposicion (+-0.5 EV)
//   ESC                salir
// -----------------------------------------------------------------------------

#include <CramionDM/CramionDM.h>

#include <CramionFX/CramionFX.h>

#include "DemoScenes.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <span>
#include <sstream>
#include <string_view>

using namespace cramion::dm;
using cramion::core::Clock;
using cramion::gfx::EngineInfo;
using cramion::gfx::VulkanRenderer;
using cramion::scene::Scene;
using cramion::demo::kScenes;
using cramion::demo::SceneEntry;

namespace {


// Tabla de milisegundos de GPU por pasada (tecla F o CRAMION_PROFILE=1).
void printGpuTimings(const cramion::gfx::VulkanRenderer& renderer) {
    const cramion::gfx::GpuProfiler& profiler = renderer.gpuProfiler();
    if (!profiler.supported() || profiler.timings().empty()) {
        return;
    }
    std::cout << "--- GPU: " << std::fixed << std::setprecision(2) << profiler.totalMilliseconds()
              << " ms por frame ---\n";
    for (const cramion::gfx::GpuTiming& timing : profiler.timings()) {
        std::cout << "  " << std::left << std::setw(22) << timing.name << std::right
                  << std::setw(7) << timing.milliseconds << " ms\n";
    }
    std::cout.flush();
}

const SceneEntry& chooseScene(int argc, char** argv) {
    if (argc >= 2) {
        for (const SceneEntry& entry : kScenes) {
            if (std::string_view(argv[1]) == entry.name) {
                return entry;
            }
        }
        std::cerr << "Escena desconocida \"" << argv[1] << "\"; opciones:";
        for (const SceneEntry& entry : kScenes) {
            std::cerr << " " << entry.name;
        }
        std::cerr << ". Se usa " << kScenes[0].name << ".\n";
    }
    return kScenes[0];
}

// Conecta los eventos de la ventana con el estado de entrada, el renderizador
// y la camara.
void installEventCallback(Window& window, Input& input, VulkanRenderer& renderer, Scene& scene) {
    window.setEventCallback([&](Event& e) {
        input.onEvent(e);  // Alimentar el estado de entrada.

        switch (e.type) {
            case EventType::WindowClose:
                std::cout << "[Evento] Cierre de ventana solicitado\n";
                break;

            case EventType::WindowResize:
                // La swapchain y el G-buffer se recrean en el siguiente frame.
                renderer.onResize(e.width, e.height);
                if (e.height > 0) {
                    scene.camera().setAspectRatio(static_cast<float>(e.width) /
                                                  static_cast<float>(e.height));
                }
                break;

            case EventType::WindowMinimized:
                renderer.onResize(0, 0);
                break;

            case EventType::WindowRestored:
                renderer.onResize(window.width(), window.height());
                break;

            case EventType::FileDropped:
                std::cout << "[Drop]   " << e.paths.size() << " archivo(s)\n";
                break;

            default:
                break;
        }
    });
}

// Titulo con las estadisticas del reloj y el estado de la demo.
void updateWindowTitle(Window& window, const Clock& clock, const Scene& scene,
                       const VulkanRenderer& renderer) {
    const auto& position = scene.camera().position();

    std::wostringstream title;
    title << L"Cramion - Vulkan diferido  |  " << std::fixed << std::setprecision(0) << clock.fps()
          << L" FPS (" << std::setprecision(2) << clock.averageFrameMilliseconds() << L" ms)"
          << L"  |  " << renderer.triangleCount() / 1000 << L"k tris  |  clusteres "
          << renderer.visibleSubmeshes() << L"/" << renderer.totalSubmeshes() << L" (sombras "
          << renderer.shadowSubmeshes() << L")  |  oclusion "
          << (renderer.occlusionCullingEnabled() ? L"ON" : L"OFF") << L" ("
          << renderer.occludedSubmeshes() << L" tapados)" << L"  |  XYZ "
          << std::setprecision(1) << position.x << L" " << position.y << L" " << position.z
          << L"  |  "<< std::setprecision(0) << std::setw(2) << std::setfill(L'0')
          << static_cast<int>(scene.timeOfDayHours()) << L":" << std::setw(2)
          << static_cast<int>(std::fmod(scene.timeOfDayHours(), 1.0f) * 60.0f)
          << std::setfill(L' ') << L"  |  ciclo " << (scene.dayCycleEnabled() ? L"ON" : L"OFF") << L"  |  sombras "
          << (renderer.shadowsEnabled() ? L"ON" : L"OFF")
          << (renderer.cascadeDebug() ? L" [cascadas]" : L"") << L"  |  FXAA "
          << (renderer.antialiasingEnabled() ? L"ON" : L"OFF") << L"  |  bloom "
          << (renderer.bloomEnabled() ? L"ON" : L"OFF") << L"  |  SSAO "
          << (renderer.ssaoEnabled() ? L"ON" : L"OFF") << L"  |  volumetrica "
          << (renderer.volumetricEnabled() ? L"ON" : L"OFF") << L"  |  SSR "
          << (renderer.ssrEnabled() ? L"ON" : L"OFF") << L"  |  RTX "
          << (!renderer.rayTracingSupported() ? L"no disponible"
              : renderer.rayTracingActive()   ? L"ON"
                                              : L"OFF")
          << L"  |  sonda "
          << (renderer.reflectionProbeEnabled() ? L"ON" : L"OFF") << L"  |  nubes "
          << (renderer.cloudsEnabled() ? L"ON" : L"OFF") << L"  |  cielo "
          << (renderer.environmentActive() ? L"HDR" : L"fisico") << L"  |  lluvia "
          << (!renderer.rainAvailable() ? L"-" : renderer.rainEnabled() ? L"ON" : L"OFF")
          << L"  |  agua "
          << (!renderer.waterAvailable() ? L"-" : renderer.waterEnabled() ? L"ON" : L"OFF")
          << L"  |  GI "
          << (renderer.giEnabled() ? L"ON" : L"OFF") << L"  |  "
          << (renderer.acesTonemapper() ? L"ACES" : L"Neutral") << L"  |  exposicion "
          << (renderer.autoExposureEnabled() ? L"auto " : L"manual ") << std::setprecision(2)
          << L"x" << renderer.currentExposure() << L" (" << std::showpos << std::setprecision(1)
          << renderer.exposureCompensation() << L" EV" << std::noshowpos << L")";

    window.setTitle(title.str());
}

}  // namespace

int main(int argc, char** argv) {
    // Cada linea del log se escribe al momento: si algo falla durante una
    // carga larga, lo ultimo que se hizo queda a la vista.
    std::cout << std::unitbuf;

    try {
        // 1) Dispositivo DirectX 12 de la capa de plataforma.
        Device device;
        if (!device.initialize(/*enableDebugLayer=*/false)) {
            std::wcerr << L"Error: no se pudo inicializar el dispositivo DirectX 12.\n";
            return EXIT_FAILURE;
        }
        std::wcout << L"CramionDM inicializado. GPU: " << device.adapterName() << L"\n";

        // 2) Ventana.
        Window window;
        const WindowConfig window_config{
            .title = L"Cramion - Vulkan diferido", .width = 1600, .height = 900};
        if (!window.create(window_config)) {
            std::wcerr << L"Error: no se pudo crear la ventana.\n";
            return EXIT_FAILURE;
        }

        Input input;

        // 3) Escena: el escenario, la camara y una luz direccional.
        Scene scene;
        scene.initialize();

        const SceneEntry& entry = chooseScene(argc, argv);
        cramion::demo::populateScene(entry, scene);
        if (std::getenv("CRAMION_TMP_CAM")) scene.placeCamera(cramion::core::Vec3{-18.7f, 4.3f, 6.3f}, cramion::core::Vec3{0.0f, 1.5f, 0.0f});  // TMP

        scene.camera().setAspectRatio(static_cast<float>(window.width()) /
                                      static_cast<float>(window.height()));

        // 4) Renderizador Vulkan sobre el HWND de la ventana.
        VulkanRenderer renderer;
        installEventCallback(window, input, renderer, scene);

        const EngineInfo engine_info{
            .app_name = "Cramion",
            .engine_name = "Cramion Engine",
#if defined(NDEBUG)
            .enable_validation = false,
#else
            .enable_validation = true,
#endif
        };

        renderer.initialize(engine_info, window.handle(), window.width(), window.height());

        cramion::demo::configureRenderer(entry, renderer, scene);
        const auto upload_start = std::chrono::steady_clock::now();
        renderer.uploadModels(scene);
        std::cout << "[Vulkan] Subida a la GPU: "
                  << std::chrono::duration<float>(std::chrono::steady_clock::now() - upload_start)
                         .count()
                  << " s" << std::endl;

        std::cout << "\nControles: WASD moverse | raton derecho mirar | Espacio/Ctrl subir-bajar\n"
                     "           Shift correr | rueda velocidad | T ciclo dia\n"
                     "           N dia/noche | G sombras | C cascadas | X antialiasing\n"
                     "           B bloom | O SSAO | R reflejos (SSR) | P sonda de reflexion\n"
                     "           I luz rebotada (GI) | V nubes volumetricas\n"
                     "           H occlusion culling | Y trazado de rayos (RTX)\n"
                     "           U cielo HDR de la escena / cielo fisico | J lluvia (charcos)\n"
                     "           M agua (zona inundada)\n"
                     "           Z luz volumetrica (rayos de sol en el polvo)\n"
                     "           F tiempos de GPU por pasada (en esta consola)\n"
                     "           L rayos de luz\n"
                     "           K tonemapper (Neutral/ACES) | E auto-exposicion\n"
                     "           RePag/AvPag compensacion de exposicion | ESC salir\n\n";

        // 5) Bucle principal: tiempo -> eventos -> actualizacion -> dibujado.
        Clock clock;
        // Tabla de tiempos de GPU en la consola: tecla F, o desde el
        // arranque con la variable de entorno CRAMION_PROFILE.
        bool print_gpu_timings = std::getenv("CRAMION_PROFILE") != nullptr;

        // Benchmark (CRAMION_BENCH=<segundos>): la camara se queda donde la
        // pone la escena, se calienta unos segundos (carga de texturas,
        // historias temporales, auto-exposicion), se promedia y se sale.
        // Mismas condiciones siempre: sirve para comparar antes y despues.
        const char* bench_env = std::getenv("CRAMION_BENCH");
        const float bench_seconds =
            bench_env != nullptr ? std::max(1.0f, static_cast<float>(std::atof(bench_env))) : 0.0f;
        constexpr float kBenchWarmupSeconds = 5.0f;
        float bench_elapsed = 0.0f;
        float bench_measured = 0.0f;
        std::uint64_t bench_frames = 0;

        while (window.isOpen()) {
            const float delta_seconds = clock.tick();

            window.pumpEvents();

            if (input.isKeyPressed(Key::Escape)) {
                std::cout << "ESC pulsada: cerrando.\n";
                break;
            }

            // Interruptores del renderizador (las sombras son suyas, no de la
            // escena).
            if (input.isKeyPressed(Key::G)) {
                renderer.setShadowsEnabled(!renderer.shadowsEnabled());
            }
            if (input.isKeyPressed(Key::C)) {
                renderer.setCascadeDebug(!renderer.cascadeDebug());
            }
            if (input.isKeyPressed(Key::X)) {
                renderer.setAntialiasingEnabled(!renderer.antialiasingEnabled());
            }
            if (input.isKeyPressed(Key::B)) {
                renderer.setBloomEnabled(!renderer.bloomEnabled());
            }
            if (input.isKeyPressed(Key::O)) {
                renderer.setSsaoEnabled(!renderer.ssaoEnabled());
            }
            if (input.isKeyPressed(Key::R)) {
                renderer.setSsrEnabled(!renderer.ssrEnabled());
            }
            // Cielo fotografiado <-> cielo fisico (con su ciclo de dia).
            if (input.isKeyPressed(Key::U) && renderer.environmentLoaded()) {
                renderer.setEnvironmentEnabled(!renderer.environmentEnabled());
                scene.setFixedSun(renderer.environmentEnabled()
                                      ? std::optional<cramion::core::Vec3>(
                                            renderer.environmentSunDirection())
                                      : std::nullopt);
            }
            if (input.isKeyPressed(Key::M)) {
                renderer.setWaterEnabled(!renderer.waterEnabled());
            }
            if (input.isKeyPressed(Key::J)) {
                renderer.setRainEnabled(!renderer.rainEnabled());
            }
            if (input.isKeyPressed(Key::Z)) {
                renderer.setVolumetricEnabled(!renderer.volumetricEnabled());
            }
            if (input.isKeyPressed(Key::F)) {
                print_gpu_timings = !print_gpu_timings;
            }
            if (input.isKeyPressed(Key::Y)) {
                renderer.setRayTracingEnabled(!renderer.rayTracingEnabled());
            }
            if (input.isKeyPressed(Key::H)) {
                renderer.setOcclusionCullingEnabled(!renderer.occlusionCullingEnabled());
            }
            if (input.isKeyPressed(Key::V)) {
                renderer.setCloudsEnabled(!renderer.cloudsEnabled());
            }
            if (input.isKeyPressed(Key::P)) {
                renderer.setReflectionProbeEnabled(!renderer.reflectionProbeEnabled());
            }
            if (input.isKeyPressed(Key::I)) {
                renderer.setGiEnabled(!renderer.giEnabled());
            }
            if (input.isKeyPressed(Key::K)) {
                renderer.setAcesTonemapper(!renderer.acesTonemapper());
            }
            if (input.isKeyPressed(Key::L)) {
                renderer.setLightShaftsEnabled(!renderer.lightShaftsEnabled());
            }
            if (input.isKeyPressed(Key::E)) {
                renderer.setAutoExposureEnabled(!renderer.autoExposureEnabled());
            }
            if (input.isKeyPressed(Key::PageUp)) {
                renderer.setExposureCompensation(
                    std::min(renderer.exposureCompensation() + 0.5f, 4.0f));
            }
            if (input.isKeyPressed(Key::PageDown)) {
                renderer.setExposureCompensation(
                    std::max(renderer.exposureCompensation() - 0.5f, -4.0f));
            }

            scene.update(input, delta_seconds);
            renderer.drawFrame(scene);

            if (clock.fpsUpdated()) {
                updateWindowTitle(window, clock, scene, renderer);
                if (print_gpu_timings) {
                    printGpuTimings(renderer);
                }
            }

            input.newFrame();  // Limpiar estados de un solo frame.

            if (bench_seconds > 0.0f) {
                bench_elapsed += delta_seconds;
                if (bench_elapsed > kBenchWarmupSeconds) {
                    bench_measured += delta_seconds;
                    ++bench_frames;
                }
                if (bench_measured >= bench_seconds) {
                    std::cout << "=== BENCHMARK " << entry.name << ": " << bench_frames
                              << " frames en " << std::fixed << std::setprecision(2)
                              << bench_measured << " s -> "
                              << static_cast<float>(bench_frames) / bench_measured << " FPS, "
                              << 1000.0f * bench_measured / static_cast<float>(bench_frames)
                              << " ms por frame ===\n";
                    printGpuTimings(renderer);
                    break;
                }
            }
        }

        std::cout << "Frames dibujados: " << clock.frameCount() << " en " << std::fixed
                  << std::setprecision(2) << clock.totalSeconds() << " s\n";

        // 6) Apagado ordenado: primero la GPU, luego la ventana.
        renderer.shutdown();
        window.destroy();

        return EXIT_SUCCESS;
    } catch (const std::exception& ex) {
        std::cerr << "Error fatal: " << ex.what() << "\n";
        return EXIT_FAILURE;
    }
}
