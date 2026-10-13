// Pruebas del cambio de escena (consola, sin GPU): la escena A con teselas de
// terreno, celdas del World Partition, colliders y malla de navegacion, con
// cargas en otros hilos a medias; se cambia a la escena B y nada de A debe
// quedar (ni entrar despues), y las teselas de B deben estar en su sitio con
// el origen flotante de B. Tambien: parar Play / deshacer tras desplazar el
// origen no mueve las teselas, y la misma rejilla con otros ajustes.
// Devuelve 0 si todo va.

#include "CramionCore/ecs/Components.h"
#include "CramionCore/ecs/FloatingOrigin.h"
#include "CramionCore/ecs/SceneReset.h"
#include "CramionCore/ecs/SceneSerializer.h"
#include "CramionCore/ecs/World.h"
#include "CramionCore/navigation/Navigation.h"
#include "CramionCore/physics/PhysicsComponents.h"
#include "CramionCore/physics/PhysicsSystem.h"
#include "CramionCore/terrain/Terrain.h"
#include "CramionCore/terrain/TerrainGrid.h"
#include "CramionCore/world/WorldPartition.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

using namespace cramion;
using core::Vec3;
namespace fs = std::filesystem;

namespace {

int failures = 0;
int checks = 0;

void check(bool condition, const char* what) {
    ++checks;
    std::printf("  %s %s\n", condition ? "OK   " : "FALLO", what);
    if (!condition) ++failures;
}

bool approx(float a, float b, float eps = 1e-3f) { return std::abs(a - b) <= eps; }
bool approx(const Vec3& a, const Vec3& b, float eps = 1e-3f) {
    return approx(a.x, b.x, eps) && approx(a.y, b.y, eps) && approx(a.z, b.z, eps);
}

fs::path tempRoot(const char* name) {
    const fs::path root = fs::temp_directory_path() / name;
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    return root;
}

terrain::TerrainGrid smallGrid(const std::string& folder, int tiles_x, int tiles_z, float tile_size) {
    terrain::TerrainGrid g;
    g.folder = folder;
    g.tiles_x = tiles_x;
    g.tiles_z = tiles_z;
    g.tile_size = tile_size;
    g.height = 50.0f;
    g.tile_resolution = 33;
    g.splat_resolution = 16;
    g.load_range = 150.0f;
    g.unload_margin = 10.0f;
    g.loads_per_frame = 16;
    g.max_loading = 16;
    return g;
}

std::vector<ecs::Entity> tilesOf(ecs::World& world) {
    std::vector<ecs::Entity> out;
    for (const entt::entity h : world.registry().view<terrain::TerrainTile>()) out.push_back(world.wrap(h));
    return out;
}

// Cada tesela, exactamente en esquina de su rejilla + (x, z) * tamano.
bool tilesInPlace(ecs::World& world, ecs::Entity grid_entity) {
    const terrain::TerrainGrid& g = grid_entity.get<terrain::TerrainGrid>();
    const Vec3 corner = grid_entity.worldPosition();
    bool ok = true;
    for (ecs::Entity t : tilesOf(world)) {
        const terrain::TerrainTile& tile = t.get<terrain::TerrainTile>();
        if (tile.grid != grid_entity.uuid()) return false;
        const Vec3 want = corner + Vec3{static_cast<float>(tile.x) * g.tile_size, 0.0f, static_cast<float>(tile.z) * g.tile_size};
        ok = ok && approx(t.worldPosition(), want);
    }
    return ok;
}

bool pump(terrain::TerrainStreamer& streamer, ecs::World& world, terrain::TerrainStore& store, const Vec3& viewer,
          int want) {
    for (int i = 0; i < 600; ++i) {
        streamer.update(world, store, viewer, false, false);
        if (streamer.stats().tiles_loaded == want && streamer.stats().tiles_loading == 0) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

// --- Escenas de prueba (en disco: el cambio de escena real usa loadScene) ---

struct Scenes {
    fs::path root;
    fs::path a, b;
    Uuid grid_a{}, grid_b{};
    terrain::TerrainGrid settings_a, settings_b;
    static constexpr double kOriginB = 10000.0;
};

Scenes writeScenes(const char* name) {
    Scenes s;
    s.root = tempRoot(name);
    std::string error;
    {
        // A: origen 0, rejilla de 4 x 1 teselas de 100 m desde (0, 0, 0).
        ecs::World a;
        a.setSceneName("A");
        s.settings_a = smallGrid("GridA", 4, 1, 100.0f);
        ecs::Entity grid = a.create("Rejilla A");
        grid.add<terrain::TerrainGrid>() = s.settings_a;
        s.grid_a = grid.uuid();
        a.create("Cubo A").setWorldPosition(Vec3{5.0f, 1.0f, 5.0f});
        s.a = s.root / "A.crscene";
        check(ecs::saveScene(a, s.a, &error), "guardar la escena A");
    }
    {
        // B: otra rejilla (otro UUID, otra carpeta) de 2 x 2 teselas de 64 m,
        // centrada en el (0,0,0) local, y la escena guardada lejos (origen 10 km).
        ecs::World b;
        b.setSceneName("B");
        b.setOrigin(ecs::DVec3{Scenes::kOriginB, 0.0, 0.0});
        s.settings_b = smallGrid("GridB", 2, 2, 64.0f);
        ecs::Entity grid = b.create("Rejilla B");
        grid.setWorldPosition(Vec3{-64.0f, 0.0f, -64.0f});
        grid.add<terrain::TerrainGrid>() = s.settings_b;
        s.grid_b = grid.uuid();
        b.create("Cubo B");
        s.b = s.root / "B.crscene";
        check(ecs::saveScene(b, s.b, &error), "guardar la escena B");
    }
    return s;
}

// --- Pruebas ---

void testRebase() {
    std::printf("Origen flotante: las raices transitorias no se mueven al recargar\n");
    ecs::World world;
    world.create("Cubo");
    ecs::Entity tile = world.create("Tesela");
    tile.get<ecs::EntityInfo>().transient = true;
    tile.setLocalPosition(Vec3{3100.0f, 0.0f, 0.0f});
    const std::string snapshot = ecs::serializeWorld(world);  // origen 0 (al dar Play)
    // En Play el jugador se aleja: el mundo se desplaza 3072 m.
    const auto offset = ecs::updateFloatingOrigin(world, Vec3{3000.0f, 0.0f, 0.0f});
    check(offset && world.origin().x == 3072.0 && approx(tile.localPosition().x, 28.0f), "el desplazamiento mueve la tesela");
    // Parar Play: el mundo vuelve con el origen 0 y la tesela se queda.
    ecs::deserializeWorld(world, snapshot);
    check(world.origin().x == 0.0 && world.valid(tile.handle()), "parar Play: origen de la escena, la tesela sigue");
    check(approx(tile.localPosition().x, 3100.0f), "la tesela sigue en el mismo sitio del mundo real (antes: 3072 m desplazada)");

    // Otra escena guardada con otro origen (deserializeWorld con origen 5000).
    ecs::World other;
    other.setOrigin(ecs::DVec3{5000.0, 0.0, 0.0});
    other.create("Lejos");
    ecs::deserializeWorld(world, ecs::serializeWorld(other));
    check(world.origin().x == 5000.0, "el origen del archivo");
    check(approx(static_cast<float>(world.absolute(tile.localPosition()).x), 3100.0f),
          "la tesela conserva su posicion absoluta con el origen nuevo");

    // rebaseOrigin a mano.
    world.rebaseOrigin(ecs::DVec3{});
    check(world.origin().x == 0.0 && approx(tile.localPosition().x, 3100.0f), "rebaseOrigin no mueve nada en el mundo real");
}

void testLoadSceneClears(const Scenes& s) {
    std::printf("loadScene: otra escena, nada de la anterior\n");
    ecs::World world;
    ecs::Entity tile = world.create("Tesela suelta");
    tile.get<ecs::EntityInfo>().transient = true;
    const std::uint64_t before = world.sceneGeneration();
    std::string error;
    check(ecs::loadScene(world, s.b, &error), "abrir B");
    check(!world.valid(tile.handle()) && !world.findByName("Tesela suelta").valid(),
          "las raices transitorias de la escena anterior no se quedan");
    check(world.sceneGeneration() > before, "la generacion de escena sube");
    check(world.origin().x == Scenes::kOriginB, "con el origen de B");
    const std::uint64_t mid = world.sceneGeneration();
    ecs::deserializeWorld(world, ecs::serializeWorld(world));
    check(world.sceneGeneration() == mid, "deshacer (deserializeWorld) no cambia la generacion");
}

void testTerrainSwitch(const Scenes& s) {
    std::printf("Teselas: cambiar de A a B con cargas a medias\n");
    terrain::TerrainStore store;
    store.setRoot(s.root);
    terrain::TerrainStreamer streamer;
    worldpart::WorldPartitionSystem partition;
    navigation::NavigationSystem nav;
    ecs::World world;
    std::string error;
    check(ecs::loadScene(world, s.a, &error), "abrir A");
    check(pump(streamer, world, store, Vec3{10.0f, 0.0f, 10.0f}, 2), "A: las teselas cerca de la camara");
    // El jugador se aleja: el origen flotante se mueve (las teselas con el).
    const auto offset = ecs::updateFloatingOrigin(world, Vec3{2500.0f, 0.0f, 0.0f});
    check(offset.has_value() && world.origin().x != 0.0, "A: el origen flotante se desplazo");
    // Cargas a medias (otros hilos) de las teselas lejanas de A.
    streamer.update(world, store, Vec3{350.0f - offset.value_or(Vec3{}).x, 0.0f, 10.0f}, false, false);

    // Cambio de escena: la secuencia comun y la escena nueva.
    ecs::SceneResetTargets targets;
    targets.terrain_streamer = &streamer;
    targets.terrain_store = &store;
    targets.partition = &partition;
    targets.navigation = &nav;
    ecs::SceneResetOptions options;
    options.discard_terrain_data = true;
    ecs::resetSceneState(world, targets, options);
    check(tilesOf(world).empty() && world.roots().empty(), "tras el reset no queda nada (ni teselas)");
    check(ecs::loadScene(world, s.b, &error), "abrir B");
    check(world.origin().x == Scenes::kOriginB, "B: su origen (y no el desplazado de A)");
    // Que terminen las lecturas canceladas de A (no deben entrar).
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    ecs::Entity grid_b = world.find(s.grid_b);
    check(grid_b.valid() && pump(streamer, world, store, Vec3{0.0f, 0.0f, 0.0f}, 4), "B: sus 4 teselas");

    bool foreign = false;
    for (ecs::Entity t : tilesOf(world)) foreign = foreign || t.get<terrain::TerrainTile>().grid != s.grid_b;
    check(!foreign, "ninguna tesela de A en B");
    check(!world.findByName("Cubo A").valid() && !world.find(s.grid_a).valid(), "nada de la escena A");
    bool data_a = false;
    for (int x = 0; x < s.settings_a.tiles_x; ++x) data_a = data_a || store.findPath(terrain::tilePath(s.settings_a, x, 0));
    check(!data_a, "ni sus datos en el almacen (las lecturas a medias no entraron)");
    check(streamer.stats().grids == 1 && streamer.stats().tiles_loaded == 4, "el streamer solo conoce la rejilla de B");
    check(tilesInPlace(world, grid_b), "las teselas de B en su sitio (esquina + x * tamano) con el origen de B");
    bool own_data = true;
    for (ecs::Entity t : tilesOf(world)) {
        const terrain::TerrainTile& tt = t.get<terrain::TerrainTile>();
        own_data = own_data && t.get<terrain::Terrain>().data == terrain::tilePath(s.settings_b, tt.x, tt.z);
    }
    check(own_data, "y con los datos de su carpeta");

    // Red de seguridad: loadScene directo (sin reset) de vuelta a A.
    check(ecs::loadScene(world, s.a, &error), "volver a A sin el reset");
    streamer.update(world, store, Vec3{10.0f, 0.0f, 10.0f}, false, false);
    foreign = false;
    for (ecs::Entity t : tilesOf(world)) foreign = foreign || t.get<terrain::TerrainTile>().grid != s.grid_a;
    check(!foreign, "en el primer update ya no queda ninguna tesela de B");
    check(pump(streamer, world, store, Vec3{10.0f, 0.0f, 10.0f}, 2) && tilesInPlace(world, world.find(s.grid_a)),
          "y las de A en su sitio (origen 0)");
    streamer.clear(world, store, false);
}

void testLoadNowPrunes(const Scenes& s) {
    std::printf("loadNow (juego, servidor): sin teselas de rejillas que ya no estan\n");
    terrain::TerrainStore store;
    store.setRoot(s.root);
    terrain::TerrainStreamer streamer;
    ecs::World world;
    std::string error;
    check(ecs::loadScene(world, s.a, &error), "abrir A");
    streamer.loadNow(world, store, Vec3{10.0f, 0.0f, 10.0f}, 500.0f);
    check(tilesOf(world).size() == 4, "A: 4 teselas ya");
    // Otra escena sin pasar por update (como el juego entre Stage::Scene y Systems).
    check(ecs::loadScene(world, s.b, &error), "abrir B");
    streamer.loadNow(world, store, Vec3{0.0f, 0.0f, 0.0f}, 500.0f);
    bool foreign = false;
    for (ecs::Entity t : tilesOf(world)) foreign = foreign || t.get<terrain::TerrainTile>().grid != s.grid_b;
    check(!foreign && tilesOf(world).size() == 4, "loadNow: solo las de B");
    check(tilesInPlace(world, world.find(s.grid_b)), "en su sitio desde el principio (antes de la fisica)");
    streamer.clear(world, store, false);
}

void testSameGridNewLayout(const Scenes& s) {
    std::printf("La misma rejilla con otra carpeta y otro tamano\n");
    terrain::TerrainStore store;
    store.setRoot(s.root);
    terrain::TerrainStreamer streamer;
    ecs::World world;
    ecs::Entity grid = world.create("Rejilla");
    const terrain::TerrainGrid old_settings = smallGrid("Capa1", 2, 1, 100.0f);
    grid.add<terrain::TerrainGrid>() = old_settings;
    streamer.loadNow(world, store, Vec3{10.0f, 0.0f, 10.0f}, 500.0f);
    check(tilesOf(world).size() == 2, "2 teselas de 100 m");
    terrain::TerrainGrid& g = grid.get<terrain::TerrainGrid>();
    g = smallGrid("Capa2", 4, 1, 50.0f);
    streamer.loadNow(world, store, Vec3{10.0f, 0.0f, 10.0f}, 500.0f);
    bool new_data = true;
    for (ecs::Entity t : tilesOf(world)) new_data = new_data && t.get<terrain::Terrain>().data.rfind("Capa2/", 0) == 0;
    check(tilesOf(world).size() == 4 && new_data, "las 4 nuevas, ninguna de la carpeta vieja");
    check(tilesInPlace(world, grid), "colocadas con el tamano nuevo");
    check(!store.findPath(terrain::tilePath(old_settings, 0, 0)) && !store.findPath(terrain::tilePath(old_settings, 1, 0)),
          "los datos viejos se soltaron");
    streamer.clear(world, store, false);
}

void testStopPlayKeepsTiles(const Scenes& s) {
    std::printf("Parar Play tras desplazar el origen: las teselas no saltan\n");
    terrain::TerrainStore store;
    store.setRoot(s.root);
    terrain::TerrainStreamer streamer;
    ecs::World world;
    ecs::Entity grid = world.create("Rejilla");
    grid.setWorldPosition(Vec3{-100.0f, 0.0f, -100.0f});
    grid.add<terrain::TerrainGrid>() = smallGrid("Juego", 2, 2, 100.0f);
    streamer.loadNow(world, store, Vec3{}, 500.0f);
    check(tilesOf(world).size() == 4, "4 teselas");
    const std::string snapshot = ecs::serializeWorld(world);  // al dar Play
    const std::uint64_t generation = world.sceneGeneration();
    check(ecs::updateFloatingOrigin(world, Vec3{3000.0f, 0.0f, 0.0f}).has_value(), "en Play el origen se desplaza");
    ecs::deserializeWorld(world, snapshot);  // Stop
    grid = world.findByName("Rejilla");
    check(world.sceneGeneration() == generation && tilesOf(world).size() == 4, "las teselas se quedan (misma escena)");
    check(grid.valid() && approx(grid.worldPosition(), Vec3{-100.0f, 0.0f, -100.0f}), "la rejilla en su sitio");
    check(tilesInPlace(world, grid), "y las teselas tambien, sin esperar a ningun update");
    streamer.clear(world, store, false);
}

void testPhysicsSwitch(const Scenes& s) {
    std::printf("Fisica: ningun collider del terreno de A en B\n");
    physics::registerPhysicsComponents();
    terrain::TerrainStore store;
    store.setRoot(s.root);
    terrain::TerrainStreamer streamer;
    physics::PhysicsSystem physics;
    physics.setTerrainProvider([&](ecs::Entity e) -> std::shared_ptr<const terrain::TerrainData> {
        const terrain::Terrain* comp = e.tryGet<terrain::Terrain>();
        return comp != nullptr ? store.get(*comp) : nullptr;
    });
    ecs::World world;
    std::string error;
    check(ecs::loadScene(world, s.a, &error), "abrir A");
    streamer.loadNow(world, store, Vec3{10.0f, 0.0f, 10.0f}, 500.0f);
    physics.start(world);
    physics.flushPendingTerrain();
    physics.update(world, 1.0f / 60.0f);
    physics::RaycastHit hit;
    check(physics.raycast(Vec3{350.0f, 100.0f, 50.0f}, Vec3{0.0f, -1.0f, 0.0f}, 200.0f, hit), "A: suelo en x = 350");
    // Cambio de escena con colliders de terreno a medias.
    physics.update(world, 1.0f / 60.0f);
    ecs::SceneResetTargets targets;
    targets.terrain_streamer = &streamer;
    targets.terrain_store = &store;
    targets.physics = &physics;
    ecs::SceneResetOptions options;
    options.discard_terrain_data = true;
    ecs::resetSceneState(world, targets, options);
    check(ecs::loadScene(world, s.b, &error), "abrir B");
    streamer.loadNow(world, store, Vec3{}, 500.0f);
    physics.start(world);
    physics.flushPendingTerrain();
    physics.update(world, 1.0f / 60.0f);
    check(!physics.raycast(Vec3{350.0f, 100.0f, 50.0f}, Vec3{0.0f, -1.0f, 0.0f}, 200.0f, hit),
          "B: donde estaba A ya no hay suelo");
    check(physics.raycast(Vec3{10.0f, 100.0f, 10.0f}, Vec3{0.0f, -1.0f, 0.0f}, 200.0f, hit) && hit.entity.valid() &&
              hit.entity.has<terrain::TerrainTile>() && hit.entity.get<terrain::TerrainTile>().grid == s.grid_b,
          "B: su propio terreno");
    physics.stop();
    streamer.clear(world, store, false);
}

void testPartitionSwitch() {
    std::printf("World Partition: celdas de A leyendose al cambiar a B\n");
    const fs::path folder = tempRoot("cramion_scene_change_wp");
    const fs::path scene = folder / "A.crscene";
    {
        ecs::World editor;
        for (int i = 0; i < 10; ++i) {
            ecs::Entity e = editor.create("Roca" + std::to_string(i));
            e.setWorldPosition(Vec3{50.0f + 100.0f * static_cast<float>(i), 0.0f, 50.0f});
            e.add<ecs::MeshRenderer>();
        }
        check(worldpart::WorldPartitionSystem::writeExternalCells(editor, 100.0f, scene) == 10, "A: 10 celdas en disco");
        ecs::World a;
        ecs::Entity wpe = a.create("World Partition");
        auto& wp = wpe.add<worldpart::WorldPartition>();
        wp.cell_size = 100.0f;
        wp.load_range = 150.0f;
        wp.unload_margin = 50.0f;
        wp.loads_per_frame = 64;
        a.create("Jugador").add<worldpart::StreamingSource>();
        std::string error;
        check(ecs::saveScene(a, scene, &error), "guardar A");
    }
    const fs::path scene_b = folder / "B.crscene";
    {
        ecs::World b;
        b.create("Cubo B");
        std::string error;
        check(ecs::saveScene(b, scene_b, &error), "guardar B");
    }
    for (int round = 0; round < 2; ++round) {
        const bool with_reset = round == 0;
        std::printf(" %s\n", with_reset ? "con resetSceneState" : "sin reset (red de seguridad)");
        worldpart::WorldPartitionSystem wp;
        wp.setCacheFolder(folder / "cache");
        ecs::World world;
        std::string error;
        check(ecs::loadScene(world, scene, &error), "abrir A");
        wp.setSceneFile(scene, false);
        wp.begin(world, Vec3{});
        // El jugador corre a la otra punta: celdas de A leyendose en otros hilos.
        world.findByName("Jugador").setWorldPosition(Vec3{950.0f, 0.0f, 50.0f});
        wp.update(world, Vec3{});
        // (Informativo: con disco rapido la lectura puede haber terminado ya;
        // aun asi faltan frames para recrear sus objetos.)
        std::printf("  (A: %d celdas cargandose, %d lecturas en otros hilos)\n", wp.stats().loading_cells,
                    wp.stats().reads_in_flight);
        if (with_reset) {
            ecs::SceneResetTargets targets;
            targets.partition = &wp;
            ecs::resetSceneState(world, targets);
        }
        check(ecs::loadScene(world, scene_b, &error), "abrir B");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        for (int i = 0; i < 20; ++i) wp.update(world, Vec3{});
        bool rocks = false;
        world.forEachDepthFirst([&](ecs::Entity e) { rocks = rocks || e.name().rfind("Roca", 0) == 0; });
        check(!rocks, "ninguna roca de A entro en B");
        check(!wp.active() && world.findByName("Cubo B").valid(), "el World Partition de A se olvido");
    }
    std::error_code ec;
    fs::remove_all(folder, ec);
}

void testNavigationSwitch() {
    std::printf("Navegacion: baldosas de A en vuelo al cambiar a B\n");
    navigation::registerNavigationComponents();
    navigation::NavigationSystem nav;
    navigation::NavigationSettings settings;
    settings.cell_size = 0.25f;
    settings.cell_height = 0.1f;
    settings.tile_size = 48;
    nav.setSettings(settings);
    ecs::World world;
    const auto level = [&](float floor_top) {
        ecs::Entity floor = world.create("Suelo");
        floor.setWorldPosition(Vec3{0.0f, floor_top - 0.5f, 0.0f});
        floor.add<physics::BoxCollider>().size = Vec3{60.0f, 1.0f, 60.0f};
        ecs::Entity volume = world.create("Volumen");
        volume.setWorldPosition(Vec3{0.0f, floor_top + 2.0f, 0.0f});
        volume.add<navigation::NavMeshBounds>().size = Vec3{40.0f, 10.0f, 40.0f};
    };
    ecs::SceneResetTargets targets;
    targets.navigation = &nav;
    // Muchos cambios de escena a medio construir: antes cada uno perdia
    // trabajos de la cuenta `in_flight` y la malla dejaba de construirse.
    for (int i = 0; i < 12; ++i) {
        level(0.0f);  // A: suelo en y = 0
        nav.update(world, 0.0f, false);  // lanza baldosas
        ecs::resetSceneState(world, targets);
    }
    level(5.0f);  // B: suelo en y = 5 (mismas baldosas en XZ)
    nav.waitForBuild(world, 30.0f);
    check(!nav.building(), "B: la malla termina (sin trabajos perdidos en la cuenta)");
    check(nav.ready(), "B: hay malla");
    Vec3 p;
    check(nav.projectPoint(Vec3{3.0f, 5.0f, 3.0f}, p, 1.0f) && approx(p.y, 5.0f, 0.4f), "B: la malla esta en su suelo (y = 5)");
    check(!nav.projectPoint(Vec3{3.0f, 0.0f, 3.0f}, p, 1.0f), "ni rastro del suelo de A (y = 0)");
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    terrain::registerTerrainComponents();
    worldpart::registerWorldPartitionComponents();
    testRebase();
    const Scenes scenes = writeScenes("cramion_scene_change");
    testLoadSceneClears(scenes);
    testTerrainSwitch(scenes);
    testLoadNowPrunes(scenes);
    testSameGridNewLayout(scenes);
    testStopPlayKeepsTiles(scenes);
    testPhysicsSwitch(scenes);
    testPartitionSwitch();
    testNavigationSwitch();
    std::error_code ec;
    fs::remove_all(scenes.root, ec);
    std::printf("\n%d comprobaciones, %d fallos\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
