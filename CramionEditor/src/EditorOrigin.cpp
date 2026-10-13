// Origen flotante en el editor (ver CramionCore/ecs/FloatingOrigin.h): la
// camara de la Escena (o la del juego en Play) nunca queda lejos del (0,0,0)
// local, asi que editar o jugar a 100 km se ve y se comporta igual que en el
// centro: sin mallas que tiemblan, sombras que parpadean, fisica que vibra ni
// gizmos a saltos. La escena guarda el origen: las posiciones no pierden
// precision al guardar.

#include "EditorApp.h"

#include <CramionCore/ecs/SceneReset.h>

#include <ImGuizmo.h>
#include <imgui.h>

#include <iostream>

namespace cramion::editor {

using core::Vec3;

void EditorApp::applyOriginShift(const Vec3& offset, bool move_world) {
    if (offset.x == 0.0f && offset.y == 0.0f && offset.z == 0.0f) return;
    if (move_world) world_.shiftOrigin(offset);
    physics_.shiftOrigin(offset);
    particles_.shiftOrigin(offset);
    fluids_.shiftOrigin(offset);
    vfx_.shiftOrigin(offset);
    nav_.shiftOrigin(offset);
    voxels_.shiftOrigin(offset);
    cinematics_.shiftOrigin(offset);
    audio_.shiftOrigin(offset);
    world_partition_.shiftOrigin(offset);  // sus celdas son absolutas: solo sigue al origen
    if (playing()) scripts_.shiftOrigin(offset);
    if (playing()) cpp_scripts_.shiftOrigin(offset);
    renderer_.shiftOrigin(offset);
    // La camara del editor, con el mundo (misma posicion real).
    scene::Camera& camera = scene_.camera();
    camera.setPosition(camera.position() - offset);
}

void EditorApp::alignOriginAfterLoad(const ecs::DVec3& before) {
    const ecs::DVec3 now = world_.origin();
    if (now != before) {
        // Las entidades ya estan en el origen nuevo (las teselas que se quedan
        // tambien: World::clearPersistent las recoloca): solo se alinea lo demas.
        applyOriginShift(Vec3{static_cast<float>(now.x - before.x), static_cast<float>(now.y - before.y),
                              static_cast<float>(now.z - before.z)},
                         /*move_world=*/false);
    }
    // El origen absoluto del renderizador DESPUES del desplazamiento:
    // VulkanRenderer::shiftOrigin tambien le suma el offset, y antes acababa
    // en 2*now - before (las nubes desplazadas tras cada cambio de escena).
    renderer_.setWorldOrigin(now.x, now.y, now.z);
}

void EditorApp::resetSceneSystems(bool keep_terrain_edits) {
    // Video: la vista previa del Inspector es de la escena que se va. (Los
    // caminos en Play ya pararon el video antes que el audio; aqui solo se
    // asegura: VideoSystem::stop es idempotente.)
    video_.stopPreview();
    video_preview_entity_ = {};
    ecs::SceneResetTargets targets;
    targets.video = &video_;
    targets.terrain_streamer = &terrain_streamer_;
    targets.terrain_store = &terrain_store_;
    targets.partition = &world_partition_;
    targets.physics = &physics_;
    targets.navigation = &nav_;
    targets.render = sync_.get();
    ecs::SceneResetOptions options;
    // Nunca se guarda solo al cambiar de escena: el usuario ya eligio
    // guardar (saveAll) o no. Antes las teselas de la escena que se dejaba se
    // guardaban al soltarse aunque se descartaran los cambios.
    options.save_terrain = false;
    options.discard_terrain_data = !keep_terrain_edits;
    options.clear_world = true;
    ecs::resetSceneState(world_, targets, options);
    // Lo que dependia de entidades de la escena anterior.
    nav_.resetAgents();
    renderer_.invalidateHistory();
}

void EditorApp::updateFloatingOrigin() {
    if (!has_project_) return;
    // Nunca a mitad de arrastrar un gizmo o un collider (guardan posiciones
    // del mundo del principio del arrastre).
    if (ImGuizmo::IsUsing() || collider_handle_drag_ != 0) return;
    Vec3 focus = scene_.camera().position();
    if (playing()) {
        // En Play manda donde esta el juego: su camara principal.
        for (const entt::entity h : world_.registry().view<ecs::Camera>()) {
            const ecs::Entity e = world_.wrap(h);
            if (e.activeInHierarchy() && e.get<ecs::Camera>().is_main) {
                focus = e.worldPosition();
                break;
            }
        }
    }
    if (const std::optional<Vec3> offset = ecs::updateFloatingOrigin(world_, focus, floating_origin_)) {
        applyOriginShift(*offset, /*move_world=*/false);  // el mundo ya lo movio updateFloatingOrigin
        const ecs::DVec3& o = world_.origin();
        std::cout << "[Origen] Mundo desplazado (" << offset->x << ", " << offset->y << ", " << offset->z
                  << "); origen = (" << o.x << ", " << o.y << ", " << o.z << ")\n";
    }
}

}  // namespace cramion::editor
