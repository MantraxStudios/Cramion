// Vista Juego y cinematicas en el editor:
//
//   - Vista Juego: la escena desde la camara real (la del componente Camera,
//     movida por el Camera Brain), separada de la Escena. El renderizador
//     dibuja una vista por frame (la visible; si se ven las dos, aquella con
//     la que se trabaja) y la copia a su imagen; la otra conserva su ultimo
//     frame.
//   - Gizmos: rieles (curva y puntos que se mueven con el gizmo de mover),
//     frustums de las camaras virtuales y de la real, lineas a Follow/LookAt,
//     carros.
//   - Ventana Cinematica (como el Timeline de Unity): planos de una Cinematic
//     Sequence como barras (mover, alargar, mezcla de entrada), objetos
//     activos por tramos y el cabezal, con vista previa fuera de Play.

#include "EditorApp.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <ImGuizmo.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace cramion::editor {

using core::Mat4;
using core::Quat;
using core::Vec3;

namespace {

constexpr ImU32 kTrackColor = IM_COL32(255, 196, 64, 255);
constexpr ImU32 kVcamColor = IM_COL32(120, 190, 255, 255);
constexpr ImU32 kLiveColor = IM_COL32(255, 90, 90, 255);
constexpr ImU32 kBrainColor = IM_COL32(235, 235, 235, 255);
constexpr ImU32 kTargetLine = IM_COL32(255, 255, 255, 110);

Quat worldRotation(const ecs::Entity& e) {
    Vec3 p{};
    Quat q{};
    Vec3 s{};
    ecs::decomposeMatrix(e.worldMatrix(), p, q, s);
    return core::normalize(q);
}

// Color estable por camara (barras del Timeline).
ImU32 shotColor(const Uuid& uuid, float alpha = 1.0f) {
    const std::size_t h = std::hash<Uuid>{}(uuid);
    const float hue = static_cast<float>(h % 360) / 360.0f;
    ImVec4 c;
    ImGui::ColorConvertHSVtoRGB(hue, 0.45f, 0.75f, c.x, c.y, c.z);
    return IM_COL32(static_cast<int>(c.x * 255), static_cast<int>(c.y * 255), static_cast<int>(c.z * 255),
                    static_cast<int>(alpha * 255));
}

std::string nameOf(const ecs::World& world, const Uuid& uuid, const char* none = "(ninguno)") {
    const ecs::Entity e = world.find(uuid);
    return e.valid() ? e.name() : std::string(none);
}

}  // namespace

// -----------------------------------------------------------------------------
// Que vista se dibuja
// -----------------------------------------------------------------------------

// Al final de la interfaz (ya se sabe que ventanas se ven): la vista que se
// dibuja en este frame.
void EditorApp::chooseRenderView() {
    if (scene_view_visible_ && game_view_visible_) {
        render_view_ = preferred_view_;
    } else if (game_view_visible_) {
        render_view_ = kGameSlot;
    } else {
        render_view_ = kSceneSlot;
    }
}

void EditorApp::afterRender() {
    if (saved_camera_) {
        scene_.camera() = *saved_camera_;
        saved_camera_.reset();
    }
}

void EditorApp::updateCinematics(float delta_seconds) {
    const bool running = play_state_ == PlayState::Playing;
    float dt = play_state_ == PlayState::Paused ? 0.0f : delta_seconds;
    // Vista previa del Timeline fuera de Play.
    const ecs::Entity sequence = world_.find(timeline_sequence_);
    if (!playing()) {
        if (timeline_preview_ && sequence.valid() && sequence.has<cinema::CinematicSequence>()) {
            if (timeline_playing_) {
                timeline_time_ = cinematics_.previewSequence() == sequence ? cinematics_.time(sequence) : timeline_time_;
                if (!cinematics_.isPlaying(sequence) && cinematics_.previewSequence() == sequence) {
                    timeline_playing_ = false;  // llego al final
                }
            }
            cinematics_.setPreview(sequence, timeline_time_, timeline_playing_);
        } else {
            cinematics_.clearPreview();
        }
    } else {
        cinematics_.clearPreview();
    }
    cinematics_.update(world_, dt, running || (playing() && dt > 0.0f));
    if (!playing() && timeline_preview_ && timeline_playing_ && sequence.valid()) {
        timeline_time_ = cinematics_.time(sequence);
    }
}

// -----------------------------------------------------------------------------
// Vista Juego
// -----------------------------------------------------------------------------

void EditorApp::drawGameView() {
    if (!show_game_) {
        game_view_visible_ = false;
        return;
    }
    // La primera vez, como pestaña junto a la Escena (como Unity).
    if (scene_dock_id_ != 0) ImGui::SetNextWindowDockID(scene_dock_id_, ImGuiCond_FirstUseEver);
    if (focus_game_) {
        ImGui::SetNextWindowFocus();
        focus_game_ = false;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    const bool open = ImGui::Begin("Juego", &show_game_, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    game_view_visible_ = open && show_game_;
    if (!open) {
        ImGui::End();
        return;
    }
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) ||
        (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))) {
        preferred_view_ = kGameSlot;
    }

    // Barra: que camara se ve.
    ImGui::SetCursorPos(ImVec2(8.0f, ImGui::GetCursorPosY() + 4.0f));
    const ecs::Entity brain = cinematics_.brain();
    ecs::Entity main_camera = brain;
    if (!main_camera.valid()) {
        for (const entt::entity h : world_.registry().view<ecs::Camera>()) {
            const ecs::Entity e = world_.wrap(h);
            if (e.activeInHierarchy() && (!main_camera.valid() || e.get<ecs::Camera>().is_main)) main_camera = e;
        }
    }
    if (main_camera.valid()) {
        ImGui::Text("Cámara: %s", main_camera.name().c_str());
        const ecs::Entity live = cinematics_.liveCamera();
        if (brain.valid() && live.valid()) {
            ImGui::SameLine();
            ImGui::TextDisabled("|");
            ImGui::SameLine();
            if (cinematics_.blending() && cinematics_.previousCamera().valid()) {
                ImGui::Text("Mezclando %s -> %s (%.0f %%)", cinematics_.previousCamera().name().c_str(),
                            live.name().c_str(), cinematics_.blendProgress() * 100.0f);
            } else {
                ImGui::Text("Virtual: %s%s", live.name().c_str(), cinematics_.solo().valid() ? " (solo)" : "");
            }
        }
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "No hay ninguna Camera en la escena");
    }
    ImGui::SameLine(std::max(ImGui::GetWindowWidth() - 110.0f, 0.0f));
    ImGui::Checkbox("Tercios", &game_guides_);

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
    ImGui::Image(imgui_.viewTexture(kGameSlot), size);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    if (game_guides_) {
        for (int i = 1; i <= 2; ++i) {
            const float x = origin.x + size.x * static_cast<float>(i) / 3.0f;
            const float y = origin.y + size.y * static_cast<float>(i) / 3.0f;
            draw->AddLine(ImVec2(x, origin.y), ImVec2(x, origin.y + size.y), IM_COL32(255, 255, 255, 70));
            draw->AddLine(ImVec2(origin.x, y), ImVec2(origin.x + size.x, y), IM_COL32(255, 255, 255, 70));
        }
    }
    if (playing()) {
        const ImU32 frame = play_state_ == PlayState::Paused ? IM_COL32(255, 190, 60, 220) : IM_COL32(80, 170, 255, 220);
        draw->AddRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), frame, 0.0f, 0, 3.0f);
    }
    if (render_view_ != kGameSlot) {
        draw->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + 24.0f), IM_COL32(0, 0, 0, 150));
        draw->AddText(ImVec2(origin.x + 8.0f, origin.y + 4.0f), IM_COL32(255, 220, 140, 255),
                      "En pausa: se esta dibujando la Escena (clic aqui para ver el Juego)");
    }
    ImGui::End();
}

