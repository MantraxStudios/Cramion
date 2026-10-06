#include "UiRenderer.h"
#include "Theme.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

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

// Un trozo de texto con estilo, ya colocado.
struct Piece {
    std::string text;
    ImFont* font = nullptr;
    float size = 16.0f;
    ImU32 color = IM_COL32_WHITE;
    bool fake_bold = false;
    bool italic = false;
    bool underline = false;
    bool strike = false;
    float x = 0.0f;  // relativo al principio de la linea
    float width = 0.0f;
};

struct Line {
    std::vector<Piece> pieces;
    float width = 0.0f;
    float height = 0.0f;
};

// Inclina los vertices anadidos desde `first` (cursiva).
void shear(ImDrawList* draw, int first, float baseline, float amount) {
    for (int v = first; v < draw->VtxBuffer.Size; ++v) {
        ImDrawVert& vert = draw->VtxBuffer[v];
        vert.pos.x += (baseline - vert.pos.y) * amount;
    }
}

void drawPiece(ImDrawList* draw, const Piece& p, ImVec2 pos, float outline, ImU32 outline_color, bool shadow) {
    const char* begin = p.text.c_str();
    const char* end = begin + p.text.size();
    const float baseline = pos.y + p.size * 0.8f;
    const auto emit = [&](ImVec2 at, ImU32 col) {
        const int first = draw->VtxBuffer.Size;
        draw->AddText(p.font, p.size, at, col, begin, end);
        if (p.fake_bold) draw->AddText(p.font, p.size, ImVec2(at.x + std::max(p.size * 0.04f, 0.6f), at.y), col, begin, end);
        if (p.italic) shear(draw, first, baseline + (at.y - pos.y), 0.2f);
    };
    if (shadow) emit(ImVec2(pos.x + p.size * 0.06f, pos.y + p.size * 0.06f), IM_COL32(0, 0, 0, 170));
    if (outline > 0.0f) {
        const int steps = outline > 2.5f ? 16 : 8;
        for (int k = 0; k < steps; ++k) {
            const float a = static_cast<float>(k) / static_cast<float>(steps) * 6.2831853f;
            emit(ImVec2(pos.x + std::cos(a) * outline, pos.y + std::sin(a) * outline), outline_color);
        }
    }
    emit(pos, p.color);
    if (p.underline) {
        const float y = pos.y + p.size * 0.95f;
        draw->AddLine(ImVec2(pos.x, y), ImVec2(pos.x + p.width, y), p.color, std::max(p.size * 0.06f, 1.0f));
    }
    if (p.strike) {
        const float y = pos.y + p.size * 0.55f;
        draw->AddLine(ImVec2(pos.x, y), ImVec2(pos.x + p.width, y), p.color, std::max(p.size * 0.06f, 1.0f));
    }
}

