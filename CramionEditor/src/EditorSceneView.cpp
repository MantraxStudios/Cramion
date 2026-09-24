// Vista de Escena: la imagen del render con la barra de herramientas, los
// gizmos (ImGuizmo) con snapping, la seleccion por clic, los iconos de luces
// y camaras, la camara del editor y soltar assets.

#include "EditorApp.h"

#include "Dialogs.h"

#include <imgui.h>
#include <iostream>
#include <imgui_internal.h>

#include <ImGuizmo.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace cramion::editor {

using core::Mat4;
using core::Vec3;
using core::Vec4;

namespace {

// Rayo contra caja alineada (placas). Distancia de entrada o < 0.
float rayBox(const Vec3& origin, const Vec3& direction, const core::Aabb& box) {
    float t_min = 0.0f;
    float t_max = std::numeric_limits<float>::max();
    const float o[3] = {origin.x, origin.y, origin.z};
    const float d[3] = {direction.x, direction.y, direction.z};
    const float lo[3] = {box.min.x, box.min.y, box.min.z};
    const float hi[3] = {box.max.x, box.max.y, box.max.z};
    for (int i = 0; i < 3; ++i) {
        if (std::abs(d[i]) < 1e-9f) {
            if (o[i] < lo[i] || o[i] > hi[i]) return -1.0f;
            continue;
        }
        float t0 = (lo[i] - o[i]) / d[i];
        float t1 = (hi[i] - o[i]) / d[i];
        if (t0 > t1) std::swap(t0, t1);
        t_min = std::max(t_min, t0);
        t_max = std::min(t_max, t1);
        if (t_min > t_max) return -1.0f;
    }
    return t_min;
}

// Boton de la barra que se queda "pulsado" cuando esta activo.
bool toolButton(const char* label, bool active, const char* tooltip) {
    if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_Header));
    const bool pressed = ImGui::Button(label, ImVec2(0.0f, 0.0f));
    if (active) ImGui::PopStyleColor();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", tooltip);
    return pressed;
}

}  // namespace

void EditorApp::drawToolbar() {
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f, 4.0f));
    ImGui::SetCursorPos(ImVec2(6.0f, ImGui::GetCursorPosY() + 4.0f));
    if (toolButton("Q Ninguno", gizmo_ == GizmoOperation::None, "Sin gizmo (Q)")) gizmo_ = GizmoOperation::None;
    ImGui::SameLine();
    if (toolButton("W Mover", gizmo_ == GizmoOperation::Translate, "Mover (W)")) gizmo_ = GizmoOperation::Translate;
    ImGui::SameLine();
    if (toolButton("E Rotar", gizmo_ == GizmoOperation::Rotate, "Rotar (E)")) gizmo_ = GizmoOperation::Rotate;
    ImGui::SameLine();
    if (toolButton("R Escalar", gizmo_ == GizmoOperation::Scale, "Escalar (R)")) gizmo_ = GizmoOperation::Scale;
    drawStampToolbar();
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    if (toolButton(gizmo_local_ ? "Local" : "Mundo", false,
                   "Espacio del gizmo: ejes del objeto o del mundo (X)")) {
        gizmo_local_ = !gizmo_local_;
    }
    ImGui::SameLine();
    if (toolButton("Snap", snap_enabled_, "Ajustar a la rejilla (mantener Ctrl invierte)")) {
        snap_enabled_ = !snap_enabled_;
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(62.0f);
    ImGui::DragFloat("##snap_t", &snap_translate_, 0.01f, 0.01f, 100.0f, "%.2f m");
    ImGui::SetItemTooltip("Paso al mover");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(52.0f);
    ImGui::DragFloat("##snap_r", &snap_rotate_, 0.5f, 1.0f, 180.0f, "%.0f°");
    ImGui::SetItemTooltip("Paso al rotar");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(52.0f);
    ImGui::DragFloat("##snap_s", &snap_scale_, 0.005f, 0.01f, 10.0f, "%.2f");
    ImGui::SetItemTooltip("Paso al escalar");
    ImGui::PopStyleVar();
}

