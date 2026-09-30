// Pruebas del terreno (consola, sin GPU): herramientas de escultura y
// pintura, guardar/leer, consultas y colision con Jolt. Devuelve 0 si todo va.

#include "CramionCore/ecs/SceneSerializer.h"
#include "CramionCore/foliage/Foliage.h"
#include "CramionCore/ecs/World.h"
#include "CramionCore/physics/PhysicsComponents.h"
#include "CramionCore/physics/PhysicsSystem.h"
#include "CramionCore/terrain/Terrain.h"
#include "CramionCore/terrain/TerrainGenerator.h"
#include "CramionCore/terrain/TerrainTools.h"

#include <CramionFX/asset/HouseGenerator.h>
#include <CramionFX/asset/ImageFile.h>
#include <CramionFX/asset/TreeGenerator.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

using namespace cramion;
using namespace cramion::terrain;
using core::Vec3;

namespace {

int failures = 0;
int checks = 0;

void check(bool condition, const char* what) {
    ++checks;
    std::printf("  %s %s\n", condition ? "OK   " : "FALLO", what);
    if (!condition) ++failures;
}

bool finite(const TerrainData& data) {
    for (float h : data.heights()) {
        if (!std::isfinite(h) || h < 0.0f || h > 1.0f) return false;
    }
    return true;
}

float maxSlope(const TerrainData& data, const Terrain& t) {
    const int res = static_cast<int>(data.resolution());
    const float cell = t.size / static_cast<float>(res - 1);
    float worst = 0.0f;
    for (int y = 1; y < res; ++y) {
        for (int x = 1; x < res; ++x) {
            const float h = data.heights()[static_cast<std::size_t>(y) * res + x];
            worst = std::max(worst, std::abs(h - data.heights()[static_cast<std::size_t>(y) * res + x - 1]) * t.height / cell);
        }
    }
    return worst;
}

void testTools() {
    std::printf("Herramientas\n");
    Terrain t;
    t.size = 128.0f;
    t.height = 50.0f;
    TerrainData data;
    data.create(129, 128, 0.2f);
    const Vec3 origin{-64.0f, -10.0f, -64.0f};
    (void)data.takeDirtyHeights();

    TerrainBrush brush;
    brush.radius = 12.0f;
    brush.strength = 1.0f;
    const float before = heightAt(data, t, origin, 0.0f, 0.0f);
    for (int i = 0; i < 30; ++i) applyBrush(data, t, origin, Vec3{0, 0, 0}, brush, 1.0f / 30.0f);
    const float after = heightAt(data, t, origin, 0.0f, 0.0f);
    check(after > before + 5.0f, "esculpir sube el centro");
    check(std::abs(heightAt(data, t, origin, 40.0f, 40.0f) - before) < 1e-4f, "y no toca lo de fuera del pincel");
    const DirtyRegion region = data.takeDirtyHeights();
    check(region.valid() && region.x1 - region.x0 < 40, "solo se marca la zona cambiada");

    for (int i = 0; i < 10; ++i) applyBrush(data, t, origin, Vec3{0, 0, 0}, brush, 1.0f / 30.0f, /*invert=*/true);
    check(heightAt(data, t, origin, 0.0f, 0.0f) < after, "Mayus (invertir) baja");

    const float spike = maxSlope(data, t);
    brush.tool = TerrainTool::Smooth;
    for (int i = 0; i < 60; ++i) applyBrush(data, t, origin, Vec3{0, 0, 0}, brush, 1.0f / 30.0f);
    check(maxSlope(data, t) < spike, "suavizar reduce la pendiente");

    brush.tool = TerrainTool::Flatten;
    brush.target_height = 3.0f;
    for (int i = 0; i < 120; ++i) applyBrush(data, t, origin, Vec3{20, 0, 20}, brush, 1.0f / 30.0f);
    check(std::abs(heightAt(data, t, origin, 20.0f, 20.0f) - 3.0f) < 0.1f, "aplanar a 3 m");
    Vec3 hit{};
    const bool hits = raycast(data, t, origin, Vec3{20, 100, 20}, Vec3{0, -1, 0}, 500.0f, hit);
    check(hits && std::abs(hit.y - heightAt(data, t, origin, 20.0f, 20.0f)) < 0.02f, "raycast contra el terreno");

    brush.tool = TerrainTool::Erosion;
    for (int i = 0; i < 60; ++i) applyBrush(data, t, origin, Vec3{0, 0, 0}, brush, 1.0f / 30.0f);
    brush.tool = TerrainTool::Hydraulic;
    for (int i = 0; i < 30; ++i) applyBrush(data, t, origin, Vec3{0, 0, 0}, brush, 1.0f / 30.0f);
    brush.tool = TerrainTool::Noise;
    for (int i = 0; i < 10; ++i) applyBrush(data, t, origin, Vec3{-30, 0, -30}, brush, 1.0f / 30.0f);
    brush.tool = TerrainTool::Terrace;
    for (int i = 0; i < 10; ++i) applyBrush(data, t, origin, Vec3{-30, 0, 30}, brush, 1.0f / 30.0f);
    check(finite(data), "erosion, hidraulica, ruido y terrazas sin valores rotos");

    brush.ramp_width = 6.0f;
    applyRamp(data, t, origin, Vec3{-40, 0, 40}, Vec3{-10, 15, 40}, brush);
    const float ramp_mid = heightAt(data, t, origin, -25.0f, 40.0f);
    check(std::abs(ramp_mid - 7.5f) < 0.5f, "la rampa sube recta de 0 a 15 m");

    brush.tool = TerrainTool::Paint;
    brush.layer = 2;
    brush.radius = 8.0f;
    for (int i = 0; i < 60; ++i) applyBrush(data, t, origin, Vec3{0, 0, 0}, brush, 1.0f / 30.0f);
    const std::uint32_t cx = data.splatResolution() / 2;
    int sum = 0;
    for (int l = 0; l < kMaxLayers; ++l) sum += data.weight(l, cx, cx);
    check(data.weight(2, cx, cx) > 240 && sum == 255, "pintar la capa 2 (los pesos suman 255)");
    check(data.weight(2, 2, 2) == 0, "fuera del pincel no se pinta");
    for (int i = 0; i < 90; ++i) applyBrush(data, t, origin, Vec3{0, 0, 0}, brush, 1.0f / 30.0f, true);
    check(data.weight(2, cx, cx) < 20, "Mayus borra la capa");

    check(!raycast(data, t, origin, Vec3{500, 100, 500}, Vec3{0, -1, 0}, 500.0f, hit), "fuera del terreno no toca");

    generateRelief(data, 7, 4.0f, 0.5f, 0.4f, 0.1f);
    check(finite(data) && maxSlope(data, t) > 0.05f, "generar relieve");
}

void testSaveLoad() {
    std::printf("Guardar y leer\n");
    TerrainData data;
    data.create(65, 32, 0.3f);
    data.heights()[100] = 0.9f;
    *data.weightPtr(5, 3, 4) = 200;
    const std::filesystem::path file = std::filesystem::temp_directory_path() / "cramion_test.crterrain";
    check(data.save(file) && !data.unsaved(), "guardar .crterrain");
    TerrainData loaded;
    check(loaded.load(file) && loaded.resolution() == 65 && loaded.splatResolution() == 32 &&
              loaded.heights()[100] == 0.9f && loaded.weight(5, 3, 4) == 200,
          "leerlo igual");
    std::filesystem::remove(file);

    ecs::World world;
    registerTerrainComponents();
    ecs::Entity e = world.create("Terreno");
    Terrain& t = e.add<Terrain>();
    t.data = "Terrains/Prueba.crterrain";
    t.layers[1].albedo = "Textures/tierra.png";
    t.layers.push_back(TerrainLayer{});
    ecs::World copy;
    ecs::deserializeWorld(copy, ecs::serializeWorld(world));
    const ecs::Entity c = copy.findByName("Terreno");
    check(c.valid() && c.has<Terrain>() && c.get<Terrain>().layers.size() == 5 &&
              c.get<Terrain>().layers[1].albedo == "Textures/tierra.png" && c.get<Terrain>().data == t.data,
          "el componente (capas incluidas) en la escena");
}

void testPhysics() {
    std::printf("Colision (Jolt)\n");
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "cramion_terrain_assets";
    std::filesystem::remove_all(root);
    TerrainStore store;
    store.setRoot(root);
    ecs::World world;
    ecs::Entity e = world.create("Terreno");
    e.setWorldPosition(Vec3{-32.0f, 0.0f, -32.0f});
    Terrain& t = e.add<Terrain>();
    t.data = "Terrains/Fisica.crterrain";
    t.size = 64.0f;
    t.height = 20.0f;
    t.resolution = 65;
    std::shared_ptr<TerrainData> data = store.get(t);
    // Una loma de 10 m en el centro.
    TerrainBrush brush;
    brush.radius = 10.0f;
    brush.strength = 1.0f;
    brush.tool = TerrainTool::Flatten;
    brush.target_height = 10.0f;
    for (int i = 0; i < 200; ++i) applyBrush(*data, t, e.worldPosition(), Vec3{0, 0, 0}, brush, 1.0f / 30.0f);
    data->commitCollision();

    ecs::Entity box = world.create("Caja");
    box.setWorldPosition(Vec3{0.0f, 20.0f, 0.0f});
    box.add<physics::BoxCollider>();
    box.add<physics::Rigidbody>();

    physics::PhysicsSystem physics;
    physics.setTerrainProvider([&](ecs::Entity entity) -> std::shared_ptr<const TerrainData> {
        const Terrain* comp = entity.tryGet<Terrain>();
        return comp != nullptr ? store.get(*comp) : nullptr;
    });
    physics.start(world);
    for (int i = 0; i < 180; ++i) physics.update(world, 1.0f / 60.0f);
    check(std::abs(box.worldPosition().y - 10.5f) < 0.1f, "la caja cae y se queda sobre la loma (10 m)");
    physics::RaycastHit hit;
    check(physics.raycast(Vec3{20, 50, 20}, Vec3{0, -1, 0}, 100.0f, hit) && hit.entity == e && std::abs(hit.point.y) < 0.1f,
          "raycast de la fisica contra el terreno");

    // Esculpir y confirmar el trazo: el collider se rehace.
    brush.target_height = 2.0f;
    for (int i = 0; i < 200; ++i) applyBrush(*data, t, e.worldPosition(), Vec3{0, 0, 0}, brush, 1.0f / 30.0f);
    data->commitCollision();
    physics.wakeUp(box);
    for (int i = 0; i < 180; ++i) physics.update(world, 1.0f / 60.0f);
    check(std::abs(box.worldPosition().y - 2.5f) < 0.1f, "al bajar la loma, la caja baja con ella");

    check(store.saveAll() == 1 && std::filesystem::exists(root / "Terrains" / "Fisica.crterrain"),
          "el almacen guarda los terrenos con cambios");
    std::filesystem::remove_all(root);
}

}  // namespace

