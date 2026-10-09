// Inspector (como el de Unity): cabecera con activo y nombre, un bloque por
// componente dibujado con su reflect(), "Add Component" con busqueda por
// categorias y edicion multiple: con varios objetos solo salen los
// componentes que tienen todos, los valores distintos se ven con "—" y lo que
// se edita se aplica a todos.

#include "EditorApp.h"

#include "Dialogs.h"
#include "Theme.h"

#include <CramionCore/ecs/ComponentPresets.h>

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <functional>
#include <iostream>
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
    if (name == "Environment") return Icon::AmbientLight;
    if (name == "PostProcessing") return Icon::ReflectionProbe;
    if (name == "Profiler") return Icon::ReflectionProbe;
    if (name == "Decal") return Icon::Decal;
    if (name == "Rigidbody") return Icon::Rigidbody;
    if (name == "BoxCollider") return Icon::ColliderBox;
    if (name == "SphereCollider") return Icon::ColliderSphere;
    if (name == "CapsuleCollider") return Icon::ColliderCapsule;
    if (name == "CharacterController") return Icon::CharacterController;
    if (name == "MeshCollider" || name == "PlaneCollider") return Icon::ColliderMesh;
    if (name == "ParticleSystem") return Icon::ParticleSystem;
    if (name == "DollyTrack" || name == "DollyCart") return Icon::Waypoint;
    if (name == "Terrain") return Icon::Terrain;
    if (name == "CinematicSequence") return Icon::Camera;
    if (name == "AudioSource") return Icon::AudioSource;
    if (name == "AudioListener") return Icon::AudioListener;
    if (name == "AudioReverbZone") return Icon::AudioSource;
    if (name == "NavAgent") return Icon::NavMeshAgent;
    if (name == "StateMachine") return Icon::NavMeshAgent;
    if (name == "NavModifier") return Icon::NavMeshObstacle;
    if (name == "NavMeshBounds") return Icon::TriggerVolume;
    if (name == "Vehicle") return Icon::Rigidbody;
    if (name == "WheelCollider") return Icon::ColliderCapsule;
    if (name == "WaterBody") return Icon::FogVolume;
    if (name == "Fire") return Icon::ParticleSystem;
    if (name == "VoxelWorld") return Icon::Terrain;
    return std::nullopt;
}

// Color de cada categoria (la barra de las fichas de Add Component): tonos
// apagados que conviven con el rojo/amarillo del tema.
ImU32 categoryColor(const std::string& category) {
    if (category == "Renderizado") return IM_COL32(229, 56, 59, 255);
    if (category == "Fisica") return IM_COL32(120, 190, 120, 255);
    if (category == "Entorno") return IM_COL32(110, 180, 190, 255);
    if (category == "Audio") return IM_COL32(255, 197, 61, 255);
    if (category == "Scripting") return IM_COL32(176, 144, 220, 255);
    if (category == "Navegacion") return IM_COL32(214, 190, 110, 255);
    if (category == "Efectos") return IM_COL32(240, 128, 88, 255);
    if (category == "Animacion") return IM_COL32(226, 120, 150, 255);
    if (category == "Cinematicas") return IM_COL32(150, 150, 220, 255);
    if (category == "UI") return IM_COL32(200, 200, 206, 255);
    // Otras: un gris calido estable por su nombre.
    std::uint32_t h = 2166136261u;
    for (const char c : category) h = (h ^ static_cast<unsigned char>(c)) * 16777619u;
    return IM_COL32(140 + (h & 0x3F), 130 + ((h >> 8) & 0x3F), 120 + ((h >> 16) & 0x3F), 255);
}

// Valores copiados de un componente (menu ⋮ > Copiar / Pegar valores).
struct ComponentClipboard {
    std::string component;
    FieldValues values;
};

ComponentClipboard& componentClipboard() {
    static ComponentClipboard clipboard;
    return clipboard;
}

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

}  // namespace

