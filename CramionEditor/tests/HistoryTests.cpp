// Pruebas del historial de deshacer por diferencias (EditorHistory.h, consola,
// sin GPU): cambiar un componente, anadir y quitar componentes, borrar y
// recrear con hijos y UUID, reordenar hermanos, cambiar de padre, el texto de
// la escena armado con los registros, el origen flotante y un mundo grande en
// paralelo (solo lo tocado entra en el paso). Cada deshacer se compara con
// serializeWorld del mundo de antes. Devuelve 0 si todo va.

#include "EditorHistory.h"

#include "CramionCore/ecs/Components.h"
#include "CramionCore/ecs/Prefab.h"
#include "CramionCore/ecs/SceneSerializer.h"
#include "CramionCore/ecs/World.h"
#include "CramionCore/jobs/JobSystem.h"
#include "CramionCore/physics/PhysicsComponents.h"
#include "CramionCore/scripting/Scripting.h"

#include <chrono>
#include <cstdio>
#include <string>

using namespace cramion;
using core::Vec3;
using editor::UndoHistory;

namespace {

int failures = 0;
int checks = 0;

void check(bool condition, const char* what) {
    ++checks;
    std::printf("  %s %s\n", condition ? "OK   " : "FALLO", what);
    if (!condition) ++failures;
}

std::string uuidAt(ecs::World& world, std::size_t root_index) {
    return world.wrap(world.roots()[root_index]).uuid().toString();
}

void basics(bool parallel) {
    std::printf("Pasos basicos (%s)\n", parallel ? "en paralelo" : "en un hilo");
    jobs::setWorkerCount(parallel ? 4 : 0);
    ecs::World world;
    world.setSceneName("Pruebas");
    ecs::Entity a = world.create("A");
    a.setLocalPosition(Vec3{1.0f, 2.0f, 3.0f});
    ecs::Entity b = world.create("B");
    ecs::Entity b1 = world.create("B1", b);
    world.create("B2", b);
    ecs::Entity c = world.create("C");
    c.add<physics::BoxCollider>().size = Vec3{2.0f, 1.0f, 1.0f};

    UndoHistory history;
    history.reset(world);
    const std::string s0 = ecs::serializeWorld(world);
    check(!history.commit(world), "sin cambios no hay paso");
    check(!history.canUndo(), "historial vacio");

    // 1. Un campo del Transform.
    a.setLocalPosition(Vec3{5.0f, 0.0f, 0.0f});
    check(history.commit(world), "mover A es un paso");
    check(history.lastStats().recorded == 1, "solo A en el paso");
    const std::string s1 = ecs::serializeWorld(world);
    const entt::entity c_handle = c.handle();
    check(history.undo(world), "deshacer");
    check(ecs::serializeWorld(world) == s0, "deshacer deja el mundo como antes");
    check(world.find(c.uuid()).handle() == c_handle, "lo no tocado conserva su entidad");
    check(history.redo(world), "rehacer");
    check(ecs::serializeWorld(world) == s1, "rehacer deja el mundo como despues");

    // 2. Un componente nuevo y luego uno de sus campos.
    a.add<ecs::Light>().intensity = 3.0f;
    check(history.commit(world), "anadir Light es un paso");
    const std::string s2 = ecs::serializeWorld(world);
    a.get<ecs::Light>().intensity = 7.0f;  // escritura directa, sin avisar a nadie
    check(history.commit(world), "cambiar un campo sin avisar tambien se ve");
    check(history.undo(world), "deshacer el campo");
    check(ecs::serializeWorld(world) == s2, "intensidad de antes");
    check(history.undo(world), "deshacer el componente");
    check(!a.has<ecs::Light>(), "sin Light");
    check(ecs::serializeWorld(world) == s1, "mundo de antes del componente");
    check(history.redo(world) && history.redo(world), "rehacer los dos");
    check(a.has<ecs::Light>() && a.get<ecs::Light>().intensity == 7.0f, "Light con 7");

    // 3. Borrar un padre con hijos y recrearlo.
    const std::string before_delete = ecs::serializeWorld(world);
    const Uuid b_uuid = b.uuid();
    const Uuid b1_uuid = b1.uuid();
    world.destroy(b);
    check(history.commit(world), "borrar B es un paso");
    check(!world.find(b_uuid).valid(), "B ya no esta");
    check(history.undo(world), "deshacer el borrado");
    const ecs::Entity b_again = world.find(b_uuid);
    const ecs::Entity b1_again = world.find(b1_uuid);
    check(b_again.valid() && b1_again.valid(), "B y B1 vuelven con su UUID");
    check(b1_again.valid() && b1_again.parent() == b_again, "B1 sigue colgando de B");
    check(uuidAt(world, 1) == b_uuid.toString(), "B vuelve a su sitio entre las raices");
    check(ecs::serializeWorld(world) == before_delete, "el mundo queda igual que antes de borrar");
    check(history.redo(world), "rehacer el borrado");
    check(!world.find(b_uuid).valid() && !world.find(b1_uuid).valid(), "B y sus hijos se van otra vez");
    check(history.undo(world), "y deshacer otra vez");

    // 4. Orden de hermanos y cambio de padre.
    const std::string before_order = ecs::serializeWorld(world);
    world.find(b1_uuid).setSiblingIndex(1);
    check(history.commit(world), "reordenar es un paso");
    check(history.undo(world), "deshacer el orden");
    check(ecs::serializeWorld(world) == before_order, "orden de antes");
    world.find(b1_uuid).setParent(world.find(c.uuid()), false);
    check(history.commit(world), "cambiar de padre es un paso");
    check(history.undo(world), "deshacer el cambio de padre");
    check(ecs::serializeWorld(world) == before_order, "padre de antes");
    check(history.redo(world), "rehacer el cambio de padre");
    check(world.find(b1_uuid).parent() == world.find(c.uuid()), "B1 bajo C");
    check(history.undo(world), "deshacer otra vez");

    // 5. El texto de la escena con los registros (Play, pestanas).
    const std::string text = history.sceneText(world);
    ecs::World copy;
    check(ecs::deserializeWorld(copy, text), "el texto armado se lee");
    check(ecs::serializeWorld(copy) == ecs::serializeWorld(world), "y es el mismo mundo");

    // 6. Una accion nueva tira lo que se podia rehacer.
    check(history.canRedo(), "hay algo que rehacer");
    a.setLocalPosition(Vec3{9.0f, 9.0f, 9.0f});
    check(history.commit(world), "accion nueva");
    check(!history.canRedo(), "ya no se puede rehacer");

    // 7. Tope de pasos.
    for (int i = 0; i < 10; ++i) {
        a.setLocalPosition(Vec3{static_cast<float>(i), 0.0f, 0.0f});
        history.commit(world);
    }
    const std::size_t dropped = history.trim(4, ~std::size_t{0});
    check(history.undoCount() == 4 && dropped > 0, "trim deja 4 pasos");
}

void floatingOrigin() {
    std::printf("Origen flotante\n");
    jobs::setWorkerCount(0);
    ecs::World world;
    ecs::Entity a = world.create("A");
    a.setLocalPosition(Vec3{100.0f, 0.0f, 0.0f});
    UndoHistory history;
    history.reset(world);
    a.setLocalPosition(Vec3{110.0f, 0.0f, 0.0f});
    check(history.commit(world), "mover A");
    // El origen se desplaza 100 m (las raices bajan 100 m) sin ser un paso.
    world.shiftOrigin(Vec3{100.0f, 0.0f, 0.0f});
    history.sync(world);
    check(history.undoCount() == 1, "el origen no es un paso");
    check(history.undo(world), "deshacer con el origen movido");
    const Vec3 p = a.localPosition();
    check(p.x > -0.01f && p.x < 0.01f, "A vuelve a 100 m del mundo (0 del origen nuevo)");
}

void large() {
    std::printf("Mundo grande\n");
    jobs::setWorkerCount(-1);
    ecs::World world;
    constexpr int kCount = 10000;
    std::vector<ecs::Entity> entities;
    entities.reserve(kCount);
    for (int i = 0; i < kCount; ++i) {
        ecs::Entity e = world.create("E" + std::to_string(i), i % 10 == 0 || entities.empty() ? ecs::Entity{} : entities.back());
        e.setLocalPosition(Vec3{static_cast<float>(i), 0.0f, 0.0f});
        if (i % 3 == 0) e.add<physics::BoxCollider>();
        if (i % 7 == 0) e.add<ecs::Light>();
        entities.push_back(e);
    }
    UndoHistory history;
    history.reset(world);
    entities[5000].setLocalPosition(Vec3{-1.0f, -1.0f, -1.0f});
    const auto start = std::chrono::steady_clock::now();
    check(history.commit(world), "un cambio entre 10000");
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    std::printf("    paso con %d entidades: %.2f ms (%zu en el paso)\n", kCount, ms, history.lastStats().recorded);
    check(history.lastStats().recorded == 1, "solo una entidad en el paso");
    const auto undo_start = std::chrono::steady_clock::now();
    check(history.undo(world), "deshacer");
    const double undo_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - undo_start).count();
    std::printf("    deshacer: %.2f ms\n", undo_ms);
    check(entities[5000].localPosition().x == 5000.0f, "posicion de antes");
}

}  // namespace

int main() {
    physics::registerPhysicsComponents();
    ecs::registerPrefabComponents();
    scripting::registerScriptComponents();
    basics(false);
    basics(true);
    floatingOrigin();
    large();
    jobs::shutdown();
    std::printf("%d comprobaciones, %d fallos\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
