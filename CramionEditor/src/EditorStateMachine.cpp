// Ventana "Maquina de estados" (como los State Graphs de Bolt / Visual
// Scripting de Unity): edita un .crfsm como un grafo. La logica de cada
// estado va en un script de C++ del objeto, que recibe los mensajes
// OnStateEnter / OnStateUpdate / OnStateExit (ai/StateMachine.h). Las flechas
// son transiciones con condiciones: variables de la pizarra, triggers,
// temporizadores o una expresion (ai/Expression.h). En Play, el objeto seleccionado que usa la maquina se ve en
// vivo: estado activo, ultima transicion, variables y el historial.
//
//   Clic derecho en el fondo   crear estado (vacio o con una plantilla)
//   Clic derecho en un estado  crear transicion, estado de entrada, borrar
//   Arrastrar desde el punto   crear una transicion hasta otro estado
//   Arrastrar                  mover estados
//   Boton central / Alt+clic   desplazar;  rueda = zoom;  Supr = borrar
//
// Tambien: el Inspector del componente StateMachine (valores propios de cada
// objeto) y las herramientas MCP de maquinas de estados.

#include "EditorApp.h"

#include "Dialogs.h"

#include <imgui.h>
#include <imgui_stdlib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>

namespace cramion::editor {

namespace {

using nlohmann::json;

constexpr float kNodeWidth = 170.0f;
constexpr float kNodeHeight = 46.0f;
constexpr int kEntryNode = -2;
constexpr int kNoNode = -3;   // ni estado, ni Entrada, ni Cualquier estado
constexpr int kNoDrag = -4;

const char* const kConditionKindNames[] = {"Variable", "Trigger", "Temporizador", "Expresión"};

std::string safeFileName(std::string name) {
    for (char& c : name) {
        if (c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' || c == '|' || c == '?' ||
            c == '*' || static_cast<unsigned char>(c) < 32) {
            c = '_';
        }
    }
    if (name.empty()) name = "Maquina";
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

ImU32 colorOf(const core::Vec3& c, float scale = 1.0f, int alpha = 255) {
    const auto channel = [&](float v) { return static_cast<int>(std::clamp(v * scale, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return IM_COL32(channel(c.x), channel(c.y), channel(c.z), alpha);
}

// Plantillas del menu "Crear estado de IA" (nombre y color tipicos).
struct StateRecipe {
    const char* name;
    core::Vec3 color;
};

const StateRecipe kRecipes[] = {
    {"Esperar", {0.30f, 0.30f, 0.33f}},  {"Patrullar", {0.20f, 0.45f, 0.25f}}, {"Perseguir", {0.62f, 0.45f, 0.12f}},
    {"Atacar", {0.62f, 0.18f, 0.18f}},   {"Huir", {0.45f, 0.25f, 0.60f}},
};

// Lo que recibe el script de C++ del objeto (para la nota del panel).
constexpr const char* kCppExample =
    "void onMessage(const std::string& method, const Value& v) override {\n"
    "    if (method == \"OnStateEnter\" && v[\"state\"].asString() == \"Perseguir\") { ... }\n"
    "    if (method == \"OnStateUpdate\") { ... }  // con \"Enviar OnStateUpdate\"\n"
    "}";

// Objeto por nombre, tag o UUID (como las variables entity en Play).
std::uint32_t resolveEntity(const ecs::World& world, const std::string& text) {
    if (text.empty()) return ai::kNoEntity;
    ecs::Entity e;
    if (const Uuid uuid = Uuid::parse(text); uuid.valid()) e = world.find(uuid);
    if (!e.valid()) e = world.findByName(text);
    if (!e.valid()) e = world.findWithTag(text);
    return e.valid() ? static_cast<std::uint32_t>(entt::to_integral(e.handle())) : ai::kNoEntity;
}

struct EditResult {
    bool changed = false;
    bool finished = false;  // se solto el control (punto de deshacer / guardar)
};

// Control del valor de una variable segun su tipo. Las entity aceptan
// arrastrar un objeto desde la Jerarquia (se guarda su nombre).
EditResult valueEditor(const char* id, ai::Value& v, const ecs::World* world) {
    EditResult r;
    ImGui::PushID(id);
    ImGui::BeginGroup();
    switch (v.type) {
        case ai::VarType::Bool: r.changed = ImGui::Checkbox("##v", &v.b); break;
        case ai::VarType::Int: {
            int i = static_cast<int>(std::llround(v.n));
            if (ImGui::DragInt("##v", &i)) {
                v.n = static_cast<double>(i);
                r.changed = true;
            }
            break;
        }
        case ai::VarType::Float: {
            float f = static_cast<float>(v.n);
            if (ImGui::DragFloat("##v", &f, 0.05f, 0.0f, 0.0f, "%.3g")) {
                v.n = static_cast<double>(f);
                r.changed = true;
            }
            break;
        }
        case ai::VarType::String: r.changed = ImGui::InputText("##v", &v.s); break;
        case ai::VarType::Entity: {
            r.changed = ImGui::InputTextWithHint("##v", "nombre, tag o UUID", &v.s);
            if (world != nullptr && ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kEntityPayload)) {
                    Uuid uuid{};
                    std::memcpy(&uuid, payload->Data, sizeof(Uuid));
                    if (const ecs::Entity e = world->find(uuid); e.valid()) {
                        v.s = e.name();
                        r.changed = true;
                        r.finished = true;
                    }
                }
                ImGui::EndDragDropTarget();
            }
            if (world != nullptr && r.changed) v.entity = resolveEntity(*world, v.s);
            break;
        }
        case ai::VarType::Vector: {
            float xyz[3] = {v.v.x, v.v.y, v.v.z};
            if (ImGui::DragFloat3("##v", xyz, 0.05f)) {
                v.v = core::Vec3{xyz[0], xyz[1], xyz[2]};
                r.changed = true;
            }
            break;
        }
    }
    ImGui::EndGroup();
    r.finished = r.finished || ImGui::IsItemDeactivatedAfterEdit() || (v.type == ai::VarType::Bool && r.changed);
    ImGui::PopID();
    return r;
}

// Los modos de comparacion que tienen sentido para un tipo.
bool compareFits(ai::VarType type, ai::Compare c) {
    switch (type) {
        case ai::VarType::Bool:
        case ai::VarType::String:
        case ai::VarType::Entity:
            return c == ai::Compare::Equal || c == ai::Compare::NotEqual || c == ai::Compare::IsTrue ||
                   c == ai::Compare::IsFalse;
        default: return true;
    }
}

// "distancia < $rangoVision", "trigger ruido", "2 s en el estado"...
std::string conditionText(const ai::Condition& c) {
    switch (c.kind) {
        case ai::ConditionKind::Variable:
            if (c.compare == ai::Compare::IsTrue) return c.variable;
            if (c.compare == ai::Compare::IsFalse) return "no " + c.variable;
            return c.variable + " " + ai::compareKey(c.compare) + " " + c.value;
        case ai::ConditionKind::Trigger: return "trigger \"" + c.variable + "\"";
        case ai::ConditionKind::Timer: {
            char text[48];
            std::snprintf(text, sizeof(text), "%.2g s en el estado", static_cast<double>(c.seconds));
            return text;
        }
        case ai::ConditionKind::Expression: return "si " + c.expression;
    }
    return {};
}

std::string transitionText(const ai::StateMachineAsset& m, const ai::Transition& t) {
    const int count = static_cast<int>(m.states.size());
    const std::string from = t.from == ai::kAnyState ? std::string("Cualquier estado")
                                                     : (t.from >= 0 && t.from < count ? m.states[t.from].name : "?");
    const std::string to = t.to >= 0 && t.to < count ? m.states[t.to].name : "?";
    std::string text = from + " -> " + to;
    if (t.conditions.empty()) return text + "\n(sin condiciones: enseguida)";
    for (std::size_t i = 0; i < t.conditions.size(); ++i) {
        text += (i == 0 ? "\n  si " : "\n  y ") + conditionText(t.conditions[i]);
    }
    if (t.priority != 0) text += "\n  prioridad " + std::to_string(t.priority);
    return text;
}

// Un valor del JSON (MCP) como texto para Value::parse.
std::string jsonText(const json& j) {
    if (j.is_string()) return j.get<std::string>();
    if (j.is_boolean()) return j.get<bool>() ? "true" : "false";
    if (j.is_array()) {
        std::string out;
        for (const json& x : j) {
            if (!out.empty()) out += ' ';
            out += x.is_number() ? x.dump() : std::string("0");
        }
        return out;
    }
    if (j.is_null()) return {};
    return j.dump();
}

}  // namespace

// --- Assets --------------------------------------------------------------------

Uuid EditorApp::createStateMachineAsset(const std::filesystem::path& folder, bool example) {
    ai::StateMachineAsset m = example ? ai::exampleEnemyStateMachine() : ai::StateMachineAsset{};
    m.uuid = Uuid::generate();
    if (!example) {
        ai::State first;
        first.name = "Inicio";
        m.states.push_back(std::move(first));
    }
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    const std::filesystem::path path =
        uniquePath(folder, example ? "Enemigo IA" : "Maquina de estados", ai::kStateMachineExtension);
    std::string error;
    if (!ai::saveStateMachine(m, path, &error)) {
        std::cerr << "[Editor] No se pudo crear la maquina de estados: " << error << "\n";
        return {};
    }
    refreshDatabase();
    std::cout << "[Editor] Maquina de estados creada: " << dialogs::utf8(path.filename()) << "\n";
    openStateMachineEditor(m.uuid);
    return m.uuid;
}

void EditorApp::openStateMachineEditor(const Uuid& uuid) {
    if (loadStateMachineEditor(uuid)) openStateMachineWorkspace();
}

bool EditorApp::loadStateMachineEditor(const Uuid& uuid) {
    if (fsm_dirty_) saveStateMachineEditor();
    const auto info = database_->find(uuid);
    if (!info || info->type != assets::AssetType::StateMachine) return false;
    ai::StateMachineAsset machine;
    std::string error;
    if (!ai::loadStateMachine(info->path, machine, &error)) {
        std::cerr << "[Editor] No se pudo abrir la maquina de estados: " << error << "\n";
        return false;
    }
    if (!machine.uuid.valid()) machine.uuid = uuid;
    fsm_ = std::move(machine);
    fsm_uuid_ = uuid;
    fsm_path_ = info->path;
    fsm_dirty_ = false;
    fsm_selected_transition_ = -1;
    fsm_link_from_ = kNoNode;
    fsm_drag_ = kNoDrag;
    fsm_selected_state_ = fsm_.states.empty() ? kNoNode : std::clamp(fsm_.entry_state, 0, static_cast<int>(fsm_.states.size()) - 1);
    show_state_machine_ = true;
    fsm_focus_ = true;
    return true;
}

void EditorApp::saveStateMachineEditor() {
    if (fsm_path_.empty()) return;
    std::string error;
    if (!ai::saveStateMachine(fsm_, fsm_path_, &error)) {
        std::cerr << "[Editor] No se pudo guardar la maquina de estados: " << error << "\n";
        return;
    }
    fsm_dirty_ = false;
    fsm_cache_.erase(fsm_uuid_);
    // En Play: se recompila y cada objeto sigue en su estado.
    scripts_.clearErrors();
    scripts_.reloadFile(assetRelative(fsm_path_));
}

void EditorApp::assignStateMachineToSelection(const Uuid& uuid) {
    int assigned = 0;
    for (ecs::Entity e : topLevelSelection()) {
        ai::StateMachine& sm = e.has<ai::StateMachine>() ? e.get<ai::StateMachine>() : e.add<ai::StateMachine>();
        sm.machine = assets::AssetRef{uuid, assets::AssetType::StateMachine};
        sm.runtime = {};
        ++assigned;
    }
    if (assigned > 0) {
        commit();
        std::cout << "[Editor] Maquina de estados asignada a " << assigned << " objeto(s)\n";
    }
}

ecs::Entity EditorApp::stateMachineLiveEntity() {
    if (!fsm_uuid_.valid()) return {};
    const auto uses = [&](const ecs::Entity& e) {
        const ai::StateMachine* c = e.valid() ? e.tryGet<ai::StateMachine>() : nullptr;
        return c != nullptr && c->machine.uuid == fsm_uuid_;
    };
    if (const ecs::Entity active = world_.find(active_); uses(active)) return active;
    for (const entt::entity handle : world_.registry().view<ai::StateMachine>()) {
        const ecs::Entity e = world_.wrap(handle);
        if (uses(e)) return e;
    }
    return {};
}

const ai::StateMachineAsset* EditorApp::stateMachineAsset(const Uuid& uuid) {
    if (!uuid.valid()) return nullptr;
    if (uuid == fsm_uuid_ && !fsm_path_.empty()) return &fsm_;
    const auto info = database_->find(uuid);
    if (!info || info->type != assets::AssetType::StateMachine) return nullptr;
    std::error_code ec;
    const std::filesystem::file_time_type time = std::filesystem::last_write_time(info->path, ec);
    if (const auto it = fsm_cache_.find(uuid); it != fsm_cache_.end() && it->second.time == time) {
        return &it->second.machine;
    }
    FsmAssetCache entry;
    if (!ai::loadStateMachine(info->path, entry.machine, nullptr)) return nullptr;
    entry.time = time;
    return &(fsm_cache_[uuid] = std::move(entry)).machine;
}

// --- Ventana --------------------------------------------------------------------

// Se dibuja en su propia pestana (EditorWorkspaces.cpp), a toda la ventana.
void EditorApp::drawStateMachineEditor() {
    fsm_focus_ = false;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                   ImGuiWindowFlags_NoSavedSettings;
    if (!ImGui::Begin("Máquina de estados###fsm_workspace", nullptr, flags)) {
        ImGui::End();
        return;
    }
    if (!fsm_uuid_.valid() || !database_->find(fsm_uuid_)) {
        ImGui::TextDisabled("Sin máquina de estados abierta.");
        ImGui::TextDisabled("Proyecto > clic derecho > Crear > Máquina de estados, o doble clic en un .crfsm.");
        if (has_project_) {
            const std::filesystem::path folder = current_folder_.empty() ? project_.assetsFolder() : current_folder_;
            if (ImGui::Button("Crear máquina vacía")) createStateMachineAsset(folder, false);
            ImGui::SameLine();
            if (ImGui::Button("Crear enemigo de ejemplo")) createStateMachineAsset(folder, true);
        }
        ImGui::End();
        return;
    }
    const ecs::Entity live = stateMachineLiveEntity();

    // --- Barra superior ---
    const auto info = database_->find(fsm_uuid_);
    ImGui::TextUnformatted(info ? info->name.c_str() : "Máquina de estados");
    ImGui::SameLine();
    ImGui::TextDisabled(fsm_dirty_ ? "(sin guardar)" : "(guardado)");
    ImGui::SameLine();
    if (ImGui::SmallButton("Guardar")) saveStateMachineEditor();
    ImGui::SameLine();
    if (ImGui::SmallButton("Asignar a la selección")) assignStateMachineToSelection(fsm_uuid_);
    ImGui::SetItemTooltip("Añade el componente StateMachine con esta máquina a los objetos seleccionados\n"
                          "(también: arrastrar el .crfsm a un objeto de la Jerarquía)");
    ImGui::SameLine();
    const ai::StateMachine* live_sm = live.valid() ? live.tryGet<ai::StateMachine>() : nullptr;
    if (live_sm != nullptr && playing() && live_sm->runtime.started) {
        const int s = live_sm->runtime.state;
        const char* name = s >= 0 && s < static_cast<int>(fsm_.states.size()) ? fsm_.states[s].name.c_str() : "-";
        ImGui::TextColored(ImVec4(0.45f, 0.75f, 1.0f, 1.0f), "En vivo: %s  ·  %s  (%.1f s)%s", live.name().c_str(), name,
                           static_cast<double>(live_sm->runtime.state_time), live_sm->runtime.running ? "" : "  [parada]");
    } else if (live.valid()) {
        if (playing()) ImGui::TextDisabled("En vivo: %s (aún no arranca)", live.name().c_str());
        else ImGui::TextDisabled("Objeto: %s  (en Play se ve en vivo)", live.name().c_str());
    } else {
        ImGui::TextDisabled("Ningún objeto usa esta máquina");
    }
    const std::vector<std::string> problems = ai::validateStateMachine(fsm_);
    if (!problems.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.35f, 1.0f), "%zu problema(s)", problems.size());
        if (ImGui::IsItemHovered()) {
            std::string all;
            for (const std::string& p : problems) all += p + "\n";
            ImGui::SetTooltip("%s", all.c_str());
        }
    }

