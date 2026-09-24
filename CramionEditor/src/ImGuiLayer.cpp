#include "ImGuiLayer.h"

#include <imgui_impl_vulkan.h>
#include <imgui_impl_win32.h>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <stdexcept>

// El backend de Win32 no declara su manejador de mensajes (para no obligar a
// incluir <windows.h>): se declara aqui.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wparam,
                                                             LPARAM lparam);

namespace cramion::editor {

namespace {

void checkVk(VkResult result) {
    if (result != VK_SUCCESS) {
        std::cerr << "[ImGui] Error de Vulkan: " << static_cast<int>(result) << "\n";
    }
}

}  // namespace

void ImGuiLayer::initialize(HWND hwnd, gfx::VulkanRenderer& renderer) {
    renderer_ = &renderer;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_NavEnableKeyboard;

    // El diseño de los paneles se guarda junto al ejecutable.
    ini_path_ = (gfx::shaders::directory().parent_path() / "CramionEditor.ini").string();
    io.IniFilename = ini_path_.c_str();

    const float dpi_scale = ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd);

    // Segoe UI (la de Windows) en lugar de la fuente de pixeles de ImGui.
    const std::filesystem::path font = "C:/Windows/Fonts/segoeui.ttf";
    if (std::filesystem::exists(font)) {
        io.Fonts->AddFontFromFileTTF(font.string().c_str(), 17.0f);
    }
    applyStyle(dpi_scale);

    ImGui_ImplWin32_Init(hwnd);

    // La plataforma Win32 no sabe de Vulkan: la superficie de una ventana la
    // crea la aplicacion. Hace falta aunque los paneles no salgan de la
    // ventana principal (el backend de Vulkan lo comprueba al iniciarse).
    ImGui::GetPlatformIO().Platform_CreateVkSurface =
        [](ImGuiViewport* viewport, ImU64 instance, const void* allocator,
           ImU64* out_surface) -> int {
        VkWin32SurfaceCreateInfoKHR info{};
        info.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
        info.hwnd = static_cast<HWND>(viewport->PlatformHandleRaw);
        info.hinstance = GetModuleHandleW(nullptr);
        return static_cast<int>(vkCreateWin32SurfaceKHR(
            reinterpret_cast<VkInstance>(instance), &info,
            static_cast<const VkAllocationCallbacks*>(allocator),
            reinterpret_cast<VkSurfaceKHR*>(out_surface)));
    };

    const gfx::VulkanRenderer::NativeHandles handles = renderer.nativeHandles();
    device_ = handles.device;
    color_format_ = handles.swapchain_format;
    image_count_ = handles.image_count;

    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = handles.api_version;
    info.Instance = handles.instance;
    info.PhysicalDevice = handles.physical_device;
    info.Device = handles.device;
    info.QueueFamily = handles.queue_family;
    info.Queue = handles.queue;
    // El backend crea su propio pool: fuente + la imagen de la escena.
    info.DescriptorPoolSize = 64;
    info.MinImageCount = std::max(2u, handles.image_count);
    info.ImageCount = std::max(2u, handles.image_count);
    info.UseDynamicRendering = true;
    info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    info.PipelineInfoMain.PipelineRenderingCreateInfo.sType =
        VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR;
    info.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    info.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &color_format_;
    info.CheckVkResultFn = checkVk;
    if (!ImGui_ImplVulkan_Init(&info)) {
        throw std::runtime_error("No se pudo iniciar el backend de Vulkan de Dear ImGui.");
    }

    // ImGui dibuja dentro del ultimo pase del renderizador, encima de todo.
    renderer.setOverlayCallback([](VkCommandBuffer cmd) {
        if (ImDrawData* draw_data = ImGui::GetDrawData()) {
            ImGui_ImplVulkan_RenderDrawData(draw_data, cmd);
        }
    });

    initialized_ = true;
}

void ImGuiLayer::shutdown() {
    if (!initialized_) {
        return;
    }
    renderer_->waitIdle();
    renderer_->setOverlayCallback(nullptr);
    if (scene_set_ != VK_NULL_HANDLE) {
        ImGui_ImplVulkan_RemoveTexture(scene_set_);
        scene_set_ = VK_NULL_HANDLE;
    }
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    initialized_ = false;
}

void ImGuiLayer::beginFrame() {
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
}

void ImGuiLayer::endFrame() {
    ImGui::Render();
}

