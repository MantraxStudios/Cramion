#include "ImGuiLayer.h"

#include <CramionFX/asset/ImageFile.h>

#include <cstdlib>

#include <chrono>

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
        io.Fonts->AddFontFromFileTTF(font.string().c_str(), 16.0f);
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
    // (+ iconos y miniaturas del navegador de proyecto.)
    info.DescriptorPoolSize = 1024;
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

    loadIcons();

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
    for (VkDescriptorSet& set : view_sets_) {
        if (set != VK_NULL_HANDLE) {
            ImGui_ImplVulkan_RemoveTexture(set);
            set = VK_NULL_HANDLE;
        }
    }
    // Miniaturas e iconos (la GPU ya esta parada).
    std::vector<std::uint32_t> textures = icon_textures_;
    for (ImTextureID& id : icons_) {
        if (id != 0) ImGui_ImplVulkan_RemoveTexture(reinterpret_cast<VkDescriptorSet>(static_cast<std::uintptr_t>(id)));
        id = 0;
    }
    for (auto& [path, thumb] : thumbnails_) {
        if (thumb.decoding.valid()) thumb.decoding.wait();
        if (thumb.set != VK_NULL_HANDLE) ImGui_ImplVulkan_RemoveTexture(thumb.set);
        if (thumb.texture != 0) textures.push_back(thumb.texture);
    }
    thumbnails_.clear();
    renderer_->destroyUiTextures(textures);
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

// -----------------------------------------------------------------------------
// Iconos
// -----------------------------------------------------------------------------

void ImGuiLayer::loadIcons() {
    // Los copia CMake junto al ejecutable (editor_icons/); si no, se prueba
    // la carpeta del codigo.
    std::vector<std::filesystem::path> folders = {gfx::shaders::directory().parent_path() / "editor_icons"};
#ifdef CRAMION_EDITOR_ICON_SOURCE
    folders.emplace_back(CRAMION_EDITOR_ICON_SOURCE);
#endif
    std::filesystem::path folder;
    for (const std::filesystem::path& f : folders) {
        std::error_code error;
        if (std::filesystem::is_directory(f, error)) {
            folder = f;
            break;
        }
    }
    if (folder.empty()) {
        std::cerr << "[Editor] No se encontraron los iconos (editor_icons/)\n";
        return;
    }
    // Los archivos van numerados: NN_nombre.png, en el orden de Icon.
    std::vector<std::filesystem::path> files;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
        if (entry.path().extension() == ".png") files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    int loaded = 0;
    for (const std::filesystem::path& file : files) {
        const std::string stem = file.stem().string();
        const int number = std::atoi(stem.c_str());
        if (number < 1 || number > static_cast<int>(Icon::Count)) continue;
        asset::ImageRgba8 image;
        if (!asset::loadImageRgba8(file, image, 128)) continue;
        const std::uint32_t texture = renderer_->createUiTexture(image.pixels.data(), image.width, image.height);
        if (texture == 0) continue;
        icon_textures_.push_back(texture);
        const VkDescriptorSet set =
            ImGui_ImplVulkan_AddTexture(renderer_->uiTextureView(texture), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        icons_[static_cast<std::size_t>(number - 1)] = static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(set));
        ++loaded;
    }
    std::cout << "[Editor] " << loaded << " iconos cargados de " << folder.string() << "\n";

    // Logo del motor (copiado como editor_icons/logo.png).
    std::vector<std::filesystem::path> logos = {folder / "logo.png"};
#ifdef CRAMION_EDITOR_LOGO_SOURCE
    logos.emplace_back(CRAMION_EDITOR_LOGO_SOURCE);
#endif
    for (const std::filesystem::path& file : logos) {
        asset::ImageRgba8 image;
        if (!asset::loadImageRgba8(file, image, 256)) continue;
        const std::uint32_t texture = renderer_->createUiTexture(image.pixels.data(), image.width, image.height);
        if (texture == 0) continue;
        icon_textures_.push_back(texture);
        const VkDescriptorSet set =
            ImGui_ImplVulkan_AddTexture(renderer_->uiTextureView(texture), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        logo_ = static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(set));
        break;
    }
}

