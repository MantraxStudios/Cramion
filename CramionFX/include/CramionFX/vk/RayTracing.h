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

// Trazado de rayos por hardware (VK_KHR_ray_query) para la luz rebotada, los
// reflejos y las sombras: rayos contra la escena real en cada frame, sin
// depender de la pantalla ni de capturas.
//
//   - Una BLAS por modelo con sus triangulos en dos geometrias: los opacos
//     (el hardware no para en ellos) y los recortados por alfa (follaje: el
//     shader prueba el alfa de cada candidato).
//   - Mallas extra que no son modelos (el terreno y las especies de arboles):
//     las da el renderizador; el terreno se puede reescribir al esculpir
//     (misma cantidad de vertices: se rehace solo su BLAS, en la GPU).
//   - Una TLAS con una instancia por actor de escenario, por terreno y por
//     arbol cercano. Se reconstruye EN LA GPU dentro del frame cuando algo se
//     mueve (sin parar la GPU: antes cada cambio hacia un waitIdle).
//   - Vertices, indices, materiales y todas las texturas accesibles desde los
//     shaders (rt_common.glsl) para sombrear el punto de impacto.
//   - Compute shaders: rt_gi.comp (media resolucion), rt_reflections.comp,
//     rt_shadows.comp (sombras del sol y de las luces locales) y
//     path_trace.comp (path tracing de referencia, activable).
//
// Nada de esto existe hasta que se pide (build): con el trazado apagado no
// hay BLAS, TLAS ni cache en la memoria de video.
class RayTracing {
public:
    // Mascaras de las instancias (las usan los rayos para elegir que ven).
    static constexpr std::uint8_t kMaskScenery = 0x01;  // modelos rigidos
    static constexpr std::uint8_t kMaskTerrain = 0x02;
    static constexpr std::uint8_t kMaskFoliage = 0x04;  // arboles (en el raster se mecen)

    struct Instance {
        std::uint32_t model = 0;
        core::Mat4 transform = core::Mat4::identity();
        std::uint8_t mask = kMaskScenery;
    };

    // Vertice de la escena de rayos (igual que rt_common.glsl, 32 bytes).
    struct Vertex {
        float px = 0.0f, py = 0.0f, pz = 0.0f;
        float nx = 0.0f, ny = 1.0f, nz = 0.0f;
        float u = 0.0f, v = 0.0f;
    };

    // Material de una malla extra. `base_color` multiplica la textura ya en
    // lineal (como el de los modelos). La textura guarda color sRGB en UNORM
    // (como las de los modelos): -1 = blanca.
    struct ExtraMaterial {
        core::Vec4 base_color{1.0f, 1.0f, 1.0f, 1.0f};
        float roughness = 0.9f;
        bool alpha_masked = false;
        std::int32_t texture = -1;  // indice en ExtraScene::textures
    };
    struct ExtraMesh {
        std::vector<Vertex> vertices;  // en el espacio del objeto
        std::vector<std::uint32_t> indices;
        std::vector<std::uint32_t> triangle_materials;  // por triangulo, indice en `materials`
        std::vector<ExtraMaterial> materials;
        // Sus vertices se pueden reescribir despues (updateExtraVertices).
        bool updatable = false;
    };
    // Textura de una malla extra: datos propios (RGBA8 con sus mips, el
    // primero el mayor; cuadrada) o una capa de un array de otro pase (se crea
    // una vista 2D UNORM de esa capa; la imagen debe vivir mas que la escena).
    struct ExtraTexture {
        std::uint32_t size = 0;
        std::vector<std::vector<std::uint8_t>> mips;
        vk::Image image{};
        vk::Format format = vk::Format::eUndefined;
        std::uint32_t layer = 0;
        std::uint32_t mip_count = 1;
    };
    struct ExtraScene {
        std::vector<ExtraMesh> meshes;
        std::vector<ExtraTexture> textures;
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
        vk::ImageView albedo;             // G-buffer (path tracing)
        vk::ImageView material;           // G-buffer: emision y metalicidad
        vk::ImageView shading;            // G-buffer: modelo de sombreado de Disney
        vk::ImageView path_output;        // storage: la imagen HDR de la escena
        vk::ImageView accumulation;       // storage RGBA32F: suma de caminos
        vk::ImageView shadow_output;      // storage RGBA16F: sombras del sol y de las luces locales
        vk::Sampler sampler;              // lineal, bordes fijados
        vk::Sampler environment_sampler;
    };