// -----------------------------------------------------------------------------
// Gizmos
// -----------------------------------------------------------------------------

void EditorApp::drawCameraFrustum(const Vec3& position, const Quat& rotation, float fov_degrees, float depth,
                                  std::uint32_t color) {
    const vk::Extent2D extent = renderer_.sceneExtent();
    const float aspect = extent.height > 0 ? static_cast<float>(extent.width) / static_cast<float>(extent.height) : 1.0f;
    const Vec3 forward = ecs::quatRotate(rotation, Vec3{0.0f, 0.0f, -1.0f});
    const Vec3 up = ecs::quatRotate(rotation, Vec3{0.0f, 1.0f, 0.0f});
    const Vec3 right = ecs::quatRotate(rotation, Vec3{1.0f, 0.0f, 0.0f});
    const float h = std::tan(core::radians(std::clamp(fov_degrees, 1.0f, 170.0f)) * 0.5f) * depth;
    const float w = h * aspect;
    const Vec3 c = position + forward * depth;
    const Vec3 corners[4] = {c + right * w + up * h, c - right * w + up * h, c - right * w - up * h,
                             c + right * w - up * h};
    for (int i = 0; i < 4; ++i) {
        overlayLine(position, corners[i], color);
        overlayLine(corners[i], corners[(i + 1) % 4], color);
    }
    // Triangulo encima: donde esta "arriba" (como Unity).
    const Vec3 top = c + up * (h * 1.35f);
    overlayTriangle(c - right * (w * 0.3f) + up * (h * 1.05f), c + right * (w * 0.3f) + up * (h * 1.05f), top, color);
}

