// Pruebas de la navegacion (consola, sin GPU): la malla dentro del volumen,
// caminos que rodean un muro, regeneracion al moverlo (solo sus baldosas),
// modificadores, lo que no cuenta (triggers, dinamicos, agentes), agentes
// que llegan, el scripting y los ajustes. Devuelve 0 si todo va.

#include "CramionCore/ecs/SceneSerializer.h"
#include "CramionCore/ecs/World.h"
#include "CramionCore/navigation/Navigation.h"
#include "CramionCore/physics/PhysicsComponents.h"
#include "CramionCore/physics/PhysicsSystem.h"
#include "CramionCore/scripting/Scripting.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>

using namespace cramion;
using namespace cramion::navigation;
using core::Vec3;

namespace {

int failures = 0;
int checks = 0;

void check(bool condition, const char* what) {
    ++checks;
    std::printf("  %s %s\n", condition ? "OK   " : "FALLO", what);
    if (!condition) ++failures;
}

float pathLength(const std::vector<Vec3>& path) {
    float total = 0.0f;
    for (std::size_t i = 1; i < path.size(); ++i) total += core::length(path[i] - path[i - 1]);
    return total;
}

// Suelo de 60 x 60 (arriba en y = 0), muro en x = 0 de z -15 a 15 y un
// volumen de 40 x 40.
struct Level {
    ecs::World world;
    ecs::Entity floor;
    ecs::Entity wall;
    ecs::Entity volume;

