#include "UiRenderer.h"

#include <algorithm>
#include <cfloat>

namespace cramion::editor {

namespace {

ImU32 toColor(const core::Vec4& c) {
    return ImGui::ColorConvertFloat4ToU32(ImVec4(std::clamp(c.x, 0.0f, 1.0f), std::clamp(c.y, 0.0f, 1.0f),
                                                 std::clamp(c.z, 0.0f, 1.0f), std::clamp(c.w, 0.0f, 1.0f)));
}

void appendUtf8(std::string& out, unsigned int c) {
    if (c < 0x80) {
        out += static_cast<char>(c);
    } else if (c < 0x800) {
        out += static_cast<char>(0xC0 | (c >> 6));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out += static_cast<char>(0xE0 | (c >> 12));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    }
}

}  // namespace

void drawUiList(ImDrawList* draw, ImVec2 origin, const std::vector<ui::UiDrawCommand>& list, ImGuiLayer& imgui,
                const std::filesystem::path& assets_root) {
    ImFont* font = ImGui::GetFont();
    for (const ui::UiDrawCommand& c : list) {
        const ImVec2 min{origin.x + c.rect.x, origin.y + c.rect.y};
        const ImVec2 max{min.x + c.rect.w, min.y + c.rect.h};
        const ImU32 color = toColor(c.color);
        switch (c.type) {
            case ui::UiDrawCommand::Type::Rect:
                draw->AddRectFilled(min, max, color, c.radius);
                break;
            case ui::UiDrawCommand::Type::Line:
                draw->AddRect(min, max, color, c.radius, 0, 2.0f);
                break;
            case ui::UiDrawCommand::Type::Circle:
                draw->AddCircleFilled(ImVec2((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f), c.rect.w * 0.5f, color, 32);
                break;
            case ui::UiDrawCommand::Type::Image: {
                ImVec2 image_size{};
                const ImTextureID texture =
                    imgui.image(assets_root / std::filesystem::path(std::u8string(c.texture.begin(), c.texture.end())), &image_size);
                if (texture == 0) {
                    draw->AddRectFilled(min, max, (color & 0x00FFFFFFu) | 0x40000000u, c.radius);
                    break;
                }
                ImVec2 a = min;
                ImVec2 b = max;
                if (c.preserve_aspect && image_size.x > 0.0f && image_size.y > 0.0f) {
                    const float aspect = image_size.x / image_size.y;
                    float w = c.rect.w;
                    float h = c.rect.h;
                    if (w / std::max(h, 1.0f) > aspect) w = h * aspect;
                    else h = w / aspect;
                    a = ImVec2(min.x + (c.rect.w - w) * 0.5f, min.y + (c.rect.h - h) * 0.5f);
                    b = ImVec2(a.x + w, a.y + h);
                }
                draw->AddImageRounded(texture, a, b, ImVec2(0, 0), ImVec2(1, 1), color, c.radius);
                break;
            }
            case ui::UiDrawCommand::Type::Text: {
                if (c.text.empty()) break;
                const float size = std::max(c.font_size, 1.0f);
                const float wrap = c.wrap ? c.rect.w : 0.0f;
                const ImVec2 text_size = font->CalcTextSizeA(size, FLT_MAX, wrap, c.text.c_str());
                float x = min.x;
                float y = min.y;
                if (c.h_align == 1) x += (c.rect.w - text_size.x) * 0.5f;
                if (c.h_align == 2) x += c.rect.w - text_size.x;
                if (c.v_align == 1) y += (c.rect.h - text_size.y) * 0.5f;
                if (c.v_align == 2) y += c.rect.h - text_size.y;
                if (c.shadow) {
                    draw->AddText(font, size, ImVec2(x + size * 0.06f, y + size * 0.06f), IM_COL32(0, 0, 0, 170),
                                  c.text.c_str(), nullptr, wrap);
                }
                draw->AddText(font, size, ImVec2(x, y), color, c.text.c_str(), nullptr, wrap);
                break;
            }
        }
    }
}

ui::UiInput uiInputFromImGui(ImVec2 origin, ImVec2 size, bool hovered, bool typing) {
    const ImGuiIO& io = ImGui::GetIO();
    ui::UiInput input;
    if (hovered) {
        input.mouse_x = io.MousePos.x - origin.x;
        input.mouse_y = io.MousePos.y - origin.y;
        input.mouse_pressed = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    } else {
        input.mouse_x = input.mouse_y = -1.0f;
    }
    (void)size;
    input.mouse_down = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    input.mouse_released = ImGui::IsMouseReleased(ImGuiMouseButton_Left);
    if (typing) {
        for (const ImWchar c : io.InputQueueCharacters) appendUtf8(input.typed, c);
        input.backspace = ImGui::IsKeyPressed(ImGuiKey_Backspace);
        input.enter = ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
    }
    return input;
}

}  // namespace cramion::editor
