// Componente de edicion de Visual Script (ver VsGraphEditor.h): un
// Document por grafo abierto con su lienzo, su historial (instantaneas JSON
// al soltar el raton), su resultado de compilacion y su portapapeles. Lo usan
// el editor .crgraph suelto (EditorGraphs.cpp) y la pestana Blueprint.
//
// Nada se guarda solo: Compilar (F7) valida, guarda y recarga; Guardar
// (Ctrl+S) solo escribe el archivo.
//
// Fase 2: el lienzo ensena un grafo cada vez (Document::active): Event
// Graph, Construction Script, una funcion o una macro (Mi Blueprint > Grafos,
// Funciones, Macros; doble clic en una llamada la abre). Detalles edita la
// firma de una funcion / macro y las pistas de un Timeline. En Play los cables
// por los que pasa la ejecucion se iluminan (VisualScriptDebug::flows) y los
// pines vigilados o de los nodos elegidos llevan su valor en una burbuja.

#include "VsGraphEditor.h"

#include "PropertyInspector.h"  // kEntityPayload
#include "Theme.h"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <utility>

namespace cramion::editor::vsedit {

namespace {

using vscript::PinType;

constexpr const char* kVariablePayload = "CR_VS_VARIABLE";
constexpr std::size_t kMaxRecent = 12;

ImU32 toColor(const core::Vec3& c) {
    return IM_COL32(static_cast<int>(std::clamp(c.x, 0.0f, 1.0f) * 255.0f), static_cast<int>(std::clamp(c.y, 0.0f, 1.0f) * 255.0f),
                    static_cast<int>(std::clamp(c.z, 0.0f, 1.0f) * 255.0f), 255);
}

// Node::comment_color es 0xRRGGBBAA (0 = defecto).
ImU32 rgbaToColor(std::uint32_t c) {
    return IM_COL32((c >> 24) & 255u, (c >> 16) & 255u, (c >> 8) & 255u, c & 255u);
}

std::uint32_t floatsToRgba(const float c[4]) {
    const auto byte = [](float v) { return static_cast<std::uint32_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
    return (byte(c[0]) << 24) | (byte(c[1]) << 16) | (byte(c[2]) << 8) | byte(c[3]);
}

ImVec4 colorVec(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

std::string formatNumber(double v) {
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%.6g", v);
    return buf;
}

std::string formatVector(const core::Vec3& v) {
    return formatNumber(v.x) + " " + formatNumber(v.y) + " " + formatNumber(v.z);
}

std::string defaultValueFor(PinType type) {
    switch (type) {
        case PinType::Bool: return "false";
        case PinType::Int:
        case PinType::Float: return "0";
        case PinType::Vector: return "0 0 0";
        default: return {};
    }
}

bool boolValue(const std::string& text) {
    std::string t = text;
    std::transform(t.begin(), t.end(), t.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return t == "true" || t == "1" || t == "si" || t == "yes";
}

nodegraph::NodeStyle styleOf(vscript::NodeLook look) {
    switch (look) {
        case vscript::NodeLook::Event: return nodegraph::NodeStyle::Event;
        case vscript::NodeLook::Function: return nodegraph::NodeStyle::Function;
        case vscript::NodeLook::Pure: return nodegraph::NodeStyle::Pure;
        case vscript::NodeLook::Flow: return nodegraph::NodeStyle::Flow;
        case vscript::NodeLook::Compact: return nodegraph::NodeStyle::Compact;
        case vscript::NodeLook::Variable: return nodegraph::NodeStyle::Variable;
        case vscript::NodeLook::Reroute: return nodegraph::NodeStyle::Reroute;
        default: return nodegraph::NodeStyle::Default;
    }
}

bool validPin(const vscript::Node& n, int pin, bool output) {
    return pin >= 0 && pin < static_cast<int>(output ? n.outputs.size() : n.inputs.size());
}

// La entrada de una funcion / macro: una por grafo (ni se borra ni se copia).
bool isEntryKind(const std::string& kind) { return kind == "fn.entry" || kind == "macro.entry"; }

// Clave de VisualScriptDebug::values / inputs / flows: "nodo:pin".
std::string debugKey(int node, std::size_t pin) { return std::to_string(node) + ":" + std::to_string(pin); }

// --- Paleta (catalogo + API), una vez ---------------------------------------------------
struct PaletteCache {
    std::vector<PaletteEntry> entries;
    std::vector<nodegraph::SearchItem> items;  // items[i].index == i
};

const PaletteCache& paletteCache() {
    static const PaletteCache cache = [] {
        PaletteCache c;
        const auto push = [&](std::string category, std::string title, std::string keywords, std::string description,
                              vscript::Node node, std::string owner) {
            nodegraph::SearchItem item;
            item.category = std::move(category);
            item.title = std::move(title);
            item.keywords = std::move(keywords);
            item.description = std::move(description);
            item.index = static_cast<int>(c.entries.size());
            item.in_types = vscript::inputTypeMask(node);
            item.out_types = vscript::outputTypeMask(node);
            item.owner = std::move(owner);
            c.items.push_back(item);
            c.entries.push_back(PaletteEntry{std::move(item), std::move(node)});
        };
        for (const vscript::NodeInfo& info : vscript::nodeCatalog()) {
            push(info.category, info.title, info.keywords, info.description, vscript::makeNode(info.kind), {});
        }
        for (const vscript::ApiNodeTemplate& t : vscript::apiNodeTemplates(scripting::ScriptSystem::apiRegistry())) {
            std::string owner = t.category.rfind("API: ", 0) == 0 ? t.category.substr(5) : std::string();
            push(t.category, t.title, t.keywords, t.description, t.node, std::move(owner));
        }
        return c;
    }();
    return cache;
}

// --- Historial y cambios -----------------------------------------------------------------
void markChanged(Document& doc) {
    doc.dirty = true;
    doc.snapshot_pending = true;
    doc.breakpoints_changed = true;
}

// Toma la instantanea pendiente cuando ya no se arrastra ni se escribe. Solo
// un cambio real (JSON distinto) pide compilar: un clic en un nodo sin moverlo
// tambien llega como "moved" y no debe dejar el estado en "?".
void flushSnapshot(Document& doc, bool force) {
    if (!doc.snapshot_pending) return;
    if (!force && (ImGui::IsAnyItemActive() || ImGui::IsMouseDown(ImGuiMouseButton_Left))) return;
    doc.snapshot_pending = false;
    if (doc.history.push(vscript::graphToJson(doc.graph))) doc.needs_compile = true;
    doc.dirty = doc.history.current != doc.saved_state;
}

bool applyState(Document& doc, const std::string& state) {
    vscript::Graph graph;
    if (!vscript::graphFromJson(state, graph, nullptr)) return false;
    doc.graph = std::move(graph);
    // El grafo que se veia puede no existir ya (deshacer "Nueva funcion").
    if (!doc.active.empty() && doc.active != vscript::kConstructionGraph && doc.graph.findSubgraph(doc.active) == nullptr) doc.active.clear();
    if (!doc.details_function.empty() && doc.graph.findSubgraph(doc.details_function) == nullptr) doc.details_function.clear();
    doc.graph.editing = doc.active;
    // Mismo texto que dara graphToJson: la siguiente instantanea no cuenta como cambio (no borra rehacer).
    doc.history.current = vscript::graphToJson(doc.graph);
    std::set<int> keep;
    for (const int id : doc.canvas.selection) {
        if (doc.graph.find(id) != nullptr) keep.insert(id);
    }
    doc.canvas.selection = std::move(keep);
    doc.canvas.link_selection.clear();
    if (doc.details_node != 0 && doc.graph.find(doc.details_node) == nullptr) doc.details_node = 0;
    if (!doc.details_variable.empty() && doc.graph.findVariable(doc.details_variable) == nullptr) doc.details_variable.clear();
    doc.dirty = doc.history.current != doc.saved_state;
    doc.needs_compile = true;
    doc.breakpoints_changed = true;
    return true;
}

void syncBreakpoints(Document& doc, const Host& host) {
    if (!doc.breakpoints_changed) return;
    doc.breakpoints_changed = false;
    if (!host.breakpoints) return;
    std::vector<int> ids;
    for (const vscript::Node& n : doc.graph.nodes) {
        if (n.breakpoint) ids.push_back(n.id);
    }
    host.breakpoints(doc.relative, ids);
}

void notify(const Host& host, const std::string& title, const std::string& text) {
    if (host.notify) host.notify(title, text);
}

// --- Vistas para el lienzo -----------------------------------------------------------------
struct ViewData {
    std::vector<nodegraph::NodeView> nodes;
    std::vector<nodegraph::LinkView> links;
};

void buildViews(Document& doc, const scripting::ScriptSystem::VisualScriptDebug* debug, ViewData& out) {
    vscript::Graph& g = doc.graph;
    std::map<int, std::string> messages, warnings;  // errores / avisos (NodeError::warning)
    for (const vscript::NodeError& e : doc.compile.errors) {
        if (e.node == 0) continue;
        std::string& m = (e.warning ? warnings : messages)[e.node];
        if (!m.empty()) m += "\n";
        m += e.message;
    }
    std::set<std::pair<int, int>> linked_in, linked_out;
    for (const vscript::Link& l : g.links) {
        linked_in.insert({l.to_node, l.to_pin});
        linked_out.insert({l.from_node, l.from_pin});
    }
    // Valores en vivo por indice de pin: salidas (values) y entradas (inputs).
    const auto debugValue = [&](int node, std::size_t pin, bool output) -> const std::string* {
        if (debug == nullptr) return nullptr;
        const std::map<std::string, std::string>& values = output ? debug->values : debug->inputs;
        const auto it = values.find(debugKey(node, pin));
        return it != values.end() ? &it->second : nullptr;
    };
    const auto fade = [&](double when) {
        return std::clamp(1.0f - static_cast<float>(debug->now - when) / 0.6f, 0.0f, 1.0f);
    };
    const auto recency = [&](int node) {
        if (debug == nullptr) return 0.0f;
        const auto it = debug->executed.find(node);
        return it == debug->executed.end() ? 0.0f : fade(it->second);
    };
    // Un cable exec: por donde paso la ejecucion de verdad (flows); sin
    // depuracion en vivo, cuando corrio el nodo al que llega.
    const auto linkFlow = [&](const vscript::Link& l) {
        if (debug == nullptr) return 0.0f;
        if (debug->flows.empty()) return recency(l.to_node);
        const auto it = debug->flows.find(debugKey(l.from_node, static_cast<std::size_t>(l.from_pin)));
        return it == debug->flows.end() ? 0.0f : fade(it->second);
    };
    // Burbuja fija: pines vigilados y, en Play, los de los nodos elegidos.
    const auto watchText = [&](int node, std::size_t pin, bool output, const std::string* value) -> std::string {
        if (debug == nullptr) return {};
        const bool watched = doc.watches.contains(std::make_tuple(node, static_cast<int>(pin), output));
        if (!watched && !doc.canvas.selection.contains(node)) return {};
        if (value != nullptr) return *value;
        return watched ? std::string("(sin valor)") : std::string();
    };

    std::set<int> shown;  // nodos del grafo que se ve
    out.nodes.reserve(g.nodes.size());
    for (vscript::Node& n : g.nodes) {
        if (n.graph != doc.active) continue;
        shown.insert(n.id);
        nodegraph::NodeView v;
        v.id = n.id;
        v.position = &n.position;
        v.title = vscript::nodeTitle(n);
        v.header = toColor(vscript::nodeColor(n) * 0.75f);
        v.breakpoint = n.breakpoint;
        v.disabled = n.disabled;
        if (n.comment_color != 0) v.comment_color = rgbaToColor(n.comment_color);
        const vscript::NodeLook look = vscript::nodeLook(n);
        if (n.kind == "comment" || look == vscript::NodeLook::Comment) {
            v.comment = true;
            v.comment_size = &n.size;
            v.header = n.comment_color != 0 ? rgbaToColor(n.comment_color) : IM_COL32(90, 110, 140, 255);
            out.nodes.push_back(std::move(v));
            continue;
        }
        v.style = styleOf(look);
        if (const char* symbol = vscript::nodeSymbol(n); symbol != nullptr) v.symbol = symbol;
        if (const vscript::NodeInfo* info = vscript::findNodeInfo(n.kind)) {
            v.tooltip = info->description;
            v.latent = info->latent;
        } else if (!n.fn.empty()) {
            v.tooltip = n.fn;
        }
        const bool child_ref = n.kind == "entity.child";  // pildora con el nombre del objeto
        if (child_ref) v.tooltip = "Objeto de este Blueprint: " + (n.fn.empty() ? std::string("(sin ruta)") : n.fn);
        if (const auto it = messages.find(n.id); it != messages.end()) v.error = it->second;
        if (const auto it = warnings.find(n.id); it != warnings.end()) v.warning = it->second;
        if (!n.comment.empty()) v.bubble = n.comment;
        const bool compact = look == vscript::NodeLook::Compact;
        const bool reroute = look == vscript::NodeLook::Reroute;
        if (reroute) v.width = 32.0f;
        else if (compact) v.width = 120.0f;
        else v.width = std::max(170.0f, 70.0f + 7.0f * static_cast<float>(v.title.size()));

        for (std::size_t k = 0; k < n.inputs.size(); ++k) {
            const vscript::Pin& p = n.inputs[k];
            const PinType type = vscript::nodePinType(g, n, false, static_cast<int>(k));
            nodegraph::PinView pin;
            pin.label = p.name;
            pin.color = vsPinColor(type);
            pin.exec = type == PinType::Exec;
            pin.shape = pin.exec ? nodegraph::PinShape::Exec : nodegraph::PinShape::Circle;
            pin.type = static_cast<int>(type);
            pin.connected = linked_in.count({n.id, static_cast<int>(k)}) != 0;
            pin.inline_value = !pin.exec && !reroute;
            pin.hide_label = compact || reroute;
            pin.tooltip = vscript::pinTypeLabel(type);
            if (!pin.exec) {
                const std::string* value = debugValue(n.id, k, false);
                if (value != nullptr) pin.tooltip += " = " + *value;
                pin.watch = watchText(n.id, k, false, value);
            }
            v.inputs.push_back(std::move(pin));
        }
        for (std::size_t k = 0; k < n.outputs.size(); ++k) {
            const vscript::Pin& p = n.outputs[k];
            const PinType type = vscript::nodePinType(g, n, true, static_cast<int>(k));
            nodegraph::PinView pin;
            pin.label = p.name;
            pin.color = vsPinColor(type);
            pin.exec = type == PinType::Exec;
            pin.shape = pin.exec ? nodegraph::PinShape::Exec : nodegraph::PinShape::Circle;
            pin.type = static_cast<int>(type);
            pin.connected = linked_out.count({n.id, static_cast<int>(k)}) != 0;
            pin.hide_label = compact || reroute || child_ref;
            pin.tooltip = vscript::pinTypeLabel(type);
            if (!pin.exec) {
                const std::string* value = debugValue(n.id, k, true);
                if (value != nullptr) pin.tooltip += " = " + *value;
                pin.watch = watchText(n.id, k, true, value);
            }
            v.outputs.push_back(std::move(pin));
        }
        if (n.kind == "fn.call") v.subtitle = n.pure ? "Funcion pura" : "Funcion";
        else if (n.kind == "macro.call") v.subtitle = "Macro";
        else if (n.kind == "flow.timeline") v.subtitle = std::to_string(n.timeline.tracks.size()) + " pista(s)";
        v.highlight = recency(n.id);
        out.nodes.push_back(std::move(v));
    }

    out.links.reserve(g.links.size());
    for (std::size_t i = 0; i < g.links.size(); ++i) {
        const vscript::Link& l = g.links[i];
        if (!shown.contains(l.from_node) || !shown.contains(l.to_node)) continue;
        nodegraph::LinkView lv;
        lv.id = static_cast<int>(i) + 1;
        lv.from_node = l.from_node;
        lv.from_pin = l.from_pin;
        lv.to_node = l.to_node;
        lv.to_pin = l.to_pin;
        const vscript::Node* from = g.find(l.from_node);
        const PinType type = from != nullptr && validPin(*from, l.from_pin, true) ? vscript::nodePinType(g, *from, true, l.from_pin)
                                                                                 : PinType::Any;
        lv.exec = type == PinType::Exec;
        lv.color = vsPinColor(type);
        if (lv.exec) lv.flow = linkFlow(l);
        out.links.push_back(lv);
    }
}

// --- Operaciones sobre el grafo ------------------------------------------------------------

// Enlaza; si solo fallan los tipos y hay conversion, la inserta en medio.
bool connectOrConvert(Document& doc, const Host& host, int from_node, int from_pin, int to_node, int to_pin) {
    vscript::Graph& g = doc.graph;
    std::string error;
    if (g.connect(from_node, from_pin, to_node, to_pin, &error)) return true;
    const vscript::Node* a = g.find(from_node);
    const vscript::Node* b = g.find(to_node);
    if (a != nullptr && b != nullptr && a != b && validPin(*a, from_pin, true) && validPin(*b, to_pin, false)) {
        const PinType from = vscript::nodePinType(g, *a, true, from_pin);
        const PinType to = vscript::nodePinType(g, *b, false, to_pin);
        if (from != PinType::Exec && to != PinType::Exec) {
            if (const std::optional<std::string> kind = vscript::conversionKind(from, to);
                kind && vscript::findNodeInfo(*kind) != nullptr) {
                const core::Vec2 at{(a->position.x + b->position.x) * 0.5f, (a->position.y + b->position.y) * 0.5f + 20.0f};
                const int id = g.add(vscript::makeNode(*kind, at)).id;  // a y b ya no valen (vector)
                const vscript::Node* c = g.find(id);
                int in = -1, out = -1;
                for (std::size_t k = 0; c != nullptr && k < c->inputs.size() && in < 0; ++k) {
                    if (c->inputs[k].type != PinType::Exec && vscript::pinTypesCompatible(from, c->inputs[k].type)) in = static_cast<int>(k);
                }
                for (std::size_t k = 0; c != nullptr && k < c->outputs.size() && out < 0; ++k) {
                    if (c->outputs[k].type != PinType::Exec && vscript::pinTypesCompatible(c->outputs[k].type, to)) out = static_cast<int>(k);
                }
                if (in >= 0 && out >= 0 && g.connect(from_node, from_pin, id, in) && g.connect(id, out, to_node, to_pin)) return true;
                g.removeNode(id);
            }
        }
    }
    notify(host, "No se pueden enlazar", error);
    return false;
}

// Tras crear un nodo desde un pin arrastrado: enlaza el primer pin que encaje
// (primero el mismo tipo, despues uno compatible).
void autoConnect(vscript::Graph& g, const nodegraph::PinRef& from, int id) {
    const vscript::Node* src = g.find(from.node);
    const vscript::Node* dst = g.find(id);
    if (src == nullptr || dst == nullptr || !validPin(*src, from.pin, from.output)) return;
    const PinType type = vscript::nodePinType(g, *src, from.output, from.pin);
    const std::size_t count = from.output ? dst->inputs.size() : dst->outputs.size();
    for (int pass = 0; pass < 2; ++pass) {
        for (std::size_t k = 0; k < count; ++k) {
            const PinType other = vscript::nodePinType(g, *dst, !from.output, static_cast<int>(k));
            const bool match = pass == 0 ? other == type
                                         : (from.output ? vscript::pinTypesCompatible(type, other) : vscript::pinTypesCompatible(other, type));
            if (!match) continue;
            const bool ok = from.output ? g.connect(from.node, from.pin, id, static_cast<int>(k))
                                        : g.connect(id, static_cast<int>(k), from.node, from.pin);
            if (ok) return;
        }
    }
}

// Caja de comentario alrededor de la seleccion (al principio: se dibuja detras).
bool commentAround(Document& doc) {
    vscript::Graph& g = doc.graph;
    float min_x = 1e30f, min_y = 1e30f, max_x = -1e30f, max_y = -1e30f;
    for (const int sel : doc.canvas.selection) {
        if (const vscript::Node* s = g.find(sel)) {
            min_x = std::min(min_x, s->position.x);
            min_y = std::min(min_y, s->position.y);
            max_x = std::max(max_x, s->position.x + 220.0f);
            max_y = std::max(max_y, s->position.y + 120.0f);
        }
    }
    if (min_x >= max_x) return false;
    vscript::Node c = vscript::makeNode("comment", core::Vec2{min_x - 20.0f, min_y - 40.0f});
    c.size = core::Vec2{max_x - min_x + 40.0f, max_y - min_y + 60.0f};
    c.comment = "Comentario";
    g.add(std::move(c));
    std::rotate(g.nodes.begin(), g.nodes.end() - 1, g.nodes.end());
    return true;
}

void copySelection(Document& doc) {
    doc.clipboard.clear();
    doc.clipboard_links.clear();
    for (const int id : doc.canvas.selection) {
        const vscript::Node* n = doc.graph.find(id);
        if (n != nullptr && !isEntryKind(n->kind)) doc.clipboard.push_back(*n);
    }
    for (const vscript::Link& l : doc.graph.links) {
        if (doc.canvas.selection.contains(l.from_node) && doc.canvas.selection.contains(l.to_node)) doc.clipboard_links.push_back(l);
    }
}

void pasteClipboard(Document& doc, core::Vec2 at, bool offset_from_original) {
    vscript::Graph& g = doc.graph;
    float min_x = 1e30f, min_y = 1e30f;
    for (const vscript::Node& n : doc.clipboard) {
        min_x = std::min(min_x, n.position.x);
        min_y = std::min(min_y, n.position.y);
    }
    if (offset_from_original) at = core::Vec2{min_x + 40.0f, min_y + 40.0f};
    std::map<int, int> remap;
    doc.canvas.selection.clear();
    for (vscript::Node n : doc.clipboard) {
        const int old = n.id;
        n.position = core::Vec2{at.x + (n.position.x - min_x), at.y + (n.position.y - min_y)};
        n.breakpoint = false;
        n.graph = doc.active;  // se pega en el grafo que se ve
        const int id = g.add(std::move(n)).id;
        remap[old] = id;
        doc.canvas.selection.insert(id);
    }
    for (const vscript::Link& l : doc.clipboard_links) {
        if (remap.count(l.from_node) && remap.count(l.to_node)) g.connect(remap[l.from_node], l.from_pin, remap[l.to_node], l.to_pin);
    }
}

void eraseNodes(Document& doc, const std::vector<int>& ids) {
    for (const int id : ids) {
        // La entrada de una funcion / macro se va solo con ella (Mi Blueprint > Borrar).
        if (const vscript::Node* n = doc.graph.find(id); n != nullptr && isEntryKind(n->kind)) continue;
        doc.graph.removeNode(id);
        doc.canvas.selection.erase(id);
        if (doc.details_node == id) doc.details_node = 0;
    }
}

void eraseLink(vscript::Graph& g, int from_node, int from_pin, int to_node, int to_pin) {
    const vscript::Link target{from_node, from_pin, to_node, to_pin};
    // remove_if: el std::remove vectorizado de MSVC no admite Link (static_assert de tamano).
    g.links.erase(std::remove_if(g.links.begin(), g.links.end(), [&](const vscript::Link& l) { return l == target; }),
                  g.links.end());
}

bool breakPinLinks(vscript::Graph& g, const nodegraph::PinRef& pin) {
    const std::size_t before = g.links.size();
    g.links.erase(std::remove_if(g.links.begin(), g.links.end(),
                                 [&](const vscript::Link& l) {
                                     return pin.output ? (l.from_node == pin.node && l.from_pin == pin.pin)
                                                       : (l.to_node == pin.node && l.to_pin == pin.pin);
                                 }),
                  g.links.end());
    return g.links.size() != before;
}

// Ctrl+arrastrar: pasa todos los enlaces de un pin a otro del mismo lado.
bool relinkPin(vscript::Graph& g, const nodegraph::PinRef& from, const nodegraph::PinRef& to) {
    if (from.output != to.output) return false;
    std::vector<vscript::Link> moving;
    for (const vscript::Link& l : g.links) {
        if (from.output ? (l.from_node == from.node && l.from_pin == from.pin) : (l.to_node == from.node && l.to_pin == from.pin)) {
            moving.push_back(l);
        }
    }
    if (moving.empty()) return false;
    breakPinLinks(g, from);
    for (const vscript::Link& l : moving) {
        const bool ok = from.output ? g.connect(to.node, to.pin, l.to_node, l.to_pin) : g.connect(l.from_node, l.from_pin, to.node, to.pin);
        if (!ok) g.links.push_back(l);  // no encaja: se queda donde estaba
    }
    return true;
}

// Doble clic en un cable: un Reroute en medio.
bool insertReroute(Document& doc, const Host& host, const nodegraph::LinkView& lv, core::Vec2 at) {
    vscript::Graph& g = doc.graph;
    if (lv.exec) {
        notify(host, "Reroute", "Los cables de ejecución no admiten Reroute todavía");
        return false;
    }
    if (vscript::findNodeInfo("util.reroute") == nullptr) return false;
    eraseLink(g, lv.from_node, lv.from_pin, lv.to_node, lv.to_pin);
    const int id = g.add(vscript::makeNode("util.reroute", core::Vec2{at.x - 12.0f, at.y - 12.0f})).id;
    const bool ok = g.connect(lv.from_node, lv.from_pin, id, 0) && g.connect(id, 0, lv.to_node, lv.to_pin);
    if (!ok) {
        g.removeNode(id);
        g.links.push_back(vscript::Link{lv.from_node, lv.from_pin, lv.to_node, lv.to_pin});
        return false;
    }
    doc.canvas.select(id);
    return true;
}

// "Actualizar nodo": un nodo de API guardado con pines viejos (Any) toma los
// de la plantilla actual; conserva valores y enlaces por nombre de pin.
bool refreshApiNode(Document& doc, int id) {
    vscript::Graph& g = doc.graph;
    vscript::Node* n = g.find(id);
    if (n == nullptr || n->fn.empty()) return false;
    const vscript::Node* tmpl = nullptr;
    for (const vscript::ApiNodeTemplate& t : vscript::apiNodeTemplates(scripting::ScriptSystem::apiRegistry())) {
        if (t.node.kind == n->kind && t.node.fn == n->fn) {
            tmpl = &t.node;
            break;
        }
    }
    if (tmpl == nullptr) return false;
    const vscript::Node old = *n;
    n->inputs = tmpl->inputs;
    n->outputs = tmpl->outputs;
    n->pure = tmpl->pure;
    for (vscript::Pin& p : n->inputs) {
        const int k = old.input(p.name);
        if (k >= 0) p.value = old.inputs[static_cast<std::size_t>(k)].value;
    }
    std::vector<vscript::Link> kept;
    for (vscript::Link l : g.links) {
        if (l.to_node == id) {
            if (!validPin(old, l.to_pin, false)) continue;
            const int k = n->input(old.inputs[static_cast<std::size_t>(l.to_pin)].name);
            if (k < 0) continue;
            l.to_pin = k;
        }
        if (l.from_node == id) {
            if (!validPin(old, l.from_pin, true)) continue;
            const int k = n->output(old.outputs[static_cast<std::size_t>(l.from_pin)].name);
            if (k < 0) continue;
            l.from_pin = k;
        }
        kept.push_back(l);
    }
    g.links = std::move(kept);
    return true;
}

// Entrada -> variable nueva con su tipo y valor, y un Get enlazado.
bool promoteToVariable(Document& doc, const nodegraph::PinRef& pin) {
    vscript::Graph& g = doc.graph;
    const vscript::Node* n = g.find(pin.node);
    if (n == nullptr || pin.output || !validPin(*n, pin.pin, false)) return false;
    const PinType type = vscript::nodePinType(g, *n, false, pin.pin);
    if (type == PinType::Exec) return false;
    const vscript::Pin& p = n->inputs[static_cast<std::size_t>(pin.pin)];
    std::string base = p.name.empty() ? std::string("Variable") : p.name;
    std::replace(base.begin(), base.end(), ' ', '_');
    std::string name = base;
    for (int i = 2; g.findVariable(name) != nullptr; ++i) name = base + std::to_string(i);
    g.variables.push_back(vscript::Variable{name, type, p.value, false, {}});
    const core::Vec2 at{n->position.x - 220.0f, n->position.y + 40.0f * static_cast<float>(pin.pin)};
    vscript::Node get = vscript::makeNode("var.get", at);
    get.fn = name;
    const int id = g.add(std::move(get)).id;  // n ya no vale
    g.connect(id, 0, pin.node, pin.pin);
    doc.details_variable = name;
    doc.details_node = 0;
    return true;
}

int addVariableNode(Document& doc, const std::string& variable, bool set, core::Vec2 at) {
    vscript::Node n = vscript::makeNode(set ? "var.set" : "var.get", at);
    n.fn = variable;
    const int id = doc.graph.add(std::move(n)).id;
    doc.canvas.select(id);
    return id;
}

// Referencia a un objeto soltado (como arrastrar un componente en Unreal):
// la raiz = Self; lo demas = su ruta desde la raiz, que vale en cada instancia.
int addEntityRefNode(Document& doc, const EntityDrop& drop, core::Vec2 at) {
    vscript::Node n = vscript::makeNode(drop.root ? "entity.self" : "entity.child", at);
    if (!drop.root) n.fn = drop.path;
    const int id = doc.graph.add(std::move(n)).id;
    doc.canvas.select(id);
    return id;
}

// Ctrl al soltar: la referencia + Get Component con ese componente, enlazados.
int addComponentGetNode(Document& doc, const EntityDrop& drop, const std::string& component, core::Vec2 at) {
    const int ref = addEntityRefNode(doc, drop, at);
    vscript::Node get = vscript::makeNode("component.get", core::Vec2{at.x + 200.0f, at.y});
    if (const int k = get.input("Componente"); k >= 0) get.inputs[static_cast<std::size_t>(k)].value = component;
    const int id = doc.graph.add(std::move(get)).id;
    doc.graph.connect(ref, 0, id, 0);
    doc.canvas.select(id);
    return id;
}

const char* spawnKind(ImGuiKey key) {
    switch (key) {
        case ImGuiKey_B: return "flow.branch";
        case ImGuiKey_S: return "flow.sequence";
        case ImGuiKey_D: return "flow.delay";
        case ImGuiKey_O: return "flow.do_once";
        case ImGuiKey_G: return "flow.gate";
        case ImGuiKey_N: return "flow.do_n";
        case ImGuiKey_F: return "flow.flip_flop";
        default: return nullptr;
    }
}

// Icono de estado de compilacion (circulo con ?, check, ! o x).
void drawStatusIcon(ImDrawList* draw, ImVec2 c, float r, Status s) {
    ImU32 color = theme::kYellow;
    if (s == Status::Good) color = theme::kOk;
    else if (s == Status::Errors) color = theme::kRed;
    draw->AddCircleFilled(c, r, color);
    const ImU32 ink = IM_COL32(20, 20, 20, 255);
    const float t = std::max(1.5f, r * 0.22f);
    switch (s) {
        case Status::Good:
            draw->AddLine(ImVec2(c.x - r * 0.45f, c.y), ImVec2(c.x - r * 0.1f, c.y + r * 0.4f), ink, t);
            draw->AddLine(ImVec2(c.x - r * 0.1f, c.y + r * 0.4f), ImVec2(c.x + r * 0.5f, c.y - r * 0.4f), ink, t);
            break;
        case Status::Errors:
            draw->AddLine(ImVec2(c.x - r * 0.4f, c.y - r * 0.4f), ImVec2(c.x + r * 0.4f, c.y + r * 0.4f), ink, t);
            draw->AddLine(ImVec2(c.x + r * 0.4f, c.y - r * 0.4f), ImVec2(c.x - r * 0.4f, c.y + r * 0.4f), ink, t);
            break;
        case Status::Warnings:
            draw->AddLine(ImVec2(c.x, c.y - r * 0.55f), ImVec2(c.x, c.y + r * 0.15f), ink, t);
            draw->AddCircleFilled(ImVec2(c.x, c.y + r * 0.45f), t * 0.6f, ink);
            break;
        default: {
            const ImVec2 s2 = ImGui::CalcTextSize("?");
            draw->AddText(ImVec2(c.x - s2.x * 0.5f, c.y - s2.y * 0.5f), ink, "?");
            break;
        }
    }
}

const char* statusText(Status s) {
    switch (s) {
        case Status::Unknown: return "Sin compilar";
        case Status::Dirty: return "Hay cambios sin compilar";
        case Status::Good: return "Compilado sin errores";
        case Status::Warnings: return "Compilado con avisos";
        case Status::Errors: return "Errores de compilación";
    }
    return "";
}

// Tabla de dos columnas (etiqueta | control) de Detalles.
bool beginDetailsTable(const char* id) {
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp)) return false;
    ImGui::TableSetupColumn("##l", ImGuiTableColumnFlags_WidthFixed, 110.0f);
    ImGui::TableSetupColumn("##v", ImGuiTableColumnFlags_WidthStretch);
    return true;
}

void detailsLabel(const char* label) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(colorVec(theme::kLabel), "%s", label);
    ImGui::TableSetColumnIndex(1);
}

// Menu del nodo: pines que anade el usuario (NodeInfo::dynamic_inputs/outputs y
// Sequence). graphFromJson los conserva al cargar.
bool editDynamicPins(vscript::Graph& g, vscript::Node& n) {
    const vscript::NodeInfo* info = vscript::findNodeInfo(n.kind);
    if (info == nullptr) return false;
    const bool outputs = info->dynamic_outputs || n.kind == "flow.sequence";
    const bool inputs = info->dynamic_inputs;
    if (!outputs && !inputs) return false;
    bool changed = false;
    ImGui::Separator();
    if (n.kind == "str.format") {
        // Una entrada por cada {nombre} del Formato (valores y enlaces por nombre).
        if (ImGui::MenuItem("Actualizar entradas del formato") && !n.inputs.empty()) {
            const vscript::Node old = n;
            n.inputs.resize(1);
            for (const std::string& name : vscript::formatTextArguments(old.inputs[0].value)) {
                vscript::Pin p{name, PinType::Any, {}, {}};
                if (const int k = old.input(name); k > 0) p.value = old.inputs[static_cast<std::size_t>(k)].value;
                n.inputs.push_back(std::move(p));
            }
            std::vector<vscript::Link> kept;
            for (vscript::Link l : g.links) {
                if (l.to_node == n.id) {
                    if (!validPin(old, l.to_pin, false)) continue;
                    const int k = n.input(old.inputs[static_cast<std::size_t>(l.to_pin)].name);
                    if (k < 0) continue;
                    l.to_pin = k;
                }
                kept.push_back(l);
            }
            g.links = std::move(kept);
            changed = true;
        }
        return changed;
    }
    std::vector<vscript::Pin>& pins = outputs ? n.outputs : n.inputs;
    const std::size_t minimum = outputs ? info->outputs.size() : info->inputs.size();
    if (ImGui::MenuItem("Añadir pin")) {
        const std::size_t count = pins.size();
        vscript::Pin p;
        p.type = outputs ? PinType::Exec : PinType::Any;
        if (n.kind == "flow.sequence") p.name = "Then " + std::to_string(count);
        else if (n.kind == "flow.switch_int") p.name = std::to_string(count == 0 ? 0 : count - 1);  // Default va primero
        else if (n.kind == "flow.switch_string") p.name = "Caso " + std::to_string(count);
        else if (n.kind == "flow.multigate") p.name = "Salida " + std::to_string(count);
        else if (n.kind == "flow.select_int") p.name = "Opcion " + std::to_string(count == 0 ? 0 : count - 1);  // Indice va primero
        else if (n.kind == "array.make") p.name = std::to_string(count + 1);
        else p.name = std::to_string(count);
        while ((outputs ? n.output(p.name) : n.input(p.name)) >= 0) p.name += "_";
        pins.push_back(std::move(p));
        changed = true;
    }
    if (pins.size() > minimum && ImGui::MenuItem("Quitar el ultimo pin")) {
        const nodegraph::PinRef last{n.id, static_cast<int>(pins.size()) - 1, outputs};
        breakPinLinks(g, last);
        pins.pop_back();
        changed = true;
    }
    if (n.kind == "flow.switch_string") {
        // Los casos se llaman como el texto que eligen (Default no se renombra).
        for (std::size_t k = 1; k < n.outputs.size(); ++k) {
            ImGui::PushID(static_cast<int>(k));
            ImGui::SetNextItemWidth(160.0f);
            std::string name = n.outputs[k].name;
            if (ImGui::InputText("##case", &name, ImGuiInputTextFlags_EnterReturnsTrue) && !name.empty() &&
                n.output(name) < 0) {
                n.outputs[k].name = name;
                changed = true;
            }
            ImGui::PopID();
        }
    }
    return changed;
}

// --- Fase 2: funciones, macros, Construction Script y Timeline -----------------------------

// Una llamada a una funcion / macro en el grafo que se ve.
int addSubgraphCall(Document& doc, const vscript::Subgraph& s, core::Vec2 at) {
    vscript::Node n;
    n.kind = s.kind == vscript::SubgraphKind::Macro ? "macro.call" : "fn.call";
    n.fn = s.name;
    n.position = at;
    n.pure = s.kind == vscript::SubgraphKind::Function && s.pure;
    vscript::subgraphNodePins(s, n.kind, n.inputs, n.outputs);
    const int id = doc.graph.add(std::move(n)).id;
    doc.canvas.select(id);
    return id;
}

// Lo que el buscador ofrece en cada grafo: los eventos solo en el Event Graph
// (Construction Script en el suyo), el Timeline solo en el Event Graph y nada
// que espere (Delay) en una funcion ni en el Construction Script.
bool paletteFits(const Document& doc, const vscript::Node& node) {
    const vscript::NodeInfo* info = vscript::findNodeInfo(node.kind);
    if (info == nullptr) return true;
    const bool construction = doc.active == vscript::kConstructionGraph;
    const bool function = doc.active.rfind("fn:", 0) == 0;
    if (node.kind == "event.construction") return construction;
    if (info->event || node.kind == "flow.timeline") return doc.active.empty();
    if (info->latent && (construction || function)) return false;
    return true;
}

// Cambia el nombre de un pin de la firma tambien en los nodos de la funcion /
// macro (asi syncSubgraph conserva sus enlaces).
void applyPinRenames(vscript::Graph& g, const vscript::Subgraph& s, bool inputs_side,
                     const std::vector<std::pair<std::string, std::string>>& renames) {
    if (renames.empty()) return;
    const bool macro = s.kind == vscript::SubgraphKind::Macro;
    for (vscript::Node& n : g.nodes) {
        if (!vscript::isSubgraphNode(n.kind) || (n.kind.rfind("macro.", 0) == 0) != macro || n.fn != s.name) continue;
        const bool entry = n.kind == "fn.entry" || n.kind == "macro.entry";
        const bool exit = n.kind == "fn.return" || n.kind == "macro.exit";
        const bool call = n.kind == "fn.call" || n.kind == "macro.call";
        std::vector<vscript::Pin>* pins = nullptr;
        if (inputs_side) pins = entry ? &n.outputs : (call ? &n.inputs : nullptr);
        else pins = exit ? &n.inputs : (call ? &n.outputs : nullptr);
        if (pins == nullptr) continue;
        for (const auto& [from, to] : renames) {
            for (vscript::Pin& p : *pins) {
                if (p.name == from) p.name = to;
            }
        }
    }
}

// Lista de pines de una firma (nombre, tipo, valor por defecto, subir, quitar).
// `exec`: una macro tambien tiene pines de ejecucion.
bool drawPinList(const char* label, std::vector<vscript::Pin>& pins, bool exec, const Host& host,
                 std::vector<std::pair<std::string, std::string>>& renames) {
    bool changed = false;
    ImGui::SeparatorText(label);
    ImGui::PushID(label);
    int remove = -1;
    int up = -1;
    for (std::size_t i = 0; i < pins.size(); ++i) {
        vscript::Pin& p = pins[i];
        if (!exec && p.type == PinType::Exec) continue;  // una funcion no usa exec en su firma
        ImGui::PushID(static_cast<int>(i));
        const ImVec2 at = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(at.x + 6.0f, at.y + ImGui::GetFrameHeight() * 0.5f), 4.5f, vsPinColor(p.type));
        ImGui::SetCursorScreenPos(ImVec2(at.x + 16.0f, at.y));
        std::string name = p.name;
        ImGui::SetNextItemWidth(110.0f);
        if (ImGui::InputText("##pin_name", &name, ImGuiInputTextFlags_EnterReturnsTrue) && !name.empty() && name != p.name) {
            const bool used = std::any_of(pins.begin(), pins.end(), [&](const vscript::Pin& other) { return other.name == name; });
            if (used) {
                notify(host, "Renombrar", "Ya hay un pin con ese nombre");
            } else {
                renames.emplace_back(p.name, name);
                p.name = name;
                changed = true;
            }
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100.0f);
        if (ImGui::BeginCombo("##pin_type", vscript::pinTypeLabel(p.type))) {
            for (int t = exec ? 0 : 1; t < vscript::kPinTypeCount; ++t) {
                const PinType type = static_cast<PinType>(t);
                if (ImGui::Selectable(vscript::pinTypeLabel(type), p.type == type) && p.type != type) {
                    p.type = type;
                    p.value = defaultValueFor(type);
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("^") && i > 0) up = static_cast<int>(i);
        ImGui::SetItemTooltip("Subir");
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) remove = static_cast<int>(i);
        ImGui::SetItemTooltip("Quitar (sus enlaces se van)");
        if (p.type != PinType::Exec) {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 16.0f);
            ImGui::TextDisabled("por defecto");
            ImGui::SameLine();
            if (drawPinValueEditor(p, std::max(60.0f, ImGui::GetContentRegionAvail().x), true, host)) changed = true;
        }
        ImGui::PopID();
    }
    if (up > 0) {
        std::swap(pins[static_cast<std::size_t>(up)], pins[static_cast<std::size_t>(up) - 1]);
        changed = true;
    }
    if (remove >= 0) {
        pins.erase(pins.begin() + remove);
        changed = true;
    }
    if (ImGui::Button("Añadir pin")) {
        std::string name = "Valor";
        for (int i = 2; std::any_of(pins.begin(), pins.end(), [&](const vscript::Pin& p) { return p.name == name; }); ++i) {
            name = "Valor" + std::to_string(i);
        }
        pins.push_back(vscript::Pin{name, PinType::Float, "0", {}});
        changed = true;
    }
    ImGui::PopID();
    return changed;
}

// Firma de una funcion / macro en Detalles.
bool drawSignature(Document& doc, const Host& host, vscript::Subgraph& s) {
    vscript::Graph& g = doc.graph;
    bool changed = false;
    bool signature = false;
    const bool macro = s.kind == vscript::SubgraphKind::Macro;
    ImGui::SeparatorText(macro ? "Macro" : "Función");
    if (beginDetailsTable("##vs_fn_details")) {
        detailsLabel("Nombre");
        std::string name = s.name;
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::InputText("##fn_name", &name, ImGuiInputTextFlags_EnterReturnsTrue) && !name.empty() && name != s.name) {
            const std::string old_key = s.key();
            if (name.find(':') == std::string::npos && g.renameSubgraph(old_key, name)) {
                const std::string new_key = s.key();
                if (doc.active == old_key) doc.active = new_key;
                doc.details_function = new_key;
                if (const auto view = doc.views.find(old_key); view != doc.views.end()) {
                    doc.views[new_key] = view->second;
                    doc.views.erase(old_key);
                }
                changed = true;
            } else {
                notify(host, "Renombrar", macro ? "Ya hay una macro con ese nombre (o lleva ':')" : "Ya hay una función con ese nombre (o lleva ':')");
            }
        }
        detailsLabel("Descripción");
        ImGui::SetNextItemWidth(-1.0f);
        changed |= ImGui::InputText("##fn_desc", &s.description);
        if (!macro) {
            detailsLabel("Pura");
            if (ImGui::Checkbox("##fn_pure", &s.pure)) signature = true;
            ImGui::SetItemTooltip("Sus llamadas no tienen pines de ejecución (como un Get):\nse calcula cada vez que se usa su valor");
        }
        detailsLabel("Usos");
        int uses = 0;
        for (const vscript::Node& n : g.nodes) {
            if ((n.kind == (macro ? "macro.call" : "fn.call")) && n.fn == s.name) ++uses;
        }
        ImGui::Text("%d llamada(s)", uses);
        ImGui::EndTable();
    }
    std::vector<std::pair<std::string, std::string>> in_renames;
    std::vector<std::pair<std::string, std::string>> out_renames;
    signature |= drawPinList(macro ? "Entradas" : "Parámetros", s.inputs, macro, host, in_renames);
    signature |= drawPinList(macro ? "Salidas" : "Resultados", s.outputs, macro, host, out_renames);
    if (macro && std::none_of(s.inputs.begin(), s.inputs.end(), [](const vscript::Pin& p) { return p.type == PinType::Exec; })) {
        ImGui::TextColored(colorVec(theme::kYellow), "Una macro necesita al menos una entrada de ejecución");
    }
    if (signature) {
        applyPinRenames(g, s, true, in_renames);
        applyPinRenames(g, s, false, out_renames);
        g.syncSubgraph(s.key());
        changed = true;
    }
    ImGui::Spacing();
    if (doc.active != s.key() && ImGui::Button(macro ? "Abrir la macro" : "Abrir la función")) showGraph(doc, s.key());
    ImGui::SameLine();
    if (ImGui::Button("Añadir llamada")) {
        // En el grafo que se ve (si es el suyo y es una macro, compileGraph lo avisa).
        addSubgraphCall(doc, s, doc.canvas.viewCenter());
        changed = true;
    }
    return changed;
}