    // --- Paneles: variables | grafo | codigo ---
    ImGui::BeginChild("fsm_left", ImVec2(270.0f, 0.0f), ImGuiChildFlags_ResizeX | ImGuiChildFlags_Borders);
    drawStateMachineVariables(live);
    ImGui::EndChild();
    ImGui::SameLine();
    drawStateMachineGraph(live);
    ImGui::SameLine(0.0f, 0.0f);
    // Separador para cambiar el ancho del panel del codigo.
    const float height = std::max(ImGui::GetContentRegionAvail().y, 1.0f);
    ImGui::InvisibleButton("##fsm_split", ImVec2(6.0f, height));
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (ImGui::IsItemActive()) fsm_right_width_ = std::clamp(fsm_right_width_ - ImGui::GetIO().MouseDelta.x, 260.0f, 1400.0f);
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::BeginChild("fsm_right", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
    drawStateMachineDetails(live);
    ImGui::EndChild();

    // Se guarda al terminar cada cambio (no mientras se arrastra o se escribe).
    if (fsm_dirty_ && !ImGui::IsAnyItemActive() && fsm_drag_ == kNoDrag) saveStateMachineEditor();
    ImGui::End();
}

void EditorApp::drawStateMachineVariables(ecs::Entity live) {
    ai::StateMachineAsset& m = fsm_;
    ai::StateMachine* sm = live.valid() ? live.tryGet<ai::StateMachine>() : nullptr;
    const bool running = sm != nullptr && playing() && sm->runtime.started;

    ImGui::SeparatorText("Variables (pizarra)");
    if (ImGui::Button("+ Variable")) ImGui::OpenPopup("add_variable");
    ImGui::SetItemTooltip("Variables de la máquina: las leen las condiciones y el código (self.vars.nombre).\n"
                          "Cada objeto puede cambiar el valor inicial en el Inspector.");
    if (ImGui::BeginPopup("add_variable")) {
        for (int t = 0; t < ai::kVarTypeCount; ++t) {
            const ai::VarType type = static_cast<ai::VarType>(t);
            if (ImGui::MenuItem(ai::varTypeLabel(type))) {
                std::string name = std::string("nueva") + ai::varTypeLabel(type);
                for (int i = 2; m.findVariable(name) != nullptr; ++i) {
                    name = std::string("nueva") + ai::varTypeLabel(type) + std::to_string(i);
                }
                ai::Variable v;
                v.name = name;
                v.value.type = type;
                m.variables.push_back(std::move(v));
                fsm_dirty_ = true;
            }
        }
        ImGui::EndPopup();
    }
    if (running) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.45f, 0.75f, 1.0f, 1.0f), "valores en vivo");
    }
    int remove = -1;
    for (std::size_t i = 0; i < m.variables.size(); ++i) {
        ai::Variable& v = m.variables[i];
        ImGui::PushID(static_cast<int>(i));
        // Nombre (renombrar tambien en las condiciones).
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.42f);
        std::string name = v.name;
        if (ImGui::InputText("##name", &name, ImGuiInputTextFlags_EnterReturnsTrue) ||
            (ImGui::IsItemDeactivatedAfterEdit() && name != v.name)) {
            if (!name.empty() && m.findVariable(name) == nullptr) {
                m.renameVariable(v.name, name);
                fsm_dirty_ = true;
            }
        }
        ImGui::SetItemTooltip("%s", ai::varTypeLabel(v.value.type));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 24.0f);
        if (running) {
            // En Play: el valor de ese objeto (se puede cambiar para probar).
            if (ai::Value* value = sm->runtime.find(v.name)) {
                valueEditor("live", *value, &world_);
            } else {
                ImGui::TextDisabled("-");
            }
        } else {
            const EditResult r = valueEditor("value", v.value, nullptr);
            if (r.finished || (r.changed && v.value.type == ai::VarType::Bool)) fsm_dirty_ = true;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) remove = static_cast<int>(i);
        // Tipo (clic derecho en el nombre).
        if (ImGui::BeginPopupContextItem("var_type")) {
            ImGui::TextDisabled("Tipo");
            for (int t = 0; t < ai::kVarTypeCount; ++t) {
                const ai::VarType type = static_cast<ai::VarType>(t);
                if (ImGui::MenuItem(ai::varTypeLabel(type), nullptr, type == v.value.type)) {
                    v.value = ai::Value::parse(type, v.value.text());
                    fsm_dirty_ = true;
                }
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    if (remove >= 0) {
        m.variables.erase(m.variables.begin() + remove);
        fsm_dirty_ = true;
    }
    if (m.variables.empty()) ImGui::TextDisabled("Sin variables.");
    else ImGui::TextDisabled("Clic derecho en una x: cambiar el tipo");

    // --- Depuracion en vivo ---
    ImGui::SeparatorText("Depuración");
    if (!running) {
        ImGui::TextWrapped("%s", playing() ? "Selecciona un objeto con esta máquina para verla en vivo."
                                           : "En Play: el estado activo se ilumina en el grafo, la última transición "
                                             "parpadea y aquí se ven las variables y el historial del objeto "
                                             "seleccionado.");
        return;
    }
    ai::Runtime& rt = sm->runtime;
    const int count = static_cast<int>(m.states.size());
    ImGui::Text("Objeto: %s", live.name().c_str());
    ImGui::Text("Estado: %s", rt.state >= 0 && rt.state < count ? m.states[rt.state].name.c_str() : "-");
    ImGui::TextDisabled("%.1f s en el estado · %llu cambios", static_cast<double>(rt.state_time),
                        static_cast<unsigned long long>(rt.changes));
    if (rt.last_transition >= 0 && rt.last_transition < static_cast<int>(m.transitions.size())) {
        ImGui::TextDisabled("Última: transición %d", rt.last_transition + 1);
    } else if (rt.last_transition == -2) {
        ImGui::TextDisabled("Última: por código (sm:go)");
    }
    if (ImGui::Button(rt.running ? "Pausar máquina" : "Reanudar máquina")) rt.running = !rt.running;
    // Disparar un trigger a mano.
    static std::string trigger;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 70.0f);
    ImGui::InputTextWithHint("##trigger", "trigger", &trigger);
    ImGui::SameLine();
    if (ImGui::Button("Disparar") && !trigger.empty()) rt.triggers.push_back(trigger);
    // Variables que el codigo creo (no estan en el asset).
    for (ai::Variable& v : rt.vars) {
        if (m.findVariable(v.name) != nullptr) continue;
        ImGui::PushID(v.name.c_str());
        ImGui::TextDisabled("%s", v.name.c_str());
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        valueEditor("extra", v.value, &world_);
        ImGui::PopID();
    }
    ImGui::SeparatorText("Historial");
    for (auto it = rt.history.rbegin(); it != rt.history.rend(); ++it) ImGui::TextDisabled("%s", it->c_str());
}

