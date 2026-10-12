// Pruebas del World Partition (consola, sin GPU): descargar lo lejano al
// empezar, cargar y descargar al mover la fuente con su presupuesto por frame,
// los objetos creados en Play, el mismo UUID al volver, restaurar todo al
// parar y lo que tarda un frame con muchos objetos. Devuelve 0 si todo va.

#include "CramionCore/ecs/Components.h"
#include "CramionCore/ecs/SceneSerializer.h"
#include "CramionCore/ecs/World.h"
#include "CramionCore/world/WorldPartition.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>

using namespace cramion;
using core::Vec3;

namespace {

int failures = 0;
int checks = 0;

void check(bool condition, const char* what) {
    ++checks;
    std::printf("  %s %s\n", condition ? "OK   " : "FALLO", what);
    if (!condition) ++failures;
}

ecs::Entity prop(ecs::World& world, const std::string& name, float x, float z) {
    ecs::Entity e = world.create(name);
    e.setWorldPosition(Vec3{x, 0.0f, z});
    e.add<ecs::MeshRenderer>();
    return e;
}

// Una fila de objetos cada 100 m en X (celdas de 100 m: uno por celda).
ecs::World& row(ecs::World& world, int count) {
    for (int i = 0; i < count; ++i) prop(world, "Roca" + std::to_string(i), 50.0f + 100.0f * static_cast<float>(i), 50.0f);
    return world;
}

// Unos frames hasta que no quede nada leyendose de disco (otro hilo).
void settle(worldpart::WorldPartitionSystem& wp, ecs::World& world) {
    for (int i = 0; i < 3; ++i) wp.update(world, Vec3{});
    for (int i = 0; i < 400 && wp.stats().loading_cells > 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        wp.update(world, Vec3{});
    }
    for (int i = 0; i < 3; ++i) wp.update(world, Vec3{});
}

ecs::Entity partition(ecs::World& world, int loads, int unloads) {
    ecs::Entity e = world.create("World Partition");
    auto& wp = e.add<worldpart::WorldPartition>();
    wp.cell_size = 100.0f;
    wp.load_range = 150.0f;
    wp.unload_margin = 50.0f;
    wp.loads_per_frame = loads;
    wp.unloads_per_frame = unloads;
    return e;
}

}  // namespace

