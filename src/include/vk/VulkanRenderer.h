#ifndef CRAMION_VK_VULKAN_RENDERER_H
#define CRAMION_VK_VULKAN_RENDERER_H

#include "vk/ComputePass.h"
#include "vk/FullscreenPass.h"
#include "vk/GBuffer.h"
#include "vk/GpuTypes.h"
#include "vk/IblProbe.h"
#include "vk/LightingPass.h"
#include "vk/LocalShadowMaps.h"
#include "vk/PostProcessPass.h"
#include "vk/ShadowMap.h"
#include "vk/SkinnedModel.h"
#include "vk/SkinnedPass.h"
#include "vk/VulkanBuffer.h"
#include "vk/VulkanCommon.h"
#include "vk/VulkanDevice.h"
#include "vk/VulkanInstance.h"
#include "vk/VulkanSurface.h"
#include "vk/VulkanSwapchain.h"

#include "core/Frustum.h"
#include "scene/LocalLightShadows.h"
#include "scene/ShadowCascades.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <vector>

namespace cramion::scene {
class Scene;
}
namespace cramion::gfx {

// Renderizador diferido completo.
//
// Orden de inicializacion:
//   instancia -> superficie -> dispositivo -> swapchain -> G-buffer ->
//   pipelines -> descriptores -> comandos -> sincronizacion
//
// Cada frame se dibuja en estas pasadas:
//   1. Sombras:      la escena se dibuja desde el sol, una vez por cascada,
//                    guardando solo profundidad (CSM). Despues, desde cada
//                    foco y cada cara del cubo de las luces puntuales con
//                    sombra, pero solo si la luz ha cambiado (cache).
//   2. Geometria:    los modelos se escriben en el G-buffer (albedo, normal,
//                    material, profundidad). No se evalua ninguna luz.
//   3. Cielo:        dispersion atmosferica (Rayleigh + Mie + ozono) sobre una
//                    LUT pequena con la radiancia del cielo en cada direccion.
//                    De ella sale el IBL (compute): cubo de entorno
//                    prefiltrado por rugosidad e irradiancia en armonicos.
//   4. SSAO:         oclusion ambiental de pantalla a partir del depth y las
//                    normales del G-buffer.
//      SSGI:         luz rebotada: rayos por el depth buffer que recogen la
//                    imagen iluminada del frame anterior (media resolucion).
//   5. Iluminacion:  un triangulo a pantalla completa lee el G-buffer y acumula
//                    el cielo, el sol (consultando las cascadas), las luces
//                    puntuales y los focos (con sus propios mapas) con una
//                    BRDF fisica, sobre una imagen HDR (RGBA16F). El ambiente,
//                    la niebla y los reflejos salen de la LUT del cielo.
//   6. Bloom:        cadena de mitades de resolucion (bajada con filtro de 13
//                    muestras, subida con tienda 3x3 aditiva).
//   7. Rayos de luz: desenfoque radial del cielo hacia el sol (media resolucion).
//   8. Exposicion:   histograma de luminancia + promedio con adaptacion
//                    temporal (compute), como la auto-exposicion de Unreal.
//   9. Composicion:  HDR + bloom + rayos -> exposicion -> ACES -> gradacion ->
//                    gamma, sobre una imagen de 8 bits.
//  10. Post-proceso: FXAA sobre esa imagen -> imagen de la swapchain. Sin esta
//                    pasada todos los bordes quedan en escalera, porque un
//                    diferido no puede usar MSAA de forma asequible.
class VulkanRenderer {
public:
    // Niveles de la cadena de bloom (debe coincidir con kBloomLevels de
    // composite.frag). El primero es la mitad de la pantalla.
    static constexpr std::uint32_t kBloomLevels = 6;

    static constexpr vk::Format kHdrFormat = vk::Format::eR16G16B16A16Sfloat;
    static constexpr vk::Format kLdrFormat = vk::Format::eR8G8B8A8Unorm;
    static constexpr vk::Format kSsaoFormat = vk::Format::eR16G16Sfloat;
    static constexpr vk::Extent2D kSkyLutExtent{256, 128};

    VulkanRenderer() = default;
    ~VulkanRenderer();

    VulkanRenderer(const VulkanRenderer&) = delete;
    VulkanRenderer& operator=(const VulkanRenderer&) = delete;

    void initialize(const EngineInfo& info, HWND window, std::uint32_t width,
                    std::uint32_t height);
    void shutdown();

    // Sube a la GPU los modelos con esqueleto de la escena (mallas, texturas y
    // materiales). Sus instancias (Scene::actors) se dibujan cada frame.
    void uploadModels(const scene::Scene& scene);

    // Dibuja un frame de la escena indicada.
    void drawFrame(const scene::Scene& scene);