void EditorApp::drawStateMachineGraph(ecs::Entity live) {
    ai::StateMachineAsset& m = fsm_;
    const float width = std::max(ImGui::GetContentRegionAvail().x - fsm_right_width_ - 6.0f, 120.0f);
    ImGui::BeginChild("fsm_graph", ImVec2(width, 0.0f), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoMove);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 size = ImGui::GetContentRegionAvail();
    ImGui::InvisibleButton("##canvas", ImVec2(std::max(size.x, 1.0f), std::max(size.y, 1.0f)),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                               ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    ImGuiIO& io = ImGui::GetIO();
    const float zoom = fsm_zoom_;
    const auto toScreen = [&](core::Vec2 p) {
        return ImVec2(origin.x + fsm_pan_.x + p.x * zoom, origin.y + fsm_pan_.y + p.y * zoom);
    };
    const ImVec2 node_size(kNodeWidth * zoom, kNodeHeight * zoom);
    const int state_count = static_cast<int>(m.states.size());

    // Posicion de cada nodo: estados >= 0, Cualquier estado = -1, Entrada = -2.
    const auto nodePosition = [&](int node) -> core::Vec2& {
        if (node == ai::kAnyState) return m.any_state_position;
        if (node == kEntryNode) return m.entry_position;
        return m.states[node].position;
    };
    const auto nodeCenter = [&](int node) {
        const ImVec2 min = toScreen(nodePosition(node));
        return ImVec2(min.x + node_size.x * 0.5f, min.y + node_size.y * 0.5f);
    };
    // El punto de la derecha de un nodo: arrastrar desde el crea una transicion.
    const auto portOf = [&](int node) {
        const ImVec2 min = toScreen(nodePosition(node));
        return ImVec2(min.x + node_size.x, min.y + node_size.y * 0.5f);
    };
    const auto nodeAt = [&](ImVec2 mouse) {
        for (int i = state_count - 1; i >= kEntryNode; --i) {
            const ImVec2 min = toScreen(nodePosition(i));
            if (mouse.x >= min.x && mouse.y >= min.y && mouse.x <= min.x + node_size.x &&
                mouse.y <= min.y + node_size.y) {
                return i;
            }
        }
        return kNoNode;
    };
    const auto portAt = [&](ImVec2 mouse) {
        for (int i = state_count - 1; i >= ai::kAnyState; --i) {
            const ImVec2 p = portOf(i);
            const float dx = mouse.x - p.x;
            const float dy = mouse.y - p.y;
            if (dx * dx + dy * dy <= 81.0f) return i;
        }
        return kNoNode;
    };

    draw->PushClipRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), true);
    draw->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), IM_COL32(30, 31, 35, 255));
    const float step = 32.0f * zoom;
    for (float x = std::fmod(fsm_pan_.x, step); x < size.x; x += step) {
        draw->AddLine(ImVec2(origin.x + x, origin.y), ImVec2(origin.x + x, origin.y + size.y), IM_COL32(255, 255, 255, 12));
    }
    for (float y = std::fmod(fsm_pan_.y, step); y < size.y; y += step) {
        draw->AddLine(ImVec2(origin.x, origin.y + y), ImVec2(origin.x + size.x, origin.y + y), IM_COL32(255, 255, 255, 12));
    }

    // En vivo (Play): estado activo y ultima transicion.
    const ai::StateMachine* sm = live.valid() ? live.tryGet<ai::StateMachine>() : nullptr;
    const bool running = sm != nullptr && playing() && sm->runtime.started;
    const int active_state = running ? sm->runtime.state : kNoNode;
    // La ultima transicion se ilumina 1.5 s y se apaga.
    float flash = 0.0f;
    if (running && sm->runtime.last_transition >= 0) {
        flash = std::clamp(1.0f - (sm->runtime.time - sm->runtime.last_change_time) / 1.5f, 0.0f, 1.0f);
    }

    // --- Transiciones ---
    const auto arrow = [&](ImVec2 a, ImVec2 b, ImU32 color, float thickness, int count_label) {
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
        if (count_label > 1) {
            const std::string text = std::to_string(count_label);
            draw->AddText(ImVec2(mid.x + n.x * 2.0f, mid.y + n.y * 2.0f), color, text.c_str());
        }
        return std::pair<ImVec2, ImVec2>{a, b};
    };
    if (state_count > 0) {
        arrow(nodeCenter(kEntryNode), nodeCenter(std::clamp(m.entry_state, 0, state_count - 1)),
              IM_COL32(230, 150, 60, 255), 2.0f, 0);
    }
    int hovered_transition = -1;
    for (std::size_t i = 0; i < m.transitions.size(); ++i) {
        const ai::Transition& t = m.transitions[i];
        if (t.to < 0 || t.to >= state_count || t.from < ai::kAnyState || t.from >= state_count) continue;
        const bool selected = static_cast<int>(i) == fsm_selected_transition_;
        const bool last = running && sm->runtime.last_transition == static_cast<int>(i) && flash > 0.0f;
        // Varias entre los mismos estados: una flecha con su numero.
        int same = 0;
        for (const ai::Transition& o : m.transitions) same += o.from == t.from && o.to == t.to ? 1 : 0;
        ImU32 color = selected ? IM_COL32(90, 170, 255, 255) : IM_COL32(200, 200, 205, 220);
        if (t.from == ai::kAnyState && !selected) color = IM_COL32(110, 200, 210, 200);
        if (last) color = IM_COL32(255, 190, 60, static_cast<int>(120 + 135 * flash));
        const auto [a, b] = arrow(nodeCenter(t.from), nodeCenter(t.to), color, selected || last ? 3.0f : 2.0f, same);
        if (hovered && distanceToSegment(io.MousePos, a, b) < 6.0f) hovered_transition = static_cast<int>(i);
    }
    if (fsm_link_from_ != kNoNode) {
        draw->AddLine(portOf(fsm_link_from_), io.MousePos, IM_COL32(90, 170, 255, 255), 2.0f);
    }

    // --- Nodos ---
    const auto drawNode = [&](int node, const std::string& label, const std::string& sub, ImU32 fill) {
        const ImVec2 min = toScreen(nodePosition(node));
        const ImVec2 max(min.x + node_size.x, min.y + node_size.y);
        const bool active = node >= 0 && node == active_state;
        if (active) {
            // Halo que late: el estado en el que esta el objeto.
            const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(ImGui::GetTime()) * 6.0f);
            draw->AddRect(ImVec2(min.x - 4.0f, min.y - 4.0f), ImVec2(max.x + 4.0f, max.y + 4.0f),
                          IM_COL32(90, 170, 255, static_cast<int>(120 + 135 * pulse)), 8.0f * zoom, 0, 3.0f);
        }
        draw->AddRectFilled(min, max, fill, 6.0f * zoom);
        const bool selected = node == fsm_selected_state_;
        draw->AddRect(min, max, selected ? IM_COL32(255, 255, 255, 255) : IM_COL32(0, 0, 0, 160), 6.0f * zoom, 0,
                      selected ? 2.0f : 1.0f);
        if (active && sm != nullptr) {
            // Tiempo en el estado: una barra que se llena cada segundo.
            const float t = std::fmod(sm->runtime.state_time, 1.0f);
            draw->AddRectFilled(ImVec2(min.x + 4.0f, max.y - 6.0f * zoom),
                                ImVec2(min.x + 4.0f + (node_size.x - 8.0f) * t, max.y - 3.0f * zoom),
                                IM_COL32(90, 170, 255, 255), 2.0f);
        }
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * std::clamp(zoom, 0.6f, 1.6f));
        const ImVec2 text = ImGui::CalcTextSize(label.c_str());
        const float line = ImGui::GetTextLineHeight();
        const float top = sub.empty() ? min.y + (node_size.y - text.y) * 0.5f : min.y + node_size.y * 0.5f - line;
        draw->AddText(ImVec2(min.x + (node_size.x - text.x) * 0.5f, top), IM_COL32(245, 245, 245, 255), label.c_str());
        ImGui::PopFont();
        if (!sub.empty()) {
            ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * std::clamp(zoom * 0.8f, 0.5f, 1.3f));
            const ImVec2 s = ImGui::CalcTextSize(sub.c_str());
            draw->AddText(ImVec2(min.x + (node_size.x - s.x) * 0.5f, top + line + 1.0f), IM_COL32(230, 230, 235, 170),
                          sub.c_str());
            ImGui::PopFont();
        }
        if (node != kEntryNode) {
            const ImVec2 p = portOf(node);
            draw->AddCircleFilled(p, 4.5f * zoom, IM_COL32(220, 220, 225, 255));
            draw->AddCircle(p, 4.5f * zoom, IM_COL32(0, 0, 0, 200));
        }
    };
    drawNode(kEntryNode, "Entrada", "", IM_COL32(60, 140, 70, 255));
    drawNode(ai::kAnyState, "Cualquier estado", m.any_code.find_first_not_of(" \t\r\n") == std::string::npos ? "" : "(sensores)",
             IM_COL32(50, 130, 140, 255));
    for (int i = 0; i < state_count; ++i) {
        const ai::State& s = m.states[i];
        std::string sub;
        if (!s.script.empty()) sub = dialogs::utf8(dialogs::fromUtf8(s.script).filename());
        else if (i == m.entry_state) sub = "(entrada)";
        if (running && i == active_state) {
            char t[32];
            std::snprintf(t, sizeof(t), "%.1f s", static_cast<double>(sm->runtime.state_time));
            sub = t;
        }
        drawNode(i, s.name, sub, colorOf(s.color, i == active_state ? 1.35f : 1.0f));
    }

    // --- Entrada del raton ---
    static int context_node = kNoNode;
    static core::Vec2 context_position{};
    static bool link_by_drag = false;
    const int hovered_node = hovered ? nodeAt(io.MousePos) : kNoNode;
    const int hovered_port = hovered ? portAt(io.MousePos) : kNoNode;
    const auto finishLink = [&](int target) {
        if (target >= 0 && (target != fsm_link_from_ || fsm_link_from_ == ai::kAnyState)) {
            ai::Transition t;
            t.from = fsm_link_from_;
            t.to = target;
            m.transitions.push_back(std::move(t));
            fsm_selected_transition_ = static_cast<int>(m.transitions.size()) - 1;
            fsm_selected_state_ = kNoNode;
            fsm_dirty_ = true;
        }
        fsm_link_from_ = kNoNode;
        link_by_drag = false;
    };
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !io.KeyAlt) {
        if (fsm_link_from_ != kNoNode) {
            finishLink(hovered_node);  // modo clic: el destino
        } else if (hovered_port != kNoNode) {
            fsm_link_from_ = hovered_port;  // arrastrar desde el punto
            link_by_drag = true;
        } else if (hovered_node != kNoNode) {
            fsm_drag_ = hovered_node;
            fsm_selected_state_ = hovered_node == kEntryNode ? kNoNode : hovered_node;
            fsm_selected_transition_ = -1;
        } else if (hovered_transition >= 0) {
            fsm_selected_transition_ = hovered_transition;
            fsm_selected_state_ = kNoNode;
        } else {
            fsm_selected_state_ = kNoNode;
            fsm_selected_transition_ = -1;
        }
    }
    if (link_by_drag && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        const int target = nodeAt(io.MousePos);
        if (target == fsm_link_from_ || target == kNoNode) {
            // Soltar sobre el mismo nodo (o fuera): se sigue en modo clic.
            link_by_drag = false;
            if (target == kNoNode) fsm_link_from_ = kNoNode;
        } else {
            finishLink(target);
        }
    }
    if (fsm_drag_ != kNoDrag) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f) {
                core::Vec2& p = nodePosition(fsm_drag_);
                p.x += io.MouseDelta.x / zoom;
                p.y += io.MouseDelta.y / zoom;
                fsm_dirty_ = true;
            }
        } else {
            fsm_drag_ = kNoDrag;
        }
    }
    if (hovered && hovered_transition >= 0 && hovered_node == kNoNode && fsm_link_from_ == kNoNode) {
        ImGui::SetTooltip("%d. %s", hovered_transition + 1, transitionText(m, m.transitions[hovered_transition]).c_str());
    }
    // Desplazar y zoom.
    if (ImGui::IsItemActive() && (ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f) ||
                                  (io.KeyAlt && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)))) {
        fsm_pan_.x += io.MouseDelta.x;
        fsm_pan_.y += io.MouseDelta.y;
    }
    if (hovered && io.MouseWheel != 0.0f) {
        const float old_zoom = fsm_zoom_;
        fsm_zoom_ = std::clamp(fsm_zoom_ * (io.MouseWheel > 0.0f ? 1.1f : 1.0f / 1.1f), 0.4f, 2.0f);
        const float mx = io.MousePos.x - origin.x - fsm_pan_.x;
        const float my = io.MousePos.y - origin.y - fsm_pan_.y;
        fsm_pan_.x -= mx * (fsm_zoom_ / old_zoom - 1.0f);
        fsm_pan_.y -= my * (fsm_zoom_ / old_zoom - 1.0f);
    }
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        fsm_link_from_ = kNoNode;
        link_by_drag = false;
        context_node = hovered_node;
        context_position = core::Vec2{(io.MousePos.x - origin.x - fsm_pan_.x) / zoom - kNodeWidth * 0.5f,
                                      (io.MousePos.y - origin.y - fsm_pan_.y) / zoom - kNodeHeight * 0.5f};
        if (hovered_node >= ai::kAnyState) {
            fsm_selected_state_ = hovered_node;
            fsm_selected_transition_ = -1;
        } else if (hovered_node == kNoNode && hovered_transition >= 0) {
            fsm_selected_transition_ = hovered_transition;
            fsm_selected_state_ = kNoNode;
        }
        ImGui::OpenPopup(hovered_node != kNoNode ? "node_menu" : (hovered_transition >= 0 ? "transition_menu" : "canvas_menu"));
    }
    const auto deleteSelected = [&] {
        if (fsm_selected_state_ >= 0 && fsm_selected_state_ < state_count) {
            m.removeState(fsm_selected_state_);
            fsm_selected_state_ = kNoNode;
            fsm_selected_transition_ = -1;
            fsm_dirty_ = true;
        } else if (fsm_selected_transition_ >= 0 && fsm_selected_transition_ < static_cast<int>(m.transitions.size())) {
            m.transitions.erase(m.transitions.begin() + fsm_selected_transition_);
            fsm_selected_transition_ = -1;
            fsm_dirty_ = true;
        }
    };
    if (ImGui::IsWindowFocused() && ImGui::IsKeyPressed(ImGuiKey_Delete, false)) deleteSelected();
    if (ImGui::IsWindowFocused() && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        fsm_link_from_ = kNoNode;
        link_by_drag = false;
    }

    const auto uniqueStateName = [&](const std::string& base) {
        std::string name = base;
        for (int i = 2; m.findState(name) >= 0; ++i) name = base + " " + std::to_string(i);
        return name;
    };
    if (ImGui::BeginPopup("canvas_menu")) {
        const auto addState = [&](const std::string& base, const core::Vec3& color) {
            ai::State st;
            st.name = uniqueStateName(base);
            st.position = context_position;
            st.color = color;
            m.states.push_back(std::move(st));
            fsm_selected_state_ = static_cast<int>(m.states.size()) - 1;
            fsm_selected_transition_ = -1;
            fsm_dirty_ = true;
        };
        if (ImGui::MenuItem("Crear estado")) addState("Estado", core::Vec3{0.30f, 0.30f, 0.33f});
        if (ImGui::BeginMenu("Crear estado de IA")) {
            for (const StateRecipe& r : kRecipes) {
                if (ImGui::MenuItem(r.name)) addState(r.name, r.color);
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Centrar vista")) {
            fsm_pan_ = core::Vec2{size.x * 0.35f, size.y * 0.3f};
            fsm_zoom_ = 1.0f;
        }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("node_menu")) {
        if (ImGui::MenuItem("Crear transición", nullptr, false, context_node != kEntryNode)) fsm_link_from_ = context_node;
        if (context_node >= 0 && context_node < state_count) {
            if (ImGui::MenuItem("Estado de entrada", nullptr, m.entry_state == context_node)) {
                m.entry_state = context_node;
                fsm_dirty_ = true;
            }
            if (ImGui::MenuItem("Duplicar")) {
                ai::State copy = m.states[context_node];
                copy.name = uniqueStateName(copy.name + " copia");
                copy.position.y += 70.0f;
                m.states.push_back(std::move(copy));
                fsm_dirty_ = true;
            }
            if (running && ImGui::MenuItem("Ir a este estado (Play)")) {
                if (ai::StateMachine* c = live.tryGet<ai::StateMachine>()) c->runtime.requested = context_node;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Borrar", "Supr")) {
                fsm_selected_state_ = context_node;
                fsm_selected_transition_ = -1;
                deleteSelected();
            }
        }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("transition_menu")) {
        if (ImGui::MenuItem("Borrar transición", "Supr")) deleteSelected();
        ImGui::EndPopup();
    }

    const char* hint = fsm_link_from_ != kNoNode
                           ? "Clic en el estado destino (Esc cancela)"
                           : "Clic derecho: crear | Arrastrar desde el punto: transición | Rueda: zoom | Botón central: desplazar";
    draw->AddText(ImVec2(origin.x + 8.0f, origin.y + size.y - ImGui::GetTextLineHeight() - 6.0f),
                  IM_COL32(255, 255, 255, 110), hint);
    draw->PopClipRect();
    ImGui::EndChild();
}