// Curva de una pista (cada componente de un color) con sus claves y, en Play, el tiempo actual.
void drawCurve(const vscript::TimelineTrack& track, float length, double playhead) {
    const float w = std::max(60.0f, ImGui::GetContentRegionAvail().x);
    const float h = 70.0f;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + w, p0.y + h);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(p0, p1, IM_COL32(28, 28, 32, 255), 3.0f);
    const int components = track.type == PinType::Vector ? 3 : 1;
    constexpr int kSamples = 64;
    const auto component = [](const core::Vec3& v, int c) { return c == 0 ? v.x : (c == 1 ? v.y : v.z); };
    float lo = 1e30f, hi = -1e30f;
    for (int s = 0; s < kSamples; ++s) {
        const core::Vec3 v = vscript::evaluateTrack(track, length * static_cast<float>(s) / (kSamples - 1));
        for (int c = 0; c < components; ++c) {
            lo = std::min(lo, component(v, c));
            hi = std::max(hi, component(v, c));
        }
    }
    if (hi - lo < 1e-4f) {
        lo -= 0.5f;
        hi += 0.5f;
    }
    const auto toY = [&](float v) { return p1.y - 4.0f - (v - lo) / (hi - lo) * (h - 8.0f); };
    const ImU32 colors[3] = {theme::kAxisX, theme::kAxisY, theme::kAxisZ};
    for (int c = 0; c < components; ++c) {
        ImVec2 points[kSamples];
        for (int s = 0; s < kSamples; ++s) {
            const float t = static_cast<float>(s) / (kSamples - 1);
            points[s] = ImVec2(p0.x + w * t, toY(component(vscript::evaluateTrack(track, length * t), c)));
        }
        draw->AddPolyline(points, kSamples, components == 1 ? IM_COL32(240, 200, 80, 255) : colors[c], ImDrawFlags_None, 1.5f);
    }
    for (const vscript::TimelineKey& k : track.keys) {
        if (length <= 0.0f) break;
        const float x = p0.x + w * std::clamp(k.time / length, 0.0f, 1.0f);
        draw->AddCircleFilled(ImVec2(x, toY(k.value.x)), 3.5f, IM_COL32(235, 235, 235, 255));
    }
    if (playhead >= 0.0 && length > 0.0f) {
        const float x = p0.x + w * std::clamp(static_cast<float>(playhead) / length, 0.0f, 1.0f);
        draw->AddLine(ImVec2(x, p0.y), ImVec2(x, p1.y), IM_COL32(230, 60, 60, 255), 1.5f);
    }
    ImGui::Dummy(ImVec2(w, h));
}

