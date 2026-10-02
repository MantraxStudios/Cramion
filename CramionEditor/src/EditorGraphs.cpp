// Editores de grafos de nodos (con el lienzo de NodeGraph.h):
//
//   Shader Graph (.crshadergraph)  material con nodos: genera un .crshader al
//                                  lado (se recompila al momento); vista
//                                  previa de la superficie en una esfera
//                                  (evaluada en la CPU), el codigo generado y
//                                  los errores sobre su nodo. Las Propiedades
//                                  salen en el Inspector del material.
//   Visual Scripting (.crgraph)    Blueprints: eventos, flujo, variables y
//                                  cualquier funcion de la API (nodos
//                                  generados del autocompletado). En Play se
//                                  iluminan los nodos que corren, los pines
//                                  enseñan su ultimo valor y los puntos de
//                                  ruptura (F9) pausan el juego.
//
// Tambien: abrir estos archivos desde el Proyecto y el menu Crear.

#include "EditorApp.h"

#include "Dialogs.h"
#include "LuaCompletion.h"
#include "NodeGraph.h"

#include <CramionCore/asset/ShaderGraph.h>
#include <CramionCore/scripting/VisualScript.h>

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>

namespace cramion::editor {

namespace sg = assets::shadergraph;

struct GraphEditorState {
    // --- Shader Graph ---
    bool show_sg = false;
    bool sg_focus = false;
    std::filesystem::path sg_path;
    sg::Graph sg_graph;
    bool sg_dirty = false;
    bool sg_regenerate = true;
    nodegraph::Canvas sg_canvas;
    sg::GenerateResult sg_result;
    std::string sg_search;
    core::Vec2 sg_create_at{};
    std::optional<nodegraph::PinRef> sg_create_from;
    std::vector<sg::Node> sg_clipboard;
    std::vector<sg::Link> sg_clipboard_links;
    int sg_tab = 0;
    // Vista previa (esfera)
    std::vector<ImU32> sg_preview;
    double sg_preview_time = -1.0;
    bool sg_preview_dirty = true;

    // --- Visual Script ---
    bool show_vs = false;
    bool vs_focus = false;
    std::filesystem::path vs_path;
    vscript::Graph vs_graph;
    bool vs_dirty = false;
    nodegraph::Canvas vs_canvas;
    vscript::CompileResult vs_compile;
    bool vs_recompile = true;
    std::string vs_search;
    core::Vec2 vs_create_at{};
    std::optional<nodegraph::PinRef> vs_create_from;
    std::vector<vscript::Node> vs_clipboard;
    std::vector<vscript::Link> vs_clipboard_links;
    std::vector<nodegraph::SearchItem> vs_items;  // catalogo + API (cache)
    std::vector<vscript::Node> vs_item_nodes;
    bool vs_debug = false;
    std::string vs_break_text;
};

namespace {

ImU32 sgPinColor(sg::PinType t) {
    switch (t) {
        case sg::PinType::Float: return IM_COL32(150, 170, 220, 255);
        case sg::PinType::Vec2: return IM_COL32(120, 220, 120, 255);
        case sg::PinType::Vec3: return IM_COL32(240, 220, 90, 255);
        case sg::PinType::Vec4: return IM_COL32(230, 130, 200, 255);
        case sg::PinType::Texture: return IM_COL32(230, 90, 80, 255);
        case sg::PinType::Dynamic: return IM_COL32(210, 210, 210, 255);
    }
    return IM_COL32(200, 200, 200, 255);
}

ImU32 sgHeaderColor(const std::string& category) {
    if (category == "Salida") return IM_COL32(150, 60, 60, 255);
    if (category == "Entrada") return IM_COL32(60, 110, 70, 255);
    if (category == "Propiedades") return IM_COL32(60, 90, 150, 255);
    if (category == "Texturas") return IM_COL32(150, 90, 50, 255);
    if (category == "Ruido") return IM_COL32(100, 70, 140, 255);
    return IM_COL32(70, 78, 92, 255);
}

const char* bindingLabel(sg::PinDefault d) {
    switch (d) {
        case sg::PinDefault::UV: return "(UV)";
        case sg::PinDefault::Position: return "(posición)";
        case sg::PinDefault::Normal: return "(normal)";
        case sg::PinDefault::VertexNormal: return "(normal vértice)";
        case sg::PinDefault::ViewDirection: return "(vista)";
        case sg::PinDefault::Time: return "(tiempo)";
        default: return "";
    }
}

ImU32 vsPinColor(vscript::PinType t) {
    switch (t) {
        case vscript::PinType::Exec: return IM_COL32(240, 240, 240, 255);
        case vscript::PinType::Any: return IM_COL32(170, 170, 170, 255);
        case vscript::PinType::Bool: return IM_COL32(200, 60, 60, 255);
        case vscript::PinType::Int: return IM_COL32(60, 210, 180, 255);
        case vscript::PinType::Float: return IM_COL32(150, 230, 90, 255);
        case vscript::PinType::String: return IM_COL32(240, 100, 200, 255);
        case vscript::PinType::Vector: return IM_COL32(250, 200, 50, 255);
        case vscript::PinType::Entity: return IM_COL32(80, 150, 250, 255);
    }
    return IM_COL32(200, 200, 200, 255);
}

ImU32 toColor(const core::Vec3& c) {
    return IM_COL32(static_cast<int>(std::clamp(c.x, 0.0f, 1.0f) * 255.0f), static_cast<int>(std::clamp(c.y, 0.0f, 1.0f) * 255.0f),
                    static_cast<int>(std::clamp(c.z, 0.0f, 1.0f) * 255.0f), 255);
}

std::filesystem::path uniqueFile(const std::filesystem::path& folder, const std::string& stem, const std::string& ext) {
    std::filesystem::path path = folder / dialogs::fromUtf8(stem + ext);
    for (int i = 2; std::filesystem::exists(path); ++i) path = folder / dialogs::fromUtf8(stem + " " + std::to_string(i) + ext);
    return path;
}

}  // namespace

GraphEditorState& EditorApp::graphEditors() {
    if (!graph_editors_) graph_editors_ = std::make_shared<GraphEditorState>();
    return *graph_editors_;
}

// --- Abrir y crear --------------------------------------------------------------------

bool EditorApp::openGraphFile(const std::filesystem::path& file) {
    std::string ext = dialogs::utf8(file.extension());
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext == assets::kShaderGraphExtension) {
        openShaderGraphEditor(file);
        return true;
    }
    if (ext == vscript::kGraphExtension) {
        openVisualScriptEditor(file);
        return true;
    }
    if (ext == twod::kTilesetExtension) {
        openTilesetEditor(file);
        return true;
    }
    // Un .crshader generado por un grafo: el grafo.
    if (ext == assets::kSurfaceShaderExtension) {
        const std::filesystem::path graph = sg::graphForShader(file);
        if (!graph.empty() && std::filesystem::exists(graph)) {
            openShaderGraphEditor(graph);
            return true;
        }
    }
    return false;
}