void EditorApp::drawStateMachineDetails(ecs::Entity live) {
    ai::StateMachineAsset& m = fsm_;
    const int count = static_cast<int>(m.states.size());
    ai::StateMachine* sm = live.valid() ? live.tryGet<ai::StateMachine>() : nullptr;
    const bool running = sm != nullptr && playing() && sm->runtime.started;

    // --- Transicion seleccionada ---
    if (fsm_selected_transition_ >= 0 && fsm_selected_transition_ < static_cast<int>(m.transitions.size())) {
        const int index = fsm_selected_transition_;
        ai::Transition& t = m.transitions[index];
        ImGui::SeparatorText("Transición");
        const char* from = t.from == ai::kAnyState ? "Cualquier estado"
                                                   : (t.from >= 0 && t.from < count ? m.states[t.from].name.c_str() : "?");
        ImGui::Text("%d.  %s  ->  %s", index + 1, from, t.to >= 0 && t.to < count ? m.states[t.to].name.c_str() : "?");
        ImGui::SetNextItemWidth(120.0f);
        ImGui::DragInt("Prioridad", &t.priority, 0.1f, -100, 100);
        if (ImGui::IsItemDeactivatedAfterEdit()) fsm_dirty_ = true;
        ImGui::SetItemTooltip("Si varias se cumplen a la vez gana la de mayor prioridad;\n"
                              "a igual prioridad, Cualquier estado y después el orden de la lista.");
        if (t.from == ai::kAnyState) {
            if (ImGui::Checkbox("También si ya está en el destino", &t.allow_self)) fsm_dirty_ = true;
        }
        ImGui::TextDisabled("Condiciones (todas deben cumplirse)");
        if (running) ImGui::TextDisabled("En vivo: verde = se cumple ahora");
        int remove = -1;
        for (std::size_t k = 0; k < t.conditions.size(); ++k) {
            ai::Condition& c = t.conditions[k];
            ImGui::PushID(static_cast<int>(k) + 1000);
            ImGui::Separator();
            if (running) {
                const bool holds = ai::conditionHolds(m, sm->runtime, c);
                ImGui::TextColored(holds ? ImVec4(0.4f, 1.0f, 0.5f, 1.0f) : ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "●");
                ImGui::SameLine();
            }
            int kind = static_cast<int>(c.kind);
            ImGui::SetNextItemWidth(130.0f);
            if (ImGui::Combo("##kind", &kind, kConditionKindNames, ai::kConditionKindCount)) {
                c.kind = static_cast<ai::ConditionKind>(kind);
                if (c.kind == ai::ConditionKind::Variable && c.variable.empty() && !m.variables.empty()) {
                    c.variable = m.variables.front().name;
                }
                if (c.kind == ai::ConditionKind::Expression && c.expression.empty()) c.expression = "true";
                fsm_dirty_ = true;
            }
            ImGui::SameLine();
            switch (c.kind) {
                case ai::ConditionKind::Variable: {
                    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 24.0f);
                    if (ImGui::BeginCombo("##var", c.variable.empty() ? "(variable)" : c.variable.c_str())) {
                        for (const ai::Variable& v : m.variables) {
                            if (ImGui::Selectable(v.name.c_str(), v.name == c.variable)) {
                                c.variable = v.name;
                                if (!compareFits(v.value.type, c.compare)) c.compare = ai::Compare::IsTrue;
                                fsm_dirty_ = true;
                            }
                        }
                        ImGui::EndCombo();
                    }
                    const ai::Variable* var = m.findVariable(c.variable);
                    const ai::VarType type = var != nullptr ? var->value.type : ai::VarType::Float;
                    ImGui::SetNextItemWidth(150.0f);
                    if (ImGui::BeginCombo("##cmp", ai::compareLabel(c.compare))) {
                        for (int i = 0; i < ai::kCompareCount; ++i) {
                            const ai::Compare cmp = static_cast<ai::Compare>(i);
                            if (!compareFits(type, cmp)) continue;
                            if (ImGui::Selectable(ai::compareLabel(cmp), cmp == c.compare)) {
                                c.compare = cmp;
                                fsm_dirty_ = true;
                            }
                        }
                        ImGui::EndCombo();
                    }
                    if (c.compare != ai::Compare::IsTrue && c.compare != ai::Compare::IsFalse) {
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 52.0f);
                        ImGui::InputTextWithHint("##value", "valor o $variable", &c.value);
                        if (ImGui::IsItemDeactivatedAfterEdit()) fsm_dirty_ = true;
                        ImGui::SetItemTooltip("Un valor (5, true, Jugador, 1 2 3) o $otra para comparar con otra variable");
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(24.0f);
                        if (ImGui::BeginCombo("##other", "", ImGuiComboFlags_NoPreview)) {
                            for (const ai::Variable& v : m.variables) {
                                if (v.name == c.variable) continue;
                                if (ImGui::Selectable(("$" + v.name).c_str())) {
                                    c.value = "$" + v.name;
                                    fsm_dirty_ = true;
                                }
                            }
                            ImGui::EndCombo();
                        }
                    }
                    break;
                }
                case ai::ConditionKind::Trigger:
                    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 24.0f);
                    ImGui::InputTextWithHint("##trigger", "nombre (sm:trigger)", &c.variable);
                    if (ImGui::IsItemDeactivatedAfterEdit()) fsm_dirty_ = true;
                    break;
                case ai::ConditionKind::Timer:
                    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 24.0f);
                    ImGui::DragFloat("##seconds", &c.seconds, 0.05f, 0.0f, 3600.0f, "después de %.2f s");
                    if (ImGui::IsItemDeactivatedAfterEdit()) fsm_dirty_ = true;
                    break;
                case ai::ConditionKind::Expression: {
                    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 24.0f);
                    ImGui::InputTextWithHint("##expr", "vida < 30 and not alerta", &c.expression);
                    if (ImGui::IsItemDeactivatedAfterEdit()) fsm_dirty_ = true;
                    ImGui::SetItemTooltip("Expresión con las variables por su nombre: números, textos, + - * / %%,\n"
                                          "== != < <= > >=, and / or / not (o && || !) y paréntesis");
                    const ai::Expression& e = c.compiled.get(c.expression);
                    if (!e.valid()) ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.4f, 1.0f), "No se puede leer: %s", e.error().c_str());
                    break;
                }
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) remove = static_cast<int>(k);
            ImGui::PopID();
        }
        if (remove >= 0) {
            t.conditions.erase(t.conditions.begin() + remove);
            fsm_dirty_ = true;
        }
        if (ImGui::Button("+ Condición")) {
            ai::Condition c;
            if (!m.variables.empty()) {
                c.variable = m.variables.front().name;
                if (!compareFits(m.variables.front().value.type, c.compare)) c.compare = ai::Compare::IsTrue;
            } else {
                c.kind = ai::ConditionKind::Timer;
            }
            t.conditions.push_back(std::move(c));
            fsm_dirty_ = true;
        }
        if (t.conditions.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.35f, 1.0f), "Sin condiciones: se toma enseguida.");
        }
        ImGui::Spacing();
        if (ImGui::Button("Borrar transición")) {
            m.transitions.erase(m.transitions.begin() + index);
            fsm_selected_transition_ = -1;
            fsm_dirty_ = true;
        }
        return;
    }

    // --- Estado seleccionado (o Cualquier estado) ---
    const int selected = fsm_selected_state_;
    if (selected != ai::kAnyState && (selected < 0 || selected >= count)) {
        ImGui::SeparatorText("Máquina de estados");
        ImGui::TextWrapped(
            "La lógica de cada estado va en un script de C++ del objeto: recibe en onMessage \"OnStateEnter\" "
            "{machine, state, from} al entrar, \"OnStateExit\" {machine, state, to} al salir y, si el estado tiene "
            "\"Enviar OnStateUpdate\", \"OnStateUpdate\" {machine, state, dt, time} cada frame.");
        ImGui::Spacing();
        ImGui::TextWrapped("Desde el script: entity.getStateMachine() y sm.get / sm.set (las variables), "
                           "sm.go(\"Estado\"), sm.trigger(\"nombre\"). Los sensores (distancia, vista...) los calcula "
                           "el script y los guarda con sm.set.");
        ImGui::Spacing();
        ImGui::TextWrapped("Selecciona un estado para verlo, o una flecha para editar sus condiciones.");
        return;
    }
    if (selected == ai::kAnyState) {
        ImGui::SeparatorText("Cualquier estado");
        ImGui::TextWrapped("Sus transiciones salen de cualquier estado (morir, huir...). Los sensores que usan sus "
                           "condiciones los calcula un script de C++ del objeto y los guarda con sm.set.");
        if (ai::hasLuaCode(m.any_code)) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.35f, 1.0f), "Tiene código Lua de antes: ya no se ejecuta.");
            ImGui::SameLine();
            if (ImGui::SmallButton("Borrar el código Lua##any")) {
                m.any_code.clear();
                fsm_dirty_ = true;
            }
            ImGui::InputTextMultiline("##any_lua", &m.any_code, ImVec2(-1.0f, 160.0f), ImGuiInputTextFlags_ReadOnly);
        }
        return;
    } else {
        ai::State& st = m.states[selected];
        ImGui::SeparatorText("Estado");
        std::string name = st.name;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
        if (ImGui::InputText("##name", &name, ImGuiInputTextFlags_EnterReturnsTrue) ||
            (ImGui::IsItemDeactivatedAfterEdit() && name != st.name)) {
            if (!name.empty() && m.findState(name) < 0) {
                st.name = name;
                fsm_dirty_ = true;
            }
        }
        ImGui::SameLine();
        float color[3] = {st.color.x, st.color.y, st.color.z};
        if (ImGui::ColorEdit3("##color", color, ImGuiColorEditFlags_NoInputs)) st.color = core::Vec3{color[0], color[1], color[2]};
        if (ImGui::IsItemDeactivatedAfterEdit()) fsm_dirty_ = true;
        ImGui::SameLine();
        const bool is_entry = m.entry_state == selected;
        ImGui::BeginDisabled(is_entry);
        if (ImGui::SmallButton(is_entry ? "Entrada" : "Hacer entrada")) {
            m.entry_state = selected;
            fsm_dirty_ = true;
        }
        ImGui::EndDisabled();
        if (running && sm->runtime.state != selected) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Ir aquí")) {
                if (ai::StateMachine* c = live.tryGet<ai::StateMachine>()) c->runtime.requested = selected;
            }
        }
        // Transiciones que salen de aqui (clic: editarlas).
        int outgoing = 0;
        for (std::size_t i = 0; i < m.transitions.size(); ++i) {
            const ai::Transition& t = m.transitions[i];
            if (t.from != selected || t.to < 0 || t.to >= count) continue;
            if (outgoing++ == 0) ImGui::TextDisabled("Sale hacia:");
            ImGui::SameLine();
            ImGui::PushID(static_cast<int>(i) + 3000);
            if (ImGui::SmallButton(m.states[t.to].name.c_str())) {
                fsm_selected_transition_ = static_cast<int>(i);
                fsm_selected_state_ = kNoNode;
            }
            ImGui::SetItemTooltip("%s", transitionText(m, t).c_str());
            ImGui::PopID();
        }
        ImGui::Spacing();
        if (ImGui::Checkbox("Enviar OnStateUpdate", &st.send_update)) fsm_dirty_ = true;
        ImGui::SetItemTooltip("Cada frame en este estado, \"OnStateUpdate\" {machine, state, dt, time} a los scripts de C++ del objeto");
        ImGui::Spacing();
        ImGui::TextWrapped("La lógica de \"%s\" va en un script de C++ del objeto (onMessage):", st.name.c_str());
        ImGui::TextDisabled("%s", kCppExample);
        if (ai::hasLuaCode(st.code) || !st.script.empty()) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.35f, 1.0f), "Este estado tiene %s de antes: ya no se ejecuta.",
                               st.script.empty() ? "código Lua" : st.script.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Quitarlo")) {
                st.code.clear();
                st.script.clear();
                fsm_dirty_ = true;
            }
            if (ai::hasLuaCode(st.code)) {
                ImGui::InputTextMultiline("##state_lua", &st.code, ImVec2(-1.0f, 160.0f), ImGuiInputTextFlags_ReadOnly);
            }
        }
    }
}

