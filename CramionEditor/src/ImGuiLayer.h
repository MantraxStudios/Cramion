#ifndef CRAMION_EDITOR_IMGUI_LAYER_H
#define CRAMION_EDITOR_IMGUI_LAYER_H

#include <CramionFX/CramionFX.h>

#include <imgui.h>

#include <cstdint>
#include <string>

namespace cramion::editor {

// Dear ImGui sobre el renderizador: comparte su dispositivo, su cola y su
// swapchain (backend de Vulkan con renderizado dinamico) y se dibuja en su
// ultimo pase (VulkanRenderer::setOverlayCallback). La ventana es la de
// CramionDM (backend de Win32 por su gancho de mensajes).
class ImGuiLayer {
public:
    void initialize(HWND hwnd, gfx::VulkanRenderer& renderer);
    void shutdown();

    // Entre beginFrame() y endFrame() se construye la interfaz; endFrame()
    // la deja lista para que el renderizador la dibuje en drawFrame().
    void beginFrame();
    void endFrame();

    // La imagen final de la escena como textura de ImGui. Se vuelve a
    // registrar si el renderizador la recreo (ventana redimensionada).
    ImTextureID sceneTexture();

private:
    void applyStyle(float dpi_scale);

    gfx::VulkanRenderer* renderer_ = nullptr;
    VkDevice device_ = VK_NULL_HANDLE;
    VkFormat color_format_ = VK_FORMAT_UNDEFINED;  // debe vivir mientras el backend
    VkDescriptorSet scene_set_ = VK_NULL_HANDLE;
    std::uint64_t scene_generation_ = ~0ull;
    std::uint32_t image_count_ = 0;
    std::string ini_path_;
    bool initialized_ = false;
};

}  // namespace cramion::editor

#endif  // CRAMION_EDITOR_IMGUI_LAYER_H
