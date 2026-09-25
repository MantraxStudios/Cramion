// Interfaz del juego en el editor (como el diseñador de UMG): se ve y se usa
// en la vista Juego; fuera de Play, clic selecciona un elemento, arrastrar lo
// mueve y las esquinas le cambian el tamano. Menu GameObject > UI para crear
// Canvas, panel, imagen, texto, boton, slider, campo de texto y casilla, y
// anclas predefinidas en el Inspector del Rect Transform.

#include "EditorApp.h"

#include "UiRenderer.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>

namespace cramion::editor {

namespace {
constexpr ImU32 kSelection = IM_COL32(255, 160, 40, 255);
}

void EditorApp::drawGameUi(ImVec2 origin, ImVec2 size) {
    const bool hovered = ImGui::IsWindowHovered();
    const bool running = play_state_ == PlayState::Playing;
    const ui::UiInput input = uiInputFromImGui(origin, size, hovered && running, running && ui_.typing());
    ui_.update(world_, size.x, size.y, input, running, static_cast<float>(ImGui::GetTime()));
    for (const ui::UiEvent& e : ui_.takeEvents()) {
        switch (e.kind) {
            case ui::UiEvent::Kind::Click: scripts_.callMethod(e.target, e.method, e.source); break;
            case ui::UiEvent::Kind::Number: scripts_.callMethod(e.target, e.method, e.number); break;
            case ui::UiEvent::Kind::Text: scripts_.callMethod(e.target, e.method, e.text); break;
            case ui::UiEvent::Kind::Bool: scripts_.callMethod(e.target, e.method, e.flag); break;
        }
    }
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->PushClipRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), true);
    drawUiList(draw, origin, ui_.drawList(), imgui_, project_.assetsFolder());

    // --- Diseñador (fuera de Play) ---
    if (!playing()) {
        const ImGuiIO& io = ImGui::GetIO();
        const float mx = io.MousePos.x - origin.x;
        const float my = io.MousePos.y - origin.y;
        const ecs::Entity selected = world_.find(active_);
        ui::UiRect sel_rect;
        const bool has_rect = selected.valid() && selected.has<ui::RectTransform>() && ui_.rectOf(selected.handle(), sel_rect);
        int hovered_handle = -1;  // 0 mover, 1..4 esquinas
        if (has_rect) {
            const ImVec2 a{origin.x + sel_rect.x, origin.y + sel_rect.y};
            const ImVec2 b{a.x + sel_rect.w, a.y + sel_rect.h};
            draw->AddRect(a, b, kSelection, 0.0f, 0, 2.0f);
            const ImVec2 corners[4] = {a, ImVec2(b.x, a.y), b, ImVec2(a.x, b.y)};
            for (int i = 0; i < 4; ++i) {
                const bool over = hovered && std::abs(io.MousePos.x - corners[i].x) < 7.0f && std::abs(io.MousePos.y - corners[i].y) < 7.0f;
                if (over) hovered_handle = i + 1;
                draw->AddRectFilled(ImVec2(corners[i].x - 5, corners[i].y - 5), ImVec2(corners[i].x + 5, corners[i].y + 5),
                                    over ? IM_COL32(255, 220, 120, 255) : IM_COL32(255, 255, 255, 255));
                draw->AddRect(ImVec2(corners[i].x - 5, corners[i].y - 5), ImVec2(corners[i].x + 5, corners[i].y + 5), kSelection);
            }
            // Anclas: triangulitos donde estan (sobre el rectangulo del padre).
            const ui::RectTransform& rt = selected.get<ui::RectTransform>();
            ui::UiRect parent{0.0f, 0.0f, size.x, size.y};
            if (const ecs::Entity p = selected.parent(); p.valid()) ui_.rectOf(p.handle(), parent);
            for (int i = 0; i < 4; ++i) {
                const float ax = i % 2 == 0 ? rt.anchor_min.x : rt.anchor_max.x;
                const float ay = i < 2 ? rt.anchor_min.y : rt.anchor_max.y;
                const ImVec2 p{origin.x + parent.x + parent.w * ax, origin.y + parent.y + parent.h * ay};
                draw->AddTriangleFilled(p, ImVec2(p.x - 6, p.y - 10), ImVec2(p.x + 6, p.y - 10), IM_COL32(120, 200, 255, 230));
            }
            if (hovered && hovered_handle < 0 && sel_rect.contains(mx, my)) hovered_handle = 0;
        }
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            if (hovered_handle >= 1 || (hovered_handle == 0 && has_rect)) {
                const ui::RectTransform& rt = selected.get<ui::RectTransform>();
                ui_drag_ = UiDrag{true, selected.uuid(), hovered_handle, io.MousePos, rt.position, rt.size,
                                  ui_.scaleOf(selected.handle())};
            } else {
                const entt::entity hit = ui_.pick(mx, my);
                if (hit != entt::null) {
                    ecs::Entity e = world_.wrap(hit);
                    selectOnly(e.uuid());
                    revealInHierarchy(e.uuid());
                    if (e.has<ui::RectTransform>()) {
                        const ui::RectTransform& rt = e.get<ui::RectTransform>();
                        ui_drag_ = UiDrag{true, e.uuid(), 0, io.MousePos, rt.position, rt.size, ui_.scaleOf(hit)};
                    }
                }
            }
        }
        if (ui_drag_.active) {
            ecs::Entity e = world_.find(ui_drag_.entity);
            ui::RectTransform* rt = e.valid() ? e.tryGet<ui::RectTransform>() : nullptr;
            if (rt == nullptr || !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                if (rt != nullptr && (rt->position.x != ui_drag_.start_position.x || rt->position.y != ui_drag_.start_position.y ||
                                      rt->size.x != ui_drag_.start_size.x || rt->size.y != ui_drag_.start_size.y)) {
                    commit();
                }
                ui_drag_.active = false;
            } else {
                const float s = std::max(ui_drag_.scale, 1e-3f);
                const core::Vec2 d{(io.MousePos.x - ui_drag_.start_mouse.x) / s, (io.MousePos.y - ui_drag_.start_mouse.y) / s};
                if (ui_drag_.mode == 0) {
                    rt->position = core::Vec2{ui_drag_.start_position.x + d.x, ui_drag_.start_position.y + d.y};
                } else {
                    // Esquina: crece hacia ese lado; la opuesta se queda quieta.
                    const float sx = (ui_drag_.mode == 2 || ui_drag_.mode == 3) ? 1.0f : -1.0f;
                    const float sy = (ui_drag_.mode == 3 || ui_drag_.mode == 4) ? 1.0f : -1.0f;
                    const core::Vec2 grow{d.x * sx, d.y * sy};
                    rt->size = core::Vec2{std::max(ui_drag_.start_size.x + grow.x, 4.0f - (rt->anchor_max.x - rt->anchor_min.x) * 1e4f),
                                          std::max(ui_drag_.start_size.y + grow.y, 4.0f - (rt->anchor_max.y - rt->anchor_min.y) * 1e4f)};
                    const float gx = rt->size.x - ui_drag_.start_size.x;
                    const float gy = rt->size.y - ui_drag_.start_size.y;
                    rt->position = core::Vec2{ui_drag_.start_position.x + gx * (sx > 0 ? rt->pivot.x : rt->pivot.x - 1.0f),
                                              ui_drag_.start_position.y + gy * (sy > 0 ? rt->pivot.y : rt->pivot.y - 1.0f)};
                }
            }
        }
        if (ui_.drawList().empty() && world_.registry().view<ui::Canvas>().empty()) {
            // Nada
        }
    }
    draw->PopClipRect();
}

