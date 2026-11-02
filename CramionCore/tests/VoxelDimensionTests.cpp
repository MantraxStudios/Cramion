// Pruebas de las dimensiones del mundo de bloques (parte de CramionVoxelTests):
// bloques y texturas nuevos, cada generador determinista, con su contenido y
// dentro de presupuesto, cambio de dimension en caliente ida y vuelta (con y
// sin nombre) conservando lo construido en cada una, spawnPoint valido y el
// ambiente que el motor aplica a la escena y deshace al volver.

#include "CramionCore/ecs/Components.h"
#include "CramionCore/ecs/World.h"
#include "CramionCore/environment/Environment.h"
#include "CramionCore/voxel/Voxel.h"
#include "CramionCore/water/Water.h"

#include "../src/voxel/VoxelInternal.h"

#include <CramionFX/vk/VoxelPass.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>

using namespace cramion;
using namespace cramion::voxel;
using core::Vec3;

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    std::printf("  %s %s\n", condition ? "OK   " : "FALLO", what);
    if (!condition) ++failures;
}

void testBlocks() {
    std::printf("Dimensiones: bloques\n");
    check(blockId("lava") == block::Lava && blockDef(block::Lava).light == 15 && !blockDef(block::Lava).solid,
          "lava: da luz, no se pisa");
    check(blockId("basalt") == block::Basalt && blockId("cloud") == block::Cloud && blockId("crystal_pink") == block::CrystalPink &&
              blockId("golden_leaves") == block::GoldenLeaves && blockId("glow_moss") == block::GlowMoss,
          "bloques de cada dimension por nombre");
    check(blockDef(block::CrystalBlue).light > 0 && blockDef(block::FireCap).light > 0 && blockDef(block::ShroomCapBlue).light > 0,
          "cristales y hongos dan luz");
    check(blockDef(block::Basalt).label == "Basalt" && blockDef(block::Cloud).label == "Cloud", "etiquetas en ingles");
    gfx::VoxelTextureLayer lava;
    buildTexture("lava", 16, {}, lava, true);
    check(lava.material.size() == 16 * 16 * 4 && lava.material[2] > 150 && lava.albedo[0] > 200, "lava: naranja y emite (color liso)");
    gfx::VoxelTextureLayer crystal;
    buildTexture("crystal_violet", 16, {}, crystal, true);
    check(crystal.material[2] > 60 && crystal.albedo[2] > crystal.albedo[1], "cristal violeta: emite y es violeta");
    const auto& dims = dimensions();
    bool labels = dims.size() == 4;
    for (const char* name : {"superficie", "abismo", "cielo", "cristal"}) {
        bool found = false;
        for (const DimensionInfo& d : dims) found = found || (d.name == name && !d.label.empty());
        labels = labels && found;
    }
    check(labels && dims[1].label == "The Cinderdeep" && dims[2].label == "The Driftreach" && dims[3].label == "Glimmerwood", "las 4 dimensiones con su etiqueta en ingles");
}

struct Census {
    std::array<long long, 256> count{};
    double ms_per_chunk = 0.0;
    bool deterministic = true;
};

Census census(VoxelGenerator profile, int seed, int cx0, int cz0, int side) {
    Census c;
    const Generator gen(seed, 62, profile);
    const auto t0 = std::chrono::steady_clock::now();
    for (int z = 0; z < side; ++z) {
        for (int x = 0; x < side; ++x) {
            ChunkData chunk;
            chunk.cx = cx0 + x;
            chunk.cz = cz0 + z;
            gen.generate(chunk);
            for (const BlockId id : chunk.blocks) ++c.count[id];
        }
    }
    c.ms_per_chunk = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / (side * side);
    // Determinista: el mismo chunk, otro generador con la misma semilla, en otro orden.
    const Generator again(seed, 62, profile);
    for (int i = 0; i < 3; ++i) {
        ChunkData a, b;
        a.cx = b.cx = cx0 + i * 3;
        a.cz = b.cz = cz0 + i;
        again.generate(b);
        gen.generate(a);
        c.deterministic = c.deterministic && a.blocks == b.blocks && a.grass_tint == b.grass_tint;
    }
    return c;
}

