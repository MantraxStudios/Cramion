// Agua en el editor: crear oceano/lago/rio, preajustes en el Inspector,
// contorno del lago y cauce del rio en la Escena con sus puntos arrastrables
// (Mayus+clic anade un punto al final, Ctrl+clic en un punto lo quita) y
// poner el rio sobre el terreno.

#include "EditorApp.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>

namespace cramion::editor {

using core::Vec3;

namespace {
constexpr ImU32 kWaterColor = IM_COL32(80, 180, 255, 255);
}

// Altura del terreno (el primero que haya debajo), si hay.
bool EditorApp::groundHeightAt(float x, float z, float& height) const {
    for (const entt::entity h : world_.registry().view<terrain::Terrain>()) {
        const ecs::Entity e = world_.wrap(h);
        if (!e.activeInHierarchy()) continue;
        const terrain::Terrain& comp = e.get<terrain::Terrain>();
        const std::shared_ptr<terrain::TerrainData> data = terrain_store_.find(comp);
        if (!data) continue;
        const Vec3 origin = e.worldPosition();
        if (x < origin.x || z < origin.z || x > origin.x + comp.size || z > origin.z + comp.size) continue;
        height = terrain::heightAt(*data, comp, origin, x, z);
        return true;
    }
    return false;
}

// Cada punto del rio a la altura del terreno bajo el (un poco por debajo de
// la orilla: el agua llena el cauce).
void EditorApp::snapRiverToTerrain(ecs::Entity entity) {
    water::WaterBody* body = entity.tryGet<water::WaterBody>();
    if (body == nullptr) return;
    const core::Mat4 world = entity.worldMatrix();
    const core::Mat4 inverse = core::inverse(world);
    for (water::RiverPoint& point : body->points) {
        const core::Vec4 w = world * core::Vec4{point.position.x, point.position.y, point.position.z, 1.0f};
        float ground = 0.0f;
        if (!groundHeightAt(w.x, w.z, ground)) continue;
        const core::Vec4 local = inverse * core::Vec4{w.x, ground + 0.25f, w.z, 1.0f};
        point.position = Vec3{local.x, local.y, local.z};
    }
}

ecs::Entity EditorApp::createWaterEntity(int kind) {
    static constexpr const char* kNames[] = {"Oceano", "Lago", "Rio"};
    kind = std::clamp(kind, 0, 2);
    ecs::Entity entity = world_.create(kNames[kind]);
    water::WaterBody& body = entity.add<water::WaterBody>();
    body = kind == 0 ? water::oceanPreset() : (kind == 1 ? water::lakePreset() : water::riverPreset());

    // Delante de la camara, sobre el suelo (terreno o y = 0). El oceano, a y = 0.
    const scene::Camera& camera = scene_.camera();
    Vec3 place{camera.position().x, 0.0f, camera.position().z};
    if (kind != 0) {
        const Vec3 forward = camera.forward();
        Vec3 flat{forward.x, 0.0f, forward.z};
        flat = core::length(flat) > 1e-3f ? core::normalize(flat) : Vec3{0.0f, 0.0f, -1.0f};
        place = place + flat * 40.0f;
        float ground = 0.0f;
        if (groundHeightAt(place.x, place.z, ground)) place.y = ground + (kind == 1 ? 0.5f : 0.0f);
    } else {
        place = Vec3{0.0f, 0.0f, 0.0f};
    }
    entity.setWorldPosition(place);
    if (kind == 2) snapRiverToTerrain(entity);
    selectOnly(entity.uuid());
    revealInHierarchy(entity.uuid());
    commit();
    return entity;
}

void EditorApp::drawWaterInspector(ecs::Entity entity) {
    water::WaterBody* body = entity.tryGet<water::WaterBody>();
    if (body == nullptr) return;
    ImGui::TextDisabled("Preajustes (conservan el tipo y los puntos):");
    const float w = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 3.0f) / 4.0f;
    const auto apply = [&](water::WaterBody preset) {
        const water::WaterType type = body->type;
        const core::Vec2 size = body->size;
        const std::vector<water::RiverPoint> points = body->points;
        *body = preset;
        body->type = type;
        body->size = size;
        if (!points.empty()) body->points = points;
        commit();
    };
    if (ImGui::Button("Mar calmo", ImVec2(w, 0.0f))) {
        water::WaterBody p = water::oceanPreset();
        p.wave_height = 0.35f;
        p.wavelength = 18.0f;
        apply(p);
    }
    ImGui::SameLine();
    if (ImGui::Button("Tormenta", ImVec2(w, 0.0f))) {
        water::WaterBody p = water::oceanPreset();
        p.wave_height = 3.2f;
        p.wavelength = 80.0f;
        p.steepness = 0.8f;
        p.foam = 1.8f;
        p.deep_color = Vec3{0.006f, 0.022f, 0.03f};
        p.shallow_color = Vec3{0.08f, 0.22f, 0.22f};
        apply(p);
    }
    ImGui::SameLine();
    if (ImGui::Button("Lago", ImVec2(w, 0.0f))) apply(water::lakePreset());
    ImGui::SameLine();
    if (ImGui::Button("Pantano", ImVec2(w, 0.0f))) {
        water::WaterBody p = water::lakePreset();
        p.wave_height = 0.02f;
        p.shallow_color = Vec3{0.25f, 0.30f, 0.12f};
        p.deep_color = Vec3{0.03f, 0.035f, 0.01f};
        p.clarity = 0.8f;
        p.roughness = 0.12f;
        apply(p);
    }
    if (body->type == water::WaterType::River) {
        if (ImGui::Button("Poner el rio sobre el terreno", ImVec2(-1.0f, 0.0f))) {
            snapRiverToTerrain(entity);
            commit();
        }
        ImGui::TextDisabled("Escena: arrastra los puntos; Mayus+clic anade; Ctrl+clic en un punto lo quita.");
    }
}

