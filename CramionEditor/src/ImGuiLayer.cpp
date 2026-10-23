#include "ImGuiLayer.h"
#include <iterator>
#include <mutex>
#include <cstring>
#include <fstream>

#include <CramionFX/vk/VulkanCompat.h>

#include "Theme.h"

#include <CramionFX/asset/Dds.h>
#include <CramionFX/asset/ImageFile.h>
#include <CramionFX/asset/TextureCompression.h>

#include <cstdlib>

#include <chrono>

#include <imgui_impl_vulkan.h>
#if defined(_WIN32)
#include <imgui_impl_win32.h>
#endif

#include <algorithm>
#include <cfloat>
#include <initializer_list>
#include <filesystem>
#include <iostream>
#include <stdexcept>

#if defined(_WIN32)
// El backend de Win32 no declara su manejador de mensajes (para no obligar a
// incluir <windows.h>): se declara aqui.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wparam,
                                                             LPARAM lparam);
#endif

#if defined(__linux__) && !defined(__ANDROID__)
#include <X11/Xlib.h>
#include <X11/Xresource.h>
#include <cmath>
#include <cstdio>
#include <regex>
#include <string>
#endif

namespace cramion::editor {

namespace {

void checkVk(VkResult result) {
    if (result != VK_SUCCESS) {
        std::cerr << "[ImGui] Error de Vulkan: " << static_cast<int>(result) << "\n";
    }
}

#if defined(__linux__) && !defined(__ANDROID__)

// Escala de XWayland en GNOME: con escalado fraccionario, mutter agranda la
// pantalla X a la mayor escala (redondeada hacia arriba) y deja que cada app
// se escale sola, pero Xft.dpi lleva la escala del monitor principal (96 si
// es 1). La escala real es el ancho de la pantalla X entre el ancho logico
// del escritorio (DisplayConfig de mutter). 0 si no se sabe.
float xwaylandScale(Display* dpy) {
    if (std::getenv("WAYLAND_DISPLAY") == nullptr) return 0.0f;
    FILE* pipe = popen("gdbus call --session --dest org.gnome.Mutter.DisplayConfig "
                       "--object-path /org/gnome/Mutter/DisplayConfig "
                       "--method org.gnome.Mutter.DisplayConfig.GetCurrentState 2>/dev/null",
                       "r");
    if (pipe == nullptr) return 0.0f;
    std::string state;
    char buffer[4096];
    for (size_t n; (n = std::fread(buffer, 1, sizeof(buffer), pipe)) > 0;) state.append(buffer, n);
    if (pclose(pipe) != 0 || state.empty()) return 0.0f;
    // Solo en disposicion logica (1); en la fisica X no esta agrandada.
    if (state.find("'layout-mode': <uint32 1>") == std::string::npos) return 0.0f;
    // Monitores logicos: (x, y, escala, rotacion, principal, [('conector', ...
    static const std::regex logical(R"(\((-?\d+), (-?\d+), ([0-9.]+), (?:uint32 )?\d+, (?:true|false), \[\('([^']+)')");
    static const std::regex mode(R"(\('[^']*', (\d+), (\d+), )");
    double extent = 0.0;
    for (std::sregex_iterator it(state.begin(), state.end(), logical), end; it != end; ++it) {
        const double x = std::stod((*it)[1]);
        const double scale = std::stod((*it)[3]);
        // Modo actual del conector: el ultimo modo antes de su 'is-current'.
        const size_t monitor = state.find("(('" + (*it)[4].str() + "',");
        if (monitor == std::string::npos || scale <= 0.0) return 0.0f;
        const size_t current = state.find("'is-current': <true>", monitor);
        if (current == std::string::npos) return 0.0f;
        double width = 0.0;
        const std::string modes = state.substr(monitor, current - monitor);
        for (std::sregex_iterator m(modes.begin(), modes.end(), mode); m != end; ++m) width = std::stod((*m)[1]);
        if (width <= 0.0) return 0.0f;
        extent = std::max(extent, x + width / scale);
    }
    if (extent <= 0.0) return 0.0f;
    const double ratio = DisplayWidth(dpy, DefaultScreen(dpy)) / extent;
    // Al cuarto mas cercano (el redondeo de mutter descuadra algun pixel).
    const float rounded = static_cast<float>(std::round(ratio * 4.0) / 4.0);
    return rounded >= 1.0f && rounded <= 4.0f ? rounded : 0.0f;
}

// Escala de la interfaz: CRAMION_UI_SCALE, o Xft.dpi del escritorio (96 = 1)
// y la escala de XWayland, la mayor.
float linuxUiScale(HWND hwnd) {
    if (const char* forced = std::getenv("CRAMION_UI_SCALE")) {
        const float value = std::strtof(forced, nullptr);
        if (value >= 0.5f && value <= 4.0f) return value;
    }
    if (hwnd == nullptr || hwnd->display == nullptr) return 1.0f;
    auto* dpy = static_cast<Display*>(hwnd->display);
    const char* resources = XResourceManagerString(dpy);
    if (resources == nullptr) return 1.0f;
    XrmInitialize();
    XrmDatabase db = XrmGetStringDatabase(resources);
    float scale = 1.0f;
    char* type = nullptr;
    XrmValue value{};
    if (db != nullptr && XrmGetResource(db, "Xft.dpi", "Xft.Dpi", &type, &value) && value.addr != nullptr) {
        const float dpi = std::strtof(value.addr, nullptr);
        if (dpi >= 48.0f && dpi <= 384.0f) scale = dpi / 96.0f;
    }
    if (db != nullptr) XrmDestroyDatabase(db);
    return std::max(scale, xwaylandScale(dpy));
}

// La primera fuente que exista de la lista.
const char* firstFont(std::initializer_list<const char*> candidates) {
    for (const char* font : candidates) {
        std::error_code error;
        if (std::filesystem::exists(font, error)) return font;
    }
    return nullptr;
}

ImGuiKey imguiKey(dm::Key key) {
    using dm::Key;
    if (key >= Key::A && key <= Key::Z) {
        return static_cast<ImGuiKey>(ImGuiKey_A + (static_cast<int>(key) - static_cast<int>(Key::A)));
    }
    if (key >= Key::Num0 && key <= Key::Num9) {
        return static_cast<ImGuiKey>(ImGuiKey_0 + (static_cast<int>(key) - static_cast<int>(Key::Num0)));
    }
    if (key >= Key::F1 && key <= Key::F12) {
        return static_cast<ImGuiKey>(ImGuiKey_F1 + (static_cast<int>(key) - static_cast<int>(Key::F1)));
    }
    if (key >= Key::Keypad0 && key <= Key::Keypad9) {
        return static_cast<ImGuiKey>(ImGuiKey_Keypad0 + (static_cast<int>(key) - static_cast<int>(Key::Keypad0)));
    }
    switch (key) {
        case Key::Backspace: return ImGuiKey_Backspace;
        case Key::Tab: return ImGuiKey_Tab;
        case Key::Enter: return ImGuiKey_Enter;
        case Key::Pause: return ImGuiKey_Pause;
        case Key::CapsLock: return ImGuiKey_CapsLock;
        case Key::Escape: return ImGuiKey_Escape;
        case Key::Space: return ImGuiKey_Space;
        case Key::PageUp: return ImGuiKey_PageUp;
        case Key::PageDown: return ImGuiKey_PageDown;
        case Key::End: return ImGuiKey_End;
        case Key::Home: return ImGuiKey_Home;
        case Key::Left: return ImGuiKey_LeftArrow;
        case Key::Up: return ImGuiKey_UpArrow;
        case Key::Right: return ImGuiKey_RightArrow;
        case Key::Down: return ImGuiKey_DownArrow;
        case Key::PrintScreen: return ImGuiKey_PrintScreen;
        case Key::Insert: return ImGuiKey_Insert;
        case Key::Delete: return ImGuiKey_Delete;
        case Key::LeftSuper: return ImGuiKey_LeftSuper;
        case Key::RightSuper: return ImGuiKey_RightSuper;
        case Key::Apps: return ImGuiKey_Menu;
        case Key::KeypadMultiply: return ImGuiKey_KeypadMultiply;
        case Key::KeypadAdd: return ImGuiKey_KeypadAdd;
        case Key::KeypadSubtract: return ImGuiKey_KeypadSubtract;
        case Key::KeypadDecimal: return ImGuiKey_KeypadDecimal;
        case Key::KeypadDivide: return ImGuiKey_KeypadDivide;
        case Key::NumLock: return ImGuiKey_NumLock;
        case Key::ScrollLock: return ImGuiKey_ScrollLock;
        case Key::LeftShift: return ImGuiKey_LeftShift;
        case Key::RightShift: return ImGuiKey_RightShift;
        case Key::LeftControl: return ImGuiKey_LeftCtrl;
        case Key::RightControl: return ImGuiKey_RightCtrl;
        case Key::LeftAlt: return ImGuiKey_LeftAlt;
        case Key::RightAlt: return ImGuiKey_RightAlt;
        case Key::Semicolon: return ImGuiKey_Semicolon;
        case Key::Equal: return ImGuiKey_Equal;
        case Key::Comma: return ImGuiKey_Comma;
        case Key::Minus: return ImGuiKey_Minus;
        case Key::Period: return ImGuiKey_Period;
        case Key::Slash: return ImGuiKey_Slash;
        case Key::GraveAccent: return ImGuiKey_GraveAccent;
        case Key::LeftBracket: return ImGuiKey_LeftBracket;
        case Key::Backslash: return ImGuiKey_Backslash;
        case Key::RightBracket: return ImGuiKey_RightBracket;
        case Key::Apostrophe: return ImGuiKey_Apostrophe;
        default: return ImGuiKey_None;
    }
}

#endif

}  // namespace

void ImGuiLayer::initialize(HWND hwnd, gfx::VulkanRenderer& renderer) {
    renderer_ = &renderer;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_NavEnableKeyboard;

#if defined(_WIN32)
    // El diseño de los paneles se guarda junto al ejecutable.
    ini_path_ = (gfx::shaders::directory().parent_path() / "CramionEditor.ini").string();
    io.IniFilename = ini_path_.c_str();

    const float dpi_scale = ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd);