bool EditorApp::drawCinematicGizmos() {
    if (!has_project_) return false;
    const ecs::Entity active = world_.find(active_);
    bool hovered_any = false;

    // --- Rieles ---
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->PushClipRect(ImVec2(view_x_, view_y_), ImVec2(view_x_ + view_w_, view_y_ + view_h_), true);
    ImGuiIO& io = ImGui::GetIO();
    const bool gizmo_busy = ImGuizmo::IsOver() || ImGuizmo::IsUsing() || flying_;
    for (const entt::entity h : world_.registry().view<cinema::DollyTrack>()) {
        ecs::Entity e = world_.wrap(h);
        if (!e.activeInHierarchy()) continue;
        cinema::DollyTrack& track = e.get<cinema::DollyTrack>();
        const Mat4 track_world = e.worldMatrix();
        const cinema::DollyPath path(track, track_world);
        const bool selected = isSelected(e.uuid());
        const bool editable = selected && e == active;
        const ImU32 color = selected ? kTrackColor : IM_COL32(255, 196, 64, 150);
        const auto& samples = path.samples();
        for (std::size_t i = 1; i < samples.size(); ++i) overlayLine(samples[i - 1], samples[i], color);
        if (!selected || !path.valid()) continue;
        // Traviesas cada metro (se ve el sentido y la escala del riel).
        const int ties = std::min(static_cast<int>(path.length()), 400);
        for (int i = 0; i <= ties; ++i) {
            const float u = path.toPathUnits(static_cast<float>(i), cinema::PositionUnits::Distance);
            const Vec3 p = path.point(u);
            const Vec3 side = core::normalize(core::cross(path.tangent(u), Vec3{0.0f, 1.0f, 0.0f})) * 0.15f;
            overlayLine(p - side, p + side, IM_COL32(255, 196, 64, 110));
        }
        if (!editable) continue;

        // Asas en pantalla: puntos (circulos grandes) y, en Bezier, tangentes.
        const bool bezier = track.mode == cinema::PathMode::Bezier;
        const auto& points = path.points();
        struct Handle {
            int kind;  // 1 punto, 2 salida, 3 entrada
            int index;
            ImVec2 screen;
        };
        std::vector<Handle> handles;
        for (std::size_t i = 0; i < points.size(); ++i) {
            float x = 0.0f;
            float y = 0.0f;
            if (bezier) {
                const Vec3 tangent = path.handle(i);
                const Vec3 out = points[i] + tangent;
                const Vec3 in = points[i] - tangent;
                overlayLine(in, out, IM_COL32(120, 200, 255, 200));
                if (worldToScreen(out, x, y)) handles.push_back(Handle{2, static_cast<int>(i), ImVec2(x, y)});
                if (worldToScreen(in, x, y)) handles.push_back(Handle{3, static_cast<int>(i), ImVec2(x, y)});
            }
            if (worldToScreen(points[i], x, y)) handles.push_back(Handle{1, static_cast<int>(i), ImVec2(x, y)});
        }
        const Handle* hovered = nullptr;
        for (const Handle& handle : handles) {
            const float radius = handle.kind == 1 ? 9.0f : 7.0f;
            if (std::hypot(io.MousePos.x - handle.screen.x, io.MousePos.y - handle.screen.y) <= radius) hovered = &handle;
        }
        for (const Handle& handle : handles) {
            const bool is_selected = handle.kind == 1 && handle.index == selected_waypoint_;
            const bool active_drag = waypoint_drag_ == handle.kind && waypoint_drag_index_ == handle.index;
            const bool over = hovered == &handle || active_drag;
            if (handle.kind == 1) {
                draw->AddCircleFilled(handle.screen, 7.0f, is_selected ? IM_COL32(255, 255, 255, 255)
                                                        : (over ? IM_COL32(255, 230, 150, 255) : kTrackColor));
                draw->AddCircle(handle.screen, 7.0f, IM_COL32(0, 0, 0, 220), 0, 1.5f);
                char label[16];
                std::snprintf(label, sizeof(label), "%d", handle.index);
                draw->AddText(ImVec2(handle.screen.x + 9.0f, handle.screen.y - 20.0f), IM_COL32(255, 230, 160, 230), label);
            } else {
                draw->AddCircleFilled(handle.screen, 5.0f, over ? IM_COL32(255, 255, 255, 255) : IM_COL32(120, 200, 255, 255));
                draw->AddCircle(handle.screen, 5.0f, IM_COL32(0, 0, 0, 220), 0, 1.5f);
            }
        }

        // Cerca de la curva: doble clic inserta un punto ahi.
        float curve_units = -1.0f;
        {
            float best = 10.0f;
            for (std::size_t i = 0; i < samples.size(); ++i) {
                float x = 0.0f;
                float y = 0.0f;
                if (!worldToScreen(samples[i], x, y)) continue;
                const float d = std::hypot(io.MousePos.x - x, io.MousePos.y - y);
                if (d < best) {
                    best = d;
                    curve_units = static_cast<float>(i) / static_cast<float>(std::max(track.resolution, 2));
                }
            }
        }
        if (view_hovered_ && (hovered != nullptr || curve_units >= 0.0f || io.KeyShift)) hovered_any = true;

        // Empezar a arrastrar / seleccionar.
        if (view_hovered_ && !gizmo_busy && waypoint_drag_ == 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            if (hovered != nullptr) {
                waypoint_drag_ = hovered->kind;
                waypoint_drag_index_ = hovered->index;
                waypoint_track_ = e.uuid();
                if (hovered->kind == 1) selected_waypoint_ = hovered->index;
                waypoint_drag_height_ = points[static_cast<std::size_t>(hovered->index)].y;
            } else if (io.KeyShift && curve_units < 0.0f) {
                // Mayus + clic en vacio: punto nuevo al final, donde apunta el
                // raton a la altura del ultimo punto.
                Vec3 origin{};
                Vec3 direction{};
                const Vec3 last = points.back();
                if (mouseRay(io.MousePos.x, io.MousePos.y, origin, direction) && std::abs(direction.y) > 1e-4f) {
                    const float t = (last.y - origin.y) / direction.y;
                    if (t > 0.0f) {
                        cinema::DollyWaypoint w{};
                        w.position = ecs::transformPoint(core::inverse(track_world), origin + direction * t);
                        track.waypoints.push_back(w);
                        selected_waypoint_ = static_cast<int>(track.waypoints.size()) - 1;
                        waypoint_track_ = e.uuid();
                        commit();
                    }
                }
            }
        }
        if (view_hovered_ && !gizmo_busy && hovered == nullptr && curve_units >= 0.0f &&
            ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            // Doble clic en la curva: punto nuevo en ese sitio, entre sus dos vecinos.
            const float u = path.wrap(curve_units);
            const auto after = static_cast<std::size_t>(std::floor(u));
            cinema::DollyWaypoint w{};
            w.position = ecs::transformPoint(core::inverse(track_world), path.point(u));
            const std::size_t at = std::min(after + 1, track.waypoints.size());
            track.waypoints.insert(track.waypoints.begin() + static_cast<std::ptrdiff_t>(at), w);
            selected_waypoint_ = static_cast<int>(at);
            waypoint_track_ = e.uuid();
            waypoint_drag_ = 0;
            commit();
        }

        // Arrastre.
        if (waypoint_drag_ != 0 && waypoint_track_ == e.uuid() &&
            waypoint_drag_index_ < static_cast<int>(track.waypoints.size())) {
            Vec3 origin{};
            Vec3 direction{};
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                waypoint_drag_ = 0;
                commit();
            } else if (mouseRay(io.MousePos.x, io.MousePos.y, origin, direction)) {
                cinema::DollyWaypoint& w = track.waypoints[static_cast<std::size_t>(waypoint_drag_index_)];
                const Vec3 point = points[static_cast<std::size_t>(waypoint_drag_index_)];
                const Mat4 inverse = core::inverse(track_world);
                if (waypoint_drag_ == 1) {
                    Vec3 target = point;
                    if (io.KeyShift) {
                        // Mayus: solo en vertical (el punto de la recta vertical mas cercano al rayo).
                        const Vec3 up{0.0f, 1.0f, 0.0f};
                        const Vec3 w0 = point - origin;
                        const float b = core::dot(up, direction);
                        const float denom = 1.0f - b * b;
                        if (denom > 1e-6f) {
                            const float s_up = (b * core::dot(direction, w0) - core::dot(up, w0)) / denom;
                            target = point + up * s_up;
                            waypoint_drag_height_ = target.y;
                        }
                    } else if (std::abs(direction.y) > 1e-4f) {
                        // En el plano horizontal a su altura.
                        const float t = (waypoint_drag_height_ - origin.y) / direction.y;
                        if (t > 0.0f) target = origin + direction * t;
                    }
                    w.position = ecs::transformPoint(inverse, target);
                } else {
                    // Asa: en el plano de la vista que pasa por el punto.
                    const Vec3 normal = scene_.camera().forward();
                    const float denom = core::dot(normal, direction);
                    if (std::abs(denom) > 1e-5f) {
                        const float t = core::dot(normal, point - origin) / denom;
                        Vec3 tangent = origin + direction * t - point;
                        if (waypoint_drag_ == 3) tangent = -tangent;
                        w.tangent = ecs::transformDirection(inverse, tangent);
                    }
                }
                dirty_ = true;
            }
        }
    }
    draw->PopClipRect();
    // Al cambiar de seleccion se deja de editar el punto.
    if (!active.valid() || !(active.uuid() == waypoint_track_) || !active.has<cinema::DollyTrack>()) {
        selected_waypoint_ = -1;
    }

    // --- Camaras virtuales, camara real y objetivos ---
    const ecs::Entity live = cinematics_.liveCamera();
    for (const entt::entity h : world_.registry().view<cinema::VirtualCamera>()) {
        const ecs::Entity e = world_.wrap(h);
        if (!e.activeInHierarchy()) continue;
        const cinema::VirtualCamera& vcam = e.get<cinema::VirtualCamera>();
        const bool selected = isSelected(e.uuid());
        const ImU32 color = e == live ? kLiveColor : kVcamColor;
        drawCameraFrustum(e.worldPosition(), worldRotation(e), vcam.fov, selected ? 1.2f : 0.7f,
                          selected ? color : (color & 0x00FFFFFFu) | 0xB0000000u);
        if (selected) {
            const ecs::Entity follow = world_.find(vcam.follow);
            const ecs::Entity look = world_.find(vcam.look_at);
            if (follow.valid()) overlayLine(e.worldPosition(), follow.worldPosition(), kTargetLine);
            if (look.valid()) {
                const Vec3 target = look.worldPosition() + vcam.look_offset;
                overlayLine(e.worldPosition(), target, kLiveColor & 0x90FFFFFFu);
                overlayScreenDisc(target, gizmoWorldSize(target) * 0.03f, kLiveColor);
            }
        }
    }
    if (const ecs::Entity brain = cinematics_.brain(); brain.valid()) {
        drawCameraFrustum(brain.worldPosition(), worldRotation(brain), brain.get<ecs::Camera>().fov,
                          isSelected(brain.uuid()) ? 1.5f : 1.0f, kBrainColor);
    }
    for (const entt::entity h : world_.registry().view<cinema::DollyCart>()) {
        const ecs::Entity e = world_.wrap(h);
        if (!e.activeInHierarchy()) continue;
        overlayBoxEdges(core::composeTrs(e.worldPosition(), worldRotation(e), Vec3{0.4f, 0.25f, 0.6f}), kTrackColor);
    }
    return hovered_any;
}