void EditorApp::drawCreateMenuExtras(const std::filesystem::path& folder) {
    drawEffectsCreateMenu(folder);
    if (ImGui::MenuItem("Shader Graph")) {
        std::error_code ec;
        std::filesystem::create_directories(folder, ec);
        const std::filesystem::path path = uniqueFile(folder, "NuevoShaderGraph", assets::kShaderGraphExtension);
        sg::Graph graph = sg::makeDefault();
        if (sg::save(graph, path)) {
            sg::GenerateResult result;
            std::string error;
            sg::writeGeneratedShader(graph, path, result, &error);
            refreshDatabase();
            if (sync_) sync_->reloadSurfaceShaders();
            openShaderGraphEditor(path);
        }
    }
    ImGui::SetItemTooltip("Material con nodos (genera un .crshader que eligen los materiales)");
    if (ImGui::MenuItem("Visual Script")) {
        std::error_code ec;
        std::filesystem::create_directories(folder, ec);
        const std::filesystem::path path = uniqueFile(folder, "NuevoVisualScript", vscript::kGraphExtension);
        vscript::Graph graph = vscript::exampleGraph();
        graph.uuid = Uuid::generate();
        std::string error;
        if (vscript::saveGraph(graph, path, &error)) {
            refreshDatabase();
            openVisualScriptEditor(path);
        } else {
            std::cerr << "[Editor] " << error << "\n";
        }
    }
    ImGui::SetItemTooltip("Lógica con nodos, como los Blueprints de Unreal");
    if (ImGui::BeginMenu("Behavior Tree (IA)")) {
        if (ImGui::MenuItem("Vacío")) createBehaviorTreeAsset(folder, false);
        if (ImGui::MenuItem("Guardia de ejemplo")) createBehaviorTreeAsset(folder, true);
        ImGui::EndMenu();
    }
    if (ImGui::MenuItem("Base de Motion Matching")) createMotionDatabaseAsset(folder);
    ImGui::SetItemTooltip("Con los clips (.cranim) seleccionados en el Proyecto");
    draw2DCreateMenu(folder);
}

// --- Shader Graph ---------------------------------------------------------------------

void EditorApp::openShaderGraphEditor(const std::filesystem::path& file) {
    GraphEditorState& st = graphEditors();
    if (st.sg_dirty) saveShaderGraphEditor();
    sg::Graph graph;
    std::string error;
    if (!sg::load(file, graph, &error)) {
        std::cerr << "[Editor] No se pudo abrir el Shader Graph: " << error << "\n";
        return;
    }
    graph.normalize();
    st.sg_graph = std::move(graph);
    st.sg_path = file;
    st.sg_dirty = false;
    st.sg_regenerate = true;
    st.sg_preview_dirty = true;
    st.sg_canvas.selection.clear();
    st.show_sg = true;
    st.sg_focus = true;
    std::vector<core::Vec2> positions;
    std::vector<nodegraph::NodeView> views;
    positions.reserve(st.sg_graph.nodes.size());
    for (const sg::Node& n : st.sg_graph.nodes) positions.push_back(core::Vec2{n.x, n.y});
    for (std::size_t i = 0; i < st.sg_graph.nodes.size(); ++i) {
        nodegraph::NodeView v;
        v.id = st.sg_graph.nodes[i].id;
        v.position = &positions[i];
        v.width = 200.0f;
        views.push_back(v);
    }
    st.sg_canvas.frame(views);
}

void EditorApp::saveShaderGraphEditor() {
    GraphEditorState& st = graphEditors();
    if (st.sg_path.empty()) return;
    if (!sg::save(st.sg_graph, st.sg_path)) {
        std::cerr << "[Editor] No se pudo guardar " << dialogs::utf8(st.sg_path) << "\n";
        return;
    }
    std::string error;
    sg::writeGeneratedShader(st.sg_graph, st.sg_path, st.sg_result, &error);
    if (!error.empty()) std::cerr << "[Shader Graph] " << error << "\n";
    st.sg_dirty = false;
    if (sync_) sync_->reloadSurfaceShaders();
    refreshDatabase();
}

