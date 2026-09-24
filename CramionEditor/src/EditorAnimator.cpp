// Ventana Animator (como la de Unity): edita un Animator Controller
// (.cranimator) como un grafo. Estados = clips; flechas = transiciones con
// condiciones sobre parametros. Con una entidad que usa el controlador
// seleccionada, se ve en vivo el estado activo y se pueden cambiar los
// parametros para probar las transiciones sin modo Play.
//
//   Clic derecho en el fondo   crear estado (vacio o con un clip del modelo)
//   Clic derecho en un estado  crear transicion, estado por defecto, borrar
//   Arrastrar                  mover estados
//   Boton central / Alt+clic   desplazar;  rueda = zoom;  Supr = borrar
//
// Tambien: extraer los clips de un objeto animado a .cranim (dialogo de
// Windows para elegir donde) y crear un Animator con sus clips.

#include "EditorApp.h"

#include "Dialogs.h"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>

namespace cramion::editor {

namespace {

constexpr float kNodeWidth = 170.0f;
constexpr float kNodeHeight = 42.0f;
constexpr int kEntryNode = -2;

const char* const kParameterTypeNames[] = {"Float", "Int", "Bool", "Trigger"};
const char* const kConditionNames[] = {"es verdadero", "es falso", "mayor que", "menor que", "igual a",
                                       "distinto de"};

// Nombre de archivo valido a partir del nombre de un clip.
std::string safeFileName(std::string name) {
    for (char& c : name) {
        if (c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' || c == '|' || c == '?' ||
            c == '*' || static_cast<unsigned char>(c) < 32) {
            c = '_';
        }
    }
    if (name.empty()) name = "Clip";
    return name;
}

std::filesystem::path uniquePath(const std::filesystem::path& folder, const std::string& stem,
                                 const std::string& extension) {
    std::filesystem::path path = folder / dialogs::fromUtf8(stem + extension);
    for (int i = 2; std::filesystem::exists(path); ++i) {
        path = folder / dialogs::fromUtf8(stem + " " + std::to_string(i) + extension);
    }
    return path;
}

float distanceToSegment(ImVec2 p, ImVec2 a, ImVec2 b) {
    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    const float length2 = dx * dx + dy * dy;
    float t = length2 > 0.0f ? ((p.x - a.x) * dx + (p.y - a.y) * dy) / length2 : 0.0f;
    t = std::clamp(t, 0.0f, 1.0f);
    const float x = a.x + t * dx - p.x;
    const float y = a.y + t * dy - p.y;
    return std::sqrt(x * x + y * y);
}

// Los modos que tienen sentido para cada tipo de parametro.
bool conditionFits(ecs::AnimatorParameterType type, int mode) {
    switch (type) {
        case ecs::AnimatorParameterType::Bool: return mode <= 1;
        case ecs::AnimatorParameterType::Trigger: return mode == 0;
        case ecs::AnimatorParameterType::Float: return mode == 2 || mode == 3;
        case ecs::AnimatorParameterType::Int: return mode >= 2;
    }
    return true;
}

int defaultCondition(ecs::AnimatorParameterType type) {
    return type == ecs::AnimatorParameterType::Float || type == ecs::AnimatorParameterType::Int ? 2 : 0;
}

}  // namespace

// --- Assets --------------------------------------------------------------------

void EditorApp::createAnimatorAsset(const std::filesystem::path& folder) {
    ecs::AnimatorController controller;
    controller.uuid = Uuid::generate();

    // Si la seleccion esta animada, un estado por clip (el primero, por
    // defecto), como al arrastrar clips al Animator de Unity.
    std::string stem = "Nuevo Animator";
    ecs::Entity animated;
    if (const ecs::Entity active = world_.find(active_); active.valid()) animated = animatedEntityIn(active);
    if (animated.valid()) {
        if (const asset::ModelData* data = sync_->actorModelData(animated, scene_)) {
            for (std::size_t i = 0; i < data->animations.size(); ++i) {
                ecs::AnimatorState state;
                state.name = data->animations[i].name.empty() ? "Clip " + std::to_string(i) : data->animations[i].name;
                state.clip_name = data->animations[i].name;
                state.position = core::Vec2{0.0f, static_cast<float>(i) * 70.0f};
                controller.states.push_back(std::move(state));
            }
        }
        const ecs::Entity root = world_.find(active_);
        stem = root.name() + " Animator";
    }

    const std::filesystem::path path = uniquePath(folder, safeFileName(stem), ".cranimator");
    std::string error;
    if (!ecs::saveAnimatorController(controller, path, &error)) {
        std::cerr << "[Editor] No se pudo crear el Animator: " << error << "\n";
        return;
    }
    refreshDatabase();
    std::cout << "[Editor] Animator creado: " << dialogs::utf8(path.filename()) << "\n";
    if (animated.valid()) {
        assignAnimatorToSelection(controller.uuid);
    }
    openAnimatorEditor(controller.uuid);
}

void EditorApp::openAnimatorEditor(const Uuid& uuid) {
    if (animator_dirty_) saveAnimatorEditor();
    const auto info = database_->find(uuid);
    if (!info || info->type != assets::AssetType::AnimatorController) return;
    ecs::AnimatorController controller;
    std::string error;
    if (!ecs::loadAnimatorController(info->path, controller, &error)) {
        std::cerr << "[Editor] No se pudo abrir el Animator: " << error << "\n";
        return;
    }
    if (!controller.uuid.valid()) controller.uuid = uuid;
    animator_ = std::move(controller);
    animator_uuid_ = uuid;
    animator_path_ = info->path;
    animator_dirty_ = false;
    animator_selected_state_ = -1;
    animator_selected_transition_ = -1;
    animator_link_from_ = -2;
    animator_drag_ = -3;
    show_animator_ = true;
    animator_focus_ = true;
}

void EditorApp::saveAnimatorEditor() {
    if (animator_path_.empty()) return;
    std::string error;
    if (!ecs::saveAnimatorController(animator_, animator_path_, &error)) {
        std::cerr << "[Editor] No se pudo guardar el Animator: " << error << "\n";
        return;
    }
    animator_dirty_ = false;
    if (sync_) sync_->reloadAnimatorController(animator_uuid_);
}

void EditorApp::assignAnimatorToSelection(const Uuid& uuid) {
    int assigned = 0;
    for (ecs::Entity e : topLevelSelection()) {
        ecs::Entity target = animatedEntityIn(e);
        if (!target.valid()) target = e;
        ecs::Animator* animator = target.tryGet<ecs::Animator>();
        if (animator == nullptr) animator = &target.add<ecs::Animator>();
        animator->controller = assets::AssetRef{uuid, assets::AssetType::AnimatorController};
        animator->runtime = {};
        ++assigned;
    }
    if (assigned > 0) {
        commit();
        std::cout << "[Editor] Animator asignado a " << assigned << " objeto(s)\n";
    }
}

ecs::Entity EditorApp::animatedEntityIn(ecs::Entity entity) {
    if (!entity.valid() || !sync_) return {};
    std::function<ecs::Entity(ecs::Entity)> search = [&](ecs::Entity e) -> ecs::Entity {
        if (e.has<ecs::Animator>()) {
            const asset::ModelData* data = sync_->actorModelData(e, scene_);
            if (data != nullptr && !data->animations.empty()) return e;
        }
        for (const entt::entity child : e.children()) {
            if (!world_.valid(child)) continue;
            if (const ecs::Entity found = search(world_.wrap(child)); found.valid()) return found;
        }
        return {};
    };
    return search(entity);
}

ecs::Entity EditorApp::animatorPreviewEntity() {
    if (!animator_uuid_.valid()) return {};
    const auto uses = [&](ecs::Entity e) {
        const ecs::Animator* a = e.valid() ? e.tryGet<ecs::Animator>() : nullptr;
        return a != nullptr && a->controller.uuid == animator_uuid_;
    };
    // Primero la seleccion (o algo animado dentro de ella)...
    if (const ecs::Entity active = world_.find(active_); active.valid()) {
        if (uses(active)) return active;
        if (const ecs::Entity inside = animatedEntityIn(active); uses(inside)) return inside;
    }
    // ...si no, cualquiera que lo use.
    for (const entt::entity handle : world_.registry().view<ecs::Animator>()) {
        const ecs::Entity e = world_.wrap(handle);
        if (uses(e)) return e;
    }
    return {};
}

// --- Extraer clips ---------------------------------------------------------------

void EditorApp::drawExtractAnimationsMenu(ecs::Entity entity) {
    const ecs::Entity target = animatedEntityIn(entity);
    if (!target.valid()) return;
    const asset::ModelData* data = sync_->actorModelData(target, scene_);
    if (data == nullptr || data->animations.empty()) return;
    if (ImGui::BeginMenu("Animaciones")) {
        if (ImGui::MenuItem("Extraer todas...")) extractAnimations(target, -1);
        ImGui::Separator();
        for (std::size_t i = 0; i < data->animations.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            const std::string label = "Extraer \"" + data->animations[i].name + "\"...";
            if (ImGui::MenuItem(label.c_str())) extractAnimations(target, static_cast<int>(i));
            ImGui::PopID();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Crear Animator con sus clips")) {
            selectOnly(entity.uuid());
            createAnimatorAsset(current_folder_.empty() ? project_.assetsFolder() : current_folder_);
        }
        ImGui::EndMenu();
    }
}

void EditorApp::extractAnimations(ecs::Entity entity, int clip) {
    const asset::ModelData* data = sync_->actorModelData(entity, scene_);
    if (data == nullptr || data->animations.empty()) return;
    const std::filesystem::path initial = current_folder_.empty() ? project_.assetsFolder() : current_folder_;

    std::vector<std::pair<int, std::filesystem::path>> jobs;
    if (clip < 0) {
        const std::filesystem::path folder = dialogs::pickFolder(window_.handle(), initial);
        if (folder.empty()) return;
        for (std::size_t i = 0; i < data->animations.size(); ++i) {
            jobs.emplace_back(static_cast<int>(i),
                              uniquePath(folder, safeFileName(data->animations[i].name), ".cranim"));
        }
    } else {
        if (clip >= static_cast<int>(data->animations.size())) return;
        const std::filesystem::path path = dialogs::saveFile(
            window_.handle(), L"Clip de animacion (*.cranim)\0*.cranim\0", L"cranim", initial,
            dialogs::fromUtf8(safeFileName(data->animations[clip].name)).wstring());
        if (path.empty()) return;
        jobs.emplace_back(clip, path);
    }
    int written = 0;
    for (const auto& [index, path] : jobs) {
        std::string error;
        if (ecs::saveAnimationClip(*data, data->animations[index], path, &error)) {
            ++written;
            std::cout << "[Editor] Clip extraido: " << dialogs::utf8(path) << "\n";
        } else {
            std::cerr << "[Editor] No se pudo extraer " << data->animations[index].name << ": " << error << "\n";
        }
    }
    if (written > 0) refreshDatabase();
}

// --- Ventana --------------------------------------------------------------------

void EditorApp::drawAnimatorEditor() {
    if (animator_focus_) {
        ImGui::SetNextWindowFocus();
        animator_focus_ = false;
    }
    // La primera vez, como pestana junto a la Escena (ancho de sobra).
    if (scene_dock_id_ != 0) ImGui::SetNextWindowDockID(scene_dock_id_, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(900.0f, 520.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Animator", &show_animator_)) {
        ImGui::End();
        return;
    }
    if (!animator_uuid_.valid() || !database_->find(animator_uuid_)) {
        ImGui::TextDisabled("Sin Animator abierto.");
        ImGui::TextDisabled("Proyecto > clic derecho > Crear > Animator, o doble clic en un .cranimator.");
        ImGui::End();
        return;
    }
    const ecs::Entity preview = animatorPreviewEntity();

    ImGui::BeginChild("animator_side", ImVec2(270.0f, 0.0f), ImGuiChildFlags_ResizeX | ImGuiChildFlags_Borders);
    drawAnimatorSidePanel(preview);
    ImGui::EndChild();
    ImGui::SameLine();
    drawAnimatorGraph(preview);

    // Se guarda solo al terminar cada cambio (no mientras se arrastra/escribe).
    if (animator_dirty_ && !ImGui::IsAnyItemActive() && animator_drag_ == -3) {
        saveAnimatorEditor();
    }
    ImGui::End();
}

void EditorApp::drawAnimatorSidePanel(ecs::Entity preview) {
    ecs::AnimatorController& c = animator_;
    const auto info = database_->find(animator_uuid_);
    ImGui::TextUnformatted(info ? info->name.c_str() : "Animator");
    ImGui::SameLine();
    ImGui::TextDisabled(animator_dirty_ ? "(sin guardar)" : "(guardado)");
    ecs::Animator* live = preview.valid() ? preview.tryGet<ecs::Animator>() : nullptr;
    if (live != nullptr) {
        ImGui::TextColored(ImVec4(0.45f, 0.75f, 1.0f, 1.0f), "En vivo: %s", preview.name().c_str());
    } else {
        ImGui::TextDisabled("Selecciona un objeto con este Animator\npara verlo y probarlo en vivo.");
    }
    if (ImGui::Button("Asignar a la selección", ImVec2(-1.0f, 0.0f))) assignAnimatorToSelection(animator_uuid_);

    // --- Parametros ---
    ImGui::SeparatorText("Parámetros");
    if (ImGui::Button("+ Parámetro")) ImGui::OpenPopup("add_parameter");
    if (ImGui::BeginPopup("add_parameter")) {
        for (int t = 0; t < 4; ++t) {
            if (ImGui::MenuItem(kParameterTypeNames[t])) {
                ecs::AnimatorParameter p;
                p.type = static_cast<ecs::AnimatorParameterType>(t);
                std::string name = std::string("Nuevo") + kParameterTypeNames[t];
                for (int i = 2; c.findParameter(name) != nullptr; ++i) {
                    name = std::string("Nuevo") + kParameterTypeNames[t] + std::to_string(i);
                }
                p.name = name;
                c.parameters.push_back(std::move(p));
                animator_dirty_ = true;
            }
        }
        ImGui::EndPopup();
    }
    int remove_parameter = -1;
    for (std::size_t i = 0; i < c.parameters.size(); ++i) {
        ecs::AnimatorParameter& p = c.parameters[i];
        ImGui::PushID(static_cast<int>(i));
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.45f);
        std::string name = p.name;
        if (ImGui::InputText("##name", &name, ImGuiInputTextFlags_EnterReturnsTrue) ||
            (ImGui::IsItemDeactivatedAfterEdit() && name != p.name)) {
            if (!name.empty() && c.findParameter(name) == nullptr) {
                // Renombrar tambien en las condiciones que lo usan.
                for (ecs::AnimatorTransition& t : c.transitions) {
                    for (ecs::AnimatorCondition& k : t.conditions) {
                        if (k.parameter == p.name) k.parameter = name;
                    }
                }
                p.name = name;
                animator_dirty_ = true;
            }
        }
        ImGui::SameLine();
        // Valor: el de la entidad en vivo si la hay (probar), si no el inicial.
        const bool trigger = p.type == ecs::AnimatorParameterType::Trigger;
        float value = live != nullptr ? ecs::animatorParameterValue(c, live->runtime, p.name) : p.default_value;
        bool changed = false;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 24.0f);
        switch (p.type) {
            case ecs::AnimatorParameterType::Float:
                changed = ImGui::DragFloat("##v", &value, 0.01f);
                break;
            case ecs::AnimatorParameterType::Int: {
                int v = static_cast<int>(std::lround(value));
                changed = ImGui::DragInt("##v", &v);
                value = static_cast<float>(v);
                break;
            }
            case ecs::AnimatorParameterType::Bool: {
                bool v = value != 0.0f;
                changed = ImGui::Checkbox("##v", &v);
                value = v ? 1.0f : 0.0f;
                break;
            }
            case ecs::AnimatorParameterType::Trigger:
                if (live != nullptr) {
                    if (ImGui::Button(value != 0.0f ? "● Disparado" : "Disparar")) {
                        value = 1.0f;
                        changed = true;
                    }
                } else {
                    ImGui::TextDisabled("trigger");
                }
                break;
        }
        if (changed) {
            if (live != nullptr) {
                live->runtime.values[p.name] = value;
            } else if (!trigger) {
                p.default_value = value;
                animator_dirty_ = true;
            }
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) remove_parameter = static_cast<int>(i);
        ImGui::PopID();
    }
    if (remove_parameter >= 0) {
        c.parameters.erase(c.parameters.begin() + remove_parameter);
        animator_dirty_ = true;
    }

