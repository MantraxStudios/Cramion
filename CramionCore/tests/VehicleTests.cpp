// Pruebas de los vehiculos (Vehicle + WheelCollider con Jolt), en consola.
#include "CramionCore/ecs/World.h"
#include "CramionCore/physics/PhysicsComponents.h"
#include "CramionCore/physics/PhysicsSystem.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace cramion;
using core::Vec3;

int failures = 0;
void check(bool ok, const char* what) {
    std::printf("  %s %s\n", ok ? "OK   " : "FALLO", what);
    if (!ok) ++failures;
}

int main() {
    physics::registerPhysicsComponents();
    ecs::World world;
    ecs::Entity ground = world.create("Suelo");
    ground.add<physics::BoxCollider>().size = Vec3{400.0f, 1.0f, 400.0f};
    ground.setWorldPosition(Vec3{0.0f, -0.5f, 0.0f});

    ecs::Entity car = world.create("Coche");
    car.setWorldPosition(Vec3{0.0f, 1.2f, 0.0f});
    car.add<physics::BoxCollider>().size = Vec3{1.8f, 0.6f, 4.2f};
    physics::Rigidbody& rb = car.add<physics::Rigidbody>();
    rb.mass = 1200.0f;
    car.add<physics::Vehicle>().keyboard = false;
    ecs::Entity visual = world.create("RuedaVisible");
    const Vec3 offsets[4] = {{-0.95f, -0.3f, -1.35f}, {0.95f, -0.3f, -1.35f}, {-0.95f, -0.3f, 1.35f}, {0.95f, -0.3f, 1.35f}};
    for (int i = 0; i < 4; ++i) {
        ecs::Entity w = world.create("Rueda", car);
        w.setLocalPosition(offsets[i]);
        physics::WheelCollider& wc = w.add<physics::WheelCollider>();
        wc.max_steer_angle = i < 2 ? 30.0f : 0.0f;       // delanteras (el frente es -Z)
        wc.max_handbrake_torque = i < 2 ? 0.0f : 4000.0f;
        if (i == 0) wc.visual = visual.uuid();
    }

    physics::PhysicsSystem physics;
    physics.start(world);
    for (int i = 0; i < 120; ++i) physics.update(world, 1.0f / 60.0f);
    physics::PhysicsSystem::VehicleState state = physics.vehicleState(car);
    std::printf("  (apoyado: y = %.2f, ruedas en el suelo %d)\n", car.worldPosition().y, state.wheels_on_ground);
    check(state.valid && state.wheels_on_ground == 4, "el coche se apoya en sus 4 ruedas");
    check(car.worldPosition().y > 0.2f && car.worldPosition().y < 1.2f, "la suspension lo sostiene");

    const Vec3 start = car.worldPosition();
    physics.setVehicleInput(car, 1.0f, 0.0f, 0.0f, 0.0f);
    for (int i = 0; i < 240; ++i) physics.update(world, 1.0f / 60.0f);
    state = physics.vehicleState(car);
    std::printf("  (tras 4 s: z = %.1f, %.0f km/h, %.0f rpm, marcha %d)\n", car.worldPosition().z, state.speed_kmh, state.rpm, state.gear);
    check(car.worldPosition().z < start.z - 10.0f, "acelera hacia su frente (-Z)");
    check(state.speed_kmh > 20.0f && state.gear >= 1, "velocidad, rpm y marcha del motor");
    check(std::abs(visual.worldPosition().x - car.worldPosition().x) < 3.0f &&
              std::abs(visual.worldPosition().z - car.worldPosition().z) < 3.0f,
          "la rueda visible sigue al coche");

    const Vec3 heading_before = car.forward();
    const Vec3 right_before = car.right();
    physics.setVehicleInput(car, 0.6f, 1.0f, 0.0f, 0.0f);
    for (int i = 0; i < 120; ++i) physics.update(world, 1.0f / 60.0f);
    const Vec3 heading_after = car.forward();
    const float turned = std::acos(std::clamp(core::dot(heading_before, heading_after), -1.0f, 1.0f)) * 57.3f;
    std::printf("  (giro: %.0f grados)\n", turned);
    check(turned > 20.0f, "la direccion lo hace girar");
    check(core::dot(heading_after, right_before) > 0.0f, "direccion +1 gira a la derecha");

    physics.setVehicleInput(car, 0.0f, 0.0f, 1.0f, 0.0f);
    for (int i = 0; i < 240; ++i) physics.update(world, 1.0f / 60.0f);
    check(physics.vehicleState(car).speed_kmh < 3.0f, "el freno lo para");
    physics.stop();
    std::printf("\n%d fallos\n", failures);
    return failures == 0 ? 0 : 1;
}