// El punto seleccionado de un riel se mueve con el gizmo de mover (en vez de
// la entidad). true si se esta editando un punto.
bool EditorApp::deleteSelectedWaypoint() {
    ecs::Entity track_entity = world_.find(active_);
    cinema::DollyTrack* track = track_entity.valid() && track_entity.uuid() == waypoint_track_
                                    ? track_entity.tryGet<cinema::DollyTrack>()
                                    : nullptr;
    if (track == nullptr || selected_waypoint_ < 0 || selected_waypoint_ >= static_cast<int>(track->waypoints.size()) ||
        track->waypoints.size() <= 2) {
        return false;
    }
    track->waypoints.erase(track->waypoints.begin() + selected_waypoint_);
    selected_waypoint_ = std::min(selected_waypoint_, static_cast<int>(track->waypoints.size()) - 1);
    commit();
    return true;
}

bool EditorApp::drawWaypointGizmo(const Mat4& view, const Mat4& projection) {
    ecs::Entity track_entity = world_.find(active_);
    if (selected_waypoint_ < 0 || !track_entity.valid() || !(track_entity.uuid() == waypoint_track_)) return false;
    cinema::DollyTrack* track = track_entity.tryGet<cinema::DollyTrack>();
    if (track == nullptr || selected_waypoint_ >= static_cast<int>(track->waypoints.size())) {
        selected_waypoint_ = -1;
        return false;
    }
    cinema::DollyWaypoint& waypoint = track->waypoints[static_cast<std::size_t>(selected_waypoint_)];
    const Mat4 track_world = track_entity.worldMatrix();
    Mat4 matrix = core::translate(ecs::transformPoint(track_world, waypoint.position));
    float snap[3] = {snap_translate_, snap_translate_, snap_translate_};
    const bool snapping = snap_enabled_ != ImGui::GetIO().KeyCtrl;
    ImGuizmo::PushID(0x57A7);
    if (ImGuizmo::Manipulate(&view.m[0][0], &projection.m[0][0], ImGuizmo::TRANSLATE, ImGuizmo::WORLD, &matrix.m[0][0],
                             nullptr, snapping ? snap : nullptr)) {
        waypoint.position = ecs::transformPoint(core::inverse(track_world), Vec3{matrix.m[3][0], matrix.m[3][1], matrix.m[3][2]});
        dirty_ = true;
    }
    const bool using_now = ImGuizmo::IsUsing();
    ImGuizmo::PopID();
    drawGizmoGeometry(matrix, false, 0);
    if (waypoint_gizmo_using_ && !using_now) commit();
    waypoint_gizmo_using_ = using_now;
    return true;
}

// -----------------------------------------------------------------------------
// Crear
// -----------------------------------------------------------------------------

// La camara real con su Camera Brain (se crea si no hay), como hace
// Cinemachine al crear la primera camara virtual.
ecs::Entity EditorApp::ensureCameraBrain() {
    ecs::Entity camera;
    for (const entt::entity h : world_.registry().view<ecs::Camera>()) {
        const ecs::Entity e = world_.wrap(h);
        if (!camera.valid() || (e.get<ecs::Camera>().is_main && !camera.get<ecs::Camera>().is_main)) camera = e;
    }
    if (!camera.valid()) {
        camera = ecs::createCamera(world_, {});
        camera.setWorldPosition(scene_.camera().position());
    }
    if (!camera.has<cinema::CameraBrain>()) camera.add<cinema::CameraBrain>();
    return camera;
}

void EditorApp::alignWithView(ecs::Entity entity) {
    const scene::Camera& camera = scene_.camera();
    const Vec3 forward = camera.forward();
    const Vec3 up = camera.up();
    const Vec3 right = core::normalize(core::cross(forward, up));
    Mat4 basis = Mat4::identity();
    const Vec3 true_up = core::cross(right, forward);
    basis.m[0][0] = right.x; basis.m[0][1] = right.y; basis.m[0][2] = right.z;
    basis.m[1][0] = true_up.x; basis.m[1][1] = true_up.y; basis.m[1][2] = true_up.z;
    basis.m[2][0] = -forward.x; basis.m[2][1] = -forward.y; basis.m[2][2] = -forward.z;
    entity.setWorldMatrix(core::composeTrs(camera.position(), ecs::quatFromRotationMatrix(basis), entity.localScale()));
}

