// Liquidos en el editor (fluid::FluidSystem): la simulacion cada frame (en
// Play y con "Simular en el editor") y los objetos del menu GameObject >
// Liquidos (grifo, bloque de agua, miel, lava, mundo, desague y un tanque de
// demostracion con un cubo que flota).

#include "EditorApp.h"

#include <algorithm>
#include <cmath>

namespace cramion::editor {

using core::Vec3;

void EditorApp::updateFluids(float delta_seconds) {
    bool simulate = false;
    switch (play_state_) {
        case PlayState::Playing: simulate = true; break;
        case PlayState::Paused: simulate = false; break;
        case PlayState::Edit: {
            simulate = fluid::FluidSystem::previewInEditor(world_);
            // Al apagar la vista previa el liquido se va (como al parar Play).
            if (!simulate && fluid_preview_) fluids_.clear();
            fluid_preview_ = simulate;
            break;
        }
    }
    fluids_.update(world_, delta_seconds, simulate, &physics_, renderer_);
}

ecs::Entity EditorApp::createFluidEntity(int kind) {
    kind = std::clamp(kind, 0, 6);
    // Delante de la camara, sobre el suelo (terreno o y = 0).
    const scene::Camera& camera = scene_.camera();
    const Vec3 forward = camera.forward();
    Vec3 flat{forward.x, 0.0f, forward.z};
    flat = core::length(flat) > 1e-3f ? core::normalize(flat) : Vec3{0.0f, 0.0f, -1.0f};
    Vec3 place = camera.position() + flat * 6.0f;
    float ground = 0.0f;
    const bool on_terrain = groundHeightAt(place.x, place.z, ground);
    place.y = on_terrain ? ground : 0.0f;

    // El mundo de liquidos de la escena (lo crea si no hay): sin terreno
    // debajo, tanque con paredes para que el liquido no se pierda.
    const auto ensureWorld = [&](const Vec3& center, const Vec3& size, bool walls) {
        for (const entt::entity h : world_.registry().view<fluid::FluidWorld>()) return world_.wrap(h);
        ecs::Entity w = world_.create("Liquidos");
        fluid::FluidWorld& fw = w.add<fluid::FluidWorld>();
        fw.size = size;
        fw.solid_walls = walls;
        fw.simulate_in_editor = true;
        w.setWorldPosition(center);
        return w;
    };

    ecs::Entity created;
    switch (kind) {
        case 0:
        case 2:
        case 3: {
            ensureWorld(place + Vec3{0.0f, 2.4f, 0.0f}, Vec3{6.0f, 5.0f, 6.0f}, !on_terrain);
            static constexpr const char* kNames[] = {"Grifo de agua", "", "Chorro de miel", "Chorro de lava"};
            created = world_.create(kNames[kind]);
            fluid::FluidEmitter& em = created.add<fluid::FluidEmitter>();
            em.shape = fluid::EmitterShape::Nozzle;
            em.direction = Vec3{0.0f, -1.0f, 0.0f};
            if (kind == 0) {
                em.fluid = fluid::FluidType::Water;
                em.speed = 2.5f;
                em.nozzle_radius = 0.08f;
            } else if (kind == 2) {
                em.fluid = fluid::FluidType::Honey;
                em.speed = 1.0f;
                em.nozzle_radius = 0.06f;
            } else {
                em.fluid = fluid::FluidType::Lava;
                em.speed = 1.5f;
                em.nozzle_radius = 0.12f;
            }
            created.setWorldPosition(place + Vec3{0.0f, 3.0f, 0.0f});
            break;
        }
        case 1: {
            ensureWorld(place + Vec3{0.0f, 2.4f, 0.0f}, Vec3{6.0f, 5.0f, 6.0f}, !on_terrain);
            created = world_.create("Bloque de agua");
            fluid::FluidEmitter& em = created.add<fluid::FluidEmitter>();
            em.fluid = fluid::FluidType::Water;
            em.shape = fluid::EmitterShape::Box;
            em.size = Vec3{1.0f, 1.0f, 1.0f};
            created.setWorldPosition(place + Vec3{0.0f, 2.5f, 0.0f});
            break;
        }
        case 4: {
            created = ensureWorld(place + Vec3{0.0f, 2.4f, 0.0f}, Vec3{6.0f, 5.0f, 6.0f}, !on_terrain);
            break;
        }
        case 5: {
            created = world_.create("Desague");
            created.add<fluid::FluidDrain>();
            created.setWorldPosition(place + Vec3{0.0f, 0.25f, 0.0f});
            break;
        }
        case 6: {
            // Tanque de 3 x 2.5 x 3 m con paredes, un bloque de agua que cae y
            // un cubo fisico que flota.
            created = world_.create("Tanque de liquido");
            created.setWorldPosition(place + Vec3{0.0f, 1.25f, 0.0f});
            fluid::FluidWorld& fw = created.add<fluid::FluidWorld>();
            fw.size = Vec3{3.0f, 2.5f, 3.0f};
            fw.solid_walls = true;
            fw.simulate_in_editor = true;
            ecs::Entity block = world_.create("Bloque de agua", created);
            fluid::FluidEmitter& em = block.add<fluid::FluidEmitter>();
            em.fluid = fluid::FluidType::Water;
            em.shape = fluid::EmitterShape::Box;
            em.size = Vec3{1.2f, 1.4f, 1.2f};
            block.setWorldPosition(place + Vec3{-0.8f, 1.6f, -0.8f});
            ecs::Entity cube = ecs::createPrimitive(world_, assets::builtin::kCube, "Cubo que flota", created);
            cube.setLocalScale(Vec3{0.4f, 0.4f, 0.4f});
            cube.setWorldPosition(place + Vec3{0.6f, 2.0f, 0.6f});
            physics::Rigidbody& rb = cube.add<physics::Rigidbody>();
            rb.mass = 20.0f;  // ~300 kg/m3: flota como la madera
            if (!physics::hasCollider(cube)) physics::addDefaultCollider(cube);
            break;
        }
        default: break;
    }
    if (!created.valid()) return created;
    selectOnly(created.uuid());
    revealInHierarchy(created.uuid());
    commit();
    return created;
}

}  // namespace cramion::editor
