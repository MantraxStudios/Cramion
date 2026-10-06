// Pruebas de la fisica (consola, sin GPU): Jolt a traves de PhysicsSystem.
// Caidas y reposo, eventos de colision y de trigger (Enter/Stay/Exit),
// raycast/sphereCast/overlap con mascaras de capas, la matriz de capas,
// cinematicos, fuerzas, particulas que chocan y guardar/leer los componentes.
// Devuelve 0 si todo va.

#include "CramionCore/ecs/Components.h"
#include "CramionCore/ecs/MathUtil.h"

#include <CramionFX/asset/Model.h>
#include "CramionCore/ecs/SceneSerializer.h"
#include "CramionCore/ecs/World.h"
#include "CramionCore/ecs/RuntimeMesh.h"
#include "CramionCore/physics/Cloth.h"
#include "CramionCore/physics/SoftBody.h"
#include "CramionCore/physics/Particles.h"
#include "CramionCore/physics/PhysicsComponents.h"
#include "CramionCore/physics/PhysicsSystem.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
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

void testLayerOverrides() {
    std::printf("Anular capas (incluir / excluir)\n");
    ecs::World world;
    PhysicsSettings settings;
    settings.layer_names[9] = "Fantasma";
    settings.setLayersCollide(9, 0, false);  // la matriz: Fantasma no choca con Default
    makeFloor(world);
    // Fantasma que incluye Default: choca con el suelo aunque la matriz diga que no.
    ecs::Entity included = makeBox(world, "Incluida", Vec3{0.0f, 3.0f, 0.0f});
    included.get<ecs::EntityInfo>().layer = 9;
    included.get<Rigidbody>().include_layers = layerBit(0);
    // Default que excluye Default: atraviesa el suelo aunque la matriz diga que si.
    ecs::Entity excluded = makeBox(world, "Excluida", Vec3{4.0f, 3.0f, 0.0f});
    excluded.get<Rigidbody>().exclude_layers = layerBit(0);
    // Excluir gana a incluir.
    ecs::Entity both = makeBox(world, "Ambas", Vec3{8.0f, 3.0f, 0.0f});
    both.get<Rigidbody>().include_layers = layerBit(0);
    both.get<BoxCollider>().material.exclude_layers = layerBit(0);
    // Un trigger que excluye la capa de la caja que lo cruza no la detecta.
    ecs::Entity zone = world.create("Zona");
    zone.setWorldPosition(Vec3{12.0f, 1.5f, 0.0f});
    BoxCollider& zone_box = zone.add<BoxCollider>();
    zone_box.size = Vec3{3.0f, 1.0f, 3.0f};
    zone_box.material.is_trigger = true;
    zone_box.material.exclude_layers = layerBit(0);
    makeBox(world, "Cruza", Vec3{12.0f, 4.0f, 0.0f});

    PhysicsSystem physics;
    physics.setSettings(settings);
    physics.start(world);
    std::map<PhysicsEventType, int> counts;
    run(physics, world, 2.0f, &counts);
    check(std::abs(included.worldPosition().y - 0.5f) < 0.05f, "Incluir: choca con una capa que la matriz no permite");
    check(excluded.worldPosition().y < -2.0f, "Excluir: atraviesa una capa que la matriz permite");
    check(both.worldPosition().y < -2.0f, "Excluir gana a Incluir");
    check(counts[PhysicsEventType::TriggerEnter] == 0, "un trigger que excluye la capa no la detecta");

    // Quitar la exclusion en marcha rehace el cuerpo y vuelve a chocar.
    excluded.get<Rigidbody>().exclude_layers = 0;
    excluded.setWorldPosition(Vec3{4.0f, 3.0f, 0.0f});
    physics.setLinearVelocity(excluded, Vec3{});
    run(physics, world, 2.0f);
    check(std::abs(excluded.worldPosition().y - 0.5f) < 0.05f, "sin la exclusion vuelve a chocar");
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

// Tela (soft body de Jolt): una sabana cae sobre una caja y se queda encima
// (no la atraviesa) con los bordes colgando; una cortina fijada sigue a su
// entidad; el viento empuja una bandera; resetCloth la devuelve a su sitio.
void testCloth() {
    std::printf("Tela\n");
    {
        ecs::World world;
        ecs::Entity box = world.create("Caja");
        box.add<BoxCollider>().size = Vec3{1.0f, 1.0f, 1.0f};  // arriba en y = 0.5
        ecs::Entity sheet = world.create("Sabana");
        Cloth& cloth = sheet.add<Cloth>();
        cloth.pin = ClothPin::None;
        cloth.width = cloth.height = 2.0f;
        cloth.segments_x = cloth.segments_y = 16;
        cloth.bending = 0.05f;
        sheet.setLocalEulerDegrees(Vec3{-90.0f, 0.0f, 0.0f});  // tumbada
        sheet.setLocalPosition(Vec3{0.0f, 1.5f, 0.0f});
        PhysicsSystem physics;
        physics.start(world);
        for (int i = 0; i < 180; ++i) physics.update(world, 1.0f / 60.0f);
        const auto& rt = sheet.get<Cloth>().runtime.ptr;
        check(rt && rt->simulated && rt->positions.size() == 17u * 17u, "la tela se simula (17x17 particulas)");
        if (rt && rt->positions.size() == 17u * 17u) {
            const Vec3 middle = rt->positions[8 * 17 + 8];
            const Vec3 corner = rt->positions[0];
            check(middle.y > 0.45f && middle.y < 0.7f, "el centro queda encima de la caja (no la atraviesa)");
            check(corner.y < 0.2f, "las esquinas cuelgan por los lados");
        }
        physics.stop();
    }
    {
        ecs::World world;
        ecs::Entity curtain = world.create("Cortina");
        Cloth& cloth = curtain.add<Cloth>();
        cloth.pin = ClothPin::TopEdge;
        curtain.setLocalPosition(Vec3{0.0f, 3.0f, 0.0f});
        PhysicsSystem physics;
        physics.start(world);
        for (int i = 0; i < 30; ++i) physics.update(world, 1.0f / 60.0f);
        // Se lleva 2 m a la derecha en un segundo y se deja quieta.
        for (int i = 1; i <= 60; ++i) {
            curtain.setLocalPosition(Vec3{2.0f * static_cast<float>(i) / 60.0f, 3.0f, 0.0f});
            physics.update(world, 1.0f / 60.0f);
        }
        for (int i = 0; i < 240; ++i) physics.update(world, 1.0f / 60.0f);
        const auto& rt = curtain.get<Cloth>().runtime.ptr;
        const int nx = cloth.particlesX();
        const Vec3 top_left = rt->positions[0];
        check(std::abs(top_left.x - (2.0f - 1.0f)) < 0.05f && std::abs(top_left.y - 4.0f) < 0.05f,
              "las particulas fijadas siguen a la entidad");
        const Vec3 bottom = rt->positions[static_cast<std::size_t>(cloth.particleCount() - nx / 2 - 1)];
        check(bottom.y < 2.2f && bottom.y > 1.5f, "la cortina cuelga sin estirarse de mas");

        // Viento: la bandera se va hacia +Z.
        cloth.wind = Vec3{0.0f, 0.0f, 10.0f};
        for (int i = 0; i < 120; ++i) physics.update(world, 1.0f / 60.0f);
        check(rt->positions[static_cast<std::size_t>(cloth.particleCount() - nx / 2 - 1)].z > 0.3f, "el viento la empuja");

        rt->reset_requested = true;
        cloth.wind = Vec3{};
        physics.update(world, 1.0f / 60.0f);
        physics.update(world, 1.0f / 60.0f);
        check(std::abs(rt->positions[static_cast<std::size_t>(cloth.particleCount() - 1)].z) < 0.1f, "resetCloth la devuelve a su sitio");
        physics.stop();
        check(!rt->simulated, "al parar, sin simulacion");
    }
    {
        // Render: cada vertice de la rejilla (skinning, un hueso por
        // particula) cae en su particula y la normal sigue a la tela, con la
        // entidad movida y girada.
        Cloth cloth;
        cloth.segments_x = cloth.segments_y = 4;
        const asset::ModelData model = clothModel(cloth);
        check(model.vertices.size() == 2u * 25u && model.bones.size() == 25u, "modelo: doble cara y un hueso por particula");
        ecs::World world;
        ecs::Entity e = world.create("Tela");
        e.setLocalPosition(Vec3{5.0f, 1.0f, -2.0f});
        e.setLocalEulerDegrees(Vec3{0.0f, 90.0f, 0.0f});
        const core::Mat4 entity_world = e.worldMatrix();
        // Tela doblada: la mitad de abajo hacia +X del mundo.
        std::vector<Vec3> positions = clothRestPositions(cloth);
        for (Vec3& p : positions) {
            p = ecs::transformPoint(entity_world, p);
            if (p.y < 1.0f) p.x += (1.0f - p.y);
        }
        std::vector<core::Mat4> globals;
        clothBoneGlobals(cloth, entity_world, positions, globals);
        float worst = 0.0f;
        for (std::size_t v = 0; v < 25; ++v) {
            const asset::SkinnedVertex& vertex = model.vertices[v];
            const core::Mat4 bone = globals[vertex.joints[0]] * model.bones[vertex.joints[0]].offset;
            const Vec3 skinned = ecs::transformPoint(entity_world * bone, vertex.position);
            worst = std::max(worst, core::length(skinned - positions[v]));
        }
        check(worst < 1e-3f, "el skinning pone cada vertice en su particula");
        std::vector<Vec3> normals;
        clothNormals(cloth, positions, normals);
        const core::Mat4 bone = globals[2] * model.bones[2].offset;  // fila de arriba (plana)
        const Vec3 n = core::normalize(ecs::transformDirection(entity_world * bone, model.vertices[2].normal));
        check(core::dot(n, normals[2]) > 0.99f && std::abs(n.x - 1.0f) < 0.01f, "la normal de delante gira con la entidad (+X)");
    }
}

// Cuerpo blando: una pelota cae al suelo, no lo atraviesa, se aplasta un
// poco pero guarda el volumen (mas presion = menos aplastada), la entidad va
// con ella, los empujones la mueven y el render pone cada vertice en su
// particula.
void testSoftBody() {
    std::printf("Cuerpo blando\n");
    const auto drop = [](float pressure, float& height, float& lowest, Vec3& entity_position) {
        ecs::World world;
        makeFloor(world);
        ecs::Entity ball = world.create("Pelota");
        SoftBody& soft = ball.add<SoftBody>();
        soft.size = Vec3{1.0f, 1.0f, 1.0f};
        soft.pressure = pressure;
        ball.setLocalPosition(Vec3{0.0f, 2.0f, 0.0f});
        PhysicsSystem physics;
        physics.start(world);
        for (int i = 0; i < 240; ++i) physics.update(world, 1.0f / 60.0f);
        const auto& rt = ball.get<SoftBody>().runtime.ptr;
        float lo = 1e9f, hi = -1e9f;
        for (const Vec3& p : rt->positions) {
            lo = std::min(lo, p.y);
            hi = std::max(hi, p.y);
        }
        height = hi - lo;
        lowest = lo;
        entity_position = ball.worldPosition();
        physics.stop();
    };
    float firm_height = 0, firm_low = 0, soft_height = 0, soft_low = 0;
    Vec3 firm_entity{}, soft_entity{};
    drop(3.0f, firm_height, firm_low, firm_entity);
    drop(0.2f, soft_height, soft_low, soft_entity);
    check(firm_low > -0.05f && soft_low > -0.05f, "no atraviesa el suelo");
    check(firm_height > 0.75f && firm_height < 1.05f, "con presion guarda casi toda su forma");
    check(soft_height < firm_height - 0.05f, "con poca presion se aplasta mas (gelatina)");
    check(std::abs(firm_entity.y - firm_height * 0.5f) < 0.2f && std::abs(firm_entity.x) < 0.1f,
          "la entidad va al centro del cuerpo");

    {
        ecs::World world;
        makeFloor(world);
        ecs::Entity jelly = world.create("Gelatina");
        SoftBody& soft = jelly.add<SoftBody>();
        soft.shape = SoftBodyShape::Cube;
        soft.resolution = 4;
        jelly.setLocalPosition(Vec3{0.0f, 0.6f, 0.0f});
        PhysicsSystem physics;
        physics.start(world);
        for (int i = 0; i < 60; ++i) physics.update(world, 1.0f / 60.0f);
        const float before = jelly.worldPosition().y;
        soft.runtime.ptr->pending_velocity = Vec3{0.0f, 6.0f, 0.0f};
        for (int i = 0; i < 15; ++i) physics.update(world, 1.0f / 60.0f);
        check(jelly.worldPosition().y > before + 0.4f, "addSoftBodyImpulse la lanza hacia arriba");

        // Render: cada vertice sigue a su particula.
        const SoftBodyMesh mesh = softBodyMesh(soft);
        const asset::ModelData model = softBodyModel(soft);
        check(model.bones.size() == mesh.particles.size() && mesh.particles.size() == 6u * 25u - 12u * 3u - 16u,
              "cubo 4x4: particulas soldadas en las aristas");
        const core::Mat4 world_matrix = jelly.worldMatrix();
        std::vector<core::Mat4> globals;
        softBodyBoneGlobals(mesh, world_matrix, soft.runtime.ptr->positions, globals);
        float worst = 0.0f;
        for (const asset::SkinnedVertex& v : model.vertices) {
            const core::Mat4 bone = globals[v.joints[0]] * model.bones[v.joints[0]].offset;
            const Vec3 skinned = ecs::transformPoint(world_matrix * bone, v.position);
            worst = std::max(worst, core::length(skinned - soft.runtime.ptr->positions[v.joints[0]]));
        }
        check(worst < 1e-3f, "el skinning pone cada vertice en su particula");
        physics.stop();
    }
}

}  // namespace