void ImGuiLayer::drawIcon(ImDrawList* draw, Icon id, ImVec2 min, float size, ImU32 tint) const {
    const ImTextureID texture = icon(id);
    if (texture == 0) {
        draw->AddRectFilled(min, ImVec2(min.x + size, min.y + size), tint, size * 0.2f);
        return;
    }
    draw->AddImage(texture, min, ImVec2(min.x + size, min.y + size), ImVec2(0, 0), ImVec2(1, 1), tint);
}

void ImGuiLayer::image(Icon id, float size, ImU32 tint) const {
    const ImTextureID texture = icon(id);
    if (texture == 0) {
        ImGui::Dummy(ImVec2(size, size));
        return;
    }
    ImGui::ImageWithBg(texture, ImVec2(size, size), ImVec2(0, 0), ImVec2(1, 1), ImVec4(0, 0, 0, 0),
                       ImGui::ColorConvertU32ToFloat4(tint));
}

// -----------------------------------------------------------------------------
// Miniaturas
// -----------------------------------------------------------------------------

ImTextureID ImGuiLayer::thumbnail(const std::filesystem::path& file, ImVec2* size) {
    return loadTexture(file, size, 128);
}

ImTextureID ImGuiLayer::image(const std::filesystem::path& file, ImVec2* size) {
    return loadTexture(file, size, 4096);
}

ImTextureID ImGuiLayer::loadTexture(const std::filesystem::path& file, ImVec2* size, std::uint32_t max_size) {
    std::error_code error;
    const std::filesystem::file_time_type stamp = std::filesystem::last_write_time(file, error);
    Thumbnail& thumb = thumbnails_[file.wstring() + L"|" + std::to_wstring(max_size)];
    thumb.max_size = max_size;
    thumb.last_used = frame_;
    // Nuevo o cambiado en disco: decodificar en otro hilo.
    if ((!thumb.decoding.valid() && thumb.set == VK_NULL_HANDLE && !thumb.failed) || (!error && stamp != thumb.stamp)) {
        if (thumb.decoding.valid()) return 0;  // aun decodificando la version anterior
        thumb.stamp = stamp;
        thumb.failed = false;
        auto pixels = std::make_shared<Thumbnail::Pixels>();
        thumb.pixels = pixels;
        thumb.decoding = std::async(std::launch::async, [file, pixels, max_size]() {
            asset::ImageRgba8 image;
            if (!asset::loadImageRgba8(file, image, max_size)) return false;
            pixels->width = image.width;
            pixels->height = image.height;
            pixels->rgba = std::move(image.pixels);
            return true;
        });
    }
    if (size != nullptr) *size = thumb.size;
    return thumb.set != VK_NULL_HANDLE ? static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(thumb.set)) : 0;
}

