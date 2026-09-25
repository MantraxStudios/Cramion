#ifndef CRAMION_VK_WATER_PASS_H
#define CRAMION_VK_WATER_PASS_H

// Agua (oceano, lagos y rios), dibujada tras la iluminacion y el vidrio sobre
// la imagen HDR, con la copia de la escena sin agua (refraccion), la
// profundidad (grosor del agua: color, espuma de orilla, causticas) y los
// reflejos en pantalla con el entorno de respaldo. Todo procedural: oleaje
// Gerstner en el vertice (el mismo que water::sampleWater en CramionCore) y
// ondulacion fina, espuma y causticas con ruido en el fragmento.
//
// Mallas: el oceano, un disco radial centrado en la camara (denso cerca,
// hasta el horizonte); el lago, una cuadricula; el rio, una cinta que llega
// hecha de la CPU (sigue sus puntos).

#include "CramionFX/core/Math.h"
#include "CramionFX/vk/VulkanBuffer.h"
#include "CramionFX/vk/VulkanCommon.h"

#include <array>
#include <cstdint>
#include <vector>

namespace cramion::gfx {

class VulkanDevice;

inline constexpr std::uint32_t kMaxWaterBodies = 16;

// Un cuerpo de agua para el shader (std140, igual que water.vert/frag).
struct GpuWaterBody {
    core::Vec4 origin{};         // xyz origen, w = giro en Y (rad)
    core::Vec4 extent{};         // xy = medio tamano (lago), z = tipo (0 oceano, 1 lago, 2 rio)
    core::Vec4 shallow{};        // rgb dispersion, w = transparencia (m)
    core::Vec4 deep{};           // rgb color profundo, w = espuma
    core::Vec4 waves{};          // altura, longitud, velocidad, crestas
    core::Vec4 wind{};           // viento (rad), dispersion (rad), corriente (m/s), ondulacion fina
    core::Vec4 look{};           // rugosidad, refraccion, causticas, espuma de orilla (m)
    core::Vec4 extra{};          // olas de playa, luz en las crestas, -, -
};
static_assert(sizeof(GpuWaterBody) == 128, "GpuWaterBody debe coincidir con water.vert");

struct WaterVertex {
    float position[3];  // oceano/lago: rejilla; rio: el mundo
    float uv[2];        // rio: (a traves 0..1, a lo largo en m)
    float flow[2];      // rio: direccion de la corriente (xz)
};

struct WaterBodyDesc {
    GpuWaterBody params{};
    // Rio: su cinta (vacia en oceano/lago). Se vuelve a subir si cambia la version.
    std::vector<WaterVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::uint64_t mesh_version = 0;
};

class WaterPass {
public:
    void create(const VulkanDevice& device, const vk::raii::DescriptorSetLayout& frame_layout,
                const vk::raii::DescriptorSetLayout& scene_layout, vk::Format color_format, vk::Format depth_format,
                std::uint32_t frames_in_flight);
    void destroy();

    // `underwater` = cuerpo en el que esta la camara (-1 = ninguno): se tine
    // lo que se ve bajo su superficie.
    void setBodies(const std::vector<WaterBodyDesc>& bodies, float time, int underwater = -1);
    bool empty() const { return bodies_.empty(); }

    // Cada frame (sus buffers ya no los usa la GPU): parametros y mallas.
    void prepare(std::uint32_t frame);
    // Dentro de un pase de dibujo sobre la imagen HDR con la profundidad de
    // solo lectura. `frame_set` = set 0 (camara), `scene_set` = set 2 (luces,
    // sombras, profundidad, copia de la escena, entorno).
    void record(const vk::raii::CommandBuffer& cmd, std::uint32_t frame, const vk::raii::DescriptorSet& frame_set,
                const vk::raii::DescriptorSet& scene_set);

private:
    struct Mesh {
        VulkanBuffer vertices;
        VulkanBuffer indices;
        std::uint32_t index_count = 0;
        std::uint64_t version = 0;
    };
    struct Garbage {
        Mesh mesh;
        std::uint32_t frames_left = 0;
    };
    void createMeshes(const VulkanDevice& device);
    static void upload(const VulkanDevice& device, Mesh& mesh, const std::vector<WaterVertex>& vertices,
                       const std::vector<std::uint32_t>& indices);

    const VulkanDevice* device_ = nullptr;
    std::uint32_t frames_ = 0;
    vk::raii::DescriptorSetLayout set_layout_{nullptr};
    vk::raii::PipelineLayout layout_{nullptr};
    vk::raii::Pipeline pipeline_{nullptr};
    vk::raii::Pipeline underwater_pipeline_{nullptr};
    int underwater_ = -1;
    vk::raii::DescriptorPool pool_{nullptr};
    std::vector<vk::raii::DescriptorSet> sets_;
    std::vector<VulkanBuffer> uniforms_;

    Mesh grid_;   // lago
    Mesh ocean_;  // disco radial
    std::array<Mesh, kMaxWaterBodies> rivers_;
    std::vector<Garbage> garbage_;

    std::vector<WaterBodyDesc> bodies_;
    float time_ = 0.0f;
};

}  // namespace cramion::gfx

#endif  // CRAMION_VK_WATER_PASS_H