    void onResize(std::uint32_t width, std::uint32_t height);
    void waitIdle() const;

    // --- Ajustes de sombras ---
    void setShadowsEnabled(bool enabled) { shadows_enabled_ = enabled; }
    bool shadowsEnabled() const { return shadows_enabled_; }

    // Pinta cada cascada de un color, como el visualizador de cascadas de
    // Unreal: sirve para comprobar el reparto del frustum.
    void setCascadeDebug(bool enabled) { cascade_debug_ = enabled; }
    bool cascadeDebug() const { return cascade_debug_; }

    scene::ShadowCascades& shadowCascades() { return cascades_; }

    // Antialiasing de la imagen final.
    void setAntialiasingEnabled(bool enabled) { antialiasing_enabled_ = enabled; }
    bool antialiasingEnabled() const { return antialiasing_enabled_; }

    // --- Post-proceso ---
    void setBloomEnabled(bool enabled) { bloom_enabled_ = enabled; }
    bool bloomEnabled() const { return bloom_enabled_; }

    void setSsaoEnabled(bool enabled) { ssao_enabled_ = enabled; }
    bool ssaoEnabled() const { return ssao_enabled_; }

    // Iluminacion global de pantalla (luz rebotada).
    void setGiEnabled(bool enabled) { gi_enabled_ = enabled; }
    bool giEnabled() const { return gi_enabled_; }

    // Tonemapper: false = Khronos PBR Neutral (colores fieles), true = ACES.
    void setAcesTonemapper(bool aces) { aces_tonemapper_ = aces; }
    bool acesTonemapper() const { return aces_tonemapper_; }

    void setLightShaftsEnabled(bool enabled) { light_shafts_enabled_ = enabled; }
    bool lightShaftsEnabled() const { return light_shafts_enabled_; }

    // --- Exposicion (como el "Exposure" de un Post Process Volume) ---
    void setAutoExposureEnabled(bool enabled) { auto_exposure_enabled_ = enabled; }
    bool autoExposureEnabled() const { return auto_exposure_enabled_; }

    // Compensacion en EV: +1 = el doble de brillo, -1 = la mitad.
    void setExposureCompensation(float ev) { exposure_compensation_ = ev; }
    float exposureCompensation() const { return exposure_compensation_; }

    // Exposicion aplicada y luminancia media medida (leidas de la GPU con un
    // par de frames de retraso; solo para mostrarlas).
    float currentExposure() const { return displayed_exposure_; }
    float measuredLuminance() const { return displayed_luminance_; }

    bool isInitialized() const { return initialized_; }
    const VulkanDevice& device() const { return device_; }
    const VulkanSwapchain& swapchain() const { return swapchain_; }

    // Estadisticas de la ultima subida de mundo y del ultimo frame.
    std::uint64_t frameCount() const { return frame_count_; }
    std::uint32_t modelCount() const { return static_cast<std::uint32_t>(skinned_models_.size()); }
    std::uint64_t triangleCount() const { return triangle_count_; }

    // Frustum culling del ultimo frame (clusteres de geometria).
    std::uint32_t visibleSubmeshes() const { return visible_submeshes_; }
    std::uint32_t shadowSubmeshes() const { return shadow_submeshes_; }
    std::uint32_t totalSubmeshes() const { return total_submeshes_; }

private:
    void createCommandObjects();
    void createSyncObjects();
    // Imagenes intermedias que dependen del tamano de la ventana: HDR, SSAO,
    // cadena de bloom y la imagen de 8 bits que lee el FXAA.
    void createRenderTargets();
    void createUniformBuffers();
    void createDescriptors();
    // Rehace los descriptores que apuntan a imagenes del tamano de la ventana.
    void updateLightingDescriptors();
    void updatePostDescriptors();
    void recreateSwapchain();

    void updateUniforms(const scene::Scene& scene, std::uint32_t frame_index);
    void recordCommandBuffer(const vk::raii::CommandBuffer& cmd, std::uint32_t image_index,
                             std::uint32_t frame_index);
    void recordShadowPass(const vk::raii::CommandBuffer& cmd, std::uint32_t frame_index);
    void recordLocalShadowPass(const vk::raii::CommandBuffer& cmd, std::uint32_t frame_index);

    // Huesos de todos los actores de este frame, en un solo storage buffer.
    void updateActors(const scene::Scene& scene, std::uint32_t frame_index);
    void ensureBoneCapacity(std::uint32_t frame_index, std::size_t bone_count);
    void writeBoneDescriptor(std::uint32_t frame_index);

