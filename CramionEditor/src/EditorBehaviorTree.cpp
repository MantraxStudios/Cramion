// Editor de Behavior Trees (.crbt), como el de Unreal: el arbol de arriba a
// abajo (los hijos de izquierda a derecha por prioridad), la pizarra a la
// izquierda y los detalles del nodo (parametros, decoradores y servicios) a
// la derecha. En Play, el objeto seleccionado que usa el arbol se ve en vivo:
// la rama activa brilla, cada nodo enseña su ultimo resultado (verde bien,
// rojo mal) y la pizarra se puede cambiar para probar.

#include "EditorApp.h"

#include "Dialogs.h"
#include "NodeGraph.h"

#include <CramionCore/ai/BehaviorTree.h>

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cstring>
#include <iostream>

namespace cramion::editor {

struct BtEditorState {
    bool show = false;
    bool focus = false;
    Uuid uuid{};
    std::filesystem::path path;
    ai::BehaviorTreeAsset tree;
    bool dirty = false;
    nodegraph::Canvas canvas;
    std::string search;
    core::Vec2 create_at{};
    std::optional<nodegraph::PinRef> create_from;
};

namespace {

ImU32 nodeHeader(ai::BtNodeKind kind) {
    if (kind == ai::BtNodeKind::Root) return IM_COL32(70, 70, 75, 255);
    if (ai::btIsComposite(kind)) return IM_COL32(60, 80, 120, 255);
    return IM_COL32(110, 60, 130, 255);
}

std::string paramsSummary(const ai::BtParams& p, const std::vector<ai::BtParamInfo>& infos) {
    std::string out;
    for (const ai::BtParamInfo& info : infos) {
        std::string value;
        switch (info.field) {
            case ai::BtField::Key: value = p.key; break;
            case ai::BtField::Key2: value = p.key2; break;
            case ai::BtField::Key3: value = p.key3; break;
            case ai::BtField::Text: value = p.text; break;
            case ai::BtField::Value: value = p.value; break;
            case ai::BtField::Number: value = std::to_string(p.number).substr(0, 5); break;
            case ai::BtField::Number2: value = std::to_string(p.number2).substr(0, 5); break;
            case ai::BtField::Number3: value = std::to_string(p.number3).substr(0, 5); break;
            case ai::BtField::Option: {
                std::string options = info.options != nullptr ? info.options : "";
                int i = 0;
                std::size_t start = 0;
                while (i < p.option && start != std::string::npos) {
                    start = options.find('|', start);
                    if (start != std::string::npos) ++start;
                    ++i;
                }
                if (start != std::string::npos) value = options.substr(start, options.find('|', start) - start);
                break;
            }
            case ai::BtField::Flag: value = p.flag ? "si" : "no"; break;
        }
        if (value.empty()) continue;
        if (!out.empty()) out += "\n";
        out += std::string(info.label) + ": " + value;
        if (out.size() > 120) break;
    }
    return out;
}

// Un parametro generico. true si cambio.
bool paramEditor(ai::BtParams& p, const ai::BtParamInfo& info, const ai::BehaviorTreeAsset& tree) {
    bool changed = false;
    ImGui::PushID(info.json);
    switch (info.field) {
        case ai::BtField::Key:
        case ai::BtField::Key2:
        case ai::BtField::Key3: {
            std::string* key = ai::btTextField(p, info.field);
            if (ImGui::BeginCombo(info.label, key->empty() ? "(ninguna)" : key->c_str())) {
                if (ImGui::Selectable("(ninguna)", key->empty())) {
                    key->clear();
                    changed = true;
                }
                for (const ai::Variable& v : tree.blackboard) {
                    const int bit = 1 << static_cast<int>(v.value.type);
                    if (info.key_types != -1 && (info.key_types & bit) == 0) continue;
                    if (ImGui::Selectable(v.name.c_str(), *key == v.name)) {
                        *key = v.name;
                        changed = true;
                    }
                }
                ImGui::EndCombo();
            }
            break;
        }
        case ai::BtField::Text:
        case ai::BtField::Value: changed |= ImGui::InputText(info.label, ai::btTextField(p, info.field)); break;
        case ai::BtField::Number:
        case ai::BtField::Number2:
        case ai::BtField::Number3: changed |= ImGui::DragFloat(info.label, ai::btNumberField(p, info.field), 0.05f); break;
        case ai::BtField::Option: {
            std::vector<std::string> names;
            std::string options = info.options != nullptr ? info.options : "";
            std::size_t start = 0;
            while (start <= options.size()) {
                const std::size_t end = options.find('|', start);
                names.push_back(options.substr(start, end == std::string::npos ? std::string::npos : end - start));
                if (end == std::string::npos) break;
                start = end + 1;
            }
            const char* current = p.option >= 0 && p.option < static_cast<int>(names.size()) ? names[static_cast<std::size_t>(p.option)].c_str() : "?";
            if (ImGui::BeginCombo(info.label, current)) {
                for (std::size_t i = 0; i < names.size(); ++i) {
                    if (ImGui::Selectable(names[i].c_str(), p.option == static_cast<int>(i))) {
                        p.option = static_cast<int>(i);
                        changed = true;
                    }
                }
                ImGui::EndCombo();
            }
            break;
        }
        case ai::BtField::Flag: changed |= ImGui::Checkbox(info.label, &p.flag); break;
    }
    if (info.tooltip != nullptr && info.tooltip[0] != 0) ImGui::SetItemTooltip("%s", info.tooltip);
    ImGui::PopID();
    return changed;
}

bool valueEditor(ai::Value& v) {
    bool changed = false;
    switch (v.type) {
        case ai::VarType::Bool: changed = ImGui::Checkbox("##v", &v.b); break;
        case ai::VarType::Int: {
            int i = static_cast<int>(v.n);
            if (ImGui::DragInt("##v", &i)) {
                v.n = i;
                changed = true;
            }
            break;
        }
        case ai::VarType::Float: {
            float f = static_cast<float>(v.n);
            if (ImGui::DragFloat("##v", &f, 0.05f)) {
                v.n = f;
                changed = true;
            }
            break;
        }
        case ai::VarType::Vector: changed = ImGui::DragFloat3("##v", &v.v.x, 0.05f); break;
        default: changed = ImGui::InputText("##v", &v.s); break;
    }
    return changed;
}

}  // namespace

BtEditorState& EditorApp::btEditor() {
    if (!bt_editor_) bt_editor_ = std::make_shared<BtEditorState>();
    return *bt_editor_;
}

Uuid EditorApp::createBehaviorTreeAsset(const std::filesystem::path& folder, bool example) {
    ai::BehaviorTreeAsset tree = example ? ai::exampleGuardBehaviorTree() : ai::BehaviorTreeAsset{};
    tree.uuid = Uuid::generate();
    tree.ensureRoot();
    if (!example) {
        const int selector = tree.addNode(ai::BtNodeKind::Selector, 0, core::Vec2{0.0f, 140.0f});
        tree.addNode(ai::BtNodeKind::Wait, selector, core::Vec2{0.0f, 280.0f});
        tree.autoLayout();
    }
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    std::filesystem::path path = folder / dialogs::fromUtf8(std::string(example ? "Guardia" : "Arbol") + ai::kBehaviorTreeExtension);
    for (int i = 2; std::filesystem::exists(path); ++i) {
        path = folder / dialogs::fromUtf8(std::string(example ? "Guardia " : "Arbol ") + std::to_string(i) + ai::kBehaviorTreeExtension);
    }
    std::string error;
    if (!ai::saveBehaviorTree(tree, path, &error)) {
        std::cerr << "[Editor] No se pudo crear el Behavior Tree: " << error << "\n";
        return {};
    }
    refreshDatabase();
    openBehaviorTreeEditor(tree.uuid);
    return tree.uuid;
}

void EditorApp::openBehaviorTreeEditor(const Uuid& uuid) {
    BtEditorState& st = btEditor();
    if (st.dirty) saveBehaviorTreeEditor();
    const auto info = database_ ? database_->find(uuid) : std::nullopt;
    if (!info) return;
    ai::BehaviorTreeAsset tree;
    std::string error;
    if (!ai::loadBehaviorTree(info->path, tree, &error)) {
        std::cerr << "[Editor] No se pudo abrir el Behavior Tree: " << error << "\n";
        return;
    }
    tree.ensureRoot();
    if (!tree.uuid.valid()) tree.uuid = uuid;
    st.tree = std::move(tree);
    st.uuid = uuid;
    st.path = info->path;
    st.dirty = false;
    st.show = true;
    st.focus = true;
    st.canvas.vertical = true;
    st.canvas.selection.clear();
    std::vector<nodegraph::NodeView> views;
    for (std::size_t i = 0; i < st.tree.nodes.size(); ++i) {
        nodegraph::NodeView v;
        v.id = static_cast<int>(i) + 1;
        v.position = &st.tree.nodes[i].position;
        views.push_back(v);
    }
    st.canvas.frame(views);
}

void EditorApp::saveBehaviorTreeEditor() {
    BtEditorState& st = btEditor();
    if (st.path.empty()) return;
    std::string error;
    if (!ai::saveBehaviorTree(st.tree, st.path, &error)) {
        std::cerr << "[Editor] No se pudo guardar el Behavior Tree: " << error << "\n";
        return;
    }
    st.dirty = false;
    scripts_.reloadFile(assetRelative(st.path));
}

EditorApp::GraphDoc EditorApp::graphDocBehaviorTree() {
    BtEditorState& st = btEditor();
    GraphDoc d;
    d.show = &st.show;
    d.focus = &st.focus;
    d.dirty = st.dirty;
    d.path = st.path;
    return d;
}

void EditorApp::drawBehaviorTreeEditor() {
    BtEditorState& st = btEditor();
    if (!st.show) return;
    const std::string title = "Behavior Tree: " + dialogs::utf8(st.path.stem()) + (st.dirty ? " *" : "") + "###behavior_tree";
    if (!beginGraphWorkspace(GraphKind::BehaviorTree, title.c_str())) return;  // en su pestana de arriba
    if (!st.show && st.dirty) saveBehaviorTreeEditor();
    ai::BehaviorTreeAsset& t = st.tree;
    t.ensureRoot();
    bool changed = false;

    // Objeto en vivo (Play): el seleccionado o el primero que usa el arbol.
    ecs::Entity live;
    if (playing()) {
        const auto uses = [&](const ecs::Entity& e) {
            const ai::BehaviorTree* c = e.valid() ? e.tryGet<ai::BehaviorTree>() : nullptr;
            return c != nullptr && c->tree.uuid == st.uuid;
        };
        if (const ecs::Entity active = world_.find(active_); uses(active)) live = active;
        if (!live.valid()) {
            for (const entt::entity h : world_.registry().view<ai::BehaviorTree>()) {
                if (uses(world_.wrap(h))) {
                    live = world_.wrap(h);
                    break;
                }
            }
        }
    }
    ai::BehaviorTree* live_bt = live.valid() ? live.tryGet<ai::BehaviorTree>() : nullptr;
    const bool running = live_bt != nullptr && live_bt->runtime.started;

    // --- Barra ---
    if (ImGui::Button("Guardar")) saveBehaviorTreeEditor();
    ImGui::SameLine();
    if (ImGui::Button("Asignar a la selección")) {
        int assigned = 0;
        for (ecs::Entity e : topLevelSelection()) {
            ai::BehaviorTree& c = e.has<ai::BehaviorTree>() ? e.get<ai::BehaviorTree>() : e.add<ai::BehaviorTree>();
            c.tree = assets::AssetRef{st.uuid, assets::AssetType::BehaviorTree};
            ++assigned;
        }
        if (assigned > 0) commit();
    }
    ImGui::SameLine();
    if (ImGui::Button("Ordenar")) {
        t.autoLayout();
        changed = true;
    }
    ImGui::SetItemTooltip("Coloca el árbol de arriba a abajo");
    ImGui::SameLine();
    if (running) {
        ImGui::TextColored(ImVec4(0.45f, 0.75f, 1.0f, 1.0f), "En vivo: %s · %llu ciclos%s", live.name().c_str(),
                           static_cast<unsigned long long>(live_bt->runtime.cycles), live_bt->runtime.running ? "" : " [parado]");
    } else {
        ImGui::TextDisabled("Clic derecho: crear · arrastrar desde abajo de un nodo: hijo · la izquierda manda");
    }
    const std::vector<std::string> problems = ai::validateBehaviorTree(t);
    if (!problems.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.35f, 1.0f), "%zu aviso(s)", problems.size());
        if (ImGui::IsItemHovered()) {
            std::string all;
            for (const std::string& p : problems) all += p + "\n";
            ImGui::SetTooltip("%s", all.c_str());
        }
    }

    // --- Izquierda: pizarra ---
    ImGui::BeginChild("bt_left", ImVec2(260.0f, 0.0f), ImGuiChildFlags_ResizeX | ImGuiChildFlags_Borders);
    ImGui::SeparatorText("Pizarra (Blackboard)");
    int remove = -1;
    for (std::size_t i = 0; i < t.blackboard.size(); ++i) {
        ai::Variable& v = t.blackboard[i];
        ImGui::PushID(static_cast<int>(i));
        std::string name = v.name;
        ImGui::SetNextItemWidth(100.0f);
        if (ImGui::InputText("##n", &name, ImGuiInputTextFlags_EnterReturnsTrue) && !name.empty() && t.findKey(name) == nullptr) {
            t.renameKey(v.name, name);
            changed = true;
        }
        ImGui::SetItemTooltip("%s", ai::varTypeLabel(v.value.type));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 24.0f);
        if (running) {
            if (ai::Value* value = live_bt->runtime.find(v.name)) valueEditor(*value);
        } else {
            changed |= valueEditor(v.value);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) remove = static_cast<int>(i);
        ImGui::PopID();
    }
    if (remove >= 0) {
        t.blackboard.erase(t.blackboard.begin() + remove);
        changed = true;
    }
    if (ImGui::Button("+ Clave")) ImGui::OpenPopup("bt_add_key");
    if (ImGui::BeginPopup("bt_add_key")) {
        for (int k = 0; k < ai::kVarTypeCount; ++k) {
            const ai::VarType type = static_cast<ai::VarType>(k);
            if (ImGui::MenuItem(ai::varTypeLabel(type))) {
                std::string name = "clave";
                for (int i = 2; t.findKey(name) != nullptr; ++i) name = "clave" + std::to_string(i);
                ai::Variable v;
                v.name = name;
                v.value.type = type;
                t.blackboard.push_back(std::move(v));
                changed = true;
            }
        }
        ImGui::EndPopup();
    }
    if (running) {
        ImGui::SeparatorText("Historial");
        for (auto it = live_bt->runtime.history.rbegin(); it != live_bt->runtime.history.rend(); ++it) ImGui::TextDisabled("%s", it->c_str());
    }
    ImGui::EndChild();
    ImGui::SameLine();

    // --- Centro: arbol ---
    std::vector<nodegraph::NodeView> views;
    std::vector<nodegraph::LinkView> links;
    const float now = running ? live_bt->runtime.time : 0.0f;
    for (std::size_t i = 0; i < t.nodes.size(); ++i) {
        ai::BtNode& n = t.nodes[i];
        nodegraph::NodeView v;
        v.id = static_cast<int>(i) + 1;
        v.position = &n.position;
        v.width = 200.0f;
        v.title = n.title();
        v.header = nodeHeader(n.kind);
        v.tooltip = ai::btNodeTooltip(n.kind);
        if (n.kind != ai::BtNodeKind::Root) v.inputs.push_back(nodegraph::PinView{"", IM_COL32(220, 220, 220, 255), false, true});
        if (n.kind == ai::BtNodeKind::Root || ai::btIsComposite(n.kind)) {
            v.outputs.push_back(nodegraph::PinView{"", IM_COL32(220, 220, 220, 255), false, !n.children.empty()});
        }
        std::string body;
        for (const ai::BtDecorator& d : n.decorators) {
            body += std::string("◆ ") + ai::btDecoratorLabel(d.kind);
            if (d.abort != ai::BtAbort::None) body += std::string(" (") + ai::btAbortLabel(d.abort) + ")";
            body += "\n";
        }
        for (const ai::BtService& s : n.services) body += std::string("⟳ ") + ai::btServiceLabel(s.kind) + "\n";
        const std::string params = paramsSummary(n.params, ai::btParamsOf(n.kind));
        if (!params.empty()) body += params;
        if (!body.empty() && body.back() == '\n') body.pop_back();
        v.body = body;
        // Lineas del cuerpo: la altura extra.
        const int lines = body.empty() ? 0 : static_cast<int>(std::count(body.begin(), body.end(), '\n')) + 1;
        v.extra_height = std::max(0, lines - 1) * 16.0f;
        if (running && i < live_bt->runtime.nodes.size()) {
            const ai::BtNodeState& ns = live_bt->runtime.nodes[i];
            v.active = ns.active;
            const float fade = std::clamp(1.0f - (now - ns.finish_time) / 1.5f, 0.0f, 1.0f);
            if (ns.status == ai::BtStatus::Success && fade > 0.0f) v.status_color = IM_COL32(80, 210, 110, static_cast<int>(255 * fade));
            if (ns.status == ai::BtStatus::Failure && fade > 0.0f) v.status_color = IM_COL32(230, 80, 70, static_cast<int>(255 * fade));
            if (ns.status == ai::BtStatus::Aborted && fade > 0.0f) v.status_color = IM_COL32(240, 180, 60, static_cast<int>(255 * fade));
            if (ns.active) v.status_color = IM_COL32(90, 170, 255, 255);
        }
        views.push_back(std::move(v));
        for (const int child : n.children) {
            nodegraph::LinkView l;
            l.from_node = static_cast<int>(i) + 1;
            l.from_pin = 0;
            l.to_node = child + 1;
            l.to_pin = 0;
            l.color = IM_COL32(200, 200, 205, 220);
            if (running && child >= 0 && static_cast<std::size_t>(child) < live_bt->runtime.nodes.size() &&
                live_bt->runtime.nodes[static_cast<std::size_t>(child)].active) {
                l.flow = 1.0f;
                l.color = IM_COL32(90, 170, 255, 255);
            }
            links.push_back(l);
        }
    }
    const float right_width = 360.0f;
    const ImVec2 graph_size(std::max(ImGui::GetContentRegionAvail().x - right_width - 8.0f, 200.0f), 0.0f);
    const nodegraph::Events ev = st.canvas.draw("bt_canvas", views, links, graph_size);
    if (ev.link) {
        if (t.attach(ev.link->to_node - 1, ev.link->from_node - 1)) {
            t.sortChildrenByPosition();
            changed = true;
        } else {
            pushToast("No se puede enganchar", "Ese nodo no admite hijos o se haría un ciclo", 2);
        }
    }
    if (ev.unlink) {
        t.detach(ev.unlink->node - 1);
        changed = true;
    }
    if (!ev.erase.empty()) {
        std::vector<int> indices;
        for (const int id : ev.erase) {
            if (id - 1 > 0) indices.push_back(id - 1);  // la Raiz no
        }
        std::sort(indices.rbegin(), indices.rend());
        for (const int i : indices) {
            if (i < static_cast<int>(t.nodes.size())) t.removeNode(i);
        }
        st.canvas.selection.clear();
        changed = true;
    }
    if (ev.moved) {
        t.sortChildrenByPosition();
        changed = true;
    }
    if (ev.background_menu || ev.dropped) {
        st.create_at = ev.menu_position;
        st.create_from = ev.dropped;
        ImGui::OpenPopup("bt_create");
    }
    {
        std::vector<nodegraph::SearchItem> items;
        for (const ai::BtNodeKind kind : ai::btNodeKinds()) {
            if (kind == ai::BtNodeKind::Root) continue;
            items.push_back(nodegraph::SearchItem{ai::btIsComposite(kind) ? "Compuestos" : "Tareas", ai::btNodeLabel(kind), ai::btNodeKey(kind),
                                                  ai::btNodeTooltip(kind), static_cast<int>(kind)});
        }
        const int chosen = nodegraph::searchPopup("bt_create", items, st.search);
        if (chosen >= 0) {
            int parent = -1;
            if (st.create_from && st.create_from->output) parent = st.create_from->node - 1;
            const int index = t.addNode(static_cast<ai::BtNodeKind>(chosen), parent, st.create_at);
            if (st.create_from && !st.create_from->output) t.attach(st.create_from->node - 1, index);
            t.sortChildrenByPosition();
            st.canvas.select(index + 1);
            changed = true;
        }
    }
    ImGui::SameLine();

    // --- Derecha: detalles ---
    ImGui::BeginChild("bt_right", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
    const int selected = st.canvas.selection.size() == 1 ? *st.canvas.selection.begin() - 1 : -1;
    if (selected < 0 || selected >= static_cast<int>(t.nodes.size())) {
        ImGui::TextDisabled("Selecciona un nodo.");
        ImGui::TextWrapped("Compuestos: Selector (el primero que salga bien), Sequence (todos en orden), Parallel, "
                           "Random. Tareas en las hojas. Decoradores: condiciones (pueden abortar) y modificadores. "
                           "Servicios: sensores que actualizan la pizarra cada cierto tiempo.");
    } else {
        ai::BtNode& n = t.nodes[static_cast<std::size_t>(selected)];
        ImGui::TextUnformatted(ai::btNodeLabel(n.kind));
        ImGui::TextWrapped("%s", ai::btNodeTooltip(n.kind));
        if (n.kind != ai::BtNodeKind::Root) {
            changed |= ImGui::InputTextWithHint("Nombre", ai::btNodeLabel(n.kind), &n.name);
            changed |= ImGui::InputTextWithHint("Nota", "comentario", &n.comment);
        }
        const std::vector<ai::BtParamInfo>& infos = ai::btParamsOf(n.kind);
        if (!infos.empty()) {
            ImGui::SeparatorText("Parámetros");
            for (const ai::BtParamInfo& info : infos) changed |= paramEditor(n.params, info, t);
        }
        if (n.kind != ai::BtNodeKind::Root) {
            ImGui::SeparatorText("Decoradores");
            int remove_d = -1;
            for (std::size_t i = 0; i < n.decorators.size(); ++i) {
                ai::BtDecorator& d = n.decorators[i];
                ImGui::PushID(static_cast<int>(i));
                const bool open = ImGui::TreeNodeEx(ai::btDecoratorLabel(d.kind), ImGuiTreeNodeFlags_DefaultOpen);
                ImGui::SetItemTooltip("%s", ai::btDecoratorTooltip(d.kind));
                ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - 20.0f);
                if (ImGui::SmallButton("x")) remove_d = static_cast<int>(i);
                if (open) {
                    if (ai::btIsCondition(d.kind)) {
                        int abort = static_cast<int>(d.abort);
                        if (ImGui::BeginCombo("Abortar", ai::btAbortLabel(d.abort))) {
                            for (int a = 0; a < ai::kBtAbortCount; ++a) {
                                if (ImGui::Selectable(ai::btAbortLabel(static_cast<ai::BtAbort>(a)), abort == a)) {
                                    d.abort = static_cast<ai::BtAbort>(a);
                                    changed = true;
                                }
                            }
                            ImGui::EndCombo();
                        }
                        ImGui::SetItemTooltip("Self: corta su rama si deja de cumplirse.\nLower Priority: corta las de la derecha si empieza a cumplirse.");
                    }
                    for (const ai::BtParamInfo& info : ai::btParamsOf(d.kind)) changed |= paramEditor(d.params, info, t);
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
            if (remove_d >= 0) {
                n.decorators.erase(n.decorators.begin() + remove_d);
                changed = true;
            }
            if (ImGui::Button("+ Decorador")) ImGui::OpenPopup("bt_add_dec");
            if (ImGui::BeginPopup("bt_add_dec")) {
                for (int k = 0; k < ai::kBtDecoratorKindCount; ++k) {
                    const ai::BtDecoratorKind kind = static_cast<ai::BtDecoratorKind>(k);
                    if (ImGui::MenuItem(ai::btDecoratorLabel(kind))) {
                        ai::BtDecorator d;
                        d.kind = kind;
                        d.params = ai::btDefaultParams(ai::btParamsOf(kind));
                        n.decorators.push_back(std::move(d));
                        changed = true;
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", ai::btDecoratorTooltip(kind));
                }
                ImGui::EndPopup();
            }
        }
        if (ai::btIsComposite(n.kind)) {
            ImGui::SeparatorText("Servicios");
            int remove_s = -1;
            for (std::size_t i = 0; i < n.services.size(); ++i) {
                ai::BtService& s = n.services[i];
                ImGui::PushID(1000 + static_cast<int>(i));
                const bool open = ImGui::TreeNodeEx(ai::btServiceLabel(s.kind), ImGuiTreeNodeFlags_DefaultOpen);
                ImGui::SetItemTooltip("%s", ai::btServiceTooltip(s.kind));
                ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - 20.0f);
                if (ImGui::SmallButton("x")) remove_s = static_cast<int>(i);
                if (open) {
                    changed |= ImGui::DragFloat("Cada (s)", &s.interval, 0.01f, 0.0f, 60.0f, "%.2f");
                    changed |= ImGui::DragFloat("± al azar (s)", &s.deviation, 0.01f, 0.0f, 10.0f, "%.2f");
                    for (const ai::BtParamInfo& info : ai::btParamsOf(s.kind)) changed |= paramEditor(s.params, info, t);
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
            if (remove_s >= 0) {
                n.services.erase(n.services.begin() + remove_s);
                changed = true;
            }
            if (ImGui::Button("+ Servicio")) ImGui::OpenPopup("bt_add_srv");
            if (ImGui::BeginPopup("bt_add_srv")) {
                for (int k = 0; k < ai::kBtServiceKindCount; ++k) {
                    const ai::BtServiceKind kind = static_cast<ai::BtServiceKind>(k);
                    if (ImGui::MenuItem(ai::btServiceLabel(kind))) {
                        ai::BtService s;
                        s.kind = kind;
                        s.params = ai::btDefaultParams(ai::btParamsOf(kind));
                        n.services.push_back(std::move(s));
                        changed = true;
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", ai::btServiceTooltip(kind));
                }
                ImGui::EndPopup();
            }
        }
        if (running && static_cast<std::size_t>(selected) < live_bt->runtime.nodes.size()) {
            const ai::BtNodeState& ns = live_bt->runtime.nodes[static_cast<std::size_t>(selected)];
            ImGui::SeparatorText("En vivo");
            ImGui::Text("Estado: %s%s", ai::btStatusLabel(ns.status), ns.active ? " (activo)" : "");
            ImGui::TextDisabled("Veces que entró: %llu", static_cast<unsigned long long>(ns.runs));
        }
    }
    ImGui::EndChild();

    if (changed) st.dirty = true;
    if (st.dirty && !ImGui::IsAnyItemActive() && ImGui::GetIO().MouseDownDuration[0] < 0.0f) saveBehaviorTreeEditor();
    ImGui::End();
}

}  // namespace cramion::editor