// Canvas raiz (o uno nuevo) donde colgar un elemento nuevo.
ecs::Entity EditorApp::uiParentForNewElement() {
    // Lo seleccionado, si esta dentro de un Canvas.
    for (ecs::Entity e = world_.find(active_); e.valid(); e = e.parent()) {
        if (e.has<ui::Canvas>()) return world_.find(active_);
    }
    ecs::Entity canvas;
    world_.forEachDepthFirst([&](ecs::Entity e) {
        if (!canvas.valid() && e.has<ui::Canvas>()) canvas = e;
    });
    if (!canvas.valid()) {
        canvas = world_.create("Canvas");
        canvas.add<ui::Canvas>();
    }
    return canvas;
}

ecs::Entity EditorApp::createUiElement(int kind) {
    if (kind == 0) {
        ecs::Entity canvas = world_.create("Canvas");
        canvas.add<ui::Canvas>();
        selectOnly(canvas.uuid());
        revealInHierarchy(canvas.uuid());
        commit();
        return canvas;
    }
    static constexpr const char* kNames[] = {"Canvas", "Panel", "Imagen", "Texto", "Boton", "Slider", "Campo de texto", "Casilla"};
    const ecs::Entity parent = uiParentForNewElement();
    ecs::Entity e = world_.create(kNames[std::clamp(kind, 0, 7)], parent);
    ui::RectTransform& rt = e.add<ui::RectTransform>();
    const auto label = [&](const char* text, float font_size, ui::HAlign align) {
        ecs::Entity t = world_.create("Texto", e);
        ui::RectTransform& trt = t.add<ui::RectTransform>();
        trt.anchor_min = core::Vec2{0.0f, 0.0f};
        trt.anchor_max = core::Vec2{1.0f, 1.0f};
        trt.size = core::Vec2{0.0f, 0.0f};
        ui::Text& text_component = t.add<ui::Text>();
        text_component.text = text;
        text_component.font_size = font_size;
        text_component.h_align = align;
        return t;
    };
    switch (kind) {
        case 1: {  // Panel: estirado, oscuro translucido
            rt.anchor_min = core::Vec2{0.0f, 0.0f};
            rt.anchor_max = core::Vec2{1.0f, 1.0f};
            rt.size = core::Vec2{-80.0f, -80.0f};
            ui::Image& image = e.add<ui::Image>();
            image.color = core::Vec3{0.08f, 0.09f, 0.11f};
            image.alpha = 0.85f;
            image.corner_radius = 12.0f;
            break;
        }
        case 2:
            rt.size = core::Vec2{200.0f, 200.0f};
            e.add<ui::Image>();
            break;
        case 3: {
            rt.size = core::Vec2{400.0f, 80.0f};
            e.add<ui::Text>().text = "Nuevo texto";
            break;
        }
        case 4:
            rt.size = core::Vec2{260.0f, 70.0f};
            e.add<ui::Button>();
            label("Boton", 30.0f, ui::HAlign::Center);
            break;
        case 5:
            rt.size = core::Vec2{360.0f, 36.0f};
            e.add<ui::Slider>();
            break;
        case 6:
            rt.size = core::Vec2{420.0f, 60.0f};
            e.add<ui::InputField>();
            break;
        case 7: {
            rt.size = core::Vec2{260.0f, 44.0f};
            e.add<ui::Toggle>();
            ecs::Entity t = label("Casilla", 26.0f, ui::HAlign::Left);
            ui::RectTransform& trt = t.get<ui::RectTransform>();
            trt.position = core::Vec2{56.0f, 0.0f};
            trt.size = core::Vec2{-56.0f, 0.0f};
            trt.pivot = core::Vec2{0.0f, 0.5f};
            break;
        }
        default:
            break;
    }
    selectOnly(e.uuid());
    revealInHierarchy(e.uuid());
    commit();
    return e;
}

