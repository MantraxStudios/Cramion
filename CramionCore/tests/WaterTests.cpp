// Pruebas del agua (consola, sin GPU): oleaje Gerstner y consulta de altura,
// lago y rio, el componente en la escena y la flotacion con Jolt.
// Devuelve 0 si todo va.

#include "CramionCore/ecs/SceneSerializer.h"
#include "CramionCore/ecs/World.h"
#include "CramionCore/physics/PhysicsComponents.h"
#include "CramionCore/physics/PhysicsSystem.h"
#include "CramionCore/water/Ripples.h"
#include "CramionCore/water/Water.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

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
    // Lagos y rios: Gerstner.
    const float gx = 12.3f;
    const float gz = -4.1f;
    WaterBody waves = ocean;
    waves.type = WaterType::Lake;
    waves.size = core::Vec2{1000.0f, 1000.0f};
    const Vec3 d = gerstner(waves, gx, gz, 5.0f);
    const WaterSample s = sampleWater(waves, at_origin, Vec3{gx + d.x, 0.0f, gz + d.z}, 5.0f);
    check(std::abs(s.height - d.y) < 0.02f, "la altura consultada coincide con la malla desplazada");
    check(s.normal.y > 0.5f, "la normal apunta hacia arriba");
    // Oceano: FFT (el mismo espectro que water_fft.comp).
    {
        const Vec3 od = oceanDisplacement(ocean, gx, gz, 5.0f);
        const WaterSample os = sampleWater(ocean, at_origin, Vec3{gx + od.x, 0.0f, gz + od.z}, 5.0f);
        check(std::abs(os.height - od.y) < 0.05f, "oceano FFT: la altura consultada coincide con la malla");
        check(os.normal.y > 0.3f, "oceano FFT: la normal apunta hacia arriba");
        double sum = 0.0;
        double sum2 = 0.0;
        int count = 0;
        for (int i = 0; i < 96; ++i) {
            for (int j = 0; j < 96; ++j) {
                const float h = oceanDisplacement(ocean, static_cast<float>(i) * 2.9f, static_cast<float>(j) * 3.3f, 9.0f).y;
                sum += h;
                sum2 += static_cast<double>(h) * h;
                ++count;
            }
        }
        const double mean = sum / count;
        const double hs = 4.0 * std::sqrt(std::max(sum2 / count - mean * mean, 0.0));
        std::printf("  oceano FFT: altura significativa medida %.2f m (pedida %.2f m)\n", hs, ocean.wave_height);
        check(std::abs(hs - ocean.wave_height) < ocean.wave_height * 0.35, "oceano FFT: altura significativa");
    }

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

void testRipples() {
    std::printf("Olas interactivas\n");
    RippleSimulation sim;
    const Vec3 camera{0.0f, 5.0f, 0.0f};
    sim.update(camera, 0.0f, {});
    check(!sim.active(), "sin nada que las mueva, agua en calma");
    // Algo cae al agua en (0, 0) durante 0.1 s.
    const std::vector<RippleSource> splash = {RippleSource{Vec3{0.0f, 0.0f, 0.0f}, 0.5f, 4.0f}};
    for (int i = 0; i < 6; ++i) sim.update(camera, 1.0f / 60.0f, splash);
    check(sim.active() && sim.heightAt(0.0f, 0.0f) < -0.05f, "la salpicadura hunde el agua donde cae");
    // La onda se propaga: a 3 m, pasado un rato, el agua se mueve.
    float moved = 0.0f;
    for (int i = 0; i < 90; ++i) {
        sim.update(camera, 1.0f / 60.0f, {});
        moved = std::max(moved, std::abs(sim.heightAt(3.0f, 0.0f)));
    }
    check(moved > 0.003f, "la onda llega a 3 m");
    // Simetrica: igual a 3 m en x y en z.
    const float hx = sim.heightAt(2.0f, 0.0f);
    const float hz = sim.heightAt(0.0f, 2.0f);
    check(std::abs(hx - hz) < 0.002f + std::abs(hx) * 0.1f, "se propaga igual en todas direcciones");
    // La camara se mueve 1 m: las ondas se quedan quietas en el mundo.
    const float before = sim.heightAt(1.0f, 1.0f);
    RippleSimulation copy = sim;
    copy.update(Vec3{1.0f, 5.0f, 0.0f}, 0.0f, {});
    check(std::abs(copy.heightAt(1.0f, 1.0f) - before) < 1e-5f, "al moverse la rejilla, las olas no se mueven");
    // Se apagan solas.
    for (int i = 0; i < 60 * 30; ++i) sim.update(camera, 1.0f / 60.0f, {});
    check(!sim.active(), "pasado un rato vuelve la calma");
}

