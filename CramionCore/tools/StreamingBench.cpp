// Banco de pruebas del streaming por prefabs (sin ventana visible): carga un
// proyecto (por defecto Overthrow_Arena), y hace lo que hace un juego que
// carga celdas alrededor de la camara (Prefabs/Maps/Ashbound/<i>_<j>.crprefab
// con Scene::instantiate / setActive / destroy), con el bucle de frames de
// verdad (RenderSync + VulkanRenderer::drawFrame), y mide:
//
//   - el tiempo de cada frame (max, p99, p95, p50) por fase y en total, con
//     el reparto en el hilo principal: leer el prefab, instanciar, activar /
//     desactivar, destruir, RenderSync y drawFrame (y dentro de drawFrame lo
//     de FrameTimings: actores, subidas, espera a la GPU, grabar);
//   - en los frames peores, las zonas del perfilador con mas tiempo propio;
//   - memoria de la CPU (privada y working set) y de la GPU (VMA por tipo y
//     VK_EXT_memory_budget) al empezar y al final de cada ciclo: si sube
//     ciclo a ciclo con la escena vacia, es una fuga.
//
//   cramion_stream_bench [--project carpeta] [--cycles N] [--block i,j,w,h]...
//                        [--cooldown F] [--settle F] [--width W] [--height H]
//                        [--no-render] [--no-base] [--csv archivo]
//                        [--spike-ms M] [--batch K] [--rt 0|1] [--no-foliage] [--validation] [--vsync]
//                        [--prefab Prefabs/Maps/Valley.crprefab] [--async [ms]]
//
// --async: instanciar como Scene.instantiateAsync (prefab parseado una vez en
// los hilos de IO y entidades creadas por tandas con un presupuesto de ms por
// frame, 2 por defecto, bajo una raiz desactivada).
//
// --block: un rectangulo de celdas (se puede repetir). Por defecto, el 3x3
// mas denso de Ashbound (-10,-11) y un 3x3 con luces y efectos (-9,-20).
// --cooldown: frames entre dos instanciaciones (el juego: 3).
// --batch: celdas por instanciacion (1 como el juego; mas = el peor caso,
// p. ej. un salto de camara con warm_).
// --no-render: sin GPU, solo el coste de instanciar y destruir (y del
// RenderSync no: necesita el renderizador).
// No escribe nada en el proyecto: la cache del AssetManager va a %TEMP%.

#include <CramionCore/CramionCore.h>
#include <CramionCore/asset/AssetDatabase.h>
#include <CramionCore/asset/AssetManager.h>
#include <CramionCore/audio/Audio.h>
#include <CramionCore/cvar/CVar.h>
#include <CramionCore/ecs/Prefab.h>
#include <CramionCore/ecs/PrefabSpawn.h>
#include <CramionCore/jobs/JobSystem.h>
#include <CramionCore/ecs/RenderSync.h>
#include <CramionCore/fire/Fire.h>
#include <CramionCore/fluid/Fluid.h>
#include <CramionCore/foliage/Foliage.h>
#include <CramionCore/navigation/Navigation.h>
#include <CramionCore/profiling/Profiler.h>
#include <CramionCore/scripting/Scripting.h>
#include <CramionCore/terrain/Terrain.h>
#include <CramionCore/ui/UI.h>
#include <CramionCore/vfx/VisualEffect.h>
#include <CramionCore/video/VideoPlayer.h>
#include <CramionCore/voxel/Voxel.h>
#include <CramionCore/water/Water.h>
#include <CramionCore/xr/XrRig.h>
#include <CramionDM/Input.h>
#include <CramionDM/Window.h>
#include <CramionFX/scene/Scene.h>
#include <CramionFX/vk/GpuMemoryStats.h>
#include <CramionFX/vk/VulkanRenderer.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>
#endif

using namespace cramion;

namespace {

constexpr float kCellMeters = 81.92f;  // kAshboundCellSize (8192 unidades del juego) / 100

struct Block {
    int i = 0, j = 0, w = 3, h = 3;
};

struct Options {
    std::filesystem::path project = "C:/Users/tupap/OneDrive/Documentos/Cramion Projects/Overthrow_Arena";
    std::string cell_folder = "Prefabs/Maps/Ashbound";
    std::vector<Block> blocks;
    int cycles = 20;
    int cooldown = 3;
    int settle = 90;
    int batch = 1;
    std::uint32_t width = 1280, height = 720;
    bool render = true;
    bool base = true;
    int rt = -1;            // -1 = lo de la GPU
    bool validation = false;
    bool vsync = false;  // --vsync: como se juega (FIFO); por defecto apagado para medir el coste
    std::string single_prefab;  // --prefab: un solo prefab grande (Valley, ChaosProps) en vez de celdas
    bool foliage = true;    // false: quita el Foliage de cada celda al instanciarla (aislar su coste)
    double async_ms = -1.0;  // --async: presupuesto por frame de PrefabSpawn (<0 = instantiatePrefab de una vez)
    double spike_ms = 50.0;
    std::filesystem::path csv;
};

// Diagnostico de cierres: la fase en curso y el motivo (excepcion sin capturar,
// abort) salen por stderr antes de que el proceso muera.
const char* volatile g_phase = "init";
void onTerminate() {
    std::fprintf(stderr, "\n[Bench] std::terminate en la fase %s", g_phase);
    if (std::exception_ptr e = std::current_exception()) {
        try {
            std::rethrow_exception(e);
        } catch (const std::exception& error) {
            std::fprintf(stderr, ": %s", error.what());
        } catch (...) {
            std::fprintf(stderr, ": excepcion desconocida");
        }
    }
    std::fprintf(stderr, "\n");
    std::fflush(stderr);
    std::abort();
}
void onAbort(int) {
    std::fprintf(stderr, "\n[Bench] abort() en la fase %s\n", g_phase);
    std::fflush(stderr);
}

double nowMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

struct CpuMem {
    double private_mb = 0, working_mb = 0;
};
CpuMem cpuMemory() {
    CpuMem m;
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
        m.private_mb = static_cast<double>(pmc.PrivateUsage) / (1024.0 * 1024.0);
        m.working_mb = static_cast<double>(pmc.WorkingSetSize) / (1024.0 * 1024.0);
    }
#endif
    return m;
}