void ImGuiLayer::updateThumbnails() {
    ++frame_;
    // Subir las que terminaron (como mucho unas pocas por frame).
    int uploads = 0;
    std::vector<std::uint32_t> replaced;
    std::vector<VkDescriptorSet> replaced_sets;
    for (auto& [path, thumb] : thumbnails_) {
        if (uploads >= 6) break;
        if (!thumb.decoding.valid() ||
            thumb.decoding.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            continue;
        }
        const bool ok = thumb.decoding.get();
        if (!ok || thumb.pixels == nullptr || thumb.pixels->rgba.empty()) {
            thumb.failed = true;
            continue;
        }
        if (thumb.set != VK_NULL_HANDLE) {
            replaced_sets.push_back(thumb.set);
            replaced.push_back(thumb.texture);
        }
        thumb.texture = renderer_->createUiTexture(thumb.pixels->rgba.data(), thumb.pixels->width, thumb.pixels->height);
        thumb.size = ImVec2(static_cast<float>(thumb.pixels->width), static_cast<float>(thumb.pixels->height));
        thumb.set = thumb.texture != 0 ? ImGui_ImplVulkan_AddTexture(renderer_->uiTextureView(thumb.texture),
                                                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
                                       : VK_NULL_HANDLE;
        thumb.pixels.reset();
        ++uploads;
    }
    // Las que llevan mas de ~10 s sin verse (otra carpeta), fuera: de una vez.
    if (frame_ % 120 == 0 || !replaced.empty()) {
        for (auto it = thumbnails_.begin(); it != thumbnails_.end();) {
            Thumbnail& thumb = it->second;
            if (frame_ - thumb.last_used > 600 && !thumb.decoding.valid()) {
                if (thumb.set != VK_NULL_HANDLE) {
                    replaced_sets.push_back(thumb.set);
                    replaced.push_back(thumb.texture);
                }
                it = thumbnails_.erase(it);
            } else {
                ++it;
            }
        }
    }
    if (!replaced.empty()) {
        renderer_->waitIdle();
        for (VkDescriptorSet set : replaced_sets) ImGui_ImplVulkan_RemoveTexture(set);
        renderer_->destroyUiTextures(replaced);
    }
}

ImTextureID ImGuiLayer::viewTexture(std::uint32_t slot) {
    slot = slot < view_sets_.size() ? slot : 0;
    const std::uint64_t generation = renderer_->sceneImageGeneration() + 1;  // 0 = sin registrar
    VkDescriptorSet& set = view_sets_[slot];
    if (generation != view_generations_[slot] || set == VK_NULL_HANDLE) {
        // La imagen anterior ya no existe: nada en vuelo puede usar su set.
        if (set != VK_NULL_HANDLE) {
            renderer_->waitIdle();
            ImGui_ImplVulkan_RemoveTexture(set);
        }
        set = ImGui_ImplVulkan_AddTexture(renderer_->viewImageView(slot), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        view_generations_[slot] = generation;

        // La swapchain nueva puede tener otro numero de imagenes.
        const std::uint32_t image_count = renderer_->nativeHandles().image_count;
        if (image_count != image_count_ && image_count >= 2) {
            ImGui_ImplVulkan_SetMinImageCount(image_count);
            image_count_ = image_count;
        }
    }
    return static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(set));
}

// Tema oscuro sobrio, en la linea del editor de Unity: grises neutros, acento
// azul para la seleccion y bordes suaves.
void ImGuiLayer::applyStyle(float dpi_scale) {
    ImGuiStyle& style = ImGui::GetStyle();
    ImGui::StyleColorsDark(&style);

    // Formas: suaves, con aire (como Unreal 5 / los editores actuales).
    style.WindowRounding = 6.0f;
    style.ChildRounding = 6.0f;
    style.FrameRounding = 5.0f;
    style.PopupRounding = 8.0f;
    style.GrabRounding = 4.0f;
    style.TabRounding = 6.0f;
    style.ScrollbarRounding = 8.0f;
    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;
    style.TabBorderSize = 0.0f;
    style.TabBarBorderSize = 1.0f;
    style.TabBarOverlineSize = 2.0f;
    style.WindowPadding = ImVec2(10.0f, 10.0f);
    style.FramePadding = ImVec2(8.0f, 5.0f);
    style.CellPadding = ImVec2(6.0f, 4.0f);
    style.ItemSpacing = ImVec2(8.0f, 6.0f);
    style.ItemInnerSpacing = ImVec2(6.0f, 4.0f);
    style.IndentSpacing = 16.0f;
    style.ScrollbarSize = 12.0f;
    style.GrabMinSize = 10.0f;
    style.SeparatorTextBorderSize = 2.0f;
    style.SeparatorTextPadding = ImVec2(12.0f, 4.0f);
    style.DockingSeparatorSize = 2.0f;
    style.WindowMenuButtonPosition = ImGuiDir_None;

    // Colores: grises azulados oscuros y el azul del logo como acento.
    ImVec4* c = style.Colors;
    const ImVec4 base{0.078f, 0.082f, 0.094f, 1.0f};      // fondo (barra, pestanas)
    const ImVec4 panel{0.114f, 0.118f, 0.133f, 1.0f};     // paneles
    const ImVec4 frame{0.157f, 0.163f, 0.184f, 1.0f};     // campos
    const ImVec4 frame_hover{0.200f, 0.208f, 0.235f, 1.0f};
    const ImVec4 border{0.200f, 0.208f, 0.235f, 1.0f};
    const ImVec4 accent{0.000f, 0.560f, 0.950f, 1.0f};    // #008FF2
    const ImVec4 accent_soft{0.000f, 0.560f, 0.950f, 0.35f};
    const ImVec4 accent_mid{0.000f, 0.560f, 0.950f, 0.55f};
    const ImVec4 text{0.902f, 0.910f, 0.929f, 1.0f};

    c[ImGuiCol_Text] = text;
    c[ImGuiCol_TextDisabled] = ImVec4{0.529f, 0.549f, 0.600f, 1.0f};
    c[ImGuiCol_WindowBg] = panel;
    c[ImGuiCol_ChildBg] = ImVec4{0.0f, 0.0f, 0.0f, 0.0f};
    c[ImGuiCol_PopupBg] = ImVec4{0.125f, 0.130f, 0.149f, 0.98f};
    c[ImGuiCol_Border] = border;
    c[ImGuiCol_BorderShadow] = ImVec4{0, 0, 0, 0};
    c[ImGuiCol_FrameBg] = frame;
    c[ImGuiCol_FrameBgHovered] = frame_hover;
    c[ImGuiCol_FrameBgActive] = ImVec4{0.227f, 0.239f, 0.278f, 1.0f};
    c[ImGuiCol_TitleBg] = base;
    c[ImGuiCol_TitleBgActive] = base;
    c[ImGuiCol_TitleBgCollapsed] = base;
    c[ImGuiCol_MenuBarBg] = base;
    c[ImGuiCol_ScrollbarBg] = ImVec4{0, 0, 0, 0};
    c[ImGuiCol_ScrollbarGrab] = ImVec4{0.260f, 0.270f, 0.305f, 1.0f};
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4{0.330f, 0.342f, 0.385f, 1.0f};
    c[ImGuiCol_ScrollbarGrabActive] = accent_mid;
    c[ImGuiCol_CheckMark] = accent;
    c[ImGuiCol_SliderGrab] = accent;
    c[ImGuiCol_SliderGrabActive] = ImVec4{0.30f, 0.72f, 1.0f, 1.0f};
    c[ImGuiCol_Button] = ImVec4{0.180f, 0.188f, 0.212f, 1.0f};
    c[ImGuiCol_ButtonHovered] = ImVec4{0.235f, 0.247f, 0.282f, 1.0f};
    c[ImGuiCol_ButtonActive] = accent_mid;
    c[ImGuiCol_Header] = ImVec4{0.000f, 0.420f, 0.720f, 0.40f};       // seleccion (Jerarquia)
    c[ImGuiCol_HeaderHovered] = ImVec4{0.220f, 0.232f, 0.265f, 1.0f};
    c[ImGuiCol_HeaderActive] = accent_mid;
    c[ImGuiCol_Separator] = border;
    c[ImGuiCol_SeparatorHovered] = accent_mid;
    c[ImGuiCol_SeparatorActive] = accent;
    c[ImGuiCol_ResizeGrip] = ImVec4{0, 0, 0, 0};
    c[ImGuiCol_ResizeGripHovered] = accent_soft;
    c[ImGuiCol_ResizeGripActive] = accent_mid;
    c[ImGuiCol_Tab] = base;
    c[ImGuiCol_TabHovered] = ImVec4{0.180f, 0.188f, 0.212f, 1.0f};
    c[ImGuiCol_TabSelected] = panel;
    c[ImGuiCol_TabSelectedOverline] = accent;
    c[ImGuiCol_TabDimmed] = base;
    c[ImGuiCol_TabDimmedSelected] = panel;
    c[ImGuiCol_TabDimmedSelectedOverline] = ImVec4{0.35f, 0.37f, 0.42f, 1.0f};
    c[ImGuiCol_DockingPreview] = accent_mid;
    c[ImGuiCol_DockingEmptyBg] = base;
    c[ImGuiCol_PlotLines] = accent;
    c[ImGuiCol_PlotHistogram] = accent;
    c[ImGuiCol_TableHeaderBg] = frame;
    c[ImGuiCol_TableBorderStrong] = border;
    c[ImGuiCol_TableBorderLight] = ImVec4{0.160f, 0.168f, 0.190f, 1.0f};
    c[ImGuiCol_TextSelectedBg] = accent_soft;
    c[ImGuiCol_DragDropTarget] = accent;
    c[ImGuiCol_NavCursor] = accent;
    c[ImGuiCol_ModalWindowDimBg] = ImVec4{0.0f, 0.0f, 0.0f, 0.55f};

    style.ScaleAllSizes(dpi_scale);
    style.FontScaleDpi = dpi_scale;
}

}  // namespace cramion::editor