// Detalles de un Timeline: duracion, bucle, autoplay y sus pistas con sus claves.
bool drawTimelineEditor(vscript::Graph& g, vscript::Node& n, const scripting::ScriptSystem::VisualScriptDebug* debug) {
    vscript::Timeline& t = n.timeline;
    bool changed = false;
    bool pins = false;
    ImGui::SeparatorText("Timeline");
    if (beginDetailsTable("##vs_timeline")) {
        detailsLabel("Duración (s)");
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::DragFloat("##length", &t.length, 0.05f, 0.01f, 3600.0f, "%.3g")) {
            t.length = std::max(0.01f, t.length);
            changed = true;
        }
        detailsLabel("Bucle");
        changed |= ImGui::Checkbox("##loop", &t.loop);
        detailsLabel("Autoplay");
        changed |= ImGui::Checkbox("##autoplay", &t.autoplay);
        ImGui::SetItemTooltip("Empieza solo al empezar Play");
        ImGui::EndTable();
    }
    double playhead = -1.0;
    if (debug != nullptr) {
        if (const auto it = debug->values.find(debugKey(n.id, 3)); it != debug->values.end()) playhead = std::atof(it->second.c_str());
    }
    static const char* const kInterp[] = {"Lineal", "Constante", "Suave"};
    int remove_track = -1;
    for (std::size_t i = 0; i < t.tracks.size(); ++i) {
        vscript::TimelineTrack& track = t.tracks[i];
        ImGui::PushID(static_cast<int>(i));
        ImGui::Separator();
        std::string name = track.name;
        ImGui::SetNextItemWidth(130.0f);
        if (ImGui::InputText("##track_name", &name, ImGuiInputTextFlags_EnterReturnsTrue) && !name.empty() && name != track.name) {
            const bool used = n.output(name) >= 0;
            if (!used) {
                // La salida se renombra antes (syncTimeline conserva sus enlaces por nombre).
                if (const int k = n.output(track.name); k >= vscript::kTimelineFixedOutputs) n.outputs[static_cast<std::size_t>(k)].name = name;
                track.name = name;
                pins = true;
            }
        }
        ImGui::SameLine();
        int type = track.type == PinType::Vector ? 1 : 0;
        ImGui::SetNextItemWidth(90.0f);
        if (ImGui::Combo("##track_type", &type, "Float\0Vector\0")) {
            track.type = type == 1 ? PinType::Vector : PinType::Float;
            pins = true;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) remove_track = static_cast<int>(i);
        ImGui::SetItemTooltip("Quitar la pista (y los enlaces de su salida)");
        drawCurve(track, t.length, playhead);
        int remove_key = -1;
        bool reorder = false;
        for (std::size_t k = 0; k < track.keys.size(); ++k) {
            vscript::TimelineKey& key = track.keys[k];
            ImGui::PushID(static_cast<int>(k));
            ImGui::SetNextItemWidth(60.0f);
            if (ImGui::DragFloat("##t", &key.time, 0.01f, 0.0f, t.length, "%.3g")) {
                key.time = std::clamp(key.time, 0.0f, t.length);
                changed = reorder = true;
            }
            ImGui::SetItemTooltip("Tiempo (s)");
            ImGui::SameLine();
            if (track.type == PinType::Vector) {
                float v[3] = {key.value.x, key.value.y, key.value.z};
                ImGui::SetNextItemWidth(150.0f);
                if (ImGui::DragFloat3("##v", v, 0.05f)) {
                    key.value = core::Vec3{v[0], v[1], v[2]};
                    changed = true;
                }
            } else {
                ImGui::SetNextItemWidth(70.0f);
                changed |= ImGui::DragFloat("##v", &key.value.x, 0.05f);
            }
            ImGui::SameLine();
            int interp = static_cast<int>(key.interp);
            ImGui::SetNextItemWidth(85.0f);
            if (ImGui::Combo("##i", &interp, kInterp, 3)) {
                key.interp = static_cast<vscript::CurveInterp>(interp);
                changed = true;
            }
            ImGui::SetItemTooltip("Como llega a la clave siguiente");
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) remove_key = static_cast<int>(k);
            ImGui::PopID();
        }
        if (remove_key >= 0) {
            track.keys.erase(track.keys.begin() + remove_key);
            changed = true;
        }
        if (ImGui::SmallButton("+ Clave")) {
            vscript::TimelineKey key;
            if (!track.keys.empty()) {
                key = track.keys.back();
                key.time = std::min(t.length, key.time + t.length * 0.25f);
            }
            track.keys.push_back(key);
            changed = reorder = true;
        }
        if (reorder) {
            std::stable_sort(track.keys.begin(), track.keys.end(),
                             [](const vscript::TimelineKey& a, const vscript::TimelineKey& b) { return a.time < b.time; });
        }
        ImGui::PopID();
    }
    if (remove_track >= 0) {
        t.tracks.erase(t.tracks.begin() + remove_track);
        pins = true;
    }
    if (ImGui::Button("Añadir pista")) {
        vscript::TimelineTrack track;
        track.name = "Pista";
        for (int i = 2; n.output(track.name) >= 0; ++i) track.name = "Pista" + std::to_string(i);
        track.keys = {vscript::TimelineKey{0.0f, core::Vec3{0.0f, 0.0f, 0.0f}, vscript::CurveInterp::Linear},
                      vscript::TimelineKey{t.length, core::Vec3{1.0f, 1.0f, 1.0f}, vscript::CurveInterp::Linear}};
        t.tracks.push_back(std::move(track));
        pins = true;
    }
    if (pins) g.syncTimeline(n.id);
    return changed || pins;
}

}  // namespace