// Vegetacion: siembra en paralelo, repetible, sobre el terreno y con filtros.
void testFoliage() {
    std::printf("\nVegetacion\n");
    foliage::Foliage f;
    f.area = 1000.0f;
    f.density = 100.0f;  // celda de 10 m -> 100 x 100
    f.on_terrain = false;
    f.min_height = -1000.0f;
    foliage::FoliageResult flat = foliage::generateFoliage(f, Vec3{0, 2, 0}, {});
    check(flat.instances.size() == 10000 && std::abs(flat.cell - 10.0f) < 1e-3f, "100 arboles/ha en 1 km2 = 10.000");
    bool inside = true;
    for (const auto& i : flat.instances) inside = inside && std::abs(i.x) <= 500.0f && std::abs(i.z) <= 500.0f;
    check(inside, "todos dentro del cuadrado");
    foliage::FoliageResult again = foliage::generateFoliage(f, Vec3{0, 2, 0}, {});
    bool same = again.instances.size() == flat.instances.size();
    for (std::size_t i = 0; same && i < flat.instances.size(); ++i) {
        same = flat.instances[i].x == again.instances[i].x && flat.instances[i].packed == again.instances[i].packed;
    }
    check(same, "misma semilla, mismo bosque (aunque se siembre en varios hilos)");
    f.seed = 2;
    check(foliage::generateFoliage(f, Vec3{0, 2, 0}, {}).instances[0].x != flat.instances[0].x, "otra semilla, otro bosque");
    int species[3] = {0, 0, 0};
    for (const auto& i : flat.instances) ++species[(i.packed >> 18) & 3u];
    check(std::abs(species[0] / 10000.0f - 0.5f) < 0.03f && std::abs(species[1] / 10000.0f - 0.3f) < 0.03f,
          "la mezcla de especies (50% pinos, 30% robles, 20% abedules)");

    // Sobre un terreno con relieve: la altura del terreno, sin el mar ni las laderas empinadas.
    auto data = std::make_shared<TerrainData>();
    data->create(257, 64, 0.0f);
    generateRelief(*data, 11, 3.0f, 0.5f, 0.4f, 0.0f);
    Terrain t;
    t.size = 1000.0f;
    t.height = 120.0f;
    std::vector<foliage::FoliageGround> ground{{data, t, Vec3{-500.0f, -30.0f, -500.0f}}};
    f.on_terrain = true;
    f.min_height = 0.0f;  // el mar esta a 0
    f.max_slope = 30.0f;
    foliage::FoliageResult hills = foliage::generateFoliage(f, Vec3{0, 0, 0}, ground);
    bool on_ground = !hills.instances.empty();
    bool dry = true;
    for (std::size_t i = 0; i < hills.instances.size(); i += 97) {
        const auto& in = hills.instances[i];
        const float h = heightAt(*data, t, ground[0].origin, in.x, in.z);
        on_ground = on_ground && std::abs(in.y - h) < 0.7f;
        dry = dry && h >= 0.0f;
    }
    std::printf("    en el terreno: %zu de %llu (descartados %llu por el mar o la pendiente)\n", hills.instances.size(),
                static_cast<unsigned long long>(hills.candidates), static_cast<unsigned long long>(hills.rejected));
    check(on_ground, "apoyados en el terreno");
    check(dry && hills.rejected > 0, "ninguno bajo el mar ni en laderas empinadas");

    // Millones: 12,65 km x 12,65 km (16.000 ha) a 250/ha = 4 millones.
    foliage::Foliage big;
    big.area = 12650.0f;
    big.density = 250.0f;
    big.on_terrain = false;
    big.min_height = -1000.0f;
    const auto start = std::chrono::steady_clock::now();
    foliage::FoliageResult millions = foliage::generateFoliage(big, Vec3{}, {});
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    std::printf("    12,65 km x 12,65 km a 250/ha: %zu arboles en %.0f ms\n", millions.instances.size(), ms);
    check(millions.instances.size() >= 3990000, "4 millones de arboles");
    big.max_instances = 1000000;
    check(foliage::generateFoliage(big, Vec3{}, {}).instances.size() <= 1000000, "el tope de arboles se respeta (celdas mayores)");
}

