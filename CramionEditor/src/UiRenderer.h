#ifndef CRAMION_EDITOR_UI_RENDERER_H
#define CRAMION_EDITOR_UI_RENDERER_H

// Dibuja la lista de la interfaz del juego (ui::UiSystem) con ImGui: en la
// vista Juego del editor y en el juego exportado (CramionPlayer).

#include "ImGuiLayer.h"

#include <CramionCore/ui/UI.h>

#include <imgui.h>

#include <filesystem>
#include <string>
#include <vector>

namespace cramion::editor {

void drawUiList(ImDrawList* draw, ImVec2 origin, const std::vector<ui::UiDrawCommand>& list, ImGuiLayer& imgui,
                const std::filesystem::path& assets_root);

// La entrada de ImGui (raton relativo a `origin`, teclas escritas) para la UI.
ui::UiInput uiInputFromImGui(ImVec2 origin, ImVec2 size, bool hovered, bool typing);

}  // namespace cramion::editor

#endif  // CRAMION_EDITOR_UI_RENDERER_H
