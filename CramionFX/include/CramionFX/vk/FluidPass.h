#ifndef CRAMION_VK_FLUID_PASS_H
#define CRAMION_VK_FLUID_PASS_H

// Liquidos por particulas en la GPU (como Obi Fluid / FleX / Niagara Fluids):
//
//   Simulacion: Position Based Fluids (Macklin y Muller, 2013) en compute
//   shaders (fluid_sim.comp). Cada subpaso fijo:
//     1. prediccion (gravedad), muertes (vida, desagues, fuera del dominio) y
//        celda de cada particula en una rejilla uniforme (envuelta: el
//        dominio puede ser grande y nunca se cuenta dos veces un vecino);
//     2. ordenacion por celdas (cuenta + prefijo + reparto): los vecinos
//        quedan contiguos en memoria y las muertas al final (compactacion);
//     3. N iteraciones de la restriccion de densidad (lambda, desplazamiento
//        con el termino anti-agrupamiento, colisiones);
//     4. velocidad, viscosidad XSPH, cohesion (tension superficial),
//        confinamiento de vorticidad y espuma.
//   Colisiones con formas analiticas (caja, esfera, capsula, plano), un
//   campo de alturas (terreno) y las paredes del dominio.
//
//   Render: liquido en espacio de pantalla (Simon Green 2010, van der Laan
//   2009): profundidad de esferas, suavizado bilateral, grosor y color
//   acumulados, normales reconstruidas y sombreado con refraccion de la
//   escena, absorcion Beer-Lambert, reflejo de Fresnel del cielo, brillo del
//   sol, espuma y emision (lava). Se compone sobre la imagen HDR tras la
//   iluminacion, respetando la profundidad de la escena.
//
//   Lectura: posiciones y velocidades vuelven a la CPU un par de frames
//   tarde (flotacion de los Rigidbody, consultas de densidad, Lua).

#include "CramionFX/core/Math.h"
#include "CramionFX/vk/VulkanBuffer.h"
#include "CramionFX/vk/VulkanCommon.h"
#include "CramionFX/vk/VulkanImage.h"

#include <array>
#include <cstdint>
#include <vector>

namespace cramion::gfx {

class VulkanDevice;

// Material de un liquido (uno por tipo: agua, aceite, miel...). Lo usan la
// simulacion (viscosidad, cohesion, vorticidad) y el sombreado.
struct FluidMaterial {
    core::Vec3 absorption{0.9f, 0.25f, 0.18f};  // Beer-Lambert (1/m por canal)
    float viscosity = 0.02f;                    // XSPH 0..1
    core::Vec3 scatter{0.02f, 0.07f, 0.09f};    // color difuso (lo que se ve con grosor)
    float cohesion = 0.6f;                      // tension superficial (m/s2)
    core::Vec3 emission{};                      // HDR (lava)
    float vorticity = 0.25f;                    // confinamiento de vorticidad
    float foam = 1.0f;                          // espuma con velocidad
    float roughness = 0.04f;
    float damping = 0.0f;                       // frenado de la velocidad (1/s)
    float crust = 0.0f;                         // lava: costra oscura con ruido
};

struct FluidSimSettings {
    float particle_radius = 0.06f;        // m (separacion = 2 * radio)
    std::uint32_t max_particles = 32768;  // capacidad
    core::Vec3 gravity{0.0f, -9.81f, 0.0f};
    std::uint32_t iterations = 3;  // de la restriccion de densidad
    float substep = 1.0f / 120.0f;  // s (fijo)
    core::Vec3 domain_min{-5.0f, -1.0f, -5.0f};
    core::Vec3 domain_max{5.0f, 6.0f, 5.0f};
    bool solid_walls = false;    // el dominio es un tanque
    bool kill_outside = true;    // las que salen del dominio mueren
    float friction = 0.1f;       // contra los colliders
    float max_speed = 25.0f;     // m/s
    float surface_tension = 1.0f;  // escala de la cohesion
};

struct FluidRenderSettings {
    float render_radius = 1.3f;     // * radio de particula (esferas de profundidad)
    float smoothing = 1.0f;         // suavizado de la superficie (0..2)
    float thickness = 1.0f;         // escala del grosor
    float refraction = 1.0f;        // escala de la refraccion
    bool debug_particles = false;   // ver las particulas (sin superficie)
};

// Formas para colisionar (mundo). Caja: centro, medias medidas y giro;
// esfera: centro y radio; capsula: dos puntos y radio; plano: normal y d.
enum class FluidShapeType : std::uint32_t { Sphere = 0, Box = 1, Capsule = 2, Plane = 3 };
struct FluidShape {
    FluidShapeType type = FluidShapeType::Box;
    core::Vec3 a{};        // centro / punto 0 / normal
    core::Vec3 b{};        // medias medidas / punto 1 / (d, 0, 0)
    core::Quat rotation{};  // caja
    float radius = 0.0f;   // esfera / capsula
};

// Una particula nueva (la CPU decide donde nacen; la GPU las anade).
struct FluidSpawn {
    core::Vec3 position{};
    std::uint32_t material = 0;
    core::Vec3 velocity{};
    float lifetime = 0.0f;  // s (0 = sin limite)
};

// Lo que vuelve de la GPU (un par de frames tarde).
struct FluidSnapshot {
    std::uint32_t count = 0;
    std::vector<core::Vec4> positions;   // xyz, w = material
    std::vector<core::Vec4> velocities;  // xyz, w = edad
    std::uint64_t frame = 0;             // sube con cada lectura nueva
};

struct FluidStats {
    std::uint32_t particles = 0;  // vivas (lectura)
    std::uint32_t capacity = 0;
    std::uint32_t substeps = 0;   // del ultimo frame
    std::uint64_t memory_bytes = 0;
    std::uint32_t grid_cells = 0;
};

class FluidPass {
public:
    static constexpr std::uint32_t kMaterials = 8;
    static constexpr std::uint32_t kMaxShapes = 256;
    static constexpr std::uint32_t kMaxSpawnsPerFrame = 32768;
    static constexpr std::uint32_t kMaxHeightfield = 256;  // lado
    static constexpr std::uint32_t kMaxParticles = 262144;