void testGenerator() {
    std::printf("Generador de terreno\n");
    GenSettings s;
    s.seed = std::getenv("CRAMION_GEN_SEED") ? static_cast<std::uint32_t>(std::atoi(std::getenv("CRAMION_GEN_SEED"))) : 42;
    s.resolution = std::getenv("CRAMION_GEN_RES") ? std::atoi(std::getenv("CRAMION_GEN_RES")) : 513;
    s.splat_resolution = s.resolution - 1;
    s.size = 2048.0f;
    s.rivers = 3;
    GenResult a;
    const auto start = std::chrono::steady_clock::now();
    const bool done = generateTerrain(s, a);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    std::printf("    isla 513: %.0f ms, %zu rios, %zu lagos, erosion %.0f m3\n", ms, a.rivers.size(), a.lakes.size(),
                a.erosion_volume);
    check(done, "genera la isla");
    if (const char* probe = std::getenv("CRAMION_GEN_PROBE")) {
        // Depuracion: pesos en un punto del mundo "x,z" (terreno centrado).
        float px = 0.0f, pz = 0.0f;
        std::sscanf(probe, "%f,%f", &px, &pz);
        const std::uint32_t sr = a.data.splatResolution();
        const auto sx = static_cast<std::uint32_t>((px / s.size + 0.5f) * sr);
        const auto sy = static_cast<std::uint32_t>((pz / s.size + 0.5f) * sr);
        std::printf("    sonda (%.0f, %.0f) h=%.3f:", px, pz, a.data.sample(px / s.size + 0.5f, pz / s.size + 0.5f));
        for (int l = 0; l < 8; ++l) std::printf(" %d", a.data.weight(l, sx, sy));
        std::printf("\n");
    }
    check(finite(a.data), "alturas finitas en 0..1");
    // Hay mar alrededor y tierra en el centro.
    const float corner = a.data.sample(0.02f, 0.02f);
    const float center = a.data.sample(0.5f, 0.5f);
    check(corner < s.sea_level && center > s.sea_level, "mar en el borde y tierra en el centro");
    check(a.erosion_volume > 0.0f, "la erosion mueve tierra");
    check(!a.rivers.empty(), "al menos un rio");
    bool downhill = true;
    bool reaches_sea = true;
    for (const GenRiver& r : a.rivers) {
        for (std::size_t i = 1; i < r.points.size(); ++i) downhill = downhill && r.points[i].y <= r.points[i - 1].y + 1e-3f;
        reaches_sea = reaches_sea && r.points.back().y < 1.5f;
    }
    check(downhill, "el agua de los rios siempre baja");
    check(reaches_sea, "los rios llegan al mar (y ~ 0)");
    // Las capas suman 255 en cada texel.
    bool sums = true;
    for (std::uint32_t y = 0; y < a.data.splatResolution() && sums; y += 17) {
        for (std::uint32_t x = 0; x < a.data.splatResolution(); x += 13) {
            int total = 0;
            for (int l = 0; l < 8; ++l) total += a.data.weight(l, x, y);
            if (total != 255) sums = false;
        }
    }
    check(sums, "los pesos de las 8 capas suman 255");
    {
        // Cobertura de cada capa (en tierra): la hierba debe dominar.
        double cover[8] = {};
        double land = 0.0;
        const std::uint32_t sr = a.data.splatResolution();
        for (std::uint32_t y = 0; y < sr; y += 2) {
            for (std::uint32_t x = 0; x < sr; x += 2) {
                const float h = a.data.sample((x + 0.5f) / sr, (y + 0.5f) / sr);
                if (h <= s.sea_level) continue;
                land += 1.0;
                for (int l = 0; l < 8; ++l) cover[l] += a.data.weight(l, x, y) / 255.0;
            }
        }
        const char* names[8] = {"hierba", "seca", "tierra", "roca", "arena", "grava", "nieve", "barro"};
        std::printf("    capas en tierra:");
        for (int l = 0; l < 8; ++l) std::printf(" %s %.0f%%", names[l], 100.0 * cover[l] / std::max(land, 1.0));
        std::printf("\n");
        check(cover[0] + cover[1] > cover[5] + cover[3], "la hierba cubre mas que la grava y la roca");
    }
    // Misma semilla, mismo terreno.
    GenResult b;
    generateTerrain(s, b);
    check(a.data.heights() == b.data.heights(), "misma semilla = mismo terreno");
    // Las otras formas tambien salen.
    for (int shape = 1; shape < kGenShapeCount; ++shape) {
        GenSettings o = s;
        o.shape = static_cast<GenShape>(shape);
        o.resolution = 257;
        o.splat_resolution = 256;
        GenResult r;
        const bool ok = generateTerrain(o, r) && finite(r.data);
        char label[96];
        std::snprintf(label, sizeof(label), "forma %s", genShapeName(o.shape));
        check(ok, label);
    }
    // Cancelar.
    GenResult c;
    check(!generateTerrain(s, c, [](float t, const char*) { return t < 0.3f; }), "se puede cancelar");
    // Texturas de las capas.
    const std::filesystem::path folder = std::filesystem::temp_directory_path() / "cramion_gen_textures";
    check(writeGeneratorTextures(folder.string(), 128) &&
              std::filesystem::exists(folder / "Roca_Color.png") && std::filesystem::exists(folder / "Hierba_Normal.png"),
          "texturas de las 8 capas");
}