    // --- Estado seleccionado ---
    if (animator_selected_state_ >= 0 && animator_selected_state_ < static_cast<int>(c.states.size())) {
        ecs::AnimatorState& st = c.states[animator_selected_state_];
        ImGui::SeparatorText("Estado");
        ImGui::InputText("Nombre", &st.name);
        if (ImGui::IsItemDeactivatedAfterEdit()) animator_dirty_ = true;

        // Clips del modelo (de la entidad en vivo); si no hay, el nombre a mano.
        const asset::ModelData* data = preview.valid() ? sync_->actorModelData(preview, scene_) : nullptr;
        if (data != nullptr && !data->animations.empty()) {
            if (ImGui::BeginCombo("Clip", st.clip_name.empty() ? "(ninguno)" : st.clip_name.c_str())) {
                for (const asset::AnimationClip& clip : data->animations) {
                    if (ImGui::Selectable(clip.name.c_str(), clip.name == st.clip_name)) {
                        st.clip_name = clip.name;
                        if (st.name == "Estado" || st.name.empty()) st.name = clip.name;
                        animator_dirty_ = true;
                    }
                }
                ImGui::EndCombo();
            }
        } else {
            ImGui::InputText("Clip", &st.clip_name);
            if (ImGui::IsItemDeactivatedAfterEdit()) animator_dirty_ = true;
        }
        // O un clip suelto (.cranim) arrastrado desde el Proyecto.
        ImGuiPropertyVisitor visitor(database_.get());
        if (visitor.asset({"clip_asset", "Clip (.cranim)", "Si se asigna, manda sobre el clip del modelo"}, st.clip,
                          assets::AssetType::AnimationClip)) {
            animator_dirty_ = true;
        }
        if (ImGui::DragFloat("Velocidad", &st.speed, 0.01f, -4.0f, 4.0f, "%.2f")) {}
        if (ImGui::IsItemDeactivatedAfterEdit()) animator_dirty_ = true;
        if (ImGui::Checkbox("Bucle", &st.loop)) animator_dirty_ = true;
        const bool is_default = c.default_state == animator_selected_state_;
        ImGui::BeginDisabled(is_default);
        if (ImGui::Button(is_default ? "Estado por defecto" : "Hacer estado por defecto")) {
            c.default_state = animator_selected_state_;
            animator_dirty_ = true;
        }
        ImGui::EndDisabled();
        if (live != nullptr && live->runtime.state != animator_selected_state_ && ImGui::Button("Saltar a este estado")) {
            live->runtime.state = animator_selected_state_;
            live->runtime.state_time = 0.0f;
        }
    }