void EditorApp::drawShaderGraphEditor() {
    GraphEditorState& st = graphEditors();
    if (!st.show_sg) return;
    const std::string title = "Shader Graph: " + dialogs::utf8(st.sg_path.stem()) + (st.sg_dirty ? " *" : "") + "###shader_graph";
    if (!beginGraphWorkspace(GraphKind::ShaderGraph, title.c_str())) return;  // en su pestana de arriba
    if (!st.show_sg && st.sg_dirty) saveShaderGraphEditor();
    sg::Graph& g = st.sg_graph;
    bool changed = false;

    if (st.sg_regenerate) {
        st.sg_result = sg::GenerateResult{};
        sg::generate(g, dialogs::utf8(st.sg_path.stem()), st.sg_result);
        st.sg_regenerate = false;
    }
    // Error de compilacion del .crshader generado (el del renderizador).
    const std::string shader_relative = assetRelative(sg::generatedShaderPath(st.sg_path));
    const std::string compile_error = sync_ ? sync_->surfaceShaderError(shader_relative) : std::string{};
    const int compile_error_node = compile_error.empty() ? 0 : sg::nodeForErrorLine(st.sg_result, compile_error);

    // --- Barra ---
    if (ImGui::Button("Guardar")) saveShaderGraphEditor();
    ImGui::SameLine();
    if (ImGui::Button("Crear material")) {
        const std::filesystem::path folder = st.sg_path.parent_path();
        assets::MaterialAsset m;
        m.uuid = Uuid::generate();
        m.shader = shader_relative;
        const std::filesystem::path path = uniqueFile(folder, dialogs::utf8(st.sg_path.stem()), ".crmat");
        if (assets::saveMaterial(m, path)) {
            refreshDatabase();
            inspected_material_ = m.uuid;
            pushToast("Material creado", dialogs::utf8(path.filename()), 1);
        }
    }
    ImGui::SetItemTooltip("Un .crmat que usa este shader (arrástralo a un objeto)");
    ImGui::SameLine();
    ImGui::TextDisabled("Clic derecho o Espacio: nodo · arrastrar desde un pin · Alt+clic: soltar · F: encuadrar");
    if (!st.sg_result.errors.empty() || !compile_error.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s",
                           !st.sg_result.errors.empty() ? st.sg_result.errors.front().c_str() : compile_error.c_str());
    }

    // --- Grafo ---
    std::vector<core::Vec2> positions(g.nodes.size());
    std::vector<core::Vec2> group_positions(g.groups.size());
    std::vector<core::Vec2> group_sizes(g.groups.size());
    std::vector<nodegraph::NodeView> views;
    std::vector<nodegraph::LinkView> links;
    // Grupos como comentarios (ids negativos).
    for (std::size_t i = 0; i < g.groups.size(); ++i) {
        sg::Group& gr = g.groups[i];
        group_positions[i] = core::Vec2{gr.x, gr.y};
        group_sizes[i] = core::Vec2{gr.w, gr.h};
        nodegraph::NodeView v;
        v.id = -static_cast<int>(i) - 1;
        v.comment = true;
        v.title = gr.title;
        v.position = &group_positions[i];
        v.comment_size = &group_sizes[i];
        v.header = IM_COL32(static_cast<int>(gr.color.x * 255), static_cast<int>(gr.color.y * 255), static_cast<int>(gr.color.z * 255), 255);
        views.push_back(std::move(v));
    }
    for (std::size_t i = 0; i < g.nodes.size(); ++i) {
        sg::Node& n = g.nodes[i];
        positions[i] = core::Vec2{n.x, n.y};
        const sg::NodeDef* def = sg::findNodeDef(n.type);
        nodegraph::NodeView v;
        v.id = n.id;
        v.position = &positions[i];
        v.width = def != nullptr && def->type == "master" ? 230.0f : 200.0f;
        v.title = def != nullptr ? def->title : n.type;
        if (def != nullptr && sg::hasField(def->fields, sg::NodeField::Name) && !n.name.empty()) v.title += ": " + n.name;
        v.header = def != nullptr ? sgHeaderColor(def->category) : IM_COL32(120, 40, 40, 255);
        v.tooltip = def != nullptr ? def->description : "Tipo de nodo desconocido";
        if (def != nullptr) {
            for (std::size_t k = 0; k < def->inputs.size(); ++k) {
                const sg::PinDef& pd = def->inputs[k];
                nodegraph::PinView pin;
                pin.label = pd.name;
                pin.color = sgPinColor(pd.type);
                pin.connected = g.linkTo(n.id, static_cast<int>(k)) != nullptr;
                pin.inline_value = pd.binding == sg::PinDefault::Value && pd.type != sg::PinType::Texture;
                if (pd.binding != sg::PinDefault::Value && !pin.connected) pin.label += std::string(" ") + bindingLabel(pd.binding);
                v.inputs.push_back(std::move(pin));
            }
            for (const sg::PinDef& pd : def->outputs) {
                v.outputs.push_back(nodegraph::PinView{pd.name, sgPinColor(pd.type), false, false});
            }
        }
        for (const sg::Link& l : g.links) {
            if (l.from_node == n.id && l.from_pin >= 0 && l.from_pin < static_cast<int>(v.outputs.size())) {
                v.outputs[static_cast<std::size_t>(l.from_pin)].connected = true;
            }
        }
        if (n.id == st.sg_result.error_node && !st.sg_result.errors.empty()) v.error = st.sg_result.errors.front();
        if (n.id == compile_error_node && compile_error_node != 0) v.error = compile_error;
        views.push_back(std::move(v));
    }
    for (const sg::Link& l : g.links) {
        nodegraph::LinkView lv;
        lv.from_node = l.from_node;
        lv.from_pin = l.from_pin;
        lv.to_node = l.to_node;
        lv.to_pin = l.to_pin;
        const sg::Node* from = g.find(l.from_node);
        const sg::NodeDef* def = from != nullptr ? sg::findNodeDef(from->type) : nullptr;
        lv.color = def != nullptr && l.from_pin < static_cast<int>(def->outputs.size()) ? sgPinColor(def->outputs[static_cast<std::size_t>(l.from_pin)].type)
                                                                                       : IM_COL32(200, 200, 200, 255);
        links.push_back(lv);
    }
    const nodegraph::InlineEditor inline_editor = [&](int node_id, int pin, float width) {
        sg::Node* n = g.find(node_id);
        const sg::NodeDef* def = n != nullptr ? sg::findNodeDef(n->type) : nullptr;
        if (def == nullptr || pin < 0 || pin >= static_cast<int>(def->inputs.size())) return false;
        if (static_cast<int>(n->inputs.size()) <= pin) n->inputs.resize(static_cast<std::size_t>(pin) + 1);
        core::Vec4& v = n->inputs[static_cast<std::size_t>(pin)];
        int components = sg::pinWidth(def->inputs[static_cast<std::size_t>(pin)].type);
        if (components <= 0) components = 1;
        ImGui::SetNextItemWidth(width);
        const bool edited = ImGui::DragScalarN("##v", ImGuiDataType_Float, &v.x, components, 0.01f, nullptr, nullptr, "%.2f");
        if (edited) changed = true;
        return edited;
    };
    const float right_width = 360.0f;
    const ImVec2 graph_size(std::max(ImGui::GetContentRegionAvail().x - right_width - 8.0f, 200.0f), 0.0f);
    const nodegraph::Events ev = st.sg_canvas.draw("sg_canvas", views, links, graph_size, &inline_editor);
    // Posiciones de vuelta al modelo.
    for (std::size_t i = 0; i < g.nodes.size(); ++i) {
        g.nodes[i].x = positions[i].x;
        g.nodes[i].y = positions[i].y;
    }
    for (std::size_t i = 0; i < g.groups.size(); ++i) {
        g.groups[i].x = group_positions[i].x;
        g.groups[i].y = group_positions[i].y;
        g.groups[i].w = group_sizes[i].x;
        g.groups[i].h = group_sizes[i].y;
    }
    if (ev.link) {
        std::string error;
        if (g.connect(ev.link->from_node, ev.link->from_pin, ev.link->to_node, ev.link->to_pin, &error)) changed = true;
        else pushToast("No se pueden conectar", error, 2);
    }
    if (ev.unlink) {
        g.disconnectInput(ev.unlink->node, ev.unlink->pin);
        changed = true;
    }
    if (!ev.erase.empty()) {
        std::vector<int> groups_to_remove;
        for (const int id : ev.erase) {
            if (id < 0) {
                groups_to_remove.push_back(-id - 1);
                continue;
            }
            const sg::Node* n = g.find(id);
            if (n != nullptr && n->type == "master") continue;
            g.remove(id);
        }
        std::sort(groups_to_remove.rbegin(), groups_to_remove.rend());
        for (const int gi : groups_to_remove) {
            if (gi >= 0 && gi < static_cast<int>(g.groups.size())) g.groups.erase(g.groups.begin() + gi);
        }
        st.sg_canvas.selection.clear();
        changed = true;
    }
    if (ev.moved) changed = true;
    if (ev.copy || ev.duplicate) {
        st.sg_clipboard.clear();
        st.sg_clipboard_links.clear();
        for (const int id : st.sg_canvas.selection) {
            if (const sg::Node* n = g.find(id); n != nullptr && n->type != "master") st.sg_clipboard.push_back(*n);
        }
        for (const sg::Link& l : g.links) {
            if (st.sg_canvas.selection.contains(l.from_node) && st.sg_canvas.selection.contains(l.to_node)) st.sg_clipboard_links.push_back(l);
        }
    }
    if ((ev.paste || ev.duplicate) && !st.sg_clipboard.empty()) {
        float min_x = 1e30f, min_y = 1e30f;
        for (const sg::Node& n : st.sg_clipboard) {
            min_x = std::min(min_x, n.x);
            min_y = std::min(min_y, n.y);
        }
        const core::Vec2 at = ev.duplicate ? core::Vec2{min_x + 40.0f, min_y + 40.0f} : ev.menu_position;
        std::map<int, int> remap;
        st.sg_canvas.selection.clear();
        for (const sg::Node& src : st.sg_clipboard) {
            const int id = g.add(src.type, at.x + (src.x - min_x), at.y + (src.y - min_y));
            if (id == 0) continue;
            sg::Node* n = g.find(id);
            n->inputs = src.inputs;
            n->name = src.name;
            n->value = src.value;
            n->min = src.min;
            n->max = src.max;
            n->texture = src.texture;
            remap[src.id] = id;
            st.sg_canvas.selection.insert(id);
        }
        for (const sg::Link& l : st.sg_clipboard_links) {
            if (remap.count(l.from_node) && remap.count(l.to_node)) g.connect(remap[l.from_node], l.from_pin, remap[l.to_node], l.to_pin);
        }
        changed = true;
    }
    if (ev.background_menu || ev.dropped) {
        st.sg_create_at = ev.menu_position;
        st.sg_create_from = ev.dropped;
        ImGui::OpenPopup("sg_create");
    }
    if (ev.node_menu != 0) ImGui::OpenPopup("sg_node_menu");
    if (ImGui::BeginPopup("sg_node_menu")) {
        if (ImGui::MenuItem("Agrupar la selección")) {
            float min_x = 1e30f, min_y = 1e30f, max_x = -1e30f, max_y = -1e30f;
            for (const int id : st.sg_canvas.selection) {
                if (const sg::Node* n = g.find(id)) {
                    min_x = std::min(min_x, n->x);
                    min_y = std::min(min_y, n->y);
                    max_x = std::max(max_x, n->x + 200.0f);
                    max_y = std::max(max_y, n->y + 140.0f);
                }
            }
            if (min_x < max_x) {
                sg::Group gr;
                gr.x = min_x - 20.0f;
                gr.y = min_y - 40.0f;
                gr.w = max_x - min_x + 40.0f;
                gr.h = max_y - min_y + 60.0f;
                g.groups.push_back(gr);
                changed = true;
            }
        }
        if (ImGui::MenuItem("Borrar")) {
            for (const int id : std::set<int>(st.sg_canvas.selection)) {
                const sg::Node* n = g.find(id);
                if (n != nullptr && n->type != "master") g.remove(id);
            }
            st.sg_canvas.selection.clear();
            changed = true;
        }
        ImGui::EndPopup();
    }
    {
        std::vector<nodegraph::SearchItem> items;
        const std::vector<sg::NodeDef>& defs = sg::nodeDefinitions();
        for (std::size_t i = 0; i < defs.size(); ++i) {
            if (defs[i].unique) continue;
            items.push_back(nodegraph::SearchItem{defs[i].category, defs[i].title, defs[i].keywords, defs[i].description, static_cast<int>(i)});
        }
        const int chosen = nodegraph::searchPopup("sg_create", items, st.sg_search);
        if (chosen >= 0) {
            const sg::NodeDef& def = defs[static_cast<std::size_t>(chosen)];
            const int id = g.add(def.type, st.sg_create_at.x, st.sg_create_at.y);
            if (id != 0 && st.sg_create_from) {
                // Conectar al primer pin compatible.
                const sg::Node* from_node = g.find(st.sg_create_from->node);
                const sg::NodeDef* from_def = from_node != nullptr ? sg::findNodeDef(from_node->type) : nullptr;
                if (from_def != nullptr && st.sg_create_from->output) {
                    const sg::PinType type = from_def->outputs[static_cast<std::size_t>(st.sg_create_from->pin)].type;
                    for (std::size_t k = 0; k < def.inputs.size(); ++k) {
                        if (sg::compatible(type, def.inputs[k].type) &&
                            g.connect(st.sg_create_from->node, st.sg_create_from->pin, id, static_cast<int>(k))) {
                            break;
                        }
                    }
                } else if (from_def != nullptr) {
                    const sg::PinType type = from_def->inputs[static_cast<std::size_t>(st.sg_create_from->pin)].type;
                    for (std::size_t k = 0; k < def.outputs.size(); ++k) {
                        if (sg::compatible(def.outputs[k].type, type) &&
                            g.connect(id, static_cast<int>(k), st.sg_create_from->node, st.sg_create_from->pin)) {
                            break;
                        }
                    }
                }
            }
            if (id != 0) st.sg_canvas.select(id);
            changed = true;
        }
    }
    ImGui::SameLine();

    // --- Derecha: vista previa, detalles y codigo ---
    ImGui::BeginChild("sg_right", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
    {
        // Esfera: la superficie evaluada en la CPU (las texturas como damero).
        constexpr int kRes = 56;
        const double now = ImGui::GetTime();
        const bool animate = st.sg_result.uses_time && now - st.sg_preview_time > 0.1;
        if (st.sg_preview_dirty || animate || st.sg_preview.size() != kRes * kRes) {
            st.sg_preview.assign(kRes * kRes, 0);
            st.sg_preview_time = now;
            st.sg_preview_dirty = false;
            const sg::TextureSampler sampler = [](const std::string&, core::Vec2 uv) {
                const int cx = static_cast<int>(std::floor(uv.x * 8.0f));
                const int cy = static_cast<int>(std::floor(uv.y * 8.0f));
                const float c = ((cx + cy) & 1) != 0 ? 0.75f : 0.4f;
                return core::Vec4{c, c, c, 1.0f};
            };
            sg::Evaluator evaluator(g, sampler);
            const core::Vec3 light = core::normalize(core::Vec3{-0.5f, 0.7f, 0.6f});
            for (int y = 0; y < kRes; ++y) {
                for (int x = 0; x < kRes; ++x) {
                    const float px = (static_cast<float>(x) + 0.5f) / kRes * 2.0f - 1.0f;
                    const float py = 1.0f - (static_cast<float>(y) + 0.5f) / kRes * 2.0f;
                    const float r2 = px * px + py * py;
                    if (r2 > 1.0f) continue;
                    const core::Vec3 nrm{px, py, std::sqrt(1.0f - r2)};
                    sg::PreviewPoint point;
                    point.normal = nrm;
                    point.position = nrm * 0.5f;
                    point.view = core::Vec3{0.0f, 0.0f, 1.0f};
                    point.uv = core::Vec2{0.5f + std::atan2(nrm.x, nrm.z) / 6.2831853f, 0.5f - std::asin(std::clamp(nrm.y, -1.0f, 1.0f)) / 3.14159265f};
                    point.time = static_cast<float>(now);
                    const sg::PreviewSurface s = evaluator.surface(point);
                    const float diffuse = std::max(core::dot(nrm, light), 0.0f) * 0.85f + 0.15f;
                    const core::Vec3 h = core::normalize(light + core::Vec3{0.0f, 0.0f, 1.0f});
                    const float spec = std::pow(std::max(core::dot(nrm, h), 0.0f), 2.0f + (1.0f - s.roughness) * 120.0f) * (1.0f - s.roughness);
                    core::Vec3 c = s.albedo * (diffuse * s.occlusion * (1.0f - s.metallic * 0.7f)) + core::Vec3{spec, spec, spec} + s.emission;
                    c = core::Vec3{std::pow(std::clamp(c.x, 0.0f, 1.0f), 1.0f / 2.2f), std::pow(std::clamp(c.y, 0.0f, 1.0f), 1.0f / 2.2f),
                                   std::pow(std::clamp(c.z, 0.0f, 1.0f), 1.0f / 2.2f)};
                    st.sg_preview[static_cast<std::size_t>(y * kRes + x)] = toColor(c);
                }
            }
        }
        const float size = std::min(ImGui::GetContentRegionAvail().x, 220.0f);
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(p0, ImVec2(p0.x + size, p0.y + size), IM_COL32(24, 25, 28, 255), 6.0f);
        const float cell = size / kRes;
        for (int y = 0; y < kRes; ++y) {
            for (int x = 0; x < kRes; ++x) {
                const ImU32 c = st.sg_preview[static_cast<std::size_t>(y * kRes + x)];
                if (c == 0) continue;
                draw->AddRectFilled(ImVec2(p0.x + x * cell, p0.y + y * cell), ImVec2(p0.x + (x + 1) * cell + 0.5f, p0.y + (y + 1) * cell + 0.5f), c);
            }
        }
        ImGui::Dummy(ImVec2(size, size));
        ImGui::TextDisabled("Vista previa (las texturas como damero)");
    }
    if (ImGui::BeginTabBar("sg_tabs")) {
        if (ImGui::BeginTabItem("Nodo")) {
            sg::Node* n = st.sg_canvas.selection.size() == 1 ? g.find(*st.sg_canvas.selection.begin()) : nullptr;
            const sg::NodeDef* def = n != nullptr ? sg::findNodeDef(n->type) : nullptr;
            if (def == nullptr) {
                ImGui::TextDisabled("Selecciona un nodo.");
                ImGui::TextWrapped("%s", g.description.empty() ? "" : g.description.c_str());
            } else {
                ImGui::TextUnformatted(def->title.c_str());
                ImGui::TextWrapped("%s", def->description.c_str());
                if (sg::hasField(def->fields, sg::NodeField::Name)) changed |= ImGui::InputText("Nombre", &n->name);
                if (sg::hasField(def->fields, sg::NodeField::Value)) {
                    if (def->color_value) changed |= ImGui::ColorEdit4("Valor", &n->value.x, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
                    else changed |= ImGui::DragScalarN("Valor", ImGuiDataType_Float, &n->value.x, std::clamp(def->value_components, 1, 4), 0.01f);
                }
                if (sg::hasField(def->fields, sg::NodeField::Range)) {
                    changed |= ImGui::DragFloat("Mínimo", &n->min, 0.01f);
                    changed |= ImGui::DragFloat("Máximo", &n->max, 0.01f);
                }
                if (sg::hasField(def->fields, sg::NodeField::Texture)) {
                    changed |= assetFilePicker("sg_tex", project_.assetsFolder(), kImageFileExts, "imagen", n->texture);
                    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
                    ImGui::SetNextItemWidth(ImGui::CalcItemWidth() - (ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x));
                    changed |= ImGui::InputTextWithHint("Textura", "ruta en Assets", &n->texture);
                    if (ImGui::BeginDragDropTarget()) {
                        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kImagePayload)) {
                            n->texture = assetRelative(dialogs::fromUtf8(static_cast<const char*>(payload->Data)));
                            changed = true;
                        }
                        ImGui::EndDragDropTarget();
                    }
                }
                if (def->fragment_only) ImGui::TextDisabled("Solo en el píxel (no en los vértices).");
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Código generado")) {
            if (!compile_error.empty()) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", compile_error.c_str());
            for (const std::string& e : st.sg_result.errors) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", e.c_str());
            ImGui::InputTextMultiline("##code", &st.sg_result.code, ImVec2(-1.0f, -1.0f), ImGuiInputTextFlags_ReadOnly);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Grafo")) {
            changed |= ImGui::InputTextMultiline("Descripción", &g.description, ImVec2(-1.0f, 80.0f));
            ImGui::TextDisabled("%zu nodos · %zu enlaces · %zu grupos", g.nodes.size(), g.links.size(), g.groups.size());
            ImGui::TextDisabled("Genera: %s", shader_relative.c_str());
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();

    if (changed) {
        st.sg_dirty = true;
        st.sg_regenerate = true;
        st.sg_preview_dirty = true;
    }
    if (st.sg_dirty && !ImGui::IsAnyItemActive() && ImGui::GetIO().MouseDownDuration[0] < 0.0f) saveShaderGraphEditor();
    ImGui::End();
}

// --- Visual Scripting -----------------------------------------------------------------

void EditorApp::openVisualScriptEditor(const std::filesystem::path& file) {
    GraphEditorState& st = graphEditors();
    if (st.vs_dirty) saveVisualScriptEditor();
    vscript::Graph graph;
    std::string error;
    if (!vscript::loadGraph(file, graph, &error)) {
        std::cerr << "[Editor] No se pudo abrir el Visual Script: " << error << "\n";
        return;
    }
    st.vs_graph = std::move(graph);
    st.vs_path = file;
    st.vs_dirty = false;
    st.vs_recompile = true;
    st.vs_canvas.selection.clear();
    st.show_vs = true;
    st.vs_focus = true;
    std::vector<nodegraph::NodeView> views;
    for (vscript::Node& n : st.vs_graph.nodes) {
        nodegraph::NodeView v;
        v.id = n.id;
        v.position = &n.position;
        views.push_back(v);
    }
    st.vs_canvas.frame(views);
}

void EditorApp::saveVisualScriptEditor() {
    GraphEditorState& st = graphEditors();
    if (st.vs_path.empty()) return;
    std::string error;
    if (!vscript::saveGraph(st.vs_graph, st.vs_path, &error)) {
        std::cerr << "[Editor] No se pudo guardar el Visual Script: " << error << "\n";
        return;
    }
    st.vs_dirty = false;
    scripts_.clearErrors();
    scripts_.reloadFile(assetRelative(st.vs_path));  // en Play: se recompila al momento
}

void EditorApp::drawVisualScriptEditor() {
    GraphEditorState& st = graphEditors();
    if (!st.show_vs) return;
    const std::string title = "Visual Script: " + dialogs::utf8(st.vs_path.stem()) + (st.vs_dirty ? " *" : "") + "###visual_script";
    if (!beginGraphWorkspace(GraphKind::VisualScript, title.c_str())) return;  // en su pestana de arriba
    if (!st.show_vs && st.vs_dirty) saveVisualScriptEditor();
    vscript::Graph& g = st.vs_graph;
    const std::string relative = assetRelative(st.vs_path);
    bool changed = false;
    if (st.vs_recompile) {
        st.vs_compile = vscript::compileGraph(g, relative);
        st.vs_recompile = false;
    }
    // Catalogo + API (una vez).
    if (st.vs_items.empty()) {
        for (const vscript::NodeInfo& info : vscript::nodeCatalog()) {
            st.vs_items.push_back(nodegraph::SearchItem{info.category, info.title, info.keywords, info.description,
                                                        static_cast<int>(st.vs_item_nodes.size())});
            st.vs_item_nodes.push_back(vscript::makeNode(info.kind));
        }
        for (const auto& [table, members] : scripting::ScriptSystem::apiReference()) {
            if (table.empty()) continue;
            const bool method = table.back() == ':';
            const std::string owner = method ? table.substr(0, table.size() - 1) : table;
            for (const scripting::ScriptSystem::ApiMember& m : members) {
                if (m.name.empty() || m.name.rfind("__", 0) == 0) continue;
                const std::string callee = owner + (method ? (m.function ? ":" : ".") : ".") + m.name;
                std::string args, description;
                bool function = m.function;
                luaApiDoc(callee, args, description, function);
                const std::string category = "API: " + owner;
                if (m.function) {
                    st.vs_items.push_back(nodegraph::SearchItem{category, callee, m.name, description, static_cast<int>(st.vs_item_nodes.size())});
                    st.vs_item_nodes.push_back(vscript::makeCallNode(callee, args, description));
                } else {
                    st.vs_items.push_back(nodegraph::SearchItem{category, "Get " + callee, m.name, description, static_cast<int>(st.vs_item_nodes.size())});
                    st.vs_item_nodes.push_back(vscript::makePropertyNode(callee, false, description));
                    st.vs_items.push_back(nodegraph::SearchItem{category, "Set " + callee, m.name, description, static_cast<int>(st.vs_item_nodes.size())});
                    st.vs_item_nodes.push_back(vscript::makePropertyNode(callee, true, description));
                }
            }
        }
    }

    // --- Depuracion en vivo ---
    ecs::Entity live;
    scripting::ScriptSystem::VisualScriptDebug debug;
    bool has_debug = false;
    if (playing()) {
        const std::vector<entt::entity> objects = scripts_.visualScriptObjects(relative);
        entt::entity chosen = entt::null;
        if (const ecs::Entity active = world_.find(active_); active.valid()) {
            for (const entt::entity h : objects) {
                if (h == active.handle()) chosen = h;
            }
        }
        if (chosen == entt::null && !objects.empty()) chosen = objects.front();
        if (chosen != entt::null) {
            live = world_.wrap(chosen);
            has_debug = scripts_.visualScriptDebug(live, debug);
        }
        // Punto de ruptura alcanzado: pausa y muestra el nodo.
        std::string break_graph;
        int break_node = 0;
        entt::entity break_entity = entt::null;
        if (scripts_.takeVisualScriptBreak(break_graph, break_node, break_entity)) {
            play_state_ = PlayState::Paused;
            st.vs_break_text = "Punto de ruptura: nodo " + std::to_string(break_node) +
                               (world_.registry().valid(break_entity) ? " en " + world_.wrap(break_entity).name() : std::string());
            pushToast("Punto de ruptura", st.vs_break_text, 2);
            if (break_graph == relative) st.vs_canvas.select(break_node);
        }
    }

    // --- Barra ---
    if (ImGui::Button("Guardar")) saveVisualScriptEditor();
    ImGui::SameLine();
    if (ImGui::Button("Asignar a la selección")) {
        int assigned = 0;
        for (ecs::Entity e : topLevelSelection()) {
            vscript::VisualScript& c = e.has<vscript::VisualScript>() ? e.get<vscript::VisualScript>() : e.add<vscript::VisualScript>();
            c.graph = relative;
            ++assigned;
        }
        if (assigned > 0) commit();
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("Puntos de ruptura", &st.vs_debug)) scripts_.setVisualScriptDebugging(st.vs_debug);
    ImGui::SetItemTooltip("F9 sobre un nodo: poner/quitar. Al pasar por uno en Play, el juego se pausa.");
    ImGui::SameLine();
    if (!st.vs_compile.errors.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%zu error(es): %s", st.vs_compile.errors.size(),
                           st.vs_compile.errors.front().message.c_str());
    } else if (has_debug) {
        ImGui::TextColored(ImVec4(0.45f, 0.75f, 1.0f, 1.0f), "En vivo: %s", live.name().c_str());
    } else {
        ImGui::TextDisabled("Compila bien · Clic derecho o Espacio: nodo · F9: punto de ruptura");
    }
    if (!st.vs_break_text.empty() && play_state_ == PlayState::Paused) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "%s", st.vs_break_text.c_str());
    }

    // --- Izquierda: variables ---
    ImGui::BeginChild("vs_left", ImVec2(260.0f, 0.0f), ImGuiChildFlags_ResizeX | ImGuiChildFlags_Borders);
    ImGui::SeparatorText("Variables");
    int remove_var = -1;
    for (std::size_t i = 0; i < g.variables.size(); ++i) {
        vscript::Variable& v = g.variables[i];
        ImGui::PushID(static_cast<int>(i));
        std::string name = v.name;
        ImGui::SetNextItemWidth(110.0f);
        if (ImGui::InputText("##n", &name, ImGuiInputTextFlags_EnterReturnsTrue) && !name.empty() && g.findVariable(name) == nullptr) {
            g.renameVariable(v.name, name);
            changed = true;
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80.0f);
        if (ImGui::BeginCombo("##t", vscript::pinTypeLabel(v.type))) {
            for (int t = 1; t < vscript::kPinTypeCount; ++t) {
                if (ImGui::Selectable(vscript::pinTypeLabel(static_cast<vscript::PinType>(t)), static_cast<int>(v.type) == t)) {
                    v.type = static_cast<vscript::PinType>(t);
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) remove_var = static_cast<int>(i);
        if (has_debug && debug.variables.count(v.name)) {
            ImGui::TextColored(ImVec4(0.45f, 0.75f, 1.0f, 1.0f), "= %s", debug.variables[v.name].c_str());
        } else {
            ImGui::SetNextItemWidth(-1.0f);
            changed |= ImGui::InputTextWithHint("##v", "valor inicial", &v.value);
        }
        changed |= ImGui::Checkbox("En el Inspector", &v.exposed);
        ImGui::SameLine();
        if (ImGui::SmallButton("Get")) {
            vscript::Node n = vscript::makeNode("var.get", st.vs_canvas.viewCenter());
            n.fn = v.name;
            st.vs_canvas.select(g.add(std::move(n)).id);
            changed = true;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Set")) {
            vscript::Node n = vscript::makeNode("var.set", st.vs_canvas.viewCenter());
            n.fn = v.name;
            st.vs_canvas.select(g.add(std::move(n)).id);
            changed = true;
        }
        ImGui::Separator();
        ImGui::PopID();
    }
    if (remove_var >= 0) {
        g.variables.erase(g.variables.begin() + remove_var);
        changed = true;
    }
    if (ImGui::Button("+ Variable")) {
        std::string name = "variable";
        for (int i = 2; g.findVariable(name) != nullptr; ++i) name = "variable" + std::to_string(i);
        g.variables.push_back(vscript::Variable{name, vscript::PinType::Float, "0", true, {}});
        changed = true;
    }
    if (!st.vs_compile.errors.empty()) {
        ImGui::SeparatorText("Errores");
        for (const vscript::NodeError& e : st.vs_compile.errors) {
            ImGui::PushID(&e);
            if (ImGui::Selectable(((e.node != 0 ? "Nodo " + std::to_string(e.node) + ": " : std::string()) + e.message).c_str()) && e.node != 0) {
                st.vs_canvas.select(e.node);
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    ImGui::SameLine();

    // --- Grafo ---
    std::vector<nodegraph::NodeView> views;
    std::vector<nodegraph::LinkView> links;
    std::map<int, std::string> errors;
    for (const vscript::NodeError& e : st.vs_compile.errors) {
        if (e.node != 0) errors[e.node] = e.message;
    }
    for (vscript::Node& n : g.nodes) {
        nodegraph::NodeView v;
        v.id = n.id;
        v.position = &n.position;
        v.title = vscript::nodeTitle(n);
        v.header = toColor(vscript::nodeColor(n) * 0.75f);
        v.breakpoint = n.breakpoint;
        if (n.kind == "comment") {
            v.comment = true;
            v.comment_size = &n.size;
            v.header = IM_COL32(90, 110, 140, 255);
            views.push_back(std::move(v));
            continue;
        }
        if (const auto it = errors.find(n.id); it != errors.end()) v.error = it->second;
        if (const vscript::NodeInfo* info = vscript::findNodeInfo(n.kind)) v.tooltip = info->description;
        if (!n.comment.empty()) v.body = n.comment;
        v.width = std::max(170.0f, 70.0f + 7.0f * static_cast<float>(v.title.size()));
        for (std::size_t k = 0; k < n.inputs.size(); ++k) {
            const vscript::Pin& p = n.inputs[k];
            nodegraph::PinView pin;
            pin.label = p.name;
            pin.color = vsPinColor(p.type);
            pin.exec = p.type == vscript::PinType::Exec;
            pin.connected = g.linkTo(n.id, static_cast<int>(k)) != nullptr;
            pin.inline_value = !pin.exec;
            pin.tooltip = std::string(vscript::pinTypeLabel(p.type));
            if (has_debug) {
                const auto it = debug.values.find(std::to_string(n.id) + ":" + p.name);
                if (it != debug.values.end()) pin.tooltip += " = " + it->second;
            }
            v.inputs.push_back(std::move(pin));
        }
        for (std::size_t k = 0; k < n.outputs.size(); ++k) {
            const vscript::Pin& p = n.outputs[k];
            nodegraph::PinView pin;
            pin.label = p.name;
            pin.color = vsPinColor(p.type);
            pin.exec = p.type == vscript::PinType::Exec;
            pin.tooltip = std::string(vscript::pinTypeLabel(p.type));
            if (has_debug) {
                const auto it = debug.values.find(std::to_string(n.id) + ":" + p.name);
                if (it != debug.values.end()) pin.tooltip += " = " + it->second;
            }
            v.outputs.push_back(std::move(pin));
        }
        if (has_debug) {
            const auto it = debug.executed.find(n.id);
            if (it != debug.executed.end()) v.highlight = std::clamp(1.0f - static_cast<float>(debug.now - it->second) / 0.6f, 0.0f, 1.0f);
        }
        views.push_back(std::move(v));
    }
    for (const vscript::Link& l : g.links) {
        nodegraph::LinkView lv;
        lv.from_node = l.from_node;
        lv.from_pin = l.from_pin;
        lv.to_node = l.to_node;
        lv.to_pin = l.to_pin;
        const vscript::Node* from = g.find(l.from_node);
        const vscript::PinType type = from != nullptr && l.from_pin < static_cast<int>(from->outputs.size())
                                          ? from->outputs[static_cast<std::size_t>(l.from_pin)].type
                                          : vscript::PinType::Any;
        lv.exec = type == vscript::PinType::Exec;
        lv.color = vsPinColor(type);
        if (has_debug && lv.exec) {
            const auto it = debug.executed.find(l.to_node);
            if (it != debug.executed.end()) lv.flow = std::clamp(1.0f - static_cast<float>(debug.now - it->second) / 0.6f, 0.0f, 1.0f);
        }
        links.push_back(lv);
    }
    for (nodegraph::NodeView& v : views) {
        for (const vscript::Link& l : g.links) {
            if (l.from_node == v.id && l.from_pin >= 0 && l.from_pin < static_cast<int>(v.outputs.size())) {
                v.outputs[static_cast<std::size_t>(l.from_pin)].connected = true;
            }
        }
    }
    const nodegraph::InlineEditor inline_editor = [&](int node_id, int pin, float width) {
        vscript::Node* n = g.find(node_id);
        if (n == nullptr || pin < 0 || pin >= static_cast<int>(n->inputs.size())) return false;
        vscript::Pin& p = n->inputs[static_cast<std::size_t>(pin)];
        bool edited = false;
        if (p.type == vscript::PinType::Bool) {
            bool b = p.value == "true" || p.value == "1";
            if (ImGui::Checkbox("##b", &b)) {
                p.value = b ? "true" : "false";
                edited = true;
            }
        } else {
            ImGui::SetNextItemWidth(width);
            edited = ImGui::InputText("##v", &p.value);
        }
        if (edited) changed = true;
        return edited;
    };
    const nodegraph::Events ev = st.vs_canvas.draw("vs_canvas", views, links, ImVec2(0.0f, 0.0f), &inline_editor);
    if (ev.link) {
        std::string error;
        if (g.connect(ev.link->from_node, ev.link->from_pin, ev.link->to_node, ev.link->to_pin, &error)) changed = true;
        else pushToast("No se pueden enlazar", error, 2);
    }
    if (ev.unlink) {
        g.links.erase(std::remove_if(g.links.begin(), g.links.end(),
                                     [&](const vscript::Link& l) { return l.to_node == ev.unlink->node && l.to_pin == ev.unlink->pin; }),
                      g.links.end());
        changed = true;
    }
    if (!ev.erase.empty()) {
        for (const int id : ev.erase) g.removeNode(id);
        st.vs_canvas.selection.clear();
        changed = true;
    }
    if (ev.moved) changed = true;
    // F9: punto de ruptura en el nodo seleccionado / bajo el raton.
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) && ImGui::IsKeyPressed(ImGuiKey_F9, false)) {
        const int target = ev.hovered_node != 0 ? ev.hovered_node : (st.vs_canvas.selection.size() == 1 ? *st.vs_canvas.selection.begin() : 0);
        if (vscript::Node* n = g.find(target)) {
            n->breakpoint = !n->breakpoint;
            changed = true;
        }
    }
    if (ev.copy || ev.duplicate) {
        st.vs_clipboard.clear();
        st.vs_clipboard_links.clear();
        for (const int id : st.vs_canvas.selection) {
            if (const vscript::Node* n = g.find(id)) st.vs_clipboard.push_back(*n);
        }
        for (const vscript::Link& l : g.links) {
            if (st.vs_canvas.selection.contains(l.from_node) && st.vs_canvas.selection.contains(l.to_node)) st.vs_clipboard_links.push_back(l);
        }
    }
    if ((ev.paste || ev.duplicate) && !st.vs_clipboard.empty()) {
        float min_x = 1e30f, min_y = 1e30f;
        for (const vscript::Node& n : st.vs_clipboard) {
            min_x = std::min(min_x, n.position.x);
            min_y = std::min(min_y, n.position.y);
        }
        const core::Vec2 at = ev.duplicate ? core::Vec2{min_x + 40.0f, min_y + 40.0f} : ev.menu_position;
        std::map<int, int> remap;
        st.vs_canvas.selection.clear();
        for (vscript::Node n : st.vs_clipboard) {
            const int old = n.id;
            n.position = core::Vec2{at.x + (n.position.x - min_x), at.y + (n.position.y - min_y)};
            n.breakpoint = false;
            const int id = g.add(std::move(n)).id;
            remap[old] = id;
            st.vs_canvas.selection.insert(id);
        }
        for (const vscript::Link& l : st.vs_clipboard_links) {
            if (remap.count(l.from_node) && remap.count(l.to_node)) g.connect(remap[l.from_node], l.from_pin, remap[l.to_node], l.to_pin);
        }
        changed = true;
    }
    if (ev.background_menu || ev.dropped) {
        st.vs_create_at = ev.menu_position;
        st.vs_create_from = ev.dropped;
        ImGui::OpenPopup("vs_create");
    }
    if (ev.node_menu != 0) ImGui::OpenPopup("vs_node_menu");
    if (ImGui::BeginPopup("vs_node_menu")) {
        const int id = st.vs_canvas.selection.size() == 1 ? *st.vs_canvas.selection.begin() : 0;
        if (vscript::Node* n = g.find(id)) {
            if (ImGui::MenuItem(n->breakpoint ? "Quitar punto de ruptura" : "Punto de ruptura", "F9")) {
                n->breakpoint = !n->breakpoint;
                changed = true;
            }
            ImGui::SetNextItemWidth(220.0f);
            changed |= ImGui::InputTextWithHint("##note", "nota del nodo", &n->comment);
        }
        if (ImGui::MenuItem("Comentario alrededor")) {
            float min_x = 1e30f, min_y = 1e30f, max_x = -1e30f, max_y = -1e30f;
            for (const int sel : st.vs_canvas.selection) {
                if (const vscript::Node* s = g.find(sel)) {
                    min_x = std::min(min_x, s->position.x);
                    min_y = std::min(min_y, s->position.y);
                    max_x = std::max(max_x, s->position.x + 220.0f);
                    max_y = std::max(max_y, s->position.y + 120.0f);
                }
            }
            if (min_x < max_x) {
                vscript::Node c = vscript::makeNode("comment", core::Vec2{min_x - 20.0f, min_y - 40.0f});
                c.size = core::Vec2{max_x - min_x + 40.0f, max_y - min_y + 60.0f};
                c.comment = "Comentario";
                // Al principio: se dibuja detras.
                vscript::Node& added = g.add(std::move(c));
                vscript::Node copy = added;
                g.nodes.pop_back();
                g.nodes.insert(g.nodes.begin(), std::move(copy));
                changed = true;
            }
        }
        if (ImGui::MenuItem("Borrar")) {
            for (const int sel : std::set<int>(st.vs_canvas.selection)) g.removeNode(sel);
            st.vs_canvas.selection.clear();
            changed = true;
        }
        ImGui::EndPopup();
    }
    {
        const int chosen = nodegraph::searchPopup("vs_create", st.vs_items, st.vs_search);
        if (chosen >= 0 && chosen < static_cast<int>(st.vs_item_nodes.size())) {
            vscript::Node n = st.vs_item_nodes[static_cast<std::size_t>(chosen)];
            n.position = st.vs_create_at;
            const int id = g.add(std::move(n)).id;
            if (st.vs_create_from) {
                const vscript::Node* from = g.find(st.vs_create_from->node);
                const vscript::Node* to = g.find(id);
                if (from != nullptr && to != nullptr) {
                    if (st.vs_create_from->output) {
                        const vscript::PinType type = from->outputs[static_cast<std::size_t>(st.vs_create_from->pin)].type;
                        for (std::size_t k = 0; k < to->inputs.size(); ++k) {
                            if (vscript::pinTypesCompatible(type, to->inputs[k].type) &&
                                g.connect(st.vs_create_from->node, st.vs_create_from->pin, id, static_cast<int>(k))) {
                                break;
                            }
                        }
                    } else {
                        const vscript::PinType type = from->inputs[static_cast<std::size_t>(st.vs_create_from->pin)].type;
                        for (std::size_t k = 0; k < to->outputs.size(); ++k) {
                            if (vscript::pinTypesCompatible(to->outputs[k].type, type) &&
                                g.connect(id, static_cast<int>(k), st.vs_create_from->node, st.vs_create_from->pin)) {
                                break;
                            }
                        }
                    }
                }
            }
            st.vs_canvas.select(id);
            changed = true;
        }
    }

    if (changed) {
        st.vs_dirty = true;
        st.vs_recompile = true;
        // Puntos de ruptura al Play.
        std::vector<int> breakpoints;
        for (const vscript::Node& n : g.nodes) {
            if (n.breakpoint) breakpoints.push_back(n.id);
        }
        scripts_.setVisualScriptBreakpoints(relative, breakpoints);
    }
    if (st.vs_dirty && !ImGui::IsAnyItemActive() && ImGui::GetIO().MouseDownDuration[0] < 0.0f) saveVisualScriptEditor();
    ImGui::End();
}

EditorApp::GraphDoc EditorApp::graphDocShaderGraph() {
    GraphEditorState& st = graphEditors();
    GraphDoc d;
    d.show = &st.show_sg;
    d.focus = &st.sg_focus;
    d.dirty = st.sg_dirty;
    d.path = st.sg_path;
    return d;
}

EditorApp::GraphDoc EditorApp::graphDocVisualScript() {
    GraphEditorState& st = graphEditors();
    GraphDoc d;
    d.show = &st.show_vs;
    d.focus = &st.vs_focus;
    d.dirty = st.vs_dirty;
    d.path = st.vs_path;
    return d;
}

void EditorApp::drawGraphEditors() {
    if (!has_project_) return;
    drawShaderGraphEditor();
    drawVisualScriptEditor();
    drawBehaviorTreeEditor();
    draw2DWindows();
}

}  // namespace cramion::editor
