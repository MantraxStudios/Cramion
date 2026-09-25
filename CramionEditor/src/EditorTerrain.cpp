// Terreno en el editor (como el modo Landscape de Unreal):
//
//   - GameObject > Terreno crea uno (sus datos en Assets/Terrains/*.crterrain).
//   - Con un terreno seleccionado, el Inspector muestra las herramientas y el
//     pincel; en la Escena el pincel sigue el relieve: arrastrar con el clic
//     izquierdo aplica la herramienta (Mayus invierte, Ctrl+clic toma la
//     altura de Aplanar, [ y ] cambian el radio). La rampa va de un clic a
//     otro.
//   - Cada trazo es un paso de deshacer (junto con el resto de acciones).
//   - Las capas se texturizan arrastrando una imagen del Proyecto a su fila.

#include "EditorApp.h"

#include "Dialogs.h"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <ImGuizmo.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>

namespace cramion::editor {

using core::Vec3;

// -----------------------------------------------------------------------------
// Crear y consultar
// -----------------------------------------------------------------------------

ecs::Entity EditorApp::createTerrainEntity() {
    terrain::Terrain comp;
    // Archivo de datos libre en Assets/Terrains.
    std::string relative = "Terrains/Terreno.crterrain";
    for (int i = 2; std::filesystem::exists(project_.assetsFolder() / dialogs::fromUtf8(relative)); ++i) {
        relative = "Terrains/Terreno " + std::to_string(i) + ".crterrain";
    }
    comp.data = relative;
    ecs::Entity entity = world_.create("Terreno");
    // Centrado en el origen, con la superficie en y = 0 y 15 % de margen
    // para cavar (como el Landscape de Unreal, que empieza a media altura).
    const float base = 0.15f;
    entity.setWorldPosition(Vec3{-comp.size * 0.5f, -comp.height * base, -comp.size * 0.5f});
    terrain::Terrain& added = entity.add<terrain::Terrain>();
    added = comp;
    if (const std::shared_ptr<terrain::TerrainData> data = terrain_store_.get(added)) {
        std::fill(data->heights().begin(), data->heights().end(), base);
        data->markAll();
        data->commitCollision();
        terrain_store_.saveAll();
        refreshDatabase();
    }
    selectOnly(entity.uuid());
    revealInHierarchy(entity.uuid());
    terrain_edit_ = true;
    commit();
    return entity;
}

// Terreno bajo el raton (el mas cercano), o Entity{}.
ecs::Entity EditorApp::terrainUnderMouse(float x, float y, Vec3* point) const {
    Vec3 origin{};
    Vec3 direction{};
    if (!mouseRay(x, y, origin, direction)) return {};
    ecs::Entity best;
    float best_distance = 1e30f;
    for (const entt::entity h : world_.registry().view<terrain::Terrain>()) {
        const ecs::Entity e = world_.wrap(h);
        if (!e.activeInHierarchy()) continue;
        const terrain::Terrain& comp = e.get<terrain::Terrain>();
        const std::shared_ptr<terrain::TerrainData> data = terrain_store_.find(comp);
        if (!data) continue;
        Vec3 hit{};
        if (terrain::raycast(*data, comp, e.worldPosition(), origin, direction, 5000.0f, hit)) {
            const float distance = core::length(hit - origin);
            if (distance < best_distance) {
                best_distance = distance;
                best = e;
                if (point != nullptr) *point = hit;
            }
        }
    }
    return best;
}

// -----------------------------------------------------------------------------
// Deshacer de los trazos
// -----------------------------------------------------------------------------

void EditorApp::pushTerrainUndo(const std::string& path, std::shared_ptr<terrain::TerrainData> before) {
    const std::shared_ptr<terrain::TerrainData> current = terrain_store_.findPath(path);
    if (!before || !current) return;
    TerrainUndo step;
    step.path = path;
    step.before = std::move(before);
    step.after = std::make_shared<terrain::TerrainData>(*current);
    terrain_undo_.push_back(std::move(step));
    undo_kinds_.push_back('T');
    // Una accion nueva: no se puede rehacer nada.
    redo_.clear();
    terrain_redo_.clear();
    redo_kinds_.clear();
    // Tope de memoria: 30 trazos.
    while (terrain_undo_.size() > 30) {
        terrain_undo_.pop_front();
        const auto it = std::find(undo_kinds_.begin(), undo_kinds_.end(), 'T');
        if (it != undo_kinds_.end()) undo_kinds_.erase(it);
    }
    dirty_ = true;
    updateTitle();
}

void EditorApp::applyTerrainSnapshot(const std::string& path, const terrain::TerrainData& snapshot) {
    if (const std::shared_ptr<terrain::TerrainData> data = terrain_store_.findPath(path)) {
        *data = snapshot;
        data->markAll();
        data->commitCollision();
    }
}

// -----------------------------------------------------------------------------
// Pincel en la Escena
// -----------------------------------------------------------------------------

// true si la herramienta de terreno se queda con el raton (no seleccionar ni
// mover con el gizmo).
bool EditorApp::drawTerrainTool(float delta_seconds) {
    ecs::Entity entity = world_.find(active_);
    terrain::Terrain* comp = entity.valid() ? entity.tryGet<terrain::Terrain>() : nullptr;
    if (!terrain_edit_ || comp == nullptr || flying_ || playing()) {
        if (terrain_stroke_) {
            terrain_stroke_ = false;
            terrain_stroke_before_.reset();
        }
        return false;
    }
    const std::shared_ptr<terrain::TerrainData> data = terrain_store_.get(*comp);
    if (!data) return false;
    ImGuiIO& io = ImGui::GetIO();
    const Vec3 origin = entity.worldPosition();

    // Radio con [ y ].
    if (view_hovered_ && !io.WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket)) terrain_brush_.radius = std::max(terrain_brush_.radius / 1.2f, 0.5f);
        if (ImGui::IsKeyPressed(ImGuiKey_RightBracket)) terrain_brush_.radius = std::min(terrain_brush_.radius * 1.2f, 500.0f);
    }

    Vec3 hit{};
    Vec3 origin_ray{};
    Vec3 direction{};
    bool on_terrain = false;
    if (view_hovered_ && mouseRay(io.MousePos.x, io.MousePos.y, origin_ray, direction)) {
        on_terrain = terrain::raycast(*data, *comp, origin, origin_ray, direction, 5000.0f, hit);
    }

    // --- Previsualizacion: el circulo del pincel sobre el relieve ---
    const bool ramp = terrain_brush_.tool == terrain::TerrainTool::Ramp;
    if (on_terrain) {
        const auto ring = [&](float radius, std::uint32_t color) {
            constexpr int kSegments = 72;
            Vec3 previous{};
            for (int i = 0; i <= kSegments; ++i) {
                const float a = static_cast<float>(i) / kSegments * 2.0f * core::kPi;
                const float x = hit.x + std::cos(a) * radius;
                const float z = hit.z + std::sin(a) * radius;
                const Vec3 p{x, terrain::heightAt(*data, *comp, origin, x, z) + 0.05f, z};
                if (i > 0) overlayLine(previous, p, color);
                previous = p;
            }
        };
        const bool invert = io.KeyShift;
        const std::uint32_t color = invert ? IM_COL32(255, 110, 90, 255) : IM_COL32(120, 220, 255, 255);
        const float radius = ramp ? terrain_brush_.ramp_width * 0.5f : terrain_brush_.radius;
        ring(radius, color);
        if (!ramp && terrain_brush_.falloff > 0.02f) {
            ring(radius * (1.0f - terrain_brush_.falloff), (color & 0x00FFFFFFu) | 0x80000000u);
        }
        overlayLine(hit, hit + terrain::normalAt(*data, *comp, origin, hit.x, hit.z) * std::max(radius * 0.3f, 0.5f), color);
    }
    // La rampa: su primer punto y la linea hasta el raton.
    if (ramp && terrain_ramp_started_) {
        overlayScreenDisc(terrain_ramp_start_, gizmoWorldSize(terrain_ramp_start_) * 0.04f, IM_COL32(255, 220, 90, 255));
        if (on_terrain) overlayLine(terrain_ramp_start_, hit, IM_COL32(255, 220, 90, 255));
    }

    const bool clicked = on_terrain && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGuizmo::IsOver() && !io.KeyAlt;
    // Ctrl+clic con Aplanar: toma la altura del terreno.
    if (clicked && io.KeyCtrl && terrain_brush_.tool == terrain::TerrainTool::Flatten) {
        terrain_brush_.target_height = hit.y;
        return true;
    }
    if (ramp) {
        if (clicked) {
            if (!terrain_ramp_started_) {
                terrain_ramp_start_ = hit;
                terrain_ramp_started_ = true;
            } else {
                auto before = std::make_shared<terrain::TerrainData>(*data);
                if (terrain::applyRamp(*data, *comp, origin, terrain_ramp_start_, hit, terrain_brush_)) {
                    data->commitCollision();
                    pushTerrainUndo(comp->data, std::move(before));
                }
                terrain_ramp_started_ = false;
            }
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) terrain_ramp_started_ = false;
        return view_hovered_;
    }

    // --- Trazo ---
    if (clicked && !terrain_stroke_) {
        terrain_stroke_ = true;
        terrain_stroke_before_ = std::make_shared<terrain::TerrainData>(*data);
    }
    if (terrain_stroke_) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if (on_terrain) {
                terrain::applyBrush(*data, *comp, origin, hit, terrain_brush_, std::min(delta_seconds, 0.05f), io.KeyShift);
                dirty_ = true;
            }
        } else {
            // Fin del trazo: colision al dia y un paso de deshacer.
            terrain_stroke_ = false;
            data->commitCollision();
            pushTerrainUndo(comp->data, std::move(terrain_stroke_before_));
        }
    }
    return view_hovered_ && (on_terrain || terrain_stroke_);
}

