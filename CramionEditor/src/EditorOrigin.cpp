// Origen flotante en el editor (ver CramionCore/ecs/FloatingOrigin.h): la
// camara de la Escena (o la del juego en Play) nunca queda lejos del (0,0,0)
// local, asi que editar o jugar a 100 km se ve y se comporta igual que en el
// centro: sin mallas que tiemblan, sombras que parpadean, fisica que vibra ni
// gizmos a saltos. La escena guarda el origen: las posiciones no pierden
// precision al guardar.

#include "EditorApp.h"

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
    nav_.shiftOrigin(offset);
    voxels_.shiftOrigin(offset);
    cinematics_.shiftOrigin(offset);
    audio_.shiftOrigin(offset);
    if (playing()) scripts_.shiftOrigin(offset);
    renderer_.shiftOrigin(offset);
    // La camara del editor, con el mundo (misma posicion real).
    scene::Camera& camera = scene_.camera();
    camera.setPosition(camera.position() - offset);
}

void EditorApp::alignOriginAfterLoad(const ecs::DVec3& before) {
    const ecs::DVec3& now = world_.origin();
    renderer_.setWorldOrigin(now.x, now.y, now.z);
    if (now == before) return;
    // Las entidades ya estan en el origen nuevo: solo se alinea lo demas.
    applyOriginShift(Vec3{static_cast<float>(now.x - before.x), static_cast<float>(now.y - before.y),
                          static_cast<float>(now.z - before.z)},
                     /*move_world=*/false);
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
