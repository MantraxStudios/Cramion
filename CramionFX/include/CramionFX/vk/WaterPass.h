#ifndef CRAMION_VK_WATER_PASS_H
#define CRAMION_VK_WATER_PASS_H

// Agua (oceano, lagos y rios), dibujada tras la iluminacion y el vidrio sobre
// la imagen HDR, con la copia de la escena sin agua (refraccion), la
// profundidad (grosor del agua: color, espuma de orilla, causticas) y los
// reflejos en pantalla con el entorno de respaldo.
//
// Oceano: oleaje FFT (water_fft.comp) en 4 cascadas de 128 x 128 con el
// espectro que calcula CramionCore (water::oceanSpectrum, el mismo que usa la
// flotacion); mipmaps para verlo sin parpadeo a cualquier distancia. Malla:
// anillos concentricos centrados en la camara (clipmap, como Crest), cada uno
// con el doble de separacion, que se funden con el siguiente en su borde
// (sin grietas) y una falda hasta el horizonte.
// Lagos y rios: Gerstner en el vertice (water::sampleWater) y el rizado de la
// cascada fina del FFT; en el rio, la corriente en dos fases (flow map).
//
// Bajo el agua (water_under.frag): absorcion y luz dispersada, causticas,
// rayos de sol con la sombra, particulas y el menisco en la linea del agua;
// la superficie vista desde abajo con la ventana de Snell (water.frag).

#include "CramionFX/core/Math.h"
#include "CramionFX/vk/ComputePass.h"
#include "CramionFX/vk/VulkanBuffer.h"
#include "CramionFX/vk/VulkanCommon.h"

#include <array>
#include <cstdint>
#include <vector>

namespace cramion::gfx {

class VulkanDevice;

inline constexpr std::uint32_t kMaxWaterBodies = 16;

// Un cuerpo de agua para el shader (std140, igual que water_common.glsl).
struct GpuWaterBody {
    core::Vec4 origin{};         // xyz origen, w = giro en Y (rad)
    core::Vec4 extent{};         // xy = medio tamano (lago), z = tipo (0 oceano, 1 lago, 2 rio)
    core::Vec4 shallow{};        // rgb dispersion, w = transparencia (m)
    core::Vec4 deep{};           // rgb color profundo, w = espuma
    core::Vec4 waves{};          // altura, longitud, velocidad, crestas
    core::Vec4 wind{};           // viento (rad), dispersion (rad), corriente (m/s), ondulacion fina
    core::Vec4 look{};           // rugosidad, refraccion, causticas, espuma de orilla (m)
    core::Vec4 extra{};          // olas de playa, luz en las crestas, superficie en la camara (rio), -
    core::Vec4 under{};          // rayos de sol, particulas, -, -
};
static_assert(sizeof(GpuWaterBody) == 144, "GpuWaterBody debe coincidir con water_common.glsl");

struct WaterVertex {
    float position[3];  // oceano: celda de la rejilla; lago: 0..1; rio: el mundo
    float uv[2];        // rio: (a traves 0..1, a lo largo en m)
    float flow[2];      // rio: direccion de la corriente (xz) por el ancho del rio (m)
    float slope = 0.0f; // rio: desnivel (m por m): rapidos
};

struct WaterBodyDesc {
    GpuWaterBody params{};
    // Rio: su cinta (vacia en oceano/lago). Se vuelve a subir si cambia la version.
    std::vector<WaterVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::uint64_t mesh_version = 0;
};

// Espectro del oceano FFT (water::OceanSpectrum de CramionCore).
inline constexpr std::uint32_t kOceanCascades = 4;
inline constexpr std::uint32_t kOceanResolution = 128;
struct WaterSpectrumDesc {
    const std::vector<float>* modes = nullptr;  // 4 x 128 x 128 x 8 floats
    std::uint64_t key = 0;
    core::Vec4 sizes{};           // lado de cada cascada (m)
    core::Vec4 slope_variance{};  // por cascada
    float choppiness = 1.0f;
    float significant_height = 0.0f;
    float foam = 1.0f;              // cantidad de espuma de las crestas
    float foam_persistence = 4.0f;  // s
    float period = 256.0f;          // las frecuencias se repiten cada `period` s
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
    // Espectro del oceano (o el de una brisa si no hay oceano: el rizado de
    // lagos y rios). Solo se vuelve a subir si cambia `key`.
    void setSpectrum(const WaterSpectrumDesc& spectrum);
    // Olas interactivas (water::RippleSimulation): `size` x `size` alturas
    // desde la esquina (origin_x, origin_z) cada `cell` metros. Vacio = calma.
    void setRipples(const std::vector<float>& heights, std::uint32_t size, float origin_x, float origin_z, float cell);
    bool empty() const { return bodies_.empty(); }

    // Cada frame (sus buffers ya no los usa la GPU): parametros y mallas.
    void prepare(std::uint32_t frame);
    // Fuera de cualquier pase de dibujo, antes del agua: el oleaje FFT de este
    // frame (computo + mipmaps).
    void recordSimulation(const vk::raii::CommandBuffer& cmd, std::uint32_t frame);
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
    // Imagen array con mipmaps (desplazamiento / derivadas del oceano).
    struct OceanImage {
        vk::raii::DeviceMemory memory{nullptr};
        vk::raii::Image image{nullptr};
        vk::raii::ImageView sampled{nullptr};  // todos los mipmaps
        vk::raii::ImageView storage{nullptr};  // mip 0 (computo)
    };
    void createMeshes(const VulkanDevice& device);
    void createOcean(const VulkanDevice& device);
    static void createOceanImage(const VulkanDevice& device, OceanImage& image, vk::Format format,
                                 std::uint32_t layers, std::uint32_t mips, vk::ImageUsageFlags usage);
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
    std::vector<VulkanBuffer> ripple_buffers_;  // por frame en vuelo
    std::vector<float> ripples_;
    std::uint32_t ripple_size_ = 0;
    core::Vec4 ripple_params_{};  // x, z de la esquina, celda, lado (0 = sin olas)

    // --- Oceano FFT ---
    ComputePass fft_pass_;
    vk::raii::DescriptorPool fft_pool_{nullptr};
    std::vector<vk::raii::DescriptorSet> fft_sets_;  // por frame en vuelo
    std::vector<VulkanBuffer> spectrum_buffers_;     // por frame en vuelo
    std::vector<std::uint64_t> spectrum_uploaded_;   // clave subida a cada uno
    std::vector<float> spectrum_modes_;
    WaterSpectrumDesc spectrum_{};
    OceanImage fft_image_;       // rgba32f, 2 capas por cascada
    OceanImage displacement_;    // rgba16f, mipmaps
    OceanImage derivatives_;     // rgba16f, mipmaps
    vk::raii::Sampler ocean_sampler_{nullptr};
    std::uint32_t ocean_mips_ = 1;
    bool ocean_ready_ = false;   // las imagenes ya tienen un frame (la espuma sigue)
    float last_simulated_ = -1.0f;

    Mesh grid_;        // lago
    Mesh ocean_full_;  // nivel 0 del clipmap
    Mesh ocean_ring_;  // niveles 1.. (la misma rejilla con un hueco)
    std::array<Mesh, kMaxWaterBodies> rivers_;
    std::vector<Garbage> garbage_;

    std::vector<WaterBodyDesc> bodies_;
    float time_ = 0.0f;
};

}  // namespace cramion::gfx

#endif  // CRAMION_VK_WATER_PASS_H
