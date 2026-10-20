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
    std::uint32_t group = 0;       // lote (modelo x material)
    std::uint32_t first_slot = 0;  // primer hueco de comando del lote
    // Matriz de mundo del actor en el buffer de huesos: va al first_instance
    // del comando y skinned.vert la lee con gl_InstanceIndex.
    std::uint32_t instance = 0;
    // Lotes con mesh shaders: los meshlets del cluster, que cull.comp copia al
    // comando (gbuffer_meshlet.task los lee de ahi).
    std::uint32_t first_meshlet = 0;
    std::uint32_t meshlet_count = 0;
    std::uint32_t mesh = 0;
};
static_assert(sizeof(GpuCluster) == 64, "GpuCluster debe coincidir con cull.comp");

// Una submalla de vidrio para glass_cull.comp (std430).
struct GpuGlassItem {
    core::Vec4 bounds_min{};
    core::Vec4 bounds_max{};
    std::uint32_t first_index = 0;
    std::uint32_t index_count = 0;
    std::uint32_t pad0 = 0;
    std::uint32_t pad1 = 0;
};
static_assert(sizeof(GpuGlassItem) == 48, "GpuGlassItem debe coincidir con glass_cull.comp");

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
    // Sin crear (modo compatible: el escenario lo recorta la CPU) no hace nada.
    bool created() const { return created_; }

    // Rehace la piramide Hi-Z con el tamano del depth buffer (al crear la
    // ventana o cambiar su tamano).
    void resize(const VulkanDevice& device, const VulkanImage& depth);

    // Sube los clusteres del frame (con la fence de `frame_index` ya
    // esperada). Si hay mas que antes se rehacen los buffers y se olvida la
    // visibilidad, sin parar la GPU: los viejos se retiran por fence y los
    // sets de cada frame pasan a los nuevos en su siguiente setClusters.
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

    // Vidrio (glass_cull.comp): la CPU sube las submallas que ve en el campo de
    // vision y la GPU deja un comando por cada una, con 0 instancias si la
    // piramide Hi-Z de este frame la tapa. Solo despues de recordHiZ() y fuera
    // de un pase de dibujo. false si no se puede (sin el shader, sin piramide):
    // entonces se dibuja directamente, como antes.
    bool recordGlassCull(const VulkanDevice& device, const vk::raii::CommandBuffer& cmd, std::uint32_t frame_index,
                         const std::vector<GpuGlassItem>& items, const std::vector<VulkanBuffer>& camera_buffers);
    const VulkanBuffer& glassCommands(std::uint32_t frame_index) const { return glass_commands_[frame_index]; }

    std::uint32_t clusterCount() const { return cluster_count_; }
    const VulkanBuffer& commands(std::uint32_t phase) const { return commands_[phase]; }
    const VulkanBuffer& counts(std::uint32_t phase) const { return counts_[phase]; }

private:
    void createBuffers(const VulkanDevice& device, std::uint32_t cluster_capacity,
                       std::uint32_t group_capacity, std::uint32_t slot_capacity);
    // Los sets (de las dos fases) de un frame con los buffers de ahora.
    void writeCullSets(const VulkanDevice& device, const std::vector<VulkanBuffer>& camera_buffers,
                       std::uint32_t frame_index);

    bool created_ = false;
    // Frames (bit por hueco) cuyos sets aun apuntan a los buffers de antes
    // del ultimo crecimiento: se reescriben en su setClusters.
    std::uint32_t sets_dirty_ = 0;
    // La visibilidad de unos buffers nuevos aun sin borrar (lo hace recordCull,
    // que es const: por eso mutable).
    mutable bool visibility_cleared_ = true;
    std::uint32_t cluster_count_ = 0;
    std::uint32_t cluster_capacity_ = 0;
    std::uint32_t group_capacity_ = 0;
    std::uint32_t slot_capacity_ = 0;
    bool clear_commands_ = false;  // sin drawIndirectCount: huecos a cero cada frame

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
    ComputePass glass_pass_;
    bool glass_ready_ = false;
    std::uint32_t glass_capacity_ = 0;
    std::array<VulkanBuffer, kMaxFramesInFlight> glass_items_;     // los escribe la CPU
    std::array<VulkanBuffer, kMaxFramesInFlight> glass_commands_;  // los escribe la GPU
    std::vector<vk::raii::DescriptorSet> glass_sets_;              // uno por frame
    vk::raii::DescriptorPool pool_{nullptr};
    // [frame][fase]
    std::vector<vk::raii::DescriptorSet> cull_sets_;
    std::vector<vk::raii::DescriptorSet> hiz_sets_;  // uno por nivel
};

}  // namespace cramion::gfx

#endif  // CRAMION_VK_GPU_CULLING_H
