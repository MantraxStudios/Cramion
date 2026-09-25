// Ventana Jerarquia (como la de Unity): el arbol del mundo en orden, con
// multiseleccion, arrastrar para emparentar o reordenar, renombrar en el
// sitio, activo atenuado, busqueda y menu Crear.

#include "EditorApp.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <cstring>

namespace cramion::editor {

namespace {


std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// Donde cae lo arrastrado respecto a una fila: antes, dentro o despues.
enum class DropZone { Before, Inside, After };

DropZone dropZone() {
    const float top = ImGui::GetItemRectMin().y;
    const float height = ImGui::GetItemRectSize().y;
    const float y = ImGui::GetMousePos().y - top;
    if (y < height * 0.25f) return DropZone::Before;
    if (y > height * 0.75f) return DropZone::After;
    return DropZone::Inside;
}

}  // namespace

Icon EditorApp::entityIcon(const ecs::Entity& e, ImU32& tint) const {
    tint = IM_COL32(200, 200, 205, 255);
    if (const ecs::Light* light = e.tryGet<ecs::Light>()) {
        tint = IM_COL32(255, 205, 80, 255);
        return light->type == ecs::LightType::Directional ? Icon::DirectionalLight
               : light->type == ecs::LightType::Spot      ? Icon::SpotLight
                                                          : Icon::PointLight;
    }
    if (e.has<ecs::Camera>()) {
        tint = IM_COL32(120, 220, 140, 255);
        return Icon::Camera;
    }
    if (e.has<terrain::Terrain>()) {
        tint = IM_COL32(140, 200, 110, 255);
        return Icon::Terrain;
    }
    if (e.has<cinema::VirtualCamera>()) {
        tint = IM_COL32(120, 190, 255, 255);
        return Icon::Camera;
    }
    if (e.has<cinema::DollyTrack>() || e.has<cinema::DollyCart>()) {
        tint = IM_COL32(255, 196, 64, 255);
        return Icon::Waypoint;
    }
    if (e.has<cinema::CinematicSequence>()) {
        tint = IM_COL32(255, 120, 120, 255);
        return Icon::Camera;
    }
    if (e.has<physics::ParticleSystem>()) {
        tint = IM_COL32(255, 160, 90, 255);
        return Icon::ParticleSystem;
    }
    if (e.has<ecs::Decal>()) {
        tint = IM_COL32(255, 170, 60, 255);
        return Icon::Decal;
    }
    if (e.has<physics::Rigidbody>()) {
        tint = IM_COL32(145, 244, 139, 255);
        return Icon::Rigidbody;
    }
    if (const physics::BoxCollider* box = e.tryGet<physics::BoxCollider>(); box != nullptr && !e.has<ecs::MeshRenderer>()) {
        tint = box->material.is_trigger ? IM_COL32(110, 190, 255, 255) : IM_COL32(145, 244, 139, 255);
        return box->material.is_trigger ? Icon::TriggerVolume : Icon::ColliderBox;
    }
    if (e.has<ecs::Animator>()) {
        tint = IM_COL32(110, 170, 255, 255);
        return Icon::SkinnedMesh;
    }
    if (e.has<ecs::MeshRenderer>()) {
        tint = IM_COL32(110, 170, 255, 255);
        return Icon::MeshRenderer;
    }
    if (e.has<ecs::Sky>() || e.has<ecs::PostProcessing>() || e.has<ecs::Weather>()) {
        tint = IM_COL32(120, 220, 230, 255);
        return Icon::AmbientLight;
    }
    return e.childCount() > 0 ? Icon::FolderClosed : Icon::AssetBrowser;
}

void EditorApp::drawHierarchy() {
    if (!ImGui::Begin("Jerarquía", &show_hierarchy_)) {
        ImGui::End();
        return;
    }

    // Barra: crear y buscar.
    if (ImGui::Button("+")) {
        ImGui::OpenPopup("crear_menu");
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##buscar", "Buscar...", &hierarchy_filter_);
    if (ImGui::BeginPopup("crear_menu")) {
        const ecs::Entity parent = world_.find(active_);
        const auto item = [&](const char* label, int kind) {
            if (ImGui::MenuItem(label)) createEntity(kind, parent);
        };
        item("Vacío", 0);
        item("Cubo", 1);
        item("Esfera", 2);
        item("Plano", 3);
        item("Cilindro", 4);
        item("Cápsula", 5);
        ImGui::Separator();
        item("Luz direccional", 6);
        item("Luz puntual", 7);
        item("Foco", 8);
        item("Cámara", 9);
        ImGui::Separator();
        item("Decal (estampa)", 10);
        item("Charco", 11);
        item("Humedad", 12);
        ImGui::Separator();
        if (ImGui::MenuItem("Terreno")) createTerrainEntity();
        ImGui::EndPopup();
    }
    ImGui::Separator();

    ImGui::BeginChild("tree", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);

    // Revelar: abrir los padres de la entidad elegida en la vista.
    if (reveal_.valid()) {
        const ecs::Entity target = world_.find(reveal_);
        if (!target.valid()) reveal_ = {};
    }

    // Solo se dibujan las filas que se ven (ImGuiListClipper): con miles de
    // objetos el coste es el de las ~40 filas de la ventana, no el del mundo.
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f, 1.0f));
    buildHierarchyRows();
    if (!hierarchy_filter_.empty()) {
        // Busqueda: lista plana de coincidencias (como Unity).
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(hierarchy_rows_.size()));
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const entt::entity handle = hierarchy_rows_[static_cast<std::size_t>(i)].entity;
                if (!world_.valid(handle)) continue;
                const ecs::Entity e = world_.wrap(handle);
                const Uuid uuid = e.uuid();
                ImGui::PushID(static_cast<int>(entt::to_integral(handle)));
                if (ImGui::Selectable(e.name().c_str(), isSelected(uuid))) {
                    if (ImGui::GetIO().KeyCtrl) toggleSelection(uuid);
                    else selectOnly(uuid);
                }
                ImGui::PopID();
            }
        }
    } else {
        // La fila que se renombra se dibuja siempre (su campo de texto) y la
        // que se revela se lleva a la vista.
        const auto row_of = [&](const Uuid& uuid) {
            const ecs::Entity target = uuid.valid() ? world_.find(uuid) : ecs::Entity{};
            for (std::size_t i = 0; target.valid() && i < hierarchy_rows_.size(); ++i) {
                if (hierarchy_rows_[i].entity == target.handle()) return static_cast<int>(i);
            }
            return -1;
        };
        const int renaming_row = row_of(renaming_);
        const int reveal_row = row_of(reveal_);
        const std::vector<HierarchyRow> rows = hierarchy_rows_;  // copia: una fila puede cambiar el mundo
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(rows.size()));
        if (renaming_row >= 0) clipper.IncludeItemByIndex(renaming_row);
        if (reveal_row >= 0) clipper.IncludeItemByIndex(reveal_row);
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                drawHierarchyRow(rows[static_cast<std::size_t>(i)], i == reveal_row);
            }
        }
    }
    ImGui::PopStyleVar();
    reveal_ = {};

    // Espacio vacio debajo: soltar = raiz al final; clic = deseleccionar;
    // clic derecho = crear.
    const ImVec2 rest = ImGui::GetContentRegionAvail();
    ImGui::InvisibleButton("##empty", ImVec2(std::max(rest.x, 1.0f), std::max(rest.y, 40.0f)));
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
        clearSelection();
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kEntityPayload)) {
            (void)payload;
            for (ecs::Entity e : topLevelSelection()) {
                e.setParent({}, true, -1);
            }
            commit();
        }
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAssetPayload)) {
            AssetPayload asset{};
            std::memcpy(&asset, payload->Data, sizeof(asset));
            if (asset.type == assets::AssetType::Model) instantiateAsset(asset.uuid, {}, std::nullopt);
            if (asset.type == assets::AssetType::Environment) assignEnvironment(asset.uuid);
        }
        ImGui::EndDragDropTarget();
    }
    if (ImGui::BeginPopupContextItem("##empty_menu")) {
        const auto item = [&](const char* label, int kind) {
            if (ImGui::MenuItem(label)) createEntity(kind, {});
        };
        item("Crear vacío", 0);
        item("Cubo", 1);
        item("Esfera", 2);
        item("Plano", 3);
        item("Luz puntual", 7);
        item("Foco", 8);
        item("Cámara", 9);
        ImGui::Separator();
        item("Decal (estampa)", 10);
        item("Charco", 11);
        item("Humedad", 12);
        ImGui::Separator();
        if (ImGui::MenuItem("Terreno")) createTerrainEntity();
        if (!clipboard_.empty() && ImGui::MenuItem("Pegar")) pasteClipboard();
        ImGui::EndPopup();
    }
    ImGui::EndChild();

    // Atajos de la jerarquia (con la ventana enfocada).
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) && !ImGui::GetIO().WantTextInput) {
        ImGuiIO& io = ImGui::GetIO();
        if (ImGui::IsKeyPressed(ImGuiKey_Delete)) deleteSelection();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D, false)) duplicateSelection();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C, false)) copySelection();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V, false)) pasteClipboard();
        if (ImGui::IsKeyPressed(ImGuiKey_F2) && active_.valid()) {
            renaming_ = active_;
            rename_buffer_ = world_.find(active_).name();
            rename_focus_ = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_F, false)) focusSelection();
    }
    ImGui::End();
}