// --- Grafos del documento --------------------------------------------------------------------

std::string graphLabel(const std::string& key) {
    if (key.empty()) return "Event Graph";
    if (key == vscript::kConstructionGraph) return "Construction Script";
    if (key.rfind("fn:", 0) == 0) return "Función: " + key.substr(3);
    if (key.rfind("macro:", 0) == 0) return "Macro: " + key.substr(6);
    return key;
}

void showGraph(Document& doc, const std::string& key) {
    vscript::Graph& g = doc.graph;
    if (!key.empty() && key != vscript::kConstructionGraph && g.findSubgraph(key) == nullptr) return;
    // El Construction Script empieza con su nodo (como Unreal).
    if (key == vscript::kConstructionGraph &&
        std::none_of(g.nodes.begin(), g.nodes.end(), [](const vscript::Node& n) { return n.kind == "event.construction"; })) {
        vscript::Node start = vscript::makeNode("event.construction", core::Vec2{0.0f, 0.0f});
        start.graph = vscript::kConstructionGraph;
        g.add(std::move(start));
        markChanged(doc);
    }
    if (key == doc.active) return;
    doc.views[doc.active] = {doc.canvas.pan, doc.canvas.zoom};
    doc.active = key;
    g.editing = key;
    doc.canvas.selection.clear();
    doc.canvas.link_selection.clear();
    doc.details_node = 0;
    doc.create_from.reset();
    doc.pin_menu.reset();
    if (const auto it = doc.views.find(key); it != doc.views.end()) {
        doc.canvas.pan = it->second.first;
        doc.canvas.zoom = it->second.second;
        return;
    }
    std::vector<nodegraph::NodeView> views;
    for (vscript::Node& n : g.nodes) {
        if (n.graph != key) continue;
        nodegraph::NodeView v;
        v.id = n.id;
        v.position = &n.position;
        views.push_back(v);
    }
    if (!views.empty()) doc.canvas.frame(views);
}

