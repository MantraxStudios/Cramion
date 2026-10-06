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
#include <CramionFX/asset/MedievalBuildings.h>
#include <CramionFX/asset/SettlementGenerator.h>
#include <CramionFX/asset/ImageFile.h>
#include <CramionFX/asset/TreeGenerator.h>

#include <algorithm>
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
// Aplanar bajo una huella girada, altura de apoyo, parches y caminos.
void testFootprints() {
    std::printf("-- Huellas y caminos --\n");
    Terrain t;
    t.size = 128.0f;
    t.height = 40.0f;
    TerrainData data;
    data.create(129, 64, 0.0f);
    const Vec3 origin{0.0f, 0.0f, 0.0f};
    // Ladera: sube 1 m cada 4 m en X, con ondas.
    const auto res = static_cast<int>(data.resolution());
    for (int y = 0; y < res; ++y) {
        for (int x = 0; x < res; ++x) {
            const float wx = static_cast<float>(x);
            const float wz = static_cast<float>(y);
            data.heights()[static_cast<std::size_t>(y) * res + x] = (5.0f + wx * 0.25f + std::sin(wz * 0.3f) * 1.5f) / t.height;
        }
    }
    Footprint fp;
    fp.center = Vec3{60.0f, 0.0f, 50.0f};
    fp.half = core::Vec2{5.0f, 3.5f};
    fp.yaw_degrees = 30.0f;
    float lo = 0.0f;
    float hi = 0.0f;
    const float h = footprintHeight(data, t, origin, fp, &lo, &hi);
    check(h >= lo && h <= hi && hi - lo > 1.0f, "altura de apoyo: la mediana entre la mas baja y la mas alta");
    const TerrainData before = data;
    const HeightPatch patch = captureFootprint(data, t, origin, fp, 4.0f);
    check(patch.valid() && !patch.heights.empty(), "se guarda el trozo que se va a tocar");
    check(flattenFootprint(data, t, origin, fp, h, 4.0f), "aplanar bajo la huella");
    // Dentro (incluidas las esquinas giradas), plano a la altura pedida.
    bool flat = true;
    const float a = fp.yaw_degrees * core::kPi / 180.0f;
    for (int i = -10; i <= 10; ++i) {
        for (int j = -10; j <= 10; ++j) {
            const float lx = fp.half.x * static_cast<float>(i) / 10.0f;
            const float lz = fp.half.y * static_cast<float>(j) / 10.0f;
            const float x = fp.center.x + std::cos(a) * lx + std::sin(a) * lz;
            const float z = fp.center.z - std::sin(a) * lx + std::cos(a) * lz;
            flat = flat && std::abs(heightAt(data, t, origin, x, z) - h) < 1e-3f;
        }
    }
    check(flat, "toda la huella (girada 30 grados) queda plana, sin asomar entre vertices");
    const float far_before = heightAt(before, t, origin, fp.center.x + 20.0f, fp.center.z);
    check(std::abs(heightAt(data, t, origin, fp.center.x + 20.0f, fp.center.z) - far_before) < 1e-5f, "lejos no cambia nada");
    // El talud: entre medias, ni lo de antes ni lo plano del todo.
    const float edge_x = fp.center.x + 5.0f * std::cos(a) + 2.0f + 2.0f;
    const float mid = heightAt(data, t, origin, edge_x, fp.center.z - 5.0f * std::sin(a));
    const float orig = heightAt(before, t, origin, edge_x, fp.center.z - 5.0f * std::sin(a));
    check(std::abs(mid - orig) > 1e-3f || std::abs(mid - h) > 1e-3f, "talud suave alrededor");
    restorePatch(data, patch);
    check(data.heights() == before.heights(), "restaurar el trozo deja el terreno como estaba");
    // Pintar tierra bajo la huella.
    check(paintFootprint(data, t, origin, fp, 2, 1.0f, 1.0f), "pintar una capa bajo la huella");
    const std::uint32_t sx = static_cast<std::uint32_t>(fp.center.x / t.size * 64.0f);
    const std::uint32_t sz = static_cast<std::uint32_t>(fp.center.z / t.size * 64.0f);
    check(data.weight(2, sx, sz) > 240, "debajo queda la capa pintada (la hierba no crece)");
    // Camino: sigue la altura de sus puntos.
    const std::vector<Vec3> road = {Vec3{10.0f, 12.0f, 100.0f}, Vec3{60.0f, 14.0f, 100.0f}, Vec3{110.0f, 16.0f, 110.0f}};
    check(flattenPath(data, t, origin, road, 5.0f, 3.0f), "allanar un camino");
    check(std::abs(heightAt(data, t, origin, 35.0f, 100.0f) - 13.0f) < 0.05f, "el camino va a la altura de sus puntos (13 m a medio tramo)");
    check(std::abs(heightAt(data, t, origin, 35.0f, 101.5f) - 13.0f) < 0.05f, "y es plano a lo ancho");
    check(paintPath(data, t, origin, road, 5.0f, 5, 1.0f, 1.0f), "pintar el camino");
    check(finite(data), "alturas validas");
}

