#ifndef CRAMION_VK_VULKAN_RENDERER_H
#define CRAMION_VK_VULKAN_RENDERER_H

#include "CramionFX/vk/CloudNoise.h"
#include "CramionFX/vk/EnvironmentMap.h"
#include "CramionFX/vk/FrameBudget.h"
#include "CramionFX/vk/ComputePass.h"
#include "CramionFX/vk/FullscreenPass.h"
#include "CramionFX/vk/GBuffer.h"
#include "CramionFX/vk/GpuCulling.h"
#include "CramionFX/vk/GpuProfiler.h"
#include "CramionFX/vk/PostProcessSettings.h"
#include "CramionFX/vk/OverlayGeometry.h"
#include "CramionFX/vk/OverlayPass.h"
#include "CramionFX/vk/ParticlePass.h"
#include "CramionFX/vk/TerrainPass.h"
#include "CramionFX/vk/VoxelPass.h"
#include "CramionFX/vk/WaterPass.h"
#include "CramionFX/vk/GpuTypes.h"
#include "CramionFX/vk/GraphicsSettings.h"
#include "CramionFX/vk/IblProbe.h"
#include "CramionFX/vk/LightingPass.h"
#include "CramionFX/vk/LocalShadowMaps.h"
#include "CramionFX/vk/PostProcessPass.h"
#include "CramionFX/vk/RayTracing.h"
#include "CramionFX/vk/ReflectionProbe.h"
#include "CramionFX/vk/ShadowMap.h"
#include "CramionFX/vk/SkinnedModel.h"
#include "CramionFX/vk/SkinnedPass.h"
#include "CramionFX/vk/VulkanBuffer.h"
#include "CramionFX/vk/VulkanCommon.h"
#include "CramionFX/vk/VulkanDevice.h"
#include "CramionFX/vk/VulkanInstance.h"
#include "CramionFX/vk/VulkanSurface.h"
#include "CramionFX/vk/VulkanSwapchain.h"
#include "CramionFX/vk/VulkanTexture.h"

#include "CramionFX/core/Frustum.h"

#include <unordered_map>
#include "CramionFX/scene/LocalLightShadows.h"
#include "CramionFX/scene/ShadowCascades.h"

