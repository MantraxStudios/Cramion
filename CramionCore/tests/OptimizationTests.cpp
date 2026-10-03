// Pruebas de las optimizaciones del roadmap (consola):
//   - Insights: zonas anidadas, estadisticas, percentiles, contadores,
//     capturas .crtrace (JSON valido) y captura automatica de tirones.
//   - Presupuesto de particulas: emision escalada al pasarse, emisores lejanos
//     sin emitir.
//   - Lua profiler: zona por script y aviso al pasarse de lua.BudgetMs.
// Devuelve 0 si todo va.

#include "CramionCore/cvar/CVar.h"
#include "CramionCore/ecs/World.h"
#include "CramionCore/physics/Particles.h"
#include "CramionCore/physics/PhysicsComponents.h"
#include "CramionCore/physics/PhysicsSystem.h"
#include "CramionCore/profiling/Profiler.h"
#include "CramionCore/scripting/Scripting.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using namespace cramion;

namespace {

int failures = 0;
int checks = 0;

void check(bool condition, const char* what) {
    ++checks;
    std::printf("  %s %s\n", condition ? "OK   " : "FALLO", what);
    if (!condition) ++failures;
}

void busy(double ms) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::microseconds(static_cast<long long>(ms * 1000.0));
    while (std::chrono::steady_clock::now() < end) {
    }
}

const prof::ZoneStats* find(const std::vector<prof::ZoneStats>& z, const std::string& path) {
    for (const auto& s : z) {
        if (s.path == path) return &s;
    }
    return nullptr;
}

void profiler() {
    std::printf("Insights (perfilador)\n");
    prof::reset();
    prof::setEnabled(true);
    cvar::Registry::instance().set("prof.HitchMs", "0");  // sin capturas en esta parte
    for (int f = 0; f < 50; ++f) {
        prof::beginFrame();
        {
            CR_PROFILE_SCOPE("Fisica");
            busy(0.4);
            {
                CR_PROFILE_SCOPE("Jolt");
                busy(0.6);
            }
        }
        for (int k = 0; k < 3; ++k) {
            CR_PROFILE_SCOPE("Script");
            busy(0.1);
        }
        CR_PROFILE_COUNTER("Cosas", f);
        prof::endFrame();
    }
    const std::vector<prof::ZoneStats> z = prof::stats(50);
    const prof::ZoneStats* fis = find(z, "Fisica");
    const prof::ZoneStats* jolt = find(z, "Fisica/Jolt");
    const prof::ZoneStats* script = find(z, "Script");
    check(fis && jolt && script, "arbol de zonas: Fisica, Fisica/Jolt y Script");
    if (fis && jolt && script) {
        std::printf("  (Fisica %.2f ms, propio %.2f; Jolt %.2f; Script %.2f ms en %.1f llamadas)\n", fis->avg_ms,
                    fis->self_ms, jolt->avg_ms, script->avg_ms, script->calls);
        check(fis->avg_ms > jolt->avg_ms && fis->avg_ms >= 0.95, "lo de dentro cuenta en el padre (inclusivo)");
        check(fis->self_ms > 0.3 && fis->self_ms < fis->avg_ms - 0.4, "tiempo propio sin los hijos");
        check(jolt->depth == 1 && fis->depth == 0, "profundidad de cada zona");
        check(script->calls > 2.9 && script->calls < 3.1, "llamadas por frame");
        check(fis->p95_ms >= fis->avg_ms * 0.9 && fis->max_ms >= fis->p95_ms, "p95 y maximo");
    }
    const auto counters = prof::counters(50);
    check(!counters.empty() && counters.front().name == "Cosas" && counters.front().last == 49.0,
          "contadores por frame");
    const prof::FrameSummary s = prof::summary(50);
    check(s.p50_ms > 1.0 && s.p99_ms >= s.p95_ms && s.p95_ms >= s.p50_ms, "percentiles del frame");

    // Captura: JSON de Chrome Trace con los frames y las zonas
    const std::filesystem::path file = std::filesystem::temp_directory_path() / "cramion_prueba.crtrace";
    std::string error;
    check(prof::saveTrace(file, 20, &error), "guarda la captura .crtrace");
    std::ifstream in(file);
    nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
    int frames = 0, jolts = 0;
    if (j.is_object() && j.contains("traceEvents")) {
        for (const auto& e : j["traceEvents"]) {
            const std::string n = e.value("name", "");
            if (e.value("cat", "") == "frame") ++frames;
            if (n == "Jolt") ++jolts;
        }
    }
    check(frames == 20 && jolts == 20, "la captura tiene 20 frames con sus zonas (JSON valido)");

    // Tiron: un frame largo guarda solo una captura de los de antes
    cvar::Registry::instance().set("prof.HitchMs", "8");
    cvar::Registry::instance().set("prof.HitchCooldown", "0");
    std::string aviso;
    prof::setLog([&](const std::string& line) { aviso = line; });
    prof::setCaptureFolder(std::filesystem::temp_directory_path() / "cramion_tirones");
    prof::beginFrame();
    {
        CR_PROFILE_SCOPE("Carga");
        busy(15.0);
    }
    prof::endFrame();
    const std::filesystem::path hitch = prof::lastHitchCapture();
    std::printf("  (%s)\n", aviso.c_str());
    check(!hitch.empty() && std::filesystem::exists(hitch), "un frame de >8 ms guarda una captura del tiron");
    check(aviso.find("Carga") != std::string::npos, "el aviso dice que zona se comio el frame");
    prof::setLog({});
    cvar::Registry::instance().set("prof.HitchMs", "50");
    cvar::Registry::instance().set("prof.HitchCooldown", "10");

    // Apagado: no mide nada
    prof::reset();
    prof::setEnabled(false);
    prof::beginFrame();
    {
        CR_PROFILE_SCOPE("Nada");
    }
    prof::endFrame();
    check(prof::stats(10).empty() && prof::frameTimes().empty(), "con prof.Enabled = false no cuesta nada");
    prof::setEnabled(true);
}

