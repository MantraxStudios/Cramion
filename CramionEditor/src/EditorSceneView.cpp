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
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>

namespace cramion::editor {

using core::Mat4;
using core::Vec3;
using core::Vec4;

namespace {

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
    if (toolButton("Q", gizmo_ == GizmoOperation::None, "Sin gizmo (Q)")) gizmo_ = GizmoOperation::None;
    ImGui::SameLine();
    const auto icon_button = [&](const char* id, Icon icon, bool active, const char* tooltip) {
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_Header));
        const float size = ImGui::GetFrameHeight() - ImGui::GetStyle().FramePadding.y * 2.0f;
        bool pressed = false;
        if (imgui_.icon(icon) != 0) {
            pressed = ImGui::ImageButton(id, imgui_.icon(icon), ImVec2(size, size), ImVec2(0, 0), ImVec2(1, 1),
                                         ImVec4(0, 0, 0, 0), ImVec4(1, 1, 1, active ? 1.0f : 0.75f));
        } else {
            pressed = ImGui::Button(id);
        }
        if (active) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", tooltip);
        return pressed;
    };
    if (icon_button("W##move", Icon::Move, gizmo_ == GizmoOperation::Translate, "Mover (W)")) gizmo_ = GizmoOperation::Translate;
    ImGui::SameLine();
    if (icon_button("E##rotate", Icon::Rotate, gizmo_ == GizmoOperation::Rotate, "Rotar (E)")) gizmo_ = GizmoOperation::Rotate;
    ImGui::SameLine();
    if (icon_button("R##scale", Icon::Scale, gizmo_ == GizmoOperation::Scale, "Escalar (R)")) gizmo_ = GizmoOperation::Scale;
    drawStampToolbar();
    drawPaintToolbar();
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
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    if (toolButton("Nav", show_navigation_, "Mostrar la navegación (P), como en Unreal")) {
        show_navigation_ = !show_navigation_;
    }
    ImGui::SameLine();
    if (toolButton("Gizmos", show_gizmos_,
                   "Mostrar los iconos y ayudas de la escena: luces, cámaras, decals, física, cinemáticas y agua (G)")) {
        show_gizmos_ = !show_gizmos_;
    }
    ImGui::SameLine();
    drawSceneDrawModeMenu();
    ImGui::SameLine();
    drawPathTracingButton();
    ImGui::SameLine();
    drawAspectMenu(kSceneSlot);
    // En Play con red: servidor o cliente y cuantos jugadores.
    if (const std::string net = scripts_.networkStatus(); !net.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.35f, 0.85f, 0.55f, 1.0f), "Red: %s", net.c_str());
        ImGui::SetItemTooltip("Partida en red de los scripts (tabla Network). Salir de Play la cierra.");
    }
    ImGui::PopStyleVar();
}

// -----------------------------------------------------------------------------
// Proporcion de las vistas (como el menu del Game view de Unity)
// -----------------------------------------------------------------------------

namespace {

struct AspectPreset {
    const char* label;
    float ratio;  // ancho / alto (0 = libre o resolucion fija)
    int width;    // resolucion fija (0 = no)
    int height;
};

constexpr AspectPreset kAspectPresets[] = {
    {"Free Aspect", 0.0f, 0, 0},
    {"16:9", 16.0f / 9.0f, 0, 0},
    {"16:10", 16.0f / 10.0f, 0, 0},
    {"21:9", 21.0f / 9.0f, 0, 0},
    {"32:9", 32.0f / 9.0f, 0, 0},
    {"4:3", 4.0f / 3.0f, 0, 0},
    {"5:4", 5.0f / 4.0f, 0, 0},
    {"3:2", 3.0f / 2.0f, 0, 0},
    {"1:1", 1.0f, 0, 0},
    {"9:16 (vertical)", 9.0f / 16.0f, 0, 0},
    {"9:19.5 (movil vertical)", 9.0f / 19.5f, 0, 0},
    {"1280x720 (HD)", 0.0f, 1280, 720},
    {"1920x1080 (Full HD)", 0.0f, 1920, 1080},
    {"2560x1440 (QHD)", 0.0f, 2560, 1440},
    {"3840x2160 (4K)", 0.0f, 3840, 2160},
    {"1080x1920 (movil)", 0.0f, 1080, 1920},
};
constexpr int kAspectPresetCount = static_cast<int>(sizeof(kAspectPresets) / sizeof(kAspectPresets[0]));
constexpr int kFirstFixedPreset = 11;

ImVec2 fitAspect(ImVec2 avail, float aspect) {
    ImVec2 size = avail;
    if (aspect > 0.0f) {
        if (avail.x / std::max(avail.y, 1.0f) > aspect) size.x = avail.y * aspect;
        else size.y = avail.x / aspect;
    }
    return ImVec2(std::max(size.x, 1.0f), std::max(size.y, 1.0f));
}

}  // namespace

