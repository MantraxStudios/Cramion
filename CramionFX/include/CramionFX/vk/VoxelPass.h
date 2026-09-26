#ifndef CRAMION_VK_VOXEL_PASS_H
#define CRAMION_VK_VOXEL_PASS_H

// Mundos de bloques (voxeles), dibujados en el G-buffer y en las cascadas de
// sombra: reciben toda la iluminacion diferida (PBR, sol con sombras, cielo,
// SSAO, SSGI, niebla, luz volumetrica) como cualquier otra superficie.
//
//   - La malla va por secciones de 16^3 bloques. Cada seccion es una lista de
//     caras (cuadrados de 4 vertices de 8 bytes) que CramionCore genera en
//     otros hilos; aqui solo se suben y se dibujan.
//   - Memoria: paginas grandes de vertices en la GPU repartidas entre las
//     secciones (sin una reserva de Vulkan por seccion). Cambiar un bloque
//     sube solo su seccion, grabado en el command buffer del frame; la malla
//     vieja se libera cuando ningun frame en vuelo la usa.
//   - Texturas PBR en arrays (una capa por textura de bloque): color con alfa
//     (recorte: hojas, plantas), normal + altura + oclusion, y rugosidad,
//     metal y emision. Relieve con parallax cerca de la camara.
//   - Por vertice: oclusion ambiental de las esquinas, luz del cielo y de las
//     antorchas (0..15, como Minecraft) y un tinte (hierba y hojas por bioma).
//
// Formato del vertice (2 x uint32), igual que voxel.vert:
//   a: x 0..16 (5 bits) | y (5) | z (5) | cara 0..6 (3; 6 = planta en cruz) |
//      u (1) | v (1) | oclusion 0..3 (2) | ondea (1)
//   b: capa de textura (10) | luz del cielo 0..15 (4) | luz de bloque (4) |
//      tinte R G B de 4 bits (12)

#include "CramionFX/core/Math.h"
#include "CramionFX/vk/VulkanBuffer.h"
#include "CramionFX/vk/VulkanCommon.h"

#include <array>
#include <cstdint>
#include <deque>
#include <memory>
#include <unordered_map>
#include <vector>

namespace cramion::gfx {

class VulkanDevice;

// Una textura de bloque: tres imagenes RGBA8 de size x size.
struct VoxelTextureLayer {
    std::vector<std::uint8_t> albedo;    // sRGB, alfa = recorte
    std::vector<std::uint8_t> normal;    // xy normal (tangente), z altura, w oclusion
    std::vector<std::uint8_t> material;  // r rugosidad, g metal, b emision, a reflectancia
};

struct VoxelStats {
    std::uint32_t sections = 0;
    std::uint32_t visible = 0;
    std::uint64_t quads = 0;
    std::uint64_t memory_bytes = 0;
    std::uint32_t pending_uploads = 0;
};

class VoxelPass {
public:
    static constexpr std::uint32_t kMaxQuadsPerSection = 32768;

    void create(const VulkanDevice& device, const vk::raii::DescriptorSetLayout& frame_layout,
                std::array<vk::Format, 4> gbuffer_formats, vk::Format depth_format, vk::Format shadow_format,
                std::uint32_t frames_in_flight);
    void destroy();

    // Las texturas de los bloques (todas del mismo tamano, potencia de dos).
    void setTextures(std::uint32_t size, const std::vector<VoxelTextureLayer>& layers);
    bool hasTextures() const { return layer_count_ > 0; }

    // Malla de una seccion (clave libre, p. ej. sus coordenadas empaquetadas):
    // `origin` = esquina minima en el mundo; `vertices` = 2 uint32 por
    // vertice, 4 vertices por cara. Sin vertices se quita.
    void setSection(std::uint64_t key, const core::Vec3& origin, const std::uint32_t* vertices,
                    std::uint32_t vertex_count);
    void removeSection(std::uint64_t key);
    void clearSections();
    // Segundos (el vaiven de hojas y plantas).
    void setTime(float seconds) { time_ = seconds; }
    void setVisible(bool visible) { visible_ = visible; }