    // Debe coincidir con el bloque de push constants de rt_common.glsl.
    struct Push {
        core::Mat4 previous_view_projection = core::Mat4::identity();
        core::Vec4 params{};  // x = numero de frame, y = hay frame anterior valido
    };

    // CacheResolve: la cache de radiancia en el mundo (rt_cache_resolve.comp),
    // despues de la GI; se lanza con cacheResolveExtent(). PathTrace: el path
    // tracing (path_trace.comp) a resolucion completa.
    // Shadows: sombras del sol y de las luces locales (rt_shadows.comp) a resolucion completa.
    enum class Pass : std::uint32_t { Gi = 0, Reflections = 1, CacheResolve = 2, PathTrace = 3, Shadows = 4 };
    static constexpr std::uint32_t kPassCount = 5;
    // Entradas de la cache (debe coincidir con kCacheSize de rt_common.glsl).
    static constexpr std::uint32_t kCacheEntries = 1u << 19;
    static vk::Extent2D cacheResolveExtent() { return vk::Extent2D{1024, kCacheEntries / 1024}; }

    RayTracing();
    ~RayTracing();
    RayTracing(const RayTracing&) = delete;
    RayTracing& operator=(const RayTracing&) = delete;

    void create(const VulkanDevice& device);
    void destroy();

    // Sube la escena: buffers, BLAS, materiales, texturas y los pipelines.
    // Los modelos van con los indices 0..models.size()-1; la malla extra i,
    // con el indice models.size() + i. Hay que haber parado la GPU.
    void build(const VulkanDevice& device, const std::vector<const asset::ModelData*>& models,
               const std::vector<SkinnedModel>& gpu_models, const VulkanBuffer& irradiance,
               const ExtraScene& extra);
    // Libera la escena (BLAS, TLAS, buffers, texturas, cache). Hay que haber
    // parado la GPU.
    void releaseScene();
    bool sceneBuilt() const;
    std::uint32_t modelCount() const;  // modelos + mallas extra

    // Nuevos vertices de una malla extra `updatable` (la misma cantidad) y
    // nuevos mips de una textura propia (el mismo tamano): se suben y se rehace
    // su BLAS en el siguiente recordUpdates.
    void updateExtraVertices(std::uint32_t extra_index, std::vector<Vertex> vertices);
    void updateExtraTexture(std::uint32_t texture_index, std::vector<std::vector<std::uint8_t>> mips);

    // Instancias de la TLAS de este frame. Solo para la CPU: la TLAS se
    // reconstruye en recordUpdates si cambian. Si no caben, la TLAS crece
    // (eso si espera a la GPU, como mucho unas pocas veces).
    void setInstances(const VulkanDevice& device, const std::vector<Instance>& instances);

    // Al principio del command buffer del frame (antes de cualquier pase de
    // rayos): subidas pendientes, BLAS rehechas y la TLAS si cambio.
    void recordUpdates(const vk::raii::CommandBuffer& cmd, std::uint32_t frame_index);

    void updateFrameSet(const VulkanDevice& device, std::uint32_t frame_index,
                        const FrameInputs& inputs);

    // Todo listo para trazar este frame (escena subida y TLAS construida o
    // por construir en recordUpdates).
    bool ready() const;
    // El path tracing va por el pipeline de rayos con Shader Execution
    // Reordering (path_trace.rgen) en vez del compute: sus barreras deben
    // incluir la etapa de ray tracing.
    bool serActive() const;

    // Vacia la cache de radiancia en la siguiente resolucion (el origen
    // flotante se movio: sus celdas estan en las coordenadas viejas).
    void resetCache();

    // Sondas dinamicas (rt_probes.comp): actualiza `probe_count` sondas de
    // los volumenes horneados. Set 2 = el del renderizador (sondas + volumenes);
    // push.params: x = frame, y = primera sonda, z = cuantas, w = histeresis.
    void recordProbeUpdate(const vk::raii::CommandBuffer& cmd, std::uint32_t frame_index,
                           vk::DescriptorSetLayout probe_layout, vk::DescriptorSet probe_set, const Push& push,
                           std::uint32_t probe_count);

    void record(const vk::raii::CommandBuffer& cmd, std::uint32_t frame_index, Pass pass,
                vk::Extent2D extent, const Push& push) const;

private:
    struct Resources;
    std::unique_ptr<Resources> resources_;
};

}  // namespace cramion::gfx

#endif  // CRAMION_VK_RAY_TRACING_H