// Filas del arbol en orden (padre antes que hijos) bajando solo por los
// nodos abiertos. El estado abierto lo guarda ImGui por el ID de la fila.
void EditorApp::buildHierarchyRows() {
    hierarchy_rows_.clear();
    if (!hierarchy_filter_.empty()) {
        const std::string needle = lower(hierarchy_filter_);
        world_.forEachDepthFirst([&](ecs::Entity e) {
            if (lower(e.name()).find(needle) != std::string::npos) {
                hierarchy_rows_.push_back(HierarchyRow{e.handle(), 0});
            }
        });
        return;
    }
    const ecs::Entity reveal_target = reveal_.valid() ? world_.find(reveal_) : ecs::Entity{};
    ImGuiStorage* storage = ImGui::GetStateStorage();
    std::vector<HierarchyRow> stack;
    const std::vector<entt::entity>& roots = world_.roots();
    for (auto it = roots.rbegin(); it != roots.rend(); ++it) stack.push_back(HierarchyRow{*it, 0});
    while (!stack.empty()) {
        const HierarchyRow row = stack.back();
        stack.pop_back();
        if (!world_.valid(row.entity)) continue;
        hierarchy_rows_.push_back(row);
        const ecs::Entity e = world_.wrap(row.entity);
        const std::vector<entt::entity>& children = e.children();
        if (children.empty()) continue;
        const ImGuiID id = ImGui::GetID(hierarchyRowId(row.entity));
        if (reveal_target.valid() && e.isAncestorOf(reveal_target)) storage->SetInt(id, 1);
        if (storage->GetInt(id, 0) == 0) continue;
        for (auto it = children.rbegin(); it != children.rend(); ++it) {
            stack.push_back(HierarchyRow{*it, row.depth + 1});
        }
    }
}

