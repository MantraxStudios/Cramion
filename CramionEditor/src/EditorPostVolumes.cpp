// Volumenes de post-proceso (como los Volume de Unity): crear global, caja o
// esfera y verlos en la Escena. La mezcla segun la camara la hace RenderSync
// (PostProcessing::influence y blendPostProcess).

#include "EditorApp.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>

namespace cramion::editor {

using core::Mat4;
using core::Vec3;

namespace {

constexpr ImU32 kVolumeColor = IM_COL32(110, 200, 255, 255);

Vec3 axisScale(const Mat4& m) {
    return Vec3{core::length(Vec3{m.m[0][0], m.m[0][1], m.m[0][2]}),
                core::length(Vec3{m.m[1][0], m.m[1][1], m.m[1][2]}),
                core::length(Vec3{m.m[2][0], m.m[2][1], m.m[2][2]})};
}

}  // namespace

// shape: 0 global, 1 caja, 2 esfera.
ecs::Entity EditorApp::createPostVolume(int shape, ecs::Entity parent) {
    static constexpr const char* kNames[] = {"Volumen global", "Volumen de caja", "Volumen de esfera"};
    ecs::Entity e = world_.create(kNames[std::clamp(shape, 0, 2)], parent);
    ecs::PostProcessing& volume = e.add<ecs::PostProcessing>();
    volume.shape = static_cast<ecs::PostVolumeShape>(std::clamp(shape, 0, 2));
    // Empieza con el aspecto del global que manda ahora: al marcar una
    // seccion como Sobrescribir se parte de lo que ya se ve.
    const ecs::PostProcessing* base = nullptr;
    for (const entt::entity h : world_.registry().view<ecs::PostProcessing>()) {
        const ecs::PostProcessing& p = world_.registry().get<ecs::PostProcessing>(h);
        if (h == e.handle() || !p.isGlobal()) continue;
        if (base == nullptr || p.priority >= base->priority) base = &p;
    }
    if (base != nullptr) {
        volume.settings = base->settings;
        // Encima del global.
        volume.priority = base->priority + 1;
    }
    if (shape != 0) {
        // Delante de la camara del editor, a ras de suelo si lo hay.
        const scene::Camera& camera = scene_.camera();
        e.setWorldPosition(camera.position() + camera.forward() * 12.0f);
    }
    selectOnly(e.uuid());
    revealInHierarchy(e.uuid());
    commit();
    return e;
}

// Forma de los volumenes locales: la seleccionada en fuerte, las demas
// tenues (con el boton Gizmos). Por fuera, la zona de transicion.
void EditorApp::drawPostVolumeGizmos() {
    for (const entt::entity h : world_.registry().view<ecs::PostProcessing>()) {
        const ecs::Entity e = world_.wrap(h);
        if (!e.activeInHierarchy()) continue;
        const ecs::PostProcessing& v = e.get<ecs::PostProcessing>();
        if (v.isGlobal()) continue;
        const bool selected = isSelected(e.uuid());
        if (!selected && !show_gizmos_) continue;
        const std::uint32_t alpha = selected ? 255u : 110u;
        const ImU32 color = (kVolumeColor & 0x00FFFFFFu) | (alpha << 24);
        const ImU32 blend_color = (kVolumeColor & 0x00FFFFFFu) | ((alpha / 3u) << 24);
        const Mat4 m = e.worldMatrix();
        if (v.shape == ecs::PostVolumeShape::Box) {
            overlayBoxEdges(m * core::scale(v.size), color);
            if (v.blend_distance > 0.0f) {
                // La transicion va en metros: se suma a cada eje ya escalado.
                const Vec3 s = axisScale(m);
                const Vec3 grown{v.size.x + 2.0f * v.blend_distance / std::max(s.x, 1e-4f),
                                 v.size.y + 2.0f * v.blend_distance / std::max(s.y, 1e-4f),
                                 v.size.z + 2.0f * v.blend_distance / std::max(s.z, 1e-4f)};
                overlayBoxEdges(m * core::scale(grown), blend_color);
            }
        } else {
            const Vec3 c{m.m[3][0], m.m[3][1], m.m[3][2]};
            const Vec3 s = axisScale(m);
            const float r = v.radius * std::max({s.x, s.y, s.z});
            const Vec3 axes[3] = {Vec3{1, 0, 0}, Vec3{0, 1, 0}, Vec3{0, 0, 1}};
            for (int i = 0; i < 3; ++i) {
                overlayCircle(c, axes[(i + 1) % 3], axes[(i + 2) % 3], r, color);
                if (v.blend_distance > 0.0f) {
                    overlayCircle(c, axes[(i + 1) % 3], axes[(i + 2) % 3], r + v.blend_distance, blend_color);
                }
            }
        }
    }
}

}  // namespace cramion::editor
