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
//   RePag / AvPag      compensacion de exposicion (+-0.5 EV)
//   ESC                salir
// -----------------------------------------------------------------------------

#include <CramionDM/CramionDM.h>

#include "core/Clock.h"
#include "scene/Scene.h"
#include "vk/VulkanRenderer.h"
#include "vk/VulkanShader.h"

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

namespace {

// Escenarios disponibles. CMake extrae cada zip que encuentre en la raiz del
// proyecto a assets/<nombre>/ junto al ejecutable. Se elige por la linea de
// comandos (cramion.exe san-miguel); sin argumento, el primero.
// Ajuste de un material por su nombre en el MTL: lo que el OBJ no guarda.
struct MaterialTweak {
    const char* material;
    float roughness;
    float metallic;
    float reflectance = 0.04f;  // F0 de la parte no metalica
};

struct SceneEntry {
    const char* name;
    const char* file;             // relativo a assets/
    cramion::core::Vec3 camera;   // posicion inicial de la camara
    cramion::core::Vec3 target;   // punto al que mira
    std::span<const MaterialTweak> tweaks;
    // Normal maps de convenio DirectX (+Y hacia abajo): assets de Unreal o
    // Lumberyard. El archivo no lo indica.
    bool directx_normals = false;
};

// Sibenik: el suelo de la nave es marmol pulido (en las fotos refleja las
// columnas como un espejo), pero el MTL lo exporta mate (Ns 8, sin
// especular). Casi liso y con mas reflectancia que la piedra comun (0.04):
// asi refleja tambien mirando hacia abajo, no solo en angulo rasante.
constexpr MaterialTweak kSibenikTweaks[] = {
    {"pod", 0.05f, 0.0f, 0.12f},  // marmol pulido del suelo
    {"pod_rub", 0.3f, 0.0f},   // cenefa de piedra del borde
};

constexpr SceneEntry kScenes[] = {
    // Catedral de Sibenik (Marko Dabrovic, texturas de Morgan McGuire): en la
    // nave, cerca de la entrada, a la altura de los ojos, mirando al altar.
    {"sibenik", "sibenik/sibenik.obj", {-15.5f, -13.4f, 0.0f}, {10.0f, -11.0f, 0.0f},
     kSibenikTweaks},
    // San Miguel (Guillermo M. Leal Llaguno, version 2017 de Morgan McGuire):
    // en el paso junto a la fachada sur, mirando al centro del patio.
    {"san-miguel", "san-miguel/san-miguel.obj", {10.5f, 1.7f, -10.5f}, {12.0f, 1.8f, 2.0f}, {}},
    // Amazon Lumberyard Bistro v5.2 (NVIDIA ORCA): la calle y el interior del
    // bistro. Texturas DDS con normal maps DirectX.
    {"bistro", "bistro/Bistro_v5_2/BistroExterior.fbx", {0.0f, 2.0f, 0.0f}, {1.0f, 2.0f, 0.0f},
     {}, true},
    {"bistro-interior", "bistro/Bistro_v5_2/BistroInterior.fbx", {0.0f, 1.7f, 0.0f},
     {1.0f, 1.7f, 0.0f}, {}, true},
};

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
          << renderer.shadowSubmeshes() << L")" << L"  |  XYZ "
          << std::setprecision(1) << position.x << L" " << position.y << L" " << position.z
          << L"  |  "<< std::setprecision(0) << std::setw(2) << std::setfill(L'0')
          << static_cast<int>(scene.timeOfDayHours()) << L":" << std::setw(2)
          << static_cast<int>(std::fmod(scene.timeOfDayHours(), 1.0f) * 60.0f)
          << std::setfill(L' ') << L"  |  ciclo " << (scene.dayCycleEnabled() ? L"ON" : L"OFF") << L"  |  sombras "
          << (renderer.shadowsEnabled() ? L"ON" : L"OFF")
          << (renderer.cascadeDebug() ? L" [cascadas]" : L"") << L"  |  FXAA "
          << (renderer.antialiasingEnabled() ? L"ON" : L"OFF") << L"  |  bloom "
          << (renderer.bloomEnabled() ? L"ON" : L"OFF") << L"  |  SSAO "
          << (renderer.ssaoEnabled() ? L"ON" : L"OFF") << L"  |  SSR "
          << (renderer.ssrEnabled() ? L"ON" : L"OFF") << L"  |  sonda "
          << (renderer.reflectionProbeEnabled() ? L"ON" : L"OFF") << L"  |  GI "
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
        const std::filesystem::path scene_path =
            cramion::gfx::shaders::directory().parent_path() / "assets" / entry.file;
        std::cout << "Escena: " << entry.name << " (" << scene_path.string() << ")\n";
        // Escenario: siempre estatico (culling por trozos aunque traiga
        // animaciones de camaras u objetos).
        const std::uint32_t model = scene.loadModel(scene_path, /*force_static=*/true);
        for (const MaterialTweak& tweak : entry.tweaks) {
            scene.overrideMaterial(model, tweak.material, tweak.roughness, tweak.metallic,
                                   tweak.reflectance);
        }
        scene.setDirectXNormalMaps(model, entry.directx_normals);
        scene.spawnStatic(model);
        scene.placeCamera(entry.camera, entry.target);

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
                     "           I luz rebotada (GI)\n"
                     "           L rayos de luz\n"
                     "           K tonemapper (Neutral/ACES) | E auto-exposicion\n"
                     "           RePag/AvPag compensacion de exposicion | ESC salir\n\n";

        // 5) Bucle principal: tiempo -> eventos -> actualizacion -> dibujado.
        Clock clock;

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
            }

            input.newFrame();  // Limpiar estados de un solo frame.
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