// --- Inspector del componente ---------------------------------------------------------

void EditorApp::drawStateMachineInspector(ecs::Entity entity) {
    ai::StateMachine* c = entity.tryGet<ai::StateMachine>();
    if (c == nullptr) return;
    if (c->machine.valid()) {
        if (ImGui::Button("Abrir en la ventana Máquina de estados", ImVec2(-1.0f, 0.0f))) openStateMachineEditor(c->machine.uuid);
    } else {
        const std::filesystem::path folder = current_folder_.empty() ? project_.assetsFolder() / "IA" : current_folder_;
        const float half = (ImGui::GetContentRegionAvail().x - 4.0f) * 0.5f;
        Uuid created{};
        if (ImGui::Button("Crear máquina vacía", ImVec2(half, 0.0f))) created = createStateMachineAsset(folder, false);
        ImGui::SameLine();
        if (ImGui::Button("Crear enemigo de ejemplo", ImVec2(half, 0.0f))) created = createStateMachineAsset(folder, true);
        ImGui::SetItemTooltip("Patrullar (Waypoints o NavMesh), Perseguir, Atacar, Huir y Volver.\n"
                              "Necesita un NavAgent y un objeto \"Jugador\".");
        if (created.valid()) {
            if (ai::StateMachine* again = entity.tryGet<ai::StateMachine>()) {
                again->machine = assets::AssetRef{created, assets::AssetType::StateMachine};
                commit();
            }
            return;
        }
    }
    const ai::StateMachineAsset* m = stateMachineAsset(c->machine.uuid);
    if (m == nullptr) return;
    const bool running = playing() && c->runtime.started;
    const int count = static_cast<int>(m->states.size());

    if (running) {
        const int s = c->runtime.state;
        ImGui::TextColored(ImVec4(0.45f, 0.75f, 1.0f, 1.0f), "Estado: %s  (%.1f s)", s >= 0 && s < count ? m->states[s].name.c_str() : "-",
                           static_cast<double>(c->runtime.state_time));
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::BeginCombo("##goto", "Ir al estado...")) {
            for (int i = 0; i < count; ++i) {
                if (ImGui::Selectable(m->states[i].name.c_str(), i == s)) c->runtime.requested = i;
            }
            ImGui::EndCombo();
        }
    }
    if (!m->variables.empty()) {
        ImGui::SeparatorText(running ? "Variables (en vivo)" : "Variables de este objeto");
    }
    for (const ai::Variable& v : m->variables) {
        ImGui::PushID(v.name.c_str());
        if (running) {
            ImGui::TextUnformatted(v.name.c_str());
            ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.4f);
            ImGui::SetNextItemWidth(-1.0f);
            if (ai::Value* value = c->runtime.find(v.name)) valueEditor("live", *value, &world_);
            ImGui::PopID();
            continue;
        }
        // Valor propio de este objeto (casilla) o el de la maquina.
        auto it = std::find_if(c->variables.begin(), c->variables.end(),
                               [&](const ai::VariableOverride& o) { return o.name == v.name; });
        bool own = it != c->variables.end();
        if (ImGui::Checkbox("##own", &own)) {
            if (own) {
                c->variables.push_back(ai::VariableOverride{v.name, static_cast<int>(v.value.type), v.value.text()});
            } else {
                c->variables.erase(it);
            }
            commit();
            ImGui::PopID();
            continue;
        }
        ImGui::SetItemTooltip("Usar un valor propio en este objeto (si no, el de la máquina)");
        ImGui::SameLine();
        if (own) ImGui::TextUnformatted(v.name.c_str());
        else ImGui::TextDisabled("%s", v.name.c_str());
        ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.45f);
        ImGui::SetNextItemWidth(-1.0f);
        ai::Value value = own ? ai::Value::parse(v.value.type, it->value) : v.value;
        ImGui::BeginDisabled(!own);
        const EditResult r = valueEditor("value", value, &world_);
        ImGui::EndDisabled();
        if (own && r.changed) {
            it->value = value.text();
            it->type = static_cast<int>(v.value.type);
        }
        if (own && r.finished) commit();
        ImGui::PopID();
    }
    // Valores de variables que la maquina ya no tiene.
    std::size_t stale = 0;
    for (const ai::VariableOverride& o : c->variables) stale += m->findVariable(o.name) == nullptr ? 1 : 0;
    if (stale > 0 && !running) {
        char label[96];
        std::snprintf(label, sizeof(label), "Quitar %zu valor(es) de variables que ya no existen", stale);
        if (ImGui::SmallButton(label)) {
            std::erase_if(c->variables, [&](const ai::VariableOverride& o) { return m->findVariable(o.name) == nullptr; });
            commit();
        }
    }
}