    // Segoe UI (la de Windows) en lugar de la fuente de pixeles de ImGui.
    const std::filesystem::path font = "C:/Windows/Fonts/segoeui.ttf";
    if (std::filesystem::exists(font)) {
        io.Fonts->AddFontFromFileTTF(font.string().c_str(), 16.0f);
    }
    for (const char* mono : {"C:/Windows/Fonts/CascadiaMono.ttf", "C:/Windows/Fonts/consola.ttf"}) {
        if (!std::filesystem::exists(mono)) continue;
        mono_font_ = io.Fonts->AddFontFromFileTTF(mono, 16.0f);
        // Lo que no tenga (los simbolos de las herramientas de IA: ⏺ ✻ ⎿...).
        ImFontConfig merge;
        merge.MergeMode = true;
        for (const char* extra : {"C:/Windows/Fonts/seguisym.ttf", "C:/Windows/Fonts/segoeui.ttf"}) {
            if (std::filesystem::exists(extra)) io.Fonts->AddFontFromFileTTF(extra, 16.0f, &merge);
        }
        break;
    }
    // Seminegrita para titulos (cabeceras de componentes, secciones).
    for (const char* bold : {"C:/Windows/Fonts/seguisb.ttf", "C:/Windows/Fonts/segoeuib.ttf"}) {
        if (!std::filesystem::exists(bold)) continue;
        theme::g_bold_font = io.Fonts->AddFontFromFileTTF(bold, 16.0f);
        break;
    }
    applyStyle(dpi_scale);