// CRAMION_DUMP_TEXTURES=carpeta: guarda las texturas procedurales de arboles
// y casas como PNG (para revisarlas a ojo).
void dumpTextures(const char* folder) {
    namespace fs = std::filesystem;
    fs::create_directories(folder);
    const cramion::asset::TreeTextures trees = cramion::asset::generateTreeTextures(512);
    for (std::size_t layer = 0; layer < trees.albedo.size(); ++layer) {
        cramion::asset::ImageRgba8 img;
        img.width = img.height = trees.size;
        img.pixels = trees.albedo[layer][0];
        cramion::asset::saveImagePng(fs::path(folder) / ("tree_" + std::to_string(layer) + ".png"), img);
        img.pixels = trees.normal[layer][0];
        cramion::asset::saveImagePng(fs::path(folder) / ("tree_" + std::to_string(layer) + "_n.png"), img);
    }
    for (int m = 0; m < cramion::asset::kHouseMaterialCount; ++m) {
        cramion::asset::HouseTextureSet set;
        if (!cramion::asset::generateHouseTexture(m, 512, 7, set)) continue;
        cramion::asset::saveImagePng(fs::path(folder) / (std::string("house_") + cramion::asset::houseTextureName(m) + ".png"), set.color);
    }
    std::printf("texturas en %s\n", folder);
}

int main() {
    if (const char* dump = std::getenv("CRAMION_DUMP_TEXTURES")) {
        dumpTextures(dump);
        return 0;
    }
    testGenerator();
    testFoliage();
    testTools();
    testSaveLoad();
    testPhysics();
    std::printf("\n%d comprobaciones, %d fallos\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
