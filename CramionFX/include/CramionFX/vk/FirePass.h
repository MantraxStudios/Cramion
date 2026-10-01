#ifndef CRAMION_VK_FIRE_PASS_H
#define CRAMION_VK_FIRE_PASS_H

// Fuego y humo volumetricos (como los incendios de las demos de Unreal): la
// simulacion (CramionCore fire::Fire) deja por zona una rejilla de celdas con
// lo quemado, el calor y el humo; aqui se sube a la GPU y se dibuja con un
// raymarching a pantalla completa sobre la imagen HDR, despues de la
// iluminacion:
//
//   - Llamas: emisivas, color de cuerpo negro (rojo oscuro -> naranja ->
//     amarillo blanco), ruido turbulento que sube, altura segun el calor.
//   - Humo: denso donde hay fuego, sube y se ensancha, el viento lo arrastra;
//     lo ilumina el sol (con unos pasos de sombra), el cielo y el resplandor
//     naranja del fuego de debajo.
//
// El mapa de cada zona (capa N = zona N) tambien lo lee la geometria
// (gbuffer_surface.glsl, binding 6 del set 0): suelo, hierba y arboles
// quemados se ven carbonizados.

#include "CramionFX/core/Math.h"
#include "CramionFX/vk/VulkanBuffer.h"
#include "CramionFX/vk/VulkanCommon.h"
#include "CramionFX/vk/VulkanImage.h"
#include "CramionFX/vk/VulkanTexture.h"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace cramion::gfx {

class VulkanDevice;

inline constexpr std::uint32_t kFireMapSize = 256;  // celdas por lado como mucho
inline constexpr std::uint32_t kFireZoneSlots = 4;  // = kMaxFireZones (GpuTypes.h)

// Una zona de fuego tal como la deja la simulacion.
struct FireZone {
    core::Vec3 origin{};             // esquina minima (x, z) y altura de referencia (y)
    float size = 0.0f;               // lado (m)
    std::uint32_t resolution = 0;    // celdas por lado (<= kFireMapSize)
    // resolution^2 texeles RGBA8: r = carbonizado, g = calor (llamas),
    // b = humo, a = humo ancho (mas difuminado, para lo alto de la columna).
    // Se comparan por puntero: un mapa nuevo (otro puntero) se vuelve a subir.
    std::shared_ptr<const std::vector<std::uint8_t>> cells;
    // Altura del suelo por celda, relativa a origin.y (m).
    std::shared_ptr<const std::vector<float>> heights;
    float ground_min = 0.0f;         // altura minima del suelo en la zona (mundo)
    float ground_max = 0.0f;         // altura maxima del suelo en la zona (mundo)
    float flame_height = 3.0f;       // m (con todo el calor)
    float flame_intensity = 1.0f;
    float smoke_height = 60.0f;      // m que sube el humo
    float smoke_density = 1.0f;
    float smoke_rise = 3.0f;         // m/s
    core::Vec3 smoke_color{0.11f, 0.105f, 0.1f};
    core::Vec2 wind{};               // m/s (x, z)
};

// Luz de la escena para el humo.
struct FireLighting {
    core::Vec3 to_sun{0.0f, 1.0f, 0.0f};
    core::Vec3 sun_color{1.0f, 1.0f, 1.0f};  // color x intensidad
    core::Vec3 ambient{0.2f, 0.25f, 0.3f};   // cielo (color x intensidad)
};

class FirePass {
public:
    void create(const VulkanDevice& device, vk::Format color_format, std::uint32_t frames_in_flight);
    void destroy();

    void setZones(const std::vector<FireZone>& zones);
    bool active() const { return active_count_ > 0; }

    // Para la geometria: xy = esquina minima (x, z), z = lado (m), w = parte
    // del mapa usada (resolucion / kFireMapSize; 0 = zona apagada).
    std::array<core::Vec4, kFireZoneSlots> zoneRects() const;

    // Mapa de quemado de la geometria (array de kFireZoneSlots capas).
    const vk::raii::ImageView& mapView() const { return map_.view(); }
    const vk::raii::Sampler& mapSampler() const { return map_sampler_; }

    // Sube lo que cambio (antes de la pasada de geometria, fuera de un render pass).
    void recordUpload(const vk::raii::CommandBuffer& cmd, std::uint32_t frame);
    // Dentro de un render pass sobre la imagen HDR (sin depth adjunto).
    void record(const vk::raii::CommandBuffer& cmd, std::uint32_t frame, vk::Extent2D extent, vk::Buffer camera_buffer,
                vk::DeviceSize camera_size, vk::ImageView depth_view, const FireLighting& lighting, float seconds,
                std::uint32_t frame_counter);

private:
    struct Slot {
        FireZone zone;
        bool active = false;
        const void* uploaded_cells = nullptr;    // lo que hay en la GPU
        const void* uploaded_heights = nullptr;
        std::uint32_t uploaded_resolution = 0;
    };
    void createPipeline(const VulkanDevice& device, vk::Format color_format);
    void createNoise(const VulkanDevice& device);

    const VulkanDevice* device_ = nullptr;
    std::array<Slot, kFireZoneSlots> slots_{};
    std::uint32_t active_count_ = 0;
    bool initialized_layout_ = false;

    VulkanImage map_;      // RGBA8, kFireZoneSlots capas
    VulkanImage height_;   // R16F (altura - origin.y), kFireZoneSlots capas
    VulkanTexture noise_;  // ruido 2D con el truco de las capas desplazadas (ruido 3D)
    vk::raii::Sampler map_sampler_{nullptr};
    vk::raii::Sampler noise_sampler_{nullptr};
    vk::raii::Sampler depth_sampler_{nullptr};
    std::vector<VulkanBuffer> staging_;   // por frame en vuelo
    std::vector<VulkanBuffer> uniforms_;  // por frame en vuelo

    vk::raii::DescriptorSetLayout set_layout_{nullptr};
    vk::raii::DescriptorPool pool_{nullptr};
    std::vector<vk::raii::DescriptorSet> sets_;
    vk::raii::PipelineLayout layout_{nullptr};
    vk::raii::Pipeline pipeline_{nullptr};
};

}  // namespace cramion::gfx

#endif  // CRAMION_VK_FIRE_PASS_H
