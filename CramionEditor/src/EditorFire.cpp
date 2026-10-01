// Fuego en el editor: crear una zona de fuego (GameObject > Fuego), su
// estado y botones en el Inspector (encender, apagar, reiniciar) y en la
// Escena el contorno de la zona y "Encender con clic".

#include "EditorApp.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <iostream>

namespace cramion::editor {

using core::Vec3;

namespace {
constexpr ImU32 kFireColor = IM_COL32(255, 120, 40, 255);
constexpr ImU32 kFireColorDim = IM_COL32(255, 120, 40, 110);
}  // namespace

ecs::Entity EditorApp::createFireEntity(const Vec3* position) {
    ecs::Entity entity = world_.create("Fuego");
    fire::Fire& f = entity.add<fire::Fire>();
    Vec3 p{};
    if (position != nullptr) {
        p = *position;
    } else {
        // Delante de la camara, sobre el suelo.
        const Vec3 eye = scene_.camera().position();
        Vec3 forward = scene_.camera().forward();
        forward.y = 0.0f;
        const float len = core::length(forward);
        forward = len > 1e-4f ? forward * (1.0f / len) : Vec3{0.0f, 0.0f, -1.0f};
        p = eye + forward * 40.0f;
        float ground = 0.0f;
        p.y = groundHeightAt(p.x, p.z, ground) ? ground : eye.y - 2.0f;
    }
    entity.setWorldPosition(p);
    // Se ve sin darle a Play (se puede quitar en el Inspector).
    f.simulate_in_editor = true;
    selectOnly(entity.uuid());
    revealInHierarchy(entity.uuid());
    commit();
    return entity;
}

void EditorApp::drawFireInspector(ecs::Entity entity) {
    fire::Fire* f = entity.tryGet<fire::Fire>();
    if (f == nullptr) return;
    const fire::FireStats st = fire::stats(*f);
    if (st.active) {
        ImGui::TextDisabled("%d celdas en llamas (%.0f m²), %d con brasas", st.burning_cells, st.burning_area,
                            st.smoldering_cells);
        ImGui::TextDisabled("Quemado: %.0f%%  ·  zona %.0f m (%d x %d)  ·  %.0f s", st.burned_fraction * 100.0f, st.size,
                            st.resolution, st.resolution, st.simulated_seconds);
    } else if (!playing() && !f->simulate_in_editor) {
        ImGui::TextDisabled("Arde en Play (o marca \"Simular en el editor\")");
    } else {
        ImGui::TextDisabled("Sin fuego");
    }
    const float third = (ImGui::GetContentRegionAvail().x - 8.0f) / 3.0f;
    const bool was_clicking = fire_click_ignite_;
    if (was_clicking) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.32f, 0.08f, 1.0f));
    if (ImGui::Button(was_clicking ? "Clic en la escena..." : "Encender con clic", ImVec2(third, 0.0f))) {
        fire_click_ignite_ = !fire_click_ignite_;
    }
    if (was_clicking) ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Activa y haz clic en el suelo de la vista Escena para encender ahi (Esc para salir)");
    }
    ImGui::SameLine();
    if (ImGui::Button("Apagar", ImVec2(third, 0.0f))) {
        const Vec3 center = entity.worldPosition() + Vec3{st.offset.x + st.size * 0.5f, 0.0f, st.offset.y + st.size * 0.5f};
        fire::extinguish(world_, st.active ? center : entity.worldPosition(),
                         std::max(st.active ? st.size : f->size, 1.0f) * 0.75f);
    }
    ImGui::SameLine();
    if (ImGui::Button("Reiniciar", ImVec2(third, 0.0f))) fire::reset(*f);
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::SliderFloat("##fire_radius", &fire_click_radius_, 0.5f, 30.0f, "Radio del clic: %.1f m");
}