ImTextureID ImGuiLayer::sceneTexture() {
    const std::uint64_t generation = renderer_->sceneImageGeneration();
    if (generation != scene_generation_ || scene_set_ == VK_NULL_HANDLE) {
        // La imagen anterior ya no existe: nada en vuelo puede usar su set.
        if (scene_set_ != VK_NULL_HANDLE) {
            renderer_->waitIdle();
            ImGui_ImplVulkan_RemoveTexture(scene_set_);
        }
        scene_set_ = ImGui_ImplVulkan_AddTexture(renderer_->sceneImageView(),
                                                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        scene_generation_ = generation;

        // La swapchain nueva puede tener otro numero de imagenes.
        const std::uint32_t image_count = renderer_->nativeHandles().image_count;
        if (image_count != image_count_ && image_count >= 2) {
            ImGui_ImplVulkan_SetMinImageCount(image_count);
            image_count_ = image_count;
        }
    }
    return static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(scene_set_));
}

// Tema oscuro sobrio, en la linea del editor de Unity: grises neutros, acento
// azul para la seleccion y bordes suaves.
void ImGuiLayer::applyStyle(float dpi_scale) {
    ImGuiStyle& style = ImGui::GetStyle();
    ImGui::StyleColorsDark(&style);

    style.WindowRounding = 3.0f;
    style.FrameRounding = 3.0f;
    style.GrabRounding = 3.0f;
    style.TabRounding = 3.0f;
    style.ScrollbarRounding = 3.0f;
    style.WindowBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.WindowPadding = ImVec2(8.0f, 8.0f);
    style.FramePadding = ImVec2(6.0f, 4.0f);
    style.ItemSpacing = ImVec2(8.0f, 5.0f);
    style.IndentSpacing = 14.0f;

    ImVec4* c = style.Colors;
    const ImVec4 background{0.16f, 0.16f, 0.17f, 1.0f};
    const ImVec4 panel{0.20f, 0.20f, 0.21f, 1.0f};
    const ImVec4 frame{0.13f, 0.13f, 0.14f, 1.0f};
    const ImVec4 hover{0.28f, 0.28f, 0.30f, 1.0f};
    const ImVec4 accent{0.17f, 0.36f, 0.53f, 1.0f};
    const ImVec4 accent_hover{0.23f, 0.45f, 0.65f, 1.0f};

    c[ImGuiCol_WindowBg] = panel;
    c[ImGuiCol_ChildBg] = panel;
    c[ImGuiCol_PopupBg] = ImVec4{0.18f, 0.18f, 0.19f, 0.98f};
    c[ImGuiCol_MenuBarBg] = background;
    c[ImGuiCol_Border] = ImVec4{0.10f, 0.10f, 0.10f, 1.0f};
    c[ImGuiCol_FrameBg] = frame;
    c[ImGuiCol_FrameBgHovered] = hover;
    c[ImGuiCol_FrameBgActive] = hover;
    c[ImGuiCol_TitleBg] = background;
    c[ImGuiCol_TitleBgActive] = background;
    c[ImGuiCol_TitleBgCollapsed] = background;
    c[ImGuiCol_Header] = accent;
    c[ImGuiCol_HeaderHovered] = accent_hover;
    c[ImGuiCol_HeaderActive] = accent_hover;
    c[ImGuiCol_Button] = ImVec4{0.27f, 0.27f, 0.29f, 1.0f};
    c[ImGuiCol_ButtonHovered] = ImVec4{0.34f, 0.34f, 0.36f, 1.0f};
    c[ImGuiCol_ButtonActive] = accent;
    c[ImGuiCol_CheckMark] = ImVec4{0.45f, 0.70f, 0.95f, 1.0f};
    c[ImGuiCol_SliderGrab] = ImVec4{0.45f, 0.62f, 0.85f, 1.0f};
    c[ImGuiCol_SliderGrabActive] = ImVec4{0.55f, 0.72f, 0.95f, 1.0f};
    c[ImGuiCol_Tab] = background;
    c[ImGuiCol_TabHovered] = hover;
    c[ImGuiCol_TabSelected] = panel;
    c[ImGuiCol_TabDimmed] = background;
    c[ImGuiCol_TabDimmedSelected] = panel;
    c[ImGuiCol_TabSelectedOverline] = accent_hover;
    c[ImGuiCol_DockingPreview] = ImVec4{0.23f, 0.45f, 0.65f, 0.7f};
    c[ImGuiCol_Separator] = ImVec4{0.12f, 0.12f, 0.12f, 1.0f};
    c[ImGuiCol_PlotHistogram] = ImVec4{0.45f, 0.62f, 0.85f, 1.0f};

    style.ScaleAllSizes(dpi_scale);
    style.FontScaleDpi = dpi_scale;
}

}  // namespace cramion::editor