// Texto (enriquecido o no) con ajuste de lineas, alineacion, contorno y
// sombra. `regular` y `bold` son las fuentes (bold puede ser nullptr: se
// simula pintando dos veces).
void drawText(ImDrawList* draw, const ui::UiDrawCommand& c, ImVec2 min, ImFont* regular, ImFont* bold) {
    const float base_size = std::max(c.font_size, 1.0f);
    std::vector<ui::RichRun> runs;
    if (c.rich) {
        runs = ui::parseRichText(c.text, c.color, base_size, c.scale, c.bold, c.italic);
    } else {
        ui::RichRun r;
        r.text = c.text;
        r.color = c.color;
        r.size = base_size;
        r.bold = c.bold;
        r.italic = c.italic;
        runs.push_back(std::move(r));
    }
    const float wrap_width = c.wrap ? c.rect.w : 0.0f;
    std::vector<Line> lines(1);
    const auto newLine = [&]() { lines.emplace_back(); };
    for (const ui::RichRun& run : runs) {
        ImFont* font = run.bold && bold != nullptr ? bold : regular;
        const ImU32 color = toColor(run.color);
        // Trocea en palabras (con su espacio detras) y saltos de linea.
        std::size_t i = 0;
        while (i < run.text.size()) {
            if (run.text[i] == '\n') {
                lines.back().height = std::max(lines.back().height, run.size);
                newLine();
                ++i;
                continue;
            }
            std::size_t j = i;
            while (j < run.text.size() && run.text[j] != ' ' && run.text[j] != '\n') ++j;
            while (j < run.text.size() && run.text[j] == ' ') ++j;
            Piece p;
            p.text = run.text.substr(i, j - i);
            p.font = font;
            p.size = run.size;
            p.color = color;
            p.fake_bold = run.bold && bold == nullptr;
            p.italic = run.italic;
            p.underline = run.underline;
            p.strike = run.strike;
            p.width = font->CalcTextSizeA(p.size, FLT_MAX, 0.0f, p.text.c_str(), p.text.c_str() + p.text.size()).x;
            Line& line = lines.back();
            if (wrap_width > 0.0f && !line.pieces.empty() && line.width + p.width > wrap_width + 0.5f) {
                // Sin los espacios del final al medir la palabra que salta.
                newLine();
            }
            Line& target = lines.back();
            p.x = target.width;
            target.width += p.width;
            target.height = std::max(target.height, p.size);
            target.pieces.push_back(std::move(p));
            i = j;
        }
    }
    float total_h = 0.0f;
    for (Line& line : lines) {
        if (line.height <= 0.0f) line.height = base_size;
        total_h += line.height * c.line_spacing;
    }
    float y = min.y;
    if (c.v_align == 1) y += (c.rect.h - total_h) * 0.5f;
    if (c.v_align == 2) y += c.rect.h - total_h;
    const ImU32 outline_color = toColor(c.outline_color);
    for (const Line& line : lines) {
        float x = min.x;
        if (c.h_align == 1) x += (c.rect.w - line.width) * 0.5f;
        if (c.h_align == 2) x += c.rect.w - line.width;
        for (const Piece& p : line.pieces) {
            // Las letras mas pequenas de la linea se apoyan en la misma base.
            const float dy = (line.height - p.size) * 0.8f;
            drawPiece(draw, p, ImVec2(x + p.x, y + dy), c.outline, outline_color, c.shadow);
        }
        y += line.height * c.line_spacing;
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
        if (c.clipped) {
            if (c.clip.w <= 0.0f || c.clip.h <= 0.0f) continue;
            draw->PushClipRect(ImVec2(origin.x + c.clip.x, origin.y + c.clip.y),
                               ImVec2(origin.x + c.clip.x + c.clip.w, origin.y + c.clip.y + c.clip.h), true);
        }
        switch (c.type) {
            case ui::UiDrawCommand::Type::Triangle: {
                // radius 1 = hacia arriba, 0 = hacia abajo.
                if (c.radius > 0.5f) {
                    draw->AddTriangleFilled(ImVec2(min.x, max.y), ImVec2(max.x, max.y), ImVec2((min.x + max.x) * 0.5f, min.y), color);
                } else {
                    draw->AddTriangleFilled(ImVec2(min.x, min.y), ImVec2((min.x + max.x) * 0.5f, max.y), ImVec2(max.x, min.y), color);
                }
                break;
            }
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
                ImFont* regular = font;
                ImFont* bold = theme::boldFont();
                if (!c.font.empty()) {
                    if (ImFont* custom = imgui.uiFont(assets_root / std::filesystem::path(std::u8string(c.font.begin(), c.font.end())))) {
                        regular = custom;
                        bold = nullptr;  // con fuente propia la negrita se simula
                    }
                }
                const bool simple = !c.rich && !c.bold && !c.italic && c.outline <= 0.0f && c.line_spacing == 1.0f;
                if (simple) {
                    const float size = std::max(c.font_size, 1.0f);
                    const float wrap = c.wrap ? c.rect.w : 0.0f;
                    const ImVec2 text_size = regular->CalcTextSizeA(size, FLT_MAX, wrap, c.text.c_str());
                    float x = min.x;
                    float y = min.y;
                    if (c.h_align == 1) x += (c.rect.w - text_size.x) * 0.5f;
                    if (c.h_align == 2) x += c.rect.w - text_size.x;
                    if (c.v_align == 1) y += (c.rect.h - text_size.y) * 0.5f;
                    if (c.v_align == 2) y += c.rect.h - text_size.y;
                    if (c.shadow) {
                        draw->AddText(regular, size, ImVec2(x + size * 0.06f, y + size * 0.06f), IM_COL32(0, 0, 0, 170),
                                      c.text.c_str(), nullptr, wrap);
                    }
                    draw->AddText(regular, size, ImVec2(x, y), color, c.text.c_str(), nullptr, wrap);
                } else {
                    drawText(draw, c, min, regular, bold);
                }
                break;
            }
        }
        if (c.clipped) draw->PopClipRect();
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
    if (hovered) input.wheel = io.MouseWheel;
    input.mouse_released = ImGui::IsMouseReleased(ImGuiMouseButton_Left);
    if (typing) {
        for (const ImWchar c : io.InputQueueCharacters) appendUtf8(input.typed, c);
        input.backspace = ImGui::IsKeyPressed(ImGuiKey_Backspace);
        input.enter = ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
    }
    return input;
}

