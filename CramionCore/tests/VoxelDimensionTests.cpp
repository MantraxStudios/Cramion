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

#include <CramionFX/asset/ImageFile.h>
#include <CramionFX/vk/VoxelPass.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

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
    bool labels = dims.size() == 12;  // + "profundo" (testDeepRealm) y "profundo2".."profundo8" (testDeepLayers)
    for (const char* name : {"superficie", "abismo", "cielo", "cristal"}) {
        bool found = false;
        for (const DimensionInfo& d : dims) found = found || (d.name == name && !d.label.empty());
        labels = labels && found;
    }
    check(labels && dims[1].label == "The Cinderdeep" && dims[2].label == "The Driftreach" && dims[3].label == "Glimmerwood", "las dimensiones con su etiqueta en ingles");
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

// --- Reino Profundo ("profundo", GeneratorDeep.cpp) y las simas abisales de la superficie ---

const StructurePoint* pointOf(const StructureInfo& s, const char* kind) {
    for (const StructurePoint& p : s.points)
        if (p.kind == kind) return &p;
    return nullptr;
}
int countOf(const StructureInfo& s, const char* kind) {
    int n = 0;
    for (const StructurePoint& p : s.points) n += p.kind == kind ? 1 : 0;
    return n;
}
BlockId blockAt(const Generator& g, int x, int y, int z) {
    ChunkData c;
    c.cx = static_cast<int>(std::floor(static_cast<float>(x) / kChunkSize));
    c.cz = static_cast<int>(std::floor(static_cast<float>(z) / kChunkSize));
    g.generate(c);
    return c.get(x - c.cx * kChunkSize, y, z - c.cz * kChunkSize);
}