ecs::Entity makeCharacter(ecs::World& world, const Vec3& feet, bool keyboard = false) {
    ecs::Entity c = world.create("Personaje");
    c.setWorldPosition(feet);
    CharacterController& cc = c.add<CharacterController>();
    cc.keyboard = keyboard;
    return c;
}

ecs::Entity makeStaticBox(ecs::World& world, const char* name, const Vec3& center, const Vec3& size) {
    ecs::Entity box = world.create(name);
    box.setWorldPosition(center);
    box.add<BoxCollider>().size = size;
    return box;
}

void testCharacterController() {
    std::printf("Character Controller\n");
    {
        // Cae, se posa, anda, choca con la pared y da sus eventos.
        ecs::World world;
        makeFloor(world);
        ecs::Entity wall = makeStaticBox(world, "Pared", Vec3{6.0f, 2.0f, 0.0f}, Vec3{1.0f, 4.0f, 6.0f});
        ecs::Entity hero = makeCharacter(world, Vec3{0.0f, 3.0f, 0.0f});
        PhysicsSystem physics;
        physics.start(world);
        std::map<PhysicsEventType, int> counts;
        bool wall_hit = false;
        physics.addEntityListener(hero, [&](const PhysicsEvent& e) {
            if (e.type == PhysicsEventType::CollisionEnter && e.b == wall) wall_hit = true;
        });
        run(physics, world, 2.0f, &counts);
        check(physics.isCharacter(hero), "el CharacterController crea su personaje al simular");
        check(std::abs(hero.worldPosition().y) < 0.06f, "cae y se posa con los pies en el suelo (y ~ 0)");
        check(physics.characterState(hero).grounded, "isGrounded en el suelo");
        check(counts[PhysicsEventType::CollisionEnter] >= 1, "CollisionEnter al tocar el suelo");

        physics.setCharacterInput(hero, Vec3{1.0f, 0.0f, 0.0f});
        run(physics, world, 1.0f);
        const float walked = hero.worldPosition().x;
        check(walked > 3.0f && walked < 4.5f, "anda a su velocidad (4 m/s, con aceleracion)");
        check(std::abs(physics.linearVelocity(hero).x - 4.0f) < 0.3f, "linearVelocity da la del personaje");
        run(physics, world, 2.0f);
        const float stop = 6.0f - 0.5f - 0.4f;  // cara de la pared - radio
        check(hero.worldPosition().x < stop + 0.01f && hero.worldPosition().x > stop - 0.1f,
              "la pared lo para (no la atraviesa)");
        check((physics.characterState(hero).collision_flags & PhysicsSystem::kCollidedSides) != 0,
              "collision_flags marca Sides contra la pared");
        check(wall_hit, "CollisionEnter con la pared (a = el personaje)");
        // Desliza a lo largo de la pared en diagonal.
        physics.setCharacterInput(hero, Vec3{1.0f, 0.0f, 1.0f});
        const float z0 = hero.worldPosition().z;
        run(physics, world, 0.5f);
        check(hero.worldPosition().z - z0 > 0.8f, "en diagonal contra la pared, desliza por ella");

        // Lo ven los rayos.
        RaycastHit hit;
        const Vec3 p = hero.worldPosition();
        check(physics.raycast(p + Vec3{0.0f, 1.0f, -5.0f}, Vec3{0.0f, 0.0f, 1.0f}, 10.0f, hit) && hit.entity == hero,
              "un rayo toca al personaje (su inner body)");
        QueryFilter skip;
        skip.ignore = hero;
        check(!physics.raycast(p + Vec3{0.0f, 1.0f, -5.0f}, Vec3{0.0f, 0.0f, 1.0f}, 10.0f, hit, skip) || hit.entity != hero,
              "QueryFilter::ignore lo salta");

        // Salto: hasta su altura de salto.
        physics.setCharacterInput(hero, Vec3{});
        run(physics, world, 0.5f);
        check(physics.characterJump(hero), "puede saltar en el suelo");
        float top = 0.0f;
        for (int i = 0; i < 120; ++i) {
            physics.update(world, 1.0f / 60.0f);
            top = std::max(top, hero.worldPosition().y);
        }
        check(top > 1.05f && top < 1.35f, "el salto sube su altura de salto (1.2 m)");
        check(physics.characterState(hero).grounded, "vuelve a posarse tras el salto");
    }
    {
        // Escalones: sube los bajos, no los altos.
        ecs::World world;
        makeFloor(world);
        makeStaticBox(world, "Escalon", Vec3{3.0f, 0.15f, 0.0f}, Vec3{2.0f, 0.3f, 4.0f});
        makeStaticBox(world, "Muro bajo", Vec3{3.0f, 0.4f, 8.0f}, Vec3{2.0f, 0.8f, 4.0f});
        ecs::Entity a = makeCharacter(world, Vec3{0.0f, 0.0f, 0.0f});
        ecs::Entity b = makeCharacter(world, Vec3{0.0f, 0.0f, 8.0f});
        PhysicsSystem physics;
        physics.start(world);
        run(physics, world, 0.3f);
        physics.setCharacterInput(a, Vec3{1.0f, 0.0f, 0.0f});
        physics.setCharacterInput(b, Vec3{1.0f, 0.0f, 0.0f});
        run(physics, world, 0.9f);
        check(std::abs(a.worldPosition().y - 0.3f) < 0.06f && a.worldPosition().x > 2.2f,
              "sube solo un escalon de 0.3 m (altura de escalon 0.35)");
        check(b.worldPosition().x < 2.0f && b.worldPosition().y < 0.1f, "un escalon de 0.8 m lo para");
        run(physics, world, 1.0f);
        check(std::abs(a.worldPosition().y) < 0.06f, "al bajar del escalon vuelve al suelo pegado");
    }
    {
        // Rampas: la suave se sube, la empinada no.
        ecs::World world;
        makeFloor(world);
        const auto ramp = [&](const char* name, float degrees, float z) {
            ecs::Entity r = makeStaticBox(world, name, Vec3{4.0f, 0.0f, z}, Vec3{6.0f, 0.4f, 3.0f});
            r.setLocalEulerDegrees(Vec3{0.0f, 0.0f, degrees});
        };
        ramp("Rampa suave", 25.0f, 0.0f);
        ramp("Rampa empinada", 60.0f, 8.0f);
        ecs::Entity soft = makeCharacter(world, Vec3{0.0f, 0.0f, 0.0f});
        ecs::Entity steep = makeCharacter(world, Vec3{0.0f, 0.0f, 8.0f});
        PhysicsSystem physics;
        physics.start(world);
        run(physics, world, 0.3f);
        physics.setCharacterInput(soft, Vec3{1.0f, 0.0f, 0.0f});
        physics.setCharacterInput(steep, Vec3{1.0f, 0.0f, 0.0f});
        run(physics, world, 1.5f);
        check(soft.worldPosition().y > 0.6f, "sube la rampa de 25 grados");
        check(steep.worldPosition().y < 0.6f, "no sube la rampa de 60 grados (pendiente maxima 45)");
    }
    {
        // Empuja Rigidbody, entra en triggers y sigue a una plataforma.
        ecs::World world;
        makeFloor(world);
        ecs::Entity crate = makeBox(world, "Caja", Vec3{2.0f, 0.5f, 0.0f});
        crate.get<Rigidbody>().mass = 5.0f;
        ecs::Entity zone = makeStaticBox(world, "Zona", Vec3{0.0f, 1.0f, 6.0f}, Vec3{2.0f, 2.0f, 2.0f});
        zone.get<BoxCollider>().material.is_trigger = true;
        ecs::Entity platform = makeBox(world, "Plataforma", Vec3{20.0f, 1.0f, 0.0f}, BodyType::Kinematic);
        platform.get<BoxCollider>().size = Vec3{4.0f, 0.2f, 4.0f};
        ecs::Entity pusher = makeCharacter(world, Vec3{0.0f, 0.0f, 0.0f});
        ecs::Entity walker = makeCharacter(world, Vec3{0.0f, 0.0f, 3.0f});
        ecs::Entity rider = makeCharacter(world, Vec3{20.0f, 1.2f, 0.0f});
        PhysicsSystem physics;
        physics.start(world);
        bool entered = false;
        physics.addEntityListener(zone, [&](const PhysicsEvent& e) {
            if (e.type == PhysicsEventType::TriggerEnter && e.b == walker) entered = true;
        });
        run(physics, world, 0.5f);
        physics.setCharacterInput(pusher, Vec3{1.0f, 0.0f, 0.0f});
        physics.setCharacterInput(walker, Vec3{0.0f, 0.0f, 1.0f});
        const float rider_y = rider.worldPosition().y;
        for (int i = 0; i < 90; ++i) {
            platform.setWorldPosition(Vec3{20.0f + 3.0f * static_cast<float>(i + 1) / 60.0f, 1.0f, 0.0f});
            physics.update(world, 1.0f / 60.0f);
        }
        check(crate.worldPosition().x > 3.0f, "empuja la caja dinamica");
        check(entered, "entra en el trigger (TriggerEnter con b = el personaje)");
        check(rider.worldPosition().x > 23.5f && std::abs(rider.worldPosition().y - rider_y) < 0.1f,
              "de pie sobre una plataforma cinematica, se mueve con ella");
    }
    {
        // Manual (Move de Unity) y agacharse bajo un techo.
        ecs::World world;
        makeFloor(world);
        makeStaticBox(world, "Pared", Vec3{0.0f, 2.0f, -4.0f}, Vec3{6.0f, 4.0f, 1.0f});
        makeStaticBox(world, "Techo bajo", Vec3{8.0f, 1.65f, 0.0f}, Vec3{4.0f, 0.3f, 4.0f});
        ecs::Entity manual = makeCharacter(world, Vec3{0.0f, 0.0f, 0.0f});
        manual.get<CharacterController>().movement = CharacterMovement::Manual;
        ecs::Entity crouch = makeCharacter(world, Vec3{4.0f, 0.0f, 0.0f});
        PhysicsSystem physics;
        physics.start(world);
        run(physics, world, 0.2f);
        const float y0 = manual.worldPosition().y;
        const std::uint32_t flags = physics.moveCharacter(world, manual, Vec3{0.0f, 0.0f, -10.0f});
        check(std::abs(manual.worldPosition().z - (-3.5f + 0.4f)) < 0.1f, "move() avanza hasta la pared y se para");
        check((flags & PhysicsSystem::kCollidedSides) != 0, "move() devuelve Sides");
        run(physics, world, 0.5f);
        check(std::abs(manual.worldPosition().y - y0) < 0.01f, "en Manual no hay gravedad propia");

        physics.setCharacterCrouch(crouch, true);
        physics.setCharacterInput(crouch, Vec3{1.0f, 0.0f, 0.0f});
        run(physics, world, 2.0f);
        check(physics.characterState(crouch).crouching, "se agacha");
        check(crouch.worldPosition().x > 6.5f, "agachado pasa bajo el techo de 1.5 m");
        physics.setCharacterInput(crouch, Vec3{});
        physics.setCharacterCrouch(crouch, false);
        run(physics, world, 0.3f);
        check(physics.characterState(crouch).crouching, "bajo el techo no puede levantarse");
    }
    {
        // Se guarda y se lee con la escena.
        ecs::World world;
        ecs::Entity c = makeCharacter(world, Vec3{1.0f, 2.0f, 3.0f});
        c.get<CharacterController>().jump_height = 2.5f;
        c.get<CharacterController>().movement = CharacterMovement::Manual;
        const std::filesystem::path file = std::filesystem::temp_directory_path() / "cramion_character_test.crscene";
        std::string error;
        check(ecs::saveScene(world, file, &error), "guardar escena con CharacterController");
        ecs::World loaded;
        check(ecs::loadScene(loaded, file, &error), "leerla");
        bool found = false;
        for (const entt::entity h : loaded.registry().view<CharacterController>()) {
            const CharacterController& cc = loaded.registry().get<CharacterController>(h);
            found = std::abs(cc.jump_height - 2.5f) < 1e-4f && cc.movement == CharacterMovement::Manual;
        }
        check(found, "conserva sus campos");
        std::filesystem::remove(file);
    }
}