struct MemSnapshot {
    std::string label;
    CpuMem cpu;
    double vram_used_mb = -1, vram_budget_mb = -1;
    double tex_mb = 0, img_mb = 0, buf_mb = 0, staging_mb = 0;
    std::size_t entities = 0, actors = 0, models = 0, uploaded = 0, lights = 0;
};

struct FrameRecord {
    int cycle = 0;
    std::string phase;
    double total = 0, read = 0, inst = 0, toggle = 0, destroy = 0, sync = 0, draw = 0;
    double fx_actors = 0, fx_upload = 0, fx_fence = 0, fx_record = 0, fx_submit = 0;
    // Dentro de drawFrame: imagen de la swapchain, uniformes, sonda y, dentro
    // de los actores, streaming, culling, escena de rayos y TLAS; fuera, lo
    // subido en segundo plano (vegetacion).
    double fx_acquire = 0, fx_uniforms = 0, fx_probe = 0, fx_stream = 0, fx_culling = 0, fx_rt = 0, fx_tlas = 0,
           fx_poll = 0;
    int fx_uploads = 0;
    std::size_t actors = 0;
    std::string top;  // zonas del perfilador (solo en los frames lentos)
};

struct Stats {
    double max = 0, p99 = 0, p95 = 0, p50 = 0, avg = 0;
    int n = 0, over16 = 0, over33 = 0, over50 = 0, over100 = 0;
};
Stats statsOf(std::vector<double> v) {
    Stats s;
    s.n = static_cast<int>(v.size());
    if (v.empty()) return s;
    std::sort(v.begin(), v.end());
    const auto pct = [&](double p) {
        const std::size_t k = std::min(v.size() - 1, static_cast<std::size_t>(std::ceil(p * static_cast<double>(v.size())) - 1));
        return v[k];
    };
    s.max = v.back();
    s.p99 = pct(0.99);
    s.p95 = pct(0.95);
    s.p50 = pct(0.50);
    double sum = 0;
    for (double x : v) {
        sum += x;
        if (x > 16.7) ++s.over16;
        if (x > 33.3) ++s.over33;
        if (x > 50) ++s.over50;
        if (x > 100) ++s.over100;
    }
    s.avg = sum / static_cast<double>(v.size());
    return s;
}

void printStats(const std::string& name, const Stats& s) {
    std::cout << "  " << std::left << std::setw(26) << name << std::right << std::fixed << std::setprecision(2)
              << " n=" << std::setw(5) << s.n << "  avg " << std::setw(7) << s.avg << "  p50 " << std::setw(7) << s.p50
              << "  p95 " << std::setw(7) << s.p95 << "  p99 " << std::setw(7) << s.p99 << "  max " << std::setw(8)
              << s.max << "  >16.7:" << s.over16 << " >33:" << s.over33 << " >50:" << s.over50 << " >100:" << s.over100
              << "\n";
}

bool parseBlock(const std::string& text, Block& b) {
    std::istringstream in(text);
    char c1 = 0, c2 = 0, c3 = 0;
    return static_cast<bool>(in >> b.i >> c1 >> b.j >> c2 >> b.w >> c3 >> b.h) && c1 == ',' && c2 == ',' && c3 == ',';
}

}  // namespace