// Contorno del lago y cauce del rio de lo seleccionado; asas de los puntos.
bool EditorApp::drawWaterGizmos() {
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

    for (const ecs::Entity entity : selectedEntities()) {
        water::WaterBody* body = entity.tryGet<water::WaterBody>();
        if (body == nullptr) continue;
        const core::Mat4 world = entity.worldMatrix();
        if (body->type == water::WaterType::Lake) {
            const Vec3 o = entity.worldPosition();
            const float yaw = std::atan2(-world.m[0][2], world.m[0][0]);
            const Vec3 ax{std::cos(yaw), 0.0f, -std::sin(yaw)};
            const Vec3 az{std::sin(yaw), 0.0f, std::cos(yaw)};
            const float hx = body->size.x * 0.5f;
            const float hz = body->size.y * 0.5f;
            const Vec3 c[4] = {o + ax * hx + az * hz, o - ax * hx + az * hz, o - ax * hx - az * hz, o + ax * hx - az * hz};
            for (int i = 0; i < 4; ++i) line(c[i], c[(i + 1) % 4], kWaterColor, 2.0f);
            continue;
        }
        if (body->type != water::WaterType::River) continue;

        // Cauce: centro y orillas.
        const std::vector<water::RiverSample> centerline = water::riverCenterline(*body, world, 2.0f);
        for (std::size_t i = 0; i + 1 < centerline.size(); ++i) {
            const water::RiverSample& a = centerline[i];
            const water::RiverSample& b = centerline[i + 1];
            const Vec3 sa{-a.tangent.z, 0.0f, a.tangent.x};
            const Vec3 sb{-b.tangent.z, 0.0f, b.tangent.x};
            line(a.position, b.position, IM_COL32(80, 180, 255, 180), 1.5f);
            line(a.position + sa * (a.width * 0.5f), b.position + sb * (b.width * 0.5f), kWaterColor, 2.0f);
            line(a.position - sa * (a.width * 0.5f), b.position - sb * (b.width * 0.5f), kWaterColor, 2.0f);
        }

        // Asas de los puntos.
        int hovered = -1;
        for (std::size_t i = 0; i < body->points.size(); ++i) {
            const core::Vec4 p = world * core::Vec4{body->points[i].position.x, body->points[i].position.y,
                                                    body->points[i].position.z, 1.0f};
            float sx = 0.0f, sy = 0.0f;
            if (!worldToScreen(Vec3{p.x, p.y, p.z}, sx, sy)) continue;
            const bool over = view_hovered_ && std::hypot(io.MousePos.x - sx, io.MousePos.y - sy) < 9.0f;
            const bool dragging = water_drag_.active && water_drag_.entity == entity.uuid() &&
                                  water_drag_.point == static_cast<int>(i);
            if (over) hovered = static_cast<int>(i);
            draw->AddCircleFilled(ImVec2(sx, sy), dragging || over ? 8.0f : 6.0f,
                                  dragging ? IM_COL32(255, 200, 60, 255) : IM_COL32(20, 40, 60, 230));
            draw->AddCircle(ImVec2(sx, sy), dragging || over ? 8.0f : 6.0f, kWaterColor, 0, 2.0f);
        }

        // Empezar a arrastrar / quitar.
        if (hovered >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            if (io.KeyCtrl && body->points.size() > 2) {
                body->points.erase(body->points.begin() + hovered);
                commit();
            } else {
                const core::Vec4 p = world * core::Vec4{body->points[hovered].position.x, body->points[hovered].position.y,
                                                        body->points[hovered].position.z, 1.0f};
                water_drag_ = WaterDrag{true, entity.uuid(), hovered, p.y};
            }
            handled = true;
        }
        // Mayus+clic en el suelo: un punto nuevo al final.
        if (hovered < 0 && io.KeyShift && view_hovered_ && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
            !body->points.empty()) {
            Vec3 point{};
            Vec3 origin{};
            Vec3 direction{};
            bool found = terrainUnderMouse(io.MousePos.x, io.MousePos.y, &point).valid();
            if (found) point.y += 0.25f;
            if (!found && mouseRay(io.MousePos.x, io.MousePos.y, origin, direction) && std::abs(direction.y) > 1e-3f) {
                const core::Vec4 last = world * core::Vec4{body->points.back().position.x, body->points.back().position.y,
                                                           body->points.back().position.z, 1.0f};
                const float t = (last.y - origin.y) / direction.y;
                if (t > 0.0f) {
                    point = origin + direction * t;
                    found = true;
                }
            }
            if (found) {
                const core::Vec4 local = core::inverse(world) * core::Vec4{point.x, point.y, point.z, 1.0f};
                body->points.push_back(water::RiverPoint{Vec3{local.x, local.y, local.z}, body->points.back().width});
                commit();
                handled = true;
            }
        }
        if (hovered >= 0) handled = true;
    }

    // Arrastre: sobre el plano horizontal del punto (o el terreno, si lo hay).
    if (water_drag_.active) {
        handled = true;
        ecs::Entity entity = world_.find(water_drag_.entity);
        water::WaterBody* body = entity.valid() ? entity.tryGet<water::WaterBody>() : nullptr;
        if (body == nullptr || water_drag_.point >= static_cast<int>(body->points.size()) ||
            !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            water_drag_.active = false;
            commit();
        } else {
            Vec3 origin{};
            Vec3 direction{};
            if (mouseRay(io.MousePos.x, io.MousePos.y, origin, direction) && std::abs(direction.y) > 1e-3f) {
                const float t = (water_drag_.plane_y - origin.y) / direction.y;
                if (t > 0.0f) {
                    Vec3 hit = origin + direction * t;
                    float ground = 0.0f;
                    if (!io.KeyAlt && groundHeightAt(hit.x, hit.z, ground)) hit.y = ground + 0.25f;
                    const core::Vec4 local =
                        core::inverse(entity.worldMatrix()) * core::Vec4{hit.x, hit.y, hit.z, 1.0f};
                    body->points[water_drag_.point].position = Vec3{local.x, local.y, local.z};
                }
            }
        }
    }
    draw->PopClipRect();
    return handled;
}

}  // namespace cramion::editor