void testGenerators() {
    std::printf("Dimensiones: generadores\n");
    {
        const Census c = census(VoxelGenerator::Abyss, 777, -6, -6, 12);
        std::printf("    abismo: %.2f ms por chunk\n", c.ms_per_chunk);
        check(c.deterministic, "abismo determinista");
        check(c.count[block::Lava] > 5000 && c.count[block::Basalt] > 100000 && c.count[block::Obsidian] > 100,
              "abismo: basalto, mares de lava y obsidiana");
        check(c.count[block::FireCap] > 100 && c.count[block::FireStem] > 20 && c.count[block::EmberGrass] > 100,
              "abismo: hongos de fuego y hierba de brasas");
        check(c.count[block::AbyssBricks] > 200, "abismo: fortalezas en ruinas");
        check(c.ms_per_chunk < 8.0, "abismo: chunk dentro de presupuesto");
    }
    {
        const Census c = census(VoxelGenerator::Sky, 777, -6, -6, 12);
        std::printf("    cielo: %.2f ms por chunk\n", c.ms_per_chunk);
        check(c.deterministic, "cielo determinista");
        check(c.count[block::Cloud] > 20000 && c.count[block::SkyGrass] > 500 && c.count[block::SkyStone] > 5000,
              "cielo: mar de nubes e islas");
        check(c.count[block::GoldenLeaves] > 300 && c.count[block::GoldenLog] > 20, "cielo: arboles dorados");
        check(c.count[block::FallingWater] > 50, "cielo: cascadas");
        check(c.count[block::SkyCrystal] > 5, "cielo: cristales");
        check(c.count[block::Water] == 0 && c.count[block::Bedrock] == 0, "cielo: sin agua del mar ni roca madre (vacio)");
        check(c.ms_per_chunk < 8.0, "cielo: chunk dentro de presupuesto");
    }
    {
        const Census c = census(VoxelGenerator::Crystal, 777, -6, -6, 12);
        std::printf("    cristal: %.2f ms por chunk\n", c.ms_per_chunk);
        check(c.deterministic, "cristal determinista");
        const long long crystals = c.count[block::CrystalPink] + c.count[block::CrystalBlue] + c.count[block::CrystalViolet] +
                                   c.count[block::CrystalGreen];
        check(crystals > 1000 && c.count[block::GlowMoss] > 5000, "cristal: cristales y musgo brillante");
        check(c.count[block::ShroomCapBlue] + c.count[block::ShroomCapViolet] > 500 && c.count[block::ShroomStem] > 50,
              "cristal: hongos gigantes");
        check(c.count[block::GlowGrass] > 500, "cristal: hierba luminosa");
        check(c.ms_per_chunk < 8.0, "cristal: chunk dentro de presupuesto");
    }
    // La altura del suelo de cada una tiene sentido.
    const Generator sky(5, 62, VoxelGenerator::Sky);
    int void_columns = 0, island_columns = 0;
    for (int i = 0; i < 400; ++i) (sky.terrainHeight(i * 7, i * 3) < 0 ? void_columns : island_columns)++;
    check(void_columns > 0 && island_columns > 0, "cielo: surfaceHeight -1 en el vacio y la cima en las islas");
    const Generator abyss(5, 62, VoxelGenerator::Abyss);
    bool floor_ok = true;
    for (int i = 0; i < 50; ++i) floor_ok = floor_ok && abyss.terrainHeight(i * 11, -i * 5) < 130;
    check(floor_ok, "abismo: surfaceHeight es el suelo de la caverna (no el techo)");
}

void makeWorld(ecs::World& world, int seed) {
    VoxelWorld& vw = world.create("Mundo").add<VoxelWorld>();
    vw.seed = seed;
    vw.render_distance = 3;
    vw.textures.clear();
    vw.generator = VoxelGenerator::Adventure;
}

