#ifndef CRAMION_UPDATER_NOTES_VIEW_H
#define CRAMION_UPDATER_NOTES_VIEW_H

// Notas de una version (Markdown sencillo) dibujadas con Dear ImGui. La usan
// el actualizador y el Hub del editor (cada uno con su ImGui).

#include <CramionUpdater/Update.h>

#include <imgui.h>

#include <vector>

namespace cramion::update {

// `bold`: fuente para los titulos (nullptr = la actual).
inline void drawReleaseNotes(const std::vector<NoteLine>& lines, ImU32 accent, ImFont* bold = nullptr) {
    const float base = ImGui::GetFontSize();
    ImGui::PushTextWrapPos(0.0f);
    for (const NoteLine& line : lines) {
        switch (line.kind) {
            case NoteLine::Kind::Heading: {
                const float size = line.level <= 1 ? base * 1.45f : line.level == 2 ? base * 1.25f : base * 1.1f;
                ImGui::Dummy(ImVec2(0.0f, line.level <= 2 ? base * 0.35f : 0.0f));
                ImGui::PushFont(bold, size);
                ImGui::TextUnformatted(line.text.c_str());
                ImGui::PopFont();
                if (line.level <= 2) {
                    const ImVec2 p = ImGui::GetCursorScreenPos();
                    ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y - 2.0f),
                                                        ImVec2(p.x + ImGui::GetContentRegionAvail().x, p.y - 2.0f),
                                                        ImGui::GetColorU32(ImGuiCol_Separator));
                    ImGui::Dummy(ImVec2(0.0f, 2.0f));
                }
                break;
            }
            case NoteLine::Kind::Bullet: {
                const float indent = base * (0.4f + 1.1f * static_cast<float>(line.level));
                ImGui::Indent(indent);
                const ImVec2 p = ImGui::GetCursorScreenPos();
                ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + base * 0.3f, p.y + base * 0.62f), base * 0.16f,
                                                            accent);
                ImGui::Indent(base * 0.95f);
                ImGui::TextUnformatted(line.text.c_str());
                ImGui::Unindent(base * 0.95f);
                ImGui::Unindent(indent);
                break;
            }
            case NoteLine::Kind::Text:
                ImGui::TextUnformatted(line.text.c_str());
                break;
            case NoteLine::Kind::Blank:
                ImGui::Dummy(ImVec2(0.0f, base * 0.35f));
                break;
        }
    }
    ImGui::PopTextWrapPos();
}

}  // namespace cramion::update

#endif  // CRAMION_UPDATER_NOTES_VIEW_H