    // Una vez por frame: sube lo pendiente y libera lo que ya no se usa.
    // true si cambio la geometria (sombras cacheadas).
    bool recordUploads(const vk::raii::CommandBuffer& cmd, std::uint32_t frame);
    // Secciones visibles (frustum), de cerca a lejos.
    void prepare(std::uint32_t frame, const core::Vec3& camera_position, const core::Mat4& view_projection);
    void recordGBuffer(const vk::raii::CommandBuffer& cmd, std::uint32_t frame,
                       const vk::raii::DescriptorSet& frame_set) const;
    void recordShadow(const vk::raii::CommandBuffer& cmd, std::uint32_t frame, const vk::raii::DescriptorSet& frame_set,
                      const core::Mat4& light_view_projection) const;

    VoxelStats stats() const;
    bool empty() const { return sections_.empty(); }
    // Origen flotante: las secciones ya subidas, -offset.
    void shiftOrigin(const core::Vec3& offset) {
        for (auto& [key, section] : sections_) section.origin = section.origin - offset;
    }

private:
    struct Allocation {
        int page = -1;
        std::uint64_t offset = 0;  // bytes
        std::uint64_t size = 0;
        bool valid() const { return page >= 0; }
    };
    struct Page {
        VulkanBuffer buffer;
        std::uint64_t capacity = 0;
        std::vector<std::pair<std::uint64_t, std::uint64_t>> free;  // (offset, size), ordenados
    };
    struct Section {
        core::Vec3 origin{};
        Allocation live;           // lo que se dibuja
        std::uint32_t quads = 0;
        Allocation pending;        // lo que se esta subiendo
        std::uint32_t pending_quads = 0;
        std::vector<std::uint32_t> pending_data;
        bool queued = false;
    };
    struct Retired {
        Allocation allocation;
        std::uint64_t release_frame = 0;
    };

    void createPipelines(const VulkanDevice& device, std::array<vk::Format, 4> gbuffer_formats,
                         vk::Format depth_format, vk::Format shadow_format);
    Allocation allocate(std::uint64_t bytes);
    void release(const Allocation& allocation);
    void retire(const Allocation& allocation);
    void drawSections(const vk::raii::CommandBuffer& cmd, const std::vector<std::uint64_t>& keys,
                      const core::Mat4* light_view_projection) const;

    const VulkanDevice* device_ = nullptr;
    std::uint32_t frames_ = 2;
    vk::raii::DescriptorSetLayout set_layout_{nullptr};
    vk::raii::PipelineLayout layout_{nullptr};
    vk::raii::Pipeline gbuffer_pipeline_{nullptr};
    vk::raii::Pipeline shadow_pipeline_{nullptr};
    vk::raii::DescriptorPool pool_{nullptr};
    vk::raii::DescriptorSet set_{nullptr};
    vk::raii::Sampler sampler_{nullptr};

    struct ArrayTexture {
        vk::raii::DeviceMemory memory{nullptr};
        vk::raii::Image image{nullptr};
        vk::raii::ImageView view{nullptr};
    };
    ArrayTexture albedo_;
    ArrayTexture normal_;
    ArrayTexture material_;
    std::uint32_t layer_count_ = 0;

    VulkanBuffer quad_indices_;
    std::vector<std::unique_ptr<Page>> pages_;
    std::unordered_map<std::uint64_t, Section> sections_;
    std::deque<std::uint64_t> upload_queue_;
    std::vector<Retired> retired_;
    std::vector<VulkanBuffer> staging_;
    std::uint64_t frame_counter_ = 0;
    float time_ = 0.0f;
    bool visible_ = true;

    std::vector<std::uint64_t> visible_keys_;
    std::vector<std::uint64_t> all_keys_;  // para las sombras (su propio recorte)
};

}  // namespace cramion::gfx

#endif  // CRAMION_VK_VOXEL_PASS_H