bool EditorApp::drawFireTool() {
    bool any_selected = false;
    ImGuiIO& io = ImGui::GetIO();
    if (show_gizmos_) {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->PushClipRect(ImVec2(view_x_, view_y_), ImVec2(view_x_ + view_w_, view_y_ + view_h_), true);
        for (const ecs::Entity entity : selectedEntities()) {
            const fire::Fire* f = entity.tryGet<fire::Fire>();
            if (f == nullptr) continue;
            any_selected = true;
            const fire::FireStats st = fire::stats(*f);
            const Vec3 o = entity.worldPosition();
            const float side = st.active ? st.size : f->size;
            const float x0 = o.x + (st.active ? st.offset.x : -side * 0.5f);
            const float z0 = o.z + (st.active ? st.offset.y : -side * 0.5f);
            // Contorno sobre el suelo: 16 tramos por lado.
            constexpr int kSegments = 16;
            const auto point = [&](float x, float z) {
                float y = o.y;
                if (!groundHeightAt(x, z, y)) y = o.y;
                return Vec3{x, y + 0.3f, z};
            };
            const Vec3 corners[4] = {Vec3{x0, 0.0f, z0}, Vec3{x0 + side, 0.0f, z0}, Vec3{x0 + side, 0.0f, z0 + side},
                                     Vec3{x0, 0.0f, z0 + side}};
            for (int c = 0; c < 4; ++c) {
                const Vec3 a = corners[c];
                const Vec3 b = corners[(c + 1) % 4];
                for (int s = 0; s < kSegments; ++s) {
                    const float t0 = static_cast<float>(s) / kSegments;
                    const float t1 = static_cast<float>(s + 1) / kSegments;
                    const Vec3 p0 = point(a.x + (b.x - a.x) * t0, a.z + (b.z - a.z) * t0);
                    const Vec3 p1 = point(a.x + (b.x - a.x) * t1, a.z + (b.z - a.z) * t1);
                    float ax = 0.0f, ay = 0.0f, bx = 0.0f, by = 0.0f;
                    if (worldToScreen(p0, ax, ay) && worldToScreen(p1, bx, by)) {
                        draw->AddLine(ImVec2(ax, ay), ImVec2(bx, by), f->limit_to_zone ? kFireColor : kFireColorDim, 2.0f);
                    }
                }
            }
            // Puntos de encendido.
            for (const fire::FireIgnition& ignition : f->ignitions) {
                const Vec3 p = point(o.x + ignition.position.x, o.z + ignition.position.z);
                float sx = 0.0f, sy = 0.0f;
                if (worldToScreen(p, sx, sy)) draw->AddCircleFilled(ImVec2(sx, sy), 4.0f, kFireColor);
            }
        }
        draw->PopClipRect();
    }

    if (!fire_click_ignite_) return false;
    if (!any_selected || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        fire_click_ignite_ = false;
        return false;
    }
    if (!view_hovered_) return false;
    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !io.KeyAlt) {
        Vec3 point{};
        bool found = terrainUnderMouse(io.MousePos.x, io.MousePos.y, &point).valid();
        if (!found) {
            // Sin terreno: el plano horizontal del primer fuego seleccionado.
            Vec3 origin{};
            Vec3 direction{};
            float plane_y = 0.0f;
            for (const ecs::Entity entity : selectedEntities()) {
                if (entity.has<fire::Fire>()) {
                    plane_y = entity.worldPosition().y;
                    break;
                }
            }
            if (mouseRay(io.MousePos.x, io.MousePos.y, origin, direction) && std::abs(direction.y) > 1e-4f) {
                const float t = (plane_y - origin.y) / direction.y;
                if (t > 0.0f) {
                    point = origin + direction * t;
                    found = true;
                }
            }
        }
        if (found) {
            // En el editor solo arden los que se simulan: se activa.
            if (!playing()) {
                for (const ecs::Entity entity : selectedEntities()) {
                    if (fire::Fire* f = entity.tryGet<fire::Fire>(); f != nullptr && !f->simulate_in_editor) {
                        f->simulate_in_editor = true;
                        commit();
                    }
                }
            }
            const int zones = fire::ignite(world_, point, fire_click_radius_);
            if (zones == 0) std::cout << "[Fuego] el punto esta fuera de las zonas de fuego\n";
        }
    }
    return true;
}

}  // namespace cramion::editor