void focusNode(Document& doc, int id) {
    const vscript::Node* n = doc.graph.find(id);
    if (n == nullptr) return;
    const std::string graph = n->graph;  // showGraph puede anadir nodos (n deja de valer)
    if (graph != doc.active) showGraph(doc, graph);
    if (graph != doc.active) return;  // su grafo ya no existe
    doc.canvas.select(id);
    doc.canvas.frameNode(id);
    doc.details_node = id;
    doc.details_variable.clear();
    doc.details_function.clear();
}

// --- Colores ---------------------------------------------------------------------------------

ImU32 vsPinColor(vscript::PinType type) {
    switch (type) {
        case PinType::Exec: return IM_COL32(255, 255, 255, 255);
        case PinType::Any: return IM_COL32(165, 165, 165, 255);
        case PinType::Bool: return IM_COL32(200, 30, 30, 255);
        case PinType::Int: return IM_COL32(31, 227, 175, 255);
        case PinType::Float: return IM_COL32(160, 250, 70, 255);
        case PinType::String: return IM_COL32(250, 60, 220, 255);
        case PinType::Vector: return IM_COL32(255, 200, 40, 255);
        case PinType::Entity: return IM_COL32(0, 170, 240, 255);
    }
    return IM_COL32(200, 200, 200, 255);
}

// --- Historial -------------------------------------------------------------------------------

void GraphHistory::reset(const std::string& state) {
    undo.clear();
    redo.clear();
    current = state;
}

bool GraphHistory::push(std::string state) {
    if (state == current) return false;
    undo.push_back(std::move(current));
    while (undo.size() > kMax) undo.pop_front();
    redo.clear();
    current = std::move(state);
    return true;
}

bool GraphHistory::undoTo(std::string& out) {
    if (undo.empty()) return false;
    redo.push_back(std::move(current));
    current = std::move(undo.back());
    undo.pop_back();
    out = current;
    return true;
}

bool GraphHistory::redoTo(std::string& out) {
    if (redo.empty()) return false;
    undo.push_back(std::move(current));
    current = std::move(redo.back());
    redo.pop_back();
    out = current;
    return true;
}

// --- Documento -------------------------------------------------------------------------------

bool open(Document& doc, const std::filesystem::path& file, const std::filesystem::path& assets_root, std::string* error) {
    vscript::Graph graph;
    if (!vscript::loadGraph(file, graph, error)) return false;
    doc.graph = std::move(graph);
    doc.path = file;
    std::error_code ec;
    const std::filesystem::path relative = std::filesystem::relative(file, assets_root, ec);
    const std::u8string text = (ec || relative.empty() ? file.filename() : relative).generic_u8string();
    doc.relative.assign(text.begin(), text.end());
    doc.canvas.selection.clear();
    doc.canvas.link_selection.clear();
    doc.canvas.snap = true;  // solo Visual Script (los otros lienzos siguen sin snap)
    doc.details_node = 0;
    doc.details_variable.clear();
    doc.create_from.reset();
    doc.pin_menu.reset();
    doc.var_drop.clear();
    doc.active.clear();  // empieza en el Event Graph
    doc.details_function.clear();
    doc.watches.clear();
    doc.views.clear();
    doc.graph.editing.clear();
    doc.saved_state = vscript::graphToJson(doc.graph);
    doc.history.reset(doc.saved_state);
    doc.snapshot_pending = false;
    doc.dirty = false;
    // Estado inicial: el del archivo tal cual (sin guardar nada).
    doc.compile = vscript::compileGraph(doc.graph, doc.relative);
    doc.compiled = true;
    doc.needs_compile = false;
    doc.breakpoints_changed = true;
    std::vector<nodegraph::NodeView> views;
    for (vscript::Node& n : doc.graph.nodes) {
        if (!n.graph.empty()) continue;  // solo el Event Graph
        nodegraph::NodeView v;
        v.id = n.id;
        v.position = &n.position;
        views.push_back(v);
    }
    doc.canvas.frame(views);
    return true;
}

bool save(Document& doc, std::string* error) {
    if (doc.path.empty()) {
        if (error) *error = "El grafo no tiene archivo";
        return false;
    }
    if (!vscript::saveGraph(doc.graph, doc.path, error)) return false;
    doc.saved_state = vscript::graphToJson(doc.graph);
    if (!doc.snapshot_pending) doc.history.push(doc.saved_state);
    doc.dirty = false;
    return true;
}

bool compile(Document& doc, const Host& host) {
    flushSnapshot(doc, true);
    doc.compile = vscript::compileGraph(doc.graph, doc.relative);
    doc.compiled = true;
    doc.needs_compile = false;
    if (!doc.compile.ok) return false;
    std::string error;
    if (!save(doc, &error)) {
        doc.compile.ok = false;
        doc.compile.errors.push_back(vscript::NodeError{0, "No se pudo guardar: " + error});
        return false;
    }
    if (host.reload) host.reload(doc.relative);
    return true;
}

Status status(const Document& doc) {
    if (!doc.compiled) return Status::Unknown;
    if (doc.needs_compile) return Status::Dirty;
    if (!doc.compile.ok) return Status::Errors;
    if (!doc.compile.errors.empty()) return Status::Warnings;
    return Status::Good;
}

bool undo(Document& doc) {
    flushSnapshot(doc, true);
    std::string state;
    if (!doc.history.undoTo(state)) return false;
    return applyState(doc, state);
}

bool redo(Document& doc) {
    flushSnapshot(doc, true);
    std::string state;
    if (!doc.history.redoTo(state)) return false;
    return applyState(doc, state);
}

// --- Editor de valores -----------------------------------------------------------------------

bool drawPinValueEditor(vscript::Pin& pin, float width, bool compact, const Host& host) {
    if (pin.type == PinType::Exec) return false;
    width = std::max(width, 24.0f);
    bool edited = false;

    // Enum (opciones del catalogo): desplegable.
    if (!pin.options.empty() && pin.type != PinType::Bool) {
        ImGui::SetNextItemWidth(width);
        if (ImGui::BeginCombo("##opt", pin.value.empty() ? "(ninguno)" : pin.value.c_str())) {
            for (const std::string& option : pin.options) {
                if (ImGui::Selectable(option.c_str(), option == pin.value)) {
                    pin.value = option;
                    edited = true;
                }
            }
            ImGui::EndCombo();
        }
        return edited;
    }

    switch (pin.type) {
        case PinType::Bool: {
            bool b = boolValue(pin.value);
            if (ImGui::Checkbox("##b", &b)) {
                pin.value = b ? "true" : "false";
                edited = true;
            }
            break;
        }
        case PinType::Int: {
            int v = static_cast<int>(std::lround(vscript::parseLiteral(pin.value, PinType::Int).number));
            ImGui::SetNextItemWidth(width);
            const bool changed = compact ? ImGui::InputInt("##i", &v, 0, 0) : ImGui::DragInt("##i", &v, 0.2f);
            if (changed) {
                pin.value = std::to_string(v);
                edited = true;
            }
            break;
        }
        case PinType::Float: {
            float v = static_cast<float>(vscript::parseLiteral(pin.value, PinType::Float).number);
            ImGui::SetNextItemWidth(width);
            const bool changed = compact ? ImGui::InputFloat("##f", &v, 0.0f, 0.0f, "%.4g") : ImGui::DragFloat("##f", &v, 0.05f, 0.0f, 0.0f, "%.4g");
            if (changed) {
                pin.value = formatNumber(v);
                edited = true;
            }
            break;
        }
        case PinType::Vector: {
            core::Vec3 v = vscript::parseLiteral(pin.value, PinType::Vector).vector;
            float* parts[3] = {&v.x, &v.y, &v.z};
            const char* ids[3] = {"##x", "##y", "##z"};
            const ImU32 axis[3] = {theme::kAxisX, theme::kAxisY, theme::kAxisZ};
            const float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
            const float each = std::max(16.0f, (width - spacing * 2.0f) / 3.0f);
            bool changed = false;
            for (int i = 0; i < 3; ++i) {
                if (i > 0) ImGui::SameLine(0.0f, spacing);
                ImGui::SetNextItemWidth(each);
                changed |= compact ? ImGui::InputFloat(ids[i], parts[i], 0.0f, 0.0f, "%.3g")
                                   : ImGui::DragFloat(ids[i], parts[i], 0.05f, 0.0f, 0.0f, "%.3g");
                // Franja del color del eje a la izquierda (X rojo, Y verde, Z azul).
                const ImVec2 min = ImGui::GetItemRectMin();
                const ImVec2 max = ImGui::GetItemRectMax();
                ImGui::GetWindowDrawList()->AddRectFilled(min, ImVec2(min.x + 3.0f, max.y), axis[i], 2.0f);
            }
            if (changed) {
                pin.value = formatVector(v);
                edited = true;
            }
            break;
        }
        case PinType::Entity: {
            // "Self" se escribe como "" (parseLiteral: vacio = este objeto).
            ImGui::SetNextItemWidth(width);
            if (ImGui::BeginCombo("##e", pin.value.empty() ? "Self" : pin.value.c_str())) {
                if (ImGui::Selectable("Self", pin.value.empty())) {
                    pin.value.clear();
                    edited = true;
                }
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::InputTextWithHint("##name", "nombre del objeto", &pin.value)) edited = true;
                if (host.entity_names) {
                    ImGui::Separator();
                    for (const std::string& name : host.entity_names()) {
                        if (name.empty() || name == "Self") continue;
                        if (ImGui::Selectable(name.c_str(), name == pin.value)) {
                            pin.value = name;
                            edited = true;
                        }
                    }
                }
                ImGui::EndCombo();
            }
            break;
        }
        default: {
            ImGui::SetNextItemWidth(width);
            edited = ImGui::InputText("##v", &pin.value);
            break;
        }
    }
    return edited;
}

// --- Paleta ----------------------------------------------------------------------------------

const std::vector<PaletteEntry>& palette() { return paletteCache().entries; }

// --- Lienzo ----------------------------------------------------------------------------------

