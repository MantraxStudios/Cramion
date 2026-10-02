#include "NodeGraph.h"

#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>

namespace cramion::editor::nodegraph {

namespace {

constexpr float kHeader = 24.0f;
constexpr float kRow = 22.0f;
constexpr float kPinRadius = 5.0f;

struct Layout {
    ImVec2 min{};
    ImVec2 max{};
    std::vector<ImVec2> in;
    std::vector<ImVec2> out;
};

ImVec2 add(ImVec2 a, ImVec2 b) { return ImVec2(a.x + b.x, a.y + b.y); }

float dist2(ImVec2 a, ImVec2 b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    return dx * dx + dy * dy;
}

ImU32 scaleColor(ImU32 c, float s) {
    const auto ch = [&](int shift) {
        const float v = static_cast<float>((c >> shift) & 0xFF) * s;
        return static_cast<ImU32>(std::clamp(v, 0.0f, 255.0f));
    };
    return (c & 0xFF000000u) | (ch(16) << 16) | (ch(8) << 8) | ch(0);
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
        min_x = std::min(min_x, n.position->x);
        min_y = std::min(min_y, n.position->y);
        max_x = std::max(max_x, n.position->x + n.width);
        max_y = std::max(max_y, n.position->y + 120.0f);
        ++count;
    }
    if (count == 0 || size_.x <= 1.0f) {
        pending_frame_ = count > 0;
        return;
    }
    const float w = std::max(max_x - min_x, 1.0f);
    const float h = std::max(max_y - min_y, 1.0f);
    zoom = std::clamp(std::min(size_.x / (w + 120.0f), size_.y / (h + 120.0f)), 0.3f, 1.2f);
    pan.x = size_.x * 0.5f - (min_x + w * 0.5f) * zoom;
    pan.y = size_.y * 0.5f - (min_y + h * 0.5f) * zoom;
}

Events Canvas::draw(const char* id, std::vector<NodeView>& nodes, const std::vector<LinkView>& links, ImVec2 size,
                    const InlineEditor* inline_editor, const BodyDrawer* body_drawer) {
    Events ev;
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
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##canvas", size_,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    const bool canvas_active = ImGui::IsItemActive();
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mouse = io.MousePos;
    const bool inside = mouse.x >= origin_.x && mouse.y >= origin_.y && mouse.x < origin_.x + size_.x &&
                        mouse.y < origin_.y + size_.y;
    const bool window_hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) && inside;

    draw->PushClipRect(origin_, add(origin_, size_), true);
    draw->AddRectFilled(origin_, add(origin_, size_), IM_COL32(28, 29, 33, 255));
    // Rejilla
    const float step = 32.0f * zoom;
    if (step > 6.0f) {
        int line = 0;
        for (float x = std::fmod(pan.x, step); x < size_.x; x += step, ++line) {
            draw->AddLine(ImVec2(origin_.x + x, origin_.y), ImVec2(origin_.x + x, origin_.y + size_.y), IM_COL32(255, 255, 255, 10));
        }
        for (float y = std::fmod(pan.y, step); y < size_.y; y += step) {
            draw->AddLine(ImVec2(origin_.x, origin_.y + y), ImVec2(origin_.x + size_.x, origin_.y + y), IM_COL32(255, 255, 255, 10));
        }
    }

