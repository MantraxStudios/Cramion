// Pruebas del terreno (consola, sin GPU): herramientas de escultura y
// pintura, guardar/leer, consultas y colision con Jolt. Devuelve 0 si todo va.

#include "CramionCore/ecs/SceneSerializer.h"
#include "CramionCore/ecs/World.h"
#include "CramionCore/physics/PhysicsComponents.h"
#include "CramionCore/physics/PhysicsSystem.h"
#include "CramionCore/terrain/Terrain.h"
#include "CramionCore/terrain/TerrainTools.h"

#include <cmath>
#include <cstdio>
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

int main() {
    testTools();
    testSaveLoad();
    testPhysics();
    std::printf("\n%d comprobaciones, %d fallos\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