    Level() {
        floor = world.create("Suelo");
        floor.setWorldPosition(Vec3{0.0f, -0.5f, 0.0f});
        floor.add<physics::BoxCollider>().size = Vec3{60.0f, 1.0f, 60.0f};
        wall = world.create("Muro");
        wall.setWorldPosition(Vec3{0.0f, 1.5f, 0.0f});
        wall.add<physics::BoxCollider>().size = Vec3{1.0f, 3.0f, 30.0f};
        volume = world.create("Volumen");
        volume.setWorldPosition(Vec3{0.0f, 2.0f, 0.0f});
        volume.add<NavMeshBounds>().size = Vec3{40.0f, 10.0f, 40.0f};
    }
};

NavigationSettings testSettings() {
    NavigationSettings s;
    s.cell_size = 0.25f;  // mas rapido que el valor por defecto
    s.cell_height = 0.1f;
    s.tile_size = 48;
    return s;
}

void testBuild() {
    std::printf("Malla y caminos\n");
    Level level;
    NavigationSystem nav;
    nav.setSettings(testSettings());
    const auto start = std::chrono::steady_clock::now();
    nav.waitForBuild(level.world);
    const float seconds = std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();
    const NavStats stats = nav.stats();
    std::printf("  (%d baldosas, %d poligonos, %.0f KB en %.2f s)\n", stats.tiles, stats.polygons,
                static_cast<double>(stats.memory_bytes) / 1024.0, static_cast<double>(seconds));
    check(nav.ready() && stats.tiles > 0 && stats.polygons > 0, "genera la malla dentro del volumen");
    check(!nav.building(), "termina de generar");

    std::vector<Vec3> path;
    bool partial = true;
    const bool found = nav.findPath(Vec3{-8, 0, 0}, Vec3{8, 0, 0}, path, &partial);
    std::printf("  (camino de %zu puntos, %.1f m)\n", path.size(), static_cast<double>(pathLength(path)));
    check(found && !partial, "hay camino de un lado del muro al otro");
    check(pathLength(path) > 30.0f, "el camino rodea el muro (no lo atraviesa)");
    check(!nav.raycast(Vec3{-8, 0, 0}, Vec3{8, 0, 0}), "la linea recta choca con el muro");
    check(nav.raycast(Vec3{-8, 0, -18}, Vec3{8, 0, -18}), "la linea recta por fuera del muro llega");

    Vec3 p{};
    check(nav.projectPoint(Vec3{5, 1, 5}, p) && std::abs(p.y) < 0.3f, "proyecta un punto sobre el suelo");
    check(!nav.projectPoint(Vec3{25, 0, 0}, p), "fuera del volumen no hay malla");
    check(!nav.projectPoint(Vec3{0, 3, 0}, p, 0.3f), "encima del muro no hay malla (muy estrecho)");
    int inside = 0;
    for (int i = 0; i < 20; ++i) {
        if (nav.randomPoint(Vec3{-8, 0, 0}, 3.0f, p) && core::length(p - Vec3{-8, 0, 0}) < 3.5f) ++inside;
    }
    check(inside == 20, "puntos al azar dentro del radio");

    // --- Tiempo real: se mueve el muro (bajo el suelo) ---
    const int tiles_before = nav.stats().tiles;
    level.wall.setWorldPosition(Vec3{0.0f, -10.0f, 0.0f});
    nav.update(level.world, 0.016f, false);
    const int pending = nav.stats().pending_tiles;
    std::printf("  (al mover el muro: %d de %d baldosas pendientes)\n", pending, tiles_before);
    check(pending > 0 && pending < tiles_before, "mover un collider solo rehace sus baldosas");
    nav.waitForBuild(level.world);
    nav.findPath(Vec3{-8, 0, 0}, Vec3{8, 0, 0}, path);
    check(!path.empty() && pathLength(path) < 16.5f, "sin muro el camino va recto");

    // --- Modificadores ---
    ecs::Entity modifier = level.world.create("Zanja");
    modifier.setWorldPosition(Vec3{0.0f, 0.0f, 0.0f});
    NavModifier& mod = modifier.add<NavModifier>();
    mod.size = Vec3{2.0f, 4.0f, 50.0f};
    nav.waitForBuild(level.world);
    const bool blocked_found = nav.findPath(Vec3{-8, 0, 0}, Vec3{8, 0, 0}, path, &partial);
    check(!blocked_found || partial, "Bloquear corta la malla");
    modifier.get<NavModifier>().area = NavArea::Avoid;
    nav.waitForBuild(level.world);
    check(nav.findPath(Vec3{-8, 0, 0}, Vec3{8, 0, 0}, path, &partial) && !partial, "Evitar deja pasar");
    // Evitar en una zona rodeable: el camino da la vuelta.
    modifier.get<NavModifier>().size = Vec3{2.0f, 4.0f, 10.0f};
    nav.waitForBuild(level.world);
    nav.findPath(Vec3{-8, 0, 0}, Vec3{8, 0, 0}, path);
    check(pathLength(path) > 17.0f, "los caminos rodean las zonas de Evitar si pueden");
    level.world.destroy(modifier);
    nav.waitForBuild(level.world);
    nav.findPath(Vec3{-8, 0, 0}, Vec3{8, 0, 0}, path);
    check(pathLength(path) < 16.5f, "borrar el modificador lo quita");

    // --- Lo que no cuenta ---
    ecs::Entity trigger = level.world.create("Trigger");
    trigger.setWorldPosition(Vec3{0.0f, 1.5f, 0.0f});
    auto& tb = trigger.add<physics::BoxCollider>();
    tb.size = Vec3{1.0f, 3.0f, 50.0f};
    tb.material.is_trigger = true;
    ecs::Entity dynamic = level.world.create("Caja dinamica");
    dynamic.setWorldPosition(Vec3{0.0f, 1.5f, 0.0f});
    dynamic.add<physics::BoxCollider>().size = Vec3{1.0f, 3.0f, 50.0f};
    dynamic.add<physics::Rigidbody>();
    nav.waitForBuild(level.world);
    nav.findPath(Vec3{-8, 0, 0}, Vec3{8, 0, 0}, path);
    check(pathLength(path) < 16.5f, "los triggers y los Rigidbody dinamicos no cuentan");

    // --- El volumen: se encoge y la malla le sigue ---
    level.volume.get<NavMeshBounds>().size = Vec3{10.0f, 10.0f, 10.0f};
    nav.waitForBuild(level.world);
    check(!nav.projectPoint(Vec3{12, 0, 0}, p) && nav.projectPoint(Vec3{3, 0, 0}, p),
          "al cambiar el volumen la malla se ajusta");
    level.volume.setActive(false);
    nav.waitForBuild(level.world);
    check(!nav.ready(), "sin volumen no hay malla");
}

void testAgents() {
    std::printf("Agentes\n");
    Level level;
    NavigationSystem nav;
    nav.setSettings(testSettings());

    ecs::Entity agent = level.world.create("Agente");
    agent.setWorldPosition(Vec3{-8.0f, 0.0f, 0.0f});
    agent.add<NavAgent>().speed = 6.0f;
    // Su collider (y el de un hijo) no tallan la malla.
    agent.add<physics::CapsuleCollider>();
    ecs::Entity arma = level.world.create("Arma", agent);
    arma.add<physics::BoxCollider>().size = Vec3{3.0f, 3.0f, 3.0f};

    // Se pide antes de que exista la malla: arranca en cuanto este.
    check(nav.moveTo(agent, Vec3{8.0f, 0.0f, 0.0f}), "moveTo antes de tener malla se guarda");
    nav.waitForBuild(level.world);
    std::vector<Vec3> path;
    nav.findPath(Vec3{-8, 0, 2}, Vec3{-8, 0, -2}, path);
    check(pathLength(path) < 4.5f, "el agente no se talla a si mismo");

    float t = 0.0f;
    bool moved = false;
    while (t < 20.0f && (nav.isMoving(agent) || t < 0.1f)) {
        nav.update(level.world, 1.0f / 60.0f, true);
        t += 1.0f / 60.0f;
        if (!moved && nav.remainingDistance(agent) > 20.0f) moved = true;
    }
    const Vec3 end = agent.worldPosition();
    std::printf("  (llego a (%.2f, %.2f, %.2f) en %.1f s)\n", end.x, end.y, end.z, static_cast<double>(t));
    check(moved, "remainingDistance cuenta el rodeo del muro");
    check(core::length(end - Vec3{8, 0, 0}) < 0.6f, "llega al destino rodeando el muro");
    check(!nav.isMoving(agent), "al llegar deja de moverse");
    // Mira hacia donde iba (ultimo tramo: hacia -Z o +X).
    check(std::abs(agent.localEulerDegrees().y) > 1.0f, "gira hacia el movimiento");

    // Un objetivo fuera de la malla: va a lo mas cerca.
    nav.moveTo(agent, Vec3{8.0f, 0.0f, 19.0f});
    for (int i = 0; i < 600 && nav.isMoving(agent); ++i) nav.update(level.world, 1.0f / 60.0f, true);
    check(agent.worldPosition().z > 15.0f, "un destino en el borde se alcanza");

    // Dos agentes que se cruzan no se atraviesan (evitacion).
    ecs::Entity a = level.world.create("A");
    a.setWorldPosition(Vec3{-6.0f, 0.0f, -17.0f});
    a.add<NavAgent>();
    ecs::Entity b = level.world.create("B");
    b.setWorldPosition(Vec3{6.0f, 0.0f, -17.0f});
    b.add<NavAgent>();
    nav.moveTo(a, Vec3{6.0f, 0.0f, -17.0f});
    nav.moveTo(b, Vec3{-6.0f, 0.0f, -17.0f});
    float closest = 1e9f;
    for (int i = 0; i < 900; ++i) {
        nav.update(level.world, 1.0f / 60.0f, true);
        closest = std::min(closest, core::length(a.worldPosition() - b.worldPosition()));
    }
    std::printf("  (distancia minima entre agentes %.2f m)\n", static_cast<double>(closest));
    check(closest > 0.45f, "los agentes se esquivan");
    check(core::length(a.worldPosition() - Vec3{6, 0, -17}) < 1.0f &&
              core::length(b.worldPosition() - Vec3{-6, 0, -17}) < 1.0f,
          "y los dos llegan");

    // Teletransporte desde fuera (un script): sigue desde ahi.
    a.setWorldPosition(Vec3{-10.0f, 0.0f, 10.0f});
    nav.moveTo(a, Vec3{-10.0f, 0.0f, 5.0f});
    for (int i = 0; i < 600 && nav.isMoving(a); ++i) nav.update(level.world, 1.0f / 60.0f, true);
    check(core::length(a.worldPosition() - Vec3{-10, 0, 5}) < 0.6f, "tras moverlo a mano sigue desde alli");
}

void testRigidbodyAgent() {
    std::printf("Agente con Rigidbody\n");
    Level level;
    NavigationSystem nav;
    nav.setSettings(testSettings());
    physics::PhysicsSystem physics;
    nav.setPhysics(&physics);
    ecs::Entity agent = level.world.create("Fisico");
    agent.setWorldPosition(Vec3{-8.0f, 1.0f, 0.0f});
    auto& capsule = agent.add<physics::CapsuleCollider>();
    capsule.center = Vec3{0.0f, 0.0f, 0.0f};
    auto& rb = agent.add<physics::Rigidbody>();
    rb.lock_rotation_x = rb.lock_rotation_z = true;
    NavAgent& na = agent.add<NavAgent>();
    na.base_offset = 1.0f;  // pivote en el centro de la capsula
    physics.start(level.world);
    nav.waitForBuild(level.world);
    nav.moveTo(agent, Vec3{-8.0f, 0.0f, 10.0f});
    for (int i = 0; i < 900 && nav.isMoving(agent); ++i) {
        physics.update(level.world, 1.0f / 60.0f, true);
        nav.update(level.world, 1.0f / 60.0f, true);
    }
    const Vec3 p = agent.worldPosition();
    std::printf("  (en (%.2f, %.2f, %.2f))\n", p.x, p.y, p.z);
    check(std::abs(p.z - 10.0f) < 1.0f && std::abs(p.x + 8.0f) < 1.0f, "se mueve por velocidad y llega");
}

void testScripting() {
    std::printf("Scripting\n");
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "cramion_nav_test";
    std::filesystem::create_directories(root / "Scripts");
    {
        std::ofstream(root / "Scripts" / "Patrulla.lua") << R"(
local Patrulla = { properties = {} }
function Patrulla:Start()
    self.ok = self.entity:moveTo(Vec3(8, 0, 0))
    local path = Navigation.findPath(Vec3(-8, 0, 0), Vec3(8, 0, 0))
    self.puntos = path and #path or 0
    local p = Navigation.projectPoint(Vec3(3, 2, 3))
    self.proyectado = p ~= nil and math.abs(p.y) < 0.3
    self.azar = Navigation.randomPoint(Vec3(0, 0, -18), 2) ~= nil
    self.listo = Navigation.isReady()
end
function Patrulla:Update(dt)
    if not self.entity.isMoving and not self.llego then
        self.llego = true
        self.restante = self.entity.remainingDistance
    end
end
return Patrulla
)";
    }
    Level level;
    NavigationSystem nav;
    nav.setSettings(testSettings());
    nav.waitForBuild(level.world);
    ecs::Entity agent = level.world.create("Guardia");
    agent.setWorldPosition(Vec3{-8.0f, 0.0f, 0.0f});
    agent.add<NavAgent>().speed = 8.0f;
    agent.add<scripting::Script>().file = "Scripts/Patrulla.lua";

