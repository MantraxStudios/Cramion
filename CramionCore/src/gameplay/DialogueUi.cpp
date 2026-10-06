#include "CramionCore/gameplay/DialogueUi.h"

#include "CramionCore/gameplay/Accessibility.h"
#include <algorithm>
#include <cmath>
#include <string>

namespace cramion::gameplay {

namespace {

core::Vec4 rgba(const core::Vec3& c, float a) { return core::Vec4{c.x, c.y, c.z, a}; }

// Los primeros `count` caracteres (puntos de codigo UTF-8) del texto.
std::string utf8Prefix(const std::string& text, std::size_t count) {
    std::size_t i = 0;
    std::size_t seen = 0;
    while (i < text.size() && seen < count) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        std::size_t len = 1;
        if (c >= 0xF0) {
            len = 4;
        } else if (c >= 0xE0) {
            len = 3;
        } else if (c >= 0xC0) {
            len = 2;
        }
        i = std::min(text.size(), i + len);
        ++seen;
    }
    return text.substr(0, i);
}

std::size_t utf8Length(const std::string& text) {
    std::size_t n = 0;
    for (const char ch : text) {
        if ((static_cast<unsigned char>(ch) & 0xC0) != 0x80) ++n;
    }
    return n;
}

}  // namespace

bool updateDialogueBox(const DialogueBox& box, const ui::UiRect& rect, float scale, const ui::UiInput& input,
                       bool interactive, entt::entity entity, std::vector<ui::UiDrawCommand>& out) {
    DialogueSystem* dialogue = activeDialogue();
    if (dialogue == nullptr || !dialogue->active() || (!dialogue->hasLine() && !dialogue->waitingChoice())) return false;

    const float s = scale;
    const float pad = 22.0f * s;
    const DialogueLine& line = dialogue->line();
    const std::vector<DialogueChoice>& choices = dialogue->choices();

    // Texto visible (maquina de escribir).
    const std::size_t total = utf8Length(line.text);
    std::size_t visible = total;
    if (box.chars_per_second > 0.0f && !dialogue->lineRevealed()) {
        visible = std::min(total, static_cast<std::size_t>(std::max(0.0f, dialogue->lineTime()) * box.chars_per_second));
    }
    const bool typing = visible < total;

    // Opciones: una fila por opcion, encima del panel (de abajo a arriba).
    const float row_h = std::max(box.font_size * 1.55f, 24.0f) * s;
    const float gap = 8.0f * s;
    std::vector<ui::UiRect> choice_rects;
    if (!typing) {
        const float width = std::min(rect.w, std::max(rect.w * 0.55f, 360.0f * s));
        float y = rect.y - gap - row_h;
        for (std::size_t i = choices.size(); i-- > 0;) {
            (void)i;
            choice_rects.push_back(ui::UiRect{rect.x + rect.w - width, y, width, row_h});
            y -= row_h + gap;
        }
        std::reverse(choice_rects.begin(), choice_rects.end());
    }

    // --- Interaccion ---
    int hovered = -1;
    bool over = rect.contains(input.mouse_x, input.mouse_y);
    for (std::size_t i = 0; i < choice_rects.size(); ++i) {
        if (choice_rects[i].contains(input.mouse_x, input.mouse_y)) {
            hovered = static_cast<int>(i);
            over = true;
        }
    }
    if (interactive) {
        bool next = false;
        int pick = -1;
        if (input.mouse_released) {
            if (hovered >= 0) {
                pick = hovered;
            } else if (box.click_to_continue && rect.contains(input.mouse_x, input.mouse_y)) {
                next = true;
            }
        }
        if (input.enter) next = true;
        for (const char c : input.typed) {
            if (c == ' ') next = true;
            if (c >= '1' && c <= '9') pick = c - '1';
        }
        if (typing && (next || pick >= 0)) {
            dialogue->revealLine();  // primero se ve todo el texto
        } else if (!choices.empty()) {
            if (pick >= 0 && pick < static_cast<int>(choices.size())) dialogue->choose(pick);
        } else if (next) {
            dialogue->advance();
        }
        // Lo de arriba puede terminar el dialogo o cambiar de linea: se
        // dibuja lo de este frame igualmente (el siguiente ya va al dia).
    }

    // --- Dibujo ---
    ui::UiDrawCommand panel;
    panel.entity = entity;
    panel.rect = rect;
    panel.color = rgba(box.panel_color, box.panel_alpha);
    panel.radius = box.corner_radius * s;
    out.push_back(panel);

    float text_top = rect.y + pad;
    if (!line.speaker.empty()) {
        ui::UiDrawCommand name;
        name.type = ui::UiDrawCommand::Type::Text;
        name.entity = entity;
        name.rect = ui::UiRect{rect.x + pad, rect.y + pad * 0.7f, rect.w - pad * 2.0f, box.speaker_size * 1.3f * s};
        name.text = line.speaker;
        name.font_size = box.speaker_size * s * accessibility().subtitle_scale;
        name.color = rgba(box.speaker_color, 1.0f);
        name.h_align = 0;
        name.v_align = 0;
        name.shadow = true;
        out.push_back(name);
        text_top = name.rect.y + name.rect.h + 6.0f * s;
    }

    ui::UiDrawCommand body;
    body.type = ui::UiDrawCommand::Type::Text;
    body.entity = entity;
    body.rect = ui::UiRect{rect.x + pad, text_top, rect.w - pad * 2.0f, std::max(0.0f, rect.y + rect.h - pad - text_top)};
    body.text = utf8Prefix(line.text, visible);
    body.font_size = box.font_size * s * accessibility().subtitle_scale;
    body.color = rgba(box.text_color, 1.0f);
    body.h_align = 0;
    body.v_align = 0;
    body.wrap = true;
    if (accessibility().subtitle_background) {
        // Accesibilidad: fondo opaco detras del texto (se lee sobre cualquier imagen).
        ui::UiDrawCommand back;
        back.entity = entity;
        back.rect = ui::UiRect{rect.x + pad * 0.5f, text_top - 4.0f * s, rect.w - pad, body.rect.h + 8.0f * s};
        back.color = core::Vec4{0.0f, 0.0f, 0.0f, 0.85f};
        back.radius = 6.0f * s;
        out.push_back(back);
    }
    out.push_back(body);

    // Indicador de "sigue" (parpadea) cuando la linea esta entera.
    if (box.show_hint && !typing && choices.empty()) {
        const float blink = 0.55f + 0.45f * std::sin(dialogue->lineTime() * 5.0f);
        ui::UiDrawCommand hint;
        hint.type = ui::UiDrawCommand::Type::Circle;
        hint.entity = entity;
        const float r = 6.0f * s;
        hint.rect = ui::UiRect{rect.x + rect.w - pad - r * 2.0f, rect.y + rect.h - pad - r * 2.0f, r * 2.0f, r * 2.0f};
        hint.color = rgba(box.speaker_color, blink);
        out.push_back(hint);
    }

    for (std::size_t i = 0; i < choice_rects.size() && i < choices.size(); ++i) {
        const DialogueChoice& c = choices[i];
        ui::UiDrawCommand bg;
        bg.entity = entity;
        bg.rect = choice_rects[i];
        const bool hot = static_cast<int>(i) == hovered && c.enabled;
        bg.color = rgba(hot ? box.choice_hover : box.choice_color, c.enabled ? 0.95f : 0.45f);
        bg.radius = 8.0f * s;
        out.push_back(bg);
        ui::UiDrawCommand label;
        label.type = ui::UiDrawCommand::Type::Text;
        label.entity = entity;
        label.rect = ui::UiRect{bg.rect.x + 14.0f * s, bg.rect.y, bg.rect.w - 28.0f * s, bg.rect.h};
        label.text = std::to_string(i + 1) + ".  " + c.text;
        label.font_size = box.font_size * 0.85f * s * accessibility().subtitle_scale;
        label.color = c.enabled ? core::Vec4{1.0f, 1.0f, 1.0f, 1.0f} : core::Vec4{0.7f, 0.7f, 0.72f, 0.8f};
        label.h_align = 0;
        label.v_align = 1;
        out.push_back(label);
    }
    return over;
}

}  // namespace cramion::gameplay