bool safeSpot(VoxelSystem& vox, const Vec3& p) {
    const int x = static_cast<int>(std::floor(p.x)), y = static_cast<int>(std::floor(p.y)), z = static_cast<int>(std::floor(p.z));
    const BlockId below = vox.getBlock(x, y - 1, z);
    const BlockId feet = vox.getBlock(x, y, z), head = vox.getBlock(x, y + 1, z);
    return blockDef(below).solid && below != block::Lava && !blockDef(feet).solid && !blockDef(head).solid && feet != block::Lava &&
           head != block::Lava && feet != block::Water;
}

void testSwitching() {
    std::printf("Dimensiones: cambio en caliente\n");
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "cramion_dimension_tests";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    ecs::World world;
    makeWorld(world, 4242);
    Vec3 surface_spot{}, abyss_spot{}, sky_spot{};
    {
        VoxelSystem vox;
        vox.setSaveRoot(root);
        vox.start(world);
        check(vox.dimension() == "superficie", "empieza en la superficie");
        check(vox.newWorld("Viajes", 4242), "mundo con nombre");
        surface_spot = vox.spawnPoint(Vec3{0.0f, 0.0f, 0.0f});
        vox.waitUntilReady(surface_spot, 1, 60.0f);
        check(safeSpot(vox, surface_spot), "spawnPoint seguro en la superficie");
        vox.setBlock(static_cast<int>(std::floor(surface_spot.x)), static_cast<int>(surface_spot.y), static_cast<int>(std::floor(surface_spot.z)),
                     block::Glowstone);

        const auto t0 = std::chrono::steady_clock::now();
        check(vox.setDimension("abismo"), "pasar al abismo");
        const double switch_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::printf("    (cambio %.2f ms)\n", switch_ms);
        check(switch_ms < 50.0, "el cambio no cuelga (los chunks cargan en los hilos)");
        check(vox.dimension() == "abismo" && vox.stats().chunks == 0, "descarga los chunks de la superficie");
        check(!vox.setDimension("inventada"), "una dimension que no existe: false");
        const auto s0 = std::chrono::steady_clock::now();
        abyss_spot = vox.spawnPoint(Vec3{0.0f, 0.0f, 0.0f});
        std::printf("    (spawnPoint abismo %.1f ms: %.1f %.1f %.1f)\n",
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - s0).count(), static_cast<double>(abyss_spot.x),
                    static_cast<double>(abyss_spot.y), static_cast<double>(abyss_spot.z));
        vox.waitUntilReady(abyss_spot, 1, 60.0f);
        check(safeSpot(vox, abyss_spot) && abyss_spot.y < 140.0f, "spawnPoint seguro en el abismo (dentro de la caverna)");
        vox.setBlock(static_cast<int>(std::floor(abyss_spot.x)), static_cast<int>(abyss_spot.y), static_cast<int>(std::floor(abyss_spot.z)),
                     block::Bricks);

        check(vox.setDimension("cielo"), "pasar al cielo");
        sky_spot = vox.spawnPoint(Vec3{0.0f, 0.0f, 0.0f});
        vox.waitUntilReady(sky_spot, 1, 60.0f);
        check(safeSpot(vox, sky_spot) && vox.getBlock(static_cast<int>(std::floor(sky_spot.x)), static_cast<int>(sky_spot.y) - 1,
                                                      static_cast<int>(std::floor(sky_spot.z))) != block::Cloud,
              "spawnPoint seguro en el cielo (sobre una isla)");

        check(vox.setDimension("cristal"), "pasar al cristal");
        const Vec3 crystal_spot = vox.spawnPoint(Vec3{0.0f, 0.0f, 0.0f});
        vox.waitUntilReady(crystal_spot, 1, 60.0f);
        check(safeSpot(vox, crystal_spot), "spawnPoint seguro en el cristal");

        check(vox.setDimension("superficie"), "volver a la superficie");
        vox.waitUntilReady(surface_spot, 1, 60.0f);
        check(vox.getBlock(static_cast<int>(std::floor(surface_spot.x)), static_cast<int>(surface_spot.y),
                           static_cast<int>(std::floor(surface_spot.z))) == block::Glowstone,
              "lo construido en la superficie sigue ahi");
        check(vox.setDimension("abismo"), "y otra vez al abismo");
        vox.waitUntilReady(abyss_spot, 1, 60.0f);
        check(vox.getBlock(static_cast<int>(std::floor(abyss_spot.x)), static_cast<int>(abyss_spot.y), static_cast<int>(std::floor(abyss_spot.z))) ==
                  block::Bricks,
              "lo construido en el abismo sigue ahi");
        check(std::filesystem::exists(root / "Viajes" / "dimensions" / "abismo" / "chunks"), "el abismo se guarda en su carpeta");
        vox.saveWorld();
    }
    {
        VoxelSystem vox;
        vox.setSaveRoot(root);
        vox.start(world);
        check(vox.loadWorld("Viajes") && vox.dimension() == "abismo", "cargar el mundo vuelve a la dimension en la que estaba");
        vox.waitUntilReady(abyss_spot, 1, 60.0f);
        check(vox.getBlock(static_cast<int>(std::floor(abyss_spot.x)), static_cast<int>(abyss_spot.y), static_cast<int>(std::floor(abyss_spot.z))) ==
                  block::Bricks,
              "y con lo construido");
    }
    std::filesystem::remove_all(root, ec);

    // Sin nombre: lo construido se conserva en memoria al ir y volver.
    {
        VoxelSystem vox;
        vox.start(world);
        const Vec3 p = vox.spawnPoint(Vec3{40.0f, 0.0f, 40.0f});
        vox.waitUntilReady(p, 1, 60.0f);
        vox.setBlock(static_cast<int>(std::floor(p.x)), static_cast<int>(p.y), static_cast<int>(std::floor(p.z)), block::Bricks);
        vox.setDimension("cristal");
        vox.waitUntilReady(vox.spawnPoint(), 1, 60.0f);
        vox.setDimension("superficie");
        vox.waitUntilReady(p, 1, 60.0f);
        check(vox.getBlock(static_cast<int>(std::floor(p.x)), static_cast<int>(p.y), static_cast<int>(std::floor(p.z))) == block::Bricks,
              "mundo sin nombre: lo construido se conserva al volver");
    }
}

