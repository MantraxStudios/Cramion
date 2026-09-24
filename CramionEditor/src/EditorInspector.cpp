// Inspector (como el de Unity): cabecera con activo y nombre, un bloque por
// componente dibujado con su reflect(), "Add Component" con busqueda por
// categorias y edicion multiple del Transform.

#include "EditorApp.h"

#include "Dialogs.h"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <map>

namespace cramion::editor {

using core::Vec3;

namespace {

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

}  // namespace

void EditorApp::drawInspector() {
    if (!ImGui::Begin("Inspector", &show_inspector_)) {
        ImGui::End();
        return;
    }
    const std::vector<ecs::Entity> selected = selectedEntities();
    ecs::Entity entity = world_.find(active_);
    if (!entity.valid() && !selected.empty()) {
        entity = selected.back();
    }
    if (!entity.valid()) {
        ImGui::TextDisabled("Selecciona un objeto en la Jerarquía o en la Escena.");
        ImGui::End();
        return;
    }

    // --- Cabecera: activo, nombre, etiqueta y capa ---
    bool active = entity.activeSelf();
    if (ImGui::Checkbox("##active", &active)) {
        for (ecs::Entity e : selected) e.setActive(active);
        commit();
    }
    ImGui::SameLine();
    std::string name = entity.name();
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputText("##name", &name, ImGuiInputTextFlags_EnterReturnsTrue) ||
        (ImGui::IsItemDeactivatedAfterEdit() && name != entity.name())) {
        if (!name.empty()) {
            entity.setName(name);
            commit();
        }
    }
    if (selected.size() > 1) {
        ImGui::TextDisabled("%zu objetos seleccionados (se edita el Transform de todos)",
                            selected.size());
    }
    ImGui::TextDisabled("UUID %s", entity.uuid().toString().c_str());
    ImGui::Separator();