// Un muro quieto que atraviesa el agua: detras casi no llegan las ondas y
// delante rebotan (mas agua moviendose que sin el).
void testRippleObstacles() {
    std::printf("Olas contra obstaculos\n");
    const Vec3 camera{0.0f, 5.0f, 0.0f};
    const std::vector<RippleSource> splash = {RippleSource{Vec3{0.0f, 0.0f, 0.0f}, 0.5f, 4.0f}};
    RippleObstacle wall;
    wall.x = 2.0f;
    wall.half_u = 0.25f;  // eje x: grosor
    wall.half_v = 8.0f;   // a lo largo de z
    const auto run = [&](bool with_wall, float probe_x, int from_step = 20) {
        RippleSimulation sim;
        const std::vector<RippleObstacle> obstacles = with_wall ? std::vector<RippleObstacle>{wall}
                                                                : std::vector<RippleObstacle>{};
        float peak = 0.0f;
        for (int i = 0; i < 150; ++i) {
            sim.update(camera, 1.0f / 60.0f, i < 6 ? splash : std::vector<RippleSource>{}, obstacles);
            if (i > from_step) peak = std::max(peak, std::abs(sim.heightAt(probe_x, 0.0f)));
        }
        return peak;
    };
    const float behind_open = run(false, 4.0f);
    const float behind_wall = run(true, 4.0f);
    std::printf("  (detras: sin muro %.4f, con muro %.4f)\n", behind_open, behind_wall);
    check(behind_wall < behind_open * 0.3f, "el muro frena las ondas");
    // Delante: pasada la primera onda (~0.5 s), llega la que rebota (~1.4 s).
    const float front_open = run(false, 1.0f, 70);
    const float front_wall = run(true, 1.0f, 70);
    std::printf("  (delante, tras la primera onda: sin muro %.4f, con muro %.4f)\n", front_open, front_wall);
    check(front_wall > front_open * 1.25f, "delante del muro rebotan");
}

}  // namespace

// Oceano FFT: las olas avanzan con el viento y las crestas son afiladas
// (desplazamiento horizontal hacia ellas), como las de Gerstner.
void testOceanShape() {
    std::printf("Oceano FFT: forma y direccion\n");
    WaterBody ocean = oceanPreset();
    ocean.wind_direction = 0.0f;  // hacia +X
    ocean.wind_spread = 15.0f;
    // Direccion: el perfil a lo largo del viento un momento despues es el de
    // antes desplazado hacia +X.
    constexpr int kSamples = 1600;
    constexpr float kStep = 0.25f;
    std::vector<float> before(kSamples);
    std::vector<float> after(kSamples);
    for (int i = 0; i < kSamples; ++i) {
        before[i] = oceanDisplacement(ocean, static_cast<float>(i) * kStep, 3.0f, 10.0f).y;
        after[i] = oceanDisplacement(ocean, static_cast<float>(i) * kStep, 3.0f, 10.4f).y;
    }
    int best_shift = 0;
    double best_error = 1e30;
    for (int shift = -40; shift <= 40; ++shift) {
        double error = 0.0;
        for (int i = 60; i < kSamples - 60; ++i) {
            const double d = after[i] - before[i - shift];
            error += d * d;
        }
        if (error < best_error) {
            best_error = error;
            best_shift = shift;
        }
    }
    std::printf("  desplazamiento del perfil en 0.4 s: %.2f m\n", best_shift * kStep);
    check(best_shift > 0, "las olas avanzan a favor del viento");

    // Forma: el desplazamiento horizontal junta los puntos en las crestas
    // (afiladas) y los separa en los valles (anchos): dX/dx < 1 arriba y > 1
    // abajo. Con el signo al reves salia justo lo contrario.
    double crest = 0.0;
    double trough = 0.0;
    int crests = 0;
    int troughs = 0;
    for (int i = 0; i < 4000; ++i) {
        const float x = static_cast<float>(i) * 0.37f;
        const Vec3 d = oceanDisplacement(ocean, x, 5.0f, 9.0f);
        const Vec3 d0 = oceanDisplacement(ocean, x - 0.05f, 5.0f, 9.0f);
        const Vec3 d1 = oceanDisplacement(ocean, x + 0.05f, 5.0f, 9.0f);
        const double stretch = 1.0 + (d1.x - d0.x) / 0.1;
        if (d.y > ocean.wave_height * 0.45f) {
            crest += stretch;
            ++crests;
        } else if (d.y < -ocean.wave_height * 0.45f) {
            trough += stretch;
            ++troughs;
        }
    }
    crest /= std::max(crests, 1);
    trough /= std::max(troughs, 1);
    std::printf("  dX/dx en las crestas %.3f, en los valles %.3f\n", crest, trough);
    check(crests > 0 && troughs > 0 && crest < 1.0 && trough > 1.0, "crestas afiladas y valles anchos");
}

int main() {
    testWaves();
    testOceanShape();
    testLakeAndRiver();
    testScene();
    testBuoyancy();
    testRipples();
    testRippleObstacles();
    std::printf("\n%d comprobaciones, %d fallos\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
