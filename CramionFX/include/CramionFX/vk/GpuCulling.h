#ifndef CRAMION_VK_GPU_CULLING_H
#define CRAMION_VK_GPU_CULLING_H

#include "CramionFX/core/Math.h"
#include "CramionFX/vk/ComputePass.h"
#include "CramionFX/vk/VulkanBuffer.h"
#include "CramionFX/vk/VulkanCommon.h"

#include <array>
#include <cstdint>
#include <vector>

namespace cramion::gfx {

class VulkanDevice;
class VulkanImage;

// Un cluster de un escenario tal como lo lee cull.comp (std430).
struct GpuCluster {
    core::Vec4 bounds_min{};  // caja en el mundo
    core::Vec4 bounds_max{};
    std::uint32_t first_index = 0;
    std::uint32_t index_count = 0;
    std::uint32_t group = 0;       // grupo (actor x material)
    std::uint32_t first_slot = 0;  // primer hueco de comando del grupo
};
static_assert(sizeof(GpuCluster) == 48, "GpuCluster debe coincidir con cull.comp");

// Culling de los clusteres en la GPU: frustum + oclusion con una piramide
// Hi-Z, en dos fases (ver cull.comp). El resultado son comandos de dibujo
// indirecto agrupados por (actor x material): la CPU ya no decide que se
// dibuja ni emite una llamada por cluster, solo una por grupo.
//
// Uso por frame:
//   setClusters()  (CPU) cajas y grupos de este frame
//   recordCull(fase 0) -> dibujar con earlyCommands()/earlyCounts()
//   recordHiZ()        -> con el depth de lo dibujado
//   recordCull(fase 1) -> dibujar con lateCommands()/lateCounts()
class GpuCulling {
public:
    // Tamano de un VkDrawIndexedIndirectCommand.
    static constexpr std::uint32_t kCommandSize = 20;

    struct Stats {
        std::uint32_t early = 0;     // dibujados en la fase 0
        std::uint32_t late = 0;      // dibujados en la fase 1 (recien descubiertos)
        std::uint32_t occluded = 0;  // en el campo de vision pero tapados
        std::uint32_t outside = 0;   // fuera del campo de vision
    };

    void create(const VulkanDevice& device);
    void destroy();

    // Rehace la piramide Hi-Z con el tamano del depth buffer (al crear la
    // ventana o cambiar su tamano).
    void resize(const VulkanDevice& device, const VulkanImage& depth);

    // Sube los clusteres del frame. Si hay mas que antes se rehacen los
    // buffers (espera a la GPU) y se olvida la visibilidad.
    void setClusters(const VulkanDevice& device, std::uint32_t frame_index,
                     const std::vector<GpuCluster>& clusters, std::uint32_t group_count,
                     std::uint32_t slot_count, const std::vector<VulkanBuffer>& camera_buffers);

    // Fase 0 (`occlusion` = false: todo lo del campo de vision, sin mirar ni
    // tocar la visibilidad) o fase 1. Antes de la fase 0 se vacian los
    // contadores de las dos fases.
    void recordCull(const vk::raii::CommandBuffer& cmd, std::uint32_t frame_index,
                    std::uint32_t phase, bool occlusion) const;
    // Piramide Hi-Z a partir del depth buffer (ya en DepthReadOnlyOptimal).
    void recordHiZ(const vk::raii::CommandBuffer& cmd) const;

    // Estadisticas de la ultima vez que se uso este hueco de frame (la fence
    // de ese frame debe estar esperada).
    Stats readStats(std::uint32_t frame_index) const;

    std::uint32_t clusterCount() const { return cluster_count_; }
    const VulkanBuffer& commands(std::uint32_t phase) const { return commands_[phase]; }
    const VulkanBuffer& counts(std::uint32_t phase) const { return counts_[phase]; }

private:
    void createBuffers(const VulkanDevice& device, std::uint32_t cluster_capacity,
                       std::uint32_t group_capacity, std::uint32_t slot_capacity);
    void writeCullSets(const VulkanDevice& device,
                       const std::vector<VulkanBuffer>& camera_buffers);

    std::uint32_t cluster_count_ = 0;
    std::uint32_t cluster_capacity_ = 0;
    std::uint32_t group_capacity_ = 0;
    std::uint32_t slot_capacity_ = 0;

    // Por frame en vuelo (los escribe la CPU): clusteres y estadisticas.
    std::array<VulkanBuffer, kMaxFramesInFlight> clusters_;
    std::array<VulkanBuffer, kMaxFramesInFlight> stats_;
    // Compartidos (solo la GPU): visibilidad y comandos/contadores por fase.
    VulkanBuffer visibility_;
    std::array<VulkanBuffer, 2> commands_;
    std::array<VulkanBuffer, 2> counts_;

    // Piramide Hi-Z (R32F), siempre en layout General.
    vk::raii::DeviceMemory hiz_memory_{nullptr};
    vk::raii::Image hiz_image_{nullptr};
    vk::raii::ImageView hiz_view_{nullptr};                 // todos los niveles
    std::vector<vk::raii::ImageView> hiz_level_views_;      // uno por nivel
    std::uint32_t hiz_levels_ = 0;
    vk::Extent2D hiz_extent_{};
    vk::raii::Sampler hiz_sampler_{nullptr};

    ComputePass cull_pass_;
    ComputePass hiz_pass_;
    vk::raii::DescriptorPool pool_{nullptr};
    // [frame][fase]
    std::vector<vk::raii::DescriptorSet> cull_sets_;
    std::vector<vk::raii::DescriptorSet> hiz_sets_;  // uno por nivel
};

}  // namespace cramion::gfx

#endif  // CRAMION_VK_GPU_CULLING_H
