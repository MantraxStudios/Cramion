// Pruebas del agua (consola, sin GPU): oleaje Gerstner y consulta de altura,
// lago y rio, el componente en la escena y la flotacion con Jolt.
// Devuelve 0 si todo va.

#include "CramionCore/ecs/SceneSerializer.h"
#include "CramionCore/ecs/World.h"
#include "CramionCore/physics/PhysicsComponents.h"
#include "CramionCore/physics/PhysicsSystem.h"
#include "CramionCore/water/Water.h"

#include <cmath>
#include <cstdio>

using namespace cramion;
using namespace cramion::water;
using core::Vec3;

namespace {

int failures = 0;
int checks = 0;

void check(bool condition, const char* what) {
    ++checks;
    std::printf("  %s %s\n", condition ? "OK   " : "FALLO", what);
    if (!condition) ++failures;
}

void testWaves() {
    std::printf("Oleaje\n");
    WaterBody ocean = oceanPreset();
    const core::Mat4 at_origin = core::Mat4::identity();
    float lowest = 1e9f;
    float highest = -1e9f;
    for (int i = 0; i < 400; ++i) {
        const float x = static_cast<float>(i) * 0.73f;
        const WaterSample s = sampleWater(ocean, at_origin, Vec3{x, 0.0f, x * 0.4f}, 3.0f);
        lowest = std::min(lowest, s.height);
        highest = std::max(highest, s.height);
    }
    check(highest - lowest > ocean.wave_height * 0.5f && highest - lowest < ocean.wave_height * 2.5f,
          "la altura de las olas corresponde a wave_height");

    // La consulta encuentra el punto de la cuadricula que el oleaje lleva a
    // la posicion pedida: su desplazamiento es el mismo que dibuja el shader.
    const float gx = 12.3f;
    const float gz = -4.1f;
    const Vec3 d = gerstner(ocean, gx, gz, 5.0f);
    const WaterSample s = sampleWater(ocean, at_origin, Vec3{gx + d.x, 0.0f, gz + d.z}, 5.0f);
    check(std::abs(s.height - d.y) < 0.02f, "la altura consultada coincide con la malla desplazada");
    check(s.normal.y > 0.5f, "la normal apunta hacia arriba");

    // Espectro JONSWAP: 4 x desviacion tipica de la superficie = altura
    // significativa (la definicion de los oceanografos).
    {
        double sum = 0.0;
        double sum2 = 0.0;
        int count = 0;
        for (int i = 0; i < 120; ++i) {
            for (int j = 0; j < 120; ++j) {
                const float h = gerstner(ocean, static_cast<float>(i) * 1.37f, static_cast<float>(j) * 1.91f, 7.0f).y;
                sum += h;
                sum2 += static_cast<double>(h) * h;
                ++count;
            }
        }
        const double mean = sum / count;
        const double hs = 4.0 * std::sqrt(std::max(sum2 / count - mean * mean, 0.0));
        std::printf("  altura significativa medida %.2f m (pedida %.2f m)\n", hs, ocean.wave_height);
        check(std::abs(hs - ocean.wave_height) < ocean.wave_height * 0.25, "la altura significativa es wave_height");
    }

    WaterBody calm = ocean;
    calm.wave_height = 0.0f;
    check(std::abs(sampleWater(calm, at_origin, Vec3{3, 0, 3}, 1.0f).height) < 1e-5f, "sin olas, plano");
}

void testLakeAndRiver() {
    std::printf("Lago y rio\n");
    WaterBody lake = lakePreset();
    lake.size = core::Vec2{20.0f, 10.0f};
    const core::Mat4 world = core::translate(Vec3{100.0f, 5.0f, 0.0f});
    check(sampleWater(lake, world, Vec3{105.0f, 5.0f, 2.0f}, 0.0f).inside, "dentro del lago");
    check(!sampleWater(lake, world, Vec3{115.0f, 5.0f, 2.0f}, 0.0f).inside, "fuera del lago (x)");
    check(std::abs(sampleWater(lake, world, Vec3{100.0f, 0.0f, 0.0f}, 0.0f).height - 5.0f) < lake.wave_height,
          "altura del lago = la de la entidad");

    WaterBody river = riverPreset();
    river.points = {RiverPoint{Vec3{0, 0, 0}, 6.0f}, RiverPoint{Vec3{30, -2, 0}, 6.0f}, RiverPoint{Vec3{60, -4, 0}, 6.0f}};
    const core::Mat4 identity = core::Mat4::identity();
    const WaterSample mid = sampleWater(river, identity, Vec3{30.0f, 0.0f, 1.0f}, 0.0f);
    check(mid.inside && std::abs(mid.height + 2.0f) < 0.3f, "el rio sigue sus puntos (altura)");
    check(mid.velocity.x > river.flow_speed * 0.8f, "la corriente va en el sentido de los puntos");
    check(!sampleWater(river, identity, Vec3{30.0f, 0.0f, 8.0f}, 0.0f).inside, "fuera del cauce");
    const std::vector<RiverSample> line = riverCenterline(river, identity, 1.0f);
    check(line.size() > 50 && std::abs(line.back().distance - 60.0f) < 2.0f, "linea central subdividida");
}

void testScene() {
    std::printf("Componente\n");
    registerWaterComponents();
    ecs::World world;
    ecs::Entity e = world.create("Rio");
    WaterBody& body = e.add<WaterBody>();
    body = riverPreset();
    body.clarity = 2.5f;
    ecs::World copy;
    ecs::deserializeWorld(copy, ecs::serializeWorld(world));
    const ecs::Entity c = copy.findByName("Rio");
    check(c.valid() && c.has<WaterBody>() && c.get<WaterBody>().type == WaterType::River &&
              c.get<WaterBody>().points.size() == body.points.size() && c.get<WaterBody>().clarity == 2.5f,
          "el agua (con los puntos del rio) en la escena");
}

void testBuoyancy() {
    std::printf("Flotacion (Jolt)\n");
    ecs::World world;
    ecs::Entity lake_entity = world.create("Lago");
    WaterBody& lake = lake_entity.add<WaterBody>();
    lake = lakePreset();
    lake.wave_height = 0.0f;
    lake.size = core::Vec2{40.0f, 40.0f};

    ecs::Entity floating = world.create("Boya");
    floating.setWorldPosition(Vec3{0.0f, 3.0f, 0.0f});
    floating.add<physics::BoxCollider>();
    floating.add<physics::Rigidbody>();

    ecs::Entity away = world.create("Fuera");
    away.setWorldPosition(Vec3{60.0f, 3.0f, 0.0f});
    away.add<physics::BoxCollider>();
    away.add<physics::Rigidbody>();

    physics::PhysicsSystem physics;
    physics.start(world);
    for (int i = 0; i < 600; ++i) physics.update(world, 1.0f / 60.0f);
    const float y = floating.worldPosition().y;
    std::printf("  (boya en y = %.2f, fuera en y = %.2f)\n", y, away.worldPosition().y);
    check(y > -0.8f && y < 0.6f, "flota en la superficie (no se hunde ni sale volando)");
    check(away.worldPosition().y < -5.0f, "fuera del lago cae");

    // Un rio arrastra lo que flota.
    ecs::World river_world;
    ecs::Entity river_entity = river_world.create("Rio");
    WaterBody& river = river_entity.add<WaterBody>();
    river = riverPreset();
    river.wave_height = 0.0f;
    river.points = {RiverPoint{Vec3{-50, 0, 0}, 10.0f}, RiverPoint{Vec3{0, 0, 0}, 10.0f}, RiverPoint{Vec3{50, 0, 0}, 10.0f}};
    ecs::Entity log = river_world.create("Tronco");
    log.setWorldPosition(Vec3{-20.0f, 1.0f, 0.0f});
    log.add<physics::BoxCollider>();
    log.add<physics::Rigidbody>();
    physics::PhysicsSystem river_physics;
    river_physics.start(river_world);
    for (int i = 0; i < 300; ++i) river_physics.update(river_world, 1.0f / 60.0f);
    std::printf("  (tronco en x = %.2f)\n", log.worldPosition().x);
    check(log.worldPosition().x > -14.0f, "la corriente del rio lo arrastra");
}

}  // namespace

int main() {
    testWaves();
    testLakeAndRiver();
    testScene();
    testBuoyancy();
    std::printf("\n%d comprobaciones, %d fallos\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