void testDeepRealm() {
    std::printf("Reino Profundo\n");
    // Bloques 231..237.
    bool names = true;
    for (BlockId id = block::UmbralStone; id <= block::Duskroot; ++id) {
        const BlockDef& d = blockDef(id);
        names = names && !d.name.empty() && d.name.rfind("unused_", 0) != 0 && blockId(d.name) == id;
    }
    check(block::Count >= 238 && blocks().size() == block::Count && names, "bloques 231..237 con nombre propio");
    check(blockDef(block::GlyphStone).light > 8 && blockDef(block::Duskroot).light > 0 && blockDef(block::LoreTablet).label == "Ancient Tablet" &&
              blockDef(block::DelverBricks).hardness > 3.0f && blockDef(block::CrackedDelverBricks).hardness < 2.0f,
          "glifos y raices dan luz; tablilla y ladrillos con etiqueta en ingles");
    gfx::VoxelTextureLayer glyph;
    buildTexture("glyph_stone", 32, {}, glyph);
    int glow = 0;
    for (std::size_t i = 2; i < glyph.material.size(); i += 4) glow += glyph.material[i] > 100 ? 1 : 0;
    check(glow > 20, "textura de la piedra de glifos (emite)");
    const DimensionInfo* deep = nullptr;
    for (const DimensionInfo& d : dimensions())
        if (d.name == "profundo") deep = &d;
    check(deep != nullptr && deep->label == "The Sunless Deep", "dimension \"profundo\" (The Sunless Deep)");

    // Generador: contenido, determinista, presupuesto.
    {
        const Census c = census(VoxelGenerator::Deep, 777, -6, -6, 12);
        std::printf("    profundo: %.2f ms por chunk\n", c.ms_per_chunk);
        check(c.deterministic, "profundo determinista");
        check(c.count[block::UmbralStone] > 200000 && c.count[block::Water] > 1000, "profundo: roca umbria y el mar sin sol");
        const long long glow_blocks = c.count[block::GlowVines] + c.count[block::Duskroot] + c.count[block::GlowMushroom] +
                                      c.count[block::GlowMoss] + c.count[block::GlyphStone] + c.count[block::ShroomCapBlue] +
                                      c.count[block::ShroomCapViolet] + c.count[block::CrystalBlue] + c.count[block::Lava];
        check(glow_blocks > 2000, "profundo: lo que da luz (enredaderas, raices, hongos, glifos, cristales, lava)");
        check(c.count[block::MithrilOre] + c.count[block::DiamondOre] + c.count[block::GoldOre] > 200, "profundo: menas hondas (mitril...)");
        check(c.count[block::Air] > 150000, "profundo: cavernas grandes");
        check(c.ms_per_chunk < 12.0, "profundo: chunk dentro de presupuesto");
        std::printf("    (umbral %lld, agua %lld, luz %lld, mitril %lld, aire %lld)\n", c.count[block::UmbralStone], c.count[block::Water],
                    glow_blocks, c.count[block::MithrilOre], c.count[block::Air]);
    }
    const Generator deep_gen(777, 62, VoxelGenerator::Deep);
    {
        // Techo cerrado (sin cielo) y el "suelo" del juego es el techo (enCueva).
        ChunkData c;
        c.cx = 3;
        c.cz = -2;
        deep_gen.generate(c);
        bool roof = true;
        for (int z = 0; z < kChunkSize; ++z)
            for (int x = 0; x < kChunkSize; ++x) roof = roof && blockDef(c.get(x, kWorldHeight - 1, z)).solid && blockDef(c.get(x, kWorldHeight - 2, z)).solid;
        check(roof && deep_gen.terrainHeight(50, 50) >= kWorldHeight - 6, "profundo: techo macizo arriba del todo (nada de cielo)");
    }

    // Simas de la superficie y su caida en el Reino Profundo.
    const Generator surface(777, 62, VoxelGenerator::Adventure);
    std::vector<Generator::Sinkhole> sinks;
    for (int gz = -5; gz <= 5; ++gz)
        for (int gx = -5; gx <= 5; ++gx) {
            const Generator::Sinkhole s = surface.sinkhole(gx, gz);
            if (s.valid) sinks.push_back(s);
        }
    std::printf("    simas validas en 7 x 7 km: %zu\n", sinks.size());
    check(sinks.size() >= 3, "hay simas abisales (lejos del origen)");
    bool lejos = true;
    for (const auto& s : sinks) lejos = lejos && std::hypot(static_cast<float>(s.x), static_cast<float>(s.z)) > 1100.0f;
    check(lejos, "ninguna a menos de 1,1 km del origen");
    if (!sinks.empty()) {
        const Generator::Sinkhole& s = sinks.front();
        std::printf("    sima en %d %d (r %.1f, suelo %d)\n", s.x, s.z, static_cast<double>(s.r), s.top);
        check(blockAt(surface, s.x, 6, s.z) == block::Water && blockAt(surface, s.x, 3, s.z) == block::Obsidian &&
                  blockAt(surface, s.x, 30, s.z) == block::Air && blockAt(surface, s.x, s.top, s.z) == block::Air,
              "superficie: el pozo abierto hasta el estanque del fondo (y 4..10)");
        const auto list = surface.deepStructuresNear(static_cast<float>(s.x), 60.0f, static_cast<float>(s.z), 30.0f);
        const StructureInfo* sima = nullptr;
        for (const StructureInfo& i : list)
            if (i.type == "sima_abisal") sima = &i;
        check(sima != nullptr && pointOf(*sima, "portal") != nullptr && pointOf(*sima, "portal")->tag == "profundo" &&
                  pointOf(*sima, "inscripcion") != nullptr,
              "superficie: estructura sima_abisal con el portal al profundo y la tablilla");
        check(deep_gen.sinkhole(static_cast<int>(std::floor(static_cast<float>(s.x) / Generator::kSinkCell)),
                                static_cast<int>(std::floor(static_cast<float>(s.z) / Generator::kSinkCell)))
                      .x == s.x,
              "el Reino Profundo ve las mismas simas");
        check(blockAt(deep_gen, s.x, Generator::kDeepArrival, s.z) == block::Air && blockAt(deep_gen, s.x, 120, s.z) == block::Air &&
                  blockAt(deep_gen, s.x, 30, s.z) == block::Water && blockDef(blockAt(deep_gen, s.x, kWorldHeight - 1, s.z)).solid,
              "profundo: pozo de la caida del techo al lago (agua honda abajo)");
        const auto down = deep_gen.deepStructuresNear(static_cast<float>(s.x), 60.0f, static_cast<float>(s.z), 60.0f);
        const StructureInfo* stone = nullptr;
        for (const StructureInfo& i : down)
            if (i.name == "Ascension Stone") stone = &i;
        check(stone != nullptr && pointOf(*stone, "llegada") != nullptr && pointOf(*stone, "portal") != nullptr &&
                  pointOf(*stone, "portal")->tag == "superficie",
              "profundo: Piedra de Ascenso (portal a la superficie) y punto de llegada");
        if (stone != nullptr) {
            const StructurePoint* p = pointOf(*stone, "portal");
            const int px = static_cast<int>(std::floor(p->position.x)), py = static_cast<int>(p->position.y), pz = static_cast<int>(std::floor(p->position.z));
            check(blockDef(blockAt(deep_gen, px, py - 1, pz)).solid && blockAt(deep_gen, px, py, pz) == block::Air &&
                      blockAt(deep_gen, px, py + 1, pz) == block::Air,
                  "profundo: el dais de la Piedra de Ascenso se pisa (sobre el lago)");
        }
    }

    // Estructuras del Reino Profundo.
    const auto all = deep_gen.deepStructuresNear(0.0f, 100.0f, 0.0f, 2000.0f);
    std::map<std::string, int> kinds;
    for (const StructureInfo& i : all) kinds[i.type]++;
    std::printf("    en 4 x 4 km:");
    for (const auto& [k, n] : kinds) std::printf(" %s %d", k.c_str(), n);
    std::printf("\n");
    check(kinds["aldea_oculta"] >= 2 && kinds["ermita"] >= 1 && kinds["salon_enano"] >= 1 && kinds["templo_runico"] >= 1 &&
              kinds["santuario_culto"] >= 1 && kinds["colonia_perdida"] >= 1 && kinds["obelisco"] >= 1,
          "aldeas ocultas, ermitas, salas, templos, santuarios, colonias y obeliscos");
    bool village_ok = false, hall_ok = false, temple_ok = false, hostile_ok = true, ids_ok = true;
    std::set<std::uint64_t> ids;
    const StructureInfo* temple = nullptr;
    for (const StructureInfo& i : all) {
        const bool id_ok = ids.insert(i.id).second && i.id < (std::uint64_t{1} << 52) && i.dimension == "profundo";
        if (!id_ok) std::printf("    id repetido o malo: %s %s %llu %.0f %.0f %zu\n", i.type.c_str(), i.name.c_str(), static_cast<unsigned long long>(i.id),
                                static_cast<double>(i.center.x), static_cast<double>(i.center.z), i.points.size());
        ids_ok = ids_ok && id_ok;
        if (i.type == "aldea_oculta" && !village_ok) {
            const StructurePoint* plaza = pointOf(i, "plaza");
            village_ok = countOf(i, "npc") == 5 && countOf(i, "mercader") == 2 && pointOf(i, "portal") != nullptr &&
                         pointOf(i, "portal")->tag == "superficie" && plaza != nullptr &&
                         blockAt(deep_gen, static_cast<int>(std::floor(plaza->position.x)), static_cast<int>(plaza->position.y) - 1,
                                 static_cast<int>(std::floor(plaza->position.z))) == block::Paving;
        }
        if (i.type == "salon_enano" && !hall_ok) {
            const StructurePoint* sec = pointOf(i, "secreto");
            hall_ok = sec != nullptr && countOf(i, "spawner") >= 3 && pointOf(i, "jefe") != nullptr &&
                      blockAt(deep_gen, static_cast<int>(std::floor(sec->position.x - sec->direction.x)), static_cast<int>(sec->position.y),
                              static_cast<int>(std::floor(sec->position.z - sec->direction.z))) == block::CrackedDelverBricks;
        }
        if (i.type == "templo_runico" && temple == nullptr) {
            temple = &i;
            temple_ok = countOf(i, "lampara_runa") == 4 && pointOf(i, "puerta_runica") != nullptr && pointOf(i, "reliquia") != nullptr;
        }
        if (i.type == "santuario_culto" || i.type == "colonia_perdida")
            hostile_ok = hostile_ok && pointOf(i, "jefe") != nullptr && countOf(i, "spawner") >= 4 && pointOf(i, "inscripcion") != nullptr;
    }
    check(ids_ok, "ids unicos (< 2^52) y dimension profundo");
    check(village_ok, "aldea oculta: 5 aldeanos, 2 mercaderes, piedra de vuelta y plaza empedrada");
    check(hall_ok, "sala de los Delvers: guardianes, jefe y muro agrietado de la sala secreta");
    check(temple_ok, "templo runico: 4 lamparas, puerta sellada y reliquia");
    check(hostile_ok, "santuarios y colonias: jefe, guarnicion y tablilla");

    // Con CRAMION_DEEP_DUMP=<carpeta>: cortes en PNG para mirarlos (seccion por una caida, planta de una aldea y un templo).
    if (const char* dir = std::getenv("CRAMION_DEEP_DUMP")) {
        const auto color = [](BlockId id, std::uint8_t* px) {
            std::uint8_t r = 12, g = 12, b = 22;
            const BlockDef& d = blockDef(id);
            if (id == block::Water) r = 30, g = 70, b = 160;
            else if (id == block::Lava) r = 255, g = 120, b = 20;
            else if (id == block::UmbralStone) r = 58, g = 60, b = 74;
            else if (id == block::DelverBricks) r = 140, g = 115, b = 60;
            else if (id == block::CrackedDelverBricks) r = 200, g = 60, b = 60;
            else if (id == block::Paving || id == block::DirtPath || id == block::Gravel) r = 150, g = 140, b = 120;
            else if (id == block::ShroomStem) r = 220, g = 210, b = 190;
            else if (id == block::RuneLever || id == block::ArcaniteTrace) r = 60, g = 255, b = 255;
            else if (id == block::SignalLamp) r = 255, g = 0, b = 255;
            else if (id == block::Bedrock) r = 20, g = 20, b = 20;
            else if (d.light > 0) r = 235, g = 225, b = 120;
            else if (d.solid) {
                const std::uint32_t h = static_cast<std::uint32_t>(id) * 2654435761u;
                r = static_cast<std::uint8_t>(70 + (h >> 8) % 90), g = static_cast<std::uint8_t>(70 + (h >> 16) % 90), b = static_cast<std::uint8_t>(70 + (h >> 24) % 90);
            } else if (id != block::Air) r = 40, g = 160, b = 60;
            px[0] = r, px[1] = g, px[2] = b, px[3] = 255;
        };
        std::map<std::pair<int, int>, ChunkData> cache;
        const auto at = [&](const Generator& g, int x, int y, int z) -> BlockId {
            const int cx = static_cast<int>(std::floor(static_cast<float>(x) / kChunkSize)), cz = static_cast<int>(std::floor(static_cast<float>(z) / kChunkSize));
            auto it = cache.find({cx, cz});
            if (it == cache.end()) {
                ChunkData c;
                c.cx = cx;
                c.cz = cz;
                g.generate(c);
                it = cache.emplace(std::make_pair(cx, cz), std::move(c)).first;
            }
            if (y < 0 || y >= kWorldHeight) return block::Air;
            return it->second.get(x - cx * kChunkSize, y, z - cz * kChunkSize);
        };
        const auto save = [&](const std::string& name, int w, int h, const std::function<BlockId(int, int)>& f, int scale) {
            asset::ImageRgba8 img;
            img.width = static_cast<std::uint32_t>(w * scale);
            img.height = static_cast<std::uint32_t>(h * scale);
            img.pixels.resize(static_cast<std::size_t>(img.width) * img.height * 4);
            for (int j = 0; j < h; ++j)
                for (int i = 0; i < w; ++i) {
                    std::uint8_t px[4];
                    color(f(i, j), px);
                    for (int sj = 0; sj < scale; ++sj)
                        for (int si = 0; si < scale; ++si)
                            std::copy(px, px + 4, &img.pixels[(static_cast<std::size_t>(j * scale + sj) * img.width + static_cast<std::size_t>(i * scale + si)) * 4]);
                }
            asset::saveImagePng(std::string(dir) + "/" + name + ".png", img);
        };
        if (!sinks.empty()) {
            const auto& s = sinks.front();
            save("deep_section", 512, 256, [&](int i, int j) { return at(deep_gen, s.x - 256 + i, 255 - j, s.z); }, 2);
            cache.clear();
            save("surface_section", 160, 200, [&](int i, int j) { return at(surface, s.x - 80 + i, 199 - j, s.z); }, 3);
            cache.clear();
        }
        for (const char* type : {"aldea_oculta", "templo_runico", "salon_enano", "santuario_culto"}) {
            for (const StructureInfo& i : all) {
                if (i.type != type) continue;
                const int cx = static_cast<int>(std::floor(i.center.x)), cy = static_cast<int>(i.center.y), cz = static_cast<int>(std::floor(i.center.z));
                // Planta: el primer bloque bajo y + 6 (de un poco por encima del suelo hacia abajo).
                save(std::string("plan_") + type, 64, 64, [&](int a, int b) {
                    for (int y = cy + 6; y >= cy - 2; --y) {
                        const BlockId id = at(deep_gen, cx - 32 + a, y, cz - 32 + b);
                        if (id != block::Air) return id;
                    }
                    return static_cast<BlockId>(block::Air);
                }, 6);
                save(std::string("floor_") + type, 64, 64, [&](int a, int b) { return at(deep_gen, cx - 32 + a, cy, cz - 32 + b); }, 6);
                save(std::string("side_") + type, 96, 40, [&](int a, int b) { return at(deep_gen, cx - 48 + a, cy + 26 - b, cz); }, 5);
                cache.clear();
                break;
            }
        }
    }

    // El acertijo del templo en el VoxelSystem: las palancas del patron encienden sus lamparas (el juego abre la puerta).
    if (temple != nullptr) {
        ecs::World world;
        makeWorld(world, 777);
        VoxelSystem vox;
        vox.start(world);
        check(vox.setDimension("profundo", false), "pasar al profundo");
        const StructurePoint* door = pointOf(*temple, "puerta_runica");
        vox.waitUntilReady(door->position, 2, 90.0f);
        const auto local = vox.structuresNear(door->position, 4.0f);
        bool found = false;
        for (const StructureInfo& i : local) found = found || i.id == temple->id;
        check(found, "VoxelSystem: structuresNear ve el templo en el profundo");
        bool wired = true;
        for (const StructurePoint& p : temple->points) {
            if (p.kind != "lampara_runa") continue;
            const bool on = p.tag.back() == '1';
            const int lx = static_cast<int>(std::floor(p.position.x + p.direction.x * 3.0f)), ly = static_cast<int>(p.position.y),
                      lz = static_cast<int>(std::floor(p.position.z + p.direction.z * 3.0f));
            wired = wired && vox.getBlock(lx, ly, lz) == block::RuneLever;
            if (on) vox.setBlock(lx, ly, lz, block::RuneLeverOn);
        }
        for (int i = 0; i < 5; ++i) vox.update(0.1f, door->position);
        bool solved = wired;
        for (const StructurePoint& p : temple->points) {
            if (p.kind != "lampara_runa") continue;
            const bool on = p.tag.back() == '1';
            const int x = static_cast<int>(std::floor(p.position.x)), y = static_cast<int>(p.position.y), z = static_cast<int>(std::floor(p.position.z));
            solved = solved && (vox.signalPower(x, y, z) > 0) == on;
        }
        const int dx = static_cast<int>(std::floor(door->position.x)), dy = static_cast<int>(door->position.y), dz = static_cast<int>(std::floor(door->position.z));
        check(solved && vox.getBlock(dx, dy, dz) == block::DelverBricks,
              "templo: palancas -> polvo de arcanita -> lamparas del patron encendidas (puerta aun sellada)");
    }
}