DrawResult drawGraph(Document& doc, const Host& host, ImVec2 size) {
    DrawResult result;
    vscript::Graph& g = doc.graph;
    bool changed = false;
    ImGuiIO& io = ImGui::GetIO();
    g.editing = doc.active;  // los nodos nuevos van al grafo que se ve

    scripting::ScriptSystem::VisualScriptDebug debug;
    const bool has_debug = host.debug && host.debug(doc.relative, debug);

    ViewData view;
    buildViews(doc, has_debug ? &debug : nullptr, view);

    const nodegraph::InlineEditor inline_editor = [&](int node_id, int pin, float width) {
        vscript::Node* n = g.find(node_id);
        if (n == nullptr || !validPin(*n, pin, false)) return false;
        vscript::Pin& p = n->inputs[static_cast<std::size_t>(pin)];
        vscript::Pin typed = p;
        typed.type = vscript::nodePinType(g, *n, false, pin);
        if (!drawPinValueEditor(typed, width, true, host)) return false;
        p.value = typed.value;
        changed = true;
        return true;
    };
    const nodegraph::LinkQuery can_link = [&](nodegraph::PinRef from, nodegraph::PinRef to) {
        nodegraph::LinkCheck check;
        const vscript::Node* a = g.find(from.node);
        const vscript::Node* b = g.find(to.node);
        if (a == nullptr || b == nullptr || !validPin(*a, from.pin, true) || !validPin(*b, to.pin, false)) return check;
        if (a == b) {
            check.message = "No se puede enlazar un nodo consigo mismo";
            return check;
        }
        const PinType ft = vscript::nodePinType(g, *a, true, from.pin);
        const PinType tt = vscript::nodePinType(g, *b, false, to.pin);
        if ((ft == PinType::Exec) != (tt == PinType::Exec)) {
            check.message = "Un pin de ejecución solo se enlaza con otro de ejecución";
            return check;
        }
        if (vscript::pinTypesCompatible(ft, tt)) {
            check.ok = true;
            return check;
        }
        if (vscript::conversionKind(ft, tt)) {
            check.ok = true;
            check.converts = true;
            check.message = std::string("Se añadirá una conversión: ") + vscript::pinTypeLabel(ft) + " -> " + vscript::pinTypeLabel(tt);
            return check;
        }
        check.message = std::string("Tipos incompatibles: ") + vscript::pinTypeLabel(ft) + " -> " + vscript::pinTypeLabel(tt);
        return check;
    };
    nodegraph::DrawOptions options;
    options.inline_editor = &inline_editor;
    options.can_link = &can_link;
    options.minimap = doc.canvas.minimap;
    options.keys = true;

    // Hijo propio: el foco del lienzo (atajos) no se confunde con los paneles de al lado.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    const bool visible = ImGui::BeginChild("##vsgraph", size, ImGuiChildFlags_None,
                                           ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    if (!visible) {
        ImGui::EndChild();
        flushSnapshot(doc, false);
        return result;
    }
    const nodegraph::Events ev = doc.canvas.draw("vs_canvas", view.nodes, view.links, ImVec2(0.0f, 0.0f), options);
    {
        // Que grafo se ve (arriba a la izquierda, como la ruta de Unreal).
        const ImVec2 corner = ImGui::GetWindowPos();
        const std::string label = graphLabel(doc.active) + (has_debug ? "  ·  en vivo" : "");
        ImGui::GetWindowDrawList()->AddText(ImVec2(corner.x + 12.0f, corner.y + 8.0f), IM_COL32(200, 200, 210, 170), label.c_str());
    }

    // Soltar una variable de Mi Blueprint: Ctrl = Get, Alt = Set, sin nada = menu.
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kVariablePayload)) {
            const std::string name(static_cast<const char*>(payload->Data));
            const core::Vec2 at = doc.canvas.toCanvas(io.MousePos);
            if (g.findVariable(name) != nullptr) {
                if (io.KeyCtrl || io.KeyAlt) {
                    addVariableNode(doc, name, io.KeyAlt, at);
                    changed = true;
                } else {
                    doc.var_drop = name;
                    doc.var_drop_at = at;
                    ImGui::OpenPopup("vs_var_drop");
                }
            }
        }
        // Soltar un objeto (Componentes o Jerarquia de la pestana Blueprint): su
        // referencia relativa a Self; Ctrl = menu "Obtener <Componente>".
        if (host.entity_drop) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kEntityPayload);
                payload != nullptr && payload->DataSize == static_cast<int>(sizeof(Uuid))) {
                Uuid uuid{};
                std::memcpy(&uuid, payload->Data, sizeof(Uuid));
                EntityDrop drop = host.entity_drop(uuid);
                const core::Vec2 at = doc.canvas.toCanvas(io.MousePos);
                if (!drop.ok) {
                    if (host.notify) host.notify("Solo objetos de este Blueprint", drop.error);
                } else if (io.KeyCtrl) {
                    doc.entity_drop = std::move(drop);
                    doc.entity_drop_at = at;
                    ImGui::OpenPopup("vs_entity_drop");
                } else {
                    addEntityRefNode(doc, drop, at);
                    changed = true;
                }
            }
        }
        ImGui::EndDragDropTarget();
    }
    doc.focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);
    const bool keys = doc.focused && !io.WantTextInput;

    // --- Eventos del lienzo ---
    if (ev.link) changed |= connectOrConvert(doc, host, ev.link->from_node, ev.link->from_pin, ev.link->to_node, ev.link->to_pin);
    if (ev.unlink) {
        nodegraph::PinRef pin = *ev.unlink;
        pin.output = false;
        changed |= breakPinLinks(g, pin);
    }
    for (const nodegraph::LinkView& l : ev.cut_links) {
        eraseLink(g, l.from_node, l.from_pin, l.to_node, l.to_pin);
        changed = true;
    }
    if (ev.relink) changed |= relinkPin(g, ev.relink->first, ev.relink->second);
    if (ev.link_double_clicked) changed |= insertReroute(doc, host, *ev.link_double_clicked, ev.link_point);
    if (ev.cut && !doc.canvas.selection.empty()) {
        copySelection(doc);
        eraseNodes(doc, std::vector<int>(doc.canvas.selection.begin(), doc.canvas.selection.end()));
        changed = true;
    }
    if (!ev.erase.empty()) {
        eraseNodes(doc, ev.erase);
        changed = true;
    }
    if (ev.moved) changed = true;
    if (ev.select_all) {
        doc.canvas.selection.clear();
        for (const vscript::Node& n : g.nodes) {
            if (n.graph == doc.active) doc.canvas.selection.insert(n.id);
        }
    }
    if (ev.copy || ev.duplicate) copySelection(doc);
    if ((ev.paste || ev.duplicate) && !doc.clipboard.empty()) {
        pasteClipboard(doc, ev.menu_position, ev.duplicate);
        changed = true;
    }
    if (ev.clicked != 0) {
        doc.details_node = ev.clicked;
        doc.details_variable.clear();
        doc.details_function.clear();
    }
    std::string open_graph;  // doble clic en una llamada: su funcion / macro (al final del frame)
    if (ev.double_clicked != 0) {
        if (const vscript::Node* n = g.find(ev.double_clicked); n != nullptr && (n->kind == "var.get" || n->kind == "var.set")) {
            doc.details_variable = n->fn;
            doc.details_node = 0;
        } else if (n != nullptr && (n->kind == "fn.call" || n->kind == "macro.call")) {
            open_graph = n->kind == "fn.call" ? vscript::functionKey(n->fn) : vscript::macroKey(n->fn);
        } else if (n != nullptr && vscript::isSubgraphNode(n->kind)) {
            doc.details_function = n->graph;  // entrada / Return / salidas: la firma
            doc.details_node = 0;
            doc.details_variable.clear();
        }
    }
    if (ev.wrap_comment) changed |= commentAround(doc);
    if (ev.spawn_key != ImGuiKey_None) {
        if (const char* kind = spawnKind(ev.spawn_key); kind != nullptr && vscript::findNodeInfo(kind) != nullptr) {
            doc.canvas.select(g.add(vscript::makeNode(kind, ev.spawn_at)).id);
            changed = true;
        }
    }
    if (ev.undo) changed |= undo(doc);
    if (ev.redo) changed |= redo(doc);
    // F9: punto de ruptura en el nodo bajo el raton o el elegido.
    if (keys && ImGui::IsKeyPressed(ImGuiKey_F9, false)) {
        const int target = ev.hovered_node != 0 ? ev.hovered_node
                                                : (doc.canvas.selection.size() == 1 ? *doc.canvas.selection.begin() : 0);
        if (vscript::Node* n = g.find(target)) {
            n->breakpoint = !n->breakpoint;
            changed = true;
        }
    }
    if (keys && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) result.save = true;
    if (keys && ImGui::IsKeyPressed(ImGuiKey_F7, false)) result.compile = true;

    // --- Menus ---
    if (ev.background_menu || ev.dropped) {
        doc.create_at = ev.menu_position;
        doc.create_from = ev.dropped;
        ImGui::OpenPopup("vs_create");
    }
    if (ev.node_menu != 0) ImGui::OpenPopup("vs_node_menu");
    if (ev.pin_menu) {
        doc.pin_menu = ev.pin_menu;
        ImGui::OpenPopup("vs_pin_menu");
    }

    if (ImGui::BeginPopup("vs_var_drop")) {
        if (!doc.var_drop.empty()) {
            ImGui::TextDisabled("%s", doc.var_drop.c_str());
            if (ImGui::MenuItem(("Get " + doc.var_drop).c_str(), "Ctrl")) {
                addVariableNode(doc, doc.var_drop, false, doc.var_drop_at);
                changed = true;
            }
            if (ImGui::MenuItem(("Set " + doc.var_drop).c_str(), "Alt")) {
                addVariableNode(doc, doc.var_drop, true, doc.var_drop_at);
                changed = true;
            }
        }
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopup("vs_entity_drop")) {
        if (doc.entity_drop) {
            const EntityDrop& d = *doc.entity_drop;
            const core::Vec2 at = doc.entity_drop_at;
            ImGui::TextDisabled("%s", d.name.c_str());
            if (ImGui::MenuItem(d.root ? "Self" : ("Obtener " + d.name).c_str())) {
                addEntityRefNode(doc, d, at);
                changed = true;
            }
            ImGui::Separator();
            if (d.components.empty()) ImGui::TextDisabled("Sin componentes");
            for (const auto& [name, label] : d.components) {
                ImGui::PushID(name.c_str());
                const std::string item = "Obtener " + (label.empty() ? name : label);
                if (ImGui::MenuItem(item.c_str())) {
                    addComponentGetNode(doc, d, name, at);
                    changed = true;
                }
                if (!label.empty() && label != name) ImGui::SetItemTooltip("%s", name.c_str());
                ImGui::PopID();
            }
        }
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopup("vs_pin_menu")) {
        if (doc.pin_menu) {
            const nodegraph::PinRef pin = *doc.pin_menu;
            if (ImGui::MenuItem("Romper enlaces")) changed |= breakPinLinks(g, pin);
            vscript::Node* n = g.find(pin.node);
            // Vigilar: en Play su valor sale en una burbuja fija (no se guarda en el archivo).
            if (n != nullptr && validPin(*n, pin.pin, pin.output) && vscript::nodePinType(g, *n, pin.output, pin.pin) != PinType::Exec) {
                const auto key = std::make_tuple(pin.node, pin.pin, pin.output);
                const bool watched = doc.watches.contains(key);
                if (ImGui::MenuItem(watched ? "Dejar de vigilar" : "Vigilar este valor")) {
                    if (watched) doc.watches.erase(key);
                    else doc.watches.insert(key);
                }
            }
            if (n != nullptr && !pin.output && validPin(*n, pin.pin, false) &&
                vscript::nodePinType(g, *n, false, pin.pin) != PinType::Exec) {
                if (ImGui::MenuItem("Restablecer valor")) {
                    std::string value;
                    if (const vscript::NodeInfo* info = vscript::findNodeInfo(n->kind)) {
                        const std::string& name = n->inputs[static_cast<std::size_t>(pin.pin)].name;
                        for (const vscript::Pin& p : info->inputs) {
                            if (p.name == name) value = p.value;
                        }
                    }
                    n->inputs[static_cast<std::size_t>(pin.pin)].value = value;
                    changed = true;
                }
                if (ImGui::MenuItem("Promover a variable")) changed |= promoteToVariable(doc, pin);
            }
        }
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopup("vs_node_menu")) {
        const int id = doc.canvas.selection.size() == 1 ? *doc.canvas.selection.begin() : 0;
        if (vscript::Node* n = g.find(id)) {
            ImGui::TextDisabled("%s", vscript::nodeTitle(*n).c_str());
            if (ImGui::MenuItem(n->breakpoint ? "Quitar punto de ruptura" : "Punto de ruptura", "F9")) {
                n->breakpoint = !n->breakpoint;
                changed = true;
            }
            if (n->kind != "comment" && ImGui::MenuItem("Desactivado", nullptr, n->disabled)) {
                n->disabled = !n->disabled;
                changed = true;
            }
            ImGui::SetNextItemWidth(220.0f);
            changed |= ImGui::InputTextWithHint("##note", "comentario del nodo", &n->comment);
            if ((n->kind == "call" || n->kind == "call.get" || n->kind == "call.set") && ImGui::MenuItem("Actualizar nodo")) {
                if (refreshApiNode(doc, id)) changed = true;
                else notify(host, "Actualizar nodo", "La función ya no existe en la API");
            }
            changed |= editDynamicPins(g, *n);
            if ((n->kind == "fn.call" || n->kind == "macro.call") && ImGui::MenuItem(n->kind == "fn.call" ? "Abrir la función" : "Abrir la macro")) {
                open_graph = n->kind == "fn.call" ? vscript::functionKey(n->fn) : vscript::macroKey(n->fn);
            }
            if (vscript::isSubgraphNode(n->kind) && ImGui::MenuItem("Editar la firma")) {
                doc.details_function = n->kind == "fn.call" ? vscript::functionKey(n->fn)
                                       : n->kind == "macro.call" ? vscript::macroKey(n->fn)
                                                                 : n->graph;
                doc.details_node = 0;
                doc.details_variable.clear();
            }
            if (ImGui::MenuItem("Detalles")) {
                doc.details_node = id;
                doc.details_variable.clear();
                doc.details_function.clear();
            }
        }
        if (!doc.canvas.selection.empty() && ImGui::BeginMenu("Alinear")) {
            struct AlignItem {
                const char* label;
                nodegraph::AlignOp op;
                const char* key;
            };
            static const AlignItem kAlign[] = {
                {"Arriba", nodegraph::AlignOp::Top, "Shift+W"},          {"Centro vertical", nodegraph::AlignOp::Middle, nullptr},
                {"Abajo", nodegraph::AlignOp::Bottom, "Shift+S"},        {"Izquierda", nodegraph::AlignOp::Left, "Shift+A"},
                {"Centro horizontal", nodegraph::AlignOp::Center, nullptr}, {"Derecha", nodegraph::AlignOp::Right, "Shift+D"},
                {"Repartir en horizontal", nodegraph::AlignOp::DistributeH, nullptr},
                {"Repartir en vertical", nodegraph::AlignOp::DistributeV, nullptr},
                {"Enderezar cables", nodegraph::AlignOp::Straighten, "Q"},
            };
            for (const AlignItem& item : kAlign) {
                if (ImGui::MenuItem(item.label, item.key)) {
                    ViewData fresh;  // nuevas: los punteros de `view` pueden no valer ya
                    buildViews(doc, nullptr, fresh);
                    doc.canvas.align(item.op, fresh.nodes, fresh.links);
                    changed = true;
                }
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Comentario alrededor", "C")) changed |= commentAround(doc);
        if (ImGui::MenuItem("Copiar", "Ctrl+C")) copySelection(doc);
        if (ImGui::MenuItem("Duplicar", "Ctrl+D")) {
            copySelection(doc);
            if (!doc.clipboard.empty()) {
                pasteClipboard(doc, core::Vec2{}, true);
                changed = true;
            }
        }
        if (ImGui::MenuItem("Borrar", "Supr")) {
            eraseNodes(doc, std::vector<int>(doc.canvas.selection.begin(), doc.canvas.selection.end()));
            changed = true;
        }
        ImGui::EndPopup();
    }

    // Buscador "Crear nodo" (por contexto si se arrastro desde un pin).
    if (ImGui::IsPopupOpen("vs_create")) {
        const PaletteCache& cache = paletteCache();
        std::vector<nodegraph::SearchItem> items;
        items.reserve(cache.items.size() + g.variables.size() * 2 + g.functions.size() + 1);
        for (const nodegraph::SearchItem& item : cache.items) {
            if (paletteFits(doc, cache.entries[static_cast<std::size_t>(item.index)].node)) items.push_back(item);
        }
        const int base = static_cast<int>(cache.entries.size());
        // Despues de las variables: llamadas a las funciones y macros y, dentro de una funcion, Return.
        const int calls_base = base + static_cast<int>(g.variables.size()) * 2;
        const int return_index = calls_base + static_cast<int>(g.functions.size());
        for (std::size_t i = 0; i < g.functions.size(); ++i) {
            const vscript::Subgraph& s = g.functions[i];
            const bool macro = s.kind == vscript::SubgraphKind::Macro;
            vscript::Node probe;
            probe.kind = macro ? "macro.call" : "fn.call";
            vscript::subgraphNodePins(s, probe.kind, probe.inputs, probe.outputs);
            nodegraph::SearchItem item;
            item.category = macro ? "Macros" : "Funciones";
            item.title = s.name;
            item.keywords = std::string(macro ? "macro " : "funcion function llamar ") + s.name;
            item.description = s.description;
            item.index = calls_base + static_cast<int>(i);
            item.in_types = vscript::inputTypeMask(probe);
            item.out_types = vscript::outputTypeMask(probe);
            items.push_back(std::move(item));
        }
        const vscript::Subgraph* active_fn = doc.active.rfind("fn:", 0) == 0 ? g.findSubgraph(doc.active) : nullptr;
        if (active_fn != nullptr) {
            nodegraph::SearchItem item;
            item.category = "Funciones";
            item.title = "Return";
            item.keywords = "return devolver salir resultado";
            item.description = "Sale de la función con sus resultados";
            item.index = return_index;
            item.in_types = 1u << static_cast<int>(PinType::Exec);
            items.push_back(std::move(item));
        }
        for (std::size_t i = 0; i < g.variables.size(); ++i) {
            const vscript::Variable& v = g.variables[i];
            const std::uint32_t type_bit = 1u << static_cast<int>(v.type);
            const std::uint32_t exec_bit = 1u << static_cast<int>(PinType::Exec);
            nodegraph::SearchItem get;
            get.category = "Variables";
            get.title = "Get " + v.name;
            get.keywords = "variable leer get " + v.name;
            get.description = v.tooltip;
            get.index = base + static_cast<int>(i) * 2;
            get.out_types = type_bit;
            items.push_back(get);
            nodegraph::SearchItem set = get;
            set.title = "Set " + v.name;
            set.keywords = "variable asignar set " + v.name;
            set.index = base + static_cast<int>(i) * 2 + 1;
            set.in_types = exec_bit | type_bit;
            set.out_types = exec_bit | type_bit;
            items.push_back(set);
        }
        nodegraph::SearchContext context;
        if (doc.create_from) {
            if (const vscript::Node* from = g.find(doc.create_from->node);
                from != nullptr && validPin(*from, doc.create_from->pin, doc.create_from->output)) {
                const PinType type = vscript::nodePinType(g, *from, doc.create_from->output, doc.create_from->pin);
                context.pin_type = static_cast<int>(type);
                context.exec = type == PinType::Exec;
                context.from_output = doc.create_from->output;
                if (type == PinType::Entity && doc.create_from->output) context.owner = "Entity";
            }
        }
        context.context_sensitive = &doc.context_sensitive;
        context.recent = &doc.recent;
        context.any_mask = 1u << static_cast<int>(PinType::Any);
        context.exec_mask = 1u << static_cast<int>(PinType::Exec);
        const int chosen = nodegraph::searchPopup("vs_create", items, doc.search, &context);
        if (chosen >= 0) {
            vscript::Node n;
            bool valid = true;
            if (chosen < base) {
                n = cache.entries[static_cast<std::size_t>(chosen)].node;
                doc.recent.erase(std::remove(doc.recent.begin(), doc.recent.end(), chosen), doc.recent.end());
                doc.recent.insert(doc.recent.begin(), chosen);
                if (doc.recent.size() > kMaxRecent) doc.recent.resize(kMaxRecent);
            } else if (chosen < calls_base) {
                const std::size_t var = static_cast<std::size_t>(chosen - base) / 2;
                if (var < g.variables.size()) {
                    n = vscript::makeNode((chosen - base) % 2 == 1 ? "var.set" : "var.get");
                    n.fn = g.variables[var].name;
                } else {
                    valid = false;
                }
            } else if (chosen < return_index) {
                const vscript::Subgraph& s = g.functions[static_cast<std::size_t>(chosen - calls_base)];
                n.kind = s.kind == vscript::SubgraphKind::Macro ? "macro.call" : "fn.call";
                n.fn = s.name;
                n.pure = s.kind == vscript::SubgraphKind::Function && s.pure;
                vscript::subgraphNodePins(s, n.kind, n.inputs, n.outputs);
            } else if (chosen == return_index && active_fn != nullptr) {
                n.kind = "fn.return";
                n.fn = active_fn->name;
                vscript::subgraphNodePins(*active_fn, n.kind, n.inputs, n.outputs);
            } else {
                valid = false;
            }
            if (valid) {
                n.position = doc.create_at;
                const int id = g.add(std::move(n)).id;
                if (doc.create_from) autoConnect(g, *doc.create_from, id);
                doc.canvas.select(id);
                doc.details_node = id;
                doc.details_variable.clear();
                changed = true;
            }
            doc.create_from.reset();
        }
    }

    ImGui::EndChild();

    if (!open_graph.empty()) showGraph(doc, open_graph);
    if (changed) markChanged(doc);
    syncBreakpoints(doc, host);
    flushSnapshot(doc, false);
    result.changed = changed;
    return result;
}

// --- Mi Blueprint ----------------------------------------------------------------------------

bool drawMyBlueprint(Document& doc, const Host& host) {
    vscript::Graph& g = doc.graph;
    bool changed = false;
    scripting::ScriptSystem::VisualScriptDebug debug;
    const bool has_debug = host.debug && host.debug(doc.relative, debug);
    ImDrawList* draw = ImGui::GetWindowDrawList();

    g.editing = doc.active;
    if (ImGui::CollapsingHeader("Grafos", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::Selectable("Event Graph", doc.active.empty())) showGraph(doc, {});
        ImGui::Indent();
        int focus = 0;
        for (const vscript::Node& n : g.nodes) {
            if (!n.graph.empty() || vscript::nodeLook(n) != vscript::NodeLook::Event) continue;
            ImGui::PushID(n.id);
            if (ImGui::Selectable(vscript::nodeTitle(n).c_str(), doc.canvas.selection.contains(n.id))) focus = n.id;
            ImGui::PopID();
        }
        ImGui::Unindent();
        if (focus != 0) focusNode(doc, focus);
        if (ImGui::Selectable("Construction Script", doc.active == vscript::kConstructionGraph)) showGraph(doc, vscript::kConstructionGraph);
        ImGui::SetItemTooltip("Corre en el editor al colocar o cambiar el objeto (fuera de Play)");
    }

    // Funciones y macros: + crea una (con su entrada y su salida) y la abre.
    for (const vscript::SubgraphKind kind : {vscript::SubgraphKind::Function, vscript::SubgraphKind::Macro}) {
        const bool macro = kind == vscript::SubgraphKind::Macro;
        const bool section = ImGui::CollapsingHeader(macro ? "Macros" : "Funciones",
                                                     ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);
        ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::GetFrameHeight());
        if (ImGui::SmallButton(macro ? "+##vs_add_macro" : "+##vs_add_fn")) {
            const std::string key = g.addSubgraph({}, kind).key();
            showGraph(doc, key);
            doc.details_function = key;
            doc.details_node = 0;
            doc.details_variable.clear();
            changed = true;
        }
        ImGui::SetItemTooltip(macro ? "Macro nueva" : "Función nueva");
        if (!section) continue;
        std::string remove;
        std::string open_key;
        for (const vscript::Subgraph& s : g.functions) {
            if (s.kind != kind) continue;
            const std::string key = s.key();
            ImGui::PushID(key.c_str());
            if (ImGui::Selectable(s.name.c_str(), doc.active == key)) open_key = key;
            if (!s.description.empty()) ImGui::SetItemTooltip("%s", s.description.c_str());
            if (ImGui::BeginPopupContextItem("##fn_menu")) {
                if (ImGui::MenuItem("Añadir llamada")) {
                    addSubgraphCall(doc, s, doc.canvas.viewCenter());
                    changed = true;
                }
                if (ImGui::MenuItem("Editar la firma")) {
                    doc.details_function = key;
                    doc.details_node = 0;
                    doc.details_variable.clear();
                }
                if (ImGui::MenuItem("Borrar")) remove = key;
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
        if (!open_key.empty()) {
            showGraph(doc, open_key);
            doc.details_function = open_key;
            doc.details_node = 0;
            doc.details_variable.clear();
        }
        if (!remove.empty()) {
            // Como Unreal: sus nodos y sus llamadas se van con ella (Ctrl+Z la devuelve).
            if (doc.active == remove) showGraph(doc, {});
            g.removeSubgraph(remove);
            if (doc.details_function == remove) doc.details_function.clear();
            std::erase_if(doc.canvas.selection, [&](int id) { return g.find(id) == nullptr; });
            if (doc.details_node != 0 && g.find(doc.details_node) == nullptr) doc.details_node = 0;
            doc.views.erase(remove);
            changed = true;
        }
        if (std::none_of(g.functions.begin(), g.functions.end(), [&](const vscript::Subgraph& s) { return s.kind == kind; })) {
            ImGui::TextDisabled(macro ? "Sin macros. Pulsa + para crear una." : "Sin funciones. Pulsa + para crear una.");
        }
    }

    const bool open = ImGui::CollapsingHeader("Variables", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);
    ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::GetFrameHeight());
    if (ImGui::SmallButton("+##vs_add_var")) {
        std::string name = "NuevaVariable";
        for (int i = 2; g.findVariable(name) != nullptr; ++i) name = "NuevaVariable" + std::to_string(i);
        g.variables.push_back(vscript::Variable{name, PinType::Float, "0", true, {}});
        doc.details_variable = name;
        doc.details_node = 0;
        changed = true;
    }
    ImGui::SetItemTooltip("Variable nueva");
    if (open) {
        int remove = -1;
        int duplicate = -1;
        for (std::size_t i = 0; i < g.variables.size(); ++i) {
            vscript::Variable& v = g.variables[i];
            ImGui::PushID(static_cast<int>(i));
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float h = ImGui::GetTextLineHeight();
            draw->AddCircleFilled(ImVec2(p.x + 7.0f, p.y + h * 0.5f + 1.0f), 4.5f, vsPinColor(v.type));
            ImGui::SetCursorScreenPos(ImVec2(p.x + 16.0f, p.y));
            const float avail = ImGui::GetContentRegionAvail().x;
            if (ImGui::Selectable(v.name.c_str(), doc.details_variable == v.name, ImGuiSelectableFlags_None,
                                  ImVec2(std::max(40.0f, avail - 80.0f), 0.0f))) {
                doc.details_variable = v.name;
                doc.details_node = 0;
            }
            if (ImGui::BeginDragDropSource()) {
                ImGui::SetDragDropPayload(kVariablePayload, v.name.c_str(), v.name.size() + 1);
                ImGui::Text("%s", v.name.c_str());
                ImGui::TextDisabled("Ctrl: Get  ·  Alt: Set");
                ImGui::EndDragDropSource();
            }
            if (ImGui::BeginPopupContextItem("##var_menu")) {
                if (ImGui::MenuItem("Añadir Get")) {
                    addVariableNode(doc, v.name, false, doc.canvas.viewCenter());
                    changed = true;
                }
                if (ImGui::MenuItem("Añadir Set")) {
                    addVariableNode(doc, v.name, true, doc.canvas.viewCenter());
                    changed = true;
                }
                if (ImGui::MenuItem("Duplicar")) duplicate = static_cast<int>(i);
                if (ImGui::MenuItem("Borrar")) remove = static_cast<int>(i);
                ImGui::EndPopup();
            }
            ImGui::SameLine();
            if (has_debug && debug.variables.count(v.name)) {
                ImGui::TextColored(colorVec(theme::kAccentHover), "= %s", debug.variables[v.name].c_str());
            } else {
                ImGui::TextDisabled("%s%s", vscript::pinTypeLabel(v.type), v.exposed ? " ·" : "");
                if (v.exposed) ImGui::SetItemTooltip("Sale en el Inspector");
            }
            ImGui::PopID();
        }
        if (duplicate >= 0) {
            vscript::Variable copy = g.variables[static_cast<std::size_t>(duplicate)];
            const std::string base = copy.name;
            for (int i = 2; g.findVariable(copy.name) != nullptr; ++i) copy.name = base + std::to_string(i);
            g.variables.push_back(copy);
            changed = true;
        }
        if (remove >= 0) {
            // Como Unreal: sus nodos Get/Set se van con ella (Ctrl+Z la devuelve).
            const std::string name = g.variables[static_cast<std::size_t>(remove)].name;
            std::vector<int> nodes;
            for (const vscript::Node& n : g.nodes) {
                if ((n.kind == "var.get" || n.kind == "var.set") && n.fn == name) nodes.push_back(n.id);
            }
            eraseNodes(doc, nodes);
            g.variables.erase(g.variables.begin() + remove);
            if (doc.details_variable == name) doc.details_variable.clear();
            changed = true;
        }
        if (g.variables.empty()) ImGui::TextDisabled("Sin variables. Pulsa + para crear una.");
    }

    if (changed) markChanged(doc);
    syncBreakpoints(doc, host);
    flushSnapshot(doc, false);
    return changed;
}

// --- Detalles --------------------------------------------------------------------------------

bool drawDetails(Document& doc, const Host& host) {
    vscript::Graph& g = doc.graph;
    bool changed = false;
    scripting::ScriptSystem::VisualScriptDebug debug;
    const bool has_debug = host.debug && host.debug(doc.relative, debug);

    if (!doc.details_variable.empty() && g.findVariable(doc.details_variable) == nullptr) doc.details_variable.clear();
    if (doc.details_node != 0 && g.find(doc.details_node) == nullptr) doc.details_node = 0;
    if (!doc.details_function.empty() && g.findSubgraph(doc.details_function) == nullptr) doc.details_function.clear();
    g.editing = doc.active;

    if (vscript::Subgraph* fn = doc.details_variable.empty() && !doc.details_function.empty() ? g.findSubgraph(doc.details_function) : nullptr) {
        changed |= drawSignature(doc, host, *fn);
    } else if (!doc.details_variable.empty()) {
        vscript::Variable* v = g.findVariable(doc.details_variable);
        ImGui::SeparatorText("Variable");
        if (beginDetailsTable("##vs_var_details")) {
            detailsLabel("Nombre");
            std::string name = v->name;
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::InputText("##name", &name, ImGuiInputTextFlags_EnterReturnsTrue) && !name.empty() && name != v->name) {
                if (g.findVariable(name) == nullptr) {
                    g.renameVariable(v->name, name);
                    doc.details_variable = name;
                    v = g.findVariable(name);
                    changed = true;
                } else {
                    notify(host, "Renombrar", "Ya hay una variable con ese nombre");
                }
            }
            detailsLabel("Tipo");
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::BeginCombo("##type", vscript::pinTypeLabel(v->type))) {
                for (int t = 1; t < vscript::kPinTypeCount; ++t) {
                    const PinType type = static_cast<PinType>(t);
                    const ImVec2 p = ImGui::GetCursorScreenPos();
                    ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + 6.0f, p.y + ImGui::GetTextLineHeight() * 0.5f + 1.0f), 4.0f,
                                                                vsPinColor(type));
                    ImGui::SetCursorScreenPos(ImVec2(p.x + 16.0f, p.y));
                    if (ImGui::Selectable(vscript::pinTypeLabel(type), v->type == type) && v->type != type) {
                        v->type = type;
                        v->value = defaultValueFor(type);
                        changed = true;
                    }
                }
                ImGui::EndCombo();
            }
            detailsLabel("Valor por defecto");
            {
                vscript::Pin pin;
                pin.name = v->name;
                pin.type = v->type;
                pin.value = v->value;
                if (drawPinValueEditor(pin, ImGui::GetContentRegionAvail().x, false, host)) {
                    v->value = pin.value;
                    changed = true;
                }
            }
            if (has_debug && debug.variables.count(v->name)) {
                detailsLabel("En vivo");
                ImGui::TextColored(colorVec(theme::kAccentHover), "%s", debug.variables[v->name].c_str());
            }
            detailsLabel("Instancia editable");
            changed |= ImGui::Checkbox("##exposed", &v->exposed);
            ImGui::SetItemTooltip("Sale en el Inspector: cada objeto puede tener su valor");
            detailsLabel("Descripción");
            ImGui::SetNextItemWidth(-1.0f);
            changed |= ImGui::InputText("##tooltip", &v->tooltip);
            detailsLabel("Usos");
            int uses = 0;
            for (const vscript::Node& n : g.nodes) {
                if ((n.kind == "var.get" || n.kind == "var.set") && n.fn == v->name) ++uses;
            }
            ImGui::Text("%d nodo(s)", uses);
            ImGui::EndTable();
        }
    } else if (vscript::Node* n = g.find(doc.details_node)) {
        const int id = n->id;
        ImGui::SeparatorText(vscript::nodeTitle(*n).c_str());
        const vscript::NodeInfo* info = vscript::findNodeInfo(n->kind);
        ImGui::TextDisabled("%s  ·  nodo %d", n->kind.c_str(), id);
        if (info != nullptr && !info->description.empty()) ImGui::TextWrapped("%s", info->description.c_str());
        const bool is_comment = n->kind == "comment";
        if (beginDetailsTable("##vs_node_details")) {
            if (n->kind == "entity.child") {
                detailsLabel("Ruta");
                ImGui::SetNextItemWidth(-1.0f);
                changed |= ImGui::InputTextWithHint("##child_path", "Cuerpo/Malla", &n->fn);
                ImGui::SetItemTooltip("Desde la raíz del Blueprint, nombres separados por '/'.\n"
                                      "Hermanos con el mismo nombre: Hueso[2] es el segundo.");
            } else if (!n->fn.empty() && n->kind != "var.get" && n->kind != "var.set") {
                detailsLabel("Función");
                ImGui::TextUnformatted(n->fn.c_str());
            }
            if (is_comment) {
                detailsLabel("Texto");
                ImGui::SetNextItemWidth(-1.0f);
                changed |= ImGui::InputText("##text", &n->comment);
                detailsLabel("Color");
                float color[4] = {0.35f, 0.43f, 0.55f, 1.0f};
                if (n->comment_color != 0) {
                    const ImVec4 c = colorVec(rgbaToColor(n->comment_color));
                    color[0] = c.x;
                    color[1] = c.y;
                    color[2] = c.z;
                    color[3] = c.w;
                }
                if (ImGui::ColorEdit4("##color", color, ImGuiColorEditFlags_NoInputs)) {
                    n->comment_color = floatsToRgba(color);
                    if (n->comment_color == 0) n->comment_color = 1;  // negro transparente != defecto
                    changed = true;
                }
            } else {
                detailsLabel("Comentario");
                ImGui::SetNextItemWidth(-1.0f);
                changed |= ImGui::InputTextWithHint("##note", "burbuja sobre el nodo", &n->comment);
                detailsLabel("Desactivado");
                changed |= ImGui::Checkbox("##disabled", &n->disabled);
                ImGui::SetItemTooltip("No se ejecuta: deja pasar la ejecución por su primera salida");
            }
            detailsLabel("Punto de ruptura");
            changed |= ImGui::Checkbox("##breakpoint", &n->breakpoint);
            ImGui::EndTable();
        }
        if (!is_comment) {
            bool any_input = false;
            for (std::size_t k = 0; k < n->inputs.size(); ++k) {
                const PinType type = vscript::nodePinType(g, *n, false, static_cast<int>(k));
                if (type == PinType::Exec) continue;
                if (!any_input) {
                    ImGui::SeparatorText("Entradas");
                    if (!beginDetailsTable("##vs_node_inputs")) break;
                    any_input = true;
                }
                vscript::Pin& p = n->inputs[k];
                ImGui::PushID(static_cast<int>(k));
                detailsLabel(p.name.empty() ? "(valor)" : p.name.c_str());
                if (g.linkTo(id, static_cast<int>(k)) != nullptr) {
                    ImGui::TextDisabled("enlazado (%s)", vscript::pinTypeLabel(type));
                } else {
                    vscript::Pin typed = p;
                    typed.type = type;
                    if (drawPinValueEditor(typed, ImGui::GetContentRegionAvail().x, false, host)) {
                        p.value = typed.value;
                        changed = true;
                    }
                }
                if (has_debug) {
                    // Lo ultimo que leyo esa entrada (depuracion en vivo).
                    const auto it = debug.inputs.find(debugKey(id, k));
                    if (it != debug.inputs.end()) ImGui::TextColored(colorVec(theme::kAccentHover), "= %s", it->second.c_str());
                }
                ImGui::PopID();
            }
            if (any_input) ImGui::EndTable();
            if (has_debug) {
                // Salidas con su ultimo valor (Play).
                bool any_output = false;
                for (std::size_t k = 0; k < n->outputs.size(); ++k) {
                    if (vscript::nodePinType(g, *n, true, static_cast<int>(k)) == PinType::Exec) continue;
                    const auto it = debug.values.find(debugKey(id, k));
                    if (it == debug.values.end()) continue;
                    if (!any_output) {
                        ImGui::SeparatorText("Salidas (en vivo)");
                        any_output = true;
                    }
                    ImGui::TextDisabled("%s", n->outputs[k].name.empty() ? "(valor)" : n->outputs[k].name.c_str());
                    ImGui::SameLine();
                    ImGui::TextColored(colorVec(theme::kAccentHover), "= %s", it->second.c_str());
                }
            }
            if (n->kind == "call" || n->kind == "call.get" || n->kind == "call.set") {
                if (ImGui::Button("Actualizar nodo")) {
                    if (refreshApiNode(doc, id)) changed = true;
                    else notify(host, "Actualizar nodo", "La función ya no existe en la API");
                }
                ImGui::SetItemTooltip("Toma los pines actuales de la API (con tipos) sin perder valores ni enlaces");
            }
            if (vscript::isSubgraphNode(n->kind)) {
                const std::string key = n->kind == "fn.call"      ? vscript::functionKey(n->fn)
                                        : n->kind == "macro.call" ? vscript::macroKey(n->fn)
                                                                  : n->graph;
                if (ImGui::Button("Editar la firma")) {
                    doc.details_function = key;
                    doc.details_node = 0;
                }
                if (n->kind == "fn.call" || n->kind == "macro.call") {
                    ImGui::SameLine();
                    if (ImGui::Button("Abrir")) showGraph(doc, key);
                }
            }
            if (n->kind == "flow.timeline") changed |= drawTimelineEditor(g, *n, has_debug ? &debug : nullptr);
        }
    } else {
        ImGui::SeparatorText(graphLabel(doc.active).c_str());
        std::size_t nodes = 0;
        for (const vscript::Node& node : g.nodes) {
            if (node.graph == doc.active) ++nodes;
        }
        ImGui::TextDisabled("%zu nodo(s) aquí; %zu variable(s), %zu función(es) y macro(s)", nodes, g.variables.size(), g.functions.size());
        ImGui::TextDisabled("Elige un nodo en el grafo, o una variable o función en Mi Blueprint.");
    }

    if (changed) markChanged(doc);
    syncBreakpoints(doc, host);
    flushSnapshot(doc, false);
    return changed;
}