    // Dibuja los actores en un mapa de sombras ya abierto. Con `range` > 0
    // solo los que tocan la esfera de la luz.
    void recordActorShadows(const vk::raii::CommandBuffer& cmd, std::uint32_t frame_index,
                            const vk::raii::Pipeline& pipeline,
                            const core::Mat4& light_view_projection,
                            const core::Vec3& light_position = {}, float range = 0.0f);
    bool actorsTouch(const core::Vec3& light_position, float range) const;
    void recordGeometryPass(const vk::raii::CommandBuffer& cmd, std::uint32_t frame_index);
    void recordSsaoPass(const vk::raii::CommandBuffer& cmd, std::uint32_t frame_index);
    void recordSsgiPass(const vk::raii::CommandBuffer& cmd, std::uint32_t frame_index);
    void recordLightingPass(const vk::raii::CommandBuffer& cmd, std::uint32_t frame_index);
    void recordSkyLutPass(const vk::raii::CommandBuffer& cmd);
    void recordBloomPass(const vk::raii::CommandBuffer& cmd);
    void recordLightShaftPass(const vk::raii::CommandBuffer& cmd);
    void recordAutoExposurePass(const vk::raii::CommandBuffer& cmd);
    void recordCompositePass(const vk::raii::CommandBuffer& cmd, std::uint32_t frame_index);
    void recordPostProcessPass(const vk::raii::CommandBuffer& cmd, std::uint32_t image_index);

    // Subsistemas, declarados en orden de creacion (se destruyen al reves).
    VulkanInstance instance_{};
    VulkanSurface surface_{};
    VulkanDevice device_{};
    VulkanSwapchain swapchain_{};
    GBuffer gbuffer_{};

    // HDR lineal: la escribe la iluminacion y la leen el bloom y la
    // composicion.
    VulkanImage scene_color_{};
    // Resultado de la composicion (tono + gamma); lo lee el FXAA.
    VulkanImage ldr_color_{};
    // r = oclusion ambiental, g = profundidad lineal (para el desenfoque).
    VulkanImage ssao_image_{};
    // Luz rebotada (rgb) y visibilidad del cielo (a), a media resolucion.
    VulkanImage gi_image_{};
    std::array<VulkanImage, kBloomLevels> bloom_levels_{};
    // Radiancia del cielo en cada direccion (no depende de la ventana).
    VulkanImage sky_lut_{};
    // IBL: entorno prefiltrado, irradiancia y LUT de la BRDF, sacados del
    // cielo cada frame.
    IblProbe ibl_probe_{};
    // Rayos de luz, a media resolucion.
    VulkanImage light_shafts_{};

    // Auto-exposicion: histograma (se vacia solo cada frame), estado
    // persistente y una copia por frame en vuelo para leerla en la CPU.
    VulkanBuffer histogram_buffer_{};
    VulkanBuffer exposure_buffer_{};
    std::vector<VulkanBuffer> exposure_readback_;

    ShadowMap shadow_map_{};
    LocalShadowMaps local_shadow_maps_{};
    LightingPass lighting_pass_{};
    PostProcessPass post_process_pass_{};
    SkinnedPass skinned_pass_{};
    FullscreenPass ssao_pass_{};
    FullscreenPass bloom_down_pass_{};
    FullscreenPass bloom_up_pass_{};
    FullscreenPass composite_pass_{};
    FullscreenPass sky_lut_pass_{};
    FullscreenPass ssgi_pass_{};
    FullscreenPass light_shaft_pass_{};
    ComputePass histogram_pass_{};
    ComputePass exposure_average_pass_{};

    // Modelos con esqueleto en la GPU, en el mismo orden que Scene::models().
    std::vector<SkinnedModel> skinned_models_;

    // Lo que se dibuja de cada actor este frame.
    struct ActorDraw {
        std::uint32_t model = 0;
        core::Mat4 transform = core::Mat4::identity();
        std::uint32_t bone_offset = 0;
        core::Vec3 bounds_center{};
        float bounds_radius = 0.0f;
        // Modelos rigidos de un solo hueso (escenarios): cada submalla tiene
        // su caja en el mundo, en submesh_bounds_ a partir de este indice.
        // Los animados se descartan enteros por su esfera.
        bool per_submesh = false;
        std::uint32_t first_bounds = 0;
    };
    std::vector<ActorDraw> actor_draws_;
    std::vector<core::Aabb> submesh_bounds_;
    core::Mat4 camera_view_projection_ = core::Mat4::identity();
    // La del frame anterior, para reproyectar su imagen en el SSGI.
    core::Mat4 previous_view_projection_ = core::Mat4::identity();
    // La imagen HDR contiene un frame anterior valido (no justo tras crearla).
    bool scene_history_valid_ = false;

    // Llama a `draw(submesh_index)` con las submallas opacas del actor que
    // tocan el volumen. Devuelve cuantas.
    template <typename Draw>
    std::uint32_t forEachVisibleSubmesh(const ActorDraw& actor, const core::Frustum& frustum,
                                        Draw&& draw) const;

