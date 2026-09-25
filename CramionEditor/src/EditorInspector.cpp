// Inspector (como el de Unity): cabecera con activo y nombre, un bloque por
// componente dibujado con su reflect(), "Add Component" con busqueda por
// categorias y edicion multiple: con varios objetos solo salen los
// componentes que tienen todos, los valores distintos se ven con "—" y lo que
// se edita se aplica a todos.

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

// Icono de cada tipo de componente (cabeceras del Inspector).
std::optional<Icon> componentIcon(const std::string& name) {
    if (name == "Transform") return Icon::Move;
    if (name == "MeshRenderer") return Icon::MeshRenderer;
    if (name == "Animator") return Icon::SkinnedMesh;
    if (name == "Light") return Icon::PointLight;
    if (name == "Camera" || name == "VirtualCamera" || name == "CameraBrain") return Icon::Camera;
    if (name == "Sky") return Icon::AmbientLight;
    if (name == "Weather") return Icon::FogVolume;
    if (name == "PostProcessing") return Icon::ReflectionProbe;
    if (name == "Decal") return Icon::Decal;
    if (name == "Rigidbody") return Icon::Rigidbody;
    if (name == "BoxCollider") return Icon::ColliderBox;
    if (name == "SphereCollider") return Icon::ColliderSphere;
    if (name == "CapsuleCollider") return Icon::ColliderCapsule;
    if (name == "MeshCollider" || name == "PlaneCollider") return Icon::ColliderMesh;
    if (name == "ParticleSystem") return Icon::ParticleSystem;
    if (name == "DollyTrack" || name == "DollyCart") return Icon::Waypoint;
    if (name == "Terrain") return Icon::Terrain;
    if (name == "CinematicSequence") return Icon::Camera;
    return std::nullopt;
}

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
    // Un material elegido en el Proyecto: su editor (como Unity).
    if (inspected_material_.valid()) {
        if (database_->find(inspected_material_)) {
            drawMaterialEditor(inspected_material_);
            ImGui::End();
            return;
        }
        inspected_material_ = {};
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
    const bool multi = selected.size() > 1;
    const bool names_differ = multi && std::any_of(selected.begin(), selected.end(),
                                                   [&](const ecs::Entity& e) { return e.name() != entity.name(); });
    // Nombres distintos: "—" y lo que se escriba se pone a todos.
    std::string name = names_differ ? std::string() : entity.name();
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputTextWithHint("##name", names_differ ? "\xe2\x80\x94" : "", &name, ImGuiInputTextFlags_EnterReturnsTrue) ||
        (ImGui::IsItemDeactivatedAfterEdit() && !name.empty() && name != entity.name())) {
        if (!name.empty()) {
            if (multi) {
                for (ecs::Entity e : selected) e.setName(name);
            } else {
                entity.setName(name);
            }
            commit();
        }
    }
    if (multi) {
        ImGui::TextDisabled("%zu objetos seleccionados (componentes en comun)", selected.size());
    }
    // Tag y capa, uno al lado del otro (como Unity).
    if (ecs::EntityInfo* info = entity.tryGet<ecs::EntityInfo>()) {
        const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        ImGui::SetNextItemWidth(half);
        const std::string current_tag = entity.tag();
        if (ImGui::BeginCombo("##tag", ("Tag  " + current_tag).c_str(), ImGuiComboFlags_HeightLarge)) {
            for (const std::string& tag : ecs::projectTags()) {
                if (ImGui::Selectable(tag.c_str(), tag == current_tag)) {
                    for (ecs::Entity e : selected) e.setTag(tag);
                    commit();
                }
            }
            ImGui::Separator();
            static std::string new_tag;
            ImGui::SetNextItemWidth(160.0f);
            const bool enter = ImGui::InputTextWithHint("##add_tag", "Añadir tag...", &new_tag,
                                                        ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            if ((ImGui::SmallButton("+") || enter) && !new_tag.empty()) {
                if (ecs::addProjectTag(new_tag)) {
                    ecs::saveTags(project_.settingsFolder() / "Tags.json", ecs::projectTags());
                }
                for (ecs::Entity e : selected) e.setTag(new_tag);
                commit();
                new_tag.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip("Tag: para encontrar objetos (findWithTag) y en los eventos (compareTag)");
        }
        ImGui::SameLine();
        int layer = info->layer;
        ImGui::SetNextItemWidth(-1.0f);
        if (layerCombo("##layer", layer)) {
            for (ecs::Entity e : selected) {
                if (ecs::EntityInfo* other = e.tryGet<ecs::EntityInfo>()) other->layer = layer;
            }
            dirty_ = true;
            commit();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip("Capa: decide con quien choca (matriz en la ventana Física) y la ven las mascaras");
        }
    }
    ImGui::TextDisabled("UUID %s", entity.uuid().toString().c_str());
    ImGui::Separator();

    // --- Componentes ---
    ImGuiPropertyVisitor visitor(database_.get(), &world_);
    const ecs::ComponentRegistry& registry = ecs::ComponentRegistry::instance();
    const ecs::ComponentType* to_remove = nullptr;
    for (const ecs::ComponentType& type : registry.types()) {
        if (!type.has(world_, entity.handle())) {
            continue;
        }
        // Varios objetos: solo lo que tienen todos (como Unity).
        if (multi && !std::all_of(selected.begin(), selected.end(),
                                  [&](const ecs::Entity& e) { return type.has(world_, e.handle()); })) {
            continue;
        }
        ImGui::PushID(type.name.c_str());
        // Hueco para el icono del componente delante del nombre.
        const std::string header = "      " + type.label;
        const bool open = ImGui::CollapsingHeader(header.c_str(),
                                                  ImGuiTreeNodeFlags_DefaultOpen |
                                                      ImGuiTreeNodeFlags_AllowOverlap);
        {
            const std::optional<Icon> icon = componentIcon(type.name);
            if (icon) {
                const ImVec2 min = ImGui::GetItemRectMin();
                const float h = ImGui::GetItemRectSize().y;
                imgui_.drawIcon(ImGui::GetWindowDrawList(), *icon,
                                ImVec2(min.x + ImGui::GetTreeNodeToLabelSpacing(), min.y + 2.0f), h - 4.0f,
                                IM_COL32(225, 225, 230, 255));
            }
        }
        if (ImGui::BeginPopupContextItem("component_menu")) {
            if (ImGui::MenuItem("Quitar componente", nullptr, false, type.removable)) {
                to_remove = &type;
            }
            if (ImGui::MenuItem("Restablecer valores")) {
                for (ecs::Entity e : multi ? selected : std::vector<ecs::Entity>{entity}) {
                    type.remove(world_, e.handle());
                    type.add(world_, e.handle());
                }
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
            // Valores distintos entre los seleccionados (se muestran con "—").
            MixedFields mixed;
            if (multi) {
                std::vector<FieldValues> values;
                values.reserve(selected.size());
                values.emplace_back();
                CollectFieldsVisitor collect_active(values.back());
                type.reflect(world_, entity.handle(), collect_active);
                for (ecs::Entity other : selected) {
                    if (other == entity) continue;
                    values.emplace_back();
                    CollectFieldsVisitor collect(values.back());
                    type.reflect(world_, other.handle(), collect);
                }
                mixed = mixedFields(values);
            }
            visitor.beginComponent(multi ? &mixed : nullptr);
            if (type.reflect(world_, entity.handle(), visitor)) {
                dirty_ = true;
                // El campo editado, a los demas (solo ese campo).
                if (multi && !is_transform && visitor.lastChange()) {
                    const auto& [path, value] = *visitor.lastChange();
                    for (ecs::Entity other : selected) {
                        if (other == entity) continue;
                        ApplyFieldVisitor apply(path, value);
                        type.reflect(world_, other.handle(), apply);
                    }
                }
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
            // Colliders: editar su forma con asas en la vista (como Unity).
            if (type.category == "Fisica" && type.name.find("Collider") != std::string::npos &&
                type.name != "MeshCollider" && type.name != "PlaneCollider") {
                if (ImGui::Button(edit_collider_ ? "Terminar de editar collider" : "Editar collider",
                                  ImVec2(-1.0f, 0.0f))) {
                    edit_collider_ = !edit_collider_;
                }
            }
            // Rigidbody en Play: su estado real en la simulacion.
            if (type.name == "Rigidbody" && playing() && physics_.hasBody(entity)) {
                const Vec3 v = physics_.linearVelocity(entity);
                const Vec3 w = physics_.angularVelocity(entity);
                ImGui::TextDisabled("Velocidad  %.2f  %.2f  %.2f  (%.2f m/s)", v.x, v.y, v.z, core::length(v));
                ImGui::TextDisabled("Giro       %.2f  %.2f  %.2f rad/s", w.x, w.y, w.z);
                ImGui::TextDisabled("%s", physics_.isSleeping(entity) ? "Dormido" : "Despierto");
                const float third = (ImGui::GetContentRegionAvail().x - 8.0f) / 3.0f;
                if (ImGui::Button("Impulso arriba", ImVec2(third, 0.0f))) {
                    physics_.addForce(entity, Vec3{0.0f, 6.0f, 0.0f}, physics::ForceMode::VelocityChange);
                }
                ImGui::SameLine();
                if (ImGui::Button("Parar", ImVec2(third, 0.0f))) {
                    physics_.setLinearVelocity(entity, Vec3{});
                    physics_.setAngularVelocity(entity, Vec3{});
                }
                ImGui::SameLine();
                if (ImGui::Button("Despertar", ImVec2(third, 0.0f))) physics_.wakeUp(entity);
            }
            // Particulas: reproducir, parar y cuantas hay.
            if (type.name == "ParticleSystem") {
                const float half = (ImGui::GetContentRegionAvail().x - 4.0f) * 0.5f;
                if (ImGui::Button("Reiniciar", ImVec2(half, 0.0f))) particles_.play(entity);
                ImGui::SameLine();
                if (ImGui::Button("Parar", ImVec2(half, 0.0f))) particles_.stop(entity, true);
                ImGui::TextDisabled("%zu partículas vivas%s", particles_.particleCount(entity),
                                    playing() ? "" : " (vista previa)");
            }
            // Cinematicas: solo, alinear, puntos del riel, secuencia...
            if (type.category == "Cinematicas") drawCinematicInspector(type.name, entity);
            if (type.name == "Terrain") drawTerrainInspector(entity);
            if (type.name == "WaterBody") drawWaterInspector(entity);
            if (type.name == "NavMeshBounds") drawNavMeshBoundsInspector(entity);
            if (type.name == "Script") drawScriptInspector(entity);
            if (type.name == "RectTransform") drawRectTransformInspector(entity);
            if (type.name == "AudioSource") drawAudioInspector(entity);
            if (type.name == "MeshRenderer") drawMeshMaterials(entity);
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
        for (ecs::Entity e : multi ? selected : std::vector<ecs::Entity>{entity}) {
            if (to_remove->has(world_, e.handle())) to_remove->remove(world_, e.handle());
        }
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
        // Con varios objetos: lo que no tienen todos.
        const std::vector<ecs::Entity> targets = selectedEntities();
        const bool all_have = targets.empty()
                                  ? type.has(world_, entity.handle())
                                  : std::all_of(targets.begin(), targets.end(),
                                                [&](const ecs::Entity& e) { return type.has(world_, e.handle()); });
        if (!type.addable || all_have) continue;
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
                    if (!type->has(world_, e.handle())) type->add(world_, e.handle());
                }
                commit();
                ImGui::CloseCurrentPopup();
            }
        }
    }
    ImGui::EndPopup();
}

}  // namespace cramion::editor