    // `glass_layout`: el set de la escena para el vidrio (camara, luces,
    // sombras, profundidad, copia de la imagen HDR, cielo).
    void create(const VulkanDevice& device, const vk::raii::DescriptorSetLayout& glass_layout, vk::Format color_format,
                std::uint32_t frames_in_flight);
    void destroy();

    // --- Datos de la CPU (cada frame) ---
    void setSettings(const FluidSimSettings& settings);
    const FluidSimSettings& settings() const { return settings_; }
    void setRenderSettings(const FluidRenderSettings& settings) { render_settings_ = settings; }
    const FluidRenderSettings& renderSettings() const { return render_settings_; }
    void setMaterials(const std::array<FluidMaterial, kMaterials>& materials) { materials_ = materials; }
    void setShapes(std::vector<FluidShape> shapes) { shapes_ = std::move(shapes); }
    // Cajas donde las particulas desaparecen (desagues).
    void setDrains(std::vector<FluidShape> drains) { drains_ = std::move(drains); }
    // Campo de alturas (terreno): `heights` w x h, celda `cell` m desde
    // `origin` (x, z). Vacio = sin terreno.
    void setHeightfield(const core::Vec3& origin, float cell, std::uint32_t width, std::uint32_t height,
                        std::vector<float> heights);
    void spawn(const std::vector<FluidSpawn>& particles);
    // Avanzar `substeps` pasos fijos en el proximo frame (0 = en pausa).
    void step(std::uint32_t substeps) { pending_substeps_ = substeps; frame_pending_ = true; }
    // Borrar todas las particulas (en el proximo frame).
    void clear();
    // El mundo se desplazo (origen flotante): las particulas tambien.
    void shiftOrigin(const core::Vec3& offset) { pending_shift_ = pending_shift_ + offset; }
    // Activo: hay mundo de liquidos (se dibuja y se simula).
    void setActive(bool active) { active_ = active; }
    bool active() const { return active_; }

    const FluidSnapshot& snapshot() const { return snapshot_; }
    FluidStats stats() const { return stats_; }

    // --- Grabacion (VulkanRenderer) ---
    // Simulacion: fuera de cualquier render pass. Solo una vez por frame (las
    // vistas aisladas no la repiten).
    void recordSimulate(const vk::raii::CommandBuffer& cmd, std::uint32_t frame);

    // Render: imagenes propias a la resolucion interna. `scene_depth`: la
    // profundidad de la escena (solo lectura). Rehace sus recursos si cambia
    // el tamano o la vista de profundidad (espera a la GPU).
    struct View {
        core::Mat4 view = core::Mat4::identity();
        core::Mat4 projection = core::Mat4::identity();  // con jitter (como la escena)
        vk::Extent2D extent{};
        vk::ImageView scene_depth{};
    };
    // Profundidad, grosor y suavizado (fuera de cualquier render pass).
    void recordPrepare(const vk::raii::CommandBuffer& cmd, std::uint32_t frame, const View& view);
    // Sombreado sobre la imagen HDR (con el renderizado abierto sobre ella).
    void recordShade(const vk::raii::CommandBuffer& cmd, std::uint32_t frame, const vk::raii::DescriptorSet& glass_set,
                     vk::Extent2D extent) const;

private:
    void createSimulation();
    void createRender(vk::Format color_format);
    void allocateParticles(std::uint32_t capacity);
    void ensureTargets(vk::Extent2D extent, vk::ImageView scene_depth);
    void writeSimSets();
    void writeRenderSets();
    void readBack(std::uint32_t frame);
    void uploadFrame(std::uint32_t frame);
    void dispatchStage(const vk::raii::CommandBuffer& cmd, std::uint32_t frame, std::uint32_t stage, bool indirect,
                       std::uint32_t groups = 1, std::uint32_t extra = 0, std::uint32_t iteration = 0) const;