    // --- Transicion seleccionada ---
    if (animator_selected_transition_ >= 0 &&
        animator_selected_transition_ < static_cast<int>(c.transitions.size())) {
        ecs::AnimatorTransition& t = c.transitions[animator_selected_transition_];
        ImGui::SeparatorText("Transición");
        const char* from = t.from == ecs::kAnyState ? "Cualquier estado" : c.states[t.from].name.c_str();
        ImGui::Text("%s  ->  %s", from, c.states[t.to].name.c_str());
        if (ImGui::Checkbox("Exit time", &t.has_exit_time)) animator_dirty_ = true;
        if (t.has_exit_time) {
            ImGui::DragFloat("Salir en", &t.exit_time, 0.01f, 0.0f, 10.0f, "%.2f (clip)");
            if (ImGui::IsItemDeactivatedAfterEdit()) animator_dirty_ = true;
        }
        ImGui::TextDisabled("Condiciones (todas deben cumplirse)");
        int remove_condition = -1;
        for (std::size_t i = 0; i < t.conditions.size(); ++i) {
            ecs::AnimatorCondition& k = t.conditions[i];
            ImGui::PushID(static_cast<int>(i) + 1000);
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.38f);
            if (ImGui::BeginCombo("##param", k.parameter.empty() ? "(parámetro)" : k.parameter.c_str())) {
                for (const ecs::AnimatorParameter& p : c.parameters) {
                    if (ImGui::Selectable(p.name.c_str(), p.name == k.parameter)) {
                        k.parameter = p.name;
                        if (!conditionFits(p.type, static_cast<int>(k.mode))) {
                            k.mode = static_cast<ecs::AnimatorConditionMode>(defaultCondition(p.type));
                        }
                        animator_dirty_ = true;
                    }
                }
                ImGui::EndCombo();
            }
            const ecs::AnimatorParameter* p = c.findParameter(k.parameter);
            const ecs::AnimatorParameterType type = p ? p->type : ecs::AnimatorParameterType::Float;
            ImGui::SameLine();
            const bool numeric = type == ecs::AnimatorParameterType::Float || type == ecs::AnimatorParameterType::Int;
            ImGui::SetNextItemWidth(numeric ? ImGui::GetContentRegionAvail().x * 0.5f
                                            : ImGui::GetContentRegionAvail().x - 24.0f);
            if (ImGui::BeginCombo("##mode", kConditionNames[static_cast<int>(k.mode)])) {
                for (int m = 0; m < 6; ++m) {
                    if (!conditionFits(type, m)) continue;
                    if (ImGui::Selectable(kConditionNames[m], m == static_cast<int>(k.mode))) {
                        k.mode = static_cast<ecs::AnimatorConditionMode>(m);
                        animator_dirty_ = true;
                    }
                }
                ImGui::EndCombo();
            }
            if (numeric) {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 24.0f);
                ImGui::DragFloat("##th", &k.threshold, 0.01f);
                if (ImGui::IsItemDeactivatedAfterEdit()) animator_dirty_ = true;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) remove_condition = static_cast<int>(i);
            ImGui::PopID();
        }
        if (remove_condition >= 0) {
            t.conditions.erase(t.conditions.begin() + remove_condition);
            animator_dirty_ = true;
        }
        if (ImGui::Button("+ Condición")) {
            ecs::AnimatorCondition k;
            if (!c.parameters.empty()) {
                k.parameter = c.parameters.front().name;
                k.mode = static_cast<ecs::AnimatorConditionMode>(defaultCondition(c.parameters.front().type));
            }
            t.conditions.push_back(std::move(k));
            animator_dirty_ = true;
        }
        if (t.conditions.empty() && !t.has_exit_time) {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.35f, 1.0f), "Sin condiciones ni exit time:\nnunca se dispara.");
        }
        if (ImGui::Button("Borrar transición")) {
            c.transitions.erase(c.transitions.begin() + animator_selected_transition_);
            animator_selected_transition_ = -1;
            animator_dirty_ = true;
        }
    }
}

