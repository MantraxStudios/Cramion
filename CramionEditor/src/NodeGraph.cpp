#include "NodeGraph.h"

#include "Theme.h"

#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>

namespace cramion::editor::nodegraph {

namespace {

// Medidas en unidades del lienzo (se multiplican por el zoom).
constexpr float kHeader = 24.0f;
constexpr float kSubHeader = 14.0f;  // segunda linea de la cabecera (subtitulo)
constexpr float kRow = 22.0f;
constexpr float kPinRadius = 5.0f;
constexpr float kStrip = 15.0f;      // franja Desactivado / AVISO / ERROR
constexpr float kRerouteW = 32.0f;
constexpr float kRerouteH = 16.0f;
constexpr float kGridSmall = 16.0f;
constexpr float kGridBig = 128.0f;
constexpr float kMinimapW = 220.0f;  // pixeles
constexpr float kMinimapH = 150.0f;

// Aspecto (diseno 1, estilo Unreal).
constexpr ImU32 kCanvasBg = IM_COL32(26, 26, 28, 255);
constexpr ImU32 kGridMinor = IM_COL32(255, 255, 255, 9);
constexpr ImU32 kGridMajor = IM_COL32(0, 0, 0, 150);
constexpr ImU32 kBody = IM_COL32(18, 18, 20, 235);
constexpr ImU32 kExecColor = IM_COL32(240, 240, 240, 255);
constexpr ImU32 kPinLabel = IM_COL32(215, 215, 220, 255);

// Teclas que, mantenidas, crean un nodo al hacer clic en el fondo.
constexpr ImGuiKey kSpawnKeys[] = {ImGuiKey_B, ImGuiKey_S, ImGuiKey_D, ImGuiKey_N, ImGuiKey_O,
                                   ImGuiKey_G, ImGuiKey_1, ImGuiKey_2, ImGuiKey_3, ImGuiKey_4};

struct Layout {
    ImVec2 min{};
    ImVec2 max{};
    std::vector<ImVec2> in;
    std::vector<ImVec2> out;
    float header = 0.0f;  // pixeles
    float top = 0.0f;     // relleno arriba de las filas (sin cabecera)
    float strip = 0.0f;   // pixeles
};

ImVec2 add(ImVec2 a, ImVec2 b) { return ImVec2(a.x + b.x, a.y + b.y); }

float dist2(ImVec2 a, ImVec2 b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    return dx * dx + dy * dy;
}

float segmentDist2(ImVec2 p, ImVec2 a, ImVec2 b) {
    const float abx = b.x - a.x;
    const float aby = b.y - a.y;
    const float len = abx * abx + aby * aby;
    float t = len > 1e-6f ? ((p.x - a.x) * abx + (p.y - a.y) * aby) / len : 0.0f;
    t = std::clamp(t, 0.0f, 1.0f);
    return dist2(p, ImVec2(a.x + abx * t, a.y + aby * t));
}

ImU32 scaleColor(ImU32 c, float s) {
    const auto ch = [&](int shift) {
        const float v = static_cast<float>((c >> shift) & 0xFF) * s;
        return static_cast<ImU32>(std::clamp(v, 0.0f, 255.0f));
    };
    return (c & 0xFF000000u) | (ch(16) << 16) | (ch(8) << 8) | ch(0);
}

// Multiplica el alfa de los vertices dibujados desde `start` (nodos desactivados).
void scaleAlpha(ImDrawList* draw, int start, float s) {
    for (int i = start; i < draw->VtxBuffer.Size; ++i) {
        ImU32& c = draw->VtxBuffer[i].col;
        const float a = static_cast<float>((c >> IM_COL32_A_SHIFT) & 0xFF) * s;
        c = (c & ~IM_COL32_A_MASK) | (static_cast<ImU32>(std::clamp(a, 0.0f, 255.0f)) << IM_COL32_A_SHIFT);
    }
}

bool hasSubtitleLine(const NodeView& n) {
    if (n.subtitle.empty()) return false;
    switch (n.style) {
        case NodeStyle::Event:
        case NodeStyle::Function:
        case NodeStyle::Pure:
        case NodeStyle::Entry:
        case NodeStyle::Result: return true;
        default: return false;  // Default: como antes (el subtitulo no se dibuja)
    }
}

bool hasIcon(NodeStyle style) {
    switch (style) {
        case NodeStyle::Event:
        case NodeStyle::Function:
        case NodeStyle::Pure:
        case NodeStyle::Flow:
        case NodeStyle::Entry:
        case NodeStyle::Result: return true;
        default: return false;
    }
}

// Alto de la cabecera en unidades del lienzo (0 = sin cabecera).
float headerUnits(const NodeView& n, bool vertical) {
    if (vertical) return kHeader;
    switch (n.style) {
        case NodeStyle::Compact:
        case NodeStyle::Reroute: return 0.0f;
        case NodeStyle::Variable: return n.inputs.empty() ? 0.0f : kHeader * 0.85f;
        default: return hasSubtitleLine(n) ? kHeader + kSubHeader : kHeader;
    }
}

float stripUnits(const NodeView& n) { return n.disabled || !n.warning.empty() || !n.error.empty() ? kStrip : 0.0f; }

std::size_t nodeSignature(const NodeView& n) {
    std::string s;
    s.reserve(64);
    s += n.title;
    s += '\x1f';
    s += n.subtitle;
    s += '\x1f';
    s += n.symbol;
    s += static_cast<char>('A' + static_cast<int>(n.style));
    s += n.latent ? 'L' : 'l';
    s += n.breakpoint ? 'B' : 'b';
    for (const PinView& p : n.inputs) {
        s += '\x1e';
        s += p.label;
        s += p.inline_value ? 'I' : 'i';
        s += p.hide_label ? 'H' : 'h';
    }
    s += '\x1d';
    for (const PinView& p : n.outputs) {
        s += '\x1e';
        s += p.label;
        s += p.hide_label ? 'H' : 'h';
    }
    return std::hash<std::string>{}(s);
}

// Ancho que necesita el contenido, en unidades del lienzo (zoom 1).
float measureNode(const NodeView& n, float font) {
    ImGui::PushFont(nullptr, font);
    const float title_w = ImGui::CalcTextSize(n.title.c_str()).x;
    ImGui::PopFont();
    const bool compact = n.style == NodeStyle::Compact;
    ImGui::PushFont(nullptr, font * 0.9f);
    const float subtitle_w = hasSubtitleLine(n) ? ImGui::CalcTextSize(n.subtitle.c_str()).x : 0.0f;
    const std::size_t rows = std::max(n.inputs.size(), n.outputs.size());
    float row_w = 0.0f;
    float first_out = 0.0f;
    for (std::size_t k = 0; k < rows; ++k) {
        float wi = 0.0f;
        float wo = 0.0f;
        if (k < n.inputs.size()) {
            const PinView& p = n.inputs[k];
            wi = 14.0f + (p.hide_label || compact ? 0.0f : ImGui::CalcTextSize(p.label.c_str()).x) + (p.inline_value ? 70.0f : 0.0f);
        }
        if (k < n.outputs.size()) {
            const PinView& p = n.outputs[k];
            wo = 14.0f + (p.hide_label || compact ? 0.0f : ImGui::CalcTextSize(p.label.c_str()).x);
            if (k == 0) first_out = wo;
        }
        row_w = std::max(row_w, wi + wo + 20.0f);
    }
    ImGui::PopFont();
    if (compact) {
        const std::string& sym = n.symbol.empty() ? n.title : n.symbol;
        ImGui::PushFont(nullptr, font * 1.5f);
        const float sym_w = ImGui::CalcTextSize(sym.c_str()).x;
        ImGui::PopFont();
        return std::max(70.0f, sym_w + 52.0f);
    }
    if (n.style == NodeStyle::Variable && n.inputs.empty()) {
        return std::max({80.0f, title_w + 22.0f + first_out + 12.0f, row_w});
    }
    const float icon = hasIcon(n.style) ? 18.0f : 0.0f;
    const float marks = (n.latent ? 16.0f : 0.0f) + (n.breakpoint ? 14.0f : 0.0f);
    return std::max({80.0f, title_w + 18.0f + icon + marks, subtitle_w + 18.0f + icon, row_w});
}

void bezierControls(ImVec2 a, ImVec2 b, bool vertical, float zoom, ImVec2& c1, ImVec2& c2) {
    if (vertical) {
        const float dy = b.y - a.y;
        float d = std::clamp(std::fabs(dy) * 0.5f, 30.0f * zoom, 250.0f * zoom);
        if (dy < 0.0f) d = std::clamp(std::fabs(dy) * 0.5f + std::fabs(b.x - a.x) * 0.25f + 60.0f * zoom, 60.0f * zoom, 250.0f * zoom);
        c1 = ImVec2(a.x, a.y + d);
        c2 = ImVec2(b.x, b.y - d);
    } else {
        const float dx = b.x - a.x;
        float d = std::clamp(std::fabs(dx) * 0.5f, 30.0f * zoom, 250.0f * zoom);
        // Hacia atras: tangentes largas para que el cable haga un bucle.
        if (dx < 0.0f) d = std::clamp(std::fabs(dx) * 0.5f + std::fabs(b.y - a.y) * 0.25f + 60.0f * zoom, 60.0f * zoom, 250.0f * zoom);
        c1 = ImVec2(a.x + d, a.y);
        c2 = ImVec2(b.x - d, b.y);
    }
}

void drawPinShape(ImDrawList* draw, ImVec2 p, float r, PinShape shape, ImU32 color, bool connected) {
    switch (shape) {
        case PinShape::Exec: {
            // Pentagono (flecha) como Unreal.
            const ImVec2 pts[5] = {ImVec2(p.x - r, p.y - r), ImVec2(p.x + r * 0.2f, p.y - r), ImVec2(p.x + r * 1.15f, p.y),
                                   ImVec2(p.x + r * 0.2f, p.y + r), ImVec2(p.x - r, p.y + r)};
            if (connected) draw->AddConvexPolyFilled(pts, 5, color);
            else draw->AddPolyline(pts, 5, color, ImDrawFlags_Closed, 1.5f);
            break;
        }
        case PinShape::Grid: {
            const float s = r * 0.5f;
            for (int y = -1; y <= 1; ++y) {
                for (int x = -1; x <= 1; ++x) {
                    const ImVec2 c(p.x + static_cast<float>(x) * s * 1.3f, p.y + static_cast<float>(y) * s * 1.3f);
                    const ImVec2 a(c.x - s * 0.5f, c.y - s * 0.5f);
                    const ImVec2 b(c.x + s * 0.5f, c.y + s * 0.5f);
                    if (connected || (x == 0 && y == 0)) draw->AddRectFilled(a, b, color);
                    else draw->AddRect(a, b, color);
                }
            }
            break;
        }
        case PinShape::Diamond: {
            const ImVec2 pts[4] = {ImVec2(p.x, p.y - r * 1.15f), ImVec2(p.x + r * 1.15f, p.y), ImVec2(p.x, p.y + r * 1.15f),
                                   ImVec2(p.x - r * 1.15f, p.y)};
            if (connected) draw->AddConvexPolyFilled(pts, 4, color);
            else draw->AddPolyline(pts, 4, color, ImDrawFlags_Closed, 1.5f);
            break;
        }
        case PinShape::Square: {
            const ImVec2 a(p.x - r * 0.9f, p.y - r * 0.9f);
            const ImVec2 b(p.x + r * 0.9f, p.y + r * 0.9f);
            if (connected) draw->AddRectFilled(a, b, color);
            else draw->AddRect(a, b, color, 0.0f, 0, 1.5f);
            break;
        }
        case PinShape::Circle:
        default:
            if (connected) {
                draw->AddCircleFilled(p, r, color);
            } else {
                draw->AddCircleFilled(p, r, IM_COL32(24, 24, 28, 255));
                draw->AddCircle(p, r, color, 0, 1.6f);
            }
            break;
    }
}

// Burbuja de texto (valor fijo de un pin/cable o comentario de nodo).
void drawBubble(ImDrawList* draw, ImVec2 anchor, const std::string& text, int side, float zoom, float font, ImU32 fill,
                ImU32 text_color) {
    ImGui::PushFont(nullptr, font * std::clamp(zoom * 0.8f, 0.45f, 1.2f));
    const ImVec2 ts = ImGui::CalcTextSize(text.c_str());
    const float pad = 4.0f * zoom;
    ImVec2 min;
    if (side > 0) min = ImVec2(anchor.x + 8.0f * zoom, anchor.y - ts.y * 0.5f - pad);           // a la derecha
    else if (side < 0) min = ImVec2(anchor.x - 8.0f * zoom - ts.x - pad * 2.0f, anchor.y - ts.y * 0.5f - pad);  // izquierda
    else min = ImVec2(anchor.x - ts.x * 0.5f - pad, anchor.y - ts.y * 0.5f - pad);                // centrada
    const ImVec2 max(min.x + ts.x + pad * 2.0f, min.y + ts.y + pad * 2.0f);
    draw->AddRectFilled(min, max, fill, 4.0f * zoom);
    draw->AddRect(min, max, IM_COL32(0, 0, 0, 160), 4.0f * zoom);
    draw->AddText(ImVec2(min.x + pad, min.y + pad), text_color, text.c_str());
    ImGui::PopFont();
}

// Sin tildes ni mayusculas (UTF-8 de las vocales espanolas).
std::string fold(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c == 0xC3 && i + 1 < text.size()) {
            const unsigned char d = static_cast<unsigned char>(text[i + 1]);
            char r = 0;
            switch (d) {
                case 0xA1: case 0x81: r = 'a'; break;
                case 0xA9: case 0x89: r = 'e'; break;
                case 0xAD: case 0x8D: r = 'i'; break;
                case 0xB3: case 0x93: r = 'o'; break;
                case 0xBA: case 0x9A: case 0xBC: case 0x9C: r = 'u'; break;
                case 0xB1: case 0x91: r = 'n'; break;
                default: break;
            }
            if (r != 0) {
                out.push_back(r);
                ++i;
                continue;
            }
        }
        out.push_back(static_cast<char>(std::tolower(c)));
    }
    return out;
}

}  // namespace