void EditorApp::drawSceneView() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    const bool open = ImGui::Begin("Escena", nullptr,
                                   ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    scene_dock_id_ = ImGui::GetWindowDockID();
    if (!open) {
        flying_ = false;
        ImGui::End();
        return;
    }
    view_focused_ = ImGui::IsWindowFocused();
    drawToolbar();

    // La imagen, entera y sin deformar.
    const vk::Extent2D extent = renderer_.sceneExtent();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float aspect = extent.height > 0 ? static_cast<float>(extent.width) / static_cast<float>(extent.height) : 1.0f;
    ImVec2 size = avail;
    if (avail.x / std::max(avail.y, 1.0f) > aspect) size.x = avail.y * aspect;
    else size.y = avail.x / aspect;
    size.x = std::max(size.x, 1.0f);
    size.y = std::max(size.y, 1.0f);
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    const ImVec2 origin{cursor.x + (avail.x - size.x) * 0.5f, cursor.y + (avail.y - size.y) * 0.5f};
    ImGui::SetCursorScreenPos(origin);
    ImGui::Image(imgui_.sceneTexture(), size);
    view_x_ = origin.x;
    view_y_ = origin.y;
    view_w_ = size.x;
    view_h_ = size.y;
    view_hovered_ = ImGui::IsItemHovered();

    // Soltar assets en la escena: en el punto del suelo (y = 0) bajo el
    // raton, o delante de la camara.
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAssetPayload)) {
            AssetPayload asset{};
            std::memcpy(&asset, payload->Data, sizeof(asset));
            if (asset.type == assets::AssetType::Model) {
                Vec3 ray_origin{};
                Vec3 ray_direction{};
                std::optional<Vec3> place;
                if (mouseRay(ImGui::GetMousePos().x, ImGui::GetMousePos().y, ray_origin, ray_direction)) {
                    if (ray_direction.y < -1e-3f) {
                        const float t = -ray_origin.y / ray_direction.y;
                        if (t > 0.0f && t < 500.0f) place = ray_origin + ray_direction * t;
                    }
                    if (!place) place = ray_origin + ray_direction * 6.0f;
                }
                instantiateAsset(asset.uuid, {}, place);
            } else if (asset.type == assets::AssetType::Environment) {
                assignEnvironment(asset.uuid);
            } else if (asset.type == assets::AssetType::Scene) {
                if (const auto info = database_->find(asset.uuid)) {
                    runOrAskToSave(PendingAction::OpenScene, info->path);
                }
            }
        }
        // Imagen: se estampa donde se suelta (como arrastrar un material de
        // decal en Unreal).
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kImagePayload)) {
            const std::string path(static_cast<const char*>(payload->Data));
            Vec3 point{};
            Vec3 normal{};
            if (surfaceHit(ImGui::GetMousePos().x, ImGui::GetMousePos().y, point, normal)) {
                const int type = stamp_brush_.type;
                stamp_brush_.type = 0;
                const ecs::Entity e = stampAt(point, normal, decalImageInAssets(dialogs::fromUtf8(path)));
                stamp_brush_.type = type;
                selectOnly(e.uuid());
                revealInHierarchy(e.uuid());
                commit();
            }
        }
        ImGui::EndDragDropTarget();
    }

    drawSceneOverlays();
    const bool light_handle = drawLightGizmos();
    drawDecalGizmos();
    const bool stamping = drawStampTool();
    if (!stamping) drawGizmo();
    handleCameraControls();

    // Seleccionar: clic izquierdo sin arrastrar, fuera del gizmo.
    ImGuiIO& io = ImGui::GetIO();
    if (view_hovered_ && !light_handle && !stamping && ImGui::IsMouseReleased(ImGuiMouseButton_Left) && !ImGuizmo::IsUsing() &&
        !ImGuizmo::IsOver() && ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 2.0f).x == 0.0f &&
        ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 2.0f).y == 0.0f) {
        pickAt(io.MousePos.x, io.MousePos.y);
    }

    // Atajos de la vista.
    if ((view_hovered_ || view_focused_) && !io.WantTextInput && !flying_) {
        if (ImGui::IsKeyPressed(ImGuiKey_Q, false)) gizmo_ = GizmoOperation::None;
        if (ImGui::IsKeyPressed(ImGuiKey_W, false)) gizmo_ = GizmoOperation::Translate;
        if (ImGui::IsKeyPressed(ImGuiKey_E, false)) gizmo_ = GizmoOperation::Rotate;
        if (ImGui::IsKeyPressed(ImGuiKey_R, false)) gizmo_ = GizmoOperation::Scale;
        if (ImGui::IsKeyPressed(ImGuiKey_X, false)) gizmo_local_ = !gizmo_local_;
        if (ImGui::IsKeyPressed(ImGuiKey_T, false)) stamp_mode_ = !stamp_mode_;
        if (ImGui::IsKeyPressed(ImGuiKey_F, false)) focusSelection();
        if (ImGui::IsKeyPressed(ImGuiKey_Delete)) deleteSelection();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D, false)) duplicateSelection();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C, false)) copySelection();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V, false)) pasteClipboard();
    }

    // Ayuda discreta.
    ImGui::SetCursorScreenPos(ImVec2(origin.x + 10.0f, origin.y + size.y - 24.0f));
    ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 0.5f),
                       "Clic: seleccionar  |  Botón derecho + WASD: volar  |  Rueda: acercar  |  "
                       "Botón central: desplazar  |  F: enfocar");
    ImGui::End();
}