const void* EditorApp::hierarchyRowId(entt::entity entity) {
    // ID numerico del handle: nada de cadenas por fila y por frame.
    return reinterpret_cast<const void*>(static_cast<std::uintptr_t>(entt::to_integral(entity)) + 1);
}

void EditorApp::drawHierarchyRow(const HierarchyRow& row, bool scroll_to) {
    if (!world_.valid(row.entity)) return;  // borrada por una fila anterior
    ecs::Entity entity = world_.wrap(row.entity);
    const Uuid uuid = entity.uuid();
    const void* id = hierarchyRowId(row.entity);
    const float indent = static_cast<float>(row.depth) * ImGui::GetStyle().IndentSpacing;
    if (indent > 0.0f) ImGui::Indent(indent);
    struct Unindent {
        float amount;
        ~Unindent() {
            if (amount > 0.0f) ImGui::Unindent(amount);
        }
    } unindent{indent};

    // Sin TreePush: la sangria la pone la profundidad de la fila.
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick |
                               ImGuiTreeNodeFlags_SpanFullWidth | ImGuiTreeNodeFlags_FramePadding |
                               ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (entity.childCount() == 0) {
        flags |= ImGuiTreeNodeFlags_Leaf;
    }
    const bool selected = isSelected(uuid);
    if (selected) {
        flags |= ImGuiTreeNodeFlags_Selected;
    }

    const bool active = entity.activeInHierarchy();
    if (!active) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    }
    const bool renaming = renaming_ == uuid;
    // Hueco para el icono delante del nombre.
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0f, 2.0f));
    if (renaming) {
        ImGui::TreeNodeEx(id, flags, "%s", "");
    } else {
        ImGui::TreeNodeEx(id, flags, "    %s", entity.name().c_str());
    }
    if (scroll_to) ImGui::SetScrollHereY(0.5f);
    ImGui::PopStyleVar();
    if (!active) {
        ImGui::PopStyleColor();
    }

    {
        // Icono de lo que es (luz, camara, malla, fisica...).
        {
            const ImVec2 min = ImGui::GetItemRectMin();
            const float h = ImGui::GetItemRectSize().y;
            const float size = h - 4.0f;
            ImU32 tint = IM_COL32_WHITE;
            const Icon icon = entityIcon(entity, tint);
            if (!entity.activeInHierarchy()) tint = (tint & 0x00FFFFFFu) | 0x70000000u;
            imgui_.drawIcon(ImGui::GetWindowDrawList(), icon,
                            ImVec2(min.x + ImGui::GetTreeNodeToLabelSpacing() - 7.0f, min.y + 2.0f), size, tint);
        }

        // Seleccion (no al abrir con la flecha).
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen()) {
            ImGuiIO& io = ImGui::GetIO();
            if (io.KeyCtrl) {
                toggleSelection(uuid);
            } else if (io.KeyShift && active_.valid()) {
                // Rango en el orden de las filas del arbol.
                const ecs::Entity anchor = world_.find(active_);
                std::ptrdiff_t a = -1;
                std::ptrdiff_t b = -1;
                for (std::size_t i = 0; i < hierarchy_rows_.size(); ++i) {
                    if (anchor.valid() && hierarchy_rows_[i].entity == anchor.handle()) a = static_cast<std::ptrdiff_t>(i);
                    if (hierarchy_rows_[i].entity == row.entity) b = static_cast<std::ptrdiff_t>(i);
                }
                if (a >= 0 && b >= 0) {
                    selection_.clear();
                    for (std::ptrdiff_t i = std::min(a, b); i <= std::max(a, b); ++i) {
                        const entt::entity h = hierarchy_rows_[static_cast<std::size_t>(i)].entity;
                        if (world_.valid(h)) selection_.push_back(world_.wrap(h).uuid());
                    }
                } else {
                    toggleSelection(uuid);
                }
            } else {
                selectOnly(uuid);
            }
        }
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) &&
            entity.childCount() == 0) {
            selectOnly(uuid);
            focusSelection();
        }

        // Arrastrar: la seleccion (si la fila esta seleccionada) o solo esta.
        if (ImGui::BeginDragDropSource()) {
            if (!selected) selectOnly(uuid);
            ImGui::SetDragDropPayload(kEntityPayload, &uuid, sizeof(Uuid));
            ImGui::Text("%s%s", entity.name().c_str(),
                        selection_.size() > 1 ? (" (+" + std::to_string(selection_.size() - 1) + ")").c_str()
                                              : "");
            ImGui::EndDragDropSource();
        }
        // Soltar: antes / dentro / despues de esta fila (linea de insercion).
        if (ImGui::BeginDragDropTarget()) {
            const DropZone zone = dropZone();
            const ImVec2 min = ImGui::GetItemRectMin();
            const ImVec2 max = ImGui::GetItemRectMax();
            ImDrawList* draw = ImGui::GetWindowDrawList();
            const ImU32 accent = IM_COL32(90, 160, 255, 255);
            if (const ImGuiPayload* payload =
                    ImGui::AcceptDragDropPayload(kEntityPayload, ImGuiDragDropFlags_AcceptBeforeDelivery |
                                                                     ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {
                if (zone == DropZone::Before) draw->AddLine(ImVec2(min.x, min.y), ImVec2(max.x, min.y), accent, 2.0f);
                else if (zone == DropZone::After) draw->AddLine(ImVec2(min.x, max.y), ImVec2(max.x, max.y), accent, 2.0f);
                else draw->AddRect(min, max, accent, 3.0f, 0, 1.5f);
                if (payload->IsDelivery()) {
                    const std::vector<ecs::Entity> moving = topLevelSelection();
                    const ecs::Entity parent = entity.parent();
                    for (ecs::Entity m : moving) {
                        if (m == entity || m.isAncestorOf(entity)) continue;  // no dentro de si mismo
                        if (zone == DropZone::Inside) {
                            m.setParent(entity, true, -1);
                        } else {
                            // Hermano: en la posicion de esta fila (o la siguiente).
                            int index = entity.siblingIndex();
                            if (m.parent() == parent && m.siblingIndex() < index) --index;
                            if (zone == DropZone::After) ++index;
                            m.setParent(parent, true, index);
                        }
                    }
                    if (zone == DropZone::Inside) ImGui::GetStateStorage()->SetInt(ImGui::GetID(id), 1);
                    commit();
                }
            }
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAssetPayload)) {
                AssetPayload asset{};
                std::memcpy(&asset, payload->Data, sizeof(asset));
                if (asset.type == assets::AssetType::Model) instantiateAsset(asset.uuid, entity, std::nullopt);
                if (asset.type == assets::AssetType::Environment) assignEnvironment(asset.uuid);
                // Material: a todos sus huecos (y a los de sus hijos si no tiene malla).
                if (asset.type == assets::AssetType::Material && applyMaterial(entity, asset.uuid, -1)) {
                    inline_material_ = asset.uuid;
                    commit();
                }
            }
            ImGui::EndDragDropTarget();
        }

        // Menu contextual.
        if (ImGui::BeginPopupContextItem()) {
            if (!selected) selectOnly(uuid);
            if (ImGui::BeginMenu("Crear hijo")) {
                const auto item = [&](const char* text, int kind) {
                    if (ImGui::MenuItem(text)) createEntity(kind, entity);
                };
                item("Vacío", 0);
                item("Cubo", 1);
                item("Esfera", 2);
                item("Plano", 3);
                item("Luz puntual", 7);
                item("Foco", 8);
                item("Cámara", 9);
        ImGui::Separator();
        item("Decal (estampa)", 10);
        item("Charco", 11);
        item("Humedad", 12);
        ImGui::Separator();
        if (ImGui::MenuItem("Terreno")) createTerrainEntity();
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem("Renombrar", "F2")) {
                renaming_ = uuid;
                rename_buffer_ = entity.name();
                rename_focus_ = true;
            }
            if (ImGui::MenuItem("Duplicar", "Ctrl+D")) duplicateSelection();
            if (ImGui::MenuItem("Copiar", "Ctrl+C")) copySelection();
            if (ImGui::MenuItem("Pegar", "Ctrl+V", false, !clipboard_.empty())) pasteClipboard();
            if (ImGui::MenuItem("Enfocar", "F")) focusSelection();
            drawExtractAnimationsMenu(entity);
            ImGui::Separator();
            if (ImGui::MenuItem(entity.activeSelf() ? "Desactivar" : "Activar")) {
                entity.setActive(!entity.activeSelf());
                commit();
            }
            if (ImGui::MenuItem("Borrar", "Supr")) {
                ImGui::EndPopup();
                deleteSelection();
                return;
            }
            ImGui::EndPopup();
        }

        // Renombrar en el sitio.
        if (renaming) {
            ImGui::SameLine();
            if (rename_focus_) {
                ImGui::SetKeyboardFocusHere();
                rename_focus_ = false;
            }
            ImGui::SetNextItemWidth(-1.0f);
            const bool done = ImGui::InputText("##rename", &rename_buffer_,
                                               ImGuiInputTextFlags_EnterReturnsTrue |
                                                   ImGuiInputTextFlags_AutoSelectAll);
            if (done || ImGui::IsItemDeactivated()) {
                if (!rename_buffer_.empty() && rename_buffer_ != entity.name()) {
                    entity.setName(rename_buffer_);
                    commit();
                }
                renaming_ = {};
            }
        }
    }
}

}  // namespace cramion::editor