#include <array>
#include <chrono>
#include <filesystem>
#include <functional>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace cramion::scene {
class Camera;
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
//                    material, profundidad). No se evalua ninguna luz. Los
//                    escenarios se descartan en la GPU (GpuCulling): campo de
//                    vision y oclusion con una piramide Hi-Z en dos fases, y se
//                    dibujan con una llamada indirecta por material.
//   3. Cielo:        dispersion atmosferica (Rayleigh + Mie + ozono) sobre una
//                    LUT pequena con la radiancia del cielo en cada direccion.
//                    De ella sale el IBL (compute): cubo de entorno
//                    prefiltrado por rugosidad e irradiancia en armonicos.
//   4. SSAO:         oclusion ambiental de pantalla a partir del depth y las
//                    normales del G-buffer.
//      SSGI:         luz rebotada: rayos por el depth buffer que recogen la
//                    imagen iluminada del frame anterior (media resolucion).
//      SSR:          reflejos de pantalla sobre las superficies poco rugosas,
//                    con la misma imagen del frame anterior. Donde no
//                    encuentra nada (lo que no esta en pantalla) se refleja
//                    la sonda de reflexion.
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
//
// Sonda de reflexion: cuando la camara se aleja de la sonda o el sol se mueve,
// durante seis frames se dibuja ademas una cara del cubo desde la sonda (pasos
// 1 a 5, sin la luz del frame anterior) y al final se prefiltra. Mientras
// tanto se sigue usando la captura anterior.
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

    // Progreso de initialize() (0..1 y que se esta haciendo): para la pantalla
    // de carga mientras se compilan los shaders. Se llama desde initialize().
    void setLoadingCallback(std::function<void(float, const char*)> callback) { loading_callback_ = std::move(callback); }

    // Sube a la GPU los modelos con esqueleto de la escena (mallas, texturas y
    // materiales). Sus instancias (Scene::actors) se dibujan cada frame.
    void uploadModels(const scene::Scene& scene);
    // Solo el modelo `index` de la escena (nuevo al final o cambiado): sin
    // parar la GPU ni resubir lo demas. Para las mallas creadas por codigo
    // (el trazado de rayos no las ve hasta la siguiente uploadModels).
    void uploadModel(const scene::Scene& scene, std::uint32_t index);

    // Dibuja un frame de la escena indicada.
    // `present` = false: dibuja la vista (a su imagen de vista) sin
    // presentarla ni la interfaz: el editor la usa para la segunda vista.
    void drawFrame(const scene::Scene& scene, bool present = true);
    // Ayudas del editor (contorno, gizmos, picking): solo en la vista Escena.
    void setEditorHelpersEnabled(bool enabled) { editor_helpers_ = enabled; }

    void onResize(std::uint32_t width, std::uint32_t height);

    // Recrea ya la swapchain y las imagenes si la ventana cambio de tamano
    // (si no, lo hace drawFrame, que entonces se salta ese frame). Una UI que
    // muestra sceneImageView() debe llamarlo ANTES de construir su frame,
    // para no apuntar a una imagen que se va a destruir. true = se recreo.
    bool applyPendingResize();
    void waitIdle() const;

    // --- Ajustes de sombras ---
    void setShadowsEnabled(bool enabled) { shadows_enabled_ = enabled; }
    // Configuracion grafica: escalador (TAA, FSR, DLSS), resolucion interna,
    // nitidez y vsync. Si cambia la resolucion se rehacen los destinos al
    // final del frame.
    void setGraphicsSettings(const GraphicsSettings& settings);
    // Los del usuario (los que se guardan). Los que se usan de verdad pueden
    // llevar la resolucion bajada por el presupuesto adaptativo.
    const GraphicsSettings& graphicsSettings() const { return user_graphics_; }
    const GraphicsSettings& effectiveGraphicsSettings() const { return graphics_; }

    // --- Presupuesto adaptativo (FrameBudget.h) ---
    const HardwareProfile& hardwareProfile() const { return hardware_; }
    const FrameBudget& frameBudget() const { return budget_; }
    // Resolucion del mapa de sombras del sol en uso (por cascada).
    std::uint32_t shadowResolution() const { return shadow_map_.resolution(); }
    // Lado maximo de las texturas de los modelos en uso (0 = sin limite).
    std::uint32_t textureSizeLimit() const { return desiredTextureSize(); }
    // Objetos de menos de un pixel que no se dibujaron el ultimo frame.
    std::uint32_t culledSmallActors() const { return culled_small_; }
    // Resolucion a la que se dibuja la escena (la de pantalla por la escala).
    vk::Extent2D renderExtent() const { return render_extent_; }
    bool shadowsEnabled() const { return shadows_enabled_; }
    // Sombras del sol/luna (la luz direccional con "Proyecta sombras").
    void setSunShadowsEnabled(bool enabled) { sun_shadows_ = enabled; }
    bool sunShadowsEnabled() const { return sun_shadows_; }

    // Pinta cada cascada de un color, como el visualizador de cascadas de
    // Unreal: sirve para comprobar el reparto del frustum.
    void setCascadeDebug(bool enabled) { cascade_debug_ = enabled; }
    bool cascadeDebug() const { return cascade_debug_; }

    scene::ShadowCascades& shadowCascades() { return cascades_; }

    // --- Post-proceso (como el Volume de Unity) ---
    // PostProcessSettings es la unica fuente de verdad: setPostProcess() lo
    // cambia entero y los setters sueltos de abajo cambian un campo. Todo se
    // aplica en el frame siguiente.
    void setPostProcess(const PostProcessSettings& settings) {
        user_post_ = settings;
        post_ = budget_.apply(settings);
    }
    const PostProcessSettings& postProcess() const { return post_; }

    // --- Picking por ID en la GPU (el clic del editor) ---
    // Pide que objeto se ve en el pixel (x, y) de la imagen de la escena: en
    // el siguiente frame se dibujan los actores en una imagen de IDs solo en
    // ese pixel (con la profundidad del G-buffer: exacto, con la forma real
    // del objeto y lo que tapa a lo que). El resultado llega unos frames
    // despues (cuando la GPU termina): takePickResult() lo devuelve una vez.
    struct PickResult {
        bool hit = false;
        std::uint32_t actor = 0;  // indice en scene.actors()
        std::uint32_t material = 0;  // hueco de material (del modelo) bajo el pixel
        std::uint32_t x = 0;
        std::uint32_t y = 0;
    };
    void requestPick(std::uint32_t x, std::uint32_t y);
    // Cambia en vivo los factores (color, metal, rugosidad, emision) de un
    // material ya subido; texturas, tiling y transparencia necesitan volver
    // a subir el modelo (uploadModels).
    void updateModelMaterial(std::uint32_t model, std::uint32_t material, const asset::MaterialData& data);

    // Tiempo de CPU de cada fase de drawFrame (sumado si se dibuja mas de una
    // vista en el frame). takeFrameTimings() lo devuelve y lo pone a cero: el
    // editor lo usa para explicar los frames lentos.
    struct FrameTimings {
        float fence_wait_ms = 0.0f;  // esperando a que la GPU acabe el frame anterior
        float actors_ms = 0.0f;      // updateActors: actores, huesos, clusteres, TLAS
        float probe_ms = 0.0f;       // captura de la sonda de reflexion
        float acquire_ms = 0.0f;     // imagen de la swapchain
        float uniforms_ms = 0.0f;
        float record_ms = 0.0f;      // grabar los comandos
        float submit_ms = 0.0f;      // enviar y presentar
        float upload_ms = 0.0f;      // uploadModels / uploadModel (subir mallas y texturas)
        int uploads = 0;
        // Tiempo de CPU de grabar cada pase (nombre de la marca de la GPU).
        std::vector<std::pair<std::string, float>> passes;
    };
    // Milisegundos esperando a la GPU desde el principio (solo crece): el
    // componente Profiler resta la diferencia entre frames al tiempo del
    // frame para saber cuanto trabajo la CPU.
    double fenceWaitTotalMs() const { return fence_wait_total_ms_; }
    FrameTimings takeFrameTimings() {
        FrameTimings t = frame_timings_;
        frame_timings_ = {};
        return t;
    }

    // Shaders de superficie del usuario (.crshader): SPIR-V de surface.vert y
    // surface.frag con su codigo (ver shaders::compile). Devuelve el id que se
    // pone en MaterialData::surface_shader (-1 si la pipeline no se pudo crear).
    std::int32_t createSurfaceShader(const std::vector<std::uint32_t>& vertex_spirv,
                                     const std::vector<std::uint32_t>& fragment_spirv, std::string* error = nullptr);
    // Cambia el codigo de uno ya creado (recarga en caliente al guardar el
    // .crshader): los materiales que lo usan lo ven en el frame siguiente.
    bool updateSurfaceShader(std::int32_t id, const std::vector<std::uint32_t>& vertex_spirv,
                             const std::vector<std::uint32_t>& fragment_spirv, std::string* error = nullptr);
    std::optional<PickResult> takePickResult();
    bool pickPending() const;

    // Gizmos y ayudas en 3D con prueba de profundidad (OverlayGeometry.h). Se
    // dibujan en el frame siguiente a la llamada; se sustituyen enteras.
    void setOverlayGeometry(OverlayGeometry geometry) { overlay_geometry_ = std::move(geometry); }
    const OverlayGeometry& overlayGeometry() const { return overlay_geometry_; }

    // Particulas (ParticleGeometry.h): discos suaves sobre la imagen HDR,
    // con profundidad. Se dibujan en el frame siguiente; se sustituyen enteras.
    void setParticles(ParticleDrawList particles) { particles_ = std::move(particles); }
    const ParticleDrawList& particles() const { return particles_; }

    // Contorno de seleccion (naranja, como Unity) alrededor de estos actores
    // (indices en scene.actors()): mas intenso donde se ven, tenue donde algo
    // los tapa. Vacio = sin contorno (y sin coste).
    void setOutlinedActors(std::vector<std::uint32_t> actor_indices) {
        outlined_actors_ = std::move(actor_indices);
    }
    const std::vector<std::uint32_t>& outlinedActors() const { return outlined_actors_; }

    // Antialiasing de la imagen final.
    void setAntialiasingEnabled(bool enabled) { post_.fxaa = enabled; }
    bool antialiasingEnabled() const { return post_.fxaa; }

    void setBloomEnabled(bool enabled) { post_.bloom = enabled; }
    bool bloomEnabled() const { return post_.bloom; }

    void setSsaoEnabled(bool enabled) { post_.ambient_occlusion = enabled; }
    // Luz volumetrica: rayos de sol visibles en el polvo del aire al entrar
    // por ventanas y huecos. `density` en 1/m (0.01-0.05 es aire con polvo).
    void setVolumetricEnabled(bool enabled) { post_.volumetric_light = enabled; }
    bool volumetricEnabled() const { return post_.volumetric_light; }
    void setVolumetricDensity(float density) { post_.volumetric_density = density; }
    float volumetricDensity() const { return post_.volumetric_density; }
    bool ssaoEnabled() const { return post_.ambient_occlusion; }

    // Milisegundos de GPU de cada pasada (medias de ~1 s).
    const GpuProfiler& gpuProfiler() const { return gpu_profiler_; }

    // Reflejos de pantalla.
    void setSsrEnabled(bool enabled) { post_.reflections = enabled; }
    bool ssrEnabled() const { return post_.reflections; }

    // Trazado de rayos por hardware para la luz rebotada y los reflejos (si
    // la GPU lo tiene). Apagado, se usan los de pantalla con la sonda.
    void setRayTracingEnabled(bool enabled) { rt_enabled_ = enabled; }
    bool rayTracingEnabled() const { return rt_enabled_; }
    bool rayTracingSupported() const { return device_.rayTracingSupported(); }
    // Se esta trazando de verdad este frame (soportado, activado y listo).
    bool rayTracingActive() const {
        return rt_enabled_ && device_.rayTracingSupported() && ray_tracing_.ready();
    }

    // Occlusion culling (Hi-Z) de los escenarios. Apagado queda el de campo
    // de vision, tambien en la GPU.
    void setOcclusionCullingEnabled(bool enabled) { occlusion_culling_enabled_ = enabled; }
    bool occlusionCullingEnabled() const { return occlusion_culling_enabled_; }

    // Mapa de entorno HDR (el cielo fotografiado de la escena, p. ej. el de
    // Bistro). Sustituye al cielo fisico en el fondo, la niebla, el IBL y los
    // rayos. false si no se pudo leer.
    bool loadEnvironment(const std::filesystem::path& path);
    void setEnvironmentEnabled(bool enabled) { environment_enabled_ = enabled; }
    bool environmentEnabled() const { return environment_enabled_; }
    bool environmentLoaded() const { return environment_.loaded(); }
    bool environmentActive() const { return environment_enabled_ && environment_.loaded(); }
    // Hacia el sol de la foto (para colocar ahi la luz direccional).
    const core::Vec3& environmentSunDirection() const { return environment_.sunDirection(); }

    // Lluvia reciente: superficies a la intemperie mojadas (`wetness`, 0..1)
    // y charcos (`puddles`, 0..1) con las ondas de las gotas. 0 = seco.
    void setWeather(float wetness, float puddles) {
        wetness_ = wetness;
        puddles_ = puddles;
    }
    void setRainEnabled(bool enabled) { rain_enabled_ = enabled; }
    bool rainEnabled() const { return rain_enabled_; }
    bool rainAvailable() const { return wetness_ > 0.0f || puddles_ > 0.0f; }

    // Zona inundada: una lamina de agua sobre el suelo (como los charcos, pero
    // grande), en una elipse de radios `radii` (x, z) centrada en `center`
    // (x, z) con la orilla irregular. Radios 0 = sin agua.
    void setWater(const core::Vec2& center, const core::Vec2& radii) {
        water_center_ = center;
        water_radii_ = radii;
    }
    void setWaterEnabled(bool enabled) { water_enabled_ = enabled; }
    bool waterEnabled() const { return water_enabled_; }
    bool waterAvailable() const { return water_radii_.x > 0.0f && water_radii_.y > 0.0f; }

    // Decals: estampas (textura o color), charcos locales y manchas de
    // humedad proyectados sobre lo que haya dentro de su caja, como los
    // Decal Actors de Unreal. Los charcos se suman a los de la lluvia global.
    // Hasta kMaxDecals por frame (el resto se ignora).
    struct Decal {
        core::Mat4 world_to_decal = core::Mat4::identity();  // mundo -> caja [-0.5, 0.5]^3
        core::Vec3 axis{0.0f, 1.0f, 0.0f};  // eje Y de la caja en el mundo (se proyecta a lo largo)
        core::Vec3 color{1.0f, 1.0f, 1.0f};
        float opacity = 1.0f;
        int type = 0;         // 0 estampa, 1 charco, 2 humedad
        int texture = -1;     // ranura de loadDecalTexture (-1 = sin textura)
        float edge_softness = 0.1f;
        float angle_fade = 0.2f;  // coseno minimo entre la superficie y el eje
        float roughness = 0.5f;
        float roughness_amount = 0.0f;
        float metallic = 0.0f;
        float amount = 1.0f;  // charco: nivel del agua; humedad: cuanto moja
    };
    void setDecals(std::vector<Decal> decals) { decals_ = std::move(decals); }
    const std::vector<Decal>& decals() const { return decals_; }
    // Carga una imagen (PNG, JPG, TGA...) para los decals y devuelve su
    // ranura (0..kMaxDecalTextures-1); la misma ruta devuelve la misma
    // ranura. -1 si no se pudo leer o no quedan ranuras.
    int loadDecalTexture(const std::filesystem::path& file);

    // Nubes volumetricas.
    void setCloudsEnabled(bool enabled) { clouds_enabled_ = enabled; }
    bool cloudsEnabled() const { return clouds_enabled_; }

    // Sonda de reflexion de la escena (si no, lo que el SSR no ve refleja el
    // cielo).
    void setReflectionProbeEnabled(bool enabled) { probe_enabled_ = enabled; }
    bool reflectionProbeEnabled() const { return probe_enabled_; }

    // Iluminacion global de pantalla (luz rebotada).
    void setGiEnabled(bool enabled) { post_.global_illumination = enabled; }
    bool giEnabled() const { return post_.global_illumination; }

    // Tonemapper: false = Khronos PBR Neutral (colores fieles), true = ACES.
    void setAcesTonemapper(bool aces) {
        post_.tonemapper = aces ? Tonemapper::Aces : Tonemapper::Neutral;
    }
    bool acesTonemapper() const { return post_.tonemapper == Tonemapper::Aces; }

    void setLightShaftsEnabled(bool enabled) { post_.light_shafts = enabled; }
    bool lightShaftsEnabled() const { return post_.light_shafts; }

    // --- Exposicion (como el "Exposure" de un Post Process Volume) ---
    void setAutoExposureEnabled(bool enabled) { post_.auto_exposure = enabled; }
    bool autoExposureEnabled() const { return post_.auto_exposure; }

    // Compensacion en EV: +1 = el doble de brillo, -1 = la mitad.
    void setExposureCompensation(float ev) { post_.exposure_compensation = ev; }
    float exposureCompensation() const { return post_.exposure_compensation; }

    // Exposicion aplicada y luminancia media medida (leidas de la GPU con un
    // par de frames de retraso; solo para mostrarlas).
    float currentExposure() const { return displayed_exposure_; }
    float measuredLuminance() const { return displayed_luminance_; }

    bool isInitialized() const { return initialized_; }

    // --- Integracion con herramientas (CramionEditor) ---
    // Lo que necesita una UI dibujada con Vulkan (Dear ImGui) para compartir
    // el dispositivo y la swapchain del renderizador.
    struct NativeHandles {
        VkInstance instance = VK_NULL_HANDLE;
        VkPhysicalDevice physical_device = VK_NULL_HANDLE;
        VkDevice device = VK_NULL_HANDLE;
        std::uint32_t queue_family = 0;
        VkQueue queue = VK_NULL_HANDLE;
        VkFormat swapchain_format = VK_FORMAT_UNDEFINED;
        std::uint32_t image_count = 0;
        std::uint32_t api_version = 0;
    };
    NativeHandles nativeHandles() const;

    // Se llama cada frame dentro del ultimo pase (sobre la imagen de la
    // swapchain, ya con el FXAA): lo que se dibuje ahi queda encima de todo.
    using OverlayCallback = std::function<void(VkCommandBuffer)>;
    void setOverlayCallback(OverlayCallback callback) { overlay_ = std::move(callback); }

    // La imagen final de la escena (tono y gamma aplicados, antes del FXAA),
    // legible como textura (SHADER_READ_ONLY_OPTIMAL) durante el overlay.
    // Cambia al redimensionar: sceneImageGeneration() avisa de ello.
    VkImageView sceneImageView() const { return *ldr_color_.view(); }

    // --- Terrenos (TerrainPass.h) ---
    // Mapa de alturas `resolution` x `resolution` (0..1) y pesos de las capas
    // `splat_resolution`^2 (dos RGBA8). Devuelve un identificador (0 = error).
    std::uint32_t createTerrain(std::uint32_t resolution, std::uint32_t splat_resolution);
    void destroyTerrain(std::uint32_t id);
    void setTerrainDesc(std::uint32_t id, const TerrainDesc& desc) { terrain_pass_.setDesc(id, desc); }
    // Agua (oceano, lagos, rios): los cuerpos de este frame y el reloj de sus olas.
    void setWaterBodies(const std::vector<WaterBodyDesc>& bodies, float time, int underwater = -1) {
        water_pass_.setBodies(bodies, time, underwater);
    }
    // Una region de los datos completos (se sube en el siguiente frame).
    void updateTerrainHeights(std::uint32_t id, const float* heights, std::uint32_t x, std::uint32_t y,
                              std::uint32_t w, std::uint32_t h);
    void updateTerrainSplat(std::uint32_t id, const std::uint8_t* splat0, const std::uint8_t* splat1, std::uint32_t x,
                            std::uint32_t y, std::uint32_t w, std::uint32_t h) {
        terrain_pass_.updateSplat(id, splat0, splat1, x, y, w, h);
    }
    std::uint32_t terrainChunkCount() const { return terrain_pass_.chunkCount(); }

    // --- Voxeles (VoxelPass.h): mundos de bloques ---
    void setVoxelTextures(std::uint32_t size, const std::vector<VoxelTextureLayer>& layers) {
        voxel_pass_.setTextures(size, layers);
        staticGeometryChanged();
    }
    bool hasVoxelTextures() const { return voxel_pass_.hasTextures(); }
    void setVoxelSection(std::uint64_t key, const core::Vec3& origin, const std::uint32_t* vertices,
                         std::uint32_t vertex_count) {
        voxel_pass_.setSection(key, origin, vertices, vertex_count);
    }
    void removeVoxelSection(std::uint64_t key) {
        voxel_pass_.removeSection(key);
        staticGeometryChanged();
    }
    void clearVoxelSections() {
        voxel_pass_.clearSections();
        staticGeometryChanged();
    }
    void setVoxelTime(float seconds) { voxel_pass_.setTime(seconds); }
    void setVoxelsVisible(bool visible) { voxel_pass_.setVisible(visible); }
    VoxelStats voxelStats() const { return voxel_pass_.stats(); }

    // --- Texturas de la interfaz (iconos, miniaturas del editor) ---
    // RGBA8 con mipmaps (se ven bien pequenas). Devuelve un identificador
    // (0 = error); su vista se registra en ImGui. destroyUiTexture espera a
    // la GPU: quitar varias de golpe, no una por frame.
    std::uint32_t createUiTexture(const std::uint8_t* rgba, std::uint32_t width, std::uint32_t height);
    VkImageView uiTextureView(std::uint32_t id) const;
    void destroyUiTextures(const std::vector<std::uint32_t>& ids);

    // --- Vistas del editor (Escena y Juego) ---
    // Cada frame se dibuja con la camara de la escena y, al terminar, se copia
    // a la imagen de la vista `slot` (la que muestra el editor). Las demas
    // vistas conservan su ultimo frame. Cambian con el tamano, como la
    // imagen de la escena (sceneImageGeneration).
    static constexpr std::uint32_t kViewSlots = 2;
    void setViewSlot(std::uint32_t slot) { view_slot_ = slot < kViewSlots ? slot : 0; }
    std::uint32_t viewSlot() const { return view_slot_; }
    VkImageView viewImageView(std::uint32_t slot) const { return *view_images_[slot < kViewSlots ? slot : 0].view(); }
    // Corte de camara (otra vista, un corte de una cinematica): las pasadas
    // temporales (GI, reflejos) no reutilizan el frame anterior.
    void invalidateHistory();
    // Origen flotante: todo el mundo se desplazo -offset (ver
    // CramionCore/ecs/FloatingOrigin.h). Mueve lo que el renderizador guarda
    // en coordenadas del mundo (secciones de bloques, sonda de reflexion) y
    // descarta lo que no se puede mover (historias de TAA/SSR/GI, sombras
    // guardadas, mapa de lluvia). setWorldOrigin fija el origen absoluto al
    // cargar una escena guardada lejos (las nubes lo usan para no saltar).
    void shiftOrigin(const core::Vec3& offset);
    void setWorldOrigin(double x, double y, double z) { world_origin_ = {x, y, z}; }
    vk::Extent2D sceneExtent() const { return ldr_color_.extent(); }
    std::uint64_t sceneImageGeneration() const { return scene_image_generation_; }

    // Materiales ya subidos de un modelo, editables en vivo.
    std::vector<SkinnedModel::Material>* materials(std::uint32_t model) {
        return model < skinned_models_.size() ? &skinned_models_[model].materials() : nullptr;
    }
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
    // Clusteres en el campo de vision que la oclusion descarto (frame reciente).
    std::uint32_t occludedSubmeshes() const { return occluded_submeshes_; }

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

    // `camera`: la de la escena, o la de una cara de la sonda de reflexion.
    void updateUniforms(const scene::Scene& scene, const scene::Camera& camera,
                        std::uint32_t frame_index);

    // Sonda de reflexion: si toca (re)capturarla, empieza o sigue la captura.
    bool probeCaptureDue(const scene::Scene& scene);
    // Todo lo que cambia la luz que ve la sonda, salvo la direccion del sol
    // (esa tiene su propio umbral): colores e intensidades, luces locales y
    // los interruptores que cambian el aspecto de la escena.
    std::vector<float> lightingSignature(const scene::Scene& scene) const;
    // Dibuja la cara que toca desde la sonda y la copia al cubo (con la sexta,
    // lo prefiltra). Se envia y se espera aparte, antes del frame normal.
    void captureProbeFace(const scene::Scene& scene);
    void recordCommandBuffer(const vk::raii::CommandBuffer& cmd, std::uint32_t image_index,
                             std::uint32_t frame_index);
    void recordShadowPass(const vk::raii::CommandBuffer& cmd, std::uint32_t frame_index);
    void recordLocalShadowPass(const vk::raii::CommandBuffer& cmd, std::uint32_t frame_index);

    // Huesos de todos los actores de este frame, en un solo storage buffer.
    void updateActors(const scene::Scene& scene, std::uint32_t frame_index);
    void ensureBoneCapacity(std::uint32_t frame_index, std::size_t bone_count);
    void writeBoneDescriptor(std::uint32_t frame_index);

    // Dibuja los actores en un mapa de sombras ya abierto. Con `range` > 0
    // (luz local) solo los que tocan la esfera de la luz y con los pipelines
    // de las luces locales; con 0, los de las cascadas (depth clamp).
    void recordActorShadows(const vk::raii::CommandBuffer& cmd, std::uint32_t frame_index,
                            const core::Mat4& light_view_projection,
                            const core::Vec3& light_position = {}, float range = 0.0f,
                            float texel_world_size = 0.0f);
    // LOD para las sombras de una cascada: el mas simple cuyo error no pasa
    // de `allowed` unidades del mundo (lo que mide un texel, por el factor
    // del presupuesto). No depende de la camara.
    std::uint32_t shadowLod(const SkinnedModel& model, float max_scale, float allowed) const;
    // Detalle de sombras con el que se dibujaron las cascadas guardadas.
    std::uint32_t cascade_detail_key_ = 0;
    bool actorsTouch(const core::Vec3& light_position, float range) const;
    void recordGeometryPass(const vk::raii::CommandBuffer& cmd, std::uint32_t frame_index);
    // Dentro de un pase de geometria abierto: actores animados (culling en la
    // CPU, por su esfera) y clusteres de escenario (comandos indirectos que
    // escribio el culling en GPU en la fase `phase`).
    void drawCpuActors(const vk::raii::CommandBuffer& cmd, std::uint32_t frame_index);
    // Deja lista la pipeline del material (la estandar o la de su shader de
    // superficie) y rellena lo que el push constant lleva de ese shader.
    void bindMaterialPipeline(const vk::raii::CommandBuffer& cmd, std::uint32_t frame_index,
                              const SkinnedModel::Material& material, std::int32_t& bound_shader,
                              GpuSkinnedPush& push);
    std::uint32_t pushSurfaceParams(std::uint32_t frame_index, const std::array<core::Vec4, 8>& params);
    void drawGpuClusters(const vk::raii::CommandBuffer& cmd, std::uint32_t frame_index,
                         std::uint32_t phase);
    void recordSsaoPass(const vk::raii::CommandBuffer& cmd, std::uint32_t frame_index);
    void recordVolumetricPass(const vk::raii::CommandBuffer& cmd, std::uint32_t frame_index);
    void recordSsgiPass(const vk::raii::CommandBuffer& cmd, std::uint32_t frame_index);
    void recordSsrPass(const vk::raii::CommandBuffer& cmd, std::uint32_t frame_index);
    // Guarda los reflejos y la luz rebotada filtrados de este frame como
    // historia del siguiente.
    void recordFilterHistoryCopies(const vk::raii::CommandBuffer& cmd);
    // Escribe en `target`: la imagen HDR, o la de la captura de la sonda.
    void recordLightingPass(const vk::raii::CommandBuffer& cmd, std::uint32_t frame_index,
                            const VulkanImage& target);
    // Vidrio (ventanas) sobre la imagen HDR ya iluminada, con reflejos
    // (glass.frag). Va justo despues de la iluminacion.
    void recordGlassPass(const vk::raii::CommandBuffer& cmd, std::uint32_t frame_index);
    void recordWaterPass(const vk::raii::CommandBuffer& cmd, std::uint32_t frame_index);
    // Copia la imagen HDR (lo que hay detras) a glass_source_ y deja la
    // profundidad lista para probarse: la leen el vidrio y el agua.
    void copySceneForTransparency(const vk::raii::CommandBuffer& cmd);
    void updateGlassDescriptors();
    void recordSkyLutPass(const vk::raii::CommandBuffer& cmd);
    // Mapa de lluvia (la escena vista desde arriba). Una vez: el escenario no
    // se mueve.
    void recordRainMap(const vk::raii::CommandBuffer& cmd, std::uint32_t frame_index);
    // Nubes volumetricas (lee la LUT del cielo: va despues de ella).
    void recordCloudPass(const vk::raii::CommandBuffer& cmd, std::uint32_t frame_index);
    void recordBloomPass(const vk::raii::CommandBuffer& cmd);
    void recordLightShaftPass(const vk::raii::CommandBuffer& cmd);
    void recordAutoExposurePass(const vk::raii::CommandBuffer& cmd);
    void recordCompositePass(const vk::raii::CommandBuffer& cmd, std::uint32_t frame_index);
    // Contorno de seleccion sobre la imagen compuesta (si hay seleccion).
    void recordOutlinePass(const vk::raii::CommandBuffer& cmd, std::uint32_t frame_index);
    // Particulas sobre la imagen HDR (despues del vidrio, antes del bloom).
    void recordParticlePass(const vk::raii::CommandBuffer& cmd, std::uint32_t frame_index);
    // Picking: los actores bajo el pixel pedido en la imagen de IDs.
    void recordPickPass(const vk::raii::CommandBuffer& cmd, std::uint32_t frame_index);
    // Gizmos 3D (overlay_geometry_) sobre la imagen compuesta, con profundidad.
    void recordOverlayPass(const vk::raii::CommandBuffer& cmd, std::uint32_t frame_index);
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
    // Copia de scene_color_ antes del vidrio: lo que el vidrio refleja (no
    // puede leer la imagen en la que esta dibujando).
    VulkanImage glass_source_{};
    // Resultado de la composicion (tono + gamma); lo lee el FXAA.
    VulkanImage ldr_color_{};
    // r = oclusion ambiental, g = profundidad lineal (para el desenfoque).
    VulkanImage ssao_image_{};
    // Luz volumetrica a media resolucion (volumetric.frag).
    VulkanImage volumetric_image_{};
    // Luz rebotada (rgb) y visibilidad del cielo (a), a media resolucion. La
    // de este frame sin filtrar (rt_gi.comp o ssgi.frag) pasa por el filtro
    // SVGF: acumulacion temporal (gi_temporal.comp) y kGiAtrousIterations
    // pasadas espaciales (gi_atrous.comp). Salvo gi_raw_, todas viven en
    // layout General (las escriben y leen compute shaders).
    VulkanImage gi_raw_{};
    VulkanImage gi_temporal_{};         // acumulada
    VulkanImage gi_variance_{};         // su varianza de luminancia
    VulkanImage gi_moments_{};          // E[L], E[L^2], frames de historia, profundidad
    VulkanImage gi_moments_history_{};  // los del frame anterior
    VulkanImage gi_history_{};          // primera pasada espacial: historia del color
    std::array<VulkanImage, 2> gi_filter_{};           // ping-pong del filtro espacial
    std::array<VulkanImage, 2> gi_filter_variance_{};
    VulkanImage gi_image_{};            // resultado: lo lee la iluminacion
    // Reflejos de pantalla (rgb) y su confianza (a), a resolucion completa:
    // los de este frame sin filtrar (ssr.frag), los filtrados en el tiempo
    // (ssr_resolve.frag, los que lee la iluminacion) y la copia de estos que
    // sirve de historia al frame siguiente.
    VulkanImage ssr_raw_{};
    VulkanImage ssr_image_{};
    VulkanImage ssr_history_{};
    std::array<VulkanImage, kBloomLevels> bloom_levels_{};
    // Radiancia del cielo en cada direccion (no depende de la ventana).
    VulkanImage sky_lut_{};
    // IBL: entorno prefiltrado, irradiancia y LUT de la BRDF, sacados del
    // cielo cada frame.
    IblProbe ibl_probe_{};
    // Rayos de luz, a media resolucion.
    VulkanImage light_shafts_{};
    // Escalado (resolucion de pantalla): resultado del TAA/EASU, historia del
    // TAA, imagen final (tras la nitidez) y la profundidad escalada para los
    // gizmos.
    VulkanImage upscale_target_{};
    VulkanImage taa_history_{};
    VulkanImage upscaled_color_{};
    VulkanImage output_depth_{};
    bool output_depth_blit_ = false;
    // Mapa de lluvia: profundidad de la escena desde arriba (lo que tiene algo
    // encima no se moja) y su muestreador (sin comparacion).
    VulkanImage rain_map_{};
    // Zona inundada (la pinta skinned.frag con la lluvia).
    core::Vec2 water_center_{};
    core::Vec2 water_radii_{};
    bool water_enabled_ = true;
    vk::raii::Sampler rain_sampler_{nullptr};
    core::Mat4 rain_view_projection_ = core::Mat4::identity();
    bool rain_map_ready_ = false;
    std::vector<VulkanBuffer> weather_buffers_;
    std::vector<Decal> decals_;
    std::array<VulkanTexture, 8> decal_textures_;
    std::array<std::filesystem::path, 8> decal_texture_paths_;
    VulkanTexture decal_white_;
    vk::raii::Sampler decal_sampler_{nullptr};
    void writeDecalTextureDescriptors();
    float wetness_ = 0.0f;
    float puddles_ = 0.0f;
    float weather_time_ = 0.0f;
    bool rain_enabled_ = true;

    // Nubes volumetricas: ruido 3D y su imagen a media resolucion.
    CloudNoise cloud_noise_{};
    // Cielo fotografiado (opcional).
    EnvironmentMap environment_{};
    VulkanImage clouds_image_{};
    // Sonda de reflexion de la escena y la imagen HDR donde se dibuja cada
    // cara antes de copiarla al cubo (no se usa la del frame: el SSR y la GI
    // del siguiente la leen como frame anterior).
    ReflectionProbe reflection_probe_{};
    // Trazado de rayos: estructuras de aceleracion y pases de GI y reflejos.
    RayTracing ray_tracing_{};
    VulkanImage probe_capture_{};

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
    FullscreenPass volumetric_pass_{};
    FullscreenPass bloom_down_pass_{};
    FullscreenPass bloom_up_pass_{};
    FullscreenPass composite_pass_{};
    FullscreenPass sky_lut_pass_{};
    FullscreenPass ssgi_pass_{};
    FullscreenPass ssr_pass_{};
    FullscreenPass ssr_resolve_pass_{};
    ComputePass gi_temporal_pass_{};
    ComputePass gi_atrous_pass_{};
    FullscreenPass clouds_pass_{};
    FullscreenPass light_shaft_pass_{};
    // Escalado: TAA/TAAU, FSR 1 (EASU) y la nitidez (RCAS).
    FullscreenPass taa_pass_{};
    FullscreenPass easu_pass_{};
    FullscreenPass rcas_pass_{};
    ComputePass histogram_pass_{};
    ComputePass exposure_average_pass_{};

    // Modelos con esqueleto en la GPU, en el mismo orden que Scene::models().
    std::vector<SkinnedModel> skinned_models_;
    // Modelos reemplazados por uploadModel: se destruyen cuando ya ningun
    // frame en vuelo puede usarlos.
    struct RetiredModel {
        SkinnedModel model;
        std::uint32_t frames_left = 0;
    };
    std::vector<RetiredModel> retired_models_;
    // Los que conoce el trazado de rayos (sus descriptores apuntan a sus
    // texturas y buffers): si uploadModel los reemplaza, siguen vivos hasta la
    // siguiente uploadModels (los rayos ven la version anterior).
    std::uint32_t ray_traced_models_ = 0;
    std::vector<bool> ray_pinned_;
    std::vector<SkinnedModel> ray_pinned_models_;

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
        // Indice del actor en scene.actors() (para el contorno de seleccion).
        std::uint32_t scene_actor = 0;
        bool cast_shadows = true;
        bool shadows_only = false;
        // Matrices en el buffer de huesos (huesos + la instancia, si hay).
        std::uint32_t bone_entries = 0;
        // LOD elegido este frame (0 = la malla original) y, si no es 0, las
        // cajas en el mundo de sus submallas en lod_bounds_.
        std::uint32_t lod = 0;
        std::uint32_t first_lod_bounds = 0;
        // Escala mayor del objeto (el error de los LODs va en unidades del
        // modelo).
        float max_scale = 1.0f;
    };
    std::vector<ActorDraw> actor_draws_;
    // Material batching: los escenarios del mismo modelo y material (de
    // todos los actores) son UN lote, una sola llamada indirecta instanciada.
    struct DrawBatch {
        std::uint32_t model = 0;
        std::uint32_t group = 0;  // grupo de dibujo del modelo (su material)
        std::uint32_t first_slot = 0;
        std::uint32_t capacity = 0;
    };
    std::vector<DrawBatch> draw_batches_;
    std::unordered_map<std::uint64_t, std::uint32_t> batch_lookup_;