// 0 camara virtual (desde la vista), 1 camara que sigue a la seleccion,
// 2 riel, 3 camara en riel, 4 carro en riel, 5 secuencia.
ecs::Entity EditorApp::createCinematic(int kind) {
    const ecs::Entity target = world_.find(active_);
    const bool has_target = target.valid() && !target.has<cinema::VirtualCamera>() && !target.has<cinema::DollyTrack>();
    ecs::Entity created;
    const auto new_vcam = [&](const char* name) {
        ensureCameraBrain();
        ecs::Entity e = world_.create(name);
        e.add<cinema::VirtualCamera>();
        alignWithView(e);
        return e;
    };
    switch (kind) {
        case 0: {
            created = new_vcam("Camara virtual");
            created.get<cinema::VirtualCamera>().aim = cinema::AimMode::DoNothing;
            break;
        }
        case 1: {
            created = new_vcam("Camara de seguimiento");
            cinema::VirtualCamera& v = created.get<cinema::VirtualCamera>();
            v.body = cinema::BodyMode::Transposer;
            v.binding = cinema::BindingMode::TargetLocal;
            if (has_target) {
                v.follow = target.uuid();
                v.look_at = target.uuid();
            }
            break;
        }
        case 2:
        case 3: {
            ecs::Entity track = world_.create("Riel");
            track.add<cinema::DollyTrack>();
            track.setWorldPosition(has_target ? target.worldPosition() + Vec3{0.0f, 1.5f, 5.0f}
                                              : scene_.camera().position() + scene_.camera().forward() * 8.0f);
            created = track;
            if (kind == 3) {
                created = new_vcam("Camara en riel");
                cinema::VirtualCamera& v = created.get<cinema::VirtualCamera>();
                v.body = cinema::BodyMode::TrackedDolly;
                v.track = track.uuid();
                if (has_target) {
                    v.follow = target.uuid();
                    v.look_at = target.uuid();
                } else {
                    v.auto_dolly = false;
                    v.aim = cinema::AimMode::DoNothing;
                }
            }
            break;
        }
        case 4: {
            ecs::Entity track = target.valid() && target.has<cinema::DollyTrack>() ? target : ecs::Entity{};
            if (!track.valid()) {
                track = world_.create("Riel");
                track.add<cinema::DollyTrack>();
                track.setWorldPosition(scene_.camera().position() + scene_.camera().forward() * 8.0f);
            }
            created = world_.create("Carro");
            created.add<cinema::DollyCart>().track = track.uuid();
            break;
        }
        default: {
            created = world_.create("Cinematica");
            cinema::CinematicSequence& seq = created.add<cinema::CinematicSequence>();
            // Un plano por cada camara virtual que ya haya, en orden.
            float start = 0.0f;
            world_.forEachDepthFirst([&](ecs::Entity e) {
                if (!e.has<cinema::VirtualCamera>()) return;
                seq.shots.push_back(cinema::CinematicShot{e.uuid(), start, 3.0f, start > 0.0f ? 1.0f : 0.0f,
                                                          cinema::BlendCurve::EaseInOut});
                start += 3.0f;
            });
            ensureCameraBrain();
            timeline_sequence_ = created.uuid();
            show_cinematic_ = true;
            break;
        }
    }
    selectOnly(created.uuid());
    revealInHierarchy(created.uuid());
    commit();
    return created;
}

// -----------------------------------------------------------------------------
// Inspector
// -----------------------------------------------------------------------------

void EditorApp::drawCinematicInspector(const std::string& type_name, ecs::Entity entity) {
    const float half = (ImGui::GetContentRegionAvail().x - 4.0f) * 0.5f;
    if (type_name == "VirtualCamera") {
        const bool live = cinematics_.liveCamera() == entity;
        if (live) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "EN VIVO (la usa la camara real)");
        const bool solo = cinematics_.solo() == entity;
        if (ImGui::Button(solo ? "Quitar solo" : "Solo (ver en el Juego)", ImVec2(half, 0.0f))) {
            cinematics_.setSolo(solo ? ecs::Entity{} : entity);
            if (!solo) {
                show_game_ = true;
                focus_game_ = true;
            }
        }
        ImGui::SetItemTooltip("La camara real usa esta camara virtual pase lo que pase (para encuadrar)");
        ImGui::SameLine();
        if (ImGui::Button("Alinear con la vista", ImVec2(half, 0.0f))) {
            alignWithView(entity);
            commit();
        }
        ImGui::SetItemTooltip("Pone la camara donde esta la de la Escena (sus objetivos pueden moverla despues)");
    } else if (type_name == "DollyTrack") {
        cinema::DollyTrack& track = entity.get<cinema::DollyTrack>();
        const cinema::DollyPath path(track, entity.worldMatrix());
        ImGui::TextDisabled("Longitud %.2f m  |  clic en un punto de la vista para moverlo", path.length());
        if (ImGui::Button("Añadir punto", ImVec2(half, 0.0f))) {
            cinema::DollyWaypoint w{};
            const std::size_t n = track.waypoints.size();
            if (n >= 2) {
                const Vec3 last = track.waypoints[n - 1].position;
                w.position = last + (last - track.waypoints[n - 2].position);
            } else if (n == 1) {
                w.position = track.waypoints[0].position + Vec3{2.0f, 0.0f, 0.0f};
            }
            // Insertar tras el seleccionado (a mitad de camino al siguiente).
            if (selected_waypoint_ >= 0 && selected_waypoint_ + 1 < static_cast<int>(n)) {
                const auto i = static_cast<std::size_t>(selected_waypoint_);
                w.position = (track.waypoints[i].position + track.waypoints[i + 1].position) * 0.5f;
                track.waypoints.insert(track.waypoints.begin() + static_cast<std::ptrdiff_t>(i + 1), w);
                selected_waypoint_ = static_cast<int>(i + 1);
            } else {
                track.waypoints.push_back(w);
                selected_waypoint_ = static_cast<int>(track.waypoints.size() - 1);
            }
            waypoint_track_ = entity.uuid();
            commit();
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(selected_waypoint_ < 0 || track.waypoints.size() <= 2);
        if (ImGui::Button("Quitar el seleccionado", ImVec2(half, 0.0f)) && selected_waypoint_ >= 0 &&
            selected_waypoint_ < static_cast<int>(track.waypoints.size())) {
            track.waypoints.erase(track.waypoints.begin() + selected_waypoint_);
            selected_waypoint_ = -1;
            commit();
        }
        ImGui::EndDisabled();
    } else if (type_name == "CameraBrain") {
        const ecs::Entity live = cinematics_.liveCamera();
        ImGui::TextDisabled("Camara virtual activa: %s", live.valid() ? live.name().c_str() : "(ninguna)");
        if (cinematics_.blending()) ImGui::ProgressBar(cinematics_.blendProgress(), ImVec2(-1.0f, 0.0f), "Mezclando");
    } else if (type_name == "CinematicSequence") {
        const cinema::CinematicSequence& seq = entity.get<cinema::CinematicSequence>();
        ImGui::TextDisabled("%zu planos, %.2f s", seq.shots.size(), seq.duration());
        if (ImGui::Button("Abrir en la ventana Cinemática", ImVec2(-1.0f, 0.0f))) {
            timeline_sequence_ = entity.uuid();
            show_cinematic_ = true;
        }
        if (playing()) {
            const bool on = cinematics_.isPlaying(entity);
            if (ImGui::Button(on ? "Parar" : "Reproducir desde el principio", ImVec2(-1.0f, 0.0f))) {
                if (on) cinematics_.stop(entity);
                else cinematics_.play(entity, 0.0f);
            }
        }
    }
}