bool fuzzyMatch(const std::string& text, const std::string& filter) {
    const std::string hay = fold(text);
    const std::string needle = fold(filter);
    std::size_t start = 0;
    while (start < needle.size()) {
        while (start < needle.size() && needle[start] == ' ') ++start;
        std::size_t end = needle.find(' ', start);
        if (end == std::string::npos) end = needle.size();
        if (end > start && hay.find(needle.substr(start, end - start)) == std::string::npos) return false;
        start = end;
    }
    return true;
}

core::Vec2 Canvas::toCanvas(ImVec2 screen) const {
    return core::Vec2{(screen.x - origin_.x - pan.x) / zoom, (screen.y - origin_.y - pan.y) / zoom};
}

ImVec2 Canvas::toScreen(core::Vec2 canvas) const {
    return ImVec2(origin_.x + pan.x + canvas.x * zoom, origin_.y + pan.y + canvas.y * zoom);
}

core::Vec2 Canvas::viewCenter() const { return toCanvas(ImVec2(origin_.x + size_.x * 0.5f, origin_.y + size_.y * 0.5f)); }

void Canvas::select(int id, bool add_to) {
    if (!add_to) selection.clear();
    if (id != 0) selection.insert(id);
}

void Canvas::frame(const std::vector<NodeView>& nodes) {
    float min_x = 1e30f, min_y = 1e30f, max_x = -1e30f, max_y = -1e30f;
    int count = 0;
    for (const NodeView& n : nodes) {
        if (n.position == nullptr) continue;
        if (!selection.empty() && !selection.contains(n.id)) continue;
        const auto g = geom_.find(n.id);
        const float w = g != geom_.end() ? g->second.size.x : n.width;
        const float h = g != geom_.end() ? g->second.size.y : 120.0f;
        min_x = std::min(min_x, n.position->x);
        min_y = std::min(min_y, n.position->y);
        max_x = std::max(max_x, n.position->x + w);
        max_y = std::max(max_y, n.position->y + h);
        ++count;
    }
    if (count == 0 || size_.x <= 1.0f) {
        pending_frame_ = count > 0;
        return;
    }
    const float w = std::max(max_x - min_x, 1.0f);
    const float h = std::max(max_y - min_y, 1.0f);
    zoom = std::clamp(std::min(size_.x / (w + 120.0f), size_.y / (h + 120.0f)), std::max(0.3f, min_zoom), std::min(1.2f, max_zoom));
    pan.x = size_.x * 0.5f - (min_x + w * 0.5f) * zoom;
    pan.y = size_.y * 0.5f - (min_y + h * 0.5f) * zoom;
}

void Canvas::frameNode(int id) { pending_frame_node_ = id; }

bool Canvas::nodeRect(int id, core::Vec2& min, core::Vec2& max) const {
    const auto it = geom_.find(id);
    if (it == geom_.end()) return false;
    min = it->second.pos;
    max = core::Vec2{it->second.pos.x + it->second.size.x, it->second.pos.y + it->second.size.y};
    return true;
}

bool Canvas::selectionBounds(core::Vec2& min, core::Vec2& max) const {
    bool any = false;
    for (const int id : selection) {
        core::Vec2 a, b;
        if (!nodeRect(id, a, b)) continue;
        if (!any) {
            min = a;
            max = b;
            any = true;
        } else {
            min = core::Vec2{std::min(min.x, a.x), std::min(min.y, a.y)};
            max = core::Vec2{std::max(max.x, b.x), std::max(max.y, b.y)};
        }
    }
    return any;
}

void Canvas::align(AlignOp op, std::vector<NodeView>& nodes, const std::vector<LinkView>& links) {
    std::map<int, NodeView*> by_id;
    for (NodeView& n : nodes) {
        if (n.position != nullptr) by_id[n.id] = &n;
    }
    const auto sizeOf = [&](const NodeView& n) {
        const auto g = geom_.find(n.id);
        return g != geom_.end() ? g->second.size : core::Vec2{n.width, 80.0f};
    };
    std::vector<NodeView*> items;
    for (NodeView& n : nodes) {
        if (n.position != nullptr && !n.comment && selection.contains(n.id)) items.push_back(&n);
    }

    if (op == AlignOp::Straighten) {
        // Sin nodos elegidos: los destinos de los cables elegidos.
        if (items.empty()) {
            for (const LinkView& l : links) {
                if (l.id == 0 || !link_selection.contains(l.id)) continue;
                const auto it = by_id.find(l.to_node);
                if (it != by_id.end() && std::find(items.begin(), items.end(), it->second) == items.end()) items.push_back(it->second);
            }
        }
        std::sort(items.begin(), items.end(), [](const NodeView* a, const NodeView* b) { return a->position->x < b->position->x; });
        const auto pinY = [&](int node, int pin, bool output) -> std::optional<float> {
            const auto g = geom_.find(node);
            if (g == geom_.end()) return std::nullopt;
            const auto& list = output ? g->second.out : g->second.in;
            if (pin < 0 || static_cast<std::size_t>(pin) >= list.size()) return std::nullopt;
            return list[static_cast<std::size_t>(pin)];
        };
        for (NodeView* n : items) {
            // Primero la entrada (prefiere exec); si no hay, una salida.
            const LinkView* best = nullptr;
            for (const LinkView& l : links) {
                if (l.to_node != n->id || l.from_node == n->id || !by_id.contains(l.from_node)) continue;
                if (best == nullptr || (l.exec && !best->exec)) best = &l;
            }
            if (best != nullptr) {
                const auto a = pinY(best->from_node, best->from_pin, true);
                const auto b = pinY(n->id, best->to_pin, false);
                if (a && b) n->position->y = by_id[best->from_node]->position->y + *a - *b;
                continue;
            }
            for (const LinkView& l : links) {
                if (l.from_node != n->id || l.to_node == n->id || !by_id.contains(l.to_node)) continue;
                if (best == nullptr || (l.exec && !best->exec)) best = &l;
            }
            if (best != nullptr) {
                const auto a = pinY(best->to_node, best->to_pin, false);
                const auto b = pinY(n->id, best->from_pin, true);
                if (a && b) n->position->y = by_id[best->to_node]->position->y + *a - *b;
            }
        }
        return;
    }

    if (items.size() < 2) return;
    float lo_x = 1e30f, lo_y = 1e30f, hi_x = -1e30f, hi_y = -1e30f, sum_cx = 0.0f, sum_cy = 0.0f;
    for (const NodeView* n : items) {
        const core::Vec2 s = sizeOf(*n);
        lo_x = std::min(lo_x, n->position->x);
        lo_y = std::min(lo_y, n->position->y);
        hi_x = std::max(hi_x, n->position->x + s.x);
        hi_y = std::max(hi_y, n->position->y + s.y);
        sum_cx += n->position->x + s.x * 0.5f;
        sum_cy += n->position->y + s.y * 0.5f;
    }
    const float count = static_cast<float>(items.size());
    switch (op) {
        case AlignOp::Top:
            for (NodeView* n : items) n->position->y = lo_y;
            break;
        case AlignOp::Bottom:
            for (NodeView* n : items) n->position->y = hi_y - sizeOf(*n).y;
            break;
        case AlignOp::Middle:
            for (NodeView* n : items) n->position->y = sum_cy / count - sizeOf(*n).y * 0.5f;
            break;
        case AlignOp::Left:
            for (NodeView* n : items) n->position->x = lo_x;
            break;
        case AlignOp::Right:
            for (NodeView* n : items) n->position->x = hi_x - sizeOf(*n).x;
            break;
        case AlignOp::Center:
            for (NodeView* n : items) n->position->x = sum_cx / count - sizeOf(*n).x * 0.5f;
            break;
        case AlignOp::DistributeH:
        case AlignOp::DistributeV: {
            if (items.size() < 3) break;
            const bool h = op == AlignOp::DistributeH;
            std::sort(items.begin(), items.end(), [h](const NodeView* a, const NodeView* b) {
                return h ? a->position->x < b->position->x : a->position->y < b->position->y;
            });
            float total = 0.0f;
            for (const NodeView* n : items) total += h ? sizeOf(*n).x : sizeOf(*n).y;
            const float span = h ? hi_x - lo_x : hi_y - lo_y;
            const float gap = (span - total) / (count - 1.0f);
            float at = h ? lo_x : lo_y;
            for (NodeView* n : items) {
                if (h) n->position->x = at;
                else n->position->y = at;
                at += (h ? sizeOf(*n).x : sizeOf(*n).y) + gap;
            }
            break;
        }
        default: break;
    }
}