// Camara del editor: volar (boton derecho, lo hace scene::Camera con la
// entrada), acercar con la rueda y desplazar con el boton central.
void EditorApp::handleCameraControls() {
    ImGuiIO& io = ImGui::GetIO();
    if (view_hovered_ && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        flying_ = true;
        ImGui::SetWindowFocus();
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
        flying_ = false;
    }
    if (flying_) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_None);
        return;
    }
    scene::Camera& camera = scene_.camera();
    // Con la herramienta de estampar, Ctrl/Mayus + rueda son del pincel.
    if (view_hovered_ && io.MouseWheel != 0.0f && !(stamp_mode_ && (io.KeyCtrl || io.KeyShift))) {
        const float step = std::max(camera.moveSpeed() * 0.08f, 0.2f) * (io.KeyShift ? 4.0f : 1.0f);
        camera.setPosition(camera.position() + camera.forward() * (io.MouseWheel * step));
    }
    if (view_hovered_ && ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f)) {
        const float k = std::max(camera.moveSpeed() * 0.0025f, 0.004f);
        camera.setPosition(camera.position() - camera.right() * (io.MouseDelta.x * k) +
                           camera.up() * (io.MouseDelta.y * k));
    }
}

bool EditorApp::worldToScreen(const Vec3& world, float& x, float& y) const {
    const Mat4 view_projection = scene_.camera().projection() * scene_.camera().view();
    const Vec4 clip = view_projection * Vec4{world.x, world.y, world.z, 1.0f};
    if (clip.w <= 0.01f) return false;
    x = view_x_ + (clip.x / clip.w * 0.5f + 0.5f) * view_w_;
    y = view_y_ + (clip.y / clip.w * 0.5f + 0.5f) * view_h_;
    return true;
}

bool EditorApp::mouseRay(float x, float y, Vec3& origin, Vec3& direction) const {
    const float u = (x - view_x_) / view_w_;
    const float v = (y - view_y_) / view_h_;
    if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f) return false;
    const Mat4 inverse = core::inverse(scene_.camera().projection() * scene_.camera().view());
    const Vec4 near_h = inverse * Vec4{u * 2.0f - 1.0f, v * 2.0f - 1.0f, 0.0f, 1.0f};
    const Vec4 far_h = inverse * Vec4{u * 2.0f - 1.0f, v * 2.0f - 1.0f, 1.0f, 1.0f};
    origin = Vec3{near_h.x / near_h.w, near_h.y / near_h.w, near_h.z / near_h.w};
    const Vec3 far_point{far_h.x / far_h.w, far_h.y / far_h.w, far_h.z / far_h.w};
    direction = core::normalize(far_point - origin);
    return true;
}

