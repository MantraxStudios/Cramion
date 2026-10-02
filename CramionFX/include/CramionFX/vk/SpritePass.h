#ifndef CRAMION_VK_SPRITE_PASS_H
#define CRAMION_VK_SPRITE_PASS_H

#include "CramionFX/vk/SpriteGeometry.h"
#include "CramionFX/vk/VulkanBuffer.h"
#include "CramionFX/vk/VulkanCommon.h"
#include "CramionFX/vk/VulkanTexture.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace cramion::gfx {

class VulkanDevice;

// Dibuja SpriteDrawList (sprites y tilemaps 2D) sobre la imagen HDR con
// prueba de profundidad (sin escribirla) y mezcla alfa, en el orden dado.
// Los cuadrados se expanden en la CPU (6 vertices cada uno) y se agrupan en
// tandas seguidas con la misma textura: un tilemap entero es una tanda.
//
// Texturas: se leen de disco la primera vez que aparecen (cache por ruta y
// filtro) y se liberan si pasan muchos frames sin usarse. Las luces 2D van en
// un buffer por frame que lee el fragment shader.
class SpritePass {
public:
    void create(const VulkanDevice& device, vk::Format color_format, vk::Format depth_format,
                std::uint32_t frames_in_flight);
    void destroy();

    // Prepara los vertices, tandas y luces del frame. false si no hay nada.
    bool prepare(const VulkanDevice& device, std::uint32_t frame, const SpriteDrawList& sprites,
                 const core::Mat4& view_projection);

    // Graba el dibujo (renderizado ya abierto sobre la HDR, depth en solo lectura).
    void record(const vk::raii::CommandBuffer& cmd, std::uint32_t frame, vk::Extent2D viewport) const;

    // Vuelve a leer de disco las texturas (el editor, al cambiar una imagen).
    void reloadTextures() { reload_requested_ = true; }
    std::size_t textureCount() const { return textures_.size(); }

    static constexpr std::uint32_t kMaxLights = 64;
    static constexpr std::uint32_t kMaxTextures = 512;

private:
    struct Vertex {
        float position[3];
        float uv[2];
        float color[4];
        float params[2];  // x = iluminado, y = corte alfa
    };
    struct Push {
        core::Mat4 view_projection;
        float ambient[4];
        std::uint32_t light_count;
        std::uint32_t pad[3];
    };
    struct GpuLight {
        float position_radius[4];
        float color_intensity[4];
        float params[4];  // x = caida
    };
    struct Batch {
        std::uint32_t first = 0;
        std::uint32_t count = 0;
        VkDescriptorSet set = VK_NULL_HANDLE;
    };
    struct TextureEntry {
        VulkanTexture texture;
        vk::raii::DescriptorSet set{nullptr};
        std::uint64_t last_used = 0;
    };

    vk::raii::Pipeline createPipeline(const VulkanDevice& device, vk::Format color_format,
                                      vk::Format depth_format) const;
    // La entrada de una textura (la carga si hace falta; si no se puede leer,
    // la blanca). Nunca devuelve nullptr.
    TextureEntry* texture(const VulkanDevice& device, const SpriteTexture& desc);
    TextureEntry* createEntry(const VulkanDevice& device, const std::uint8_t* rgba, std::uint32_t width,
                              std::uint32_t height, bool point);
    void evictUnused();

    vk::raii::DescriptorSetLayout texture_layout_{nullptr};
    vk::raii::DescriptorSetLayout light_layout_{nullptr};
    vk::raii::DescriptorPool pool_{nullptr};
    vk::raii::Sampler linear_sampler_{nullptr};
    vk::raii::Sampler point_sampler_{nullptr};
    vk::raii::PipelineLayout layout_{nullptr};
    vk::raii::Pipeline pipeline_{nullptr};

    std::vector<VulkanBuffer> vertex_buffers_;
    std::vector<VulkanBuffer> light_buffers_;
    std::vector<vk::raii::DescriptorSet> light_sets_;
    std::vector<std::vector<Batch>> batches_;
    std::vector<Push> pushes_;

    std::unordered_map<std::string, std::unique_ptr<TextureEntry>> textures_;
    std::unique_ptr<TextureEntry> white_;
    std::unique_ptr<TextureEntry> white_point_;
    std::vector<Vertex> scratch_;
    std::uint64_t frame_counter_ = 0;
    std::uint32_t frames_in_flight_ = 2;
    bool reload_requested_ = false;
};

}  // namespace cramion::gfx

#endif  // CRAMION_VK_SPRITE_PASS_H
