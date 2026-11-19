// Pruebas de la RAM de los modelos y del envio de informes de cierre (consola,
// sin GPU ni red):
//
//   - AssetManager: lo que ocupa cada modelo, presupuesto con expulsion LRU
//     (solo lo que nadie mas usa; lo recien usado se respeta), trim por nivel
//     y que lo soltado se vuelve a leer al pedirlo.
//   - MemoryPressure: avisos del sistema (el mayor gana, una vez), niveles de
//     onTrimMemory -> que se suelta, presupuesto por gama, CVars y
//     tickAssetRam (presupuesto y aviso pendiente).
//   - CrashUpload: el JSON (UTF-8 valido, recortado), direcciones permitidas,
//     la cola (sin repetidos, como mucho 8) y el envio con un "servidor" falso
//     (lo enviado sale de la cola, lo que falla se reintenta y se abandona al
//     tercer intento). Nada sale a la red.

#include <CramionCore/asset/AssetManager.h>
#include <CramionCore/asset/AssetTypes.h>
#include <CramionCore/cvar/CVar.h>
#include <CramionCore/platform/CrashUpload.h>
#include <CramionCore/platform/MemoryPressure.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
    std::cout << (ok ? "  OK    " : "  FALLO ") << what << "\n";
    if (!ok) ++g_failures;
}

using namespace cramion;

void writeFile(const std::filesystem::path& file, const std::string& text) {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << text;
}

}  // namespace

