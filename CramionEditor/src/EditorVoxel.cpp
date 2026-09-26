// Mundo de bloques en el editor: crearlo desde el menu (con su mar), la
// vista previa alrededor de la camara del editor mientras se edita (se
// rehace al cambiar los ajustes en el Inspector) y, en Play, un mundo nuevo
// alrededor de la camara principal (los scripts lo guardan con Voxel.*).

#include "EditorApp.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace cramion::editor {

using core::Vec3;

namespace {

// El VoxelWorld activo de la escena (el primero), o nulo.
const voxel::VoxelWorld* findVoxelWorld(ecs::World& world) {
    for (const entt::entity h : world.registry().view<voxel::VoxelWorld>()) {
        const ecs::Entity e = world.wrap(h);
        if (e.activeInHierarchy()) return &e.get<voxel::VoxelWorld>();
    }
    return nullptr;
}

// Lo que obliga a rehacer el mundo si cambia en el Inspector.
std::string voxelSignature(const voxel::VoxelWorld& w) {
    std::ostringstream s;
    s << w.seed << '|' << w.render_distance << '|' << w.sea_level << '|' << w.textures << '|' << w.texture_size;
    return s.str();
}

}  // namespace

// Quien mira: en Play la camara principal; editando, la del editor.
Vec3 EditorApp::voxelViewer() {
    Vec3 position = scene_.camera().position();
    if (!playing()) return position;
    world_.forEachDepthFirst([&](ecs::Entity e) {
        if (const ecs::Camera* c = e.tryGet<ecs::Camera>(); c != nullptr && c->is_main && e.activeInHierarchy()) {
            position = e.worldPosition();
        }
    });
    return position;
}

void EditorApp::startVoxels() {
    voxels_.start(world_);
    const voxel::VoxelWorld* settings = findVoxelWorld(world_);
    voxel_signature_ = settings != nullptr ? voxelSignature(*settings) : std::string();
}

void EditorApp::stopVoxels() {
    voxels_.stop();
    voxel_signature_.clear();
}

void EditorApp::updateVoxels(float delta_seconds) {
    if (!has_project_) return;
    const voxel::VoxelWorld* settings = findVoxelWorld(world_);
    if (!playing()) {
        // Vista previa: encender, apagar o rehacer segun la escena.
        const bool wanted = settings != nullptr && settings->preview_in_editor;
        if (!wanted) {
            if (voxels_.active()) stopVoxels();
        } else if (!voxels_.active() || voxelSignature(*settings) != voxel_signature_) {
            startVoxels();
        }
    } else if (settings == nullptr && voxels_.active()) {
        stopVoxels();  // lo quito un script (o Scene.load a otra escena)
    }
    if (voxels_.active()) voxels_.update(delta_seconds, voxelViewer());
}

ecs::Entity EditorApp::createVoxelWorldEntity() {
    // Uno por escena: si ya hay, se selecciona.
    for (const entt::entity h : world_.registry().view<voxel::VoxelWorld>()) {
        const ecs::Entity existing = world_.wrap(h);
        selectOnly(existing.uuid());
        revealInHierarchy(existing.uuid());
        return existing;
    }
    ecs::Entity entity = world_.create("Mundo de bloques");
    voxel::VoxelWorld& settings = entity.add<voxel::VoxelWorld>();
    // El mar: el oceano del motor justo en la superficie de los bloques de agua.
    ecs::Entity sea = world_.create("Mar", entity);
    water::WaterBody& body = sea.add<water::WaterBody>();
    body = water::oceanPreset();
    sea.setWorldPosition(Vec3{0.0f, std::round(settings.sea_level) - 0.1f, 0.0f});

    // La camara del editor sobre el suelo del origen.
    startVoxels();
    const float ground = static_cast<float>(voxels_.surfaceHeight(8, 8));
    scene_.placeCamera(Vec3{8.0f, ground + 18.0f, 30.0f}, Vec3{8.0f, ground, 8.0f});
    selectOnly(entity.uuid());
    revealInHierarchy(entity.uuid());
    commit();
    return entity;
}

}  // namespace cramion::editor