    // --- Disposicion ---
    std::vector<Layout> layout(nodes.size());
    std::map<int, std::size_t> index;
    const float header = kHeader * zoom;
    const float row = kRow * zoom;
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        NodeView& n = nodes[i];
        index[n.id] = i;
        if (n.position == nullptr) continue;
        Layout& l = layout[i];
        l.min = toScreen(*n.position);
        const float w = n.width * zoom;
        if (n.comment) {
            const core::Vec2 s = n.comment_size != nullptr ? *n.comment_size : core::Vec2{300.0f, 200.0f};
            l.max = ImVec2(l.min.x + s.x * zoom, l.min.y + s.y * zoom);
            continue;
        }
        if (vertical) {
            const float body_lines = n.body.empty() ? 0.0f : 1.0f;
            const float h = header + (body_lines * 18.0f + 14.0f + n.extra_height) * zoom;
            l.max = ImVec2(l.min.x + w, l.min.y + h);
            for (std::size_t p = 0; p < n.inputs.size(); ++p) {
                l.in.push_back(ImVec2(l.min.x + w * (static_cast<float>(p) + 1.0f) / (static_cast<float>(n.inputs.size()) + 1.0f), l.min.y));
            }
            for (std::size_t p = 0; p < n.outputs.size(); ++p) {
                l.out.push_back(ImVec2(l.min.x + w * (static_cast<float>(p) + 1.0f) / (static_cast<float>(n.outputs.size()) + 1.0f), l.max.y));
            }
        } else {
            const std::size_t rows = std::max(n.inputs.size(), n.outputs.size());
            float h = header + static_cast<float>(rows) * row + 6.0f * zoom + n.extra_height * zoom;
            if (!n.body.empty()) h += 18.0f * zoom;
            l.max = ImVec2(l.min.x + w, l.min.y + h);
            for (std::size_t p = 0; p < n.inputs.size(); ++p) {
                l.in.push_back(ImVec2(l.min.x, l.min.y + header + row * (static_cast<float>(p) + 0.5f)));
            }
            for (std::size_t p = 0; p < n.outputs.size(); ++p) {
                l.out.push_back(ImVec2(l.max.x, l.min.y + header + row * (static_cast<float>(p) + 0.5f)));
            }
        }
    }

    const auto pinAt = [&](ImVec2 p) -> std::optional<PinRef> {
        const float r2 = std::max(9.0f * zoom, 6.0f) * std::max(9.0f * zoom, 6.0f);
        for (std::size_t i = nodes.size(); i-- > 0;) {
            if (nodes[i].comment || nodes[i].position == nullptr) continue;
            for (std::size_t k = 0; k < layout[i].in.size(); ++k) {
                if (dist2(p, layout[i].in[k]) <= r2) return PinRef{nodes[i].id, static_cast<int>(k), false};
            }
            for (std::size_t k = 0; k < layout[i].out.size(); ++k) {
                if (dist2(p, layout[i].out[k]) <= r2) return PinRef{nodes[i].id, static_cast<int>(k), true};
            }
        }
        return std::nullopt;
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
                if (p.x >= l.min.x && p.y >= l.min.y && p.x <= l.max.x && p.y <= l.min.y + header) return nodes[i].id;
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

    // --- Comentarios (al fondo) ---
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        const NodeView& n = nodes[i];
        if (!n.comment || n.position == nullptr) continue;
        const Layout& l = layout[i];
        const bool selected = selection.contains(n.id);
        draw->AddRectFilled(l.min, l.max, (n.header & 0x00FFFFFFu) | 0x30000000u, 6.0f * zoom);
        draw->AddRectFilled(l.min, ImVec2(l.max.x, l.min.y + header), (n.header & 0x00FFFFFFu) | 0x90000000u, 6.0f * zoom,
                            ImDrawFlags_RoundCornersTop);
        draw->AddRect(l.min, l.max, selected ? IM_COL32(255, 255, 255, 200) : ((n.header & 0x00FFFFFFu) | 0xA0000000u), 6.0f * zoom,
                      0, selected ? 2.0f : 1.0f);
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * std::clamp(zoom, 0.5f, 1.6f));
        draw->AddText(ImVec2(l.min.x + 8.0f * zoom, l.min.y + 4.0f * zoom), IM_COL32(240, 240, 240, 255), n.title.c_str());
        ImGui::PopFont();
        // Esquina para cambiar el tamano.
        draw->AddTriangleFilled(ImVec2(l.max.x - 12.0f * zoom, l.max.y), l.max, ImVec2(l.max.x, l.max.y - 12.0f * zoom),
                                IM_COL32(255, 255, 255, 90));
    }

    // --- Enlaces ---
    const auto bezier = [&](ImVec2 a, ImVec2 b, ImU32 color, float thickness, float flow) {
        ImVec2 c1, c2;
        if (vertical) {
            const float d = std::max(std::fabs(b.y - a.y) * 0.5f, 30.0f * zoom);
            c1 = ImVec2(a.x, a.y + d);
            c2 = ImVec2(b.x, b.y - d);
        } else {
            const float d = std::max(std::fabs(b.x - a.x) * 0.5f, 40.0f * zoom);
            c1 = ImVec2(a.x + d, a.y);
            c2 = ImVec2(b.x - d, b.y);
        }
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
    for (const LinkView& link : links) {
        const auto a = pinPosition(link.from_node, link.from_pin, true);
        const auto b = pinPosition(link.to_node, link.to_pin, false);
        if (!a || !b) continue;
        const float thickness = (link.exec ? 3.0f : 2.2f) * std::max(zoom, 0.5f);
        bezier(*a, *b, link.flow > 0.0f ? scaleColor(link.color, 1.0f + link.flow * 0.6f) : link.color, thickness, link.flow);
    }
    if (link_from_) {
        if (const auto p = pinPosition(link_from_->node, link_from_->pin, link_from_->output)) {
            if (link_from_->output) bezier(*p, mouse, IM_COL32(230, 230, 230, 255), 2.0f, 0.0f);
            else bezier(mouse, *p, IM_COL32(230, 230, 230, 255), 2.0f, 0.0f);
        }
    }

    // --- Nodos ---
    bool inline_hovered = false;
    const float font = ImGui::GetStyle().FontSizeBase;
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        NodeView& n = nodes[i];
        if (n.comment || n.position == nullptr) continue;
        const Layout& l = layout[i];
        if (l.max.x < origin_.x - 50.0f || l.min.x > origin_.x + size_.x + 50.0f || l.max.y < origin_.y - 50.0f ||
            l.min.y > origin_.y + size_.y + 50.0f) {
            continue;  // fuera de la vista
        }
        const bool selected = selection.contains(n.id);
        const float rounding = 6.0f * zoom;
        // Sombra, halo de ejecucion y cuerpo.
        draw->AddRectFilled(add(l.min, ImVec2(3.0f, 4.0f)), add(l.max, ImVec2(3.0f, 4.0f)), IM_COL32(0, 0, 0, 70), rounding);
        if (n.highlight > 0.0f) {
            draw->AddRect(add(l.min, ImVec2(-4.0f, -4.0f)), add(l.max, ImVec2(4.0f, 4.0f)),
                          IM_COL32(255, 200, 70, static_cast<int>(220 * n.highlight)), rounding + 3.0f, 0, 3.0f);
        }
        if (n.active) {
            const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(ImGui::GetTime()) * 6.0f);
            draw->AddRect(add(l.min, ImVec2(-4.0f, -4.0f)), add(l.max, ImVec2(4.0f, 4.0f)),
                          IM_COL32(90, 170, 255, static_cast<int>(120 + 135 * pulse)), rounding + 3.0f, 0, 3.0f);
        }
        draw->AddRectFilled(l.min, l.max, IM_COL32(44, 46, 52, 245), rounding);
        draw->AddRectFilled(l.min, ImVec2(l.max.x, l.min.y + header), n.header, rounding, ImDrawFlags_RoundCornersTop);
        ImU32 border = selected ? IM_COL32(255, 255, 255, 230) : IM_COL32(0, 0, 0, 200);
        if (!n.error.empty()) border = IM_COL32(240, 70, 60, 255);
        draw->AddRect(l.min, l.max, border, rounding, 0, selected || !n.error.empty() ? 2.0f : 1.0f);
        if (n.status_color != 0) {
            draw->AddRectFilled(ImVec2(l.min.x + 3.0f, l.max.y - 4.0f * zoom), ImVec2(l.max.x - 3.0f, l.max.y - 1.0f),
                                n.status_color, 2.0f);
        }
        // Titulo
        ImGui::PushFont(nullptr, font * std::clamp(zoom, 0.5f, 1.6f));
        std::string title = n.title;
        const float max_title = l.max.x - l.min.x - 16.0f * zoom;
        while (title.size() > 3 && ImGui::CalcTextSize(title.c_str()).x > max_title) title.pop_back();
        if (title.size() != n.title.size()) title += "..";
        draw->AddText(ImVec2(l.min.x + 8.0f * zoom, l.min.y + (header - ImGui::GetTextLineHeight()) * 0.5f),
                      IM_COL32(250, 250, 250, 255), title.c_str());
        ImGui::PopFont();
        if (n.breakpoint) {
            draw->AddCircleFilled(ImVec2(l.max.x - 9.0f * zoom, l.min.y + header * 0.5f), 5.0f * zoom, IM_COL32(230, 50, 50, 255));
        }
        // Pines
        ImGui::PushFont(nullptr, font * std::clamp(zoom * 0.9f, 0.45f, 1.4f));
        const float r = kPinRadius * zoom;
        const auto drawPin = [&](ImVec2 p, const PinView& pin) {
            if (pin.exec) {
                const ImVec2 a(p.x - r, p.y - r);
                const ImVec2 b(p.x - r, p.y + r);
                const ImVec2 c(p.x + r, p.y);
                if (pin.connected) draw->AddTriangleFilled(a, b, c, IM_COL32(240, 240, 240, 255));
                else draw->AddTriangle(a, b, c, IM_COL32(240, 240, 240, 255), 1.5f);
            } else {
                if (pin.connected) draw->AddCircleFilled(p, r, pin.color);
                else {
                    draw->AddCircleFilled(p, r, IM_COL32(30, 30, 34, 255));
                    draw->AddCircle(p, r, pin.color, 0, 1.6f);
                }
            }
        };
        for (std::size_t k = 0; k < n.inputs.size(); ++k) {
            const PinView& pin = n.inputs[k];
            const ImVec2 p = l.in[k];
            drawPin(p, pin);
            if (!vertical && zoom > 0.45f) {
                const float text_y = p.y - ImGui::GetTextLineHeight() * 0.5f;
                draw->AddText(ImVec2(p.x + 10.0f * zoom, text_y), IM_COL32(215, 215, 220, 255), pin.label.c_str());
                if (pin.inline_value && !pin.connected && inline_editor != nullptr && zoom >= 0.6f) {
                    const float label_w = ImGui::CalcTextSize(pin.label.c_str()).x;
                    const float x0 = p.x + 16.0f * zoom + label_w;
                    const float available = (l.min.x + (l.max.x - l.min.x) * (n.outputs.empty() ? 0.95f : 0.62f)) - x0;
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
        }
        for (std::size_t k = 0; k < n.outputs.size(); ++k) {
            const PinView& pin = n.outputs[k];
            const ImVec2 p = l.out[k];
            drawPin(p, pin);
            if (!vertical && zoom > 0.45f) {
                const ImVec2 s = ImGui::CalcTextSize(pin.label.c_str());
                draw->AddText(ImVec2(p.x - 10.0f * zoom - s.x, p.y - s.y * 0.5f), IM_COL32(215, 215, 220, 255), pin.label.c_str());
            }
        }
        ImGui::PopFont();
        // Cuerpo
        if (!n.body.empty() && zoom > 0.45f) {
            ImGui::PushFont(nullptr, font * std::clamp(zoom * 0.85f, 0.45f, 1.3f));
            const float body_y = vertical ? l.min.y + header + 4.0f * zoom
                                          : l.min.y + header + row * static_cast<float>(std::max(n.inputs.size(), n.outputs.size())) + 2.0f * zoom;
            draw->PushClipRect(l.min, l.max, true);
            draw->AddText(ImVec2(l.min.x + 8.0f * zoom, body_y), IM_COL32(200, 200, 210, 230), n.body.c_str());
            draw->PopClipRect();
            ImGui::PopFont();
        }
        if (body_drawer != nullptr && n.extra_height > 0.0f) {
            const ImVec2 bmin(l.min.x + 4.0f, l.max.y - n.extra_height * zoom - 2.0f);
            const ImVec2 bmax(l.max.x - 4.0f, l.max.y - 4.0f);
            ImGui::SetCursorScreenPos(bmin);
            ImGui::PushID(n.id);
            ImGui::BeginGroup();
            (*body_drawer)(n.id, bmin, bmax);
            ImGui::EndGroup();
            if (ImGui::IsItemHovered() || ImGui::IsItemActive()) inline_hovered = true;
            ImGui::PopID();
        }
    }

    // --- Caja de seleccion ---
    if (box_selecting_) {
        draw->AddRectFilled(box_start_, mouse, IM_COL32(90, 150, 255, 40));
        draw->AddRect(box_start_, mouse, IM_COL32(90, 150, 255, 200));
    }
    draw->PopClipRect();

    // --- Entrada ---
    const bool other_active = ImGui::IsAnyItemActive() && !canvas_active;
    const bool can_interact = window_hovered && !inline_hovered && !other_active;
    const std::optional<PinRef> hovered_pin = can_interact ? pinAt(mouse) : std::nullopt;
    const int hovered_node = can_interact && !hovered_pin ? nodeAt(mouse, true) : 0;
    ev.hovered_pin = hovered_pin;
    ev.hovered_node = hovered_node;
    const bool ctrl = io.KeyCtrl;

    if (can_interact && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !io.KeyAlt) {
        if (hovered_pin) {
            link_from_ = hovered_pin;
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
                } else if (!selection.contains(hovered_node)) {
                    selection = {hovered_node};
                }
                dragging_nodes_ = true;
                ev.clicked = hovered_node;
            }
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) ev.double_clicked = hovered_node;
        } else {
            if (!ctrl) selection.clear();
            box_selecting_ = true;
            box_start_ = mouse;
        }
    }
    // Alt+clic en una entrada: quitar su enlace.
    if (can_interact && io.KeyAlt && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && hovered_pin && !hovered_pin->output) {
        ev.unlink = hovered_pin;
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
    if (dragging_nodes_) {
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
                    p.x = std::round(p.x / 16.0f) * 16.0f;
                    p.y = std::round(p.y / 16.0f) * 16.0f;
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
        if (target && target->output != link_from_->output && target->node != link_from_->node) {
            const PinRef out = link_from_->output ? *link_from_ : *target;
            const PinRef in = link_from_->output ? *target : *link_from_;
            LinkView l;
            l.from_node = out.node;
            l.from_pin = out.pin;
            l.to_node = in.node;
            l.to_pin = in.pin;
            ev.link = l;
        } else if (!target && window_hovered) {
            if (dist2(mouse, pinPosition(link_from_->node, link_from_->pin, link_from_->output).value_or(mouse)) > 100.0f) {
                ev.dropped = link_from_;
                ev.menu_position = toCanvas(mouse);
            }
        }
        link_from_.reset();
    }
    // Desplazar y zoom
    if ((canvas_active || window_hovered) && (ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f) ||
                                              (io.KeyAlt && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f) && !link_from_))) {
        if (inside || canvas_active) {
            pan.x += io.MouseDelta.x;
            pan.y += io.MouseDelta.y;
        }
    }
    if (window_hovered && io.MouseWheel != 0.0f && !other_active) {
        const float old = zoom;
        zoom = std::clamp(zoom * (io.MouseWheel > 0.0f ? 1.1f : 1.0f / 1.1f), 0.25f, 2.0f);
        const float mx = mouse.x - origin_.x - pan.x;
        const float my = mouse.y - origin_.y - pan.y;
        pan.x -= mx * (zoom / old - 1.0f);
        pan.y -= my * (zoom / old - 1.0f);
    }
    // Menus
    if (can_interact && ImGui::IsMouseReleased(ImGuiMouseButton_Right) && !ImGui::IsMouseDragging(ImGuiMouseButton_Right)) {
        ev.menu_position = toCanvas(mouse);
        if (hovered_node != 0) {
            if (!selection.contains(hovered_node)) selection = {hovered_node};
            ev.node_menu = hovered_node;
        } else {
            ev.background_menu = true;
        }
    }
    // Teclado (con el lienzo enfocado)
    const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) && !ImGui::GetIO().WantTextInput;
    if (focused) {
        if ((ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) && !selection.empty()) {
            ev.erase.assign(selection.begin(), selection.end());
        }
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_C, false)) ev.copy = true;
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_V, false)) {
            ev.paste = true;
            ev.menu_position = window_hovered ? toCanvas(mouse) : viewCenter();
        }
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_D, false)) ev.duplicate = true;
        if (!ctrl && ImGui::IsKeyPressed(ImGuiKey_F, false)) frame(nodes);
        if (ImGui::IsKeyPressed(ImGuiKey_Space, false) && window_hovered) {
            ev.background_menu = true;
            ev.menu_position = toCanvas(mouse);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) link_from_.reset();
    }
    // Ayudas al pasar el raton
    if (hovered_pin && !link_from_) {
        const auto it = index.find(hovered_pin->node);
        if (it != index.end()) {
            const auto& list = hovered_pin->output ? nodes[it->second].outputs : nodes[it->second].inputs;
            if (hovered_pin->pin >= 0 && static_cast<std::size_t>(hovered_pin->pin) < list.size()) {
                const PinView& pin = list[static_cast<std::size_t>(hovered_pin->pin)];
                if (!pin.tooltip.empty()) ImGui::SetTooltip("%s", pin.tooltip.c_str());
            }
        }
    } else if (hovered_node != 0 && !dragging_nodes_ && !link_from_) {
        const auto it = index.find(hovered_node);
        if (it != index.end()) {
            const NodeView& n = nodes[it->second];
            if (!n.error.empty()) ImGui::SetTooltip("%s", n.error.c_str());
            else if (!n.tooltip.empty() && ImGui::GetIO().MouseDownDuration[0] < 0.0f) ImGui::SetTooltip("%s", n.tooltip.c_str());
        }
    }
    ImGui::EndChild();
    return ev;
}

