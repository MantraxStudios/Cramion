// Pruebas del World Partition (consola, sin GPU): descargar lo lejano al
// empezar, cargar y descargar al mover la fuente con su presupuesto por frame,
// los objetos creados en Play, el mismo UUID al volver, restaurar todo al
// parar y lo que tarda un frame con muchos objetos. Devuelve 0 si todo va.

#include "CramionCore/ecs/Components.h"
#include "CramionCore/ecs/World.h"
#include "CramionCore/world/WorldPartition.h"

#include <chrono>
#include <cstdio>
#include <string>

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
        for (int i = 0; i < 3; ++i) wp.update(world, Vec3{});
        check(wp.cellLoaded({9, 0}), "la celda del jugador se carga al llegar");
        check(world.find(roca9).valid(), "la roca vuelve con el mismo UUID");
        check(!wp.cellLoaded({0, 0}), "la celda de donde salio se descarga");

        std::printf("Creado en Play\n");
        ecs::Entity spawned = prop(world, "Disparo", 950.0f, 50.0f);
        const Uuid spawned_uuid = spawned.uuid();
        for (int i = 0; i < 12; ++i) wp.update(world, Vec3{});  // un repaso cada 10 frames
        player.setWorldPosition(Vec3{50.0f, 0.0f, 50.0f});
        for (int i = 0; i < 3; ++i) wp.update(world, Vec3{});
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
        check(wp.stats().loaded_cells == 1, "1 carga por frame como mucho");
        wp.end(world, true);
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
        for (int i = 0; i < 400 && wp.stats().loaded_cells < wp.stats().cells; ++i) wp.update(world, Vec3{});
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

    std::printf("\n%d de %d comprobaciones bien\n", checks - failures, checks);
    return failures == 0 ? 0 : 1;
}