Events Canvas::draw(const char* id, std::vector<NodeView>& nodes, const std::vector<LinkView>& links, ImVec2 size,
                    const InlineEditor* inline_editor, const BodyDrawer* body_drawer) {
    DrawOptions options;
    options.inline_editor = inline_editor;
    options.body = body_drawer;
    options.minimap = false;
    options.keys = false;
    options.extended = false;
    return draw(id, nodes, links, size, options);
}

Events Canvas::draw(const char* id, std::vector<NodeView>& nodes, const std::vector<LinkView>& links, ImVec2 size,
                    const DrawOptions& options) {
    Events ev;
    const InlineEditor* inline_editor = options.inline_editor;
    const BodyDrawer* body_drawer = options.body;
    const bool extended = options.extended;
    ImGui::BeginChild(id, size, ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoMove);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    origin_ = ImGui::GetCursorScreenPos();
    size_ = ImGui::GetContentRegionAvail();
    size_.x = std::max(size_.x, 1.0f);
    size_.y = std::max(size_.y, 1.0f);
    if (pending_frame_) {
        pending_frame_ = false;
        frame(nodes);
    }
    if (pending_frame_node_ != 0) {
        for (const NodeView& n : nodes) {
            if (n.id != pending_frame_node_ || n.position == nullptr) continue;
            const auto g = geom_.find(n.id);
            const core::Vec2 s = g != geom_.end() ? g->second.size : core::Vec2{n.width, 80.0f};
            zoom = std::clamp(std::max(zoom, 0.8f), min_zoom, max_zoom);
            pan.x = size_.x * 0.5f - (n.position->x + s.x * 0.5f) * zoom;
            pan.y = size_.y * 0.5f - (n.position->y + s.y * 0.5f) * zoom;
        }
        pending_frame_node_ = 0;
    }
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##canvas", size_,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    const bool canvas_active = ImGui::IsItemActive();
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mouse = io.MousePos;
    const bool inside = mouse.x >= origin_.x && mouse.y >= origin_.y && mouse.x < origin_.x + size_.x &&
                        mouse.y < origin_.y + size_.y;
    const bool window_hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) && inside;
    const float font = ImGui::GetStyle().FontSizeBase;
    const float lod = zoom;  // umbrales 0.6 / 0.45 / 0.3 / 0.18

    // Minimapa (abajo a la derecha).
    const bool show_minimap = options.minimap && minimap && !nodes.empty() && size_.x > kMinimapW * 1.8f && size_.y > kMinimapH * 1.8f;
    const ImVec2 mm_min(origin_.x + size_.x - kMinimapW - 10.0f, origin_.y + size_.y - kMinimapH - 10.0f);
    const ImVec2 mm_max(mm_min.x + kMinimapW, mm_min.y + kMinimapH);
    const bool in_minimap = show_minimap && window_hovered && mouse.x >= mm_min.x && mouse.y >= mm_min.y &&
                            mouse.x <= mm_max.x && mouse.y <= mm_max.y;

    draw->PushClipRect(origin_, add(origin_, size_), true);
    draw->AddRectFilled(origin_, add(origin_, size_), kCanvasBg);
    // Rejilla de 16 y de 128.
    const auto grid = [&](float step, ImU32 color) {
        for (float x = std::fmod(pan.x, step); x < size_.x; x += step) {
            draw->AddLine(ImVec2(origin_.x + x, origin_.y), ImVec2(origin_.x + x, origin_.y + size_.y), color);
        }
        for (float y = std::fmod(pan.y, step); y < size_.y; y += step) {
            draw->AddLine(ImVec2(origin_.x, origin_.y + y), ImVec2(origin_.x + size_.x, origin_.y + y), color);
        }
    };
    if (kGridSmall * zoom > 7.0f) grid(kGridSmall * zoom, kGridMinor);
    if (kGridBig * zoom > 12.0f) grid(kGridBig * zoom, kGridMajor);

    // --- Disposicion ---
    std::vector<Layout> layout(nodes.size());
    std::map<int, std::size_t> index;
    const float row = kRow * zoom;
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        NodeView& n = nodes[i];
        index[n.id] = i;
        if (n.position == nullptr) continue;
        Layout& l = layout[i];
        l.min = toScreen(*n.position);
        NodeGeom& g = geom_[n.id];
        g.pos = *n.position;
        if (n.comment) {
            const core::Vec2 s = n.comment_size != nullptr ? *n.comment_size : core::Vec2{300.0f, 200.0f};
            l.max = ImVec2(l.min.x + s.x * zoom, l.min.y + s.y * zoom);
            l.header = kHeader * zoom;
            g.size = s;
            g.in.clear();
            g.out.clear();
            continue;
        }
        const bool reroute = n.style == NodeStyle::Reroute && !vertical;
        float width_units = n.width;
        if (!vertical && !reroute) {
            const std::size_t sig = nodeSignature(n);
            if (sig != g.signature || g.measured <= 0.0f) {
                g.signature = sig;
                g.measured = measureNode(n, font);
            }
            width_units = n.style == NodeStyle::Compact || n.style == NodeStyle::Variable ? g.measured : std::max(n.width, g.measured);
        }
        l.header = headerUnits(n, vertical) * zoom;
        l.strip = stripUnits(n) * zoom;
        const float w = width_units * zoom;
        if (reroute) {
            l.max = ImVec2(l.min.x + kRerouteW * zoom, l.min.y + kRerouteH * zoom);
            const float cy = (l.min.y + l.max.y) * 0.5f;
            for (std::size_t p = 0; p < n.inputs.size(); ++p) l.in.push_back(ImVec2(l.min.x + 3.0f * zoom, cy));
            for (std::size_t p = 0; p < n.outputs.size(); ++p) l.out.push_back(ImVec2(l.max.x - 3.0f * zoom, cy));
        } else if (vertical) {
            const float body_lines = n.body.empty() ? 0.0f : 1.0f;
            const float h = l.header + (body_lines * 18.0f + 14.0f + n.extra_height) * zoom + l.strip;
            l.max = ImVec2(l.min.x + w, l.min.y + h);
            for (std::size_t p = 0; p < n.inputs.size(); ++p) {
                l.in.push_back(ImVec2(l.min.x + w * (static_cast<float>(p) + 1.0f) / (static_cast<float>(n.inputs.size()) + 1.0f), l.min.y));
            }
            for (std::size_t p = 0; p < n.outputs.size(); ++p) {
                l.out.push_back(ImVec2(l.min.x + w * (static_cast<float>(p) + 1.0f) / (static_cast<float>(n.outputs.size()) + 1.0f), l.max.y));
            }
        } else {
            l.top = l.header <= 0.0f ? 4.0f * zoom : 0.0f;
            const std::size_t rows = std::max(n.inputs.size(), n.outputs.size());
            float h = l.header + l.top + static_cast<float>(rows) * row + 6.0f * zoom + n.extra_height * zoom + l.strip;
            if (!n.body.empty()) h += 18.0f * zoom;
            if (n.style == NodeStyle::Compact) h = std::max(h, 30.0f * zoom + l.strip);
            l.max = ImVec2(l.min.x + w, l.min.y + h);
            for (std::size_t p = 0; p < n.inputs.size(); ++p) {
                l.in.push_back(ImVec2(l.min.x, l.min.y + l.header + l.top + row * (static_cast<float>(p) + 0.5f)));
            }
            for (std::size_t p = 0; p < n.outputs.size(); ++p) {
                l.out.push_back(ImVec2(l.max.x, l.min.y + l.header + l.top + row * (static_cast<float>(p) + 0.5f)));
            }
        }
        g.size = core::Vec2{(l.max.x - l.min.x) / zoom, (l.max.y - l.min.y) / zoom};
        g.in.resize(l.in.size());
        g.out.resize(l.out.size());
        for (std::size_t p = 0; p < l.in.size(); ++p) g.in[p] = (l.in[p].y - l.min.y) / zoom;
        for (std::size_t p = 0; p < l.out.size(); ++p) g.out[p] = (l.out[p].y - l.min.y) / zoom;
    }
    std::erase_if(geom_, [&](const auto& kv) { return !index.contains(kv.first); });
    // Cables elegidos que ya no existen.
    if (!link_selection.empty()) {
        std::set<int> alive;
        for (const LinkView& l : links) {
            if (l.id != 0 && link_selection.contains(l.id)) alive.insert(l.id);
        }
        link_selection = std::move(alive);
    }

    const auto pinAt = [&](ImVec2 p) -> std::optional<PinRef> {
        std::optional<PinRef> best;
        float best_d = 1e30f;
        for (std::size_t i = nodes.size(); i-- > 0;) {
            if (nodes[i].comment || nodes[i].position == nullptr) continue;
            const bool reroute = nodes[i].style == NodeStyle::Reroute && !vertical;
            const float rr = reroute ? std::max(5.0f * zoom, 3.0f) : std::max(9.0f * zoom, 6.0f);
            const float r2 = rr * rr;
            const auto test = [&](const std::vector<ImVec2>& list, bool output) {
                for (std::size_t k = 0; k < list.size(); ++k) {
                    float d = dist2(p, list[k]);
                    if (d > r2) continue;
                    // Al arrastrar, prefiere los pines del otro lado (o del mismo al mover enlaces).
                    if (link_from_ && (relinking_ ? output != link_from_->output : output == link_from_->output)) d += 1e6f;
                    if (d < best_d) {
                        best_d = d;
                        best = PinRef{nodes[i].id, static_cast<int>(k), output};
                    }
                }
            };
            test(layout[i].in, false);
            test(layout[i].out, true);
        }
        return best;
    };
    const auto nodeAt = [&](ImVec2 p, bool comments) -> int {
        for (std::size_t i = nodes.size(); i-- > 0;) {
            if (nodes[i].position == nullptr || nodes[i].comment) continue;
            const Layout& l = layout[i];
            if (p.x >= l.min.x && p.y >= l.min.y && p.x <= l.max.x && p.y <= l.max.y) return nodes[i].id;
        }
        if (comments) {
            // Los comentarios se cogen por su cabecera.
            for (std::size_t i = nodes.size(); i-- > 0;) {
                if (nodes[i].position == nullptr || !nodes[i].comment) continue;
                const Layout& l = layout[i];
                if (p.x >= l.min.x && p.y >= l.min.y && p.x <= l.max.x && p.y <= l.min.y + l.header) return nodes[i].id;
                // Y por la esquina de abajo a la derecha (cambiar el tamano).
                if (p.x <= l.max.x && p.y <= l.max.y && dist2(p, l.max) < 400.0f) return nodes[i].id;
            }
        }
        return 0;
    };
    const auto pinPosition = [&](int node, int pin, bool output) -> std::optional<ImVec2> {
        const auto it = index.find(node);
        if (it == index.end()) return std::nullopt;
        const Layout& l = layout[it->second];
        const auto& list = output ? l.out : l.in;
        if (pin < 0 || static_cast<std::size_t>(pin) >= list.size()) return std::nullopt;
        return list[static_cast<std::size_t>(pin)];
    };
    const auto pinView = [&](const PinRef& ref) -> const PinView* {
        const auto it = index.find(ref.node);
        if (it == index.end()) return nullptr;
        const auto& list = ref.output ? nodes[it->second].outputs : nodes[it->second].inputs;
        if (ref.pin < 0 || static_cast<std::size_t>(ref.pin) >= list.size()) return nullptr;
        return &list[static_cast<std::size_t>(ref.pin)];
    };
    const auto linkAt = [&](ImVec2 p) -> int {
        const float thr = 6.0f;
        float best = thr * thr;
        int found = -1;
        for (std::size_t i = 0; i < links.size(); ++i) {
            const auto a = pinPosition(links[i].from_node, links[i].from_pin, true);
            const auto b = pinPosition(links[i].to_node, links[i].to_pin, false);
            if (!a || !b) continue;
            ImVec2 c1, c2;
            bezierControls(*a, *b, vertical, zoom, c1, c2);
            const float x0 = std::min({a->x, b->x, c1.x, c2.x}) - thr;
            const float x1 = std::max({a->x, b->x, c1.x, c2.x}) + thr;
            const float y0 = std::min({a->y, b->y, c1.y, c2.y}) - thr;
            const float y1 = std::max({a->y, b->y, c1.y, c2.y}) + thr;
            if (p.x < x0 || p.x > x1 || p.y < y0 || p.y > y1) continue;
            ImVec2 prev = *a;
            for (int s = 1; s <= 24; ++s) {
                const ImVec2 q = ImBezierCubicCalc(*a, c1, c2, *b, static_cast<float>(s) / 24.0f);
                const float d = segmentDist2(p, prev, q);
                if (d < best) {
                    best = d;
                    found = static_cast<int>(i);
                }
                prev = q;
            }
        }
        return found;
    };
    // Compatibilidad del pin destino con el que se arrastra (en cache durante el arrastre).
    const auto checkTarget = [&](const PinRef& target) -> LinkCheck {
        if (options.can_link == nullptr || !link_from_ || relinking_) return LinkCheck{true, false, {}};
        if (target.node == link_from_->node || target.output == link_from_->output) return LinkCheck{false, false, {}};
        const auto key = std::make_pair(target.node, (target.pin << 1) | (target.output ? 1 : 0));
        const auto it = link_checks_.find(key);
        if (it != link_checks_.end()) return it->second;
        const PinRef out = link_from_->output ? *link_from_ : target;
        const PinRef in = link_from_->output ? target : *link_from_;
        LinkCheck check = (*options.can_link)(out, in);
        link_checks_.emplace(key, check);
        return check;
    };

    // Lo que hay bajo el raton (antes de dibujar para resaltarlo).
    const bool pre_interact = window_hovered && !in_minimap && !(ImGui::IsAnyItemActive() && !canvas_active);
    std::optional<PinRef> hovered_pin = pre_interact ? pinAt(mouse) : std::nullopt;
    int hovered_node = pre_interact && !hovered_pin ? nodeAt(mouse, true) : 0;
    int hovered_link = extended && pre_interact && !hovered_pin && hovered_node == 0 && !link_from_ && !dragging_nodes_ && !box_selecting_
                           ? linkAt(mouse)
                           : -1;

    // --- Comentarios (al fondo) ---
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        const NodeView& n = nodes[i];
        if (!n.comment || n.position == nullptr) continue;
        const Layout& l = layout[i];
        const bool selected = selection.contains(n.id);
        const ImU32 base = n.comment_color != 0 ? n.comment_color : n.header;
        const float rounding = 6.0f * zoom;
        draw->AddRectFilled(l.min, l.max, (base & 0x00FFFFFFu) | 0x30000000u, rounding);
        draw->AddRectFilled(l.min, ImVec2(l.max.x, l.min.y + l.header), (base & 0x00FFFFFFu) | 0x90000000u, rounding,
                            ImDrawFlags_RoundCornersTop);
        draw->AddRect(l.min, l.max, selected ? theme::kYellow : ((base & 0x00FFFFFFu) | 0xA0000000u), rounding, 0,
                      selected ? 2.0f : 1.0f);
        if (lod >= 0.18f) {
            ImGui::PushFont(nullptr, font * std::clamp(zoom, 0.5f, 1.6f));
            draw->AddText(ImVec2(l.min.x + 8.0f * zoom, l.min.y + 4.0f * zoom), IM_COL32(240, 240, 240, 255), n.title.c_str());
            ImGui::PopFont();
        }
        // Esquina para cambiar el tamano.
        draw->AddTriangleFilled(ImVec2(l.max.x - 12.0f * zoom, l.max.y), l.max, ImVec2(l.max.x, l.max.y - 12.0f * zoom),
                                IM_COL32(255, 255, 255, 90));
    }

    // --- Enlaces ---
    const auto bezier = [&](ImVec2 a, ImVec2 b, ImU32 color, float thickness, float flow) {
        ImVec2 c1, c2;
        bezierControls(a, b, vertical, zoom, c1, c2);
        draw->AddBezierCubic(a, c1, c2, b, color, thickness);
        if (flow > 0.0f) {
            const float t0 = static_cast<float>(std::fmod(ImGui::GetTime() * 1.2, 1.0));
            for (int k = 0; k < 4; ++k) {
                const float t = std::fmod(t0 + static_cast<float>(k) * 0.25f, 1.0f);
                const ImVec2 p = ImBezierCubicCalc(a, c1, c2, b, t);
                draw->AddCircleFilled(p, 3.0f * zoom, IM_COL32(255, 220, 120, static_cast<int>(255 * flow)));
            }
        }
    };
    for (std::size_t i = 0; i < links.size(); ++i) {
        const LinkView& link = links[i];
        const auto a = pinPosition(link.from_node, link.from_pin, true);
        const auto b = pinPosition(link.to_node, link.to_pin, false);
        if (!a || !b) continue;
        float thickness = std::max((link.exec ? 3.2f : 2.0f) * zoom, 1.0f);
        ImU32 color = link.flow > 0.0f ? scaleColor(link.color, 1.0f + link.flow * 0.6f) : link.color;
        const bool selected = link.id != 0 && link_selection.contains(link.id);
        if (selected) bezier(*a, *b, theme::withAlpha(theme::kYellow, 200), thickness + 3.0f, 0.0f);
        if (static_cast<int>(i) == hovered_link) {
            thickness += 1.5f;
            color = scaleColor(color, 1.3f);
        }
        bezier(*a, *b, color, thickness, link.flow);
        if (!link.watch.empty() && lod >= 0.45f) {
            ImVec2 c1, c2;
            bezierControls(*a, *b, vertical, zoom, c1, c2);
            drawBubble(draw, ImBezierCubicCalc(*a, c1, c2, *b, 0.5f), link.watch, 0, zoom, font, IM_COL32(40, 40, 46, 235),
                       IM_COL32(235, 235, 200, 255));
        }
    }
    // Cable que se esta arrastrando.
    std::optional<LinkCheck> drag_check;
    if (link_from_) {
        const PinView* from_pin = pinView(*link_from_);
        ImU32 color = from_pin != nullptr ? (from_pin->exec ? kExecColor : from_pin->color) : IM_COL32(230, 230, 230, 255);
        const std::optional<PinRef> target = window_hovered ? pinAt(mouse) : std::nullopt;
        // Solo pines del otro lado y de otro nodo (al empezar, el raton sigue sobre el pin de origen).
        if (target && options.can_link != nullptr && !relinking_ && target->output != link_from_->output &&
            target->node != link_from_->node) {
            drag_check = checkTarget(*target);
            if (!drag_check->ok) color = theme::kRed;
            else if (drag_check->converts) color = theme::kYellow;
        }
        const float thickness = std::max(2.5f * zoom, 1.5f);
        if (relinking_) {
            // Se mueven todos los enlaces de este pin: fantasmas desde el otro extremo.
            for (const LinkView& l : links) {
                if (link_from_->output && l.from_node == link_from_->node && l.from_pin == link_from_->pin) {
                    if (const auto b = pinPosition(l.to_node, l.to_pin, false)) bezier(mouse, *b, theme::withAlpha(color, 170), thickness, 0.0f);
                } else if (!link_from_->output && l.to_node == link_from_->node && l.to_pin == link_from_->pin) {
                    if (const auto a = pinPosition(l.from_node, l.from_pin, true)) bezier(*a, mouse, theme::withAlpha(color, 170), thickness, 0.0f);
                }
            }
        } else if (const auto p = pinPosition(link_from_->node, link_from_->pin, link_from_->output)) {
            if (link_from_->output) bezier(*p, mouse, color, thickness, 0.0f);
            else bezier(mouse, *p, color, thickness, 0.0f);
        }
    }

    // --- Nodos ---
    bool inline_hovered = false;
    const bool highlight_targets = link_from_.has_value() && options.can_link != nullptr && !relinking_;
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        NodeView& n = nodes[i];
        if (n.comment || n.position == nullptr) continue;
        const Layout& l = layout[i];
        if (l.max.x < origin_.x - 50.0f || l.min.x > origin_.x + size_.x + 50.0f || l.max.y < origin_.y - 80.0f ||
            l.min.y > origin_.y + size_.y + 50.0f) {
            continue;  // fuera de la vista
        }
        const bool selected = selection.contains(n.id);
        const bool hovered = n.id == hovered_node;
        const bool reroute = n.style == NodeStyle::Reroute && !vertical;
        const bool compact = n.style == NodeStyle::Compact && !vertical;
        const float rounding = reroute ? (l.max.y - l.min.y) * 0.5f : 8.0f * zoom;
        const int vtx_start = draw->VtxBuffer.Size;

        // LOD minimo: solo un bloque de color.
        if (lod < 0.18f) {
            draw->AddRectFilled(l.min, l.max, theme::withAlpha(n.header, n.disabled ? 90 : 230), 2.0f);
            if (selected) draw->AddRect(l.min, l.max, theme::kYellow, 2.0f, 0, 2.0f);
            continue;
        }

        // Sombras, halo de ejecucion y cuerpo.
        draw->AddRectFilled(ImVec2(l.min.x - 1.0f * zoom, l.min.y + 2.0f * zoom), ImVec2(l.max.x + 2.0f * zoom, l.max.y + 6.0f * zoom),
                            IM_COL32(0, 0, 0, 40), rounding + 3.0f * zoom);
        draw->AddRectFilled(ImVec2(l.min.x + 1.0f * zoom, l.min.y + 1.0f * zoom), ImVec2(l.max.x + 1.5f * zoom, l.max.y + 3.0f * zoom),
                            IM_COL32(0, 0, 0, 80), rounding);
        if (n.highlight > 0.0f) {
            draw->AddRect(add(l.min, ImVec2(-4.0f, -4.0f)), add(l.max, ImVec2(4.0f, 4.0f)),
                          IM_COL32(255, 200, 70, static_cast<int>(220 * n.highlight)), rounding + 3.0f, 0, 3.0f);
        }
        if (n.active) {
            const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(ImGui::GetTime()) * 6.0f);
            draw->AddRect(add(l.min, ImVec2(-4.0f, -4.0f)), add(l.max, ImVec2(4.0f, 4.0f)),
                          IM_COL32(90, 170, 255, static_cast<int>(120 + 135 * pulse)), rounding + 3.0f, 0, 3.0f);
        }

        if (reroute) {
            // Reroute: una bolita del color del cable.
            const ImU32 c = !n.inputs.empty() ? (n.inputs[0].exec ? kExecColor : n.inputs[0].color)
                                              : (!n.outputs.empty() ? (n.outputs[0].exec ? kExecColor : n.outputs[0].color) : n.header);
            const ImVec2 center((l.min.x + l.max.x) * 0.5f, (l.min.y + l.max.y) * 0.5f);
            draw->AddRectFilled(l.min, l.max, IM_COL32(18, 18, 20, hovered || selected ? 200 : 90), rounding);
            draw->AddCircleFilled(center, 5.5f * zoom, c);
            draw->AddCircle(center, 5.5f * zoom, IM_COL32(0, 0, 0, 200), 0, 1.0f);
            if (selected) draw->AddRect(add(l.min, ImVec2(-1.5f, -1.5f)), add(l.max, ImVec2(1.5f, 1.5f)), theme::kYellow, rounding + 1.5f, 0, 2.0f);
            if (n.disabled) scaleAlpha(draw, vtx_start, 0.4f);
            continue;
        }

        const float bottom = l.max.y - l.strip;  // fin del contenido (encima de la franja)
        draw->AddRectFilled(l.min, l.max, kBody, rounding);
        if (n.style == NodeStyle::Variable) {
            // Pildora con el color del tipo que se apaga hacia la derecha.
            const int start = draw->VtxBuffer.Size;
            draw->AddRectFilled(l.min, l.max, IM_COL32(255, 255, 255, 120), rounding);
            ImGui::ShadeVertsLinearColorGradientKeepAlpha(draw, start, draw->VtxBuffer.Size, l.min, ImVec2(l.max.x, l.min.y),
                                                          scaleColor(n.header, 0.9f), kBody);
        }
        if (l.header > 0.0f) {
            const ImVec2 hmax(l.max.x, l.min.y + l.header);
            const int start = draw->VtxBuffer.Size;
            draw->AddRectFilled(l.min, hmax, n.header, rounding, ImDrawFlags_RoundCornersTop);
            const ImU32 end = n.header2 != 0 ? n.header2 : scaleColor(n.header, 0.35f);
            ImGui::ShadeVertsLinearColorGradientKeepAlpha(draw, start, draw->VtxBuffer.Size, l.min, ImVec2(l.max.x, l.min.y), n.header, end);
            // Brillo arriba y separacion abajo.
            draw->AddLine(ImVec2(l.min.x + rounding, l.min.y + 1.0f), ImVec2(l.max.x - rounding, l.min.y + 1.0f), IM_COL32(255, 255, 255, 55));
            draw->AddLine(ImVec2(l.min.x, hmax.y), ImVec2(l.max.x, hmax.y), IM_COL32(0, 0, 0, 140));
        }
        ImU32 border = hovered ? IM_COL32(255, 255, 255, 70) : IM_COL32(0, 0, 0, 200);
        float border_w = 1.0f;
        if (!n.error.empty()) {
            border = theme::kRed;
            border_w = 2.0f;
        }
        draw->AddRect(l.min, l.max, border, rounding, 0, border_w);
        if (selected) {
            draw->AddRect(add(l.min, ImVec2(-1.5f, -1.5f)), add(l.max, ImVec2(1.5f, 1.5f)), theme::kYellow, rounding + 1.5f, 0, 2.0f);
        }
        if (n.status_color != 0) {
            draw->AddRectFilled(ImVec2(l.min.x + 3.0f, bottom - 4.0f * zoom), ImVec2(l.max.x - 3.0f, bottom - 1.0f), n.status_color, 2.0f);
        }

        // Cabecera: icono, titulo, subtitulo, reloj y punto de ruptura.
        const float hcy = l.min.y + std::min(l.header, kHeader * zoom) * 0.5f;  // centro de la primera linea
        float mark_x = l.max.x - 9.0f * zoom;
        if (n.latent && lod >= 0.3f) {
            const ImVec2 c(mark_x, hcy);
            const float rr = 5.0f * zoom;
            draw->AddCircle(c, rr, IM_COL32(235, 235, 235, 230), 0, 1.3f);
            draw->AddLine(c, ImVec2(c.x, c.y - rr * 0.7f), IM_COL32(235, 235, 235, 230), 1.3f);
            draw->AddLine(c, ImVec2(c.x + rr * 0.55f, c.y), IM_COL32(235, 235, 235, 230), 1.3f);
            mark_x -= 14.0f * zoom;
        }
        if (n.breakpoint) {
            const float by = l.header > 0.0f ? hcy : l.min.y + 6.0f * zoom;
            draw->AddCircleFilled(ImVec2(mark_x, by), 5.0f * zoom, IM_COL32(230, 50, 50, 255));
            mark_x -= 12.0f * zoom;
        }
        if (lod >= 0.3f) {
            ImGui::PushFont(nullptr, font * std::clamp(zoom, 0.5f, 1.6f));
            if (l.header > 0.0f) {
                float tx = l.min.x + 8.0f * zoom;
                if (hasIcon(n.style) && !vertical) {
                    const ImVec2 c(tx + 5.0f * zoom, hcy);
                    const float s = 5.0f * zoom;
                    const ImU32 ic = IM_COL32(240, 240, 240, 230);
                    switch (n.style) {
                        case NodeStyle::Event:
                            draw->AddCircle(c, s, ic, 0, 1.4f);
                            draw->AddTriangleFilled(ImVec2(c.x - s * 0.35f, c.y - s * 0.5f), ImVec2(c.x - s * 0.35f, c.y + s * 0.5f),
                                                    ImVec2(c.x + s * 0.55f, c.y), ic);
                            break;
                        case NodeStyle::Flow: {
                            const ImVec2 pts[4] = {ImVec2(c.x, c.y - s), ImVec2(c.x + s, c.y), ImVec2(c.x, c.y + s), ImVec2(c.x - s, c.y)};
                            draw->AddPolyline(pts, 4, ic, ImDrawFlags_Closed, 1.4f);
                            break;
                        }
                        case NodeStyle::Entry:
                            draw->AddTriangleFilled(ImVec2(c.x - s * 0.7f, c.y - s), ImVec2(c.x - s * 0.7f, c.y + s), ImVec2(c.x + s, c.y), ic);
                            break;
                        case NodeStyle::Result:
                            draw->AddRectFilled(ImVec2(c.x - s * 0.8f, c.y - s * 0.8f), ImVec2(c.x + s * 0.8f, c.y + s * 0.8f), ic, 1.0f);
                            break;
                        default: {
                            const ImVec2 fs = ImGui::CalcTextSize("f");
                            draw->AddText(ImVec2(c.x - fs.x * 0.5f, hcy - fs.y * 0.5f), IM_COL32(200, 220, 255, 255), "f");
                            break;
                        }
                    }
                    tx += 16.0f * zoom;
                }
                std::string title = n.title;
                const float max_title = mark_x + 6.0f * zoom - tx;
                while (title.size() > 3 && ImGui::CalcTextSize(title.c_str()).x > max_title) title.pop_back();
                if (title.size() != n.title.size()) title += "..";
                draw->AddText(ImVec2(tx, hcy - ImGui::GetTextLineHeight() * 0.5f), IM_COL32(250, 250, 250, 255), title.c_str());
                if (hasSubtitleLine(n) && !vertical) {
                    ImGui::PushFont(nullptr, font * std::clamp(zoom * 0.8f, 0.45f, 1.3f));
                    draw->AddText(ImVec2(tx, l.min.y + kHeader * zoom - 2.0f * zoom), IM_COL32(200, 200, 210, 200), n.subtitle.c_str());
                    ImGui::PopFont();
                }
            } else if (compact) {
                const std::string& sym = n.symbol.empty() ? n.title : n.symbol;
                ImGui::PushFont(nullptr, font * std::clamp(zoom * 1.5f, 0.6f, 2.6f));
                const ImVec2 ts = ImGui::CalcTextSize(sym.c_str());
                draw->AddText(ImVec2((l.min.x + l.max.x - ts.x) * 0.5f, (l.min.y + bottom - ts.y) * 0.5f), IM_COL32(220, 220, 220, 150),
                              sym.c_str());
                ImGui::PopFont();
            } else if (n.style == NodeStyle::Variable && n.inputs.empty()) {
                const float ty = l.min.y + l.top + row * 0.5f - ImGui::GetTextLineHeight() * 0.5f;
                draw->AddText(ImVec2(l.min.x + 10.0f * zoom, ty), IM_COL32(250, 250, 250, 255), n.title.c_str());
            }
            ImGui::PopFont();
        }

        // Pines
        ImGui::PushFont(nullptr, font * std::clamp(zoom * 0.9f, 0.45f, 1.4f));
        const float r = kPinRadius * zoom;
        const bool show_labels = !vertical && !compact && lod >= 0.45f;
        const auto drawPin = [&](ImVec2 p, const PinView& pin, bool output, int k) {
            const PinShape shape = pin.exec ? PinShape::Exec : pin.shape;
            const ImU32 color = pin.exec ? kExecColor : pin.color;
            if (lod < 0.3f) {
                draw->AddCircleFilled(p, r, color);
            } else {
                drawPinShape(draw, p, r, shape, color, pin.connected);
                if (shape == PinShape::Circle && !vertical && lod >= 0.6f) {
                    const float x = p.x + r + 1.5f * zoom;
                    draw->AddTriangleFilled(ImVec2(x, p.y - 2.5f * zoom), ImVec2(x, p.y + 2.5f * zoom), ImVec2(x + 3.0f * zoom, p.y), color);
                }
            }
            if (highlight_targets && n.id != link_from_->node && output != link_from_->output) {
                const LinkCheck chk = checkTarget(PinRef{n.id, k, output});
                if (!chk.ok) draw->AddCircleFilled(p, r + 2.0f * zoom, IM_COL32(18, 18, 20, 180));
                else draw->AddCircle(p, r + 3.0f * zoom, chk.converts ? theme::kYellow : theme::withAlpha(color, 220), 0, 2.0f);
            }
            if (!pin.watch.empty() && lod >= 0.6f) {
                drawBubble(draw, p, pin.watch, output ? 1 : -1, zoom, font, IM_COL32(40, 40, 46, 235), IM_COL32(235, 235, 200, 255));
            }
        };
        for (std::size_t k = 0; k < n.inputs.size(); ++k) {
            const PinView& pin = n.inputs[k];
            const ImVec2 p = l.in[k];
            drawPin(p, pin, false, static_cast<int>(k));
            if (!show_labels && !(compact && lod >= 0.6f)) continue;
            const float text_y = p.y - ImGui::GetTextLineHeight() * 0.5f;
            float label_w = 0.0f;
            if (show_labels && !pin.hide_label && !pin.label.empty()) {
                draw->AddText(ImVec2(p.x + 10.0f * zoom, text_y), kPinLabel, pin.label.c_str());
                label_w = ImGui::CalcTextSize(pin.label.c_str()).x;
            }
            if (pin.inline_value && !pin.connected && inline_editor != nullptr && lod >= 0.6f) {
                const float x0 = p.x + 16.0f * zoom + label_w;
                float right = l.max.x - 8.0f * zoom;
                if (compact) {
                    right = (l.min.x + l.max.x) * 0.5f + 4.0f * zoom;  // el simbolo queda a la derecha
                } else if (k < n.outputs.size()) {
                    const PinView& out = n.outputs[k];
                    const float out_w = out.hide_label ? 0.0f : ImGui::CalcTextSize(out.label.c_str()).x;
                    right = l.max.x - 14.0f * zoom - out_w - 8.0f * zoom;
                }
                const float available = right - x0;
                if (available > 24.0f) {
                    ImGui::SetCursorScreenPos(ImVec2(x0, p.y - ImGui::GetFrameHeight() * 0.5f));
                    ImGui::PushID(n.id * 1024 + static_cast<int>(k));
                    ImGui::BeginGroup();
                    (*inline_editor)(n.id, static_cast<int>(k), available);
                    ImGui::EndGroup();
                    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) inline_hovered = true;
                    ImGui::PopID();
                }
            }
        }
        for (std::size_t k = 0; k < n.outputs.size(); ++k) {
            const PinView& pin = n.outputs[k];
            const ImVec2 p = l.out[k];
            drawPin(p, pin, true, static_cast<int>(k));
            if (!show_labels || pin.hide_label || pin.label.empty()) continue;
            if (n.style == NodeStyle::Variable && n.inputs.empty() && k == 0 && pin.label == n.title) continue;
            const ImVec2 s = ImGui::CalcTextSize(pin.label.c_str());
            draw->AddText(ImVec2(p.x - 10.0f * zoom - s.x, p.y - s.y * 0.5f), kPinLabel, pin.label.c_str());
        }
        ImGui::PopFont();
        // Cuerpo
        if (!n.body.empty() && lod >= 0.45f) {
            ImGui::PushFont(nullptr, font * std::clamp(zoom * 0.85f, 0.45f, 1.3f));
            const float body_y = vertical ? l.min.y + l.header + 4.0f * zoom
                                          : l.min.y + l.header + l.top + row * static_cast<float>(std::max(n.inputs.size(), n.outputs.size())) +
                                                2.0f * zoom;
            draw->PushClipRect(l.min, ImVec2(l.max.x, bottom), true);
            draw->AddText(ImVec2(l.min.x + 8.0f * zoom, body_y), IM_COL32(200, 200, 210, 230), n.body.c_str());
            draw->PopClipRect();
            ImGui::PopFont();
        }
        if (body_drawer != nullptr && n.extra_height > 0.0f) {
            const ImVec2 bmin(l.min.x + 4.0f, bottom - n.extra_height * zoom - 2.0f);
            const ImVec2 bmax(l.max.x - 4.0f, bottom - 4.0f);
            ImGui::SetCursorScreenPos(bmin);
            ImGui::PushID(n.id);
            ImGui::BeginGroup();
            (*body_drawer)(n.id, bmin, bmax);
            ImGui::EndGroup();
            if (ImGui::IsItemHovered() || ImGui::IsItemActive()) inline_hovered = true;
            ImGui::PopID();
        }
        if (n.disabled) scaleAlpha(draw, vtx_start, 0.4f);

        // Franja de estado abajo (a opacidad completa).
        if (l.strip > 0.0f) {
            const ImVec2 smin(l.min.x, bottom);
            ImU32 fill = IM_COL32(80, 80, 84, 230);
            const char* label = "Desactivado";
            if (!n.error.empty()) {
                fill = theme::withAlpha(theme::kRed, 220);
                label = "ERROR";
            } else if (!n.warning.empty()) {
                fill = theme::withAlpha(theme::kYellowDeep, 220);
                label = "AVISO";
            }
            draw->AddRectFilled(smin, l.max, fill, rounding, ImDrawFlags_RoundCornersBottom);
            if (n.disabled && n.error.empty() && n.warning.empty()) {
                draw->PushClipRect(smin, l.max, true);
                for (float x = smin.x - l.strip; x < l.max.x; x += 8.0f * zoom) {
                    draw->AddLine(ImVec2(x, l.max.y), ImVec2(x + l.strip, smin.y), IM_COL32(255, 255, 255, 25), 2.0f * zoom);
                }
                draw->PopClipRect();
            }
            if (lod >= 0.3f) {
                ImGui::PushFont(nullptr, font * std::clamp(zoom * 0.75f, 0.45f, 1.2f));
                const ImVec2 ts = ImGui::CalcTextSize(label);
                draw->AddText(ImVec2((l.min.x + l.max.x - ts.x) * 0.5f, smin.y + (l.strip - ts.y) * 0.5f), IM_COL32(255, 255, 255, 235), label);
                ImGui::PopFont();
            }
        }
        // Comentario flotante encima del nodo.
        if (!n.bubble.empty() && lod >= 0.3f) {
            ImGui::PushFont(nullptr, font * std::clamp(zoom * 0.8f, 0.45f, 1.2f));
            const float th = ImGui::CalcTextSize(n.bubble.c_str()).y;
            ImGui::PopFont();
            drawBubble(draw, ImVec2(l.min.x, l.min.y - th * 0.5f - 10.0f * zoom), n.bubble, 1, zoom, font, IM_COL32(225, 225, 225, 235),
                       IM_COL32(25, 25, 25, 255));
        }
    }

    // --- Caja de seleccion ---
    if (box_selecting_) {
        draw->AddRectFilled(box_start_, mouse, theme::withAlpha(theme::kAccent, 45));
        draw->AddRect(box_start_, mouse, theme::withAlpha(theme::kAccentHover, 210));
    }

    // --- Minimapa ---
    core::Vec2 mm_bmin{};
    float mm_scale = 1.0f;
    ImVec2 mm_off{};
    if (show_minimap) {
        const core::Vec2 v0 = toCanvas(origin_);
        const core::Vec2 v1 = toCanvas(add(origin_, size_));
        float x0 = v0.x, y0 = v0.y, x1 = v1.x, y1 = v1.y;
        for (const auto& kv : geom_) {
            const NodeGeom& g = kv.second;
            x0 = std::min(x0, g.pos.x);
            y0 = std::min(y0, g.pos.y);
            x1 = std::max(x1, g.pos.x + g.size.x);
            y1 = std::max(y1, g.pos.y + g.size.y);
        }
        const float bw = std::max(x1 - x0, 1.0f);
        const float bh = std::max(y1 - y0, 1.0f);
        mm_scale = std::min((kMinimapW - 12.0f) / bw, (kMinimapH - 12.0f) / bh);
        mm_bmin = core::Vec2{x0, y0};
        mm_off = ImVec2(mm_min.x + (kMinimapW - bw * mm_scale) * 0.5f, mm_min.y + (kMinimapH - bh * mm_scale) * 0.5f);
        const auto map = [&](core::Vec2 p) { return ImVec2(mm_off.x + (p.x - x0) * mm_scale, mm_off.y + (p.y - y0) * mm_scale); };
        draw->AddRectFilled(mm_min, mm_max, IM_COL32(12, 12, 14, 210), 4.0f);
        draw->AddRect(mm_min, mm_max, IM_COL32(70, 70, 76, 255), 4.0f);
        draw->PushClipRect(mm_min, mm_max, true);
        for (const NodeView& n : nodes) {
            const auto g = geom_.find(n.id);
            if (g == geom_.end()) continue;
            const ImVec2 a = map(g->second.pos);
            const ImVec2 b = map(core::Vec2{g->second.pos.x + g->second.size.x, g->second.pos.y + g->second.size.y});
            if (n.comment) draw->AddRectFilled(a, b, (n.header & 0x00FFFFFFu) | 0x40000000u);
            else draw->AddRectFilled(a, ImVec2(std::max(b.x, a.x + 2.0f), std::max(b.y, a.y + 2.0f)),
                                     selection.contains(n.id) ? theme::kYellow : theme::withAlpha(n.header, 220));
        }
        draw->AddRect(map(v0), map(v1), IM_COL32(255, 255, 255, 190), 0.0f, 0, 1.5f);
        draw->PopClipRect();
    }
    draw->PopClipRect();

    // --- Entrada ---
    const bool other_active = ImGui::IsAnyItemActive() && !canvas_active;
    const bool can_interact = window_hovered && !inline_hovered && !other_active && !in_minimap;
    if (!can_interact) {
        hovered_pin.reset();
        hovered_node = 0;
        hovered_link = -1;
    }
    ev.hovered_pin = hovered_pin;
    ev.hovered_node = hovered_node;
    const bool ctrl = io.KeyCtrl;
    const bool shift = io.KeyShift;
    const bool alt = io.KeyAlt;
    const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) && !io.WantTextInput;

    // Minimapa: clic o arrastre centra la vista ahi.
    const auto minimapCenter = [&]() {
        const core::Vec2 c{mm_bmin.x + (mouse.x - mm_off.x) / mm_scale, mm_bmin.y + (mouse.y - mm_off.y) / mm_scale};
        pan.x = size_.x * 0.5f - c.x * zoom;
        pan.y = size_.y * 0.5f - c.y * zoom;
    };
    if (minimap_drag_) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && show_minimap) minimapCenter();
        else minimap_drag_ = false;
    } else if (in_minimap && !other_active && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        minimap_drag_ = true;
        minimapCenter();
    }

    // Tecla de nodo mantenida (B, S, D...) + clic en el fondo.
    ImGuiKey spawn_key = ImGuiKey_None;
    if (options.keys && window_hovered && !ctrl && !shift && !alt && !io.WantTextInput) {
        for (const ImGuiKey key : kSpawnKeys) {
            if (ImGui::IsKeyDown(key)) {
                spawn_key = key;
                break;
            }
        }
    }

    if (can_interact && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !alt) {
        if (hovered_pin) {
            link_from_ = hovered_pin;
            link_checks_.clear();
            const PinView* pv = pinView(*hovered_pin);
            relinking_ = extended && ctrl && pv != nullptr && pv->connected;
        } else if (hovered_node != 0) {
            // Esquina de un comentario: cambiar el tamano.
            const auto it = index.find(hovered_node);
            const NodeView& n = nodes[it->second];
            if (n.comment && dist2(mouse, layout[it->second].max) < 400.0f) {
                resizing_comment_ = hovered_node;
            } else {
                if (ctrl) {
                    if (selection.contains(hovered_node)) selection.erase(hovered_node);
                    else selection.insert(hovered_node);
                } else {
                    if (!selection.contains(hovered_node)) selection = {hovered_node};
                    link_selection.clear();
                }
                dragging_nodes_ = true;
                drag_started_ = false;
                drag_items_.clear();
                drag_delta_ = {0.0f, 0.0f};
                ev.clicked = hovered_node;
            }
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) ev.double_clicked = hovered_node;
        } else if (hovered_link >= 0) {
            const LinkView& lk = links[static_cast<std::size_t>(hovered_link)];
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                ev.link_double_clicked = lk;
                ev.link_point = toCanvas(mouse);
            }
            if (!ctrl) selection.clear();
            if (lk.id != 0) {
                if (!ctrl) link_selection = {lk.id};
                else if (link_selection.contains(lk.id)) link_selection.erase(lk.id);
                else link_selection.insert(lk.id);
            }
        } else if (spawn_key != ImGuiKey_None) {
            ev.spawn_key = spawn_key;
            ev.spawn_at = toCanvas(mouse);
        } else {
            if (!ctrl) {
                selection.clear();
                link_selection.clear();
            }
            box_selecting_ = true;
            box_start_ = mouse;
        }
    }
    // Alt+clic: en una entrada quita su enlace; en una salida (extendido) corta
    // todos; en un cable (sin arrastrar) lo corta al soltar.
    if (can_interact && alt && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (hovered_pin && !hovered_pin->output) {
            ev.unlink = hovered_pin;
        } else if (extended && hovered_pin && hovered_pin->output) {
            for (const LinkView& l : links) {
                if (l.from_node == hovered_pin->node && l.from_pin == hovered_pin->pin) ev.cut_links.push_back(l);
            }
        } else if (hovered_link >= 0) {
            alt_press_link_ = links[static_cast<std::size_t>(hovered_link)];
        }
    }
    if (alt_press_link_ && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const float threshold = io.MouseDragThreshold;
        if (io.MouseDragMaxDistanceSqr[ImGuiMouseButton_Left] < threshold * threshold) ev.cut_links.push_back(*alt_press_link_);
        alt_press_link_.reset();
    }
    // Esquina de un comentario.
    if (resizing_comment_ != 0) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            const auto it = index.find(resizing_comment_);
            if (it != index.end() && nodes[it->second].comment_size != nullptr) {
                core::Vec2& s = *nodes[it->second].comment_size;
                s.x = std::max(s.x + io.MouseDelta.x / zoom, 120.0f);
                s.y = std::max(s.y + io.MouseDelta.y / zoom, 60.0f);
            }
        } else {
            resizing_comment_ = 0;
            ev.moved = true;
        }
    }
    // Mover nodos (los comentarios arrastran lo que tienen dentro).
    const auto snapValue = [](float v) { return std::round(v / kGridSmall) * kGridSmall; };
    if (dragging_nodes_ && snap) {
        // Snap durante el arrastre (como Unreal): cada nodo va a
        // round((inicio + delta) / rejilla) * rejilla, asi se mueve a saltos de
        // rejilla y no salta al soltar. Alt mantenido = movimiento libre. Los
        // nodos dentro de un comentario seleccionado se mueven con el mismo
        // delta ya ajustado del comentario (conservan su posicion relativa).
        if (!drag_started_) {
            drag_started_ = true;
            drag_items_.clear();
            drag_delta_ = {0.0f, 0.0f};
            for (const int sel : selection) {
                const auto it = index.find(sel);
                if (it == index.end() || nodes[it->second].position == nullptr) continue;
                drag_items_[sel] = DragItem{*nodes[it->second].position, 0};
            }
            for (const int sel : selection) {
                const auto it = index.find(sel);
                if (it == index.end() || !nodes[it->second].comment || nodes[it->second].position == nullptr) continue;
                const Layout& c = layout[it->second];
                for (std::size_t k = 0; k < nodes.size(); ++k) {
                    if (nodes[k].comment || nodes[k].position == nullptr) continue;
                    if (layout[k].min.x >= c.min.x && layout[k].min.y >= c.min.y && layout[k].max.x <= c.max.x &&
                        layout[k].max.y <= c.max.y) {
                        DragItem& d = drag_items_[nodes[k].id];
                        d.start = *nodes[k].position;
                        if (d.owner == 0) d.owner = sel;
                    }
                }
            }
        }
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            // Alt pulsado a mitad del arrastre: el lienzo se queda la tecla para
            // que ImGui no active la barra de menu al soltarla (navegacion).
            if (alt) ImGui::SetKeyOwner(ImGuiMod_Alt, ImGui::GetID("##canvas"));
            if (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f) {
                drag_delta_.x += io.MouseDelta.x / zoom;
                drag_delta_.y += io.MouseDelta.y / zoom;
            }
            if (drag_delta_.x != 0.0f || drag_delta_.y != 0.0f) {
                // Delta ajustado de cada comentario (o crudo con Alt).
                const auto offsetOf = [&](const DragItem& d) {
                    if (alt) return drag_delta_;
                    return core::Vec2{snapValue(d.start.x + drag_delta_.x) - d.start.x,
                                      snapValue(d.start.y + drag_delta_.y) - d.start.y};
                };
                for (const auto& [id, d] : drag_items_) {
                    const auto it = index.find(id);
                    if (it == index.end() || nodes[it->second].position == nullptr) continue;
                    core::Vec2 off = offsetOf(d);
                    if (d.owner != 0) {
                        const auto ow = drag_items_.find(d.owner);
                        if (ow != drag_items_.end()) off = offsetOf(ow->second);
                    }
                    nodes[it->second].position->x = d.start.x + off.x;
                    nodes[it->second].position->y = d.start.y + off.y;
                }
            }
        } else {
            dragging_nodes_ = false;
            // Seguridad (normalmente no cambia nada): lo que se ajusta por si
            // mismo queda en la rejilla; con Alt se respeta la posicion libre.
            if (!alt) {
                for (const auto& [id, d] : drag_items_) {
                    if (d.owner != 0 && drag_items_.contains(d.owner)) continue;
                    const auto it = index.find(id);
                    if (it == index.end() || nodes[it->second].position == nullptr) continue;
                    core::Vec2& p = *nodes[it->second].position;
                    p.x = snapValue(p.x);
                    p.y = snapValue(p.y);
                }
            }
            drag_items_.clear();
            drag_started_ = false;
            ev.moved = true;
        }
    } else if (dragging_nodes_) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f) {
                std::set<int> moving = selection;
                for (const int sel : selection) {
                    const auto it = index.find(sel);
                    if (it == index.end() || !nodes[it->second].comment) continue;
                    const Layout& c = layout[it->second];
                    for (std::size_t k = 0; k < nodes.size(); ++k) {
                        if (nodes[k].comment || nodes[k].position == nullptr) continue;
                        if (layout[k].min.x >= c.min.x && layout[k].min.y >= c.min.y && layout[k].max.x <= c.max.x &&
                            layout[k].max.y <= c.max.y) {
                            moving.insert(nodes[k].id);
                        }
                    }
                }
                for (const int m : moving) {
                    const auto it = index.find(m);
                    if (it == index.end() || nodes[it->second].position == nullptr) continue;
                    nodes[it->second].position->x += io.MouseDelta.x / zoom;
                    nodes[it->second].position->y += io.MouseDelta.y / zoom;
                }
            }
        } else {
            dragging_nodes_ = false;
            if (snap) {
                for (const int sel : selection) {
                    const auto it = index.find(sel);
                    if (it == index.end() || nodes[it->second].position == nullptr) continue;
                    core::Vec2& p = *nodes[it->second].position;
                    p.x = std::round(p.x / kGridSmall) * kGridSmall;
                    p.y = std::round(p.y / kGridSmall) * kGridSmall;
                }
            }
            ev.moved = true;
        }
    }
    // Caja de seleccion
    if (box_selecting_ && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        box_selecting_ = false;
        const ImVec2 a(std::min(box_start_.x, mouse.x), std::min(box_start_.y, mouse.y));
        const ImVec2 b(std::max(box_start_.x, mouse.x), std::max(box_start_.y, mouse.y));
        if (b.x - a.x > 3.0f || b.y - a.y > 3.0f) {
            for (std::size_t i = 0; i < nodes.size(); ++i) {
                if (nodes[i].position == nullptr) continue;
                const Layout& l = layout[i];
                if (l.max.x >= a.x && l.min.x <= b.x && l.max.y >= a.y && l.min.y <= b.y) selection.insert(nodes[i].id);
            }
        }
    }
    // Soltar un enlace
    if (link_from_ && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const std::optional<PinRef> target = window_hovered ? pinAt(mouse) : std::nullopt;
        if (relinking_) {
            if (target && target->output == link_from_->output &&
                !(target->node == link_from_->node && target->pin == link_from_->pin)) {
                ev.relink = std::make_pair(*link_from_, *target);
            }
        } else if (target && target->output != link_from_->output && target->node != link_from_->node) {
            if (checkTarget(*target).ok) {
                const PinRef out = link_from_->output ? *link_from_ : *target;
                const PinRef in = link_from_->output ? *target : *link_from_;
                LinkView l;
                l.from_node = out.node;
                l.from_pin = out.pin;
                l.to_node = in.node;
                l.to_pin = in.pin;
                ev.link = l;
            }
        } else if (!target && window_hovered) {
            // Soltar sobre un nodo: el primer pin compatible (solo con can_link).
            const int over = extended && options.can_link != nullptr ? nodeAt(mouse, false) : 0;
            if (over != 0 && over != link_from_->node) {
                const NodeView& n = nodes[index[over]];
                const auto& list = link_from_->output ? n.inputs : n.outputs;
                for (std::size_t k = 0; k < list.size() && !ev.link; ++k) {
                    const PinRef t{over, static_cast<int>(k), !link_from_->output};
                    if (!checkTarget(t).ok) continue;
                    const PinRef out = link_from_->output ? *link_from_ : t;
                    const PinRef in = link_from_->output ? t : *link_from_;
                    LinkView l;
                    l.from_node = out.node;
                    l.from_pin = out.pin;
                    l.to_node = in.node;
                    l.to_pin = in.pin;
                    ev.link = l;
                }
            }
            if (!ev.link &&
                dist2(mouse, pinPosition(link_from_->node, link_from_->pin, link_from_->output).value_or(mouse)) > 100.0f) {
                ev.dropped = link_from_;
                ev.menu_position = toCanvas(mouse);
            }
        }
        link_from_.reset();
        relinking_ = false;
        link_checks_.clear();
    }
    // Desplazar (boton central, derecho arrastrando o Alt+izquierdo) y zoom
    if ((canvas_active || window_hovered) && !minimap_drag_ &&
        (ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f) || ImGui::IsMouseDragging(ImGuiMouseButton_Right) ||
         (alt && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f) && !link_from_ && !dragging_nodes_ && !box_selecting_))) {
        if (inside || canvas_active) {
            pan.x += io.MouseDelta.x;
            pan.y += io.MouseDelta.y;
        }
    }
    if (window_hovered && io.MouseWheel != 0.0f && !other_active) {
        const float old = zoom;
        zoom = std::clamp(zoom * (io.MouseWheel > 0.0f ? 1.1f : 1.0f / 1.1f), min_zoom, max_zoom);
        const float mx = mouse.x - origin_.x - pan.x;
        const float my = mouse.y - origin_.y - pan.y;
        pan.x -= mx * (zoom / old - 1.0f);
        pan.y -= my * (zoom / old - 1.0f);
    }
    // Menus (clic derecho sin arrastrar: arrastrando se desplaza)
    const float right_threshold = io.MouseDragThreshold;
    if (can_interact && ImGui::IsMouseReleased(ImGuiMouseButton_Right) &&
        io.MouseDragMaxDistanceSqr[ImGuiMouseButton_Right] < right_threshold * right_threshold) {
        ev.menu_position = toCanvas(mouse);
        if (extended && hovered_pin) {
            ev.pin_menu = hovered_pin;
        } else if (hovered_node != 0) {
            if (!selection.contains(hovered_node)) selection = {hovered_node};
            ev.node_menu = hovered_node;
        } else {
            if (hovered_link >= 0 && links[static_cast<std::size_t>(hovered_link)].id != 0) {
                link_selection = {links[static_cast<std::size_t>(hovered_link)].id};
            }
            ev.background_menu = true;
        }
    }
    // Teclado (con el lienzo enfocado)
    if (focused) {
        if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) {
            if (!selection.empty()) ev.erase.assign(selection.begin(), selection.end());
            if (extended && !link_selection.empty()) {
                for (const LinkView& l : links) {
                    if (l.id != 0 && link_selection.contains(l.id)) ev.cut_links.push_back(l);
                }
                link_selection.clear();
            }
        }
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_C, false)) ev.copy = true;
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_V, false)) {
            ev.paste = true;
            ev.menu_position = window_hovered ? toCanvas(mouse) : viewCenter();
        }
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_D, false)) ev.duplicate = true;
        if (!ctrl && !shift && ImGui::IsKeyPressed(ImGuiKey_F, false)) frame(nodes);
        if (ImGui::IsKeyPressed(ImGuiKey_Space, false) && window_hovered) {
            ev.background_menu = true;
            ev.menu_position = toCanvas(mouse);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            link_from_.reset();
            relinking_ = false;
            link_checks_.clear();
        }
        if (options.keys) {
            if (ctrl && ImGui::IsKeyPressed(ImGuiKey_A, false)) {
                selection.clear();
                for (const NodeView& n : nodes) {
                    if (n.position != nullptr) selection.insert(n.id);
                }
                ev.select_all = true;
            }
            if (ctrl && ImGui::IsKeyPressed(ImGuiKey_X, false)) ev.cut = true;
            if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
                if (shift) ev.redo = true;
                else ev.undo = true;
            }
            if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) ev.redo = true;
            if (ctrl && ImGui::IsKeyPressed(ImGuiKey_F, false)) ev.find = true;
            if (ImGui::IsKeyPressed(ImGuiKey_F2, false)) ev.rename = true;
            if (!ctrl && !shift && !alt) {
                if (ImGui::IsKeyPressed(ImGuiKey_Q, false) && (!selection.empty() || !link_selection.empty())) {
                    align(AlignOp::Straighten, nodes, links);
                    ev.moved = true;
                }
                if (ImGui::IsKeyPressed(ImGuiKey_M, false)) minimap = !minimap;
                if (ImGui::IsKeyPressed(ImGuiKey_C, false) && !selection.empty()) ev.wrap_comment = true;
            }
            if (shift && !ctrl && !alt && selection.size() >= 2) {
                std::optional<AlignOp> op;
                if (ImGui::IsKeyPressed(ImGuiKey_W, false)) op = AlignOp::Top;
                if (ImGui::IsKeyPressed(ImGuiKey_S, false)) op = AlignOp::Bottom;
                if (ImGui::IsKeyPressed(ImGuiKey_A, false)) op = AlignOp::Left;
                if (ImGui::IsKeyPressed(ImGuiKey_D, false)) op = AlignOp::Right;
                if (op) {
                    align(*op, nodes, links);
                    ev.moved = true;
                }
            }
        }
    }
    // Ayudas al pasar el raton
    if (link_from_ && drag_check && !drag_check->ok) {
        ImGui::SetTooltip("%s", drag_check->message.empty() ? "Tipos incompatibles" : drag_check->message.c_str());
    } else if (link_from_ && drag_check && drag_check->converts) {
        ImGui::SetTooltip("%s", drag_check->message.empty() ? "Se insertara una conversion" : drag_check->message.c_str());
    } else if (link_from_ && drag_check && !drag_check->message.empty()) {
        ImGui::SetTooltip("%s", drag_check->message.c_str());
    } else if (hovered_pin && !link_from_) {
        if (const PinView* pin = pinView(*hovered_pin); pin != nullptr && !pin->tooltip.empty()) {
            ImGui::SetTooltip("%s", pin->tooltip.c_str());
        }
    } else if (hovered_node != 0 && !dragging_nodes_ && !link_from_) {
        const auto it = index.find(hovered_node);
        if (it != index.end()) {
            const NodeView& n = nodes[it->second];
            if (!n.error.empty()) ImGui::SetTooltip("%s", n.error.c_str());
            else if (!n.warning.empty()) ImGui::SetTooltip("%s", n.warning.c_str());
            else if (!n.tooltip.empty() && io.MouseDownDuration[0] < 0.0f) ImGui::SetTooltip("%s", n.tooltip.c_str());
        }
    }
    ImGui::EndChild();
    return ev;
}