// Clic en la vista: los iconos de luces y camaras primero; luego el actor mas
// cercano que toca el rayo. Como Unity: el primer clic elige la raiz del
// objeto (el modelo entero); otro clic en el mismo sitio baja un nivel hacia
// la pieza tocada.
void EditorApp::pickAt(float x, float y) {
    const bool additive = ImGui::GetIO().KeyCtrl;
    const auto choose = [&](const Uuid& uuid) {
        if (additive) toggleSelection(uuid);
        else selectOnly(uuid);
        revealInHierarchy(uuid);
    };

    // Iconos.
    Uuid icon_hit{};
    world_.forEachDepthFirst([&](ecs::Entity e) {
        if (icon_hit.valid() || !e.activeInHierarchy()) return;
        if (!e.has<ecs::Light>() && !e.has<ecs::Camera>() && !e.has<ecs::Decal>()) return;
        float sx = 0.0f;
        float sy = 0.0f;
        if (worldToScreen(e.worldPosition(), sx, sy) && std::hypot(sx - x, sy - y) < 12.0f) {
            icon_hit = e.uuid();
        }
    });
    if (icon_hit.valid()) {
        choose(icon_hit);
        return;
    }

    Vec3 origin{};
    Vec3 direction{};
    if (!mouseRay(x, y, origin, direction) || !sync_) return;
    float best = std::numeric_limits<float>::max();
    ecs::Entity hit;
    for (std::uint32_t i = 0; i < scene_.actors().size(); ++i) {
        const ecs::Entity owner = sync_->entityForActor(world_, i);
        if (!owner.valid() || !owner.activeInHierarchy()) continue;
        Vec3 mn{};
        Vec3 mx{};
        if (!sync_->actorLocalBounds(i, mn, mx)) continue;
        const core::Aabb box = core::transformAabb(owner.worldMatrix(), core::Aabb{mn, mx});
        const float t = rayBox(origin, direction, box);
        if (t >= 0.0f && t < best) {
            best = t;
            hit = owner;
        }
    }
    if (!hit.valid()) {
        if (!additive) clearSelection();
        last_pick_x_ = last_pick_y_ = -1.0f;
        return;
    }

    // Cadena raiz -> tocado.
    std::vector<ecs::Entity> chain;
    for (ecs::Entity e = hit; e.valid(); e = e.parent()) chain.insert(chain.begin(), e);
    std::size_t level = 0;
    const bool same_spot = std::hypot(x - last_pick_x_, y - last_pick_y_) < 4.0f;
    if (same_spot && active_.valid()) {
        for (std::size_t i = 0; i < chain.size(); ++i) {
            if (chain[i].uuid() == active_) {
                level = std::min(i + 1, chain.size() - 1);
                break;
            }
        }
    }
    last_pick_x_ = x;
    last_pick_y_ = y;
    choose(chain[level].uuid());
}

