// Splines en el editor: crear carreteras, caminos, rios, muros, vallas,
// tuberias, railes y cintas (GameObject > Spline), la curva y sus puntos
// arrastrables en la Escena (Mayus+clic anade un punto al final, Ctrl+clic en
// un punto lo quita, Alt al arrastrar ignora el terreno) y en el Inspector
// "Aplicar al terreno" (allana y pinta debajo).

#include "EditorApp.h"

#include <CramionCore/environment/FogVolume.h>
#include <CramionCore/spline/Spline.h>

#include <imgui.h>

#include <algorithm>
#include <cmath>

namespace cramion::editor {

using core::Vec3;

namespace {
constexpr ImU32 kSplineColor = IM_COL32(255, 170, 40, 255);
constexpr ImU32 kSplineDim = IM_COL32(255, 170, 40, 90);
constexpr const char* kShapeObjectNames[] = {"Carretera", "Camino", "Rio", "Muro", "Valla", "Tuberia", "Railes", "Cinta"};
}  // namespace

ecs::Entity EditorApp::createSplineObject(int shape) {
    spline::registerSplineComponents();
    const scene::Camera& camera = scene_.camera();
    Vec3 forward = camera.forward();
    Vec3 flat{forward.x, 0.0f, forward.z};
    flat = core::length(flat) > 1e-3f ? core::normalize(flat) : Vec3{0.0f, 0.0f, -1.0f};
    const Vec3 right{-flat.z, 0.0f, flat.x};
    const float len = shape == 2 ? 120.0f : (shape == 4 || shape == 3 ? 24.0f : 60.0f);
    const Vec3 start{camera.position().x + flat.x * 15.0f, 0.0f, camera.position().z + flat.z * 15.0f};
    std::vector<Vec3> pts;
    for (int i = 0; i < 4; ++i) {
        const float t = static_cast<float>(i) / 3.0f;
        Vec3 p = start + flat * (len * t) + right * (std::sin(t * core::kPi * 1.5f) * len * 0.15f);
        float ground = 0.0f;
        p.y = groundHeightAt(p.x, p.z, ground) ? ground : camera.position().y - 2.0f;
        pts.push_back(p);
    }
    const std::string name = shape >= 0 && shape < 8 ? kShapeObjectNames[shape] : "Spline";
    ecs::Entity entity = spline::createSplineEntity(world_, pts, shape, name);
    selectOnly(entity.uuid());
    revealInHierarchy(entity.uuid());
    commit();
    return entity;
}

void EditorApp::drawSplineInspector(ecs::Entity entity) {
    spline::Spline* s = entity.tryGet<spline::Spline>();
    if (s == nullptr) return;
    spline::SplinePath path = spline::worldPath(entity, &terrain_store_);
    ImGui::TextDisabled("%d puntos, %.1f m%s", static_cast<int>(s->points.size()), path.length(),
                        s->closed ? " (cerrada)" : "");
    const float w = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    if (ImGui::Button("Anadir punto al final", ImVec2(w, 0.0f))) {
        spline::SplinePoint p = s->points.empty() ? spline::SplinePoint{} : s->points.back();
        if (s->points.size() >= 2) {
            p.position = p.position + (s->points.back().position - s->points[s->points.size() - 2].position);
        } else {
            p.position = p.position + Vec3{0.0f, 0.0f, 10.0f};
        }
        s->points.push_back(p);
        s->markModified();
        commit();
    }
    ImGui::SameLine();
    if (ImGui::Button("Invertir sentido", ImVec2(w, 0.0f))) {
        std::reverse(s->points.begin(), s->points.end());
        s->markModified();
        commit();
    }
    if (ImGui::Button("Puntos sobre el terreno", ImVec2(w, 0.0f))) {
        const core::Mat4 m = entity.worldMatrix();
        const core::Mat4 inv = core::inverse(m);
        for (spline::SplinePoint& p : s->points) {
            const core::Vec4 wp = m * core::Vec4{p.position, 1.0f};
            float ground = 0.0f;
            if (groundHeightAt(wp.x, wp.z, ground)) {
                const core::Vec4 local = inv * core::Vec4{wp.x, ground, wp.z, 1.0f};
                p.position = Vec3{local.x, local.y, local.z};
            }
        }
        s->markModified();
        commit();
    }
    ImGui::SameLine();
    if (ImGui::Button("Centrar el origen", ImVec2(w, 0.0f)) && !s->points.empty()) {
        // Mueve la entidad al centro de sus puntos sin mover la curva.
        const core::Mat4 m = entity.worldMatrix();
        Vec3 c{};
        for (const spline::SplinePoint& p : s->points) c += p.position;
        c = c * (1.0f / static_cast<float>(s->points.size()));
        const core::Vec4 wc = m * core::Vec4{c, 1.0f};
        for (spline::SplinePoint& p : s->points) p.position = p.position - c;
        entity.setWorldPosition(Vec3{wc.x, wc.y, wc.z});
        s->markModified();
        commit();
    }
    if (entity.has<spline::SplineExtrude>()) {
        ImGui::BeginDisabled(world_.registry().view<terrain::Terrain>().empty());
        if (ImGui::Button("Aplicar al terreno (allanar y pintar)", ImVec2(-1.0f, 0.0f))) {
            const int changed = spline::applyToTerrain(world_, entity, terrain_store_);
            pushToast(changed > 0 ? "Terreno allanado bajo la spline" : "La spline no pasa por ningun terreno");
            dirty_ = true;
            commit();
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("Deja el terreno a la altura de la curva a lo ancho (rio: el cauce, con su profundidad)\n"
                              "y pinta la capa de camino/tierra/arena. La curva deja de pegarse al terreno.");
        }
    }
    ImGui::TextDisabled("Escena: arrastra los puntos; Mayus+clic anade; Ctrl+clic quita; Alt ignora el terreno.");
}

// La curva de las splines (la elegida con sus asas).
bool EditorApp::drawSplineGizmos() {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->PushClipRect(ImVec2(view_x_, view_y_), ImVec2(view_x_ + view_w_, view_y_ + view_h_), true);
    ImGuiIO& io = ImGui::GetIO();
    bool handled = false;
    const auto line = [&](const Vec3& a, const Vec3& b, ImU32 color, float thickness) {
        float ax = 0.0f, ay = 0.0f, bx = 0.0f, by = 0.0f;
        if (worldToScreen(a, ax, ay) && worldToScreen(b, bx, by)) {
            draw->AddLine(ImVec2(ax, ay), ImVec2(bx, by), color, thickness);
        }
    };
    const std::vector<ecs::Entity> selected = selectedEntities();
    const auto isSelected = [&](const ecs::Entity& e) {
        return std::any_of(selected.begin(), selected.end(), [&](const ecs::Entity& o) { return o.uuid() == e.uuid(); });
    };
    for (const entt::entity h : world_.registry().view<spline::Spline>()) {
        ecs::Entity entity = world_.wrap(h);
        spline::Spline& s = entity.get<spline::Spline>();
        const bool sel = isSelected(entity);
        if (!sel && (!s.show_gizmo || !entity.activeInHierarchy())) continue;
        const spline::SplinePath path = spline::worldPath(entity, &terrain_store_);
        const auto& samples = path.samples();
        for (std::size_t i = 0; i + 1 < samples.size(); ++i) {
            line(samples[i].position, samples[i + 1].position, sel ? kSplineColor : kSplineDim, sel ? 2.5f : 1.5f);
        }
        if (!sel) continue;
        // Bordes del ancho.
        if (const spline::SplineExtrude* ex = entity.tryGet<spline::SplineExtrude>()) {
            const float half = ex->width * 0.5f;
            for (std::size_t i = 0; i + 1 < samples.size(); i += 1) {
                const auto& a = samples[i];
                const auto& b = samples[i + 1];
                line(a.position + a.right * (half * a.width), b.position + b.right * (half * b.width), kSplineDim, 1.0f);
                line(a.position - a.right * (half * a.width), b.position - b.right * (half * b.width), kSplineDim, 1.0f);
            }
        }
        const core::Mat4 world = entity.worldMatrix();
        int hovered = -1;
        for (std::size_t i = 0; i < s.points.size(); ++i) {
            const core::Vec4 p = world * core::Vec4{s.points[i].position, 1.0f};
            float sx = 0.0f, sy = 0.0f;
            if (!worldToScreen(Vec3{p.x, p.y, p.z}, sx, sy)) continue;
            const bool over = view_hovered_ && std::hypot(io.MousePos.x - sx, io.MousePos.y - sy) < 9.0f;
            const bool dragging =
                spline_drag_.active && spline_drag_.entity == entity.uuid() && spline_drag_.point == static_cast<int>(i);
            if (over) hovered = static_cast<int>(i);
            const float r = dragging || over ? 8.0f : 6.0f;
            draw->AddCircleFilled(ImVec2(sx, sy), r, dragging ? IM_COL32(255, 230, 90, 255) : IM_COL32(50, 30, 10, 230));
            draw->AddCircle(ImVec2(sx, sy), r, kSplineColor, 0, 2.0f);
            if (i == 0) draw->AddText(ImVec2(sx + 9.0f, sy - 7.0f), kSplineColor, "0");
        }
        if (hovered >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            if (io.KeyCtrl && s.points.size() > 2) {
                s.points.erase(s.points.begin() + hovered);
                s.markModified();
                commit();
            } else {
                const core::Vec4 p = world * core::Vec4{s.points[static_cast<std::size_t>(hovered)].position, 1.0f};
                spline_drag_ = SplineDrag{true, entity.uuid(), hovered, p.y};
            }
            handled = true;
        }
        if (hovered < 0 && io.KeyShift && view_hovered_ && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
            !s.points.empty()) {
            Vec3 point{};
            Vec3 origin{};
            Vec3 direction{};
            bool found = terrainUnderMouse(io.MousePos.x, io.MousePos.y, &point).valid();
            if (!found && mouseRay(io.MousePos.x, io.MousePos.y, origin, direction) && std::abs(direction.y) > 1e-3f) {
                const core::Vec4 last = world * core::Vec4{s.points.back().position, 1.0f};
                const float t = (last.y - origin.y) / direction.y;
                if (t > 0.0f) {
                    point = origin + direction * t;
                    found = true;
                }
            }
            if (found) {
                const core::Vec4 local = core::inverse(world) * core::Vec4{point, 1.0f};
                spline::SplinePoint np = s.points.back();
                np.position = Vec3{local.x, local.y, local.z};
                s.points.push_back(np);
                s.markModified();
                commit();
                handled = true;
            }
        }
        if (hovered >= 0) handled = true;
    }
    if (spline_drag_.active) {
        handled = true;
        ecs::Entity entity = world_.find(spline_drag_.entity);
        spline::Spline* s = entity.valid() ? entity.tryGet<spline::Spline>() : nullptr;
        if (s == nullptr || spline_drag_.point >= static_cast<int>(s->points.size()) ||
            !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            spline_drag_.active = false;
            commit();
        } else {
            Vec3 origin{};
            Vec3 direction{};
            if (mouseRay(io.MousePos.x, io.MousePos.y, origin, direction) && std::abs(direction.y) > 1e-3f) {
                const float t = (spline_drag_.plane_y - origin.y) / direction.y;
                if (t > 0.0f) {
                    Vec3 hit = origin + direction * t;
                    float ground = 0.0f;
                    if (!io.KeyAlt && groundHeightAt(hit.x, hit.z, ground)) hit.y = ground;
                    const core::Vec4 local = core::inverse(entity.worldMatrix()) * core::Vec4{hit, 1.0f};
                    s->points[static_cast<std::size_t>(spline_drag_.point)].position = Vec3{local.x, local.y, local.z};
                    s->markModified();
                }
            }
        }
    }
    draw->PopClipRect();
    return handled;
}

// Volumen de niebla delante de la camara, apoyado en el suelo (12 x 4 x 12 m).
ecs::Entity EditorApp::createFogVolumeEntity() {
    environment::registerFogVolumeComponents();
    const scene::Camera& camera = scene_.camera();
    Vec3 forward = camera.forward();
    Vec3 flat{forward.x, 0.0f, forward.z};
    flat = core::length(flat) > 1e-3f ? core::normalize(flat) : Vec3{0.0f, 0.0f, -1.0f};
    Vec3 p = camera.position() + flat * 15.0f;
    float ground = 0.0f;
    p.y = groundHeightAt(p.x, p.z, ground) ? ground + 2.0f : p.y;
    ecs::Entity e = world_.create("Volumen de niebla");
    e.setWorldPosition(p);
    e.setLocalScale(Vec3{12.0f, 4.0f, 12.0f});
    e.add<environment::FogVolume>();
    selectOnly(e.uuid());
    revealInHierarchy(e.uuid());
    commit();
    return e;
}

}  // namespace cramion::editor
