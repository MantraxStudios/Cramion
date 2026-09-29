// Entrada del proyecto (Archivo > Entrada del proyecto), como el Enhanced
// Input de Unreal:
//
//   Acciones   Move (Axis2D), Jump (Bool), Zoom (Axis1D), Fly (Axis3D)...
//              con su tipo, modificadores y triggers.
//   Contextos  Default, Vuelo, Menu... cada uno asigna a las acciones muchas
//              teclas, botones o ejes (con sus modificadores y triggers), con
//              prioridad y si esta activo al empezar.
//   Depurar    en Play: valor y estado de cada accion y los contextos activos.
//
// Se guarda en ProjectSettings/InputActions.json al cambiar algo; en Play se
// aplica al momento. Lua: Input.getAction("Move"), Input.bindAction(...),
// Input.addMappingContext("Vuelo")...

#include "EditorApp.h"

#include "Dialogs.h"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <functional>
#include <iostream>

namespace cramion::editor {

namespace {

std::string lowerText(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool validKey(const std::string& key) { return input::parseSource(key).kind != input::SourceKind::None; }

// Boton con la tecla; al pulsarlo, lista de teclas, raton, mando y tactil.
// `capture`: pide "pulsa una tecla" (lo resuelve el que llama).
bool keyPicker(const char* id, std::string& key, bool& capture) {
    bool changed = false;
    ImGui::PushID(id);
    const bool ok = validKey(key);
    if (!ok) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.4f, 1.0f));
    const std::string label = (key.empty() ? std::string("(ninguna)") : key) + "##key";
    if (ImGui::Button(label.c_str(), ImVec2(170.0f, 0.0f))) ImGui::OpenPopup("keys");
    if (!ok) {
        ImGui::PopStyleColor();
        ImGui::SetItemTooltip("\"%s\" no es una tecla, boton ni eje conocido", key.c_str());
    }
    ImGui::SameLine(0.0f, 2.0f);
    if (ImGui::SmallButton("...")) capture = true;
    ImGui::SetItemTooltip("Pulsar la tecla o el boton (teclado, raton o mando). Escape cancela.");
    ImGui::SetNextWindowSizeConstraints(ImVec2(260.0f, 0.0f), ImVec2(320.0f, 420.0f));
    if (ImGui::BeginPopup("keys")) {
        static std::string filter;
        if (ImGui::IsWindowAppearing()) {
            filter.clear();
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##filter", "buscar (W, Space, Gamepad...)", &filter);
        const std::string f = lowerText(filter);
        for (const input::SourceGroup& group : input::sourceGroups()) {
            bool header = false;
            for (const std::string& name : group.names) {
                if (!f.empty() && lowerText(name).find(f) == std::string::npos) continue;
                if (!header) {
                    ImGui::SeparatorText(group.title);
                    header = true;
                }
                if (ImGui::Selectable(name.c_str(), name == key)) {
                    key = name;
                    changed = true;
                    ImGui::CloseCurrentPopup();
                }
            }
        }
        ImGui::EndPopup();
    }
    ImGui::PopID();
    return changed;
}

std::string modifiersSummary(const std::vector<input::Modifier>& mods) {
    std::string out;
    for (const input::Modifier& m : mods) {
        if (!out.empty()) out += ", ";
        out += input::modifierTypeName(m.type);
        if (m.type == input::ModifierType::Swizzle) out += std::string(" ") + input::swizzleName(m.order);
        if (m.type == input::ModifierType::Negate && !(m.axes[0] && m.axes[1] && m.axes[2])) {
            out += " ";
            if (m.axes[0]) out += "X";
            if (m.axes[1]) out += "Y";
            if (m.axes[2]) out += "Z";
        }
    }
    return out;
}

std::string triggersSummary(const std::vector<input::Trigger>& triggers) {
    std::string out;
    for (const input::Trigger& t : triggers) {
        if (!out.empty()) out += ", ";
        out += input::triggerTypeName(t.type);
        if (t.type == input::TriggerType::Chord && !t.chord.empty()) out += " (" + t.chord + ")";
    }
    return out;
}

bool editModifiers(std::vector<input::Modifier>& mods) {
    bool changed = false;
    int remove = -1;
    for (int i = 0; i < static_cast<int>(mods.size()); ++i) {
        input::Modifier& m = mods[static_cast<std::size_t>(i)];
        ImGui::PushID(i);
        ImGui::SetNextItemWidth(110.0f);
        if (ImGui::BeginCombo("##type", input::modifierTypeName(m.type))) {
            for (int t = 0; t < static_cast<int>(input::ModifierType::Count); ++t) {
                const auto type = static_cast<input::ModifierType>(t);
                if (ImGui::Selectable(input::modifierTypeName(type), m.type == type)) {
                    m.type = type;
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        switch (m.type) {
            case input::ModifierType::Negate: {
                bool x = m.axes[0], y = m.axes[1], z = m.axes[2];
                changed |= ImGui::Checkbox("X", &x);
                ImGui::SameLine();
                changed |= ImGui::Checkbox("Y", &y);
                ImGui::SameLine();
                changed |= ImGui::Checkbox("Z", &z);
                m.axes = {x, y, z};
                break;
            }
            case input::ModifierType::Swizzle:
                ImGui::SetNextItemWidth(80.0f);
                if (ImGui::BeginCombo("##order", input::swizzleName(m.order))) {
                    for (int o = 0; o <= static_cast<int>(input::SwizzleOrder::ZXY); ++o) {
                        const auto order = static_cast<input::SwizzleOrder>(o);
                        if (ImGui::Selectable(input::swizzleName(order), m.order == order)) {
                            m.order = order;
                            changed = true;
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::SameLine();
                ImGui::TextDisabled("(?)");
                ImGui::SetItemTooltip("YXZ: una tecla 1D mueve el eje Y (W = adelante en un Vec2).\n"
                                      "ZYX: al eje Z (adelante en un Vec3). XZY: la Y del stick pasa a Z.");
                break;
            case input::ModifierType::DeadZone:
                ImGui::SetNextItemWidth(70.0f);
                changed |= ImGui::DragFloat("Min", &m.lower, 0.01f, 0.0f, 0.99f, "%.2f");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(70.0f);
                changed |= ImGui::DragFloat("Max", &m.upper, 0.01f, m.lower + 0.01f, 10.0f, "%.2f");
                ImGui::SameLine();
                changed |= ImGui::Checkbox("Radial", &m.radial);
                ImGui::SetItemTooltip("Radial: por la magnitud (sticks). Si no, cada eje por separado.");
                break;
            case input::ModifierType::Scale:
                ImGui::SetNextItemWidth(200.0f);
                changed |= ImGui::DragFloat3("##scale", m.scale.data(), 0.01f, -100.0f, 100.0f, "%.2f");
                break;
            default: break;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) remove = i;
        ImGui::PopID();
    }
    if (remove >= 0) {
        mods.erase(mods.begin() + remove);
        changed = true;
    }
    if (ImGui::SmallButton("+ Modificador")) ImGui::OpenPopup("add_modifier");
    if (ImGui::BeginPopup("add_modifier")) {
        for (int t = 0; t < static_cast<int>(input::ModifierType::Count); ++t) {
            if (ImGui::Selectable(input::modifierTypeName(static_cast<input::ModifierType>(t)))) {
                input::Modifier m;
                m.type = static_cast<input::ModifierType>(t);
                mods.push_back(m);
                changed = true;
            }
        }
        ImGui::EndPopup();
    }
    return changed;
}

bool editTriggers(std::vector<input::Trigger>& triggers, const input::InputActionSettings& settings) {
    bool changed = false;
    int remove = -1;
    for (int i = 0; i < static_cast<int>(triggers.size()); ++i) {
        input::Trigger& t = triggers[static_cast<std::size_t>(i)];
        ImGui::PushID(i);
        ImGui::SetNextItemWidth(130.0f);
        if (ImGui::BeginCombo("##type", input::triggerTypeName(t.type))) {
            for (int k = 0; k < static_cast<int>(input::TriggerType::Count); ++k) {
                const auto type = static_cast<input::TriggerType>(k);
                if (ImGui::Selectable(input::triggerTypeName(type), t.type == type)) {
                    t.type = type;
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (t.type != input::TriggerType::Chord) {
            ImGui::SetNextItemWidth(60.0f);
            changed |= ImGui::DragFloat("Umbral", &t.threshold, 0.01f, 0.0f, 1.0f, "%.2f");
            ImGui::SetItemTooltip("Actuacion: el valor minimo (magnitud) para contar como pulsado.");
            ImGui::SameLine();
        }
        switch (t.type) {
            case input::TriggerType::Hold:
                ImGui::SetNextItemWidth(60.0f);
                changed |= ImGui::DragFloat("s##hold", &t.time, 0.01f, 0.0f, 60.0f, "%.2f");
                ImGui::SameLine();
                changed |= ImGui::Checkbox("Una vez", &t.one_shot);
                break;
            case input::TriggerType::HoldAndRelease:
                ImGui::SetNextItemWidth(60.0f);
                changed |= ImGui::DragFloat("s minimo", &t.time, 0.01f, 0.0f, 60.0f, "%.2f");
                break;
            case input::TriggerType::Tap:
                ImGui::SetNextItemWidth(60.0f);
                changed |= ImGui::DragFloat("s maximo", &t.time, 0.01f, 0.01f, 10.0f, "%.2f");
                break;
            case input::TriggerType::Pulse:
                ImGui::SetNextItemWidth(60.0f);
                changed |= ImGui::DragFloat("s intervalo", &t.time, 0.01f, 0.01f, 60.0f, "%.2f");
                ImGui::SameLine();
                changed |= ImGui::Checkbox("Al empezar", &t.pulse_at_start);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(60.0f);
                changed |= ImGui::DragInt("Limite", &t.pulse_limit, 0.1f, 0, 1000);
                break;
            case input::TriggerType::Chord:
                ImGui::SetNextItemWidth(140.0f);
                if (ImGui::BeginCombo("Con la accion", t.chord.empty() ? "(elige)" : t.chord.c_str())) {
                    for (const input::InputAction& a : settings.actions) {
                        if (ImGui::Selectable(a.name.c_str(), a.name == t.chord)) {
                            t.chord = a.name;
                            changed = true;
                        }
                    }
                    ImGui::EndCombo();
                }
                break;
            default: break;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) remove = i;
        ImGui::PopID();
    }
    if (remove >= 0) {
        triggers.erase(triggers.begin() + remove);
        changed = true;
    }
    if (ImGui::SmallButton("+ Trigger")) ImGui::OpenPopup("add_trigger");
    ImGui::SetItemTooltip("Sin triggers: se dispara mientras hay valor (Down).");
    if (ImGui::BeginPopup("add_trigger")) {
        for (int k = 0; k < static_cast<int>(input::TriggerType::Count); ++k) {
            if (ImGui::Selectable(input::triggerTypeName(static_cast<input::TriggerType>(k)))) {
                input::Trigger t;
                t.type = static_cast<input::TriggerType>(k);
                if (t.type == input::TriggerType::Tap) t.time = 0.2f;
                if (t.type == input::TriggerType::Pulse) t.time = 0.25f;
                triggers.push_back(t);
                changed = true;
            }
        }
        ImGui::EndPopup();
    }
    return changed;
}

std::string uniqueName(const std::string& base, const std::function<bool(const std::string&)>& taken) {
    if (!taken(base)) return base;
    for (int i = 2;; ++i) {
        const std::string name = base + " " + std::to_string(i);
        if (!taken(name)) return name;
    }
}

}  // namespace

void EditorApp::loadInputActions() {
    if (!has_project_) return;
    const std::filesystem::path file = input::inputActionsFile(project_.settingsFolder());
    if (input_actions_file_ == file) return;
    input_actions_file_ = file;
    input_actions_ = input::loadInputActions(file);
}

void EditorApp::saveInputActionsNow() {
    if (input_actions_file_.empty()) return;
    if (!input::saveInputActions(input_actions_file_, input_actions_)) {
        std::cerr << "[Entrada] No se pudo guardar " << dialogs::utf8(input_actions_file_) << "\n";
    }
    if (playing()) scripts_.setInputActions(input_actions_);  // en Play, al momento
}

void EditorApp::drawInputActionsWindow() {
    if (!show_input_actions_ || !has_project_) return;
    loadInputActions();
    ImGui::SetNextWindowSize(ImVec2(900.0f, 620.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    if (!ImGui::Begin("Entrada del proyecto", &show_input_actions_)) {
        ImGui::End();
        return;
    }
    input::InputActionSettings& s = input_actions_;
    bool changed = false;
    ImGui::TextWrapped("Como el Enhanced Input de Unreal: las Acciones dicen que hace el jugador (Move = Vec2, Jump = Bool, "
                       "Zoom = float, Fly = Vec3) y los Contextos asignan a cada accion muchas teclas, botones o ejes. "
                       "Se guarda en ProjectSettings/InputActions.json.");

    if (ImGui::BeginTabBar("##input_tabs")) {
        // ------------------------------------------------------------------
        if (ImGui::BeginTabItem("Acciones (Input Actions)")) {
            input_action_selected_ = std::clamp(input_action_selected_, 0, std::max(0, static_cast<int>(s.actions.size()) - 1));
            ImGui::BeginChild("##action_list", ImVec2(200.0f, 0.0f), ImGuiChildFlags_Borders);
            for (int i = 0; i < static_cast<int>(s.actions.size()); ++i) {
                const input::InputAction& a = s.actions[static_cast<std::size_t>(i)];
                const std::string label = a.name + "##a" + std::to_string(i);
                if (ImGui::Selectable(label.c_str(), i == input_action_selected_)) input_action_selected_ = i;
                ImGui::SameLine(150.0f);
                static constexpr const char* kShort[] = {"bool", "float", "vec2", "vec3"};
                ImGui::TextDisabled("%s", kShort[static_cast<int>(a.type)]);
            }
            ImGui::Separator();
            if (ImGui::Button("+ Accion")) {
                input::InputAction a;
                a.name = uniqueName("NuevaAccion", [&](const std::string& n) { return s.findAction(n) != nullptr; });
                s.actions.push_back(a);
                input_action_selected_ = static_cast<int>(s.actions.size()) - 1;
                changed = true;
            }
            ImGui::EndChild();
            ImGui::SameLine();
            ImGui::BeginChild("##action_detail", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
            if (!s.actions.empty()) {
                input::InputAction& a = s.actions[static_cast<std::size_t>(input_action_selected_)];
                const std::string old_name = a.name;
                // Se edita una copia; al terminar se renombra y las teclas y
                // los Chord de todos los contextos la siguen.
                static std::string name_edit;
                static int name_edit_for = -1;
                static bool name_editing = false;
                if (name_edit_for != input_action_selected_ || !name_editing) {
                    name_edit = a.name;
                    name_edit_for = input_action_selected_;
                }
                ImGui::SetNextItemWidth(260.0f);
                ImGui::InputText("Nombre", &name_edit);
                name_editing = ImGui::IsItemActive();
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    if (!name_edit.empty() && name_edit != old_name && s.findAction(name_edit) == nullptr) {
                        a.name = name_edit;
                        for (input::MappingContext& c : s.contexts) {
                            for (input::KeyMapping& m : c.mappings) {
                                if (m.action == old_name) m.action = a.name;
                                for (input::Trigger& t : m.triggers) {
                                    if (t.chord == old_name) t.chord = a.name;
                                }
                            }
                        }
                        for (input::InputAction& other : s.actions) {
                            for (input::Trigger& t : other.triggers) {
                                if (t.chord == old_name) t.chord = a.name;
                            }
                        }
                        changed = true;
                    }
                    // Vacio o repetido: vuelve al nombre (se copia el frame siguiente).
                }
                ImGui::SetNextItemWidth(260.0f);
                if (ImGui::BeginCombo("Tipo de valor", input::valueTypeName(a.type))) {
                    for (int t = 0; t <= static_cast<int>(input::ValueType::Axis3D); ++t) {
                        const auto type = static_cast<input::ValueType>(t);
                        if (ImGui::Selectable(input::valueTypeName(type), a.type == type)) {
                            a.type = type;
                            changed = true;
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::SetItemTooltip("Lua: Input.getAction devuelve true/false (Bool), un numero (Axis1D) o un Vec3 (Axis2D/3D).");
                ImGui::SetNextItemWidth(-1.0f);
                changed |= ImGui::InputTextWithHint("##desc", "Descripcion", &a.description);
                int acc = a.accumulate ? 1 : 0;
                ImGui::SetNextItemWidth(260.0f);
                if (ImGui::Combo("Varias teclas a la vez", &acc, "Mayor valor por sentido (W+S = 0)\0Sumar\0")) {
                    a.accumulate = acc == 1;
                    changed = true;
                }
                changed |= ImGui::Checkbox("Consumir las teclas", &a.consume_input);
                ImGui::SetItemTooltip("Sus teclas no llegan a contextos de menos prioridad (como bConsumeInput).");

                ImGui::SeparatorText("Modificadores de la accion");
                ImGui::TextDisabled("Despues de los de cada tecla.");
                ImGui::PushID("amods");
                changed |= editModifiers(a.modifiers);
                ImGui::PopID();
                ImGui::SeparatorText("Triggers de la accion");
                ImGui::PushID("atrig");
                changed |= editTriggers(a.triggers, s);
                ImGui::PopID();

                ImGui::SeparatorText("Teclas (en los contextos)");
                bool any = false;
                for (const input::MappingContext& c : s.contexts) {
                    std::string keys;
                    for (const input::KeyMapping& m : c.mappings) {
                        if (m.action != a.name) continue;
                        if (!keys.empty()) keys += ", ";
                        keys += m.key;
                    }
                    if (keys.empty()) continue;
                    any = true;
                    ImGui::TextWrapped("%s: %s", c.name.c_str(), keys.c_str());
                }
                if (!any) ImGui::TextDisabled("Ninguna. Anadela en la pestana Contextos.");

                ImGui::Separator();
                if (ImGui::Button("Duplicar")) {
                    input::InputAction copy = a;
                    copy.name = uniqueName(a.name, [&](const std::string& n) { return s.findAction(n) != nullptr; });
                    s.actions.push_back(copy);
                    input_action_selected_ = static_cast<int>(s.actions.size()) - 1;
                    changed = true;
                }
                ImGui::SameLine();
                if (ImGui::Button("Borrar accion")) {
                    const std::string name = a.name;
                    s.actions.erase(s.actions.begin() + input_action_selected_);
                    name_edit_for = -1;
                    for (input::MappingContext& c : s.contexts) {
                        std::erase_if(c.mappings, [&](const input::KeyMapping& m) { return m.action == name; });
                    }
                    changed = true;
                }
                ImGui::SameLine();
                ImGui::TextDisabled("Lua: Input.getAction(\"%s\")", old_name.c_str());
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }

        // ------------------------------------------------------------------
        if (ImGui::BeginTabItem("Contextos (Mapping Contexts)")) {
            input_context_selected_ = std::clamp(input_context_selected_, 0, std::max(0, static_cast<int>(s.contexts.size()) - 1));
            ImGui::BeginChild("##context_list", ImVec2(200.0f, 0.0f), ImGuiChildFlags_Borders);
            for (int i = 0; i < static_cast<int>(s.contexts.size()); ++i) {
                const input::MappingContext& c = s.contexts[static_cast<std::size_t>(i)];
                const std::string label = c.name + "##c" + std::to_string(i);
                if (ImGui::Selectable(label.c_str(), i == input_context_selected_)) {
                    input_context_selected_ = i;
                    input_capture_mapping_ = -1;
                }
                ImGui::SameLine(150.0f);
                ImGui::TextDisabled("%d%s", c.priority, c.active_at_start ? " *" : "");
            }
            ImGui::Separator();
            if (ImGui::Button("+ Contexto")) {
                input::MappingContext c;
                c.name = uniqueName("Contexto", [&](const std::string& n) { return s.findContext(n) != nullptr; });
                c.active_at_start = false;
                s.contexts.push_back(c);
                input_context_selected_ = static_cast<int>(s.contexts.size()) - 1;
                changed = true;
            }
            ImGui::TextDisabled("* activo al empezar");
            ImGui::EndChild();
            ImGui::SameLine();
            ImGui::BeginChild("##context_detail", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
            if (!s.contexts.empty()) {
                input::MappingContext& c = s.contexts[static_cast<std::size_t>(input_context_selected_)];
                ImGui::SetNextItemWidth(200.0f);
                const std::string old_name = c.name;
                if (ImGui::InputText("Nombre", &c.name)) changed = true;
                if (c.name.empty()) c.name = old_name;
                ImGui::SameLine();
                ImGui::SetNextItemWidth(90.0f);
                changed |= ImGui::InputInt("Prioridad", &c.priority);
                ImGui::SetItemTooltip("Con varios contextos activos, el de mas prioridad se queda las teclas que comparten.");
                ImGui::SameLine();
                changed |= ImGui::Checkbox("Activo al empezar", &c.active_at_start);
                ImGui::SetItemTooltip("Si no, se activa desde Lua: Input.addMappingContext(\"%s\")", c.name.c_str());
                ImGui::SetNextItemWidth(-1.0f);
                changed |= ImGui::InputTextWithHint("##cdesc", "Descripcion", &c.description);

                // Captura de "pulsa una tecla" de la fila elegida.
                if (input_capture_mapping_ >= 0 && input_capture_mapping_ < static_cast<int>(c.mappings.size()) && input_ != nullptr) {
                    if (input_->isKeyPressed(dm::Key::Escape)) {
                        input_capture_mapping_ = -1;
                    } else {
                        const std::string pressed = input::pressedSourceName(*input_);
                        if (!pressed.empty()) {
                            c.mappings[static_cast<std::size_t>(input_capture_mapping_)].key = pressed;
                            input_capture_mapping_ = -1;
                            changed = true;
                        }
                    }
                }

                // Por accion: sus teclas (como la lista de mappings de Unreal).
                std::vector<std::string> used;
                for (const input::KeyMapping& m : c.mappings) {
                    if (std::find(used.begin(), used.end(), m.action) == used.end()) used.push_back(m.action);
                }
                int remove = -1;
                std::string remove_action;
                for (const std::string& action_name : used) {
                    const input::InputAction* action = s.findAction(action_name);
                    ImGui::PushID(action_name.c_str());
                    const std::string header = action_name + "   (" +
                                               (action != nullptr ? input::valueTypeName(action->type) : "no existe") + ")###hdr";
                    const bool open = ImGui::CollapsingHeader(header.c_str(), ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);
                    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 60.0f);
                    if (ImGui::SmallButton("+ tecla")) {
                        input::KeyMapping m;
                        m.action = action_name;
                        // Justo despues de la ultima de esa accion.
                        auto at = c.mappings.end();
                        for (auto it = c.mappings.begin(); it != c.mappings.end(); ++it) {
                            if (it->action == action_name) at = it + 1;
                        }
                        const auto inserted = c.mappings.insert(at, m);
                        input_capture_mapping_ = static_cast<int>(inserted - c.mappings.begin());
                        changed = true;
                    }
                    ImGui::SetItemTooltip("Anade una tecla y espera a que la pulses (o eligela de la lista)");
                    ImGui::SameLine();
                    if (ImGui::SmallButton("x")) remove_action = action_name;
                    ImGui::SetItemTooltip("Quitar la accion de este contexto");
                    if (open) {
                        ImGui::Indent(8.0f);
                        for (int mi = 0; mi < static_cast<int>(c.mappings.size()); ++mi) {
                            input::KeyMapping& m = c.mappings[static_cast<std::size_t>(mi)];
                            if (m.action != action_name) continue;
                            ImGui::PushID(mi);
                            bool capture = false;
                            if (input_capture_mapping_ == mi) {
                                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.25f, 0.45f, 0.85f, 1.0f));
                                ImGui::Button("Pulsa una tecla...##cap", ImVec2(170.0f, 0.0f));
                                ImGui::PopStyleColor();
                                ImGui::SameLine(0.0f, 2.0f);
                                if (ImGui::SmallButton("...")) input_capture_mapping_ = -1;
                            } else {
                                changed |= keyPicker("key", m.key, capture);
                                if (capture) input_capture_mapping_ = mi;
                            }
                            ImGui::SameLine();
                            if (ImGui::SmallButton("x##del")) remove = mi;
                            ImGui::SetItemTooltip("Quitar esta tecla");
                            ImGui::SameLine();
                            const std::string mods = modifiersSummary(m.modifiers);
                            const std::string trig = triggersSummary(m.triggers);
                            std::string node = mods.empty() ? std::string("Sin modificadores") : mods;
                            if (!trig.empty()) node += "  |  " + trig;
                            node += "###opts";
                            if (ImGui::TreeNodeEx(node.c_str())) {
                                ImGui::TextDisabled("Modificadores");
                                ImGui::PushID("mods");
                                changed |= editModifiers(m.modifiers);
                                ImGui::PopID();
                                ImGui::TextDisabled("Triggers");
                                ImGui::PushID("trig");
                                changed |= editTriggers(m.triggers, s);
                                ImGui::PopID();
                                changed |= ImGui::Checkbox("El jugador puede cambiarla", &m.player_mappable);
                                ImGui::SetItemTooltip("Input.rebind(contexto, accion, n, tecla) en el menu de opciones del juego.");
                                ImGui::TreePop();
                            }
                            ImGui::PopID();
                        }
                        ImGui::Unindent(8.0f);
                    }
                    ImGui::PopID();
                }
                if (remove >= 0) {
                    c.mappings.erase(c.mappings.begin() + remove);
                    input_capture_mapping_ = -1;
                    changed = true;
                }
                if (!remove_action.empty()) {
                    std::erase_if(c.mappings, [&](const input::KeyMapping& m) { return m.action == remove_action; });
                    input_capture_mapping_ = -1;
                    changed = true;
                }
                ImGui::Spacing();
                ImGui::SetNextItemWidth(220.0f);
                if (ImGui::BeginCombo("##add_action", "+ Anadir accion al contexto")) {
                    for (const input::InputAction& a : s.actions) {
                        if (std::find(used.begin(), used.end(), a.name) != used.end()) continue;
                        if (ImGui::Selectable(a.name.c_str())) {
                            input::KeyMapping m;
                            m.action = a.name;
                            c.mappings.push_back(m);
                            input_capture_mapping_ = static_cast<int>(c.mappings.size()) - 1;
                            changed = true;
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::Separator();
                if (ImGui::Button("Duplicar contexto")) {
                    input::MappingContext copy = c;
                    copy.name = uniqueName(c.name, [&](const std::string& n) { return s.findContext(n) != nullptr; });
                    copy.active_at_start = false;
                    s.contexts.push_back(copy);
                    input_context_selected_ = static_cast<int>(s.contexts.size()) - 1;
                    changed = true;
                } else {
                    ImGui::SameLine();
                    if (ImGui::Button("Borrar contexto")) {
                        s.contexts.erase(s.contexts.begin() + input_context_selected_);
                        input_capture_mapping_ = -1;
                        changed = true;
                    }
                }
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }

        // ------------------------------------------------------------------
        if (ImGui::BeginTabItem("Depurar (Play)")) {
            if (!playing()) {
                ImGui::TextDisabled("Dale a Play para ver el valor y el estado de cada accion.");
            } else {
                const input::InputMapper& mapper = scripts_.inputMapper();
                std::string contexts;
                for (const std::string& name : mapper.activeContexts()) contexts += (contexts.empty() ? "" : ", ") + name;
                ImGui::Text("Contextos activos: %s", contexts.empty() ? "(ninguno)" : contexts.c_str());
                if (ImGui::BeginTable("##debug", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
                    ImGui::TableSetupColumn("Accion");
                    ImGui::TableSetupColumn("Estado");
                    ImGui::TableSetupColumn("Valor");
                    ImGui::TableSetupColumn("Tiempo");
                    ImGui::TableHeadersRow();
                    const auto& actions = mapper.settings().actions;
                    for (std::size_t i = 0; i < actions.size() && i < mapper.states().size(); ++i) {
                        const input::ActionState& st = mapper.states()[i];
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(actions[i].name.c_str());
                        ImGui::TableNextColumn();
                        const ImVec4 color = st.state == input::TriggerState::Triggered ? ImVec4(0.4f, 0.9f, 0.4f, 1.0f)
                                             : st.state == input::TriggerState::Ongoing  ? ImVec4(0.95f, 0.8f, 0.3f, 1.0f)
                                                                                         : ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
                        ImGui::TextColored(color, "%s", input::triggerStateName(st.state));
                        ImGui::TableNextColumn();
                        switch (actions[i].type) {
                            case input::ValueType::Bool: ImGui::TextUnformatted(st.value.x > 0.5f ? "true" : "false"); break;
                            case input::ValueType::Axis1D: ImGui::Text("%.3f", st.value.x); break;
                            case input::ValueType::Axis2D: ImGui::Text("(%.2f, %.2f)", st.value.x, st.value.y); break;
                            case input::ValueType::Axis3D: ImGui::Text("(%.2f, %.2f, %.2f)", st.value.x, st.value.y, st.value.z); break;
                        }
                        ImGui::TableNextColumn();
                        ImGui::Text("%.2f s", st.elapsed);
                    }
                    ImGui::EndTable();
                }
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    if (changed) saveInputActionsNow();
    ImGui::End();
}

}  // namespace cramion::editor