ImVec2 EditorApp::layoutViewImage(std::uint32_t slot, ImVec2 avail, ImVec2& origin) {
    const ViewAspect& a = view_aspect_[slot & 1];
    // Lo que pide la vista: libre = todo el panel; proporcion = lo que cabe;
    // resolucion fija = esa, mostrada a escala.
    float target = avail.x / std::max(avail.y, 1.0f);
    std::uint32_t want_w = 0;
    std::uint32_t want_h = 0;
    if (a.preset < 0) {
        want_w = static_cast<std::uint32_t>(std::clamp(a.custom_w, 16, 8192));
        want_h = static_cast<std::uint32_t>(std::clamp(a.custom_h, 16, 8192));
        target = static_cast<float>(want_w) / static_cast<float>(want_h);
    } else if (a.preset < kAspectPresetCount) {
        const AspectPreset& p = kAspectPresets[a.preset];
        if (p.width > 0) {
            want_w = static_cast<std::uint32_t>(p.width);
            want_h = static_cast<std::uint32_t>(p.height);
            target = static_cast<float>(p.width) / static_cast<float>(p.height);
        } else if (p.ratio > 0.0f) {
            target = p.ratio;
        }
    }
    ImVec2 size = fitAspect(avail, target);
    if (want_w == 0) {
        want_w = static_cast<std::uint32_t>(std::lround(size.x));
        want_h = static_cast<std::uint32_t>(std::lround(size.y));
    }
    view_desired_[slot & 1][0] = want_w;
    view_desired_[slot & 1][1] = want_h;
    // La otra vista a la vez (Escena y Juego visibles): comparten la imagen,
    // que tiene la proporcion de la que se dibuja; esta se ve sin deformar.
    const vk::Extent2D extent = renderer_.sceneExtent();
    if (slot != render_view_ && extent.width > 0 && extent.height > 0) {
        size = fitAspect(avail, static_cast<float>(extent.width) / static_cast<float>(extent.height));
    }
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    origin = ImVec2(std::floor(cursor.x + (avail.x - size.x) * 0.5f), std::floor(cursor.y + (avail.y - size.y) * 0.5f));
    return size;
}

void EditorApp::drawAspectMenu(std::uint32_t slot) {
    ViewAspect& a = view_aspect_[slot & 1];
    char label[64];
    if (a.preset < 0) std::snprintf(label, sizeof(label), "%dx%d", a.custom_w, a.custom_h);
    else std::snprintf(label, sizeof(label), "%s", kAspectPresets[std::clamp(a.preset, 0, kAspectPresetCount - 1)].label);
    ImGui::SetNextItemWidth(std::max(ImGui::CalcTextSize(label).x + 34.0f, 110.0f));
    ImGui::PushID(static_cast<int>(slot));
    if (ImGui::BeginCombo("##aspect", label, ImGuiComboFlags_HeightLarge)) {
        bool changed = false;
        for (int i = 0; i < kAspectPresetCount; ++i) {
            if (i == 1 || i == kFirstFixedPreset) ImGui::Separator();
            if (ImGui::Selectable(kAspectPresets[i].label, a.preset == i)) {
                a.preset = i;
                changed = true;
            }
        }
        ImGui::Separator();
        ImGui::TextDisabled("Resolucion propia");
        ImGui::SetNextItemWidth(70.0f);
        int w = a.custom_w;
        int h = a.custom_h;
        const bool edit_w = ImGui::InputInt("##cw", &w, 0);
        ImGui::SameLine();
        ImGui::TextUnformatted("x");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70.0f);
        const bool edit_h = ImGui::InputInt("##ch", &h, 0);
        ImGui::SameLine();
        if (edit_w || edit_h) {
            a.custom_w = std::clamp(w, 16, 8192);
            a.custom_h = std::clamp(h, 16, 8192);
        }
        if (ImGui::Button("Usar") || ((edit_w || edit_h) && a.preset < 0)) {
            a.preset = -1;
            changed = true;
        }
        if (changed) saveViewSettings();
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("Proporcion de la vista: Free Aspect ocupa todo el panel; las demas lo recortan como la "
                          "pantalla del juego; una resolucion fija se dibuja a ese tamano y se muestra a escala.");
    ImGui::PopID();
}