int searchPopup(const char* popup_id, const std::vector<SearchItem>& items, std::string& filter) {
    return searchPopup(popup_id, items, filter, nullptr);
}

int searchPopup(const char* popup_id, const std::vector<SearchItem>& items, std::string& filter, const SearchContext* context) {
    int chosen = -1;
    ImGui::SetNextWindowSize(ImVec2(380.0f, 440.0f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopup(popup_id)) return -1;
    // Solo hay un buscador abierto a la vez: el cursor de teclado puede ser estatico.
    static int cursor = 0;
    static std::string last_filter;
    if (ImGui::IsWindowAppearing()) {
        filter.clear();
        last_filter.clear();
        cursor = 0;
    }
    bool sensitive = context == nullptr || context->context_sensitive == nullptr || *context->context_sensitive;
    const bool has_masks = std::any_of(items.begin(), items.end(), [](const SearchItem& it) { return it.in_types != 0 || it.out_types != 0; });
    const bool typed = context != nullptr && context->pin_type >= 0 && context->pin_type < 32 && has_masks;
    if (typed && context->context_sensitive != nullptr) {
        // La casilla va antes del campo para no robarle el foco de teclado.
        ImGui::Checkbox("Sensible al contexto", context->context_sensitive);
        sensitive = *context->context_sensitive;
    }
    const bool filtering = typed && sensitive;
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-1.0f);
    // CallbackHistory: el campo se queda las flechas (si no, la navegacion por
    // teclado le quita el foco); las leemos abajo para mover el cursor.
    const bool enter = ImGui::InputTextWithHint("##search", "Buscar nodo...", &filter,
                                                ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory,
                                                [](ImGuiInputTextCallbackData*) { return 0; });
    if (filter != last_filter) {
        last_filter = filter;
        cursor = 0;
    }

    const auto matches = [&](const SearchItem& item) {
        if (!filtering) return true;
        const std::uint32_t want = 1u << static_cast<unsigned>(context->pin_type);
        const std::uint32_t mask = context->from_output ? item.in_types : item.out_types;
        if ((mask & want) != 0) return true;
        if (context->exec) return false;
        const std::uint32_t data = mask & ~context->exec_mask;
        if ((data & context->any_mask) != 0) return true;
        return (want & context->any_mask) != 0 && data != 0;
    };
    const std::string folded_filter = fold(filter);
    const auto rank = [&](const SearchItem& item) {
        int r = 0;
        if (context != nullptr && !context->owner.empty() && item.owner == context->owner) r -= 4;
        if (item.favorite) r -= 2;
        if (!folded_filter.empty() && fold(item.title).rfind(folded_filter, 0) == 0) r -= 1;
        return r;
    };
    std::vector<const SearchItem*> visible;
    visible.reserve(items.size());
    for (const SearchItem& item : items) {
        if (!matches(item)) continue;
        if (!filter.empty() && !fuzzyMatch(item.title + " " + item.keywords + " " + item.category + " " + item.owner, filter)) continue;
        visible.push_back(&item);
    }
    std::stable_sort(visible.begin(), visible.end(), [&](const SearchItem* a, const SearchItem* b) { return rank(*a) < rank(*b); });
    if (filtering) ImGui::TextDisabled("%d nodos compatibles con el pin", static_cast<int>(visible.size()));

    ImGui::BeginChild("##results", ImVec2(360.0f, 360.0f));
    const auto entry = [&](const SearchItem& item, bool highlighted, bool show_category) {
        const std::string label = (item.favorite ? "* " : "") + item.title + "##" + std::to_string(item.index);
        if (ImGui::Selectable(label.c_str(), highlighted)) chosen = item.index;
        if (ImGui::IsItemHovered() && !item.description.empty()) ImGui::SetTooltip("%s", item.description.c_str());
        if (show_category) {
            ImGui::SameLine();
            if (!item.owner.empty() && context != nullptr && item.owner == context->owner) {
                ImGui::TextColored(theme::vec(theme::kYellow), "%s", item.owner.c_str());
            } else {
                ImGui::TextDisabled("%s", item.category.c_str());
            }
        }
    };
    if (!filter.empty() || filtering) {
        // Lista plana con cursor de teclado (flechas + Enter).
        const int count = static_cast<int>(visible.size());
        bool moved = false;
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
            cursor = std::min(cursor + 1, std::max(count - 1, 0));
            moved = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
            cursor = std::max(cursor - 1, 0);
            moved = true;
        }
        if (cursor >= count) cursor = std::max(count - 1, 0);
        for (int i = 0; i < count; ++i) {
            entry(*visible[static_cast<std::size_t>(i)], i == cursor, true);
            if (i == cursor && moved) ImGui::SetScrollHereY();
        }
        if (count == 0) ImGui::TextDisabled("Ningun nodo coincide");
        if (enter && count > 0) chosen = visible[static_cast<std::size_t>(cursor)]->index;
    } else {
        // Recientes y favoritos arriba; despues el arbol de categorias.
        if (context != nullptr && context->recent != nullptr && !context->recent->empty()) {
            ImGui::SetNextItemOpen(true, ImGuiCond_Appearing);
            if (ImGui::TreeNode("Recientes")) {
                for (const int idx : *context->recent) {
                    for (const SearchItem* item : visible) {
                        if (item->index == idx) {
                            entry(*item, false, true);
                            break;
                        }
                    }
                }
                ImGui::TreePop();
            }
        }
        if (std::any_of(visible.begin(), visible.end(), [](const SearchItem* it) { return it->favorite; })) {
            ImGui::SetNextItemOpen(true, ImGuiCond_Appearing);
            if (ImGui::TreeNode("Favoritos")) {
                for (const SearchItem* item : visible) {
                    if (item->favorite) entry(*item, false, true);
                }
                ImGui::TreePop();
            }
        }
        std::vector<std::string> categories;
        for (const SearchItem* item : visible) {
            if (std::find(categories.begin(), categories.end(), item->category) == categories.end()) categories.push_back(item->category);
        }
        for (const std::string& category : categories) {
            if (!ImGui::TreeNode(category.c_str())) continue;
            for (const SearchItem* item : visible) {
                if (item->category == category) entry(*item, false, false);
            }
            ImGui::TreePop();
        }
    }
    ImGui::EndChild();
    if (chosen >= 0) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    return chosen;
}

}  // namespace cramion::editor::nodegraph