void particles() {
    std::printf("Presupuesto de particulas\n");
    ecs::World world;
    ecs::Entity near = world.create("Cerca");
    near.setWorldPosition(core::Vec3{0.0f, 0.0f, -5.0f});
    physics::ParticleSystem& a = near.add<physics::ParticleSystem>();
    a.rate = 5000.0f;
    a.max_particles = 100000;
    a.lifetime_min = a.lifetime_max = 5.0f;
    a.collision = false;
    ecs::Entity far = world.create("Lejos");
    far.setWorldPosition(core::Vec3{0.0f, 0.0f, -500.0f});
    physics::ParticleSystem& b = far.add<physics::ParticleSystem>();
    b = a;

    cvar::Registry::instance().set("fx.particles.Budget", "2000");
    cvar::Registry::instance().set("fx.particles.CullDistance", "120");
    physics::ParticleWorld pw;
    pw.setViewer(core::Vec3{0.0f, 0.0f, 0.0f}, core::Vec3{0.0f, 0.0f, -1.0f});
    for (int i = 0; i < 120; ++i) pw.update(world, 1.0f / 60.0f, nullptr);  // 2 s
    const std::size_t total = pw.particleCount();
    std::printf("  (%zu particulas; sin presupuesto serian ~10000; escala de emision %.2f)\n", total,
                static_cast<double>(pw.budgetStats().emission_scale));
    check(total <= 2000 && total > 1000, "con presupuesto de 2000 no pasa de 2000 (y sigue emitiendo)");
    check(pw.budgetStats().emission_scale < 1.0f, "la escala de emision baja al pasarse");
    check(pw.budgetStats().culled == 1, "el emisor a 500 m no emite (CullDistance 120)");
    cvar::Registry::instance().set("fx.particles.Budget", "30000");
}