void EditorApp::drawSceneDrawModeMenu() {
    static constexpr const char* kModes[] = {"Lit", "Unlit", "Wireframe", "Lit + Wireframe"};
    static constexpr const char* kHelp[] = {
        "Con luz, sombras y efectos (como se vera el juego)",
        "Solo el color de los materiales: sin luz, sombras ni niebla",
        "Solo las lineas de la geometria (mallas, terreno, voxeles, vegetacion)",
        "Con luz y las lineas de las mallas encima"};
    const bool lines = renderer_.wireframeSupported();
    scene_draw_mode_ = std::clamp(scene_draw_mode_, 0, 3);
    ImGui::SetNextItemWidth(ImGui::CalcTextSize("Lit + Wireframe").x + 34.0f);
    if (ImGui::BeginCombo("##draw_mode", kModes[scene_draw_mode_])) {
        for (int i = 0; i < 4; ++i) {
            const bool needs_lines = i >= 2;
            if (ImGui::Selectable(kModes[i], scene_draw_mode_ == i, needs_lines && !lines ? ImGuiSelectableFlags_Disabled : 0)) {
                scene_draw_mode_ = i;
                saveViewSettings();
            }
            ImGui::SetItemTooltip("%s%s", kHelp[i], needs_lines && !lines ? " (tu GPU no dibuja lineas)" : "");
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("Modo de dibujo de la vista Escena (la vista Juego siempre con luz)");
}

// Path tracing (como el de Unreal): un boton que lo enciende y lo apaga. Con
// clic derecho, rebotes y muestras; mientras esta encendido, cuantas lleva.
void EditorApp::drawPathTracingButton() {
    const bool supported = renderer_.rayTracingSupported();
    const bool on = renderer_.pathTracingEnabled();
    ImGui::BeginDisabled(!supported);
    if (toolButton("Path Tracing", on,
                   supported ? "Imagen de referencia con luz fisicamente correcta: rayos que rebotan por la escena real, "
                               "sombras suaves, luz indirecta y reflejos. Se limpia sola mientras la camara esta quieta "
                               "(al moverla vuelve a empezar). Clic derecho: rebotes y muestras."
                             : "Necesita una GPU con trazado de rayos por hardware (Vulkan ray query)")) {
        renderer_.setPathTracingEnabled(!on);
    }
    ImGui::EndDisabled();
    if (ImGui::BeginPopupContextItem("path_tracing_options")) {
        int bounces = static_cast<int>(renderer_.pathTracingBounces());
        if (ImGui::SliderInt("Rebotes", &bounces, 1, 12)) renderer_.setPathTracingBounces(static_cast<std::uint32_t>(bounces));
        ImGui::SetItemTooltip("Cuantas veces rebota la luz. Interiores: 4 a 8; exteriores: 2 a 4.");
        int samples = static_cast<int>(renderer_.pathTracingMaxSamples());
        if (ImGui::SliderInt("Muestras", &samples, 16, 16384, "%d", ImGuiSliderFlags_Logarithmic)) {
            renderer_.setPathTracingMaxSamples(static_cast<std::uint32_t>(samples));
        }
        ImGui::SetItemTooltip("Caminos por pixel hasta dar la imagen por terminada (deja de trazar).");
        if (ImGui::MenuItem("Empezar de nuevo")) renderer_.resetPathTracing();
        ImGui::EndPopup();
    }
    if (on && renderer_.pathTracingActive()) {
        ImGui::SameLine();
        const std::uint32_t done = renderer_.pathTracingSamples();
        const std::uint32_t total = renderer_.pathTracingMaxSamples();
        if (done >= total) ImGui::TextColored(ImVec4(0.35f, 0.85f, 0.55f, 1.0f), "%u/%u", done, total);
        else ImGui::TextDisabled("%u/%u", done, total);
        ImGui::SetItemTooltip("Muestras por pixel acumuladas");
    }
}

void EditorApp::updateViewExtent(float delta_seconds) {
    const std::uint32_t* want = view_desired_[render_view_ & 1];
    if (want[0] == 0 || want[1] == 0) return;  // su panel no se dibujo
    const vk::Extent2D current = renderer_.requestedViewExtent();
    if (current.width == want[0] && current.height == want[1]) {
        pending_view_time_ = 0.0f;
        return;
    }
    if (pending_view_[0] != want[0] || pending_view_[1] != want[1]) {
        pending_view_[0] = want[0];
        pending_view_[1] = want[1];
        pending_view_time_ = 0.0f;
    }
    pending_view_time_ += delta_seconds;
    // Arrastrando el borde de un panel se espera a que pare un momento (cada
    // cambio rehace las imagenes de la escena); si no, al momento.
    if (current.width == 0 || !ImGui::IsMouseDown(ImGuiMouseButton_Left) || pending_view_time_ >= 0.15f) {
        renderer_.setViewExtent(want[0], want[1]);
        pending_view_time_ = 0.0f;
    }
}

void EditorApp::loadViewSettings() {
    view_aspect_[0] = ViewAspect{};
    view_aspect_[1] = ViewAspect{};
    std::ifstream in(project_.settingsFolder() / "EditorViews.ini");
    scene_draw_mode_ = 0;
    std::string key;
    while (in >> key) {
        if (key == "dibujo") {
            in >> scene_draw_mode_;
            scene_draw_mode_ = std::clamp(scene_draw_mode_, 0, 3);
            continue;
        }
        ViewAspect a;
        if (!(in >> a.preset >> a.custom_w >> a.custom_h)) break;
        a.preset = std::clamp(a.preset, -1, kAspectPresetCount - 1);
        a.custom_w = std::clamp(a.custom_w, 16, 8192);
        a.custom_h = std::clamp(a.custom_h, 16, 8192);
        if (key == "escena") view_aspect_[kSceneSlot] = a;
        else if (key == "juego") view_aspect_[kGameSlot] = a;
    }
}

void EditorApp::saveViewSettings() const {
    if (!has_project_) return;
    std::ofstream out(project_.settingsFolder() / "EditorViews.ini");
    const auto line = [&](const char* key, const ViewAspect& a) {
        out << key << ' ' << a.preset << ' ' << a.custom_w << ' ' << a.custom_h << '\n';
    };
    line("escena", view_aspect_[kSceneSlot]);
    line("juego", view_aspect_[kGameSlot]);
    out << "dibujo " << scene_draw_mode_ << '\n';
}

void EditorApp::drawSceneView() {
    if (focus_scene_) {
        ImGui::SetNextWindowFocus();
        focus_scene_ = false;
        preferred_view_ = kSceneSlot;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    const bool open = ImGui::Begin(panelTitle("Escena").c_str(), nullptr,
                                   ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    if (activeWorkspaceKind() == WorkspaceKind::Scene) scene_dock_id_ = ImGui::GetWindowDockID();
    scene_view_visible_ = open;
    overlay_.clear();
    if (!open) {
        flying_ = false;
        flushOverlay();  // vista oculta: sin gizmos
        ImGui::End();
        return;
    }
    view_focused_ = ImGui::IsWindowFocused();
    if (view_focused_ || (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))) {
        preferred_view_ = kSceneSlot;
    }
    drawToolbar();

    // La imagen: todo el panel (Free Aspect) o la proporcion elegida.
    ImVec2 origin;
    const ImVec2 size = layoutViewImage(kSceneSlot, ImGui::GetContentRegionAvail(), origin);
    ImGui::SetCursorScreenPos(origin);
    ImGui::Image(imgui_.viewTexture(kSceneSlot), size);
    view_x_ = origin.x;
    view_y_ = origin.y;
    view_w_ = size.x;
    view_h_ = size.y;
    view_hovered_ = ImGui::IsItemHovered();
    // En Play (o en pausa) un marco de color, como el tinte de Unity.
    if (playing()) {
        const ImU32 frame = play_state_ == PlayState::Paused ? IM_COL32(255, 190, 60, 220) : IM_COL32(80, 170, 255, 220);
        ImGui::GetWindowDrawList()->AddRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), frame, 0.0f, 0, 3.0f);
    }

    // Soltar assets en la escena: en el punto del suelo (y = 0) bajo el
    // raton, o delante de la camara.
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAssetPayload)) {
            AssetPayload asset{};
            std::memcpy(&asset, payload->Data, sizeof(asset));
            if (asset.type == assets::AssetType::Model || asset.type == assets::AssetType::Prefab) {
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
                if (asset.type == assets::AssetType::Prefab) {
                    instantiatePrefabAsset(asset.uuid, {}, place);
                } else {
                    instantiateAsset(asset.uuid, {}, place);
                }
            } else if (asset.type == assets::AssetType::Environment) {
                assignEnvironment(asset.uuid);
            } else if (asset.type == assets::AssetType::Material) {
                // Al objeto (y la parte) bajo el raton: picking por GPU; se
                // aplica al llegar el resultado (finishPick).
                const vk::Extent2D extent = renderer_.sceneExtent();
                const ImVec2 mouse = ImGui::GetMousePos();
                const float u = (mouse.x - view_x_) / view_w_;
                const float v = (mouse.y - view_y_) / view_h_;
                if (u >= 0.0f && u < 1.0f && v >= 0.0f && v < 1.0f && extent.width > 0 && extent.height > 0) {
                    renderer_.requestPick(static_cast<std::uint32_t>(u * static_cast<float>(extent.width)),
                                          static_cast<std::uint32_t>(v * static_cast<float>(extent.height)));
                    pending_pick_ = PendingPick{true, mouse.x, mouse.y, false, asset.uuid};
                }
            } else if (asset.type == assets::AssetType::Scene) {
                if (const auto info = database_->find(asset.uuid)) {
                    runOrAskToSave(PendingAction::OpenScene, info->path);
                }
            }
        }
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kScriptPayload)) {
            const vk::Extent2D extent = renderer_.sceneExtent();
            const ImVec2 mouse = ImGui::GetMousePos();
            const float u = (mouse.x - view_x_) / view_w_;
            const float v = (mouse.y - view_y_) / view_h_;
            if (u >= 0.0f && u < 1.0f && v >= 0.0f && v < 1.0f && extent.width > 0 && extent.height > 0) {
                renderer_.requestPick(static_cast<std::uint32_t>(u * static_cast<float>(extent.width)),
                                      static_cast<std::uint32_t>(v * static_cast<float>(extent.height)));
                pending_pick_ = PendingPick{true, mouse.x, mouse.y, false, {},
                                            assetRelative(dialogs::fromUtf8(static_cast<const char*>(payload->Data)))};
            }
        }
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAudioPayload)) {
            // Un objeto nuevo con el sonido donde se suelta (suelo o delante).
            Vec3 point{};
            Vec3 normal{};
            if (!surfaceHit(ImGui::GetMousePos().x, ImGui::GetMousePos().y, point, normal)) {
                point = scene_.camera().position() + scene_.camera().forward() * 5.0f;
            }
            const std::filesystem::path file = dialogs::fromUtf8(static_cast<const char*>(payload->Data));
            ecs::Entity e = world_.create(dialogs::utf8(file.stem()));
            e.setWorldPosition(point + Vec3{0.0f, 0.5f, 0.0f});
            e.add<audio::AudioSource>().clip = assetRelative(file);
            selectOnly(e.uuid());
            revealInHierarchy(e.uuid());
            commit();
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

    // Con los gizmos apagados solo quedan la navegacion (su propio boton), las
    // herramientas (terreno, estampar) y el gizmo de transformar.
    bool light_handle = false;
    bool waypoint_handle = false;
    bool collider_handle = false;
    if (show_gizmos_) {
        drawSceneOverlays();
        light_handle = drawLightGizmos();
        drawDecalGizmos();
        drawPostVolumeGizmos();
        drawAudioGizmos();
        drawPhysicsGizmos();
        light_handle = drawRigGizmos() || light_handle;
    } else {
        // Un arrastre de asa a medias no puede quedarse enganchado.
        if (light_handle_drag_ != 0 || collider_handle_drag_ != 0) commit();
        light_handle_drag_ = 0;
        collider_handle_drag_ = 0;
        waypoint_drag_ = 0;
    }
    if (activeWorkspaceKind() == WorkspaceKind::Scene) drawNavigationGizmos();
    if (show_gizmos_) {
        const bool cinematic_handle = drawCinematicGizmos();
        const bool water_handle = drawWaterGizmos();
        waypoint_handle = cinematic_handle || water_handle;
        collider_handle = drawColliderHandles();
    }
    // Herramienta de terreno: se queda con el raton mientras pinta.
    const bool terrain_tool = drawTerrainTool(frame_delta_);
    collider_handle = collider_handle || waypoint_handle || terrain_tool;
    const bool stamping = drawStampTool() || drawPrefabPaintTool();
    if (!stamping && collider_handle_drag_ == 0 && !(terrain_edit_ && terrain_tool)) drawGizmo();
    handleCameraControls();

    // Alt + clic: raycast de fisica desde el raton (probador de la ventana
    // Fisica), sin cambiar la seleccion.
    ImGuiIO& io = ImGui::GetIO();
    if (view_hovered_ && io.KeyAlt && raycast_.click_from_mouse && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        Vec3 ray_origin{};
        Vec3 ray_direction{};
        if (mouseRay(io.MousePos.x, io.MousePos.y, ray_origin, ray_direction)) {
            raycast_.mouse_origin = ray_origin;
            raycast_.mouse_direction = ray_direction;
            raycast_.has_mouse_ray = true;
            raycast_.origin = 3;
            raycast_.enabled = true;
            runRaycastTester();
        }
    }
    // Resultado del picking por GPU pedido en un clic anterior.
    if (std::optional<gfx::VulkanRenderer::PickResult> result = renderer_.takePickResult()) {
        finishPick(*result);
    }
    // Seleccionar: clic izquierdo sin arrastrar, fuera del gizmo.
    if (view_hovered_ && !io.KeyAlt && !light_handle && !collider_handle && !stamping && !free_rotate_hover_ &&
        ImGui::IsMouseReleased(ImGuiMouseButton_Left) && !ImGuizmo::IsUsing() &&
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
        if (!io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_G, false)) show_gizmos_ = !show_gizmos_;
        if (ImGui::IsKeyPressed(ImGuiKey_T, false)) {
            stamp_mode_ = !stamp_mode_;
            if (stamp_mode_) paint_mode_ = false;
        }
        if (!io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_B, false)) {
            paint_mode_ = !paint_mode_;
            if (paint_mode_) {
                stamp_mode_ = false;
                show_paint_window_ = true;
            }
        }
        if (ImGui::IsKeyPressed(ImGuiKey_F, false)) focusSelection();
        // Supr con un punto de riel elegido borra el punto, no el objeto.
        if (ImGui::IsKeyPressed(ImGuiKey_Delete) && !deleteSelectedWaypoint()) deleteSelection();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D, false)) duplicateSelection();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C, false)) copySelection();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V, false)) pasteClipboard();
    }

    // Ayuda discreta.
    ImGui::SetCursorScreenPos(ImVec2(origin.x + 10.0f, origin.y + size.y - 24.0f));
    ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 0.5f),
                       "Clic: seleccionar  |  Botón derecho + WASD: volar  |  Rueda: acercar  |  "
                       "Botón central: desplazar  |  F: enfocar  |  Alt+clic: raycast");
    flushOverlay();
    ImGui::End();
}

