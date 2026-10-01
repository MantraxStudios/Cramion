#ifndef CRAMION_VK_PRECIPITATION_PASS_H
#define CRAMION_VK_PRECIPITATION_PASS_H

#include "CramionFX/core/Math.h"
#include "CramionFX/vk/VulkanBuffer.h"
#include "CramionFX/vk/VulkanCommon.h"

#include <cstdint>
#include <vector>

namespace cramion::gfx {

class VulkanDevice;
class VulkanImage;

// Lo que el sistema de ambiente (CramionCore/environment) pide dibujar y
// sombrear este frame. Por defecto, nada.
struct PrecipitationSettings {
    float rain = 0.0f;     // intensidad 0..1 (gotas con estela)
    float snow = 0.0f;     // 0..1 (copos)
    float dust = 0.0f;     // 0..1 (arena)
    float density = 1.0f;  // multiplica el numero de particulas
    bool splashes = true;  // salpicaduras en el suelo
    core::Vec3 wind{};     // m/s
    // Nieve acumulada (gbuffer_surface.glsl): cobertura 0..1, espesor (m) y
    // humedad al derretirse 0..1. Tinte de la estacion de la vegetacion (0
    // verde .. 1 otono).
    float snow_cover = 0.0f;
    float snow_thickness = 0.0f;
    float snow_melt = 0.0f;
    float season_tint = 0.0f;
    // Rayo: destello que ilumina la escena, el cielo y las nubes, donde cayo
    // (mundo) y su trazo en el cielo (segmentos: pares a.xyz + ancho,
    // b.xyz + brillo).
    float flash = 0.0f;
    core::Vec3 flash_position{};
    std::vector<core::Vec4> bolt;
    float bolt_brightness = 0.0f;
    // Niebla en el horizonte del cielo (0..1).
    float sky_fog = 0.0f;

    bool needsRainMap() const { return rain > 0.0f || snow > 0.0f || snow_cover > 0.0f || dust > 0.0f; }
    bool drawsSomething() const {
        return rain > 0.001f || snow > 0.001f || dust > 0.001f || (bolt_brightness > 0.001f && !bolt.empty());
    }
};

// Datos del frame para el dibujo.
struct PrecipitationFrame {
    core::Mat4 view_projection = core::Mat4::identity();
    core::Mat4 inverse_view_projection = core::Mat4::identity();
    core::Mat4 rain_view_projection = core::Mat4::identity();
    core::Vec3 camera_position{};
    core::Vec3 camera_right{1.0f, 0.0f, 0.0f};
    float tan_half_fov = 0.7f;
    vk::Extent2D extent{1, 1};
    bool rain_map_ready = false;
    core::Vec3 light{0.5f, 0.5f, 0.5f};  // luz (HDR lineal) que reciben las gotas
    float delta_seconds = 0.0f;
};

// Lluvia, nieve, polvo, salpicaduras y el trazo de los rayos sobre la imagen
// HDR, despues de la iluminacion (con el depth de la escena en solo lectura,
// como las particulas). Todo se genera en el vertex shader a partir del
// indice de instancia: miles a ~100 000 particulas sin memoria por particula
// ni trabajo en la CPU. Las particulas viven en una caja alrededor de la
// camara pero ancladas al mundo (al moverse la camara no se mueven con ella),
// caen con su velocidad y el viento, y no aparecen bajo techo (mapa de lluvia
// desde arriba).
class PrecipitationPass {
public:
    static constexpr std::uint32_t kMaxBoltSegments = 96;

    void create(const VulkanDevice& device, vk::Format color_format, vk::Format depth_format,
                std::uint32_t frames_in_flight);
    void destroy();

    // Prepara el frame (buffer y descriptores). false si no hay nada.
    bool prepare(const VulkanDevice& device, std::uint32_t frame, const PrecipitationSettings& settings,
                 const PrecipitationFrame& view, const VulkanImage& rain_map, vk::Sampler rain_sampler,
                 const VulkanImage& scene_depth);

    // Graba el dibujo (renderizado abierto sobre la imagen HDR con el depth
    // en solo lectura).
    void record(const vk::raii::CommandBuffer& cmd, std::uint32_t frame, vk::Extent2D viewport) const;

    // Particulas dibujadas el ultimo frame (estadisticas).
    std::uint32_t lastCount() const { return last_count_; }

private:
    vk::raii::Pipeline createPipeline(const VulkanDevice& device, vk::Format color_format, vk::Format depth_format,
                                      bool additive) const;

    vk::raii::DescriptorSetLayout set_layout_{nullptr};
    vk::raii::DescriptorPool pool_{nullptr};
    std::vector<vk::raii::DescriptorSet> sets_;
    vk::raii::PipelineLayout layout_{nullptr};
    vk::raii::Pipeline alpha_pipeline_{nullptr};
    vk::raii::Pipeline additive_pipeline_{nullptr};
    vk::raii::Sampler depth_sampler_{nullptr};
    std::vector<VulkanBuffer> buffers_;
    struct Counts {
        std::uint32_t rain = 0;
        std::uint32_t snow = 0;
        std::uint32_t dust = 0;
        std::uint32_t splashes = 0;
        std::uint32_t bolt = 0;
    };
    std::vector<Counts> counts_;
    double drift_[3] = {0.0, 0.0, 0.0};  // lo que ha empujado el viento (m)
    double seconds_ = 0.0;
    std::uint32_t last_count_ = 0;
};

}  // namespace cramion::gfx

#endif  // CRAMION_VK_PRECIPITATION_PASS_H
