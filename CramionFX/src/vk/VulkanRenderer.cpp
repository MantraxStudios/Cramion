#include "CramionFX/vk/VulkanCompat.h"
#include "CramionFX/vk/VulkanRenderer.h"

#include "CramionFX/asset/ImageFile.h"
#include "CramionFX/scene/Scene.h"
#include "CramionFX/vk/GpuTypes.h"

#include <stb_image.h>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <filesystem>
#include <cstdlib>
#include <fstream>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace cramion::gfx {

namespace {

// %LOCALAPPDATA%/Cramion/ShaderCache/<aplicacion>.bin (una por programa: el
// editor y cada juego exportado).
std::filesystem::path pipelineCacheFile(const char* app_name) {
    std::filesystem::path base;
    if (const char* local = std::getenv("LOCALAPPDATA"); local != nullptr && *local != '\0') {
        base = std::filesystem::path(local);
    } else {
        std::error_code error;
        base = std::filesystem::temp_directory_path(error);
    }
    std::string name = app_name != nullptr && *app_name != '\0' ? app_name : "Cramion";
    for (char& c : name) {
        if (c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' || c == '|' || c == '?' || c == '*') c = '_';
    }
    return base / "Cramion" / "ShaderCache" / std::filesystem::path(std::u8string(name.begin(), name.end())).concat(".bin");
}

}  // namespace
namespace {

using core::Vec3;
using core::Vec4;

// Barrera de layout para una imagen de color completa.
vk::ImageMemoryBarrier2 colorBarrier(vk::Image image, vk::ImageLayout old_layout,
                                     vk::ImageLayout new_layout,
                                     vk::PipelineStageFlags2 src_stage,
                                     vk::AccessFlags2 src_access,
                                     vk::PipelineStageFlags2 dst_stage,
                                     vk::AccessFlags2 dst_access) {
    vk::ImageMemoryBarrier2 barrier{};
    barrier.srcStageMask = src_stage;
    barrier.srcAccessMask = src_access;
    barrier.dstStageMask = dst_stage;
    barrier.dstAccessMask = dst_access;
    barrier.oldLayout = old_layout;
    barrier.newLayout = new_layout;
    barrier.image = image;
    barrier.subresourceRange = vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    return barrier;
}

// Balance de blancos como el de Unity (ColorUtils.ComputeColorBalance): la
// temperatura y el tinte (-100..100) desplazan el blanco de referencia D65 por
// el locus del iluminante estandar, y el resultado son los factores por los
// que se multiplica el color en espacio LMS (composite.frag). 0, 0 = (1,1,1).
Vec3 whiteBalanceLms(float temperature, float tint) {
    const float t1 = temperature / 65.0f;
    const float t2 = tint / 65.0f;
    const float x = 0.31271f - t1 * (t1 < 0.0f ? 0.1f : 0.05f);
    const float standard_illuminant_y = 2.87f * x - 3.0f * x * x - 0.27509507f;
    const float y = standard_illuminant_y + t2 * 0.05f;

    // CIE xy (Y = 1) -> XYZ -> LMS.
    const float big_x = x / y;
    const float big_z = (1.0f - x - y) / y;
    const float l = 0.7328f * big_x + 0.4296f - 0.1624f * big_z;
    const float m = -0.7036f * big_x + 1.6975f + 0.0061f * big_z;
    const float s = 0.0030f * big_x + 0.0136f + 0.9834f * big_z;

    // El blanco D65 en LMS, dividido por el nuevo.
    return Vec3{0.949237f / l, 1.03542f / m, 1.08728f / s};
}

// Barrera de una imagen de color que acaba de escribirse como destino y pasa a
// leerse como textura en el siguiente fragment shader.
vk::ImageMemoryBarrier2 writtenToSampled(vk::Image image) {
    return colorBarrier(image, vk::ImageLayout::eColorAttachmentOptimal,
                        vk::ImageLayout::eShaderReadOnlyOptimal,
                        vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                        vk::AccessFlagBits2::eColorAttachmentWrite,
                        vk::PipelineStageFlagBits2::eFragmentShader,
                        vk::AccessFlagBits2::eShaderSampledRead);
}

// Barrera de una imagen que se va a sobrescribir entera como destino. Lo que
// tuviera antes no importa (layout indefinido), pero hay que esperar a que la
// lectura anterior como textura haya terminado.
vk::ImageMemoryBarrier2 discardToAttachment(vk::Image image) {
    return colorBarrier(image, vk::ImageLayout::eUndefined,
                        vk::ImageLayout::eColorAttachmentOptimal,
                        vk::PipelineStageFlagBits2::eFragmentShader,
                        vk::AccessFlagBits2::eShaderSampledRead,
                        vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                        vk::AccessFlagBits2::eColorAttachmentWrite);
}

void pipelineBarrier(const vk::raii::CommandBuffer& cmd,
                     vk::ArrayProxy<const vk::ImageMemoryBarrier2> barriers) {
    vk::DependencyInfo dependency{};
    dependency.imageMemoryBarrierCount = barriers.size();
    dependency.pImageMemoryBarriers = barriers.data();
    compat::pipelineBarrier(cmd, dependency);
}

// Dibuja un triangulo a pantalla completa sobre `target` con el pipeline y el
// set indicados. `load` conserva el contenido (mezcla aditiva del bloom).
template <typename Push>
void drawFullscreen(const vk::raii::CommandBuffer& cmd, const FullscreenPass& pass,
                    const vk::raii::DescriptorSet* set, const VulkanImage& target,
                    const Push* push, bool load = false) {
    const vk::Extent2D extent = target.extent();

    vk::RenderingAttachmentInfo color_attachment{};
    color_attachment.imageView = *target.view();
    color_attachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
    color_attachment.loadOp = load ? vk::AttachmentLoadOp::eLoad : vk::AttachmentLoadOp::eDontCare;
    color_attachment.storeOp = vk::AttachmentStoreOp::eStore;

    vk::RenderingInfo rendering_info{};
    rendering_info.renderArea = vk::Rect2D{vk::Offset2D{0, 0}, extent};
    rendering_info.layerCount = 1;
    rendering_info.setColorAttachments(color_attachment);

    compat::beginRendering(cmd, rendering_info);
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *pass.pipeline());
    cmd.setViewport(0, vk::Viewport{0.0f, 0.0f, static_cast<float>(extent.width),
                                    static_cast<float>(extent.height), 0.0f, 1.0f});
    cmd.setScissor(0, vk::Rect2D{vk::Offset2D{0, 0}, extent});
    if (set != nullptr) {
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *pass.layout(), 0, **set,
                               nullptr);
    }
    if (push != nullptr) {
        cmd.pushConstants<Push>(*pass.layout(), vk::ShaderStageFlagBits::eFragment, 0, *push);
    }
    cmd.draw(3, 1, 0, 0);
    compat::endRendering(cmd);
}

// Barrera de memoria global entre etapas (buffers de la auto-exposicion).
void memoryBarrier(const vk::raii::CommandBuffer& cmd, vk::PipelineStageFlags2 src_stage,
                   vk::AccessFlags2 src_access, vk::PipelineStageFlags2 dst_stage,
                   vk::AccessFlags2 dst_access) {
    vk::MemoryBarrier2 barrier{};
    barrier.srcStageMask = src_stage;
    barrier.srcAccessMask = src_access;
    barrier.dstStageMask = dst_stage;
    barrier.dstAccessMask = dst_access;

    vk::DependencyInfo dependency{};
    dependency.setMemoryBarriers(barrier);
    compat::pipelineBarrier(cmd, dependency);
}

// Iluminancia con la que el sol y la luna iluminan la atmosfera. La del sol
// parte de pi veces la intensidad de la luz direccional de mediodia (Scene),
// la escala fisica, y se dobla para compensar la dispersion multiple que la
// LUT solo aproxima: con ella, la luz del cielo en sombra queda en ~1/6 de la
// del sol, como en un dia despejado real.
// La luna refleja una fraccion minima de la luz del sol.
constexpr float kSunIlluminance = 3.14159265f * 2.4f * 2.0f;
constexpr float kMoonIlluminance = 0.02f;

float smoothstepf(float edge0, float edge1, float x) {
    const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

vk::Extent2D bloomLevelExtent(vk::Extent2D screen, std::uint32_t level) {
    return vk::Extent2D{std::max(screen.width >> (level + 1), 1u),
                        std::max(screen.height >> (level + 1), 1u)};
}

core::Vec2 inverseExtent(vk::Extent2D extent) {
    return core::Vec2{1.0f / static_cast<float>(extent.width),
                      1.0f / static_cast<float>(extent.height)};
}

Vec4 toVec4(const Vec3& v, float w) {
    return Vec4{v.x, v.y, v.z, w};
}

// Sonda de reflexion. Cada cara cuesta casi un frame entero (sombras,
// geometria e iluminacion desde la sonda), asi que se recaptura poco:
//   - en cuanto cambia la luz de otra forma (color o intensidad del sol, del
//     ambiente, luces locales, sombras / GI / nubes encendidas o apagadas):
//     un cambio relativo de mas de kProbeLightChange. La GI lee de la sonda
//     lo que no esta en pantalla; sin esto no se enteraba hasta moverse,
//   - cuando la luz direccional gira mas de ~5 grados (sus sombras y su color
//     ya no cuadran; con el ciclo de dia, cada ~0.7 s),
//   - cuando la camara se ha alejado mas de kProbeMoveDistance y va despacio
//     (al pararse), o mas de kProbeFarDistance aunque siga corriendo: la
//     correccion de paralaje aguanta bastantes metros.
// Y solo una cara cada kProbeFaceInterval frames, para repartir el coste.
constexpr float kProbeMoveDistance = 4.0f;
constexpr float kProbeFarDistance = 15.0f;
constexpr float kProbeSlowSpeed = 1.5f;  // m/s
constexpr float kProbeLightCos = 0.99619f;
constexpr std::uint64_t kProbeFaceInterval = 2;
constexpr float kProbeLightChange = 0.03f;

// true si alguna entrada cambio mas de `tolerance` (relativo; absoluto cerca
// de cero) o cambio el numero de entradas (se anadio o quito una luz).
bool signatureChanged(const std::vector<float>& a, const std::vector<float>& b,
                      float tolerance) {
    if (a.size() != b.size()) {
        return true;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::abs(a[i] - b[i]) > tolerance * std::max(1.0f, std::abs(b[i]))) {
            return true;
        }
    }
    return false;
}
// Fundido de la sonda anterior a la nueva.
constexpr float kProbeFadeSeconds = 0.35f;
// Peso de lo acumulado en el filtro temporal de los reflejos de pantalla.
constexpr float kSsrHistoryWeight = 0.88f;
// Pasadas del filtro espacial de la luz rebotada (separacion 1, 2, 4, 8, 16).
constexpr std::uint32_t kGiAtrousIterations = 5;
// Filtro temporal de las sombras por rayos: peso minimo del rayo nuevo (la
// penumbra promedia ~10 frames; lo que cambia de verdad lo recorta la
// vecindad y no espera).
constexpr float kRtShadowMinAlpha = 0.1f;
// Pasadas del filtro espacial de los reflejos por rayos (separacion 1 y 2).
constexpr std::uint32_t kReflectionAtrousIterations = 2;
struct ReflectionAtrousPush {
    std::int32_t step = 1;
    std::int32_t last = 0;  // 1 = ultima pasada (alfa = confianza)
    std::int32_t pad[2] = {0, 0};
};

// Constantes de push del filtro de la GI (gi_temporal.comp, gi_atrous.comp).
struct GiTemporalPush {
    core::Mat4 previous_view_projection = core::Mat4::identity();
    Vec4 params{};  // x = hay historia valida
};
struct GiAtrousPush {
    std::int32_t step = 1;
    std::int32_t pad[3] = {0, 0, 0};
};

// Mapa de lluvia: resolucion (con ~180 m de escenario, texeles de ~9 cm).
constexpr std::uint32_t kRainMapSize = 2048;

// Nubes: fraccion del cielo cubierta y densidad (multiplica la extincion).
// Sombra de las nubes: 512^2 texeles sobre 16 km (31 m por texel; son sombras
// grandes y suaves).
constexpr std::uint32_t kCloudShadowSize = 512;
constexpr float kCloudShadowExtent = 16000.0f;

// Fraccion final de cada cascada en la que se mezcla con la siguiente, para
// que el salto de resolucion entre cascadas no se vea como una linea.
constexpr float kCascadeBlendBand = 0.12f;

// Barrera de layout para un rango de capas de un array de profundidad.
vk::ImageMemoryBarrier2 depthLayersBarrier(vk::Image image, std::uint32_t base_layer,
                                           std::uint32_t layer_count, vk::ImageLayout old_layout,
                                           vk::ImageLayout new_layout) {
    vk::ImageMemoryBarrier2 barrier{};
    if (new_layout == compat::depthAttachmentLayout()) {
        // Antes la leia la pasada de iluminacion (de este o del frame anterior).
        barrier.srcStageMask = vk::PipelineStageFlagBits2::eFragmentShader;
        barrier.srcAccessMask = vk::AccessFlagBits2::eShaderSampledRead;
        barrier.dstStageMask = vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                               vk::PipelineStageFlagBits2::eLateFragmentTests;
        barrier.dstAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentRead |
                                vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
    } else {
        barrier.srcStageMask = vk::PipelineStageFlagBits2::eLateFragmentTests;
        barrier.srcAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
        barrier.dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader;
        barrier.dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead;
    }
    barrier.oldLayout = old_layout;
    barrier.newLayout = new_layout;
    barrier.image = image;
    barrier.subresourceRange =
        vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eDepth, 0, 1, base_layer, layer_count};
    return barrier;
}

}  // namespace

VulkanRenderer::~VulkanRenderer() {
    shutdown();
}

// -----------------------------------------------------------------------------
// Inicializacion y apagado
// -----------------------------------------------------------------------------

void VulkanRenderer::initialize(const EngineInfo& info, NativeWindow window, std::uint32_t width,
                                std::uint32_t height) {
    if (initialized_) {
        return;
    }

    window_width_ = width;
    window_height_ = height;

    const auto report = [&](float fraction, const char* what) {
        if (loading_callback_) loading_callback_(fraction, what);
    };
    report(0.0f, "Iniciando Vulkan");
    // VR: el runtime de OpenXR dice que extensiones y que GPU (la del casco).
    xr_app_name_ = info.app_name != nullptr && *info.app_name != '\0' ? info.app_name : "Cramion";
    xr_runtime_choice_ = info.xr_runtime;
    std::vector<std::string> xr_instance_extensions;
    if (info.enable_xr) {
        report(0.0f, "Buscando el casco de VR");
        if (xr_.createInstance(xr_app_name_.c_str(), static_cast<xr::RuntimeChoice>(info.xr_runtime))) {
            xr_instance_extensions = xr_.requiredInstanceExtensions();
        }
    }
    // Las que piden SteamVR y Meta para compartir las imagenes del casco, si
    // existen (no cuestan nada): asi el casco se puede conectar despues, al
    // dar Play en VR, sin crear Vulkan otra vez.
    std::vector<std::string> xr_optional_instance_extensions;
    std::vector<std::string> xr_optional_device_extensions;
#if defined(_WIN32)
    if (xr::XrSystem::compiled()) {
        xr_optional_instance_extensions = {"VK_KHR_external_memory_capabilities", "VK_KHR_get_physical_device_properties2",
                                           "VK_KHR_external_fence_capabilities", "VK_KHR_external_semaphore_capabilities",
                                           "VK_NV_external_memory_capabilities"};
        xr_optional_device_extensions = {"VK_KHR_external_memory",           "VK_KHR_external_memory_win32",
                                         "VK_KHR_external_semaphore",        "VK_KHR_external_semaphore_win32",
                                         "VK_KHR_external_fence",            "VK_KHR_external_fence_win32",
                                         "VK_KHR_timeline_semaphore",        "VK_KHR_dedicated_allocation",
                                         "VK_KHR_get_memory_requirements2",  "VK_KHR_win32_keyed_mutex"};
    }
#endif
    // DLSS (NGX) pide sus extensiones (se anaden si existen).
    std::vector<std::string> instance_extensions = xr_instance_extensions;
    for (const std::string& name : DlssUpscaler::instanceExtensions()) {
        if (std::find(instance_extensions.begin(), instance_extensions.end(), name) == instance_extensions.end()) {
            instance_extensions.push_back(name);
        }
    }
    instance_.initialize(info, instance_extensions, xr_optional_instance_extensions);
    surface_.initialize(instance_, window);
    VkPhysicalDevice xr_gpu = VK_NULL_HANDLE;
    std::vector<std::string> xr_device_extensions;
    if (!xr_instance_extensions.empty()) {
        xr_gpu = xr_.physicalDevice(static_cast<VkInstance>(*instance_.handle()));
        if (xr_gpu != VK_NULL_HANDLE) xr_device_extensions = xr_.requiredDeviceExtensions();
    }
    std::vector<std::string> device_extensions = xr_device_extensions;
    for (const std::string& name : DlssUpscaler::deviceExtensions(static_cast<VkInstance>(*instance_.handle()))) {
        if (std::find(device_extensions.begin(), device_extensions.end(), name) == device_extensions.end()) {
            device_extensions.push_back(name);
        }
    }
    device_.initialize(instance_, surface_, xr_gpu, device_extensions, xr_optional_device_extensions);
    xr::XrSystem::setQueueMutex(&device_.queueMutex());
    {
        // DLSS: solo en GPUs NVIDIA RTX con un controlador que lo tenga.
        std::string dlss_error;
        const char* local = std::getenv("LOCALAPPDATA");
        const std::filesystem::path logs = local != nullptr ? std::filesystem::path(local) / "Cramion" / "Logs" / "NGX"
                                                            : std::filesystem::temp_directory_path() / "Cramion" / "NGX";
        // (No en el modo compatible: su dispositivo no lleva las extensiones de NGX.)
        if (DlssUpscaler::compiled() && !device_.compatMode() &&
            !dlss_.initialize(static_cast<VkInstance>(*instance_.handle()),
                              static_cast<VkPhysicalDevice>(*device_.physicalDevice()),
                              static_cast<VkDevice>(*device_.handle()), logs, dlss_error)) {
            std::cout << "[DLSS] No disponible: " << dlss_error << "\n";
        }
    }
    if (xr_gpu != VK_NULL_HANDLE && !info.xr_session) {
        // Vulkan ya esta en la GPU del casco y con sus extensiones: la sesion
        // se abrira con connectXrSession (Play on VR). Se suelta el runtime.
        std::cout << "[VR] Vulkan preparado para " << xr_.systemName() << " (" << xr_.runtimeName()
                  << "); la sesion se abre al jugar en VR\n";
        xr_.shutdown();
    } else if (xr_gpu != VK_NULL_HANDLE) {
        if (static_cast<VkPhysicalDevice>(*device_.physicalDevice()) != xr_gpu ||
            !xr_.createSession(static_cast<VkInstance>(*instance_.handle()), xr_gpu,
                               static_cast<VkDevice>(*device_.handle()), device_.queueFamilies().graphics, 0)) {
            std::cerr << "[VR] No se pudo empezar la sesion de VR: " << xr_.error() << "\n";
            xr_.shutdown();
        } else {
            createXrStaging();
        }
    } else if (!xr_instance_extensions.empty()) {
        xr_.shutdown();
    }

    // Shaders compilados para esta GPU (cache en disco) y cuantos pipelines
    // hubo la ultima vez (para el porcentaje).
    const std::filesystem::path cache_file = pipelineCacheFile(info.app_name);
    std::filesystem::path count_file = cache_file;
    count_file += ".count";
    std::uint32_t expected = 0;
    {
        std::ifstream in(count_file);
        in >> expected;
    }
    if (expected == 0) expected = 64;
    device_.loadPipelineCache(cache_file);
    const std::uint32_t first = device_.pipelinesCreated();
    device_.setPipelineCallback([&](std::uint32_t created) {
        const float done = std::min(static_cast<float>(created - first) / static_cast<float>(expected), 1.0f);
        report(0.05f + 0.93f * done, "Compilando shaders");
    });
    report(0.05f, "Compilando shaders");
    swapchain_.initialize(device_, surface_, width, height);
    computeRenderExtent();
    gbuffer_.create(device_, render_extent_);
    // Modo compatible (moviles): el escenario se recorta en la CPU
    // (drawCpuActors); cull.comp pide 5 storage buffers y los comandos
    // indirectos con firstInstance fallan en varios drivers de movil.
    if (!device_.compatMode()) gpu_culling_.create(device_);
    gpu_profiler_.create(device_, kMaxFramesInFlight);
    createRenderTargets();
    // Perfil de hardware: VRAM (el heap local mas grande) y tipo de GPU.
    {
        const vk::PhysicalDeviceProperties properties = device_.physicalDevice().getProperties();
        const vk::PhysicalDeviceMemoryProperties memory = device_.physicalDevice().getMemoryProperties();
        std::uint64_t vram = 0;
        for (std::uint32_t i = 0; i < memory.memoryHeapCount; ++i) {
            if (memory.memoryHeaps[i].flags & vk::MemoryHeapFlagBits::eDeviceLocal) {
                vram = std::max<std::uint64_t>(vram, memory.memoryHeaps[i].size);
            }
        }
        hardware_.gpu_name = device_.name();
        hardware_.vram_mb = vram >> 20;
        hardware_.integrated = properties.deviceType == vk::PhysicalDeviceType::eIntegratedGpu;
        hardware_.ray_tracing = device_.rayTracingSupported();
        hardware_.tier = tierFor(hardware_.vram_mb, hardware_.integrated);
        // Perfil forzado (pruebas de gama baja en un PC potente, o una GPU mal
        // detectada): CRAMION_HARDWARE_TIER=low|medium|high|ultra.
        if (const char* forced = std::getenv("CRAMION_HARDWARE_TIER")) {
            const std::string tier = forced;
            if (tier == "low") hardware_.tier = HardwareTier::Low;
            else if (tier == "medium") hardware_.tier = HardwareTier::Medium;
            else if (tier == "high") hardware_.tier = HardwareTier::High;
            else if (tier == "ultra") hardware_.tier = HardwareTier::Ultra;
        }
        budget_.setStartLevels(hardware_.tier);
        asset::setMaxTextureSize(desiredTextureSize());
        // Sin texturas BC (Mali, PowerVR...): los DDS se descomprimen al cargar.
        asset::setDecodeBlockCompressed(!device_.textureCompressionBcSupported());
        std::cout << "[Rendimiento] Perfil de hardware: " << tierName(hardware_.tier) << " (" << hardware_.gpu_name
                  << ", " << hardware_.vram_mb << " MB de VRAM" << (hardware_.integrated ? ", integrada" : "")
                  << ")\n";
    }
    shadow_map_.create(device_, desiredShadowResolution());
    shadow_cache_valid_ = {};
    shadow_cache_layout_ready_ = false;
    cascades_clear_ = false;
    local_shadow_maps_.create(device_);

    // La iluminacion escribe en HDR; la composicion lo lleva a 8 bits y el
    // FXAA es quien escribe en la swapchain.
    lighting_pass_.create(device_, kHdrFormat);
    post_process_pass_.create(device_, swapchain_.imageFormat());
    skinned_pass_.create(device_, gbuffer_, shadow_map_.format(), kHdrFormat);
    // Terrenos: comparten el set 0 de la geometria (camara, lluvia, decals).
    water_pass_.create(device_, skinned_pass_.frameSetLayout(), skinned_pass_.glassSetLayout(), kHdrFormat,
                       gbuffer_.depthFormat(), kMaxFramesInFlight);
    terrain_pass_.create(device_, skinned_pass_.frameSetLayout(), gbuffer_.colorFormats(), gbuffer_.depthFormat(),
                         shadow_map_.format(), kMaxFramesInFlight);
    voxel_pass_.create(device_, skinned_pass_.frameSetLayout(), gbuffer_.colorFormats(), gbuffer_.depthFormat(),
                       shadow_map_.format(), kMaxFramesInFlight);
    foliage_pass_.create(device_, skinned_pass_.frameSetLayout(), gbuffer_.colorFormats(), gbuffer_.depthFormat(),
                         shadow_map_.format(), kMaxFramesInFlight);
    // Liquidos: sombrean con el set del vidrio (escena detras, cielo, luces).
    // Modo compatible (moviles): lo que no cabe en los limites de la GPU no
    // se crea (y no se activa nunca). Los liquidos simulan con 21 storage
    // buffers en un mismo shader y grupos de 256 hilos.
    const vk::PhysicalDeviceLimits& limits = device_.limits();
    const bool compat = device_.compatMode();
    if (!compat || (limits.maxPerStageDescriptorStorageBuffers >= 21 && limits.maxComputeWorkGroupInvocations >= 256)) {
        fluid_pass_.create(device_, skinned_pass_.glassSetLayout(), kHdrFormat, kMaxFramesInFlight);
    } else {
        std::cout << "[Vulkan] Modo compatible: sin liquidos (la GPU no tiene los recursos por shader que piden)\n";
    }
    if (compat) {
        probe_enabled_ = false;          // VulkanRenderer::setReflectionProbeEnabled
        sky_occlusion_enabled_ = false;  // su iluminacion no la lee
    }

    {
        using Type = vk::DescriptorType;
        const std::array<Type, 3> ssao_bindings = {Type::eUniformBuffer,
                                                   Type::eCombinedImageSampler,
                                                   Type::eCombinedImageSampler};
        const std::array<Type, 1> one_texture = {Type::eCombinedImageSampler};
        const std::array<Type, 2> two_textures = {Type::eCombinedImageSampler,
                                                  Type::eCombinedImageSampler};

        FullscreenPassDesc ssao{};
        ssao.fragment_shader = "ssao.frag.spv";
        ssao.bindings = ssao_bindings;
        ssao.color_format = kSsaoFormat;
        // Profundidades exactas: interpolar el depth en los bordes de los
        // objetos inventaria superficies intermedias.
        ssao.filter = vk::Filter::eNearest;
        ssao_pass_.create(device_, ssao);

        // Luz volumetrica: camara, luces, cascadas + su mapa, la profundidad,
        // las sombras de focos y puntuales + sus matrices, y la irradiancia
        // del cielo (armonicos esfericos: la luz del cielo que dispersa el
        // polvo en todas direcciones).
        // Binding 9: los volumenes de niebla locales.
        const std::array<Type, 10> volumetric_bindings = {
            Type::eUniformBuffer,        Type::eUniformBuffer,        Type::eUniformBuffer,
            Type::eCombinedImageSampler, Type::eCombinedImageSampler, Type::eCombinedImageSampler,
            Type::eCombinedImageSampler, Type::eUniformBuffer,        Type::eStorageBuffer,
            Type::eUniformBuffer};
        FullscreenPassDesc volumetric{};
        volumetric.fragment_shader = "volumetric.frag.spv";
        volumetric.bindings = volumetric_bindings;
        volumetric.push_constant_size = sizeof(GpuVolumetricPush);
        volumetric.color_format = kHdrFormat;
        volumetric.filter = vk::Filter::eNearest;
        volumetric_pass_.create(device_, volumetric);

        FullscreenPassDesc bloom_down{};
        bloom_down.fragment_shader = "bloom_down.frag.spv";
        bloom_down.bindings = one_texture;
        bloom_down.push_constant_size = sizeof(GpuBloomPush);
        bloom_down.color_format = kHdrFormat;
        bloom_down_pass_.create(device_, bloom_down);

        FullscreenPassDesc bloom_up = bloom_down;
        bloom_up.fragment_shader = "bloom_up.frag.spv";
        bloom_up.additive_blend = true;
        bloom_up_pass_.create(device_, bloom_up);

        // Escena, bloom, rayos de luz, exposicion y ajustes (GpuCompositeSettings).
        const std::array<Type, 5> composite_bindings = {
            Type::eCombinedImageSampler, Type::eCombinedImageSampler,
            Type::eCombinedImageSampler, Type::eStorageBuffer, Type::eUniformBuffer};
        FullscreenPassDesc composite{};
        composite.fragment_shader = "composite.frag.spv";
        composite.bindings = composite_bindings;
        composite.color_format = kLdrFormat;
        composite_pass_.create(device_, composite);

        // Contorno de seleccion: lee la mascara y se mezcla (alfa) sobre la
        // imagen compuesta.
        FullscreenPassDesc outline{};
        outline.fragment_shader = "outline.frag.spv";
        outline.bindings = one_texture;
        outline.color_format = kLdrFormat;
        outline.alpha_blend = true;
        outline.filter = vk::Filter::eNearest;
        outline_pass_.create(device_, outline);
        // Gizmos del editor en 3D, con prueba contra el depth de la escena.
        overlay_pass_.create(device_, kLdrFormat, gbuffer_.depthFormat(), kMaxFramesInFlight);
        // UI en el mundo: sus texturas y sus paneles (igual: tras el tono).
        world_ui_pass_.create(device_, kLdrFormat, gbuffer_.depthFormat(), kMaxFramesInFlight);
        // Particulas: sobre la imagen HDR, antes del bloom.
        particle_pass_.create(device_, kHdrFormat, gbuffer_.depthFormat(), kMaxFramesInFlight);
        // VFX Graph: particulas simuladas en la GPU (igual: sobre la HDR).
        // VFX Graph: 7 storage buffers y 17 texturas en un mismo shader.
        if (!compat || (limits.maxPerStageDescriptorStorageBuffers >= 7 && limits.maxPerStageDescriptorSamplers >= 17 &&
                        limits.maxPerStageDescriptorSampledImages >= 17)) {
            vfx_pass_.create(device_, kHdrFormat, gbuffer_.depthFormat(), kMaxFramesInFlight);
        } else {
            std::cout << "[Vulkan] Modo compatible: sin VFX Graph (la GPU no tiene los recursos por shader que pide)\n";
        }
        // Sprites y tilemaps 2D: igual, sobre la HDR con el depth de la escena.
        sprite_pass_.create(device_, kHdrFormat, gbuffer_.depthFormat(), kMaxFramesInFlight);
        // Fuego y humo volumetricos (y el mapa de quemado de la geometria).
        fire_pass_.create(device_, kHdrFormat, kMaxFramesInFlight);
        // Lluvia, nieve y rayos del sistema de ambiente (igual: sobre la HDR).
        precipitation_pass_.create(device_, kHdrFormat, gbuffer_.depthFormat(), kMaxFramesInFlight);

        FullscreenPassDesc sky{};
        sky.fragment_shader = "sky_lut.frag.spv";
        sky.push_constant_size = sizeof(GpuSkyPush);
        sky.color_format = kHdrFormat;
        sky_lut_pass_.create(device_, sky);

        const std::array<Type, 4> ssgi_bindings = {
            Type::eUniformBuffer, Type::eCombinedImageSampler, Type::eCombinedImageSampler,
            Type::eCombinedImageSampler};
        FullscreenPassDesc ssgi{};
        ssgi.fragment_shader = "ssgi.frag.spv";
        ssgi.bindings = ssgi_bindings;
        ssgi.push_constant_size = sizeof(GpuSsgiPush);
        ssgi.color_format = kHdrFormat;

        // La GI ademas lee los dos cubos de la sonda (lo que no esta en
        // pantalla). Los demas pases que copian esta descripcion no.
        const std::array<Type, 6> gi_bindings = {
            Type::eUniformBuffer,        Type::eCombinedImageSampler,
            Type::eCombinedImageSampler, Type::eCombinedImageSampler,
            Type::eCombinedImageSampler, Type::eCombinedImageSampler};
        FullscreenPassDesc gi = ssgi;
        gi.bindings = gi_bindings;
        ssgi_pass_.create(device_, gi);
        createBakedGi();  // GI horneada (VulkanRendererBaked.cpp)

        // Mismos recursos que la GI (camara, profundidad, normales e imagen
        // anterior), pero a resolucion completa.
        FullscreenPassDesc ssr = ssgi;
        ssr.fragment_shader = "ssr.frag.spv";
        ssr_pass_.create(device_, ssr);

        // Filtro temporal del SSR: camara + profundidad + reflejos de este
        // frame + historia.
        FullscreenPassDesc ssr_resolve = ssgi;
        ssr_resolve.fragment_shader = "ssr_resolve.frag.spv";
        ssr_resolve_pass_.create(device_, ssr_resolve);

        // Filtro SVGF de la GI: acumulacion temporal y a trous.
        const std::array<Type, 8> temporal_bindings = {
            Type::eUniformBuffer,        Type::eCombinedImageSampler, Type::eCombinedImageSampler,
            Type::eCombinedImageSampler, Type::eCombinedImageSampler, Type::eStorageImage,
            Type::eStorageImage,         Type::eStorageImage};
        ComputePassDesc temporal{};
        temporal.shader = "gi_temporal.comp.spv";
        temporal.bindings = temporal_bindings;
        temporal.push_constant_size = sizeof(GiTemporalPush);
        gi_temporal_pass_.create(device_, temporal);

        const std::array<Type, 7> atrous_bindings = {
            Type::eUniformBuffer,        Type::eCombinedImageSampler, Type::eCombinedImageSampler,
            Type::eCombinedImageSampler, Type::eCombinedImageSampler, Type::eStorageImage,
            Type::eStorageImage};
        ComputePassDesc atrous{};
        atrous.shader = "gi_atrous.comp.spv";
        atrous.bindings = atrous_bindings;
        atrous.push_constant_size = sizeof(GiAtrousPush);
        gi_atrous_pass_.create(device_, atrous);

        // Filtros del trazado de rayos (solo con rayos por hardware): sombras
        // (camara, profundidad, normales, mascara de este frame, historia ->
        // acumulada) y reflejos (temporal y a trous).
        if (device_.rayTracingSupported()) {
            const std::array<Type, 6> shadow_temporal_bindings = {
                Type::eUniformBuffer,        Type::eCombinedImageSampler, Type::eCombinedImageSampler,
                Type::eCombinedImageSampler, Type::eCombinedImageSampler, Type::eStorageImage};
            ComputePassDesc shadow_temporal{};
            shadow_temporal.shader = "rt_shadow_temporal.comp.spv";
            shadow_temporal.bindings = shadow_temporal_bindings;
            shadow_temporal.push_constant_size = sizeof(GiTemporalPush);
            rt_shadow_temporal_pass_.create(device_, shadow_temporal);

            // Camara, profundidad, normales, reflejo de este frame, historia
            // (color y momentos) -> acumulado y momentos.
            const std::array<Type, 8> reflection_temporal_bindings = {
                Type::eUniformBuffer,        Type::eCombinedImageSampler, Type::eCombinedImageSampler,
                Type::eCombinedImageSampler, Type::eCombinedImageSampler, Type::eCombinedImageSampler,
                Type::eStorageImage,         Type::eStorageImage};
            ComputePassDesc reflection_temporal{};
            reflection_temporal.shader = "rt_reflection_temporal.comp.spv";
            reflection_temporal.bindings = reflection_temporal_bindings;
            reflection_temporal.push_constant_size = sizeof(GiTemporalPush);
            rt_reflection_temporal_pass_.create(device_, reflection_temporal);

            const std::array<Type, 5> reflection_atrous_bindings = {
                Type::eUniformBuffer, Type::eCombinedImageSampler, Type::eCombinedImageSampler,
                Type::eCombinedImageSampler, Type::eStorageImage};
            ComputePassDesc reflection_atrous{};
            reflection_atrous.shader = "rt_reflection_atrous.comp.spv";
            reflection_atrous.bindings = reflection_atrous_bindings;
            reflection_atrous.push_constant_size = sizeof(ReflectionAtrousPush);
            rt_reflection_atrous_pass_.create(device_, reflection_atrous);
        }

        // Nubes: camara + ruido 3D + LUT del cielo (ambiente).
        const std::array<Type, 3> cloud_bindings = {Type::eUniformBuffer,
                                                    Type::eCombinedImageSampler,
                                                    Type::eCombinedImageSampler};
        FullscreenPassDesc clouds{};
        clouds.fragment_shader = "clouds.frag.spv";
        clouds.bindings = cloud_bindings;
        clouds.push_constant_size = sizeof(GpuCloudPush);
        clouds.color_format = kHdrFormat;
        clouds_pass_.create(device_, clouds);
        // Sombra de las nubes: el mismo shader (modo mapa de sombra) y los
        // mismos recursos, sobre un cuadrado del suelo.
        FullscreenPassDesc cloud_shadow = clouds;
        cloud_shadow.color_format = vk::Format::eR16Sfloat;
        cloud_shadow_pass_.create(device_, cloud_shadow);

        FullscreenPassDesc shafts{};
        shafts.fragment_shader = "light_shafts.frag.spv";
        shafts.bindings = two_textures;
        shafts.push_constant_size = sizeof(GpuLightShaftPush);
        shafts.color_format = kHdrFormat;
        light_shaft_pass_.create(device_, shafts);

        const std::array<Type, 3> camera_fx_bindings = {Type::eCombinedImageSampler, Type::eCombinedImageSampler,
                                                        Type::eCombinedImageSampler};
        FullscreenPassDesc camera_fx{};
        camera_fx.fragment_shader = "camera_fx.frag.spv";
        camera_fx.bindings = camera_fx_bindings;
        camera_fx.push_constant_size = sizeof(GpuCameraFxPush);
        camera_fx.color_format = kHdrFormat;
        camera_fx_pass_.create(device_, camera_fx);

        const std::array<Type, 4> taa_bindings = {Type::eCombinedImageSampler, Type::eCombinedImageSampler,
                                                  Type::eCombinedImageSampler, Type::eCombinedImageSampler};
        FullscreenPassDesc taa{};
        taa.fragment_shader = "taa.frag.spv";
        taa.bindings = taa_bindings;
        taa.push_constant_size = sizeof(GpuTaaPush);
        taa.color_format = kHdrFormat;
        taa_pass_.create(device_, taa);

        const std::array<Type, 2> motion_bindings = {Type::eCombinedImageSampler, Type::eCombinedImageSampler};
        FullscreenPassDesc motion{};
        motion.fragment_shader = "upscaler_motion.frag.spv";
        motion.bindings = motion_bindings;
        motion.push_constant_size = sizeof(core::Mat4);
        motion.color_format = GBuffer::kVelocityFormat;
        upscaler_motion_pass_.create(device_, motion);

        FullscreenPassDesc easu{};
        easu.fragment_shader = "fsr_easu.frag.spv";
        easu.bindings = one_texture;
        easu.push_constant_size = sizeof(GpuEasuPush);
        easu.color_format = kHdrFormat;
        easu_pass_.create(device_, easu);

        FullscreenPassDesc rcas{};
        rcas.fragment_shader = "fsr_rcas.frag.spv";
        rcas.bindings = one_texture;
        rcas.push_constant_size = sizeof(GpuRcasPush);
        rcas.color_format = kHdrFormat;
        rcas.filter = vk::Filter::eNearest;
        rcas_pass_.create(device_, rcas);

        const std::array<Type, 2> histogram_bindings = {Type::eCombinedImageSampler,
                                                        Type::eStorageBuffer};
        ComputePassDesc histogram{};
        histogram.shader = "exposure_histogram.comp.spv";
        histogram.bindings = histogram_bindings;
        histogram_pass_.create(device_, histogram);

        const std::array<Type, 2> average_bindings = {Type::eStorageBuffer,
                                                      Type::eStorageBuffer};
        ComputePassDesc average{};
        average.shader = "exposure_average.comp.spv";
        average.bindings = average_bindings;
        average.push_constant_size = sizeof(GpuExposurePush);
        exposure_average_pass_.create(device_, average);
    }

    sky_lut_.create(device_, kSkyLutExtent, kHdrFormat,
                    vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled,
                    vk::ImageAspectFlagBits::eColor);
    ibl_probe_.create(device_, sky_lut_);
    reflection_probe_.create(device_);
    cloud_noise_.create(device_);
    cloud_shadow_image_.create(device_, vk::Extent2D{kCloudShadowSize, kCloudShadowSize}, vk::Format::eR16Sfloat,
                               vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled,
                               vk::ImageAspectFlagBits::eColor);
    terrain_pass_.setKeepCpuCopy(device_.rayTracingSupported());
    foliage_pass_.setKeepCpuCopy(device_.rayTracingSupported());
    if (device_.rayTracingSupported()) {
        ray_tracing_.create(device_);
    }

    // --- Mapa de lluvia (mismo formato que las sombras: lo dibuja su pipeline) ---
    rain_map_.create(device_, vk::Extent2D{kRainMapSize, kRainMapSize}, shadow_map_.format(),
                     vk::ImageUsageFlagBits::eDepthStencilAttachment |
                         vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
                     vk::ImageAspectFlagBits::eDepth);
    {
        // Hasta dibujarlo: vacio (profundidad maxima) y ya como textura.
        const vk::Image image = *rain_map_.handle();
        device_.submitOneTime([&](const vk::raii::CommandBuffer& cmd) {
            const vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1};
            vk::ImageMemoryBarrier2 barrier{};
            barrier.dstStageMask = vk::PipelineStageFlagBits2::eTransfer;
            barrier.dstAccessMask = vk::AccessFlagBits2::eTransferWrite;
            barrier.oldLayout = vk::ImageLayout::eUndefined;
            barrier.newLayout = vk::ImageLayout::eTransferDstOptimal;
            barrier.image = image;
            barrier.subresourceRange = range;
            pipelineBarrier(cmd, barrier);
            cmd.clearDepthStencilImage(image, vk::ImageLayout::eTransferDstOptimal,
                                       vk::ClearDepthStencilValue{1.0f, 0}, range);
            barrier.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
            barrier.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
            barrier.dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader;
            barrier.dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead;
            barrier.oldLayout = vk::ImageLayout::eTransferDstOptimal;
            barrier.newLayout = compat::depthReadOnlyLayout();
            pipelineBarrier(cmd, barrier);
        });

        vk::SamplerCreateInfo sampler_info{};
        sampler_info.magFilter = vk::Filter::eNearest;
        sampler_info.minFilter = vk::Filter::eNearest;
        sampler_info.mipmapMode = vk::SamplerMipmapMode::eNearest;
        sampler_info.addressModeU = vk::SamplerAddressMode::eClampToEdge;
        sampler_info.addressModeV = vk::SamplerAddressMode::eClampToEdge;
        sampler_info.addressModeW = vk::SamplerAddressMode::eClampToEdge;
        rain_sampler_ = vk::raii::Sampler(device_.handle(), sampler_info);
    }

    // Oclusion del cielo desde arriba y autoenfoque suave.
    createSkyMap();
    createDofFocus();

    // Decals: textura blanca en las ranuras libres y muestreo con mips.
    {
        const std::uint8_t white[4] = {255, 255, 255, 255};
        decal_white_.create(device_, 1, 1, white);
        vk::SamplerCreateInfo sampler_info{};
        sampler_info.magFilter = vk::Filter::eLinear;
        sampler_info.minFilter = vk::Filter::eLinear;
        sampler_info.mipmapMode = vk::SamplerMipmapMode::eLinear;
        sampler_info.addressModeU = vk::SamplerAddressMode::eClampToEdge;
        sampler_info.addressModeV = vk::SamplerAddressMode::eClampToEdge;
        sampler_info.addressModeW = vk::SamplerAddressMode::eClampToEdge;
        sampler_info.maxLod = VK_LOD_CLAMP_NONE;
        decal_sampler_ = vk::raii::Sampler(device_.handle(), sampler_info);
    }

    createUniformBuffers();
    createDescriptors();
    createCommandObjects();
    createSyncObjects();

    // Los pipelines de este arranque quedan en disco para el siguiente.
    device_.setPipelineCallback(nullptr);
    std::ofstream(count_file) << std::max<std::uint32_t>(device_.pipelinesCreated() - first, 1);
    device_.savePipelineCache();
    report(1.0f, "Listo");
    loading_callback_ = nullptr;

    initialized_ = true;
    std::cout << "[Vulkan] Renderizador diferido listo\n";
}

void VulkanRenderer::shutdown() {
    if (!initialized_) {
        return;
    }

    finishModelJobs();
    device_.waitIdle();

    // VR: la sesion usa el dispositivo; se cierra antes.
    xr_.shutdown();
    // FSR 3 y DLSS tambien (sus recursos son del dispositivo).
    fsr3_.destroy();
    dlss_.shutdown();
    upscaler_motion_ = VulkanImage{};
    for (VulkanImage& image : xr_staging_) image = VulkanImage{};

    skinned_models_.clear();
    retired_models_.clear();
    ray_pinned_models_.clear();
    render_textures_.clear();
    retired_render_textures_.clear();
    actor_draws_.clear();
    gpu_culling_.destroy();
    gpu_profiler_.destroy();

    render_finished_.clear();
    in_flight_fences_.clear();
    image_available_.clear();
    command_buffers_.clear();
    command_pool_ = nullptr;

    exposure_average_sets_.clear();
    histogram_sets_.clear();
    light_shaft_sets_.clear();
    camera_fx_sets_.clear();
    taa_sets_.clear();
    upscaler_motion_sets_.clear();
    easu_sets_.clear();
    rcas_sets_.clear();
    outline_sets_.clear();
    clouds_sets_.clear();
    gi_atrous_sets_.clear();
    gi_temporal_sets_.clear();
    gi_filter_pool_ = nullptr;
    ssr_resolve_sets_.clear();
    ssr_sets_.clear();
    ssgi_sets_.clear();
    composite_sets_.clear();
    bloom_up_sets_.clear();
    bloom_down_sets_.clear();
    ssao_sets_.clear();
    // Antes que su pool: si no, su destructor los libera sobre un pool (y un
    // dispositivo) ya destruidos y el motor revienta al cerrar.
    volumetric_sets_.clear();
    post_pool_ = nullptr;
    post_process_sets_.clear();
    lighting_sets_.clear();
    descriptor_pool_ = nullptr;
    mesh_draw_sets_.clear();
    mesh_draw_pool_ = nullptr;
    mesh_draw_buffers_.clear();
    skin_sets_.clear();
    skin_pool_ = nullptr;
    glass_sets_.clear();
    glass_pool_ = nullptr;

    for (VulkanBuffer& buffer : bone_buffers_) {
        buffer.destroy();
    }
    bone_buffers_.clear();
    for (VulkanBuffer& buffer : surface_param_buffers_) {
        buffer.destroy();
    }
    surface_param_buffers_.clear();
    for (VulkanBuffer& buffer : exposure_readback_) {
        buffer.destroy();
    }
    exposure_readback_.clear();
    exposure_buffer_.destroy();
    histogram_buffer_.destroy();
    for (VulkanBuffer& buffer : local_shadow_buffers_) {
        buffer.destroy();
    }
    for (VulkanBuffer& buffer : shadow_buffers_) {
        buffer.destroy();
    }
    for (VulkanBuffer& buffer : light_buffers_) {
        buffer.destroy();
    }
    for (VulkanBuffer& buffer : fog_volume_buffers_) {
        buffer.destroy();
    }
    fog_volume_buffers_.clear();
    for (VulkanBuffer& buffer : camera_buffers_) {
        buffer.destroy();
    }
    for (VulkanBuffer& buffer : composite_buffers_) {
        buffer.destroy();
    }
    composite_buffers_.clear();
    local_shadow_buffers_.clear();
    shadow_buffers_.clear();
    light_buffers_.clear();
    camera_buffers_.clear();

    exposure_average_pass_.destroy();
    histogram_pass_.destroy();
    light_shaft_pass_.destroy();
    camera_fx_pass_.destroy();
    taa_pass_.destroy();
    upscaler_motion_pass_.destroy();
    easu_pass_.destroy();
    rcas_pass_.destroy();
    clouds_pass_.destroy();
    // Sin esto su pipeline se liberaba en el destructor, con el dispositivo
    // ya destruido: crash en el driver al cerrar el editor o el juego.
    cloud_shadow_pass_.destroy();
    rt_shadow_temporal_sets_.clear();
    rt_reflection_temporal_sets_.clear();
    rt_reflection_atrous_sets_.clear();
    rt_filter_pool_ = nullptr;
    rt_shadow_temporal_pass_.destroy();
    rt_reflection_temporal_pass_.destroy();
    rt_reflection_atrous_pass_.destroy();
    gi_atrous_pass_.destroy();
    gi_temporal_pass_.destroy();
    ssr_resolve_pass_.destroy();
    ssr_pass_.destroy();
    ssgi_pass_.destroy();
    destroyBakedGi();
    sky_lut_pass_.destroy();
    composite_pass_.destroy();
    outline_pass_.destroy();
    overlay_pass_.destroy();
    world_ui_pass_.destroy();
    particle_pass_.destroy();
    vfx_pass_.destroy();
    sprite_pass_.destroy();
    fire_pass_.destroy();
    precipitation_pass_.destroy();
    bloom_up_pass_.destroy();
    bloom_down_pass_.destroy();
    ssao_pass_.destroy();
    volumetric_pass_.destroy();
    terrain_pass_.destroy();
    voxel_pass_.destroy();
    foliage_pass_.destroy();
    fluid_pass_.destroy();
    water_pass_.destroy();
    surface_pipelines_.clear();
    skinned_pass_.destroy();
    post_process_pass_.destroy();
    lighting_pass_.destroy();
    local_shadow_maps_.destroy();
    shadow_map_.destroy();
    for (VulkanImage& level : bloom_levels_) {
        level.destroy();
    }
    light_shafts_.destroy();
    camera_fx_source_.destroy();
    gi_image_.destroy();
    gi_raw_.destroy();
    path_tracing_accumulation_.destroy();
    gi_history_.destroy();
    gi_temporal_.destroy();
    gi_variance_.destroy();
    gi_moments_.destroy();
    gi_moments_history_.destroy();
    for (std::uint32_t i = 0; i < 2; ++i) {
        gi_filter_[i].destroy();
        gi_filter_variance_[i].destroy();
    }
    ssr_image_.destroy();
    ssr_raw_.destroy();
    rt_shadow_mask_.destroy();
    rt_shadow_accum_.destroy();
    rt_shadow_history_.destroy();
    rt_reflection_temporal_.destroy();
    rt_reflection_moments_.destroy();
    rt_reflection_history_.destroy();
    rt_reflection_moments_history_.destroy();
    rt_reflection_filter_.destroy();
    ssr_history_.destroy();
    probe_capture_.destroy();
    for (VulkanBuffer& buffer : weather_buffers_) {
        buffer.destroy();
    }
    weather_buffers_.clear();
    for (VulkanTexture& texture : decal_textures_) {
        texture.destroy();
    }
    decal_texture_paths_ = {};
    decal_white_.destroy();
    decal_sampler_ = nullptr;
    decals_.clear();
    rain_sampler_ = nullptr;
    rain_map_.destroy();
    rain_map_ready_ = false;
    sky_map_terrain_.destroy();
    sky_map_scene_.destroy();
    sky_map_drawn_ = false;
    dof_focus_sets_.clear();
    dof_focus_pool_ = nullptr;
    for (VulkanBuffer& buffer : dof_focus_buffers_) buffer.destroy();
    dof_focus_buffers_.clear();
    dof_focus_pass_.destroy();
    ray_tracing_.destroy();
    environment_.destroy();
    clouds_image_.destroy();
    cloud_shadow_image_.destroy();
    cloud_noise_.destroy();
    reflection_probe_.destroy();
    ibl_probe_.destroy();
    sky_lut_.destroy();
    ssao_image_.destroy();
    volumetric_image_.destroy();
    ldr_color_.destroy();
    outline_mask_.destroy();
    pick_ids_.destroy();
    ui_textures_.clear();
    for (VulkanImage& image : view_images_) image.destroy();
    for (VulkanBuffer& buffer : pick_buffers_) buffer.destroy();
    pick_buffers_.clear();
    glass_source_.destroy();
    water_depth_.destroy();
    upscale_target_.destroy();
    upscaled_color_.destroy();
    taa_history_.destroy();
    xr_eye1_.taa_history.destroy();
    xr_eye1_.ssr_history.destroy();
    xr_eye1_.gi_history.destroy();
    xr_eye1_.gi_moments_history.destroy();
    xr_eye1_.rt_shadow_history.destroy();
    xr_eye1_.rt_reflection_history.destroy();
    xr_eye1_.rt_reflection_moments_history.destroy();
    output_depth_.destroy();
    scene_color_.destroy();
    gbuffer_.destroy();

    swapchain_.shutdown();
    xr::XrSystem::setQueueMutex(nullptr);
    device_.shutdown();
    surface_.shutdown();
    instance_.shutdown();

    current_frame_ = 0;
    frame_count_ = 0;
    triangle_count_ = 0;
    framebuffer_resized_ = false;
    local_shadow_layout_ready_ = false;
    local_shadows_.invalidate();
    probe_face_ = -1;
    probe_ready_ = false;
    probe_fade_ = 1.0f;
    initialized_ = false;

    std::cout << "[Vulkan] Recursos liberados\n";
}

void VulkanRenderer::createUniformBuffers() {
    camera_buffers_.resize(kMaxFramesInFlight);
    composite_buffers_.resize(kMaxFramesInFlight);
    light_buffers_.resize(kMaxFramesInFlight);
    shadow_buffers_.resize(kMaxFramesInFlight);
    local_shadow_buffers_.resize(kMaxFramesInFlight);

    // Memoria visible y coherente: se escribe directamente cada frame sin
    // necesidad de staging ni de invalidar rangos.
    const vk::MemoryPropertyFlags host_visible =
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;

    for (std::uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
        camera_buffers_[i].create(device_, sizeof(GpuCamera),
                                  vk::BufferUsageFlagBits::eUniformBuffer, host_visible);
        composite_buffers_[i].create(device_, sizeof(GpuCompositeSettings),
                                     vk::BufferUsageFlagBits::eUniformBuffer, host_visible);
        light_buffers_[i].create(device_, sizeof(GpuLights),
                                 vk::BufferUsageFlagBits::eUniformBuffer, host_visible);
        shadow_buffers_[i].create(device_, sizeof(GpuShadows),
                                  vk::BufferUsageFlagBits::eUniformBuffer, host_visible);
        local_shadow_buffers_[i].create(device_, sizeof(GpuLocalShadows),
                                        vk::BufferUsageFlagBits::eUniformBuffer, host_visible);
    }
    fog_volume_buffers_.resize(kMaxFramesInFlight);
    for (VulkanBuffer& buffer : fog_volume_buffers_) {
        buffer.create(device_, sizeof(GpuFogVolumes), vk::BufferUsageFlagBits::eUniformBuffer, host_visible);
        const GpuFogVolumes none{};
        buffer.write(&none, sizeof(none));
    }
    weather_buffers_.resize(kMaxFramesInFlight);
    for (VulkanBuffer& buffer : weather_buffers_) {
        buffer.create(device_, sizeof(GpuWeather), vk::BufferUsageFlagBits::eUniformBuffer,
                      host_visible);
        const GpuWeather dry{};
        buffer.write(&dry, sizeof(dry));
    }

    // --- Auto-exposicion ---
    // Histograma y estado en memoria de la GPU (los atomicos sobre memoria
    // visible desde la CPU irian por el bus PCIe). Ambos empiezan a cero: el
    // estado con `initialized` = 0 hace que el primer frame no tenga
    // transicion.
    const std::array<std::uint32_t, 256> empty_histogram{};
    histogram_buffer_ = VulkanBuffer::createDeviceLocal(
        device_, empty_histogram.data(), sizeof(empty_histogram),
        vk::BufferUsageFlagBits::eStorageBuffer);

    const GpuExposureState initial_exposure{};
    exposure_buffer_ = VulkanBuffer::createDeviceLocal(
        device_, &initial_exposure, sizeof(initial_exposure),
        vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferSrc);

    exposure_readback_.resize(kMaxFramesInFlight);
    for (VulkanBuffer& buffer : exposure_readback_) {
        buffer.create(device_, sizeof(GpuExposureState), vk::BufferUsageFlagBits::eTransferDst,
                      host_visible);
        // Hasta la primera copia no hay nada que mostrar.
        const GpuExposureState empty{};
        buffer.write(&empty, sizeof(empty));
    }

    // Capacidad inicial de los huesos; ensureBoneCapacity() la amplia si la
    // escena necesita mas.
    bone_buffers_.resize(kMaxFramesInFlight);
    for (VulkanBuffer& buffer : bone_buffers_) {
        buffer.create(device_, sizeof(core::Mat4) * 256, vk::BufferUsageFlagBits::eStorageBuffer,
                      host_visible);
    }
    surface_param_buffers_.resize(kMaxFramesInFlight);
    surface_param_used_.assign(kMaxFramesInFlight, 0);
    for (VulkanBuffer& buffer : surface_param_buffers_) {
        buffer.create(device_, sizeof(core::Vec4) * SkinnedPass::kSurfaceParamCount * kMaxSurfaceParamBlocks,
                      vk::BufferUsageFlagBits::eStorageBuffer, host_visible);
    }
}

void VulkanRenderer::writeDecalTextureDescriptors() {
    std::array<vk::DescriptorImageInfo, kMaxDecalTextures> infos{};
    const std::uint32_t slots = decalTextureSlots(device_.compatMode());
    for (std::uint32_t slot = 0; slot < slots; ++slot) {
        const bool loaded = !decal_texture_paths_[slot].empty();
        infos[slot].sampler = *decal_sampler_;
        infos[slot].imageView = loaded ? *decal_textures_[slot].view() : *decal_white_.view();
        infos[slot].imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
    }
    for (std::uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
        vk::WriteDescriptorSet write{};
        write.dstSet = *skin_sets_[i];
        write.dstBinding = 4;
        write.dstArrayElement = 0;
        write.descriptorType = vk::DescriptorType::eCombinedImageSampler;
        write.setImageInfo(infos);
        write.descriptorCount = slots;
        device_.handle().updateDescriptorSets(write, nullptr);
    }
}

int VulkanRenderer::loadDecalTexture(const std::filesystem::path& file) {
    if (!initialized_ || file.empty()) {
        return -1;
    }
    std::error_code error;
    const std::filesystem::path key = std::filesystem::weakly_canonical(file, error);
    for (std::uint32_t slot = 0; slot < kMaxDecalTextures; ++slot) {
        if (decal_texture_paths_[slot] == key) return static_cast<int>(slot);
    }
    const std::uint32_t slots = decalTextureSlots(device_.compatMode());
    std::uint32_t slot = 0;
    while (slot < slots && !decal_texture_paths_[slot].empty()) ++slot;
    if (slot == slots) {
        std::cerr << "[Vulkan] Sin ranuras para mas texturas de decal (maximo " << slots << ")\n";
        return -1;
    }
    int width = 0;
    int height = 0;
    int channels = 0;
    FILE* handle = nullptr;
#if defined(_WIN32)
    if (_wfopen_s(&handle, file.wstring().c_str(), L"rb") != 0) handle = nullptr;
#else
    handle = std::fopen(file.c_str(), "rb");
#endif
    stbi_uc* pixels = handle ? stbi_load_from_file(handle, &width, &height, &channels, 4) : nullptr;
    if (handle) fclose(handle);
    if (pixels == nullptr) {
        std::cerr << "[Vulkan] No se pudo leer la textura de decal " << file.string() << "\n";
        return -1;
    }
    // La ranura puede estar en uso en un frame en vuelo: se espera a la GPU
    // (cargar una textura es raro, no cada frame).
    waitIdle();
    decal_textures_[slot].destroy();
    decal_textures_[slot].create(device_, static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height),
                                 pixels);
    stbi_image_free(pixels);
    decal_texture_paths_[slot] = key;
    writeDecalTextureDescriptors();
    std::cout << "[Vulkan] Textura de decal " << file.filename().string() << " en la ranura " << slot << "\n";
    return static_cast<int>(slot);
}

void VulkanRenderer::createDescriptors() {
    // Por frame: camara (geometria), camara + luces + cascadas + sombras
    // locales (iluminacion), y los muestreadores del G-buffer mas los mapas de
    // sombras.
    std::array<vk::DescriptorPoolSize, 3> pool_sizes{};
    pool_sizes[0].type = vk::DescriptorType::eUniformBuffer;
    pool_sizes[0].descriptorCount = kMaxFramesInFlight * 5;
    // Irradiancia del IBL (armonicos esfericos), una por frame.
    pool_sizes[2].type = vk::DescriptorType::eStorageBuffer;
    pool_sizes[2].descriptorCount = kMaxFramesInFlight;
    pool_sizes[1].type = vk::DescriptorType::eCombinedImageSampler;
    // Destinos de color del G-buffer + profundidad + cascadas + focos +
    // puntuales + SSAO + cielo. +1 mas por frame para la imagen que lee el
    // FXAA.
    // (+ entorno y LUT de la BRDF del IBL, + la GI, + el SSR, + los dos cubos
    // de la sonda.)
    // (+ las nubes, + el mapa de entorno HDR.)
    // (+ la luz volumetrica, + la sombra de las nubes.)
    // (+ las cascadas sin comparacion para las sombras suaves.)
    // (+ las sombras por rayos de las luces locales.)
    // (+ los dos mapas de la oclusion del cielo vista desde arriba.)
    pool_sizes[1].descriptorCount = kMaxFramesInFlight * (GBuffer::kColorAttachmentCount + 21);

    vk::DescriptorPoolCreateInfo pool_info{};
    pool_info.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    pool_info.maxSets = kMaxFramesInFlight * 2;
    pool_info.setPoolSizes(pool_sizes);

    descriptor_pool_ = vk::raii::DescriptorPool(device_.handle(), pool_info);

    const std::vector<vk::DescriptorSetLayout> lighting_layouts(
        kMaxFramesInFlight, *lighting_pass_.descriptorSetLayout());

    vk::DescriptorSetAllocateInfo lighting_alloc{};
    lighting_alloc.descriptorPool = *descriptor_pool_;
    lighting_alloc.setSetLayouts(lighting_layouts);
    lighting_sets_ = vk::raii::DescriptorSets(device_.handle(), lighting_alloc);

    const std::vector<vk::DescriptorSetLayout> post_process_layouts(
        kMaxFramesInFlight, *post_process_pass_.descriptorSetLayout());

    vk::DescriptorSetAllocateInfo post_process_alloc{};
    post_process_alloc.descriptorPool = *descriptor_pool_;
    post_process_alloc.setSetLayouts(post_process_layouts);
    post_process_sets_ = vk::raii::DescriptorSets(device_.handle(), post_process_alloc);

    // --- Modelos con esqueleto: camara + storage buffer de huesos ---
    // Pool propio: el set de huesos se reescribe cuando su buffer crece.
    // (+ la lluvia: mapa y parametros.)
    const std::array<vk::DescriptorPoolSize, 3> skin_sizes = {
        vk::DescriptorPoolSize{vk::DescriptorType::eUniformBuffer, kMaxFramesInFlight * 2},
        vk::DescriptorPoolSize{vk::DescriptorType::eStorageBuffer, kMaxFramesInFlight * 2},
        vk::DescriptorPoolSize{vk::DescriptorType::eCombinedImageSampler,
                               kMaxFramesInFlight * (1 + kMaxDecalTextures + 1)}};  // + el mapa de quemado

    vk::DescriptorPoolCreateInfo skin_pool_info{};
    skin_pool_info.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    skin_pool_info.maxSets = kMaxFramesInFlight;
    skin_pool_info.setPoolSizes(skin_sizes);
    skin_pool_ = vk::raii::DescriptorPool(device_.handle(), skin_pool_info);

    const std::vector<vk::DescriptorSetLayout> skin_layouts(kMaxFramesInFlight,
                                                            *skinned_pass_.frameSetLayout());
    vk::DescriptorSetAllocateInfo skin_alloc{};
    skin_alloc.descriptorPool = *skin_pool_;
    skin_alloc.setSetLayouts(skin_layouts);
    skin_sets_ = vk::raii::DescriptorSets(device_.handle(), skin_alloc);

    for (std::uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
        vk::DescriptorBufferInfo camera_info{};
        camera_info.buffer = *camera_buffers_[i].handle();
        camera_info.range = sizeof(GpuCamera);

        vk::WriteDescriptorSet write{};
        write.dstSet = *skin_sets_[i];
        write.dstBinding = 0;
        write.descriptorType = vk::DescriptorType::eUniformBuffer;
        write.setBufferInfo(camera_info);
        device_.handle().updateDescriptorSets(write, nullptr);

        writeBoneDescriptor(i);

        // Lluvia: mapa desde arriba + parametros.
        vk::DescriptorImageInfo rain_info{};
        rain_info.sampler = *rain_sampler_;
        rain_info.imageView = *rain_map_.view();
        rain_info.imageLayout = compat::depthReadOnlyLayout();
        vk::WriteDescriptorSet rain_write{};
        rain_write.dstSet = *skin_sets_[i];
        rain_write.dstBinding = 2;
        rain_write.descriptorType = vk::DescriptorType::eCombinedImageSampler;
        rain_write.setImageInfo(rain_info);
        device_.handle().updateDescriptorSets(rain_write, nullptr);

        vk::DescriptorBufferInfo weather_info{};
        weather_info.buffer = *weather_buffers_[i].handle();
        weather_info.range = sizeof(GpuWeather);
        vk::WriteDescriptorSet weather_write{};
        weather_write.dstSet = *skin_sets_[i];
        weather_write.dstBinding = 3;
        weather_write.descriptorType = vk::DescriptorType::eUniformBuffer;
        weather_write.setBufferInfo(weather_info);
        device_.handle().updateDescriptorSets(weather_write, nullptr);

        // Propiedades de los shaders de superficie del usuario.
        vk::DescriptorBufferInfo surface_info{};
        surface_info.buffer = *surface_param_buffers_[i].handle();
        surface_info.range = VK_WHOLE_SIZE;
        vk::WriteDescriptorSet surface_write{};
        surface_write.dstSet = *skin_sets_[i];
        surface_write.dstBinding = 5;
        surface_write.descriptorType = vk::DescriptorType::eStorageBuffer;
        surface_write.setBufferInfo(surface_info);
        device_.handle().updateDescriptorSets(surface_write, nullptr);

        // Mapa de quemado de las zonas de fuego (siempre la misma imagen).
        vk::DescriptorImageInfo burn_info{};
        burn_info.sampler = *fire_pass_.mapSampler();
        burn_info.imageView = *fire_pass_.mapView();
        burn_info.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
        vk::WriteDescriptorSet burn_write{};
        burn_write.dstSet = *skin_sets_[i];
        burn_write.dstBinding = 6;
        burn_write.descriptorType = vk::DescriptorType::eCombinedImageSampler;
        burn_write.setImageInfo(burn_info);
        device_.handle().updateDescriptorSets(burn_write, nullptr);
    }
    writeDecalTextureDescriptors();

    // --- Post-proceso: SSAO (por frame), bloom (por nivel) y composicion ---
    constexpr std::uint32_t kBloomSets = kBloomLevels + (kBloomLevels - 1);
    // Texturas: SSAO (2 por frame), bloom (1 por set), composicion (3), rayos
    // de luz (2) e histograma (1). Storage: histograma (1), promedio (2) y
    // composicion (1).
    // Y la GI, el SSR y su filtro temporal: camara + 3 texturas por frame
    // cada uno (el filtro del SSR y el TAA, otra vez para el ojo derecho de VR).
    const std::array<vk::DescriptorPoolSize, 3> post_sizes = {
    // Y las nubes: camara + 2 texturas por frame. Y el filtro de la GI:
    // camara + 3 texturas por frame.
        // (+ la luz volumetrica: 4 buffers y 4 texturas por frame.)
        // (+ la composicion por frame: 3 texturas, su exposicion y sus ajustes.)
        vk::DescriptorPoolSize{vk::DescriptorType::eUniformBuffer, kMaxFramesInFlight * 13},
        vk::DescriptorPoolSize{vk::DescriptorType::eCombinedImageSampler,
                               kMaxFramesInFlight * 28 + kBloomSets + 2 + 1 + 1 + 6 + 3 + 4},
        vk::DescriptorPoolSize{vk::DescriptorType::eStorageBuffer, 3 + kMaxFramesInFlight * 2}};

    vk::DescriptorPoolCreateInfo post_pool_info{};
    post_pool_info.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    post_pool_info.maxSets = kMaxFramesInFlight * 9 + kBloomSets + 5 + 3 + 1 + 1;
    post_pool_info.setPoolSizes(post_sizes);
    post_pool_ = vk::raii::DescriptorPool(device_.handle(), post_pool_info);

    const auto allocate = [&](const auto& pass, std::uint32_t count) {
        const std::vector<vk::DescriptorSetLayout> layouts(count, *pass.descriptorSetLayout());
        vk::DescriptorSetAllocateInfo alloc{};
        alloc.descriptorPool = *post_pool_;
        alloc.setSetLayouts(layouts);
        return vk::raii::DescriptorSets(device_.handle(), alloc);
    };
    ssao_sets_ = allocate(ssao_pass_, kMaxFramesInFlight);
    volumetric_sets_ = allocate(volumetric_pass_, kMaxFramesInFlight);
    bloom_down_sets_ = allocate(bloom_down_pass_, kBloomLevels);
    bloom_up_sets_ = allocate(bloom_up_pass_, kBloomLevels - 1);
    composite_sets_ = allocate(composite_pass_, kMaxFramesInFlight);
    light_shaft_sets_ = allocate(light_shaft_pass_, 1);
    camera_fx_sets_ = allocate(camera_fx_pass_, 1);
    taa_sets_ = allocate(taa_pass_, 2);  // uno por ojo (VR)
    upscaler_motion_sets_ = allocate(upscaler_motion_pass_, 1);
    easu_sets_ = allocate(easu_pass_, 1);
    rcas_sets_ = allocate(rcas_pass_, 1);
    outline_sets_ = allocate(outline_pass_, 1);
    ssgi_sets_ = allocate(ssgi_pass_, kMaxFramesInFlight);
    ssr_sets_ = allocate(ssr_pass_, kMaxFramesInFlight);
    ssr_resolve_sets_ = allocate(ssr_resolve_pass_, kMaxFramesInFlight * 2);  // por frame y ojo
    clouds_sets_ = allocate(clouds_pass_, kMaxFramesInFlight);

    // --- Filtro de la GI ---
    {
        // Por frame: la vista y el ojo derecho de VR (temporal); la vista, las
        // caras de la sonda y el ojo derecho (cadenas del filtro espacial).
        constexpr std::uint32_t kTemporalSets = kMaxFramesInFlight * 2;
        constexpr std::uint32_t kAtrousSets = kMaxFramesInFlight * 3 * kGiAtrousIterations;
        const std::array<vk::DescriptorPoolSize, 3> sizes = {
            vk::DescriptorPoolSize{vk::DescriptorType::eUniformBuffer, kTemporalSets + kAtrousSets},
            vk::DescriptorPoolSize{vk::DescriptorType::eCombinedImageSampler,
                                   (kTemporalSets + kAtrousSets) * 4},
            vk::DescriptorPoolSize{vk::DescriptorType::eStorageImage,
                                   kTemporalSets * 3 + kAtrousSets * 2}};
        vk::DescriptorPoolCreateInfo info{};
        info.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
        info.maxSets = kTemporalSets + kAtrousSets;
        info.setPoolSizes(sizes);
        gi_filter_pool_ = vk::raii::DescriptorPool(device_.handle(), info);

        const auto allocate_gi = [&](const ComputePass& pass, std::uint32_t count) {
            const std::vector<vk::DescriptorSetLayout> layouts(count, *pass.descriptorSetLayout());
            vk::DescriptorSetAllocateInfo alloc{};
            alloc.descriptorPool = *gi_filter_pool_;
            alloc.setSetLayouts(layouts);
            return vk::raii::DescriptorSets(device_.handle(), alloc);
        };
        gi_temporal_sets_ = allocate_gi(gi_temporal_pass_, kTemporalSets);
        gi_atrous_sets_ = allocate_gi(gi_atrous_pass_, kAtrousSets);
    }

    // --- Filtros de las sombras y los reflejos por rayos ---
    // Temporales: por frame y ojo de VR. A trous de los reflejos: por frame y
    // pasada (la historia ya quedo en la temporal: no hay variantes por ojo).
    if (device_.rayTracingSupported()) {
        constexpr std::uint32_t kTemporalSets = kMaxFramesInFlight * 2;
        constexpr std::uint32_t kAtrousSets = kMaxFramesInFlight * kReflectionAtrousIterations;
        constexpr std::uint32_t kSets = kTemporalSets * 2 + kAtrousSets;
        const std::array<vk::DescriptorPoolSize, 3> sizes = {
            vk::DescriptorPoolSize{vk::DescriptorType::eUniformBuffer, kSets},
            vk::DescriptorPoolSize{vk::DescriptorType::eCombinedImageSampler, kSets * 5},
            vk::DescriptorPoolSize{vk::DescriptorType::eStorageImage, kSets * 2}};
        vk::DescriptorPoolCreateInfo info{};
        info.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
        info.maxSets = kSets;
        info.setPoolSizes(sizes);
        rt_filter_pool_ = vk::raii::DescriptorPool(device_.handle(), info);
        const auto allocate_rt = [&](const ComputePass& pass, std::uint32_t count) {
            const std::vector<vk::DescriptorSetLayout> layouts(count, *pass.descriptorSetLayout());
            vk::DescriptorSetAllocateInfo alloc{};
            alloc.descriptorPool = *rt_filter_pool_;
            alloc.setSetLayouts(layouts);
            return vk::raii::DescriptorSets(device_.handle(), alloc);
        };
        rt_shadow_temporal_sets_ = allocate_rt(rt_shadow_temporal_pass_, kTemporalSets);
        rt_reflection_temporal_sets_ = allocate_rt(rt_reflection_temporal_pass_, kTemporalSets);
        rt_reflection_atrous_sets_ = allocate_rt(rt_reflection_atrous_pass_, kAtrousSets);
    }
    histogram_sets_ = allocate(histogram_pass_, 1);
    exposure_average_sets_ = allocate(exposure_average_pass_, 1);

    // --- Vidrio: camara, luces, cascadas + seis texturas, por frame ---
    {
        const std::array<vk::DescriptorPoolSize, 2> glass_sizes = {
            vk::DescriptorPoolSize{vk::DescriptorType::eUniformBuffer, kMaxFramesInFlight * 3},
            vk::DescriptorPoolSize{vk::DescriptorType::eCombinedImageSampler,
                                   kMaxFramesInFlight * 7}};
        vk::DescriptorPoolCreateInfo glass_pool_info{};
        glass_pool_info.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
        glass_pool_info.maxSets = kMaxFramesInFlight;
        glass_pool_info.setPoolSizes(glass_sizes);
        glass_pool_ = vk::raii::DescriptorPool(device_.handle(), glass_pool_info);

        const std::vector<vk::DescriptorSetLayout> glass_layouts(
            kMaxFramesInFlight, *skinned_pass_.glassSetLayout());
        vk::DescriptorSetAllocateInfo glass_alloc{};
        glass_alloc.descriptorPool = *glass_pool_;
        glass_alloc.setSetLayouts(glass_layouts);
        glass_sets_ = vk::raii::DescriptorSets(device_.handle(), glass_alloc);
    }

    updateLightingDescriptors();
    updatePostDescriptors();
}

void VulkanRenderer::writeBoneDescriptor(std::uint32_t frame_index) {
    // Todo el buffer: el shader lo ve como un array de longitud variable.
    vk::DescriptorBufferInfo bones_info{};
    bones_info.buffer = *bone_buffers_[frame_index].handle();
    bones_info.range = VK_WHOLE_SIZE;

    vk::WriteDescriptorSet write{};
    write.dstSet = *skin_sets_[frame_index];
    write.dstBinding = 1;
    write.descriptorType = vk::DescriptorType::eStorageBuffer;
    write.setBufferInfo(bones_info);
    device_.handle().updateDescriptorSets(write, nullptr);
}

void VulkanRenderer::ensureBoneCapacity(std::uint32_t frame_index, std::size_t bone_count) {
    const std::size_t capacity = bone_buffers_[frame_index].size() / sizeof(core::Mat4);
    if (bone_count <= capacity) {
        return;
    }

    // Se dobla (como un std::vector) para no recrearlo cada vez que aparece un
    // actor mas. Es seguro: la fence de este frame garantiza que la GPU ya no
    // usa ni este buffer ni el descriptor set que apunta a el.
    const std::size_t new_capacity = std::max(bone_count, capacity * 2);
    bone_buffers_[frame_index].create(
        device_, sizeof(core::Mat4) * new_capacity, vk::BufferUsageFlagBits::eStorageBuffer,
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
    writeBoneDescriptor(frame_index);

    std::cout << "[Vulkan] Buffer de huesos del frame " << frame_index << " ampliado a "
              << new_capacity << " matrices\n";
}

void VulkanRenderer::updateActors(const scene::Scene& scene, std::uint32_t frame_index) {
    // Lo del frame anterior, para saber que se movio.
    previous_actor_motion_.resize(actor_draws_.size());
    for (std::size_t i = 0; i < actor_draws_.size(); ++i) {
        previous_actor_motion_[i] =
            ActorMotion{actor_draws_[i].transform, actor_draws_[i].bounds_center, actor_draws_[i].bounds_radius,
                        actor_draws_[i].cast_shadows, actor_draws_[i].lod, actor_draws_[i].model};
    }
    const std::size_t previous_count = actor_draws_.size();
    moved_spheres_.clear();
    moved_sphere_animated_.clear();

    actor_draws_.clear();
    bone_staging_.clear();
    submesh_bounds_.clear();
    lod_bounds_.clear();
    lod_triangles_ = 0;
    lod_actors_ = 0;
    culled_small_ = 0;
    gpu_clusters_.clear();

    // Para los LODs: pixeles de pantalla por unidad del mundo a distancia 1
    // (la proyeccion de Vulkan lleva la Y invertida en m[1][1]).
    // En VR, la cabeza (la misma para los dos ojos: no cambian de LOD por
    // separado); la camara de la escena puede ser la del editor, lejos.
    const scene::Camera& lod_camera = xr_view_frame_ && shadow_camera_override_ != nullptr ? *shadow_camera_override_
                                      : xr_view_frame_ && camera_override_ != nullptr     ? *camera_override_
                                                                                          : scene.camera();
    const core::Vec3 camera_position = lod_camera.position();
    // Con la resolucion interna: si el presupuesto la baja, los LODs tambien
    // pueden ser mas simples (hay menos pixeles que llenar).
    const float pixels_per_unit = std::abs(lod_camera.projection().m[1][1]) * 0.5f *
                                  static_cast<float>(std::max(render_extent_.height, 1u));
    // Lo que chooseLod() deja desviarse la malla, en metros por metro de
    // distancia (sin LODs, nada).
    lod_error_per_meter_ = post_.lods && pixels_per_unit > 0.0f
                               ? std::max(post_.lod_pixel_error, 0.05f) / pixels_per_unit
                               : 0.0f;
    const float cull_pixels = budget_.cullPixels();
    draw_batches_.clear();
    batch_lookup_.clear();

    for (const scene::Actor& actor : scene.actors()) {
        if (actor.model >= skinned_models_.size()) {
            continue;  // Modelo sin subir a la GPU.
        }
        // Lo grande que se ve (pixeles): decide si el modelo sigue en la GPU.
        if (model_streaming_) {
            if (model_pixels_.size() <= actor.model) model_pixels_.resize(actor.model + 1, 0.0f);
            const float distance = std::max(core::length(actor.bounds_center - camera_position), 0.01f);
            const float pixels = actor.bounds_radius * pixels_per_unit / distance;
            model_pixels_[actor.model] = std::max(model_pixels_[actor.model], pixels);
        }
        if (!modelResident(actor.model)) {
            continue;  // Fuera de la GPU (streaming): vuelve en cuanto se acerque.
        }

        const std::vector<core::Mat4>& bones = actor.animator.boneMatrices();
        ActorDraw draw{};
        draw.scene_actor = static_cast<std::uint32_t>(&actor - scene.actors().data());
        draw.model = actor.model;
        draw.transform = actor.transform;
        draw.bone_offset = static_cast<std::uint32_t>(bone_staging_.size());
        draw.bounds_center = actor.bounds_center;
        draw.bounds_radius = actor.bounds_radius;
        draw.cast_shadows = actor.cast_shadows;
        draw.shadows_only = actor.shadows_only;
        // Dibujando una Render Texture: lo que la muestra (la pantalla) no lo
        // ve su camara; si no, sale dentro de si misma una y otra vez (tunel).
        // Su sombra sigue.
        if (render_texture_target_ >= 0) {
            for (const SkinnedModel::RenderTextureRef& ref : skinned_models_[actor.model].renderTextureRefs()) {
                if (ref.texture == render_texture_target_) {
                    draw.shadows_only = true;
                    break;
                }
            }
        }

        // Con un solo hueso todo el modelo se mueve con el: la caja de cada
        // submalla en el mundo es la de reposo por (actor x hueso).
        const SkinnedModel& model = skinned_models_[actor.model];
        if (model.rigid() && bones.size() == 1) {
            draw.per_submesh = true;
            draw.first_bounds = static_cast<std::uint32_t>(submesh_bounds_.size());
            const core::Mat4 to_world = actor.transform * bones[0];
            for (const asset::SubMesh& submesh : model.submeshes()) {
                submesh_bounds_.push_back(core::transformAabb(
                    to_world, core::Aabb{submesh.bounds_min, submesh.bounds_max}));
            }

            draw.max_scale = std::max({core::length(Vec3{to_world.m[0][0], to_world.m[0][1], to_world.m[0][2]}),
                                       core::length(Vec3{to_world.m[1][0], to_world.m[1][1], to_world.m[1][2]}),
                                       core::length(Vec3{to_world.m[2][0], to_world.m[2][1], to_world.m[2][2]})});
            // LOD de la camara (las cascadas eligen el suyo por texel).
            draw.lod = chooseLod(model, to_world, draw.bounds_center, draw.bounds_radius, camera_position,
                                 pixels_per_unit);
            // Mas pequeno que un pixel (o lo que diga el presupuesto): la
            // camara no lo dibuja; su sombra sigue.
            const float camera_distance = core::length(draw.bounds_center - camera_position);
            const bool tiny = camera_distance > draw.bounds_radius &&
                              draw.bounds_radius * pixels_per_unit / camera_distance < cull_pixels;
            if (tiny) ++culled_small_;
            const std::vector<asset::SubMesh>& lod_submeshes = model.lodSubmeshes(draw.lod);
            if (draw.lod > 0) {
                ++lod_actors_;
                draw.first_lod_bounds = static_cast<std::uint32_t>(lod_bounds_.size());
                for (const asset::SubMesh& submesh : lod_submeshes) {
                    lod_bounds_.push_back(core::transformAabb(
                        to_world, core::Aabb{submesh.bounds_min, submesh.bounds_max}));
                }
            }
            for (const asset::SubMesh& submesh : lod_submeshes) lod_triangles_ += submesh.index_count / 3;

            // Sus clusteres los descarta la GPU: caja en el mundo y lote
            // (modelo x material, compartido con los demas actores). La
            // matriz de mundo va detras de sus huesos.
            const std::uint32_t instance = draw.bone_offset + static_cast<std::uint32_t>(bones.size());
            for (std::uint32_t i = 0; i < lod_submeshes.size(); ++i) {
                const std::uint32_t group = model.lodSubmeshGroup(draw.lod, i);
                if (group == SkinnedModel::kNoGroup || draw.shadows_only || tiny) {
                    continue;  // Transparente, solo sombras o diminuto: la camara no lo ve.
                }
                const asset::SubMesh& submesh = lod_submeshes[i];
                const core::Aabb& box = draw.lod > 0 ? lod_bounds_[draw.first_lod_bounds + i]
                                                     : submesh_bounds_[draw.first_bounds + i];
                GpuCluster cluster{};
                // El relieve teselado sube la superficie: la caja crece lo que
                // sube, para que el culling no la descarte asomando.
                const SkinnedModel::Material& cluster_material = model.materials()[model.drawGroups()[group].material];
                const float relief = tessellatedMaterial(cluster_material) ? cluster_material.emissive.w : 0.0f;
                const Vec3 grow{relief, relief, relief};
                cluster.bounds_min = toVec4(box.min - grow, 0.0f);
                cluster.bounds_max = toVec4(box.max + grow, 0.0f);
                cluster.first_index = submesh.first_index;
                cluster.index_count = submesh.index_count;
                const std::uint64_t key = (static_cast<std::uint64_t>(actor.model) << 32) | group;
                const auto [it, inserted] =
                    batch_lookup_.try_emplace(key, static_cast<std::uint32_t>(draw_batches_.size()));
                if (inserted) {
                    // Por mesh shaders si el modelo tiene meshlets, es una malla
                    // cerrada y el material usa el shader estandar (los del
                    // usuario son vertex shaders). Solo cerradas: lo que ganan
                    // los meshlets en la camara es descartar los que miran hacia
                    // atras; en mallas abiertas (hierba, follaje) el culling de
                    // clusters ya hace el resto y el task shader solo sumaba
                    // (medido: 1.94 ms frente a 1.71 en la hierba de minecraft).
                    const SkinnedModel::Material& batch_material = model.materials()[model.drawGroups()[group].material];
                    const bool mesh = meshGeometryAllowed() && model.hasMeshlets() && model.closed(draw.lod) &&
                                      batch_material.surface_shader < 0 && model.maxClusterMeshlets() <= 256 &&
                                      !tessellatedMaterial(batch_material);
                    draw_batches_.push_back(DrawBatch{actor.model, group, 0, 0, mesh, mesh && model.closed(draw.lod)});
                }
                DrawBatch& cluster_batch = draw_batches_[it->second];
                ++cluster_batch.capacity;
                cluster_batch.closed = cluster_batch.closed && model.closed(draw.lod);
                cluster.group = it->second;
                cluster.instance = instance;
                if (cluster_batch.mesh) {
                    const SkinnedModel::MeshletRange range = model.meshletRanges(draw.lod)[i];
                    cluster.first_meshlet = range.first;
                    cluster.meshlet_count = range.count;
                    cluster.mesh = 1;
                }
                gpu_clusters_.push_back(cluster);
            }
        }

        actor_draws_.push_back(draw);

        // Movimiento: transform distinto del frame anterior, o animado con
        // esqueleto (se deforma cada frame).
        const std::size_t index = actor_draws_.size() - 1;
        const bool animated = !draw.per_submesh;
        if (index < previous_actor_motion_.size()) {
            const ActorMotion& before = previous_actor_motion_[index];
            // (Las cascadas ya no se redibujan por turno si su encuadre no
            // cambia: lo que cambie aqui tiene que llegar como movimiento.)
            if (animated || before.cast_shadows != draw.cast_shadows || before.model != draw.model ||
                std::memcmp(&before.transform, &draw.transform, sizeof(core::Mat4)) != 0) {
                moved_spheres_.push_back(toVec4(before.center, before.radius));
                moved_spheres_.push_back(toVec4(draw.bounds_center, draw.bounds_radius));
                // Los animados no van en la cache de las cascadas (se dibujan
                // encima cada frame): moverlos no la invalida.
                moved_sphere_animated_.push_back(animated);
                moved_sphere_animated_.push_back(animated);
            }
        }
        bone_staging_.insert(bone_staging_.end(), bones.begin(), bones.end());
        if (draw.per_submesh) {
            bone_staging_.push_back(actor.transform * bones[0]);  // la instancia
        }
        actor_draws_.back().bone_entries =
            static_cast<std::uint32_t>(bones.size()) + (draw.per_submesh ? 1u : 0u);
    }

    // --- Vectores de movimiento: cada matriz del frame anterior, en el mundo ---
    // Van detras de las de este frame (el shader suma motion_offset_). Un
    // actor nuevo (o con otro modelo) no se movio: usa las de ahora.
    {
        const std::size_t count = bone_staging_.size();
        std::vector<core::Mat4> world_now(count);
        std::vector<BoneRange> ranges_now(actor_draws_.size());
        for (std::size_t i = 0; i < actor_draws_.size(); ++i) {
            const ActorDraw& draw = actor_draws_[i];
            const std::uint32_t bones_only = draw.bone_entries - (draw.per_submesh ? 1u : 0u);
            for (std::uint32_t k = 0; k < bones_only; ++k) {
                world_now[draw.bone_offset + k] = draw.transform * bone_staging_[draw.bone_offset + k];
            }
            if (draw.per_submesh) {
                world_now[draw.bone_offset + bones_only] = bone_staging_[draw.bone_offset + bones_only];
            }
            ranges_now[i] = BoneRange{draw.model, draw.bone_offset, draw.bone_entries};
        }
        bone_staging_.resize(count * 2);
        for (std::size_t i = 0; i < actor_draws_.size(); ++i) {
            const BoneRange& now = ranges_now[i];
            const bool same = i < last_bone_ranges_.size() && last_bone_ranges_[i].model == now.model &&
                              last_bone_ranges_[i].count == now.count;
            for (std::uint32_t k = 0; k < now.count; ++k) {
                bone_staging_[count + now.start + k] =
                    same ? last_world_bones_[last_bone_ranges_[i].start + k] : world_now[now.start + k];
            }
        }
        last_world_bones_ = std::move(world_now);
        last_bone_ranges_ = std::move(ranges_now);
        motion_offset_ = static_cast<std::uint32_t>(count);
    }

    // Lotes ordenados por modelo (y material): menos cambios de buffers de
    // vertices al dibujarlos. Luego sus huecos de comando, seguidos.
    std::vector<std::uint32_t> order(draw_batches_.size());
    for (std::uint32_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](std::uint32_t a, std::uint32_t b) {
        const DrawBatch& x = draw_batches_[a];
        const DrawBatch& y = draw_batches_[b];
        return x.model != y.model ? x.model < y.model : x.group < y.group;
    });
    std::vector<std::uint32_t> remap(draw_batches_.size());
    std::vector<DrawBatch> sorted;
    sorted.reserve(draw_batches_.size());
    std::uint32_t slot_count = 0;
    for (std::uint32_t i = 0; i < order.size(); ++i) {
        remap[order[i]] = i;
        DrawBatch batch = draw_batches_[order[i]];
        batch.first_slot = slot_count;
        slot_count += batch.capacity;
        sorted.push_back(batch);
    }
    draw_batches_.swap(sorted);
    for (GpuCluster& cluster : gpu_clusters_) {
        cluster.group = remap[cluster.group];
        cluster.first_slot = draw_batches_[cluster.group].first_slot;
    }
    const auto group_count = static_cast<std::uint32_t>(draw_batches_.size());

    // Modo compatible (moviles): el escenario se dibuja con el culling de la
    // CPU (drawCpuActors), sin comandos indirectos generados en la GPU: varios
    // drivers de movil no aplican bien su firstInstance y el culling usa 5
    // storage buffers (el minimo garantizado es 4).
    if (device_.compatMode()) {
        gpu_clusters_.clear();
        draw_batches_.clear();
    }
    gpu_culling_.setClusters(device_, frame_index, gpu_clusters_, group_count, slot_count,
                             camera_buffers_);

    // Escala de los niveles de detalle de los arboles: sus distancias son
    // las de 1080p con 60 grados; con menos pixeles (vista del editor,
    // resolucion interna baja) o si el presupuesto sube el error permitido de
    // los LODs, cambian antes de nivel.
    {
        constexpr float kReferencePixelsPerUnit = 935.3f;  // 1080 / 2 / tan(30)
        const float error = std::max(post_.lod_pixel_error, 0.05f);
        foliage_lod_scale_ = std::clamp(pixels_per_unit / kReferencePixelsPerUnit / error, 0.25f, 2.0f);
        foliage_pass_.setLodScale(foliage_lod_scale_);
    }

    // La escena de rayos: los escenarios (rigidos) que conoce, el terreno y
    // los arboles cercanos. Solo si se usa el trazado; si lleva un rato sin
    // usarse, se libera (su memoria de video).
    if (rayTracingWanted() && !isolated()) {
        rt_idle_ = false;
        std::vector<RayTracing::Instance> instances;
        for (const ActorDraw& draw : actor_draws_) {
            // Los modelos subidos despues de construirla aun no tienen BLAS
            // (sus indices serian los de las mallas extra).
            if (draw.per_submesh && draw.model < ray_traced_models_) {
                instances.push_back(RayTracing::Instance{
                    draw.model, draw.transform * bone_staging_[draw.bone_offset], RayTracing::kMaskScenery});
            }
        }
        prepareRayTracingScene(scene, instances);
        ray_tracing_.setInstances(device_, instances);
    } else if (!rayTracingWanted() && ray_tracing_.sceneBuilt()) {
        const auto now = std::chrono::steady_clock::now();
        if (!rt_idle_) {
            rt_idle_ = true;
            rt_idle_since_ = now;
        } else if (now - rt_idle_since_ > std::chrono::seconds(10)) {
            device_.waitIdle();
            ray_tracing_.releaseScene();
            ray_pinned_models_.clear();
            ray_traced_models_ = 0;
            rt_scene_dirty_ = true;
            rt_idle_ = false;
            std::cout << "[Vulkan] Escena de rayos liberada (sin usar)\n";
        }
    }

    // Actores anadidos o quitados: las cascadas guardadas no valen.
    actor_set_changed_ = actor_draws_.size() != previous_count;
    for (std::size_t i = 0; !actor_set_changed_ && i < actor_draws_.size(); ++i) {
        // Quien proyecta sombra tambien cambia las cascadas guardadas.
        actor_set_changed_ = previous_actor_motion_[i].cast_shadows != actor_draws_[i].cast_shadows;
    }
    // Y los mapas de las luces locales tampoco (aparece o se va algo).
    if (actor_set_changed_ && !isolated()) {
        local_static_dirty_ = true;
        path_tracing_reset_ = true;
    }

    if (bone_staging_.empty()) {
        return;
    }

    ensureBoneCapacity(frame_index, bone_staging_.size());
    bone_buffers_[frame_index].write(bone_staging_.data(),
                                     sizeof(core::Mat4) * bone_staging_.size());
}

bool VulkanRenderer::actorsTouch(const Vec3& light_position, float range) const {
    // Solo lo que se ha movido o se anima este frame (sus esferas de antes y
    // de ahora): lo estatico ya esta en la cache. Antes se miraban todos los
    // actores, asi que cualquier luz con un mueble al alcance se redibujaba
    // entera cada frame (y con el LOD de la camara: la sombra temblaba).
    for (const core::Vec4& sphere : moved_spheres_) {
        const float reach = range + sphere.w;
        const Vec3 offset = Vec3{sphere.x, sphere.y, sphere.z} - light_position;
        if (core::dot(offset, offset) <= reach * reach) {
            return true;
        }
    }
    return false;
}

std::uint32_t VulkanRenderer::chooseLod(const SkinnedModel& model, const core::Mat4& to_world,
                                        const core::Vec3& center, float radius,
                                        const core::Vec3& camera_position, float pixels_per_unit) const {
    if (!post_.lods || model.lods().empty()) {
        return 0;
    }
    // Escala mayor del objeto: el error del LOD esta en unidades del modelo.
    const auto column = [&](int c) {
        return core::length(core::Vec3{to_world.m[c][0], to_world.m[c][1], to_world.m[c][2]});
    };
    const float scale = std::max({column(0), column(1), column(2)});
    // Distancia al punto mas cercano de su esfera: conservador (con la camara
    // dentro, LOD0).
    const float distance = core::length(center - camera_position) - radius;
    if (distance <= 0.01f) {
        return 0;
    }
    const float pixels = scale * pixels_per_unit / distance;
    const float limit = std::max(post_.lod_pixel_error, 0.05f);
    const auto& lods = model.lods();
    for (std::size_t level = lods.size(); level > 0; --level) {
        if (lods[level - 1].error * pixels <= limit) {
            return static_cast<std::uint32_t>(level);
        }
    }
    return 0;
}

template <typename Draw>
std::uint32_t VulkanRenderer::forEachVisibleSubmesh(const ActorDraw& actor,
                                                    const core::Frustum& frustum,
                                                    Draw&& draw) const {
    const SkinnedModel& model = skinned_models_[actor.model];
    // Rigidos: el LOD elegido (actor.lod, 0 en los animados).
    const auto& submeshes = model.lodSubmeshes(actor.lod);

    if (!actor.per_submesh) {
        // Animado: la esfera del actor, como caja.
        const Vec3 r{actor.bounds_radius, actor.bounds_radius, actor.bounds_radius};
        if (!frustum.intersects(core::Aabb{actor.bounds_center - r, actor.bounds_center + r})) {
            return 0;
        }
    }

    std::uint32_t drawn = 0;
    for (std::uint32_t i = 0; i < submeshes.size(); ++i) {
        if (model.materials()[submeshes[i].material].transparent) {
            continue;  // Vidrio y agua: un diferido no puede mezclarlos.
        }
        if (actor.per_submesh &&
            !frustum.intersects(actor.lod > 0 ? lod_bounds_[actor.first_lod_bounds + i]
                                              : submesh_bounds_[actor.first_bounds + i])) {
            continue;
        }
        draw(i);
        ++drawn;
    }
    return drawn;
}

// Error de LOD (en texeles del mapa) que se permite en las sombras de las
// luces locales. Por debajo del desplazamiento por normal del shader (0.5 a 2
// texeles), para que la malla simplificada no tape a la real.
constexpr float kLocalShadowLodTexels = 0.5f;
constexpr float kCascadeShadowLodTexels = 0.5f;

std::uint32_t VulkanRenderer::shadowLod(const SkinnedModel& model, float max_scale, float allowed) const {
    if (!post_.lods) return 0;
    const auto& lods = model.lods();
    for (std::size_t level = lods.size(); level > 0; --level) {
        if (lods[level - 1].error * max_scale <= allowed) return static_cast<std::uint32_t>(level);
    }
    return 0;
}

void VulkanRenderer::recordActorShadows(const vk::raii::CommandBuffer& cmd,
                                        std::uint32_t frame_index,
                                        const core::Mat4& light_view_projection,
                                        const Vec3& light_position, float range,
                                        float texel_world_size, ShadowActors which) {
    // Las cascadas se dibujan con depth clamp (range == 0): lo que queda entre
    // el sol y la cascada tambien proyecta sombra dentro, asi que no se
    // descarta por el plano cercano.
    const bool local = range > 0.0f;
    const core::Frustum frustum(light_view_projection, /*ignore_near=*/!local);

    // Dos pipelines: solo profundidad para lo opaco (casi todo) y con recorte
    // por alfa para hojas y rejas. Comparten layout, asi que al cambiar de
    // uno a otro los sets y las push constants siguen valiendo.
    const vk::Pipeline opaque_pipeline =
        local ? *skinned_pass_.localShadowPipeline(false) : *skinned_pass_.shadowPipeline(false);
    const vk::Pipeline masked_pipeline =
        local ? *skinned_pass_.localShadowPipeline(true) : *skinned_pass_.shadowPipeline(true);
    vk::Pipeline bound_pipeline{};
    const auto bind = [&](vk::Pipeline pipeline) {
        if (bound_pipeline == pipeline) {
            return;
        }
        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
        if (!bound_pipeline) {
            cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *skinned_pass_.shadowLayout(),
                                   0, *skin_sets_[frame_index], nullptr);
        }
        bound_pipeline = pipeline;
    };

    std::vector<std::uint32_t>& visible = shadow_scratch_;

    for (const ActorDraw& draw : actor_draws_) {
        if (!draw.cast_shadows) {
            continue;  // MeshRenderer con "Proyecta sombras" apagado.
        }
        if (which != ShadowActors::All && (which == ShadowActors::Animated) == draw.per_submesh) {
            continue;  // cache de las cascadas: estaticos o animados
        }
        if (local) {
            const float reach = range + draw.bounds_radius;
            const Vec3 offset = draw.bounds_center - light_position;
            if (core::dot(offset, offset) > reach * reach) {
                continue;
            }
        }

        const SkinnedModel& model = skinned_models_[draw.model];

        // Cascadas: el detalle lo decide el texel, no la camara. Un objeto
        // de menos de medio texel no deja una sombra que se vea, y el LOD
        // puede desviarse hasta lo que mide un texel (el mapa no ve mas).
        std::uint32_t lod = draw.lod;
        if (local && draw.per_submesh) {
            // Luces locales: el detalle lo decide la distancia a la luz, no a
            // la camara (con la de la camara la silueta cambiaba al andar y la
            // sombra temblaba). El error permitido es medio texel del mapa de
            // ESA luz en su parte mas cercana: antes era un tope fijo (2/1024
            // por metro) que en un foco de 2048 o de cono estrecho eran varios
            // texeles, mas que el sesgo del shader, y la malla simplificada
            // asomaba por encima de la real: el objeto se sombreaba a si mismo
            // a manchas (ruido).
            //
            // NO depende del LOD de la camara: con el, cada vez que un objeto
            // cambiaba de LOD al moverse la camara su sombra cambiaba de forma
            // o desaparecia (parpadeo). Que la camara vea una malla mas simple
            // que la del mapa lo compensa el shader (lod_error_per_meter_).
            const float distance = std::max(core::length(draw.bounds_center - light_position) - draw.bounds_radius, 0.05f);
            lod = shadowLod(model, draw.max_scale, distance * texel_world_size * kLocalShadowLodTexels);
        } else if (texel_world_size > 0.0f && draw.per_submesh) {
            if (draw.bounds_radius < texel_world_size * budget_.shadowMinTexels()) {
                continue;
            }
            // Como mucho medio texel, sin depender del presupuesto: al bajar
            // el detalle con muchos objetos la malla de la sombra asomaba
            // varios texeles por encima de la real y el objeto se sombreaba a
            // si mismo (y parpadeaba cada vez que el presupuesto cambiaba de
            // nivel). El shader cubre ese medio texel.
            lod = shadowLod(model, draw.max_scale, texel_world_size * kCascadeShadowLodTexels);
        }

        // Submallas que tocan el volumen de la luz (una sola prueba).
        visible.clear();
        if (lod == draw.lod) {
            shadow_submeshes_ += forEachVisibleSubmesh(
                draw, frustum, [&](std::uint32_t i) { visible.push_back(i); });
        } else if (lod == 0) {
            ActorDraw full = draw;
            full.lod = 0;
            shadow_submeshes_ += forEachVisibleSubmesh(
                full, frustum, [&](std::uint32_t i) { visible.push_back(i); });
        } else {
            // Otro LOD que el de la camara: sin cajas por cluster, la esfera
            // del actor (los LODs lejanos tienen pocos clusteres).
            const Vec3 r{draw.bounds_radius, draw.bounds_radius, draw.bounds_radius};
            if (frustum.intersects(core::Aabb{draw.bounds_center - r, draw.bounds_center + r})) {
                const auto& submeshes = model.lodSubmeshes(lod);
                for (std::uint32_t i = 0; i < submeshes.size(); ++i) {
                    if (!model.materials()[submeshes[i].material].transparent) visible.push_back(i);
                }
                shadow_submeshes_ += static_cast<std::uint32_t>(visible.size());
            }
        }
        if (visible.empty()) {
            continue;
        }

        // --- Mesh shaders: lo opaco de los escenarios en las cascadas ---
        // Por meshlets (shadow_meshlet.task/.mesh): la GPU descarta los que
        // caen fuera de la cascada y, en mallas cerradas, los que miran en
        // contra de la luz. Lo recortado por alfa sigue por el pipeline de
        // siempre (necesita la textura).
        bool meshlet_opaque = false;
        if (!local && texel_world_size > 0.0f && draw.per_submesh && skinned_pass_.meshShadersEnabled() &&
            model.hasMeshlets()) {
            meshlet_opaque = true;
            const auto& ranges = model.meshletRanges(lod);
            const auto& lod_submeshes = model.lodSubmeshes(lod);
            // Direccion de los rayos en el espacio del modelo (la inversa de
            // un giro es su traspuesta). Con escala no uniforme los conos no
            // valen: sin descarte por cono.
            const Vec3 travel = -ibl_to_light_;
            const core::Mat4& t = draw.transform;
            Vec3 columns[3];
            float scales[3];
            for (int c = 0; c < 3; ++c) {
                columns[c] = Vec3{t.m[c][0], t.m[c][1], t.m[c][2]};
                scales[c] = core::length(columns[c]);
            }
            const bool uniform = std::abs(scales[0] - scales[1]) < scales[0] * 0.01f &&
                                 std::abs(scales[0] - scales[2]) < scales[0] * 0.01f && scales[0] > 1e-6f;
            const Vec3 local_travel = core::normalize(
                Vec3{core::dot(columns[0], travel), core::dot(columns[1], travel), core::dot(columns[2], travel)});
            GpuMeshletShadowPush mesh_push{};
            mesh_push.light_model_view_projection = light_view_projection * draw.transform;
            mesh_push.bone_offset = draw.bone_offset;
            mesh_push.flags = model.closed(lod) && uniform ? 1u : 0u;
            mesh_push.light_direction = toVec4(local_travel, 0.0f);

            bool mesh_bound = false;
            std::uint32_t run_first = 0;
            std::uint32_t run_count = 0;
            const auto flush_meshlets = [&]() {
                if (run_count == 0) return;
                if (!mesh_bound) {
                    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *skinned_pass_.meshShadowPipeline());
                    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *skinned_pass_.meshShadowLayout(), 0,
                                           *skin_sets_[frame_index], nullptr);
                    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *skinned_pass_.meshShadowLayout(), 1,
                                           *model.meshletSet(), nullptr);
                    mesh_bound = true;
                    bound_pipeline = vk::Pipeline{};  // el pipeline de siempre vuelve a enlazar sus sets
                }
                mesh_push.first_meshlet = run_first;
                mesh_push.meshlet_count = run_count;
                cmd.pushConstants<GpuMeshletShadowPush>(
                    *skinned_pass_.meshShadowLayout(),
                    vk::ShaderStageFlagBits::eTaskEXT | vk::ShaderStageFlagBits::eMeshEXT, 0, mesh_push);
                cmd.drawMeshTasksEXT((run_count + 31) / 32, 1, 1);
                ++shadow_draw_calls_;
                run_count = 0;
            };
            for (const std::uint32_t i : visible) {
                const SkinnedModel::Material& material = model.materials()[lod_submeshes[i].material];
                if (material.alpha_masked || material.transparent || ranges[i].count == 0 ||
                    tessellatedMaterial(material)) {
                    continue;  // recortado o teselado: por el pipeline de siempre
                }
                if (run_count > 0 && run_first + run_count == ranges[i].first) {
                    run_count += ranges[i].count;
                } else {
                    flush_meshlets();
                    run_first = ranges[i].first;
                    run_count = ranges[i].count;
                }
            }
            flush_meshlets();
        }

        bind(bound_pipeline ? bound_pipeline : opaque_pipeline);

        GpuSkinnedShadowPush push{};
        push.light_model_view_projection = light_view_projection * draw.transform;
        push.bone_offset = draw.bone_offset;
        cmd.pushConstants<GpuSkinnedShadowPush>(*skinned_pass_.shadowLayout(), skinned_pass_.shadowPushStages(), 0,
                                                push);

        cmd.bindVertexBuffers(0, *model.vertices().handle(), {0});
        cmd.bindIndexBuffer(*model.indices().handle(), 0, vk::IndexType::eUint32);

        // Primero todo lo opaco con el pipeline de solo profundidad (sin
        // material: no lee texturas). Luego lo recortado por alfa: la textura
        // del material decide (las hojas proyectan la sombra de las hojas, no
        // la de su rectangulo); el material se vincula solo al cambiar.
        //
        // Batching: la sombra opaca no depende del material, asi que las
        // submallas visibles que estan seguidas en el buffer de indices (los
        // clusteres de un escenario lo estan casi siempre) van en UNA llamada.
        // Las recortadas solo se juntan si comparten material.
        for (int masked = 0; masked < 2; ++masked) {
            if (masked == 0 && meshlet_opaque) continue;  // ya lo dibujaron los meshlets
            bool pipeline_set = false;
            std::uint32_t bound_material = UINT32_MAX;
            std::uint32_t run_first = 0;
            std::uint32_t run_count = 0;
            const auto flush = [&]() {
                if (run_count == 0) return;
                cmd.drawIndexed(run_count, 1, run_first, 0, 0);
                ++shadow_draw_calls_;
                run_count = 0;
            };
            for (const std::uint32_t i : visible) {
                const asset::SubMesh& submesh = model.lodSubmeshes(lod)[i];
                if (model.materials()[submesh.material].alpha_masked != (masked != 0) ||
                    tessellatedMaterial(model.materials()[submesh.material])) {
                    continue;  // los teselados van despues, con su pipeline
                }
                if (!pipeline_set) {
                    bind(masked != 0 ? masked_pipeline : opaque_pipeline);
                    pipeline_set = true;
                }
                if (masked != 0 && submesh.material != bound_material) {
                    flush();
                    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
                                           *skinned_pass_.shadowLayout(), 1,
                                           *model.materialSet(submesh.material), nullptr);
                    bound_material = submesh.material;
                }
                if (run_count > 0 && run_first + run_count == submesh.first_index) {
                    run_count += submesh.index_count;  // sigue al anterior: se alarga
                } else {
                    flush();
                    run_first = submesh.first_index;
                    run_count = submesh.index_count;
                }
            }
            flush();
        }

        // --- Relieve teselado: la sombra sube con los vertices ---
        // Cada submalla con su material (la altura sale de su textura). La
        // camara va al espacio del modelo para partir como lo que ella ve.
        bool tess_push_ready = false;
        for (const std::uint32_t i : visible) {
            const asset::SubMesh& submesh = model.lodSubmeshes(lod)[i];
            const SkinnedModel::Material& material = model.materials()[submesh.material];
            if (!tessellatedMaterial(material)) continue;
            bind(*skinned_pass_.shadowTessPipeline(local, material.alpha_masked));
            if (!tess_push_ready) {
                const core::Mat4& t = draw.transform;
                const core::Vec4 camera_model = core::inverse(t) * toVec4(camera_position_, 1.0f);
                push.camera_model = core::Vec4{camera_model.x, camera_model.y, camera_model.z, 0.0f};
                push.model_scale = core::Vec4{core::length(Vec3{t.m[0][0], t.m[0][1], t.m[0][2]}),
                                              core::length(Vec3{t.m[1][0], t.m[1][1], t.m[1][2]}),
                                              core::length(Vec3{t.m[2][0], t.m[2][1], t.m[2][2]}), 0.0f};
                tess_push_ready = true;
            }
            push.height = material.emissive.w;
            push.max_factor = static_cast<float>((material.shader_flags & GpuSkinnedPush::kTessFactorMask) >>
                                                 GpuSkinnedPush::kTessFactorShift);
            cmd.pushConstants<GpuSkinnedShadowPush>(*skinned_pass_.shadowLayout(), skinned_pass_.shadowPushStages(),
                                                    0, push);
            cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *skinned_pass_.shadowLayout(), 1,
                                   *model.materialSet(submesh.material), nullptr);
            cmd.drawIndexed(submesh.index_count, 1, submesh.first_index, 0, 0);
            ++shadow_draw_calls_;
        }
    }
}

void VulkanRenderer::updateLightingDescriptors() {
    // Se rehace cada vez que cambia el G-buffer (al redimensionar la ventana).

    for (std::uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
        vk::DescriptorBufferInfo camera_info{};
        camera_info.buffer = *camera_buffers_[i].handle();
        camera_info.range = sizeof(GpuCamera);

        vk::DescriptorBufferInfo lights_info{};
        lights_info.buffer = *light_buffers_[i].handle();
        lights_info.range = sizeof(GpuLights);

        vk::DescriptorBufferInfo shadows_info{};
        shadows_info.buffer = *shadow_buffers_[i].handle();
        shadows_info.range = sizeof(GpuShadows);

        vk::DescriptorImageInfo shadow_map_info{};
        shadow_map_info.sampler = *shadow_map_.sampler();
        shadow_map_info.imageView = *shadow_map_.image().view();
        shadow_map_info.imageLayout = compat::depthReadOnlyLayout();

        // Las sombras locales reutilizan el muestreador de comparacion de las
        // cascadas: mismo filtrado y mismo borde "iluminado".
        vk::DescriptorImageInfo spot_shadow_info{};
        spot_shadow_info.sampler = *shadow_map_.sampler();
        spot_shadow_info.imageView = *local_shadow_maps_.spotImage().view();
        spot_shadow_info.imageLayout = compat::depthReadOnlyLayout();

        vk::DescriptorImageInfo point_shadow_info{};
        point_shadow_info.sampler = *shadow_map_.sampler();
        point_shadow_info.imageView = *local_shadow_maps_.pointImage().view();
        point_shadow_info.imageLayout = compat::depthReadOnlyLayout();

        vk::DescriptorBufferInfo local_shadows_info{};
        local_shadows_info.buffer = *local_shadow_buffers_[i].handle();
        local_shadows_info.range = sizeof(GpuLocalShadows);

        // Bindings 1 y 2: albedo y normales. Binding 3: la profundidad, que se
        // lee en su propio layout de solo lectura. (El material va aparte, en
        // el binding 12.)
        std::array<vk::DescriptorImageInfo, 3> image_infos{};
        const std::array<const VulkanImage*, 2> surface = {&gbuffer_.albedo(), &gbuffer_.normal()};
        for (std::size_t attachment = 0; attachment < surface.size(); ++attachment) {
            image_infos[attachment].sampler = *lighting_pass_.sampler();
            image_infos[attachment].imageView = *surface[attachment]->view();
            image_infos[attachment].imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
        }

        image_infos[2].sampler = *lighting_pass_.sampler();
        image_infos[2].imageView = *gbuffer_.depth().view();
        image_infos[2].imageLayout = compat::depthReadOnlyLayout();

        vk::DescriptorImageInfo material_info{};
        material_info.sampler = *lighting_pass_.sampler();
        material_info.imageView = *gbuffer_.material().view();
        material_info.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;

        vk::DescriptorImageInfo environment_info{};
        environment_info.sampler = *ibl_probe_.sampler();
        environment_info.imageView = *ibl_probe_.environmentView();
        environment_info.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;

        vk::DescriptorImageInfo brdf_info{};
        brdf_info.sampler = *ibl_probe_.sampler();
        brdf_info.imageView = *ibl_probe_.brdfLutView();
        brdf_info.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;

        vk::DescriptorBufferInfo irradiance_info{};
        irradiance_info.buffer = *ibl_probe_.irradianceBuffer().handle();
        irradiance_info.range = VK_WHOLE_SIZE;

        vk::DescriptorImageInfo ssao_info{};
        ssao_info.sampler = *lighting_pass_.sampler();
        ssao_info.imageView = *ssao_image_.view();
        ssao_info.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;

        vk::DescriptorImageInfo sky_info{};
        sky_info.sampler = *lighting_pass_.sampler();
        sky_info.imageView = *sky_lut_.view();
        sky_info.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;

        vk::DescriptorImageInfo gi_info{};
        gi_info.sampler = *lighting_pass_.sampler();
        gi_info.imageView = *gi_image_.view();
        gi_info.imageLayout = vk::ImageLayout::eGeneral;  // la escribe un compute

        vk::DescriptorImageInfo ssr_info{};
        ssr_info.sampler = *lighting_pass_.sampler();
        ssr_info.imageView = *ssr_image_.view();
        ssr_info.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;

        std::array<vk::DescriptorImageInfo, ReflectionProbe::kCubeCount> probe_infos{};
        for (std::uint32_t cube = 0; cube < ReflectionProbe::kCubeCount; ++cube) {
            probe_infos[cube].sampler = *reflection_probe_.sampler();
            probe_infos[cube].imageView = *reflection_probe_.view(cube);
            probe_infos[cube].imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
        }

        vk::DescriptorImageInfo clouds_info{};
        clouds_info.sampler = *lighting_pass_.sampler();
        clouds_info.imageView = *clouds_image_.view();
        clouds_info.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;

        // Sin HDR, la LUT del cielo ocupa su hueco (el shader no lo lee).
        vk::DescriptorImageInfo environment_hdr_info = sky_info;
        if (environment_.loaded()) {
            environment_hdr_info.sampler = *environment_.sampler();
            environment_hdr_info.imageView = *environment_.view();
        }

        std::array<vk::WriteDescriptorSet, 29> writes{};

        writes[0].dstSet = *lighting_sets_[i];
        writes[0].dstBinding = 0;
        writes[0].descriptorType = vk::DescriptorType::eUniformBuffer;
        writes[0].setBufferInfo(camera_info);

        for (std::uint32_t attachment = 0; attachment < 3; ++attachment) {
            writes[1 + attachment].dstSet = *lighting_sets_[i];
            writes[1 + attachment].dstBinding = 1 + attachment;
            writes[1 + attachment].descriptorType = vk::DescriptorType::eCombinedImageSampler;
            writes[1 + attachment].setImageInfo(image_infos[attachment]);
        }

        writes[4].dstSet = *lighting_sets_[i];
        writes[4].dstBinding = 4;
        writes[4].descriptorType = vk::DescriptorType::eUniformBuffer;
        writes[4].setBufferInfo(lights_info);

        writes[5].dstSet = *lighting_sets_[i];
        writes[5].dstBinding = 5;
        writes[5].descriptorType = vk::DescriptorType::eCombinedImageSampler;
        writes[5].setImageInfo(shadow_map_info);

        writes[6].dstSet = *lighting_sets_[i];
        writes[6].dstBinding = 6;
        writes[6].descriptorType = vk::DescriptorType::eUniformBuffer;
        writes[6].setBufferInfo(shadows_info);

        writes[7].dstSet = *lighting_sets_[i];
        writes[7].dstBinding = 7;
        writes[7].descriptorType = vk::DescriptorType::eCombinedImageSampler;
        writes[7].setImageInfo(spot_shadow_info);

        writes[8].dstSet = *lighting_sets_[i];
        writes[8].dstBinding = 8;
        writes[8].descriptorType = vk::DescriptorType::eCombinedImageSampler;
        writes[8].setImageInfo(point_shadow_info);

        writes[9].dstSet = *lighting_sets_[i];
        writes[9].dstBinding = 9;
        writes[9].descriptorType = vk::DescriptorType::eUniformBuffer;
        writes[9].setBufferInfo(local_shadows_info);

        writes[10].dstSet = *lighting_sets_[i];
        writes[10].dstBinding = 10;
        writes[10].descriptorType = vk::DescriptorType::eCombinedImageSampler;
        writes[10].setImageInfo(ssao_info);

        writes[11].dstSet = *lighting_sets_[i];
        writes[11].dstBinding = 11;
        writes[11].descriptorType = vk::DescriptorType::eCombinedImageSampler;
        writes[11].setImageInfo(sky_info);

        writes[12].dstSet = *lighting_sets_[i];
        writes[12].dstBinding = 12;
        writes[12].descriptorType = vk::DescriptorType::eCombinedImageSampler;
        writes[12].setImageInfo(material_info);

        writes[13].dstSet = *lighting_sets_[i];
        writes[13].dstBinding = 13;
        writes[13].descriptorType = vk::DescriptorType::eCombinedImageSampler;
        writes[13].setImageInfo(environment_info);

        writes[14].dstSet = *lighting_sets_[i];
        writes[14].dstBinding = 14;
        writes[14].descriptorType = vk::DescriptorType::eCombinedImageSampler;
        writes[14].setImageInfo(brdf_info);

        writes[15].dstSet = *lighting_sets_[i];
        writes[15].dstBinding = 15;
        writes[15].descriptorType = vk::DescriptorType::eStorageBuffer;
        writes[15].setBufferInfo(irradiance_info);

        writes[16].dstSet = *lighting_sets_[i];
        writes[16].dstBinding = 16;
        writes[16].descriptorType = vk::DescriptorType::eCombinedImageSampler;
        writes[16].setImageInfo(gi_info);

        writes[17].dstSet = *lighting_sets_[i];
        writes[17].dstBinding = 17;
        writes[17].descriptorType = vk::DescriptorType::eCombinedImageSampler;
        writes[17].setImageInfo(ssr_info);

        for (std::uint32_t cube = 0; cube < ReflectionProbe::kCubeCount; ++cube) {
            vk::WriteDescriptorSet& write = writes[18 + cube];
            write.dstSet = *lighting_sets_[i];
            write.dstBinding = 18 + cube;
            write.descriptorType = vk::DescriptorType::eCombinedImageSampler;
            write.setImageInfo(probe_infos[cube]);
        }

        writes[20].dstSet = *lighting_sets_[i];
        writes[20].dstBinding = 20;
        writes[20].descriptorType = vk::DescriptorType::eCombinedImageSampler;
        writes[20].setImageInfo(clouds_info);

        writes[21].dstSet = *lighting_sets_[i];
        writes[21].dstBinding = 21;
        writes[21].descriptorType = vk::DescriptorType::eCombinedImageSampler;
        writes[21].setImageInfo(environment_hdr_info);

        vk::DescriptorImageInfo volumetric_info{};
        volumetric_info.sampler = *lighting_pass_.sampler();
        volumetric_info.imageView = *volumetric_image_.view();
        volumetric_info.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
        writes[22].dstSet = *lighting_sets_[i];
        writes[22].dstBinding = 22;
        writes[22].descriptorType = vk::DescriptorType::eCombinedImageSampler;
        writes[22].setImageInfo(volumetric_info);

        vk::DescriptorImageInfo cloud_shadow_info{};
        cloud_shadow_info.sampler = *lighting_pass_.sampler();
        cloud_shadow_info.imageView = *cloud_shadow_image_.view();
        cloud_shadow_info.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
        writes[23].dstSet = *lighting_sets_[i];
        writes[23].dstBinding = 23;
        writes[23].descriptorType = vk::DescriptorType::eCombinedImageSampler;
        writes[23].setImageInfo(cloud_shadow_info);

        vk::DescriptorImageInfo shadow_raw_info = shadow_map_info;
        shadow_raw_info.sampler = *shadow_map_.rawSampler();
        writes[24].dstSet = *lighting_sets_[i];
        writes[24].dstBinding = 24;
        writes[24].descriptorType = vk::DescriptorType::eCombinedImageSampler;
        writes[24].setImageInfo(shadow_raw_info);

        // Sombras por rayos, ya acumuladas en el tiempo (RGBA16UI, se lee con
        // texelFetch; vive en General: la escribe y la copia un compute).
        // (Un formato entero no admite filtro lineal: el muestreador sin
        // filtro del mapa de lluvia.)
        vk::DescriptorImageInfo rt_shadow_info{};
        rt_shadow_info.sampler = *rain_sampler_;
        rt_shadow_info.imageView = *rt_shadow_accum_.view();
        rt_shadow_info.imageLayout = vk::ImageLayout::eGeneral;
        writes[25].dstSet = *lighting_sets_[i];
        writes[25].dstBinding = 25;
        writes[25].descriptorType = vk::DescriptorType::eCombinedImageSampler;
        writes[25].setImageInfo(rt_shadow_info);

        // Modelo de sombreado de Disney (disney_brdf.glsl).
        vk::DescriptorImageInfo shading_info{};
        shading_info.sampler = *lighting_pass_.sampler();
        shading_info.imageView = *gbuffer_.shading().view();
        shading_info.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
        writes[26].dstSet = *lighting_sets_[i];
        writes[26].dstBinding = 26;
        writes[26].descriptorType = vk::DescriptorType::eCombinedImageSampler;
        writes[26].setImageInfo(shading_info);

        // Oclusion del cielo vista desde arriba (profundidades, sin filtrar).
        vk::DescriptorImageInfo sky_terrain_info{};
        sky_terrain_info.sampler = *rain_sampler_;
        sky_terrain_info.imageView = *sky_map_terrain_.view();
        sky_terrain_info.imageLayout = compat::depthReadOnlyLayout();
        writes[27].dstSet = *lighting_sets_[i];
        writes[27].dstBinding = 27;
        writes[27].descriptorType = vk::DescriptorType::eCombinedImageSampler;
        writes[27].setImageInfo(sky_terrain_info);
        vk::DescriptorImageInfo sky_scene_info = sky_terrain_info;
        sky_scene_info.imageView = *sky_map_scene_.view();
        writes[28].dstSet = *lighting_sets_[i];
        writes[28].dstBinding = 28;
        writes[28].descriptorType = vk::DescriptorType::eCombinedImageSampler;
        writes[28].setImageInfo(sky_scene_info);

        // Modo compatible: el layout no tiene todos (16 texturas).
        std::vector<vk::WriteDescriptorSet> used;
        for (const vk::WriteDescriptorSet& w : writes) {
            if (lighting_pass_.hasBinding(w.dstBinding)) used.push_back(w);
        }
        device_.handle().updateDescriptorSets(used, nullptr);
    }

    // Descriptor del FXAA: la imagen ya compuesta en 8 bits.
    for (std::uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
        vk::DescriptorImageInfo scene_info{};
        scene_info.sampler = *post_process_pass_.sampler();
        scene_info.imageView = *ldr_color_.view();
        scene_info.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;

        vk::WriteDescriptorSet write{};
        write.dstSet = *post_process_sets_[i];
        write.dstBinding = 0;
        write.descriptorType = vk::DescriptorType::eCombinedImageSampler;
        write.setImageInfo(scene_info);

        device_.handle().updateDescriptorSets(write, nullptr);
    }

    updateGlassDescriptors();
}

void VulkanRenderer::updateGlassDescriptors() {
    // Mismas imagenes y muestreadores que la iluminacion (se rehace con ella)
    // mas la copia de la imagen HDR sin vidrio.
    for (std::uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
        vk::DescriptorBufferInfo camera_info{};
        camera_info.buffer = *camera_buffers_[i].handle();
        camera_info.range = sizeof(GpuCamera);

        vk::DescriptorBufferInfo lights_info{};
        lights_info.buffer = *light_buffers_[i].handle();
        lights_info.range = sizeof(GpuLights);

        vk::DescriptorBufferInfo shadows_info{};
        shadows_info.buffer = *shadow_buffers_[i].handle();
        shadows_info.range = sizeof(GpuShadows);

        std::array<vk::DescriptorImageInfo, 7> images{};
        images[0] = vk::DescriptorImageInfo{*shadow_map_.sampler(), *shadow_map_.image().view(),
                                            compat::depthReadOnlyLayout()};
        images[1] = vk::DescriptorImageInfo{*lighting_pass_.sampler(), *gbuffer_.depth().view(),
                                            compat::depthReadOnlyLayout()};
        images[2] = vk::DescriptorImageInfo{*lighting_pass_.sampler(), *glass_source_.view(),
                                            vk::ImageLayout::eShaderReadOnlyOptimal};
        images[3] = vk::DescriptorImageInfo{*ibl_probe_.sampler(), *ibl_probe_.environmentView(),
                                            vk::ImageLayout::eShaderReadOnlyOptimal};
        for (std::uint32_t cube = 0; cube < 2; ++cube) {
            images[4 + cube] =
                vk::DescriptorImageInfo{*reflection_probe_.sampler(), *reflection_probe_.view(cube),
                                        vk::ImageLayout::eShaderReadOnlyOptimal};
        }
        // Luz volumetrica: el agua la aplica hasta su superficie (la
        // iluminacion la aplico hasta el fondo, que queda detras).
        images[6] = vk::DescriptorImageInfo{*lighting_pass_.sampler(), *volumetric_image_.view(),
                                            vk::ImageLayout::eShaderReadOnlyOptimal};

        std::array<vk::WriteDescriptorSet, SkinnedPass::kGlassBindingCount> writes{};
        const std::array<const vk::DescriptorBufferInfo*, 3> buffers = {&camera_info, &lights_info,
                                                                        &shadows_info};
        for (std::uint32_t b = 0; b < SkinnedPass::kGlassBindingCount; ++b) {
            writes[b].dstSet = *glass_sets_[i];
            writes[b].dstBinding = b;
            writes[b].descriptorCount = 1;
            if (b < 3) {
                writes[b].descriptorType = vk::DescriptorType::eUniformBuffer;
                writes[b].pBufferInfo = buffers[b];
            } else {
                writes[b].descriptorType = vk::DescriptorType::eCombinedImageSampler;
                writes[b].pImageInfo = &images[b - 3];
            }
        }
        // Modo compatible: el set no tiene las sondas ni la luz volumetrica.
        device_.handle().updateDescriptorSets(
            vk::ArrayProxy<const vk::WriteDescriptorSet>(SkinnedPass::glassBindingCount(), writes.data()), nullptr);
    }
}

void VulkanRenderer::updatePostDescriptors() {
    // Los sets de cada ojo con las historias de ese ojo: si llegara a mitad
    // del ojo derecho, primero se deshace el cambio (y se rehace al salir).
    struct EyeStateRest {
        VulkanRenderer& renderer;
        bool swapped;
        ~EyeStateRest() {
            if (swapped) renderer.swapEyeState();
        }
    } rest{*this, eye_state_swapped_};
    if (rest.swapped) swapEyeState();
    // Las del ojo derecho (sin casco no existen: sus sets apuntan a las del
    // izquierdo y no se usan).
    const auto eye_image = [](std::uint32_t eye, const VulkanImage& left, const VulkanImage& right) -> const VulkanImage& {
        return eye == 1 && right.isValid() ? right : left;
    };
    writeDofFocusDescriptors();  // el autoenfoque lee la profundidad del G-buffer
    updateBakedGiSets();         // GI horneada: profundidad y normales nuevas
    const auto write_texture = [&](const vk::raii::DescriptorSet& set, std::uint32_t binding,
                                   const vk::raii::Sampler& sampler, const VulkanImage& image,
                                   vk::ImageLayout layout) {
        vk::DescriptorImageInfo info{};
        info.sampler = *sampler;
        info.imageView = *image.view();
        info.imageLayout = layout;

        vk::WriteDescriptorSet write{};
        write.dstSet = *set;
        write.dstBinding = binding;
        write.descriptorType = vk::DescriptorType::eCombinedImageSampler;
        write.setImageInfo(info);
        device_.handle().updateDescriptorSets(write, nullptr);
    };

    constexpr vk::ImageLayout kRead = vk::ImageLayout::eShaderReadOnlyOptimal;

    // SSAO: camara del frame + profundidad + normales.
    for (std::uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
        vk::DescriptorBufferInfo camera_info{};
        camera_info.buffer = *camera_buffers_[i].handle();
        camera_info.range = sizeof(GpuCamera);

        vk::WriteDescriptorSet write{};
        write.dstSet = *ssao_sets_[i];
        write.dstBinding = 0;
        write.descriptorType = vk::DescriptorType::eUniformBuffer;
        write.setBufferInfo(camera_info);
        device_.handle().updateDescriptorSets(write, nullptr);

        write_texture(ssao_sets_[i], 1, ssao_pass_.sampler(), gbuffer_.depth(),
                      compat::depthReadOnlyLayout());
        write_texture(ssao_sets_[i], 2, ssao_pass_.sampler(), gbuffer_.normal(), kRead);

        // Luz volumetrica: camara, luces, cascadas, su mapa (con el
        // muestreador de comparacion de las sombras) y la profundidad.
        {
            vk::DescriptorBufferInfo lights_info{*light_buffers_[i].handle(), 0, sizeof(GpuLights)};
            vk::DescriptorBufferInfo shadows_info{*shadow_buffers_[i].handle(), 0,
                                                  sizeof(GpuShadows)};
            const std::array<const vk::DescriptorBufferInfo*, 3> buffers = {&camera_info,
                                                                            &lights_info,
                                                                            &shadows_info};
            for (std::uint32_t b = 0; b < 3; ++b) {
                vk::WriteDescriptorSet buffer_write{};
                buffer_write.dstSet = *volumetric_sets_[i];
                buffer_write.dstBinding = b;
                buffer_write.descriptorType = vk::DescriptorType::eUniformBuffer;
                buffer_write.setBufferInfo(*buffers[b]);
                device_.handle().updateDescriptorSets(buffer_write, nullptr);
            }
            write_texture(volumetric_sets_[i], 3, shadow_map_.sampler(), shadow_map_.image(),
                          compat::depthReadOnlyLayout());
            write_texture(volumetric_sets_[i], 4, volumetric_pass_.sampler(), gbuffer_.depth(),
                          compat::depthReadOnlyLayout());

            // Sombras de las luces locales (el mismo muestreador de
            // comparacion que las cascadas, como en la iluminacion).
            write_texture(volumetric_sets_[i], 5, shadow_map_.sampler(),
                          local_shadow_maps_.spotImage(), compat::depthReadOnlyLayout());
            write_texture(volumetric_sets_[i], 6, shadow_map_.sampler(),
                          local_shadow_maps_.pointImage(), compat::depthReadOnlyLayout());
            vk::DescriptorBufferInfo local_info{*local_shadow_buffers_[i].handle(), 0,
                                                sizeof(GpuLocalShadows)};
            vk::WriteDescriptorSet local_write{};
            local_write.dstSet = *volumetric_sets_[i];
            local_write.dstBinding = 7;
            local_write.descriptorType = vk::DescriptorType::eUniformBuffer;
            local_write.setBufferInfo(local_info);
            device_.handle().updateDescriptorSets(local_write, nullptr);

            vk::DescriptorBufferInfo sky_info{*ibl_probe_.irradianceBuffer().handle(), 0, VK_WHOLE_SIZE};
            vk::WriteDescriptorSet sky_write{};
            sky_write.dstSet = *volumetric_sets_[i];
            sky_write.dstBinding = 8;
            sky_write.descriptorType = vk::DescriptorType::eStorageBuffer;
            sky_write.setBufferInfo(sky_info);
            device_.handle().updateDescriptorSets(sky_write, nullptr);

            vk::DescriptorBufferInfo fog_info{*fog_volume_buffers_[i].handle(), 0, sizeof(GpuFogVolumes)};
            vk::WriteDescriptorSet fog_write{};
            fog_write.dstSet = *volumetric_sets_[i];
            fog_write.dstBinding = 9;
            fog_write.descriptorType = vk::DescriptorType::eUniformBuffer;
            fog_write.setBufferInfo(fog_info);
            device_.handle().updateDescriptorSets(fog_write, nullptr);
        }

        // SSGI: camara + profundidad + normales + imagen del frame anterior.
        vk::WriteDescriptorSet ssgi_camera = write;
        ssgi_camera.dstSet = *ssgi_sets_[i];
        device_.handle().updateDescriptorSets(ssgi_camera, nullptr);
        write_texture(ssgi_sets_[i], 1, ssgi_pass_.sampler(), gbuffer_.depth(),
                      compat::depthReadOnlyLayout());
        write_texture(ssgi_sets_[i], 2, ssgi_pass_.sampler(), gbuffer_.normal(), kRead);
        write_texture(ssgi_sets_[i], 3, ssgi_pass_.sampler(), scene_color_, kRead);
        for (std::uint32_t cube = 0; cube < ReflectionProbe::kCubeCount; ++cube) {
            vk::DescriptorImageInfo probe_info{};
            probe_info.sampler = *reflection_probe_.sampler();
            probe_info.imageView = *reflection_probe_.view(cube);
            probe_info.imageLayout = kRead;
            vk::WriteDescriptorSet probe_write{};
            probe_write.dstSet = *ssgi_sets_[i];
            probe_write.dstBinding = 4 + cube;
            probe_write.descriptorType = vk::DescriptorType::eCombinedImageSampler;
            probe_write.setImageInfo(probe_info);
            device_.handle().updateDescriptorSets(probe_write, nullptr);
        }

        // SSR: los mismos recursos.
        vk::WriteDescriptorSet ssr_camera = write;
        ssr_camera.dstSet = *ssr_sets_[i];
        device_.handle().updateDescriptorSets(ssr_camera, nullptr);
        write_texture(ssr_sets_[i], 1, ssr_pass_.sampler(), gbuffer_.depth(),
                      compat::depthReadOnlyLayout());
        write_texture(ssr_sets_[i], 2, ssr_pass_.sampler(), gbuffer_.normal(), kRead);
        write_texture(ssr_sets_[i], 3, ssr_pass_.sampler(), scene_color_, kRead);

        // Filtro temporal del SSR (cada ojo de VR con su historia).
        for (std::uint32_t eye = 0; eye < 2; ++eye) {
            const vk::raii::DescriptorSet& resolve_set = ssr_resolve_sets_[i + eye * kMaxFramesInFlight];
            vk::WriteDescriptorSet resolve_camera = write;
            resolve_camera.dstSet = *resolve_set;
            device_.handle().updateDescriptorSets(resolve_camera, nullptr);
            write_texture(resolve_set, 1, ssr_resolve_pass_.sampler(), gbuffer_.depth(),
                          compat::depthReadOnlyLayout());
            write_texture(resolve_set, 2, ssr_resolve_pass_.sampler(), ssr_raw_, kRead);
            write_texture(resolve_set, 3, ssr_resolve_pass_.sampler(),
                          eye_image(eye, ssr_history_, xr_eye1_.ssr_history), kRead);
        }

        // Trazado de rayos: camara, luces, G-buffer, imagen anterior, salidas
        // y entorno.
        if (device_.rayTracingSupported()) {
            RayTracing::FrameInputs inputs{};
            inputs.camera = &camera_buffers_[i];
            inputs.lights = &light_buffers_[i];
            inputs.depth = *gbuffer_.depth().view();
            inputs.normal = *gbuffer_.normal().view();
            inputs.previous_color = *scene_color_.view();
            inputs.gi_output = *gi_raw_.view();
            inputs.reflection_output = *ssr_raw_.view();
            inputs.environment = *ibl_probe_.environmentView();
            inputs.albedo = *gbuffer_.albedo().view();
            inputs.material = *gbuffer_.material().view();
            inputs.shading = *gbuffer_.shading().view();
            inputs.path_output = *scene_color_.view();
            inputs.accumulation = *path_tracing_accumulation_.view();
            inputs.shadow_output = *rt_shadow_mask_.view();
            inputs.sampler = *ssgi_pass_.sampler();
            inputs.environment_sampler = *ibl_probe_.sampler();
            ray_tracing_.updateFrameSet(device_, i, inputs);
        }

        // --- Filtro SVGF de la GI ---
        const auto write_storage_image = [&](const vk::raii::DescriptorSet& set,
                                             std::uint32_t binding, const VulkanImage& image) {
            vk::DescriptorImageInfo info{};
            info.imageView = *image.view();
            info.imageLayout = vk::ImageLayout::eGeneral;
            vk::WriteDescriptorSet storage_write{};
            storage_write.dstSet = *set;
            storage_write.dstBinding = binding;
            storage_write.descriptorType = vk::DescriptorType::eStorageImage;
            storage_write.setImageInfo(info);
            device_.handle().updateDescriptorSets(storage_write, nullptr);
        };
        constexpr vk::ImageLayout kGeneral = vk::ImageLayout::eGeneral;

        // Temporal: raw (este frame) + historia -> acumulada, momentos, varianza
        // (cada ojo de VR con su historia).
        const vk::raii::Sampler& gi_sampler = gi_temporal_pass_.sampler();
        for (std::uint32_t eye = 0; eye < 2; ++eye) {
            const vk::raii::DescriptorSet& temporal_set = gi_temporal_sets_[i + eye * kMaxFramesInFlight];
            vk::WriteDescriptorSet temporal_camera = write;
            temporal_camera.dstSet = *temporal_set;
            device_.handle().updateDescriptorSets(temporal_camera, nullptr);
            write_texture(temporal_set, 1, gi_sampler, gbuffer_.depth(),
                          compat::depthReadOnlyLayout());
            write_texture(temporal_set, 2, gi_sampler, gi_raw_, kRead);
            write_texture(temporal_set, 3, gi_sampler, eye_image(eye, gi_history_, xr_eye1_.gi_history), kGeneral);
            write_texture(temporal_set, 4, gi_sampler,
                          eye_image(eye, gi_moments_history_, xr_eye1_.gi_moments_history), kGeneral);
            write_storage_image(temporal_set, 5, gi_temporal_);
            write_storage_image(temporal_set, 6, gi_moments_);
            write_storage_image(temporal_set, 7, gi_variance_);
        }

        // A trous: la primera pasada es la historia del color (como en SVGF),
        // salvo en las caras de la sonda, que no deben tocarla. Variantes: 0 la
        // vista (ojo izquierdo), 1 la sonda, 2 el ojo derecho de VR.
        for (std::uint32_t variant = 0; variant < 3; ++variant) {
            // Entrada de cada pasada = salida de la anterior: acumulada ->
            // primera salida -> ping-pong entre gi_filter_[1] y [0] -> resultado.
            const VulkanImage& first_output = variant == 1   ? gi_filter_[0]
                                              : variant == 2 ? eye_image(1, gi_history_, xr_eye1_.gi_history)
                                                             : gi_history_;
            std::array<const VulkanImage*, kGiAtrousIterations + 1> colors{};
            std::array<const VulkanImage*, kGiAtrousIterations + 1> variances{};
            for (std::uint32_t k = 0; k <= kGiAtrousIterations; ++k) {
                colors[k] = k == 0                     ? &gi_temporal_
                            : k == 1                   ? &first_output
                            : k == kGiAtrousIterations ? &gi_image_
                                                       : &gi_filter_[k % 2 == 0 ? 1 : 0];
                variances[k] = k == 0 ? &gi_variance_ : &gi_filter_variance_[k % 2 == 1 ? 0 : 1];
            }
            for (std::uint32_t iteration = 0; iteration < kGiAtrousIterations; ++iteration) {
                const vk::raii::DescriptorSet& set =
                    gi_atrous_sets_[(i * 3 + variant) * kGiAtrousIterations + iteration];
                vk::WriteDescriptorSet atrous_camera = write;
                atrous_camera.dstSet = *set;
                device_.handle().updateDescriptorSets(atrous_camera, nullptr);
                write_texture(set, 1, gi_sampler, gbuffer_.depth(),
                              compat::depthReadOnlyLayout());
                write_texture(set, 2, gi_sampler, gbuffer_.normal(), kRead);
                write_texture(set, 3, gi_sampler, *colors[iteration], kGeneral);
                write_texture(set, 4, gi_sampler, *variances[iteration], kGeneral);
                write_storage_image(set, 5, *colors[iteration + 1]);
                write_storage_image(set, 6, *variances[iteration + 1]);
            }
        }

        // --- Filtros de las sombras y los reflejos por rayos ---
        if (device_.rayTracingSupported() && !rt_shadow_temporal_sets_.empty()) {
            const vk::raii::Sampler& rt_sampler = rt_shadow_temporal_pass_.sampler();
            for (std::uint32_t eye = 0; eye < 2; ++eye) {
                // Sombras: mascara de este frame + historia -> acumulada.
                const vk::raii::DescriptorSet& shadow_set = rt_shadow_temporal_sets_[i + eye * kMaxFramesInFlight];
                vk::WriteDescriptorSet shadow_camera = write;
                shadow_camera.dstSet = *shadow_set;
                device_.handle().updateDescriptorSets(shadow_camera, nullptr);
                write_texture(shadow_set, 1, rt_sampler, gbuffer_.depth(), compat::depthReadOnlyLayout());
                write_texture(shadow_set, 2, rt_sampler, gbuffer_.normal(), kRead);
                write_texture(shadow_set, 3, rt_sampler, rt_shadow_mask_, kRead);
                // (Entera: sin filtro lineal.)
                write_texture(shadow_set, 4, rain_sampler_, eye_image(eye, rt_shadow_history_, xr_eye1_.rt_shadow_history),
                              kGeneral);
                write_storage_image(shadow_set, 5, rt_shadow_accum_);

                // Reflejos: el de este frame + historia (color y momentos) ->
                // acumulado y momentos.
                const vk::raii::DescriptorSet& reflection_set =
                    rt_reflection_temporal_sets_[i + eye * kMaxFramesInFlight];
                vk::WriteDescriptorSet reflection_camera = write;
                reflection_camera.dstSet = *reflection_set;
                device_.handle().updateDescriptorSets(reflection_camera, nullptr);
                write_texture(reflection_set, 1, rt_sampler, gbuffer_.depth(), compat::depthReadOnlyLayout());
                write_texture(reflection_set, 2, rt_sampler, gbuffer_.normal(), kRead);
                write_texture(reflection_set, 3, rt_sampler, ssr_raw_, kRead);
                write_texture(reflection_set, 4, rt_sampler,
                              eye_image(eye, rt_reflection_history_, xr_eye1_.rt_reflection_history), kGeneral);
                write_texture(reflection_set, 5, rt_sampler,
                              eye_image(eye, rt_reflection_moments_history_, xr_eye1_.rt_reflection_moments_history),
                              kGeneral);
                write_storage_image(reflection_set, 6, rt_reflection_temporal_);
                write_storage_image(reflection_set, 7, rt_reflection_moments_);
            }
            // A trous: acumulado -> rt_reflection_filter_ -> ssr_image_ (lo lee
            // la iluminacion).
            static_assert(kReflectionAtrousIterations == 2, "el ping-pong de los reflejos es de dos pasadas");
            for (std::uint32_t iteration = 0; iteration < kReflectionAtrousIterations; ++iteration) {
                const vk::raii::DescriptorSet& set = rt_reflection_atrous_sets_[i * kReflectionAtrousIterations + iteration];
                const VulkanImage& input = iteration == 0 ? rt_reflection_temporal_ : rt_reflection_filter_;
                const VulkanImage& output = iteration == 0 ? rt_reflection_filter_ : ssr_image_;
                vk::WriteDescriptorSet atrous_camera = write;
                atrous_camera.dstSet = *set;
                device_.handle().updateDescriptorSets(atrous_camera, nullptr);
                write_texture(set, 1, rt_sampler, gbuffer_.depth(), compat::depthReadOnlyLayout());
                write_texture(set, 2, rt_sampler, gbuffer_.normal(), kRead);
                write_texture(set, 3, rt_sampler, input, kGeneral);
                write_storage_image(set, 4, output);
            }
        }

        // Nubes: camara + ruido 3D (con su muestreador, que repite) + cielo.
        vk::WriteDescriptorSet clouds_camera = write;
        clouds_camera.dstSet = *clouds_sets_[i];
        device_.handle().updateDescriptorSets(clouds_camera, nullptr);
        vk::DescriptorImageInfo noise_info{};
        noise_info.sampler = *cloud_noise_.sampler();
        noise_info.imageView = *cloud_noise_.view();
        noise_info.imageLayout = kRead;
        vk::WriteDescriptorSet noise_write{};
        noise_write.dstSet = *clouds_sets_[i];
        noise_write.dstBinding = 1;
        noise_write.descriptorType = vk::DescriptorType::eCombinedImageSampler;
        noise_write.setImageInfo(noise_info);
        device_.handle().updateDescriptorSets(noise_write, nullptr);
        write_texture(clouds_sets_[i], 2, clouds_pass_.sampler(), sky_lut_, kRead);
    }

    // Bloom: cada nivel de bajada lee el anterior (el primero, la imagen HDR);
    // cada subida lee el nivel mas pequeno y se suma al siguiente.
    for (std::uint32_t level = 0; level < kBloomLevels; ++level) {
        const VulkanImage& source = (level == 0) ? postSource() : bloom_levels_[level - 1];
        write_texture(bloom_down_sets_[level], 0, bloom_down_pass_.sampler(), source, kRead);
    }
    for (std::uint32_t level = 1; level < kBloomLevels; ++level) {
        write_texture(bloom_up_sets_[level - 1], 0, bloom_up_pass_.sampler(),
                      bloom_levels_[level], kRead);
    }

    const auto write_storage = [&](const vk::raii::DescriptorSet& set, std::uint32_t binding,
                                   const VulkanBuffer& buffer) {
        vk::DescriptorBufferInfo info{};
        info.buffer = *buffer.handle();
        info.range = VK_WHOLE_SIZE;

        vk::WriteDescriptorSet write{};
        write.dstSet = *set;
        write.dstBinding = binding;
        write.descriptorType = vk::DescriptorType::eStorageBuffer;
        write.setBufferInfo(info);
        device_.handle().updateDescriptorSets(write, nullptr);
    };

    for (std::uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
        write_texture(composite_sets_[i], 0, composite_pass_.sampler(), postSource(), kRead);
        write_texture(composite_sets_[i], 1, composite_pass_.sampler(), bloom_levels_[0], kRead);
        write_texture(composite_sets_[i], 2, composite_pass_.sampler(), light_shafts_, kRead);
        write_storage(composite_sets_[i], 3, exposure_buffer_);

        vk::DescriptorBufferInfo settings_info{};
        settings_info.buffer = *composite_buffers_[i].handle();
        settings_info.range = sizeof(GpuCompositeSettings);
        vk::WriteDescriptorSet settings_write{};
        settings_write.dstSet = *composite_sets_[i];
        settings_write.dstBinding = 4;
        settings_write.descriptorType = vk::DescriptorType::eUniformBuffer;
        settings_write.setBufferInfo(settings_info);
        device_.handle().updateDescriptorSets(settings_write, nullptr);
    }

    write_texture(outline_sets_[0], 0, outline_pass_.sampler(), outline_mask_, kRead);

    write_texture(light_shaft_sets_[0], 0, light_shaft_pass_.sampler(), gbuffer_.depth(),
                  compat::depthReadOnlyLayout());
    write_texture(light_shaft_sets_[0], 1, light_shaft_pass_.sampler(), postSource(), kRead);
    write_texture(camera_fx_sets_[0], 0, camera_fx_pass_.sampler(), camera_fx_source_, kRead);
    write_texture(camera_fx_sets_[0], 1, camera_fx_pass_.sampler(), gbuffer_.depth(),
                  compat::depthReadOnlyLayout());
    write_texture(camera_fx_sets_[0], 2, camera_fx_pass_.sampler(), gbuffer_.velocity(), kRead);

    write_texture(histogram_sets_[0], 0, histogram_pass_.sampler(), postSource(), kRead);

    // Escalado: la escena (interna), su profundidad y movimiento y la historia
    // (la de cada ojo de VR).
    for (std::uint32_t eye = 0; eye < 2; ++eye) {
        write_texture(taa_sets_[eye], 0, taa_pass_.sampler(), scene_color_, kRead);
        write_texture(taa_sets_[eye], 1, taa_pass_.sampler(), gbuffer_.depth(), compat::depthReadOnlyLayout());
        write_texture(taa_sets_[eye], 2, taa_pass_.sampler(), gbuffer_.velocity(), kRead);
        write_texture(taa_sets_[eye], 3, taa_pass_.sampler(), eye_image(eye, taa_history_, xr_eye1_.taa_history), kRead);
    }
    write_texture(upscaler_motion_sets_[0], 0, upscaler_motion_pass_.sampler(), gbuffer_.depth(),
                  compat::depthReadOnlyLayout());
    write_texture(upscaler_motion_sets_[0], 1, upscaler_motion_pass_.sampler(), gbuffer_.velocity(), kRead);
    write_texture(easu_sets_[0], 0, easu_pass_.sampler(), scene_color_, kRead);
    write_texture(rcas_sets_[0], 0, rcas_pass_.sampler(), upscale_target_, kRead);
    write_storage(histogram_sets_[0], 1, histogram_buffer_);

    write_storage(exposure_average_sets_[0], 0, histogram_buffer_);
    write_storage(exposure_average_sets_[0], 1, exposure_buffer_);
}

vk::Extent2D VulkanRenderer::outputExtent() const {
    if (xr_output_extent_.width > 0 && xr_output_extent_.height > 0) return xr_output_extent_;
    if (view_extent_request_.width > 0 && view_extent_request_.height > 0) return view_extent_request_;
    return swapchain_.extent();
}

void VulkanRenderer::setViewExtent(std::uint32_t width, std::uint32_t height) {
    vk::Extent2D request{0, 0};
    if (width > 0 && height > 0) request = vk::Extent2D{std::clamp(width, 16u, 8192u), std::clamp(height, 16u, 8192u)};
    if (request == view_extent_request_) return;
    view_extent_request_ = request;
    targets_dirty_ = true;
}

// Solo los destinos de la escena (otro tamano de vista): la swapchain se queda.
void VulkanRenderer::recreateRenderTargets() {
    targets_dirty_ = false;
    if (!swapchain_.isValid()) return;
    device_.waitIdle();
    computeRenderExtent();
    gbuffer_.create(device_, render_extent_);
    createRenderTargets();
    updateLightingDescriptors();
    updatePostDescriptors();
    taa_history_valid_ = false;
    jitter_index_ = 0;
    invalidateHistory();
    ++scene_image_generation_;
}

void VulkanRenderer::computeRenderExtent() {
    const vk::Extent2D output = outputExtent();
    const float scale = renderScale(graphics_);
    render_extent_ = vk::Extent2D{
        std::max(1u, static_cast<std::uint32_t>(std::lround(static_cast<float>(output.width) * scale))),
        std::max(1u, static_cast<std::uint32_t>(std::lround(static_cast<float>(output.height) * scale)))};
    upscaling_ = graphics_.upscaler != Upscaler::Off;
}

std::uint32_t VulkanRenderer::desiredShadowResolution() const {
    if (user_graphics_.shadow_resolution > 0) {
        return static_cast<std::uint32_t>(std::clamp(user_graphics_.shadow_resolution, 512, 8192));
    }
    // Movil: la imagen del juego es de 720-1080 lineas y cada cascada se
    // escribe entera en memoria (en una GPU de movil, lo caro): 1024 en Baja
    // y Media (4 MB por cascada en vez de 9), 1536 en Alta, 2048 en Ultra.
    if (mobile_level_ >= 0) return mobile_level_ <= 1 ? 1024u : mobile_level_ == 2 ? 1536u : 2048u;
    return shadowResolutionFor(hardware_.tier);
}

std::uint32_t VulkanRenderer::desiredTextureSize() const {
    if (user_graphics_.texture_max_size > 0) {
        return static_cast<std::uint32_t>(std::clamp(user_graphics_.texture_max_size, 256, 16384));
    }
    // Movil: la memoria es la del telefono entero (2-4 GB en la gama baja) y
    // sin BC las texturas van sin comprimir. Una de 2048 son 21 MB con mips;
    // de 1024, 5. En una imagen de 720 lineas apenas se distinguen.
    if (mobile_level_ >= 0) return mobile_level_ <= 1 ? 1024u : 2048u;
    return textureSizeFor(hardware_.tier);
}

void VulkanRenderer::setGraphicsSettings(const GraphicsSettings& settings) {
    const bool changed = !(settings == user_graphics_);
    user_graphics_ = settings;
    asset::setMaxTextureSize(desiredTextureSize());
    budget_.setEnabled(settings.adaptive);
    budget_.setTargetFps(settings.target_fps);
    // Lo que el presupuesto adaptativo ya habia bajado (resolucion, efectos)
    // seguia bajado con los ajustes nuevos hasta reiniciar: se vuelve a medir.
    if (changed) budget_.reset();
    // Otro mapa de sombras: se rehace con los destinos (applyPendingResize).
    if (initialized_ && desiredShadowResolution() != shadow_map_.resolution()) {
        shadow_map_dirty_ = true;
        settings_dirty_ = true;
    }
    applyEffectiveGraphics();
}

// Los ajustes en uso: los del usuario con la resolucion interna que pida el
// presupuesto (con FSR 1 si el usuario no tenia escalador).
void VulkanRenderer::applyEffectiveGraphics() {
    GraphicsSettings settings = user_graphics_;
    // VR con una camara por ojo: FSR 3 y DLSS guardan la historia de una sola
    // vista (mezclarian los ojos); el TAA del motor la lleva por ojo.
    xr_forced_taa_ = ((xr_output_extent_.width > 0 && xr_.stereo()) || stereo_emulation_) &&
                     (settings.upscaler == Upscaler::Fsr3 || settings.upscaler == Upscaler::Dlss);
    if (xr_forced_taa_) settings.upscaler = Upscaler::Taa;
    const float budget_scale = budget_.renderScale();
    applied_budget_scale_ = budget_scale;
    if (budget_scale < 0.999f) {
        const float scale = renderScale(user_graphics_) * budget_scale;
        if (settings.upscaler == Upscaler::Off) settings.upscaler = Upscaler::Fsr1;
        settings.quality = UpscaleQuality::Custom;
        settings.custom_scale = std::clamp(scale, 0.25f, 1.0f);
    }
    // VR: el ojo del casco (2528x2704 en Quest 3) son 3,3 veces los pixeles de
    // 1080p y cada destino (G-buffer, GI, reflejos, historias por ojo) va a ese
    // tamano: con 8 GB la escena ya no cabia (ErrorOutOfDeviceMemory al subir
    // sus texturas). Con menos de 12 GB, como mucho ~1,5 veces 1080p por ojo.
    if (xr_output_extent_.width > 0 && xr_output_extent_.height > 0 && hardware_.vram_mb > 0 &&
        hardware_.vram_mb < 12 * 1024) {
        constexpr double kMaxEyePixels = 1920.0 * 1080.0 * 1.5;
        const double eye_pixels = static_cast<double>(xr_output_extent_.width) * xr_output_extent_.height;
        const float cap = static_cast<float>(std::sqrt(std::min(1.0, kMaxEyePixels / eye_pixels)));
        if (renderScale(settings) > cap + 0.001f) {
            if (settings.upscaler == Upscaler::Off) settings.upscaler = Upscaler::Fsr1;
            settings.quality = UpscaleQuality::Custom;
            settings.custom_scale = std::clamp(cap, 0.25f, 1.0f);
        }
    }
    if (settings == graphics_) return;
    const bool vsync_changed = settings.vsync != graphics_.vsync;
    const bool rebuild = renderScale(settings) != renderScale(graphics_) || settings.upscaler != graphics_.upscaler ||
                         settings.quality != graphics_.quality;
    graphics_ = settings;
    swapchain_.setVsync(settings.vsync);
    taa_history_valid_ = false;
    xr_eye1_.taa_valid = false;
    jitter_index_ = 0;
    // Destinos de otro tamano (o el post-proceso lee otra imagen): se rehacen
    // al empezar el frame siguiente (applyPendingResize). La swapchain solo
    // con otro vsync: rehacerla en cada paso del presupuesto adaptativo daba
    // tirones (y con FSR 3 / DLSS, que ademas rehacian su contexto, mas lag).
    if (vsync_changed) settings_dirty_ = true;
    if (rebuild) targets_dirty_ = true;
}

void VulkanRenderer::createRenderTargets() {
    // La escena se dibuja a la resolucion interna; lo que va despues del
    // escalado (bloom, composicion, gizmos, vistas), a la de salida.
    const vk::Extent2D extent = render_extent_;
    const vk::Extent2D output = outputExtent();
    const vk::ImageUsageFlags target_usage =
        vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled;

    // (Origen de copia: el vidrio lee una copia sin si mismo, glass_source_.)
    // Storage: el path tracing escribe en ella (path_trace.comp).
    scene_color_.create(device_, extent, kHdrFormat,
                        target_usage | vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eStorage,
                        vk::ImageAspectFlagBits::eColor);
    glass_source_.create(device_, extent, kHdrFormat,
                         vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
                         vk::ImageAspectFlagBits::eColor);
    // Profundidad propia del agua (copia de la de la escena + las olas).
    water_depth_.create(device_, extent, device_.depthFormat(),
                        vk::ImageUsageFlagBits::eDepthStencilAttachment | vk::ImageUsageFlagBits::eTransferDst,
                        vk::ImageAspectFlagBits::eDepth);
    // Que tengan un layout valido aunque el primer frame no tenga vidrio. La
    // imagen HDR tambien: SSGI y SSR la tienen enlazada como color del frame
    // anterior y el primer frame tras crearla (abrir un proyecto, cambiar el
    // tamano de la vista, un Render Texture) aun no se escribio.
    device_.submitOneTime([&](const vk::raii::CommandBuffer& cmd) {
        pipelineBarrier(cmd, colorBarrier(*scene_color_.handle(), vk::ImageLayout::eUndefined,
                                          vk::ImageLayout::eShaderReadOnlyOptimal,
                                          vk::PipelineStageFlagBits2::eTopOfPipe,
                                          vk::AccessFlagBits2::eNone,
                                          vk::PipelineStageFlagBits2::eFragmentShader,
                                          vk::AccessFlagBits2::eShaderSampledRead));
        pipelineBarrier(cmd, colorBarrier(*glass_source_.handle(), vk::ImageLayout::eUndefined,
                                          vk::ImageLayout::eShaderReadOnlyOptimal,
                                          vk::PipelineStageFlagBits2::eTopOfPipe,
                                          vk::AccessFlagBits2::eNone,
                                          vk::PipelineStageFlagBits2::eFragmentShader,
                                          vk::AccessFlagBits2::eShaderSampledRead));
    });
    ldr_color_.create(device_, output, kLdrFormat, target_usage | vk::ImageUsageFlagBits::eTransferSrc,
                      vk::ImageAspectFlagBits::eColor);
    // Mascara del contorno de seleccion (R = silueta, G = visible). Se deja
    // como textura desde el principio: el set del contorno la referencia.
    outline_mask_.create(device_, output, SkinnedPass::kOutlineMaskFormat, target_usage,
                         vk::ImageAspectFlagBits::eColor);
    device_.submitOneTime([&](const vk::raii::CommandBuffer& cmd) {
        pipelineBarrier(cmd, colorBarrier(*outline_mask_.handle(), vk::ImageLayout::eUndefined,
                                          vk::ImageLayout::eShaderReadOnlyOptimal,
                                          vk::PipelineStageFlagBits2::eTopOfPipe,
                                          vk::AccessFlagBits2::eNone,
                                          vk::PipelineStageFlagBits2::eFragmentShader,
                                          vk::AccessFlagBits2::eShaderSampledRead));
    });
    ssao_image_.create(device_, extent, kSsaoFormat, target_usage,
                       vk::ImageAspectFlagBits::eColor);
    // Vistas del editor: copias del resultado (texturas de ImGui). Se dejan
    // como textura desde el principio (una vista que aun no se dibujo).
    for (VulkanImage& image : view_images_) {
        image.create(device_, output, kLdrFormat,
                     vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
                     vk::ImageAspectFlagBits::eColor);
        device_.submitOneTime([&](const vk::raii::CommandBuffer& cmd) {
            pipelineBarrier(cmd, colorBarrier(*image.handle(), vk::ImageLayout::eUndefined,
                                              vk::ImageLayout::eShaderReadOnlyOptimal,
                                              vk::PipelineStageFlagBits2::eTopOfPipe, vk::AccessFlagBits2::eNone,
                                              vk::PipelineStageFlagBits2::eFragmentShader,
                                              vk::AccessFlagBits2::eShaderSampledRead));
        });
    }
    // Picking por ID: se dibuja un pixel y se copia a un buffer de lectura.
    pick_ids_.create(device_, extent, SkinnedPass::kPickFormat,
                     vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferSrc,
                     vk::ImageAspectFlagBits::eColor);
    if (pick_buffers_.empty()) {
        pick_buffers_.resize(kMaxFramesInFlight);
        for (VulkanBuffer& buffer : pick_buffers_) {
            buffer.create(device_, sizeof(std::uint32_t), vk::BufferUsageFlagBits::eTransferDst,
                          vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
        }
    }
    pick_in_flight_.fill(false);  // tamano nuevo: los picks en vuelo ya no valen

    for (std::uint32_t level = 0; level < kBloomLevels; ++level) {
        bloom_levels_[level].create(device_, bloomLevelExtent(output, level), kHdrFormat,
                                    target_usage, vk::ImageAspectFlagBits::eColor);
    }

    // Los rayos son un desenfoque muy amplio: media resolucion basta.
    light_shafts_.create(device_, bloomLevelExtent(output, 0), kHdrFormat, target_usage,
                         vk::ImageAspectFlagBits::eColor);
    light_shafts_ready_ = false;
    camera_fx_source_.create(device_, output, kHdrFormat,
                             vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
                             vk::ImageAspectFlagBits::eColor);

    // Los reflejos del suelo tienen que ser nitidos: resolucion completa.
    // (Storage: con trazado de rayos las escribe un compute shader.)
    ssr_raw_.create(device_, extent, kHdrFormat,
                    target_usage | vk::ImageUsageFlagBits::eStorage,
                    vk::ImageAspectFlagBits::eColor);
    // (Storage: con trazado de rayos la escribe la ultima pasada de su filtro.)
    ssr_image_.create(device_, extent, kHdrFormat,
                      target_usage | vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eStorage,
                      vk::ImageAspectFlagBits::eColor);
    ssr_history_.create(device_, extent, kHdrFormat,
                        vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
                        vk::ImageAspectFlagBits::eColor);
    ssr_filter_history_valid_ = false;
    ssr_zeroed_ = false;

    // Sombras por rayos (sol y luces locales): resolucion completa (bordes
    // nitidos). Sin trazado no se usan: 1 x 1 (antes ~30 MB a 1440p siempre).
    rt_targets_key_ = rayTargetsKey();
    const vk::Extent2D rt_extent = (rt_targets_key_ & 1u) != 0 ? extent : vk::Extent2D{1, 1};
    rt_shadow_mask_.create(device_, rt_extent, vk::Format::eR16G16B16A16Sfloat,
                           vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eStorage,
                           vk::ImageAspectFlagBits::eColor);
    // Su filtro temporal y el de los reflejos por rayos (mismo tamano: 1 x 1
    // sin trazado). Todas viven en General y empiezan a cero.
    const vk::ImageUsageFlags rt_filter_usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
                                                vk::ImageUsageFlagBits::eTransferSrc |
                                                vk::ImageUsageFlagBits::eTransferDst;
    rt_shadow_accum_.create(device_, rt_extent, vk::Format::eR16G16B16A16Uint, rt_filter_usage,
                            vk::ImageAspectFlagBits::eColor);
    rt_shadow_history_.create(device_, rt_extent, vk::Format::eR16G16B16A16Uint, rt_filter_usage,
                              vk::ImageAspectFlagBits::eColor);
    for (VulkanImage* image : {&rt_reflection_temporal_, &rt_reflection_moments_, &rt_reflection_history_,
                               &rt_reflection_moments_history_, &rt_reflection_filter_}) {
        image->create(device_, rt_extent, kHdrFormat, rt_filter_usage, vk::ImageAspectFlagBits::eColor);
    }
    device_.submitOneTime([&](const vk::raii::CommandBuffer& cmd) {
        std::vector<vk::ImageMemoryBarrier2> barriers = {
            colorBarrier(*rt_shadow_mask_.handle(), vk::ImageLayout::eUndefined, vk::ImageLayout::eShaderReadOnlyOptimal,
                         vk::PipelineStageFlagBits2::eTopOfPipe, vk::AccessFlagBits2::eNone,
                         vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderSampledRead)};
        const std::array<const VulkanImage*, 7> general = {&rt_shadow_accum_,       &rt_shadow_history_,
                                                           &rt_reflection_temporal_, &rt_reflection_moments_,
                                                           &rt_reflection_history_,  &rt_reflection_moments_history_,
                                                           &rt_reflection_filter_};
        for (const VulkanImage* image : general) {
            barriers.push_back(colorBarrier(*image->handle(), vk::ImageLayout::eUndefined, vk::ImageLayout::eGeneral,
                                            vk::PipelineStageFlagBits2::eTopOfPipe, vk::AccessFlagBits2::eNone,
                                            vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite));
        }
        pipelineBarrier(cmd, barriers);
        const vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
        for (const VulkanImage* image : general) {
            if (image == &rt_shadow_accum_ || image == &rt_shadow_history_) {
                cmd.clearColorImage(*image->handle(), vk::ImageLayout::eGeneral,
                                    vk::ClearColorValue{std::array<std::uint32_t, 4>{0u, 0u, 0u, 0u}}, range);
            } else {
                cmd.clearColorImage(*image->handle(), vk::ImageLayout::eGeneral, vk::ClearColorValue{0.0f, 0.0f, 0.0f, 0.0f},
                                    range);
            }
        }
        // Lo que lee la iluminacion (la mascara acumulada) desde el primer frame.
        memoryBarrier(cmd, vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite,
                      vk::PipelineStageFlagBits2::eFragmentShader | vk::PipelineStageFlagBits2::eComputeShader,
                      vk::AccessFlagBits2::eShaderSampledRead | vk::AccessFlagBits2::eShaderStorageRead);
    });
    rt_shadow_history_valid_ = false;
    rt_reflection_history_valid_ = false;

    // La luz volumetrica tambien es suave: media resolucion. Se deja como
    // textura desde el principio (las caras de la sonda no la dibujan).
    volumetric_image_.create(device_, bloomLevelExtent(extent, 0), kHdrFormat, target_usage,
                             vk::ImageAspectFlagBits::eColor);
    device_.submitOneTime([&](const vk::raii::CommandBuffer& cmd) {
        pipelineBarrier(cmd, colorBarrier(*volumetric_image_.handle(), vk::ImageLayout::eUndefined,
                                          vk::ImageLayout::eShaderReadOnlyOptimal,
                                          vk::PipelineStageFlagBits2::eTopOfPipe,
                                          vk::AccessFlagBits2::eNone,
                                          vk::PipelineStageFlagBits2::eFragmentShader,
                                          vk::AccessFlagBits2::eShaderSampledRead));
    });

    // Las nubes son suaves: media resolucion.
    clouds_image_.create(device_, bloomLevelExtent(extent, 0), kHdrFormat, target_usage,
                         vk::ImageAspectFlagBits::eColor);

    // Suma de caminos del path tracing (resolucion interna completa, 16 bytes
    // por pixel: solo con el path tracing encendido).
    if (device_.rayTracingSupported()) {
        const vk::Extent2D path_extent = (rt_targets_key_ & 2u) != 0 ? extent : vk::Extent2D{1, 1};
        path_tracing_accumulation_.create(device_, path_extent, vk::Format::eR32G32B32A32Sfloat,
                                          vk::ImageUsageFlagBits::eStorage, vk::ImageAspectFlagBits::eColor);
        path_tracing_layout_ready_ = false;
        path_tracing_reset_ = true;
    }

    // La luz rebotada es de baja frecuencia: media resolucion.
    gi_raw_.create(device_, bloomLevelExtent(extent, 0), kHdrFormat,
                   target_usage | vk::ImageUsageFlagBits::eStorage,
                   vk::ImageAspectFlagBits::eColor);
    // Las del filtro SVGF: storage (compute) y copia, siempre en General.
    {
        const vk::ImageUsageFlags filter_usage =
            vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
            vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst;
        const vk::Extent2D half = bloomLevelExtent(extent, 0);
        std::vector<VulkanImage*> filter_images = {&gi_image_,   &gi_history_, &gi_temporal_,
                                                   &gi_variance_, &gi_moments_,
                                                   &gi_moments_history_};
        for (std::uint32_t i = 0; i < 2; ++i) {
            filter_images.push_back(&gi_filter_[i]);
            filter_images.push_back(&gi_filter_variance_[i]);
        }
        for (VulkanImage* image : filter_images) {
            image->create(device_, half, kHdrFormat, filter_usage,
                          vk::ImageAspectFlagBits::eColor);
        }
        device_.submitOneTime([&](const vk::raii::CommandBuffer& cmd) {
            std::vector<vk::ImageMemoryBarrier2> to_general;
            for (VulkanImage* image : filter_images) {
                to_general.push_back(colorBarrier(
                    *image->handle(), vk::ImageLayout::eUndefined, vk::ImageLayout::eGeneral,
                    vk::PipelineStageFlagBits2::eTopOfPipe, vk::AccessFlagBits2::eNone,
                    vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite));
            }
            pipelineBarrier(cmd, to_general);
            for (VulkanImage* image : filter_images) {
                cmd.clearColorImage(*image->handle(), vk::ImageLayout::eGeneral,
                                    vk::ClearColorValue{0.0f, 0.0f, 0.0f, 0.0f},
                                    vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0,
                                                              1, 0, 1});
            }
        });
    }
    gi_filter_history_valid_ = false;

    // Caras de la sonda de reflexion: se dibujan a pantalla completa y se
    // copia el cuadrado central.
    probe_capture_.create(device_, extent, kHdrFormat,
                          vk::ImageUsageFlagBits::eColorAttachment |
                              vk::ImageUsageFlagBits::eTransferSrc,
                          vk::ImageAspectFlagBits::eColor);

    // Piramide Hi-Z del culling, del tamano del depth buffer.
    gpu_culling_.resize(device_, gbuffer_.depth());

    // --- Escalado (a la resolucion de pantalla) ---
    upscale_target_.create(device_, output, kHdrFormat,
                           target_usage | vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eStorage,
                           vk::ImageAspectFlagBits::eColor);
    upscaler_motion_.create(device_, render_extent_, GBuffer::kVelocityFormat,
                            vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled,
                            vk::ImageAspectFlagBits::eColor);
    // Origen de copia: el motion blur y la profundidad de campo la copian.
    upscaled_color_.create(device_, output, kHdrFormat, target_usage | vk::ImageUsageFlagBits::eTransferSrc,
                           vk::ImageAspectFlagBits::eColor);
    // FSR 3 / DLSS con los tamanos nuevos (solo se rehacen si hace falta).
    configureVendorUpscaler();
    taa_history_.create(device_, output, kHdrFormat,
                        vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
                        vk::ImageAspectFlagBits::eColor);
    output_depth_.create(device_, output, gbuffer_.depthFormat(),
                         vk::ImageUsageFlagBits::eDepthStencilAttachment | vk::ImageUsageFlagBits::eTransferDst,
                         vk::ImageAspectFlagBits::eDepth);
    const vk::FormatFeatureFlags depth_features =
        device_.physicalDevice().getFormatProperties(gbuffer_.depthFormat()).optimalTilingFeatures;
    output_depth_blit_ = (depth_features & vk::FormatFeatureFlagBits::eBlitSrc) &&
                         (depth_features & vk::FormatFeatureFlagBits::eBlitDst);
    device_.submitOneTime([&](const vk::raii::CommandBuffer& cmd) {
        pipelineBarrier(cmd, {colorBarrier(*taa_history_.handle(), vk::ImageLayout::eUndefined,
                                           vk::ImageLayout::eShaderReadOnlyOptimal,
                                           vk::PipelineStageFlagBits2::eTopOfPipe, vk::AccessFlagBits2::eNone,
                                           vk::PipelineStageFlagBits2::eFragmentShader,
                                           vk::AccessFlagBits2::eShaderSampledRead),
                              colorBarrier(*upscaled_color_.handle(), vk::ImageLayout::eUndefined,
                                           vk::ImageLayout::eShaderReadOnlyOptimal,
                                           vk::PipelineStageFlagBits2::eTopOfPipe, vk::AccessFlagBits2::eNone,
                                           vk::PipelineStageFlagBits2::eFragmentShader,
                                           vk::AccessFlagBits2::eShaderSampledRead),
                              colorBarrier(*upscale_target_.handle(), vk::ImageLayout::eUndefined,
                                           vk::ImageLayout::eShaderReadOnlyOptimal,
                                           vk::PipelineStageFlagBits2::eTopOfPipe, vk::AccessFlagBits2::eNone,
                                           vk::PipelineStageFlagBits2::eFragmentShader,
                                           vk::AccessFlagBits2::eShaderSampledRead)});
    });
    taa_history_valid_ = false;
    createXrEyeTargets(extent, output);
    std::cout << "[Vulkan] Resolucion interna " << extent.width << "x" << extent.height << " -> pantalla "
              << output.width << "x" << output.height << "\n";

    // La imagen HDR es nueva: no tiene un frame anterior que reutilizar.
    scene_history_valid_ = false;
}

void VulkanRenderer::createCommandObjects() {
    vk::CommandPoolCreateInfo pool_info{};
    pool_info.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
    pool_info.queueFamilyIndex = device_.queueFamilies().graphics;

    command_pool_ = vk::raii::CommandPool(device_.handle(), pool_info);

    vk::CommandBufferAllocateInfo alloc_info{};
    alloc_info.commandPool = *command_pool_;
    alloc_info.level = vk::CommandBufferLevel::ePrimary;
    alloc_info.commandBufferCount = kMaxFramesInFlight;

    command_buffers_ = vk::raii::CommandBuffers(device_.handle(), alloc_info);
}

void VulkanRenderer::createSyncObjects() {
    const vk::SemaphoreCreateInfo semaphore_info{};
    vk::FenceCreateInfo fence_info{};
    fence_info.flags = vk::FenceCreateFlagBits::eSignaled;

    image_available_.clear();
    in_flight_fences_.clear();
    render_finished_.clear();

    image_available_.reserve(kMaxFramesInFlight);
    in_flight_fences_.reserve(kMaxFramesInFlight);
    for (std::uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
        image_available_.emplace_back(device_.handle(), semaphore_info);
        in_flight_fences_.emplace_back(device_.handle(), fence_info);
    }

    render_finished_.reserve(swapchain_.imageCount());
    for (std::uint32_t i = 0; i < swapchain_.imageCount(); ++i) {
        render_finished_.emplace_back(device_.handle(), semaphore_info);
    }
}

// -----------------------------------------------------------------------------
// Swapchain
// -----------------------------------------------------------------------------

void VulkanRenderer::clearModels() {
    finishModelJobs();
    device_.waitIdle();
    skinned_models_.clear();
    retired_models_.clear();
    ray_pinned_models_.clear();

    // Lo que dependia de la escena anterior se rehace: el mapa de lluvia
    // (se dibuja una vez), las cascadas guardadas y las sombras locales.
    rain_map_ready_ = false;
    cascades_valid_ = false;
    local_shadows_.invalidate();
    triangle_count_ = 0;

    // La escena de los rayos se rehace la proxima vez que se use (con el
    // trazado apagado no se construye nada). La vieja era de otros modelos.
    if (ray_tracing_.sceneBuilt()) ray_tracing_.releaseScene();
    rt_scene_dirty_ = true;
    rt_model_edit_pending_ = false;
    ray_traced_models_ = 0;
    ray_pinned_.clear();
    model_evicted_.clear();
    model_last_seen_.clear();
}

void VulkanRenderer::uploadModels(const scene::Scene& scene) {
    if (!initialized_) {
        throw std::runtime_error("uploadModels() antes de inicializar el renderizador.");
    }
    const auto upload_start = std::chrono::steady_clock::now();
    ++frame_timings_.uploads;

    clearModels();
    skinned_models_.reserve(scene.models().size());
    for (const auto& model : scene.models()) {
        skinned_models_.emplace_back().create(device_, *model, skinned_pass_);
        bindRenderTextures(static_cast<std::uint32_t>(skinned_models_.size() - 1));
        triangle_count_ += model->indices.size() / 3;
    }
    ray_pinned_.assign(skinned_models_.size(), false);
    model_evicted_.assign(skinned_models_.size(), false);
    model_last_seen_.assign(skinned_models_.size(), std::chrono::steady_clock::now());
    frame_timings_.upload_ms +=
        std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - upload_start).count();
}

void VulkanRenderer::uploadNewModels(const scene::Scene& scene) {
    if (!initialized_) return;
    // Menos modelos que los subidos: se vacio la escena (se abrio otra). Se
    // suelta lo de antes (sin subir nada: lo nuevo va por los hilos).
    if (skinned_models_.size() > scene.models().size()) clearModels();

    // Lo que ya esta listo entra, en orden (los indices son los de la escena).
    for (;;) {
        const auto next = static_cast<std::uint32_t>(skinned_models_.size());
        const auto it = std::find_if(model_jobs_.begin(), model_jobs_.end(),
                                     [&](const ModelJob& job) { return job.append && job.index == next; });
        if (it == model_jobs_.end() ||
            it->result.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            break;
        }
        std::unique_ptr<SkinnedModel> fresh = it->result.get();
        model_jobs_.erase(it);
        // Si fallo, un hueco vacio (no se dibuja) para no frenar a los demas.
        installModel(scene, next, fresh ? std::move(*fresh) : SkinnedModel{}, false);
    }

    // Los siguientes, a los hilos (unos pocos a la vez: cada uno es un modelo
    // entero en memoria). Con streaming de texturas entran con un cuarto de
    // lado (lod 2) y el streaming sube el detalle de lo que se ve grande
    // mientras quede VRAM: subirlo todo a tope llenaba la GPU (una casa con
    // 240 texturas de 4K son 21 GB sin comprimir) y fallaban las subidas.
    constexpr std::size_t kMaxAppendJobs = 3;
    const int first_lod = texture_streaming_ ? 2 : 0;
    std::size_t in_flight = static_cast<std::size_t>(
        std::count_if(model_jobs_.begin(), model_jobs_.end(), [](const ModelJob& job) { return job.append; }));
    const auto total = static_cast<std::uint32_t>(scene.models().size());
    for (auto i = static_cast<std::uint32_t>(skinned_models_.size()); i < total && in_flight < kMaxAppendJobs; ++i) {
        if (modelJobPending(i)) continue;
        launchModelJob(scene, i, first_lod, false, true);
        ++in_flight;
    }
}

void VulkanRenderer::uploadModel(const scene::Scene& scene, std::uint32_t index) {
    if (!initialized_ || index >= scene.models().size()) return;
    // Un hilo subia este mismo modelo: lo que traiga ya no vale.
    if (modelJobPending(index)) finishModelJobs(index, index + 1);
    // Faltan otros antes que el, o lo subido es de otra escena (se vacio y
    // se empezo otra): todo de nuevo.
    if (index > skinned_models_.size() || skinned_models_.size() > scene.models().size()) {
        uploadModels(scene);
        return;
    }
    const auto upload_start = std::chrono::steady_clock::now();
    ++frame_timings_.uploads;
    SkinnedModel fresh;
    fresh.create(device_, *scene.models()[index], skinned_pass_, upload_texture_lod_);
    installModel(scene, index, std::move(fresh), streaming_restore_);
    frame_timings_.upload_ms +=
        std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - upload_start).count();
}

void VulkanRenderer::installModel(const scene::Scene& scene, std::uint32_t index, SkinnedModel fresh, bool restore) {
    if (index > skinned_models_.size()) return;
    // Los rayos lo veran cuando se rehaga su escena (al rato, por si se sigue
    // editando). Volver a la GPU tras el streaming no es un cambio: la escena de
    // rayos conserva su copia, salvo que se construyera mientras estaba fuera.
    const bool rt_missing = index < rt_scene_evicted_.size() && rt_scene_evicted_[index];
    if (ray_tracing_.sceneBuilt() && (!restore || rt_missing)) {
        rt_model_edit_pending_ = true;
        rt_model_edit_time_ = std::chrono::steady_clock::now();
    }
    if (model_evicted_.size() < skinned_models_.size()) model_evicted_.resize(skinned_models_.size(), false);
    if (model_last_seen_.size() < skinned_models_.size() + 1) {
        model_last_seen_.resize(skinned_models_.size() + 1, std::chrono::steady_clock::now());
    }
    model_last_seen_[index] = std::chrono::steady_clock::now();
    if (index == skinned_models_.size()) {
        skinned_models_.push_back(std::move(fresh));
        model_evicted_.push_back(false);
        bindRenderTextures(index);
    } else {
        model_evicted_[index] = false;
        if (index < ray_traced_models_ && index < ray_pinned_.size() && !ray_pinned_[index]) {
            ray_pinned_[index] = true;
            ray_pinned_models_.push_back(std::move(skinned_models_[index]));
        } else {
            retired_models_.push_back(RetiredModel{std::move(skinned_models_[index]), kMaxFramesInFlight + 1});
        }
        skinned_models_[index] = std::move(fresh);
        bindRenderTextures(index);
    }
    triangle_count_ = 0;
    for (const auto& model : scene.models()) triangle_count_ += model->indices.size() / 3;
    // Sombras cacheadas: solo si cambio de verdad (el streaming sube la misma
    // malla; rehacer todas las sombras por eso era otro tiron).
    if (!restore) staticGeometryChanged();
}

bool VulkanRenderer::loadEnvironment(const std::filesystem::path& path) {
    device_.waitIdle();
    if (!environment_.load(device_, path)) {
        return false;
    }
    ibl_probe_.setEnvironment(device_, *environment_.view(), *environment_.sampler());
    updateLightingDescriptors();
    ++environment_generation_;  // otra foto: el IBL se rehace
    return true;
}

void VulkanRenderer::onResize(std::uint32_t width, std::uint32_t height) {
    if (width == window_width_ && height == window_height_) {
        return;
    }
    window_width_ = width;
    window_height_ = height;
    framebuffer_resized_ = true;
}

void VulkanRenderer::releaseSurface() {
    if (!initialized_) return;
    device_.waitIdle();
    swapchain_.recreate(0, 0);  // libera las imagenes y la swapchain
    surface_.shutdown();
    window_width_ = window_height_ = 0;
    framebuffer_resized_ = false;
}

void VulkanRenderer::replaceWindow(NativeWindow window, std::uint32_t width, std::uint32_t height) {
    if (!initialized_ || window == nullptr) return;
    device_.waitIdle();
    swapchain_.recreate(0, 0);
    surface_.shutdown();
    surface_.initialize(instance_, window);
    window_width_ = width;
    window_height_ = height;
    framebuffer_resized_ = true;  // el siguiente frame rehace swapchain y destinos
}

// Los arrays de sombra de focos y puntuales, a tamano completo solo mientras
// hay luces de ese tipo con sombra: crecen al frame siguiente de aparecer una
// y se sueltan tras unos segundos sin ninguna (sin rehacerlos cada vez que
// una luz se apaga un momento).
void VulkanRenderer::requestLocalShadowMaps() {
    bool spots = false;
    bool points = false;
    for (const scene::SpotShadow& spot : local_shadows_.spots()) spots = spots || spot.active;
    for (const scene::PointShadow& point : local_shadows_.points()) points = points || point.active();
    if (spots) spot_shadow_maps_used_frame_ = frame_count_;
    if (points) point_shadow_maps_used_frame_ = frame_count_;
    constexpr std::uint64_t kReleaseFrames = 600;
    const bool keep_spots = spots || (local_shadow_maps_.spotsAllocated() &&
                                      frame_count_ - spot_shadow_maps_used_frame_ < kReleaseFrames);
    const bool keep_points = points || (local_shadow_maps_.pointsAllocated() &&
                                        frame_count_ - point_shadow_maps_used_frame_ < kReleaseFrames);
    if (keep_spots != local_shadow_maps_.spotsAllocated() || keep_points != local_shadow_maps_.pointsAllocated()) {
        want_spot_shadow_maps_ = keep_spots;
        want_point_shadow_maps_ = keep_points;
        local_shadow_maps_dirty_ = true;
    }
}

void VulkanRenderer::applyLocalShadowMaps() {
    local_shadow_maps_dirty_ = false;
    device_.waitIdle();
    local_shadow_maps_.create(device_, want_spot_shadow_maps_, want_point_shadow_maps_);
    // Mapas nuevos: vacios y en layout indefinido; todo se redibuja.
    local_shadow_layout_ready_ = false;
    local_shadows_.invalidate();
    local_static_dirty_ = true;
    updateLightingDescriptors();
    updatePostDescriptors();
}

void VulkanRenderer::applyShadowCache() {
    shadow_cache_dirty_ = false;
    device_.waitIdle();
    if (want_shadow_cache_) {
        shadow_map_.createStaticCache(device_);
    } else {
        shadow_map_.destroyStaticCache();
    }
    // Cache nueva (o ninguna): vacia y sin layout; las cascadas se rehacen.
    shadow_cache_valid_ = {};
    shadow_cache_layout_ready_ = false;
    cascades_valid_ = false;
    cascades_clear_ = false;
}

void VulkanRenderer::recreateSwapchain() {
    framebuffer_resized_ = false;

    if (!swapchain_.recreate(window_width_, window_height_)) {
        return;  // Ventana minimizada.
    }

    // El G-buffer tiene el tamano de la swapchain: se rehace con ella, y los
    // descriptores de la pasada de iluminacion apuntan a las nuevas vistas.
    computeRenderExtent();
    if (shadow_map_dirty_) {
        device_.waitIdle();
        shadow_map_.create(device_, desiredShadowResolution());
        shadow_map_dirty_ = false;
        cascades_valid_ = false;  // el mapa nuevo esta vacio
        shadow_cache_valid_ = {};
        shadow_cache_layout_ready_ = false;
        cascades_clear_ = false;
    }
    gbuffer_.create(device_, render_extent_);
    createRenderTargets();
    updateLightingDescriptors();
    updatePostDescriptors();
    ++scene_image_generation_;

    const vk::SemaphoreCreateInfo semaphore_info{};
    render_finished_.clear();
    render_finished_.reserve(swapchain_.imageCount());
    for (std::uint32_t i = 0; i < swapchain_.imageCount(); ++i) {
        render_finished_.emplace_back(device_.handle(), semaphore_info);
    }
}

// -----------------------------------------------------------------------------
// Datos por frame
// -----------------------------------------------------------------------------

void VulkanRenderer::updateUniforms(const scene::Scene& scene, const scene::Camera& camera,
                                    std::uint32_t frame_index) {
    using core::Vec2;
    GpuCamera camera_data{};
    camera_data.view = camera.view();
    const core::Mat4 unjittered = camera.projection() * camera_data.view;
    camera_projection_ = camera.projection();
    if (!isolated()) {
        camera_near_ = camera.nearPlane();
        camera_far_ = camera.farPlane();
        camera_fov_ = camera.fovY();
    }

    // Jitter del TAA / escalado temporal: una fraccion de pixel distinta cada
    // frame (Halton 2,3), con mas fases cuanto mas se escala (como DLSS/FSR).
    const bool temporal = upscaling_ && graphics_.upscaler != Upscaler::Fsr1 && !isolated();
    Vec2 jitter_pixels{};
    if (temporal && render_extent_.width > 0) {
        const float ratio = static_cast<float>(outputExtent().width) / static_cast<float>(render_extent_.width);
        const auto phases = std::clamp(static_cast<std::uint32_t>(8.0f * ratio * ratio), 8u, 64u);
        // Los dos ojos del casco, el mismo jitter (cada uno lleva su historia).
        if (!xrSecondEye()) jitter_index_ = jitter_index_ % phases + 1;
        const auto halton = [](std::uint32_t index, std::uint32_t base) {
            float f = 1.0f;
            float r = 0.0f;
            while (index > 0) {
                f /= static_cast<float>(base);
                r += f * static_cast<float>(index % base);
                index /= base;
            }
            return r;
        };
        jitter_pixels = Vec2{halton(jitter_index_, 2) - 0.5f, halton(jitter_index_, 3) - 0.5f};
    }
    const Vec2 jitter_ndc{render_extent_.width > 0 ? jitter_pixels.x * 2.0f / static_cast<float>(render_extent_.width) : 0.0f,
                         render_extent_.height > 0 ? jitter_pixels.y * 2.0f / static_cast<float>(render_extent_.height) : 0.0f};
    camera_data.projection = temporal ? core::translate(Vec3{jitter_ndc.x, jitter_ndc.y, 0.0f}) * camera.projection()
                                      : camera.projection();
    camera_data.view_projection = camera_data.projection * camera_data.view;
    // Vectores de movimiento: sin jitter, este frame y el anterior (las caras
    // de la sonda no cuentan).
    camera_data.unjittered_view_projection = unjittered;
    camera_data.previous_view_projection = isolated() ? unjittered : motion_view_projection_;
    // zw: resolucion interna (la teselacion mide los bordes en pixeles).
    camera_data.jitter = Vec4{jitter_ndc.x, jitter_ndc.y, static_cast<float>(render_extent_.width),
                              static_cast<float>(render_extent_.height)};
    camera_data.motion[0] = motion_offset_;
    if (!isolated()) {
        jitter_ndc_ = jitter_ndc;
        taa_reproject_ = motion_view_projection_ * core::inverse(unjittered);
        history_view_projection_ = motion_view_projection_;
        motion_view_projection_ = unjittered;
    }
    previous_view_projection_ = camera_view_projection_;
    camera_view_projection_ = unjittered;
    camera_view_ = camera_data.view;
    // Se invierte una vez aqui para que el shader de iluminacion no tenga que
    // hacerlo por pixel al reconstruir la posicion del mundo.
    camera_data.inverse_view_projection = core::inverse(camera_data.view_projection);
    camera_data.position = toVec4(camera.position(), 1.0f);
    camera_position_ = camera.position();
    // La oclusion del cielo desde arriba: si toca rehacerla, donde (antes de
    // escribir las luces de este frame, que llevan su sitio). Una vez por frame.
    if (!xrSecondEye()) planSkyMap();
    // VR: las sombras salen de la camara que cubre los dos ojos.
    const scene::Camera& shadow_camera = shadow_camera_override_ != nullptr ? *shadow_camera_override_ : camera;

    camera_buffers_[frame_index].write(&camera_data, sizeof(camera_data));

    const scene::LightSet& lights = scene.lights();

    // --- Sombras de las luces locales ---
    // Va antes que las luces porque decide que hueco de sombra tiene cada una.
    // Con las sombras apagadas no se toca la cache: al volver a encenderlas
    // solo se redibuja lo que haya cambiado entretanto. Las caras de la sonda
    // usan los mapas tal como estan: su camara no debe decidir los huecos.
    if (shadows_enabled_ && !isolated() && !xrSecondEye()) {
        local_shadows_.update(shadow_camera, lights, LocalShadowMaps::kSpotResolution,
                              LocalShadowMaps::kPointResolution);
        requestLocalShadowMaps();
    }

    GpuLights light_data{};
    light_data.sun_direction_intensity =
        toVec4(core::normalize(lights.sun.direction), lights.sun.intensity);
    light_data.sun_color_ambient = toVec4(lights.sun.color, lights.ambient.intensity);
    // w: fuerza de la sombra del sol para los rayos (rt_common.glsl).
    light_data.ambient_color = toVec4(lights.ambient.color, sunShadows() ? sun_shadow_strength_ : 0.0f);
    light_data.sky_sun = toVec4(lights.sky.to_sun, lights.sky.daylight);
    light_data.sky_moon = toVec4(lights.sky.to_moon, lights.sky.twilight);

    const auto point_count = static_cast<std::int32_t>(
        std::min<std::size_t>(lights.points.size(), scene::kMaxPointLights));
    for (std::int32_t i = 0; i < point_count; ++i) {
        const scene::PointLight& light = lights.points[static_cast<std::size_t>(i)];
        light_data.points[i].position_range = toVec4(light.position, light.range);
        light_data.points[i].color_intensity = toVec4(light.color, light.intensity);
        light_data.points[i].shadow = Vec4{
            static_cast<float>(local_shadows_.pointSlot(static_cast<std::size_t>(i))),
            light.cast_shadows ? light.shadow_strength : 0.0f, std::max(light.source_radius, 0.0f), 0.0f};
    }
    light_data.point_count = point_count;

    // Los focos apagados simplemente no se envian.
    std::int32_t spot_count = 0;
    for (std::size_t index = 0; index < lights.spots.size(); ++index) {
        const scene::SpotLight& light = lights.spots[index];
        if (!light.enabled || spot_count >= static_cast<std::int32_t>(scene::kMaxSpotLights)) {
            continue;
        }

        GpuSpotLight& gpu = light_data.spots[spot_count];
        gpu.position_range = toVec4(light.position, light.range);
        gpu.direction_intensity = toVec4(core::normalize(light.direction), light.intensity);
        gpu.color_inner = toVec4(light.color, std::cos(light.inner_angle));
        gpu.outer_shadow = Vec4{std::cos(light.outer_angle),
                                static_cast<float>(local_shadows_.spotSlot(index)),
                                light.cast_shadows ? light.shadow_strength : 0.0f,
                                std::max(light.source_radius, 0.0f)};
        ++spot_count;
    }
    light_data.spot_count = spot_count;
    // Sombras por rayos: con rayos por hardware y en la vista de pantalla, de
    // las luces locales que proyectan sombra y del sol (si esta sobre el
    // horizonte y da sombra).
    {
        bool shadowed = false;
        for (std::int32_t i = 0; i < point_count; ++i) shadowed = shadowed || light_data.points[i].shadow.y > 0.0f;
        for (std::int32_t i = 0; i < spot_count; ++i) shadowed = shadowed || light_data.spots[i].outer_shadow.z > 0.0f;
        const bool traced = shadows_enabled_ && rayTracingActive() && !isolated();
        const bool local = shadowed && traced;
        const bool sun = traced && sunShadows() && sun_shadow_strength_ > 0.0f && lights.sun.intensity > 0.0f &&
                         core::length(lights.sun.direction) > 1e-6f && core::normalize(lights.sun.direction).y < 0.0f;
        const bool planned = local || sun;
        if (!isolated()) {
            rt_shadows_planned_ = planned;
            rt_shadow_flags_ = (local ? 1u : 0u) | (sun ? 2u : 0u);
        }
        // y: vision nocturna para la luz de la luna y el cielo (lighting.frag):
        // de la hora, no de lo que se ve. 0 de dia, 0.9 con noche cerrada.
        const float adaptation = 0.01f + 0.3f * lights.sky.daylight;
        const float t = std::clamp((std::log2(adaptation) - std::log2(0.012f)) /
                                       (std::log2(0.09f) - std::log2(0.012f)), 0.0f, 1.0f);
        const float night = 1.0f - t * t * (3.0f - 2.0f * t);
        // z: destello de un rayo (sistema de ambiente); w: niebla en el
        // horizonte del cielo (0..1).
        // x: 1 = sombras por rayos de las luces locales, 2 = del sol (se suman).
        light_data.rt_shadows = Vec4{planned ? static_cast<float>(rt_shadow_flags_) : 0.0f,
                                     night * std::clamp(post_.night_vision, 0.0f, 1.0f) * 0.9f,
                                     std::max(precipitation_.flash, 0.0f), std::clamp(precipitation_.sky_fog, 0.0f, 1.0f)};
    }
    {
        // Luz que reciben la lluvia y la nieve (HDR lineal): algo del sol
        // (menos con el cielo cubierto) y la del cielo.
        const auto lin = [](float c) { return std::pow(std::max(c, 0.0f), 2.2f); };
        const float covered = std::clamp(cloud_settings_.coverage, 0.0f, 1.0f);
        const Vec3 sun{lin(lights.sun.color.x), lin(lights.sun.color.y), lin(lights.sun.color.z)};
        const Vec3 amb{lin(lights.ambient.color.x), lin(lights.ambient.color.y), lin(lights.ambient.color.z)};
        precipitation_light_ = sun * (lights.sun.intensity * 0.12f * (1.0f - 0.6f * covered)) +
                               Vec3{0.30f, 0.33f, 0.38f} * (lights.sky.daylight * (1.0f - 0.45f * covered)) +
                               amb * (lights.ambient.intensity * 0.3f);
    }
    light_data.ssao_enabled = post_.ambient_occlusion ? 1 : 0;
    light_data.gi_enabled = post_.global_illumination || bakedGiActive() ? 1 : 0;
    // --- Lluvia ---
    if (!isolated()) {
        weather_time_ += frame_delta_seconds_;
    }
    GpuWeather weather{};
    weather.rain_view_projection = rain_view_projection_;
    const bool raining = rain_enabled_ && rainAvailable();
    weather.params = Vec4{raining ? wetness_ : 0.0f, raining ? puddles_ : 0.0f, weather_time_,
                          rain_map_ready_ ? 1.0f : 0.0f};
    // La zona inundada no depende de que llueva ahora (el agua sigue ahi).
    const bool flooded = water_enabled_ && waterAvailable();
    weather.flood = Vec4{water_center_.x, water_center_.y, flooded ? water_radii_.x : 0.0f,
                         flooded ? water_radii_.y : 0.0f};
    // Nieve acumulada y tinte de la estacion (sistema de ambiente).
    weather.snow = Vec4{std::clamp(precipitation_.snow_cover, 0.0f, 1.0f), std::max(precipitation_.snow_thickness, 0.0f),
                        std::clamp(precipitation_.snow_melt, 0.0f, 1.0f), std::clamp(precipitation_.season_tint, 0.0f, 1.0f)};
    // Decals (estampas, charcos y humedad locales).
    std::uint32_t decal_count = 0;
    for (const Decal& decal : decals_) {
        if (decal_count >= kMaxDecals) break;
        GpuDecal& gpu = weather.decals[decal_count++];
        gpu.world_to_decal = decal.world_to_decal;
        gpu.color = toVec4(decal.color, decal.opacity);
        gpu.axis = toVec4(core::normalize(decal.axis), decal.angle_fade);
        gpu.params = Vec4{static_cast<float>(decal.type), static_cast<float>(decal.texture),
                          std::clamp(decal.edge_softness, 0.001f, 0.5f), decal.amount};
        gpu.material = Vec4{decal.roughness, decal.roughness_amount, decal.metallic, 0.0f};
    }
    // yzw: hacia el sol, para la auto-sombra del parallax (0 = sin sol).
    const Vec3 to_sun = lights.sun.intensity > 0.0f && core::length(lights.sun.direction) > 1e-6f
                            ? -core::normalize(lights.sun.direction)
                            : Vec3{};
    weather.decal_info = Vec4{static_cast<float>(decal_count), to_sun.x, to_sun.y, to_sun.z};
    // Zonas de fuego: suelo y vegetacion quemados (fire_burn.glsl).
    {
        const std::array<Vec4, kFireZoneSlots> rects = fire_pass_.zoneRects();
        for (std::uint32_t i = 0; i < kFireZoneSlots && i < kMaxFireZones; ++i) weather.fire_zones[i] = rects[i];
        // Luz del humo (HDR lineal): el sol (o la luna) y el cielo.
        const auto lin = [](float c) { return std::pow(std::max(c, 0.0f), 2.2f); };
        const float covered = std::clamp(cloud_settings_.coverage, 0.0f, 1.0f);
        const Vec3 sun{lin(lights.sun.color.x), lin(lights.sun.color.y), lin(lights.sun.color.z)};
        const Vec3 amb{lin(lights.ambient.color.x), lin(lights.ambient.color.y), lin(lights.ambient.color.z)};
        fire_lighting_.to_sun = core::length(lights.sun.direction) > 1e-6f ? -core::normalize(lights.sun.direction)
                                                                          : Vec3{0.0f, 1.0f, 0.0f};
        fire_lighting_.sun_color = sun * (lights.sun.intensity * 0.35f * (1.0f - 0.5f * covered));
        fire_lighting_.ambient = Vec3{0.32f, 0.36f, 0.42f} * (lights.sky.daylight * 0.55f) +
                                 amb * (lights.ambient.intensity * 0.5f);
    }
    weather_buffers_[frame_index].write(&weather, sizeof(weather));
    // w: lo que el LOD de la camara deja desviarse la malla (m por metro de
    // distancia): los rayos salen por encima de la malla que ve la camara.
    light_data.rain = Vec4{weather.params.x, weather.params.y, weather.params.z, lod_error_per_meter_};
    light_data.flood = weather.flood;

    // --- Nubes ---
    // Con el cielo fotografiado no hay nubes volumetricas: la foto trae las
    // suyas.
    // x: nubes; y, z: niebla por altura (densidad y caida con la altura).
    // w: cobertura de las nubes (el cielo cubierto da una luz ambiente gris).
    light_data.clouds = Vec4{clouds_enabled_ && !environmentActive() ? 1.0f : 0.0f, std::max(post_.fog_density, 0.0f),
                             std::max(post_.fog_height_falloff, 0.0001f),
                             clouds_enabled_ && !environmentActive() ? std::clamp(cloud_settings_.coverage, 0.0f, 1.0f)
                                                                     : 0.0f};
    // y: luz volumetrica. No en las caras de la sonda: su imagen es de la
    // camara de pantalla.
    // z: sombras de contacto (largo del rayo); no en la sonda (su depth es otro).
    // w: numero de frame (0..63) para el ruido que el TAA promedia (sombras de
    // contacto); -1 sin filtro temporal (entonces el shader no usa ruido).
    const bool temporal_filter = upscaling_ && graphics_.upscaler != Upscaler::Fsr1 && !isolated();
    light_data.environment = Vec4{environmentActive() ? 1.0f : 0.0f,
                                  post_.volumetric_light && !capturing_ ? 1.0f : 0.0f,
                                  post_.contact_shadows && !capturing_ ? std::max(post_.contact_shadow_length, 0.0f) : 0.0f,
                                  temporal_filter ? static_cast<float>(noise_frame_ % 64u) : -1.0f};
    if (!isolated()) {
        cloud_time_ += frame_delta_seconds_;
        // El viento se acumula (cambiar su direccion no hace saltar las nubes).
        const float wind_angle = cloud_settings_.wind_direction * (core::kPi / 180.0f);
        constexpr double kWindPeriod = 168000.0;
        cloud_wind_offset_[0] = std::fmod(cloud_wind_offset_[0] + static_cast<double>(std::cos(wind_angle) *
                                              cloud_settings_.wind_speed * frame_delta_seconds_), kWindPeriod);
        cloud_wind_offset_[1] = std::fmod(cloud_wind_offset_[1] + static_cast<double>(std::sin(wind_angle) *
                                              cloud_settings_.wind_speed * frame_delta_seconds_), kWindPeriod);
    }

    // Sonda de reflexion: el cubo nuevo entra fundiendose con el anterior.
    // No se refleja a si misma mientras se captura.
    if (!isolated()) {
        probe_fade_ = std::min(probe_fade_ + frame_delta_seconds_ / kProbeFadeSeconds, 1.0f);
        if (probe_enabled_ && probe_ready_ && !rayTracingActive()) {
            const std::uint32_t previous = 1 - probe_cube_;
            light_data.probes[probe_cube_] = toVec4(probe_centers_[probe_cube_], probe_fade_);
            if (probe_fade_ < 1.0f) {
                light_data.probes[previous] =
                    toVec4(probe_centers_[previous], 1.0f - probe_fade_);
            }
        }
    }

    // Exposicion: de noche el ojo se adapta a la oscuridad, asi que se abre.
    // Sin esto la noche seria negra y el dia quedaria quemado.
    constexpr float kDayExposure = 1.15f;
    constexpr float kNightExposure = 2.6f;
    exposure_ = kNightExposure + (kDayExposure - kNightExposure) * lights.sky.daylight;
    if (!isolated()) last_daylight_ = lights.sky.daylight;

    // --- Cielo fisico ---
    sky_push_.sun = toVec4(lights.sky.to_sun, kSunIlluminance);

    // Luz direccional activa (sol o luna) para el suelo del entorno del IBL,
    // en la misma escala lineal que la pasada de iluminacion.
    const auto to_linear = [](float c) { return std::pow(c, 2.2f); };
    ibl_light_radiance_ = Vec3{to_linear(lights.sun.color.x), to_linear(lights.sun.color.y),
                               to_linear(lights.sun.color.z)} *
                          lights.sun.intensity;
    ibl_to_light_ = -core::normalize(lights.sun.direction);
    sky_push_.moon = toVec4(lights.sky.to_moon, kMoonIlluminance);

    cloud_push_.to_light_time = toVec4(ibl_to_light_, cloud_time_);
    cloud_push_.light_coverage = toVec4(ibl_light_radiance_, std::clamp(cloud_settings_.coverage, 0.0f, 1.0f));
    // zw = origen del mundo (origen flotante) modulo 168 km: el ruido de forma
    // se repite cada 24 km y el de detalle cada 3.5 km, asi que las nubes no
    // saltan al desplazarse el mundo y el numero sigue siendo pequeno.
    constexpr double kCloudPeriod = 168000.0;
    cloud_push_.params = Vec4{static_cast<float>(noise_frame_ % 64), std::max(cloud_settings_.density, 0.0f),
                              static_cast<float>(std::fmod(world_origin_[0] - cloud_wind_offset_[0], kCloudPeriod)),
                              static_cast<float>(std::fmod(world_origin_[2] - cloud_wind_offset_[1], kCloudPeriod))};
    {
        const float bottom = std::max(cloud_settings_.bottom, 50.0f);
        const float wind_angle = cloud_settings_.wind_direction * (core::kPi / 180.0f);
        cloud_push_.layer = Vec4{bottom, bottom + std::max(cloud_settings_.thickness, 100.0f),
                                 std::clamp(cloud_settings_.type, 0.0f, 1.0f), 0.0f};
        // Las cimas van por delante con el viento (mas rapido en altura).
        cloud_push_.wind = Vec4{std::cos(wind_angle), std::sin(wind_angle),
                                std::min(cloud_settings_.wind_speed * 40.0f, 1500.0f), 0.0f};
        // Mapa de sombra centrado en la camara, pegado a sus texeles (no tiembla).
        const float texel = kCloudShadowExtent / static_cast<float>(kCloudShadowSize);
        const Vec4 eye = camera_data.position;
        const float cx = std::floor(eye.x / texel) * texel;
        const float cz = std::floor(eye.z / texel) * texel;
        // Destello de un rayo dentro de las nubes (sistema de ambiente).
        cloud_push_.flash = toVec4(precipitation_.flash_position, std::max(precipitation_.flash, 0.0f));
        cloud_shadow_push_ = cloud_push_;
        cloud_shadow_push_.wind.w = 1.0f;
        cloud_shadow_push_.shadow = Vec4{cx, cz, kCloudShadowExtent, std::clamp(cloud_settings_.shadow_strength, 0.0f, 1.0f)};
        cloud_push_.shadow = cloud_shadow_push_.shadow;
        const bool cloud_shadows = clouds_enabled_ && !environmentActive() && cloud_settings_.shadows &&
                                   cloud_settings_.coverage > 0.0f && cloud_settings_.shadow_strength > 0.0f;
        light_data.cloud_shadow = cloud_shadows ? cloud_shadow_push_.shadow : Vec4{};
        // Sin trazado de rayos: lo que tapa el cielo desde arriba.
        light_data.sky_map = sky_map_drawn_ && skyMapWanted() ? sky_map_params_ : Vec4{};
        light_data.sky_map_depth = sky_map_depth_;
    }

    // --- Rayos de luz: el sol proyectado en pantalla ---
    // Con w = 0 se proyecta la direccion (un punto en el infinito).
    light_shaft_push_ = GpuLightShaftPush{};
    const vk::Extent2D extent = outputExtent();
    light_shaft_push_.aspect =
        static_cast<float>(extent.width) / static_cast<float>(std::max(extent.height, 1u));
    // Sin el jitter del TAA: los rayos de luz y los destellos se aplican a la
    // imagen ya resuelta; con el, el sol temblaba medio pixel cada frame.
    const Vec4 sun_clip = camera_data.unjittered_view_projection * toVec4(lights.sky.to_sun, 0.0f);
    // El sol en pantalla tambien para los destellos de la lente.
    sun_screen_weight_ = 0.0f;
    if (sun_clip.w > 0.0f) {
        const float u = sun_clip.x / sun_clip.w * 0.5f + 0.5f;
        const float v = sun_clip.y / sun_clip.w * 0.5f + 0.5f;
        sun_screen_uv_ = core::Vec2{u, v};
        const float outside = std::max({-u, u - 1.0f, -v, v - 1.0f, 0.0f});
        sun_screen_weight_ = (1.0f - smoothstepf(0.0f, 0.15f, outside)) *
                             smoothstepf(-0.02f, 0.08f, lights.sky.to_sun.y);
    }
    if (post_.light_shafts && sun_clip.w > 0.0f) {
        const float u = sun_clip.x / sun_clip.w * 0.5f + 0.5f;
        const float v = sun_clip.y / sun_clip.w * 0.5f + 0.5f;
        light_shaft_push_.sun_uv = core::Vec2{u, v};

        // Se apagan poco a poco cuando el sol sale de la pantalla o se pone.
        const float outside = std::max({-u, u - 1.0f, -v, v - 1.0f, 0.0f});
        light_shaft_push_.intensity = (1.0f - smoothstepf(0.0f, 0.6f, outside)) *
                                      smoothstepf(-0.02f, 0.08f, lights.sky.to_sun.y);
    }

    light_buffers_[frame_index].write(&light_data, sizeof(light_data));
    // Luces que ve el path tracing (sin lo que cambia cada frame: lluvia).
    if (!isolated()) {
        std::uint64_t hash = 1469598103934665603ull;
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(&light_data);
        for (std::size_t i = 0; i < offsetof(GpuLights, probes); ++i) {
            hash = (hash ^ bytes[i]) * 1099511628211ull;
        }
        if (hash != path_tracing_lights_hash_) {
            path_tracing_lights_hash_ = hash;
            path_tracing_reset_ = true;
        }
    }

    // --- Cascadas de sombra ---
    // Dependen de la camara y del sol, asi que se recalculan cada frame. En VR,
    // una vez por frame (el primer ojo, con la camara de los dos): el segundo
    // usa las mismas cascadas ya dibujadas.
    if (!xrSecondEye()) {
        cascades_.update(shadow_camera, lights.sun, shadow_map_.resolution());

        // Actualizacion escalonada (como Unreal y Frostbite): redibujar las cuatro
        // cascadas cada frame era casi la mitad del frame en Bistro, y las
        // lejanas, que cubren toda la escena, apenas cambian de un frame a otro.
        //   cascada 0: cada frame, 1: cada 2, 2 y 3: cada 4 (por turnos).
        // Se fuerza si la camara se alejo mas del 10% del alcance de la cascada
        // desde que se dibujo. Una cascada que no se redibuja se sigue leyendo
        // con la matriz con la que se dibujo (el escenario no se ha movido).
        // Las caras de la sonda dibujan todas desde su camara y ensucian el
        // mapa: la siguiente vista de pantalla las rehace todas.
        // Detalle de sombras (LODs y palanca del presupuesto): si cambia, las
        // cascadas guardadas ya no son las de ahora.
        const std::uint32_t detail_key = (post_.lods ? 1u : 0u) | (static_cast<std::uint32_t>(
                                                                        budget_.level(Lever::ShadowDetail)) << 1);
        const bool detail_changed = detail_key != cascade_detail_key_;
        if (!isolated()) cascade_detail_key_ = detail_key;
        // Sin sombras del sol basta con dejar el mapa limpio (nada ocluye)
        // una vez: borrar las cuatro capas cada frame (y antes copiarlas de la
        // cache) no cambiaba nada.
        const bool kept_clear = !sunShadows() && cascades_clear_ && !isolated();
        const bool redraw_all =
            !kept_clear && (isolated() || !cascades_valid_ || !sunShadows() || actor_set_changed_ || detail_changed);
        const std::uint64_t far_period = 4ull * budget_.farCascadeInterval();
        // La cache de lo estatico solo con animados que proyectan sombra (se
        // crea al frame siguiente; se suelta tras ~10 s sin ninguno).
        if (!isolated() && !xrSecondEye()) {
            const bool animated = sunShadows() && std::any_of(actor_draws_.begin(), actor_draws_.end(), [](const ActorDraw& d) {
                return !d.per_submesh && d.cast_shadows;
            });
            if (animated) shadow_cache_used_frame_ = frame_count_;
            const bool want = shadow_map_.canCacheStatic() &&
                              (animated || (shadow_map_.hasStaticCache() && frame_count_ - shadow_cache_used_frame_ < 600));
            if (want != shadow_map_.hasStaticCache()) {
                want_shadow_cache_ = want;
                shadow_cache_dirty_ = true;
            }
        }
        const bool static_cache = shadow_map_.hasStaticCache();
        if (!isolated()) {
            ++cascade_frame_;
        }
        // Lo que se mueve solo, sin que cambie ningun transform: el viento en
        // la vegetacion, la hierba y las hojas de los bloques. Con eso en la
        // escena las cascadas siguen su turno aunque nada mas cambie.
        const bool self_animated = !foliage_pass_.empty() || !terrain_pass_.empty() || !voxel_pass_.empty();
        const auto same_framing = [](const scene::ShadowCascade& a, const scene::ShadowCascade& b) {
            return std::memcmp(&a.light_view_projection, &b.light_view_projection, sizeof(core::Mat4)) == 0 &&
                   std::memcmp(&a.split_distance, &b.split_distance, sizeof(float)) == 0 &&
                   std::memcmp(&a.texel_world_size, &b.texel_world_size, sizeof(float)) == 0;
        };
        // Actores animados (esqueleto) que proyectan sombra dentro de una cascada.
        const auto animated_inside = [this](const scene::ShadowCascade& cascade) {
            const core::Frustum frustum(cascade.light_view_projection, /*ignore_near=*/true);
            for (const ActorDraw& draw : actor_draws_) {
                if (draw.per_submesh || !draw.cast_shadows) continue;
                const Vec3 r{draw.bounds_radius, draw.bounds_radius, draw.bounds_radius};
                if (frustum.intersects(core::Aabb{draw.bounds_center - r, draw.bounds_center + r})) return true;
            }
            return false;
        };
        for (std::uint32_t i = 0; i < scene::kShadowCascadeCount; ++i) {
            const scene::ShadowCascade& current = cascades_.cascade(i);
            bool due = redraw_all;
            if (!due && !kept_clear) {
                const std::uint64_t f = cascade_frame_;
                if (budget_.level(Lever::ShadowDetail) >= 2) {
                    // Gama baja: la cercana cada 2 frames y la siguiente cada
                    // 4, intercaladas con las lejanas (cada frame dibuja una
                    // sola: ~1 cascada por frame en vez de 2). Con un frame de
                    // retraso valen igual: se leen con la matriz con la que se
                    // dibujaron, y donde no lleguen (la camara giro)
                    // lighting.frag usa la siguiente.
                    due = (i == 0 && f % 2 == 0) || (i == 1 && f % 4 == 1) || (i == 2 && f % far_period == 3) ||
                          (i == 3 && f % far_period == 7);
                } else {
                    due = i == 0 || (i == 1 && f % 2 == 0) || (i == 2 && f % far_period == 1) ||
                          (i == 3 && f % far_period == 3);
                }
                // Mismo encuadre que el que tiene la capa (camara, sol y escena
                // quietos) y nada que se mueva solo: la capa ya tiene
                // exactamente lo que se dibujaria. Redibujarla por turno (y
                // copiarla de la cache) era la mitad del frame en un PC de gama
                // baja con la camara quieta (Bistro: 2.8 de 5.2 ms). Sin cache,
                // los animados de dentro siguen su turno: se deforman sin
                // cambiar de sitio. Lo que si se mueve lo fuerza mas abajo.
                if (due && !self_animated && same_framing(current, rendered_cascades_[i]) &&
                    (static_cache || !animated_inside(current))) {
                    due = false;
                }
                const Vec3 moved = shadow_camera.position() - rendered_cascade_camera_[i];
                const float limit = 0.1f * current.split_distance;
                due = due || core::dot(moved, moved) > limit * limit;

                // Algo se movio dentro de la cascada (donde estaba o donde esta):
                // su sombra guardada ya no vale. En las lejanas (2 y 3) solo si
                // es grande para sus texeles: un personaje animado esta siempre
                // junto a la camara, dentro de las cuatro, y forzaba a redibujar
                // cada frame las que cubren todo el mapa (en Play, 18 ms de
                // sombras de 22; ningun escalador podia ayudar). Lo pequeno sigue
                // su turno (cada 4 frames); de cerca lo dibujan la 0 y la 1.
                // Con la cache de lo estatico, los animados no cuentan: se dibujan
                // encima cada frame sin tocar lo demas.
                const float min_radius = i >= 2 ? 8.0f * current.texel_world_size : 0.0f;
                if (!due && !moved_spheres_.empty()) {
                    const core::Frustum frustum(current.light_view_projection, /*ignore_near=*/true);
                    for (std::size_t s = 0; s < moved_spheres_.size(); ++s) {
                        const core::Vec4& sphere = moved_spheres_[s];
                        if (static_cache && moved_sphere_animated_[s]) continue;
                        if (sphere.w < min_radius) continue;
                        const Vec3 r{sphere.w, sphere.w, sphere.w};
                        const Vec3 c{sphere.x, sphere.y, sphere.z};
                        if (frustum.intersects(core::Aabb{c - r, c + r})) {
                            due = true;
                            break;
                        }
                    }
                }
            }
            cascade_due_[i] = due;
            if (due) {
                rendered_cascades_[i] = current;
                rendered_cascade_camera_[i] = shadow_camera.position();
            }
            // Actores animados dentro de la cascada (con la matriz con la que
            // se dibujo): se copian la cache y ellos encima.
            cascade_had_animated_[i] = cascade_animated_[i] && !redraw_all;
            cascade_animated_[i] = static_cache && sunShadows() && animated_inside(rendered_cascades_[i]);
        }
        cascades_valid_ = !isolated() && sunShadows();
        cascades_clear_ = !sunShadows();
    }

    GpuShadows shadow_data{};
    std::array<float, scene::kShadowCascadeCount> splits{};
    std::array<float, scene::kShadowCascadeCount> texels{};

    for (std::uint32_t i = 0; i < scene::kShadowCascadeCount; ++i) {
        const scene::ShadowCascade& cascade = rendered_cascades_[i];
        shadow_data.light_view_projection[i] = cascade.light_view_projection;
        splits[i] = cascades_.cascade(i).split_distance;
        texels[i] = cascade.texel_world_size;
    }

    shadow_data.split_distances = Vec4{splits[0], splits[1], splits[2], splits[3]};
    shadow_data.texel_world_sizes = Vec4{texels[0], texels[1], texels[2], texels[3]};
    shadow_data.params = Vec4{static_cast<float>(shadow_map_.resolution()),
                              sunShadows() ? sun_shadow_strength_ : 0.0f,
                              drawModeNow() == SceneDrawMode::Unlit       ? 2.0f
                              : drawModeNow() == SceneDrawMode::Wireframe ? 3.0f
                              : cascade_debug_                            ? 1.0f
                                                                          : 0.0f,
                              kCascadeBlendBand};

    shadow_buffers_[frame_index].write(&shadow_data, sizeof(shadow_data));

    GpuLocalShadows local_data{};
    for (std::uint32_t slot = 0; slot < scene::kMaxShadowedSpotLights; ++slot) {
        const scene::SpotShadow& spot = local_shadows_.spots()[slot];
        local_data.spot_view_projection[slot] = spot.light_view_projection;
        local_data.spot_params[slot] = Vec4{spot.texel_scale, 0.0f, 0.0f, 0.0f};
    }
    for (std::uint32_t slot = 0; slot < scene::kMaxShadowedPointLights; ++slot) {
        const scene::PointShadow& point = local_shadows_.points()[slot];
        for (std::uint32_t face = 0; face < scene::kPointShadowFaceCount; ++face) {
            local_data.point_view_projection[slot * scene::kPointShadowFaceCount + face] =
                point.face_view_projection[face];
        }
        local_data.point_params[slot] = Vec4{point.texel_scale, point.fade, 0.0f, 0.0f};
    }
    local_data.params = Vec4{static_cast<float>(LocalShadowMaps::kSpotResolution),
                             static_cast<float>(LocalShadowMaps::kPointResolution),
                             shadows_enabled_ ? 1.0f : 0.0f, lod_error_per_meter_};

    local_shadow_buffers_[frame_index].write(&local_data, sizeof(local_data));
}

// -----------------------------------------------------------------------------
// Dibujado
// -----------------------------------------------------------------------------

void VulkanRenderer::drawFrame(const scene::Scene& scene, bool present) {
    if (!initialized_) {
        return;
    }

    // Cambio de tamano pendiente: se recrea y este frame no se dibuja. Lo que
    // se preparo para el (la interfaz del editor apunta a la imagen de la
    // escena) era de las imagenes que se acaban de destruir; el siguiente
    // frame ya se construye con las nuevas. Una herramienta puede evitarse el
    // frame perdido llamando antes a applyPendingResize().
    if (framebuffer_resized_ && applyPendingResize()) {
        return;
    }
    if (!swapchain_.isValid()) {
        return;
    }
    if (local_shadow_maps_dirty_) applyLocalShadowMaps();
    if (shadow_cache_dirty_) applyShadowCache();
    // En VR la segunda vista del editor (Escena y Juego a la vez) no se
    // dibuja: el casco necesita la GPU para llegar a sus Hz.
    if (!present && xr_eye_target_ < 0 && render_texture_target_ < 0 && xrViewsMode()) {
        return;
    }
    // VR estereo, segundo ojo: su propio estado temporal mientras se dibuja.
    struct EyeStateScope {
        VulkanRenderer& renderer;
        bool active;
        ~EyeStateScope() {
            if (active) renderer.swapEyeState();
        }
    } eye_scope{*this, xrSecondEye() && xrEyeStateReady()};
    if (eye_scope.active) swapEyeState();

    const vk::raii::Device& device = device_.handle();
    const vk::raii::Fence& fence = in_flight_fences_[current_frame_];

    using TimingClock = std::chrono::steady_clock;
    const auto since = [](TimingClock::time_point t) {
        return std::chrono::duration<float, std::milli>(TimingClock::now() - t).count();
    };
    TimingClock::time_point stage = TimingClock::now();
    if (device.waitForFences(*fence, VK_TRUE, std::numeric_limits<std::uint64_t>::max()) !=
        vk::Result::eSuccess) {
        throw std::runtime_error("Tiempo de espera agotado esperando la fence del frame.");
    }
    const float fence_wait = since(stage);
    frame_timings_.fence_wait_ms += fence_wait;
    fence_wait_total_ms_ += fence_wait;
    // La GPU ya no lee las propiedades de este hueco.
    if (current_frame_ < surface_param_used_.size()) surface_param_used_[current_frame_] = 0;
    // Modelos reemplazados que ya no usa ningun frame en vuelo.
    for (RetiredModel& retired : retired_models_) {
        if (retired.frames_left > 0) --retired.frames_left;
    }
    std::erase_if(retired_models_, [](const RetiredModel& r) { return r.frames_left == 0; });
    // (Solo en los frames de la pantalla: la interfaz se dibuja en ellos.)
    if (render_texture_target_ < 0 && xr_eye_target_ < 0) {
        for (RetiredRenderTexture& retired : retired_render_textures_) {
            if (retired.frames_left > 0) --retired.frames_left;
        }
        std::erase_if(retired_render_textures_, [](const RetiredRenderTexture& r) { return r.frames_left == 0; });
    }

    // Autoenfoque: lo que midio la GPU la ultima vez que se uso este hueco.
    readDofFocus(current_frame_);

    // Picking que se grabo la ultima vez que se uso este hueco: ya termino.
    if (pick_in_flight_[current_frame_]) {
        pick_in_flight_[current_frame_] = false;
        std::uint32_t id = 0;
        std::memcpy(&id, pick_buffers_[current_frame_].mapped(), sizeof(id));
        PickResult result = pick_requests_[current_frame_];
        // Actor + 1 en los 20 bits bajos, hueco de material en los altos.
        const std::uint32_t actor_id = id & 0xFFFFFu;
        result.hit = actor_id != 0;
        result.actor = actor_id != 0 ? actor_id - 1 : 0;
        result.material = id >> 20;
        pick_result_ = result;
    }

    // Resultado del culling en GPU de la ultima vez que se uso este hueco.
    const GpuCulling::Stats culling = gpu_culling_.readStats(current_frame_);
    // VR con las vistas del casco dibujadas: la ventana solo enseña la ultima
    // (sin volver a dibujar la escena: costaba lo mismo que el casco y lo
    // dejaba a la mitad de frames). No mide: deja en su hueco los tiempos del ojo.
    mirror_only_ = present && xr_views_active_ && xrViewsMode();
    const bool profiled = !mirror_only_ && gpu_profiler_.collect(current_frame_);
    {
        // En VR cuenta el frame entero del casco: los tiempos del ojo derecho
        // se guardan y, con los del izquierdo del mismo frame (sombras, cielo y
        // su vista), se mide contra los Hz del casco.
        bool budget_frame = !xrViewsMode();
        float gpu_ms = gpu_profiler_.totalMilliseconds();
        if (xrViewsMode() && profiled) {
            const int eye = slot_xr_eye_[current_frame_];
            if (eye == 1) xr_right_eye_gpu_ms_ = gpu_profiler_.lastTotalMilliseconds();
            if (eye == 0) {
                budget_frame = true;
                gpu_ms = gpu_profiler_.lastTotalMilliseconds() +
                         (xr_.stereo() || stereo_emulation_ ? xr_right_eye_gpu_ms_ : 0.0f);
            }
        }
        if (budget_frame) {
            const auto now = std::chrono::steady_clock::now();
            const float dt = last_budget_time_.time_since_epoch().count() == 0
                                 ? 0.0f
                                 : std::chrono::duration<float>(now - last_budget_time_).count();
            last_budget_time_ = now;
            // Sin timestamps de GPU (algunas de movil, Mali y PowerVR antiguas):
            // el tiempo del frame entero hace de tiempo de GPU (con VSync no baja
            // de los Hz, pero si la GPU no llega es lo que tarda). Sin esto el
            // presupuesto veia 0 ms, creia que sobraba tiempo y lo subia todo.
            if (!gpu_profiler_.supported()) gpu_ms = std::min(dt, 0.5f) * 1000.0f;
            if (!budget_suspended_) budget_.update(std::min(dt, 0.5f), gpu_ms, gpu_profiler_.timings());
        }
        post_ = mobilePost(budget_.apply(user_post_));
        if (const SceneDrawMode mode = drawModeNow(); mode == SceneDrawMode::Unlit || mode == SceneDrawMode::Wireframe) {
            post_.auto_exposure = false;
            post_.manual_exposure = 1.0f;
            post_.exposure_compensation = 0.0f;
            post_.bloom = false;
            post_.light_shafts = false;
            post_.volumetric_light = false;
            post_.vignette = false;
            post_.film_grain = 0.0f;
            post_.chromatic_aberration = 0.0f;
            post_.lens_distortion = 0.0f;
            post_.lens_flare = 0.0f;
            post_.motion_blur = false;
            post_.depth_of_field = false;
            post_.ambient_occlusion = false;
            post_.global_illumination = false;
            post_.reflections = false;
            post_.contact_shadows = false;
        }
        if (accessibility_.reduce_motion) {
            // Accesibilidad: sin lo que marea.
            post_.motion_blur = false;
            post_.lens_distortion = 0.0f;
            post_.chromatic_aberration = 0.0f;
        }
        if (xr_view_frame_) {
            // VR: lo que marea o no tiene sentido pegado a los ojos.
            post_.motion_blur = false;
            post_.depth_of_field = false;
            post_.lens_distortion = 0.0f;
            post_.chromatic_aberration = 0.0f;
            post_.film_grain = 0.0f;
        }
        if (budget_.renderScale() != applied_budget_scale_) applyEffectiveGraphics();
    }
    gpu_visible_submeshes_ = culling.early + culling.late;
    occluded_submeshes_ = culling.occluded;

    // Huesos de este frame (tambien los usa la captura de la sonda).
    stage = TimingClock::now();
    // Streaming de la GPU: lo que no se ve sale y lo que se acerca vuelve, con
    // lo medido desde la ultima vez en todas las vistas (Render Textures, XR,
    // la vista de juego). ANTES de la lista de dibujo de este frame: si sale
    // algo despues, este frame intentaria dibujar un modelo ya vaciado.
    if (render_texture_target_ < 0 && xr_eye_target_ < 0 && present) {
        streamModels(scene);
        std::fill(model_pixels_.begin(), model_pixels_.end(), 0.0f);
    }
    if (!mirror_only_) {
        if (xr_view_frame_ && xr_stereo_view_) {
            // Los dos ojos ven el mismo movimiento de los objetos (del frame
            // anterior a este): el segundo parte de los mismos huesos previos.
            if (xrSecondEye()) {
                last_world_bones_ = stereo_last_world_bones_;
                last_bone_ranges_ = stereo_last_bone_ranges_;
            } else {
                stereo_last_world_bones_ = last_world_bones_;
                stereo_last_bone_ranges_ = last_bone_ranges_;
            }
        }
        updateActors(scene, current_frame_);
    }
    frame_timings_.actors_ms += since(stage);

    if (render_texture_target_ < 0 && xr_eye_target_ < 0 && !mirror_only_ && probeCaptureDue(scene)) {
        stage = TimingClock::now();
        captureProbeFace(scene);
        frame_timings_.probe_ms += since(stage);
    }
    stage = TimingClock::now();

    // Sin presentar (segunda vista del editor): sin imagen de la swapchain.
    const auto [acquire_result, image_index] =
        present ? swapchain_.handle().acquireNextImage(std::numeric_limits<std::uint64_t>::max(),
                                                       *image_available_[current_frame_], nullptr)
                : vk::ResultValue<std::uint32_t>(vk::Result::eSuccess, 0u);

    frame_timings_.acquire_ms += since(stage);
    if (acquire_result == vk::Result::eErrorOutOfDateKHR) {
        recreateSwapchain();
        return;
    }
    if (acquire_result != vk::Result::eSuccess && acquire_result != vk::Result::eSuboptimalKHR) {
        throw std::runtime_error("Fallo al adquirir la imagen de la swapchain: " +
                                 vk::to_string(acquire_result));
    }

    // Exposicion que calculo la GPU la ultima vez que se uso este hueco de
    // frame (la fence garantiza que la copia termino).
    GpuExposureState exposure_state{};
    std::memcpy(&exposure_state, exposure_readback_[current_frame_].mapped(),
                sizeof(exposure_state));
    if (exposure_state.initialized > 0.5f) {
        displayed_exposure_ = post_.auto_exposure ? exposure_state.exposure : exposure_;
        displayed_luminance_ = exposure_state.average_luminance;
    }

    const auto now = std::chrono::steady_clock::now();
    frame_delta_seconds_ =
        (frame_count_ == 0) ? 0.0f : std::chrono::duration<float>(now - last_frame_time_).count();
    last_frame_time_ = now;
    if (xr_view_frame_ && !xrSecondEye()) {
        // La vista del casco avanza el tiempo (nubes, agua, particulas): desde
        // la anterior del casco, no desde el espejo de la ventana.
        frame_delta_seconds_ = last_xr_view_time_.time_since_epoch().count() == 0
                                   ? 0.0f
                                   : std::min(std::chrono::duration<float>(now - last_xr_view_time_).count(), 0.25f);
        last_xr_view_time_ = now;
    }
    if (frame_delta_override_ >= 0.0f) frame_delta_seconds_ = frame_delta_override_;
    // El segundo ojo es el mismo instante: el tiempo ya avanzo con el primero.
    if (xrSecondEye()) frame_delta_seconds_ = 0.0f;
    if (!xrSecondEye()) noise_frame_ = frame_count_;

    // Los uniform buffers de este frame no estan en uso: la fence lo garantiza.
    const core::Mat4 saved_view_projection = camera_view_projection_;
    const core::Mat4 saved_previous_view_projection = previous_view_projection_;
    const core::Mat4 saved_view = camera_view_;
    const Vec3 saved_camera_position = camera_position_;
    // Con el casco en una camara, la vista principal es la del casco: la
    // ventana (el espejo) va como secundaria y no toca historias ni exposicion.
    secondary_view_ = xr_view_frame_ ? false : (!present || xr_views_active_);
    stage = TimingClock::now();
    if (!mirror_only_) updateUniforms(scene, camera_override_ != nullptr ? *camera_override_ : scene.camera(), current_frame_);
    if (!mirror_only_) slot_xr_eye_[current_frame_] = xr_view_frame_ ? xr_view_eye_ : -1;
    if (xr_view_frame_) xr_eye_recorded_ = true;
    frame_timings_.uniforms_ms += since(stage);

    device.resetFences(*fence);

    const vk::raii::CommandBuffer& cmd = command_buffers_[current_frame_];
    cmd.reset();
    presenting_ = present;
    stage = TimingClock::now();
    recordCommandBuffer(cmd, image_index, current_frame_);
    frame_timings_.record_ms += since(stage);
    stage = TimingClock::now();
    presenting_ = true;
    if (!present && !xr_view_frame_) {
        // La vista principal sigue con su camara del frame anterior (en VR con
        // una camara, la principal es la del casco: se queda).
        camera_view_projection_ = saved_view_projection;
        previous_view_projection_ = saved_previous_view_projection;
        camera_view_ = saved_view;
        camera_position_ = saved_camera_position;
    }
    secondary_view_ = false;
    if (present) xr_views_active_ = false;  // el siguiente frame lo vuelve a decir

    if (!present) {
        vk::SubmitInfo view_submit{};
        const vk::CommandBuffer view_command = *cmd;
        view_submit.commandBufferCount = 1;
        view_submit.pCommandBuffers = &view_command;
        {
            const std::lock_guard queue_lock(device_.queueMutex());
            device_.graphicsQueue().submit(view_submit, *fence);
        }
        frame_timings_.submit_ms += since(stage);
        current_frame_ = (current_frame_ + 1) % kMaxFramesInFlight;
        ++frame_count_;
        return;
    }

    const vk::PipelineStageFlags wait_stage = vk::PipelineStageFlagBits::eColorAttachmentOutput;
    const vk::Semaphore wait_semaphore = *image_available_[current_frame_];
    const vk::Semaphore signal_semaphore = *render_finished_[image_index];
    const vk::CommandBuffer command_buffer = *cmd;

    vk::SubmitInfo submit_info{};
    submit_info.waitSemaphoreCount = 1;
    submit_info.pWaitSemaphores = &wait_semaphore;
    submit_info.pWaitDstStageMask = &wait_stage;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &command_buffer;
    submit_info.signalSemaphoreCount = 1;
    submit_info.pSignalSemaphores = &signal_semaphore;

    {
        const std::lock_guard queue_lock(device_.queueMutex());
        device_.graphicsQueue().submit(submit_info, *fence);
    }

    const vk::SwapchainKHR swapchain = *swapchain_.handle();

    vk::PresentInfoKHR present_info{};
    present_info.waitSemaphoreCount = 1;
    present_info.pWaitSemaphores = &signal_semaphore;
    present_info.swapchainCount = 1;
    present_info.pSwapchains = &swapchain;
    present_info.pImageIndices = &image_index;

    const vk::Result present_result = [&] {
        const std::lock_guard queue_lock(device_.queueMutex());
        return device_.presentQueue().presentKHR(present_info);
    }();

#if defined(__ANDROID__)
    // Android devuelve "suboptimo" siempre que la pantalla esta girada y la
    // imagen no va pre-rotada (la rota el compositor): rehacerla cada frame
    // no lo arregla. Se rehace si de verdad cambio el tamano (onResize).
    const bool suboptimal = false;
#else
    const bool suboptimal = present_result == vk::Result::eSuboptimalKHR;
#endif
    if (present_result == vk::Result::eErrorOutOfDateKHR || suboptimal || framebuffer_resized_) {
        recreateSwapchain();
    } else if (present_result != vk::Result::eSuccess && present_result != vk::Result::eSuboptimalKHR) {
        throw std::runtime_error("Fallo al presentar la imagen: " + vk::to_string(present_result));
    }
    frame_timings_.submit_ms += since(stage);

    current_frame_ = (current_frame_ + 1) % kMaxFramesInFlight;
    ++frame_count_;
}

// -----------------------------------------------------------------------------
// Sonda de reflexion
// -----------------------------------------------------------------------------

bool VulkanRenderer::probeCaptureDue(const scene::Scene& scene) {
    // Hacen falta los mapas de sombra locales y la imagen HDR de un frame ya
    // dibujado (se leen tal cual en la captura).
    // Con trazado de rayos la sonda no hace falta: nada de capturas.
    if (!probe_enabled_ || rayTracingActive() || !scene_history_valid_ ||
        !local_shadow_layout_ready_) {
        return false;
    }

    const Vec3 position = scene.camera().position();
    const float speed = frame_delta_seconds_ > 0.0f
                            ? core::length(position - probe_last_camera_) / frame_delta_seconds_
                            : 0.0f;
    probe_last_camera_ = position;

    // Una cara cada pocos frames, y ninguna captura nueva hasta que la
    // anterior termine de fundirse (se escribira en el cubo que sale).
    if (frame_count_ < probe_next_face_frame_ || (probe_face_ < 0 && probe_fade_ < 1.0f)) {
        return false;
    }
    if (probe_face_ >= 0) {
        return true;  // Captura a medias.
    }

    const Vec3 to_light = -core::normalize(scene.lights().sun.direction);
    std::vector<float> signature = lightingSignature(scene);

    if (probe_face_ >= 0) {
        // La luz cambio a mitad de captura: se empieza de nuevo, o las caras
        // mezclarian dos iluminaciones.
        if (signatureChanged(signature, probe_capture_signature_, kProbeLightChange)) {
            probe_face_ = 0;
            probe_capture_sun_ = to_light;
            probe_capture_signature_ = std::move(signature);
        }
        return true;
    }

    const float moved = core::length(position - probe_position_);
    const bool due = !probe_ready_ || core::dot(to_light, probe_sun_) < kProbeLightCos ||
                     signatureChanged(signature, probe_signature_, kProbeLightChange) ||
                     moved > kProbeFarDistance ||
                     (moved > kProbeMoveDistance && speed < kProbeSlowSpeed);
    if (!due) {
        return false;
    }

    probe_face_ = 0;
    probe_capture_position_ = position;
    probe_capture_sun_ = to_light;
    probe_capture_signature_ = std::move(signature);
    return true;
}

std::vector<float> VulkanRenderer::lightingSignature(const scene::Scene& scene) const {
    const scene::LightSet& lights = scene.lights();
    std::vector<float> signature;
    signature.reserve(16 + lights.points.size() * 8 + lights.spots.size() * 12);

    const auto add = [&](const Vec3& v) {
        signature.push_back(v.x);
        signature.push_back(v.y);
        signature.push_back(v.z);
    };

    add(lights.sun.color * lights.sun.intensity);
    add(lights.ambient.color * lights.ambient.intensity);
    signature.push_back(lights.sky.daylight);
    signature.push_back(lights.sky.twilight);
    for (const scene::PointLight& light : lights.points) {
        add(light.position);
        add(light.color * light.intensity);
        signature.push_back(light.range);
    }
    for (const scene::SpotLight& light : lights.spots) {
        add(light.position);
        add(light.direction);
        add(light.color * (light.enabled ? light.intensity : 0.0f));
        signature.push_back(light.range);
        signature.push_back(light.outer_angle);
    }
    // Interruptores que cambian lo que se ve (y por tanto lo que se refleja
    // y rebota).
    signature.push_back(shadows_enabled_ ? 1.0f : 0.0f);
    signature.push_back(sun_shadows_ ? 1.0f : 0.0f);
    signature.push_back(post_.global_illumination ? 1.0f : 0.0f);
    signature.push_back(post_.ambient_occlusion ? 1.0f : 0.0f);
    signature.push_back(clouds_enabled_ ? 1.0f : 0.0f);
    return signature;
}

void VulkanRenderer::captureProbeFace(const scene::Scene& scene) {
    // Hacia donde mira cada cara del cubo y que queda arriba en su imagen
    // (orden y convencion de Vulkan: +X, -X, +Y, -Y, +Z, -Z).
    const std::array<std::pair<Vec3, Vec3>, 6> faces = {{
        {Vec3{1.0f, 0.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f}},
        {Vec3{-1.0f, 0.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f}},
        {Vec3{0.0f, 1.0f, 0.0f}, Vec3{0.0f, 0.0f, -1.0f}},
        {Vec3{0.0f, -1.0f, 0.0f}, Vec3{0.0f, 0.0f, 1.0f}},
        {Vec3{0.0f, 0.0f, 1.0f}, Vec3{0.0f, 1.0f, 0.0f}},
        {Vec3{0.0f, 0.0f, -1.0f}, Vec3{0.0f, 1.0f, 0.0f}},
    }};

    const auto face = static_cast<std::uint32_t>(probe_face_);
    const vk::Extent2D extent = render_extent_;

    // Se dibuja con la resolucion de la pantalla y 90 grados en su lado
    // corto: el cuadrado central es exactamente la cara del cubo.
    scene::Camera camera = scene.camera();
    camera.setPosition(probe_capture_position_);
    camera.setOrientation(faces[face].first, faces[face].second);
    const float aspect = static_cast<float>(extent.width) / static_cast<float>(extent.height);
    camera.setAspectRatio(aspect);
    camera.setFovY(extent.width >= extent.height ? core::kPi * 0.5f
                                                 : 2.0f * std::atan(1.0f / aspect));
    const std::uint32_t side = std::min(extent.width, extent.height);
    const vk::Rect2D square{
        vk::Offset2D{static_cast<std::int32_t>((extent.width - side) / 2),
                     static_cast<std::int32_t>((extent.height - side) / 2)},
        vk::Extent2D{side, side}};

    // La camara de la cara no debe quedar como "frame anterior" del SSR y la
    // GI del frame de pantalla.
    const core::Mat4 view_projection = camera_view_projection_;
    const core::Mat4 previous_view_projection = previous_view_projection_;

    capturing_ = true;
    updateUniforms(scene, camera, current_frame_);

    // Mismo hueco de frame que el de pantalla que viene detras: sus buffers
    // estan libres (la fence ya se espero) y se vuelven a esperar al acabar.
    const vk::raii::Fence& fence = in_flight_fences_[current_frame_];
    device_.handle().resetFences(*fence);

    const vk::raii::CommandBuffer& cmd = command_buffers_[current_frame_];
    cmd.reset();
    cmd.begin(vk::CommandBufferBeginInfo{});

    recordShadowPass(cmd, current_frame_);
    recordSkyLutPass(cmd);
    recordCloudPass(cmd, current_frame_);
    recordGeometryPass(cmd, current_frame_);
    recordSsaoPass(cmd, current_frame_);
    recordSsgiPass(cmd, current_frame_);
    recordSsrPass(cmd, current_frame_);
    recordLightingPass(cmd, current_frame_, probe_capture_);

    pipelineBarrier(cmd, colorBarrier(*probe_capture_.handle(),
                                      vk::ImageLayout::eColorAttachmentOptimal,
                                      vk::ImageLayout::eTransferSrcOptimal,
                                      vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                      vk::AccessFlagBits2::eColorAttachmentWrite,
                                      vk::PipelineStageFlagBits2::eBlit,
                                      vk::AccessFlagBits2::eTransferRead));
    reflection_probe_.recordFaceCopy(cmd, *probe_capture_.handle(), square, face);

    // La captura nueva va al cubo que no se esta mostrando (a la primera,
    // al 0).
    const bool last_face = face + 1 == faces.size();
    const std::uint32_t target_cube = probe_ready_ ? 1 - probe_cube_ : 0;
    if (last_face) {
        reflection_probe_.recordPrefilter(cmd, target_cube);
    }

    cmd.end();

    const vk::CommandBuffer command_buffer = *cmd;
    vk::SubmitInfo submit_info{};
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &command_buffer;
    {
        const std::lock_guard queue_lock(device_.queueMutex());
        device_.graphicsQueue().submit(submit_info, *fence);
    }

    if (device_.handle().waitForFences(*fence, VK_TRUE,
                                       std::numeric_limits<std::uint64_t>::max()) !=
        vk::Result::eSuccess) {
        throw std::runtime_error("Tiempo de espera agotado capturando la sonda de reflexion.");
    }

    capturing_ = false;
    camera_view_projection_ = view_projection;
    previous_view_projection_ = previous_view_projection;
    probe_next_face_frame_ = frame_count_ + kProbeFaceInterval;

    if (last_face) {
        // Con una captura anterior, se funde desde ella; la primera entra
        // de golpe (antes solo habia cielo).
        probe_fade_ = probe_ready_ ? 0.0f : 1.0f;
        probe_cube_ = target_cube;
        probe_centers_[target_cube] = probe_capture_position_;
        probe_ready_ = true;
        probe_position_ = probe_capture_position_;
        probe_sun_ = probe_capture_sun_;
        probe_signature_ = probe_capture_signature_;
        probe_face_ = -1;
    } else {
        ++probe_face_;
    }
}

void VulkanRenderer::markPass(const vk::raii::CommandBuffer& cmd, std::uint32_t frame_index, const char* name) {
    const auto now = std::chrono::steady_clock::now();
    const float ms = std::chrono::duration<float, std::milli>(now - pass_start_).count();
    pass_start_ = now;
    bool found = false;
    for (auto& [pass, total] : frame_timings_.passes) {
        if (pass == name) {
            total += ms;
            found = true;
        }
    }
    if (!found) frame_timings_.passes.emplace_back(name, ms);
    gpu_profiler_.mark(cmd, frame_index, name);
}

void VulkanRenderer::recordCommandBuffer(const vk::raii::CommandBuffer& cmd,
                                         std::uint32_t image_index, std::uint32_t frame_index) {
    cmd.begin(vk::CommandBufferBeginInfo{});
    if (mirror_only_) {
        // Espejo de VR: la imagen del casco (sigue en ldr_color_) a la vista y
        // a la ventana. Sin medir (el hueco guarda los tiempos del ojo).
        recordViewCopy(cmd);
        if (presenting_) recordPostProcessPass(cmd, image_index);
        cmd.end();
        mirror_only_ = false;
        return;
    }
    gpu_profiler_.begin(cmd, frame_index);
    pass_start_ = std::chrono::steady_clock::now();
    output_depth_ready_ = false;
    // UI en el mundo: sus texturas, una vez por frame (la primera vista).
    if (world_ui_pass_.needsPaint() && !capturing_) {
        world_ui_pass_.recordPaint(device_, cmd, frame_index);
        markPass(cmd, frame_index, "UI en el mundo (texturas)");
    }

    // Los clusteres de escenario los cuenta la GPU (frame reciente); los
    // animados se suman al dibujarlos.
    visible_submeshes_ = gpu_visible_submeshes_;
    shadow_submeshes_ = 0;
    last_shadow_draw_calls_ = shadow_draw_calls_;
    shadow_draw_calls_ = 0;
    total_submeshes_ = 0;
    for (const ActorDraw& draw : actor_draws_) {
        total_submeshes_ +=
            static_cast<std::uint32_t>(skinned_models_[draw.model].submeshes().size());
    }

    // Terrenos: lo esculpido/pintado desde el frame anterior, y sus trozos
    // (LOD) para este frame (antes de las sombras, que tambien los dibujan).
    if (terrain_pass_.recordUploads(cmd, frame_index)) {
        local_static_dirty_ = true;
        path_tracing_reset_ = true;
    }
    terrain_pass_.prepare(frame_index, camera_position_, camera_view_projection_);
    // Escena de rayos: terreno esculpido, BLAS rehechas y la TLAS de este
    // frame (en la GPU, sin pararla).
    if (ray_tracing_.sceneBuilt()) ray_tracing_.recordUpdates(cmd, frame_index);
    // Hierba: las briznas visibles de este frame (compute). En las capturas
    // aisladas (sondas, Render Textures) no avanza el reloj del viento.
    terrain_pass_.recordGrassCull(cmd, frame_index, camera_position_, camera_view_projection_,
                                  isolated() ? 0.0f : frame_delta_seconds_);
    // Voxeles: secciones rehechas (romper/poner bloques) y las visibles.
    if (voxel_pass_.recordUploads(cmd, frame_index)) staticGeometryChanged();
    voxel_pass_.prepare(frame_index, camera_position_, camera_view_projection_);
    // Vegetacion: recorte y niveles de detalle en la GPU (antes de las sombras).
    foliage_pass_.recordCull(cmd, frame_index, camera_position_, camera_view_projection_, frame_delta_seconds_);
    water_pass_.prepare(frame_index);
    // Oceano FFT: el oleaje de este frame (compute + mipmaps), una vez por
    // frame; las vistas aisladas usan el ultimo.
    if (!isolated() && !xrSecondEye() && !water_pass_.empty()) water_pass_.recordSimulation(cmd, frame_index);
    // Liquidos: los subpasos de este frame (compute; una vez por frame).
    if (!isolated() && !xrSecondEye()) fluid_pass_.recordSimulate(cmd, frame_index);

    if (!rain_map_ready_ && (rainAvailable() || waterAvailable() || precipitation_.needsRainMap()) &&
        !actor_draws_.empty()) {
        recordRainMap(cmd, frame_index);
        markPass(cmd, frame_index, "Mapa de lluvia");
    }
    // VR, segundo ojo: las sombras, el cielo y el IBL de este frame ya los
    // dibujo el primero (mismas imagenes, misma cola).
    if (!xrSecondEye()) {
        recordShadowPass(cmd, frame_index);
        markPass(cmd, frame_index, "Sombras (cascadas)");
        recordLocalShadowPass(cmd, frame_index);
        markPass(cmd, frame_index, "Sombras locales");
        recordSkyLutPass(cmd);
        markPass(cmd, frame_index, "Cielo + IBL");
    }
    recordCloudPass(cmd, frame_index);
    markPass(cmd, frame_index, "Nubes");
    fire_pass_.recordUpload(cmd, frame_index);  // mapa de quemado antes de la geometria
    if (sky_map_redraw_ && !isolated() && !xrSecondEye()) recordSkyMap(cmd, frame_index);
    recordGeometryPass(cmd, frame_index);
    markPass(cmd, frame_index, "Geometria + culling");
    recordSsaoPass(cmd, frame_index);
    markPass(cmd, frame_index, "SSAO");
    recordSsgiPass(cmd, frame_index);
    markPass(cmd, frame_index, "GI");
    recordSsrPass(cmd, frame_index);
    markPass(cmd, frame_index, "Reflejos");
    recordVolumetricPass(cmd, frame_index);
    markPass(cmd, frame_index, "Volumetrica");
    recordRtShadowPass(cmd, frame_index);
    if (rt_shadows_this_frame_) markPass(cmd, frame_index, "Sombras por rayos");
    recordLightingPass(cmd, frame_index, scene_color_);
    markPass(cmd, frame_index, "Iluminacion");
    recordPathTracePass(cmd, frame_index);
    markPass(cmd, frame_index, "Path tracing");
    const bool wire_only = drawModeNow() == SceneDrawMode::Wireframe;
    if (!wire_only) recordGlassPass(cmd, frame_index);
    if (!water_pass_.empty() && !capturing_ && !wire_only) {
        recordWaterPass(cmd, frame_index);
        markPass(cmd, frame_index, "Agua");
    }
    markPass(cmd, frame_index, "Vidrio");
    if (fluid_pass_.active() && !capturing_ && !wire_only) {
        recordFluidPass(cmd, frame_index);
        markPass(cmd, frame_index, "Liquidos");
    }
    if (!sprites_.empty() && !capturing_ && !wire_only) {
        recordSpritePass(cmd, frame_index);
        markPass(cmd, frame_index, "Sprites 2D");
    }
    if (!particles_.empty() && !capturing_ && !wire_only) {
        recordParticlePass(cmd, frame_index);
        markPass(cmd, frame_index, "Particulas");
    }
    if (vfx_pass_.active() && !capturing_ && !wire_only) {
        recordVfxPass(cmd, frame_index);
        markPass(cmd, frame_index, "VFX Graph");
    }
    if (fire_pass_.active() && !capturing_ && !wire_only) {
        recordFirePass(cmd, frame_index);
        markPass(cmd, frame_index, "Fuego y humo");
    }
    if (precipitation_.drawsSomething() && !capturing_ && !wire_only) {
        recordPrecipitationPass(cmd, frame_index);
        markPass(cmd, frame_index, "Lluvia y nieve");
    }
    if (drawModeNow() == SceneDrawMode::LitWireframe && *skinned_pass_.wireOverlayPipeline()) {
        recordWireOverlayPass(cmd, frame_index);
        markPass(cmd, frame_index, "Lineas (Wireframe)");
    }
    recordFilterHistoryCopies(cmd);
    markPass(cmd, frame_index, "Copias de historia");
    if (upscaling_) {
        recordUpscalePass(cmd);
        markPass(cmd, frame_index, activeUpscaler() == Upscaler::Fsr1   ? "FSR 1"
                                   : activeUpscaler() == Upscaler::Fsr3 ? "FSR 3"
                                   : activeUpscaler() == Upscaler::Dlss ? "DLSS"
                                                                        : "TAA / escalado");
    }
    recordCameraFxPass(cmd);
    markPass(cmd, frame_index, "Motion blur + profundidad de campo");
    recordBloomPass(cmd);
    markPass(cmd, frame_index, "Bloom");
    recordLightShaftPass(cmd);
    markPass(cmd, frame_index, "Rayos de luz");
    // La exposicion es de la vista principal (en VR, del primer ojo: la misma
    // en los dos).
    if (!secondary_view_ && !xrSecondEye()) recordAutoExposurePass(cmd);
    markPass(cmd, frame_index, "Auto-exposicion");
    recordCompositePass(cmd, frame_index);
    markPass(cmd, frame_index, "Composicion");
    if (!world_ui_pass_.empty() && !capturing_ && drawModeNow() != SceneDrawMode::Wireframe) {
        recordWorldUiPass(cmd, frame_index);
        markPass(cmd, frame_index, "UI en el mundo");
    }
    if (!outlined_actors_.empty() && editor_helpers_) {
        recordOutlinePass(cmd, frame_index);
        markPass(cmd, frame_index, "Contorno de seleccion");
    }
    if (pick_requested_ && !capturing_ && editor_helpers_) {
        recordPickPass(cmd, frame_index);
        markPass(cmd, frame_index, "Picking");
    }
    if (!overlay_geometry_.empty() && !capturing_ && editor_helpers_) {
        recordOverlayPass(cmd, frame_index);
        markPass(cmd, frame_index, "Gizmos 3D");
    }
    if (xr_eye_target_ >= 0) recordXrEyeCopy(cmd);
    else if (render_texture_target_ >= 0) recordRenderTextureCopy(cmd);
    else recordViewCopy(cmd);
    if (presenting_) recordPostProcessPass(cmd, image_index);
    markPass(cmd, frame_index, "FXAA + presentacion");

    cmd.end();
}

void VulkanRenderer::recordShadowPass(const vk::raii::CommandBuffer& cmd,
                                      std::uint32_t frame_index) {
    using Stage = vk::PipelineStageFlagBits2;
    using Access = vk::AccessFlagBits2;
    const vk::Extent2D extent = shadow_map_.extent();
    const vk::ImageSubresourceRange all_cascades{vk::ImageAspectFlagBits::eDepth, 0, 1, 0,
                                                 scene::kShadowCascadeCount};
    const bool cached = shadow_map_.hasStaticCache();

    bool all_due = true;
    bool any_due = false;
    bool any_update = false;
    for (std::uint32_t i = 0; i < scene::kShadowCascadeCount; ++i) {
        all_due = all_due && cascade_due_[i];
        any_due = any_due || cascade_due_[i];
        any_update = any_update || cascade_due_[i] || cascade_animated_[i] || cascade_had_animated_[i];
    }
    const auto barrier = [&](vk::Image image, vk::ImageLayout from, vk::ImageLayout to,
                             vk::PipelineStageFlags2 src_stage, vk::AccessFlags2 src_access,
                             vk::PipelineStageFlags2 dst_stage, vk::AccessFlags2 dst_access) {
        vk::ImageMemoryBarrier2 b{};
        b.srcStageMask = src_stage;
        b.srcAccessMask = src_access;
        b.dstStageMask = dst_stage;
        b.dstAccessMask = dst_access;
        b.oldLayout = from;
        b.newLayout = to;
        b.image = image;
        b.subresourceRange = all_cascades;
        vk::DependencyInfo dependency{};
        dependency.setImageMemoryBarriers(b);
        compat::pipelineBarrier(cmd, dependency);
    };
    const vk::PipelineStageFlags2 depth_stages = Stage::eEarlyFragmentTests | Stage::eLateFragmentTests;
    const vk::AccessFlags2 depth_access =
        Access::eDepthStencilAttachmentWrite | Access::eDepthStencilAttachmentRead;
    const vk::Image map = *shadow_map_.image().handle();

    // Una cascada en `view`: borrada y con lo que diga `which`, o (sin borrar)
    // solo los animados encima de lo que ya hay.
    const auto draw_cascade = [&](const vk::raii::ImageView& view, std::uint32_t cascade, ShadowActors which) {
        vk::RenderingAttachmentInfo depth_attachment{};
        depth_attachment.imageView = *view;
        depth_attachment.imageLayout = compat::depthAttachmentLayout();
        depth_attachment.loadOp =
            which == ShadowActors::Animated ? vk::AttachmentLoadOp::eLoad : vk::AttachmentLoadOp::eClear;
        depth_attachment.storeOp = vk::AttachmentStoreOp::eStore;
        depth_attachment.clearValue = vk::ClearValue{vk::ClearDepthStencilValue{1.0f, 0}};

        vk::RenderingInfo rendering_info{};
        rendering_info.renderArea = vk::Rect2D{vk::Offset2D{0, 0}, extent};
        rendering_info.layerCount = 1;
        rendering_info.pDepthAttachment = &depth_attachment;

        compat::beginRendering(cmd, rendering_info);
        // Con las sombras apagadas basta con dejar el mapa limpio: todo queda
        // a profundidad maxima, o sea, sin nada que ocluya.
        if (sunShadows()) {
            cmd.setViewport(0, vk::Viewport{0.0f, 0.0f, static_cast<float>(extent.width),
                                            static_cast<float>(extent.height), 0.0f, 1.0f});
            cmd.setScissor(0, vk::Rect2D{vk::Offset2D{0, 0}, extent});
            const core::Mat4& light_view_projection = rendered_cascades_[cascade].light_view_projection;
            recordActorShadows(cmd, frame_index, light_view_projection, {}, 0.0f,
                               rendered_cascades_[cascade].texel_world_size, which);
            if (which != ShadowActors::Animated) {
                terrain_pass_.recordShadow(cmd, frame_index, skin_sets_[frame_index], light_view_projection);
                voxel_pass_.recordShadow(cmd, frame_index, skin_sets_[frame_index], light_view_projection);
                foliage_pass_.recordShadow(cmd, frame_index, skin_sets_[frame_index], light_view_projection);
            }
        }
        compat::endRendering(cmd);
        // Diagnostico (CRAMION_SHADOW_MARKS=1): el tiempo de GPU de cada
        // cascada por separado en el perfilador.
        static const bool cascade_marks = std::getenv("CRAMION_SHADOW_MARKS") != nullptr;
        if (cascade_marks) {
            static constexpr std::array<const char*, 4> kNames = {"Cascada 0", "Cascada 1", "Cascada 2", "Cascada 3"};
            markPass(cmd, frame_index, kNames[cascade & 3u]);
        }
    };

    if (!cached) {
        if (!any_due) return;  // ninguna cambia: el mapa sigue como textura
        // --- Sin cache: las cascadas que tocan, enteras ---
        // El frame anterior las dejo como textura de la pasada de iluminacion.
        // Si alguna no se redibuja, su contenido se conserva (layout
        // anterior); si se redibujan todas, se puede descartar.
        barrier(map, all_due ? vk::ImageLayout::eUndefined : compat::depthReadOnlyLayout(),
                compat::depthAttachmentLayout(), Stage::eFragmentShader | Stage::eComputeShader,
                Access::eShaderSampledRead, depth_stages, depth_access);
        for (std::uint32_t cascade = 0; cascade < scene::kShadowCascadeCount; ++cascade) {
            if (cascade_due_[cascade]) draw_cascade(shadow_map_.cascadeView(cascade), cascade, ShadowActors::All);
        }
        barrier(map, compat::depthAttachmentLayout(), compat::depthReadOnlyLayout(),
                Stage::eLateFragmentTests, Access::eDepthStencilAttachmentWrite, Stage::eFragmentShader,
                Access::eShaderSampledRead);
        return;
    }

    // --- Con cache (como los "cached shadow maps" de Unreal) ---
    // Por cascada:
    //   - se redibuja y tiene animados: lo estatico a la cache, cache -> mapa
    //     y los animados encima;
    //   - se redibuja sin animados: lo estatico directo al mapa, sin cache ni
    //     copia (una capa de 4096 son 64 MB de ida y 64 de vuelta: medio
    //     milisegundo por frame en una GPU media, y mas en una integrada, para
    //     acabar con lo mismo). La capa de la cache queda vieja; si despues
    //     entra un animado, se rellena desde el mapa, que solo tiene lo estatico;
    //   - no se redibuja y tiene (o tenia el frame anterior) animados: cache ->
    //     mapa (borra la silueta de antes) y los animados encima.
    if (!any_update) return;
    const vk::Image cache = *shadow_map_.staticImage().handle();
    std::array<bool, scene::kShadowCascadeCount> into_cache{};
    std::array<bool, scene::kShadowCascadeCount> refill{};
    std::array<bool, scene::kShadowCascadeCount> restore{};
    bool any_into_cache = false;
    bool any_refill = false;
    bool any_restore = false;
    for (std::uint32_t c = 0; c < scene::kShadowCascadeCount; ++c) {
        const bool overlay = !cascade_due_[c] && (cascade_animated_[c] || cascade_had_animated_[c]);
        into_cache[c] = cascade_due_[c] && cascade_animated_[c];
        refill[c] = overlay && cascade_animated_[c] && !shadow_cache_valid_[c];
        restore[c] = into_cache[c] || (overlay && shadow_cache_valid_[c]);
        any_into_cache = any_into_cache || into_cache[c];
        any_refill = any_refill || refill[c];
        any_restore = any_restore || restore[c];
    }
    const auto layer_copies = [&](const std::array<bool, scene::kShadowCascadeCount>& which) {
        std::vector<vk::ImageCopy> copies;
        for (std::uint32_t c = 0; c < scene::kShadowCascadeCount; ++c) {
            if (!which[c]) continue;
            vk::ImageCopy region{};
            region.srcSubresource = vk::ImageSubresourceLayers{vk::ImageAspectFlagBits::eDepth, 0, c, 1};
            region.dstSubresource = region.srcSubresource;
            region.extent = vk::Extent3D{extent.width, extent.height, 1};
            copies.push_back(region);
        }
        return copies;
    };
    // Entre frames la cache esta como origen de copia y el mapa como textura.
    // Si se redibujan todas, lo de antes se puede descartar.
    vk::ImageLayout cache_layout = shadow_cache_layout_ready_ && !all_due ? vk::ImageLayout::eTransferSrcOptimal
                                                                          : vk::ImageLayout::eUndefined;
    vk::ImageLayout map_layout = all_due ? vk::ImageLayout::eUndefined : compat::depthReadOnlyLayout();
    const vk::PipelineStageFlags2 readers = Stage::eFragmentShader | Stage::eComputeShader;

    // 1) Lo estatico de las que se redibujan con animados, en la cache.
    if (any_into_cache) {
        barrier(cache, cache_layout, compat::depthAttachmentLayout(), Stage::eTransfer, Access::eTransferRead,
                depth_stages, depth_access);
        for (std::uint32_t c = 0; c < scene::kShadowCascadeCount; ++c) {
            if (into_cache[c]) draw_cascade(shadow_map_.staticCascadeView(c), c, ShadowActors::Static);
        }
        barrier(cache, compat::depthAttachmentLayout(), vk::ImageLayout::eTransferSrcOptimal,
                Stage::eLateFragmentTests, Access::eDepthStencilAttachmentWrite, Stage::eTransfer,
                Access::eTransferRead);
        cache_layout = vk::ImageLayout::eTransferSrcOptimal;
        shadow_cache_layout_ready_ = true;
    }

    // 2) Animados que entran en una cascada cuya cache es vieja: el mapa solo
    //    tiene lo estatico (se dibujo directo y nada encima), pasa a la cache.
    if (any_refill) {
        barrier(map, map_layout, vk::ImageLayout::eTransferSrcOptimal, readers, Access::eShaderSampledRead,
                Stage::eTransfer, Access::eTransferRead);
        barrier(cache, cache_layout, vk::ImageLayout::eTransferDstOptimal, Stage::eTransfer, Access::eTransferRead,
                Stage::eTransfer, Access::eTransferWrite);
        cmd.copyImage(map, vk::ImageLayout::eTransferSrcOptimal, cache, vk::ImageLayout::eTransferDstOptimal,
                      layer_copies(refill));
        barrier(cache, vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eTransferSrcOptimal, Stage::eTransfer,
                Access::eTransferWrite, Stage::eTransfer, Access::eTransferRead);
        cache_layout = vk::ImageLayout::eTransferSrcOptimal;
        shadow_cache_layout_ready_ = true;
        map_layout = vk::ImageLayout::eTransferSrcOptimal;
    }
    const bool map_was_copied = map_layout == vk::ImageLayout::eTransferSrcOptimal;
    const vk::PipelineStageFlags2 map_src_stage = map_was_copied ? vk::PipelineStageFlags2(Stage::eTransfer) : readers;
    const vk::AccessFlags2 map_src_access =
        map_was_copied ? vk::AccessFlags2(Access::eTransferRead) : vk::AccessFlags2(Access::eShaderSampledRead);

    // 3) Cache -> mapa donde hace falta la base estatica, y el mapa listo
    //    para dibujar.
    if (any_restore) {
        barrier(map, map_layout, vk::ImageLayout::eTransferDstOptimal, map_src_stage, map_src_access, Stage::eTransfer,
                Access::eTransferWrite);
        cmd.copyImage(cache, vk::ImageLayout::eTransferSrcOptimal, map, vk::ImageLayout::eTransferDstOptimal,
                      layer_copies(restore));
        barrier(map, vk::ImageLayout::eTransferDstOptimal, compat::depthAttachmentLayout(), Stage::eTransfer,
                Access::eTransferWrite, depth_stages, depth_access);
    } else {
        barrier(map, map_layout, compat::depthAttachmentLayout(), map_src_stage, map_src_access, depth_stages,
                depth_access);
    }

    // 4) Lo estatico directo al mapa (las que se redibujan sin animados) y 5)
    //    los animados encima.
    for (std::uint32_t c = 0; c < scene::kShadowCascadeCount; ++c) {
        if (cascade_due_[c] && !into_cache[c]) {
            draw_cascade(shadow_map_.cascadeView(c), c, ShadowActors::Static);
            shadow_cache_valid_[c] = false;
        } else if (into_cache[c] || refill[c]) {
            shadow_cache_valid_[c] = true;
        }
    }
    for (std::uint32_t c = 0; c < scene::kShadowCascadeCount; ++c) {
        if (cascade_animated_[c]) draw_cascade(shadow_map_.cascadeView(c), c, ShadowActors::Animated);
    }
    barrier(map, compat::depthAttachmentLayout(), compat::depthReadOnlyLayout(), Stage::eLateFragmentTests,
            Access::eDepthStencilAttachmentWrite, readers, Access::eShaderSampledRead);
}

void VulkanRenderer::recordLocalShadowPass(const vk::raii::CommandBuffer& cmd,
                                           std::uint32_t frame_index) {
    // --- Capas que hay que (re)dibujar este frame ---
    // Solo las de luces que han cambiado: el resto conserva su mapa.
    struct ShadowJob {
        const vk::raii::ImageView* view = nullptr;
        vk::Image image;
        std::uint32_t layer = 0;
        vk::Extent2D extent;
        core::Mat4 light_view_projection;
        Vec3 light_position;
        float range = 0.0f;
        // Eje de la cara del cubo, para descartar lo que queda detras. Cero en
        // los focos.
        Vec3 face_axis;
        // Texel del mapa por metro de distancia a la luz (LOD de los actores).
        float texel_scale = 0.0f;
    };

    const vk::Image spot_image = *local_shadow_maps_.spotImage().handle();
    const vk::Image point_image = *local_shadow_maps_.pointImage().handle();

    // La cache solo vale para lo estatico: si un actor animado esta al alcance
    // de la luz, su mapa se redibuja cada frame (y uno mas cuando se va, para
    // borrar su silueta).
    const bool static_dirty = local_static_dirty_;
    const bool has_terrain = !terrain_pass_.empty();
    const auto needs_render = [this, static_dirty, has_terrain](bool active, bool dirty, const Vec3& position,
                                                                float range, bool& had_actor,
                                                                std::uint64_t& terrain_signature) {
        const bool touches = active && actorsTouch(position, range);
        // El terreno se dibuja en el mapa con el LOD de la camara: si ese
        // LOD cambia cerca de la luz, el mapa cacheado ya no coincide.
        const std::uint64_t signature =
            active && has_terrain ? terrain_pass_.localSignature(position, range) : 0;
        const bool terrain_changed = signature != terrain_signature;
        const bool render = active && (dirty || static_dirty || touches || had_actor || terrain_changed);
        had_actor = touches;
        if (render || !active) terrain_signature = signature;
        return render;
    };

    std::vector<ShadowJob> jobs;
    // Solo con la escena de verdad: un render aislado (miniaturas del
    // Proyecto, capturas) tiene otros actores (el modelo de la miniatura) y
    // dejaba sus siluetas en la cache de una luz: sombras sin objeto (discos,
    // cuadrados) que no se volvian a dibujar.
    if (shadows_enabled_ && !isolated()) {
        local_static_dirty_ = false;
        for (std::uint32_t slot = 0; slot < scene::kMaxShadowedSpotLights; ++slot) {
            const scene::SpotShadow& spot = local_shadows_.spots()[slot];
            if (!needs_render(spot.active, spot.dirty, spot.position, spot.range,
                              spot_had_actor_[slot], spot_terrain_signature_[slot])) {
                continue;
            }
            jobs.push_back(ShadowJob{&local_shadow_maps_.spotView(slot), spot_image, slot,
                                     local_shadow_maps_.spotExtent(), spot.light_view_projection,
                                     spot.position, spot.range, Vec3{}, spot.texel_scale});
        }

        for (std::uint32_t slot = 0; slot < scene::kMaxShadowedPointLights; ++slot) {
            const scene::PointShadow& point = local_shadows_.points()[slot];
            if (!needs_render(point.active(), point.dirty, point.position, point.range,
                              point_had_actor_[slot], point_terrain_signature_[slot])) {
                continue;
            }
            for (std::uint32_t face = 0; face < scene::kPointShadowFaceCount; ++face) {
                jobs.push_back(ShadowJob{&local_shadow_maps_.pointFaceView(slot, face), point_image,
                                         slot * scene::kPointShadowFaceCount + face,
                                         local_shadow_maps_.pointExtent(),
                                         point.face_view_projection[face], point.position,
                                         point.range,
                                         scene::LocalLightShadows::faceDirection(face),
                                         point.texel_scale});
            }
        }
    }

    // --- Barreras de entrada y salida ---
    // El primer frame todas las capas salen de un layout indefinido; despues
    // viven en solo lectura y solo se tocan las que se redibujan.
    std::vector<vk::ImageMemoryBarrier2> to_attachment;
    std::vector<vk::ImageMemoryBarrier2> to_read;

    if (!local_shadow_layout_ready_) {
        const std::array<std::pair<vk::Image, std::uint32_t>, 2> all_layers = {
            std::pair{spot_image, LocalShadowMaps::kSpotLayerCount},
            std::pair{point_image, LocalShadowMaps::kPointLayerCount}};

        for (const auto& [image, count] : all_layers) {
            to_attachment.push_back(depthLayersBarrier(image, 0, count, vk::ImageLayout::eUndefined,
                                                       compat::depthAttachmentLayout()));
            to_read.push_back(depthLayersBarrier(image, 0, count,
                                                 compat::depthAttachmentLayout(),
                                                 compat::depthReadOnlyLayout()));
        }
    } else {
        for (const ShadowJob& job : jobs) {
            to_attachment.push_back(depthLayersBarrier(job.image, job.layer, 1,
                                                       compat::depthReadOnlyLayout(),
                                                       compat::depthAttachmentLayout()));
            to_read.push_back(depthLayersBarrier(job.image, job.layer, 1,
                                                 compat::depthAttachmentLayout(),
                                                 compat::depthReadOnlyLayout()));
        }
    }

    if (to_attachment.empty()) {
        return;  // Todo sigue en cache.
    }

    vk::DependencyInfo to_attachment_dependency{};
    to_attachment_dependency.setImageMemoryBarriers(to_attachment);
    compat::pipelineBarrier(cmd, to_attachment_dependency);

    // --- Una pasada por capa ---
    for (const ShadowJob& job : jobs) {
        vk::RenderingAttachmentInfo depth_attachment{};
        depth_attachment.imageView = **job.view;
        depth_attachment.imageLayout = compat::depthAttachmentLayout();
        depth_attachment.loadOp = vk::AttachmentLoadOp::eClear;
        depth_attachment.storeOp = vk::AttachmentStoreOp::eStore;
        depth_attachment.clearValue = vk::ClearValue{vk::ClearDepthStencilValue{1.0f, 0}};

        vk::RenderingInfo rendering_info{};
        rendering_info.renderArea = vk::Rect2D{vk::Offset2D{0, 0}, job.extent};
        rendering_info.layerCount = 1;
        rendering_info.pDepthAttachment = &depth_attachment;

        compat::beginRendering(cmd, rendering_info);

        cmd.setViewport(0, vk::Viewport{0.0f, 0.0f, static_cast<float>(job.extent.width),
                                        static_cast<float>(job.extent.height), 0.0f, 1.0f});
        cmd.setScissor(0, vk::Rect2D{vk::Offset2D{0, 0}, job.extent});

        recordActorShadows(cmd, frame_index, job.light_view_projection, job.light_position,
                           job.range, job.texel_scale);
        // El terreno y los bloques tambien tapan la luz de antorchas y focos.
        terrain_pass_.recordShadow(cmd, frame_index, skin_sets_[frame_index], job.light_view_projection,
                                   /*local=*/true);
        voxel_pass_.recordShadow(cmd, frame_index, skin_sets_[frame_index], job.light_view_projection);

        compat::endRendering(cmd);
    }

    vk::DependencyInfo to_read_dependency{};
    to_read_dependency.setImageMemoryBarriers(to_read);
    compat::pipelineBarrier(cmd, to_read_dependency);

    local_shadow_layout_ready_ = true;
}

void VulkanRenderer::recordGeometryPass(const vk::raii::CommandBuffer& cmd,
                                        std::uint32_t frame_index) {
    wire_gbuffer_ = drawModeNow() == SceneDrawMode::Wireframe && device_.fillModeNonSolidSupported();
    terrain_pass_.setWireframe(wire_gbuffer_);
    voxel_pass_.setWireframe(wire_gbuffer_);
    foliage_pass_.setWireframe(wire_gbuffer_);
    const vk::Extent2D extent = gbuffer_.extent();
    const auto attachments = gbuffer_.colorAttachments();

    // Oclusion en dos fases solo para la camara de pantalla: las caras de la
    // sonda (y el culling de oclusion apagado) dibujan todo lo que esta en el
    // campo de vision, sin tocar la visibilidad guardada.
    const bool occlusion = occlusion_culling_enabled_ && !isolated();

    // --- Culling en GPU, fase temprana: lo visible el frame anterior ---
    // (No en el modo compatible: el escenario lo dibuja drawCpuActors.)
    const bool gpu_scenery = !device_.compatMode() && gpu_culling_.clusterCount() > 0;
    if (gpu_scenery) gpu_culling_.recordCull(cmd, frame_index, 0, occlusion);

    // --- Los tres destinos pasan a destino de color ---
    std::array<vk::ImageMemoryBarrier2, GBuffer::kColorAttachmentCount + 1> barriers{};
    for (std::size_t i = 0; i < attachments.size(); ++i) {
        barriers[i] = colorBarrier(
            *attachments[i]->handle(), vk::ImageLayout::eUndefined,
            vk::ImageLayout::eColorAttachmentOptimal,
            vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderSampledRead,
            vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            vk::AccessFlagBits2::eColorAttachmentWrite);
    }

    // --- Y la profundidad a destino de profundidad ---
    // El frame anterior la dejo como textura de la pasada de iluminacion.
    vk::ImageMemoryBarrier2& depth_barrier = barriers[GBuffer::kColorAttachmentCount];
    depth_barrier.srcStageMask = vk::PipelineStageFlagBits2::eLateFragmentTests |
                                 vk::PipelineStageFlagBits2::eFragmentShader |
                                 vk::PipelineStageFlagBits2::eComputeShader;
    depth_barrier.srcAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentWrite |
                                  vk::AccessFlagBits2::eShaderSampledRead;
    depth_barrier.dstStageMask = vk::PipelineStageFlagBits2::eEarlyFragmentTests;
    depth_barrier.dstAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentWrite |
                                  vk::AccessFlagBits2::eDepthStencilAttachmentRead;
    depth_barrier.oldLayout = vk::ImageLayout::eUndefined;
    depth_barrier.newLayout = compat::depthAttachmentLayout();
    depth_barrier.image = *gbuffer_.depth().handle();
    depth_barrier.subresourceRange =
        vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1};

    vk::DependencyInfo dependency{};
    dependency.setImageMemoryBarriers(barriers);
    compat::pipelineBarrier(cmd, dependency);

    // Abre el pase de geometria: limpiando (fase temprana) o conservando lo
    // ya dibujado (fase tardia).
    const auto begin_rendering = [&](bool clear) {
        std::array<vk::RenderingAttachmentInfo, GBuffer::kColorAttachmentCount> color_attachments{};
        for (std::size_t i = 0; i < attachments.size(); ++i) {
            color_attachments[i].imageView = *attachments[i]->view();
            color_attachments[i].imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
            color_attachments[i].loadOp =
                clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad;
            color_attachments[i].storeOp = vk::AttachmentStoreOp::eStore;
            // Alfa 0 en el destino de posiciones = "aqui no hay geometria", que es
            // lo que la pasada de iluminacion interpreta como cielo.
            color_attachments[i].clearValue =
                vk::ClearValue{vk::ClearColorValue{0.0f, 0.0f, 0.0f, 0.0f}};
        }

        vk::RenderingAttachmentInfo depth_attachment{};
        depth_attachment.imageView = *gbuffer_.depth().view();
        depth_attachment.imageLayout = compat::depthAttachmentLayout();
        depth_attachment.loadOp = clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad;
        depth_attachment.storeOp = vk::AttachmentStoreOp::eStore;
        depth_attachment.clearValue = vk::ClearValue{vk::ClearDepthStencilValue{1.0f, 0}};

        vk::RenderingInfo rendering_info{};
        rendering_info.renderArea = vk::Rect2D{vk::Offset2D{0, 0}, extent};
        rendering_info.layerCount = 1;
        rendering_info.setColorAttachments(color_attachments);
        // Modo compatible: 4 destinos (sin el modelo de sombreado).
        rendering_info.colorAttachmentCount = GBuffer::activeColorAttachments();
        rendering_info.pDepthAttachment = &depth_attachment;

        compat::beginRendering(cmd, rendering_info);
        cmd.setViewport(0, vk::Viewport{0.0f, 0.0f, static_cast<float>(extent.width),
                                        static_cast<float>(extent.height), 0.0f, 1.0f});
        cmd.setScissor(0, vk::Rect2D{vk::Offset2D{0, 0}, extent});
    };

    // --- Fase temprana: animados + lo que era visible ---
    begin_rendering(/*clear=*/true);
    drawCpuActors(cmd, frame_index);
    // El terreno, pronto: tapa mucho y entra en la piramide Hi-Z.
    terrain_pass_.recordGBuffer(cmd, frame_index, skin_sets_[frame_index]);
    terrain_pass_.recordGrassGBuffer(cmd, frame_index, skin_sets_[frame_index]);
    // Los voxeles tambien tapan mucho: pronto, de cerca a lejos.
    voxel_pass_.recordGBuffer(cmd, frame_index, skin_sets_[frame_index]);
    // Vegetacion: una llamada indirecta por especie (sus 3 niveles).
    foliage_pass_.recordGBuffer(cmd, frame_index, skin_sets_[frame_index]);
    if (gpu_scenery) drawGpuClusters(cmd, frame_index, 0);
    compat::endRendering(cmd);

    if (!occlusion || !gpu_scenery) {
        return;
    }

    // --- Piramide Hi-Z con la profundidad de lo ya dibujado ---
    vk::ImageMemoryBarrier2 depth_to_read{};
    depth_to_read.srcStageMask = vk::PipelineStageFlagBits2::eLateFragmentTests;
    depth_to_read.srcAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
    depth_to_read.dstStageMask = vk::PipelineStageFlagBits2::eComputeShader;
    depth_to_read.dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead;
    depth_to_read.oldLayout = compat::depthAttachmentLayout();
    depth_to_read.newLayout = compat::depthReadOnlyLayout();
    depth_to_read.image = *gbuffer_.depth().handle();
    depth_to_read.subresourceRange = depth_barrier.subresourceRange;
    pipelineBarrier(cmd, depth_to_read);

    gpu_culling_.recordHiZ(cmd);

    // --- Fase tardia: lo recien descubierto, probado contra la Hi-Z ---
    gpu_culling_.recordCull(cmd, frame_index, 1, true);

    vk::ImageMemoryBarrier2 depth_to_attachment = depth_to_read;
    depth_to_attachment.srcStageMask = vk::PipelineStageFlagBits2::eComputeShader;
    depth_to_attachment.srcAccessMask = vk::AccessFlagBits2::eShaderSampledRead;
    depth_to_attachment.dstStageMask = vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                                       vk::PipelineStageFlagBits2::eLateFragmentTests;
    depth_to_attachment.dstAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentRead |
                                        vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
    depth_to_attachment.oldLayout = compat::depthReadOnlyLayout();
    depth_to_attachment.newLayout = compat::depthAttachmentLayout();
    pipelineBarrier(cmd, depth_to_attachment);

    // Los destinos de color siguen en su layout: basta con ordenar las
    // escrituras de las dos fases.
    vk::MemoryBarrier2 color_order{};
    color_order.srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput;
    color_order.srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite;
    color_order.dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput;
    color_order.dstAccessMask =
        vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite;
    vk::DependencyInfo color_dependency{};
    color_dependency.setMemoryBarriers(color_order);
    compat::pipelineBarrier(cmd, color_dependency);

    begin_rendering(/*clear=*/false);
    drawGpuClusters(cmd, frame_index, 1);
    compat::endRendering(cmd);
}

void VulkanRenderer::drawCpuActors(const vk::raii::CommandBuffer& cmd,
                                   std::uint32_t frame_index) {
    bool bound = false;
    std::int32_t bound_shader = -1;
    const core::Frustum frustum(camera_view_projection_);

    // Modo compatible: tambien el escenario (sin culling en GPU).
    const bool scenery = device_.compatMode();
    for (const ActorDraw& draw : actor_draws_) {
        if ((draw.per_submesh && !scenery) || draw.shadows_only) {
            continue;  // Escenario (lo dibuja drawGpuClusters) o solo sombras.
        }
        if (!bound) {
            cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *meshGeometryPipeline());
            cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
                                   *skinned_pass_.geometryLayout(), 0, *skin_sets_[frame_index],
                                   nullptr);
            bound = true;
        }

        const SkinnedModel& model = skinned_models_[draw.model];
        cmd.bindVertexBuffers(0, *model.vertices().handle(), {0});
        cmd.bindIndexBuffer(*model.indices().handle(), 0, vk::IndexType::eUint32);

        // Solo las submallas dentro del campo de vision. El material (set y
        // push constants) se cambia solo al pasar a otro.
        std::uint32_t bound_material = UINT32_MAX;
        // Batching: submallas seguidas del mismo material en una llamada.
        std::uint32_t run_first = 0;
        std::uint32_t run_count = 0;
        const auto flush = [&]() {
            if (run_count == 0) return;
            cmd.drawIndexed(run_count, 1, run_first, 0, 0);
            run_count = 0;
        };
        visible_submeshes_ += forEachVisibleSubmesh(draw, frustum, [&](std::uint32_t i) {
            const asset::SubMesh& submesh = model.submeshes()[i];
            if (submesh.material != bound_material) {
                flush();
                const SkinnedModel::Material& material = model.materials()[submesh.material];
                cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
                                       *skinned_pass_.geometryLayout(), 1,
                                       *model.materialSet(submesh.material), nullptr);

                GpuSkinnedPush push{};
                push.model = draw.transform;
                push.base_color = material.base_color;
                push.emissive = material.emissive;
                push.material = material.params;
                push.bone_offset = draw.bone_offset;
                push.reflectance = material.reflectance;
                push.flags = material.shader_flags;
                push.pick_id = material.shading;  // modelo de Disney (skinned.frag)
                bindMaterialPipeline(cmd, frame_index, material, bound_shader, push);
                cmd.pushConstants<GpuSkinnedPush>(*skinned_pass_.geometryLayout(),
                                                  skinned_pass_.geometryPushStages(), 0, push);
                bound_material = submesh.material;
            }
            if (run_count > 0 && run_first + run_count == submesh.first_index) {
                run_count += submesh.index_count;
            } else {
                flush();
                run_first = submesh.first_index;
                run_count = submesh.index_count;
            }
        });
        flush();
    }
}

// G-buffer por mesh shaders: no en Wireframe ni con las lineas encima (esas
// pasadas dibujan los mismos comandos con pipelines de vertices).
bool VulkanRenderer::meshGeometryAllowed() const {
    const SceneDrawMode mode = drawModeNow();
    return skinned_pass_.meshShadersEnabled() && mode != SceneDrawMode::Wireframe &&
           mode != SceneDrawMode::LitWireframe;
}

void VulkanRenderer::drawGpuClusters(const vk::raii::CommandBuffer& cmd,
                                     std::uint32_t frame_index, std::uint32_t phase) {
    if (gpu_culling_.clusterCount() == 0) {
        return;
    }

    const vk::Buffer commands = *gpu_culling_.commands(phase).handle();
    const vk::Buffer counts = *gpu_culling_.counts(phase).handle();

    // --- Lotes por mesh shaders (gbuffer_meshlet.task/.mesh + skinned.frag) ---
    // Un grupo de task shader por hueco del lote: los que tienen un cluster
    // visible (cull.comp) lo parten en meshlets y descartan los que sobran.
    bool any_mesh = false;
    for (const DrawBatch& batch : draw_batches_) any_mesh = any_mesh || batch.mesh;
    if (any_mesh) {
        // Set 3: comandos y contadores de esta fase (se reescribe si el
        // culling creo buffers nuevos; nunca mientras un frame lo usa).
        const std::size_t set_index = static_cast<std::size_t>(frame_index) * 2 + phase;
        if (mesh_draw_sets_.empty()) {
            const vk::DescriptorPoolSize pool_size{vk::DescriptorType::eStorageBuffer, kMaxFramesInFlight * 2 * 2};
            vk::DescriptorPoolCreateInfo pool_info{};
            pool_info.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
            pool_info.maxSets = kMaxFramesInFlight * 2;
            pool_info.setPoolSizes(pool_size);
            mesh_draw_pool_ = vk::raii::DescriptorPool(device_.handle(), pool_info);
            const std::vector<vk::DescriptorSetLayout> layouts(kMaxFramesInFlight * 2,
                                                               *skinned_pass_.meshDrawSetLayout());
            vk::DescriptorSetAllocateInfo alloc{};
            alloc.descriptorPool = *mesh_draw_pool_;
            alloc.setSetLayouts(layouts);
            mesh_draw_sets_ = vk::raii::DescriptorSets(device_.handle(), alloc);
            mesh_draw_buffers_.assign(kMaxFramesInFlight * 2, {vk::Buffer{}, vk::Buffer{}});
        }
        if (mesh_draw_buffers_[set_index][0] != commands || mesh_draw_buffers_[set_index][1] != counts) {
            const std::array<vk::DescriptorBufferInfo, 2> infos = {vk::DescriptorBufferInfo{commands, 0, VK_WHOLE_SIZE},
                                                                   vk::DescriptorBufferInfo{counts, 0, VK_WHOLE_SIZE}};
            std::array<vk::WriteDescriptorSet, 2> writes{};
            for (std::uint32_t i = 0; i < 2; ++i) {
                writes[i].dstSet = *mesh_draw_sets_[set_index];
                writes[i].dstBinding = i;
                writes[i].descriptorType = vk::DescriptorType::eStorageBuffer;
                writes[i].setBufferInfo(infos[i]);
            }
            device_.handle().updateDescriptorSets(writes, nullptr);
            mesh_draw_buffers_[set_index] = {commands, counts};
        }

        const vk::PipelineLayout layout = *skinned_pass_.meshGeometryLayout();
        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *skinned_pass_.meshGeometryPipeline());
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, layout, 0, *skin_sets_[frame_index], nullptr);
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, layout, 3, *mesh_draw_sets_[set_index], nullptr);
        std::uint32_t mesh_model = UINT32_MAX;
        for (std::uint32_t b = 0; b < draw_batches_.size(); ++b) {
            const DrawBatch& batch = draw_batches_[b];
            if (!batch.mesh || batch.capacity == 0) continue;
            const SkinnedModel& model = skinned_models_[batch.model];
            if (batch.model != mesh_model) {
                cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, layout, 2, *model.meshletSet(), nullptr);
                mesh_model = batch.model;
            }
            const SkinnedModel::DrawGroup& group = model.drawGroups()[batch.group];
            const SkinnedModel::Material& material = model.materials()[group.material];
            cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, layout, 1, *model.materialSet(group.material),
                                   nullptr);
            GpuSkinnedPush push{};
            // Donde el raster lleva la matriz del modelo: el lote (gbuffer_meshlet.task).
            push.model.m[0][0] = std::bit_cast<float>(batch.first_slot);
            push.model.m[0][1] = std::bit_cast<float>(batch.capacity);
            push.model.m[0][2] = std::bit_cast<float>(b);
            push.model.m[0][3] = std::bit_cast<float>(batch.closed ? 1u : 0u);
            push.base_color = material.base_color;
            push.emissive = material.emissive;
            push.material = material.params;
            push.reflectance = material.reflectance;
            push.flags = 1u | material.shader_flags;
            push.pick_id = material.shading;  // modelo de Disney (skinned.frag)
            cmd.pushConstants<GpuSkinnedPush>(layout,
                                              vk::ShaderStageFlagBits::eTaskEXT | vk::ShaderStageFlagBits::eMeshEXT |
                                                  vk::ShaderStageFlagBits::eFragment,
                                              0, push);
            cmd.drawMeshTasksEXT(batch.capacity, 1, 1);
        }
    }

    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *meshGeometryPipeline());
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *skinned_pass_.geometryLayout(), 0,
                           *skin_sets_[frame_index], nullptr);

    // Una llamada por lote (modelo x material, todos los actores juntos):
    // la GPU decide cuantas submallas (y de quien) dibuja, leyendo el
    // contador del lote; cada comando lleva la matriz de su actor.
    std::uint32_t bound_model = UINT32_MAX;
    std::int32_t bound_shader = -1;
    for (std::uint32_t b = 0; b < draw_batches_.size(); ++b) {
        const DrawBatch& batch = draw_batches_[b];
        if (batch.mesh) continue;  // ya dibujado por meshlets (sus comandos son de otro formato)
        const SkinnedModel& model = skinned_models_[batch.model];
        if (batch.model != bound_model) {
            cmd.bindVertexBuffers(0, *model.vertices().handle(), {0});
            cmd.bindIndexBuffer(*model.indices().handle(), 0, vk::IndexType::eUint32);
            bound_model = batch.model;
        }
        const SkinnedModel::DrawGroup& group = model.drawGroups()[batch.group];
        const SkinnedModel::Material& material = model.materials()[group.material];
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *skinned_pass_.geometryLayout(), 1,
                               *model.materialSet(group.material), nullptr);

        GpuSkinnedPush push{};
        push.model = core::Mat4::identity();
        push.base_color = material.base_color;
        push.emissive = material.emissive;
        push.material = material.params;
        push.reflectance = material.reflectance;
        push.flags = 1u | material.shader_flags;
        push.pick_id = material.shading;  // modelo de Disney (skinned.frag)
        bindMaterialPipeline(cmd, frame_index, material, bound_shader, push);
        cmd.pushConstants<GpuSkinnedPush>(*skinned_pass_.geometryLayout(), skinned_pass_.geometryPushStages(), 0,
                                          push);

        const vk::DeviceSize offset = static_cast<vk::DeviceSize>(batch.first_slot) * GpuCulling::kCommandSize;
        if (device_.indirectCountSupported()) {
            cmd.drawIndexedIndirectCount(commands, offset, counts, static_cast<vk::DeviceSize>(b) * sizeof(std::uint32_t),
                                         batch.capacity, GpuCulling::kCommandSize);
        } else {
            // GPU de movil: un comando por llamada; los huecos vacios estan a cero.
            for (std::uint32_t i = 0; i < batch.capacity; ++i) {
                cmd.drawIndexedIndirect(commands, offset + static_cast<vk::DeviceSize>(i) * GpuCulling::kCommandSize, 1,
                                        GpuCulling::kCommandSize);
            }
        }
    }
}

void VulkanRenderer::recordSsaoPass(const vk::raii::CommandBuffer& cmd,
                                    std::uint32_t frame_index) {
    const auto attachments = gbuffer_.colorAttachments();

    // --- El G-buffer pasa de destino de color a textura ---
    // Destinos de color + profundidad + la imagen del SSAO.
    std::array<vk::ImageMemoryBarrier2, GBuffer::kColorAttachmentCount + 2> barriers{};
    for (std::size_t i = 0; i < attachments.size(); ++i) {
        barriers[i] = writtenToSampled(*attachments[i]->handle());
    }

    // --- La profundidad pasa a poder muestrearse ---
    vk::ImageMemoryBarrier2& depth_barrier = barriers[GBuffer::kColorAttachmentCount];
    depth_barrier.srcStageMask = vk::PipelineStageFlagBits2::eLateFragmentTests;
    depth_barrier.srcAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
    depth_barrier.dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader;
    depth_barrier.dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead;
    depth_barrier.oldLayout = compat::depthAttachmentLayout();
    depth_barrier.newLayout = compat::depthReadOnlyLayout();
    depth_barrier.image = *gbuffer_.depth().handle();
    depth_barrier.subresourceRange =
        vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1};

    // --- Y la imagen del SSAO a destino (el frame anterior la leyo) ---
    barriers[GBuffer::kColorAttachmentCount + 1] = discardToAttachment(*ssao_image_.handle());

    pipelineBarrier(cmd, barriers);

    // Apagado (preset Bajo, presupuesto adaptativo, movil) la iluminacion no
    // lo lee (lights.counts.z = 0): las barreras de arriba siguen (el G-buffer
    // pasa a textura aqui), el dibujo no.
    if (!post_.ambient_occlusion) return;
    drawFullscreen<GpuBloomPush>(cmd, ssao_pass_, &ssao_sets_[frame_index], ssao_image_,
                                 nullptr);
}

void VulkanRenderer::recordVolumetricPass(const vk::raii::CommandBuffer& cmd,
                                          std::uint32_t frame_index) {
    // Va despues del SSAO: la profundidad y las cascadas ya son texturas. Con
    // la luz volumetrica apagada no se dibuja (lighting.frag no la lee).
    if (!post_.volumetric_light || capturing_) {
        return;
    }
    pipelineBarrier(cmd, discardToAttachment(*volumetric_image_.handle()));

    // Volumenes de niebla locales: mundo -> local y el color en lineal. El
    // rayo llega hasta el mas lejano (hasta 250 m).
    GpuFogVolumes fog{};
    float fog_reach = 0.0f;
    const Vec3 eye = camera_position_;
    for (const FogVolume& v : fog_volumes_) {
        if (fog.count[0] >= kMaxFogVolumes || v.density <= 0.0f) continue;
        GpuFogVolume& g = fog.volumes[fog.count[0]++];
        g.to_local = core::inverse(v.world);
        const auto lin = [](float c) { return std::pow(std::clamp(c, 0.0f, 1.0f), 2.2f); };
        g.color_density = Vec4{lin(v.color.x), lin(v.color.y), lin(v.color.z), v.density};
        g.params = Vec4{static_cast<float>(v.shape), std::clamp(v.edge_falloff, 0.0f, 1.0f),
                        std::clamp(v.noise, 0.0f, 1.0f), std::max(v.noise_scale, 0.001f)};
        const Vec3 center{v.world.m[3][0], v.world.m[3][1], v.world.m[3][2]};
        const float extent = 0.87f * std::max({std::sqrt(v.world.m[0][0] * v.world.m[0][0] + v.world.m[0][1] * v.world.m[0][1] + v.world.m[0][2] * v.world.m[0][2]),
                                               std::sqrt(v.world.m[1][0] * v.world.m[1][0] + v.world.m[1][1] * v.world.m[1][1] + v.world.m[1][2] * v.world.m[1][2]),
                                               std::sqrt(v.world.m[2][0] * v.world.m[2][0] + v.world.m[2][1] * v.world.m[2][1] + v.world.m[2][2] * v.world.m[2][2])});
        const Vec3 d{center.x - eye.x, center.y - eye.y, center.z - eye.z};
        fog_reach = std::max(fog_reach, std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z) + extent);
    }
    fog_volume_buffers_[frame_index].write(&fog, sizeof(fog));

    GpuVolumetricPush push{};
    // Anisotropia 0.6: polvo y humo finos dispersan sobre todo hacia delante.
    // 60 m: mas alla el mapa de sombras ya es grueso y el efecto no aporta
    // (salvo para llegar a los volumenes de niebla).
    push.params = Vec4{post_.volumetric_density, post_.volumetric_anisotropy, weather_time_,
                       std::clamp(fog_reach, 60.0f, 250.0f)};
    push.quality = Vec4{static_cast<float>(std::clamp(post_.volumetric_steps, 8, 32)), 0.0f, 0.0f, 0.0f};
    drawFullscreen(cmd, volumetric_pass_, &volumetric_sets_[frame_index], volumetric_image_,
                   &push);

    pipelineBarrier(cmd, writtenToSampled(*volumetric_image_.handle()));
}

// Sombras por rayos de las luces locales (rt_shadows.comp): en cada pixel,
// un rayo de sombra hacia un punto al azar de la bombilla de cada una de las 4
// luces que mas le aportan. La iluminacion lo lee (binding 25) y lo suaviza
// con los vecinos. Solo con rayos por hardware, en la vista de pantalla y si
// hay luces locales con sombra.
void VulkanRenderer::recordRtShadowPass(const vk::raii::CommandBuffer& cmd, std::uint32_t frame_index) {
    rt_shadows_this_frame_ = rt_shadows_planned_;
    if (!rt_shadows_planned_) {
        // Lo acumulado se queda viejo (la camara sigue moviendose).
        if (!isolated()) rt_shadow_history_valid_ = false;
        return;
    }

    // (La lee el filtro temporal del frame anterior, en compute.)
    pipelineBarrier(cmd, colorBarrier(*rt_shadow_mask_.handle(), vk::ImageLayout::eShaderReadOnlyOptimal,
                                      vk::ImageLayout::eGeneral,
                                      vk::PipelineStageFlagBits2::eFragmentShader |
                                          vk::PipelineStageFlagBits2::eComputeShader,
                                      vk::AccessFlagBits2::eShaderSampledRead,
                                      vk::PipelineStageFlagBits2::eComputeShader,
                                      vk::AccessFlagBits2::eShaderStorageWrite));
    // El compute lee el G-buffer (como la GI).
    memoryBarrier(cmd,
                  vk::PipelineStageFlagBits2::eLateFragmentTests | vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                  vk::AccessFlagBits2::eDepthStencilAttachmentWrite | vk::AccessFlagBits2::eColorAttachmentWrite,
                  vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderSampledRead);
    RayTracing::Push push{};
    push.previous_view_projection = previous_view_projection_;
    // x: numero de frame (el mismo en los dos ojos de VR: la acumulacion es
    // por ojo); z: que se traza (1 = luces locales, 2 = sol).
    push.params = Vec4{static_cast<float>(noise_frame_ % 1024), 0.0f, static_cast<float>(rt_shadow_flags_), 0.0f};
    ray_tracing_.record(cmd, frame_index, RayTracing::Pass::Shadows, rt_shadow_mask_.extent(), push);
    pipelineBarrier(cmd, colorBarrier(*rt_shadow_mask_.handle(), vk::ImageLayout::eGeneral,
                                      vk::ImageLayout::eShaderReadOnlyOptimal,
                                      vk::PipelineStageFlagBits2::eComputeShader,
                                      vk::AccessFlagBits2::eShaderStorageWrite,
                                      vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eFragmentShader,
                                      vk::AccessFlagBits2::eShaderSampledRead));

    // --- Acumulacion temporal (rt_shadow_temporal.comp) ---
    // Lo que leyo la iluminacion del frame anterior (la acumulada) y la copia a
    // la historia tienen que haber terminado antes de reescribirlas.
    memoryBarrier(cmd,
                  vk::PipelineStageFlagBits2::eFragmentShader | vk::PipelineStageFlagBits2::eCopy |
                      vk::PipelineStageFlagBits2::eComputeShader,
                  vk::AccessFlagBits2::eShaderSampledRead | vk::AccessFlagBits2::eTransferRead |
                      vk::AccessFlagBits2::eTransferWrite | vk::AccessFlagBits2::eShaderStorageWrite,
                  vk::PipelineStageFlagBits2::eComputeShader,
                  vk::AccessFlagBits2::eShaderSampledRead | vk::AccessFlagBits2::eShaderStorageWrite);
    GiTemporalPush temporal{};
    temporal.previous_view_projection = history_view_projection_;
    temporal.params = Vec4{rt_shadow_history_valid_ ? 1.0f : 0.0f, kRtShadowMinAlpha, 0.0f, 0.0f};
    const vk::Extent2D extent = rt_shadow_accum_.extent();
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *rt_shadow_temporal_pass_.pipeline());
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *rt_shadow_temporal_pass_.layout(), 0,
                           *rt_shadow_temporal_sets_[frame_index + eyeSet() * kMaxFramesInFlight], nullptr);
    cmd.pushConstants<GiTemporalPush>(*rt_shadow_temporal_pass_.layout(), vk::ShaderStageFlagBits::eCompute, 0,
                                      temporal);
    cmd.dispatch((extent.width + 7) / 8, (extent.height + 7) / 8, 1);

    // La acumulada es la historia del frame siguiente (las dos en General).
    memoryBarrier(cmd, vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderStorageWrite,
                  vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferRead);
    vk::ImageCopy region{};
    region.srcSubresource = vk::ImageSubresourceLayers{vk::ImageAspectFlagBits::eColor, 0, 0, 1};
    region.dstSubresource = region.srcSubresource;
    region.extent = vk::Extent3D{extent.width, extent.height, 1};
    cmd.copyImage(*rt_shadow_accum_.handle(), vk::ImageLayout::eGeneral, *rt_shadow_history_.handle(),
                  vk::ImageLayout::eGeneral, region);
    // La iluminacion lee la acumulada; el filtro del frame siguiente, la historia.
    memoryBarrier(cmd, vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eCopy,
                  vk::AccessFlagBits2::eShaderStorageWrite | vk::AccessFlagBits2::eTransferWrite,
                  vk::PipelineStageFlagBits2::eFragmentShader | vk::PipelineStageFlagBits2::eComputeShader,
                  vk::AccessFlagBits2::eShaderSampledRead);
    rt_shadow_history_valid_ = true;
}

// Filtro de los reflejos por rayos: acumulacion temporal con historia por
// pixel (rt_reflection_temporal.comp) y dos pasadas espaciales a la medida de
// la rugosidad (rt_reflection_atrous.comp). La ultima escribe ssr_image_ (lo
// que lee la iluminacion), que queda como textura.
void VulkanRenderer::recordRtReflectionFilter(const vk::raii::CommandBuffer& cmd, std::uint32_t frame_index) {
    const vk::Extent2D extent = rt_reflection_temporal_.extent();
    // ssr_raw_ ya esta como textura; ssr_image_ se sobrescribe entera (lo de
    // antes no importa) y lo que leyeron/copiaron los frames anteriores tiene
    // que haber terminado.
    pipelineBarrier(cmd, colorBarrier(*ssr_image_.handle(), vk::ImageLayout::eUndefined, vk::ImageLayout::eGeneral,
                                      vk::PipelineStageFlagBits2::eFragmentShader | vk::PipelineStageFlagBits2::eCopy,
                                      vk::AccessFlagBits2::eShaderSampledRead | vk::AccessFlagBits2::eTransferRead,
                                      vk::PipelineStageFlagBits2::eComputeShader,
                                      vk::AccessFlagBits2::eShaderStorageWrite));
    // La historia del filtro del SSR (recordFilterHistoryCopies copia el
    // resultado en ella cada frame) recien creada: como textura, igual que la
    // deja el SSR la primera vez.
    if (!ssr_filter_history_valid_) {
        pipelineBarrier(cmd, colorBarrier(*ssr_history_.handle(), vk::ImageLayout::eUndefined,
                                          vk::ImageLayout::eShaderReadOnlyOptimal, vk::PipelineStageFlagBits2::eTopOfPipe,
                                          vk::AccessFlagBits2::eNone, vk::PipelineStageFlagBits2::eFragmentShader,
                                          vk::AccessFlagBits2::eShaderSampledRead));
    }
    memoryBarrier(cmd, vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eCopy,
                  vk::AccessFlagBits2::eShaderSampledRead | vk::AccessFlagBits2::eShaderStorageWrite |
                      vk::AccessFlagBits2::eTransferRead | vk::AccessFlagBits2::eTransferWrite,
                  vk::PipelineStageFlagBits2::eComputeShader,
                  vk::AccessFlagBits2::eShaderSampledRead | vk::AccessFlagBits2::eShaderStorageWrite);
    const auto dispatch = [&](const ComputePass& pass, const vk::raii::DescriptorSet& set, const auto& constants) {
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *pass.pipeline());
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *pass.layout(), 0, *set, nullptr);
        cmd.pushConstants<std::decay_t<decltype(constants)>>(*pass.layout(), vk::ShaderStageFlagBits::eCompute, 0,
                                                             constants);
        cmd.dispatch((extent.width + 7) / 8, (extent.height + 7) / 8, 1);
        memoryBarrier(cmd, vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderStorageWrite,
                      vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eCopy,
                      vk::AccessFlagBits2::eShaderSampledRead | vk::AccessFlagBits2::eTransferRead);
    };

    GiTemporalPush temporal{};
    temporal.previous_view_projection = history_view_projection_;
    temporal.params = Vec4{rt_reflection_history_valid_ ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f};
    dispatch(rt_reflection_temporal_pass_, rt_reflection_temporal_sets_[frame_index + eyeSet() * kMaxFramesInFlight],
             temporal);

    // Lo acumulado (sin el filtro espacial: no se emborrona frame a frame) es
    // la historia del siguiente.
    vk::ImageCopy region{};
    region.srcSubresource = vk::ImageSubresourceLayers{vk::ImageAspectFlagBits::eColor, 0, 0, 1};
    region.dstSubresource = region.srcSubresource;
    region.extent = vk::Extent3D{extent.width, extent.height, 1};
    cmd.copyImage(*rt_reflection_temporal_.handle(), vk::ImageLayout::eGeneral, *rt_reflection_history_.handle(),
                  vk::ImageLayout::eGeneral, region);
    cmd.copyImage(*rt_reflection_moments_.handle(), vk::ImageLayout::eGeneral,
                  *rt_reflection_moments_history_.handle(), vk::ImageLayout::eGeneral, region);
    rt_reflection_history_valid_ = true;

    for (std::uint32_t iteration = 0; iteration < kReflectionAtrousIterations; ++iteration) {
        ReflectionAtrousPush atrous{};
        atrous.step = 1 << iteration;
        atrous.last = iteration + 1 == kReflectionAtrousIterations ? 1 : 0;
        dispatch(rt_reflection_atrous_pass_, rt_reflection_atrous_sets_[frame_index * kReflectionAtrousIterations + iteration],
                 atrous);
    }
    // Las copias a la historia, antes del filtro del frame siguiente.
    memoryBarrier(cmd, vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferWrite,
                  vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderSampledRead);
    // ssr_image_ a textura para la iluminacion (recordLightingPass lo sabe).
    pipelineBarrier(cmd, colorBarrier(*ssr_image_.handle(), vk::ImageLayout::eGeneral,
                                      vk::ImageLayout::eShaderReadOnlyOptimal, vk::PipelineStageFlagBits2::eComputeShader,
                                      vk::AccessFlagBits2::eShaderStorageWrite,
                                      vk::PipelineStageFlagBits2::eFragmentShader,
                                      vk::AccessFlagBits2::eShaderSampledRead));
    ssr_image_from_compute_ = true;
}

void VulkanRenderer::recordSsgiPass(const vk::raii::CommandBuffer& cmd,
                                    std::uint32_t frame_index) {
    // GI horneada: las sondas en lugar de los rayos o el SSGI.
    const bool baked = bakedGiActive();
    const bool ray_traced = !baked && rayTracingActive() && !isolated();

    // GI apagada (preset Bajo, presupuesto adaptativo, movil): la iluminacion
    // no la lee (lights.counts.w = 0) y sus imagenes siguen en General, asi
    // que no se calcula. Eran 0.7 ms por frame (SSGI + filtro SVGF) para nada,
    // justo en los PC de gama baja. Con rayos sigue: su cache de radiancia la
    // leen tambien los reflejos.
    if (!post_.global_illumination && !baked && !ray_traced) {
        // El SSR lee la imagen HDR del frame anterior: recien creada, se pasa
        // a textura como hace la GI.
        if (!scene_history_valid_ && !isolated()) {
            pipelineBarrier(cmd, colorBarrier(*scene_color_.handle(), vk::ImageLayout::eUndefined,
                                              vk::ImageLayout::eShaderReadOnlyOptimal,
                                              vk::PipelineStageFlagBits2::eTopOfPipe, vk::AccessFlagBits2::eNone,
                                              vk::PipelineStageFlagBits2::eFragmentShader,
                                              vk::AccessFlagBits2::eShaderSampledRead));
        }
        if (!isolated()) {
            gi_filter_history_valid_ = false;  // al volver, sin la luz de hace rato
            ssr_history_ready_ = scene_history_valid_;
            scene_history_valid_ = true;
        }
        return;
    }

    // Con rayos, la imagen la escribe un compute shader (layout General).
    std::vector<vk::ImageMemoryBarrier2> barriers = {
        ray_traced ? colorBarrier(*gi_raw_.handle(), vk::ImageLayout::eUndefined,
                                  vk::ImageLayout::eGeneral,
                                  vk::PipelineStageFlagBits2::eFragmentShader,
                                  vk::AccessFlagBits2::eShaderSampledRead,
                                  vk::PipelineStageFlagBits2::eComputeShader,
                                  vk::AccessFlagBits2::eShaderStorageWrite)
                   : discardToAttachment(*gi_raw_.handle())};

    // La imagen HDR guarda aun el frame anterior (ya como textura). Recien
    // creada no tiene nada: se pasa a textura para poder enlazarla y el
    // shader no la lee.
    if (!scene_history_valid_ && !isolated()) {
        barriers.push_back(colorBarrier(*scene_color_.handle(), vk::ImageLayout::eUndefined,
                                        vk::ImageLayout::eShaderReadOnlyOptimal,
                                        vk::PipelineStageFlagBits2::eTopOfPipe,
                                        vk::AccessFlagBits2::eNone,
                                        vk::PipelineStageFlagBits2::eFragmentShader,
                                        vk::AccessFlagBits2::eShaderSampledRead));
    }
    pipelineBarrier(cmd, barriers);

    // Las caras de la sonda no tienen frame anterior (la imagen HDR es la de
    // la pantalla): solo la visibilidad del cielo, sin luz rebotada.
    GpuSsgiPush push{};
    push.previous_view_projection = previous_view_projection_;
    push.params = Vec4{scene_history_valid_ && !isolated() ? 1.0f : 0.0f, 1.0f, 0.0f, 0.0f};
    // Rayos distintos cada frame, y la sonda para lo que no esta en pantalla
    // (con el mismo fundido entre cubos que la iluminacion).
    push.extra.x = static_cast<float>(noise_frame_ % 64);
    if (probe_enabled_ && probe_ready_ && !capturing_) {
        const std::uint32_t previous = 1 - probe_cube_;
        const float current_weight = probe_fade_;
        const float previous_weight = probe_fade_ < 1.0f ? 1.0f - probe_fade_ : 0.0f;
        (probe_cube_ == 0 ? push.extra.y : push.extra.z) = current_weight;
        (previous == 0 ? push.extra.y : push.extra.z) = previous_weight;
    }

    vk::ImageMemoryBarrier2 raw_to_sampled = writtenToSampled(*gi_raw_.handle());
    if (ray_traced) {
        // El compute lee el G-buffer (profundidad y normales), la imagen
        // anterior y el entorno del IBL, que hasta aqui solo se prepararon
        // para los fragment shaders.
        memoryBarrier(cmd,
                      vk::PipelineStageFlagBits2::eLateFragmentTests |
                          vk::PipelineStageFlagBits2::eColorAttachmentOutput |
                          vk::PipelineStageFlagBits2::eComputeShader,
                      vk::AccessFlagBits2::eDepthStencilAttachmentWrite |
                          vk::AccessFlagBits2::eColorAttachmentWrite |
                          vk::AccessFlagBits2::eShaderStorageWrite,
                      vk::PipelineStageFlagBits2::eComputeShader,
                      vk::AccessFlagBits2::eShaderSampledRead |
                          vk::AccessFlagBits2::eShaderStorageRead);

        RayTracing::Push rt_push{};
        rt_push.previous_view_projection = previous_view_projection_;
        rt_push.params = Vec4{static_cast<float>(noise_frame_ % 64),
                              scene_history_valid_ ? 1.0f : 0.0f, 0.0f, 0.0f};
        ray_tracing_.record(cmd, frame_index, RayTracing::Pass::Gi, gi_raw_.extent(), rt_push);
        // La cache de radiancia en el mundo: las muestras de este frame a su
        // media (la leen los rayos de los reflejos y la GI del siguiente).
        ray_tracing_.record(cmd, frame_index, RayTracing::Pass::CacheResolve, RayTracing::cacheResolveExtent(),
                            rt_push);

        raw_to_sampled = colorBarrier(*gi_raw_.handle(), vk::ImageLayout::eGeneral,
                                      vk::ImageLayout::eShaderReadOnlyOptimal,
                                      vk::PipelineStageFlagBits2::eComputeShader,
                                      vk::AccessFlagBits2::eShaderStorageWrite,
                                      vk::PipelineStageFlagBits2::eComputeShader,
                                      vk::AccessFlagBits2::eShaderSampledRead);
    } else if (baked) {
        // Sondas dinamicas (DDGI): unas cuantas se rehacen con rayos antes de usarlas.
        if (dynamicProbesActive()) {
            RayTracing::Push probe_push{};
            const std::uint32_t n = std::min(dynamic_probes_per_frame_, baked_probe_count_);
            probe_push.params = Vec4{static_cast<float>(frame_count_ % 1024), static_cast<float>(dynamic_probe_cursor_),
                                     static_cast<float>(n), 0.85f};
            ray_tracing_.recordProbeUpdate(cmd, frame_index, *probe_update_layout_, *probe_update_sets_[0], probe_push, n);
            dynamic_probe_cursor_ = (dynamic_probe_cursor_ + n) % std::max(baked_probe_count_, 1u);
        }
        drawFullscreen(cmd, baked_gi_pass_, &baked_gi_sets_[frame_index], gi_raw_, &push);
        raw_to_sampled.dstStageMask = vk::PipelineStageFlagBits2::eComputeShader;
    } else {
        drawFullscreen(cmd, ssgi_pass_, &ssgi_sets_[frame_index], gi_raw_, &push);
        raw_to_sampled.dstStageMask = vk::PipelineStageFlagBits2::eComputeShader;
    }

    // --- Filtro SVGF: acumulacion temporal + a trous (compute) ---
    // Lo del frame anterior (la iluminacion leyo el resultado, las copias de
    // historia) tiene que haber terminado antes de reescribirlo; y el
    // G-buffer, estar listo para leerse desde compute.
    pipelineBarrier(cmd, raw_to_sampled);
    memoryBarrier(cmd,
                  vk::PipelineStageFlagBits2::eFragmentShader |
                      vk::PipelineStageFlagBits2::eComputeShader |
                      vk::PipelineStageFlagBits2::eCopy |
                      vk::PipelineStageFlagBits2::eLateFragmentTests |
                      vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                  vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite |
                      vk::AccessFlagBits2::eTransferRead | vk::AccessFlagBits2::eTransferWrite |
                      vk::AccessFlagBits2::eDepthStencilAttachmentWrite |
                      vk::AccessFlagBits2::eColorAttachmentWrite,
                  vk::PipelineStageFlagBits2::eComputeShader,
                  vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite);

    const vk::Extent2D half = gi_temporal_.extent();
    const auto dispatch = [&](const ComputePass& pass, const vk::raii::DescriptorSet& set,
                              const auto& constants) {
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *pass.pipeline());
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *pass.layout(), 0, *set,
                               nullptr);
        cmd.pushConstants<std::decay_t<decltype(constants)>>(
            *pass.layout(), vk::ShaderStageFlagBits::eCompute, 0, constants);
        cmd.dispatch((half.width + 7) / 8, (half.height + 7) / 8, 1);
        memoryBarrier(cmd, vk::PipelineStageFlagBits2::eComputeShader,
                      vk::AccessFlagBits2::eShaderWrite,
                      vk::PipelineStageFlagBits2::eComputeShader |
                          vk::PipelineStageFlagBits2::eFragmentShader |
                          vk::PipelineStageFlagBits2::eCopy,
                      vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite |
                          vk::AccessFlagBits2::eTransferRead);
    };

    // Las caras de la sonda no usan la historia (es de la camara de pantalla).
    // (Su historia es de esta vista: en VR, de este ojo.)
    GiTemporalPush temporal{};
    temporal.previous_view_projection = history_view_projection_;
    temporal.params = Vec4{gi_filter_history_valid_ && !isolated() ? 1.0f : 0.0f, 0.0f, 0.0f,
                           0.0f};
    dispatch(gi_temporal_pass_, gi_temporal_sets_[frame_index + eyeSet() * kMaxFramesInFlight], temporal);

    // Cadenas del filtro espacial por hueco de frame: 0 = la vista (ojo
    // izquierdo), 1 = caras de la sonda (no tocan la historia), 2 = ojo derecho.
    const std::uint32_t variant = isolated() ? 1u : eye_state_swapped_ ? 2u : 0u;
    const std::uint32_t chain = (frame_index * 3 + variant) * kGiAtrousIterations;
    for (std::uint32_t iteration = 0; iteration < kGiAtrousIterations; ++iteration) {
        GiAtrousPush atrous{};
        atrous.step = 1 << iteration;
        dispatch(gi_atrous_pass_, gi_atrous_sets_[chain + iteration], atrous);
    }

    if (!isolated()) {
        // Momentos de este frame -> los del siguiente (el color ya quedo en
        // gi_history_ en la primera pasada espacial).
        vk::ImageCopy region{};
        region.srcSubresource = vk::ImageSubresourceLayers{vk::ImageAspectFlagBits::eColor, 0, 0, 1};
        region.dstSubresource = region.srcSubresource;
        region.extent = vk::Extent3D{half.width, half.height, 1};
        cmd.copyImage(*gi_moments_.handle(), vk::ImageLayout::eGeneral,
                      *gi_moments_history_.handle(), vk::ImageLayout::eGeneral, region);
        memoryBarrier(cmd, vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferWrite,
                      vk::PipelineStageFlagBits2::eComputeShader,
                      vk::AccessFlagBits2::eShaderRead);
        gi_filter_history_valid_ = true;
    }

    if (isolated()) {
        return;
    }

    // El SSR de este frame puede leer la imagen anterior solo si ya existia.
    ssr_history_ready_ = scene_history_valid_;

    scene_history_valid_ = true;
}

void VulkanRenderer::recordSsrPass(const vk::raii::CommandBuffer& cmd,
                                   std::uint32_t frame_index) {
    // Con los reflejos apagados tampoco se trazan: el compute no corre y la
    // imagen se deja vacia como con el SSR.
    const bool ray_traced = rayTracingActive() && !isolated() && post_.reflections;
    vk::ImageMemoryBarrier2 raw_to_sampled = writtenToSampled(*ssr_raw_.handle());
    ssr_image_from_compute_ = false;
    ssr_skipped_ = false;

    // Reflejos apagados (preset Bajo, presupuesto adaptativo, movil): el SSR
    // escribe ceros y la iluminacion usa el entorno. Con la imagen ya a ceros
    // no hace falta repetirlo cada frame: eran dos pasadas a pantalla
    // completa mas la copia a la historia, en los PC que menos pueden.
    // (Las vistas aisladas siempre lo escriben con ceros: no lo estropean.)
    if (!post_.reflections && !isolated() && ssr_zeroed_) {
        ssr_skipped_ = true;
        ssr_filter_history_valid_ = false;  // al volver, sin reflejos de hace rato
        rt_reflection_history_valid_ = false;
        return;
    }

    if (ray_traced) {
        pipelineBarrier(cmd, colorBarrier(*ssr_raw_.handle(), vk::ImageLayout::eUndefined,
                                          vk::ImageLayout::eGeneral,
                                          vk::PipelineStageFlagBits2::eFragmentShader |
                                              vk::PipelineStageFlagBits2::eComputeShader,
                                          vk::AccessFlagBits2::eShaderSampledRead,
                                          vk::PipelineStageFlagBits2::eComputeShader,
                                          vk::AccessFlagBits2::eShaderStorageWrite));
        RayTracing::Push rt_push{};
        rt_push.previous_view_projection = previous_view_projection_;
        rt_push.params = Vec4{static_cast<float>(noise_frame_ % 64),
                              ssr_history_ready_ ? 1.0f : 0.0f, 0.0f, 0.0f};
        ray_tracing_.record(cmd, frame_index, RayTracing::Pass::Reflections, ssr_raw_.extent(),
                            rt_push);
        // Lo lee su filtro (compute): acumulacion temporal + a trous, que
        // escriben ssr_image_ directamente (sin el filtro del SSR).
        pipelineBarrier(cmd, colorBarrier(*ssr_raw_.handle(), vk::ImageLayout::eGeneral,
                                          vk::ImageLayout::eShaderReadOnlyOptimal,
                                          vk::PipelineStageFlagBits2::eComputeShader,
                                          vk::AccessFlagBits2::eShaderStorageWrite,
                                          vk::PipelineStageFlagBits2::eComputeShader,
                                          vk::AccessFlagBits2::eShaderSampledRead));
        recordRtReflectionFilter(cmd, frame_index);
        ssr_zeroed_ = false;
        return;
    } else {
        // Lo acumulado por el filtro de los reflejos por rayos se queda viejo.
        if (!isolated()) rt_reflection_history_valid_ = false;
        // Va despues de la GI: la imagen HDR ya esta como textura.
        pipelineBarrier(cmd, discardToAttachment(*ssr_raw_.handle()));

        GpuSsgiPush push{};
        push.previous_view_projection = previous_view_projection_;
        // Sin frame anterior o con el SSR apagado, el shader no refleja nada.
        // z: numero de frame, para que el ruido del primer paso cambie cada
        // frame.
        push.params = Vec4{post_.reflections && ssr_history_ready_ && !isolated() ? 1.0f : 0.0f,
                           1.0f, static_cast<float>(noise_frame_ % 64), 0.0f};
        drawFullscreen(cmd, ssr_pass_, &ssr_sets_[frame_index], ssr_raw_, &push);
    }

    // --- Filtro temporal ---
    // La historia recien creada no tiene nada: se pasa a textura para poder
    // enlazarla y el shader no la lee.
    std::vector<vk::ImageMemoryBarrier2> barriers = {raw_to_sampled,
                                                     discardToAttachment(*ssr_image_.handle())};
    if (!ssr_filter_history_valid_) {
        barriers.push_back(colorBarrier(*ssr_history_.handle(), vk::ImageLayout::eUndefined,
                                        vk::ImageLayout::eShaderReadOnlyOptimal,
                                        vk::PipelineStageFlagBits2::eTopOfPipe,
                                        vk::AccessFlagBits2::eNone,
                                        vk::PipelineStageFlagBits2::eFragmentShader,
                                        vk::AccessFlagBits2::eShaderSampledRead));
    }
    pipelineBarrier(cmd, barriers);

    // Las caras de la sonda no usan la historia (es de la camara de pantalla).
    // (Su historia es de esta vista: en VR, de este ojo.)
    GpuSsgiPush resolve{};
    resolve.previous_view_projection = history_view_projection_;
    resolve.params = Vec4{ssr_filter_history_valid_ && post_.reflections && !isolated() ? 1.0f : 0.0f,
                          kSsrHistoryWeight, 0.0f, 0.0f};
    drawFullscreen(cmd, ssr_resolve_pass_, &ssr_resolve_sets_[frame_index + eyeSet() * kMaxFramesInFlight],
                   ssr_image_, &resolve);
    // Apagados, las dos pasadas dejan ceros en toda la imagen.
    if (!isolated()) ssr_zeroed_ = !post_.reflections;
}

void VulkanRenderer::recordFilterHistoryCopies(const vk::raii::CommandBuffer& cmd) {
    if (isolated()) return;  // la segunda vista no escribe las historias
    if (ssr_skipped_) return;  // reflejos apagados: nada nuevo que guardar
    // La iluminacion ya leyo los reflejos y la luz rebotada filtrados: se
    // copian a sus historias. Todas quedan como textura al terminar.
    constexpr vk::ImageLayout kRead = vk::ImageLayout::eShaderReadOnlyOptimal;
    // (La GI guarda su historia en su propio filtro: recordSsgiPass.)
    const std::array<std::pair<const VulkanImage*, const VulkanImage*>, 1> copies = {{
        {&ssr_image_, &ssr_history_},
    }};

    std::vector<vk::ImageMemoryBarrier2> before;
    std::vector<vk::ImageMemoryBarrier2> after;
    for (const auto& [source, history] : copies) {
        before.push_back(colorBarrier(*source->handle(), kRead,
                                      vk::ImageLayout::eTransferSrcOptimal,
                                      vk::PipelineStageFlagBits2::eFragmentShader,
                                      vk::AccessFlagBits2::eShaderSampledRead,
                                      vk::PipelineStageFlagBits2::eCopy,
                                      vk::AccessFlagBits2::eTransferRead));
        before.push_back(colorBarrier(*history->handle(), kRead,
                                      vk::ImageLayout::eTransferDstOptimal,
                                      vk::PipelineStageFlagBits2::eFragmentShader,
                                      vk::AccessFlagBits2::eShaderSampledRead,
                                      vk::PipelineStageFlagBits2::eCopy,
                                      vk::AccessFlagBits2::eTransferWrite));
        after.push_back(colorBarrier(*source->handle(), vk::ImageLayout::eTransferSrcOptimal,
                                     kRead, vk::PipelineStageFlagBits2::eCopy,
                                     vk::AccessFlagBits2::eTransferRead,
                                     vk::PipelineStageFlagBits2::eFragmentShader,
                                     vk::AccessFlagBits2::eShaderSampledRead));
        after.push_back(colorBarrier(*history->handle(), vk::ImageLayout::eTransferDstOptimal,
                                     kRead, vk::PipelineStageFlagBits2::eCopy,
                                     vk::AccessFlagBits2::eTransferWrite,
                                     vk::PipelineStageFlagBits2::eFragmentShader,
                                     vk::AccessFlagBits2::eShaderSampledRead));
    }
    pipelineBarrier(cmd, before);

    for (const auto& [source, history] : copies) {
        const vk::Extent2D extent = source->extent();
        vk::ImageCopy region{};
        region.srcSubresource =
            vk::ImageSubresourceLayers{vk::ImageAspectFlagBits::eColor, 0, 0, 1};
        region.dstSubresource = region.srcSubresource;
        region.extent = vk::Extent3D{extent.width, extent.height, 1};
        cmd.copyImage(*source->handle(), vk::ImageLayout::eTransferSrcOptimal,
                      *history->handle(), vk::ImageLayout::eTransferDstOptimal, region);
    }

    pipelineBarrier(cmd, after);
    ssr_filter_history_valid_ = true;
}

void VulkanRenderer::recordLightingPass(const vk::raii::CommandBuffer& cmd,
                                        std::uint32_t frame_index, const VulkanImage& target) {
    const vk::Extent2D extent = render_extent_;

    // El SSAO pasa a textura y el destino a destino de color: la imagen HDR
    // la dejo el frame anterior como textura del bloom y la composicion; la
    // de la sonda, como origen de la copia al cubo.
    // (La GI la dejo lista su filtro, en compute; los reflejos por rayos,
    // tambien: ssr_image_ ya es textura.)
    std::vector<vk::ImageMemoryBarrier2> barriers = {writtenToSampled(*ssao_image_.handle())};
    // (Saltado con los reflejos apagados: sigue como textura del frame anterior.)
    if (!ssr_image_from_compute_ && !ssr_skipped_) barriers.push_back(writtenToSampled(*ssr_image_.handle()));
    pipelineBarrier(cmd, barriers);
    pipelineBarrier(cmd, {colorBarrier(*target.handle(), vk::ImageLayout::eUndefined,
                                       vk::ImageLayout::eColorAttachmentOptimal,
                                       vk::PipelineStageFlagBits2::eFragmentShader |
                                           vk::PipelineStageFlagBits2::eBlit,
                                       vk::AccessFlagBits2::eShaderSampledRead |
                                           vk::AccessFlagBits2::eTransferRead,
                                       vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                       vk::AccessFlagBits2::eColorAttachmentWrite)});

    vk::RenderingAttachmentInfo color_attachment{};
    color_attachment.imageView = *target.view();
    color_attachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
    // El triangulo cubre toda la pantalla, asi que no hace falta limpiar.
    color_attachment.loadOp = vk::AttachmentLoadOp::eDontCare;
    color_attachment.storeOp = vk::AttachmentStoreOp::eStore;

    vk::RenderingInfo rendering_info{};
    rendering_info.renderArea = vk::Rect2D{vk::Offset2D{0, 0}, extent};
    rendering_info.layerCount = 1;
    rendering_info.setColorAttachments(color_attachment);

    compat::beginRendering(cmd, rendering_info);

    // La ligera en los PC de gama baja (presupuesto) y en el modo compatible
    // (su shader ya lo es).
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *lighting_pass_.pipeline(liteLighting()));
    cmd.setViewport(0, vk::Viewport{0.0f, 0.0f, static_cast<float>(extent.width),
                                    static_cast<float>(extent.height), 0.0f, 1.0f});
    cmd.setScissor(0, vk::Rect2D{vk::Offset2D{0, 0}, extent});
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *lighting_pass_.layout(), 0,
                           *lighting_sets_[frame_index], nullptr);

    cmd.draw(3, 1, 0, 0);

    compat::endRendering(cmd);
}

void VulkanRenderer::copySceneForTransparency(const vk::raii::CommandBuffer& cmd) {
    const vk::Extent2D extent = render_extent_;
    pipelineBarrier(cmd, {colorBarrier(*scene_color_.handle(),
                                       vk::ImageLayout::eColorAttachmentOptimal,
                                       vk::ImageLayout::eTransferSrcOptimal,
                                       vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                       vk::AccessFlagBits2::eColorAttachmentWrite,
                                       vk::PipelineStageFlagBits2::eCopy,
                                       vk::AccessFlagBits2::eTransferRead),
                          colorBarrier(*glass_source_.handle(), vk::ImageLayout::eUndefined,
                                       vk::ImageLayout::eTransferDstOptimal,
                                       vk::PipelineStageFlagBits2::eFragmentShader,
                                       vk::AccessFlagBits2::eShaderSampledRead,
                                       vk::PipelineStageFlagBits2::eCopy,
                                       vk::AccessFlagBits2::eTransferWrite)});
    vk::ImageCopy region{};
    region.srcSubresource = vk::ImageSubresourceLayers{vk::ImageAspectFlagBits::eColor, 0, 0, 1};
    region.dstSubresource = region.srcSubresource;
    region.extent = vk::Extent3D{extent.width, extent.height, 1};
    cmd.copyImage(*scene_color_.handle(), vk::ImageLayout::eTransferSrcOptimal,
                  *glass_source_.handle(), vk::ImageLayout::eTransferDstOptimal, region);

    // La profundidad sigue en solo lectura (la leyo la iluminacion): ahora
    // tambien la lee la prueba de profundidad.
    vk::ImageMemoryBarrier2 depth_barrier{};
    depth_barrier.srcStageMask = vk::PipelineStageFlagBits2::eLateFragmentTests;
    depth_barrier.srcAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
    depth_barrier.dstStageMask = vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                                 vk::PipelineStageFlagBits2::eLateFragmentTests;
    depth_barrier.dstAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentRead;
    depth_barrier.oldLayout = compat::depthReadOnlyLayout();
    depth_barrier.newLayout = compat::depthReadOnlyLayout();
    depth_barrier.image = *gbuffer_.depth().handle();
    depth_barrier.subresourceRange =
        vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1};

    pipelineBarrier(cmd, {colorBarrier(*scene_color_.handle(), vk::ImageLayout::eTransferSrcOptimal,
                                       vk::ImageLayout::eColorAttachmentOptimal,
                                       vk::PipelineStageFlagBits2::eCopy,
                                       vk::AccessFlagBits2::eTransferRead,
                                       vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                       vk::AccessFlagBits2::eColorAttachmentRead |
                                           vk::AccessFlagBits2::eColorAttachmentWrite),
                          colorBarrier(*glass_source_.handle(),
                                       vk::ImageLayout::eTransferDstOptimal,
                                       vk::ImageLayout::eShaderReadOnlyOptimal,
                                       vk::PipelineStageFlagBits2::eCopy,
                                       vk::AccessFlagBits2::eTransferWrite,
                                       vk::PipelineStageFlagBits2::eFragmentShader,
                                       vk::AccessFlagBits2::eShaderSampledRead),
                          depth_barrier});
}

// Agua: sobre la imagen HDR (tras el vidrio), con la copia de lo que hay
// detras para la refraccion y la profundidad de solo lectura.
void VulkanRenderer::recordWaterPass(const vk::raii::CommandBuffer& cmd, std::uint32_t frame_index) {
    copySceneForTransparency(cmd);
    const vk::Extent2D extent = render_extent_;

    // El agua se prueba y ESCRIBE en una copia de la profundidad de la escena
    // (las olas se tapan entre si); el shader sigue leyendo la de la escena,
    // sin agua, para el grosor y la refraccion.
    const auto depth_barrier = [](vk::Image image, vk::ImageLayout from, vk::ImageLayout to,
                                  vk::PipelineStageFlags2 src_stage, vk::AccessFlags2 src_access,
                                  vk::PipelineStageFlags2 dst_stage, vk::AccessFlags2 dst_access) {
        vk::ImageMemoryBarrier2 b{};
        b.srcStageMask = src_stage;
        b.srcAccessMask = src_access;
        b.dstStageMask = dst_stage;
        b.dstAccessMask = dst_access;
        b.oldLayout = from;
        b.newLayout = to;
        b.image = image;
        b.subresourceRange = vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1};
        return b;
    };
    using Stage = vk::PipelineStageFlagBits2;
    using Access = vk::AccessFlagBits2;
    const Stage tests = Stage::eEarlyFragmentTests;
    pipelineBarrier(cmd, {depth_barrier(*gbuffer_.depth().handle(), compat::depthReadOnlyLayout(),
                                        vk::ImageLayout::eTransferSrcOptimal,
                                        tests | Stage::eLateFragmentTests | Stage::eFragmentShader | Stage::eComputeShader,
                                        Access::eDepthStencilAttachmentRead | Access::eShaderSampledRead,
                                        Stage::eCopy, Access::eTransferRead),
                          depth_barrier(*water_depth_.handle(), vk::ImageLayout::eUndefined,
                                        vk::ImageLayout::eTransferDstOptimal, tests | Stage::eLateFragmentTests,
                                        Access::eDepthStencilAttachmentWrite, Stage::eCopy, Access::eTransferWrite)});
    vk::ImageCopy depth_region{};
    depth_region.srcSubresource = vk::ImageSubresourceLayers{vk::ImageAspectFlagBits::eDepth, 0, 0, 1};
    depth_region.dstSubresource = depth_region.srcSubresource;
    depth_region.extent = vk::Extent3D{extent.width, extent.height, 1};
    cmd.copyImage(*gbuffer_.depth().handle(), vk::ImageLayout::eTransferSrcOptimal, *water_depth_.handle(),
                  vk::ImageLayout::eTransferDstOptimal, depth_region);
    pipelineBarrier(cmd, {depth_barrier(*gbuffer_.depth().handle(), vk::ImageLayout::eTransferSrcOptimal,
                                        compat::depthReadOnlyLayout(), Stage::eCopy, Access::eTransferRead,
                                        tests | Stage::eLateFragmentTests | Stage::eFragmentShader | Stage::eComputeShader,
                                        Access::eDepthStencilAttachmentRead | Access::eShaderSampledRead),
                          depth_barrier(*water_depth_.handle(), vk::ImageLayout::eTransferDstOptimal,
                                        compat::depthAttachmentLayout(), Stage::eCopy, Access::eTransferWrite,
                                        tests | Stage::eLateFragmentTests,
                                        Access::eDepthStencilAttachmentRead | Access::eDepthStencilAttachmentWrite)});

    vk::RenderingAttachmentInfo color_attachment{};
    color_attachment.imageView = *scene_color_.view();
    color_attachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
    color_attachment.loadOp = vk::AttachmentLoadOp::eLoad;
    color_attachment.storeOp = vk::AttachmentStoreOp::eStore;
    vk::RenderingAttachmentInfo depth_attachment{};
    depth_attachment.imageView = *water_depth_.view();
    depth_attachment.imageLayout = compat::depthAttachmentLayout();
    depth_attachment.loadOp = vk::AttachmentLoadOp::eLoad;
    depth_attachment.storeOp = vk::AttachmentStoreOp::eDontCare;
    vk::RenderingInfo rendering_info{};
    rendering_info.renderArea = vk::Rect2D{vk::Offset2D{0, 0}, extent};
    rendering_info.layerCount = 1;
    rendering_info.setColorAttachments(color_attachment);
    rendering_info.pDepthAttachment = &depth_attachment;

    compat::beginRendering(cmd, rendering_info);
    cmd.setViewport(0, vk::Viewport{0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height),
                                    0.0f, 1.0f});
    cmd.setScissor(0, vk::Rect2D{vk::Offset2D{0, 0}, extent});
    water_pass_.record(cmd, frame_index, skin_sets_[frame_index], glass_sets_[frame_index]);
    compat::endRendering(cmd);
}

void VulkanRenderer::recordPathTracePass(const vk::raii::CommandBuffer& cmd, std::uint32_t frame_index) {
    // Solo la vista principal en Lit: la acumulacion es de una camara.
    if (!pathTracingActive() || isolated() || xr_view_frame_ || drawModeNow() != SceneDrawMode::Lit) {
        return;
    }

    // --- Empezar de nuevo si algo cambio ---
    // Camara (cualquier movimiento), objetos que se mueven o animan, luces,
    // materiales y geometria (los marcan updateUniforms y compania).
    bool camera_moved = false;
    for (int c = 0; c < 4 && !camera_moved; ++c) {
        for (int r = 0; r < 4; ++r) {
            if (std::abs(camera_view_projection_.m[c][r] - path_tracing_view_projection_.m[c][r]) > 1e-6f) {
                camera_moved = true;
                break;
            }
        }
    }
    if (camera_moved || !moved_spheres_.empty() || path_tracing_reset_ || !path_tracing_layout_ready_) {
        path_tracing_samples_ = 0;
        path_tracing_view_projection_ = camera_view_projection_;
        path_tracing_reset_ = false;
    }
    const bool trace = path_tracing_samples_ < path_tracing_max_samples_;

    const vk::Image scene = *scene_color_.handle();
    const vk::Image accumulation = *path_tracing_accumulation_.handle();
    pipelineBarrier(cmd, {colorBarrier(scene, vk::ImageLayout::eColorAttachmentOptimal, vk::ImageLayout::eGeneral,
                                       vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                       vk::AccessFlagBits2::eColorAttachmentWrite,
                                       vk::PipelineStageFlagBits2::eComputeShader,
                                       vk::AccessFlagBits2::eShaderStorageWrite),
                          colorBarrier(accumulation,
                                       path_tracing_layout_ready_ ? vk::ImageLayout::eGeneral : vk::ImageLayout::eUndefined,
                                       vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                                       vk::AccessFlagBits2::eShaderStorageWrite,
                                       vk::PipelineStageFlagBits2::eComputeShader,
                                       vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite)});
    path_tracing_layout_ready_ = true;

    RayTracing::Push push{};
    push.previous_view_projection = previous_view_projection_;
    push.params = Vec4{static_cast<float>(path_tracing_samples_ + frame_count_ * 7919u % 65536u),
                       path_tracing_samples_ > 0 ? 1.0f : 0.0f, static_cast<float>(path_tracing_bounces_),
                       trace ? 1.0f : 0.0f};
    ray_tracing_.record(cmd, frame_index, RayTracing::Pass::PathTrace, render_extent_, push);
    if (trace) ++path_tracing_samples_;

    // De vuelta a destino de color: el vidrio, el agua y las particulas se
    // dibujan encima.
    pipelineBarrier(cmd, {colorBarrier(scene, vk::ImageLayout::eGeneral, vk::ImageLayout::eColorAttachmentOptimal,
                                       vk::PipelineStageFlagBits2::eComputeShader,
                                       vk::AccessFlagBits2::eShaderStorageWrite,
                                       vk::PipelineStageFlagBits2::eColorAttachmentOutput |
                                           vk::PipelineStageFlagBits2::eTransfer,
                                       vk::AccessFlagBits2::eColorAttachmentRead |
                                           vk::AccessFlagBits2::eColorAttachmentWrite |
                                           vk::AccessFlagBits2::eTransferRead)});
}

void VulkanRenderer::recordGlassPass(const vk::raii::CommandBuffer& cmd,
                                     std::uint32_t frame_index) {
    // --- Que vidrio se ve ---
    // Pocas submallas (las ventanas): se prueban en la CPU contra el frustum.
    const core::Frustum frustum(camera_view_projection_);
    struct GlassDraw {
        const ActorDraw* actor;
        std::uint32_t submesh;
    };
    std::vector<GlassDraw> visible;
    for (const ActorDraw& draw : actor_draws_) {
        if (draw.shadows_only) {
            continue;
        }
        const SkinnedModel& model = skinned_models_[draw.model];
        const auto& submeshes = model.submeshes();
        for (std::uint32_t i = 0; i < submeshes.size(); ++i) {
            if (!model.materials()[submeshes[i].material].transparent) {
                continue;
            }
            if (draw.per_submesh && !frustum.intersects(submesh_bounds_[draw.first_bounds + i])) {
                continue;
            }
            visible.push_back(GlassDraw{&draw, i});
        }
    }
    if (visible.empty()) {
        return;
    }

    // --- Copia de la imagen sin vidrio: es lo que el vidrio refleja ---
    copySceneForTransparency(cmd);
    const vk::Extent2D extent = render_extent_;

    // --- Dibujo, mezclado sobre la imagen HDR ---
    vk::RenderingAttachmentInfo color_attachment{};
    color_attachment.imageView = *scene_color_.view();
    color_attachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
    color_attachment.loadOp = vk::AttachmentLoadOp::eLoad;
    color_attachment.storeOp = vk::AttachmentStoreOp::eStore;

    vk::RenderingAttachmentInfo depth_attachment{};
    depth_attachment.imageView = *gbuffer_.depth().view();
    depth_attachment.imageLayout = compat::depthReadOnlyLayout();
    depth_attachment.loadOp = vk::AttachmentLoadOp::eLoad;
    depth_attachment.storeOp = vk::AttachmentStoreOp::eNone;

    vk::RenderingInfo rendering_info{};
    rendering_info.renderArea = vk::Rect2D{vk::Offset2D{0, 0}, extent};
    rendering_info.layerCount = 1;
    rendering_info.setColorAttachments(color_attachment);
    rendering_info.pDepthAttachment = &depth_attachment;

    compat::beginRendering(cmd, rendering_info);
    cmd.setViewport(0, vk::Viewport{0.0f, 0.0f, static_cast<float>(extent.width),
                                    static_cast<float>(extent.height), 0.0f, 1.0f});
    cmd.setScissor(0, vk::Rect2D{vk::Offset2D{0, 0}, extent});
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *skinned_pass_.glassPipeline());
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *skinned_pass_.glassLayout(), 0,
                           *skin_sets_[frame_index], nullptr);
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *skinned_pass_.glassLayout(), 2,
                           *glass_sets_[frame_index], nullptr);

    const ActorDraw* bound_actor = nullptr;
    std::uint32_t bound_material = UINT32_MAX;
    for (const GlassDraw& glass : visible) {
        const ActorDraw& draw = *glass.actor;
        const SkinnedModel& model = skinned_models_[draw.model];
        if (&draw != bound_actor) {
            cmd.bindVertexBuffers(0, *model.vertices().handle(), {0});
            cmd.bindIndexBuffer(*model.indices().handle(), 0, vk::IndexType::eUint32);
            bound_actor = &draw;
            bound_material = UINT32_MAX;
        }
        const asset::SubMesh& submesh = model.submeshes()[glass.submesh];
        if (submesh.material != bound_material) {
            const SkinnedModel::Material& material = model.materials()[submesh.material];
            cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *skinned_pass_.glassLayout(),
                                   1, *model.materialSet(submesh.material), nullptr);
            GpuSkinnedPush push{};
            push.model = draw.transform;
            push.base_color = material.base_color;
            push.emissive = material.emissive;
            push.material = material.params;
            push.bone_offset = draw.bone_offset;
            push.reflectance = material.reflectance;
            push.pick_id = material.shading;  // transmision de Disney (glass.frag)
            cmd.pushConstants<GpuSkinnedPush>(
                *skinned_pass_.glassLayout(),
                vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment, 0, push);
            bound_material = submesh.material;
        }
        cmd.drawIndexed(submesh.index_count, 1, submesh.first_index, 0, 0);
    }

    compat::endRendering(cmd);
}

void VulkanRenderer::recordOutlinePass(const vk::raii::CommandBuffer& cmd,
                                       std::uint32_t frame_index) {
    if (capturing_) {
        return;
    }
    std::vector<const ActorDraw*> selected;
    for (const ActorDraw& draw : actor_draws_) {
        if (std::find(outlined_actors_.begin(), outlined_actors_.end(), draw.scene_actor) !=
            outlined_actors_.end()) {
            selected.push_back(&draw);
        }
    }
    if (selected.empty()) {
        return;
    }

    // --- 1) Mascara: silueta entera (R) y parte visible (G) ---
    // A la resolucion de pantalla (linea fina y nitida); con escalado se
    // prueba contra la profundidad copiada a ese tamano.
    const bool scaled = render_extent_ != outline_mask_.extent();
    if (scaled) recordOutputDepth(cmd);
    const VulkanImage& depth_image = scaled ? output_depth_ : gbuffer_.depth();
    // La profundidad sigue en solo lectura (la leyeron la iluminacion y el
    // vidrio): ahora tambien la prueba de profundidad.
    vk::ImageMemoryBarrier2 depth_barrier{};
    depth_barrier.srcStageMask = vk::PipelineStageFlagBits2::eLateFragmentTests |
                                 vk::PipelineStageFlagBits2::eFragmentShader |
                                 vk::PipelineStageFlagBits2::eComputeShader;
    depth_barrier.srcAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
    depth_barrier.dstStageMask = vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                                 vk::PipelineStageFlagBits2::eLateFragmentTests;
    depth_barrier.dstAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentRead;
    depth_barrier.oldLayout = compat::depthReadOnlyLayout();
    depth_barrier.newLayout = compat::depthReadOnlyLayout();
    depth_barrier.image = *depth_image.handle();
    depth_barrier.subresourceRange =
        vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1};
    pipelineBarrier(cmd, {discardToAttachment(*outline_mask_.handle()), depth_barrier});

    const vk::Extent2D extent = outline_mask_.extent();
    vk::RenderingAttachmentInfo mask_attachment{};
    mask_attachment.imageView = *outline_mask_.view();
    mask_attachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
    mask_attachment.loadOp = vk::AttachmentLoadOp::eClear;
    mask_attachment.storeOp = vk::AttachmentStoreOp::eStore;
    mask_attachment.clearValue = vk::ClearValue{vk::ClearColorValue{0.0f, 0.0f, 0.0f, 0.0f}};

    vk::RenderingAttachmentInfo depth_attachment{};
    depth_attachment.imageView = *depth_image.view();
    depth_attachment.imageLayout = compat::depthReadOnlyLayout();
    depth_attachment.loadOp = vk::AttachmentLoadOp::eLoad;
    depth_attachment.storeOp = vk::AttachmentStoreOp::eNone;

    vk::RenderingInfo rendering_info{};
    rendering_info.renderArea = vk::Rect2D{vk::Offset2D{0, 0}, extent};
    rendering_info.layerCount = 1;
    rendering_info.setColorAttachments(mask_attachment);
    rendering_info.pDepthAttachment = &depth_attachment;

    compat::beginRendering(cmd, rendering_info);
    cmd.setViewport(0, vk::Viewport{0.0f, 0.0f, static_cast<float>(extent.width),
                                    static_cast<float>(extent.height), 0.0f, 1.0f});
    cmd.setScissor(0, vk::Rect2D{vk::Offset2D{0, 0}, extent});
    // Solo las submallas en el campo de vision (seleccionar un escenario
    // entero, p. ej. la ciudad de Bistro, no debe dibujarlo todo dos veces).
    const core::Frustum frustum(camera_view_projection_);
    for (int visible_only = 0; visible_only < 2; ++visible_only) {
        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics,
                         *skinned_pass_.outlineMaskPipeline(visible_only != 0));
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *skinned_pass_.geometryLayout(),
                               0, *skin_sets_[frame_index], nullptr);
        for (const ActorDraw* draw : selected) {
            const SkinnedModel& model = skinned_models_[draw->model];
            cmd.bindVertexBuffers(0, *model.vertices().handle(), {0});
            cmd.bindIndexBuffer(*model.indices().handle(), 0, vk::IndexType::eUint32);
            GpuSkinnedPush push{};
            push.model = draw->transform;
            push.bone_offset = draw->bone_offset;
            push.flags = 2u;  // sin jitter: el contorno no tiembla con el TAA
            cmd.pushConstants<GpuSkinnedPush>(*skinned_pass_.geometryLayout(),
                                              skinned_pass_.geometryPushStages(), 0, push);
            // Todas las submallas, opacas y de vidrio.
            const auto& submeshes = model.submeshes();
            for (std::uint32_t i = 0; i < submeshes.size(); ++i) {
                if (draw->per_submesh &&
                    !frustum.intersects(submesh_bounds_[draw->first_bounds + i])) {
                    continue;
                }
                cmd.drawIndexed(submeshes[i].index_count, 1, submeshes[i].first_index, 0, 0);
            }
        }
    }
    compat::endRendering(cmd);

    // --- 2) Contorno sobre la imagen compuesta ---
    pipelineBarrier(cmd, {writtenToSampled(*outline_mask_.handle()),
                          colorBarrier(*ldr_color_.handle(), vk::ImageLayout::eShaderReadOnlyOptimal,
                                       vk::ImageLayout::eColorAttachmentOptimal,
                                       vk::PipelineStageFlagBits2::eFragmentShader,
                                       vk::AccessFlagBits2::eShaderSampledRead,
                                       vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                       vk::AccessFlagBits2::eColorAttachmentRead |
                                           vk::AccessFlagBits2::eColorAttachmentWrite)});
    drawFullscreen<GpuBloomPush>(cmd, outline_pass_, &outline_sets_[0], ldr_color_, nullptr,
                                 /*load=*/true);
    pipelineBarrier(cmd, writtenToSampled(*ldr_color_.handle()));
}

std::uint32_t VulkanRenderer::createUiTexture(const std::uint8_t* rgba, std::uint32_t width, std::uint32_t height) {
    if (!initialized_ || rgba == nullptr || width == 0 || height == 0) return 0;
    const std::uint32_t id = next_ui_texture_++;
    VulkanTexture& texture = ui_textures_[id];
    texture.create(device_, width, height, rgba);
    return id;
}

VkImageView VulkanRenderer::uiTextureView(std::uint32_t id) const {
    const auto it = ui_textures_.find(id);
    return it != ui_textures_.end() ? static_cast<VkImageView>(*it->second.view()) : VK_NULL_HANDLE;
}

void VulkanRenderer::destroyUiTextures(const std::vector<std::uint32_t>& ids) {
    if (ids.empty()) return;
    waitIdle();  // algun frame en vuelo puede estar dibujandolas
    for (const std::uint32_t id : ids) ui_textures_.erase(id);
}

std::uint32_t VulkanRenderer::createTerrain(std::uint32_t resolution, std::uint32_t splat_resolution) {
    if (!initialized_) return 0;
    const std::uint32_t id = terrain_pass_.createTerrain(resolution, splat_resolution);
    cascades_valid_ = false;
    return id;
}

void VulkanRenderer::destroyTerrain(std::uint32_t id) {
    terrain_pass_.destroyTerrain(id);
    cascades_valid_ = false;
}

void VulkanRenderer::updateTerrainHeights(std::uint32_t id, const float* heights, std::uint32_t x, std::uint32_t y,
                                          std::uint32_t w, std::uint32_t h) {
    terrain_pass_.updateHeights(id, heights, x, y, w, h);
    cascades_valid_ = false;  // las sombras cacheadas ya no valen
}

void VulkanRenderer::shiftOrigin(const core::Vec3& offset) {
    world_origin_[0] += offset.x;
    world_origin_[1] += offset.y;
    world_origin_[2] += offset.z;
    voxel_pass_.shiftOrigin(offset);
    probe_position_ = probe_position_ - offset;
    probe_capture_position_ = probe_capture_position_ - offset;
    // Lo guardado del frame anterior esta en las coordenadas viejas.
    invalidateHistory();
    cascades_valid_ = false;
    local_shadows_.invalidate();
    rain_map_ready_ = false;
    previous_actor_motion_.clear();
    ray_tracing_.resetCache();  // sus celdas estan en las coordenadas viejas
}

void VulkanRenderer::invalidateHistory() {
    taa_history_valid_ = false;
    scene_history_valid_ = false;
    ssr_history_ready_ = false;
    ssr_filter_history_valid_ = false;
    gi_filter_history_valid_ = false;
    rt_shadow_history_valid_ = false;
    rt_reflection_history_valid_ = false;
    xr_eye1_.taa_valid = xr_eye1_.ssr_valid = xr_eye1_.gi_valid = false;  // el otro ojo
    xr_eye1_.rt_shadow_valid = xr_eye1_.rt_reflection_valid = false;
}

// El resultado del frame (imagen compuesta con contorno y gizmos) a la imagen
// de la vista activa. Las dos terminan como textura.
void VulkanRenderer::recordViewCopy(const vk::raii::CommandBuffer& cmd) {
    VulkanImage& target = view_images_[view_slot_];
    constexpr vk::ImageLayout kRead = vk::ImageLayout::eShaderReadOnlyOptimal;
    pipelineBarrier(cmd, {colorBarrier(*ldr_color_.handle(), kRead, vk::ImageLayout::eTransferSrcOptimal,
                                       vk::PipelineStageFlagBits2::eColorAttachmentOutput |
                                           vk::PipelineStageFlagBits2::eFragmentShader,
                                       vk::AccessFlagBits2::eColorAttachmentWrite | vk::AccessFlagBits2::eShaderSampledRead,
                                       vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferRead),
                          colorBarrier(*target.handle(), kRead, vk::ImageLayout::eTransferDstOptimal,
                                       vk::PipelineStageFlagBits2::eFragmentShader,
                                       vk::AccessFlagBits2::eShaderSampledRead, vk::PipelineStageFlagBits2::eCopy,
                                       vk::AccessFlagBits2::eTransferWrite)});
    const vk::Extent2D extent = ldr_color_.extent();
    vk::ImageCopy region{};
    region.srcSubresource = vk::ImageSubresourceLayers{vk::ImageAspectFlagBits::eColor, 0, 0, 1};
    region.dstSubresource = region.srcSubresource;
    region.extent = vk::Extent3D{extent.width, extent.height, 1};
    cmd.copyImage(*ldr_color_.handle(), vk::ImageLayout::eTransferSrcOptimal, *target.handle(),
                  vk::ImageLayout::eTransferDstOptimal, region);
    pipelineBarrier(cmd, {colorBarrier(*ldr_color_.handle(), vk::ImageLayout::eTransferSrcOptimal, kRead,
                                       vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferRead,
                                       vk::PipelineStageFlagBits2::eFragmentShader,
                                       vk::AccessFlagBits2::eShaderSampledRead),
                          colorBarrier(*target.handle(), vk::ImageLayout::eTransferDstOptimal, kRead,
                                       vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferWrite,
                                       vk::PipelineStageFlagBits2::eFragmentShader,
                                       vk::AccessFlagBits2::eShaderSampledRead)});
}

// -----------------------------------------------------------------------------
// Render Textures
// -----------------------------------------------------------------------------

void VulkanRenderer::retireRenderTextureImage(RenderTextureSlot& slot) {
    RetiredRenderTexture retired;
    retired.image = std::move(slot.image);
    retired.ui_view = std::move(slot.ui_view);
    retired.frames_left = kMaxFramesInFlight + 1;
    retired_render_textures_.push_back(std::move(retired));
    slot.image = VulkanImage{};
    slot.ui_view = nullptr;
}

bool VulkanRenderer::renderTextureValid(std::int32_t id) const {
    return id >= 0 && static_cast<std::size_t>(id) < render_textures_.size() && render_textures_[id]->alive;
}

void VulkanRenderer::createRenderTextureImage(RenderTextureSlot& slot, std::uint32_t width, std::uint32_t height) {
    // UNORM con vista sRGB: la composicion ya escribe con gamma (como la
    // pantalla) y la copia no la cambia; al leerla en un material, la vista
    // sRGB la devuelve lineal, como cualquier textura de color.
    slot.image.create(device_, vk::Extent2D{std::clamp(width, 1u, 8192u), std::clamp(height, 1u, 8192u)},
                      vk::Format::eR8G8B8A8Unorm,
                      vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
                      vk::ImageAspectFlagBits::eColor, 1, vk::Format::eR8G8B8A8Srgb);
    vk::ImageViewCreateInfo ui_info{};
    ui_info.image = *slot.image.handle();
    ui_info.viewType = vk::ImageViewType::e2D;
    ui_info.format = vk::Format::eR8G8B8A8Unorm;
    ui_info.subresourceRange = vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    slot.ui_view = vk::raii::ImageView(device_.handle(), ui_info);
    device_.submitOneTime([&](const vk::raii::CommandBuffer& cmd) {
        pipelineBarrier(cmd, colorBarrier(*slot.image.handle(), vk::ImageLayout::eUndefined,
                                          vk::ImageLayout::eTransferDstOptimal, vk::PipelineStageFlagBits2::eTopOfPipe,
                                          vk::AccessFlagBits2::eNone, vk::PipelineStageFlagBits2::eClear,
                                          vk::AccessFlagBits2::eTransferWrite));
        const vk::ClearColorValue black(std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f});
        const vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
        cmd.clearColorImage(*slot.image.handle(), vk::ImageLayout::eTransferDstOptimal, black, range);
        pipelineBarrier(cmd, colorBarrier(*slot.image.handle(), vk::ImageLayout::eTransferDstOptimal,
                                          vk::ImageLayout::eShaderReadOnlyOptimal, vk::PipelineStageFlagBits2::eClear,
                                          vk::AccessFlagBits2::eTransferWrite, vk::PipelineStageFlagBits2::eFragmentShader,
                                          vk::AccessFlagBits2::eShaderSampledRead));
    });
    slot.alive = true;
}

std::int32_t VulkanRenderer::createRenderTexture(std::uint32_t width, std::uint32_t height) {
    if (!initialized_ || width == 0 || height == 0) return -1;
    std::int32_t id = -1;
    for (std::size_t i = 0; i < render_textures_.size(); ++i) {
        if (!render_textures_[i]->alive) {
            id = static_cast<std::int32_t>(i);
            break;
        }
    }
    if (id < 0) {
        render_textures_.push_back(std::make_unique<RenderTextureSlot>());
        id = static_cast<std::int32_t>(render_textures_.size() - 1);
    }
    createRenderTextureImage(*render_textures_[id], width, height);
    ++render_texture_generation_;
    rebindRenderTexture(id);  // modelos que ya la esperaban
    return id;
}

void VulkanRenderer::resizeRenderTexture(std::int32_t id, std::uint32_t width, std::uint32_t height) {
    if (!renderTextureValid(id) || width == 0 || height == 0) return;
    const vk::Extent2D current = render_textures_[id]->image.extent();
    if (current.width == std::clamp(width, 1u, 8192u) && current.height == std::clamp(height, 1u, 8192u)) return;
    // La imagen vieja la puede estar leyendo la interfaz de este frame (vista
    // previa, miniatura del material): se retira, no se destruye.
    device_.waitIdle();
    retireRenderTextureImage(*render_textures_[id]);
    createRenderTextureImage(*render_textures_[id], width, height);
    ++render_texture_generation_;
    rebindRenderTexture(id);
}

void VulkanRenderer::destroyRenderTexture(std::int32_t id) {
    if (!renderTextureValid(id)) return;
    device_.waitIdle();
    render_textures_[id]->alive = false;
    rebindRenderTexture(id);  // sus materiales vuelven a la textura por defecto
    retireRenderTextureImage(*render_textures_[id]);
    ++render_texture_generation_;
}

vk::Extent2D VulkanRenderer::renderTextureExtent(std::int32_t id) const {
    return renderTextureValid(id) ? render_textures_[id]->image.extent() : vk::Extent2D{0, 0};
}

VkImageView VulkanRenderer::renderTextureView(std::int32_t id) const {
    return renderTextureValid(id) ? static_cast<VkImageView>(*render_textures_[id]->ui_view) : VK_NULL_HANDLE;
}

void VulkanRenderer::bindRenderTextures(std::uint32_t model) {
    if (model >= skinned_models_.size()) return;
    SkinnedModel& m = skinned_models_[model];
    for (const SkinnedModel::RenderTextureRef& ref : m.renderTextureRefs()) {
        const vk::ImageView view = renderTextureValid(ref.texture) ? *render_textures_[ref.texture]->image.view()
                                                                   : vk::ImageView{};
        m.setMaterialImage(device_, skinned_pass_, ref.material, ref.binding, view);
    }
}

void VulkanRenderer::rebindRenderTexture(std::int32_t id) {
    for (std::uint32_t i = 0; i < skinned_models_.size(); ++i) {
        for (const SkinnedModel::RenderTextureRef& ref : skinned_models_[i].renderTextureRefs()) {
            if (ref.texture == id) {
                bindRenderTextures(i);
                break;
            }
        }
    }
}

void VulkanRenderer::renderToTexture(const scene::Scene& scene, const scene::Camera& camera, std::int32_t id) {
    if (!initialized_ || !renderTextureValid(id) || !swapchain_.isValid()) return;
    const vk::Extent2D extent = render_textures_[id]->image.extent();
    // La camara con la proporcion de la textura: la imagen de la escena (la de
    // la vista) se estira al copiarla y la geometria queda bien.
    scene::Camera view = camera;
    view.setAspectRatio(static_cast<float>(extent.width) / static_cast<float>(std::max(extent.height, 1u)));
    const bool helpers = editor_helpers_;
    editor_helpers_ = false;  // sin contorno, gizmos ni picking
    camera_override_ = &view;
    render_texture_target_ = id;
    drawFrame(scene, false);
    render_texture_target_ = -1;
    camera_override_ = nullptr;
    editor_helpers_ = helpers;

    ++render_texture_draw_count_;
    const auto now = std::chrono::steady_clock::now();
    if (now - render_texture_second_ >= std::chrono::seconds(1)) {
        render_texture_draws_ = render_texture_draw_count_;
        render_texture_draw_count_ = 0;
        render_texture_second_ = now;
    }
}

// -----------------------------------------------------------------------------
// Realidad virtual: cada ojo es un frame sin presentar que se copia a la
// swapchain de OpenXR.
// -----------------------------------------------------------------------------

void VulkanRenderer::createXrStaging() {
    const VkExtent2D e = xr_.eyeExtent();
    // Mismo orden de canales que la swapchain, en UNORM: el blit escala sin
    // tocar la gamma y la copia a la imagen sRGB lleva los bytes tal cual.
    const vk::Format format =
        xr_.swapchainFormat() == VK_FORMAT_B8G8R8A8_SRGB ? vk::Format::eB8G8R8A8Unorm : vk::Format::eR8G8B8A8Unorm;
    for (VulkanImage& image : xr_staging_) {
        image.create(device_, vk::Extent2D{e.width, e.height}, format,
                     vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst, vk::ImageAspectFlagBits::eColor);
    }
}

bool VulkanRenderer::connectXrSession(std::string* error, bool* needs_restart) {
    if (needs_restart != nullptr) *needs_restart = false;
    const auto fail = [&](const std::string& text, bool restart) {
        if (error != nullptr) *error = text;
        if (needs_restart != nullptr) *needs_restart = restart;
        std::cerr << "[VR] " << text << "\n";
        xr_.shutdown();
        return false;
    };
    if (xr_.available()) return true;
    if (!initialized_) return fail("El render aun no esta listo", false);
    const VkInstance instance = static_cast<VkInstance>(*instance_.handle());
    const VkPhysicalDevice gpu = static_cast<VkPhysicalDevice>(*device_.physicalDevice());
    const VkPhysicalDevice xr_gpu = xr_.physicalDevice(instance);
    if (xr_gpu == VK_NULL_HANDLE) {
        return fail(xr_.error().empty() ? std::string("El runtime de VR no dice en que GPU esta el casco") : xr_.error(), false);
    }
    if (xr_gpu != gpu) {
        return fail("El casco esta en otra GPU (no en " + device_.name() + "): hay que abrir de nuevo con el casco conectado.",
                    true);
    }
    // Lo que pide el runtime: activado, o que este PC no tiene (creando
    // Vulkan con el casco tampoco se activaria; se avisa y se sigue igual).
    std::string missing;
    const auto check = [&](const std::vector<std::string>& wanted, const std::vector<std::string>& enabled,
                           const auto& available) {
        for (const std::string& name : wanted) {
            if (std::find(enabled.begin(), enabled.end(), name) != enabled.end() || !available(name)) continue;
            missing += (missing.empty() ? "" : ", ") + name;
        }
    };
    check(xr_.requiredInstanceExtensions(), instance_.enabledExtensions(),
          [&](const std::string& name) { return instance_.extensionAvailable(name); });
    check(xr_.requiredDeviceExtensions(), device_.enabledExtensions(),
          [&](const std::string& name) { return device_.extensionAvailable(name); });
    if (!missing.empty()) {
        return fail("El casco pide extensiones de Vulkan que no se activaron al abrir (" + missing +
                        "): hay que abrir de nuevo con el casco conectado.",
                    true);
    }
    device_.waitIdle();
    if (!xr_.createSession(instance, gpu, static_cast<VkDevice>(*device_.handle()), device_.queueFamilies().graphics, 0)) {
        return fail("No se pudo empezar la sesion de VR: " + xr_.error(), false);
    }
    createXrStaging();
    std::cout << "[VR] Casco conectado: " << xr_.systemName() << " (" << xr_.runtimeName() << ")" << std::endl;
    return true;
}

void VulkanRenderer::stopXr() {
    const bool had_session = xr_.available();
    if (had_session && initialized_) device_.waitIdle();  // nada en vuelo con las imagenes del casco
    xr_.shutdown();
    xr_views_active_ = false;
    last_xr_view_time_ = {};
    if (eye_state_swapped_) swapEyeState();
    if (had_session) std::cout << "[VR] Casco soltado" << std::endl;
}

void VulkanRenderer::renderXrEye(const scene::Scene& scene, const scene::Camera& camera, int eye) {
    if (!initialized_ || eye < 0 || eye > 1) return;
    const VkImage image = xr_.acquireEye(eye);
    if (image == VK_NULL_HANDLE) return;
    const bool helpers = editor_helpers_;
    editor_helpers_ = false;
    camera_override_ = &camera;
    xr_eye_target_ = eye;
    xr_target_image_ = image;
    xr_copied_ = false;
    drawFrame(scene, false);
    xr_eye_target_ = -1;
    xr_target_image_ = VK_NULL_HANDLE;
    camera_override_ = nullptr;
    editor_helpers_ = helpers;
    if (!xr_copied_) clearXrImage(image);  // no se pudo dibujar (ventana minimizada...)
    xr_.releaseEye(eye);
}

// El estado temporal del ojo derecho entra (o sale) de los miembros: las
// pasadas usan taa_history_ & co. sin saber de que ojo son, y los descriptor
// sets del ojo (eyeSet()) apuntan a sus imagenes.
void VulkanRenderer::swapEyeState() {
    std::swap(taa_history_, xr_eye1_.taa_history);
    std::swap(ssr_history_, xr_eye1_.ssr_history);
    std::swap(gi_history_, xr_eye1_.gi_history);
    std::swap(gi_moments_history_, xr_eye1_.gi_moments_history);
    std::swap(rt_shadow_history_, xr_eye1_.rt_shadow_history);
    std::swap(rt_reflection_history_, xr_eye1_.rt_reflection_history);
    std::swap(rt_reflection_moments_history_, xr_eye1_.rt_reflection_moments_history);
    std::swap(taa_history_valid_, xr_eye1_.taa_valid);
    std::swap(ssr_filter_history_valid_, xr_eye1_.ssr_valid);
    std::swap(gi_filter_history_valid_, xr_eye1_.gi_valid);
    std::swap(rt_shadow_history_valid_, xr_eye1_.rt_shadow_valid);
    std::swap(rt_reflection_history_valid_, xr_eye1_.rt_reflection_valid);
    std::swap(motion_view_projection_, xr_eye1_.motion_view_projection);
    eye_state_swapped_ = !eye_state_swapped_;
}

bool VulkanRenderer::xrEyeStateReady() const {
    return xr_eye1_.taa_history.isValid() && xr_eye1_.ssr_history.isValid() && xr_eye1_.gi_history.isValid() &&
           xr_eye1_.gi_moments_history.isValid() && xr_eye1_.rt_shadow_history.isValid() &&
           xr_eye1_.rt_reflection_history.isValid() && xr_eye1_.rt_reflection_moments_history.isValid();
}

// VR: las historias del ojo derecho (mismos formatos y tamanos que las del
// izquierdo). Sin casco no se crean.
void VulkanRenderer::createXrEyeTargets(vk::Extent2D render, vk::Extent2D output) {
    if (eye_state_swapped_) swapEyeState();
    xr_eye1_.taa_valid = xr_eye1_.ssr_valid = xr_eye1_.gi_valid = false;
    xr_eye1_.rt_shadow_valid = xr_eye1_.rt_reflection_valid = false;
    if ((xr_output_extent_.width == 0 || xr_output_extent_.height == 0) && !stereo_emulation_) {
        xr_eye1_.taa_history.destroy();
        xr_eye1_.ssr_history.destroy();
        xr_eye1_.gi_history.destroy();
        xr_eye1_.gi_moments_history.destroy();
        xr_eye1_.rt_shadow_history.destroy();
        xr_eye1_.rt_reflection_history.destroy();
        xr_eye1_.rt_reflection_moments_history.destroy();
        return;
    }
    xr_eye1_.ssr_history.create(device_, render, kHdrFormat,
                                vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
                                vk::ImageAspectFlagBits::eColor);
    const vk::ImageUsageFlags filter_usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
                                             vk::ImageUsageFlagBits::eTransferSrc |
                                             vk::ImageUsageFlagBits::eTransferDst;
    const vk::Extent2D half = bloomLevelExtent(render, 0);
    xr_eye1_.gi_history.create(device_, half, kHdrFormat, filter_usage, vk::ImageAspectFlagBits::eColor);
    xr_eye1_.gi_moments_history.create(device_, half, kHdrFormat, filter_usage, vk::ImageAspectFlagBits::eColor);
    // Historias de los filtros del trazado de rayos (como las del izquierdo:
    // 1 x 1 sin trazado).
    const vk::Extent2D rt_extent = rt_shadow_history_.isValid() ? rt_shadow_history_.extent() : vk::Extent2D{1, 1};
    xr_eye1_.rt_shadow_history.create(device_, rt_extent, vk::Format::eR16G16B16A16Uint, filter_usage,
                                      vk::ImageAspectFlagBits::eColor);
    xr_eye1_.rt_reflection_history.create(device_, rt_extent, kHdrFormat, filter_usage, vk::ImageAspectFlagBits::eColor);
    xr_eye1_.rt_reflection_moments_history.create(device_, rt_extent, kHdrFormat, filter_usage,
                                                  vk::ImageAspectFlagBits::eColor);
    xr_eye1_.taa_history.create(device_, output, kHdrFormat,
                                vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
                                vk::ImageAspectFlagBits::eColor);
    // Como las del ojo izquierdo: las de la GI en General y a cero, la del TAA
    // como textura (la del SSR la prepara su filtro la primera vez).
    device_.submitOneTime([&](const vk::raii::CommandBuffer& cmd) {
        std::vector<vk::ImageMemoryBarrier2> barriers;
        for (const VulkanImage* image : {&xr_eye1_.gi_history, &xr_eye1_.gi_moments_history, &xr_eye1_.rt_shadow_history,
                                         &xr_eye1_.rt_reflection_history, &xr_eye1_.rt_reflection_moments_history}) {
            barriers.push_back(colorBarrier(*image->handle(), vk::ImageLayout::eUndefined, vk::ImageLayout::eGeneral,
                                            vk::PipelineStageFlagBits2::eTopOfPipe, vk::AccessFlagBits2::eNone,
                                            vk::PipelineStageFlagBits2::eTransfer,
                                            vk::AccessFlagBits2::eTransferWrite));
        }
        barriers.push_back(colorBarrier(*xr_eye1_.taa_history.handle(), vk::ImageLayout::eUndefined,
                                        vk::ImageLayout::eShaderReadOnlyOptimal, vk::PipelineStageFlagBits2::eTopOfPipe,
                                        vk::AccessFlagBits2::eNone, vk::PipelineStageFlagBits2::eFragmentShader,
                                        vk::AccessFlagBits2::eShaderSampledRead));
        pipelineBarrier(cmd, barriers);
        for (const VulkanImage* image : {&xr_eye1_.gi_history, &xr_eye1_.gi_moments_history,
                                         &xr_eye1_.rt_reflection_history, &xr_eye1_.rt_reflection_moments_history}) {
            cmd.clearColorImage(*image->handle(), vk::ImageLayout::eGeneral, vk::ClearColorValue{0.0f, 0.0f, 0.0f, 0.0f},
                                vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1});
        }
        cmd.clearColorImage(*xr_eye1_.rt_shadow_history.handle(), vk::ImageLayout::eGeneral,
                            vk::ClearColorValue{std::array<std::uint32_t, 4>{0u, 0u, 0u, 0u}},
                            vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1});
    });
}

void VulkanRenderer::renderXrStereo(const scene::Scene& scene, const std::array<scene::Camera, 2>& eyes,
                                    const scene::Camera& center) {
    if (!initialized_) return;
    if (!xrEyeStateReady()) {
        // Las historias del ojo derecho se crean con los destinos al tamano
        // del casco (applyPendingResize, al empezar el frame): hasta entonces,
        // negro (el casco espera cada ojo desde su sitio, no una imagen comun).
        clearXrEye(0);
        clearXrEye(1);
        return;
    }
    renderStereoViews(scene, eyes, center, /*headset=*/true);
}

void VulkanRenderer::setStereoEmulation(bool on) {
    if (on == stereo_emulation_) return;
    stereo_emulation_ = on;
    targets_dirty_ = true;  // las historias del ojo derecho (createXrEyeTargets)
    applyEffectiveGraphics();
}

void VulkanRenderer::renderStereoEmulated(const scene::Scene& scene, const std::array<scene::Camera, 2>& eyes,
                                          const scene::Camera& center) {
    if (!initialized_ || !stereo_emulation_ || !xrEyeStateReady()) return;
    renderStereoViews(scene, eyes, center, /*headset=*/false);
}

// Los dos ojos como vistas principales (con el casco, cada uno a su imagen de
// OpenXR; emulado, sin imagen: la ventana enseña el ultimo).
void VulkanRenderer::renderStereoViews(const scene::Scene& scene, const std::array<scene::Camera, 2>& eyes,
                                       const scene::Camera& center, bool headset) {
    const bool helpers = editor_helpers_;
    editor_helpers_ = false;
    shadow_camera_override_ = &center;
    xr_stereo_view_ = true;
    xr_eye_recorded_ = false;
    for (int eye = 0; eye < 2; ++eye) {
        VkImage image = VK_NULL_HANDLE;
        if (headset) {
            image = xr_.acquireEye(eye);
            if (image == VK_NULL_HANDLE) continue;
        }
        // El segundo ojo usa lo que dibujo el primero (sombras, cielo): si el
        // primero no se pudo dibujar, este tampoco.
        if (eye == 1 && !xr_eye_recorded_) {
            if (headset) {
                clearXrImage(image);
                xr_.releaseEye(eye);
            }
            continue;
        }
        camera_override_ = &eyes[static_cast<std::size_t>(eye)];
        xr_eye_target_ = eye;
        xr_target_image_ = image;
        xr_view_frame_ = true;
        xr_view_eye_ = eye;
        xr_copied_ = false;
        drawFrame(scene, false);
        xr_view_frame_ = false;
        xr_eye_target_ = -1;
        xr_target_image_ = VK_NULL_HANDLE;
        if (headset) {
            if (!xr_copied_) clearXrImage(image);  // no se pudo dibujar
            xr_.releaseEye(eye);
        }
    }
    xr_stereo_view_ = false;
    xr_view_eye_ = 0;
    shadow_camera_override_ = nullptr;
    camera_override_ = nullptr;
    editor_helpers_ = helpers;
    xr_views_active_ = xr_eye_recorded_;  // la ventana enseña el ultimo ojo
}

void VulkanRenderer::renderXrMono(const scene::Scene& scene, const scene::Camera& camera) {
    if (!initialized_) return;
    const VkImage left = xr_.acquireEye(0);
    const VkImage right = xr_.acquireEye(1);
    if (left == VK_NULL_HANDLE || right == VK_NULL_HANDLE) {
        if (left != VK_NULL_HANDLE) xr_.releaseEye(0);
        if (right != VK_NULL_HANDLE) xr_.releaseEye(1);
        return;
    }
    const bool helpers = editor_helpers_;
    editor_helpers_ = false;
    camera_override_ = &camera;
    xr_eye_target_ = 0;
    xr_target_image_ = left;
    xr_mono_second_image_ = right;
    xr_view_frame_ = true;
    xr_copied_ = false;
    drawFrame(scene, false);
    xr_view_frame_ = false;
    xr_views_active_ = true;
    xr_eye_target_ = -1;
    xr_target_image_ = VK_NULL_HANDLE;
    xr_mono_second_image_ = VK_NULL_HANDLE;
    camera_override_ = nullptr;
    editor_helpers_ = helpers;
    if (!xr_copied_) {
        clearXrImage(left);
        clearXrImage(right);
    }
    xr_.releaseEye(0);
    xr_.releaseEye(1);
}

void VulkanRenderer::clearXrEye(int eye) {
    if (!initialized_ || eye < 0 || eye > 1) return;
    const VkImage image = xr_.acquireEye(eye);
    if (image == VK_NULL_HANDLE) return;
    clearXrImage(image);
    xr_.releaseEye(eye);
}

// Negro (un ojo sin camara, o un frame que no se pudo dibujar).
void VulkanRenderer::clearXrImage(VkImage image) {
    device_.submitOneTime([&](const vk::raii::CommandBuffer& cmd) {
        const vk::Image target(image);
        pipelineBarrier(cmd, colorBarrier(target, vk::ImageLayout::eColorAttachmentOptimal, vk::ImageLayout::eTransferDstOptimal,
                                          vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                          vk::AccessFlagBits2::eColorAttachmentWrite, vk::PipelineStageFlagBits2::eClear,
                                          vk::AccessFlagBits2::eTransferWrite));
        const vk::ClearColorValue black(std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f});
        const vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
        cmd.clearColorImage(target, vk::ImageLayout::eTransferDstOptimal, black, range);
        pipelineBarrier(cmd, colorBarrier(target, vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eColorAttachmentOptimal,
                                          vk::PipelineStageFlagBits2::eClear, vk::AccessFlagBits2::eTransferWrite,
                                          vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                          vk::AccessFlagBits2::eColorAttachmentWrite));
    });
}

void VulkanRenderer::recordXrEyeCopy(const vk::raii::CommandBuffer& cmd) {
    VulkanImage& staging = xr_staging_[static_cast<std::size_t>(xr_eye_target_)];
    if (xr_target_image_ == VK_NULL_HANDLE || !*staging.handle()) return;
    const vk::Image target(xr_target_image_);
    constexpr vk::ImageLayout kRead = vk::ImageLayout::eShaderReadOnlyOptimal;
    using Stage = vk::PipelineStageFlagBits2;
    using Access = vk::AccessFlagBits2;
    // Resultado (8 bits con gamma) -> intermedia del tamano del ojo.
    pipelineBarrier(cmd, {colorBarrier(*ldr_color_.handle(), kRead, vk::ImageLayout::eTransferSrcOptimal,
                                       Stage::eColorAttachmentOutput | Stage::eFragmentShader,
                                       Access::eColorAttachmentWrite | Access::eShaderSampledRead, Stage::eBlit,
                                       Access::eTransferRead),
                          colorBarrier(*staging.handle(), vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
                                       Stage::eAllTransfer, Access::eTransferRead, Stage::eBlit, Access::eTransferWrite)});
    const vk::Extent2D from = ldr_color_.extent();
    const vk::Extent2D to = staging.extent();
    vk::ImageBlit region{};
    region.srcSubresource = vk::ImageSubresourceLayers{vk::ImageAspectFlagBits::eColor, 0, 0, 1};
    region.dstSubresource = region.srcSubresource;
    region.srcOffsets[1] = vk::Offset3D{static_cast<std::int32_t>(from.width), static_cast<std::int32_t>(from.height), 1};
    region.dstOffsets[1] = vk::Offset3D{static_cast<std::int32_t>(to.width), static_cast<std::int32_t>(to.height), 1};
    cmd.blitImage(*ldr_color_.handle(), vk::ImageLayout::eTransferSrcOptimal, *staging.handle(),
                  vk::ImageLayout::eTransferDstOptimal, region, vk::Filter::eLinear);
    // Intermedia -> imagen del casco (sRGB): los mismos bytes.
    pipelineBarrier(cmd, {colorBarrier(*ldr_color_.handle(), vk::ImageLayout::eTransferSrcOptimal, kRead, Stage::eBlit,
                                       Access::eTransferRead, Stage::eFragmentShader, Access::eShaderSampledRead),
                          colorBarrier(*staging.handle(), vk::ImageLayout::eTransferDstOptimal,
                                       vk::ImageLayout::eTransferSrcOptimal, Stage::eBlit, Access::eTransferWrite,
                                       Stage::eCopy, Access::eTransferRead),
                          colorBarrier(target, vk::ImageLayout::eColorAttachmentOptimal, vk::ImageLayout::eTransferDstOptimal,
                                       Stage::eColorAttachmentOutput, Access::eColorAttachmentWrite, Stage::eCopy,
                                       Access::eTransferWrite)});
    vk::ImageCopy copy{};
    copy.srcSubresource = vk::ImageSubresourceLayers{vk::ImageAspectFlagBits::eColor, 0, 0, 1};
    copy.dstSubresource = copy.srcSubresource;
    copy.extent = vk::Extent3D{to.width, to.height, 1};
    cmd.copyImage(*staging.handle(), vk::ImageLayout::eTransferSrcOptimal, target, vk::ImageLayout::eTransferDstOptimal, copy);
    // El runtime la espera en COLOR_ATTACHMENT_OPTIMAL.
    pipelineBarrier(cmd, colorBarrier(target, vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eColorAttachmentOptimal,
                                      Stage::eCopy, Access::eTransferWrite, Stage::eColorAttachmentOutput,
                                      Access::eColorAttachmentWrite));
    // Una camara: la misma imagen al otro ojo.
    if (xr_mono_second_image_ != VK_NULL_HANDLE) {
        const vk::Image second(xr_mono_second_image_);
        pipelineBarrier(cmd, colorBarrier(second, vk::ImageLayout::eColorAttachmentOptimal, vk::ImageLayout::eTransferDstOptimal,
                                          Stage::eColorAttachmentOutput, Access::eColorAttachmentWrite, Stage::eCopy,
                                          Access::eTransferWrite));
        cmd.copyImage(*staging.handle(), vk::ImageLayout::eTransferSrcOptimal, second, vk::ImageLayout::eTransferDstOptimal,
                      copy);
        pipelineBarrier(cmd, colorBarrier(second, vk::ImageLayout::eTransferDstOptimal,
                                          vk::ImageLayout::eColorAttachmentOptimal, Stage::eCopy, Access::eTransferWrite,
                                          Stage::eColorAttachmentOutput, Access::eColorAttachmentWrite));
    }
    xr_copied_ = true;
}

// El resultado (compuesto, con tonemapping) a la textura, escalado.
void VulkanRenderer::recordRenderTextureCopy(const vk::raii::CommandBuffer& cmd) {
    VulkanImage& target = render_textures_[render_texture_target_]->image;
    constexpr vk::ImageLayout kRead = vk::ImageLayout::eShaderReadOnlyOptimal;
    pipelineBarrier(cmd, {colorBarrier(*ldr_color_.handle(), kRead, vk::ImageLayout::eTransferSrcOptimal,
                                       vk::PipelineStageFlagBits2::eColorAttachmentOutput |
                                           vk::PipelineStageFlagBits2::eFragmentShader,
                                       vk::AccessFlagBits2::eColorAttachmentWrite | vk::AccessFlagBits2::eShaderSampledRead,
                                       vk::PipelineStageFlagBits2::eBlit, vk::AccessFlagBits2::eTransferRead),
                          colorBarrier(*target.handle(), kRead, vk::ImageLayout::eTransferDstOptimal,
                                       vk::PipelineStageFlagBits2::eFragmentShader,
                                       vk::AccessFlagBits2::eShaderSampledRead, vk::PipelineStageFlagBits2::eBlit,
                                       vk::AccessFlagBits2::eTransferWrite)});
    const vk::Extent2D from = ldr_color_.extent();
    const vk::Extent2D to = target.extent();
    vk::ImageBlit region{};
    region.srcSubresource = vk::ImageSubresourceLayers{vk::ImageAspectFlagBits::eColor, 0, 0, 1};
    region.dstSubresource = region.srcSubresource;
    region.srcOffsets[1] = vk::Offset3D{static_cast<std::int32_t>(from.width), static_cast<std::int32_t>(from.height), 1};
    region.dstOffsets[1] = vk::Offset3D{static_cast<std::int32_t>(to.width), static_cast<std::int32_t>(to.height), 1};
    cmd.blitImage(*ldr_color_.handle(), vk::ImageLayout::eTransferSrcOptimal, *target.handle(),
                  vk::ImageLayout::eTransferDstOptimal, region, vk::Filter::eLinear);
    pipelineBarrier(cmd, {colorBarrier(*ldr_color_.handle(), vk::ImageLayout::eTransferSrcOptimal, kRead,
                                       vk::PipelineStageFlagBits2::eBlit, vk::AccessFlagBits2::eTransferRead,
                                       vk::PipelineStageFlagBits2::eFragmentShader,
                                       vk::AccessFlagBits2::eShaderSampledRead),
                          colorBarrier(*target.handle(), vk::ImageLayout::eTransferDstOptimal, kRead,
                                       vk::PipelineStageFlagBits2::eBlit, vk::AccessFlagBits2::eTransferWrite,
                                       vk::PipelineStageFlagBits2::eFragmentShader,
                                       vk::AccessFlagBits2::eShaderSampledRead)});
}

void VulkanRenderer::requestPick(std::uint32_t x, std::uint32_t y) {
    // Llegan en pixeles de pantalla; el picking se dibuja a la resolucion interna.
    const vk::Extent2D output = ldr_color_.extent();
    if (output.width > 0 && output.height > 0 && render_extent_.width > 0) {
        x = static_cast<std::uint32_t>(static_cast<std::uint64_t>(x) * render_extent_.width / output.width);
        y = static_cast<std::uint32_t>(static_cast<std::uint64_t>(y) * render_extent_.height / output.height);
    }
    pick_requested_ = true;
    pick_request_ = PickResult{};
    pick_request_.x = x;
    pick_request_.y = y;
    pick_result_.reset();
}

void VulkanRenderer::updateModelMaterial(std::uint32_t model, std::uint32_t material,
                                         const asset::MaterialData& data) {
    if (model >= skinned_models_.size() || material >= skinned_models_[model].materials().size()) {
        return;
    }
    SkinnedModel::Material& gpu = skinned_models_[model].materials()[material];
    path_tracing_reset_ = true;  // el material cambia lo que se ve
    // Lo que cambia la silueta en los mapas de sombra (relieve teselado, su
    // altura, el alfa del recorte): las sombras guardadas ya no valen. Solo
    // si cambia de verdad (el Inspector puede mandarlo cada frame).
    const std::uint32_t shadow_flags_before = gpu.shader_flags;
    const float height_before = gpu.emissive.w;
    const float alpha_before = gpu.base_color.w;
    gpu.base_color = data.base_color;
    // El relieve del parallax solo si el material ya tiene mapa de alturas
    // (ponerlo o quitarlo rehace el modelo).
    const bool relief = (gpu.shader_flags & (GpuSkinnedPush::kFlagHeightMap | GpuSkinnedPush::kFlagTessellation)) != 0;
    gpu.emissive = core::Vec4{data.emissive.x, data.emissive.y, data.emissive.z,
                              relief ? std::max(data.height_scale, 0.0f) : 0.0f};
    // Parallax o teselado, auto-sombra y densidad: se cambian en vivo.
    gpu.shader_flags = SkinnedModel::reliefFlags(gpu.shader_flags, data, relief, skinned_pass_.tessellationEnabled());
    gpu.shading = SkinnedModel::shadingBits(data);
    gpu.params = core::Vec4{data.metallic, data.roughness, data.occlusion_strength,
                            data.normal_map_directx ? -data.normal_scale : data.normal_scale};
    gpu.reflectance = data.reflectance;
    gpu.surface_shader = data.surface_shader;
    gpu.surface_params = data.surface_params;
    if (gpu.shader_flags != shadow_flags_before || gpu.emissive.w != height_before || gpu.base_color.w != alpha_before) {
        staticGeometryChanged();
    }
}

std::int32_t VulkanRenderer::createSurfaceShader(const std::vector<std::uint32_t>& vertex_spirv,
                                                 const std::vector<std::uint32_t>& fragment_spirv,
                                                 std::string* error) {
    // Modo compatible (moviles, GPU antiguas): sin shaders de superficie del
    // usuario (su set de material no tiene sus texturas); el material se ve
    // con el shader estandar.
    if (device_.compatMode()) {
        if (error) *error = "los shaders de superficie propios no estan en el modo compatible";
        return -1;
    }
    try {
        surface_pipelines_.push_back(skinned_pass_.createSurfacePipeline(device_, vertex_spirv, fragment_spirv));
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return -1;
    }
    return static_cast<std::int32_t>(surface_pipelines_.size()) - 1;
}

bool VulkanRenderer::updateSurfaceShader(std::int32_t id, const std::vector<std::uint32_t>& vertex_spirv,
                                         const std::vector<std::uint32_t>& fragment_spirv, std::string* error) {
    if (id < 0 || static_cast<std::size_t>(id) >= surface_pipelines_.size()) {
        if (error) *error = "shader de superficie desconocido";
        return false;
    }
    try {
        vk::raii::Pipeline pipeline = skinned_pass_.createSurfacePipeline(device_, vertex_spirv, fragment_spirv);
        // La anterior puede estar en un frame en vuelo: se espera a la GPU (solo
        // pasa al guardar un .crshader).
        device_.waitIdle();
        surface_pipelines_[static_cast<std::size_t>(id)] = std::move(pipeline);
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
    return true;
}

std::uint32_t VulkanRenderer::pushSurfaceParams(std::uint32_t frame_index, const std::array<core::Vec4, 8>& params) {
    if (frame_index >= surface_param_buffers_.size()) return 0;
    std::uint32_t& used = surface_param_used_[frame_index];
    // Lleno (mas de 2048 materiales con shader propio en un frame): se
    // reutiliza el ultimo bloque.
    const std::uint32_t block = std::min(used, kMaxSurfaceParamBlocks - 1);
    if (used < kMaxSurfaceParamBlocks) ++used;
    surface_param_buffers_[frame_index].write(params.data(), sizeof(core::Vec4) * params.size(),
                                              sizeof(core::Vec4) * SkinnedPass::kSurfaceParamCount * block);
    return block;
}

void VulkanRenderer::bindMaterialPipeline(const vk::raii::CommandBuffer& cmd, std::uint32_t frame_index,
                                          const SkinnedModel::Material& material, std::int32_t& bound_shader,
                                          GpuSkinnedPush& push) {
    std::int32_t shader = material.surface_shader;
    if (shader >= 0 && (static_cast<std::size_t>(shader) >= surface_pipelines_.size() ||
                        !*surface_pipelines_[static_cast<std::size_t>(shader)])) {
        shader = -1;  // sin pipeline (fallo al compilar): el estandar
    }
    // En lineas (Wireframe y la pasada de lineas) todos con la estandar.
    if (wire_gbuffer_ || mesh_pipeline_override_ != nullptr) shader = -1;
    // Relieve teselado: solo con el shader estandar y dibujando relleno.
    constexpr std::int32_t kTessellated = -2;
    const bool tessellated = shader < 0 && !wire_gbuffer_ && mesh_pipeline_override_ == nullptr &&
                             (material.shader_flags & GpuSkinnedPush::kFlagTessellation) != 0 &&
                             skinned_pass_.tessellationEnabled();
    const std::int32_t key = tessellated ? kTessellated : shader;
    if (key != bound_shader) {
        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics,
                         tessellated   ? *skinned_pass_.geometryTessPipeline()
                         : shader >= 0 ? *surface_pipelines_[static_cast<std::size_t>(shader)]
                                       : *meshGeometryPipeline());
        bound_shader = key;
    }
    // En la geometria pick_id no se usa: dice el bloque de propiedades.
    if (shader >= 0) push.pick_id = pushSurfaceParams(frame_index, material.surface_params);
}

std::optional<VulkanRenderer::PickResult> VulkanRenderer::takePickResult() {
    std::optional<PickResult> result = pick_result_;
    pick_result_.reset();
    return result;
}

bool VulkanRenderer::pickPending() const {
    if (pick_requested_) return true;
    for (bool in_flight : pick_in_flight_) {
        if (in_flight) return true;
    }
    return false;
}

void VulkanRenderer::recordPickPass(const vk::raii::CommandBuffer& cmd, std::uint32_t frame_index) {
    pick_requested_ = false;
    const vk::Extent2D extent = pick_ids_.extent();
    if (pick_request_.x >= extent.width || pick_request_.y >= extent.height || pick_buffers_.empty()) {
        pick_result_ = pick_request_;  // fuera de la imagen: nada
        return;
    }
    const vk::Offset2D pixel{static_cast<std::int32_t>(pick_request_.x), static_cast<std::int32_t>(pick_request_.y)};
    const vk::Rect2D area{pixel, vk::Extent2D{1, 1}};

    // Rayo del pixel (en el mundo) para no mandar a la GPU lo que no puede
    // estar debajo: solo los actores y submallas cuya caja toca el rayo.
    const core::Mat4 inverse = core::inverse(camera_view_projection_);
    const float nx = (static_cast<float>(pick_request_.x) + 0.5f) / static_cast<float>(extent.width) * 2.0f - 1.0f;
    const float ny = (static_cast<float>(pick_request_.y) + 0.5f) / static_cast<float>(extent.height) * 2.0f - 1.0f;
    const core::Vec4 near_h = inverse * core::Vec4{nx, ny, 0.0f, 1.0f};
    const core::Vec4 far_h = inverse * core::Vec4{nx, ny, 1.0f, 1.0f};
    const core::Vec3 origin{near_h.x / near_h.w, near_h.y / near_h.w, near_h.z / near_h.w};
    const core::Vec3 far_point{far_h.x / far_h.w, far_h.y / far_h.w, far_h.z / far_h.w};
    const core::Vec3 direction = core::normalize(far_point - origin);
    const auto hits_sphere = [&](const core::Vec3& center, float radius) {
        const core::Vec3 to_center = center - origin;
        const float along = core::dot(to_center, direction);
        const core::Vec3 closest = origin + direction * std::max(along, 0.0f);
        const core::Vec3 d = center - closest;
        return core::dot(d, d) <= radius * radius;
    };
    const auto hits_box = [&](const core::Aabb& box) {
        float t_min = 0.0f;
        float t_max = 1e30f;
        const float o[3] = {origin.x, origin.y, origin.z};
        const float dir[3] = {direction.x, direction.y, direction.z};
        const float lo[3] = {box.min.x, box.min.y, box.min.z};
        const float hi[3] = {box.max.x, box.max.y, box.max.z};
        for (int i = 0; i < 3; ++i) {
            if (std::abs(dir[i]) < 1e-9f) {
                if (o[i] < lo[i] || o[i] > hi[i]) return false;
                continue;
            }
            float t0 = (lo[i] - o[i]) / dir[i];
            float t1 = (hi[i] - o[i]) / dir[i];
            if (t0 > t1) std::swap(t0, t1);
            t_min = std::max(t_min, t0);
            t_max = std::min(t_max, t1);
            if (t_min > t_max) return false;
        }
        return true;
    };

    // Imagen de IDs como destino (solo se limpia y dibuja el pixel); el
    // depth del G-buffer en solo lectura, como el contorno.
    vk::ImageMemoryBarrier2 depth_barrier{};
    depth_barrier.srcStageMask = vk::PipelineStageFlagBits2::eLateFragmentTests |
                                 vk::PipelineStageFlagBits2::eFragmentShader |
                                 vk::PipelineStageFlagBits2::eComputeShader;
    depth_barrier.srcAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
    depth_barrier.dstStageMask = vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                                 vk::PipelineStageFlagBits2::eLateFragmentTests;
    depth_barrier.dstAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentRead;
    depth_barrier.oldLayout = compat::depthReadOnlyLayout();
    depth_barrier.newLayout = compat::depthReadOnlyLayout();
    depth_barrier.image = *gbuffer_.depth().handle();
    depth_barrier.subresourceRange = vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1};
    pipelineBarrier(cmd, {colorBarrier(*pick_ids_.handle(), vk::ImageLayout::eUndefined,
                                       vk::ImageLayout::eColorAttachmentOptimal,
                                       vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead,
                                       vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                       vk::AccessFlagBits2::eColorAttachmentWrite),
                          depth_barrier});

    vk::RenderingAttachmentInfo id_attachment{};
    id_attachment.imageView = *pick_ids_.view();
    id_attachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
    id_attachment.loadOp = vk::AttachmentLoadOp::eClear;
    id_attachment.storeOp = vk::AttachmentStoreOp::eStore;
    id_attachment.clearValue = vk::ClearValue{vk::ClearColorValue{std::array<std::uint32_t, 4>{0, 0, 0, 0}}};

    vk::RenderingAttachmentInfo depth_attachment{};
    depth_attachment.imageView = *gbuffer_.depth().view();
    depth_attachment.imageLayout = compat::depthReadOnlyLayout();
    depth_attachment.loadOp = vk::AttachmentLoadOp::eLoad;
    depth_attachment.storeOp = vk::AttachmentStoreOp::eNone;

    vk::RenderingInfo rendering_info{};
    rendering_info.renderArea = area;
    rendering_info.layerCount = 1;
    rendering_info.setColorAttachments(id_attachment);
    rendering_info.pDepthAttachment = &depth_attachment;

    compat::beginRendering(cmd, rendering_info);
    cmd.setViewport(0, vk::Viewport{0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height),
                                    0.0f, 1.0f});
    cmd.setScissor(0, area);
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *skinned_pass_.pickPipeline());
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *skinned_pass_.geometryLayout(), 0,
                           *skin_sets_[frame_index], nullptr);
    for (const ActorDraw& draw : actor_draws_) {
        if (draw.shadows_only || !hits_sphere(draw.bounds_center, draw.bounds_radius * 1.01f + 1e-3f)) continue;
        const SkinnedModel& model = skinned_models_[draw.model];
        cmd.bindVertexBuffers(0, *model.vertices().handle(), {0});
        cmd.bindIndexBuffer(*model.indices().handle(), 0, vk::IndexType::eUint32);
        GpuSkinnedPush push{};
        push.model = draw.transform;
        push.bone_offset = draw.bone_offset;
        push.pick_id = draw.scene_actor + 1;
        cmd.pushConstants<GpuSkinnedPush>(*skinned_pass_.geometryLayout(), skinned_pass_.geometryPushStages(), 0,
                                          push);
        const auto& submeshes = model.submeshes();
        std::uint32_t pushed_material = UINT32_MAX;
        for (std::uint32_t i = 0; i < submeshes.size(); ++i) {
            if (draw.per_submesh && !hits_box(submesh_bounds_[draw.first_bounds + i])) continue;
            if (submeshes[i].material != pushed_material) {
                // Tambien el hueco de material: soltar un material en la escena
                // lo pone en la parte que hay bajo el raton.
                pushed_material = submeshes[i].material;
                const std::uint32_t id = (draw.scene_actor + 1) | (std::min(pushed_material, 4095u) << 20);
                cmd.pushConstants<std::uint32_t>(*skinned_pass_.geometryLayout(), skinned_pass_.geometryPushStages(),
                                                 offsetof(GpuSkinnedPush, pick_id), id);
            }
            cmd.drawIndexed(submeshes[i].index_count, 1, submeshes[i].first_index, 0, 0);
        }
    }
    compat::endRendering(cmd);

    // El pixel al buffer que lee la CPU cuando la GPU termine este frame.
    pipelineBarrier(cmd, colorBarrier(*pick_ids_.handle(), vk::ImageLayout::eColorAttachmentOptimal,
                                      vk::ImageLayout::eTransferSrcOptimal,
                                      vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                      vk::AccessFlagBits2::eColorAttachmentWrite, vk::PipelineStageFlagBits2::eTransfer,
                                      vk::AccessFlagBits2::eTransferRead));
    vk::BufferImageCopy region{};
    region.imageSubresource = vk::ImageSubresourceLayers{vk::ImageAspectFlagBits::eColor, 0, 0, 1};
    region.imageOffset = vk::Offset3D{pixel.x, pixel.y, 0};
    region.imageExtent = vk::Extent3D{1, 1, 1};
    cmd.copyImageToBuffer(*pick_ids_.handle(), vk::ImageLayout::eTransferSrcOptimal,
                          *pick_buffers_[frame_index].handle(), region);
    vk::MemoryBarrier2 to_host{};
    to_host.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
    to_host.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
    to_host.dstStageMask = vk::PipelineStageFlagBits2::eHost;
    to_host.dstAccessMask = vk::AccessFlagBits2::eHostRead;
    vk::DependencyInfo dependency{};
    dependency.setMemoryBarriers(to_host);
    compat::pipelineBarrier(cmd, dependency);

    pick_requests_[frame_index] = pick_request_;
    pick_in_flight_[frame_index] = true;
}

void VulkanRenderer::recordParticlePass(const vk::raii::CommandBuffer& cmd,
                                        std::uint32_t frame_index) {
    const vk::Extent2D extent = scene_color_.extent();
    if (!particle_pass_.prepare(device_, frame_index, particles_, camera_view_,
                                camera_view_projection_)) {
        return;
    }

    // La imagen HDR sigue como destino de color (iluminacion y vidrio): solo
    // hay que ordenar las escrituras. El depth, en solo lectura.
    vk::ImageMemoryBarrier2 color_barrier = colorBarrier(
        *scene_color_.handle(), vk::ImageLayout::eColorAttachmentOptimal,
        vk::ImageLayout::eColorAttachmentOptimal, vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        vk::AccessFlagBits2::eColorAttachmentWrite, vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite);
    vk::ImageMemoryBarrier2 depth_barrier{};
    depth_barrier.srcStageMask = vk::PipelineStageFlagBits2::eLateFragmentTests |
                                 vk::PipelineStageFlagBits2::eFragmentShader;
    depth_barrier.srcAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
    depth_barrier.dstStageMask = vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                                 vk::PipelineStageFlagBits2::eLateFragmentTests;
    depth_barrier.dstAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentRead;
    depth_barrier.oldLayout = compat::depthReadOnlyLayout();
    depth_barrier.newLayout = compat::depthReadOnlyLayout();
    depth_barrier.image = *gbuffer_.depth().handle();
    depth_barrier.subresourceRange =
        vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1};
    pipelineBarrier(cmd, {color_barrier, depth_barrier});

    vk::RenderingAttachmentInfo color_attachment{};
    color_attachment.imageView = *scene_color_.view();
    color_attachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
    color_attachment.loadOp = vk::AttachmentLoadOp::eLoad;
    color_attachment.storeOp = vk::AttachmentStoreOp::eStore;

    vk::RenderingAttachmentInfo depth_attachment{};
    depth_attachment.imageView = *gbuffer_.depth().view();
    depth_attachment.imageLayout = compat::depthReadOnlyLayout();
    depth_attachment.loadOp = vk::AttachmentLoadOp::eLoad;
    depth_attachment.storeOp = vk::AttachmentStoreOp::eNone;

    vk::RenderingInfo rendering_info{};
    rendering_info.renderArea = vk::Rect2D{vk::Offset2D{0, 0}, extent};
    rendering_info.layerCount = 1;
    rendering_info.setColorAttachments(color_attachment);
    rendering_info.pDepthAttachment = &depth_attachment;

    compat::beginRendering(cmd, rendering_info);
    particle_pass_.record(cmd, frame_index, extent);
    compat::endRendering(cmd);
}

// Sistema de ambiente: lluvia, nieve, polvo, salpicaduras y el trazo de los
// rayos sobre la imagen HDR (como las particulas: depth en solo lectura, que
// ademas leen las salpicaduras para ponerse sobre el suelo).
void VulkanRenderer::recordPrecipitationPass(const vk::raii::CommandBuffer& cmd, std::uint32_t frame_index) {
    const vk::Extent2D extent = scene_color_.extent();
    PrecipitationFrame view;
    view.view_projection = camera_view_projection_;
    view.inverse_view_projection = core::inverse(camera_view_projection_);
    view.rain_view_projection = rain_view_projection_;
    view.camera_position = camera_position_;
    view.camera_right = core::normalize(Vec3{camera_view_.m[0][0], camera_view_.m[1][0], camera_view_.m[2][0]});
    view.tan_half_fov = std::abs(camera_projection_.m[1][1]) > 1e-6f ? 1.0f / std::abs(camera_projection_.m[1][1]) : 0.7f;
    view.extent = extent;
    view.rain_map_ready = rain_map_ready_;
    view.light = precipitation_light_;
    view.delta_seconds = isolated() ? 0.0f : frame_delta_seconds_;
    if (!precipitation_pass_.prepare(device_, frame_index, precipitation_, view, rain_map_, *rain_sampler_,
                                     gbuffer_.depth())) {
        return;
    }

    vk::ImageMemoryBarrier2 color_barrier = colorBarrier(
        *scene_color_.handle(), vk::ImageLayout::eColorAttachmentOptimal, vk::ImageLayout::eColorAttachmentOptimal,
        vk::PipelineStageFlagBits2::eColorAttachmentOutput, vk::AccessFlagBits2::eColorAttachmentWrite,
        vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite);
    vk::ImageMemoryBarrier2 depth_barrier{};
    depth_barrier.srcStageMask = vk::PipelineStageFlagBits2::eLateFragmentTests | vk::PipelineStageFlagBits2::eFragmentShader;
    depth_barrier.srcAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
    depth_barrier.dstStageMask = vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                                 vk::PipelineStageFlagBits2::eLateFragmentTests | vk::PipelineStageFlagBits2::eVertexShader;
    depth_barrier.dstAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentRead | vk::AccessFlagBits2::eShaderSampledRead;
    depth_barrier.oldLayout = compat::depthReadOnlyLayout();
    depth_barrier.newLayout = compat::depthReadOnlyLayout();
    depth_barrier.image = *gbuffer_.depth().handle();
    depth_barrier.subresourceRange = vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1};
    pipelineBarrier(cmd, {color_barrier, depth_barrier});

    vk::RenderingAttachmentInfo color_attachment{};
    color_attachment.imageView = *scene_color_.view();
    color_attachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
    color_attachment.loadOp = vk::AttachmentLoadOp::eLoad;
    color_attachment.storeOp = vk::AttachmentStoreOp::eStore;
    vk::RenderingAttachmentInfo depth_attachment{};
    depth_attachment.imageView = *gbuffer_.depth().view();
    depth_attachment.imageLayout = compat::depthReadOnlyLayout();
    depth_attachment.loadOp = vk::AttachmentLoadOp::eLoad;
    depth_attachment.storeOp = vk::AttachmentStoreOp::eNone;
    vk::RenderingInfo rendering_info{};
    rendering_info.renderArea = vk::Rect2D{vk::Offset2D{0, 0}, extent};
    rendering_info.layerCount = 1;
    rendering_info.setColorAttachments(color_attachment);
    rendering_info.pDepthAttachment = &depth_attachment;
    compat::beginRendering(cmd, rendering_info);
    precipitation_pass_.record(cmd, frame_index, extent);
    compat::endRendering(cmd);
}

// Fuego y humo volumetricos (FirePass): sobre la imagen HDR, leyendo el depth
// de la escena como textura (sin adjuntarlo).
void VulkanRenderer::recordFirePass(const vk::raii::CommandBuffer& cmd, std::uint32_t frame_index) {
    const vk::Extent2D extent = scene_color_.extent();
    vk::ImageMemoryBarrier2 color_barrier = colorBarrier(
        *scene_color_.handle(), vk::ImageLayout::eColorAttachmentOptimal, vk::ImageLayout::eColorAttachmentOptimal,
        vk::PipelineStageFlagBits2::eColorAttachmentOutput, vk::AccessFlagBits2::eColorAttachmentWrite,
        vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite);
    vk::ImageMemoryBarrier2 depth_barrier{};
    depth_barrier.srcStageMask = vk::PipelineStageFlagBits2::eLateFragmentTests | vk::PipelineStageFlagBits2::eFragmentShader;
    depth_barrier.srcAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
    depth_barrier.dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader;
    depth_barrier.dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead;
    depth_barrier.oldLayout = compat::depthReadOnlyLayout();
    depth_barrier.newLayout = compat::depthReadOnlyLayout();
    depth_barrier.image = *gbuffer_.depth().handle();
    depth_barrier.subresourceRange = vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1};
    pipelineBarrier(cmd, {color_barrier, depth_barrier});

    vk::RenderingAttachmentInfo color_attachment{};
    color_attachment.imageView = *scene_color_.view();
    color_attachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
    color_attachment.loadOp = vk::AttachmentLoadOp::eLoad;
    color_attachment.storeOp = vk::AttachmentStoreOp::eStore;
    vk::RenderingInfo rendering_info{};
    rendering_info.renderArea = vk::Rect2D{vk::Offset2D{0, 0}, extent};
    rendering_info.layerCount = 1;
    rendering_info.setColorAttachments(color_attachment);
    compat::beginRendering(cmd, rendering_info);
    fire_pass_.record(cmd, frame_index, extent, *camera_buffers_[frame_index].handle(), sizeof(GpuCamera),
                      *gbuffer_.depth().view(), fire_lighting_, weather_time_, static_cast<std::uint32_t>(frame_count_));
    compat::endRendering(cmd);
}

// Lit + Wireframe: las lineas de las mallas encima de la imagen iluminada,
// con el depth de la escena (solo lectura): lo tapado no se ve.
void VulkanRenderer::recordWireOverlayPass(const vk::raii::CommandBuffer& cmd, std::uint32_t frame_index) {
    const vk::Extent2D extent = scene_color_.extent();
    vk::ImageMemoryBarrier2 color_barrier = colorBarrier(
        *scene_color_.handle(), vk::ImageLayout::eColorAttachmentOptimal,
        vk::ImageLayout::eColorAttachmentOptimal, vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        vk::AccessFlagBits2::eColorAttachmentWrite, vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite);
    vk::ImageMemoryBarrier2 depth_barrier{};
    depth_barrier.srcStageMask = vk::PipelineStageFlagBits2::eLateFragmentTests |
                                 vk::PipelineStageFlagBits2::eFragmentShader;
    depth_barrier.srcAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
    depth_barrier.dstStageMask = vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                                 vk::PipelineStageFlagBits2::eLateFragmentTests;
    depth_barrier.dstAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentRead;
    depth_barrier.oldLayout = compat::depthReadOnlyLayout();
    depth_barrier.newLayout = compat::depthReadOnlyLayout();
    depth_barrier.image = *gbuffer_.depth().handle();
    depth_barrier.subresourceRange = vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1};
    pipelineBarrier(cmd, {color_barrier, depth_barrier});

    vk::RenderingAttachmentInfo color_attachment{};
    color_attachment.imageView = *scene_color_.view();
    color_attachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
    color_attachment.loadOp = vk::AttachmentLoadOp::eLoad;
    color_attachment.storeOp = vk::AttachmentStoreOp::eStore;
    vk::RenderingAttachmentInfo depth_attachment{};
    depth_attachment.imageView = *gbuffer_.depth().view();
    depth_attachment.imageLayout = compat::depthReadOnlyLayout();
    depth_attachment.loadOp = vk::AttachmentLoadOp::eLoad;
    depth_attachment.storeOp = vk::AttachmentStoreOp::eNone;
    vk::RenderingInfo rendering_info{};
    rendering_info.renderArea = vk::Rect2D{vk::Offset2D{0, 0}, extent};
    rendering_info.layerCount = 1;
    rendering_info.setColorAttachments(color_attachment);
    rendering_info.pDepthAttachment = &depth_attachment;

    compat::beginRendering(cmd, rendering_info);
    cmd.setViewport(0, vk::Viewport{0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height),
                                    0.0f, 1.0f});
    cmd.setScissor(0, vk::Rect2D{vk::Offset2D{0, 0}, extent});
    mesh_pipeline_override_ = &skinned_pass_.wireOverlayPipeline();
    drawCpuActors(cmd, frame_index);
    drawGpuClusters(cmd, frame_index, 0);
    drawGpuClusters(cmd, frame_index, 1);
    mesh_pipeline_override_ = nullptr;
    compat::endRendering(cmd);
}

// Con escalado: la profundidad de la escena copiada (sin filtrar) a la
// resolucion de pantalla, para el contorno y los gizmos. Una vez por frame.
void VulkanRenderer::recordOutputDepth(const vk::raii::CommandBuffer& cmd) {
    if (output_depth_ready_) return;
    output_depth_ready_ = true;
    const vk::Extent2D extent = output_depth_.extent();
    const vk::ImageSubresourceRange depth_range{vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1};
    vk::ImageMemoryBarrier2 source{};
    source.srcStageMask = vk::PipelineStageFlagBits2::eLateFragmentTests | vk::PipelineStageFlagBits2::eFragmentShader |
                          vk::PipelineStageFlagBits2::eComputeShader;
    source.srcAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
    source.dstStageMask = vk::PipelineStageFlagBits2::eTransfer;
    source.dstAccessMask = vk::AccessFlagBits2::eTransferRead;
    source.oldLayout = compat::depthReadOnlyLayout();
    source.newLayout = vk::ImageLayout::eTransferSrcOptimal;
    source.image = *gbuffer_.depth().handle();
    source.subresourceRange = depth_range;
    vk::ImageMemoryBarrier2 target{};
    target.srcStageMask = vk::PipelineStageFlagBits2::eEarlyFragmentTests | vk::PipelineStageFlagBits2::eLateFragmentTests;
    target.srcAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentRead;
    target.dstStageMask = vk::PipelineStageFlagBits2::eTransfer;
    target.dstAccessMask = vk::AccessFlagBits2::eTransferWrite;
    target.oldLayout = vk::ImageLayout::eUndefined;
    target.newLayout = vk::ImageLayout::eTransferDstOptimal;
    target.image = *output_depth_.handle();
    target.subresourceRange = depth_range;
    pipelineBarrier(cmd, {source, target});
    if (output_depth_blit_) {
        vk::ImageBlit blit{};
        blit.srcSubresource = vk::ImageSubresourceLayers{vk::ImageAspectFlagBits::eDepth, 0, 0, 1};
        blit.dstSubresource = blit.srcSubresource;
        blit.srcOffsets[1] = vk::Offset3D{static_cast<std::int32_t>(render_extent_.width),
                                          static_cast<std::int32_t>(render_extent_.height), 1};
        blit.dstOffsets[1] = vk::Offset3D{static_cast<std::int32_t>(extent.width),
                                          static_cast<std::int32_t>(extent.height), 1};
        cmd.blitImage(*gbuffer_.depth().handle(), vk::ImageLayout::eTransferSrcOptimal, *output_depth_.handle(),
                      vk::ImageLayout::eTransferDstOptimal, blit, vk::Filter::eNearest);
    } else {
        // Sin blit de profundidad: los gizmos se ven por encima de todo.
        cmd.clearDepthStencilImage(*output_depth_.handle(), vk::ImageLayout::eTransferDstOptimal,
                                   vk::ClearDepthStencilValue{1.0f, 0}, depth_range);
    }
    source.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
    source.srcAccessMask = vk::AccessFlagBits2::eTransferRead;
    source.dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader | vk::PipelineStageFlagBits2::eEarlyFragmentTests;
    source.dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead | vk::AccessFlagBits2::eDepthStencilAttachmentRead;
    source.oldLayout = vk::ImageLayout::eTransferSrcOptimal;
    source.newLayout = compat::depthReadOnlyLayout();
    target.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
    target.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
    target.dstStageMask = vk::PipelineStageFlagBits2::eEarlyFragmentTests | vk::PipelineStageFlagBits2::eLateFragmentTests;
    target.dstAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentRead;
    target.oldLayout = vk::ImageLayout::eTransferDstOptimal;
    target.newLayout = compat::depthReadOnlyLayout();
    pipelineBarrier(cmd, {source, target});
}

void VulkanRenderer::recordOverlayPass(const vk::raii::CommandBuffer& cmd,
                                       std::uint32_t frame_index) {
    const vk::Extent2D extent = ldr_color_.extent();
    if (!overlay_pass_.prepare(device_, frame_index, overlay_geometry_, camera_view_projection_,
                               extent)) {
        return;
    }

    // Con escalado, el depth de la escena es de la resolucion interna: se
    // copia escalado (sin filtrar) a uno de la de pantalla para probarlo.
    const bool scaled = render_extent_ != extent;
    const VulkanImage& depth_image = scaled ? output_depth_ : gbuffer_.depth();
    if (scaled) recordOutputDepth(cmd);

    // La imagen compuesta vuelve a ser destino de color; el depth de la
    // escena se prueba en solo lectura (como el contorno).
    vk::ImageMemoryBarrier2 depth_barrier{};
    depth_barrier.srcStageMask = vk::PipelineStageFlagBits2::eLateFragmentTests |
                                 vk::PipelineStageFlagBits2::eFragmentShader |
                                 vk::PipelineStageFlagBits2::eComputeShader;
    depth_barrier.srcAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
    depth_barrier.dstStageMask = vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                                 vk::PipelineStageFlagBits2::eLateFragmentTests;
    depth_barrier.dstAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentRead;
    depth_barrier.oldLayout = compat::depthReadOnlyLayout();
    depth_barrier.newLayout = compat::depthReadOnlyLayout();
    depth_barrier.image = *depth_image.handle();
    depth_barrier.subresourceRange =
        vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1};
    pipelineBarrier(cmd, {colorBarrier(*ldr_color_.handle(), vk::ImageLayout::eShaderReadOnlyOptimal,
                                       vk::ImageLayout::eColorAttachmentOptimal,
                                       vk::PipelineStageFlagBits2::eFragmentShader,
                                       vk::AccessFlagBits2::eShaderSampledRead,
                                       vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                       vk::AccessFlagBits2::eColorAttachmentRead |
                                           vk::AccessFlagBits2::eColorAttachmentWrite),
                          depth_barrier});

    vk::RenderingAttachmentInfo color_attachment{};
    color_attachment.imageView = *ldr_color_.view();
    color_attachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
    color_attachment.loadOp = vk::AttachmentLoadOp::eLoad;
    color_attachment.storeOp = vk::AttachmentStoreOp::eStore;

    vk::RenderingAttachmentInfo depth_attachment{};
    depth_attachment.imageView = *depth_image.view();
    depth_attachment.imageLayout = compat::depthReadOnlyLayout();
    depth_attachment.loadOp = vk::AttachmentLoadOp::eLoad;
    depth_attachment.storeOp = vk::AttachmentStoreOp::eNone;

    vk::RenderingInfo rendering_info{};
    rendering_info.renderArea = vk::Rect2D{vk::Offset2D{0, 0}, extent};
    rendering_info.layerCount = 1;
    rendering_info.setColorAttachments(color_attachment);
    rendering_info.pDepthAttachment = &depth_attachment;

    compat::beginRendering(cmd, rendering_info);
    overlay_pass_.record(cmd, frame_index, extent, overlay_geometry_);
    compat::endRendering(cmd);
    pipelineBarrier(cmd, writtenToSampled(*ldr_color_.handle()));
}

// Paneles de la UI en el mundo sobre la imagen final, con la profundidad de
// la escena (como los gizmos, pero de esta vista: tambien en el juego y en VR).
void VulkanRenderer::recordWorldUiPass(const vk::raii::CommandBuffer& cmd, std::uint32_t frame_index) {
    (void)frame_index;
    const vk::Extent2D extent = ldr_color_.extent();
    const bool scaled = render_extent_ != extent;
    const VulkanImage& depth_image = scaled ? output_depth_ : gbuffer_.depth();
    if (scaled) recordOutputDepth(cmd);

    vk::ImageMemoryBarrier2 depth_barrier{};
    depth_barrier.srcStageMask = vk::PipelineStageFlagBits2::eLateFragmentTests |
                                 vk::PipelineStageFlagBits2::eFragmentShader |
                                 vk::PipelineStageFlagBits2::eComputeShader;
    depth_barrier.srcAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
    depth_barrier.dstStageMask = vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                                 vk::PipelineStageFlagBits2::eLateFragmentTests;
    depth_barrier.dstAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentRead;
    depth_barrier.oldLayout = compat::depthReadOnlyLayout();
    depth_barrier.newLayout = compat::depthReadOnlyLayout();
    depth_barrier.image = *depth_image.handle();
    depth_barrier.subresourceRange = vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1};
    pipelineBarrier(cmd, {colorBarrier(*ldr_color_.handle(), vk::ImageLayout::eShaderReadOnlyOptimal,
                                       vk::ImageLayout::eColorAttachmentOptimal,
                                       vk::PipelineStageFlagBits2::eFragmentShader,
                                       vk::AccessFlagBits2::eShaderSampledRead,
                                       vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                       vk::AccessFlagBits2::eColorAttachmentRead |
                                           vk::AccessFlagBits2::eColorAttachmentWrite),
                          depth_barrier});

    vk::RenderingAttachmentInfo color_attachment{};
    color_attachment.imageView = *ldr_color_.view();
    color_attachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
    color_attachment.loadOp = vk::AttachmentLoadOp::eLoad;
    color_attachment.storeOp = vk::AttachmentStoreOp::eStore;

    vk::RenderingAttachmentInfo depth_attachment{};
    depth_attachment.imageView = *depth_image.view();
    depth_attachment.imageLayout = compat::depthReadOnlyLayout();
    depth_attachment.loadOp = vk::AttachmentLoadOp::eLoad;
    depth_attachment.storeOp = vk::AttachmentStoreOp::eNone;

    vk::RenderingInfo rendering_info{};
    rendering_info.renderArea = vk::Rect2D{vk::Offset2D{0, 0}, extent};
    rendering_info.layerCount = 1;
    rendering_info.setColorAttachments(color_attachment);
    rendering_info.pDepthAttachment = &depth_attachment;

    compat::beginRendering(cmd, rendering_info);
    world_ui_pass_.recordQuads(cmd, camera_view_projection_, extent);
    compat::endRendering(cmd);
    pipelineBarrier(cmd, writtenToSampled(*ldr_color_.handle()));
}

// Escalado de la imagen HDR interna a la de pantalla, antes del bloom y la
// composicion: TAA/TAAU (temporal, con historia) o FSR 1 (EASU, espacial), y
// despues la nitidez (RCAS) a upscaled_color_, que es lo que lee el resto.
// -----------------------------------------------------------------------------
// FSR 3.1 y DLSS 4 (TemporalUpscalers.h)
// -----------------------------------------------------------------------------

bool VulkanRenderer::fsr3Supported() const { return !device_.compatMode() && Fsr3Upscaler::supported(); }

Upscaler VulkanRenderer::activeUpscaler() const {
    if (!upscaling_) return Upscaler::Off;
    if ((graphics_.upscaler == Upscaler::Fsr3 || graphics_.upscaler == Upscaler::Dlss) && !vendor_upscaler_) {
        return Upscaler::Taa;
    }
    return graphics_.upscaler;
}

// Tras crear los destinos (el dispositivo esta parado): el contexto del SDK
// elegido con la resolucion interna y la de pantalla. Si no se puede, TAA.
// Crear un contexto cuesta decenas o cientos de ms (FSR 3 compila sus
// pipelines): antes se rehacia cada vez que el presupuesto adaptativo movia la
// resolucion interna, y el tiron hacia bajar otro paso (mas lag que sin
// escalador). Ahora los dos admiten resolucion dinamica: FSR 3 se crea con
// la resolucion de pantalla como maxima y DLSS con la del modo del usuario
// (y un subrectangulo por frame); solo se rehacen con otra pantalla u otro modo.
void VulkanRenderer::configureVendorUpscaler() {
    const vk::Extent2D output = outputExtent();
    const bool same_output = vendor_output_.width == output.width && vendor_output_.height == output.height;
    std::string error;
    if (graphics_.upscaler != Upscaler::Dlss) dlss_.releaseFeature();
    if (graphics_.upscaler != Upscaler::Fsr3) fsr3_.destroy();
    vendor_upscaler_ = false;
    if (device_.compatMode() && (graphics_.upscaler == Upscaler::Fsr3 || graphics_.upscaler == Upscaler::Dlss)) {
        // Modo compatible (moviles, GPU antiguas): ni FSR 3 ni DLSS.
        upscaler_status_ = "FSR 3 y DLSS no estan en el modo compatible: se usa TAA";
        vendor_output_ = output;
        taa_history_valid_ = false;
        xr_eye1_.taa_valid = false;
        return;
    }
    if (graphics_.upscaler == Upscaler::Fsr3) {
        if (!fsr3_.ready() || !same_output) {
            if (!fsr3_.create(static_cast<VkInstance>(*instance_.handle()), static_cast<VkDevice>(*device_.handle()),
                              static_cast<VkPhysicalDevice>(*device_.physicalDevice()), output.width, output.height,
                              output.width, output.height, error)) {
                upscaler_status_ = "FSR 3 no disponible (" + error + "): se usa TAA";
                std::cerr << "[FSR 3] " << upscaler_status_ << "\n";
            }
        }
        vendor_upscaler_ = fsr3_.ready();
        if (vendor_upscaler_) upscaler_status_ = "AMD FSR 3.1 (INESTABLE)";
    } else if (graphics_.upscaler == Upscaler::Dlss) {
        if (!dlss_.available()) {
            upscaler_status_ = "DLSS no disponible en esta GPU: se usa TAA";
        } else {
            // El modo y la resolucion maxima salen de lo que eligio el usuario;
            // lo que baje el presupuesto va como subrectangulo.
            const float scale = renderScale(user_graphics_);
            const vk::Extent2D base{
                std::max(render_extent_.width,
                         static_cast<std::uint32_t>(std::lround(static_cast<float>(output.width) * scale))),
                std::max(render_extent_.height,
                         static_cast<std::uint32_t>(std::lround(static_cast<float>(output.height) * scale)))};
            const bool fits = dlss_render_.width >= render_extent_.width && dlss_render_.height >= render_extent_.height;
            const bool same_mode = dlss_quality_ == user_graphics_.quality &&
                                   (user_graphics_.quality != UpscaleQuality::Custom ||
                                    (dlss_render_.width == base.width && dlss_render_.height == base.height));
            if (!dlss_.featureReady() || !same_output || !fits || !same_mode) {
                bool created = false;
                device_.submitOneTime([&](const vk::raii::CommandBuffer& cmd) {
                    created = dlss_.createFeature(static_cast<VkCommandBuffer>(*cmd), base.width, base.height,
                                                  output.width, output.height, user_graphics_.quality, error);
                });
                dlss_render_ = created ? base : vk::Extent2D{};
                dlss_quality_ = user_graphics_.quality;
                if (!created) {
                    upscaler_status_ = "DLSS no se pudo crear (" + error + "): se usa TAA";
                    std::cerr << "[DLSS] " << upscaler_status_ << "\n";
                }
            }
            vendor_upscaler_ = dlss_.featureReady();
            if (vendor_upscaler_) {
                upscaler_status_ =
                    user_graphics_.quality == UpscaleQuality::Native ? "NVIDIA DLAA (DLSS 4, INESTABLE)" : "NVIDIA DLSS 4 (INESTABLE)";
            }
        }
    } else {
        upscaler_status_ = xr_forced_taa_ ? "VR (un ojo por camara): TAA del motor con historia por ojo" : "";
    }
    vendor_output_ = output;
    taa_history_valid_ = false;
    xr_eye1_.taa_valid = false;
}

void VulkanRenderer::recordVendorUpscale(const vk::raii::CommandBuffer& cmd) {
    using Stage = vk::PipelineStageFlagBits2;
    using Access = vk::AccessFlagBits2;
    const vk::Extent2D render = render_extent_;
    const vk::Extent2D output = upscale_target_.extent();

    // 1) Vectores de movimiento completos (el cielo con la camara).
    pipelineBarrier(cmd, discardToAttachment(*upscaler_motion_.handle()));
    const core::Mat4 reproject = taa_reproject_;
    drawFullscreen(cmd, upscaler_motion_pass_, &upscaler_motion_sets_[0], upscaler_motion_, &reproject);

    // 2) Entradas para leer (SHADER_READ_ONLY) y la salida para escribir (GENERAL).
    const vk::ImageSubresourceRange depth_range{gbuffer_.depth().aspect(), 0, 1, 0, 1};
    vk::ImageMemoryBarrier2 depth_in{};
    depth_in.srcStageMask = Stage::eAllCommands;
    depth_in.srcAccessMask = Access::eMemoryRead | Access::eMemoryWrite;
    depth_in.dstStageMask = Stage::eAllCommands;
    depth_in.dstAccessMask = Access::eMemoryRead;
    depth_in.oldLayout = compat::depthReadOnlyLayout();
    depth_in.newLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
    depth_in.image = *gbuffer_.depth().handle();
    depth_in.subresourceRange = depth_range;
    vk::ImageMemoryBarrier2 out_in = colorBarrier(*upscale_target_.handle(), vk::ImageLayout::eUndefined,
                                                  vk::ImageLayout::eGeneral, Stage::eAllCommands, Access::eNone,
                                                  Stage::eAllCommands, Access::eMemoryRead | Access::eMemoryWrite);
    vk::ImageMemoryBarrier2 motion_read = writtenToSampled(*upscaler_motion_.handle());
    motion_read.dstStageMask = Stage::eAllCommands;
    pipelineBarrier(cmd, {depth_in, out_in, motion_read});

    UpscaleDispatch d{};
    d.command_buffer = static_cast<VkCommandBuffer>(*cmd);
    const auto image = [](const VulkanImage& img, bool depth) {
        return UpscaleImage{static_cast<VkImage>(*img.handle()), static_cast<VkImageView>(*img.view()),
                            static_cast<VkFormat>(img.format()), img.extent().width, img.extent().height, depth};
    };
    d.color = image(scene_color_, false);
    d.depth = image(gbuffer_.depth(), true);
    d.motion = image(upscaler_motion_, false);
    d.output = image(upscale_target_, false);
    d.render_width = render.width;
    d.render_height = render.height;
    // La imagen se desplazo jitter_ndc * tamano / 2 pixeles (+x derecha, +y abajo).
    d.jitter_x = jitter_ndc_.x * 0.5f * static_cast<float>(render.width);
    d.jitter_y = jitter_ndc_.y * 0.5f * static_cast<float>(render.height);
    d.reset = !taa_history_valid_;
    d.frame_ms = std::max(frame_delta_seconds_, 0.0001f) * 1000.0f;
    d.near_plane = camera_near_;
    d.far_plane = camera_far_;
    d.fov_y = camera_fov_;
    const bool ok = graphics_.upscaler == Upscaler::Dlss ? dlss_.evaluate(d) : fsr3_.dispatch(d);
    (void)output;
    taa_history_valid_ = ok;

    // 3) La profundidad vuelve a como la leen los demas; la salida, a textura.
    vk::ImageMemoryBarrier2 depth_back = depth_in;
    depth_back.oldLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
    depth_back.newLayout = compat::depthReadOnlyLayout();
    depth_back.srcAccessMask = Access::eMemoryRead | Access::eMemoryWrite;
    vk::ImageMemoryBarrier2 out_back = colorBarrier(*upscale_target_.handle(), vk::ImageLayout::eGeneral,
                                                    vk::ImageLayout::eShaderReadOnlyOptimal, Stage::eAllCommands,
                                                    Access::eMemoryWrite, Stage::eFragmentShader | Stage::eComputeShader,
                                                    Access::eShaderSampledRead);
    pipelineBarrier(cmd, {depth_back, out_back});
}

void VulkanRenderer::recordUpscalePass(const vk::raii::CommandBuffer& cmd) {
    const vk::Extent2D render = render_extent_;
    const vk::Extent2D output = upscale_target_.extent();
    const auto bits = [](float value) {
        std::uint32_t result = 0;
        std::memcpy(&result, &value, sizeof(result));
        return result;
    };

    vk::ImageMemoryBarrier2 scene_read = writtenToSampled(*scene_color_.handle());
    scene_read.dstStageMask |= vk::PipelineStageFlagBits2::eComputeShader;
    pipelineBarrier(cmd, {scene_read, discardToAttachment(*upscale_target_.handle())});

    if (vendor_upscaler_ && !isolated()) {
        recordVendorUpscale(cmd);
    } else if (graphics_.upscaler == Upscaler::Fsr1) {
        // Constantes de FsrEasuCon (ffx_fsr1.h).
        const float in_w = static_cast<float>(render.width);
        const float in_h = static_cast<float>(render.height);
        const float out_w = static_cast<float>(output.width);
        const float out_h = static_cast<float>(output.height);
        GpuEasuPush push{};
        push.con[0] = bits(in_w / out_w);
        push.con[1] = bits(in_h / out_h);
        push.con[2] = bits(0.5f * in_w / out_w - 0.5f);
        push.con[3] = bits(0.5f * in_h / out_h - 0.5f);
        push.con[4] = bits(1.0f / in_w);
        push.con[5] = bits(1.0f / in_h);
        push.con[6] = bits(1.0f / in_w);
        push.con[7] = bits(-1.0f / in_h);
        push.con[8] = bits(-1.0f / in_w);
        push.con[9] = bits(2.0f / in_h);
        push.con[10] = bits(1.0f / in_w);
        push.con[11] = bits(2.0f / in_h);
        push.con[12] = bits(0.0f);
        push.con[13] = bits(4.0f / in_h);
        drawFullscreen(cmd, easu_pass_, &easu_sets_[0], upscale_target_, &push);
        pipelineBarrier(cmd, writtenToSampled(*upscale_target_.handle()));
    } else {
        GpuTaaPush push{};
        push.render_size = Vec4{static_cast<float>(render.width), static_cast<float>(render.height),
                                1.0f / static_cast<float>(render.width), 1.0f / static_cast<float>(render.height)};
        push.output_size = Vec4{static_cast<float>(output.width), static_cast<float>(output.height),
                                1.0f / static_cast<float>(output.width), 1.0f / static_cast<float>(output.height)};
        push.jitter = isolated() ? Vec4{} : Vec4{jitter_ndc_.x * 0.5f, jitter_ndc_.y * 0.5f, taa_history_valid_ ? 1.0f : 0.0f, 0.0f};
        push.reproject = taa_reproject_;
        drawFullscreen(cmd, taa_pass_, &taa_sets_[eyeSet()], upscale_target_, &push);
        if (isolated()) {
            pipelineBarrier(cmd, writtenToSampled(*upscale_target_.handle()));
        } else {

        // Historia del frame siguiente (antes de la nitidez).
        pipelineBarrier(cmd, {colorBarrier(*upscale_target_.handle(), vk::ImageLayout::eColorAttachmentOptimal,
                                           vk::ImageLayout::eTransferSrcOptimal,
                                           vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                           vk::AccessFlagBits2::eColorAttachmentWrite,
                                           vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead),
                              colorBarrier(*taa_history_.handle(), vk::ImageLayout::eShaderReadOnlyOptimal,
                                           vk::ImageLayout::eTransferDstOptimal,
                                           vk::PipelineStageFlagBits2::eFragmentShader,
                                           vk::AccessFlagBits2::eShaderSampledRead,
                                           vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite)});
        vk::ImageCopy region{};
        region.srcSubresource = vk::ImageSubresourceLayers{vk::ImageAspectFlagBits::eColor, 0, 0, 1};
        region.dstSubresource = region.srcSubresource;
        region.extent = vk::Extent3D{output.width, output.height, 1};
        cmd.copyImage(*upscale_target_.handle(), vk::ImageLayout::eTransferSrcOptimal, *taa_history_.handle(),
                      vk::ImageLayout::eTransferDstOptimal, region);
        pipelineBarrier(cmd, {colorBarrier(*upscale_target_.handle(), vk::ImageLayout::eTransferSrcOptimal,
                                           vk::ImageLayout::eShaderReadOnlyOptimal,
                                           vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead,
                                           vk::PipelineStageFlagBits2::eFragmentShader,
                                           vk::AccessFlagBits2::eShaderSampledRead),
                              colorBarrier(*taa_history_.handle(), vk::ImageLayout::eTransferDstOptimal,
                                           vk::ImageLayout::eShaderReadOnlyOptimal,
                                           vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite,
                                           vk::PipelineStageFlagBits2::eFragmentShader,
                                           vk::AccessFlagBits2::eShaderSampledRead)});
        taa_history_valid_ = true;
        }
    }

    // Nitidez (RCAS): 0 = sin tocar, 1 = la maxima de FSR.
    const float sharpness = std::clamp(graphics_.sharpness, 0.0f, 1.0f);
    GpuRcasPush rcas{};
    rcas.con[0] = bits(std::exp2(-(1.0f - sharpness) * 2.0f));
    rcas.bypass = sharpness <= 0.001f ? 1u : 0u;
    pipelineBarrier(cmd, discardToAttachment(*upscaled_color_.handle()));
    drawFullscreen(cmd, rcas_pass_, &rcas_sets_[0], upscaled_color_, &rcas);
    // (El bloom la pasa a textura: es postSource().)
}

void VulkanRenderer::recordBloomPass(const vk::raii::CommandBuffer& cmd) {
    const vk::Extent2D screen = outputExtent();

    // La imagen HDR pasa a textura; todos los niveles se reescriben enteros.
    // La imagen HDR la leen tambien los rayos de luz y el histograma de la
    // exposicion (compute shader).
    std::array<vk::ImageMemoryBarrier2, kBloomLevels + 1> barriers{};
    barriers[0] = writtenToSampled(*postSource().handle());
    barriers[0].dstStageMask |= vk::PipelineStageFlagBits2::eComputeShader;
    for (std::uint32_t level = 0; level < kBloomLevels; ++level) {
        barriers[level + 1] = discardToAttachment(*bloom_levels_[level].handle());
    }
    pipelineBarrier(cmd, barriers);

    // --- Bajada: pantalla -> 1/2 -> 1/4 -> ... ---
    for (std::uint32_t level = 0; level < kBloomLevels; ++level) {
        const vk::Extent2D source_extent =
            (level == 0) ? screen : bloomLevelExtent(screen, level - 1);

        GpuBloomPush push{};
        push.source_texel = inverseExtent(source_extent);
        push.first_level = (level == 0) ? 1.0f : 0.0f;
        // En la bajada, el ultimo campo es el umbral (solo el primer nivel).
        push.radius = std::max(post_.bloom_threshold, 0.0f);
        drawFullscreen(cmd, bloom_down_pass_, &bloom_down_sets_[level], bloom_levels_[level],
                       &push);

        pipelineBarrier(cmd, writtenToSampled(*bloom_levels_[level].handle()));
    }

    // --- Subida: cada nivel se suma, filtrado, al del doble de tamano ---
    for (std::uint32_t level = kBloomLevels - 1; level > 0; --level) {
        const VulkanImage& target = bloom_levels_[level - 1];

        // El destino ya tiene su resultado de la bajada, que se conserva
        // (mezcla aditiva): lectura + escritura del attachment.
        pipelineBarrier(cmd, colorBarrier(*target.handle(),
                                          vk::ImageLayout::eShaderReadOnlyOptimal,
                                          vk::ImageLayout::eColorAttachmentOptimal,
                                          vk::PipelineStageFlagBits2::eFragmentShader,
                                          vk::AccessFlagBits2::eShaderSampledRead,
                                          vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                          vk::AccessFlagBits2::eColorAttachmentRead |
                                              vk::AccessFlagBits2::eColorAttachmentWrite));

        GpuBloomPush push{};
        push.source_texel = inverseExtent(bloomLevelExtent(screen, level));
        push.radius = std::clamp(post_.bloom_scatter, 0.1f, 4.0f);
        drawFullscreen(cmd, bloom_up_pass_, &bloom_up_sets_[level - 1], target, &push,
                       /*load=*/true);

        pipelineBarrier(cmd, writtenToSampled(*target.handle()));
    }
}

void VulkanRenderer::recordSkyLutPass(const vk::raii::CommandBuffer& cmd) {
    // Suelo del entorno mojado: la pelicula de agua de la humedad y los
    // charcos (mismas cantidades que la pasada de geometria).
    const bool raining = rain_enabled_ && rainAvailable();
    const float ground_wet =
        raining ? std::min(0.25f * wetness_ + 0.45f * puddles_, 0.7f) : 0.0f;

    // Nada cambio (la hora quieta, el mismo cielo): la LUT y el IBL de antes
    // siguen valiendo y siguen como texturas. Rehacer el IBL prefiltra 6
    // caras x 6 niveles por compute: en una GPU integrada o de movil eran
    // milisegundos cada frame para obtener lo mismo.
    const std::array<float, 16> signature = {
        sky_push_.sun.x,          sky_push_.sun.y,          sky_push_.sun.z,       sky_push_.sun.w,
        sky_push_.moon.x,         sky_push_.moon.y,         sky_push_.moon.z,      sky_push_.moon.w,
        ibl_light_radiance_.x,    ibl_light_radiance_.y,    ibl_light_radiance_.z, ibl_to_light_.x,
        ibl_to_light_.y,          ibl_to_light_.z,          ground_wet,
        static_cast<float>(environmentActive() ? environment_generation_ + 1u : 0u)};
    if (!capturing_ && sky_recorded_once_ && signature == sky_signature_) {
        return;
    }

    // Movil: el cielo y el IBL cambian despacio; se rehacen 1 de cada 8
    // frames (el resto se usan los de antes, que siguen como texturas).
    if (mobile_level_ >= 0 && mobile_level_ < 3 && !capturing_) {
        if (sky_frames_skipped_ < 7 && sky_recorded_once_) {
            ++sky_frames_skipped_;
            return;
        }
        sky_frames_skipped_ = 0;
        sky_recorded_once_ = true;
    }
    pipelineBarrier(cmd, discardToAttachment(*sky_lut_.handle()));
    drawFullscreen(cmd, sky_lut_pass_, nullptr, sky_lut_, &sky_push_);

    // La leen la iluminacion (fragment) y el IBL (compute).
    vk::ImageMemoryBarrier2 to_sampled = writtenToSampled(*sky_lut_.handle());
    to_sampled.dstStageMask |= vk::PipelineStageFlagBits2::eComputeShader;
    pipelineBarrier(cmd, to_sampled);

    ibl_probe_.record(cmd, ibl_light_radiance_, ibl_to_light_, environmentActive(), ground_wet);
    if (!capturing_) {
        sky_signature_ = signature;
        sky_recorded_once_ = true;
    }
}

void VulkanRenderer::recordRainMap(const vk::raii::CommandBuffer& cmd,
                                   std::uint32_t frame_index) {
    // --- Caja del escenario y proyeccion desde arriba ---
    Vec3 low{1e30f, 1e30f, 1e30f};
    Vec3 high{-1e30f, -1e30f, -1e30f};
    for (const core::Aabb& box : submesh_bounds_) {
        low = Vec3{std::min(low.x, box.min.x), std::min(low.y, box.min.y), std::min(low.z, box.min.z)};
        high = Vec3{std::max(high.x, box.max.x), std::max(high.y, box.max.y),
                    std::max(high.z, box.max.z)};
    }
    if (submesh_bounds_.empty()) {
        return;  // Solo actores animados: nada que cubra.
    }
    const Vec3 center = (low + high) * 0.5f;
    const float half = std::max(high.x - low.x, high.z - low.z) * 0.5f + 1.0f;
    const float height = high.y - low.y + 20.0f;
    const Vec3 eye{center.x, high.y + 10.0f, center.z};
    const core::Mat4 view = core::lookAt(eye, Vec3{center.x, low.y, center.z},
                                         Vec3{0.0f, 0.0f, -1.0f});
    rain_view_projection_ = core::orthographic(-half, half, -half, half, 0.0f, height) * view;

    // --- Dibujo: solo profundidad, con el pipeline de las sombras ---
    const vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1};
    vk::ImageMemoryBarrier2 to_attachment{};
    to_attachment.srcStageMask = vk::PipelineStageFlagBits2::eFragmentShader;
    to_attachment.srcAccessMask = vk::AccessFlagBits2::eShaderSampledRead;
    to_attachment.dstStageMask = vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                                 vk::PipelineStageFlagBits2::eLateFragmentTests;
    to_attachment.dstAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentRead |
                                  vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
    to_attachment.oldLayout = vk::ImageLayout::eUndefined;
    to_attachment.newLayout = compat::depthAttachmentLayout();
    to_attachment.image = *rain_map_.handle();
    to_attachment.subresourceRange = range;
    pipelineBarrier(cmd, to_attachment);

    vk::RenderingAttachmentInfo depth_attachment{};
    depth_attachment.imageView = *rain_map_.view();
    depth_attachment.imageLayout = compat::depthAttachmentLayout();
    depth_attachment.loadOp = vk::AttachmentLoadOp::eClear;
    depth_attachment.storeOp = vk::AttachmentStoreOp::eStore;
    depth_attachment.clearValue = vk::ClearValue{vk::ClearDepthStencilValue{1.0f, 0}};
    const vk::Extent2D extent{kRainMapSize, kRainMapSize};
    vk::RenderingInfo rendering_info{};
    rendering_info.renderArea = vk::Rect2D{vk::Offset2D{0, 0}, extent};
    rendering_info.layerCount = 1;
    rendering_info.pDepthAttachment = &depth_attachment;
    compat::beginRendering(cmd, rendering_info);
    cmd.setViewport(0, vk::Viewport{0.0f, 0.0f, static_cast<float>(kRainMapSize),
                                    static_cast<float>(kRainMapSize), 0.0f, 1.0f});
    cmd.setScissor(0, vk::Rect2D{vk::Offset2D{0, 0}, extent});
    recordActorShadows(cmd, frame_index, rain_view_projection_);
    compat::endRendering(cmd);

    vk::ImageMemoryBarrier2 to_read = to_attachment;
    to_read.srcStageMask = vk::PipelineStageFlagBits2::eLateFragmentTests;
    to_read.srcAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
    to_read.dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader;
    to_read.dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead;
    to_read.oldLayout = compat::depthAttachmentLayout();
    to_read.newLayout = compat::depthReadOnlyLayout();
    pipelineBarrier(cmd, to_read);

    rain_map_ready_ = true;
    std::cout << "[Vulkan] Mapa de lluvia listo: " << kRainMapSize << "^2 sobre "
              << 2.0f * half << " m\n";
}

void VulkanRenderer::recordCloudPass(const vk::raii::CommandBuffer& cmd,
                                     std::uint32_t frame_index) {
    // Sombra de las nubes sobre el suelo (antes que la iluminacion). En VR, una
    // vez por frame: el segundo ojo usa la del primero.
    if (!xrSecondEye()) {
        pipelineBarrier(cmd, discardToAttachment(*cloud_shadow_image_.handle()));
        if (clouds_enabled_ && !environmentActive() && cloud_settings_.shadows && cloud_settings_.coverage > 0.0f) {
            drawFullscreen(cmd, cloud_shadow_pass_, &clouds_sets_[frame_index], cloud_shadow_image_,
                           &cloud_shadow_push_);
        }
        pipelineBarrier(cmd, writtenToSampled(*cloud_shadow_image_.handle()));
    }

    pipelineBarrier(cmd, discardToAttachment(*clouds_image_.handle()));
    // Apagadas no se dibujan (la iluminacion no las lee), pero la imagen
    // queda igualmente como textura para el descriptor.
    if (clouds_enabled_ && !environmentActive()) {
        drawFullscreen(cmd, clouds_pass_, &clouds_sets_[frame_index], clouds_image_,
                       &cloud_push_);
    }
    pipelineBarrier(cmd, writtenToSampled(*clouds_image_.handle()));
}

void VulkanRenderer::recordCameraFxPass(const vk::raii::CommandBuffer& cmd) {
    const PostProcessSettings& p = post_;
    const bool blur = p.motion_blur && p.motion_blur_intensity > 0.0f && !capturing_;
    const bool dof = p.depth_of_field && !capturing_;
    if ((!blur && !dof) || drawModeNow() != SceneDrawMode::Lit) {
        return;
    }
    const VulkanImage& target = postSource();
    const vk::Extent2D extent = target.extent();

    GpuCameraFxPush push{};
    // Reproyeccion del cielo: del clip de este frame (sin jitter) al anterior.
    // La de TAA: al grabar, motion_view_projection_ y camera_view_projection_
    // ya son los dos los de este frame (la identidad: el cielo nunca se
    // emborronaba y lo que pasaba por delante quedaba con el borde nitido).
    push.reproject = taa_reproject_;
    push.params = Vec4{0.0f, std::clamp(p.motion_blur_intensity, 0.0f, 1.0f), std::clamp(p.motion_blur_max, 0.001f, 0.25f),
                       static_cast<float>(frame_count_ % 1024)};
    // Autoenfoque suave: la distancia que midio la GPU (dof_focus.comp) hace
    // un par de frames, seguida como el motor de una lente (no salta).
    float focus = std::max(p.dof_focus_distance, 0.05f);
    if (dof && p.dof_auto_focus) {
        if (!isolated()) recordDofFocusMeasure(cmd, current_frame_);
        focus = isolated() && dof_focus_current_ > 0.0f ? dof_focus_current_ : smoothedDofFocus();
    } else {
        dof_focus_current_ = -1.0f;  // al volver al autoenfoque empieza donde este
    }
    push.dof = Vec4{focus, std::max(p.dof_aperture, 0.5f),
                    std::clamp(p.dof_focal_length, 5.0f, 600.0f), 0.025f};
    push.camera = Vec4{camera_projection_.m[3][2], camera_projection_.m[2][2],
                       static_cast<float>(extent.width) / static_cast<float>(std::max(extent.height, 1u)),
                       1.0f / static_cast<float>(std::max(extent.height, 1u))};
    push.projection_w = Vec4{camera_projection_.m[2][3], camera_projection_.m[3][3], 0.0f, 0.0f};

    const auto run = [&](float mode) {
        // Copia de lo que hay (la lee el shader) y se dibuja encima.
        pipelineBarrier(cmd, {colorBarrier(*target.handle(), vk::ImageLayout::eColorAttachmentOptimal,
                                           vk::ImageLayout::eTransferSrcOptimal,
                                           vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                           vk::AccessFlagBits2::eColorAttachmentWrite, vk::PipelineStageFlagBits2::eCopy,
                                           vk::AccessFlagBits2::eTransferRead),
                              colorBarrier(*camera_fx_source_.handle(), vk::ImageLayout::eUndefined,
                                           vk::ImageLayout::eTransferDstOptimal,
                                           vk::PipelineStageFlagBits2::eFragmentShader,
                                           vk::AccessFlagBits2::eShaderSampledRead, vk::PipelineStageFlagBits2::eCopy,
                                           vk::AccessFlagBits2::eTransferWrite)});
        vk::ImageCopy region{};
        region.srcSubresource = vk::ImageSubresourceLayers{vk::ImageAspectFlagBits::eColor, 0, 0, 1};
        region.dstSubresource = region.srcSubresource;
        region.extent = vk::Extent3D{extent.width, extent.height, 1};
        cmd.copyImage(*target.handle(), vk::ImageLayout::eTransferSrcOptimal, *camera_fx_source_.handle(),
                      vk::ImageLayout::eTransferDstOptimal, region);
        pipelineBarrier(cmd, {colorBarrier(*target.handle(), vk::ImageLayout::eTransferSrcOptimal,
                                           vk::ImageLayout::eColorAttachmentOptimal, vk::PipelineStageFlagBits2::eCopy,
                                           vk::AccessFlagBits2::eTransferRead,
                                           vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                           vk::AccessFlagBits2::eColorAttachmentWrite),
                              colorBarrier(*camera_fx_source_.handle(), vk::ImageLayout::eTransferDstOptimal,
                                           vk::ImageLayout::eShaderReadOnlyOptimal, vk::PipelineStageFlagBits2::eCopy,
                                           vk::AccessFlagBits2::eTransferWrite, vk::PipelineStageFlagBits2::eFragmentShader,
                                           vk::AccessFlagBits2::eShaderSampledRead)});
        push.params.x = mode;
        drawFullscreen(cmd, camera_fx_pass_, &camera_fx_sets_[0], target, &push);
    };
    // Primero el desenfoque de la lente y despues el del movimiento (como
    // una camara: el rastro es de la imagen ya desenfocada).
    if (dof) run(0.0f);
    if (blur) run(1.0f);
}

void VulkanRenderer::recordLightShaftPass(const vk::raii::CommandBuffer& cmd) {
    // Apagados, de noche o con el sol fuera de la vista (intensidad 0) no
    // hay nada que dibujar: la composicion no los lee (settings.tone.x = 0).
    // Antes se dibujaba igual una imagen negra cada frame.
    light_shafts_drawn_ = light_shaft_push_.intensity > 0.0f;
    if (!light_shafts_drawn_) {
        if (!light_shafts_ready_) {
            // Recien creada: a textura una vez, para el descriptor.
            pipelineBarrier(cmd, colorBarrier(*light_shafts_.handle(), vk::ImageLayout::eUndefined,
                                              vk::ImageLayout::eShaderReadOnlyOptimal,
                                              vk::PipelineStageFlagBits2::eTopOfPipe, vk::AccessFlagBits2::eNone,
                                              vk::PipelineStageFlagBits2::eFragmentShader,
                                              vk::AccessFlagBits2::eShaderSampledRead));
            light_shafts_ready_ = true;
        }
        return;
    }
    light_shafts_ready_ = true;
    pipelineBarrier(cmd, discardToAttachment(*light_shafts_.handle()));
    drawFullscreen(cmd, light_shaft_pass_, &light_shaft_sets_[0], light_shafts_,
                   &light_shaft_push_);
    pipelineBarrier(cmd, writtenToSampled(*light_shafts_.handle()));
}

void VulkanRenderer::recordAutoExposurePass(const vk::raii::CommandBuffer& cmd) {
    // El frame anterior vacio el histograma y leyo/copio la exposicion: hay
    // que esperar a que acabe antes de volver a escribirlos.
    memoryBarrier(cmd,
                  vk::PipelineStageFlagBits2::eComputeShader |
                      vk::PipelineStageFlagBits2::eFragmentShader |
                      vk::PipelineStageFlagBits2::eTransfer,
                  vk::AccessFlagBits2::eShaderStorageWrite |
                      vk::AccessFlagBits2::eShaderStorageRead |
                      vk::AccessFlagBits2::eTransferRead,
                  vk::PipelineStageFlagBits2::eComputeShader,
                  vk::AccessFlagBits2::eShaderStorageRead |
                      vk::AccessFlagBits2::eShaderStorageWrite);

    // --- 1) Histograma ---
    const vk::Extent2D extent = postSource().extent();
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *histogram_pass_.pipeline());
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *histogram_pass_.layout(), 0,
                           *histogram_sets_[0], nullptr);
    cmd.dispatch((extent.width + 15) / 16, (extent.height + 15) / 16, 1);

    memoryBarrier(cmd, vk::PipelineStageFlagBits2::eComputeShader,
                  vk::AccessFlagBits2::eShaderStorageWrite,
                  vk::PipelineStageFlagBits2::eComputeShader,
                  vk::AccessFlagBits2::eShaderStorageRead |
                      vk::AccessFlagBits2::eShaderStorageWrite);

    // --- 2) Promedio entre percentiles y adaptacion ---
    GpuExposurePush push{};
    // Un frame muy largo (carga, ventana arrastrada) no debe saltar la
    // adaptacion de golpe.
    push.delta_seconds = std::min(frame_delta_seconds_, 0.1f);
    push.min_log_exposure = std::min(post_.min_ev, post_.max_ev);
    push.max_log_exposure = std::max(post_.min_ev, post_.max_ev);
    push.speed_up = std::max(post_.adaptation_speed_up, 0.01f);
    push.speed_down = std::max(post_.adaptation_speed_down, 0.01f);
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *exposure_average_pass_.pipeline());
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *exposure_average_pass_.layout(), 0,
                           *exposure_average_sets_[0], nullptr);
    cmd.pushConstants<GpuExposurePush>(*exposure_average_pass_.layout(),
                                       vk::ShaderStageFlagBits::eCompute, 0, push);
    cmd.dispatch(1, 1, 1);

    // La composicion la lee y se copia para mostrarla en la CPU.
    memoryBarrier(cmd, vk::PipelineStageFlagBits2::eComputeShader,
                  vk::AccessFlagBits2::eShaderStorageWrite,
                  vk::PipelineStageFlagBits2::eFragmentShader |
                      vk::PipelineStageFlagBits2::eTransfer,
                  vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eTransferRead);
}

void VulkanRenderer::recordCompositePass(const vk::raii::CommandBuffer& cmd,
                                         std::uint32_t frame_index) {
    pipelineBarrier(cmd, discardToAttachment(*ldr_color_.handle()));

    // Ajustes del frame (PostProcessSettings -> GpuCompositeSettings).
    const PostProcessSettings& p = post_;
    const vk::Extent2D screen = ldr_color_.extent();
    GpuCompositeSettings settings{};
    settings.exposure = Vec4{exposure_ * p.manual_exposure, p.bloom ? p.bloom_intensity : 0.0f,
                             p.auto_exposure ? 1.0f : 0.0f, p.exposure_compensation};
    settings.tone = Vec4{p.light_shafts && light_shafts_drawn_ ? 0.6f * p.light_shaft_intensity : 0.0f,
                         static_cast<float>(static_cast<std::int32_t>(p.tonemapper)),
                         p.saturation, p.contrast};
    settings.look = Vec4{p.vibrance, p.vignette ? p.vignette_intensity : 0.0f,
                         p.vignette_smoothness, p.chromatic_aberration};
    settings.film = Vec4{p.film_grain, static_cast<float>(frame_count_ % 4096),
                         1.0f / static_cast<float>(std::max(screen.width, 1u)),
                         1.0f / static_cast<float>(std::max(screen.height, 1u))};
    settings.white_balance = toVec4(whiteBalanceLms(p.temperature, p.tint), 0.0f);
    // w: filtro de daltonismo (modo, fuerza, corregir).
    settings.color_filter = toVec4(p.color_filter, static_cast<float>(std::clamp(accessibility_.colorblind_mode, 0, 4)));
    settings.lift = toVec4(p.lift, std::clamp(accessibility_.colorblind_strength, 0.0f, 1.0f));
    settings.gamma = toVec4(p.gamma, accessibility_.colorblind_correct ? 1.0f : 0.0f);
    settings.gain = toVec4(p.gain, 0.0f);
    settings.vignette_color = toVec4(p.vignette_color, 0.0f);
    settings.bloom_tint = toVec4(p.bloom_tint, 0.0f);
    settings.lens = Vec4{std::clamp(p.lens_distortion, -1.0f, 1.0f), std::max(p.lens_flare, 0.0f), sun_screen_uv_.x,
                         sun_screen_uv_.y};
    // Vision nocturna: con exposicion manual, la adaptacion del ojo sale de la
    // luz del dia (la auto-exposicion la mide en la GPU).
    settings.flare = Vec4{sun_screen_weight_,
                          static_cast<float>(screen.width) / static_cast<float>(std::max(screen.height, 1u)),
                          std::clamp(p.night_vision, 0.0f, 1.0f), 0.01f + 0.3f * last_daylight_};
    composite_buffers_[frame_index].write(&settings, sizeof(settings));

    drawFullscreen<GpuBloomPush>(cmd, composite_pass_, &composite_sets_[frame_index], ldr_color_,
                                 nullptr);

    pipelineBarrier(cmd, writtenToSampled(*ldr_color_.handle()));

    // Copia de la exposicion para el titulo de la ventana.
    vk::BufferCopy region{};
    region.size = sizeof(GpuExposureState);
    cmd.copyBuffer(*exposure_buffer_.handle(), *exposure_readback_[frame_index].handle(), region);
    memoryBarrier(cmd, vk::PipelineStageFlagBits2::eTransfer,
                  vk::AccessFlagBits2::eTransferWrite, vk::PipelineStageFlagBits2::eHost,
                  vk::AccessFlagBits2::eHostRead);
}

void VulkanRenderer::recordPostProcessPass(const vk::raii::CommandBuffer& cmd,
                                           std::uint32_t image_index) {
    const vk::Extent2D extent = swapchain_.extent();

    // --- La imagen de la swapchain pasa a destino ---
    // (la imagen compuesta ya quedo como textura en recordCompositePass).
    pipelineBarrier(cmd, colorBarrier(swapchain_.images()[image_index],
                                      vk::ImageLayout::eUndefined,
                                      vk::ImageLayout::eColorAttachmentOptimal,
                                      vk::PipelineStageFlagBits2::eTopOfPipe,
                                      vk::AccessFlagBits2::eNone,
                                      vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                      vk::AccessFlagBits2::eColorAttachmentWrite));

    vk::RenderingAttachmentInfo color_attachment{};
    color_attachment.imageView = *swapchain_.imageViews()[image_index];
    color_attachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
    color_attachment.loadOp = vk::AttachmentLoadOp::eDontCare;
    color_attachment.storeOp = vk::AttachmentStoreOp::eStore;

    vk::RenderingInfo rendering_info{};
    rendering_info.renderArea = vk::Rect2D{vk::Offset2D{0, 0}, extent};
    rendering_info.layerCount = 1;
    rendering_info.setColorAttachments(color_attachment);

    compat::beginRendering(cmd, rendering_info);

    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *post_process_pass_.pipeline());
    cmd.setViewport(0, vk::Viewport{0.0f, 0.0f, static_cast<float>(extent.width),
                                    static_cast<float>(extent.height), 0.0f, 1.0f});
    cmd.setScissor(0, vk::Rect2D{vk::Offset2D{0, 0}, extent});
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *post_process_pass_.layout(), 0,
                           *post_process_sets_[current_frame_], nullptr);

    GpuPostProcessPush push{};
    // (FXAA mira pixeles de la imagen de origen: la de salida de la escena.)
    const vk::Extent2D source = ldr_color_.extent();
    push.inverse_resolution = core::Vec2{1.0f / static_cast<float>(std::max(source.width, 1u)),
                                         1.0f / static_cast<float>(std::max(source.height, 1u))};
    push.enabled = post_.fxaa ? 1.0f : 0.0f;
    cmd.pushConstants<GpuPostProcessPush>(*post_process_pass_.layout(),
                                          vk::ShaderStageFlagBits::eFragment, 0, push);

    cmd.draw(3, 1, 0, 0);

    // Lo que dibuje la herramienta (el editor) va encima, en el mismo pase.
    if (overlay_) {
        overlay_(*cmd);
    }

    compat::endRendering(cmd);

    // --- La imagen queda lista para presentarse ---
    vk::ImageMemoryBarrier2 present_barrier = colorBarrier(
        swapchain_.images()[image_index], vk::ImageLayout::eColorAttachmentOptimal,
        vk::ImageLayout::ePresentSrcKHR, vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        vk::AccessFlagBits2::eColorAttachmentWrite, vk::PipelineStageFlagBits2::eBottomOfPipe,
        vk::AccessFlagBits2::eNone);

    vk::DependencyInfo present_dependency{};
    present_dependency.setImageMemoryBarriers(present_barrier);
    compat::pipelineBarrier(cmd, present_dependency);
}

bool VulkanRenderer::applyPendingResize() {
    // VR: con el casco en marcha la escena va a la resolucion de su ojo (la
    // de la ventana, estirada, se veia borrosa); al soltarlo, la de la vista.
    if (initialized_) {
        const VkExtent2D eye = xr_.running() ? xr_.eyeExtent() : VkExtent2D{0, 0};
        const vk::Extent2D wanted{std::min(eye.width, 8192u), std::min(eye.height, 8192u)};
        if (wanted != xr_output_extent_) {
            xr_output_extent_ = wanted;
            targets_dirty_ = true;
        }
        // El casco tiene que ir a su frecuencia (72-120 Hz): si no llega, el
        // runtime repite frames y al mover la cabeza todo tiembla. En VR el
        // presupuesto adaptativo va siempre, con los Hz del casco (se mide el
        // frame entero: los dos ojos).
        if (xr_.running()) {
            budget_.setEnabled(true);
            budget_.setTargetFps(xr_.displayHz());
            xr_budget_applied_ = true;
            applyEffectiveGraphics();  // estereo: TAA por ojo en vez de FSR 3 / DLSS
        } else if (xr_budget_applied_) {
            xr_budget_applied_ = false;
            budget_.setEnabled(user_graphics_.adaptive);
            budget_.setTargetFps(user_graphics_.target_fps);
            applyEffectiveGraphics();
        }
    }
    if (!initialized_ || (!framebuffer_resized_ && !settings_dirty_ && !targets_dirty_ && swapchain_.isValid())) {
        return false;
    }
    if (!framebuffer_resized_ && !settings_dirty_ && swapchain_.isValid()) {
        recreateRenderTargets();  // solo otro tamano de vista
        return true;
    }
    settings_dirty_ = false;
    targets_dirty_ = false;
    recreateSwapchain();
    return true;
}

VulkanRenderer::NativeHandles VulkanRenderer::nativeHandles() const {
    NativeHandles handles{};
    handles.instance = *instance_.handle();
    handles.physical_device = *device_.physicalDevice();
    handles.device = *device_.handle();
    handles.queue_family = device_.queueFamilies().graphics;
    handles.queue = *device_.graphicsQueue();
    handles.swapchain_format = static_cast<VkFormat>(swapchain_.imageFormat());
    handles.image_count = swapchain_.imageCount();
    handles.api_version = instance_.apiVersion();
    return handles;
}

void VulkanRenderer::waitIdle() const {
    if (initialized_) {
        device_.waitIdle();
    }
}

bool VulkanRenderer::readSceneImage(asset::ImageRgba8& image) {
    image = asset::ImageRgba8{};
    if (!initialized_ || !ldr_color_.isValid()) return false;
    device_.waitIdle();
    const vk::Extent2D extent = ldr_color_.extent();
    const vk::DeviceSize size = static_cast<vk::DeviceSize>(extent.width) * extent.height * 4;
    VulkanBuffer staging;
    staging.create(device_, size, vk::BufferUsageFlagBits::eTransferDst,
                   vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
    constexpr vk::ImageLayout kRead = vk::ImageLayout::eShaderReadOnlyOptimal;
    device_.submitOneTime([&](const vk::raii::CommandBuffer& cmd) {
        pipelineBarrier(cmd, colorBarrier(*ldr_color_.handle(), kRead, vk::ImageLayout::eTransferSrcOptimal,
                                          vk::PipelineStageFlagBits2::eAllCommands, vk::AccessFlagBits2::eMemoryWrite,
                                          vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferRead));
        vk::BufferImageCopy region{};
        region.imageSubresource = vk::ImageSubresourceLayers{vk::ImageAspectFlagBits::eColor, 0, 0, 1};
        region.imageExtent = vk::Extent3D{extent.width, extent.height, 1};
        cmd.copyImageToBuffer(*ldr_color_.handle(), vk::ImageLayout::eTransferSrcOptimal, *staging.handle(), region);
        pipelineBarrier(cmd, colorBarrier(*ldr_color_.handle(), vk::ImageLayout::eTransferSrcOptimal, kRead,
                                          vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferRead,
                                          vk::PipelineStageFlagBits2::eFragmentShader,
                                          vk::AccessFlagBits2::eShaderSampledRead));
    });
    const auto* pixels = static_cast<const std::uint8_t*>(staging.mapped());
    if (pixels == nullptr) return false;
    image.width = extent.width;
    image.height = extent.height;
    image.pixels.assign(pixels, pixels + size);  // kLdrFormat ya es RGBA8
    for (std::size_t i = 3; i < image.pixels.size(); i += 4) image.pixels[i] = 255;
    staging.destroy();
    return true;
}

}  // namespace cramion::gfx