// Volar con el boton derecho: el cursor capturado (oculto, fijo en el centro
// de la vista, movimiento en bruto) para girar sin tope aunque el raton
// llegue al borde de la vista o de la pantalla. Se llama cada frame; tambien
// suelta el cursor si se dejo de volar desde otro sitio (pestanas, Hub).
void EditorApp::syncFlyCursor() {
    // El Play con Input.lockCursor manda sobre el cursor.
    const bool want = flying_ && !(playing() && window_.cursorCaptured() && !fly_cursor_captured_);
    if (want == fly_cursor_captured_) return;
    if (want) {
        GetCursorPos(&fly_cursor_restore_);
        const RECT area{static_cast<LONG>(view_x_), static_cast<LONG>(view_y_), static_cast<LONG>(view_x_ + view_w_),
                        static_cast<LONG>(view_y_ + view_h_)};
        window_.setCursorCaptured(true, view_w_ > 1.0f ? &area : nullptr);
    } else {
        window_.setCursorCaptured(false);
        SetCursorPos(fly_cursor_restore_.x, fly_cursor_restore_.y);  // donde empezo, como en Unity
    }
    fly_cursor_captured_ = want;
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
    if (view_hovered_ && io.MouseWheel != 0.0f && !(stamp_mode_ && (io.KeyCtrl || io.KeyShift)) && !(paint_mode_ && io.KeyCtrl)) {
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
        if (!show_gizmos_ || icon_hit.valid() || !e.activeInHierarchy()) return;
        if (!e.has<ecs::Light>() && !e.has<ecs::Camera>() && !e.has<ecs::Decal>() && !e.has<physics::ParticleSystem>() &&
            !e.has<water::WaterBody>() &&
            !e.has<cinema::VirtualCamera>() && !e.has<cinema::DollyTrack>() && !e.has<cinema::CinematicSequence>()) {
            return;
        }
        float sx = 0.0f;
        float sy = 0.0f;
        if (worldToScreen(e.worldPosition(), sx, sy) && std::hypot(sx - x, sy - y) < 15.0f) {
            icon_hit = e.uuid();
        }
    });
    if (icon_hit.valid()) {
        choose(icon_hit);
        return;
    }

    // Lo demas: picking por ID en la GPU. El renderizador dibuja los objetos
    // en el pixel del raton y devuelve cual se ve (con su forma real y lo que
    // tapa a lo que); llega unos frames despues (finishPick).
    const vk::Extent2D extent = renderer_.sceneExtent();
    const float u = (x - view_x_) / view_w_;
    const float v = (y - view_y_) / view_h_;
    if (u < 0.0f || u >= 1.0f || v < 0.0f || v >= 1.0f || extent.width == 0 || extent.height == 0) return;
    renderer_.requestPick(static_cast<std::uint32_t>(u * static_cast<float>(extent.width)),
                          static_cast<std::uint32_t>(v * static_cast<float>(extent.height)));
    pending_pick_ = PendingPick{true, x, y, additive};
}