int main() {
    worldpart::registerWorldPartitionComponents();

    std::printf("Al empezar\n");
    {
        ecs::World world;
        partition(world, 64, 64);
        row(world, 10);
        ecs::Entity player = world.create("Jugador");
        player.add<worldpart::StreamingSource>();
        player.setWorldPosition(Vec3{50.0f, 0.0f, 50.0f});
        Uuid roca9{};
        world.forEachDepthFirst([&](ecs::Entity e) {
            if (e.name() == "Roca9") roca9 = e.uuid();
        });
        const std::size_t before = world.entityCount();

        worldpart::WorldPartitionSystem wp;
        wp.begin(world, Vec3{});
        check(wp.active(), "activo con un WorldPartition");
        check(wp.stats().streamed_entities == 10, "10 objetos que se pueden descargar (no el jugador ni el WP)");
        // Fuente en x=50: celdas 0 y 1 a menos de 150 m (la 1 empieza en 100).
        check(wp.cellLoaded({0, 0}) && wp.cellLoaded({1, 0}), "las celdas cerca del jugador, cargadas");
        check(!wp.cellLoaded({5, 0}) && !wp.cellLoaded({9, 0}), "las lejanas, descargadas");
        check(!world.find(roca9).valid(), "la roca lejana ya no esta en el mundo");

        std::printf("Mover la fuente\n");
        player.setWorldPosition(Vec3{950.0f, 0.0f, 50.0f});
        settle(wp, world);
        check(wp.cellLoaded({9, 0}), "la celda del jugador se carga al llegar");
        check(world.find(roca9).valid(), "la roca vuelve con el mismo UUID");
        check(!wp.cellLoaded({0, 0}), "la celda de donde salio se descarga");

        std::printf("Creado en Play\n");
        ecs::Entity spawned = prop(world, "Disparo", 950.0f, 50.0f);
        const Uuid spawned_uuid = spawned.uuid();
        for (int i = 0; i < 12; ++i) wp.update(world, Vec3{});  // un repaso cada 10 frames
        player.setWorldPosition(Vec3{50.0f, 0.0f, 50.0f});
        settle(wp, world);
        check(!world.find(spawned_uuid).valid(), "lo creado en Play tambien se descarga");

        std::printf("Al parar\n");
        wp.end(world, true);
        check(world.entityCount() == before + 1, "todo vuelve (y lo creado en Play)");
        check(world.find(roca9).valid() && world.find(spawned_uuid).valid(), "con sus UUID");
    }

    std::printf("Presupuesto por frame\n");
    {
        ecs::World world;
        partition(world, 1, 2);
        row(world, 10);
        ecs::Entity player = world.create("Jugador");
        player.add<worldpart::StreamingSource>().range_scale = 100.0f;  // todo a la vista
        worldpart::WorldPartitionSystem wp;
        wp.begin(world, Vec3{});
        check(wp.stats().loaded_cells == 10, "todo cargado con la fuente que lo ve todo");
        player.get<worldpart::StreamingSource>().range_scale = 0.01f;
        player.setWorldPosition(Vec3{-5000.0f, 0.0f, 0.0f});
        wp.update(world, Vec3{});
        check(wp.stats().loaded_cells == 8, "2 descargas por frame como mucho");
        for (int i = 0; i < 10; ++i) wp.update(world, Vec3{});
        check(wp.stats().loaded_cells == 0, "y al rato, todas");
        player.get<worldpart::StreamingSource>().range_scale = 100.0f;
        wp.update(world, Vec3{});
        check(wp.stats().loaded_cells + wp.stats().loading_cells == 1, "1 carga por frame como mucho");
        wp.end(world, true);
    }

    std::printf("Celdas descargadas en disco\n");
    {
        namespace fs = std::filesystem;
        const fs::path cache = fs::temp_directory_path() / "cramion_wp_cache_test";
        std::error_code ec;
        fs::remove_all(cache, ec);
        ecs::World world;
        partition(world, 64, 64);
        row(world, 10);
        ecs::Entity player = world.create("Jugador");
        player.add<worldpart::StreamingSource>();
        player.setWorldPosition(Vec3{50.0f, 0.0f, 50.0f});
        Uuid roca9{};
        world.forEachDepthFirst([&](ecs::Entity e) {
            if (e.name() == "Roca9") roca9 = e.uuid();
        });
        worldpart::WorldPartitionSystem wp;
        wp.setCacheFolder(cache);
        wp.begin(world, Vec3{});
        // La escritura va en el hilo de IO (wp.AsyncCells): unos frames.
        for (int i = 0; i < 400 && wp.stats().writes_in_flight > 0; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            wp.update(world, Vec3{});
        }
        wp.update(world, Vec3{});
        check(wp.stats().disk_bytes > 0 && wp.stats().stored_bytes == 0, "lo descargado va a disco, no a memoria");
        player.setWorldPosition(Vec3{950.0f, 0.0f, 50.0f});
        bool back = false;
        for (int i = 0; i < 400 && !back; ++i) {
            wp.update(world, Vec3{});
            back = world.find(roca9).valid();
            if (!back) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        check(back, "se lee en otro hilo y vuelve con el mismo UUID");
        wp.end(world, true);
        check(world.entityCount() == 10 + 2, "al parar, todo de vuelta");
        check(!fs::exists(cache) || fs::is_empty(cache), "la cache de la sesion se borra");
        fs::remove_all(cache, ec);
    }

    std::printf("Celdas en disco de la escena (mundos grandes)\n");
    {
        namespace fs = std::filesystem;
        const fs::path folder = fs::temp_directory_path() / "cramion_wp_scene_test";
        std::error_code ec;
        fs::remove_all(folder, ec);
        fs::create_directories(folder, ec);
        const fs::path scene = folder / "Mundo.crscene";
        {
            ecs::World editor;
            row(editor, 10);
            check(worldpart::WorldPartitionSystem::writeExternalCells(editor, 100.0f, scene) == 10, "una celda por archivo (10)");
            check(fs::exists(worldpart::WorldPartitionSystem::cellsFolder(scene) / "cell_9_0.crcell"), "en Mundo.cells/");
            ecs::World reopened;
            check(worldpart::WorldPartitionSystem::loadExternalCells(reopened, scene) == 10, "el editor las vuelve a leer todas");
        }
        // El juego: la escena sin esos objetos; solo se leen los cercanos.
        ecs::World world;
        partition(world, 64, 64);
        ecs::Entity player = world.create("Jugador");
        player.add<worldpart::StreamingSource>();
        player.setWorldPosition(Vec3{50.0f, 0.0f, 50.0f});
        worldpart::WorldPartitionSystem wp;
        wp.setCacheFolder(folder / "cache");
        wp.setSceneFile(scene, false);
        wp.begin(world, Vec3{});
        check(wp.stats().external_cells == 10, "las 10 celdas registradas sin leerlas");
        check(world.entityCount() >= 2 + 2 && world.entityCount() <= 3 + 2 && !world.findByName("Roca9").valid(),
              "al empezar, solo las cercanas en el mundo");
        player.setWorldPosition(Vec3{950.0f, 0.0f, 50.0f});
        for (int i = 0; i < 400 && !wp.cellLoaded({9, 0}); ++i) {
            wp.update(world, Vec3{});
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        check(wp.cellLoaded({9, 0}) && world.findByName("Roca9").valid(), "al llegar, la celda se lee de disco");
        wp.end(world, true);
        check(world.entityCount() == 10 + 2, "al parar, las 10");
        worldpart::WorldPartitionSystem::removeExternalCells(scene);
        check(!worldpart::WorldPartitionSystem::hasExternalCells(scene), "y se borran al apagarlo");
        fs::remove_all(folder, ec);
    }

    std::printf("Muchos objetos\n");
    {
        ecs::World world;
        partition(world, 4, 4);
        for (int i = 0; i < 20000; ++i) {
            ecs::Entity e = prop(world, "Arbol", static_cast<float>(i % 200) * 20.0f, static_cast<float>(i / 200) * 20.0f);
            world.create("Hoja", e);  // un hijo: antes se recorria cada frame
        }
        ecs::Entity player = world.create("Jugador");
        player.add<worldpart::StreamingSource>();
        worldpart::WorldPartitionSystem wp;
        wp.begin(world, Vec3{});
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 100; ++i) wp.update(world, Vec3{});
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 100.0;
        std::printf("  frame sin moverse: %.3f ms (20000 raices con un hijo)\n", ms);
        check(ms < 2.0, "un frame quieto cuesta menos de 2 ms");
        // Todo cargado (lo caro: antes cada frame miraba cada objeto y su hijo).
        player.get<worldpart::StreamingSource>().range_scale = 100.0f;
        // Leer y parsear va en otros hilos (wp.AsyncCells): con pausas cortas.
        for (int i = 0; i < 4000 && wp.stats().loaded_cells < wp.stats().cells; ++i) {
            wp.update(world, Vec3{});
            if (wp.stats().loading_cells > 0) std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
        check(wp.stats().loaded_cells == wp.stats().cells, "todo cargado al ampliar la distancia");
        const auto t1 = std::chrono::steady_clock::now();
        for (int i = 0; i < 100; ++i) wp.update(world, Vec3{});
        const double loaded_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count() / 100.0;
        std::printf("  frame con las 20000 cargadas: %.3f ms\n", loaded_ms);
        check(loaded_ms < 2.0, "con todo cargado, menos de 2 ms");
        wp.end(world, true);
        check(world.entityCount() == 40000 + 2, "todo vuelve al parar");
    }

    std::printf("UUID repetidos\n");
    {
        ecs::World world;
        ecs::Entity a = prop(world, "Roca", 10.0f, 10.0f);
        world.create("Hijo", a);
        const Uuid id = a.uuid();
        const std::string saved = ecs::serializeEntity(world, a);
        const std::size_t before = world.entityCount();
        // Recrear lo guardado con el original aun vivo: no se duplica.
        const ecs::Entity again = ecs::restoreEntities(world, saved);
        check(!again.valid() && world.entityCount() == before, "restaurar un UUID que ya esta: no se crea otra copia");
        check(world.find(id) == a, "y el original sigue encontrandose por su UUID");
        // Lo mismo parseado en otro hilo.
        auto parsed = ecs::parseEntities(saved);
        check(parsed != nullptr, "parsear en otro hilo (sin mundo)");
        check(parsed && !ecs::restoreParsedEntities(world, *parsed).valid() && world.entityCount() == before,
              "tampoco desde lo parseado");
        // Borrar el original y recrear: vuelve con el mismo UUID y su hijo.
        world.destroy(a);
        const ecs::Entity back = parsed ? ecs::restoreParsedEntities(world, *parsed) : ecs::Entity{};
        check(back.valid() && back.uuid() == id && world.find(id) == back && back.children().size() == 1,
              "borrado y recreado: mismo UUID, con su hijo");
        // Destruir otra entidad no toca el mapa de UUID del resto.
        ecs::Entity other = world.create("Otro");
        world.destroy(other);
        check(world.find(id) == back, "destruir otra entidad no toca el mapa de UUID ajeno");
    }

    std::printf("\n%d de %d comprobaciones bien\n", checks - failures, checks);
    return failures == 0 ? 0 : 1;
}
