// Pruebas de las cinematicas (consola, sin GPU): rieles, carros, camaras
// virtuales, Camera Brain con mezclas, secuencias y guardar/leer las listas y
// referencias a entidades. Devuelve 0 si todo va.

#include "CramionCore/cinematics/Cinematics.h"
#include "CramionCore/ecs/Components.h"
#include "CramionCore/ecs/SceneSerializer.h"
#include "CramionCore/ecs/World.h"

#include <cmath>
#include <cstdio>

using namespace cramion;
using namespace cramion::cinema;
using core::Vec3;

namespace {

int failures = 0;
int checks = 0;

void check(bool condition, const char* what) {
    ++checks;
    std::printf("  %s %s\n", condition ? "OK   " : "FALLO", what);
    if (!condition) ++failures;
}

bool near(const Vec3& a, const Vec3& b, float eps = 1e-2f) {
    return std::abs(a.x - b.x) < eps && std::abs(a.y - b.y) < eps && std::abs(a.z - b.z) < eps;
}

ecs::Entity makeTrack(ecs::World& world, std::vector<Vec3> points, bool looped = false) {
    ecs::Entity e = world.create("Riel");
    DollyTrack& track = e.add<DollyTrack>();
    track.waypoints.clear();
    for (const Vec3& p : points) track.waypoints.push_back(DollyWaypoint{p, 0.0f});
    track.looped = looped;
    return e;
}

void run(CinematicSystem& system, ecs::World& world, float seconds, bool playing = true) {
    for (int i = 0; i < static_cast<int>(seconds * 60.0f); ++i) system.update(world, 1.0f / 60.0f, playing);
}

void testPath() {
    std::printf("Rieles\n");
    ecs::World world;
    ecs::Entity e = makeTrack(world, {Vec3{0, 0, 0}, Vec3{10, 0, 0}});
    e.setWorldPosition(Vec3{0, 2, 0});
    const DollyPath path(e.get<DollyTrack>(), e.worldMatrix());
    check(std::abs(path.length() - 10.0f) < 1e-3f, "longitud de un riel recto");
    check(near(path.point(path.toPathUnits(5.0f, PositionUnits::Distance)), Vec3{5, 2, 0}),
          "5 metros = mitad (con la posicion de la entidad)");
    check(near(path.point(path.closest(Vec3{3, 7, 0})), Vec3{3, 2, 0}), "punto mas cercano");
    check(std::abs(path.fromPathUnits(0.5f, PositionUnits::Normalized) - 0.5f) < 1e-3f, "unidades normalizadas");

    ecs::Entity curve = makeTrack(world, {Vec3{0, 0, 0}, Vec3{5, 0, 5}, Vec3{10, 0, 0}, Vec3{5, 0, -5}}, true);
    const DollyPath loop(curve.get<DollyTrack>(), curve.worldMatrix());
    check(near(loop.point(1.0f), Vec3{5, 0, 5}) && near(loop.point(2.0f), Vec3{10, 0, 0}),
          "la curva pasa por todos los puntos");
    check(near(loop.point(4.0f), loop.point(0.0f)) && std::abs(loop.segments() - 4.0f) < 1e-5f,
          "en bucle se cierra sobre el primero");
    check(near(loop.point(loop.wrap(-0.5f)), loop.point(3.5f)), "en bucle las posiciones dan la vuelta");

    // Bezier: asas hacia arriba en el primer punto y hacia abajo al llegar.
    ecs::Entity arc = makeTrack(world, {Vec3{0, 0, 0}, Vec3{10, 0, 0}});
    DollyTrack& arc_track = arc.get<DollyTrack>();
    arc_track.mode = PathMode::Bezier;
    arc_track.waypoints[0].tangent = Vec3{0, 6, 0};
    arc_track.waypoints[1].tangent = Vec3{0, -6, 0};
    const DollyPath bezier(arc_track, arc.worldMatrix());
    check(std::abs(bezier.point(0.5f).y - 4.5f) < 1e-3f && near(bezier.point(1.0f), Vec3{10, 0, 0}),
          "Bezier: las asas curvan el tramo y sigue pasando por los puntos");
    arc_track.waypoints[0].tangent = Vec3{};
    arc_track.waypoints[1].tangent = Vec3{};
    const DollyPath automatic(arc_track, arc.worldMatrix());
    check(near(automatic.point(0.5f), Vec3{5, 0, 0}), "Bezier con asas a cero: tangentes automaticas");
}

void testCart() {
    std::printf("Carro sobre riel\n");
    ecs::World world;
    ecs::Entity track = makeTrack(world, {Vec3{0, 0, 0}, Vec3{0, 0, -20}});
    ecs::Entity cart = world.create("Carro");
    DollyCart& c = cart.add<DollyCart>();
    c.track = track.uuid();
    c.speed = 2.0f;
    CinematicSystem system;
    run(system, world, 1.0f, false);
    check(std::abs(c.position) < 1e-4f && near(cart.worldPosition(), Vec3{0, 0, 0}),
          "fuera de Play no avanza (pero se coloca en el riel)");
    run(system, world, 1.0f);
    check(std::abs(c.position - 2.0f) < 0.05f && near(cart.worldPosition(), Vec3{0, 0, -2}, 0.05f),
          "en Play avanza 2 m por segundo");
    check(near(cart.forward(), Vec3{0, 0, -1}), "orientado a lo largo del riel");
    run(system, world, 20.0f);
    check(near(cart.worldPosition(), Vec3{0, 0, -20}, 0.05f), "un riel abierto se para al final");
}

void testVirtualCameras() {
    std::printf("Camaras virtuales y Camera Brain\n");
    ecs::World world;
    ecs::Entity target = world.create("Jugador");
    target.setWorldPosition(Vec3{3, 0, 0});
    ecs::Entity main = world.create("Main Camera");
    main.add<ecs::Camera>();
    main.add<CameraBrain>().default_blend = 1.0f;

    ecs::Entity follow_cam = world.create("Seguir");
    VirtualCamera& a = follow_cam.add<VirtualCamera>();
    a.follow = target.uuid();
    a.look_at = target.uuid();
    a.body = BodyMode::Transposer;
    a.follow_offset = Vec3{0, 2, 6};
    a.damping = Vec3{0, 0, 0};
    a.look_offset = Vec3{0, 0, 0};
    a.fov = 50.0f;
    a.priority = 20;

    ecs::Entity wide = world.create("Plano general");
    wide.setWorldPosition(Vec3{0, 10, 20});
    VirtualCamera& b = wide.add<VirtualCamera>();
    b.aim = AimMode::HardLookAt;
    b.look_at = target.uuid();
    b.fov = 30.0f;
    b.priority = 10;

    CinematicSystem system;
    run(system, world, 0.1f);
    check(near(follow_cam.worldPosition(), Vec3{3, 2, 6}), "Transposer: a su desfase del objetivo");
    const Vec3 to_target = core::normalize(target.worldPosition() - follow_cam.worldPosition());
    check(near(follow_cam.forward(), to_target), "Composer: mira al objetivo");
    check(system.liveCamera() == follow_cam && near(main.worldPosition(), Vec3{3, 2, 6}) &&
              std::abs(main.get<ecs::Camera>().fov - 50.0f) < 1e-3f,
          "el Brain usa la de mayor prioridad (posicion y campo de vision)");

    // Mover el objetivo con amortiguacion: la camara tarda en llegar.
    a.damping = Vec3{1, 1, 1};
    target.setWorldPosition(Vec3{13, 0, 0});
    run(system, world, 0.2f);
    const float x = follow_cam.worldPosition().x;
    check(x > 3.5f && x < 12.0f, "con amortiguacion va detras del objetivo");
    run(system, world, 2.0f);
    check(std::abs(follow_cam.worldPosition().x - 13.0f) < 0.1f, "y acaba llegando");

    // Subir la prioridad del plano general: mezcla de 1 s.
    b.priority = 30;
    run(system, world, 0.5f);
    check(system.liveCamera() == wide && system.blending(), "cambio de prioridad: mezcla");
    const float mid_fov = main.get<ecs::Camera>().fov;
    check(mid_fov > 30.5f && mid_fov < 49.5f, "a mitad de la mezcla, el campo de vision intermedio");
    run(system, world, 1.0f);
    check(!system.blending() && near(main.worldPosition(), Vec3{0, 10, 20}) &&
              std::abs(main.get<ecs::Camera>().fov - 30.0f) < 1e-3f,
          "al terminar la mezcla, la pose de la nueva");

    // Camara en un riel con auto dolly.
    ecs::Entity track = makeTrack(world, {Vec3{-10, 1, 5}, Vec3{30, 1, 5}});
    ecs::Entity dolly = world.create("Dolly");
    VirtualCamera& d = dolly.add<VirtualCamera>();
    d.body = BodyMode::TrackedDolly;
    d.track = track.uuid();
    d.follow = target.uuid();
    d.look_at = target.uuid();
    d.priority = 40;
    d.dolly_damping = 0.0f;
    run(system, world, 0.1f);
    check(near(dolly.worldPosition(), Vec3{13, 1, 5}, 0.05f), "auto dolly: el punto del riel mas cercano al objetivo");

    // Riel muy largo (400 m en 3 puntos): la camara sigue al objetivo sin
    // temblar (sin saltos ni retrocesos entre frames).
    ecs::Entity long_track = makeTrack(world, {Vec3{0, 3, -30}, Vec3{200, 3, -30}, Vec3{400, 3, -30}});
    ecs::Entity runner = world.create("Corredor");
    ecs::Entity long_cam = world.create("Larga");
    VirtualCamera& l = long_cam.add<VirtualCamera>();
    l.body = BodyMode::TrackedDolly;
    l.track = long_track.uuid();
    l.follow = runner.uuid();
    l.look_at = runner.uuid();
    l.dolly_damping = 0.0f;
    l.priority = 50;
    float previous_x = -1.0f;
    float worst_step = 0.0f;
    bool backwards = false;
    for (int i = 0; i < 600; ++i) {
        runner.setWorldPosition(Vec3{10.0f + static_cast<float>(i) * 0.5f, 0.0f, -25.0f});
        system.update(world, 1.0f / 60.0f, true);
        const float x = long_cam.worldPosition().x;
        if (previous_x >= 0.0f) {
            worst_step = std::max(worst_step, std::abs((x - previous_x) - 0.5f));
            backwards = backwards || x < previous_x;
        }
        previous_x = x;
    }
    check(!backwards && worst_step < 0.02f, "riel de 400 m: la camara avanza suave (sin temblar)");
}

void testSequence() {
    std::printf("Secuencias\n");
    ecs::World world;
    ecs::Entity main = world.create("Main Camera");
    main.add<ecs::Camera>();
    main.add<CameraBrain>();
    ecs::Entity cam_a = world.create("A");
    cam_a.setWorldPosition(Vec3{1, 0, 0});
    cam_a.add<VirtualCamera>().priority = 100;  // la secuencia manda aunque haya prioridades
    ecs::Entity cam_b = world.create("B");
    cam_b.setWorldPosition(Vec3{2, 0, 0});
    cam_b.add<VirtualCamera>();
    ecs::Entity prop = world.create("Explosion");
    prop.setActive(false);

    ecs::Entity seq_entity = world.create("Cinematica");
    CinematicSequence& seq = seq_entity.add<CinematicSequence>();
    seq.shots.push_back(CinematicShot{cam_b.uuid(), 0.0f, 2.0f, 0.0f, BlendCurve::Cut});
    seq.shots.push_back(CinematicShot{cam_a.uuid(), 2.0f, 2.0f, 0.0f, BlendCurve::Cut});
    seq.activations.push_back(CinematicActivation{prop.uuid(), 1.0f, 3.0f});
    check(std::abs(seq.duration() - 4.0f) < 1e-5f && seq.shotAt(2.5f) == 1, "duracion y plano en cada momento");

    CinematicSystem system;
    run(system, world, 0.5f);
    check(system.liveCamera() == cam_b && near(main.worldPosition(), Vec3{2, 0, 0}), "primer plano (B)");
    check(!prop.activeSelf(), "el objeto aun no esta activo");
    run(system, world, 1.0f);
    check(prop.activeSelf(), "objeto activo en su tramo");
    bool cut = false;
    for (int i = 0; i < 60; ++i) {
        system.update(world, 1.0f / 60.0f, true);
        cut = cut || system.cutThisFrame();
    }
    check(system.liveCamera() == cam_a && cut, "segundo plano (A) con corte seco");
    run(system, world, 2.0f);
    check(!system.isPlaying(seq_entity) && !prop.activeSelf(), "al terminar: para y apaga los objetos");

    // Vista previa en el editor (sin Play): se queda en el tiempo pedido.
    CinematicSystem editor;
    editor.setPreview(seq_entity, 0.5f, false);
    run(editor, world, 0.2f, false);
    check(editor.liveCamera() == cam_b && std::abs(editor.time(seq_entity) - 0.5f) < 1e-5f,
          "vista previa quieta en 0.5 s: plano B");
    editor.setPreview(seq_entity, 2.5f, false);
    run(editor, world, 0.2f, false);
    check(editor.liveCamera() == cam_a, "mover el cabezal cambia de plano");
}

void testSerialization() {
    std::printf("Guardar y leer\n");
    registerCinematicComponents();
    ecs::World world;
    ecs::Entity target = world.create("Objetivo");
    ecs::Entity track = makeTrack(world, {Vec3{1, 2, 3}, Vec3{4, 5, 6}, Vec3{7, 8, 9}}, true);
    track.get<DollyTrack>().waypoints[1].roll = 15.0f;
    ecs::Entity cam = world.create("Cam");
    VirtualCamera& v = cam.add<VirtualCamera>();
    v.follow = target.uuid();
    v.track = track.uuid();
    v.body = BodyMode::TrackedDolly;
    ecs::Entity seq = world.create("Seq");
    seq.add<CinematicSequence>().shots.push_back(CinematicShot{cam.uuid(), 1.0f, 2.5f, 0.5f, BlendCurve::Linear});

    ecs::World copy;
    ecs::deserializeWorld(copy, ecs::serializeWorld(world));
    const ecs::Entity t = copy.findByName("Riel");
    const ecs::Entity c = copy.findByName("Cam");
    const ecs::Entity s = copy.findByName("Seq");
    check(t.valid() && t.has<DollyTrack>() && t.get<DollyTrack>().waypoints.size() == 3 &&
              near(t.get<DollyTrack>().waypoints[2].position, Vec3{7, 8, 9}) &&
              t.get<DollyTrack>().waypoints[1].roll == 15.0f && t.get<DollyTrack>().looped,
          "lista de puntos del riel de ida y vuelta");
    check(c.valid() && c.get<VirtualCamera>().follow == target.uuid() && c.get<VirtualCamera>().track == track.uuid() &&
              c.get<VirtualCamera>().body == BodyMode::TrackedDolly,
          "referencias a entidades (Follow, riel)");
    check(s.valid() && s.get<CinematicSequence>().shots.size() == 1 &&
              s.get<CinematicSequence>().shots[0].camera == cam.uuid() &&
              s.get<CinematicSequence>().shots[0].duration == 2.5f &&
              s.get<CinematicSequence>().shots[0].curve == BlendCurve::Linear,
          "planos de la secuencia");
}

}  // namespace

int main() {
    testPath();
    testCart();
    testVirtualCameras();
    testSequence();
    testSerialization();
    std::printf("\n%d comprobaciones, %d fallos\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