    ImGui_ImplWin32_Init(hwnd);
#elif defined(__linux__) && !defined(__ANDROID__)
    // Linux: sin backend de plataforma de ImGui; la entrada llega por
    // onEvent (eventos de la ventana X11 de CramionDM). El diseno de los
    // paneles se guarda junto al ejecutable, como en Windows.
    ini_path_ = (gfx::shaders::directory().parent_path() / "CramionEditor.ini").string();
    io.IniFilename = ini_path_.c_str();
    io.BackendPlatformName = "cramion_x11";
    io.BackendFlags |= ImGuiBackendFlags_HasMouseCursors;
    const float dpi_scale = content_scale_ > 0.0f ? content_scale_ : linuxUiScale(hwnd);
    // Noto Sans / DejaVu / Liberation en lugar de Segoe UI.
    if (const char* font = firstFont({"/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
                                      "/usr/share/fonts/noto/NotoSans-Regular.ttf",
                                      "/usr/share/fonts/truetype/ubuntu/Ubuntu-R.ttf",
                                      "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                                      "/usr/share/fonts/TTF/DejaVuSans.ttf",
                                      "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf"})) {
        io.Fonts->AddFontFromFileTTF(font, 16.0f);
    }
    if (const char* mono = firstFont({"/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
                                      "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
                                      "/usr/share/fonts/truetype/noto/NotoSansMono-Regular.ttf",
                                      "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
                                      "/usr/share/fonts/truetype/ubuntu/UbuntuMono-R.ttf"})) {
        mono_font_ = io.Fonts->AddFontFromFileTTF(mono, 16.0f);
        // Lo que no tenga (los simbolos de las herramientas de IA: ⏺ ✻ ⎿...).
        ImFontConfig merge;
        merge.MergeMode = true;
        for (const char* extra : {"/usr/share/fonts/truetype/noto/NotoSansSymbols-Regular.ttf",
                                  "/usr/share/fonts/truetype/noto/NotoSansSymbols2-Regular.ttf",
                                  "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"}) {
            std::error_code error;
            if (std::filesystem::exists(extra, error)) io.Fonts->AddFontFromFileTTF(extra, 16.0f, &merge);
        }
    }
    // Seminegrita para titulos (cabeceras de componentes, secciones).
    if (const char* bold = firstFont({"/usr/share/fonts/truetype/noto/NotoSans-SemiBold.ttf",
                                      "/usr/share/fonts/truetype/noto/NotoSans-Bold.ttf",
                                      "/usr/share/fonts/noto/NotoSans-Bold.ttf",
                                      "/usr/share/fonts/truetype/ubuntu/Ubuntu-M.ttf",
                                      "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
                                      "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf"})) {
        theme::g_bold_font = io.Fonts->AddFontFromFileTTF(bold, 16.0f);
    }
    applyStyle(dpi_scale);
    last_frame_ = std::chrono::steady_clock::now();
#else
    // Android: sin .ini, Roboto (la del sistema) y la escala de la pantalla.
    // La entrada la da el player (tactil convertido a raton).
    (void)hwnd;
    io.IniFilename = nullptr;
    io.BackendPlatformName = "cramion_android";
    io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;
    const float dpi_scale = content_scale_ > 0.0f ? content_scale_ : 1.0f;
    for (const char* font : {"/system/fonts/Roboto-Regular.ttf", "/system/fonts/DroidSans.ttf"}) {
        if (std::filesystem::exists(font)) {
            io.Fonts->AddFontFromFileTTF(font, 16.0f);
            break;
        }
    }
    applyStyle(dpi_scale);
    last_frame_ = std::chrono::steady_clock::now();
#endif