void EditorApp::drawAnimatorGraph(ecs::Entity preview) {
    ecs::AnimatorController& c = animator_;
    ImGui::BeginChild("animator_graph", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoMove);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 size = ImGui::GetContentRegionAvail();
    ImGui::InvisibleButton("##canvas", ImVec2(std::max(size.x, 1.0f), std::max(size.y, 1.0f)),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                               ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    ImGuiIO& io = ImGui::GetIO();
    const float zoom = animator_zoom_;
    const auto toScreen = [&](core::Vec2 p) {
        return ImVec2(origin.x + animator_pan_.x + p.x * zoom, origin.y + animator_pan_.y + p.y * zoom);
    };
    const ImVec2 node_size(kNodeWidth * zoom, kNodeHeight * zoom);

    // Posicion de cada nodo: estados >= 0, Any = -1, Entry = -2.
    const auto nodePosition = [&](int node) -> core::Vec2& {
        if (node == ecs::kAnyState) return c.any_state_position;
        if (node == kEntryNode) return c.entry_position;
        return c.states[node].position;
    };
    const auto nodeCenter = [&](int node) {
        const ImVec2 min = toScreen(nodePosition(node));
        return ImVec2(min.x + node_size.x * 0.5f, min.y + node_size.y * 0.5f);
    };
    const int state_count = static_cast<int>(c.states.size());
    const auto nodeAt = [&](ImVec2 mouse) {
        for (int i = state_count - 1; i >= kEntryNode; --i) {
            const ImVec2 min = toScreen(nodePosition(i));
            if (mouse.x >= min.x && mouse.y >= min.y && mouse.x <= min.x + node_size.x &&
                mouse.y <= min.y + node_size.y) {
                return i;
            }
        }
        return -3;
    };

    draw->PushClipRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), true);
    draw->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), IM_COL32(32, 32, 35, 255));
    // Rejilla.
    const float step = 32.0f * zoom;
    for (float x = std::fmod(animator_pan_.x, step); x < size.x; x += step) {
        draw->AddLine(ImVec2(origin.x + x, origin.y), ImVec2(origin.x + x, origin.y + size.y), IM_COL32(255, 255, 255, 12));
    }
    for (float y = std::fmod(animator_pan_.y, step); y < size.y; y += step) {
        draw->AddLine(ImVec2(origin.x, origin.y + y), ImVec2(origin.x + size.x, origin.y + y), IM_COL32(255, 255, 255, 12));
    }

    // --- Transiciones (flechas) ---
    const auto arrow = [&](ImVec2 a, ImVec2 b, ImU32 color, float thickness) {
        // Desplazada a un lado: A->B y B->A no se pisan.
        const float dx = b.x - a.x;
        const float dy = b.y - a.y;
        const float length = std::max(std::sqrt(dx * dx + dy * dy), 1.0f);
        const ImVec2 n(-dy / length * 6.0f * zoom, dx / length * 6.0f * zoom);
        a = ImVec2(a.x + n.x, a.y + n.y);
        b = ImVec2(b.x + n.x, b.y + n.y);
        draw->AddLine(a, b, color, thickness);
        const ImVec2 mid((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
        const ImVec2 d(dx / length * 7.0f * zoom, dy / length * 7.0f * zoom);
        draw->AddTriangleFilled(ImVec2(mid.x + d.x, mid.y + d.y), ImVec2(mid.x - d.x + d.y * 0.8f, mid.y - d.y - d.x * 0.8f),
                                ImVec2(mid.x - d.x - d.y * 0.8f, mid.y - d.y + d.x * 0.8f), color);
        return std::pair<ImVec2, ImVec2>{a, b};
    };
    if (state_count > 0) {
        arrow(nodeCenter(kEntryNode), nodeCenter(std::clamp(c.default_state, 0, state_count - 1)),
              IM_COL32(230, 150, 60, 255), 2.0f);
    }
    int hovered_transition = -1;
    for (std::size_t i = 0; i < c.transitions.size(); ++i) {
        const ecs::AnimatorTransition& t = c.transitions[i];
        const bool selected = static_cast<int>(i) == animator_selected_transition_;
        const auto [a, b] = arrow(nodeCenter(t.from), nodeCenter(t.to),
                                  selected ? IM_COL32(90, 170, 255, 255) : IM_COL32(200, 200, 205, 220),
                                  selected ? 3.0f : 2.0f);
        if (hovered && distanceToSegment(io.MousePos, a, b) < 6.0f) hovered_transition = static_cast<int>(i);
    }
    if (animator_link_from_ != -2) {
        draw->AddLine(nodeCenter(animator_link_from_), io.MousePos, IM_COL32(90, 170, 255, 255), 2.0f);
    }

    // --- Nodos ---
    const ecs::Animator* live = preview.valid() ? preview.tryGet<ecs::Animator>() : nullptr;
    const asset::ModelData* live_data = preview.valid() ? sync_->actorModelData(preview, scene_) : nullptr;
    const auto drawNode = [&](int node, const char* label, ImU32 fill) {
        const ImVec2 min = toScreen(nodePosition(node));
        const ImVec2 max(min.x + node_size.x, min.y + node_size.y);
        draw->AddRectFilled(min, max, fill, 6.0f * zoom);
        const bool selected = node >= 0 && node == animator_selected_state_;
        draw->AddRect(min, max, selected ? IM_COL32(255, 255, 255, 255) : IM_COL32(0, 0, 0, 160), 6.0f * zoom, 0,
                      selected ? 2.0f : 1.0f);
        // Estado activo en la entidad en vivo: barra de progreso del clip.
        if (node >= 0 && live != nullptr && live->runtime.state == node) {
            float progress = 0.0f;
            if (live_data != nullptr) {
                for (const asset::AnimationClip& clip : live_data->animations) {
                    if (clip.name == c.states[node].clip_name && clip.duration > 0.0f) {
                        progress = std::fmod(live->time, clip.duration) / clip.duration;
                    }
                }
            }
            draw->AddRectFilled(ImVec2(min.x + 4.0f, max.y - 6.0f * zoom),
                                ImVec2(min.x + 4.0f + (node_size.x - 8.0f) * progress, max.y - 3.0f * zoom),
                                IM_COL32(90, 170, 255, 255), 2.0f);
        }
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * std::clamp(zoom, 0.6f, 1.6f));
        const ImVec2 text = ImGui::CalcTextSize(label);
        draw->AddText(ImVec2(min.x + (node_size.x - text.x) * 0.5f, min.y + (node_size.y - text.y) * 0.5f),
                      IM_COL32(245, 245, 245, 255), label);
        ImGui::PopFont();
    };
    drawNode(kEntryNode, "Entry", IM_COL32(60, 140, 70, 255));
    drawNode(ecs::kAnyState, "Cualquier estado", IM_COL32(50, 130, 140, 255));
    for (int i = 0; i < state_count; ++i) {
        ImU32 fill = i == c.default_state ? IM_COL32(190, 110, 40, 255) : IM_COL32(75, 75, 82, 255);
        if (live != nullptr && live->runtime.state == i) fill = IM_COL32(45, 95, 170, 255);
        drawNode(i, c.states[i].name.c_str(), fill);
    }

    // --- Entrada ---
    static int context_node = -3;
    static core::Vec2 context_position{};
    const int hovered_node = hovered ? nodeAt(io.MousePos) : -3;
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !io.KeyAlt) {
        if (animator_link_from_ != -2) {
            // Terminar la transicion sobre un estado.
            if (hovered_node >= 0 && hovered_node != animator_link_from_) {
                ecs::AnimatorTransition t;
                t.from = animator_link_from_;
                t.to = hovered_node;
                t.has_exit_time = animator_link_from_ >= 0 && c.parameters.empty();  // sin parametros: al acabar
                c.transitions.push_back(std::move(t));
                animator_selected_transition_ = static_cast<int>(c.transitions.size()) - 1;
                animator_selected_state_ = -1;
                animator_dirty_ = true;
            }
            animator_link_from_ = -2;
        } else if (hovered_node != -3) {
            animator_drag_ = hovered_node;
            animator_selected_state_ = hovered_node >= 0 ? hovered_node : -1;
            animator_selected_transition_ = -1;
        } else if (hovered_transition >= 0) {
            animator_selected_transition_ = hovered_transition;
            animator_selected_state_ = -1;
        } else {
            animator_selected_state_ = -1;
            animator_selected_transition_ = -1;
        }
    }
    if (animator_drag_ != -3) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f) {
                core::Vec2& p = nodePosition(animator_drag_);
                p.x += io.MouseDelta.x / zoom;
                p.y += io.MouseDelta.y / zoom;
                animator_dirty_ = true;
            }
        } else {
            animator_drag_ = -3;
        }
    }
    // Desplazar y zoom.
    if (ImGui::IsItemActive() && (ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f) ||
                                  (io.KeyAlt && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)))) {
        animator_pan_.x += io.MouseDelta.x;
        animator_pan_.y += io.MouseDelta.y;
    }
    if (hovered && io.MouseWheel != 0.0f) {
        const float old_zoom = animator_zoom_;
        animator_zoom_ = std::clamp(animator_zoom_ * (io.MouseWheel > 0.0f ? 1.1f : 1.0f / 1.1f), 0.4f, 2.0f);
        // Zoom hacia el raton.
        const float mx = io.MousePos.x - origin.x - animator_pan_.x;
        const float my = io.MousePos.y - origin.y - animator_pan_.y;
        animator_pan_.x -= mx * (animator_zoom_ / old_zoom - 1.0f);
        animator_pan_.y -= my * (animator_zoom_ / old_zoom - 1.0f);
    }
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        animator_link_from_ = -2;
        context_node = hovered_node;
        context_position = core::Vec2{(io.MousePos.x - origin.x - animator_pan_.x) / zoom - kNodeWidth * 0.5f,
                                      (io.MousePos.y - origin.y - animator_pan_.y) / zoom - kNodeHeight * 0.5f};
        if (hovered_node >= 0) {
            animator_selected_state_ = hovered_node;
            animator_selected_transition_ = -1;
        } else if (hovered_node == -3 && hovered_transition >= 0) {
            animator_selected_transition_ = hovered_transition;
            animator_selected_state_ = -1;
        }
        ImGui::OpenPopup(hovered_node != -3 ? "node_menu" : (hovered_transition >= 0 ? "transition_menu" : "canvas_menu"));
    }
    const auto deleteSelected = [&] {
        if (animator_selected_state_ >= 0) {
            c.removeState(animator_selected_state_);
            animator_selected_state_ = -1;
            animator_dirty_ = true;
        } else if (animator_selected_transition_ >= 0) {
            c.transitions.erase(c.transitions.begin() + animator_selected_transition_);
            animator_selected_transition_ = -1;
            animator_dirty_ = true;
        }
    };
    if (ImGui::IsWindowFocused() && ImGui::IsKeyPressed(ImGuiKey_Delete, false)) deleteSelected();
    if (ImGui::IsWindowFocused() && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) animator_link_from_ = -2;

    if (ImGui::BeginPopup("canvas_menu")) {
        const auto addState = [&](const std::string& name, const std::string& clip) {
            ecs::AnimatorState st;
            st.name = name;
            st.clip_name = clip;
            st.position = context_position;
            c.states.push_back(std::move(st));
            animator_selected_state_ = static_cast<int>(c.states.size()) - 1;
            animator_selected_transition_ = -1;
            animator_dirty_ = true;
        };
        if (ImGui::MenuItem("Crear estado vacío")) addState("Estado", "");
        if (live_data != nullptr && !live_data->animations.empty() && ImGui::BeginMenu("Crear estado con clip")) {
            for (const asset::AnimationClip& clip : live_data->animations) {
                if (ImGui::MenuItem(clip.name.c_str())) addState(clip.name, clip.name);
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Centrar vista")) {
            animator_pan_ = core::Vec2{size.x * 0.4f, size.y * 0.3f};
            animator_zoom_ = 1.0f;
        }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("node_menu")) {
        if (ImGui::MenuItem("Crear transición", nullptr, false, context_node != kEntryNode)) {
            animator_link_from_ = context_node;
        }
        if (context_node >= 0) {
            if (ImGui::MenuItem("Estado por defecto", nullptr, c.default_state == context_node)) {
                c.default_state = context_node;
                animator_dirty_ = true;
            }
            if (ImGui::MenuItem("Duplicar")) {
                ecs::AnimatorState copy = c.states[context_node];
                copy.name += " copia";
                copy.position.y += 60.0f;
                c.states.push_back(std::move(copy));
                animator_dirty_ = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Borrar", "Supr")) {
                animator_selected_state_ = context_node;
                animator_selected_transition_ = -1;
                deleteSelected();
            }
        }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("transition_menu")) {
        if (ImGui::MenuItem("Borrar transición", "Supr")) deleteSelected();
        ImGui::EndPopup();
    }

    // Ayuda.
    const char* hint = animator_link_from_ != -2
                           ? "Clic en el estado destino (Esc cancela)"
                           : "Clic derecho: crear | Arrastrar: mover | Rueda: zoom | Boton central: desplazar";
    draw->AddText(ImVec2(origin.x + 8.0f, origin.y + size.y - ImGui::GetTextLineHeight() - 6.0f),
                  IM_COL32(255, 255, 255, 110), hint);
    draw->PopClipRect();
    ImGui::EndChild();
}

}  // namespace cramion::editor