int main() {
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "cramion_ram_crash_tests";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "Assets");

    std::cout << "RAM de los modelos (AssetManager)\n";
    {
        assets::AssetDatabase database;
        database.open(root / "Assets");
        assets::AssetManager manager(database);
        const Uuid ids[] = {assets::builtin::kCube, assets::builtin::kSphere, assets::builtin::kPlane,
                            assets::builtin::kCylinder, assets::builtin::kCapsule};
        std::shared_ptr<const assets::ModelAsset> held;
        for (const Uuid& id : ids) {
            auto model = manager.loadModel(id);
            if (id == assets::builtin::kSphere) held = model;  // la usa "alguien" (como RenderSync)
        }
        assets::AssetRamStats stats = manager.ramStats();
        check(stats.models == 5 && stats.bytes > 0, "5 modelos en memoria (" + std::to_string(stats.bytes) + " bytes)");
        check(stats.evictable == 4, "4 se pueden soltar (la esfera la usa alguien)");
        check(stats.budget == 0 && manager.enforceRamBudget() == 0, "sin presupuesto no se suelta nada (como antes)");

        // Presupuesto minimo: se suelta todo lo que se puede, pero lo usado
        // hace menos de 2 s se respeta.
        manager.setRamBudget(1);
        check(manager.enforceRamBudget() == 0 && manager.ramStats().models == 5, "lo recien usado se respeta");
        // Sin respetar lo reciente: todo lo libre.
        check(manager.enforceRamBudget(0) > 0, "por encima del presupuesto se suelta");
        stats = manager.ramStats();
        check(stats.models == 1 && manager.isLoaded(assets::builtin::kSphere), "solo queda la esfera (en uso)");
        check(stats.evicted == 4 && stats.evicted_bytes > 0, "cuenta lo soltado");
        check(manager.loadModel(assets::builtin::kCube) != nullptr && manager.isLoaded(assets::builtin::kCube),
              "lo soltado se vuelve a leer al pedirlo");

        // LRU: con presupuesto para uno, se va el usado hace mas tiempo.
        manager.setRamBudget(0);
        manager.loadModel(assets::builtin::kPlane);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        manager.loadModel(assets::builtin::kCube);  // el mas reciente
        held.reset();
        const std::uint64_t cube = assets::AssetManager::modelBytes(*manager.loadModel(assets::builtin::kCube));
        const std::uint64_t sphere = assets::AssetManager::modelBytes(*manager.loadModel(assets::builtin::kSphere));
        manager.setRamBudget(cube + sphere);
        manager.enforceRamBudget(0);
        check(!manager.isLoaded(assets::builtin::kPlane) && manager.isLoaded(assets::builtin::kCube),
              "LRU: se va el plano (usado hace mas), queda el cubo");

        // trim: moderado respeta lo usado hace poco; bajo suelta todo lo libre.
        manager.setRamBudget(0);
        manager.loadModel(assets::builtin::kCylinder);
        check(manager.trim(assets::TrimLevel::Moderate, 60000) == 0 && manager.ramStats().models >= 2,
              "trim moderado: lo usado en el ultimo minuto se queda");
        check(manager.trim(assets::TrimLevel::Low) > 0 && manager.ramStats().models == 0, "trim bajo: todo lo libre fuera");
        check(manager.ramStats().trims == 2, "cuenta los avisos atendidos");
    }

    std::cout << "Avisos de memoria (MemoryPressure)\n";
    {
        check(memory::takePendingTrim() == 0, "sin avisos al empezar");
        memory::notifyTrimMemory(memory::kTrimRunningModerate);
        memory::notifyTrimMemory(memory::kTrimRunningCritical);
        memory::notifyTrimMemory(memory::kTrimUiHidden);
        check(memory::takePendingTrim() == memory::kTrimUiHidden, "gana el mayor nivel pendiente");
        check(memory::takePendingTrim() == 0, "se recoge una vez");
        memory::notifyLowMemory();
        check(memory::takePendingTrim() == memory::kLowMemory, "onLowMemory");
        check(memory::trimLevelFor(memory::kTrimRunningModerate) == assets::TrimLevel::Moderate &&
                  memory::trimLevelFor(memory::kTrimUiHidden) == assets::TrimLevel::Moderate &&
                  memory::trimLevelFor(memory::kTrimRunningLow) == assets::TrimLevel::Low &&
                  memory::trimLevelFor(memory::kTrimBackground) == assets::TrimLevel::Low &&
                  memory::trimLevelFor(memory::kTrimRunningCritical) == assets::TrimLevel::Critical &&
                  memory::trimLevelFor(memory::kTrimComplete) == assets::TrimLevel::Critical &&
                  memory::trimLevelFor(memory::kLowMemory) == assets::TrimLevel::Critical && !memory::trimLevelFor(0),
              "niveles de onTrimMemory -> que se suelta");
        constexpr std::uint64_t kGb = 1ull << 30;
        check(memory::autoAssetRamBudget(2 * kGb) == 256ull << 20 && memory::autoAssetRamBudget(4 * kGb) == 384ull << 20 &&
                  memory::autoAssetRamBudget(6 * kGb) == 512ull << 20 && memory::autoAssetRamBudget(8 * kGb) == 768ull << 20 &&
                  memory::autoAssetRamBudget(16 * kGb) == 1024ull << 20 && memory::autoAssetRamBudget(0) == 384ull << 20,
              "presupuesto por gama (2/4/6/8/16 GB)");
        check(memory::physicalMemoryBytes() > 0 && memory::processResidentBytes() > 0, "RAM del aparato y del proceso");
        check(memory::configuredAssetRamBudget() == 0, "por defecto: sin presupuesto");

        assets::AssetDatabase database;
        database.open(root / "Assets");
        assets::AssetManager manager(database);
        manager.loadModel(assets::builtin::kCube);
        manager.loadModel(assets::builtin::kSphere);
        memory::tickAssetRam(manager, true);
        check(memory::lastAssetRamStats().models == 2 && manager.ramBudget() == 0, "tick sin presupuesto: nada se suelta");
        check(cvar::Registry::instance().set("memory.AssetRamBudgetMB", "64") && memory::configuredAssetRamBudget() == 64ull << 20,
              "memory.AssetRamBudgetMB = 64");
        memory::tickAssetRam(manager, true);
        check(manager.ramBudget() == 64ull << 20 && memory::lastAssetRamStats().models == 2,
              "tick con 64 MB: cabe todo");
        cvar::Registry::instance().set("memory.AssetRamBudgetAuto", "1");
        check(memory::configuredAssetRamBudget() == memory::autoAssetRamBudget(memory::physicalMemoryBytes()),
              "memory.AssetRamBudgetAuto manda");
        cvar::Registry::instance().set("memory.AssetRamBudgetAuto", "0");
        cvar::Registry::instance().set("memory.AssetRamBudgetMB", "0");
        memory::notifyTrimMemory(memory::kTrimRunningLow);
        const std::uint64_t freed = memory::tickAssetRam(manager);
        check(freed > 0 && manager.ramStats().models == 0, "aviso pendiente: el tick suelta lo libre");
        cvar::Registry::instance().set("memory.TrimOnPressure", "0");
        manager.loadModel(assets::builtin::kCube);
        memory::notifyTrimMemory(memory::kTrimComplete);
        check(memory::tickAssetRam(manager) == 0 && manager.isLoaded(assets::builtin::kCube),
              "memory.TrimOnPressure = 0: el aviso no suelta nada");
        cvar::Registry::instance().set("memory.TrimOnPressure", "1");
    }

    std::cout << "Informes de cierre (CrashUpload, sin red)\n";
    {
        const std::filesystem::path crashes = root / "Crashes";
        std::filesystem::create_directories(crashes);
        crash::UploadConfig config;
        config.app = "MiJuego";
        config.version = "1.2.3";
        config.max_bytes = 64;
        // Texto con un byte suelto que no es UTF-8.
        const std::string text = std::string("Motivo: acceso no valido ") + char(0xE9) + " fin";
        const nlohmann::json body = nlohmann::json::parse(crash::buildUploadBody(config, text, "MiJuego_1.txt"));
        check(body["format"] == "cramion-crash-1" && body["app"] == "MiJuego" && body["version"] == "1.2.3" &&
                  body["platform"] == crash::platformName() && body["report_name"] == "MiJuego_1.txt",
              "JSON con formato, juego, version, plataforma y nombre");
        check(body["report"].get<std::string>().find('?') != std::string::npos, "UTF-8 no valido -> '?'");
        const std::string long_text(500, 'x');
        check(nlohmann::json::parse(crash::buildUploadBody(config, long_text + "FINAL", "a"))["report"].get<std::string>().size() == 64,
              "recortado a max_bytes (lo ultimo)");

        const std::filesystem::path report1 = crashes / "MiJuego_1.txt";
        const std::filesystem::path report2 = crashes / "MiJuego_2.txt";
        writeFile(report1, "informe uno");
        writeFile(report2, "informe dos");
        net::HttpRequest request;
        std::string error;
        check(!crash::buildUploadRequest(config, report1, request, &error), "sin direccion: no hay peticion");
        config.url = "http://example.com/crash";
        check(!crash::buildUploadRequest(config, report1, request, &error), "http:// a otro equipo: no (" + error + ")");
        config.url = "https://example.com/crash";
        check(crash::buildUploadRequest(config, report1, request, &error) && request.method == "POST" &&
                  request.body.find("informe uno") != std::string::npos,
              "https: POST con el informe (sin enviarla)");

        crash::queueReport(crashes, "MiJuego", report1);
        crash::queueReport(crashes, "MiJuego", report1);
        crash::queueReport(crashes, "MiJuego", report2);
        check(crash::queuedReports(crashes, "MiJuego").size() == 2, "cola sin repetidos");

        // "Servidor" falso: el primero responde 200, el segundo falla siempre.
        int calls = 0;
        const crash::Sender fake = [&](const net::HttpRequest& r) {
            ++calls;
            net::HttpResponse response;
            if (r.body.find("informe uno") != std::string::npos) {
                response.ok = true;
                response.status = 200;
            } else {
                response.status = 503;
            }
            return response;
        };
        check(crash::uploadQueued(config, crashes, "MiJuego", fake) == 1 && calls == 2, "envia la cola: uno llega");
        check(crash::queuedReports(crashes, "MiJuego").size() == 1, "lo enviado sale de la cola");
        crash::uploadQueued(config, crashes, "MiJuego", fake);
        check(crash::queuedReports(crashes, "MiJuego").size() == 1, "lo que falla se reintenta");
        crash::uploadQueued(config, crashes, "MiJuego", fake);
        check(crash::queuedReports(crashes, "MiJuego").empty(), "al tercer fallo se abandona");
        crash::UploadConfig off = config;
        off.url.clear();
        crash::queueReport(crashes, "MiJuego", report1);
        calls = 0;
        check(crash::uploadQueued(off, crashes, "MiJuego", fake) == 0 && calls == 0, "sin direccion no se envia nada");
        for (int i = 0; i < 12; ++i) {
            const std::filesystem::path r = crashes / ("MiJuego_x" + std::to_string(i) + ".txt");
            writeFile(r, "x");
            crash::queueReport(crashes, "MiJuego", r);
        }
        check(crash::queuedReports(crashes, "MiJuego").size() == 8, "como mucho 8 en la cola");
        check(crash::editorReportUrl().empty(), "crash.ReportUrl vacia por defecto");
    }

    std::filesystem::remove_all(root);
    std::cout << (g_failures == 0 ? "TODO OK\n" : "HAY FALLOS\n");
    return g_failures == 0 ? 0 : 1;
}