// -----------------------------------------------------------------------------
// Inspector
// -----------------------------------------------------------------------------

void EditorApp::drawTerrainInspector(ecs::Entity entity) {
    terrain::Terrain& comp = entity.get<terrain::Terrain>();
    const std::shared_ptr<terrain::TerrainData> data = terrain_store_.get(comp);
    ImGui::SeparatorText("Herramientas");
    if (ImGui::Checkbox("Editar terreno", &terrain_edit_)) terrain_ramp_started_ = false;
    ImGui::SameLine();
    ImGui::TextDisabled("(Mayus invierte, [ ] radio)");
    if (data) {
        ImGui::TextDisabled("%u x %u alturas  |  %u x %u pintura  |  %.0f m x %.0f m", data->resolution(), data->resolution(),
                            data->splatResolution(), data->splatResolution(), comp.size, comp.size);
    }
    // Paleta: dos columnas de botones.
    const float half = (ImGui::GetContentRegionAvail().x - 4.0f) * 0.5f;
    for (int i = 0; i < terrain::kTerrainToolCount; ++i) {
        const auto tool = static_cast<terrain::TerrainTool>(i);
        const bool active = terrain_brush_.tool == tool;
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_Header));
        if (ImGui::Button(terrain::toolName(tool), ImVec2(half, 0.0f))) {
            terrain_brush_.tool = tool;
            terrain_edit_ = true;
            terrain_ramp_started_ = false;
        }
        if (active) ImGui::PopStyleColor();
        if (i % 2 == 0) ImGui::SameLine();
    }

    ImGui::SeparatorText("Pincel");
    const terrain::TerrainTool tool = terrain_brush_.tool;
    if (tool == terrain::TerrainTool::Ramp) {
        ImGui::DragFloat("Ancho", &terrain_brush_.ramp_width, 0.1f, 0.5f, 200.0f, "%.1f m");
        ImGui::DragFloat("Caida del borde", &terrain_brush_.falloff, 0.01f, 0.0f, 1.0f, "%.2f");
        ImGui::TextDisabled(terrain_ramp_started_ ? "Clic en el punto final" : "Clic en el punto inicial (Esc cancela)");
    } else {
        ImGui::DragFloat("Radio", &terrain_brush_.radius, 0.1f, 0.5f, 500.0f, "%.1f m");
        ImGui::SliderFloat("Fuerza", &terrain_brush_.strength, 0.01f, 1.0f, "%.2f");
        ImGui::SliderFloat("Caida", &terrain_brush_.falloff, 0.0f, 1.0f, "%.2f");
    }
    if (tool == terrain::TerrainTool::Flatten) {
        ImGui::DragFloat("Altura", &terrain_brush_.target_height, 0.05f, -10000.0f, 10000.0f, "%.2f m");
        ImGui::TextDisabled("Ctrl+clic en el terreno toma su altura");
    }
    if (tool == terrain::TerrainTool::Noise) {
        ImGui::DragFloat("Escala del ruido", &terrain_brush_.noise_scale, 0.001f, 0.001f, 2.0f, "%.3f");
        int seed = static_cast<int>(terrain_brush_.seed);
        if (ImGui::DragInt("Semilla", &seed)) terrain_brush_.seed = static_cast<std::uint32_t>(seed);
    }
    if (tool == terrain::TerrainTool::Terrace) {
        ImGui::DragFloat("Paso", &terrain_brush_.terrace_step, 0.05f, 0.1f, 200.0f, "%.2f m");
        ImGui::SliderFloat("Filo", &terrain_brush_.terrace_sharpness, 0.0f, 1.0f, "%.2f");
    }
    if (tool == terrain::TerrainTool::Paint) {
        ImGui::TextUnformatted("Capa que se pinta:");
        for (std::size_t i = 0; i < comp.layers.size() && i < static_cast<std::size_t>(terrain::kMaxLayers); ++i) {
            const terrain::TerrainLayer& layer = comp.layers[i];
            ImGui::PushID(static_cast<int>(i));
            const ImVec4 color(layer.tint.x, layer.tint.y, layer.tint.z, 1.0f);
            if (ImGui::ColorButton("##c", color, ImGuiColorEditFlags_NoTooltip, ImVec2(18, 18))) terrain_brush_.layer = static_cast<int>(i);
            ImGui::SameLine();
            if (ImGui::Selectable(layer.name.c_str(), terrain_brush_.layer == static_cast<int>(i))) {
                terrain_brush_.layer = static_cast<int>(i);
            }
            ImGui::PopID();
        }
    }

    // --- Texturas de las capas (arrastrar una imagen del Proyecto) ---
    ImGui::SeparatorText("Texturas de las capas");
    for (std::size_t i = 0; i < comp.layers.size(); ++i) {
        terrain::TerrainLayer& layer = comp.layers[i];
        ImGui::PushID(static_cast<int>(i) + 1000);
        ImGui::AlignTextToFramePadding();
        ImGui::Text("%zu. %s", i, layer.name.c_str());
        ImGui::SameLine(120.0f);
        const auto slot = [&](const char* label, std::string& path) {
            const std::string text = path.empty() ? std::string(label) : std::filesystem::path(path).filename().string();
            if (ImGui::Button(text.c_str(), ImVec2(ImGui::GetContentRegionAvail().x * 0.48f, 0.0f))) {
                const std::string chosen = importDecalImage();  // copia la imagen a Assets/Textures
                if (!chosen.empty()) {
                    path = chosen;
                    commit();
                }
            }
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kImagePayload)) {
                    path = decalImageInAssets(dialogs::fromUtf8(static_cast<const char*>(payload->Data)));
                    commit();
                }
                ImGui::EndDragDropTarget();
            }
            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("Quitar")) {
                    path.clear();
                    commit();
                }
                ImGui::EndPopup();
            }
            ImGui::SetItemTooltip("Arrastra aqui una imagen del Proyecto o haz clic para elegirla (clic derecho: quitar)");
        };
        slot("Textura...", layer.albedo);
        ImGui::SameLine();
        slot("Normal...", layer.normal);
        ImGui::PopID();
    }

    // --- Generar ---
    if (ImGui::CollapsingHeader("Generar relieve")) {
        ImGui::DragInt("Semilla##gen", &terrain_generate_.seed);
        ImGui::DragFloat("Frecuencia", &terrain_generate_.frequency, 0.05f, 0.2f, 40.0f, "%.2f");
        ImGui::SliderFloat("Rugosidad", &terrain_generate_.roughness, 0.1f, 0.9f, "%.2f");
        ImGui::SliderFloat("Crestas (montanas)", &terrain_generate_.ridges, 0.0f, 1.0f, "%.2f");
        ImGui::SliderFloat("Altura base", &terrain_generate_.base, 0.0f, 0.9f, "%.2f");
        if (ImGui::Button("Generar", ImVec2(-1.0f, 0.0f)) && data) {
            auto before = std::make_shared<terrain::TerrainData>(*data);
            terrain::generateRelief(*data, static_cast<std::uint32_t>(terrain_generate_.seed), terrain_generate_.frequency,
                                    terrain_generate_.roughness, terrain_generate_.ridges, terrain_generate_.base);
            pushTerrainUndo(comp.data, std::move(before));
        }
    }
    if (ImGui::CollapsingHeader("Pintar por reglas")) {
        ImGui::SliderInt("Capa##rules", &terrain_rules_.layer, 0, std::max(static_cast<int>(comp.layers.size()) - 1, 0));
        ImGui::DragFloatRange2("Pendiente", &terrain_rules_.min_slope, &terrain_rules_.max_slope, 0.5f, 0.0f, 90.0f,
                               "%.0f°", "%.0f°");
        ImGui::DragFloatRange2("Altura (0-1)", &terrain_rules_.min_height, &terrain_rules_.max_height, 0.005f, 0.0f, 1.0f);
        if (ImGui::Button("Pintar", ImVec2(-1.0f, 0.0f)) && data) {
            auto before = std::make_shared<terrain::TerrainData>(*data);
            terrain::paintByRules(*data, comp, terrain_rules_.layer, terrain_rules_.min_slope, terrain_rules_.max_slope,
                                  terrain_rules_.min_height, terrain_rules_.max_height);
            pushTerrainUndo(comp.data, std::move(before));
        }
        ImGui::TextDisabled("Ej.: roca en pendientes de 35 a 90°, nieve por encima de 0.8");
    }
}

}  // namespace cramion::editor