    // --- Componentes ---
    ImGuiPropertyVisitor visitor(database_.get());
    const ecs::ComponentRegistry& registry = ecs::ComponentRegistry::instance();
    const ecs::ComponentType* to_remove = nullptr;
    for (const ecs::ComponentType& type : registry.types()) {
        if (!type.has(world_, entity.handle())) {
            continue;
        }
        ImGui::PushID(type.name.c_str());
        const bool open = ImGui::CollapsingHeader(type.label.c_str(),
                                                  ImGuiTreeNodeFlags_DefaultOpen |
                                                      ImGuiTreeNodeFlags_AllowOverlap);
        if (ImGui::BeginPopupContextItem("component_menu")) {
            if (ImGui::MenuItem("Quitar componente", nullptr, false, type.removable)) {
                to_remove = &type;
            }
            if (ImGui::MenuItem("Restablecer valores")) {
                type.remove(world_, entity.handle());
                type.add(world_, entity.handle());
                commit();
            }
            ImGui::EndPopup();
        }
        if (open) {
            ImGui::Indent(4.0f);
            // Multiseleccion: el Transform se aplica a todos como diferencia
            // (mover 1 m en X mueve 1 m a cada uno).
            const bool is_transform = type.name == "Transform";
            Vec3 old_position{};
            Vec3 old_euler{};
            Vec3 old_scale{};
            if (is_transform) {
                old_position = entity.localPosition();
                old_euler = entity.localEulerDegrees();
                old_scale = entity.localScale();
            }
            if (type.reflect(world_, entity.handle(), visitor)) {
                dirty_ = true;
                if (is_transform && selected.size() > 1) {
                    const Vec3 dp = entity.localPosition() - old_position;
                    const Vec3 de = entity.localEulerDegrees() - old_euler;
                    const Vec3 ns = entity.localScale();
                    const auto ratio = [](float now, float before) {
                        return std::abs(before) > 1e-6f ? now / before : 1.0f;
                    };
                    const Vec3 rs{ratio(ns.x, old_scale.x), ratio(ns.y, old_scale.y),
                                  ratio(ns.z, old_scale.z)};
                    for (ecs::Entity other : selected) {
                        if (other == entity) continue;
                        other.setLocalPosition(other.localPosition() + dp);
                        other.setLocalEulerDegrees(other.localEulerDegrees() + de);
                        const Vec3 s = other.localScale();
                        other.setLocalScale(Vec3{s.x * rs.x, s.y * rs.y, s.z * rs.z});
                    }
                }
            }
            // Decal: elegir la imagen (dialogo o arrastrarla desde el Proyecto).
            if (type.name == "Decal") {
                if (ecs::Decal* decal = entity.tryGet<ecs::Decal>(); decal != nullptr) {
                    if (ImGui::Button("Elegir imagen...", ImVec2(ImGui::GetContentRegionAvail().x * 0.6f, 0.0f))) {
                        const std::string texture = importDecalImage();
                        if (!texture.empty()) {
                            for (ecs::Entity e : selected) {
                                if (ecs::Decal* d = e.tryGet<ecs::Decal>()) d->texture = texture;
                            }
                            commit();
                        }
                    }
                    if (ImGui::BeginDragDropTarget()) {
                        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kImagePayload)) {
                            const std::string texture =
                                decalImageInAssets(dialogs::fromUtf8(static_cast<const char*>(payload->Data)));
                            for (ecs::Entity e : selected) {
                                if (ecs::Decal* d = e.tryGet<ecs::Decal>()) d->texture = texture;
                            }
                            commit();
                        }
                        ImGui::EndDragDropTarget();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Quitar imagen", ImVec2(-1.0f, 0.0f))) {
                        for (ecs::Entity e : selected) {
                            if (ecs::Decal* d = e.tryGet<ecs::Decal>()) d->texture.clear();
                        }
                        commit();
                    }
                }
            }
            // Animator con controlador: abrirlo en la ventana Animator.
            if (type.name == "Animator") {
                if (const ecs::Animator* animator = entity.tryGet<ecs::Animator>(); animator != nullptr) {
                    if (animator->controller.valid()) {
                        if (ImGui::Button("Abrir en la ventana Animator", ImVec2(-1.0f, 0.0f))) {
                            openAnimatorEditor(animator->controller.uuid);
                        }
                    } else if (ImGui::Button("Crear Animator con sus clips", ImVec2(-1.0f, 0.0f))) {
                        createAnimatorAsset(current_folder_.empty() ? project_.assetsFolder() : current_folder_);
                    }
                }
            }
            ImGui::Unindent(4.0f);
            ImGui::Spacing();
        }
        ImGui::PopID();
    }
    if (to_remove != nullptr) {
        to_remove->remove(world_, entity.handle());
        commit();
    }
    if (visitor.editFinished()) {
        commit();
    }

    // --- Add Component ---
    ImGui::Spacing();
    const float width = std::min(ImGui::GetContentRegionAvail().x, 260.0f);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (ImGui::GetContentRegionAvail().x - width) * 0.5f);
    if (ImGui::Button("Add Component", ImVec2(width, 28.0f))) {
        ImGui::OpenPopup("add_component");
    }
    drawAddComponent(entity);
    ImGui::End();
}

void EditorApp::drawAddComponent(ecs::Entity entity) {
    static std::string search;
    if (!ImGui::BeginPopup("add_component")) {
        return;
    }
    if (ImGui::IsWindowAppearing()) {
        search.clear();
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::SetNextItemWidth(240.0f);
    ImGui::InputTextWithHint("##search", "Buscar componente...", &search);
    ImGui::Separator();

    // Por categoria (Renderizado, Iluminacion, Entorno...).
    std::map<std::string, std::vector<const ecs::ComponentType*>> by_category;
    const std::string needle = lower(search);
    for (const ecs::ComponentType& type : ecs::ComponentRegistry::instance().types()) {
        if (!type.addable || type.has(world_, entity.handle())) continue;
        if (!needle.empty() && lower(type.label).find(needle) == std::string::npos &&
            lower(type.name).find(needle) == std::string::npos) {
            continue;
        }
        by_category[type.category].push_back(&type);
    }
    if (by_category.empty()) {
        ImGui::TextDisabled("Nada que añadir");
    }
    for (const auto& [category, types] : by_category) {
        ImGui::SeparatorText(category.c_str());
        for (const ecs::ComponentType* type : types) {
            if (ImGui::Selectable(type->label.c_str())) {
                for (ecs::Entity e : selectedEntities()) {
                    type->add(world_, e.handle());
                }
                commit();
                ImGui::CloseCurrentPopup();
            }
        }
    }
    ImGui::EndPopup();
}

}  // namespace cramion::editor
