// Pruebas de la fisica (consola, sin GPU): Jolt a traves de PhysicsSystem.
// Caidas y reposo, eventos de colision y de trigger (Enter/Stay/Exit),
// raycast/sphereCast/overlap con mascaras de capas, la matriz de capas,
// cinematicos, fuerzas, particulas que chocan y guardar/leer los componentes.
// Devuelve 0 si todo va.

#include "CramionCore/ecs/Components.h"
#include "CramionCore/ecs/SceneSerializer.h"
#include "CramionCore/ecs/World.h"
#include "CramionCore/physics/Particles.h"
#include "CramionCore/physics/PhysicsComponents.h"
#include "CramionCore/physics/PhysicsSystem.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>

using namespace cramion;
using namespace cramion::physics;
using core::Vec3;

namespace {

int failures = 0;
int checks = 0;

void check(bool condition, const char* what) {
    ++checks;
    std::printf("  %s %s\n", condition ? "OK   " : "FALLO", what);
    if (!condition) ++failures;
}

ecs::Entity makeFloor(ecs::World& world) {
    ecs::Entity floor = world.create("Suelo");
    floor.add<PlaneCollider>();
    return floor;
}

ecs::Entity makeBox(ecs::World& world, const char* name, const Vec3& position, BodyType type = BodyType::Dynamic) {
    ecs::Entity box = world.create(name);
    box.setWorldPosition(position);
    box.add<BoxCollider>();
    box.add<Rigidbody>().type = type;
    return box;
}

void run(PhysicsSystem& physics, ecs::World& world, float seconds, std::map<PhysicsEventType, int>* counts = nullptr) {
    const int frames = static_cast<int>(seconds * 60.0f);
    for (int i = 0; i < frames; ++i) {
        physics.update(world, 1.0f / 60.0f);
        if (counts != nullptr) {
            for (const PhysicsEvent& e : physics.events()) ++(*counts)[e.type];
        }
    }
}

void testFallAndRest() {
    std::printf("Caida, reposo y eventos de colision\n");
    ecs::World world;
    makeFloor(world);
    ecs::Entity box = makeBox(world, "Caja", Vec3{0.0f, 5.0f, 0.0f});
    PhysicsSystem physics;
    physics.start(world);

    std::map<PhysicsEventType, int> counts;
    int box_listener_hits = 0;
    physics.addEntityListener(box, [&](const PhysicsEvent& e) {
        if (e.a == box) ++box_listener_hits;
    });
    run(physics, world, 3.0f, &counts);
    const float y = box.worldPosition().y;
    check(std::abs(y - 0.5f) < 0.05f, "la caja cae y reposa sobre el plano (y ~ 0.5)");
    check(counts[PhysicsEventType::CollisionEnter] == 1, "un CollisionEnter");
    check(counts[PhysicsEventType::CollisionStay] > 0, "CollisionStay mientras se tocan");
    check(box_listener_hits > 0, "el oyente de la entidad recibe sus eventos con a = ella");
    check(physics.bodyCount() == 2, "dos cuerpos (suelo y caja)");

    // Quieta se duerme: sin Exit (como Unity) y sin Stay.
    counts.clear();
    run(physics, world, 3.0f, &counts);
    check(physics.isSleeping(box), "en reposo se duerme");
    check(counts[PhysicsEventType::CollisionExit] == 0, "dormirse no da CollisionExit");

    // Separar: impulso hacia arriba -> Exit.
    counts.clear();
    physics.setLinearVelocity(box, Vec3{0.0f, 8.0f, 0.0f});
    run(physics, world, 0.3f, &counts);
    check(counts[PhysicsEventType::CollisionExit] == 1, "CollisionExit al separarse");
    check(box.worldPosition().y > 1.0f, "setLinearVelocity la lanza hacia arriba");

    // Borrar la caja mientras toca: el cuerpo desaparece.
    run(physics, world, 2.0f);
    world.destroy(box);
    run(physics, world, 0.1f);
    check(physics.bodyCount() == 1, "al borrar la entidad se borra su cuerpo");
}

void testInterpolation() {
    std::printf("Interpolacion (dibujar a mas FPS que la fisica)\n");
    ecs::World world;
    ecs::Entity smooth = makeBox(world, "Suave", Vec3{0.0f, 20.0f, 0.0f});
    ecs::Entity raw = makeBox(world, "Sin interpolar", Vec3{5.0f, 20.0f, 0.0f});
    raw.get<Rigidbody>().interpolate = false;
    PhysicsSystem physics;
    physics.start(world);
    run(physics, world, 0.2f);
    int smooth_still = 0;
    int raw_still = 0;
    float smooth_y = smooth.worldPosition().y;
    float raw_y = raw.worldPosition().y;
    for (int i = 0; i < 60; ++i) {
        physics.update(world, 1.0f / 120.0f);  // 120 FPS, fisica a 60 Hz
        if (!(smooth.worldPosition().y < smooth_y)) ++smooth_still;
        if (!(raw.worldPosition().y < raw_y)) ++raw_still;
        smooth_y = smooth.worldPosition().y;
        raw_y = raw.worldPosition().y;
    }
    check(smooth_still == 0, "interpolado: se mueve en todos los frames");
    check(raw_still >= 25, "sin interpolar: un frame de cada dos se queda quieto (a saltos)");
    check(std::abs(smooth_y - raw_y) < 0.2f, "la interpolacion va como mucho un paso por detras");
}

void testTriggers() {
    std::printf("Triggers\n");
    ecs::World world;
    makeFloor(world);
    ecs::Entity zone = world.create("Zona");
    zone.setWorldPosition(Vec3{0.0f, 2.0f, 0.0f});
    BoxCollider& collider = zone.add<BoxCollider>();
    collider.size = Vec3{3.0f, 1.0f, 3.0f};
    collider.material.is_trigger = true;
    ecs::Entity ball = world.create("Bola");
    ball.setWorldPosition(Vec3{0.0f, 6.0f, 0.0f});
    ball.add<SphereCollider>().radius = 0.25f;
    ball.add<Rigidbody>();

    PhysicsSystem physics;
    physics.start(world);
    std::map<PhysicsEventType, int> counts;
    ecs::Entity trigger_a;
    ecs::Entity trigger_b;
    physics.addListener([&](const PhysicsEvent& e) {
        if (e.type == PhysicsEventType::TriggerEnter) {
            trigger_a = e.a;
            trigger_b = e.b;
        }
    });
    run(physics, world, 3.0f, &counts);
    check(counts[PhysicsEventType::TriggerEnter] == 1, "TriggerEnter al entrar en la zona");
    check(counts[PhysicsEventType::TriggerStay] > 0, "TriggerStay mientras cruza");
    check(counts[PhysicsEventType::TriggerExit] == 1, "TriggerExit al salir por abajo");
    check(trigger_a == zone && trigger_b == ball, "en los eventos de trigger, a = el trigger");
    check(ball.worldPosition().y < 0.5f, "el trigger no la frena (cae hasta el suelo)");

    // Collider solido + trigger en la misma entidad: no se detectan entre si.
    ecs::Entity mixed = makeBox(world, "Mixta", Vec3{5.0f, 3.0f, 0.0f});
    SphereCollider& aura = mixed.add<SphereCollider>();
    aura.radius = 1.5f;
    aura.material.is_trigger = true;
    int self_hits = 0;
    int aura_floor = 0;
    physics.addListener([&](const PhysicsEvent& e) {
        if (e.type != PhysicsEventType::TriggerEnter) return;
        if (e.a == mixed && e.b == mixed) ++self_hits;
        if (e.a == mixed && e.b.name() == "Suelo") ++aura_floor;
    });
    run(physics, world, 2.0f);
    check(self_hits == 0, "el trigger no se detecta con el solido de su entidad");
    check(aura_floor == 1, "el trigger de un cuerpo que se mueve detecta el suelo estatico");
    check(std::abs(mixed.worldPosition().y - 0.5f) < 0.05f, "la parte solida choca con el suelo");
}

void testLayersAndQueries() {
    std::printf("Capas, raycast y overlaps\n");
    ecs::World world;
    PhysicsSettings settings;
    settings.layer_names[8] = "Jugador";
    settings.layer_names[9] = "Fantasma";
    settings.setLayersCollide(9, 0, false);  // Fantasma atraviesa Default
    makeFloor(world);
    ecs::Entity ghost = makeBox(world, "Fantasma", Vec3{0.0f, 3.0f, 0.0f});
    ghost.get<ecs::EntityInfo>().layer = 9;
    ecs::Entity wall = world.create("Pared");
    wall.setWorldPosition(Vec3{0.0f, 1.0f, -5.0f});
    wall.add<BoxCollider>().size = Vec3{4.0f, 2.0f, 0.5f};
    wall.get<ecs::EntityInfo>().layer = 8;
    ecs::Entity sensor = world.create("Sensor");
    sensor.setWorldPosition(Vec3{0.0f, 1.0f, -2.0f});
    SphereCollider& sensor_shape = sensor.add<SphereCollider>();
    sensor_shape.radius = 0.5f;
    sensor_shape.material.is_trigger = true;

    PhysicsSystem physics;
    physics.setSettings(settings);
    check(settings.layerIndex("Jugador") == 8 && settings.mask({"Jugador", "Fantasma"}) == ((1u << 8) | (1u << 9)),
          "nombres de capas y mascaras");
    physics.start(world);
    run(physics, world, 1.5f);
    check(ghost.worldPosition().y < -2.0f, "la capa Fantasma atraviesa el suelo (matriz de capas)");

    RaycastHit hit;
    const Vec3 origin{0.0f, 1.0f, 5.0f};
    const Vec3 forward{0.0f, 0.0f, -1.0f};
    check(physics.raycast(origin, forward, 100.0f, hit) && hit.entity == sensor && hit.trigger,
          "raycast toca primero el trigger (queries_hit_triggers)");
    QueryFilter solid_only;
    solid_only.triggers = QueryTriggers::Ignore;
    check(physics.raycast(origin, forward, 100.0f, hit, solid_only) && hit.entity == wall,
          "raycast ignorando triggers toca la pared");
    check(std::abs(hit.distance - 9.75f) < 0.02f && std::abs(hit.normal.z - 1.0f) < 1e-3f,
          "distancia y normal del impacto");
    QueryFilter only_default;
    only_default.layer_mask = layerBit(0);
    only_default.triggers = QueryTriggers::Ignore;
    check(!physics.raycast(origin, forward, 100.0f, hit, only_default), "la mascara sin la capa de la pared no la toca");
    const auto all = physics.raycastAll(origin, forward, 100.0f);
    check(all.size() == 2 && all[0].entity == sensor && all[1].entity == wall, "raycastAll ordenado por distancia");
    check(physics.raycast(Vec3{0.0f, 5.0f, 0.0f}, Vec3{0.0f, -1.0f, 0.0f}, 10.0f, hit) &&
              std::abs(hit.point.y) < 1e-3f,
          "raycast hacia abajo toca el plano en y = 0");
    check(physics.sphereCast(origin, 0.3f, forward, 100.0f, hit, solid_only) && hit.entity == wall &&
              std::abs(hit.distance - 9.45f) < 0.05f,
          "sphereCast toca la pared a la distancia correcta");
    const auto near_wall = physics.overlapSphere(Vec3{0.0f, 1.0f, -4.0f}, 1.0f);
    check(near_wall.size() == 1 && near_wall[0] == wall, "overlapSphere encuentra la pared");
    const auto box_hits = physics.overlapBox(Vec3{0.0f, 1.0f, -3.5f}, Vec3{0.5f, 0.5f, 2.0f}, core::Quat{});
    check(box_hits.size() == 2, "overlapBox encuentra pared y sensor");
    QueryFilter ignore_wall;
    ignore_wall.ignore = wall;
    ignore_wall.triggers = QueryTriggers::Ignore;
    check(!physics.raycast(origin, forward, 100.0f, hit, ignore_wall), "QueryFilter::ignore descarta una entidad");

    // Cambiar la capa en marcha rehace el cuerpo.
    ghost.setWorldPosition(Vec3{0.0f, 3.0f, 0.0f});
    ghost.get<ecs::EntityInfo>().layer = 0;
    physics.setLinearVelocity(ghost, Vec3{});
    run(physics, world, 2.0f);
    check(std::abs(ghost.worldPosition().y - 0.5f) < 0.05f, "al volver a Default choca con el suelo");
}

void testKinematicAndForces() {
    std::printf("Cinematicos, fuerzas y restricciones\n");
    ecs::World world;
    makeFloor(world);
    ecs::Entity pusher = makeBox(world, "Empujador", Vec3{-3.0f, 0.5f, 0.0f}, BodyType::Kinematic);
    ecs::Entity target = makeBox(world, "Objetivo", Vec3{0.0f, 0.5f, 0.0f});
    ecs::Entity locked = makeBox(world, "Bloqueado", Vec3{6.0f, 3.0f, 0.0f});
    locked.get<Rigidbody>().lock_position_y = true;
    PhysicsSystem physics;
    physics.start(world);
    run(physics, world, 0.5f);
    std::map<PhysicsEventType, int> counts;
    for (int i = 0; i < 120; ++i) {
        pusher.setWorldPosition(pusher.worldPosition() + Vec3{0.05f, 0.0f, 0.0f});
        physics.update(world, 1.0f / 60.0f);
        for (const PhysicsEvent& e : physics.events()) ++counts[e.type];
    }
    check(target.worldPosition().x > 1.5f, "el cinematico empuja al dinamico");
    check(counts[PhysicsEventType::CollisionEnter] >= 1, "el empujon da CollisionEnter");
    check(std::abs(locked.worldPosition().y - 3.0f) < 1e-3f, "congelar posicion Y lo mantiene en el aire");

    ecs::Entity ball = world.create("Bola");
    ball.setWorldPosition(Vec3{0.0f, 0.5f, 10.0f});
    ball.add<SphereCollider>();
    ball.add<Rigidbody>().mass = 2.0f;
    run(physics, world, 0.2f);
    physics.addForce(ball, Vec3{0.0f, 0.0f, 3.0f}, ForceMode::VelocityChange);
    check(std::abs(physics.linearVelocity(ball).z - 3.0f) < 1e-3f, "VelocityChange = 3 m/s sin importar la masa");
    physics.addForce(ball, Vec3{0.0f, 0.0f, 4.0f}, ForceMode::Impulse);
    check(std::abs(physics.linearVelocity(ball).z - 5.0f) < 1e-3f, "Impulse / masa (2 kg) = +2 m/s");
    physics.sleep(ball);
    check(physics.isSleeping(ball), "dormir un cuerpo");
    physics.wakeUp(ball);
    check(!physics.isSleeping(ball), "despertarlo");
}

void testParticles() {
    std::printf("Particulas que chocan\n");
    ecs::World world;
    makeFloor(world);
    ecs::Entity emitter = world.create("Chispas");
    emitter.setWorldPosition(Vec3{0.0f, 2.0f, 0.0f});
    ParticleSystem& ps = emitter.add<ParticleSystem>();
    ps.rate = 200.0f;
    ps.speed_min = ps.speed_max = 1.0f;
    ps.lifetime_min = ps.lifetime_max = 4.0f;
    ps.bounce = 0.5f;
    ecs::Entity roof = world.create("Techo");  // una capa con la que no chocan
    roof.setWorldPosition(Vec3{0.0f, 1.0f, 0.0f});
    roof.add<BoxCollider>().size = Vec3{0.2f, 0.1f, 0.2f};
    roof.get<ecs::EntityInfo>().layer = 3;
    ps.collides_with = ~layerBit(3);

    PhysicsSystem physics;
    physics.start(world);
    ParticleWorld particles;
    int particle_events = 0;
    bool hit_floor = false;
    bool hit_roof = false;
    physics.addListener([&](const PhysicsEvent& e) {
        if (e.type != PhysicsEventType::ParticleCollision) return;
        ++particle_events;
        hit_floor = hit_floor || e.b.name() == "Suelo";
        hit_roof = hit_roof || e.b == roof;
    });
    float lowest = 100.0f;
    for (int i = 0; i < 180; ++i) {
        physics.update(world, 1.0f / 60.0f);
        particles.update(world, 1.0f / 60.0f, &physics);
        const gfx::ParticleDrawList list = particles.drawList(Vec3{0.0f, 2.0f, 10.0f});
        for (const auto& p : list.additive) lowest = std::min(lowest, p.position.y);
    }
    check(particles.particleCount() > 100, "el emisor emite");
    check(particle_events > 0 && hit_floor, "eventos ParticleCollision contra el suelo");
    check(!hit_roof, "la mascara de capas del modulo de colision se respeta");
    check(lowest > -0.01f, "ninguna particula atraviesa el suelo");
    check(particles.collisionCount() > 0, "contador de choques");
}

void testSerialization() {
    std::printf("Guardar y leer componentes de fisica\n");
    physics::registerPhysicsComponents();
    ecs::World world;
    ecs::Entity e = world.create("Objeto");
    Rigidbody& rb = e.add<Rigidbody>();
    rb.type = BodyType::Kinematic;
    rb.mass = 7.0f;
    CapsuleCollider& capsule = e.add<CapsuleCollider>();
    capsule.axis = CapsuleAxis::Z;
    capsule.material.is_trigger = true;
    capsule.material.bounciness = 0.75f;
    ParticleSystem& ps = e.add<ParticleSystem>();
    ps.collides_with = layerBit(31) | layerBit(0);
    const std::string json = ecs::serializeWorld(world);
    ecs::World copy;
    ecs::deserializeWorld(copy, json);
    const ecs::Entity c = copy.findByName("Objeto");
    check(c.valid() && c.has<Rigidbody>() && c.get<Rigidbody>().type == BodyType::Kinematic &&
              c.get<Rigidbody>().mass == 7.0f,
          "Rigidbody de ida y vuelta");
    check(c.valid() && c.has<CapsuleCollider>() && c.get<CapsuleCollider>().axis == CapsuleAxis::Z &&
              c.get<CapsuleCollider>().material.is_trigger && c.get<CapsuleCollider>().material.bounciness == 0.75f,
          "CapsuleCollider con su material");
    check(c.valid() && c.has<ParticleSystem>() && c.get<ParticleSystem>().collides_with == (layerBit(31) | layerBit(0)),
          "mascara de capas (con el bit 31) de ida y vuelta");

    PhysicsSettings settings;
    settings.layer_names[12] = "Enemigos";
    settings.setLayersCollide(12, 12, false);
    settings.gravity = Vec3{0.0f, -3.0f, 0.0f};
    const std::filesystem::path file = std::filesystem::temp_directory_path() / "cramion_physics_test.json";
    PhysicsSettings loaded;
    check(savePhysicsSettings(file, settings) && loadPhysicsSettings(file, loaded), "guardar y leer Physics.json");
    check(loaded.layer_names[12] == "Enemigos" && !loaded.layersCollide(12, 12) && loaded.layersCollide(12, 0) &&
              loaded.gravity.y == -3.0f,
          "capas, matriz y gravedad leidas");
    std::filesystem::remove(file);
}

}  // namespace

int main() {
    testFallAndRest();
    testInterpolation();
    testTriggers();
    testLayersAndQueries();
    testKinematicAndForces();
    testParticles();
    testSerialization();
    std::printf("\n%d comprobaciones, %d fallos\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
