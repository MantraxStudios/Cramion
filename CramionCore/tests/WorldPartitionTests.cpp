// Pruebas del World Partition (consola, sin GPU): descargar lo lejano al
// empezar, cargar y descargar al mover la fuente con su presupuesto por frame,
// los objetos creados en Play, el mismo UUID al volver, restaurar todo al
// parar y lo que tarda un frame con muchos objetos. Fase 1: varias rejillas,
// Data Layers, dependencias entre celdas, lo que se guarda con la escena y
// las celdas en disco con rejillas y capas (y las de antes). Devuelve 0 si
// todo va.

#include "CramionCore/ecs/Components.h"
#include "CramionCore/ecs/SceneSerializer.h"
#include "CramionCore/ecs/World.h"
#include "CramionCore/world/WorldPartition.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
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

// Frames (con pausas cortas: hay lecturas y escrituras en otros hilos) hasta
// que se cumpla `done` (como mucho ~1 s).
template <typename Fn>
bool until(worldpart::WorldPartitionSystem& wp, ecs::World& world, Fn&& done) {
    for (int i = 0; i < 500; ++i) {
        wp.update(world, Vec3{});
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

// Un objeto con PartitionObject (rejilla y/o capas).
ecs::Entity placed(ecs::World& world, const std::string& name, float x, float z, const std::string& grid,
                   const std::string& layers) {
    ecs::Entity e = prop(world, name, x, z);
    auto& po = e.add<worldpart::PartitionObject>();
    po.grid = grid;
    po.data_layers = layers;
    return e;
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

    std::printf("Varias rejillas\n");
    {
        ecs::World world;
        auto& settings = partition(world, 64, 64).get<worldpart::WorldPartition>();
        worldpart::PartitionGrid big;
        big.name = "Grandes";
        big.cell_size = 400.0f;
        big.load_range = 2000.0f;
        big.tag = "Edificio";
        settings.grids.push_back(big);
        ecs::Entity torre = prop(world, "Torre", 1050.0f, 50.0f);
        torre.setTag("Edificio");
        placed(world, "Faro", 1550.0f, 50.0f, "Grandes", "");
        placed(world, "Boya", 1150.0f, 50.0f, "NoExiste", "");  // rejilla que no hay: la principal
        prop(world, "Roca", 1050.0f, 50.0f);
        ecs::Entity player = world.create("Jugador");
        player.add<worldpart::StreamingSource>();
        player.setWorldPosition(Vec3{50.0f, 0.0f, 50.0f});
        check(worldpart::WorldPartitionSystem::gridOf(settings, torre) == 1, "la etiqueta lleva a su rejilla");
        check(worldpart::WorldPartitionSystem::gridOf(settings, world.findByName("Faro")) == 1, "el PartitionObject pide la suya");
        check(worldpart::WorldPartitionSystem::gridOf(settings, world.findByName("Boya")) == 0, "una rejilla que no existe: la principal");
        worldpart::WorldPartitionSystem wp;
        wp.begin(world, Vec3{});
        // Torre: celda 2 de 400 m (800..1200), a 750 m; Faro: celda 3, a 1150 m.
        check(world.findByName("Torre").valid() && world.findByName("Faro").valid(), "lo grande se queda (2 km de carga)");
        check(wp.cellLoaded(1, {2, 0}) && wp.cellLoaded(1, {3, 0}), "sus celdas de 400 m, cargadas");
        check(!world.findByName("Roca").valid() && !world.findByName("Boya").valid() && !wp.cellLoaded({10, 0}),
              "lo pequeno al mismo sitio, descargado (150 m)");
        bool sizes = true;
        for (const worldpart::CellInfo& c : wp.cellInfos()) sizes = sizes && c.cell_size == (c.grid == 1 ? 400.0f : 100.0f);
        check(sizes, "cada celda con el tamano de su rejilla");
        player.setWorldPosition(Vec3{-3000.0f, 0.0f, 50.0f});
        check(until(wp, world, [&] { return !world.findByName("Torre").valid() && !world.findByName("Faro").valid(); }),
              "lejos de todo, lo grande tambien sale");
        player.setWorldPosition(Vec3{1050.0f, 0.0f, 50.0f});
        check(until(wp, world, [&] { return world.findByName("Roca").valid() && world.findByName("Torre").valid(); }),
              "y vuelve todo al llegar");
        wp.end(world, true);
        check(world.entityCount() == 4 + 2, "al parar, las 4 y el resto");
    }

    std::printf("Data Layers\n");
    {
        check(worldpart::WorldPartitionSystem::canonicalLayers(" Ruinas ,Noche,,Ruinas") == "Noche,Ruinas",
              "capas ordenadas, sin espacios ni repetidas");
        ecs::World world;
        auto& settings = partition(world, 64, 64).get<worldpart::WorldPartition>();
        worldpart::DataLayer noche;
        noche.name = "Noche";
        noche.active = false;
        worldpart::DataLayer ruinas;
        ruinas.name = "Ruinas";
        settings.data_layers = {noche, ruinas};
        placed(world, "Farola", 50.0f, 50.0f, "", "Noche");
        placed(world, "Columna", 60.0f, 50.0f, "", "Ruinas");
        placed(world, "Mixto", 70.0f, 50.0f, "", "Ruinas, Noche");
        prop(world, "Roca", 80.0f, 50.0f);
        ecs::Entity luz = world.create("Luz");  // sin malla, pero en una capa
        luz.setWorldPosition(Vec3{40.0f, 3.0f, 50.0f});
        luz.add<ecs::Light>();
        luz.add<worldpart::PartitionObject>().data_layers = "Noche";
        ecs::Entity solo = world.create("Sin malla");
        solo.add<ecs::Light>();
        check(worldpart::WorldPartitionSystem::streamable(luz) && !worldpart::WorldPartitionSystem::streamable(solo),
              "con capa se descarga aunque no tenga malla (sin ella, no)");
        ecs::Entity player = world.create("Jugador");
        player.add<worldpart::StreamingSource>();
        player.setWorldPosition(Vec3{50.0f, 0.0f, 50.0f});
        const std::size_t before = world.entityCount();
        worldpart::WorldPartitionSystem wp;
        wp.begin(world, Vec3{});
        check(!world.findByName("Farola").valid() && !world.findByName("Mixto").valid() && !world.findByName("Luz").valid(),
              "la capa apagada al empezar no se carga (aunque este cerca)");
        check(world.findByName("Columna").valid() && world.findByName("Roca").valid(), "lo de la capa activa y lo sin capa, si");
        bool mixed = false;
        for (const worldpart::CellInfo& c : wp.cellInfos()) mixed = mixed || c.layers == "Noche,Ruinas";
        check(mixed && wp.stats().cells == 4, "una celda por cada juego de capas en el mismo sitio");
        wp.setDataLayerActive("Noche", true);
        check(until(wp, world, [&] { return world.findByName("Farola").valid() && world.findByName("Mixto").valid(); }),
              "encender la capa la carga");
        wp.setDataLayerActive("Ruinas", false);
        check(until(wp, world, [&] { return !world.findByName("Columna").valid() && !world.findByName("Mixto").valid(); }),
              "apagar otra la descarga (y lo que esta en las dos)");
        check(world.findByName("Farola").valid() && world.findByName("Roca").valid(), "lo demas se queda");
        check(!wp.dataLayerActive("Ruinas") && wp.dataLayerActive("Noche") && wp.dataLayerActive("SinDeclarar"),
              "estado de las capas (sin declarar = activa)");
        wp.end(world, true);
        check(world.entityCount() == before, "al parar, todo de vuelta");
        // setDataLayerActive antes de empezar: vale al empezar.
        worldpart::WorldPartitionSystem early;
        early.setDataLayerActive("Ruinas", false);
        early.begin(world, Vec3{});
        check(!world.findByName("Columna").valid() && !world.findByName("Farola").valid(),
              "lo tocado antes de begin se aplica (y lo del componente)");
        early.end(world, true);
        check(world.entityCount() == before && early.dataLayerActive("Ruinas"), "end lo olvida");
    }

    std::printf("Dependencias entre celdas\n");
    {
        ecs::World world;
        auto& settings = partition(world, 64, 64).get<worldpart::WorldPartition>();
        worldpart::CellDependency dep;
        dep.x = 0;
        dep.z = 0;
        dep.load_x = 20;
        dep.load_z = 0;
        settings.dependencies.push_back(dep);
        prop(world, "Fachada", 50.0f, 50.0f);
        prop(world, "Interior", 2050.0f, 50.0f);  // celda 20, a 2 km
        prop(world, "Lejos", 1050.0f, 50.0f);
        ecs::Entity player = world.create("Jugador");
        player.add<worldpart::StreamingSource>();
        player.setWorldPosition(Vec3{50.0f, 0.0f, 50.0f});
        worldpart::WorldPartitionSystem wp;
        wp.begin(world, Vec3{});
        check(world.findByName("Interior").valid() && wp.cellLoaded({20, 0}), "la celda 0 carga la 20 (aunque este lejos)");
        check(!world.findByName("Lejos").valid(), "lo que no pide nadie, lejos, fuera");
        player.setWorldPosition(Vec3{-5000.0f, 0.0f, 50.0f});
        check(until(wp, world, [&] { return !world.findByName("Fachada").valid() && !world.findByName("Interior").valid(); }),
              "al irse, las dos salen");
        player.setWorldPosition(Vec3{50.0f, 0.0f, 50.0f});
        check(until(wp, world, [&] { return world.findByName("Fachada").valid() && world.findByName("Interior").valid(); }),
              "al volver, las dos entran");
        wp.end(world, true);
        // En bucle (0 -> 20 -> 0) y con una rejilla que no existe: no se cuelga.
        worldpart::CellDependency back;
        back.x = 20;
        back.load_x = 0;
        settings.dependencies.push_back(back);
        worldpart::CellDependency broken;
        broken.grid = "NoExiste";
        settings.dependencies.push_back(broken);
        worldpart::WorldPartitionSystem loop;
        loop.begin(world, Vec3{});
        for (int i = 0; i < 20; ++i) loop.update(world, Vec3{});
        check(world.findByName("Interior").valid() && !world.findByName("Lejos").valid(), "dependencias en bucle: sin colgarse");
        loop.end(world, true);
    }

    std::printf("Se guarda con la escena\n");
    {
        ecs::World world;
        ecs::Entity e = world.create("World Partition");
        auto& settings = e.add<worldpart::WorldPartition>();
        worldpart::PartitionGrid g;
        g.name = "Grandes";
        g.cell_size = 512.0f;
        g.load_range = 3000.0f;
        g.tag = "Edificio";
        settings.grids.push_back(g);
        worldpart::DataLayer l;
        l.name = "Noche";
        l.active = false;
        l.editor_visible = false;
        settings.data_layers.push_back(l);
        worldpart::CellDependency d;
        d.grid = "Grandes";
        d.x = 1;
        d.z = -2;
        d.load_x = 7;
        d.load_z = 8;
        settings.dependencies.push_back(d);
        placed(world, "Farola", 10.0f, 10.0f, "Grandes", "Noche");
        ecs::World copy;
        std::string error;
        check(ecs::deserializeWorld(copy, ecs::serializeWorld(world), &error), "escena guardada y leida");
        const ecs::Entity ce = copy.findByName("World Partition");
        const worldpart::WorldPartition* cw = ce.valid() ? ce.tryGet<worldpart::WorldPartition>() : nullptr;
        check(cw != nullptr && cw->grids.size() == 1 && cw->grids[0].name == "Grandes" && cw->grids[0].cell_size == 512.0f &&
                  cw->grids[0].load_range == 3000.0f && cw->grids[0].tag == "Edificio",
              "las rejillas");
        check(cw != nullptr && cw->data_layers.size() == 1 && cw->data_layers[0].name == "Noche" && !cw->data_layers[0].active &&
                  !cw->data_layers[0].editor_visible,
              "las Data Layers");
        check(cw != nullptr && cw->dependencies.size() == 1 && cw->dependencies[0].grid == "Grandes" &&
                  cw->dependencies[0].x == 1 && cw->dependencies[0].z == -2 && cw->dependencies[0].load_x == 7 &&
                  cw->dependencies[0].load_z == 8,
              "las dependencias");
        const ecs::Entity cf = copy.findByName("Farola");
        const worldpart::PartitionObject* cpo = cf.valid() ? cf.tryGet<worldpart::PartitionObject>() : nullptr;
        check(cpo != nullptr && cpo->grid == "Grandes" && cpo->data_layers == "Noche", "el PartitionObject del objeto");
        // Una escena de antes (sin las listas): una rejilla, sin capas.
        ecs::World old;
        ecs::Entity oe = old.create("World Partition");
        check(ecs::componentFromJson(old, oe, "WorldPartition", R"({"cell_size": 64.0, "load_range": 200.0, "enabled": true})"),
              "un WorldPartition de antes se lee");
        const worldpart::WorldPartition* ow = oe.tryGet<worldpart::WorldPartition>();
        check(ow != nullptr && ow->cell_size == 64.0f && ow->grids.empty() && ow->data_layers.empty() && ow->dependencies.empty(),
              "y queda como antes (sin rejillas, capas ni dependencias)");
    }

    std::printf("Celdas en disco con rejillas y capas\n");
    {
        namespace fs = std::filesystem;
        const fs::path folder = fs::temp_directory_path() / "cramion_wp_layers_test";
        std::error_code ec;
        fs::remove_all(folder, ec);
        fs::create_directories(folder, ec);
        const fs::path scene = folder / "Mundo.crscene";
        worldpart::WorldPartition settings;
        settings.cell_size = 100.0f;
        settings.load_range = 150.0f;
        settings.unload_margin = 50.0f;
        settings.loads_per_frame = 64;
        settings.unloads_per_frame = 64;
        worldpart::PartitionGrid big;
        big.name = "Grandes";
        big.cell_size = 400.0f;
        big.load_range = 2000.0f;
        big.tag = "Edificio";
        settings.grids.push_back(big);
        worldpart::DataLayer noche;
        noche.name = "Noche";
        noche.active = false;
        settings.data_layers.push_back(noche);
        {
            ecs::World editor;
            prop(editor, "Roca", 50.0f, 50.0f);
            prop(editor, "Torre", 1050.0f, 50.0f).setTag("Edificio");
            placed(editor, "Farola", 60.0f, 50.0f, "", "Noche");
            check(worldpart::WorldPartitionSystem::writeExternalCells(editor, settings, scene) == 3,
                  "3 celdas: principal, la de la capa y la de la otra rejilla");
            const fs::path cells = worldpart::WorldPartitionSystem::cellsFolder(scene);
            int files = 0;
            for (const auto& entry : fs::directory_iterator(cells, ec)) files += entry.path().extension() == ".crcell" ? 1 : 0;
            check(files == 3 && fs::exists(cells / "cell_0_0.crcell"), "la principal sin capas con el nombre de siempre");
            ecs::World reopened;
            check(worldpart::WorldPartitionSystem::loadExternalCells(reopened, scene) == 3, "el editor las lee todas");
            // Una celda de antes (version 2, sin rejilla ni capas): la principal.
            ecs::World tmp;
            const std::string text = ecs::serializeEntity(tmp, prop(tmp, "Vieja", 550.0f, 50.0f));
            std::ofstream out(cells / "cell_5_0.crcell", std::ios::binary);
            out << "#CramionCell 2 1 0 0 0\n" << text << '\n';
        }
        ecs::World world;
        world.create("World Partition").add<worldpart::WorldPartition>() = settings;
        ecs::Entity player = world.create("Jugador");
        player.add<worldpart::StreamingSource>();
        player.setWorldPosition(Vec3{50.0f, 0.0f, 50.0f});
        worldpart::WorldPartitionSystem wp;
        wp.setCacheFolder(folder / "cache");
        wp.setSceneFile(scene, false);
        wp.begin(world, Vec3{});
        check(wp.stats().external_cells == 4, "las 4 celdas registradas (tambien la de antes)");
        check(world.findByName("Roca").valid() && world.findByName("Torre").valid(), "cerca: la principal y la grande");
        check(!world.findByName("Farola").valid() && !world.findByName("Vieja").valid(), "la capa apagada y lo lejano, no");
        wp.setDataLayerActive("Noche", true);
        check(until(wp, world, [&] { return world.findByName("Farola").valid(); }), "al encender la capa se lee de disco");
        player.setWorldPosition(Vec3{550.0f, 0.0f, 50.0f});
        check(until(wp, world, [&] { return world.findByName("Vieja").valid(); }), "la celda de antes se lee como la principal");
        wp.end(world, true);
        check(world.entityCount() == 4 + 2, "al parar, las 4");
        fs::remove_all(folder, ec);
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