// Iconos de luces y camaras (y la direccion de la seleccionada).
void EditorApp::drawSceneOverlays() {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->PushClipRect(ImVec2(view_x_, view_y_), ImVec2(view_x_ + view_w_, view_y_ + view_h_), true);
    // Solo las entidades con luz o camara (vistas de EnTT), no todo el mundo.
    const auto overlay = [&](ecs::Entity e) {
        if (!e.activeInHierarchy()) return;
        const ecs::Light* light = e.tryGet<ecs::Light>();
        const bool camera = e.has<ecs::Camera>();
        if (light == nullptr && !camera) return;
        float x = 0.0f;
        float y = 0.0f;
        if (!worldToScreen(e.worldPosition(), x, y)) return;
        const bool selected = isSelected(e.uuid());
        const ImU32 color = camera ? IM_COL32(120, 220, 140, 235)
                                   : IM_COL32(static_cast<int>(std::min(light->color.x, 1.0f) * 200 + 55),
                                              static_cast<int>(std::min(light->color.y, 1.0f) * 200 + 55),
                                              static_cast<int>(std::min(light->color.z, 1.0f) * 200 + 55), 235);
        if (camera) {
            draw->AddRectFilled(ImVec2(x - 7, y - 5), ImVec2(x + 5, y + 5), color, 2.0f);
            draw->AddTriangleFilled(ImVec2(x + 5, y), ImVec2(x + 9, y - 4), ImVec2(x + 9, y + 4), color);
        } else {
            draw->AddCircleFilled(ImVec2(x, y), selected ? 7.0f : 5.5f, color);
        }
        draw->AddCircle(ImVec2(x, y), selected ? 10.0f : 8.0f, IM_COL32(0, 0, 0, 170), 0, 1.5f);
        // Direccion de focos, direccionales y camaras seleccionados.
        if (selected && (camera || (light != nullptr && light->type != ecs::LightType::Point))) {
            float x1 = 0.0f;
            float y1 = 0.0f;
            if (worldToScreen(e.worldPosition() + e.forward() * 1.5f, x1, y1)) {
                draw->AddLine(ImVec2(x, y), ImVec2(x1, y1), color, 2.0f);
            }
        }
    };
    for (const entt::entity handle : world_.registry().view<ecs::Decal>()) {
        const ecs::Entity e = world_.wrap(handle);
        if (!e.activeInHierarchy()) continue;
        float x = 0.0f;
        float y = 0.0f;
        if (!worldToScreen(e.worldPosition(), x, y)) continue;
        const bool selected = isSelected(e.uuid());
        const ImU32 color = IM_COL32(255, 170, 60, 235);
        const float s = selected ? 7.0f : 5.5f;
        draw->AddQuadFilled(ImVec2(x, y - s), ImVec2(x + s, y), ImVec2(x, y + s), ImVec2(x - s, y), color);
        draw->AddQuad(ImVec2(x, y - s - 2), ImVec2(x + s + 2, y), ImVec2(x, y + s + 2), ImVec2(x - s - 2, y),
                      IM_COL32(0, 0, 0, 170), 1.5f);
    }
    for (const entt::entity handle : world_.registry().view<ecs::Light>()) overlay(world_.wrap(handle));
    for (const entt::entity handle : world_.registry().view<ecs::Camera>()) {
        if (!world_.registry().all_of<ecs::Light>(handle)) overlay(world_.wrap(handle));
    }
    draw->PopClipRect();
}

namespace {

// Punto de la recta A (a + s*da) mas cercano a la recta B (b + t*db): devuelve s.
float closestOnLine(const Vec3& a, const Vec3& da, const Vec3& b, const Vec3& db) {
    const Vec3 w = a - b;
    const float aa = core::dot(da, da);
    const float ab = core::dot(da, db);
    const float bb = core::dot(db, db);
    const float d = core::dot(da, w);
    const float e = core::dot(db, w);
    const float denom = aa * bb - ab * ab;
    if (std::abs(denom) < 1e-8f) return 0.0f;
    return (ab * e - bb * d) / denom;
}

}  // namespace