void EditorApp::drawInspector() {
    if (focus_inspector_) {
        ImGui::SetNextWindowFocus();
        focus_inspector_ = false;
    }
    if (!ImGui::Begin(panelTitle("Inspector").c_str(), &show_inspector_)) {
        ImGui::End();
        return;
    }
    // Una Render Texture elegida en el Proyecto: su tamano y lo que tiene.
    if (inspected_render_texture_.valid()) {
        drawRenderTextureEditor(inspected_render_texture_);
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
    // Un modelo elegido en el Proyecto: sus ajustes de importacion (hasta que
    // se elija un objeto de la escena).
    if (inspected_model_.valid()) {
        if (active_ == inspected_model_active_ && database_ && database_->find(inspected_model_)) {
            drawModelImportSettings(inspected_model_);
            ImGui::End();
            return;
        }
        inspected_model_ = {};
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

    // --- Cabecera: activo, nombre, etiqueta y capa (una tarjeta) ---
    theme::beginCard("entity_header", theme::kBg2);
    bool active = entity.activeSelf();
    if (ImGui::Checkbox("##active", &active)) {
        for (ecs::Entity e : selected) e.setActive(active);
        commit();
    }
    ImGui::SetItemTooltip("Activo: desactivado no se dibuja ni ejecuta sus scripts (ni sus hijos)");
    ImGui::SameLine();
    {
        // Icono del tipo de objeto (el mismo de la Jerarquia).
        ImU32 tint = theme::kText;
        const Icon icon = entityIcon(entity, tint);
        const float size = ImGui::GetFrameHeight();
        imgui_.drawIcon(ImGui::GetWindowDrawList(), icon, ImGui::GetCursorScreenPos(), size,
                        active ? tint : theme::kTextFaint);
        ImGui::Dummy(ImVec2(size, size));
        ImGui::SameLine();
    }
    const bool multi = selected.size() > 1;
    const bool names_differ = multi && std::any_of(selected.begin(), selected.end(),
                                                   [&](const ecs::Entity& e) { return e.name() != entity.name(); });
    // Nombres distintos: "—" y lo que se escriba se pone a todos.
    std::string name = names_differ ? std::string() : entity.name();
    // Hueco a la derecha para la casilla Static (como Unity).
    const float static_width = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x +
                               ImGui::CalcTextSize("Static").x + ImGui::GetStyle().ItemSpacing.x;
    ImGui::SetNextItemWidth(-static_width);
    if (theme::boldFont() != nullptr) ImGui::PushFont(theme::boldFont(), 0.0f);
    const bool name_entered =
        ImGui::InputTextWithHint("##name", names_differ ? "\xe2\x80\x94" : "", &name, ImGuiInputTextFlags_EnterReturnsTrue);
    if (theme::boldFont() != nullptr) ImGui::PopFont();
    if (name_entered || (ImGui::IsItemDeactivatedAfterEdit() && !name.empty() && name != entity.name())) {
        if (!name.empty()) {
            if (multi) {
                for (ecs::Entity e : selected) e.setName(name);
            } else {
                entity.setName(name);
            }
            commit();
        }
    }
    ImGui::SameLine();
    {
        const ecs::EntityInfo* info = entity.tryGet<ecs::EntityInfo>();
        bool is_static = info != nullptr && info->is_static;
        if (ImGui::Checkbox("Static", &is_static)) {
            for (ecs::Entity e : selected) {
                if (ecs::EntityInfo* other = e.tryGet<ecs::EntityInfo>()) other->is_static = is_static;
            }
            commit();
            // Con hijos se pregunta si tambien a ellos (como Unity).
            if (std::any_of(selected.begin(), selected.end(), [](const ecs::Entity& e) { return e.childCount() > 0; })) {
                static_children_value_ = is_static;
                ImGui::OpenPopup("static_children");
            }
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip("Static: el objeto no se mueve en el juego.\n"
                              "Al exportar, las mallas Static se combinan en un lote por escena\n"
                              "(static batching): una llamada de dibujo por material.\n"
                              "No lo marques en lo que muevas, ocultes o cambies por script.");
        }
        if (ImGui::BeginPopup("static_children")) {
            ImGui::Text("%s Static tambien a los hijos?", static_children_value_ ? "Marcar" : "Quitar");
            if (ImGui::Button("Si, a los hijos tambien")) {
                const std::function<void(ecs::Entity)> apply = [&](ecs::Entity e) {
                    if (ecs::EntityInfo* other = e.tryGet<ecs::EntityInfo>()) other->is_static = static_children_value_;
                    for (std::size_t i = 0; i < e.childCount(); ++i) apply(e.child(i));
                };
                for (ecs::Entity e : selected) apply(e);
                commit();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Solo este objeto")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }
    if (multi) {
        const std::string count = std::to_string(selected.size()) + " OBJETOS";
        theme::badge(count.c_str(), theme::kYellow);
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("edición múltiple: solo los componentes en común");
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
    theme::smallText(theme::kTextFaint, "UUID  %s", entity.uuid().toString().c_str());
    // Origen flotante: las posiciones del Inspector son relativas al origen
    // actual; la real (doble precision) se ensena aparte.
    if (world_.origin() != ecs::DVec3{}) {
        const ecs::DVec3 real = world_.absolute(entity.worldPosition());
        theme::smallText(theme::kTextDim, "Posición real  %.3f  %.3f  %.3f", real.x, real.y, real.z);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            const ecs::DVec3& o = world_.origin();
            ImGui::SetTooltip("Mundo grande: el origen se desplazo a (%.0f, %.0f, %.0f) para no perder precision.\n"
                              "Las posiciones del Transform son relativas a ese origen.",
                              o.x, o.y, o.z);
        }
    }
    theme::endCard();
    if (!multi) drawPrefabInspectorBar(entity);

    // --- Buscar una propiedad (filtra componentes y campos) ---
    static std::string property_filter;
    ImGui::Spacing();
    theme::searchBox("inspector_filter", property_filter, "Buscar propiedad o componente...");
    const std::string filter_needle = lowerText(property_filter.c_str());
    ImGui::Spacing();

    // --- Componentes ---
    ImGuiPropertyVisitor visitor(database_.get(), &world_);
    const ecs::ComponentRegistry& registry = ecs::ComponentRegistry::instance();
    const ecs::ComponentType* to_remove = nullptr;
    int components_shown = 0;
    for (const ecs::ComponentType& type : registry.types()) {
        if (!type.has(world_, entity.handle())) {
            continue;
        }
        // El enlace con el prefab se ve en su barra, no como componente.
        if (type.name == "PrefabInstance" || type.name == "PrefabLink") continue;
        // Varios objetos: solo lo que tienen todos (como Unity).
        if (multi && !std::all_of(selected.begin(), selected.end(),
                                  [&](const ecs::Entity& e) { return type.has(world_, e.handle()); })) {
            continue;
        }
        // Filtro: el componente entero si coincide su nombre; si no, solo
        // los campos que coinciden (y nada si ninguno).
        const bool name_match = filter_needle.empty() || lower(type.label).find(filter_needle) != std::string::npos ||
                                lower(type.name).find(filter_needle) != std::string::npos;
        if (!name_match) {
            FilterMatchVisitor match(filter_needle);
            type.reflect(world_, entity.handle(), match);
            if (!match.matched()) continue;
        }
        ++components_shown;
        visitor.setFilter(name_match ? std::string() : filter_needle);
        ImGui::PushID(type.name.c_str());
        const std::vector<ecs::Entity> targets = multi ? selected : std::vector<ecs::Entity>{entity};
        // Cabecera del componente: flecha, icono, nombre en seminegrita y, a
        // la derecha, Presets y el menu ⋮ (tambien con clic derecho).
        const std::string header = "      " + type.label;
        if (!filter_needle.empty()) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
        ImGui::PushStyleColor(ImGuiCol_Header, theme::kHeader);
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, theme::kHeaderHover);
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, theme::kHeaderActive);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 6.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
        if (theme::boldFont() != nullptr) ImGui::PushFont(theme::boldFont(), 0.0f);
        const bool open = ImGui::CollapsingHeader(header.c_str(),
                                                  ImGuiTreeNodeFlags_DefaultOpen |
                                                      ImGuiTreeNodeFlags_AllowOverlap);
        if (theme::boldFont() != nullptr) ImGui::PopFont();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(3);
        const ImVec2 header_min = ImGui::GetItemRectMin();
        const ImVec2 header_max = ImGui::GetItemRectMax();
        const bool open_menu = ImGui::IsItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Right);
        {
            const std::optional<Icon> icon = componentIcon(type.name);
            const float h = header_max.y - header_min.y;
            const ImVec2 at(header_min.x + ImGui::GetTreeNodeToLabelSpacing(), header_min.y + 4.0f);
            if (icon) {
                imgui_.drawIcon(ImGui::GetWindowDrawList(), *icon, at, h - 8.0f, theme::kText);
            } else {
                // Sin icono propio: un rombo pequeno del color de su categoria.
                const ImVec2 c(at.x + (h - 8.0f) * 0.5f, header_min.y + h * 0.5f);
                const float r = (h - 8.0f) * 0.28f;
                ImGui::GetWindowDrawList()->AddQuadFilled(ImVec2(c.x, c.y - r), ImVec2(c.x + r, c.y), ImVec2(c.x, c.y + r),
                                                          ImVec2(c.x - r, c.y), categoryColor(type.category));
            }
        }
        bool menu_clicked = false;
        {
            const float h = header_max.y - header_min.y;
            ImGui::SameLine();
            menu_clicked = theme::kebabButton("##component_menu_button", ImVec2(header_max.x - h - 1.0f, header_min.y), h);
            ImGui::SetItemTooltip("Opciones del componente");
            if (!ecs::builtinPresets(type.name).empty()) {
                // "Presets" pequeno, a la izquierda del menu.
                const float small_size = theme::smallFontSize();
                const ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(small_size, FLT_MAX, 0.0f, "Presets");
                const float w = ts.x + 14.0f;
                const ImVec2 bmin(header_max.x - h - 4.0f - w, header_min.y + 5.0f);
                ImGui::SameLine();
                ImGui::SetCursorScreenPos(bmin);
                if (ImGui::InvisibleButton("##presets_button", ImVec2(w, h - 10.0f))) {
                    ImGui::OpenPopup("component_presets");
                }
                const bool hovered = ImGui::IsItemHovered();
                ImDrawList* draw = ImGui::GetWindowDrawList();
                draw->AddRect(bmin, ImVec2(bmin.x + w, bmin.y + h - 10.0f), hovered ? theme::kBorderStrong : theme::kBorder,
                              3.0f);
                draw->AddText(ImGui::GetFont(), small_size,
                              ImVec2(bmin.x + 7.0f, bmin.y + (h - 10.0f - ts.y) * 0.5f),
                              hovered ? theme::kText : theme::kTextDim, "Presets");
                ImGui::SetItemTooltip("Aplicar un preset de fabrica o del proyecto, o guardar estos valores como preset");
            }
        }
        if (open_menu || menu_clicked) ImGui::OpenPopup("component_menu");
        if (ImGui::BeginPopup("component_menu")) {
            ImGui::TextDisabled("%s", type.label.c_str());
            ImGui::Separator();
            if (ImGui::MenuItem("Restablecer valores")) {
                for (ecs::Entity e : targets) {
                    type.remove(world_, e.handle());
                    type.add(world_, e.handle());
                    // Como el Reset de Unity: el collider vuelve a la medida de la malla.
                    if (physics::isFittableCollider(type.name)) physics_.fitColliderToMesh(e, type.name);
                }
                commit();
            }
            if (physics::isFittableCollider(type.name) && ImGui::MenuItem("Ajustar a la malla (AABB)")) {
                for (ecs::Entity e : targets) physics_.fitColliderToMesh(e, type.name);
                commit();
            }
            if (ImGui::MenuItem("Copiar valores")) {
                ComponentClipboard& clipboard = componentClipboard();
                clipboard.component = type.name;
                clipboard.values.clear();
                CollectFieldsVisitor collect(clipboard.values);
                type.reflect(world_, entity.handle(), collect);
            }
            const ComponentClipboard& clipboard = componentClipboard();
            if (ImGui::MenuItem("Pegar valores", nullptr, false, clipboard.component == type.name)) {
                for (ecs::Entity e : targets) {
                    // Primero el tamano de las listas, luego los campos.
                    for (int pass = 0; pass < 2; ++pass) {
                        for (const auto& [path, value] : clipboard.values) {
                            if ((value.kind == FieldValue::Kind::ListCount) != (pass == 0)) continue;
                            ApplyFieldVisitor apply(path, value);
                            type.reflect(world_, e.handle(), apply);
                        }
                    }
                }
                dirty_ = true;
                commit();
            }
            if (ImGui::BeginMenu("Presets")) {
                drawComponentPresets(type.name, targets);
                ImGui::EndMenu();
            }
            ImGui::Separator();
            ImGui::PushStyleColor(ImGuiCol_Text, type.removable ? theme::kRedText : theme::kTextFaint);
            if (ImGui::MenuItem("Quitar componente", nullptr, false, type.removable)) {
                to_remove = &type;
            }
            ImGui::PopStyleColor();
            ImGui::EndPopup();
        }
        if (ImGui::BeginPopup("component_presets")) {
            drawComponentPresets(type.name, targets);
            ImGui::EndPopup();
        }
        if (open) {
            theme::beginCard(type.name.c_str(), theme::kBg2, true);
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
            // C++ Script: el archivo (soltar el .cpp) y la clase, encima de sus propiedades.
            if (type.name == "CppScript") drawCppScriptInspector(entity, true);
            visitor.beginComponent(multi ? &mixed : nullptr);
            if (type.reflect(world_, entity.handle(), visitor)) {
                dirty_ = true;
                if (type.name == "PostProcessing") renderer_.resetAdaptiveBudget();
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
                const bool fittable = physics::isFittableCollider(type.name);
                const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
                if (ImGui::Button(edit_collider_ ? "Terminar de editar" : "Editar collider",
                                  ImVec2(fittable ? half : -1.0f, 0.0f))) {
                    edit_collider_ = !edit_collider_;
                }
                if (fittable) {
                    ImGui::SameLine();
                    if (ImGui::Button("Ajustar a la malla", ImVec2(-1.0f, 0.0f))) {
                        int fitted = 0;
                        for (ecs::Entity e : targets) fitted += physics_.fitColliderToMesh(e, type.name) ? 1 : 0;
                        if (fitted > 0) commit();
                        else std::cerr << "[Fisica] " << entity.name() << ": no tiene malla (ni sus hijos) para ajustar el collider" << std::endl;
                    }
                    ImGui::SetItemTooltip("Centro y tamano = el AABB de su malla (o el de las mallas de sus hijos), como al "
                                          "anadirlo en Unity. La escala del Transform lo multiplica igual que a la malla.");
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
            if (type.name == "Spline") drawSplineInspector(entity);
            if (type.name == "WorldPartition") drawWorldPartitionInspector(entity);
            if (type.name == "Fire") drawFireInspector(entity);
            if (type.name == "Environment") drawEnvironmentInspector(entity);
            if (type.name == "NavMeshBounds") drawNavMeshBoundsInspector(entity);
            if (type.name == "Script") drawScriptInspector(entity);
            if (type.name == "CppScript") drawCppScriptInspector(entity, false);
            if (type.name == "StateMachine") drawStateMachineInspector(entity);
            if (type.name == "RectTransform") drawRectTransformInspector(entity);
            if (type.name == "AudioSource") drawAudioInspector(entity);
            if (type.name == "MeshRenderer") drawMeshMaterials(entity);
            if (type.name == "XrController") drawXrControllerInspector(entity);
            if (type.name == "Skeleton" || type.name == "BoneSocket" || type.name == "InverseKinematics" ||
                type.name == "Ragdoll" || type.name == "PhysBones") {
                drawRigInspector(type.name, entity);
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
            theme::endCard();
        }
        ImGui::Spacing();
        ImGui::PopID();
    }
    if (components_shown == 0 && !filter_needle.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("Ninguna propiedad coincide con \"%s\"", property_filter.c_str());
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

    // --- Guardar un preset (lo pide el menu de Presets) ---
    if (preset_save_request_) {
        preset_save_request_ = false;
        ImGui::OpenPopup("Guardar preset");
    }
    if (ImGui::BeginPopupModal("Guardar preset", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Preset de %s", preset_save_component_.c_str());
        ImGui::TextDisabled("Se guarda en Assets/Presets/%s", preset_save_component_.c_str());
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool enter = ImGui::InputText("Nombre", &preset_save_name_, ImGuiInputTextFlags_EnterReturnsTrue);
        const bool exists = !preset_save_name_.empty() &&
                            ecs::findPreset(ecs::projectPresets(project_.assetsFolder(), preset_save_component_),
                                            preset_save_name_) != nullptr;
        if (exists) ImGui::TextColored(theme::vec(theme::kYellow), "Ya existe: se sobrescribe");
        ImGui::BeginDisabled(preset_save_name_.empty());
        if (theme::primaryButton("Guardar", ImVec2(120.0f, 0.0f)) || (enter && !preset_save_name_.empty())) {
            std::string error;
            const std::filesystem::path file = ecs::savePreset(world_, entity, preset_save_component_, preset_save_name_,
                                                               project_.assetsFolder(), &error);
            if (file.empty()) {
                std::cerr << "[Editor] No se pudo guardar el preset: " << error << "\n";
            } else {
                std::cout << "[Editor] Preset guardado: " << preset_save_name_ << "\n";
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancelar", ImVec2(120.0f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // --- Add Component ---
    // Boton principal (rojo) a todo el ancho.
    ImGui::Spacing();
    if (theme::boldFont() != nullptr) ImGui::PushFont(theme::boldFont(), 0.0f);
    const bool add_pressed = theme::primaryButton("+  Add Component", ImVec2(-1.0f, ImGui::GetFrameHeight() + 8.0f));
    if (theme::boldFont() != nullptr) ImGui::PopFont();
    // Como en Unity: soltar un script (.lua o .cpp) aqui lo agrega al objeto.
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kScriptPayload)) {
            const std::string relative = assetRelative(dialogs::fromUtf8(static_cast<const char*>(payload->Data)));
            for (ecs::Entity e : selected) attachScriptFile(e, relative);
            commit();
        }
        ImGui::EndDragDropTarget();
    }
    if (add_pressed) {
        ImGui::OpenPopup("add_component");
    }
    ImGui::Spacing();
    // El desplegable, centrado en la ventana del editor (como el de Unreal).
    {
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        const float popup_width = std::clamp(viewport->WorkSize.x * 0.6f, 560.0f, 1100.0f);
        const float popup_height = std::clamp(viewport->WorkSize.y * 0.72f, 420.0f, 860.0f);
        ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                       viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                                ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(popup_width, popup_height), ImGuiCond_Appearing);
        ImGui::SetNextWindowSizeConstraints(ImVec2(300.0f, 240.0f), ImVec2(1200.0f, 1000.0f));
    }
    drawAddComponent(entity);
    ImGui::End();
}

void EditorApp::drawComponentPresets(const std::string& component, const std::vector<ecs::Entity>& targets) {
    const std::vector<ecs::ComponentPreset> presets = ecs::allPresets(project_.assetsFolder(), component);
    const auto apply = [&](const ecs::ComponentPreset& preset) {
        std::string error;
        int applied = 0;
        for (ecs::Entity e : targets) {
            if (ecs::applyPreset(world_, e, preset, &error)) ++applied;
        }
        if (applied > 0) {
            commit();
            if (component == "PostProcessing") renderer_.resetAdaptiveBudget();
            std::cout << "[Editor] Preset \"" << preset.name << "\" aplicado a " << applied << " objeto(s)\n";
        } else {
            std::cerr << "[Editor] No se pudo aplicar el preset: " << error << "\n";
        }
    };
    bool builtin_title = false;
    bool project_title = false;
    for (const ecs::ComponentPreset& preset : presets) {
        if (preset.builtin && !builtin_title) {
            ImGui::SeparatorText("De fábrica");
            builtin_title = true;
        }
        if (!preset.builtin && !project_title) {
            ImGui::SeparatorText("Del proyecto");
            project_title = true;
        }
        ImGui::PushID(preset.name.c_str());
        if (ImGui::MenuItem(preset.name.c_str())) apply(preset);
        if (!preset.description.empty()) ImGui::SetItemTooltip("%s", preset.description.c_str());
        if (!preset.builtin && ImGui::BeginPopupContextItem("preset_item")) {
            if (ImGui::MenuItem("Borrar preset")) {
                std::error_code ec;
                std::filesystem::remove(preset.file, ec);
                std::cout << "[Editor] Preset borrado: " << preset.name << "\n";
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    if (!project_title) {
        ImGui::SeparatorText("Del proyecto");
        ImGui::TextDisabled("(ninguno todavía)");
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Guardar estos valores como preset...", nullptr, false, !targets.empty())) {
        preset_save_component_ = component;
        preset_save_name_.clear();
        preset_save_request_ = true;
    }
}

void EditorApp::drawAddComponent(ecs::Entity entity) {
    static std::string search;
    static std::string category_filter;  // vacio = todas
    if (!ImGui::BeginPopup("add_component")) {
        return;
    }
    if (ImGui::IsWindowAppearing()) {
        search.clear();
        ImGui::SetKeyboardFocusHere();
    }

    // Lo que se puede anadir (con varios objetos: lo que no tienen todos).
    const std::vector<ecs::Entity> targets = selectedEntities();
    const std::string needle = lower(search);
    std::map<std::string, std::vector<const ecs::ComponentType*>> by_category;
    std::map<std::string, int> category_counts;
    for (const ecs::ComponentType& type : ecs::ComponentRegistry::instance().types()) {
        const bool all_have = targets.empty()
                                  ? type.has(world_, entity.handle())
                                  : std::all_of(targets.begin(), targets.end(),
                                                [&](const ecs::Entity& e) { return type.has(world_, e.handle()); });
        if (!type.addable || all_have) continue;
        if (!needle.empty() && lower(type.label).find(needle) == std::string::npos &&
            lower(type.name).find(needle) == std::string::npos && lower(type.category).find(needle) == std::string::npos) {
            continue;
        }
        ++category_counts[type.category];
        if (!category_filter.empty() && type.category != category_filter) continue;
        by_category[type.category].push_back(&type);
    }

    // --- Busqueda y, a su derecha, el filtro de categoria ---
    int total = 0;
    for (const auto& [name, count] : category_counts) total += count;
    if (!category_filter.empty() && !category_counts.contains(category_filter)) category_filter.clear();
    constexpr float kFilterWidth = 190.0f;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f, 7.0f));
    theme::searchBox("component_search", search, "Buscar componente...",
                     ImGui::GetContentRegionAvail().x - kFilterWidth - ImGui::GetStyle().ItemSpacing.x);
    // Enter: el primero de la lista.
    const bool enter = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) &&
                       (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(kFilterWidth);
    const std::string preview = category_filter.empty() ? "Todas (" + std::to_string(total) + ")"
                                                        : category_filter + " (" +
                                                              std::to_string(category_counts[category_filter]) + ")";
    if (ImGui::BeginCombo("##category", preview.c_str(), ImGuiComboFlags_HeightLarge)) {
        if (ImGui::Selectable(("Todas (" + std::to_string(total) + ")").c_str(), category_filter.empty())) {
            category_filter.clear();
        }
        ImGui::Separator();
        for (const auto& [name, count] : category_counts) {
            // Punto del color de la categoria delante del nombre.
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float h = ImGui::GetTextLineHeight();
            ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + 6.0f, p.y + h * 0.5f), 4.5f, categoryColor(name));
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 16.0f);
            if (ImGui::Selectable((name + " (" + std::to_string(count) + ")").c_str(), category_filter == name)) {
                category_filter = name;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::PopStyleVar();
    ImGui::Spacing();
    ImGui::Separator();

    // --- Rejilla de fichas ---
    const ecs::ComponentType* chosen = nullptr;
    ImGui::BeginChild("##component_grid", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);
    if (by_category.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled(search.empty() ? "Nada que añadir" : "Ningún componente coincide");
    }
    constexpr float kTileW = 104.0f;
    constexpr float kTileH = 92.0f;
    constexpr float kGap = 8.0f;
    const float avail = ImGui::GetContentRegionAvail().x;
    const int columns = std::max(1, static_cast<int>((avail + kGap) / (kTileW + kGap)));
    // Las fichas se estiran para llenar el ancho (rejilla sin hueco a la derecha).
    const float tile_w = std::floor((avail - kGap * static_cast<float>(columns - 1)) / static_cast<float>(columns));
    ImDrawList* draw = ImGui::GetWindowDrawList();
    for (const auto& [category, types] : by_category) {
        const ImU32 accent = categoryColor(category);
        if (category_filter.empty()) {
            // Titulo de la categoria con su marca de color.
            ImGui::Spacing();
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float h = ImGui::GetFrameHeight();
            draw->AddRectFilled(ImVec2(p.x, p.y + h * 0.5f - 5.0f), ImVec2(p.x + 3.0f, p.y + h * 0.5f + 5.0f), accent,
                                1.0f);
            ImGui::SetCursorScreenPos(ImVec2(p.x + 10.0f, p.y));
            theme::sectionHeader(category.c_str());
        }
        for (std::size_t i = 0; i < types.size(); ++i) {
            const ecs::ComponentType* type = types[i];
            if (chosen == nullptr && enter) chosen = type;  // Enter: el primero
            if (i % static_cast<std::size_t>(columns) != 0) ImGui::SameLine(0.0f, kGap);
            ImGui::PushID(type->name.c_str());
            const ImVec2 p0 = ImGui::GetCursorScreenPos();
            if (ImGui::InvisibleButton("##tile", ImVec2(tile_w, kTileH))) chosen = type;
            const bool hovered = ImGui::IsItemHovered();
            const bool held = ImGui::IsItemActive();
            const ImVec2 p1(p0.x + tile_w, p0.y + kTileH);
            // Fondo, borde y la barra de color de la categoria.
            draw->AddRectFilled(p0, p1, held ? theme::kBg5 : (hovered ? theme::kBg4 : theme::kBg3), 4.0f);
            draw->AddRect(p0, p1, hovered ? theme::kRed : theme::kBorder, 4.0f, 0, hovered ? 1.5f : 1.0f);
            draw->AddRectFilled(ImVec2(p0.x + 10.0f, p0.y + 1.0f), ImVec2(p1.x - 10.0f, p0.y + 3.0f),
                                theme::withAlpha(accent, hovered ? 255 : 170), 1.0f);
            // Icono (o la inicial en un circulo).
            constexpr float kIcon = 34.0f;
            const ImVec2 icon_min(p0.x + (tile_w - kIcon) * 0.5f, p0.y + 14.0f);
            if (const std::optional<Icon> icon = componentIcon(type->name)) {
                imgui_.drawIcon(draw, *icon, icon_min, kIcon, hovered ? theme::kText : theme::kLabel);
            } else {
                const ImVec2 c(icon_min.x + kIcon * 0.5f, icon_min.y + kIcon * 0.5f);
                draw->AddCircleFilled(c, kIcon * 0.5f, (accent & 0x00FFFFFFu) | 0x50000000u, 24);
                draw->AddCircle(c, kIcon * 0.5f, accent, 24, 1.5f);
                const char initial[2] = {type->label.empty() ? '?' : type->label[0], 0};
                ImFont* font = ImGui::GetFont();
                const float size = ImGui::GetFontSize() * 1.35f;
                const ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0.0f, initial);
                draw->AddText(font, size, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), IM_COL32(240, 240, 245, 255), initial);
            }
            // Nombre centrado, en dos lineas como mucho.
            const float text_w = tile_w - 10.0f;
            const char* label = type->label.c_str();
            const ImVec2 ts = ImGui::CalcTextSize(label, nullptr, false, text_w);
            const float tx = p0.x + (tile_w - std::min(ts.x, text_w)) * 0.5f;
            const float ty = p0.y + 56.0f;
            draw->PushClipRect(ImVec2(p0.x + 4.0f, ty), ImVec2(p1.x - 4.0f, p1.y - 4.0f), true);
            if (ts.x <= text_w) {
                draw->AddText(ImVec2(tx, ty), hovered ? theme::kText : theme::kLabel, label);
            } else {
                draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(p0.x + 5.0f, ty),
                              hovered ? theme::kText : theme::kLabel, label, nullptr, text_w);
            }
            draw->PopClipRect();
            if (hovered) {
                ImGui::SetTooltip("%s\n%s  ·  %s", type->label.c_str(), type->category.c_str(), type->name.c_str());
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();

    if (chosen != nullptr) {
        for (ecs::Entity e : targets.empty() ? std::vector<ecs::Entity>{entity} : targets) {
            if (!chosen->has(world_, e.handle())) {
                chosen->add(world_, e.handle());
                // Como Unity: caja, esfera y capsula nacen con la medida (AABB) de la malla.
                if (physics::isFittableCollider(chosen->name)) physics_.fitColliderToMesh(e, chosen->name);
            }
        }
        commit();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

}  // namespace cramion::editor