// Physics.IgnoreCollision: parejas que no chocan (cuerpos y la capsula de
// un personaje, como el Jugador VR con lo que coge).
void testIgnoreCollision() {
    std::printf("Ignorar colisiones\n");
    {
        // Una caja cae sobre otra: con la pareja ignorada la atraviesa.
        ecs::World world;
        makeFloor(world);
        ecs::Entity bottom = makeBox(world, "Abajo", Vec3{0.0f, 0.5f, 0.0f}, BodyType::Static);
        ecs::Entity falling = makeBox(world, "Cae", Vec3{0.0f, 3.0f, 0.0f});
        ecs::Entity stacked = makeBox(world, "Se apila", Vec3{3.0f, 3.0f, 0.0f});
        makeBox(world, "Abajo 2", Vec3{3.0f, 0.5f, 0.0f}, BodyType::Static);
        PhysicsSystem physics;
        physics.start(world);
        physics.ignoreCollision(falling, bottom);
        run(physics, world, 2.0f);
        check(physics.collisionIgnored(bottom, falling) && falling.worldPosition().y < 0.7f,
              "con la pareja ignorada cae a traves de la caja (hasta el suelo)");
        check(stacked.worldPosition().y > 1.3f, "sin ignorar se queda encima");
    }
    {
        // Un Character Controller anda contra una caja dinamica: la atraviesa
        // sin empujarla; ignorar despues de tocarla tambien la suelta.
        ecs::World world;
        makeFloor(world);
        ecs::Entity crate = makeBox(world, "Caja", Vec3{2.0f, 0.5f, 0.0f});
        crate.get<Rigidbody>().mass = 5.0f;
        ecs::Entity walker = makeCharacter(world, Vec3{0.0f, 0.0f, 0.0f});
        PhysicsSystem physics;
        physics.start(world);
        physics.ignoreCollision(walker, crate);
        run(physics, world, 0.3f);
        physics.setCharacterInput(walker, Vec3{1.0f, 0.0f, 0.0f});
        run(physics, world, 1.5f);
        check(walker.worldPosition().x > 3.0f && std::abs(crate.worldPosition().x - 2.0f) < 0.05f,
              "el personaje atraviesa la caja sin empujarla");
        physics.ignoreCollision(walker, crate, false);
        check(!physics.collisionIgnored(walker, crate), "false lo deshace");
    }
}