    // Estadisticas del ultimo frame: submallas dibujadas en el G-buffer y en
    // todas las pasadas de sombra, y el total que habria sin culling.
    std::uint32_t visible_submeshes_ = 0;
    std::uint32_t shadow_submeshes_ = 0;
    std::uint32_t total_submeshes_ = 0;
    std::vector<core::Mat4> bone_staging_;

    // Storage buffers de huesos, uno por frame en vuelo. Crecen segun haga
    // falta: no hay un maximo de huesos fijado de antemano.
    std::vector<VulkanBuffer> bone_buffers_;
    vk::raii::DescriptorPool skin_pool_{nullptr};
    std::vector<vk::raii::DescriptorSet> skin_sets_;

    // Huecos de sombra local que el frame anterior dibujaron algun actor: hay
    // que redibujarlos una vez mas cuando el actor sale, o su silueta se
    // quedaria congelada en la cache.
    std::array<bool, scene::kMaxShadowedSpotLights> spot_had_actor_{};
    std::array<bool, scene::kMaxShadowedPointLights> point_had_actor_{};

    // Reparto del frustum entre cascadas; se recalcula cada frame.
    scene::ShadowCascades cascades_{};
    // Matrices, huecos y cache de las sombras de focos y luces puntuales.
    scene::LocalLightShadows local_shadows_{};

    vk::raii::DescriptorPool descriptor_pool_{nullptr};
    std::vector<vk::raii::DescriptorSet> lighting_sets_;
    std::vector<vk::raii::DescriptorSet> post_process_sets_;

    // Descriptores del SSAO (uno por frame: leen la camara de ese frame), del
    // bloom (uno por nivel) y de la composicion.
    vk::raii::DescriptorPool post_pool_{nullptr};
    std::vector<vk::raii::DescriptorSet> ssao_sets_;
    std::vector<vk::raii::DescriptorSet> bloom_down_sets_;
    std::vector<vk::raii::DescriptorSet> bloom_up_sets_;
    std::vector<vk::raii::DescriptorSet> composite_sets_;
    std::vector<vk::raii::DescriptorSet> light_shaft_sets_;
    std::vector<vk::raii::DescriptorSet> ssgi_sets_;  // uno por frame
    std::vector<vk::raii::DescriptorSet> histogram_sets_;
    std::vector<vk::raii::DescriptorSet> exposure_average_sets_;

    // Exposicion manual (con la auto-exposicion apagada); sigue a la luz de
    // dia.
    float exposure_ = 1.0f;
    float exposure_compensation_ = 0.0f;
    float displayed_exposure_ = 1.0f;
    float displayed_luminance_ = 0.0f;

    // Datos de este frame para el cielo, el IBL y los rayos de luz.
    GpuSkyPush sky_push_{};
    core::Vec3 ibl_light_radiance_{};
    core::Vec3 ibl_to_light_{0.0f, 1.0f, 0.0f};
    GpuLightShaftPush light_shaft_push_{};

    // Tiempo entre frames para la adaptacion de la exposicion.
    std::chrono::steady_clock::time_point last_frame_time_{};
    float frame_delta_seconds_ = 0.0f;

    std::vector<VulkanBuffer> camera_buffers_;
    std::vector<VulkanBuffer> light_buffers_;
    std::vector<VulkanBuffer> shadow_buffers_;
    std::vector<VulkanBuffer> local_shadow_buffers_;


    vk::raii::CommandPool command_pool_{nullptr};
    std::vector<vk::raii::CommandBuffer> command_buffers_;

    std::vector<vk::raii::Semaphore> image_available_;
    std::vector<vk::raii::Fence> in_flight_fences_;
    std::vector<vk::raii::Semaphore> render_finished_;

    std::uint32_t current_frame_ = 0;
    std::uint64_t frame_count_ = 0;
    std::uint64_t triangle_count_ = 0;

    std::uint32_t window_width_ = 0;
    std::uint32_t window_height_ = 0;
    bool framebuffer_resized_ = false;
    bool initialized_ = false;
    bool shadows_enabled_ = true;
    bool antialiasing_enabled_ = true;
    bool cascade_debug_ = false;
    bool bloom_enabled_ = true;
    bool ssao_enabled_ = true;
    bool light_shafts_enabled_ = true;
    bool gi_enabled_ = true;
    bool aces_tonemapper_ = false;
    bool auto_exposure_enabled_ = true;
    // Los mapas locales se conservan entre frames (cache), asi que solo el
    // primero parte de un layout indefinido.
    bool local_shadow_layout_ready_ = false;
};

}  // namespace cramion::gfx

#endif  // CRAMION_VK_VULKAN_RENDERER_H