    const VulkanDevice* device_ = nullptr;
    std::uint32_t frames_ = 2;
    bool active_ = false;

    FluidSimSettings settings_{};
    FluidRenderSettings render_settings_{};
    std::array<FluidMaterial, kMaterials> materials_{};
    std::vector<FluidShape> shapes_;
    std::vector<FluidShape> drains_;
    std::vector<FluidSpawn> spawns_;
    std::uint32_t pending_substeps_ = 0;
    bool frame_pending_ = false;
    bool clear_pending_ = false;
    core::Vec3 pending_shift_{};

    // Rejilla y nucleo (derivados de los ajustes).
    std::uint32_t capacity_ = 0;
    std::array<std::int32_t, 3> grid_{1, 1, 1};
    std::uint32_t cells_ = 1;
    float rest_density_ = 1.0f;
    float constraint_epsilon_ = 1.0f;
    float scorr_scale_ = 0.0f;

    // Campo de alturas.
    core::Vec3 height_origin_{};
    float height_cell_ = 1.0f;
    std::uint32_t height_w_ = 0, height_h_ = 0;
    std::vector<float> heights_;
    bool heights_dirty_ = false;

    // --- Simulacion ---
    vk::raii::DescriptorSetLayout sim_set_layout_{nullptr};
    vk::raii::PipelineLayout sim_layout_{nullptr};
    vk::raii::Pipeline sim_pipeline_{nullptr};
    vk::raii::DescriptorPool pool_{nullptr};
    std::vector<vk::raii::DescriptorSet> sim_sets_;

    // Particulas (2 juegos: A = canonico, B = ordenado) y auxiliares.
    std::array<VulkanBuffer, 4> set_a_;  // pos, vel, info, pred
    std::array<VulkanBuffer, 4> set_b_;
    VulkanBuffer delta_, scalar_, omega_, vel_tmp_;
    VulkanBuffer cell_of_, cell_count_, cell_start_, cell_offset_, block_sums_;
    VulkanBuffer state_;  // contador + argumentos indirectos
    VulkanBuffer heightfield_;
    std::vector<VulkanBuffer> params_;    // por frame (UBO)
    std::vector<VulkanBuffer> shapes_buf_;  // por frame
    std::vector<VulkanBuffer> spawn_buf_;   // por frame
    std::vector<VulkanBuffer> readback_;    // por frame
    std::vector<bool> readback_ready_;
    std::vector<std::uint32_t> frame_spawns_;
    std::vector<std::uint32_t> frame_shapes_;

    FluidSnapshot snapshot_{};
    FluidStats stats_{};

    // --- Render ---
    vk::raii::DescriptorSetLayout render_set_layout_{nullptr};
    vk::raii::PipelineLayout render_layout_{nullptr};   // set 0 = este
    vk::raii::PipelineLayout shade_layout_{nullptr};    // set 0 = este, set 1 = vidrio
    vk::raii::PipelineLayout smooth_layout_{nullptr};
    vk::raii::Pipeline depth_pipeline_{nullptr};
    vk::raii::Pipeline thickness_pipeline_{nullptr};
    vk::raii::Pipeline smooth_pipeline_{nullptr};
    vk::raii::Pipeline shade_pipeline_{nullptr};
    vk::raii::Sampler sampler_{nullptr};
    std::vector<vk::raii::DescriptorSet> render_sets_;
    std::vector<VulkanBuffer> render_params_;  // por frame (UBO)
    VulkanImage depth_raw_;     // R32F: profundidad lineal de las esferas
    VulkanImage depth_test_;    // D32: la mas cercana
    VulkanImage depth_temp_;    // R32F: suavizado horizontal
    VulkanImage depth_smooth_;  // R32F: suavizado final
    std::array<VulkanImage, 3> thick_{};  // RGBA16F a media resolucion
    vk::Extent2D extent_{};
    vk::ImageView bound_depth_{};
    bool targets_ready_ = false;
    float time_ = 0.0f;
};

}  // namespace cramion::gfx

#endif  // CRAMION_VK_FLUID_PASS_H
