#ifndef CRAMION_VK_WORLD_UI_PASS_H
#define CRAMION_VK_WORLD_UI_PASS_H

#include "CramionFX/core/Math.h"
#include "CramionFX/vk/VulkanBuffer.h"
#include "CramionFX/vk/VulkanCommon.h"
#include "CramionFX/vk/VulkanImage.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace cramion::gfx {

class VulkanDevice;

// Un vertice de la UI: como ImDrawVert (posicion en pixeles de la textura,
// uv, color RGBA8 con gamma). 20 bytes.
struct WorldUiVertex {
    float x = 0.0f;
    float y = 0.0f;
    float u = 0.0f;
    float v = 0.0f;
    std::uint32_t color = 0xFFFFFFFFu;
};

// Un lote: triangulos con una textura. `texture` es un descriptor set con un
// combined image sampler en el binding 0 del fragment shader (los de ImGui
// sirven tal cual); nulo = blanco (formas lisas).
struct WorldUiBatch {
    VkDescriptorSet texture = VK_NULL_HANDLE;
    std::uint32_t first_index = 0;
    std::uint32_t index_count = 0;
    std::uint32_t vertex_offset = 0;
    float clip[4] = {0.0f, 0.0f, 0.0f, 0.0f};  // x0, y0, x1, y1 en pixeles
};

// Un Canvas en el mundo: lo que se pinta en su textura (width x height) y el
// panel que la muestra: `transform` (centro y giro, sin escala) y `size` en
// metros; el panel mira hacia +Z (X a la derecha, Y arriba).
struct WorldUiCanvas {
    std::uint64_t id = 0;  // estable entre frames (la entidad del Canvas)
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    core::Mat4 transform = core::Mat4::identity();
    core::Vec2 size{};
    float opacity = 1.0f;
    std::vector<WorldUiVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<WorldUiBatch> batches;
};

// UI en el mundo (Canvas en modo Mundo, la de VR): cada canvas se pinta en
// su propia textura (como el RenderMode World Space de Unity) y se dibuja
// como un panel en la escena, despues del tono (no le afectan la exposicion
// ni el bloom) y con la profundidad de la escena (lo de delante lo tapa).
class WorldUiPass {
public:
    void create(const VulkanDevice& device, vk::Format scene_format, vk::Format depth_format,
                std::uint32_t frames_in_flight);
    void destroy();

    // Los canvas de este frame: se pintan en la siguiente vista que se grabe.
    // Las texturas de los que ya no estan se liberan unos frames despues.
    void setCanvases(std::vector<WorldUiCanvas> canvases);
    bool empty() const { return canvases_.empty(); }
    bool needsPaint() const { return dirty_; }

    // Pinta las texturas (fuera de cualquier renderizado).
    void recordPaint(const VulkanDevice& device, const vk::raii::CommandBuffer& cmd, std::uint32_t frame);
    // Los paneles (con el renderizado abierto sobre la imagen final y el
    // depth de la escena en solo lectura).
    void recordQuads(const vk::raii::CommandBuffer& cmd, const core::Mat4& view_projection,
                     vk::Extent2D viewport) const;

private:
    struct Texture {
        std::uint64_t id = 0;
        VulkanImage image;
        vk::raii::DescriptorSet set{nullptr};
        bool painted = false;      // tiene contenido (layout de lectura)
        std::uint32_t unused = 0;  // frames sin su canvas
    };
    Texture* textureFor(const VulkanDevice& device, const WorldUiCanvas& canvas);
    void retire(std::unique_ptr<Texture> texture);
    vk::raii::Pipeline createPipeline(const VulkanDevice& device, bool quad, vk::Format color_format,
                                      vk::Format depth_format) const;

    std::uint32_t frames_in_flight_ = 2;
    vk::raii::DescriptorSetLayout set_layout_{nullptr};
    vk::raii::PipelineLayout paint_layout_{nullptr};
    vk::raii::PipelineLayout quad_layout_{nullptr};
    vk::raii::Pipeline paint_pipeline_{nullptr};
    vk::raii::Pipeline quad_pipeline_{nullptr};
    vk::raii::Sampler sampler_{nullptr};
    vk::raii::DescriptorPool pool_{nullptr};
    VulkanImage white_;
    vk::raii::DescriptorSet white_set_{nullptr};
    bool white_ready_ = false;

    std::vector<WorldUiCanvas> canvases_;
    std::vector<std::unique_ptr<Texture>> textures_;
    struct Retired {
        std::unique_ptr<Texture> texture;
        std::uint32_t frames_left = 0;
    };
    std::vector<Retired> retired_;
    std::vector<VulkanBuffer> vertex_buffers_;  // uno por frame en vuelo, crecen
    std::vector<VulkanBuffer> index_buffers_;
    bool dirty_ = false;
};

}  // namespace cramion::gfx

#endif  // CRAMION_VK_WORLD_UI_PASS_H