// Colliders que se ajustan al AABB de la malla (como al anadirlos en Unity).
void testFitColliders() {
    std::printf("-- Ajustar colliders a la malla --\n");
    ecs::World world;
    PhysicsSystem physics;
    // Malla de 4 x 2 x 1 m desplazada (centro en (1, 1.5, -0.5)).
    const auto make_mesh = [](const Vec3& lo, const Vec3& hi) {
        auto mesh = std::make_shared<ecs::Mesh>();
        for (int i = 0; i < 8; ++i) mesh->vertices.push_back(Vec3{(i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y, (i & 4) ? hi.z : lo.z});
        return mesh;
    };
    ecs::Entity e = world.create("Caja");
    e.add<ecs::MeshRenderer>().mesh = make_mesh(Vec3{-1.0f, 0.5f, -1.0f}, Vec3{3.0f, 2.5f, 0.0f});
    e.add<BoxCollider>();
    check(physics.fitColliderToMesh(e), "fitColliderToMesh con una malla");
    const BoxCollider& box = e.get<BoxCollider>();
    check(std::abs(box.size.x - 4.0f) < 1e-4f && std::abs(box.size.y - 2.0f) < 1e-4f && std::abs(box.size.z - 1.0f) < 1e-4f,
          "la caja mide lo que su malla (4 x 2 x 1)");
    check(std::abs(box.center.x - 1.0f) < 1e-4f && std::abs(box.center.y - 1.5f) < 1e-4f && std::abs(box.center.z + 0.5f) < 1e-4f,
          "y esta centrada en ella");
    // La escala del Transform la multiplica despues (no se mete en el tamano).
    e.setLocalScale(Vec3{2.0f, 2.0f, 2.0f});
    physics.fitColliderToMesh(e);
    check(std::abs(e.get<BoxCollider>().size.x - 4.0f) < 1e-4f, "la escala no cambia el tamano local (la aplica la fisica)");
    // Esfera y capsula.
    ecs::Entity s = world.create("Esfera");
    s.add<ecs::MeshRenderer>().mesh = make_mesh(Vec3{-0.5f, 0.0f, -0.5f}, Vec3{0.5f, 3.0f, 0.5f});
    s.add<SphereCollider>();
    s.add<CapsuleCollider>();
    physics.fitColliderToMesh(s);
    check(std::abs(s.get<SphereCollider>().radius - 1.5f) < 1e-4f && std::abs(s.get<SphereCollider>().center.y - 1.5f) < 1e-4f,
          "esfera: la mayor media medida y el centro de la caja");
    const CapsuleCollider& cap = s.get<CapsuleCollider>();
    check(cap.axis == CapsuleAxis::Y && std::abs(cap.height - 3.0f) < 1e-4f && std::abs(cap.radius - 0.5f) < 1e-4f,
          "capsula: a lo largo del eje mas largo (Y, 3 m, radio 0.5)");
    ecs::Entity log = world.create("Tronco");
    log.add<ecs::MeshRenderer>().mesh = make_mesh(Vec3{-2.0f, -0.3f, -0.3f}, Vec3{2.0f, 0.3f, 0.3f});
    log.add<CapsuleCollider>();
    physics.fitColliderToMesh(log);
    check(log.get<CapsuleCollider>().axis == CapsuleAxis::X && std::abs(log.get<CapsuleCollider>().height - 4.0f) < 1e-4f,
          "capsula tumbada: eje X");
    // Sin malla propia: las de los hijos en el espacio del padre (giro y escala incluidos).
    ecs::Entity root = world.create("Raiz");
    root.setWorldPosition(Vec3{10.0f, 0.0f, 0.0f});
    ecs::Entity child = world.create("Hijo", root);
    child.setLocalPosition(Vec3{0.0f, 1.0f, 0.0f});
    child.setLocalEulerDegrees(Vec3{0.0f, 90.0f, 0.0f});
    child.setLocalScale(Vec3{2.0f, 1.0f, 1.0f});
    child.add<ecs::MeshRenderer>().mesh = make_mesh(Vec3{-1.0f, 0.0f, -0.25f}, Vec3{1.0f, 1.0f, 0.25f});
    Vec3 lo{};
    Vec3 hi{};
    check(physics.localMeshBounds(root, lo, hi), "localMeshBounds junta las mallas de los hijos");
    // El hijo mide 4 x 1 x 0.5 (escala 2 en X) y girado 90 grados queda a lo largo de Z.
    check(std::abs((hi.z - lo.z) - 4.0f) < 1e-3f && std::abs((hi.x - lo.x) - 0.5f) < 1e-3f && std::abs(lo.y - 1.0f) < 1e-3f,
          "con el giro y la escala del hijo");
    root.add<BoxCollider>();
    check(physics.fitColliderToMesh(root, "BoxCollider") && std::abs(root.get<BoxCollider>().size.z - 4.0f) < 1e-3f,
          "la caja del padre envuelve a los hijos");
    ecs::Entity empty = world.create("Vacio");
    empty.add<BoxCollider>();
    check(!physics.fitColliderToMesh(empty) && std::abs(empty.get<BoxCollider>().size.x - 1.0f) < 1e-6f,
          "sin malla no cambia nada (caja de 1 m)");
    // Plano: caja fina, no nula.
    check(fitColliderToBounds(empty, Vec3{-5.0f, 0.0f, -5.0f}, Vec3{5.0f, 0.0f, 5.0f}) && empty.get<BoxCollider>().size.y > 0.0f,
          "malla plana: caja fina (no de grosor 0)");
}

int main() {
    testIgnoreCollision();
    testCharacterController();
    testFallAndRest();
    testInterpolation();
    testTriggers();
    testLayersAndQueries();
    testLayerOverrides();
    testKinematicAndForces();
    testParticles();
    testSerialization();
    testCloth();
    testSoftBody();
    testFitColliders();
    std::printf("\n%d comprobaciones, %d fallos\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