bool EditorApp::drawLightGizmos() {
    ecs::Entity e = world_.find(active_);
    ecs::Light* light = e.valid() ? e.tryGet<ecs::Light>() : nullptr;
    if (light == nullptr || flying_) {
        light_handle_drag_ = 0;
        return false;
    }
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->PushClipRect(ImVec2(view_x_, view_y_), ImVec2(view_x_ + view_w_, view_y_ + view_h_), true);
    const ImU32 wire = IM_COL32(255, 235, 140, 200);
    const ImU32 dim = IM_COL32(255, 235, 140, 90);
    const Vec3 center = e.worldPosition();
    const Vec3 forward = core::normalize(e.forward());
    const Vec3 right = core::normalize(e.right());
    const Vec3 up = core::normalize(e.up());

    // Polilinea 3D (solo los tramos que quedan delante de la camara).
    const auto circle = [&](const Vec3& c, const Vec3& u, const Vec3& v, float radius, ImU32 color) {
        constexpr int kSegments = 64;
        float px = 0.0f, py = 0.0f;
        bool prev = false;
        for (int i = 0; i <= kSegments; ++i) {
            const float a = static_cast<float>(i) / kSegments * 2.0f * core::kPi;
            const Vec3 p = c + u * (std::cos(a) * radius) + v * (std::sin(a) * radius);
            float x = 0.0f, y = 0.0f;
            const bool ok = worldToScreen(p, x, y);
            if (ok && prev) draw->AddLine(ImVec2(px, py), ImVec2(x, y), color, 1.5f);
            px = x;
            py = y;
            prev = ok;
        }
    };
    const auto line = [&](const Vec3& a, const Vec3& b, ImU32 color, float thickness) {
        float ax = 0.0f, ay = 0.0f, bx = 0.0f, by = 0.0f;
        if (worldToScreen(a, ax, ay) && worldToScreen(b, bx, by)) {
            draw->AddLine(ImVec2(ax, ay), ImVec2(bx, by), color, thickness);
        }
    };

    // Asas: cuadraditos que se arrastran; devuelve si el raton esta encima.
    ImGuiIO& io = ImGui::GetIO();
    bool hovered_any = false;
    const auto handle = [&](const Vec3& p, int id) {
        float x = 0.0f, y = 0.0f;
        if (!worldToScreen(p, x, y)) return;
        const bool over = std::abs(io.MousePos.x - x) < 7.0f && std::abs(io.MousePos.y - y) < 7.0f;
        const bool active = light_handle_drag_ == id;
        hovered_any = hovered_any || over || active;
        draw->AddRectFilled(ImVec2(x - 4.5f, y - 4.5f), ImVec2(x + 4.5f, y + 4.5f),
                            active || over ? IM_COL32(255, 255, 255, 255) : IM_COL32(255, 220, 90, 255));
        draw->AddRect(ImVec2(x - 4.5f, y - 4.5f), ImVec2(x + 4.5f, y + 4.5f), IM_COL32(0, 0, 0, 200));
        if (over && view_hovered_ && light_handle_drag_ == 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
            !ImGuizmo::IsOver()) {
            light_handle_drag_ = id;
        }
    };

    const float kDeg = core::kPi / 180.0f;
    if (light->type == ecs::LightType::Point) {
        circle(center, Vec3{1, 0, 0}, Vec3{0, 1, 0}, light->range, wire);
        circle(center, Vec3{0, 1, 0}, Vec3{0, 0, 1}, light->range, wire);
        circle(center, Vec3{1, 0, 0}, Vec3{0, 0, 1}, light->range, wire);
        // Contorno de cara a la camara.
        const core::Mat4 view = scene_.camera().view();
        const Vec3 cam_right = core::normalize(Vec3{view.m[0][0], view.m[1][0], view.m[2][0]});
        const Vec3 cam_up = core::normalize(Vec3{view.m[0][1], view.m[1][1], view.m[2][1]});
        circle(center, cam_right, cam_up, light->range, dim);
        handle(center + cam_right * light->range, 1);
        handle(center - cam_right * light->range, 1);
        handle(center + cam_up * light->range, 1);
        handle(center - cam_up * light->range, 1);
    } else if (light->type == ecs::LightType::Spot) {
        const Vec3 end = center + forward * light->range;
        const float outer = light->range * std::tan(std::clamp(light->outer_angle, 1.0f, 89.0f) * kDeg);
        const float inner = light->range * std::tan(std::clamp(light->inner_angle, 0.5f, 89.0f) * kDeg);
        circle(end, right, up, outer, wire);
        circle(end, right, up, inner, dim);
        for (const Vec3& dir : {right, right * -1.0f, up, up * -1.0f}) {
            line(center, end + dir * outer, wire, 1.5f);
        }
        line(center, end, dim, 1.0f);
        handle(end, 1);
        handle(end + up * outer, 2);
        handle(end + right * inner, 3);
    } else {
        // Direccional: haz de rayos paralelos en la direccion de la luz.
        const float r = 0.35f;
        for (int i = 0; i < 8; ++i) {
            const float a = static_cast<float>(i) / 8.0f * 2.0f * core::kPi;
            const Vec3 o = center + right * (std::cos(a) * r) + up * (std::sin(a) * r);
            line(o, o + forward * 2.0f, wire, 1.5f);
        }
        circle(center, right, up, r, wire);
    }

    // Arrastre de un asa.
    if (light_handle_drag_ != 0) {
        Vec3 origin{};
        Vec3 direction{};
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && mouseRay(io.MousePos.x, io.MousePos.y, origin, direction)) {
            if (light_handle_drag_ == 1 && light->type == ecs::LightType::Point) {
                // Distancia del centro al punto del rayo mas cercano.
                const float t = core::dot(center - origin, direction) / std::max(core::dot(direction, direction), 1e-8f);
                light->range = std::max(0.05f, core::length(origin + direction * t - center));
            } else if (light_handle_drag_ == 1) {
                light->range = std::max(0.05f, closestOnLine(center, forward, origin, direction));
            } else {
                // Radio en el plano del final del cono -> angulo.
                const Vec3 end = center + forward * light->range;
                const Vec3 axis = light_handle_drag_ == 2 ? up : right;
                const float s = std::abs(closestOnLine(end, axis, origin, direction));
                const float angle = std::atan2(s, light->range) / kDeg;
                if (light_handle_drag_ == 2) {
                    light->outer_angle = std::clamp(angle, 1.0f, 89.0f);
                    light->inner_angle = std::min(light->inner_angle, light->outer_angle);
                } else {
                    light->inner_angle = std::clamp(angle, 0.5f, light->outer_angle);
                }
            }
            dirty_ = true;
        } else {
            light_handle_drag_ = 0;
            commit();  // un paso de deshacer por arrastre
        }
    }
    draw->PopClipRect();
    return hovered_any;
}