// Casas medievales con interior, edificios del pueblo, muralla y vallas.
void testBuildings() {
    std::printf("-- Casas y edificios medievales --\n");
    using namespace cramion::asset;
    bool all_ok = true;
    for (int st = 0; st < kHouseStyleCount; ++st) {
        HouseSettings s = housePreset(static_cast<HouseStyle>(st), 7);
        s.interior = false;
        const HouseModel bare = buildHouse(s);
        s.interior = true;
        const HouseModel full = buildHouse(s);
        const bool ok = bare.triangles > 500 && full.triangles > bare.triangles && !full.door.vertices.empty() &&
                        full.bounds_max.y > 3.0f && (!s.chimney || !full.lights.empty());
        if (!ok) std::printf("    estilo %s: %zu / %zu triangulos\n", houseStyleName(s.style), bare.triangles, full.triangles);
        all_ok = all_ok && ok;
    }
    check(all_ok, "los 6 estilos: con interior tienen mas geometria, puerta y fuego en el hogar");
    // Tamanos extremos de la ventana (4 x 3.5 m con 3 plantas, 16 x 12 m) y todos los usos.
    bool extremes = true;
    for (int st = 0; st < kHouseStyleCount; ++st) {
        for (const auto& [w, d] : {std::pair<float, float>{4.0f, 3.5f}, std::pair<float, float>{16.0f, 12.0f}}) {
            for (int u = 0; u < kHouseUseCount; ++u) {
                HouseSettings s = housePreset(static_cast<HouseStyle>(st), 5);
                s.width = w;
                s.depth = d;
                s.floors = 3;
                s.use = static_cast<HouseUse>(u);
                const HouseModel m = buildHouse(s);
                bool finite_bounds = std::isfinite(m.bounds_min.x) && std::isfinite(m.bounds_max.y) && m.bounds_max.x > m.bounds_min.x;
                for (const asset::SkinnedVertex& v : m.house.vertices) {
                    finite_bounds = finite_bounds && std::isfinite(v.position.x) && std::isfinite(v.position.y) && std::isfinite(v.position.z);
                }
                if (!finite_bounds || m.triangles < 500) {
                    std::printf("    %s %.0fx%.0f uso %d: %zu triangulos\n", houseStyleName(s.style), w, d, u, m.triangles);
                    extremes = false;
                }
            }
        }
    }
    check(extremes, "casas de 4 x 3.5 m y de 16 x 12 m con 3 plantas en todos los estilos y usos");
    HouseSettings t = housePreset(HouseStyle::HalfTimbered, 3);
    t.floors = 3;
    const HouseModel tall = buildHouse(t);
    check(tall.bounds_max.z > t.depth * 0.5f + t.jetty, "entramado: la planta alta vuela sobre la calle");
    bool uses = true;
    for (int u = 0; u < kHouseUseCount; ++u) {
        t.use = static_cast<HouseUse>(u);
        t.floors = 2;
        uses = uses && buildHouse(t).triangles > 2000;
    }
    check(uses, "vivienda, taberna, herreria y tienda");
    bool textures = true;
    for (int m = kHousePlaster; m <= kHouseBeams; ++m) {
        HouseTextureSet set;
        textures = textures && generateHouseTexture(m, 64, 7, set) && set.color.width == 64;
    }
    check(textures, "texturas nuevas: enlucido, paja, teja, tela y vigas");
    bool buildings = true;
    for (int b = 0; b < kMedievalBuildingCount; ++b) {
        MedievalSettings ms;
        ms.type = static_cast<MedievalBuilding>(b);
        const HouseModel m = buildMedieval(ms);
        const bool ok = m.triangles > 50 && m.bounds_max.x > m.bounds_min.x;
        if (!ok) std::printf("    %s: %zu triangulos\n", medievalBuildingName(ms.type), m.triangles);
        buildings = buildings && ok;
    }
    check(buildings, "iglesia, granero, pozo, puesto, torre, puerta, molino y objetos");
    MedievalSettings mill;
    mill.type = MedievalBuilding::Windmill;
    const HouseModel mm = buildMedieval(mill);
    check(mm.parts.size() == 1 && mm.parts[0].name == "Aspas" && !mm.parts[0].model.vertices.empty(), "el molino lleva las aspas aparte");
    MedievalSettings church;
    church.type = MedievalBuilding::Church;
    check(!buildMedieval(church).door.vertices.empty() && !buildMedieval(church).lights.empty(), "la iglesia tiene puerta y luz en el altar");
    CityWallSettings ws;
    for (int i = 0; i < 12; ++i) {
        const float ang = 2.0f * core::kPi * static_cast<float>(i) / 12.0f;
        ws.points.push_back(Vec3{std::cos(ang) * 60.0f, std::sin(ang * 2.0f) * 2.0f, std::sin(ang) * 60.0f});
        ws.towers.push_back(i % 3 != 1);
        ws.gaps.push_back(i == 4);
    }
    const HouseModel wall = buildCityWall(ws);
    check(wall.triangles > 1000 && wall.bounds_max.x > 58.0f && wall.bounds_min.x < -58.0f, "muralla con torres y un hueco de puerta");
    const HouseModel fences = buildFences({{Vec3{0, 0, 0}, Vec3{8, 0.5f, 0}}, {Vec3{8, 0.5f, 0}, Vec3{8, 1.0f, 6}}});
    check(fences.triangles > 50, "vallas");
}

