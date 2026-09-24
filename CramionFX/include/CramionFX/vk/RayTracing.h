#ifndef CRAMION_VK_RAY_TRACING_H
#define CRAMION_VK_RAY_TRACING_H

#include "CramionFX/core/Math.h"
#include "CramionFX/vk/VulkanCommon.h"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace cramion::asset {
struct ModelData;
}

namespace cramion::gfx {

class SkinnedModel;
class VulkanBuffer;
class VulkanDevice;

// Trazado de rayos por hardware (VK_KHR_ray_query) para la luz rebotada y los
// reflejos: rayos contra la escena real en cada frame, sin depender de la
// pantalla ni de capturas.
//
//   - Una BLAS por modelo con sus triangulos en dos geometrias: los opacos
//     (el hardware no para en ellos) y los recortados por alfa (follaje: el
//     shader prueba el alfa de cada candidato).
//   - Una TLAS con una instancia por actor de escenario (modelos rigidos).
//   - Vertices, indices, materiales y todas las texturas accesibles desde los
//     shaders (rt_common.glsl) para sombrear el punto de impacto.
//   - Dos compute shaders: rt_gi.comp (media resolucion) y rt_reflections.comp.
class RayTracing {
public:
    struct Instance {
        std::uint32_t model = 0;
        core::Mat4 transform = core::Mat4::identity();
    };

    // Lo que leen y escriben los shaders cada frame (set 0).
    struct FrameInputs {
        const VulkanBuffer* camera = nullptr;
        const VulkanBuffer* lights = nullptr;
        vk::ImageView depth;
        vk::ImageView normal;
        vk::ImageView previous_color;
        vk::ImageView gi_output;          // storage, media resolucion
        vk::ImageView reflection_output;  // storage, resolucion completa
        vk::ImageView environment;        // cubo del IBL
        vk::Sampler sampler;              // lineal, bordes fijados
        vk::Sampler environment_sampler;
    };

    // Debe coincidir con el bloque de push constants de rt_common.glsl.
    struct Push {
        core::Mat4 previous_view_projection = core::Mat4::identity();
        core::Vec4 params{};  // x = numero de frame, y = hay frame anterior valido
    };

    enum class Pass : std::uint32_t { Gi = 0, Reflections = 1 };
    static constexpr std::uint32_t kPassCount = 2;

    RayTracing();
    ~RayTracing();
    RayTracing(const RayTracing&) = delete;
    RayTracing& operator=(const RayTracing&) = delete;

    void create(const VulkanDevice& device);
    void destroy();

    // Sube la escena (una vez, al cargar los modelos): buffers, BLAS,
    // materiales, texturas y los pipelines.
    void build(const VulkanDevice& device, const std::vector<const asset::ModelData*>& models,
               const std::vector<SkinnedModel>& gpu_models, const VulkanBuffer& irradiance);

    // Instancias de la TLAS. Solo se reconstruye si cambian.
    void setInstances(const VulkanDevice& device, const std::vector<Instance>& instances);

    void updateFrameSet(const VulkanDevice& device, std::uint32_t frame_index,
                        const FrameInputs& inputs);

    // Todo listo para trazar (escena subida y al menos una instancia).
    bool ready() const;

    void record(const vk::raii::CommandBuffer& cmd, std::uint32_t frame_index, Pass pass,
                vk::Extent2D extent, const Push& push) const;

private:
    struct Resources;
    std::unique_ptr<Resources> resources_;
};

}  // namespace cramion::gfx

#endif  // CRAMION_VK_RAY_TRACING_H