// Gizmo de mover/rotar/escalar sobre la seleccion activa; el cambio se
// aplica tambien al resto de la seleccion (misma transformacion en el mundo).
void EditorApp::drawGizmo() {
    ecs::Entity target = world_.find(active_);
    if (!target.valid() || gizmo_ == GizmoOperation::None || flying_) {
        if (gizmo_was_using_) commit();
        gizmo_was_using_ = false;
        return;
    }
    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetDrawlist(ImGui::GetWindowDrawList());
    ImGuizmo::SetRect(view_x_, view_y_, view_w_, view_h_);

    // La proyeccion del motor es de Vulkan (Y invertida en m[1][1]); ImGuizmo
    // espera la de OpenGL: se deshace la inversion para que no salga espejado.
    const Mat4 view = scene_.camera().view();
    Mat4 projection = scene_.camera().projection();
    projection.m[1][1] = -projection.m[1][1];

    ImGuizmo::OPERATION operation = ImGuizmo::TRANSLATE;
    float snap[3] = {snap_translate_, snap_translate_, snap_translate_};
    if (gizmo_ == GizmoOperation::Rotate) {
        operation = ImGuizmo::ROTATE;
        snap[0] = snap[1] = snap[2] = snap_rotate_;
    } else if (gizmo_ == GizmoOperation::Scale) {
        operation = ImGuizmo::SCALE;
        snap[0] = snap[1] = snap[2] = snap_scale_;
    }
    // La escala solo tiene sentido en local (ImGuizmo lo fuerza igual).
    const ImGuizmo::MODE mode =
        (gizmo_local_ || gizmo_ == GizmoOperation::Scale) ? ImGuizmo::LOCAL : ImGuizmo::WORLD;
    const bool snapping = snap_enabled_ != ImGui::GetIO().KeyCtrl;

    const Mat4 before = target.worldMatrix();
    Mat4 matrix = before;
    if (ImGuizmo::Manipulate(&view.m[0][0], &projection.m[0][0], operation, mode, &matrix.m[0][0],
                             nullptr, snapping ? snap : nullptr)) {
        target.setWorldMatrix(matrix);
        // El resto de la seleccion: la misma transformacion relativa.
        const Mat4 delta = matrix * core::inverse(before);
        for (ecs::Entity other : topLevelSelection()) {
            if (other == target || other.isAncestorOf(target) || target.isAncestorOf(other)) continue;
            other.setWorldMatrix(delta * other.worldMatrix());
        }
        dirty_ = true;
    }
    const bool using_now = ImGuizmo::IsUsing();
    if (gizmo_was_using_ && !using_now) {
        commit();  // se solto el gizmo: un paso de deshacer
    }
    gizmo_was_using_ = using_now;
}

}  // namespace cramion::editor