    // La plataforma Win32 no sabe de Vulkan: la superficie de una ventana la
    // crea la aplicacion. Hace falta aunque los paneles no salgan de la
    // ventana principal (el backend de Vulkan lo comprueba al iniciarse).
#if defined(_WIN32)
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
#endif

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
    // Sin dynamic rendering (moviles con Vulkan 1.0-1.2): el ultimo pase es
    // un render pass del modo compatible y la interfaz usa uno compatible.
    info.UseDynamicRendering = gfx::compat::caps().dynamic_rendering;
    info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    if (info.UseDynamicRendering) {
        info.PipelineInfoMain.PipelineRenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR;
        info.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
        info.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &color_format_;
    } else {
        info.PipelineInfoMain.RenderPass =
            gfx::compat::compatibleRenderPass({static_cast<vk::Format>(color_format_)});
    }
    info.CheckVkResultFn = checkVk;
    if (!ImGui_ImplVulkan_Init(&info)) {
        throw std::runtime_error("No se pudo iniciar el backend de Vulkan de Dear ImGui.");
    }

    loadIcons();

    // ImGui dibuja dentro del ultimo pase del renderizador, encima de todo.
    // (Con la cola puesta: las texturas de ImGui 1.92 se suben con un submit
    // propio, y el streaming de modelos la usa desde otros hilos.)
    std::mutex* queue_mutex = &renderer.device().queueMutex();
    renderer.setOverlayCallback([queue_mutex](VkCommandBuffer cmd) {
        if (ImDrawData* draw_data = ImGui::GetDrawData()) {
            const std::lock_guard lock(*queue_mutex);
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
        if (id != 0) ImGui_ImplVulkan_RemoveTexture(toDescriptorSet(id));
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
#if defined(_WIN32)
    ImGui_ImplWin32_Shutdown();
#endif
    ImGui::DestroyContext();
    initialized_ = false;
}

void ImGuiLayer::beginFrame() {
    ImGui_ImplVulkan_NewFrame();
#if defined(_WIN32)
    ImGui_ImplWin32_NewFrame();
#else
    ImGuiIO& io = ImGui::GetIO();
#if defined(__linux__) && !defined(__ANDROID__)
    if (window_ != nullptr) {
        display_width_ = window_->width();
        display_height_ = window_->height();
        // La forma del cursor que pide ImGui (redimensionar, texto...).
        if (!(io.ConfigFlags & ImGuiConfigFlags_NoMouseCursorChange)) {
            static constexpr dm::SystemCursor kCursors[] = {
                dm::SystemCursor::Arrow,    dm::SystemCursor::TextInput,  dm::SystemCursor::ResizeAll,
                dm::SystemCursor::ResizeNS, dm::SystemCursor::ResizeEW,   dm::SystemCursor::ResizeNESW,
                dm::SystemCursor::ResizeNWSE, dm::SystemCursor::Hand,     dm::SystemCursor::Wait,
                dm::SystemCursor::Wait,     dm::SystemCursor::NotAllowed};
            const ImGuiMouseCursor cursor = ImGui::GetMouseCursor();
            if (cursor == ImGuiMouseCursor_None || io.MouseDrawCursor) {
                window_->setCursorVisible(false);
            } else {
                window_->setCursorVisible(true);
                if (cursor >= 0 && cursor < static_cast<int>(std::size(kCursors))) window_->setCursorShape(kCursors[cursor]);
            }
        }
    }
#endif
    io.DisplaySize = ImVec2(static_cast<float>(display_width_), static_cast<float>(display_height_));
    io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
    const auto now = std::chrono::steady_clock::now();
    io.DeltaTime = std::max(std::chrono::duration<float>(now - last_frame_).count(), 1.0f / 1000.0f);
    last_frame_ = now;
#endif
    ImGui::NewFrame();
}

#if defined(__linux__) && !defined(__ANDROID__)
void ImGuiLayer::setWindow(dm::Window* window) {
    window_ = window;
    if (window != nullptr) {
        display_width_ = window->width();
        display_height_ = window->height();
    }
    ImGuiPlatformIO& platform = ImGui::GetPlatformIO();
    platform.Platform_ClipboardUserData = this;
    platform.Platform_SetClipboardTextFn = [](ImGuiContext*, const char* text) {
        auto* self = static_cast<ImGuiLayer*>(ImGui::GetPlatformIO().Platform_ClipboardUserData);
        if (self->window_ != nullptr) self->window_->setClipboardText(text != nullptr ? text : "");
    };
    platform.Platform_GetClipboardTextFn = [](ImGuiContext*) -> const char* {
        auto* self = static_cast<ImGuiLayer*>(ImGui::GetPlatformIO().Platform_ClipboardUserData);
        self->clipboard_ = self->window_ != nullptr ? self->window_->clipboardText() : std::string{};
        return self->clipboard_.c_str();
    };
}

void ImGuiLayer::onEvent(const dm::Event& e) {
    if (!initialized_) return;
    ImGuiIO& io = ImGui::GetIO();
    const auto updateMods = [&io](dm::KeyMods mods) {
        const auto has = [mods](dm::KeyMods m) { return (mods & m) != dm::KeyMods{}; };
        io.AddKeyEvent(ImGuiMod_Ctrl, has(dm::KeyMods::Control));
        io.AddKeyEvent(ImGuiMod_Shift, has(dm::KeyMods::Shift));
        io.AddKeyEvent(ImGuiMod_Alt, has(dm::KeyMods::Alt));
        io.AddKeyEvent(ImGuiMod_Super, has(dm::KeyMods::Super));
    };
    switch (e.type) {
        case dm::EventType::MouseMoved:
            io.AddMouseSourceEvent(ImGuiMouseSource_Mouse);
            io.AddMousePosEvent(e.mouseX, e.mouseY);
            break;
        case dm::EventType::MouseLeave:
            // Con un boton pulsado (arrastrando fuera) se conserva la posicion.
            if (!ImGui::IsAnyMouseDown()) io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
            break;
        case dm::EventType::MouseButtonPressed:
        case dm::EventType::MouseButtonReleased: {
            int button = -1;
            switch (e.button) {
                case dm::MouseButton::Left: button = 0; break;
                case dm::MouseButton::Right: button = 1; break;
                case dm::MouseButton::Middle: button = 2; break;
                case dm::MouseButton::X1: button = 3; break;
                case dm::MouseButton::X2: button = 4; break;
                default: break;
            }
            if (button < 0) break;
            updateMods(e.mods);
            io.AddMouseSourceEvent(ImGuiMouseSource_Mouse);
            io.AddMousePosEvent(e.mouseX, e.mouseY);
            io.AddMouseButtonEvent(button, e.type == dm::EventType::MouseButtonPressed);
            break;
        }
        case dm::EventType::MouseScrolled:
            io.AddMouseWheelEvent(e.scrollX, e.scrollY);
            break;
        case dm::EventType::KeyPressed:
        case dm::EventType::KeyReleased: {
            updateMods(e.mods);
            const ImGuiKey key = imguiKey(e.key);
            if (key != ImGuiKey_None) io.AddKeyEvent(key, e.type == dm::EventType::KeyPressed);
            break;
        }
        case dm::EventType::TextInput:
            io.AddInputCharacter(e.codepoint);
            break;
        case dm::EventType::WindowFocus:
        case dm::EventType::WindowLostFocus:
            io.AddFocusEvent(e.type == dm::EventType::WindowFocus);
            break;
        case dm::EventType::WindowResize:
            display_width_ = e.width;
            display_height_ = e.height;
            break;
        default:
            break;
    }
}
#endif

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
        icons_[static_cast<std::size_t>(number - 1)] = toTextureId(set);
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
        logo_ = toTextureId(set);
        break;
    }

    // Banner del motor (junto al ejecutable): el dialogo de carga del proyecto.
    {
        asset::ImageRgba8 image;
        if (asset::loadImageRgba8(folder.parent_path() / "player_banner.png", image, 1920)) {
            const std::uint32_t texture = renderer_->createUiTexture(image.pixels.data(), image.width, image.height);
            if (texture != 0) {
                icon_textures_.push_back(texture);
                const VkDescriptorSet set = ImGui_ImplVulkan_AddTexture(renderer_->uiTextureView(texture),
                                                                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                banner_ = toTextureId(set);
            }
        }
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

ImFont* ImGuiLayer::uiFont(const std::filesystem::path& file) {
    const std::u8string key8 = file.lexically_normal().u8string();
    const std::string key(key8.begin(), key8.end());
    if (const auto it = ui_fonts_.find(key); it != ui_fonts_.end()) return it->second;
    ImFont* font = nullptr;
    std::error_code ec;
    if (std::filesystem::is_regular_file(file, ec)) {
        std::ifstream in(file, std::ios::binary);
        std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (bytes.size() > 16) {
            // ImGui se queda con la memoria (FontDataOwnedByAtlas).
            void* data = IM_ALLOC(bytes.size());
            std::memcpy(data, bytes.data(), bytes.size());
            ImFontConfig config;
            config.FontDataOwnedByAtlas = true;
            font = ImGui::GetIO().Fonts->AddFontFromMemoryTTF(data, static_cast<int>(bytes.size()), 16.0f, &config);
            if (font == nullptr) std::cerr << "[UI] No se pudo leer la fuente " << key << "\n";
        }
    }
    ui_fonts_[key] = font;  // tambien los fallos: no se reintenta cada frame
    return font;
}

ImTextureID ImGuiLayer::image(const std::filesystem::path& file, ImVec2* size) {
    // Hasta 2048 de lado: la interfaz no necesita mas y una de 4096 en RGBA8
    // con sus mips eran 85 MB de VRAM.
    return loadTexture(file, size, 2048);
}

ImTextureID ImGuiLayer::uiImage(const std::filesystem::path& assets_root, const std::string& relative, ImVec2* size) {
    // native(): comparar las cadenas tal cual (path::operator!= las descompone
    // por elementos, y esto va por cada imagen de cada frame).
    if (assets_root.native() != ui_refs_root_.native()) {  // otro proyecto: las rutas ya no valen
        ui_refs_root_ = assets_root;
        ui_image_refs_.clear();
        ui_font_refs_.clear();
    }
    auto it = ui_image_refs_.find(relative);
    if (it == ui_image_refs_.end()) {
        UiImageRef ref;
        ref.file = assets_root / std::filesystem::path(std::u8string(relative.begin(), relative.end()));
        ref.key = ref.file.wstring() + L"|" + std::to_wstring(2048u);  // la misma clave que image()
        it = ui_image_refs_.emplace(relative, std::move(ref)).first;
    }
    return loadTexture(it->second.file, it->second.key, size, 2048);
}

ImFont* ImGuiLayer::uiFont(const std::filesystem::path& assets_root, const std::string& relative) {
    if (assets_root.native() != ui_refs_root_.native()) {
        ui_refs_root_ = assets_root;
        ui_image_refs_.clear();
        ui_font_refs_.clear();
    }
    if (const auto it = ui_font_refs_.find(relative); it != ui_font_refs_.end()) return it->second;
    ImFont* font = uiFont(assets_root / std::filesystem::path(std::u8string(relative.begin(), relative.end())));
    ui_font_refs_.emplace(relative, font);
    return font;
}

ImTextureID ImGuiLayer::loadTexture(const std::filesystem::path& file, ImVec2* size, std::uint32_t max_size) {
    return loadTexture(file, file.wstring() + L"|" + std::to_wstring(max_size), size, max_size);
}

ImTextureID ImGuiLayer::loadTexture(const std::filesystem::path& file, const std::wstring& key, ImVec2* size,
                                    std::uint32_t max_size) {
    Thumbnail& thumb = thumbnails_[key];
    thumb.max_size = max_size;
    thumb.last_used = frame_;
    // La fecha del archivo (para recargarlo si cambia en disco) se mira al
    // pedirlo por primera vez y despues cada 1,5 s, no en cada frame: con
    // muchos iconos de la interfaz eran muchas llamadas al sistema por frame.
    constexpr auto kStampInterval = std::chrono::milliseconds(1500);
    const bool fresh = !thumb.decoding.valid() && thumb.set == VK_NULL_HANDLE && !thumb.failed;
    const auto now = std::chrono::steady_clock::now();
    std::error_code error;
    std::filesystem::file_time_type stamp = thumb.stamp;
    if (fresh || now - thumb.stamp_checked >= kStampInterval) {
        stamp = std::filesystem::last_write_time(file, error);
        thumb.stamp_checked = now;
    }
    // Nuevo o cambiado en disco: decodificar en otro hilo.
    if (fresh || (!error && stamp != thumb.stamp)) {
        if (thumb.decoding.valid()) return 0;  // aun decodificando la version anterior
        thumb.stamp = stamp;
        thumb.failed = false;
        auto pixels = std::make_shared<Thumbnail::Pixels>();
        thumb.pixels = pixels;
        // En una GPU sin BC no se intenta el camino BC7 (ni comprimir ni
        // descomprimir despues): RGBA8 como antes.
        const bool bc_supported = renderer_ != nullptr && renderer_->device().textureCompressionBcSupported();
        thumb.decoding = std::async(std::launch::async, [file, pixels, max_size, bc_supported]() {
            // Las imagenes de la interfaz (image/uiImage, hasta 2048): BC7 de
            // la cache de texturas si no pierden nitidez (loadUiTexture).
            if (bc_supported && max_size >= 2048) {
                try {
                    asset::TextureData bc;
                    if (asset::loadUiTexture(file, max_size, bc) && bc.format != asset::TextureFormat::Rgba8 &&
                        !bc.pixels.empty()) {
                        pixels->width = bc.width;
                        pixels->height = bc.height;
                        pixels->compressed = std::move(bc);
                        return true;
                    }
                } catch (const std::exception&) {
                    // (p. ej. un nombre que no se puede pasar a la pagina de
                    // codigos al escribir el registro): RGBA8, como antes.
                }
            }
            asset::ImageRgba8 image;
            if (!asset::loadImageRgba8(file, image, max_size)) return false;
            pixels->width = image.width;
            pixels->height = image.height;
            pixels->rgba = std::move(image.pixels);
            return true;
        });
    }
    if (size != nullptr) *size = thumb.size;
    return thumb.set != VK_NULL_HANDLE ? toTextureId(thumb.set) : 0;
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
        if (ok && thumb.pixels != nullptr && !thumb.pixels->compressed.pixels.empty() &&
            !renderer_->device().textureCompressionBcSupported()) {
            // La GPU no lee BC: se descomprime (solo el nivel 0).
            thumb.pixels->rgba = asset::decodeBlockCompressed(thumb.pixels->compressed);
            thumb.pixels->compressed = asset::TextureData{};
        }
        const bool compressed = ok && thumb.pixels != nullptr && !thumb.pixels->compressed.pixels.empty();
        if (!ok || thumb.pixels == nullptr || (thumb.pixels->rgba.empty() && !compressed)) {
            thumb.failed = true;
            continue;
        }
        if (thumb.set != VK_NULL_HANDLE) {
            replaced_sets.push_back(thumb.set);
            replaced.push_back(thumb.texture);
        }
        thumb.texture = compressed ? renderer_->createUiTextureCompressed(thumb.pixels->compressed)
                                   : renderer_->createUiTexture(thumb.pixels->rgba.data(), thumb.pixels->width,
                                                                thumb.pixels->height);
        if (compressed && thumb.texture == 0) {
            // No se pudo subir en BC: se descomprime (nivel 0) y va en RGBA8.
            thumb.pixels->rgba = asset::decodeBlockCompressed(thumb.pixels->compressed);
            thumb.pixels->compressed = asset::TextureData{};
            if (!thumb.pixels->rgba.empty()) {
                thumb.texture = renderer_->createUiTexture(thumb.pixels->rgba.data(), thumb.pixels->width,
                                                           thumb.pixels->height);
            }
        }
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
            const std::lock_guard lock(renderer_->device().queueMutex());  // hace vkDeviceWaitIdle
            ImGui_ImplVulkan_SetMinImageCount(image_count);
            image_count_ = image_count;
        }
    }
    return toTextureId(set);
}

ImTextureID ImGuiLayer::renderTexture(std::int32_t id) {
    const VkImageView view = renderer_->renderTextureView(id);
    if (view == VK_NULL_HANDLE) return 0;
    const std::uint64_t generation = renderer_->renderTextureGeneration() + 1;
    auto& [registered, set] = render_texture_sets_[id];
    if (registered != generation || set == VK_NULL_HANDLE) {
        if (set != VK_NULL_HANDLE) {
            renderer_->waitIdle();
            ImGui_ImplVulkan_RemoveTexture(set);
        }
        set = ImGui_ImplVulkan_AddTexture(view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        registered = generation;
    }
    return toTextureId(set);
}

// Tema del editor (Theme.h): negros en capas, texto blanco, rojo para lo
// principal y la seleccion, amarillo para avisos y estados.
void ImGuiLayer::applyStyle(float dpi_scale) {
    ImGuiStyle& style = ImGui::GetStyle();
    theme::applyStyle(style);

    style.ScaleAllSizes(dpi_scale);
    style.FontScaleDpi = dpi_scale;
}

}  // namespace cramion::editor
