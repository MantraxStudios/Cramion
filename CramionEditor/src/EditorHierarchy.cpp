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

constexpr const char* kEntityPayload = "CRAMION_ENTITY";

// Color del icono por lo que hace la entidad.
ImU32 entityColor(const ecs::Entity& e) {
    if (e.has<ecs::Light>()) return IM_COL32(255, 205, 80, 255);
    if (e.has<ecs::Camera>()) return IM_COL32(120, 220, 140, 255);
    if (e.has<ecs::MeshRenderer>()) return IM_COL32(110, 170, 255, 255);
    if (e.has<ecs::Sky>() || e.has<ecs::PostProcessing>() || e.has<ecs::Weather>()) {
        return IM_COL32(120, 220, 230, 255);
    }
    return IM_COL32(170, 170, 175, 255);
}

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
        ImGui::EndPopup();
    }
    ImGui::Separator();

    ImGui::BeginChild("tree", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);
    visible_order_.clear();

    // Revelar: abrir los padres de la entidad elegida en la vista.
    if (reveal_.valid()) {
        const ecs::Entity target = world_.find(reveal_);
        if (!target.valid()) reveal_ = {};
    }

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f, 1.0f));
    if (!hierarchy_filter_.empty()) {
        // Busqueda: lista plana de coincidencias (como Unity).
        const std::string needle = lower(hierarchy_filter_);
        world_.forEachDepthFirst([&](ecs::Entity e) {
            if (lower(e.name()).find(needle) == std::string::npos) return;
            const Uuid uuid = e.uuid();
            visible_order_.push_back(uuid);
            ImGui::PushID(static_cast<int>(entt::to_integral(e.handle())));
            if (ImGui::Selectable(e.name().c_str(), isSelected(uuid))) {
                if (ImGui::GetIO().KeyCtrl) toggleSelection(uuid);
                else selectOnly(uuid);
            }
            ImGui::PopID();
        });
    } else {
        const std::vector<entt::entity> roots = world_.roots();  // copia: se puede reordenar
        for (const entt::entity root : roots) {
            if (world_.valid(root)) {
                drawHierarchyNode(world_.wrap(root));
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

void EditorApp::drawHierarchyNode(ecs::Entity entity) {
    const Uuid uuid = entity.uuid();
    visible_order_.push_back(uuid);
    // ID numerico del handle: nada de cadenas por fila y por frame.
    const void* id = reinterpret_cast<const void*>(static_cast<std::uintptr_t>(entt::to_integral(entity.handle())) + 1);

    // Revelar: si la entidad elegida esta debajo, abrir este nodo.
    if (reveal_.valid()) {
        const ecs::Entity target = world_.find(reveal_);
        if (target.valid() && entity.isAncestorOf(target)) {
            ImGui::SetNextItemOpen(true);
        }
    }

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick |
                               ImGuiTreeNodeFlags_SpanFullWidth | ImGuiTreeNodeFlags_FramePadding;
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
    const bool open = renaming ? ImGui::TreeNodeEx(id, flags, "%s", "")
                               : ImGui::TreeNodeEx(id, flags, "      %s", entity.name().c_str());
    ImGui::PopStyleVar();
    if (!active) {
        ImGui::PopStyleColor();
    }

    // Filas fuera de la vista (arboles grandes): solo cuenta su altura.
    if (ImGui::IsItemVisible() || renaming) {
        // Icono.
        {
            const ImVec2 min = ImGui::GetItemRectMin();
            const float h = ImGui::GetItemRectSize().y;
            const float x = min.x + ImGui::GetTreeNodeToLabelSpacing() + 6.0f;
            ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(x - 4.0f, min.y + h * 0.5f - 4.0f),
                                                      ImVec2(x + 4.0f, min.y + h * 0.5f + 4.0f),
                                                      entityColor(entity), 2.0f);
        }

        // Seleccion (no al abrir con la flecha).
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen()) {
            ImGuiIO& io = ImGui::GetIO();
            if (io.KeyCtrl) {
                toggleSelection(uuid);
            } else if (io.KeyShift && active_.valid()) {
                // Rango en el orden visible (el nodo actual ya esta en la lista;
                // si el activo esta mas abajo, se completa al terminar el frame:
                // se toma lo que haya).
                const auto a = findUuid(visible_order_, active_);
                const auto b = findUuid(visible_order_, uuid);
                if (a != visible_order_.end() && b != visible_order_.end()) {
                    selection_.assign(std::min(a, b), std::max(a, b) + 1);
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
                    if (zone == DropZone::Inside) ImGui::SetNextItemOpen(true);
                    commit();
                }
            }
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAssetPayload)) {
                AssetPayload asset{};
                std::memcpy(&asset, payload->Data, sizeof(asset));
                if (asset.type == assets::AssetType::Model) instantiateAsset(asset.uuid, entity, std::nullopt);
                if (asset.type == assets::AssetType::Environment) assignEnvironment(asset.uuid);
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
                if (open) ImGui::TreePop();
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

    if (open) {
        const std::vector<entt::entity> children = entity.children();  // copia
        for (const entt::entity child : children) {
            if (world_.valid(child)) {
                drawHierarchyNode(world_.wrap(child));
            }
        }
        ImGui::TreePop();
    }
}

}  // namespace cramion::editor