// -----------------------------------------------------------------------------
// Ventana Cinematica (Timeline)
// -----------------------------------------------------------------------------

void EditorApp::drawCinematicWindow() {
    if (console_dock_id_ != 0) ImGui::SetNextWindowDockID(console_dock_id_, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Cinemática", &show_cinematic_)) {
        ImGui::End();
        return;
    }
    // Que secuencia: la seleccionada si tiene una.
    if (const ecs::Entity active = world_.find(active_); active.valid() && active.has<cinema::CinematicSequence>()) {
        if (!(active.uuid() == timeline_sequence_)) {
            timeline_sequence_ = active.uuid();
            timeline_selected_shot_ = -1;
        }
    }
    ecs::Entity seq_entity = world_.find(timeline_sequence_);
    if (!seq_entity.valid() || !seq_entity.has<cinema::CinematicSequence>()) {
        seq_entity = {};
        for (const entt::entity h : world_.registry().view<cinema::CinematicSequence>()) {
            seq_entity = world_.wrap(h);
            timeline_sequence_ = seq_entity.uuid();
            break;
        }
    }
    if (!seq_entity.valid()) {
        ImGui::TextDisabled("No hay ninguna Cinematic Sequence en la escena.");
        if (ImGui::Button("Crear una cinemática con las cámaras virtuales")) createCinematic(5);
        ImGui::End();
        return;
    }
    cinema::CinematicSequence& seq = seq_entity.get<cinema::CinematicSequence>();

    // --- Barra de transporte ---
    ImGui::SetNextItemWidth(180.0f);
    if (ImGui::BeginCombo("##seq", seq_entity.name().c_str())) {
        for (const entt::entity h : world_.registry().view<cinema::CinematicSequence>()) {
            const ecs::Entity e = world_.wrap(h);
            if (ImGui::Selectable(e.name().c_str(), e == seq_entity)) timeline_sequence_ = e.uuid();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    float current_time = playing() ? cinematics_.time(seq_entity) : timeline_time_;
    if (!playing()) {
        if (ImGui::Button(timeline_playing_ ? "Pausa" : "Reproducir")) {
            timeline_playing_ = !timeline_playing_;
            timeline_preview_ = true;
            if (timeline_playing_ && timeline_time_ >= seq.duration() - 1e-3f) timeline_time_ = 0.0f;
            cinematics_.setPreview(seq_entity, timeline_time_, timeline_playing_);
            show_game_ = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Inicio")) {
            timeline_time_ = 0.0f;
            cinematics_.setPreview(seq_entity, 0.0f, timeline_playing_);
        }
        ImGui::SameLine();
        if (ImGui::Checkbox("Vista previa", &timeline_preview_) && !timeline_preview_) {
            timeline_playing_ = false;
            cinematics_.clearPreview();
        }
        ImGui::SetItemTooltip("Fuera de Play, la cinematica mueve la camara real (se ve en la vista Juego)");
    } else {
        const bool on = cinematics_.isPlaying(seq_entity);
        if (ImGui::Button(on ? "Parar" : "Reproducir")) {
            if (on) cinematics_.stop(seq_entity);
            else cinematics_.play(seq_entity, current_time >= seq.duration() ? 0.0f : current_time);
        }
    }
    ImGui::SameLine();
    ImGui::Text("%.2f / %.2f s", current_time, seq.duration());
    ImGui::SameLine();
    if (ImGui::Button("+ Plano (cámara nueva desde la vista)")) {
        ecs::Entity cam = createCinematic(0);
        const float start = seq_entity.get<cinema::CinematicSequence>().duration();
        seq_entity.get<cinema::CinematicSequence>().shots.push_back(
            cinema::CinematicShot{cam.uuid(), start, 3.0f, start > 0.0f ? 1.0f : 0.0f, cinema::BlendCurve::EaseInOut});
        selectOnly(seq_entity.uuid());
        commit();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110.0f);
    ImGui::SliderFloat("##zoom", &timeline_zoom_, 20.0f, 400.0f, "%.0f px/s");
    cinema::CinematicSequence& s = seq_entity.get<cinema::CinematicSequence>();  // (puede haber crecido)

    // --- Lienzo ---
    const float label_w = 110.0f;
    const float ruler_h = 22.0f;
    const float row_h = 34.0f;
    const ImVec2 canvas_pos = ImGui::GetCursorScreenPos();
    const ImVec2 canvas_size(std::max(ImGui::GetContentRegionAvail().x, 200.0f),
                             std::max(ImGui::GetContentRegionAvail().y, ruler_h + row_h * 2.0f + 10.0f));
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float x0 = canvas_pos.x + label_w;
    const float y_shots = canvas_pos.y + ruler_h + 4.0f;
    const float y_acts = y_shots + row_h + 6.0f;
    const auto to_x = [&](float t) { return x0 + t * timeline_zoom_; };
    const auto to_t = [&](float x) { return std::max((x - x0) / timeline_zoom_, 0.0f); };
    draw->AddRectFilled(canvas_pos, ImVec2(canvas_pos.x + canvas_size.x, canvas_pos.y + canvas_size.y),
                        IM_COL32(28, 28, 32, 255));
    draw->PushClipRect(canvas_pos, ImVec2(canvas_pos.x + canvas_size.x, canvas_pos.y + canvas_size.y), true);

    // Regla (segundos).
    const float visible_seconds = (canvas_size.x - label_w) / timeline_zoom_;
    const float step = timeline_zoom_ >= 150.0f ? 0.5f : (timeline_zoom_ >= 50.0f ? 1.0f : 5.0f);
    for (float t = 0.0f; t <= visible_seconds + step; t += step) {
        const float x = to_x(t);
        const bool major = std::fmod(t, step * 2.0f) < 1e-3f;
        draw->AddLine(ImVec2(x, canvas_pos.y + (major ? 4.0f : 12.0f)), ImVec2(x, canvas_pos.y + canvas_size.y),
                      major ? IM_COL32(80, 80, 90, 255) : IM_COL32(50, 50, 58, 255));
        if (major) {
            char text[16];
            std::snprintf(text, sizeof(text), "%gs", t);
            draw->AddText(ImVec2(x + 3.0f, canvas_pos.y + 2.0f), IM_COL32(170, 170, 180, 255), text);
        }
    }
    draw->AddRectFilled(canvas_pos, ImVec2(x0 - 1.0f, canvas_pos.y + canvas_size.y), IM_COL32(36, 36, 42, 255));
    draw->AddText(ImVec2(canvas_pos.x + 6.0f, y_shots + 9.0f), IM_COL32(210, 210, 220, 255), "Cámaras");
    draw->AddText(ImVec2(canvas_pos.x + 6.0f, y_acts + 9.0f), IM_COL32(210, 210, 220, 255), "Objetos activos");

    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mouse = io.MousePos;
    const bool canvas_hovered = ImGui::IsWindowHovered() && mouse.x >= canvas_pos.x &&
                                mouse.x <= canvas_pos.x + canvas_size.x && mouse.y >= canvas_pos.y &&
                                mouse.y <= canvas_pos.y + canvas_size.y;
    const bool clicked = canvas_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    bool consumed = false;
    ImGui::SetCursorScreenPos(canvas_pos);
    ImGui::InvisibleButton("##timeline_canvas", canvas_size);  // captura el raton (no mueve la ventana)
    const bool snap = !io.KeyCtrl;
    const auto snap_time = [&](float t) { return snap ? std::round(t * 10.0f) / 10.0f : t; };

    // Planos.
    for (std::size_t i = 0; i < s.shots.size(); ++i) {
        cinema::CinematicShot& shot = s.shots[i];
        const float xa = to_x(shot.start);
        const float xb = to_x(shot.start + shot.duration);
        const ImVec2 a(xa, y_shots);
        const ImVec2 b(xb, y_shots + row_h);
        const bool selected = timeline_selected_shot_ == static_cast<int>(i);
        draw->AddRectFilled(a, b, shotColor(shot.camera, selected ? 1.0f : 0.8f), 4.0f);
        if (selected) draw->AddRect(a, b, IM_COL32(255, 255, 255, 255), 4.0f, 0, 2.0f);
        // Mezcla de entrada: rampa.
        const float xm = to_x(shot.start + std::min(shot.blend_in, shot.duration));
        if (shot.blend_in > 0.0f) {
            draw->AddTriangleFilled(ImVec2(xa, b.y), ImVec2(xm, a.y), ImVec2(xa, a.y), IM_COL32(0, 0, 0, 90));
            draw->AddLine(ImVec2(xa, b.y), ImVec2(xm, a.y), IM_COL32(255, 255, 255, 160), 1.5f);
        }
        // Asas visibles: fin del plano (alargar) y mezcla de entrada.
        const bool near_end = std::abs(mouse.x - xb) < 6.0f && mouse.y >= a.y && mouse.y <= b.y;
        const bool near_blend = std::abs(mouse.x - xm) < 6.0f && mouse.y >= a.y && mouse.y <= a.y + row_h * 0.5f;
        draw->AddCircleFilled(ImVec2(xb, (a.y + b.y) * 0.5f), near_end ? 6.0f : 4.5f, IM_COL32(255, 255, 255, 230));
        draw->AddCircleFilled(ImVec2(xm, a.y + 5.0f), near_blend ? 6.0f : 4.5f, IM_COL32(255, 220, 120, 240));
        if (near_end || near_blend) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        const std::string label = nameOf(world_, shot.camera, "(sin camara)");
        draw->PushClipRect(a, b, true);
        draw->AddText(ImVec2(std::max(xa, xm) + 5.0f, a.y + 9.0f), IM_COL32(15, 15, 20, 255), label.c_str());
        draw->PopClipRect();
        if (!clicked || consumed) continue;
        if (mouse.y < a.y || mouse.y > b.y || mouse.x < xa - 4.0f || mouse.x > xb + 4.0f) continue;
        consumed = true;
        timeline_selected_shot_ = static_cast<int>(i);
        timeline_item_ = static_cast<int>(i);
        if (std::abs(mouse.x - xb) < 6.0f) {
            timeline_drag_ = 3;  // duracion
        } else if (std::abs(mouse.x - xm) < 6.0f && mouse.y < a.y + row_h * 0.5f) {
            timeline_drag_ = 4;  // mezcla
        } else {
            timeline_drag_ = 2;  // mover
            timeline_drag_offset_ = to_t(mouse.x) - shot.start;
        }
        if (const ecs::Entity cam = world_.find(shot.camera); cam.valid() && io.KeyAlt) {
            selectOnly(cam.uuid());  // Alt+clic: seleccionar la camara del plano
        }
    }
    // Objetos activos.
    for (std::size_t i = 0; i < s.activations.size(); ++i) {
        cinema::CinematicActivation& act = s.activations[i];
        const float xa = to_x(act.start);
        const float xb = to_x(std::max(act.end, act.start));
        const ImVec2 a(xa, y_acts);
        const ImVec2 b(xb, y_acts + row_h);
        draw->AddRectFilled(a, b, IM_COL32(110, 140, 110, 220), 4.0f);
        draw->AddCircleFilled(ImVec2(xb, (a.y + b.y) * 0.5f), std::abs(mouse.x - xb) < 6.0f ? 6.0f : 4.5f,
                              IM_COL32(255, 255, 255, 230));
        const std::string label = nameOf(world_, act.entity, "(sin objeto)");
        draw->PushClipRect(a, b, true);
        draw->AddText(ImVec2(xa + 5.0f, a.y + 9.0f), IM_COL32(15, 20, 15, 255), label.c_str());
        draw->PopClipRect();
        if (!clicked || consumed) continue;
        if (mouse.y < a.y || mouse.y > b.y || mouse.x < xa - 4.0f || mouse.x > xb + 4.0f) continue;
        consumed = true;
        timeline_item_ = static_cast<int>(i);
        if (std::abs(mouse.x - xb) < 6.0f) {
            timeline_drag_ = 6;
        } else {
            timeline_drag_ = 5;
            timeline_drag_offset_ = to_t(mouse.x) - act.start;
        }
    }
    // Clic en la regla o en un hueco: mover el cabezal.
    if (clicked && !consumed && mouse.x >= x0) {
        timeline_drag_ = 1;
        timeline_selected_shot_ = -1;
    }

    // Arrastres.
    if (timeline_drag_ != 0) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if (timeline_drag_ >= 2) commit();
            timeline_drag_ = 0;
        } else {
            const float t = to_t(mouse.x);
            const auto item = static_cast<std::size_t>(std::max(timeline_item_, 0));
            switch (timeline_drag_) {
                case 1:
                    if (playing()) {
                        cinematics_.play(seq_entity, t);
                    } else {
                        timeline_time_ = t;
                        timeline_preview_ = true;
                    }
                    break;
                case 2:
                    if (item < s.shots.size()) s.shots[item].start = std::max(snap_time(t - timeline_drag_offset_), 0.0f);
                    break;
                case 3:
                    if (item < s.shots.size()) s.shots[item].duration = std::max(snap_time(t) - s.shots[item].start, 0.1f);
                    break;
                case 4:
                    if (item < s.shots.size()) {
                        s.shots[item].blend_in = std::clamp(snap_time(t) - s.shots[item].start, 0.0f, s.shots[item].duration);
                    }
                    break;
                case 5:
                    if (item < s.activations.size()) {
                        const float length = s.activations[item].end - s.activations[item].start;
                        s.activations[item].start = std::max(snap_time(t - timeline_drag_offset_), 0.0f);
                        s.activations[item].end = s.activations[item].start + length;
                    }
                    break;
                case 6:
                    if (item < s.activations.size()) {
                        s.activations[item].end = std::max(snap_time(t), s.activations[item].start + 0.1f);
                    }
                    break;
                default: break;
            }
            if (timeline_drag_ >= 2) dirty_ = true;
        }
    }

    // Menu contextual: plano o hueco.
    if (canvas_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        timeline_item_ = -1;
        for (std::size_t i = 0; i < s.shots.size(); ++i) {
            if (mouse.y >= y_shots && mouse.y <= y_shots + row_h && mouse.x >= to_x(s.shots[i].start) &&
                mouse.x <= to_x(s.shots[i].start + s.shots[i].duration)) {
                timeline_item_ = static_cast<int>(i);
            }
        }
        timeline_drag_offset_ = to_t(mouse.x);
        ImGui::OpenPopup("timeline_menu");
    }
    if (ImGui::BeginPopup("timeline_menu")) {
        const float at = snap_time(timeline_drag_offset_);
        if (timeline_item_ >= 0 && timeline_item_ < static_cast<int>(s.shots.size())) {
            const auto i = static_cast<std::size_t>(timeline_item_);
            if (ImGui::MenuItem("Seleccionar la cámara")) selectOnly(s.shots[i].camera);
            if (ImGui::MenuItem("Duplicar plano")) {
                cinema::CinematicShot copy = s.shots[i];
                copy.start = s.shots[i].start + s.shots[i].duration;
                s.shots.push_back(copy);
                commit();
            }
            if (ImGui::MenuItem("Borrar plano")) {
                s.shots.erase(s.shots.begin() + static_cast<std::ptrdiff_t>(i));
                timeline_selected_shot_ = -1;
                commit();
            }
        } else {
            if (ImGui::BeginMenu("Añadir plano con...")) {
                for (const entt::entity h : world_.registry().view<cinema::VirtualCamera>()) {
                    const ecs::Entity cam = world_.wrap(h);
                    if (ImGui::MenuItem(cam.name().c_str())) {
                        s.shots.push_back(cinema::CinematicShot{cam.uuid(), at, 3.0f, 1.0f, cinema::BlendCurve::EaseInOut});
                        commit();
                    }
                }
                ImGui::EndMenu();
            }
            const ecs::Entity chosen = world_.find(active_);
            if (ImGui::MenuItem("Activar la selección aquí (2 s)", nullptr, false,
                                chosen.valid() && !(chosen == seq_entity))) {
                s.activations.push_back(cinema::CinematicActivation{chosen.uuid(), at, at + 2.0f});
                commit();
            }
        }
        ImGui::EndPopup();
    }

    // Cabezal.
    current_time = playing() ? cinematics_.time(seq_entity) : timeline_time_;
    const float xp = to_x(current_time);
    draw->AddLine(ImVec2(xp, canvas_pos.y), ImVec2(xp, canvas_pos.y + canvas_size.y), IM_COL32(255, 80, 80, 255), 2.0f);
    draw->AddTriangleFilled(ImVec2(xp - 6.0f, canvas_pos.y), ImVec2(xp + 6.0f, canvas_pos.y),
                            ImVec2(xp, canvas_pos.y + 9.0f), IM_COL32(255, 80, 80, 255));
    draw->PopClipRect();
    ImGui::End();
}

}  // namespace cramion::editor