// --- MCP --------------------------------------------------------------------------------

std::string EditorApp::stateMachineMcpTool(const std::string& name, const std::string& args_json, std::string& error) {
    const json args = json::parse(args_json, nullptr, false);
    if (!args.is_object()) {
        error = "argumentos invalidos";
        return {};
    }
    if (!has_project_) {
        error = "no hay proyecto abierto";
        return {};
    }
    // Una maquina por UUID, ruta dentro de Assets, nombre o nombre de archivo.
    const auto findMachine = [&](const std::string& ref) -> std::optional<assets::AssetInfo> {
        if (const Uuid uuid = Uuid::parse(ref); uuid.valid()) {
            if (auto info = database_->find(uuid); info && info->type == assets::AssetType::StateMachine) return info;
        }
        std::string rel = ref;
        if (rel.rfind("Assets/", 0) == 0) rel = rel.substr(7);
        for (const assets::AssetInfo& info : database_->all()) {
            if (info.type != assets::AssetType::StateMachine) continue;
            if (assetRelative(info.path) == rel || info.name == ref || dialogs::utf8(info.path.filename()) == ref ||
                dialogs::utf8(info.path.stem()) == ref) {
                return info;
            }
        }
        return std::nullopt;
    };
    const auto machineArg = [&]() -> std::optional<assets::AssetInfo> {
        const std::string ref = args.value("machine", std::string{});
        auto info = findMachine(ref);
        if (!info) error = "no existe la maquina de estados \"" + ref + "\"";
        return info;
    };
    const auto entityArg = [&]() -> ecs::Entity {
        const ecs::Entity e = world_.find(Uuid::parse(args.value("entity", std::string{})));
        if (!e.valid()) error = "no existe la entidad";
        return e;
    };
    const auto problemsJson = [](const ai::StateMachineAsset& m) {
        json out = json::array();
        for (const std::string& p : ai::validateStateMachine(m)) out.push_back(p);
        return out;
    };
    // Tras escribir un .crfsm: base de datos, ventana abierta y Play.
    const auto afterWrite = [&](const ai::StateMachineAsset& m, const std::filesystem::path& path) {
        refreshDatabase();
        fsm_cache_.erase(m.uuid);
        if (fsm_uuid_ == m.uuid) {
            const bool shown = show_state_machine_;
            fsm_dirty_ = false;
            loadStateMachineEditor(m.uuid);  // sin cambiar de pestana
            show_state_machine_ = shown;
            fsm_focus_ = false;
        }
        scripts_.clearErrors();
        scripts_.reloadFile(assetRelative(path));
    };

    if (name == "create_state_machine") {
        std::string file_name = safeFileName(args.value("name", std::string("Maquina de estados")));
        std::string folder_rel = args.value("folder", std::string("IA"));
        if (folder_rel.rfind("Assets/", 0) == 0) folder_rel = folder_rel.substr(7);
        if (folder_rel.find("..") != std::string::npos) {
            error = "carpeta invalida";
            return {};
        }
        const std::filesystem::path folder = project_.assetsFolder() / dialogs::fromUtf8(folder_rel);
        std::error_code ec;
        std::filesystem::create_directories(folder, ec);
        const std::filesystem::path path = folder / dialogs::fromUtf8(file_name + ai::kStateMachineExtension);
        ai::StateMachineAsset m;
        if (args.value("example", false)) {
            m = ai::exampleEnemyStateMachine();
        } else if (args.contains("machine") && args["machine"].is_object()) {
            std::string parse_error;
            if (!ai::stateMachineFromJson(args["machine"].dump(), m, &parse_error)) {
                error = parse_error.empty() ? "maquina invalida" : parse_error;
                return {};
            }
        } else {
            ai::State first;
            first.name = "Inicio";
            m.states.push_back(std::move(first));
        }
        // Si ya existe, se reemplaza conservando su UUID (las escenas la siguen usando).
        ai::StateMachineAsset old;
        m.uuid = std::filesystem::exists(path) && ai::loadStateMachine(path, old) && old.uuid.valid() ? old.uuid : Uuid::generate();
        std::string write_error;
        if (!ai::saveStateMachine(m, path, &write_error)) {
            error = write_error;
            return {};
        }
        afterWrite(m, path);
        json out{{"path", assetRelative(path)}, {"uuid", m.uuid.toString()}, {"states", m.states.size()},
                 {"transitions", m.transitions.size()}, {"problems", problemsJson(m)}};
        if (args.contains("attach_to")) {
            ecs::Entity e = world_.find(Uuid::parse(args.value("attach_to", std::string{})));
            if (!e.valid()) {
                error = "no existe la entidad de attach_to";
                return {};
            }
            ai::StateMachine& sm = e.has<ai::StateMachine>() ? e.get<ai::StateMachine>() : e.add<ai::StateMachine>();
            sm.machine = assets::AssetRef{m.uuid, assets::AssetType::StateMachine};
            commit();
            out["attached_to"] = e.name();
        }
        if (args.value("open", false)) openStateMachineEditor(m.uuid);
        return out.dump();
    }
    if (name == "get_state_machine") {
        const auto info = machineArg();
        if (!info) return {};
        if (fsm_uuid_ == info->uuid && fsm_dirty_) saveStateMachineEditor();
        ai::StateMachineAsset m;
        std::string load_error;
        if (!ai::loadStateMachine(info->path, m, &load_error)) {
            error = load_error;
            return {};
        }
        json out{{"path", assetRelative(info->path)}, {"machine", json::parse(ai::stateMachineToJson(m), nullptr, false)},
                 {"problems", problemsJson(m)}};
        return out.dump();
    }
    if (name == "update_state_machine") {
        const auto info = machineArg();
        if (!info) return {};
        if (fsm_uuid_ == info->uuid && fsm_dirty_) saveStateMachineEditor();
        ai::StateMachineAsset m;
        std::string load_error;
        if (!ai::loadStateMachine(info->path, m, &load_error)) {
            error = load_error;
            return {};
        }
        const Uuid uuid = m.uuid.valid() ? m.uuid : info->uuid;
        // Reemplazo completo...
        if (args.contains("data") && args["data"].is_object()) {
            ai::StateMachineAsset fresh;
            std::string parse_error;
            if (!ai::stateMachineFromJson(args["data"].dump(), fresh, &parse_error)) {
                error = parse_error.empty() ? "maquina invalida" : parse_error;
                return {};
            }
            m = std::move(fresh);
        }
        m.uuid = uuid;
        // ...y/o que estados mandan OnStateUpdate: {"Patrullar": true}.
        if (args.contains("send_update") && args["send_update"].is_object()) {
            for (const auto& [state, on] : args["send_update"].items()) {
                const int index = m.findState(state);
                if (index < 0) {
                    error = "no hay un estado \"" + state + "\"";
                    return {};
                }
                m.states[index].send_update = on.is_boolean() ? on.get<bool>() : true;
            }
        }
        if (args.contains("entry") && args["entry"].is_string()) {
            const int index = m.findState(args["entry"].get<std::string>());
            if (index >= 0) m.entry_state = index;
        }
        std::string write_error;
        if (!ai::saveStateMachine(m, info->path, &write_error)) {
            error = write_error;
            return {};
        }
        afterWrite(m, info->path);
        return json{{"path", assetRelative(info->path)}, {"states", m.states.size()},
                    {"transitions", m.transitions.size()}, {"problems", problemsJson(m)}}.dump();
    }
    if (name == "assign_state_machine") {
        ecs::Entity e = entityArg();
        if (!e.valid()) return {};
        const auto info = machineArg();
        if (!info) return {};
        ai::StateMachine& sm = e.has<ai::StateMachine>() ? e.get<ai::StateMachine>() : e.add<ai::StateMachine>();
        sm.machine = assets::AssetRef{info->uuid, assets::AssetType::StateMachine};
        if (args.contains("start_active")) sm.start_active = args.value("start_active", true);
        if (args.contains("debug")) sm.debug = args.value("debug", false);
        if (args.contains("variables") && args["variables"].is_object()) {
            const ai::StateMachineAsset* m = stateMachineAsset(info->uuid);
            for (const auto& [var, value] : args["variables"].items()) {
                const ai::Variable* v = m != nullptr ? m->findVariable(var) : nullptr;
                const int type = v != nullptr ? static_cast<int>(v->value.type) : static_cast<int>(ai::VarType::String);
                auto it = std::find_if(sm.variables.begin(), sm.variables.end(),
                                       [&](const ai::VariableOverride& o) { return o.name == var; });
                if (it == sm.variables.end()) {
                    sm.variables.push_back(ai::VariableOverride{var, type, jsonText(value)});
                } else {
                    it->type = type;
                    it->value = jsonText(value);
                }
            }
        }
        commit();
        return json{{"entity", e.name()}, {"machine", assetRelative(info->path)}, {"variables", sm.variables.size()}}.dump();
    }
    if (name == "state_machine_debug") {
        ecs::Entity e = entityArg();
        if (!e.valid()) return {};
        ai::StateMachine* sm = e.tryGet<ai::StateMachine>();
        if (sm == nullptr) {
            error = "la entidad no tiene el componente StateMachine";
            return {};
        }
        const ai::StateMachineAsset* m = stateMachineAsset(sm->machine.uuid);
        if (m == nullptr) {
            error = "la entidad no tiene una maquina de estados valida";
            return {};
        }
        ai::Runtime& rt = sm->runtime;
        const int count = static_cast<int>(m->states.size());
        // Ordenes (en Play): ir a un estado, disparar triggers, cambiar variables.
        if (args.contains("go") && args["go"].is_string()) {
            const int index = m->findState(args["go"].get<std::string>());
            if (index < 0) {
                error = "no hay un estado \"" + args["go"].get<std::string>() + "\"";
                return {};
            }
            rt.requested = index;
        }
        if (args.contains("trigger") && args["trigger"].is_string()) rt.triggers.push_back(args["trigger"].get<std::string>());
        if (args.contains("set") && args["set"].is_object()) {
            for (const auto& [var, value] : args["set"].items()) {
                if (ai::Value* v = rt.find(var)) {
                    *v = ai::Value::parse(v->type, jsonText(value));
                    if (v->type == ai::VarType::Entity) v->entity = resolveEntity(world_, v->s);
                }
            }
        }
        const auto stateName = [&](int i) -> json { return i >= 0 && i < count ? json(m->states[i].name) : json(nullptr); };
        json vars = json::object();
        for (const ai::Variable& v : rt.vars) {
            if (v.value.type == ai::VarType::Entity) {
                vars[v.name] = v.value.entity == ai::kNoEntity ? json(nullptr) : json(v.value.s);
            } else if (v.value.type == ai::VarType::Bool) {
                vars[v.name] = v.value.b;
            } else if (v.value.type == ai::VarType::Int || v.value.type == ai::VarType::Float) {
                vars[v.name] = v.value.n;
            } else if (v.value.type == ai::VarType::Vector) {
                vars[v.name] = json::array({v.value.v.x, v.value.v.y, v.value.v.z});
            } else {
                vars[v.name] = v.value.s;
            }
        }
        json out{{"entity", e.name()},
                 {"playing", playing()},
                 {"started", rt.started},
                 {"running", rt.running},
                 {"state", stateName(rt.state)},
                 {"previous", stateName(rt.previous)},
                 {"state_time", rt.state_time},
                 {"time", rt.time},
                 {"changes", rt.changes},
                 {"last_transition", rt.last_transition >= 0 ? json(rt.last_transition + 1) : json(rt.last_transition == -2 ? "codigo" : "entrada")},
                 {"variables", vars},
                 {"history", rt.history}};
        if (!playing()) out["note"] = "fuera de Play la maquina no corre: entra en Play para ver el estado activo";
        return out.dump();
    }
    error = "herramienta desconocida: " + name;
    return {};
}

}  // namespace cramion::editor