// Trazado de aldea, pueblo y ciudad sobre un terreno con un lago.
void testSettlements() {
    std::printf("-- Pueblos medievales --\n");
    using namespace cramion::asset;
    SettlementTerrain T;
    T.height = [](float x, float z) { return 20.0f + std::sin(x * 0.01f) * 6.0f + std::cos(z * 0.013f) * 5.0f; };
    T.dry = [](float x, float z, float margin) {
        const float dx = x - 420.0f;
        const float dz = z - 300.0f;
        return std::sqrt(dx * dx + dz * dz) > 40.0f + margin;  // un lago
    };
    T.min_x = -600.0f;
    T.min_z = -600.0f;
    T.max_x = 600.0f;
    T.max_z = 600.0f;
    // Huellas de verdad (los modelos).
    std::vector<HouseModel> houses;
    for (int st : {4, 4, 2, 5, 5, 0}) houses.push_back(buildHouse(housePreset(static_cast<HouseStyle>(st), 3U + static_cast<std::uint32_t>(houses.size()))));
    std::vector<HouseModel> specials;
    for (int b = 0; b < kMedievalBuildingCount; ++b) {
        MedievalSettings ms;
        ms.type = static_cast<MedievalBuilding>(b);
        specials.push_back(buildMedieval(ms));
    }
    HouseSettings tv = housePreset(HouseStyle::HalfTimbered, 9);
    tv.use = HouseUse::Tavern;
    tv.width = 11.5f;
    const HouseModel tavern = buildHouse(tv);
    HouseSettings sm = housePreset(HouseStyle::StoneCottage, 9);
    sm.use = HouseUse::Smithy;
    const HouseModel smithy = buildHouse(sm);
    const auto fp = [&](const HouseModel& m, core::Vec2& half, core::Vec2& offset) {
        half = core::Vec2{(m.bounds_max.x - m.bounds_min.x) * 0.5f, (m.bounds_max.z - m.bounds_min.z) * 0.5f};
        offset = core::Vec2{(m.bounds_min.x + m.bounds_max.x) * 0.5f, (m.bounds_min.z + m.bounds_max.z) * 0.5f};
    };
    const SettlementFootprint footprint = [&](LotKind kind, int variant, core::Vec2& half, core::Vec2& offset) {
        switch (kind) {
            case LotKind::House: fp(houses[static_cast<std::size_t>(variant >= 0 ? variant : -1 - variant) % houses.size()], half, offset); break;
            case LotKind::Tavern: fp(tavern, half, offset); break;
            case LotKind::Smithy: fp(smithy, half, offset); break;
            case LotKind::Church: fp(specials[static_cast<std::size_t>(MedievalBuilding::Church)], half, offset); break;
            case LotKind::Barn: fp(specials[static_cast<std::size_t>(MedievalBuilding::Barn)], half, offset); break;
            case LotKind::Well: fp(specials[static_cast<std::size_t>(MedievalBuilding::Well)], half, offset); break;
            case LotKind::MarketStall: fp(specials[static_cast<std::size_t>(MedievalBuilding::MarketStall)], half, offset); break;
            case LotKind::Keep: fp(specials[static_cast<std::size_t>(MedievalBuilding::Keep)], half, offset); break;
            case LotKind::Gatehouse: fp(specials[static_cast<std::size_t>(MedievalBuilding::Gatehouse)], half, offset); break;
            case LotKind::Windmill: fp(specials[static_cast<std::size_t>(MedievalBuilding::Windmill)], half, offset); break;
            case LotKind::Prop: fp(specials[static_cast<std::size_t>(variant) % specials.size()], half, offset); break;
        }
    };
    for (int type = 0; type < kSettlementTypeCount; ++type) {
        SettlementSettings s;
        s.type = static_cast<SettlementType>(type);
        s.seed = 11;
        s.urban_variants = 3;
        s.rural_variants = 3;
        const auto t0 = std::chrono::steady_clock::now();
        const SettlementLayout L = layoutSettlement(s, T, footprint, Vec3{350.0f, 0.0f, 250.0f}, 200.0f);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        const int target = type == 0 ? 10 : (type == 1 ? 28 : 75);
        char label[200];
        std::snprintf(label, sizeof(label), "%s: %d casas, %zu lotes, %zu calles, %zu campos (%.0f ms)", settlementTypeName(s.type), L.houses,
                      L.lots.size(), L.roads.size(), L.fields.size(), ms);
        check(L.ok && L.houses >= target * 6 / 10, label);
        // Nada se pisa (salvo objetos y pacas de los campos).
        bool clean = true;
        for (std::size_t i = 0; i < L.lots.size() && clean; ++i) {
            if (L.lots[i].kind == LotKind::Prop) continue;
            const float ai = L.lots[i].yaw * core::kPi / 180.0f;
            for (std::size_t j = i + 1; j < L.lots.size() && clean; ++j) {
                if (L.lots[j].kind == LotKind::Prop) continue;
                // Separacion por circulos interiores (barato): los centros de las cajas
                // no pueden estar mas cerca que la menor de las medias medidas.
                const float aj = L.lots[j].yaw * core::kPi / 180.0f;
                const float cix = L.lots[i].position.x + std::cos(ai) * L.lots[i].offset.x + std::sin(ai) * L.lots[i].offset.y;
                const float ciz = L.lots[i].position.z - std::sin(ai) * L.lots[i].offset.x + std::cos(ai) * L.lots[i].offset.y;
                const float cjx = L.lots[j].position.x + std::cos(aj) * L.lots[j].offset.x + std::sin(aj) * L.lots[j].offset.y;
                const float cjz = L.lots[j].position.z - std::sin(aj) * L.lots[j].offset.x + std::cos(aj) * L.lots[j].offset.y;
                const float d = std::hypot(cix - cjx, ciz - cjz);
                const float ri = std::min(L.lots[i].half.x, L.lots[i].half.y);
                const float rj = std::min(L.lots[j].half.x, L.lots[j].half.y);
                if (d < ri + rj - 0.05f) clean = false;
            }
        }
        check(clean, "ningun edificio se mete en otro");
        bool dry = true;
        for (const SettlementLot& lot : L.lots) dry = dry && T.dry(lot.position.x, lot.position.z, 0.0f);
        check(dry, "nada en el lago");
        int church = 0;
        int gates = 0;
        int keep = 0;
        for (const SettlementLot& lot : L.lots) {
            church += lot.kind == LotKind::Church ? 1 : 0;
            gates += lot.kind == LotKind::Gatehouse ? 1 : 0;
            keep += lot.kind == LotKind::Keep ? 1 : 0;
        }
        if (type == 0) check(!L.fields.empty() && !L.fences.empty(), "la aldea tiene campos con vallas");
        if (type >= 1) check(church == 1, "una iglesia en la plaza");
        if (type == 2) {
            check(L.wall.size() >= 10 && gates >= 2 && gates == static_cast<int>(std::count(L.wall_gaps.begin(), L.wall_gaps.end(), true)),
                  "muralla con una puerta en cada camino que entra");
            check(keep == 1, "torre del homenaje");
        }
        const SettlementLayout again = layoutSettlement(s, T, footprint, Vec3{350.0f, 0.0f, 250.0f}, 200.0f);
        check(again.lots.size() == L.lots.size() && again.center.x == L.center.x, "misma semilla, mismo pueblo");
    }
}

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
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // si algo falla, que se vea hasta donde llego
    if (const char* dump = std::getenv("CRAMION_DUMP_TEXTURES")) {
        dumpTextures(dump);
        return 0;
    }
    testGenerator();
    testFoliage();
    testTools();
    testSaveLoad();
    testPhysics();
    testFootprints();
    testBuildings();
    testSettlements();
    std::printf("\n%d comprobaciones, %d fallos\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
