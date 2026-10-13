#ifndef CRAMION_EDITOR_UI_RENDERER_H
#define CRAMION_EDITOR_UI_RENDERER_H

// Dibuja la lista de la interfaz del juego (ui::UiSystem) con ImGui: en la
// vista Juego del editor y en el juego exportado (CramionPlayer).

#include "ImGuiLayer.h"

#include <CramionCore/ui/UI.h>
#include <CramionFX/vk/WorldUiPass.h>

#include <imgui.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace cramion::editor {

void drawUiList(ImDrawList* draw, ImVec2 origin, const std::vector<ui::UiDrawCommand>& list, ImGuiLayer& imgui,
                const std::filesystem::path& assets_root);

// Una Render Texture (.crrt) en una UI Image (p. ej. la de un VideoPlayer):
// su id en el renderizador por su ruta en Assets (RenderSync::
// renderTextureIdForAsset) y, en `size`, su tamano (VulkanRenderer::
// renderTextureExtent). -1 si no existe. Lo ponen el editor y el juego; sin
// el, una .crrt en una imagen se ve como hueco vacio.
using UiRenderTextureResolver = std::function<std::int32_t(const std::string& relative, ImVec2* size)>;
void setUiRenderTextureResolver(UiRenderTextureResolver resolver);

// La entrada de ImGui (raton relativo a `origin`, teclas escritas) para la UI.
ui::UiInput uiInputFromImGui(ImVec2 origin, ImVec2 size, bool hovered, bool typing);

// La UI en el mundo (Canvas en modo Mundo) para el render: la lista de cada
// canvas pintada con ImGui (las mismas letras e imagenes que en la pantalla),
// como geometria que el render dibuja en su textura (setWorldUi). Dentro del
// frame de ImGui.
std::vector<gfx::WorldUiCanvas> buildWorldUi(const std::vector<ui::WorldCanvasDraw>& canvases, ImGuiLayer& imgui,
                                             const std::filesystem::path& assets_root);

}  // namespace cramion::editor

#endif  // CRAMION_EDITOR_UI_RENDERER_H
