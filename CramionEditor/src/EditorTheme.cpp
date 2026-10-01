// Widgets compartidos del tema del editor (Theme.h): boton principal,
// titulos de seccion, insignias, caja de busqueda, menu de tres puntos y
// tarjetas con fondo.

#include "Theme.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <vector>

namespace cramion::editor::theme {

bool primaryButton(const char* label, const ImVec2& size) {
    ImGui::PushStyleColor(ImGuiCol_Button, kRed);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kRedHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, kRedActive);
    ImGui::PushStyleColor(ImGuiCol_Border, withAlpha(kRedHover, 140));
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 255, 255, 255));
    const bool pressed = ImGui::Button(label, size);
    ImGui::PopStyleColor(5);
    return pressed;
}

bool ghostButton(const char* label, const ImVec2& size) {
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kBg4);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, kBg5);
    ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(0, 0, 0, 0));
    const bool pressed = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return pressed;
}

void boldText(const char* text) {
    if (g_bold_font != nullptr) ImGui::PushFont(g_bold_font, 0.0f);
    ImGui::TextUnformatted(text);
    if (g_bold_font != nullptr) ImGui::PopFont();
}

void sectionHeader(const char* text) {
    if (g_bold_font != nullptr) ImGui::PushFont(g_bold_font, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, kText);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    if (g_bold_font != nullptr) ImGui::PopFont();
    // Linea fina desde el texto hasta el borde.
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    const float y = std::floor((min.y + max.y) * 0.5f) + 0.5f;
    const float x1 = ImGui::GetCurrentWindow()->WorkRect.Max.x;
    if (x1 > max.x + 8.0f) {
        ImGui::GetWindowDrawList()->AddLine(ImVec2(max.x + 8.0f, y), ImVec2(x1, y), kBorder, 1.0f);
    }
}

void badge(const char* text, ImU32 color) {
    const float size = ImGui::GetFontSize() * 0.84f;  // en pixeles (con DPI)
    ImFont* font = ImGui::GetFont();
    const ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    const ImVec2 pad(6.0f, 2.0f);
    const ImVec2 box(ts.x + pad.x * 2.0f, ts.y + pad.y * 2.0f);
    // Centrado en la altura de la linea (con o sin marco).
    const float line = ImGui::GetFrameHeight();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 a(p.x, p.y + std::floor((line - box.y) * 0.5f));
    ImGui::Dummy(ImVec2(box.x, line));
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(a, ImVec2(a.x + box.x, a.y + box.y), color, 3.0f);
    // Texto oscuro sobre colores claros (amarillo), blanco sobre el resto.
    const ImVec4 c = vec(color);
    const float luma = c.x * 0.299f + c.y * 0.587f + c.z * 0.114f;
    const ImU32 text_color = luma > 0.6f ? IM_COL32(14, 14, 16, 255) : IM_COL32(255, 255, 255, 255);
    draw->AddText(font, size, ImVec2(a.x + pad.x, a.y + pad.y), text_color, text);
}

bool searchBox(const char* id, std::string& text, const char* hint, float width) {
    ImGui::PushID(id);
    const float w = width > 0.0f ? width
                                 : std::max(ImGui::GetContentRegionAvail().x + (width < 0.0f ? width + 1.0f : 0.0f), 40.0f);
    const float h = ImGui::GetFrameHeight();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    // Hueco a la izquierda para la lupa y a la derecha para la x.
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(h + 2.0f, ImGui::GetStyle().FramePadding.y));
    ImGui::SetNextItemWidth(w);
    ImGui::SetNextItemAllowOverlap();  // la x de borrar va encima
    const bool changed = ImGui::InputTextWithHint("##search", hint, &text);
    ImGui::PopStyleVar();
    const bool focused = ImGui::IsItemActive();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    // Lupa.
    const ImVec2 c(p.x + h * 0.5f + 1.0f, p.y + h * 0.5f - 1.0f);
    const float r = h * 0.2f;
    const ImU32 glass = focused || !text.empty() ? kText : kTextDim;
    draw->AddCircle(c, r, glass, 16, 1.5f);
    draw->AddLine(ImVec2(c.x + r * 0.7f, c.y + r * 0.7f), ImVec2(c.x + r * 1.7f, c.y + r * 1.7f), glass, 1.6f);
    if (focused) {
        draw->AddRect(p, ImVec2(p.x + w, p.y + h), withAlpha(kRed, 200), ImGui::GetStyle().FrameRounding, 0, 1.0f);
    }
    bool cleared = false;
    if (!text.empty()) {
        ImGui::SameLine(0.0f, 0.0f);
        const ImVec2 bmin(p.x + w - h, p.y);
        ImGui::SetCursorScreenPos(bmin);
        if (ImGui::InvisibleButton("##clear", ImVec2(h, h))) {
            text.clear();
            cleared = true;
        }
        const ImU32 xc = ImGui::IsItemHovered() ? kText : kTextDim;
        const ImVec2 xm(bmin.x + h * 0.5f, bmin.y + h * 0.5f);
        const float s = h * 0.16f;
        draw->AddLine(ImVec2(xm.x - s, xm.y - s), ImVec2(xm.x + s, xm.y + s), xc, 1.5f);
        draw->AddLine(ImVec2(xm.x + s, xm.y - s), ImVec2(xm.x - s, xm.y + s), xc, 1.5f);
        ImGui::SetItemTooltip("Borrar");
    }
    ImGui::PopID();
    return changed || cleared;
}