void testAmbience() {
    std::printf("Dimensiones: ambiente\n");
    ecs::World world;
    makeWorld(world, 99);
    ecs::Entity amb = world.create("Ambiente");
    amb.add<ecs::Sky>().clouds = true;
    amb.add<environment::Environment>().time_of_day = 9.0f;
    ecs::Entity sun = world.create("Sol");
    ecs::Light& light = sun.add<ecs::Light>();
    light.type = ecs::LightType::Directional;
    light.color = Vec3{1.0f, 1.0f, 1.0f};
    light.intensity = 5.0f;
    ecs::Entity sea = world.create("Mar");
    sea.add<water::WaterBody>() = water::voxelSeaPreset();
    VoxelSystem vox;
    vox.start(world);
    vox.setDimension("abismo");
    check(!sea.activeSelf() && !amb.get<ecs::Sky>().clouds && sun.get<ecs::Light>().intensity < 5.0f &&
              amb.get<environment::Environment>().time_of_day > 18.0f,
          "abismo: sin mar ni nubes, luz roja y baja");
    vox.setDimension("cristal");
    check(sea.activeSelf() && amb.get<environment::Environment>().time_of_day < 1.0f && sun.get<ecs::Light>().color.z > sun.get<ecs::Light>().color.x,
          "cristal: noche, luz azulada y lagos");
    vox.setDimension("superficie");
    check(sea.activeSelf() && amb.get<ecs::Sky>().clouds && sun.get<ecs::Light>().intensity == 5.0f &&
              amb.get<environment::Environment>().time_of_day == 9.0f,
          "superficie: vuelve todo como estaba");
    vox.setDimension("cielo", false);
    check(sea.activeSelf() && sun.get<ecs::Light>().intensity == 5.0f, "sin ambiente: el motor no toca la escena");
}

}  // namespace

int runDimensionTests() {
    testBlocks();
    testGenerators();
    testSwitching();
    testAmbience();
    return failures;
}
