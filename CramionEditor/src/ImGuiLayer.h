#ifndef CRAMION_EDITOR_IMGUI_LAYER_H
#define CRAMION_EDITOR_IMGUI_LAYER_H

#include <CramionFX/CramionFX.h>

#include <imgui.h>

#include <cstdint>
#include <array>
#include <filesystem>
#include <future>
#include <string>
#include <unordered_map>
#include <vector>

namespace cramion::editor {

// Iconos del editor: los PNG de CramionEditor/assets/gizmos (glifos blancos
// sobre transparente: se tinen con el color que se quiera). El orden es el de
// sus nombres de archivo (01_move.png ... 33_folder_open.png).
enum class Icon : int {
    Move = 0,
    Rotate,
    Scale,
    PointLight,
    DirectionalLight,
    SpotLight,
    AreaLight,
    AmbientLight,
    AudioSource,
    AudioListener,
    ColliderSphere,
    ColliderBox,
    ColliderCapsule,
    ColliderMesh,
    MeshRenderer,
    SkinnedMesh,
    Camera,
    ParticleSystem,
    Rigidbody,
    CharacterController,
    NavMeshAgent,
    NavMeshObstacle,
    ReflectionProbe,
    LightProbe,
    Decal,
    Terrain,
    FogVolume,
    TriggerVolume,
    Joint,
    Waypoint,
    AssetBrowser,
    FolderClosed,
    FolderOpen,
    Count
};

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

    // La imagen de una vista (0 = Escena, 1 = Juego; VulkanRenderer::
    // setViewSlot) como textura de ImGui. Se vuelve a registrar si el
    // renderizador la recreo (ventana redimensionada).
    ImTextureID viewTexture(std::uint32_t slot);

    // Icono del editor (0 si no se pudo cargar: quien lo usa dibuja algo en
    // su lugar).
    ImTextureID icon(Icon id) const { return icons_[static_cast<std::size_t>(id)]; }
    // Logo del motor (assets/icon.png): barra de menus y Hub. 0 si falta.
    ImTextureID logo() const { return logo_; }
    // Dibuja un icono en una lista de dibujo (tenido con `tint`).
    void drawIcon(ImDrawList* draw, Icon id, ImVec2 min, float size, ImU32 tint) const;
    // Widget: el icono como imagen (tamano en pixeles).
    void image(Icon id, float size, ImU32 tint = IM_COL32_WHITE) const;

    // Miniatura de una imagen de disco (PNG, JPG, TGA, HDR...). La primera
    // vez se decodifica en otro hilo y devuelve 0 hasta que esta lista (se
    // suben unas pocas por frame para no dar tirones). Si el archivo cambia,
    // se rehace.
    // `size` (opcional) recibe su tamano en pixeles (para no deformarla).
    ImTextureID thumbnail(const std::filesystem::path& file, ImVec2* size = nullptr);
    // Una vez por frame: sube las miniaturas decodificadas y libera las que
    // hace tiempo que no se usan.
    void updateThumbnails();

private:
    void applyStyle(float dpi_scale);

    gfx::VulkanRenderer* renderer_ = nullptr;
    VkDevice device_ = VK_NULL_HANDLE;
    VkFormat color_format_ = VK_FORMAT_UNDEFINED;  // debe vivir mientras el backend
    std::array<VkDescriptorSet, gfx::VulkanRenderer::kViewSlots> view_sets_{};
    std::array<std::uint64_t, gfx::VulkanRenderer::kViewSlots> view_generations_{};
    std::uint32_t image_count_ = 0;

    void loadIcons();
    std::array<ImTextureID, static_cast<std::size_t>(Icon::Count)> icons_{};
    ImTextureID logo_ = 0;
    std::vector<std::uint32_t> icon_textures_;

    struct Thumbnail {
        std::future<bool> decoding;
        struct Pixels {
            std::uint32_t width = 0;
            std::uint32_t height = 0;
            std::vector<std::uint8_t> rgba;
        };
        std::shared_ptr<Pixels> pixels;
        std::filesystem::file_time_type stamp{};
        std::uint32_t texture = 0;
        VkDescriptorSet set = VK_NULL_HANDLE;
        ImVec2 size{1.0f, 1.0f};
        bool failed = false;
        std::uint64_t last_used = 0;
    };
    std::unordered_map<std::wstring, Thumbnail> thumbnails_;
    std::uint64_t frame_ = 0;
    std::string ini_path_;
    bool initialized_ = false;
};

}  // namespace cramion::editor

#endif  // CRAMION_EDITOR_IMGUI_LAYER_H