    scripting::ScriptSystem scripts;
    scripts.setAssetsRoot(root);
    scripts.setNavigation(&nav);
    scripts.start(level.world);
    for (int i = 0; i < 60 * 15; ++i) {
        nav.update(level.world, 1.0f / 60.0f, true);
        scripts.update(level.world, 1.0f / 60.0f);
    }
    std::string out;
    const auto value = [&](const char* expr) {
        out.clear();
        scripts.run(std::string("local g = Scene.find('Guardia'):getScript(); return ") + expr, &out);
        return out;
    };
    check(value("g.ok") == "true", "entity:moveTo devuelve true");
    check(std::atoi(value("g.puntos").c_str()) >= 3, "Navigation.findPath da los puntos de giro");
    check(value("g.proyectado") == "true" && value("g.azar") == "true" && value("g.listo") == "true",
          "projectPoint, randomPoint e isReady");
    check(value("g.llego") == "true", "entity.isMoving pasa a false al llegar");
    std::printf("  (guardia en (%.2f, %.2f, %.2f))\n", agent.worldPosition().x, agent.worldPosition().y,
                agent.worldPosition().z);
    check(core::length(agent.worldPosition() - Vec3{8, 0, 0}) < 0.6f, "el script mueve al agente");
    scripts.stop();
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

void testSettingsAndScene() {
    std::printf("Ajustes y escena\n");
    NavigationSettings s;
    s.agent_radius = 0.6f;
    s.max_slope = 30.0f;
    s.tile_size = 32;
    s.runtime_generation = false;
    const std::filesystem::path file = std::filesystem::temp_directory_path() / "cramion_nav_settings.json";
    check(saveNavigationSettings(file, s), "guarda Navigation.json");
    NavigationSettings loaded;
    check(loadNavigationSettings(file, loaded) && loaded.agent_radius == 0.6f && loaded.max_slope == 30.0f &&
              loaded.tile_size == 32 && !loaded.runtime_generation,
          "y lo vuelve a leer");
    std::filesystem::remove(file);

    registerNavigationComponents();
    ecs::World world;
    ecs::Entity e = world.create("Zona");
    e.add<NavMeshBounds>().size = Vec3{12.0f, 3.0f, 7.0f};
    e.add<NavModifier>().area = NavArea::Avoid;
    e.add<NavAgent>().speed = 7.5f;
    ecs::World copy;
    ecs::deserializeWorld(copy, ecs::serializeWorld(world));
    const ecs::Entity c = copy.findByName("Zona");
    check(c.valid() && c.has<NavMeshBounds>() && c.get<NavMeshBounds>().size.x == 12.0f &&
              c.get<NavModifier>().area == NavArea::Avoid && c.get<NavAgent>().speed == 7.5f,
          "los componentes se guardan en la escena");
}

// Rendimiento con los ajustes por defecto: 400 x 400 m con 3000 cajas.
void testPerformance() {
    std::printf("Rendimiento\n");
    ecs::World world;
    ecs::Entity floor = world.create("Suelo");
    floor.setWorldPosition(Vec3{0.0f, -0.5f, 0.0f});
    floor.add<physics::BoxCollider>().size = Vec3{400.0f, 1.0f, 400.0f};
    ecs::Entity volume = world.create("Volumen");
    volume.setWorldPosition(Vec3{0.0f, 4.0f, 0.0f});
    volume.add<NavMeshBounds>().size = Vec3{400.0f, 12.0f, 400.0f};
    std::uint32_t seed = 12345;
    const auto rnd = [&]() {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<float>(seed >> 8) / static_cast<float>(1u << 24);
    };
    ecs::Entity last;
    for (int i = 0; i < 3000; ++i) {
        last = world.create("Caja");
        last.setWorldPosition(Vec3{rnd() * 380.0f - 190.0f, 1.0f, rnd() * 380.0f - 190.0f});
        last.setLocalEulerDegrees(Vec3{0.0f, rnd() * 360.0f, 0.0f});
        last.add<physics::BoxCollider>().size = Vec3{0.5f + rnd() * 3.0f, 2.0f, 0.5f + rnd() * 3.0f};
    }
    NavigationSystem nav;
    auto start = std::chrono::steady_clock::now();
    nav.waitForBuild(world, 120.0f);
    const float full = std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();
    const NavStats stats = nav.stats();
    std::printf("  (malla completa: %d baldosas, %d poligonos, %.1f MB en %.2f s)\n", stats.tiles, stats.polygons,
                static_cast<double>(stats.memory_bytes) / (1024.0 * 1024.0), static_cast<double>(full));
    check(stats.tiles > 100 && full < 60.0f, "genera 400 x 400 m con 3000 obstaculos");

    // Un frame del editor sin cambios: solo mira firmas.
    start = std::chrono::steady_clock::now();
    nav.update(world, 0.016f, false);
    const float idle_ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();

    last.setWorldPosition(last.worldPosition() + Vec3{2.0f, 0.0f, 0.0f});
    start = std::chrono::steady_clock::now();
    nav.update(world, 0.016f, false);
    const float frame_ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
    const int pending = nav.stats().pending_tiles;
    nav.waitForBuild(world);
    const float moved = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
    std::printf("  (frame sin cambios %.2f ms; mover una caja: frame %.2f ms, %d baldosas, lista en %.0f ms)\n",
                static_cast<double>(idle_ms), static_cast<double>(frame_ms), pending, static_cast<double>(moved));
    check(pending <= 6 && moved < 500.0f, "mover un obstaculo se actualiza al momento");
    check(frame_ms < 16.0f, "sin parar el frame del editor");
}

}  // namespace

int main() {
    testBuild();
    testAgents();
    testRigidbodyAgent();
    testScripting();
    testSettingsAndScene();
    testPerformance();
    std::printf("\n%d comprobaciones, %d fallos\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