// --- Capas hondas del Reino Profundo ("profundo2".."profundo8") y los pozos entre capas ---

const StructureInfo* findOf(const std::vector<StructureInfo>& list, const char* type, const char* name = nullptr) {
    for (const StructureInfo& i : list)
        if (i.type == type && (name == nullptr || i.name == name)) return &i;
    return nullptr;
}
std::string layerName(int layer) { return layer <= 1 ? std::string("profundo") : "profundo" + std::to_string(layer); }
bool standable(const Generator& g, const Vec3& p) {
    const int x = static_cast<int>(std::floor(p.x)), y = static_cast<int>(std::floor(p.y + 0.01f)), z = static_cast<int>(std::floor(p.z));
    const BlockId a = blockAt(g, x, y, z), b = blockAt(g, x, y + 1, z);
    return blockDef(blockAt(g, x, y - 1, z)).solid && !blockDef(a).solid && !blockDef(b).solid && !block::isWet(a);
}

void testDeepLayers() {
    std::printf("Capas del Reino Profundo\n");
    // Bloques 238..240 y las ocho dimensiones.
    check(block::Count >= 241 && blockId("adamant_ore") == block::AdamantOre && blockId("hollow_stone") == block::HollowStone &&
              blockId("heart_root") == block::HeartRoot && blockDef(block::HeartRoot).light > 0 && blockDef(block::AdamantOre).label == "Adamant Ore",
          "bloques 238..240 (adamantita, roca del Hueco, raiz del Corazon) con nombre y etiqueta");
    gfx::VoxelTextureLayer heart;
    buildTexture("heart_root", 32, {}, heart);
    int glow = 0;
    for (std::size_t i = 2; i < heart.material.size(); i += 4) glow += heart.material[i] > 100 ? 1 : 0;
    check(glow > 20, "textura de la raiz del Corazon (emite)");
    static const char* const kLabels[9] = {"", "The Sunless Deep", "The Fungal Caverns", "The Drowned Halls", "The Crystal Depths",
                                           "The Delver Kingdom", "The Magma Roots", "The Hollow", "The Root Heart"};
    bool dims_ok = true;
    for (int l = 1; l <= Generator::kDeepLayers; ++l) {
        bool found = false;
        for (const DimensionInfo& d : dimensions()) found = found || (d.name == layerName(l) && d.label == kLabels[l]);
        dims_ok = dims_ok && found;
    }
    check(dims_ok, "las ocho capas (profundo, profundo2..profundo8) con su etiqueta");

    // Cada capa: determinista, su tema y dentro de presupuesto.
    double worst_ms = 0.0;
    for (int l = 2; l <= Generator::kDeepLayers; ++l) {
        Generator gen(777, 62, VoxelGenerator::Deep);
        gen.setDeepLayer(l);
        Generator again(777, 62, VoxelGenerator::Deep);
        again.setDeepLayer(l);
        std::array<long long, 256> n{};
        constexpr int kSide = 6;
        const auto t0 = std::chrono::steady_clock::now();
        for (int z = 0; z < kSide; ++z)
            for (int x = 0; x < kSide; ++x) {
                ChunkData c;
                c.cx = x - 3;
                c.cz = z - 3;
                gen.generate(c);
                for (const BlockId id : c.blocks) ++n[id];
            }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / (kSide * kSide);
        worst_ms = std::max(worst_ms, ms);
        bool same = true;
        for (int i = 0; i < 2; ++i) {
            ChunkData a, b;
            a.cx = b.cx = 5 + i * 7;
            a.cz = b.cz = -4 - i;
            again.generate(b);
            gen.generate(a);
            same = same && a.blocks == b.blocks;
        }
        // Distinta de la capa 1 en el mismo sitio.
        const Generator first(777, 62, VoxelGenerator::Deep);
        ChunkData a, b;
        a.cx = b.cx = 2;
        a.cz = b.cz = 2;
        first.generate(a);
        gen.generate(b);
        std::printf("    capa %d (%s): %.2f ms por chunk (agua %lld, lava %lld, hongo %lld, cristal %lld, hueco %lld, raiz %lld, mitril %lld, adamantita %lld)\n", l,
                    kLabels[l], ms, n[block::Water], n[block::Lava], n[block::ShroomCapBlue] + n[block::ShroomCapViolet] + n[block::ShroomStem],
                    n[block::CrystalBlue] + n[block::CrystalPink] + n[block::CrystalViolet] + n[block::CrystalGreen], n[block::HollowStone], n[block::HeartRoot],
                    n[block::MithrilOre], n[block::AdamantOre]);
        bool theme = true;
        switch (l) {
            case 2: theme = n[block::ShroomCapBlue] + n[block::ShroomCapViolet] > 3000 && n[block::GlowMoss] > 3000; break;
            case 3: theme = n[block::Water] > 200000; break;
            case 4: theme = n[block::CrystalBlue] + n[block::CrystalPink] + n[block::CrystalViolet] + n[block::CrystalGreen] > 2000; break;
            case 6: theme = n[block::Lava] > 20000 && n[block::Basalt] > 100000; break;
            case 7: theme = n[block::HollowStone] > 300000 && n[block::Water] < 200000; break;
            case 8: theme = n[block::HeartRoot] > 500 && n[block::Lava] > 2000; break;
            default: theme = n[block::UmbralStone] > 200000; break;
        }
        char what[160];
        std::snprintf(what, sizeof(what), "capa %d: determinista, distinta de la 1 y con su tema (%s)", l, kLabels[l]);
        check(same && a.blocks != b.blocks && theme, what);
        if (l >= 5) {
            std::snprintf(what, sizeof(what), "capa %d: adamantita y mitril", l);
            check(n[block::AdamantOre] > 0 && n[block::MithrilOre] > 50, what);
        }
    }
    std::printf("    peor capa: %.2f ms por chunk\n", worst_ms);
    check(worst_ms < 15.0, "capas: chunk dentro de presupuesto");

    // Pozos entre capas: el agujero de la capa k y el pozo de la k+1 en el mismo x, z; caer por uno y subir por el otro.
    for (int b = 1; b < Generator::kDeepLayers; ++b) {
        Generator upper(777, 62, VoxelGenerator::Deep), lower(777, 62, VoxelGenerator::Deep);
        upper.setDeepLayer(b);
        lower.setDeepLayer(b + 1);
        std::vector<Generator::Sinkhole> shafts;
        for (int gz = -4; gz <= 4; ++gz)
            for (int gx = -4; gx <= 4; ++gx) {
                const Generator::Sinkhole s = upper.layerShaft(b, gx, gz);
                if (s.valid) shafts.push_back(s);
            }
        // Cerca unos de otros: el pozo mas cercano a cualquier punto, a menos de ~400 m.
        bool near_ok = true;
        for (int i = 0; i < 9; ++i) {
            const float px = static_cast<float>(i % 3 - 1) * 700.0f, pz = static_cast<float>(i / 3 - 1) * 700.0f;
            float best = 1e9f;
            for (const auto& s : shafts) best = std::min(best, std::hypot(static_cast<float>(s.x) - px, static_cast<float>(s.z) - pz));
            near_ok = near_ok && best < 480.0f;
            if (best >= 480.0f) std::printf("    frontera %d: el pozo mas cercano a (%.0f, %.0f) esta a %.0f m\n", b, static_cast<double>(px), static_cast<double>(pz), static_cast<double>(best));
        }
        bool same_xz = !shafts.empty();
        bool hole_ok = true, well_ok = true, stair_ok = true, links_ok = true;
        const Generator::Sinkhole& s = shafts.empty() ? Generator::Sinkhole{} : shafts.front();
        if (!shafts.empty()) {
            const Generator::Sinkhole t = lower.layerShaft(b, static_cast<int>(std::floor(static_cast<float>(s.x - ((b & 1) ? 192 : 0)) / 384.0f)),
                                                           static_cast<int>(std::floor(static_cast<float>(s.z - ((b & 1) ? 192 : 0)) / 384.0f)));
            same_xz = t.valid && t.x == s.x && t.z == s.z;
            // Capa k: estanque abajo, abierto hasta la plataforma.
            hole_ok = blockAt(upper, s.x, 5, s.z) == block::Water && blockAt(upper, s.x, 0, s.z) == block::Bedrock &&
                      blockAt(upper, s.x, Generator::kShaftPool + 2, s.z) == block::Air && blockAt(upper, s.x, s.top - 2, s.z) == block::Air;
            const auto up = upper.deepStructuresNear(static_cast<float>(s.x), static_cast<float>(s.top), static_cast<float>(s.z), 20.0f);
            const StructureInfo* drop = findOf(up, "sima_honda");
            links_ok = drop != nullptr && pointOf(*drop, "portal") != nullptr && pointOf(*drop, "portal")->tag == layerName(b + 1) &&
                       pointOf(*drop, "llegada") != nullptr && standable(upper, pointOf(*drop, "llegada")->position) &&
                       pointOf(*drop, "inscripcion") != nullptr;
            // Capa k+1: pozo abierto arriba (llegada), lago abajo, Piedra de Subida y la escalera hasta el rellano.
            const int lake = std::max(b + 1 == 3 ? 70 : (b + 1 == 6 ? 50 : 0), Generator::kDeepSea);
            well_ok = blockAt(lower, s.x, Generator::kDeepArrival, s.z) == block::Air && blockAt(lower, s.x, 150, s.z) == block::Air &&
                      blockAt(lower, s.x, lake - 4, s.z) == block::Water && blockDef(blockAt(lower, s.x, kWorldHeight - 1, s.z)).solid;
            const auto down = lower.deepStructuresNear(static_cast<float>(s.x), static_cast<float>(lake), static_cast<float>(s.z), 40.0f);
            const StructureInfo* stone = findOf(down, "portal", "Rising Stone");
            const StructurePoint* stair_top = nullptr;
            const StructurePoint* stone_portal = nullptr;
            if (stone != nullptr)
                for (const StructurePoint& p : stone->points)
                    if (p.kind == "portal") (p.position.y > 200.0f ? stair_top : stone_portal) = &p;
            links_ok = links_ok && stone != nullptr && stone_portal != nullptr && stone_portal->tag == layerName(b) && stair_top != nullptr &&
                       stair_top->tag == layerName(b) && pointOf(*stone, "llegada") != nullptr && pointOf(*stone, "llegada")->tag == "abajo";
            stair_ok = stair_top != nullptr && standable(lower, stair_top->position) && stone_portal != nullptr && standable(lower, stone_portal->position);
            if (!stair_ok && stair_top != nullptr && stone_portal != nullptr)
                std::printf("    rellano %d (%.1f %.1f %.1f) piedra %d (%.1f %.1f %.1f)\n", standable(lower, stair_top->position), static_cast<double>(stair_top->position.x),
                            static_cast<double>(stair_top->position.y), static_cast<double>(stair_top->position.z), standable(lower, stone_portal->position),
                            static_cast<double>(stone_portal->position.x), static_cast<double>(stone_portal->position.y), static_cast<double>(stone_portal->position.z));
            if (!stair_ok && stair_top != nullptr) {
                const int lx = static_cast<int>(std::floor(stair_top->position.x)), lz = static_cast<int>(std::floor(stair_top->position.z));
                for (int y = 238; y <= 250; ++y) std::printf(" %d", static_cast<int>(blockAt(lower, lx, y, lz)));
                std::printf(" <- columna del rellano\n");
            }
            // Un escalon a media altura: a un cuarto de vuelta, dos vueltas arriba.
            const float a = s.phase + 0.25f * 2.0f * 3.14159265f;
            const int sx = static_cast<int>(std::floor(static_cast<float>(s.x) + 0.5f + std::cos(a) * (s.r + 2.0f)));
            const int sz = static_cast<int>(std::floor(static_cast<float>(s.z) + 0.5f + std::sin(a) * (s.r + 2.0f)));
            const int ys = lake + 1 + static_cast<int>(2.25f * 24.0f);
            bool step = false;
            for (int y = ys - 2; y <= ys + 1 && !step; ++y)
                step = (blockAt(lower, sx, y - 1, sz) == block::DelverBricks || blockAt(lower, sx, y - 1, sz) == block::GlyphStone) &&
                       blockAt(lower, sx, y, sz) == block::Air && blockAt(lower, sx, y + 1, sz) == block::Air;
            stair_ok = stair_ok && step;
            if (!step) {
                std::printf("    escalon esperado en %d %d y~%d (lago %d):", sx, sz, ys, lake);
                for (int y = ys - 6; y <= ys + 4; ++y) std::printf(" %d", static_cast<int>(blockAt(lower, sx, y, sz)));
                std::printf("\n");
            }
        }
        if (!(near_ok && same_xz && hole_ok && well_ok && links_ok && stair_ok))
            std::printf("    frontera %d: cerca %d mismo %d agujero %d pozo %d enlaces %d escalera %d (pozo en %d %d r %.1f arriba %d)\n", b, near_ok, same_xz,
                        hole_ok, well_ok, links_ok, stair_ok, s.x, s.z, static_cast<double>(s.r), s.top);
        char what[200];
        std::snprintf(what, sizeof(what), "frontera %d -> %d: %zu pozos en 3,5 km, todos a menos de 480 m, mismo x, z arriba y abajo", b, b + 1, shafts.size());
        check(shafts.size() >= 6 && near_ok && same_xz, what);
        std::snprintf(what, sizeof(what), "frontera %d -> %d: agujero con estanque (capa %d) y pozo con lago (capa %d)", b, b + 1, b, b + 1);
        check(hole_ok && well_ok, what);
        std::snprintf(what, sizeof(what), "frontera %d -> %d: portales (bajar / subir), llegadas que se pisan y la Escalera de los Delvers", b, b + 1);
        check(links_ok && stair_ok, what);
    }

    // Ciudades (capas 3+), la capital (5) y la guarida del Corazon (8); ids unicos entre capas.
    std::set<std::uint64_t> ids;
    bool ids_ok = true, city_ok = false, capital_ok = false, ruin_ok = false, lair_ok = false, villages_ok = true, levels_ok = true;
    int cities = 0;
    for (int l = 1; l <= Generator::kDeepLayers; ++l) {
        Generator gen(777, 62, VoxelGenerator::Deep);
        gen.setDeepLayer(l);
        const auto all = gen.deepStructuresNear(0.0f, 100.0f, 0.0f, 1600.0f);
        std::map<std::string, int> kinds;
        for (const StructureInfo& i : all) {
            kinds[i.type]++;
            ids_ok = ids_ok && ids.insert(i.id).second && i.id < (std::uint64_t{1} << 52) && i.dimension == layerName(l);
            if (i.type == "ciudad_profunda") {
                ++cities;
                const int people = countOf(i, "npc") + countOf(i, "mercader");
                const StructurePoint* plaza = pointOf(i, "plaza");
                const bool ok = people >= 25 && countOf(i, "mercader") >= 6 && pointOf(i, "portal") != nullptr && pointOf(i, "portal")->tag == "superficie" &&
                                plaza != nullptr && standable(gen, plaza->position) && pointOf(i, "inscripcion") != nullptr;
                city_ok = city_ok || ok;
                if (i.name == "Orrun's Hold") capital_ok = ok && l == 5;
            }
            if (i.type == "ciudad_caida")
                ruin_ok = ruin_ok || (pointOf(i, "jefe") != nullptr && countOf(i, "spawner") >= 6 && countOf(i, "npc") == 0);
            if (i.type == "corazon_raiz") {
                const StructurePoint* boss = pointOf(i, "jefe");
                lair_ok = boss != nullptr && boss->tag == "dragon" && pointOf(i, "reliquia") != nullptr && pointOf(i, "inscripcion") != nullptr &&
                          pointOf(i, "inscripcion")->tag == "27" && countOf(i, "cofre") >= 3 && standable(gen, boss->position);
            }
            if (i.type == "salon_enano" || i.type == "santuario_culto") levels_ok = levels_ok && i.level >= 6 + 3 * (l - 1);
        }
        villages_ok = villages_ok && kinds["aldea_oculta"] + kinds["ermita"] >= 2;
        std::printf("    capa %d en 3,2 x 3,2 km:", l);
        for (const auto& [k, n] : kinds) std::printf(" %s %d", k.c_str(), n);
        std::printf("\n");
    }
    // Coste de los chunks de una ciudad (la ciudad entera se construye en cada chunk que toca).
    {
        Generator gen(777, 62, VoxelGenerator::Deep);
        gen.setDeepLayer(5);
        for (const StructureInfo& i : gen.deepStructuresNear(0.0f, 100.0f, 0.0f, 2500.0f)) {
            if (i.type != "ciudad_profunda") continue;
            const int ccx = static_cast<int>(std::floor(i.center.x / kChunkSize)), ccz = static_cast<int>(std::floor(i.center.z / kChunkSize));
            const auto t0 = std::chrono::steady_clock::now();
            for (int dz = -3; dz < 3; ++dz)
                for (int dx = -3; dx < 3; ++dx) {
                    ChunkData c;
                    c.cx = ccx + dx;
                    c.cz = ccz + dz;
                    gen.generate(c);
                }
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 36.0;
            std::printf("    ciudad %s: %.2f ms por chunk (6 x 6 chunks sobre ella)\n", i.name.c_str(), ms);
            check(ms < 15.0, "chunks de una ciudad dentro de presupuesto");
            break;
        }
    }
    check(ids_ok, "ids unicos entre las ocho capas (< 2^52) y la dimension de cada capa");
    check(villages_ok, "aldeas ocultas o ermitas en todas las capas");
    check(cities >= 4 && city_ok, "ciudades de los Delvers (capas 3+): guardias, mercaderes, plaza, piedra de vuelta y tablilla");
    check(capital_ok, "capa 5: Orrun's Hold, la capital (palacio)");
    check(ruin_ok, "ciudades caidas: guarnicion y jefe, sin gente");
    check(lair_ok, "capa 8: la guarida del Corazon de Raiz (jefe final, reliquia, tesoro y la tablilla del final)");
    check(levels_ok, "plazas hostiles mas duras cuanto mas honda la capa");

    // Con CRAMION_DEEP_DUMP=<carpeta>: planta de la capital (capa 5) y de una ciudad caida, y el corte de un pozo
    // (capa 4 arriba, capa 5 abajo, en el mismo x).
    if (const char* dir = std::getenv("CRAMION_DEEP_DUMP")) {
        const auto color = [](BlockId id, std::uint8_t* px) {
            std::uint8_t r = 12, g = 12, b = 22;
            const BlockDef& d = blockDef(id);
            if (id == block::Water || id == block::PoolWater) r = 30, g = 70, b = 160;
            else if (id == block::Lava) r = 255, g = 120, b = 20;
            else if (id == block::DelverBricks) r = 140, g = 115, b = 60;
            else if (id == block::Paving) r = 170, g = 165, b = 150;
            else if (id == block::GlyphStone) r = 90, g = 255, b = 200;
            else if (id == block::RoofSlate) r = 70, g = 80, b = 110;
            else if (id == block::Bedrock) r = 20, g = 20, b = 20;
            else if (d.light > 0) r = 235, g = 225, b = 120;
            else if (d.solid) {
                const std::uint32_t h = static_cast<std::uint32_t>(id) * 2654435761u;
                r = static_cast<std::uint8_t>(60 + (h >> 8) % 80), g = static_cast<std::uint8_t>(60 + (h >> 16) % 80), b = static_cast<std::uint8_t>(60 + (h >> 24) % 80);
            } else if (id != block::Air) r = 40, g = 160, b = 60;
            px[0] = r, px[1] = g, px[2] = b, px[3] = 255;
        };
        const auto save = [&](const std::string& name, int w, int h, const std::function<BlockId(int, int)>& f, int scale) {
            asset::ImageRgba8 img;
            img.width = static_cast<std::uint32_t>(w * scale);
            img.height = static_cast<std::uint32_t>(h * scale);
            img.pixels.resize(static_cast<std::size_t>(img.width) * img.height * 4);
            for (int j = 0; j < h; ++j)
                for (int i = 0; i < w; ++i) {
                    std::uint8_t px[4];
                    color(f(i, j), px);
                    for (int sj = 0; sj < scale; ++sj)
                        for (int si = 0; si < scale; ++si)
                            std::copy(px, px + 4, &img.pixels[(static_cast<std::size_t>(j * scale + sj) * img.width + static_cast<std::size_t>(i * scale + si)) * 4]);
                }
            asset::saveImagePng(std::string(dir) + "/" + name + ".png", img);
        };
        for (const int l : {5, 6, 8}) {
            Generator gen(777, 62, VoxelGenerator::Deep);
            gen.setDeepLayer(l);
            std::map<std::pair<int, int>, ChunkData> cache;
            const auto at = [&](int x, int y, int z) -> BlockId {
                const int cx = static_cast<int>(std::floor(static_cast<float>(x) / kChunkSize)), cz = static_cast<int>(std::floor(static_cast<float>(z) / kChunkSize));
                auto it = cache.find({cx, cz});
                if (it == cache.end()) {
                    ChunkData c;
                    c.cx = cx;
                    c.cz = cz;
                    gen.generate(c);
                    it = cache.emplace(std::make_pair(cx, cz), std::move(c)).first;
                }
                if (y < 0 || y >= kWorldHeight) return block::Air;
                return it->second.get(x - cx * kChunkSize, y, z - cz * kChunkSize);
            };
            const char* type = l == 8 ? "corazon_raiz" : (l == 5 ? "ciudad_profunda" : "ciudad_caida");
            for (const StructureInfo& i : gen.deepStructuresNear(0.0f, 100.0f, 0.0f, 2500.0f)) {
                if (i.type != type) continue;
                const int cx = static_cast<int>(std::floor(i.center.x)), cy = static_cast<int>(i.center.y), cz = static_cast<int>(std::floor(i.center.z));
                save(std::string("plan_") + type, 100, 100, [&](int a, int b) {
                    for (int y = cy + 14; y >= cy - 2; --y) {
                        const BlockId id = at(cx - 50 + a, y, cz - 50 + b);
                        if (id != block::Air) return id;
                    }
                    return static_cast<BlockId>(block::Air);
                }, 4);
                save(std::string("side_") + type, 120, 50, [&](int a, int b) { return at(cx - 60 + a, cy + 34 - b, cz + 21); }, 4);
                break;
            }
        }
        Generator up(777, 62, VoxelGenerator::Deep), down(777, 62, VoxelGenerator::Deep);
        up.setDeepLayer(4);
        down.setDeepLayer(5);
        for (int gz = 0; gz <= 3; ++gz) {
            const Generator::Sinkhole s = up.layerShaft(4, 0, gz);
            if (!s.valid) continue;
            save("shaft_section_4_5", 120, 512, [&](int a, int b) {
                const int x = s.x - 60 + a;
                return b < 256 ? blockAt(up, x, 255 - b, s.z) : blockAt(down, x, 511 - b, s.z);
            }, 2);
            break;
        }
    }

    // VoxelSystem: pasar a una capa honda (otra carpeta, su generador).
    {
        ecs::World world;
        makeWorld(world, 777);
        VoxelSystem vox;
        vox.start(world);
        check(vox.setDimension("profundo5", false) && vox.dimension() == "profundo5", "VoxelSystem: pasar a la capa 5");
        const Vec3 p = vox.spawnPoint(Vec3{0.0f, 0.0f, 0.0f});
        check(p.y > 1.0f && p.y < static_cast<float>(kWorldHeight - 4), "VoxelSystem: sitio para aparecer dentro de la capa");
        check(vox.setDimension("superficie", false), "y volver a la superficie");
        check(!vox.setDimension("profundo9", false), "no hay capa 9");
    }
}

}  // namespace

int runDimensionTests() {
    testBlocks();
    testGenerators();
    testSwitching();
    testAmbience();
    testDeepRealm();
    testDeepLayers();
    return failures;
}

int runDeepRealmTests() {
    testDeepRealm();
    testDeepLayers();
    return failures;
}