bool kebabButton(const char* id, const ImVec2& min, float size) {
    ImGui::SetCursorScreenPos(min);
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(size, size));
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    if (hovered || ImGui::IsItemActive()) {
        draw->AddRectFilled(ImVec2(min.x + 2.0f, min.y + 2.0f), ImVec2(min.x + size - 2.0f, min.y + size - 2.0f),
                            kBg5, 3.0f);
    }
    const ImU32 dot = hovered ? kText : kTextDim;
    const float cx = std::floor(min.x + size * 0.5f) + 0.5f;
    const float cy = min.y + size * 0.5f;
    const float gap = std::max(3.5f, size * 0.17f);
    for (int i = -1; i <= 1; ++i) draw->AddCircleFilled(ImVec2(cx, cy + gap * static_cast<float>(i)), 1.6f, dot, 8);
    return pressed;
}

void smallText(ImU32 color, const char* fmt, ...) {
    char buffer[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    ImGui::PushFont(nullptr, smallFontSize());
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextUnformatted(buffer);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

namespace {

// Tarjetas SIN ImDrawListSplitter: partir la lista de dibujo de la ventana
// dejaba comandos con recortes equivocados (filas enteras del Inspector
// invisibles). El fondo se dibuja al empezar con el alto medido en el frame
// anterior (guardado en la ventana); si cambia, se corrige al siguiente.
constexpr float kCardPad = 5.0f;
struct Card {
    ImGuiID id = 0;
    float start_y = 0.0f;
};

std::vector<Card>& cardStack() {
    static std::vector<Card> stack;
    return stack;
}

}  // namespace

void beginCard(const char* id, ImU32 background, bool attached) {
    if (attached) ImGui::SetCursorPosY(ImGui::GetCursorPosY() - ImGui::GetStyle().ItemSpacing.y);
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    const ImGuiID key = ImGui::GetID(id);
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float height = window->StateStorage.GetFloat(key, 0.0f);
    if (height > 0.0f) {
        // El fondo sale un poco por los lados (dentro del margen de la
        // ventana, lo mismo que las cabeceras plegables).
        const float pad = ImGui::GetStyle().WindowPadding.x;
        const float x0 = start.x - std::max(IM_TRUNC(pad * 0.5f - 1.0f), 0.0f);
        const float x1 = window->WorkRect.Max.x + IM_TRUNC(pad * 0.5f);
        const ImDrawFlags corners = attached ? ImDrawFlags_RoundCornersBottom : ImDrawFlags_RoundCornersAll;
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(ImVec2(x0, start.y), ImVec2(x1, start.y + height), background, 4.0f, corners);
        // Sin flags de esquinas: AddRect con RoundCorners* rellenaba todo el panel.
        draw->AddRect(ImVec2(x0, start.y), ImVec2(x1, start.y + height), kBorder, 4.0f);
    }
    cardStack().push_back(Card{key, start.y});
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + kCardPad);
}

void endCard() {
    if (cardStack().empty()) return;
    const Card card = cardStack().back();
    cardStack().pop_back();
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    const float y1 = window->DC.CursorPos.y - ImGui::GetStyle().ItemSpacing.y + kCardPad;
    window->StateStorage.SetFloat(card.id, std::max(y1 - card.start_y, 0.0f));
    ImGui::Dummy(ImVec2(0.0f, kCardPad));
}

}  // namespace cramion::editor::theme