// El resultado del picking por GPU: el objeto bajo el pixel del clic.
void EditorApp::finishPick(const gfx::VulkanRenderer::PickResult& result) {
    if (!pending_pick_.active) return;
    pending_pick_.active = false;
    if (!pending_pick_.script.empty()) {
        const std::string file = pending_pick_.script;
        pending_pick_.script.clear();
        ecs::Entity target = result.hit && sync_ ? sync_->entityForActor(world_, result.actor) : ecs::Entity{};
        // El script va a la raiz del objeto tocado (el modelo entero, como Unity).
        while (target.valid() && target.parent().valid()) target = target.parent();
        if (target.valid()) {
            scripting::Script& s = target.has<scripting::Script>() ? target.get<scripting::Script>() : target.add<scripting::Script>();
            s.file = file;
            selectOnly(target.uuid());
            revealInHierarchy(target.uuid());
            commit();
        }
        return;
    }
    if (pending_pick_.material.valid()) {
        const Uuid material = pending_pick_.material;
        pending_pick_.material = {};
        const ecs::Entity target = result.hit && sync_ ? sync_->entityForActor(world_, result.actor) : ecs::Entity{};
        if (target.valid() && applyMaterial(target, material, static_cast<int>(result.material))) {
            selectOnly(target.uuid());
            revealInHierarchy(target.uuid());
            inline_material_ = material;
            commit();
        }
        return;
    }
    const float x = pending_pick_.x;
    const float y = pending_pick_.y;
    const bool additive = pending_pick_.additive;
    const auto choose = [&](const Uuid& uuid) {
        if (additive) toggleSelection(uuid);
        else selectOnly(uuid);
        revealInHierarchy(uuid);
    };
    ecs::Entity hit = result.hit && sync_ ? sync_->entityForActor(world_, result.actor) : ecs::Entity{};
    // El terreno no es un actor: si no hay nada, se prueba contra los terrenos.
    if (!hit.valid()) hit = terrainUnderMouse(x, y);
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

// Iconos de lo que no tiene malla (luces, camaras, decals, particulas,
// camaras virtuales, rieles), como los gizmos de Unity, y la direccion de las
// luces y camaras seleccionadas.
void EditorApp::drawSceneOverlays() {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->PushClipRect(ImVec2(view_x_, view_y_), ImVec2(view_x_ + view_w_, view_y_ + view_h_), true);
    const auto overlay = [&](ecs::Entity e) {
        if (!e.activeInHierarchy()) return;
        float x = 0.0f;
        float y = 0.0f;
        if (!worldToScreen(e.worldPosition(), x, y)) return;
        const bool selected = isSelected(e.uuid());
        ImU32 tint = IM_COL32_WHITE;
        const Icon icon = entityIcon(e, tint);
        // Las luces, del color de su luz.
        if (const ecs::Light* light = e.tryGet<ecs::Light>()) {
            tint = IM_COL32(static_cast<int>(std::min(light->color.x, 1.0f) * 200 + 55),
                            static_cast<int>(std::min(light->color.y, 1.0f) * 200 + 55),
                            static_cast<int>(std::min(light->color.z, 1.0f) * 200 + 55), 255);
        }
        const float size = selected ? 30.0f : 24.0f;
        draw->AddCircleFilled(ImVec2(x, y), size * 0.62f, IM_COL32(0, 0, 0, selected ? 150 : 95));
        if (selected) draw->AddCircle(ImVec2(x, y), size * 0.62f, IM_COL32(255, 160, 40, 255), 0, 2.0f);
        imgui_.drawIcon(draw, icon, ImVec2(x - size * 0.5f, y - size * 0.5f), size, tint);
        // Direccion de focos, direccionales y camaras seleccionados.
        const ecs::Light* light = e.tryGet<ecs::Light>();
        const bool camera = e.has<ecs::Camera>() || e.has<cinema::VirtualCamera>();
        if (selected && (camera || (light != nullptr && light->type != ecs::LightType::Point))) {
            float x1 = 0.0f;
            float y1 = 0.0f;
            if (worldToScreen(e.worldPosition() + e.forward() * 1.5f, x1, y1)) {
                draw->AddLine(ImVec2(x, y), ImVec2(x1, y1), tint, 2.0f);
            }
        }
    };
    world_.forEachDepthFirst([&](ecs::Entity e) {
        if (e.has<ecs::Light>() || e.has<ecs::Camera>() || e.has<ecs::Decal>() || e.has<physics::ParticleSystem>() ||
            e.has<cinema::VirtualCamera>() || e.has<cinema::DollyTrack>() || e.has<cinema::CinematicSequence>() ||
            e.has<water::WaterBody>()) {
            overlay(e);
        }
    });
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

    // Alcance, conos y rayos en 3D con profundidad (se ve por donde la esfera
    // o el cono atraviesan los objetos). Las asas siguen en 2D: son botones.
    const auto circle = [&](const Vec3& c, const Vec3& u, const Vec3& v, float radius, ImU32 color) {
        overlayCircle(c, u, v, radius, color, false, 64);
    };
    const auto line = [&](const Vec3& a, const Vec3& b, ImU32 color, float) { overlayLine(a, b, color); };

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
    if (gizmo_ != GizmoOperation::Rotate) free_rotate_hover_ = free_rotate_drag_ = false;
    ecs::Entity target = world_.find(active_);
    if (!target.valid() || gizmo_ == GizmoOperation::None || flying_) {
        if (gizmo_was_using_) commit();
        gizmo_was_using_ = false;
        return;
    }
    // ImGuizmo solo para la interaccion: sus colores, transparentes (ImGui no
    // dibuja las primitivas de alfa 0), y los ejes sin invertirse, para que
    // la geometria 3D (drawGizmoGeometry) coincida con lo que se agarra.
    static bool invisible_style = false;
    if (!invisible_style) {
        ImGuizmo::Style& style = ImGuizmo::GetStyle();
        for (ImVec4& color : style.Colors) {
            color.w = 0.0f;
        }
        ImGuizmo::AllowAxisFlip(false);
        ImGuizmo::SetGizmoSizeClipSpace(kGizmoClipSize);
        invisible_style = true;
    }
    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetDrawlist(ImGui::GetWindowDrawList());
    ImGuizmo::SetRect(view_x_, view_y_, view_w_, view_h_);

    // La proyeccion del motor es de Vulkan (Y invertida en m[1][1]); ImGuizmo
    // espera la de OpenGL: se deshace la inversion para que no salga espejado.
    const Mat4 view = scene_.camera().view();
    Mat4 projection = scene_.camera().projection();
    projection.m[1][1] = -projection.m[1][1];

    // Un punto de un riel seleccionado: el gizmo mueve el punto.
    if (drawWaypointGizmo(view, projection)) return;

    ImGuizmo::OPERATION operation = ImGuizmo::TRANSLATE;
    float snap[3] = {snap_translate_, snap_translate_, snap_translate_};
    if (gizmo_ == GizmoOperation::Rotate) {
        // Solo los tres anillos: sin el circulo blanco de la vista, que
        // estorbaba. El giro libre es la bola central (drawFreeRotateHandle).
        operation = static_cast<ImGuizmo::OPERATION>(ImGuizmo::ROTATE_X | ImGuizmo::ROTATE_Y |
                                                     ImGuizmo::ROTATE_Z);
        snap[0] = snap[1] = snap[2] = snap_rotate_;
    } else if (gizmo_ == GizmoOperation::Scale) {
        operation = ImGuizmo::SCALE;
        snap[0] = snap[1] = snap[2] = snap_scale_;
    }
    // La escala solo tiene sentido en local (ImGuizmo lo fuerza igual).
    const ImGuizmo::MODE mode =
        (gizmo_local_ || gizmo_ == GizmoOperation::Scale) ? ImGuizmo::LOCAL : ImGuizmo::WORLD;
    const bool snapping = snap_enabled_ != ImGui::GetIO().KeyCtrl;

    // Giro libre con la bola central (antes que ImGuizmo: si se agarra la
    // bola, los anillos no reciben el clic).
    if (gizmo_ == GizmoOperation::Rotate && drawFreeRotateHandle(target)) {
        drawGizmoGeometry(target.worldMatrix(), mode == ImGuizmo::LOCAL, 1);
        return;
    }

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
    // El gizmo en 3D (con profundidad) donde esta el objeto ahora.
    drawGizmoGeometry(target.worldMatrix(), mode == ImGuizmo::LOCAL,
                      gizmo_ == GizmoOperation::Translate ? 0 : (gizmo_ == GizmoOperation::Rotate ? 1 : 2));

    const bool using_now = ImGuizmo::IsUsing();
    if (gizmo_was_using_ && !using_now) {
        commit();  // se solto el gizmo: un paso de deshacer
    }
    gizmo_was_using_ = using_now;
}

// Bola central del gizmo de rotar: arrastrarla gira el objeto libremente
// alrededor de su centro, en ejes de la camara (horizontal = eje arriba de
// la vista, vertical = eje derecho), como la bola de Blender / Maya. true
// mientras se arrastra (entonces ImGuizmo no recibe el raton).
bool EditorApp::drawFreeRotateHandle(ecs::Entity target) {
    const ImGuiIO& io = ImGui::GetIO();
    const Mat4 start = target.worldMatrix();
    const Vec3 origin{start.m[3][0], start.m[3][1], start.m[3][2]};
    free_rotate_hover_ = false;

    float cx = 0.0f, cy = 0.0f, ex = 0.0f, ey = 0.0f;
    const Mat4 view = scene_.camera().view();
    const Vec3 right = core::normalize(Vec3{view.m[0][0], view.m[1][0], view.m[2][0]});
    const Vec3 up = core::normalize(Vec3{view.m[0][1], view.m[1][1], view.m[2][1]});
    const float size = gizmoWorldSize(origin);
    if (!worldToScreen(origin, cx, cy) || !worldToScreen(origin + right * (kFreeRotateRadius * size), ex, ey)) {
        free_rotate_drag_ = false;
        return false;
    }
    const float radius_px = std::hypot(ex - cx, ey - cy);
    const float mouse_d = std::hypot(io.MousePos.x - cx, io.MousePos.y - cy);

    if (!free_rotate_drag_) {
        // Solo si ImGuizmo no tiene ya un anillo bajo el raton.
        const bool inside = mouse_d <= radius_px && view_hovered_ && !ImGuizmo::IsOver() &&
                            !ImGuizmo::IsUsing();
        free_rotate_hover_ = inside;
        if (inside && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            free_rotate_drag_ = true;
        } else {
            return false;
        }
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        free_rotate_drag_ = false;
        free_rotate_hover_ = true;  // que soltar la bola no seleccione lo de debajo
        commit();  // un paso de deshacer por arrastre
        return false;
    }

    const float dx = io.MouseDelta.x;
    const float dy = io.MouseDelta.y;
    if (dx != 0.0f || dy != 0.0f) {
        constexpr float kDegreesPerPixel = 0.5f;
        const auto axisAngle = [](const Vec3& axis, float degrees) {
            const float half = core::radians(degrees) * 0.5f;
            const Vec3 a = axis * std::sin(half);
            return core::composeTrs(Vec3{}, core::Quat{a.x, a.y, a.z, std::cos(half)}, Vec3{1.0f, 1.0f, 1.0f});
        };
        const Mat4 rotation = axisAngle(up, dx * kDegreesPerPixel) * axisAngle(right, dy * kDegreesPerPixel);
        const Mat4 delta = core::translate(origin) * rotation * core::translate(origin * -1.0f);
        target.setWorldMatrix(delta * start);
        for (ecs::Entity other : topLevelSelection()) {
            if (other == target || other.isAncestorOf(target) || target.isAncestorOf(other)) continue;
            other.setWorldMatrix(delta * other.worldMatrix());
        }
        dirty_ = true;
    }
    return true;
}

}  // namespace cramion::editor