void EditorApp::drawUiCreateMenu() {
    if (!ImGui::BeginMenu("UI")) return;
    static constexpr const char* kItems[] = {"Canvas", "Panel", "Imagen", "Texto", "Botón", "Slider", "Campo de texto", "Casilla"};
    for (int i = 0; i < 8; ++i) {
        if (ImGui::MenuItem(kItems[i])) createUiElement(i);
    }
    ImGui::EndMenu();
}

// Anclas predefinidas (como las de UMG / Unity): arriba/centro/abajo por
// izquierda/centro/derecha, y estirar en horizontal, vertical o ambos.
void EditorApp::drawRectTransformInspector(ecs::Entity entity) {
    ui::RectTransform* rt = entity.tryGet<ui::RectTransform>();
    if (rt == nullptr) return;
    ImGui::TextDisabled("Anclas (Mayus: tambien el pivote y la posicion)");
    const float cell = 34.0f;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    // Columnas: izq, centro, der, estirar; filas: arriba, medio, abajo, estirar.
    static constexpr float kMin[4] = {0.0f, 0.5f, 1.0f, 0.0f};
    static constexpr float kMax[4] = {0.0f, 0.5f, 1.0f, 1.0f};
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            if (col > 0) ImGui::SameLine();
            ImGui::PushID(row * 4 + col);
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const bool current = rt->anchor_min.x == kMin[col] && rt->anchor_max.x == kMax[col] &&
                                 rt->anchor_min.y == kMin[row] && rt->anchor_max.y == kMax[row];
            if (ImGui::InvisibleButton("##preset", ImVec2(cell, cell))) {
                rt->anchor_min = core::Vec2{kMin[col], kMin[row]};
                rt->anchor_max = core::Vec2{kMax[col], kMax[row]};
                if (ImGui::GetIO().KeyShift) {
                    rt->pivot = core::Vec2{col == 3 ? 0.5f : kMin[col], row == 3 ? 0.5f : kMin[row]};
                    rt->position = core::Vec2{0.0f, 0.0f};
                }
                if (col == 3) rt->size.x = ImGui::GetIO().KeyShift ? 0.0f : std::min(rt->size.x, 0.0f);
                if (row == 3) rt->size.y = ImGui::GetIO().KeyShift ? 0.0f : std::min(rt->size.y, 0.0f);
                commit();
            }
            const bool over = ImGui::IsItemHovered();
            draw->AddRectFilled(p, ImVec2(p.x + cell, p.y + cell),
                                current ? IM_COL32(40, 90, 160, 255) : (over ? IM_COL32(70, 70, 80, 255) : IM_COL32(45, 45, 52, 255)), 4.0f);
            // Dibujito: el rectangulo y donde se ancla.
            const ImVec2 a{p.x + 7, p.y + 7};
            const ImVec2 b{p.x + cell - 7, p.y + cell - 7};
            draw->AddRect(a, b, IM_COL32(150, 150, 160, 255));
            const float x0 = a.x + (b.x - a.x) * kMin[col];
            const float x1 = a.x + (b.x - a.x) * kMax[col];
            const float y0 = a.y + (b.y - a.y) * kMin[row];
            const float y1 = a.y + (b.y - a.y) * kMax[row];
            if (col == 3 || row == 3) {
                draw->AddRectFilled(ImVec2(x0 - 2, y0 - 2), ImVec2(x1 + 2, y1 + 2), IM_COL32(255, 160, 60, 200));
            } else {
                draw->AddCircleFilled(ImVec2(x0, y0), 3.0f, IM_COL32(255, 160, 60, 255));
            }
            ImGui::PopID();
        }
    }
}

}  // namespace cramion::editor