void luaProfiler() {
    std::printf("Lua profiler y presupuesto\n");
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "cramion_opt_lua";
    std::filesystem::create_directories(root / "Scripts");
    std::ofstream(root / "Scripts" / "Lento.lua") << "local L = {}\n"
                                                     "function L:Update(dt)\n"
                                                     "  local x = 0\n"
                                                     "  for i = 1, 3000000 do x = x + i end\n"
                                                     "end\n"
                                                     "return L\n";
    scripting::registerScriptComponents();
    ecs::World world;
    ecs::Entity e = world.create("Lento");
    e.add<scripting::Script>().file = "Scripts/Lento.lua";
    scripting::ScriptSystem system;
    system.setAssetsRoot(root);
    std::vector<std::string> log;
    system.setLog([&](int, const std::string& m) { log.push_back(m); });
    cvar::Registry::instance().set("lua.BudgetMs", "0.5");
    prof::reset();
    system.start(world);
    for (int f = 0; f < 8; ++f) {
        prof::beginFrame();
        system.update(world, 1.0f / 60.0f);
        prof::endFrame();
        if (f == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const std::vector<prof::ZoneStats> z = prof::stats(8);
    const prof::ZoneStats* zone = find(z, "Scripts/Lento.lua:Update");
    if (zone) std::printf("  (Scripts/Lento.lua:Update %.2f ms por frame)\n", zone->avg_ms);
    check(zone != nullptr && zone->avg_ms > 0.5, "cada script es una zona con su tiempo");
    bool warned = false;
    for (const std::string& m : log) warned = warned || m.find("Lento.lua va lento") != std::string::npos;
    int warnings = 0;
    for (const std::string& m : log) warnings += m.find("Lento.lua va lento") != std::string::npos ? 1 : 0;
    check(warned && warnings == 1, "aviso al pasarse de lua.BudgetMs (una vez, no cada frame)");
    bool counter = false;
    for (const auto& c : prof::counters(8)) counter = counter || c.name == "Lua (ms)";
    check(counter, "contador Lua (ms) por frame");
    system.stop();
    cvar::Registry::instance().set("lua.BudgetMs", "2");
}

void sueloDelIK() {
    std::printf("Suelo para los pies (IK)\n");
    ecs::World world;
    ecs::Entity suelo = world.create("Suelo");
    ecs::Entity yo = world.create("Yo");
    yo.add<physics::CharacterController>();
    ecs::Entity modelo = world.create("Modelo", yo);
    ecs::Entity otro = world.create("Rival");
    otro.add<physics::CharacterController>();
    ecs::Entity otroModelo = world.create("ModeloRival", otro);
    otroModelo.add<ecs::Ragdoll>();
    ecs::Entity caja = world.create("Caja");
    caja.add<physics::Rigidbody>();  // dinamico por defecto
    ecs::Entity plataforma = world.create("Plataforma");
    plataforma.add<physics::Rigidbody>().type = physics::BodyType::Kinematic;
    check(physics::isFootGround(suelo, modelo), "el suelo vale");
    check(physics::isFootGround(plataforma, modelo), "una plataforma cinematica vale");
    check(!physics::isFootGround(yo, modelo), "la propia capsula no");
    check(!physics::isFootGround(otro, modelo), "la capsula del rival no (los pies no lo pisan)");
    check(!physics::isFootGround(otroModelo, modelo), "un ragdoll no");
    check(!physics::isFootGround(caja, modelo), "un objeto dinamico no");
    // Un collider del mismo personaje en otra rama (el modelo en un hijo y un
    // collider de los pies en otro): tampoco.
    ecs::Entity cuerpo = world.create("Cuerpo");
    cuerpo.add<physics::Rigidbody>().type = physics::BodyType::Kinematic;
    ecs::Entity pies = world.create("Pies", cuerpo);
    ecs::Entity modeloCuerpo = world.create("ModeloCuerpo", cuerpo);
    check(!physics::isFootGround(pies, modeloCuerpo), "un collider del propio personaje en otra rama no");
    check(physics::isFootGround(plataforma, modeloCuerpo), "el escenario si");
}

// El rayo de los pies con la fisica de verdad: suelo, una pared alta y un
// escalon. Si el rayo empieza dentro de la pared o del escalon (el pie metido
// en ellos) no cuenta: antes daba un impacto a distancia 0 y el pie saltaba.
using core::Vec3;

void rayoDeLosPies() {
    std::printf("Rayo de los pies (IK)\n");
    ecs::World world;
    ecs::Entity suelo = world.create("Suelo");
    suelo.add<physics::PlaneCollider>();
    ecs::Entity pared = world.create("Pared");
    pared.setWorldPosition(Vec3{2.0f, 1.0f, 0.0f});
    pared.add<physics::BoxCollider>().size = Vec3{1.0f, 2.0f, 4.0f};
    ecs::Entity yo = world.create("Yo");
    yo.add<physics::Rigidbody>().type = physics::BodyType::Kinematic;
    ecs::Entity modelo = world.create("Modelo", yo);
    ecs::Entity pies = world.create("Collider de los pies", yo);
    pies.setWorldPosition(Vec3{-2.0f, 0.1f, 0.0f});
    pies.add<physics::BoxCollider>().size = Vec3{0.4f, 0.2f, 0.4f};
    physics::PhysicsSystem physics;
    physics.start(world);
    physics.update(world, 0.0f, false);
    Vec3 point{};
    Vec3 normal{};
    const Vec3 down{0.0f, -1.0f, 0.0f};
    const bool open = physics::footGroundRaycast(physics, Vec3{0.0f, 0.5f, 0.0f}, down, 1.0f, point, normal, modelo);
    check(open && std::abs(point.y) < 1e-3f && normal.y > 0.99f, "en el suelo libre toca el suelo");
    // Desde dentro de la pared (su techo esta a 2 m, el rayo sale a 0.5 m).
    const bool inside = physics::footGroundRaycast(physics, Vec3{2.0f, 0.5f, 0.0f}, down, 1.0f, point, normal, modelo);
    std::printf("    (desde dentro de la pared: %s, y = %.3f)\n", inside ? "toca" : "nada", inside ? point.y : 0.0f);
    check(!inside || std::abs(point.y) < 1e-3f, "desde dentro de una pared no la cuenta (ni salta a su techo)");
    // Encima de su propio collider de los pies: no lo pisa, pisa el suelo.
    const bool own = physics::footGroundRaycast(physics, Vec3{-2.0f, 0.5f, 0.0f}, down, 1.0f, point, normal, modelo);
    check(own && std::abs(point.y) < 1e-3f, "no pisa un collider del propio personaje");
    physics.stop();
}

}  // namespace

int main() {
    profiler();
    particles();
    luaProfiler();
    sueloDelIK();
    rayoDeLosPies();
    std::printf("\n%d comprobaciones, %d fallos\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