public:
    // Llamadas de dibujo de los escenarios (lotes) y clusteres que agrupan.
    std::uint32_t batchCount() const { return static_cast<std::uint32_t>(draw_batches_.size()); }
    // Llamadas de dibujo de las sombras del ultimo frame (tras juntar los
    // tramos seguidos).
    std::uint32_t shadowDrawCalls() const { return last_shadow_draw_calls_; }
    // LODs: triangulos de los escenarios con el LOD de cada uno (antes del
    // culling) y cuantos actores se dibujan con un LOD simplificado.
    std::uint64_t lodTriangles() const { return lod_triangles_; }
    std::uint32_t lodActors() const { return lod_actors_; }
private:
    // Deteccion de movimiento (updateActors): transform y esfera de cada
    // actor el frame anterior, y las esferas (antes y despues) de lo que se
    // movio. Una cascada guardada que contiene algo que se movio se redibuja
    // ya: si no, el objeto se sombreaba a si mismo con su sombra vieja.
    struct ActorMotion {
        core::Mat4 transform = core::Mat4::identity();
        core::Vec3 center{};
        float radius = 0.0f;
        bool cast_shadows = true;
        std::uint32_t lod = 0;
    };
    std::vector<ActorMotion> previous_actor_motion_;
    std::vector<core::Aabb> lod_bounds_;
    // Estadisticas de LOD del ultimo frame: triangulos de los escenarios con
    // el LOD elegido (sin culling) y cuantos actores usan un LOD > 0.
    std::uint64_t lod_triangles_ = 0;
    std::uint32_t lod_actors_ = 0;
    // Nivel para un actor rigido: el mas simple cuyo error proyectado no pasa
    // de post_.lod_pixel_error pixeles.
    std::uint32_t chooseLod(const SkinnedModel& model, const core::Mat4& to_world, const core::Vec3& center,
                            float radius, const core::Vec3& camera_position, float pixels_per_unit) const;
    std::vector<core::Vec4> moved_spheres_;  // xyz = centro, w = radio
    bool actor_set_changed_ = true;
    std::vector<core::Aabb> submesh_bounds_;
    // Culling en GPU de los clusteres de escenario.
    GpuCulling gpu_culling_{};
    GpuProfiler gpu_profiler_{};
    double fence_wait_total_ms_ = 0.0;
    std::uint32_t shadow_draw_calls_ = 0;
    std::uint32_t last_shadow_draw_calls_ = 0;
    std::array<double, 3> world_origin_{0.0, 0.0, 0.0};
    OverlayCallback overlay_;
    OverlayGeometry overlay_geometry_;
    OverlayPass overlay_pass_{};
    // Texturas de la interfaz.
    std::unordered_map<std::uint32_t, VulkanTexture> ui_textures_;
    std::uint32_t next_ui_texture_ = 1;
    // Imagenes de las vistas del editor (copias del resultado de cada frame).
    std::array<VulkanImage, kViewSlots> view_images_{};
    std::uint32_t view_slot_ = 0;
    void recordViewCopy(const vk::raii::CommandBuffer& cmd);
    // Picking por ID.
    VulkanImage pick_ids_{};
    std::vector<VulkanBuffer> pick_buffers_;  // 4 bytes por frame en vuelo (lectura en la CPU)
    std::array<bool, kMaxFramesInFlight> pick_in_flight_{};
    std::array<PickResult, kMaxFramesInFlight> pick_requests_{};
    bool pick_requested_ = false;
    PickResult pick_request_{};
    std::optional<PickResult> pick_result_;
    TerrainPass terrain_pass_{};
    VoxelPass voxel_pass_{};
    WaterPass water_pass_{};
    core::Vec3 camera_position_{};
    ParticleDrawList particles_;
    ParticlePass particle_pass_{};
    core::Mat4 camera_view_ = core::Mat4::identity();
    // Post-proceso y efectos de pantalla (unica fuente de verdad).
    PostProcessSettings post_{};
    // Contorno de seleccion: actores (de la escena) y sus recursos.
    std::vector<std::uint32_t> outlined_actors_;
    VulkanImage outline_mask_{};  // R = silueta entera, G = parte visible
    FullscreenPass outline_pass_{};
    std::vector<vk::raii::DescriptorSet> outline_sets_;
    // Uniform buffers de la composicion (GpuCompositeSettings), uno por frame.
    std::vector<VulkanBuffer> composite_buffers_;
    std::uint64_t scene_image_generation_ = 0;
    // Submallas visibles de un actor al dibujar sombras (se reutiliza).
    std::vector<std::uint32_t> shadow_scratch_;
    std::vector<GpuCluster> gpu_clusters_;
    std::uint32_t gpu_visible_submeshes_ = 0;
    std::uint32_t occluded_submeshes_ = 0;
    core::Mat4 camera_view_projection_ = core::Mat4::identity();
    // La del frame anterior, para reproyectar su imagen en el SSGI.
    core::Mat4 previous_view_projection_ = core::Mat4::identity();
    // La imagen HDR contiene un frame anterior valido (no justo tras crearla).
    bool scene_history_valid_ = false;
    bool ssr_history_ready_ = false;
    // ssr_history_ tiene los reflejos filtrados del frame anterior.
    bool ssr_filter_history_valid_ = false;
    // gi_history_ tiene la luz rebotada filtrada del frame anterior.
    bool gi_filter_history_valid_ = false;

    // --- Sonda de reflexion ---
    // Se esta dibujando una cara de la sonda (no un frame para la pantalla).
    bool capturing_ = false;
    // Cara que toca capturar (-1 = ninguna captura en curso).
    int probe_face_ = -1;
    // Donde y con que sol se hace la captura en curso...
    core::Vec3 probe_capture_position_{};
    core::Vec3 probe_capture_sun_{0.0f, 1.0f, 0.0f};
    // ...y los de la ultima terminada.
    core::Vec3 probe_position_{};
    core::Vec3 probe_sun_{0.0f, 1.0f, 0.0f};
    bool probe_ready_ = false;
    bool probe_enabled_ = true;
    // Cubo con la ultima captura y centro de cada cubo. Al terminar una
    // captura, la iluminacion pasa del cubo anterior al nuevo en
    // kProbeFadeSeconds (probe_fade_ de 0 a 1), sin saltos.
    std::uint32_t probe_cube_ = 0;
    std::array<core::Vec3, ReflectionProbe::kCubeCount> probe_centers_{};
    float probe_fade_ = 1.0f;
    // Firma de la luz de la captura en curso y de la ultima terminada.
    std::vector<float> probe_capture_signature_;
    std::vector<float> probe_signature_;
    // Primer frame en el que se puede capturar otra cara (reparto del coste).
    std::uint64_t probe_next_face_frame_ = 0;
    // Posicion de la camara el frame anterior, para saber si va despacio.
    core::Vec3 probe_last_camera_{};

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
    FrameTimings frame_timings_{};
    // Marca del perfilador de GPU que ademas apunta el tiempo de CPU del pase.
    void markPass(const vk::raii::CommandBuffer& cmd, std::uint32_t frame_index, const char* name);
    std::chrono::steady_clock::time_point pass_start_{};
    // Propiedades de los materiales con shader propio: 8 vec4 por material
    // dibujado, por frame en vuelo (set 0, binding 5). Se vacia al esperar la
    // fence del frame.
    static constexpr std::uint32_t kMaxSurfaceParamBlocks = 2048;
    std::vector<VulkanBuffer> surface_param_buffers_;
    std::vector<std::uint32_t> surface_param_used_;
    // Pipelines de los shaders de superficie del usuario (por id).
    std::vector<vk::raii::Pipeline> surface_pipelines_;
    vk::raii::DescriptorPool skin_pool_{nullptr};
    std::vector<vk::raii::DescriptorSet> skin_sets_;
    // Set 2 del vidrio, por frame (SkinnedPass::glassSetLayout).
    vk::raii::DescriptorPool glass_pool_{nullptr};
    std::vector<vk::raii::DescriptorSet> glass_sets_;

    // Huecos de sombra local que el frame anterior dibujaron algun actor: hay
    // que redibujarlos una vez mas cuando el actor sale, o su silueta se
    // quedaria congelada en la cache.
    std::array<bool, scene::kMaxShadowedSpotLights> spot_had_actor_{};
    std::array<bool, scene::kMaxShadowedPointLights> point_had_actor_{};

    // Reparto del frustum entre cascadas; se recalcula cada frame.
    scene::ShadowCascades cascades_{};
    // Actualizacion escalonada de las cascadas (ver updateUniforms): la que
    // tiene cada capa del mapa, con que camara se dibujo, y cuales se
    // redibujan este frame.
    std::array<scene::ShadowCascade, scene::kShadowCascadeCount> rendered_cascades_{};
    std::array<core::Vec3, scene::kShadowCascadeCount> rendered_cascade_camera_{};
    std::array<bool, scene::kShadowCascadeCount> cascade_due_{};
    bool cascades_valid_ = false;
    // El terreno o los voxeles cambiaron: los mapas de las luces locales se
    // redibujan (su cache solo vigila a los actores).
    bool local_static_dirty_ = true;
    void staticGeometryChanged() {
        cascades_valid_ = false;
        local_static_dirty_ = true;
    }
    std::uint64_t cascade_frame_ = 0;
    // Matrices, huecos y cache de las sombras de focos y luces puntuales.
    scene::LocalLightShadows local_shadows_{};

    vk::raii::DescriptorPool descriptor_pool_{nullptr};
    std::vector<vk::raii::DescriptorSet> lighting_sets_;
    std::vector<vk::raii::DescriptorSet> post_process_sets_;

    // Descriptores del SSAO (uno por frame: leen la camara de ese frame), del
    // bloom (uno por nivel) y de la composicion.
    vk::raii::DescriptorPool post_pool_{nullptr};
    std::vector<vk::raii::DescriptorSet> ssao_sets_;
    std::vector<vk::raii::DescriptorSet> volumetric_sets_;
    std::vector<vk::raii::DescriptorSet> bloom_down_sets_;
    std::vector<vk::raii::DescriptorSet> bloom_up_sets_;
    std::vector<vk::raii::DescriptorSet> composite_sets_;
    std::vector<vk::raii::DescriptorSet> light_shaft_sets_;
    std::vector<vk::raii::DescriptorSet> taa_sets_;
    std::vector<vk::raii::DescriptorSet> easu_sets_;
    std::vector<vk::raii::DescriptorSet> rcas_sets_;
    std::vector<vk::raii::DescriptorSet> ssgi_sets_;  // uno por frame
    std::vector<vk::raii::DescriptorSet> ssr_sets_;   // uno por frame
    std::vector<vk::raii::DescriptorSet> ssr_resolve_sets_;  // uno por frame
    // Filtro de la GI: pool propio. Temporal: uno por frame. A trous: [frame]
    // [captura de la sonda o no][iteracion] (la captura no toca la historia).
    vk::raii::DescriptorPool gi_filter_pool_{nullptr};
    std::vector<vk::raii::DescriptorSet> gi_temporal_sets_;
    std::vector<vk::raii::DescriptorSet> gi_atrous_sets_;
    std::vector<vk::raii::DescriptorSet> clouds_sets_;       // uno por frame
    std::vector<vk::raii::DescriptorSet> histogram_sets_;
    std::vector<vk::raii::DescriptorSet> exposure_average_sets_;

    // Exposicion manual (con la auto-exposicion apagada); sigue a la luz de
    // dia.
    float exposure_ = 1.0f;
    float displayed_exposure_ = 1.0f;
    float displayed_luminance_ = 0.0f;

    // Datos de este frame para el cielo, el IBL y los rayos de luz.
    GpuSkyPush sky_push_{};
    core::Vec3 ibl_light_radiance_{};
    core::Vec3 ibl_to_light_{0.0f, 1.0f, 0.0f};
    GpuLightShaftPush light_shaft_push_{};
    GpuCloudPush cloud_push_{};
    // Tiempo que llevan moviendose las nubes con el viento.
    float cloud_time_ = 0.0f;

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
    std::function<void(float, const char*)> loading_callback_;
    bool shadows_enabled_ = true;
    bool editor_helpers_ = true;
    bool presenting_ = true;
    // Segunda vista del editor (drawFrame sin presentar): como las caras de la
    // sonda, no toca las historias temporales (GI, reflejos, TAA, exposicion,
    // vectores de movimiento) de la vista principal.
    bool secondary_view_ = false;
    bool isolated() const { return capturing_ || secondary_view_; }
    // Cambio de ajustes graficos: se aplica al empezar el frame siguiente
    // (applyPendingResize), no a mitad (la interfaz ya apunta a las imagenes).
    bool settings_dirty_ = false;
    GraphicsSettings graphics_{};       // en uso (con la escala del presupuesto)
    GraphicsSettings user_graphics_{};  // los del usuario
    void applyEffectiveGraphics();
    std::uint32_t desiredShadowResolution() const;
    std::uint32_t desiredTextureSize() const;
    HardwareProfile hardware_{};
    FrameBudget budget_{};
    PostProcessSettings user_post_{};
    float applied_budget_scale_ = 1.0f;
    bool shadow_map_dirty_ = false;
    std::uint32_t culled_small_ = 0;
    std::chrono::steady_clock::time_point last_budget_time_{};
    vk::Extent2D render_extent_{0, 0};
    bool upscaling_ = false;  // hay pasada de escalado (TAA o FSR)
    bool taa_history_valid_ = false;
    std::uint32_t jitter_index_ = 0;
    core::Vec2 jitter_ndc_{};
    core::Mat4 motion_view_projection_ = core::Mat4::identity();  // sin jitter, frame anterior
    core::Mat4 taa_reproject_ = core::Mat4::identity();
    // Matrices del frame anterior (en el mundo) de cada actor, para los
    // vectores de movimiento: van tras las de este frame en el buffer.
    struct BoneRange {
        std::uint32_t model = 0;
        std::uint32_t start = 0;
        std::uint32_t count = 0;
    };
    std::vector<core::Mat4> last_world_bones_;
    std::vector<BoneRange> last_bone_ranges_;
    std::uint32_t motion_offset_ = 0;
    // La imagen que lee el post-proceso: la escalada o la de la escena.
    const VulkanImage& postSource() const { return upscaling_ ? upscaled_color_ : scene_color_; }
    void computeRenderExtent();
    void recordUpscalePass(const vk::raii::CommandBuffer& cmd);
    void recordOutputDepth(const vk::raii::CommandBuffer& cmd);
    bool output_depth_ready_ = false;
    bool sun_shadows_ = true;
    bool sunShadows() const { return shadows_enabled_ && sun_shadows_; }
    bool cascade_debug_ = false;
    bool clouds_enabled_ = true;
    bool environment_enabled_ = true;
    bool rt_enabled_ = true;
    bool occlusion_culling_enabled_ = true;
    // Los mapas locales se conservan entre frames (cache), asi que solo el
    // primero parte de un layout indefinido.
    bool local_shadow_layout_ready_ = false;
};

}  // namespace cramion::gfx

#endif  // CRAMION_VK_VULKAN_RENDERER_H
