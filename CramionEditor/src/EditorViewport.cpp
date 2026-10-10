// La vista 3D al estilo de Blender:
//
//   - Cabecera: el tipo de editor, Escena | Juego, los menus Vista,
//     Seleccionar, Anadir y Objeto, lo del modo activo (modelado, estampar,
//     2D) y a la derecha la orientacion, el pivote, el ajuste a la rejilla,
//     los gizmos, la navegacion, el modo de dibujo y la proporcion.
//   - Estante de herramientas a la izquierda (seleccionar, mover, rotar,
//     escalar; estampar, pintar y modelar), la activa en azul.
//   - Gizmo de navegacion arriba a la derecha: los ejes de la camara (clic en
//     uno alinea la vista, arrastrar gira alrededor de la seleccion) y debajo
//     acercar, desplazar, la vista de la camara del juego y
//     perspectiva/ortografica.
//   - Arriba a la izquierda, el tipo de vista y lo seleccionado.
//
// Y la cabecera de la vista Juego.

#include "EditorApp.h"

#include "BlenderUi.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>

namespace cramion::editor {

using core::Vec3;

namespace {

namespace bl = blender;

// Pone lo siguiente pegado al borde derecho de la cabecera: el ancho se mide
// en el frame anterior (BeginGroup/EndGroup).
void alignRight(float width) {
    const float x = ImGui::GetWindowWidth() - width - ImGui::GetStyle().WindowPadding.x - 6.0f;
    if (x > ImGui::GetCursorPosX()) ImGui::SetCursorPosX(x);
}

}  // namespace

// -----------------------------------------------------------------------------
// Cabeceras
// -----------------------------------------------------------------------------

void EditorApp::drawSceneHeader() {
    const bool scene_workspace = activeWorkspaceKind() == WorkspaceKind::Scene;
    // Escena | Juego en la misma area (como cambiar de modo en Blender).
    if (scene_workspace) {
        static const char* const kLabels[2] = {"Escena", "Juego"};
        static const bl::Glyph kGlyphs[2] = {bl::Glyph::View3D, bl::Glyph::Game};
        static const char* const kTips[2] = {"La escena con la cámara del editor", "El juego visto por su cámara"};
        if (bl::segmented("##view_mode", kLabels, kGlyphs, 2, 0, kTips) == 1) switchArea(AreaEditor::Scene, AreaEditor::Game);
    }
    if (ImGui::BeginMenu("Vista")) {
        if (ImGui::MenuItem("Enfocar la selección", "F", false, !selection_.empty())) focusSelection();
        ImGui::Separator();
        if (ImGui::MenuItem("Superior")) alignView(1);
        if (ImGui::MenuItem("Inferior")) alignView(4);
        if (ImGui::MenuItem("Frontal")) alignView(2);
        if (ImGui::MenuItem("Trasera")) alignView(5);
        if (ImGui::MenuItem("Derecha")) alignView(0);
        if (ImGui::MenuItem("Izquierda")) alignView(3);
        ImGui::Separator();
        scene::Camera& camera = scene_.camera();
        if (ImGui::MenuItem("Ortográfica", nullptr, camera.orthographic())) {
            const float distance = core::length(viewPivot() - camera.position());
            camera.setOrthographic(!camera.orthographic(), std::max(distance * 0.5f, 0.5f));
        }
        ImGui::Separator();
        ImGui::MenuItem("Gizmos e iconos", "G", &show_gizmos_);
        ImGui::MenuItem("Navegación (navmesh)", "P", &show_navigation_);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Seleccionar")) {
        if (ImGui::MenuItem("Todo")) {
            world_.forEachDepthFirst([&](ecs::Entity e) {
                if (!isStageHelper(e.uuid()) && !isSelected(e.uuid())) toggleSelection(e.uuid());
            });
        }
        if (ImGui::MenuItem("Nada", nullptr, false, !selection_.empty())) clearSelection();
        if (ImGui::MenuItem("Invertir")) {
            world_.forEachDepthFirst([&](ecs::Entity e) {
                if (!isStageHelper(e.uuid())) toggleSelection(e.uuid());
            });
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Añadir")) {
        drawAddMenuItems();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Objeto")) {
        const bool any = !selection_.empty();
        if (ImGui::MenuItem("Duplicar", "Ctrl+D", false, any)) duplicateSelection();
        if (ImGui::MenuItem("Copiar", "Ctrl+C", false, any)) copySelection();
        if (ImGui::MenuItem("Pegar", "Ctrl+V", false, !clipboard_.empty())) pasteClipboard();
        ImGui::Separator();
        if (ImGui::MenuItem("Borrar", "Supr", false, any)) deleteSelection();
        ImGui::Separator();
        if (ImGui::MenuItem("Mover", "W", gizmo_ == GizmoOperation::Translate)) gizmo_ = GizmoOperation::Translate;
        if (ImGui::MenuItem("Rotar", "E", gizmo_ == GizmoOperation::Rotate)) gizmo_ = GizmoOperation::Rotate;
        if (ImGui::MenuItem("Escalar", "R", gizmo_ == GizmoOperation::Scale)) gizmo_ = GizmoOperation::Scale;
        ImGui::EndMenu();
    }
    // Lo del modo activo (como los modos de seleccion de Blender en edicion).
    if (modelingTarget() != nullptr && !playing()) drawModelingToolbar();
    if (stamp_mode_) drawStampToolbar();
    draw2DViewToggle();

    // --- A la derecha ---
    alignRight(scene_header_right_w_);
    ImGui::BeginGroup();
    {
        const char* space = gizmo_local_ ? "Local" : "Global";
        static const bl::Glyph kSpaceGlyphs[1] = {bl::Glyph::Globe};
        const char* labels[1] = {space};
        const char* tips[1] = {"Espacio del gizmo: ejes del mundo o del objeto (X)"};
        if (bl::segmented("##space", labels, kSpaceGlyphs, 1, -1, tips) == 0) gizmo_local_ = !gizmo_local_;
        ImGui::SameLine(0.0f, 2.0f);
        if (bl::iconButton("##pivot", bl::Glyph::Pivot, gizmo_center_,
                           gizmo_center_ ? "Pivote: centro de la selección (Z)" : "Pivote: el de cada objeto (Z)")) {
            gizmo_center_ = !gizmo_center_;
        }
        ImGui::SameLine(0.0f, 2.0f);
        if (bl::iconButton("##snap", bl::Glyph::Magnet, snap_enabled_, "Ajustar a la rejilla (mantener Ctrl invierte)")) {
            snap_enabled_ = !snap_enabled_;
        }
        ImGui::SameLine(0.0f, 0.0f);
        if (bl::iconButton("##snap_steps", bl::Glyph::ChevronDown, false, "Pasos del ajuste", ImGui::GetFrameHeight() * 0.7f)) {
            ImGui::OpenPopup("##snap_popup");
        }
        if (ImGui::BeginPopup("##snap_popup")) {
            ImGui::SeparatorText("Ajustar");
            ImGui::Checkbox("Activado", &snap_enabled_);
            ImGui::SetNextItemWidth(110.0f);
            ImGui::DragFloat("Mover", &snap_translate_, 0.01f, 0.01f, 100.0f, "%.2f m");
            ImGui::SetNextItemWidth(110.0f);
            ImGui::DragFloat("Rotar", &snap_rotate_, 0.5f, 1.0f, 180.0f, "%.0f°");
            ImGui::SetNextItemWidth(110.0f);
            ImGui::DragFloat("Escalar", &snap_scale_, 0.005f, 0.01f, 10.0f, "%.2f");
            ImGui::EndPopup();
        }
        ImGui::SameLine(0.0f, 10.0f);
        if (bl::iconButton("##gizmos", bl::Glyph::Gizmo, show_gizmos_,
                           "Gizmos e iconos de la escena: luces, cámaras, decals, física, cinemáticas y agua (G)")) {
            show_gizmos_ = !show_gizmos_;
        }
        ImGui::SameLine(0.0f, 2.0f);
        if (bl::iconButton("##navmesh", bl::Glyph::Grid, show_navigation_, "Mostrar la navegación (P)")) {
            show_navigation_ = !show_navigation_;
        }
        ImGui::SameLine(0.0f, 10.0f);
        drawSceneDrawModeMenu();
        ImGui::SameLine();
        drawPathTracingButton();
        ImGui::SameLine();
        drawAspectMenu(kSceneSlot);
        // En Play con red: servidor o cliente y cuantos jugadores.
        if (const std::string net = scripts_.networkStatus(); !net.empty()) {
            ImGui::SameLine();
            ImGui::TextColored(theme::vec(theme::kOk), "Red: %s", net.c_str());
            ImGui::SetItemTooltip("Partida en red de los scripts (tabla Network). Salir de Play la cierra.");
        }
    }
    ImGui::EndGroup();
    scene_header_right_w_ = ImGui::GetItemRectSize().x;
}

void EditorApp::drawGameHeader() {
    static const char* const kLabels[2] = {"Escena", "Juego"};
    static const bl::Glyph kGlyphs[2] = {bl::Glyph::View3D, bl::Glyph::Game};
    static const char* const kTips[2] = {"La escena con la cámara del editor", "El juego visto por su cámara"};
    if (bl::segmented("##view_mode", kLabels, kGlyphs, 2, 1, kTips) == 0) switchArea(AreaEditor::Game, AreaEditor::Scene);

    // Que camara se ve.
    const ecs::Entity brain = cinematics_.brain();
    ecs::Entity main_camera = brain;
    if (!main_camera.valid()) {
        for (const entt::entity h : world_.registry().view<ecs::Camera>()) {
            const ecs::Entity e = world_.wrap(h);
            if (e.activeInHierarchy() && (!main_camera.valid() || e.get<ecs::Camera>().is_main)) main_camera = e;
        }
    }
    ImGui::SameLine(0.0f, 12.0f);
    if (main_camera.valid()) {
        ImGui::TextDisabled("Cámara: %s", main_camera.name().c_str());
        const ecs::Entity live = cinematics_.liveCamera();
        if (brain.valid() && live.valid()) {
            ImGui::SameLine();
            if (cinematics_.blending() && cinematics_.previousCamera().valid()) {
                ImGui::TextDisabled("| Mezclando %s -> %s (%.0f %%)", cinematics_.previousCamera().name().c_str(),
                                    live.name().c_str(), cinematics_.blendProgress() * 100.0f);
            } else {
                ImGui::TextDisabled("| Virtual: %s%s", live.name().c_str(), cinematics_.solo().valid() ? " (solo)" : "");
            }
        }
    } else {
        ImGui::TextColored(theme::vec(theme::kYellow), "No hay ninguna Camera en la escena");
    }

    alignRight(game_header_right_w_);
    ImGui::BeginGroup();
    ImGui::Checkbox("Táctil", &touch_simulate_);
    ImGui::SetItemTooltip("En Play: los controles tactiles del proyecto sobre la vista, con el raton como dedo.\n"
                          "Se disenan en Archivo > Controles tactiles.");
    ImGui::SameLine();
    ImGui::Checkbox("Tercios", &game_guides_);
    ImGui::SameLine();
    drawAspectMenu(kGameSlot);
    ImGui::EndGroup();
    game_header_right_w_ = ImGui::GetItemRectSize().x;
}

// -----------------------------------------------------------------------------
// Encima de la vista
// -----------------------------------------------------------------------------

void EditorApp::layoutViewportOverlays(ImVec2 origin, ImVec2 size) {
    ViewportOverlays& o = viewport_overlays_;
    const float frame = ImGui::GetFrameHeight();
    o.tool = std::floor(frame * 1.4f);
    o.tools = 7;
    o.shelf = ImVec2(origin.x + 6.0f, origin.y + 6.0f);
    o.gizmo_radius = std::floor(frame * 1.85f);
    o.button_radius = std::floor(frame * 0.5f);
    o.gizmo = ImVec2(origin.x + size.x - o.gizmo_radius - 12.0f, origin.y + o.gizmo_radius + 10.0f);
    // Sin sitio (vista muy pequena) o en 2D, sin gizmo de navegacion.
    o.navigation = size.x > o.gizmo_radius * 6.0f && size.y > o.gizmo_radius * 2.0f + o.button_radius * 10.0f + 30.0f &&
                   !view2DActive();
    if (size.y < o.tool * 8.0f || playing()) o.tools = 0;
}

bool EditorApp::viewportOverlayHit(ImVec2 p) const {
    const ViewportOverlays& o = viewport_overlays_;
    if (o.tools > 0) {
        const float height = o.tool * 7.0f + 3.0f * 6.0f + 10.0f;
        if (p.x >= o.shelf.x - 3.0f && p.x <= o.shelf.x + o.tool + 3.0f && p.y >= o.shelf.y - 3.0f &&
            p.y <= o.shelf.y + height + 3.0f) {
            return true;
        }
    }
    if (o.navigation) {
        const float dx = p.x - o.gizmo.x;
        const float dy = p.y - o.gizmo.y;
        if (dx * dx + dy * dy <= o.gizmo_radius * o.gizmo_radius) return true;
        const float top = o.gizmo.y + o.gizmo_radius + 8.0f;
        const float bottom = top + 4.0f * (o.button_radius * 2.0f + 6.0f);
        if (std::abs(dx) <= o.button_radius + 2.0f && p.y >= top && p.y <= bottom) return true;
    }
    return false;
}

void EditorApp::drawViewportShelf() {
    const ViewportOverlays& o = viewport_overlays_;
    if (o.tools == 0 || playing()) return;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float height = o.tool * 7.0f + 3.0f * 6.0f + 10.0f;
    bl::toolShelfBackground(draw, ImVec2(o.shelf.x - 3.0f, o.shelf.y - 3.0f),
                            ImVec2(o.shelf.x + o.tool + 3.0f, o.shelf.y + height + 3.0f));
    float y = o.shelf.y;
    const auto tool = [&](const char* id, bl::Glyph glyph, bool active, const char* tip) {
        ImGui::SetCursorScreenPos(ImVec2(o.shelf.x, y));
        const bool pressed = bl::toolButton(id, glyph, active, tip, o.tool);
        y += o.tool + 3.0f;
        return pressed;
    };
    const bool editing = !stamp_mode_ && !paint_mode_;
    if (tool("##tool_select", bl::Glyph::Select, editing && gizmo_ == GizmoOperation::None, "Seleccionar (Q)")) {
        gizmo_ = GizmoOperation::None;
        stamp_mode_ = paint_mode_ = false;
    }
    if (tool("##tool_move", bl::Glyph::Move, editing && gizmo_ == GizmoOperation::Translate, "Mover (W)")) {
        gizmo_ = GizmoOperation::Translate;
        stamp_mode_ = paint_mode_ = false;
    }
    if (tool("##tool_rotate", bl::Glyph::Rotate, editing && gizmo_ == GizmoOperation::Rotate, "Rotar (E)")) {
        gizmo_ = GizmoOperation::Rotate;
        stamp_mode_ = paint_mode_ = false;
    }
    if (tool("##tool_scale", bl::Glyph::Scale, editing && gizmo_ == GizmoOperation::Scale, "Escalar (R)")) {
        gizmo_ = GizmoOperation::Scale;
        stamp_mode_ = paint_mode_ = false;
    }
    y += 10.0f;
    if (tool("##tool_stamp", bl::Glyph::Stamp, stamp_mode_, "Estampar decals, charcos o humedad (T)")) {
        stamp_mode_ = !stamp_mode_;
        if (stamp_mode_) paint_mode_ = false;
    }
    if (tool("##tool_paint", bl::Glyph::Brush, paint_mode_,
             "Pintar prefabs (árboles, rocas...) con un pincel (B). Mayús + arrastrar borra.")) {
        paint_mode_ = !paint_mode_;
        if (paint_mode_) {
            stamp_mode_ = false;
            show_paint_window_ = true;
        }
    }
    if (tool("##tool_model", bl::Glyph::Modeling, show_modeling_window_,
             "Modelado poligonal (como ProBuilder): formas, extruir, biselar, booleanas...")) {
        show_modeling_window_ = !show_modeling_window_;
    }
}

void EditorApp::drawNavigationGizmo() {
    const ViewportOverlays& o = viewport_overlays_;
    if (!o.navigation) return;
    scene::Camera& camera = scene_.camera();
    const Vec3 f = camera.forward();
    const Vec3 r = camera.right();
    const Vec3 u = camera.up();
    const float forward[3] = {f.x, f.y, f.z};
    const float right[3] = {r.x, r.y, r.z};
    const float up[3] = {u.x, u.y, u.z};
    const bl::NavigationResult nav = bl::navigationGizmo("##navigation_gizmo", o.gizmo, o.gizmo_radius, right, up, forward);
    if (nav.axis >= 0) alignView(nav.axis);
    if (nav.dragging) orbitView(nav.drag.x, nav.drag.y);

    // Acercar, desplazar, camara del juego y perspectiva (como Blender).
    const float step = o.button_radius * 2.0f + 6.0f;
    float y = o.gizmo.y + o.gizmo_radius + 8.0f + o.button_radius;
    ImVec2 drag{};
    if (bl::viewButton("##view_zoom", bl::Glyph::Zoom, ImVec2(o.gizmo.x, y), o.button_radius, false,
                       "Arrastra para acercar o alejar", &drag) ||
        drag.y != 0.0f) {
        const float k = std::max(camera.moveSpeed() * 0.02f, 0.02f);
        camera.setPosition(camera.position() + camera.forward() * (-drag.y * k));
    }
    y += step;
    drag = ImVec2(0.0f, 0.0f);
    bl::viewButton("##view_pan", bl::Glyph::Pan, ImVec2(o.gizmo.x, y), o.button_radius, false, "Arrastra para desplazar la vista",
                   &drag);
    if (drag.x != 0.0f || drag.y != 0.0f) {
        const float k = std::max(camera.moveSpeed() * 0.0025f, 0.004f);
        camera.setPosition(camera.position() - camera.right() * (drag.x * k) + camera.up() * (drag.y * k));
    }
    y += step;
    if (bl::viewButton("##view_camera", bl::Glyph::Camera, ImVec2(o.gizmo.x, y), o.button_radius, false,
                       "Ver desde la cámara principal del juego")) {
        for (const entt::entity h : world_.registry().view<ecs::Camera>()) {
            const ecs::Entity e = world_.wrap(h);
            if (!e.activeInHierarchy()) continue;
            camera.setOrthographic(false);
            camera.setPosition(e.worldPosition());
            camera.lookAt(e.worldPosition() + e.forward());
            if (e.get<ecs::Camera>().is_main) break;
        }
    }
    y += step;
    if (bl::viewButton("##view_ortho", camera.orthographic() ? bl::Glyph::Orthographic : bl::Glyph::Perspective,
                       ImVec2(o.gizmo.x, y), o.button_radius, false,
                       camera.orthographic() ? "Ortográfica (clic: perspectiva)" : "Perspectiva (clic: ortográfica)")) {
        const float distance = core::length(viewPivot() - camera.position());
        camera.setOrthographic(!camera.orthographic(), std::max(distance * 0.5f, 0.5f));
    }
}

void EditorApp::drawViewportInfo() {
    if (view_w_ < 260.0f) return;
    const ViewportOverlays& o = viewport_overlays_;
    const scene::Camera& camera = scene_.camera();
    const char* kind = camera.orthographic() ? "Ortográfica del usuario" : "Perspectiva del usuario";
    if (view2DActive()) kind = "Vista 2D (plano XY)";
    const float x = (o.tools > 0 && !playing() ? o.shelf.x + o.tool + 12.0f : view_x_ + 10.0f);
    const float y = view_y_ + 8.0f;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImU32 color = IM_COL32(230, 230, 230, 235);
    bl::overlayText(draw, ImVec2(x, y), color, kind);
    std::string second = "(" + world_.sceneName() + ")";
    if (const ecs::Entity active = world_.find(active_); active.valid()) {
        second += " | " + active.name();
        if (selection_.size() > 1) second += "  (+" + std::to_string(selection_.size() - 1) + ")";
    }
    bl::overlayText(draw, ImVec2(x, y + ImGui::GetFontSize() + 2.0f), color, second.c_str());
    if (playing()) {
        bl::overlayText(draw, ImVec2(x, y + (ImGui::GetFontSize() + 2.0f) * 2.0f), theme::kYellow,
                        play_state_ == PlayState::Paused ? "En pausa" : "Jugando");
    }
}

// -----------------------------------------------------------------------------
// Orbita y vistas alineadas
// -----------------------------------------------------------------------------

Vec3 EditorApp::viewPivot() {
    Vec3 low{};
    Vec3 high{};
    if (!selection_.empty() && selectionBounds(low, high)) return (low + high) * 0.5f;
    // Sin seleccion: donde mira la camara (el suelo si lo ve cerca).
    const scene::Camera& camera = scene_.camera();
    const Vec3 f = camera.forward();
    if (f.y < -0.05f) {
        const float t = -camera.position().y / f.y;
        if (t > 0.5f && t < 200.0f) return camera.position() + f * t;
    }
    return camera.position() + f * 10.0f;
}

void EditorApp::alignView(int axis) {
    scene::Camera& camera = scene_.camera();
    const Vec3 pivot = viewPivot();
    const float distance = std::max(core::length(camera.position() - pivot), 1.0f);
    Vec3 direction{0.0f, 0.0f, 0.0f};  // desde el pivote hacia la camara
    const float sign = axis < 3 ? 1.0f : -1.0f;
    if (axis % 3 == 0) direction.x = sign;
    if (axis % 3 == 1) direction.y = sign;
    if (axis % 3 == 2) direction.z = sign;
    camera.setPosition(pivot + direction * distance);
    // Mirando al pivote; las vistas de arriba/abajo con el yaw de ahora.
    if (axis % 3 == 1) {
        const Vec3 f = camera.forward();
        camera.setRotation(std::atan2(f.z, f.x), -sign * (core::kPi * 0.5f));
    } else {
        camera.lookAt(pivot);
    }
}

void EditorApp::orbitView(float dx, float dy) {
    scene::Camera& camera = scene_.camera();
    const Vec3 pivot = viewPivot();
    const float distance = std::max(core::length(camera.position() - pivot), 0.5f);
    const Vec3 f = camera.forward();
    float yaw = std::atan2(f.z, f.x);
    float pitch = std::asin(std::clamp(f.y, -1.0f, 1.0f));
    yaw += dx * 0.01f;
    pitch = std::clamp(pitch - dy * 0.01f, -core::kPi * 0.5f + 0.02f, core::kPi * 0.5f - 0.02f);
    camera.setRotation(yaw, pitch);
    camera.setPosition(pivot - camera.forward() * distance);
}

}  // namespace cramion::editor