int searchPopup(const char* popup_id, const std::vector<SearchItem>& items, std::string& filter) {
    int chosen = -1;
    ImGui::SetNextWindowSize(ImVec2(360.0f, 420.0f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopup(popup_id)) return -1;
    if (ImGui::IsWindowAppearing()) {
        filter.clear();
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::SetNextItemWidth(-1.0f);
    const bool enter = ImGui::InputTextWithHint("##search", "Buscar nodo...", &filter, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::BeginChild("##results", ImVec2(340.0f, 360.0f));
    int first = -1;
    if (!filter.empty()) {
        for (const SearchItem& item : items) {
            if (!fuzzyMatch(item.title + " " + item.keywords + " " + item.category, filter)) continue;
            if (first < 0) first = item.index;
            if (ImGui::Selectable((item.title + "##" + std::to_string(item.index)).c_str())) chosen = item.index;
            ImGui::SameLine();
            ImGui::TextDisabled("%s", item.category.c_str());
            if (ImGui::IsItemHovered() && !item.description.empty()) ImGui::SetTooltip("%s", item.description.c_str());
        }
    } else {
        std::vector<std::string> categories;
        for (const SearchItem& item : items) {
            if (std::find(categories.begin(), categories.end(), item.category) == categories.end()) categories.push_back(item.category);
        }
        for (const std::string& category : categories) {
            if (!ImGui::TreeNode(category.c_str())) continue;
            for (const SearchItem& item : items) {
                if (item.category != category) continue;
                if (ImGui::Selectable((item.title + "##" + std::to_string(item.index)).c_str())) chosen = item.index;
                if (ImGui::IsItemHovered() && !item.description.empty()) ImGui::SetTooltip("%s", item.description.c_str());
            }
            ImGui::TreePop();
        }
    }
    ImGui::EndChild();
    if (enter && first >= 0) chosen = first;
    if (chosen >= 0) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    return chosen;
}

}  // namespace cramion::editor::nodegraph