std::vector<gfx::WorldUiCanvas> buildWorldUi(const std::vector<ui::WorldCanvasDraw>& canvases, ImGuiLayer& imgui,
                                             const std::filesystem::path& assets_root) {
    std::vector<gfx::WorldUiCanvas> out;
    out.reserve(canvases.size());
    for (const ui::WorldCanvasDraw& c : canvases) {
        // Una lista de ImGui propia (como las de las ventanas): las letras del
        // atlas de fuentes y las imagenes de la UI, a pixeles del canvas.
        ImDrawList list(ImGui::GetDrawListSharedData());
        list._ResetForNewFrame();
        list.PushTexture(ImGui::GetIO().Fonts->TexRef);
        list.PushClipRect(ImVec2(0.0f, 0.0f), ImVec2(c.width, c.height));
        drawUiList(&list, ImVec2(0.0f, 0.0f), c.commands, imgui, assets_root);

        gfx::WorldUiCanvas canvas;
        canvas.id = static_cast<std::uint64_t>(entt::to_integral(c.canvas)) + 1u;
        canvas.width = static_cast<std::uint32_t>(std::lround(std::clamp(c.width, 1.0f, 4096.0f)));
        canvas.height = static_cast<std::uint32_t>(std::lround(std::clamp(c.height, 1.0f, 4096.0f)));
        canvas.transform = c.transform;
        canvas.size = c.size;
        canvas.vertices.reserve(static_cast<std::size_t>(list.VtxBuffer.Size));
        for (const ImDrawVert& v : list.VtxBuffer) {
            canvas.vertices.push_back(gfx::WorldUiVertex{v.pos.x, v.pos.y, v.uv.x, v.uv.y, v.col});
        }
        canvas.indices.reserve(static_cast<std::size_t>(list.IdxBuffer.Size));
        for (const ImDrawIdx i : list.IdxBuffer) canvas.indices.push_back(static_cast<std::uint32_t>(i));
        for (const ImDrawCmd& cmd : list.CmdBuffer) {
            if (cmd.UserCallback != nullptr || cmd.ElemCount == 0) continue;
            // La textura aun sin subir (letras nuevas: el frame siguiente).
            const ImTextureID texture = cmd.TexRef._TexData != nullptr ? cmd.TexRef._TexData->TexID : cmd.TexRef._TexID;
            if (texture == ImTextureID_Invalid) continue;
            gfx::WorldUiBatch batch;
            batch.texture = toDescriptorSet(texture);
            batch.first_index = cmd.IdxOffset;
            batch.index_count = cmd.ElemCount;
            batch.vertex_offset = cmd.VtxOffset;
            batch.clip[0] = cmd.ClipRect.x;
            batch.clip[1] = cmd.ClipRect.y;
            batch.clip[2] = cmd.ClipRect.z;
            batch.clip[3] = cmd.ClipRect.w;
            canvas.batches.push_back(batch);
        }
        out.push_back(std::move(canvas));
    }
    return out;
}

}  // namespace cramion::editor