// --- Compilar --------------------------------------------------------------------------------

void drawCompilerResults(Document& doc) {
    const Status s = status(doc);
    const float h = ImGui::GetTextLineHeight();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    drawStatusIcon(ImGui::GetWindowDrawList(), ImVec2(p.x + h * 0.5f, p.y + h * 0.5f), h * 0.45f, s);
    ImGui::SetCursorScreenPos(ImVec2(p.x + h + 6.0f, p.y));
    if (s == Status::Errors || s == Status::Warnings) {
        ImGui::Text("%s: %zu mensaje(s)", statusText(s), doc.compile.errors.size());
    } else {
        ImGui::TextUnformatted(statusText(s));
    }
    if (s == Status::Dirty) {
        ImGui::SameLine();
        ImGui::TextDisabled("(F7 o Compilar)");
    }
    ImGui::Separator();
    for (std::size_t i = 0; i < doc.compile.errors.size(); ++i) {
        const vscript::NodeError& e = doc.compile.errors[i];
        ImGui::PushID(static_cast<int>(i));
        ImGui::TextColored(colorVec(e.warning ? theme::kYellow : theme::kRedText), "%s", e.warning ? "Aviso" : "Error");
        ImGui::SameLine();
        std::string text = e.message;
        if (e.node != 0) {
            const vscript::Node* n = doc.graph.find(e.node);
            text = (n != nullptr ? (n->graph.empty() ? std::string() : graphLabel(n->graph) + " > ") + vscript::nodeTitle(*n)
                                 : "Nodo " + std::to_string(e.node)) +
                   ": " + text;
        }
        if (ImGui::Selectable(text.c_str(), false) && e.node != 0 && doc.graph.find(e.node) != nullptr) {
            focusNode(doc, e.node);  // en su grafo (funcion, macro...)
        }
        ImGui::PopID();
    }
}

void drawCompileButton(Document& doc, const Host& host) {
    const Status s = status(doc);
    const float h = ImGui::GetFrameHeight();
    const bool pressed = ImGui::Button("      Compilar##vs_compile");
    const ImVec2 min = ImGui::GetItemRectMin();
    drawStatusIcon(ImGui::GetWindowDrawList(), ImVec2(min.x + h * 0.55f, min.y + h * 0.5f), h * 0.3f, s);
    ImGui::SetItemTooltip("%s (F7)", statusText(s));
    if (pressed && !compile(doc, host)) {
        notify(host, "Compilar", doc.compile.errors.empty() ? std::string("No se pudo compilar")
                                                             : doc.compile.errors.front().message);
    }
}

}  // namespace cramion::editor::vsedit