int main(int argc, char** argv) {
    std::cout << std::unitbuf;  // que un cierre no pierda lo escrito
    std::set_terminate(onTerminate);
    std::signal(SIGABRT, onAbort);
    Options options;
    for (int a = 1; a < argc; ++a) {
        const std::string arg = argv[a];
        const auto next = [&]() -> std::string { return a + 1 < argc ? argv[++a] : std::string{}; };
        if (arg == "--project") options.project = next();
        else if (arg == "--cells") options.cell_folder = next();
        else if (arg == "--cycles") options.cycles = std::max(1, std::atoi(next().c_str()));
        else if (arg == "--cooldown") options.cooldown = std::max(0, std::atoi(next().c_str()));
        else if (arg == "--settle") options.settle = std::max(1, std::atoi(next().c_str()));
        else if (arg == "--batch") options.batch = std::max(1, std::atoi(next().c_str()));
        else if (arg == "--width") options.width = static_cast<std::uint32_t>(std::atoi(next().c_str()));
        else if (arg == "--height") options.height = static_cast<std::uint32_t>(std::atoi(next().c_str()));
        else if (arg == "--no-render") options.render = false;
        else if (arg == "--no-base") options.base = false;
        else if (arg == "--rt") options.rt = std::atoi(next().c_str());
        else if (arg == "--no-foliage") options.foliage = false;
        else if (arg == "--async") {
            options.async_ms = 2.0;
            if (a + 1 < argc && argv[a + 1][0] != '-') options.async_ms = std::atof(argv[++a]);
        }
        else if (arg == "--validation") options.validation = true;
        else if (arg == "--vsync") options.vsync = true;
        else if (arg == "--prefab") options.single_prefab = next();
        else if (arg == "--spike-ms") options.spike_ms = std::atof(next().c_str());
        else if (arg == "--csv") options.csv = next();
        else if (arg == "--cvar") {  // --cvar nombre=valor (repetible)
            const std::string pair = next();
            const auto eq = pair.find('=');
            std::string error;
            if (eq == std::string::npos ||
                !cramion::cvar::Registry::instance().set(pair.substr(0, eq), pair.substr(eq + 1), &error, true)) {
                std::cerr << "--cvar " << pair << ": " << error << "\n";
                return 2;
            }
        }
        else if (arg == "--block") {
            Block b;
            if (!parseBlock(next(), b)) {
                std::cerr << "--block i,j,w,h\n";
                return 2;
            }
            options.blocks.push_back(b);
        } else {
            std::cerr << "Opcion desconocida: " << arg << "\n";
            return 2;
        }
    }
    if (options.blocks.empty()) {
        options.blocks.push_back({-10, -11, 3, 3});  // el 3x3 mas denso (~8.500 entidades)
        options.blocks.push_back({-9, -20, 3, 3});   // 3x3 con luces y efectos (~2.800, 60 luces/VFX)
    }
    const std::filesystem::path assets_root = options.project / "Assets";
    if (!std::filesystem::is_directory(assets_root)) {
        std::cerr << "No existe " << assets_root.string() << "\n";
        return 2;
    }

    try {
        // --- Componentes (los del reproductor) ---
        ecs::registerPrefabComponents();
        terrain::registerTerrainComponents();
        water::registerWaterComponents();
        foliage::registerFoliageComponents();
        fire::registerFireComponents();
        fluid::registerFluidComponents();
        navigation::registerNavigationComponents();
        voxel::registerVoxelComponents();
        audio::registerAudioComponents();
        video::registerVideoComponents();
        scripting::registerScriptComponents();
        ui::registerUiComponents();
        xr::registerXrComponents();
        vfx::registerVfxComponents();

        prof::setEnabled(true);

        const double t_db = nowMs();
        assets::AssetDatabase database;
        database.open(assets_root);
        std::cout << "[Bench] Base de datos de assets: " << std::fixed << std::setprecision(0) << (nowMs() - t_db)
                  << " ms\n";
        assets::AssetManager asset_manager(database);
        asset_manager.setCacheFolder(std::filesystem::temp_directory_path() / "Cramion" / "StreamingBench");

        // --- Celdas ---
        struct CellFile {
            int i, j, block;
            std::filesystem::path file;
            std::size_t bytes = 0;
        };
        std::vector<CellFile> cells;
        if (!options.single_prefab.empty()) {
            options.blocks = {Block{0, 0, 1, 1}};
            const std::filesystem::path f = assets_root / options.single_prefab;
            std::error_code ec;
            if (std::filesystem::is_regular_file(f, ec))
                cells.push_back({0, 0, 0, f, static_cast<std::size_t>(std::filesystem::file_size(f, ec))});
        }
        for (std::size_t b = 0; b < options.blocks.size() && options.single_prefab.empty(); ++b) {
            const Block& bl = options.blocks[b];
            for (int i = bl.i; i < bl.i + bl.w; ++i)
                for (int j = bl.j; j < bl.j + bl.h; ++j) {
                    std::filesystem::path f = assets_root / options.cell_folder /
                                              (std::to_string(i) + "_" + std::to_string(j) + ".crprefab");
                    std::error_code ec;
                    if (!std::filesystem::is_regular_file(f, ec)) continue;
                    cells.push_back({i, j, static_cast<int>(b), f, static_cast<std::size_t>(std::filesystem::file_size(f, ec))});
                }
        }
        if (cells.empty()) {
            std::cerr << "No hay celdas en los bloques pedidos\n";
            return 2;
        }
        std::size_t total_bytes = 0;
        for (const CellFile& c : cells) total_bytes += c.bytes;
        std::cout << "[Bench] " << cells.size() << " celdas, " << (total_bytes >> 10) << " KB de prefab\n";

        // --- Renderizador (ventana invisible) ---
        std::optional<dm::Window> window;
        std::unique_ptr<gfx::VulkanRenderer> renderer;
        scene::Scene scene;
        if (options.render) {
            window.emplace();
            if (!window->create({.title = L"Cramion streaming bench", .width = options.width, .height = options.height,
                                 .resizable = false, .visible = false})) {
                std::cerr << "No se pudo crear la ventana\n";
                return 1;
            }
            gfx::EngineInfo info;
            info.app_name = "cramion_stream_bench";
            info.enable_validation = options.validation;
            renderer = std::make_unique<gfx::VulkanRenderer>();
            renderer->initialize(info, window->handle(), window->width(), window->height());
            renderer->setEditorHelpersEnabled(false);
            gfx::GraphicsSettings graphics = renderer->graphicsSettings();
            graphics.vsync = options.vsync;
            graphics.adaptive = false;  // que el presupuesto no cambie la calidad durante la medida
            renderer->setGraphicsSettings(graphics);
            renderer->setBudgetSuspended(true);
            if (options.rt >= 0) renderer->setRayTracingEnabled(options.rt != 0 && renderer->rayTracingSupported());
            std::cout << "[Bench] Trazado de rayos: " << (renderer->rayTracingEnabled() && renderer->rayTracingSupported() ? "si" : "no") << "\n";
            std::cout << "[Bench] GPU: " << renderer->device().name() << "\n";
        }
        scene.initialize();
        scene.setDayCycleEnabled(false);
        scene.setTimeOfDayHours(15.5f);
        scene.camera().setAspectRatio(static_cast<float>(options.width) / static_cast<float>(std::max(options.height, 1u)));

        ecs::World world;
        ecs::RenderSync sync(asset_manager);
        sync.reset(scene);
        terrain::TerrainStore terrains;
        terrains.setRoot(assets_root);
        sync.setTerrainStore(&terrains);
        ecs::RenderSync::Options sync_options;
        sync_options.apply_main_camera = false;

        const dm::Input input;
        std::vector<FrameRecord> frames;
        std::map<std::string, double> warm_zones;  // tiempo propio por zona en los ciclos 2+
        int warm_frames = 0;
        std::vector<MemSnapshot> mems;
        std::string phase = "init";
        int cycle = 0;

        const auto snapshot = [&](const std::string& label) {
            MemSnapshot s;
            s.label = label;
            s.cpu = cpuMemory();
            constexpr double mb = 1024.0 * 1024.0;
            s.tex_mb = static_cast<double>(gfx::gpuMemoryBytes(gfx::GpuMemoryKind::Texture)) / mb;
            s.img_mb = static_cast<double>(gfx::gpuMemoryBytes(gfx::GpuMemoryKind::Image)) / mb;
            s.buf_mb = static_cast<double>(gfx::gpuMemoryBytes(gfx::GpuMemoryKind::Buffer)) / mb;
            s.staging_mb = static_cast<double>(gfx::gpuMemoryBytes(gfx::GpuMemoryKind::Staging)) / mb;
            if (renderer) {
                std::uint64_t used = 0, budget = 0;
                if (renderer->device().videoMemory(used, budget)) {
                    s.vram_used_mb = static_cast<double>(used) / mb;
                    s.vram_budget_mb = static_cast<double>(budget) / mb;
                }
                s.uploaded = renderer->uploadedModelCount();
            }
            s.entities = world.entityCount();
            s.actors = scene.actors().size();
            s.models = scene.models().size();
            s.lights = scene.lights().points.size() + scene.lights().spots.size();
            mems.push_back(s);
        };

        // --async: las instanciaciones en marcha avanzan cada frame (como
        // Scene.instantiateAsync en la fase Begin de los scripts).
        std::function<void()> pump_spawns;
        // Un frame del juego. `work` es lo que hace el script en ese frame.
        const auto frame = [&](const std::function<void(FrameRecord&)>& work) {
            FrameRecord r;
            r.cycle = cycle;
            r.phase = phase;
            g_phase = prof::intern(phase + " (ciclo " + std::to_string(cycle + 1) + ")");
            prof::beginFrame();
            const double t0 = nowMs();
            if (window) window->pumpEvents();
            if (renderer) renderer->applyPendingResize();
            if (work) {
                CR_PROFILE_SCOPE("Bench: script (instanciar/destruir)");
                work(r);
            }
            if (pump_spawns) {
                CR_PROFILE_SCOPE("Bench: PrefabSpawn");
                const double ti = nowMs();
                pump_spawns();
                r.inst += nowMs() - ti;
            }
            scene.update(input, 1.0f / 60.0f);
            if (renderer) {
                const double ts = nowMs();
                {
                    CR_PROFILE_SCOPE("Bench: RenderSync");
                    sync.sync(world, scene, *renderer, 1.0f / 60.0f, sync_options);
                }
                r.sync = nowMs() - ts;
                const double td = nowMs();
                {
                    CR_PROFILE_SCOPE("Render (CPU)");
                    renderer->drawFrame(scene);
                }
                r.draw = nowMs() - td;
                const gfx::VulkanRenderer::FrameTimings ft = renderer->takeFrameTimings();
                r.fx_actors = ft.actors_ms;
                r.fx_upload = ft.upload_ms;
                r.fx_uploads = ft.uploads;
                r.fx_fence = ft.fence_wait_ms;
                r.fx_record = ft.record_ms;
                r.fx_submit = ft.submit_ms;
                r.fx_acquire = ft.acquire_ms;
                r.fx_uniforms = ft.uniforms_ms;
                r.fx_probe = ft.probe_ms;
                r.fx_stream = ft.stream_ms;
                r.fx_culling = ft.culling_ms;
                r.fx_rt = ft.rt_scene_ms;
                r.fx_tlas = ft.rt_tlas_ms;
                r.fx_poll = ft.async_poll_ms;
            }
            r.total = nowMs() - t0;
            r.actors = scene.actors().size();
            prof::endFrame();
            if (cycle > 0) {
                ++warm_frames;
                for (const prof::ZoneCost& z : prof::topZones(1, 64)) warm_zones[z.name] += z.self_ms;
            }
            if (r.total > options.spike_ms) {
                std::ostringstream top;
                top << std::fixed << std::setprecision(1);
                for (const prof::ZoneCost& z : prof::topZones(1, 6)) top << z.name << "=" << z.self_ms << "; ";
                r.top = top.str();
            }
            frames.push_back(std::move(r));
        };
        const auto idle = [&](int n) {
            for (int k = 0; k < n; ++k) frame({});
        };
        // Espera a que lleguen los modelos pedidos (y unos frames mas para las
        // subidas a la GPU), con tope.
        const auto settle = [&](int n) {
            int extra = 0;
            for (int k = 0; k < 2000 && extra < n; ++k) {
                frame({});
                if (asset_manager.loadsInFlight() == 0) ++extra;
            }
        };

        std::map<std::string, std::string> prefab_cache;  // como ScriptSystem::prefabText (cache de por vida)
        const auto prefabText = [&](const std::filesystem::path& f) -> const std::string& {
            std::string& t = prefab_cache[f.string()];
            if (t.empty()) t = ecs::readPrefabFile(f);
            return t;
        };

        const auto placeCameraOver = [&](const Block& b) {
            const float cx = (static_cast<float>(b.i) + static_cast<float>(b.w) * 0.5f) * kCellMeters;
            const float cz = (static_cast<float>(b.j) + static_cast<float>(b.h) * 0.5f) * kCellMeters;
            // Camara de MOBA: alta y hacia delante, todo el bloque a la vista.
            scene.placeCamera(core::Vec3{cx, 70.0f, cz - 90.0f}, core::Vec3{cx, 0.0f, cz});
        };

        // --- Arranque ---
        phase = "warmup";
        idle(30);
        ecs::Entity base;
        if (options.base) {
            const std::filesystem::path base_file = assets_root / "Prefabs/Maps/Ashbound.crprefab";
            if (std::filesystem::exists(base_file)) {
                phase = "base";
                frame([&](FrameRecord& r) {
                    const double t = nowMs();
                    base = ecs::instantiatePrefab(world, ecs::readPrefabFile(base_file));
                    r.inst = nowMs() - t;
                });
                settle(60);
            }
        }
        ecs::Entity cells_root = world.create("AshboundCells");
        placeCameraOver(options.blocks.front());
        phase = "warmup";
        settle(60);
        snapshot("inicio (mapa base, sin celdas)");

        // --- Ciclos ---
        std::vector<ecs::Entity> live(cells.size());
        // --async: documentos parseados en los hilos de IO (una vez, como la
        // cache de Scene.instantiateAsync) y las instanciaciones en marcha.
        std::map<std::string, std::shared_ptr<const ecs::PrefabDocument>> documents;
        std::vector<std::pair<std::size_t, std::unique_ptr<ecs::PrefabSpawn>>> spawning;
        if (options.async_ms >= 0.0) {
            std::vector<std::shared_ptr<std::shared_ptr<const ecs::PrefabDocument>>> slots;
            std::vector<jobs::JobHandle> handles;
            for (const auto& cell : cells) {
                auto slot = std::make_shared<std::shared_ptr<const ecs::PrefabDocument>>();
                const std::filesystem::path file = cell.file;
                handles.push_back(jobs::scheduleIo([slot, file]() { *slot = ecs::parsePrefabDocument(ecs::readPrefabFile(file)); }));
                slots.push_back(slot);
            }
            for (const jobs::JobHandle& h : handles) jobs::wait(h);
            for (std::size_t k = 0; k < cells.size(); ++k) documents[cells[k].file.string()] = *slots[k];
            pump_spawns = [&]() {
                const double start = nowMs();
                for (auto it = spawning.begin(); it != spawning.end();) {
                    const double left = options.async_ms - (nowMs() - start);
                    if (left <= 0.0 && it != spawning.begin()) break;
                    if (!it->second->step(world, std::max(left, 0.0))) break;
                    ecs::Entity e = it->second->root();
                    if (!options.foliage && e.valid()) {
                        std::vector<entt::entity> with;
                        for (const entt::entity h : world.registry().view<foliage::Foliage>()) with.push_back(h);
                        for (const entt::entity h : with) world.wrap(h).remove<foliage::Foliage>();
                    }
                    live[it->first] = e;
                    it = spawning.erase(it);
                }
            };
        }
        for (cycle = 0; cycle < options.cycles; ++cycle) {
            for (std::size_t b = 0; b < options.blocks.size(); ++b) {
                placeCameraOver(options.blocks[b]);
                std::vector<std::size_t> mine;
                for (std::size_t k = 0; k < cells.size(); ++k)
                    if (cells[k].block == static_cast<int>(b)) mine.push_back(k);

                // 1) Instanciar (una cada `cooldown` frames, como Maps.h).
                phase = "instantiate";
                for (std::size_t n = 0; n < mine.size(); n += static_cast<std::size_t>(options.batch)) {
                    frame([&](FrameRecord& r) {
                        for (std::size_t m = n; m < std::min(mine.size(), n + static_cast<std::size_t>(options.batch)); ++m) {
                            const std::size_t k = mine[m];
                            if (options.async_ms >= 0.0) {
                                const auto doc = documents.find(cells[k].file.string());
                                if (doc != documents.end() && doc->second) {
                                    spawning.emplace_back(k, std::make_unique<ecs::PrefabSpawn>(doc->second, cells_root));
                                }
                                continue;
                            }
                            const double tr = nowMs();
                            const std::string& text = prefabText(cells[k].file);
                            r.read += nowMs() - tr;
                            const double ti = nowMs();
                            ecs::Entity e = ecs::instantiatePrefab(world, text);
                            if (e.valid()) e.setParent(cells_root);
                            if (!options.foliage && e.valid()) {
                                std::vector<entt::entity> with;
                                for (const entt::entity h : world.registry().view<foliage::Foliage>()) with.push_back(h);
                                for (const entt::entity h : with) world.wrap(h).remove<foliage::Foliage>();
                            }
                            r.inst += nowMs() - ti;
                            live[k] = e;
                        }
                    });
                    phase = "after-instantiate";
                    idle(options.cooldown);
                    phase = "instantiate";
                }
                // --async: que terminen las que quedan (cuenta como after-instantiate).
                phase = "after-instantiate";
                for (int guard = 0; !spawning.empty() && guard < 10000; ++guard) frame({});
                phase = "settle";
                settle(options.settle);

                // 2) Desactivar (dos por frame, como Maps.h) y reactivar (una por frame).
                phase = "deactivate";
                for (std::size_t n = 0; n < mine.size(); n += 2) {
                    frame([&](FrameRecord& r) {
                        const double t = nowMs();
                        for (std::size_t m = n; m < std::min(mine.size(), n + 2); ++m)
                            if (live[mine[m]].valid()) live[mine[m]].setActive(false);
                        r.toggle = nowMs() - t;
                    });
                }
                phase = "settle";
                idle(20);
                phase = "reactivate";
                for (std::size_t n = 0; n < mine.size(); ++n) {
                    frame([&](FrameRecord& r) {
                        const double t = nowMs();
                        if (live[mine[n]].valid()) live[mine[n]].setActive(true);
                        r.toggle = nowMs() - t;
                    });
                    phase = "after-reactivate";
                    idle(1);
                    phase = "reactivate";
                }
                phase = "settle";
                idle(20);

                // 3) Destruir (una por frame).
                phase = "destroy";
                for (std::size_t n = 0; n < mine.size(); ++n) {
                    frame([&](FrameRecord& r) {
                        const double t = nowMs();
                        if (live[mine[n]].valid()) world.destroy(live[mine[n]]);
                        live[mine[n]] = {};
                        r.destroy = nowMs() - t;
                    });
                    phase = "after-destroy";
                    idle(1);
                    phase = "destroy";
                }
                phase = "settle";
                settle(30);
            }
            // Fin de ciclo: sin celdas; espera a que se suelte lo que el
            // renderizador expulsa por tiempo (unos segundos de frames).
            phase = "idle";
            idle(120);
            snapshot("fin del ciclo " + std::to_string(cycle + 1));
            const MemSnapshot& s = mems.back();
            std::cout << "[Bench] ciclo " << (cycle + 1) << "/" << options.cycles << ": RAM privada " << std::setprecision(1)
                      << s.cpu.private_mb << " MB, VRAM " << s.vram_used_mb << " MB, entidades " << s.entities
                      << ", actores " << s.actors << ", modelos " << s.models << "\n";
        }
        phase = "end";

        // --- Resultados ---
        std::cout << std::fixed << std::setprecision(2);
        // Tiempo propio medio por zona del perfilador en los ciclos en caliente:
        // para comparar dos versiones aunque no haya tirones.
        {
            std::vector<std::pair<std::string, double>> zones(warm_zones.begin(), warm_zones.end());
            std::sort(zones.begin(), zones.end(), [](const auto& x, const auto& y) { return x.second > y.second; });
            std::cout << "\n=== Zonas en caliente (ms por frame, tiempo propio, " << warm_frames << " frames) ===\n";
            for (std::size_t i = 0; i < zones.size() && i < 25; ++i) {
                std::cout << "  " << std::left << std::setw(48) << zones[i].first << std::right << std::setprecision(3)
                          << zones[i].second / std::max(warm_frames, 1) << "\n";
            }
            std::cout << std::setprecision(2);
        }
        std::cout << "\n=== Frames (ms, hilo principal; vsync " << (options.vsync ? "activado" : "apagado") << ") ===\n";
        const auto collect = [&](auto pred, auto field) {
            std::vector<double> v;
            for (const FrameRecord& f : frames)
                if (pred(f)) v.push_back(field(f));
            return v;
        };
        const auto total = [](const FrameRecord& f) { return f.total; };
        const auto warm = [](const FrameRecord& f) { return f.cycle >= 1 && f.phase != "end"; };
        const auto cold = [](const FrameRecord& f) { return f.cycle == 0 && f.phase != "warmup" && f.phase != "init" && f.phase != "base"; };
        printStats("Ciclo 1 (frio) todo", statsOf(collect(cold, total)));
        printStats("Ciclos 2+ (caliente) todo", statsOf(collect(warm, total)));
        for (const char* ph : {"instantiate", "after-instantiate", "settle", "deactivate", "reactivate",
                               "after-reactivate", "destroy", "after-destroy", "idle"}) {
            const std::string p = ph;
            printStats(std::string("  caliente ") + ph,
                       statsOf(collect([&](const FrameRecord& f) { return f.cycle >= 1 && f.phase == p; }, total)));
        }
        std::cout << "\n=== Reparto (caliente, ms) ===\n";
        const auto part = [&](const char* name, const char* ph, auto field) {
            const std::string p = ph;
            printStats(std::string(name) + " [" + ph + "]",
                       statsOf(collect([&](const FrameRecord& f) { return f.cycle >= 1 && f.phase == p; }, field)));
        };
        part("leer prefab", "instantiate", [](const FrameRecord& f) { return f.read; });
        part("instantiatePrefab", "instantiate", [](const FrameRecord& f) { return f.inst; });
        part("RenderSync", "instantiate", [](const FrameRecord& f) { return f.sync; });
        part("drawFrame", "instantiate", [](const FrameRecord& f) { return f.draw; });
        part("RenderSync", "after-instantiate", [](const FrameRecord& f) { return f.sync; });
        part("drawFrame", "after-instantiate", [](const FrameRecord& f) { return f.draw; });
        part("  FX actores", "after-instantiate", [](const FrameRecord& f) { return f.fx_actors; });
        part("  FX subidas", "after-instantiate", [](const FrameRecord& f) { return f.fx_upload; });
        part("  FX espera GPU", "after-instantiate", [](const FrameRecord& f) { return f.fx_fence; });
        part("  FX grabar", "after-instantiate", [](const FrameRecord& f) { return f.fx_record; });
        part("setActive", "deactivate", [](const FrameRecord& f) { return f.toggle; });
        part("RenderSync", "deactivate", [](const FrameRecord& f) { return f.sync; });
        part("drawFrame", "deactivate", [](const FrameRecord& f) { return f.draw; });
        part("setActive", "reactivate", [](const FrameRecord& f) { return f.toggle; });
        part("RenderSync", "reactivate", [](const FrameRecord& f) { return f.sync; });
        part("drawFrame", "after-reactivate", [](const FrameRecord& f) { return f.draw; });
        part("World::destroy", "destroy", [](const FrameRecord& f) { return f.destroy; });
        part("RenderSync", "destroy", [](const FrameRecord& f) { return f.sync; });
        part("drawFrame", "destroy", [](const FrameRecord& f) { return f.draw; });
        part("drawFrame", "after-destroy", [](const FrameRecord& f) { return f.draw; });
        part("RenderSync", "settle", [](const FrameRecord& f) { return f.sync; });
        part("drawFrame", "settle", [](const FrameRecord& f) { return f.draw; });

        std::cout << "\n=== Los 12 frames mas lentos ===\n";
        std::vector<const FrameRecord*> worst;
        for (const FrameRecord& f : frames)
            if (f.phase != "init" && f.phase != "warmup") worst.push_back(&f);
        std::sort(worst.begin(), worst.end(), [](const FrameRecord* a, const FrameRecord* b) { return a->total > b->total; });
        for (std::size_t k = 0; k < std::min<std::size_t>(12, worst.size()); ++k) {
            const FrameRecord& f = *worst[k];
            std::cout << "  " << std::setw(8) << f.total << " ms  ciclo " << (f.cycle + 1) << " " << std::left << std::setw(18)
                      << f.phase << std::right << " inst " << f.inst << " toggle " << f.toggle << " destroy " << f.destroy
                      << " sync " << f.sync << " draw " << f.draw << " (actores " << f.fx_actors << ", subidas "
                      << f.fx_upload << "/" << f.fx_uploads << ", GPU " << f.fx_fence << ", grabar " << f.fx_record
                      << ", imagen " << f.fx_acquire << ", enviar " << f.fx_submit << ", sonda " << f.fx_probe
                      << ", streaming " << f.fx_stream << ", culling " << f.fx_culling << ", rayos " << f.fx_rt
                      << ", TLAS " << f.fx_tlas << ", segundo plano " << f.fx_poll << ")  " << f.top << "\n";
        }
        // Tirones en caliente: cuantos frames pasan de 16,6 / 33 ms y que
        // parte del frame es la mayor en cada uno (para atribuirlos).
        {
            std::map<std::string, std::pair<int, int>> causes;  // parte -> (>16,6, >33)
            for (const FrameRecord& f : frames) {
                if (f.cycle < 1 || f.total <= 16.6) continue;
                const double other = f.draw - f.fx_actors - f.fx_upload - f.fx_fence - f.fx_record - f.fx_acquire -
                                     f.fx_submit - f.fx_probe - f.fx_poll - f.fx_uniforms;
                const std::pair<const char*, double> parts[] = {
                    {"leer prefab", f.read},
                    {"instanciar", f.inst},
                    {"setActive", f.toggle},
                    {"World::destroy", f.destroy},
                    {"RenderSync", f.sync},
                    {"FX streaming", f.fx_stream},
                    {"FX culling", f.fx_culling},
                    {"FX escena de rayos", f.fx_rt},
                    {"FX TLAS", f.fx_tlas},
                    {"FX actores (resto)", f.fx_actors - f.fx_stream - f.fx_culling - f.fx_rt - f.fx_tlas},
                    {"FX subidas", f.fx_upload},
                    {"FX espera GPU", f.fx_fence + f.fx_acquire},
                    {"FX grabar", f.fx_record},
                    {"FX enviar", f.fx_submit},
                    {"FX sonda", f.fx_probe},
                    {"FX uniformes", f.fx_uniforms},
                    {"FX segundo plano", f.fx_poll},
                    {"FX otros", other}};
                const auto* top = &parts[0];
                for (const auto& part : parts)
                    if (part.second > top->second) top = &part;
                auto& c = causes[top->first];
                ++c.first;
                if (f.total > 33.3) ++c.second;
            }
            std::cout << "\n=== Tirones en caliente por la parte mayor del frame (>16,6 / >33 ms) ===\n";
            for (const auto& [name, c] : causes)
                std::cout << "  " << std::left << std::setw(22) << name << std::right << " " << c.first << " / "
                          << c.second << "\n";
        }

        std::cout << "\n=== Memoria ===\n";
        for (const MemSnapshot& s : mems) {
            std::cout << "  " << std::left << std::setw(32) << s.label << std::right << std::setprecision(1)
                      << " RAM priv " << std::setw(8) << s.cpu.private_mb << " MB  WS " << std::setw(8) << s.cpu.working_mb
                      << " MB | VRAM usada " << std::setw(8) << s.vram_used_mb << " / " << s.vram_budget_mb
                      << " MB  (tex " << s.tex_mb << ", img " << s.img_mb << ", buf " << s.buf_mb << ", staging "
                      << s.staging_mb << ") | entidades " << s.entities << " actores " << s.actors << " modelos "
                      << s.models << " subidos " << s.uploaded << " luces " << s.lights << "\n";
        }
        if (mems.size() >= 3) {
            const MemSnapshot& a = mems[1];  // fin del ciclo 1 (todo ya cargado una vez)
            const MemSnapshot& z = mems.back();
            std::cout << "  Deriva ciclo 1 -> " << (mems.size() - 1) << ": RAM priv " << std::showpos
                      << (z.cpu.private_mb - a.cpu.private_mb) << " MB, VRAM " << (z.vram_used_mb - a.vram_used_mb)
                      << " MB, buf " << (z.buf_mb - a.buf_mb) << " MB, tex " << (z.tex_mb - a.tex_mb) << " MB"
                      << std::noshowpos << "\n";
        }

        if (!options.csv.empty()) {
            std::ofstream out(options.csv);
            out << "cycle,phase,total,read,inst,toggle,destroy,sync,draw,fx_actors,fx_upload,fx_uploads,fx_fence,fx_record,fx_submit,actors,top\n";
            for (const FrameRecord& f : frames) {
                out << f.cycle << "," << f.phase << "," << f.total << "," << f.read << "," << f.inst << "," << f.toggle << ","
                    << f.destroy << "," << f.sync << "," << f.draw << "," << f.fx_actors << "," << f.fx_upload << ","
                    << f.fx_uploads << "," << f.fx_fence << "," << f.fx_record << "," << f.fx_submit << "," << f.actors
                    << ",\"" << f.top << "\"\n";
            }
            std::cout << "CSV: " << options.csv.string() << "\n";
        }

        if (base.valid()) world.destroy(base);
        if (renderer) {
            renderer->waitIdle();
            sync.reset(scene);
            renderer->shutdown();
        }
        if (window) window->destroy();
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << "\n";
        return 1;
    }
    std::cout << "OK\n";
    return 0;
}
