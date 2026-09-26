#include "CramionFX/vk/VulkanRenderer.h"

#include "CramionFX/scene/Scene.h"
#include "CramionFX/vk/GpuTypes.h"

#include <stb_image.h>

#include <algorithm>
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
    cmd.pipelineBarrier2(dependency);
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

    cmd.beginRendering(rendering_info);
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
    cmd.endRendering();
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
    cmd.pipelineBarrier2(dependency);
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
constexpr float kCloudCoverage = 0.38f;
constexpr float kCloudDensity = 1.0f;

// Fraccion final de cada cascada en la que se mezcla con la siguiente, para
// que el salto de resolucion entre cascadas no se vea como una linea.
constexpr float kCascadeBlendBand = 0.12f;

// Barrera de layout para un rango de capas de un array de profundidad.
vk::ImageMemoryBarrier2 depthLayersBarrier(vk::Image image, std::uint32_t base_layer,
                                           std::uint32_t layer_count, vk::ImageLayout old_layout,
                                           vk::ImageLayout new_layout) {
    vk::ImageMemoryBarrier2 barrier{};
    if (new_layout == vk::ImageLayout::eDepthAttachmentOptimal) {
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

void VulkanRenderer::initialize(const EngineInfo& info, HWND window, std::uint32_t width,
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
    instance_.initialize(info);
    surface_.initialize(instance_, window);
    device_.initialize(instance_, surface_);

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
    gpu_culling_.create(device_);
    gpu_profiler_.create(device_, kMaxFramesInFlight);
    createRenderTargets();
    shadow_map_.create(device_);
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
        // y las sombras de focos y puntuales + sus matrices.
        const std::array<Type, 8> volumetric_bindings = {
            Type::eUniformBuffer,        Type::eUniformBuffer,        Type::eUniformBuffer,
            Type::eCombinedImageSampler, Type::eCombinedImageSampler, Type::eCombinedImageSampler,
            Type::eCombinedImageSampler, Type::eUniformBuffer};
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
        // Particulas: sobre la imagen HDR, antes del bloom.
        particle_pass_.create(device_, kHdrFormat, gbuffer_.depthFormat(), kMaxFramesInFlight);

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

        FullscreenPassDesc shafts{};
        shafts.fragment_shader = "light_shafts.frag.spv";
        shafts.bindings = two_textures;
        shafts.push_constant_size = sizeof(GpuLightShaftPush);
        shafts.color_format = kHdrFormat;
        light_shaft_pass_.create(device_, shafts);

        const std::array<Type, 4> taa_bindings = {Type::eCombinedImageSampler, Type::eCombinedImageSampler,
                                                  Type::eCombinedImageSampler, Type::eCombinedImageSampler};
        FullscreenPassDesc taa{};
        taa.fragment_shader = "taa.frag.spv";
        taa.bindings = taa_bindings;
        taa.push_constant_size = sizeof(GpuTaaPush);
        taa.color_format = kHdrFormat;
        taa_pass_.create(device_, taa);

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
            barrier.newLayout = vk::ImageLayout::eDepthReadOnlyOptimal;
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

    device_.waitIdle();

    skinned_models_.clear();
    retired_models_.clear();
    ray_pinned_models_.clear();
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
    taa_sets_.clear();
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
    taa_pass_.destroy();
    easu_pass_.destroy();
    rcas_pass_.destroy();
    clouds_pass_.destroy();
    gi_atrous_pass_.destroy();
    gi_temporal_pass_.destroy();
    ssr_resolve_pass_.destroy();
    ssr_pass_.destroy();
    ssgi_pass_.destroy();
    sky_lut_pass_.destroy();
    composite_pass_.destroy();
    outline_pass_.destroy();
    overlay_pass_.destroy();
    particle_pass_.destroy();
    bloom_up_pass_.destroy();
    bloom_down_pass_.destroy();
    ssao_pass_.destroy();
    volumetric_pass_.destroy();
    terrain_pass_.destroy();
    voxel_pass_.destroy();
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
    gi_image_.destroy();
    gi_raw_.destroy();
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
    ray_tracing_.destroy();
    environment_.destroy();
    clouds_image_.destroy();
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
    upscale_target_.destroy();
    upscaled_color_.destroy();
    taa_history_.destroy();
    output_depth_.destroy();
    scene_color_.destroy();
    gbuffer_.destroy();

    swapchain_.shutdown();
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
    for (std::uint32_t slot = 0; slot < kMaxDecalTextures; ++slot) {
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
    std::uint32_t slot = 0;
    while (slot < kMaxDecalTextures && !decal_texture_paths_[slot].empty()) ++slot;
    if (slot == kMaxDecalTextures) {
        std::cerr << "[Vulkan] Sin ranuras para mas texturas de decal (maximo " << kMaxDecalTextures << ")\n";
        return -1;
    }
    int width = 0;
    int height = 0;
    int channels = 0;
    FILE* handle = nullptr;
    if (_wfopen_s(&handle, file.wstring().c_str(), L"rb") != 0) handle = nullptr;
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
    // (+ la luz volumetrica.)
    pool_sizes[1].descriptorCount = kMaxFramesInFlight * (GBuffer::kColorAttachmentCount + 16);

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
                               kMaxFramesInFlight * (1 + kMaxDecalTextures)}};

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
        rain_info.imageLayout = vk::ImageLayout::eDepthReadOnlyOptimal;
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
    }
    writeDecalTextureDescriptors();

    // --- Post-proceso: SSAO (por frame), bloom (por nivel) y composicion ---
    constexpr std::uint32_t kBloomSets = kBloomLevels + (kBloomLevels - 1);
    // Texturas: SSAO (2 por frame), bloom (1 por set), composicion (3), rayos
    // de luz (2) e histograma (1). Storage: histograma (1), promedio (2) y
    // composicion (1).
    // Y la GI, el SSR y su filtro temporal: camara + 3 texturas por frame
    // cada uno.
    const std::array<vk::DescriptorPoolSize, 3> post_sizes = {
    // Y las nubes: camara + 2 texturas por frame. Y el filtro de la GI:
    // camara + 3 texturas por frame.
        // (+ la luz volumetrica: 4 buffers y 4 texturas por frame.)
        // (+ la composicion por frame: 3 texturas, su exposicion y sus ajustes.)
        vk::DescriptorPoolSize{vk::DescriptorType::eUniformBuffer, kMaxFramesInFlight * 11},
        vk::DescriptorPoolSize{vk::DescriptorType::eCombinedImageSampler,
                               kMaxFramesInFlight * 25 + kBloomSets + 2 + 1 + 1 + 6},
        vk::DescriptorPoolSize{vk::DescriptorType::eStorageBuffer, 3 + kMaxFramesInFlight}};

    vk::DescriptorPoolCreateInfo post_pool_info{};
    post_pool_info.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    post_pool_info.maxSets = kMaxFramesInFlight * 8 + kBloomSets + 5 + 3;
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
    taa_sets_ = allocate(taa_pass_, 1);
    easu_sets_ = allocate(easu_pass_, 1);
    rcas_sets_ = allocate(rcas_pass_, 1);
    outline_sets_ = allocate(outline_pass_, 1);
    ssgi_sets_ = allocate(ssgi_pass_, kMaxFramesInFlight);
    ssr_sets_ = allocate(ssr_pass_, kMaxFramesInFlight);
    ssr_resolve_sets_ = allocate(ssr_resolve_pass_, kMaxFramesInFlight);
    clouds_sets_ = allocate(clouds_pass_, kMaxFramesInFlight);

    // --- Filtro de la GI ---
    {
        constexpr std::uint32_t kTemporalSets = kMaxFramesInFlight;
        constexpr std::uint32_t kAtrousSets = kMaxFramesInFlight * 2 * kGiAtrousIterations;
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
            ActorMotion{actor_draws_[i].transform, actor_draws_[i].bounds_center,
                        actor_draws_[i].bounds_radius, actor_draws_[i].cast_shadows, actor_draws_[i].lod};
    }
    const std::size_t previous_count = actor_draws_.size();
    moved_spheres_.clear();

    actor_draws_.clear();
    bone_staging_.clear();
    submesh_bounds_.clear();
    lod_bounds_.clear();
    lod_triangles_ = 0;
    lod_actors_ = 0;
    gpu_clusters_.clear();

    // Para los LODs: pixeles de pantalla por unidad del mundo a distancia 1
    // (la proyeccion de Vulkan lleva la Y invertida en m[1][1]).
    const core::Vec3 camera_position = scene.camera().position();
    const float pixels_per_unit = std::abs(scene.camera().projection().m[1][1]) * 0.5f *
                                  static_cast<float>(std::max(sceneExtent().height, 1u));
    draw_batches_.clear();
    batch_lookup_.clear();

    for (const scene::Actor& actor : scene.actors()) {
        if (actor.model >= skinned_models_.size()) {
            continue;  // Modelo sin subir a la GPU.
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

            // LOD: el mismo para la camara y las sombras.
            draw.lod = chooseLod(model, to_world, draw.bounds_center, draw.bounds_radius, camera_position,
                                 pixels_per_unit);
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
                if (group == SkinnedModel::kNoGroup || draw.shadows_only) {
                    continue;  // Transparente o solo sombras: la camara no lo ve.
                }
                const asset::SubMesh& submesh = lod_submeshes[i];
                const core::Aabb& box = draw.lod > 0 ? lod_bounds_[draw.first_lod_bounds + i]
                                                     : submesh_bounds_[draw.first_bounds + i];
                GpuCluster cluster{};
                cluster.bounds_min = toVec4(box.min, 0.0f);
                cluster.bounds_max = toVec4(box.max, 0.0f);
                cluster.first_index = submesh.first_index;
                cluster.index_count = submesh.index_count;
                const std::uint64_t key = (static_cast<std::uint64_t>(actor.model) << 32) | group;
                const auto [it, inserted] =
                    batch_lookup_.try_emplace(key, static_cast<std::uint32_t>(draw_batches_.size()));
                if (inserted) {
                    draw_batches_.push_back(DrawBatch{actor.model, group, 0, 0});
                }
                ++draw_batches_[it->second].capacity;
                cluster.group = it->second;
                cluster.instance = instance;
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
            // Otro LOD tambien cambia la sombra guardada (poco, pero se nota).
            if (animated || before.cast_shadows != draw.cast_shadows || before.lod != draw.lod ||
                std::memcmp(&before.transform, &draw.transform, sizeof(core::Mat4)) != 0) {
                moved_spheres_.push_back(toVec4(before.center, before.radius));
                moved_spheres_.push_back(toVec4(draw.bounds_center, draw.bounds_radius));
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

    gpu_culling_.setClusters(device_, frame_index, gpu_clusters_, group_count, slot_count,
                             camera_buffers_);

    // Los escenarios (rigidos) son las instancias de la TLAS de los rayos.
    if (device_.rayTracingSupported()) {
        std::vector<RayTracing::Instance> instances;
        for (const ActorDraw& draw : actor_draws_) {
            if (draw.per_submesh) {
                instances.push_back(RayTracing::Instance{
                    draw.model, draw.transform * bone_staging_[draw.bone_offset]});
            }
        }
        ray_tracing_.setInstances(device_, instances);
    }

    // Actores anadidos o quitados: las cascadas guardadas no valen.
    actor_set_changed_ = actor_draws_.size() != previous_count;
    for (std::size_t i = 0; !actor_set_changed_ && i < actor_draws_.size(); ++i) {
        // Quien proyecta sombra tambien cambia las cascadas guardadas.
        actor_set_changed_ = previous_actor_motion_[i].cast_shadows != actor_draws_[i].cast_shadows;
    }

    if (bone_staging_.empty()) {
        return;
    }

    ensureBoneCapacity(frame_index, bone_staging_.size());
    bone_buffers_[frame_index].write(bone_staging_.data(),
                                     sizeof(core::Mat4) * bone_staging_.size());
}

bool VulkanRenderer::actorsTouch(const Vec3& light_position, float range) const {
    for (const ActorDraw& draw : actor_draws_) {
        const float reach = range + draw.bounds_radius;
        const Vec3 offset = draw.bounds_center - light_position;
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

void VulkanRenderer::recordActorShadows(const vk::raii::CommandBuffer& cmd,
                                        std::uint32_t frame_index,
                                        const core::Mat4& light_view_projection,
                                        const Vec3& light_position, float range) {
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
        if (local) {
            const float reach = range + draw.bounds_radius;
            const Vec3 offset = draw.bounds_center - light_position;
            if (core::dot(offset, offset) > reach * reach) {
                continue;
            }
        }

        // Submallas que tocan el volumen de la luz (una sola prueba).
        visible.clear();
        shadow_submeshes_ += forEachVisibleSubmesh(
            draw, frustum, [&](std::uint32_t i) { visible.push_back(i); });
        if (visible.empty()) {
            continue;
        }

        const SkinnedModel& model = skinned_models_[draw.model];
        bind(bound_pipeline ? bound_pipeline : opaque_pipeline);

        GpuSkinnedShadowPush push{};
        push.light_model_view_projection = light_view_projection * draw.transform;
        push.bone_offset = draw.bone_offset;
        cmd.pushConstants<GpuSkinnedShadowPush>(*skinned_pass_.shadowLayout(),
                                                vk::ShaderStageFlagBits::eVertex, 0, push);

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
                const asset::SubMesh& submesh = model.lodSubmeshes(draw.lod)[i];
                if (model.materials()[submesh.material].alpha_masked != (masked != 0)) {
                    continue;
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
        shadow_map_info.imageLayout = vk::ImageLayout::eDepthReadOnlyOptimal;

        // Las sombras locales reutilizan el muestreador de comparacion de las
        // cascadas: mismo filtrado y mismo borde "iluminado".
        vk::DescriptorImageInfo spot_shadow_info{};
        spot_shadow_info.sampler = *shadow_map_.sampler();
        spot_shadow_info.imageView = *local_shadow_maps_.spotImage().view();
        spot_shadow_info.imageLayout = vk::ImageLayout::eDepthReadOnlyOptimal;

        vk::DescriptorImageInfo point_shadow_info{};
        point_shadow_info.sampler = *shadow_map_.sampler();
        point_shadow_info.imageView = *local_shadow_maps_.pointImage().view();
        point_shadow_info.imageLayout = vk::ImageLayout::eDepthReadOnlyOptimal;

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
        image_infos[2].imageLayout = vk::ImageLayout::eDepthReadOnlyOptimal;

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

        std::array<vk::WriteDescriptorSet, 23> writes{};

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

        device_.handle().updateDescriptorSets(writes, nullptr);
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
                                            vk::ImageLayout::eDepthReadOnlyOptimal};
        images[1] = vk::DescriptorImageInfo{*lighting_pass_.sampler(), *gbuffer_.depth().view(),
                                            vk::ImageLayout::eDepthReadOnlyOptimal};
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
        device_.handle().updateDescriptorSets(writes, nullptr);
    }
}

void VulkanRenderer::updatePostDescriptors() {
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
                      vk::ImageLayout::eDepthReadOnlyOptimal);
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
                          vk::ImageLayout::eDepthReadOnlyOptimal);
            write_texture(volumetric_sets_[i], 4, volumetric_pass_.sampler(), gbuffer_.depth(),
                          vk::ImageLayout::eDepthReadOnlyOptimal);

            // Sombras de las luces locales (el mismo muestreador de
            // comparacion que las cascadas, como en la iluminacion).
            write_texture(volumetric_sets_[i], 5, shadow_map_.sampler(),
                          local_shadow_maps_.spotImage(), vk::ImageLayout::eDepthReadOnlyOptimal);
            write_texture(volumetric_sets_[i], 6, shadow_map_.sampler(),
                          local_shadow_maps_.pointImage(), vk::ImageLayout::eDepthReadOnlyOptimal);
            vk::DescriptorBufferInfo local_info{*local_shadow_buffers_[i].handle(), 0,
                                                sizeof(GpuLocalShadows)};
            vk::WriteDescriptorSet local_write{};
            local_write.dstSet = *volumetric_sets_[i];
            local_write.dstBinding = 7;
            local_write.descriptorType = vk::DescriptorType::eUniformBuffer;
            local_write.setBufferInfo(local_info);
            device_.handle().updateDescriptorSets(local_write, nullptr);
        }

        // SSGI: camara + profundidad + normales + imagen del frame anterior.
        vk::WriteDescriptorSet ssgi_camera = write;
        ssgi_camera.dstSet = *ssgi_sets_[i];
        device_.handle().updateDescriptorSets(ssgi_camera, nullptr);
        write_texture(ssgi_sets_[i], 1, ssgi_pass_.sampler(), gbuffer_.depth(),
                      vk::ImageLayout::eDepthReadOnlyOptimal);
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
                      vk::ImageLayout::eDepthReadOnlyOptimal);
        write_texture(ssr_sets_[i], 2, ssr_pass_.sampler(), gbuffer_.normal(), kRead);
        write_texture(ssr_sets_[i], 3, ssr_pass_.sampler(), scene_color_, kRead);

        // Filtro temporal del SSR.
        vk::WriteDescriptorSet resolve_camera = write;
        resolve_camera.dstSet = *ssr_resolve_sets_[i];
        device_.handle().updateDescriptorSets(resolve_camera, nullptr);
        write_texture(ssr_resolve_sets_[i], 1, ssr_resolve_pass_.sampler(), gbuffer_.depth(),
                      vk::ImageLayout::eDepthReadOnlyOptimal);
        write_texture(ssr_resolve_sets_[i], 2, ssr_resolve_pass_.sampler(), ssr_raw_, kRead);
        write_texture(ssr_resolve_sets_[i], 3, ssr_resolve_pass_.sampler(), ssr_history_, kRead);

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

        // Temporal: raw (este frame) + historia -> acumulada, momentos, varianza.
        const vk::raii::DescriptorSet& temporal_set = gi_temporal_sets_[i];
        vk::WriteDescriptorSet temporal_camera = write;
        temporal_camera.dstSet = *temporal_set;
        device_.handle().updateDescriptorSets(temporal_camera, nullptr);
        const vk::raii::Sampler& gi_sampler = gi_temporal_pass_.sampler();
        write_texture(temporal_set, 1, gi_sampler, gbuffer_.depth(),
                      vk::ImageLayout::eDepthReadOnlyOptimal);
        write_texture(temporal_set, 2, gi_sampler, gi_raw_, kRead);
        write_texture(temporal_set, 3, gi_sampler, gi_history_, kGeneral);
        write_texture(temporal_set, 4, gi_sampler, gi_moments_history_, kGeneral);
        write_storage_image(temporal_set, 5, gi_temporal_);
        write_storage_image(temporal_set, 6, gi_moments_);
        write_storage_image(temporal_set, 7, gi_variance_);

        // A trous: la primera pasada es la historia del color (como en SVGF),
        // salvo en las caras de la sonda, que no deben tocarla.
        for (std::uint32_t capture = 0; capture < 2; ++capture) {
            // Entrada de cada pasada = salida de la anterior: acumulada ->
            // primera salida -> ping-pong entre gi_filter_[1] y [0] -> resultado.
            const VulkanImage& first_output = capture ? gi_filter_[0] : gi_history_;
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
                    gi_atrous_sets_[(i * 2 + capture) * kGiAtrousIterations + iteration];
                vk::WriteDescriptorSet atrous_camera = write;
                atrous_camera.dstSet = *set;
                device_.handle().updateDescriptorSets(atrous_camera, nullptr);
                write_texture(set, 1, gi_sampler, gbuffer_.depth(),
                              vk::ImageLayout::eDepthReadOnlyOptimal);
                write_texture(set, 2, gi_sampler, gbuffer_.normal(), kRead);
                write_texture(set, 3, gi_sampler, *colors[iteration], kGeneral);
                write_texture(set, 4, gi_sampler, *variances[iteration], kGeneral);
                write_storage_image(set, 5, *colors[iteration + 1]);
                write_storage_image(set, 6, *variances[iteration + 1]);
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
                  vk::ImageLayout::eDepthReadOnlyOptimal);
    write_texture(light_shaft_sets_[0], 1, light_shaft_pass_.sampler(), postSource(), kRead);

    write_texture(histogram_sets_[0], 0, histogram_pass_.sampler(), postSource(), kRead);

    // Escalado: la escena (interna), su profundidad y movimiento y la historia.
    write_texture(taa_sets_[0], 0, taa_pass_.sampler(), scene_color_, kRead);
    write_texture(taa_sets_[0], 1, taa_pass_.sampler(), gbuffer_.depth(), vk::ImageLayout::eDepthReadOnlyOptimal);
    write_texture(taa_sets_[0], 2, taa_pass_.sampler(), gbuffer_.velocity(), kRead);
    write_texture(taa_sets_[0], 3, taa_pass_.sampler(), taa_history_, kRead);
    write_texture(easu_sets_[0], 0, easu_pass_.sampler(), scene_color_, kRead);
    write_texture(rcas_sets_[0], 0, rcas_pass_.sampler(), upscale_target_, kRead);
    write_storage(histogram_sets_[0], 1, histogram_buffer_);

    write_storage(exposure_average_sets_[0], 0, histogram_buffer_);
    write_storage(exposure_average_sets_[0], 1, exposure_buffer_);
}

void VulkanRenderer::computeRenderExtent() {
    const vk::Extent2D output = swapchain_.extent();
    const float scale = renderScale(graphics_);
    render_extent_ = vk::Extent2D{
        std::max(1u, static_cast<std::uint32_t>(std::lround(static_cast<float>(output.width) * scale))),
        std::max(1u, static_cast<std::uint32_t>(std::lround(static_cast<float>(output.height) * scale)))};
    upscaling_ = graphics_.upscaler != Upscaler::Off;
}

void VulkanRenderer::setGraphicsSettings(const GraphicsSettings& settings) {
    if (settings == graphics_) return;
    const bool rebuild = renderScale(settings) != renderScale(graphics_) || settings.vsync != graphics_.vsync ||
                         (settings.upscaler == Upscaler::Off) != (graphics_.upscaler == Upscaler::Off);
    graphics_ = settings;
    swapchain_.setVsync(settings.vsync);
    taa_history_valid_ = false;
    jitter_index_ = 0;
    // Destinos de otro tamano (o el post-proceso lee otra imagen): se rehacen
    // al empezar el frame siguiente (applyPendingResize).
    if (rebuild) settings_dirty_ = true;
}

void VulkanRenderer::createRenderTargets() {
    // La escena se dibuja a la resolucion interna; lo que va despues del
    // escalado (bloom, composicion, gizmos, vistas), a la de pantalla.
    const vk::Extent2D extent = render_extent_;
    const vk::Extent2D output = swapchain_.extent();
    const vk::ImageUsageFlags target_usage =
        vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled;

    // (Origen de copia: el vidrio lee una copia sin si mismo, glass_source_.)
    scene_color_.create(device_, extent, kHdrFormat,
                        target_usage | vk::ImageUsageFlagBits::eTransferSrc,
                        vk::ImageAspectFlagBits::eColor);
    glass_source_.create(device_, extent, kHdrFormat,
                         vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
                         vk::ImageAspectFlagBits::eColor);
    // Que tenga un layout valido aunque el primer frame no tenga vidrio.
    device_.submitOneTime([&](const vk::raii::CommandBuffer& cmd) {
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

    // Los reflejos del suelo tienen que ser nitidos: resolucion completa.
    // (Storage: con trazado de rayos las escribe un compute shader.)
    ssr_raw_.create(device_, extent, kHdrFormat,
                    target_usage | vk::ImageUsageFlagBits::eStorage,
                    vk::ImageAspectFlagBits::eColor);
    ssr_image_.create(device_, extent, kHdrFormat,
                      target_usage | vk::ImageUsageFlagBits::eTransferSrc,
                      vk::ImageAspectFlagBits::eColor);
    ssr_history_.create(device_, extent, kHdrFormat,
                        vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
                        vk::ImageAspectFlagBits::eColor);
    ssr_filter_history_valid_ = false;

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
    upscale_target_.create(device_, output, kHdrFormat, target_usage | vk::ImageUsageFlagBits::eTransferSrc,
                           vk::ImageAspectFlagBits::eColor);
    upscaled_color_.create(device_, output, kHdrFormat, target_usage, vk::ImageAspectFlagBits::eColor);
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

void VulkanRenderer::uploadModels(const scene::Scene& scene) {
    if (!initialized_) {
        throw std::runtime_error("uploadModels() antes de inicializar el renderizador.");
    }
    const auto upload_start = std::chrono::steady_clock::now();
    ++frame_timings_.uploads;

    device_.waitIdle();
    skinned_models_.clear();
    retired_models_.clear();
    ray_pinned_models_.clear();
    skinned_models_.reserve(scene.models().size());

    // Lo que dependia de la escena anterior se rehace: el mapa de lluvia
    // (se dibuja una vez), las cascadas guardadas y las sombras locales.
    rain_map_ready_ = false;
    cascades_valid_ = false;
    local_shadows_.invalidate();

    triangle_count_ = 0;
    std::vector<const asset::ModelData*> models;
    for (const auto& model : scene.models()) {
        skinned_models_.emplace_back().create(device_, *model, skinned_pass_);
        triangle_count_ += model->indices.size() / 3;
        models.push_back(model.get());
    }

    // La misma escena para los rayos (estructuras de aceleracion).
    if (device_.rayTracingSupported()) {
        ray_tracing_.build(device_, models, skinned_models_, ibl_probe_.irradianceBuffer());
    }
    ray_traced_models_ = static_cast<std::uint32_t>(skinned_models_.size());
    ray_pinned_.assign(skinned_models_.size(), false);
    frame_timings_.upload_ms +=
        std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - upload_start).count();
}

void VulkanRenderer::uploadModel(const scene::Scene& scene, std::uint32_t index) {
    if (!initialized_ || index >= scene.models().size()) return;
    // Faltan otros antes que el, o lo subido es de otra escena (se vacio y
    // se empezo otra): todo de nuevo.
    if (index > skinned_models_.size() || skinned_models_.size() > scene.models().size()) {
        uploadModels(scene);
        return;
    }
    const auto upload_start = std::chrono::steady_clock::now();
    ++frame_timings_.uploads;
    SkinnedModel fresh;
    fresh.create(device_, *scene.models()[index], skinned_pass_);
    if (index == skinned_models_.size()) {
        skinned_models_.push_back(std::move(fresh));
    } else {
        if (index < ray_traced_models_ && index < ray_pinned_.size() && !ray_pinned_[index]) {
            ray_pinned_[index] = true;
            ray_pinned_models_.push_back(std::move(skinned_models_[index]));
        } else {
            retired_models_.push_back(RetiredModel{std::move(skinned_models_[index]), kMaxFramesInFlight + 1});
        }
        skinned_models_[index] = std::move(fresh);
    }
    triangle_count_ = 0;
    for (const auto& model : scene.models()) triangle_count_ += model->indices.size() / 3;
    staticGeometryChanged();  // sombras cacheadas
    frame_timings_.upload_ms +=
        std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - upload_start).count();
}

bool VulkanRenderer::loadEnvironment(const std::filesystem::path& path) {
    device_.waitIdle();
    if (!environment_.load(device_, path)) {
        return false;
    }
    ibl_probe_.setEnvironment(device_, *environment_.view(), *environment_.sampler());
    updateLightingDescriptors();
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

void VulkanRenderer::recreateSwapchain() {
    framebuffer_resized_ = false;

    if (!swapchain_.recreate(window_width_, window_height_)) {
        return;  // Ventana minimizada.
    }

    // El G-buffer tiene el tamano de la swapchain: se rehace con ella, y los
    // descriptores de la pasada de iluminacion apuntan a las nuevas vistas.
    computeRenderExtent();
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

    // Jitter del TAA / escalado temporal: una fraccion de pixel distinta cada
    // frame (Halton 2,3), con mas fases cuanto mas se escala (como DLSS/FSR).
    const bool temporal = upscaling_ && graphics_.upscaler != Upscaler::Fsr1 && !isolated();
    Vec2 jitter_pixels{};
    if (temporal && render_extent_.width > 0) {
        const float ratio = static_cast<float>(swapchain_.extent().width) / static_cast<float>(render_extent_.width);
        const auto phases = std::clamp(static_cast<std::uint32_t>(8.0f * ratio * ratio), 8u, 64u);
        jitter_index_ = jitter_index_ % phases + 1;
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
    camera_data.jitter = Vec4{jitter_ndc.x, jitter_ndc.y, 0.0f, 0.0f};
    camera_data.motion[0] = motion_offset_;
    if (!isolated()) {
        jitter_ndc_ = jitter_ndc;
        taa_reproject_ = motion_view_projection_ * core::inverse(unjittered);
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

    camera_buffers_[frame_index].write(&camera_data, sizeof(camera_data));

    const scene::LightSet& lights = scene.lights();

    // --- Sombras de las luces locales ---
    // Va antes que las luces porque decide que hueco de sombra tiene cada una.
    // Con las sombras apagadas no se toca la cache: al volver a encenderlas
    // solo se redibuja lo que haya cambiado entretanto. Las caras de la sonda
    // usan los mapas tal como estan: su camara no debe decidir los huecos.
    if (shadows_enabled_ && !isolated()) {
        local_shadows_.update(camera, lights, LocalShadowMaps::kSpotResolution,
                              LocalShadowMaps::kPointResolution);
    }

    GpuLights light_data{};
    light_data.sun_direction_intensity =
        toVec4(core::normalize(lights.sun.direction), lights.sun.intensity);
    light_data.sun_color_ambient = toVec4(lights.sun.color, lights.ambient.intensity);
    light_data.ambient_color = toVec4(lights.ambient.color, 0.0f);
    light_data.sky_sun = toVec4(lights.sky.to_sun, lights.sky.daylight);
    light_data.sky_moon = toVec4(lights.sky.to_moon, lights.sky.twilight);

    const auto point_count = static_cast<std::int32_t>(
        std::min<std::size_t>(lights.points.size(), scene::kMaxPointLights));
    for (std::int32_t i = 0; i < point_count; ++i) {
        const scene::PointLight& light = lights.points[static_cast<std::size_t>(i)];
        light_data.points[i].position_range = toVec4(light.position, light.range);
        light_data.points[i].color_intensity = toVec4(light.color, light.intensity);
        light_data.points[i].shadow = Vec4{
            static_cast<float>(local_shadows_.pointSlot(static_cast<std::size_t>(i))), 0.0f,
            0.0f, 0.0f};
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
                                static_cast<float>(local_shadows_.spotSlot(index)), 0.0f, 0.0f};
        ++spot_count;
    }
    light_data.spot_count = spot_count;
    light_data.ssao_enabled = post_.ambient_occlusion ? 1 : 0;
    light_data.gi_enabled = post_.global_illumination ? 1 : 0;
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
    weather.decal_info = Vec4{static_cast<float>(decal_count), 0.0f, 0.0f, 0.0f};
    weather_buffers_[frame_index].write(&weather, sizeof(weather));
    light_data.rain = Vec4{weather.params.x, weather.params.y, weather.params.z, 0.0f};
    light_data.flood = weather.flood;

    // --- Nubes ---
    // Con el cielo fotografiado no hay nubes volumetricas: la foto trae las
    // suyas.
    light_data.clouds =
        Vec4{clouds_enabled_ && !environmentActive() ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f};
    // y: luz volumetrica. No en las caras de la sonda: su imagen es de la
    // camara de pantalla.
    // z: sombras de contacto (largo del rayo); no en la sonda (su depth es otro).
    light_data.environment = Vec4{environmentActive() ? 1.0f : 0.0f,
                                  post_.volumetric_light && !capturing_ ? 1.0f : 0.0f,
                                  post_.contact_shadows && !capturing_ ? std::max(post_.contact_shadow_length, 0.0f) : 0.0f,
                                  0.0f};
    if (!isolated()) {
        cloud_time_ += frame_delta_seconds_;
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
    cloud_push_.light_coverage = toVec4(ibl_light_radiance_, kCloudCoverage);
    // zw = origen del mundo (origen flotante) modulo 168 km: el ruido de forma
    // se repite cada 24 km y el de detalle cada 3.5 km, asi que las nubes no
    // saltan al desplazarse el mundo y el numero sigue siendo pequeno.
    constexpr double kCloudPeriod = 168000.0;
    cloud_push_.params = Vec4{static_cast<float>(frame_count_ % 64), kCloudDensity,
                              static_cast<float>(std::fmod(world_origin_[0], kCloudPeriod)),
                              static_cast<float>(std::fmod(world_origin_[2], kCloudPeriod))};

    // --- Rayos de luz: el sol proyectado en pantalla ---
    // Con w = 0 se proyecta la direccion (un punto en el infinito).
    light_shaft_push_ = GpuLightShaftPush{};
    const vk::Extent2D extent = swapchain_.extent();
    light_shaft_push_.aspect =
        static_cast<float>(extent.width) / static_cast<float>(std::max(extent.height, 1u));
    const Vec4 sun_clip = camera_data.view_projection * toVec4(lights.sky.to_sun, 0.0f);
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

    // --- Cascadas de sombra ---
    // Dependen de la camara y del sol, asi que se recalculan cada frame.
    cascades_.update(camera, lights.sun, ShadowMap::kResolution);

    // Actualizacion escalonada (como Unreal y Frostbite): redibujar las cuatro
    // cascadas cada frame era casi la mitad del frame en Bistro, y las
    // lejanas, que cubren toda la escena, apenas cambian de un frame a otro.
    //   cascada 0: cada frame, 1: cada 2, 2 y 3: cada 4 (por turnos).
    // Se fuerza si la camara se alejo mas del 10% del alcance de la cascada
    // desde que se dibujo. Una cascada que no se redibuja se sigue leyendo
    // con la matriz con la que se dibujo (el escenario no se ha movido).
    // Las caras de la sonda dibujan todas desde su camara y ensucian el
    // mapa: la siguiente vista de pantalla las rehace todas.
    const bool redraw_all =
        isolated() || !cascades_valid_ || !sunShadows() || actor_set_changed_;
    if (!isolated()) {
        ++cascade_frame_;
    }
    for (std::uint32_t i = 0; i < scene::kShadowCascadeCount; ++i) {
        const scene::ShadowCascade& current = cascades_.cascade(i);
        bool due = redraw_all;
        if (!due) {
            const std::uint64_t f = cascade_frame_;
            due = i == 0 || (i == 1 && f % 2 == 0) || (i == 2 && f % 4 == 1) ||
                  (i == 3 && f % 4 == 3);
            const Vec3 moved = camera.position() - rendered_cascade_camera_[i];
            const float limit = 0.1f * current.split_distance;
            due = due || core::dot(moved, moved) > limit * limit;

            // Algo se movio dentro de la cascada (donde estaba o donde esta):
            // su sombra guardada ya no vale.
            if (!due && !moved_spheres_.empty()) {
                const core::Frustum frustum(current.light_view_projection, /*ignore_near=*/true);
                for (const core::Vec4& sphere : moved_spheres_) {
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
            rendered_cascade_camera_[i] = camera.position();
        }
    }
    cascades_valid_ = !isolated() && sunShadows();

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
    shadow_data.params = Vec4{static_cast<float>(ShadowMap::kResolution),
                              sunShadows() ? 1.0f : 0.0f, cascade_debug_ ? 1.0f : 0.0f,
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
                             shadows_enabled_ ? 1.0f : 0.0f, 0.0f};

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
    gpu_profiler_.collect(current_frame_);
    gpu_visible_submeshes_ = culling.early + culling.late;
    occluded_submeshes_ = culling.occluded;

    // Huesos de este frame (tambien los usa la captura de la sonda).
    stage = TimingClock::now();
    updateActors(scene, current_frame_);
    frame_timings_.actors_ms += since(stage);

    if (probeCaptureDue(scene)) {
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

    // Los uniform buffers de este frame no estan en uso: la fence lo garantiza.
    const core::Mat4 saved_view_projection = camera_view_projection_;
    const core::Mat4 saved_previous_view_projection = previous_view_projection_;
    const core::Mat4 saved_view = camera_view_;
    const Vec3 saved_camera_position = camera_position_;
    secondary_view_ = !present;
    stage = TimingClock::now();
    updateUniforms(scene, scene.camera(), current_frame_);
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
    if (!present) {
        // La vista principal sigue con su camara del frame anterior.
        camera_view_projection_ = saved_view_projection;
        previous_view_projection_ = saved_previous_view_projection;
        camera_view_ = saved_view;
        camera_position_ = saved_camera_position;
        secondary_view_ = false;
    }

    if (!present) {
        vk::SubmitInfo view_submit{};
        const vk::CommandBuffer view_command = *cmd;
        view_submit.commandBufferCount = 1;
        view_submit.pCommandBuffers = &view_command;
        device_.graphicsQueue().submit(view_submit, *fence);
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

    device_.graphicsQueue().submit(submit_info, *fence);

    const vk::SwapchainKHR swapchain = *swapchain_.handle();

    vk::PresentInfoKHR present_info{};
    present_info.waitSemaphoreCount = 1;
    present_info.pWaitSemaphores = &signal_semaphore;
    present_info.swapchainCount = 1;
    present_info.pSwapchains = &swapchain;
    present_info.pImageIndices = &image_index;

    const vk::Result present_result = device_.presentQueue().presentKHR(present_info);

    if (present_result == vk::Result::eErrorOutOfDateKHR ||
        present_result == vk::Result::eSuboptimalKHR || framebuffer_resized_) {
        recreateSwapchain();
    } else if (present_result != vk::Result::eSuccess) {
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
    device_.graphicsQueue().submit(submit_info, *fence);

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
    gpu_profiler_.begin(cmd, frame_index);
    pass_start_ = std::chrono::steady_clock::now();
    output_depth_ready_ = false;

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
    if (terrain_pass_.recordUploads(cmd, frame_index)) local_static_dirty_ = true;
    terrain_pass_.prepare(frame_index, camera_position_, camera_view_projection_);
    // Voxeles: secciones rehechas (romper/poner bloques) y las visibles.
    if (voxel_pass_.recordUploads(cmd, frame_index)) staticGeometryChanged();
    voxel_pass_.prepare(frame_index, camera_position_, camera_view_projection_);
    water_pass_.prepare(frame_index);

    if (!rain_map_ready_ && (rainAvailable() || waterAvailable()) && !actor_draws_.empty()) {
        recordRainMap(cmd, frame_index);
        markPass(cmd, frame_index, "Mapa de lluvia");
    }
    recordShadowPass(cmd, frame_index);
    markPass(cmd, frame_index, "Sombras (cascadas)");
    recordLocalShadowPass(cmd, frame_index);
    markPass(cmd, frame_index, "Sombras locales");
    recordSkyLutPass(cmd);
    markPass(cmd, frame_index, "Cielo + IBL");
    recordCloudPass(cmd, frame_index);
    markPass(cmd, frame_index, "Nubes");
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
    recordLightingPass(cmd, frame_index, scene_color_);
    markPass(cmd, frame_index, "Iluminacion");
    recordGlassPass(cmd, frame_index);
    if (!water_pass_.empty() && !capturing_) {
        recordWaterPass(cmd, frame_index);
        markPass(cmd, frame_index, "Agua");
    }
    markPass(cmd, frame_index, "Vidrio");
    if (!particles_.empty() && !capturing_) {
        recordParticlePass(cmd, frame_index);
        markPass(cmd, frame_index, "Particulas");
    }
    recordFilterHistoryCopies(cmd);
    markPass(cmd, frame_index, "Copias de historia");
    if (upscaling_) {
        recordUpscalePass(cmd);
        markPass(cmd, frame_index, graphics_.upscaler == Upscaler::Fsr1 ? "FSR 1" : "TAA / escalado");
    }
    recordBloomPass(cmd);
    markPass(cmd, frame_index, "Bloom");
    recordLightShaftPass(cmd);
    markPass(cmd, frame_index, "Rayos de luz");
    if (!secondary_view_) recordAutoExposurePass(cmd);  // la exposicion es de la vista principal
    markPass(cmd, frame_index, "Auto-exposicion");
    recordCompositePass(cmd, frame_index);
    markPass(cmd, frame_index, "Composicion");
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
    recordViewCopy(cmd);
    if (presenting_) recordPostProcessPass(cmd, image_index);
    markPass(cmd, frame_index, "FXAA + presentacion");

    cmd.end();
}

void VulkanRenderer::recordShadowPass(const vk::raii::CommandBuffer& cmd,
                                      std::uint32_t frame_index) {
    const vk::Extent2D extent = shadow_map_.extent();
    const vk::ImageSubresourceRange all_cascades{vk::ImageAspectFlagBits::eDepth, 0, 1, 0,
                                                 scene::kShadowCascadeCount};

    // --- Todas las capas pasan a destino de profundidad ---
    // El frame anterior las dejo como textura de la pasada de iluminacion. Si
    // alguna cascada no se redibuja, su contenido se conserva (layout
    // anterior); si se redibujan todas, se puede descartar.
    bool all_due = true;
    for (bool due : cascade_due_) {
        all_due = all_due && due;
    }
    vk::ImageMemoryBarrier2 to_attachment{};
    to_attachment.srcStageMask = vk::PipelineStageFlagBits2::eFragmentShader |
                                 vk::PipelineStageFlagBits2::eComputeShader;
    to_attachment.srcAccessMask = vk::AccessFlagBits2::eShaderSampledRead;
    to_attachment.dstStageMask = vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                                 vk::PipelineStageFlagBits2::eLateFragmentTests;
    to_attachment.dstAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentWrite |
                                  vk::AccessFlagBits2::eDepthStencilAttachmentRead;
    to_attachment.oldLayout =
        all_due ? vk::ImageLayout::eUndefined : vk::ImageLayout::eDepthReadOnlyOptimal;
    to_attachment.newLayout = vk::ImageLayout::eDepthAttachmentOptimal;
    to_attachment.image = *shadow_map_.image().handle();
    to_attachment.subresourceRange = all_cascades;

    vk::DependencyInfo to_attachment_dependency{};
    to_attachment_dependency.setImageMemoryBarriers(to_attachment);
    cmd.pipelineBarrier2(to_attachment_dependency);

    // --- Una pasada por cascada ---
    for (std::uint32_t cascade = 0; cascade < scene::kShadowCascadeCount; ++cascade) {
        if (!cascade_due_[cascade]) {
            continue;  // Se queda la que ya habia (ver updateUniforms).
        }
        vk::RenderingAttachmentInfo depth_attachment{};
        depth_attachment.imageView = *shadow_map_.cascadeView(cascade);
        depth_attachment.imageLayout = vk::ImageLayout::eDepthAttachmentOptimal;
        depth_attachment.loadOp = vk::AttachmentLoadOp::eClear;
        depth_attachment.storeOp = vk::AttachmentStoreOp::eStore;
        depth_attachment.clearValue = vk::ClearValue{vk::ClearDepthStencilValue{1.0f, 0}};

        vk::RenderingInfo rendering_info{};
        rendering_info.renderArea = vk::Rect2D{vk::Offset2D{0, 0}, extent};
        rendering_info.layerCount = 1;
        rendering_info.pDepthAttachment = &depth_attachment;

        cmd.beginRendering(rendering_info);

        // Con las sombras apagadas basta con dejar el mapa limpio: todo queda
        // a profundidad maxima, o sea, sin nada que ocluya.
        if (sunShadows()) {
            cmd.setViewport(0, vk::Viewport{0.0f, 0.0f, static_cast<float>(extent.width),
                                            static_cast<float>(extent.height), 0.0f, 1.0f});
            cmd.setScissor(0, vk::Rect2D{vk::Offset2D{0, 0}, extent});

            recordActorShadows(cmd, frame_index, rendered_cascades_[cascade].light_view_projection);
            terrain_pass_.recordShadow(cmd, frame_index, skin_sets_[frame_index],
                                       rendered_cascades_[cascade].light_view_projection);
            voxel_pass_.recordShadow(cmd, frame_index, skin_sets_[frame_index],
                                     rendered_cascades_[cascade].light_view_projection);
        }

        cmd.endRendering();
    }

    // --- Y quedan listas para muestrearse desde la iluminacion ---
    vk::ImageMemoryBarrier2 to_read{};
    to_read.srcStageMask = vk::PipelineStageFlagBits2::eLateFragmentTests;
    to_read.srcAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
    to_read.dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader;
    to_read.dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead;
    to_read.oldLayout = vk::ImageLayout::eDepthAttachmentOptimal;
    to_read.newLayout = vk::ImageLayout::eDepthReadOnlyOptimal;
    to_read.image = *shadow_map_.image().handle();
    to_read.subresourceRange = all_cascades;

    vk::DependencyInfo to_read_dependency{};
    to_read_dependency.setImageMemoryBarriers(to_read);
    cmd.pipelineBarrier2(to_read_dependency);
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
    };

    const vk::Image spot_image = *local_shadow_maps_.spotImage().handle();
    const vk::Image point_image = *local_shadow_maps_.pointImage().handle();

    // La cache solo vale para lo estatico: si un actor animado esta al alcance
    // de la luz, su mapa se redibuja cada frame (y uno mas cuando se va, para
    // borrar su silueta).
    const bool static_dirty = local_static_dirty_;
    const auto needs_render = [this, static_dirty](bool active, bool dirty, const Vec3& position, float range,
                                                   bool& had_actor) {
        const bool touches = active && actorsTouch(position, range);
        const bool render = active && (dirty || static_dirty || touches || had_actor);
        had_actor = touches;
        return render;
    };

    std::vector<ShadowJob> jobs;
    if (shadows_enabled_) {
        local_static_dirty_ = false;
        for (std::uint32_t slot = 0; slot < scene::kMaxShadowedSpotLights; ++slot) {
            const scene::SpotShadow& spot = local_shadows_.spots()[slot];
            if (!needs_render(spot.active, spot.dirty, spot.position, spot.range,
                              spot_had_actor_[slot])) {
                continue;
            }
            jobs.push_back(ShadowJob{&local_shadow_maps_.spotView(slot), spot_image, slot,
                                     local_shadow_maps_.spotExtent(), spot.light_view_projection,
                                     spot.position, spot.range, Vec3{}});
        }

        for (std::uint32_t slot = 0; slot < scene::kMaxShadowedPointLights; ++slot) {
            const scene::PointShadow& point = local_shadows_.points()[slot];
            if (!needs_render(point.active(), point.dirty, point.position, point.range,
                              point_had_actor_[slot])) {
                continue;
            }
            for (std::uint32_t face = 0; face < scene::kPointShadowFaceCount; ++face) {
                jobs.push_back(ShadowJob{&local_shadow_maps_.pointFaceView(slot, face), point_image,
                                         slot * scene::kPointShadowFaceCount + face,
                                         local_shadow_maps_.pointExtent(),
                                         point.face_view_projection[face], point.position,
                                         point.range,
                                         scene::LocalLightShadows::faceDirection(face)});
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
                                                       vk::ImageLayout::eDepthAttachmentOptimal));
            to_read.push_back(depthLayersBarrier(image, 0, count,
                                                 vk::ImageLayout::eDepthAttachmentOptimal,
                                                 vk::ImageLayout::eDepthReadOnlyOptimal));
        }
    } else {
        for (const ShadowJob& job : jobs) {
            to_attachment.push_back(depthLayersBarrier(job.image, job.layer, 1,
                                                       vk::ImageLayout::eDepthReadOnlyOptimal,
                                                       vk::ImageLayout::eDepthAttachmentOptimal));
            to_read.push_back(depthLayersBarrier(job.image, job.layer, 1,
                                                 vk::ImageLayout::eDepthAttachmentOptimal,
                                                 vk::ImageLayout::eDepthReadOnlyOptimal));
        }
    }

    if (to_attachment.empty()) {
        return;  // Todo sigue en cache.
    }

    vk::DependencyInfo to_attachment_dependency{};
    to_attachment_dependency.setImageMemoryBarriers(to_attachment);
    cmd.pipelineBarrier2(to_attachment_dependency);

    // --- Una pasada por capa ---
    for (const ShadowJob& job : jobs) {
        vk::RenderingAttachmentInfo depth_attachment{};
        depth_attachment.imageView = **job.view;
        depth_attachment.imageLayout = vk::ImageLayout::eDepthAttachmentOptimal;
        depth_attachment.loadOp = vk::AttachmentLoadOp::eClear;
        depth_attachment.storeOp = vk::AttachmentStoreOp::eStore;
        depth_attachment.clearValue = vk::ClearValue{vk::ClearDepthStencilValue{1.0f, 0}};

        vk::RenderingInfo rendering_info{};
        rendering_info.renderArea = vk::Rect2D{vk::Offset2D{0, 0}, job.extent};
        rendering_info.layerCount = 1;
        rendering_info.pDepthAttachment = &depth_attachment;

        cmd.beginRendering(rendering_info);

        cmd.setViewport(0, vk::Viewport{0.0f, 0.0f, static_cast<float>(job.extent.width),
                                        static_cast<float>(job.extent.height), 0.0f, 1.0f});
        cmd.setScissor(0, vk::Rect2D{vk::Offset2D{0, 0}, job.extent});

        recordActorShadows(cmd, frame_index, job.light_view_projection, job.light_position,
                           job.range);
        // El terreno y los bloques tambien tapan la luz de antorchas y focos.
        terrain_pass_.recordShadow(cmd, frame_index, skin_sets_[frame_index], job.light_view_projection,
                                   /*local=*/true);
        voxel_pass_.recordShadow(cmd, frame_index, skin_sets_[frame_index], job.light_view_projection);

        cmd.endRendering();
    }

    vk::DependencyInfo to_read_dependency{};
    to_read_dependency.setImageMemoryBarriers(to_read);
    cmd.pipelineBarrier2(to_read_dependency);

    local_shadow_layout_ready_ = true;
}

void VulkanRenderer::recordGeometryPass(const vk::raii::CommandBuffer& cmd,
                                        std::uint32_t frame_index) {
    const vk::Extent2D extent = gbuffer_.extent();
    const auto attachments = gbuffer_.colorAttachments();

    // Oclusion en dos fases solo para la camara de pantalla: las caras de la
    // sonda (y el culling de oclusion apagado) dibujan todo lo que esta en el
    // campo de vision, sin tocar la visibilidad guardada.
    const bool occlusion = occlusion_culling_enabled_ && !isolated();

    // --- Culling en GPU, fase temprana: lo visible el frame anterior ---
    gpu_culling_.recordCull(cmd, frame_index, 0, occlusion);

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
    depth_barrier.newLayout = vk::ImageLayout::eDepthAttachmentOptimal;
    depth_barrier.image = *gbuffer_.depth().handle();
    depth_barrier.subresourceRange =
        vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1};

    vk::DependencyInfo dependency{};
    dependency.setImageMemoryBarriers(barriers);
    cmd.pipelineBarrier2(dependency);

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
        depth_attachment.imageLayout = vk::ImageLayout::eDepthAttachmentOptimal;
        depth_attachment.loadOp = clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad;
        depth_attachment.storeOp = vk::AttachmentStoreOp::eStore;
        depth_attachment.clearValue = vk::ClearValue{vk::ClearDepthStencilValue{1.0f, 0}};

        vk::RenderingInfo rendering_info{};
        rendering_info.renderArea = vk::Rect2D{vk::Offset2D{0, 0}, extent};
        rendering_info.layerCount = 1;
        rendering_info.setColorAttachments(color_attachments);
        rendering_info.pDepthAttachment = &depth_attachment;

        cmd.beginRendering(rendering_info);
        cmd.setViewport(0, vk::Viewport{0.0f, 0.0f, static_cast<float>(extent.width),
                                        static_cast<float>(extent.height), 0.0f, 1.0f});
        cmd.setScissor(0, vk::Rect2D{vk::Offset2D{0, 0}, extent});
    };

    // --- Fase temprana: animados + lo que era visible ---
    begin_rendering(/*clear=*/true);
    drawCpuActors(cmd, frame_index);
    // El terreno, pronto: tapa mucho y entra en la piramide Hi-Z.
    terrain_pass_.recordGBuffer(cmd, frame_index, skin_sets_[frame_index]);
    // Los voxeles tambien tapan mucho: pronto, de cerca a lejos.
    voxel_pass_.recordGBuffer(cmd, frame_index, skin_sets_[frame_index]);
    drawGpuClusters(cmd, frame_index, 0);
    cmd.endRendering();

    if (!occlusion || gpu_culling_.clusterCount() == 0) {
        return;
    }

    // --- Piramide Hi-Z con la profundidad de lo ya dibujado ---
    vk::ImageMemoryBarrier2 depth_to_read{};
    depth_to_read.srcStageMask = vk::PipelineStageFlagBits2::eLateFragmentTests;
    depth_to_read.srcAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
    depth_to_read.dstStageMask = vk::PipelineStageFlagBits2::eComputeShader;
    depth_to_read.dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead;
    depth_to_read.oldLayout = vk::ImageLayout::eDepthAttachmentOptimal;
    depth_to_read.newLayout = vk::ImageLayout::eDepthReadOnlyOptimal;
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
    depth_to_attachment.oldLayout = vk::ImageLayout::eDepthReadOnlyOptimal;
    depth_to_attachment.newLayout = vk::ImageLayout::eDepthAttachmentOptimal;
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
    cmd.pipelineBarrier2(color_dependency);

    begin_rendering(/*clear=*/false);
    drawGpuClusters(cmd, frame_index, 1);
    cmd.endRendering();
}

void VulkanRenderer::drawCpuActors(const vk::raii::CommandBuffer& cmd,
                                   std::uint32_t frame_index) {
    bool bound = false;
    std::int32_t bound_shader = -1;
    const core::Frustum frustum(camera_view_projection_);

    for (const ActorDraw& draw : actor_draws_) {
        if (draw.per_submesh || draw.shadows_only) {
            continue;  // Escenario (lo dibuja drawGpuClusters) o solo sombras.
        }
        if (!bound) {
            cmd.bindPipeline(vk::PipelineBindPoint::eGraphics,
                             *skinned_pass_.geometryPipeline());
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
                bindMaterialPipeline(cmd, frame_index, material, bound_shader, push);
                cmd.pushConstants<GpuSkinnedPush>(
                    *skinned_pass_.geometryLayout(),
                    vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment, 0,
                    push);
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

void VulkanRenderer::drawGpuClusters(const vk::raii::CommandBuffer& cmd,
                                     std::uint32_t frame_index, std::uint32_t phase) {
    if (gpu_culling_.clusterCount() == 0) {
        return;
    }

    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *skinned_pass_.geometryPipeline());
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *skinned_pass_.geometryLayout(), 0,
                           *skin_sets_[frame_index], nullptr);

    const vk::Buffer commands = *gpu_culling_.commands(phase).handle();
    const vk::Buffer counts = *gpu_culling_.counts(phase).handle();

    // Una llamada por lote (modelo x material, todos los actores juntos):
    // la GPU decide cuantas submallas (y de quien) dibuja, leyendo el
    // contador del lote; cada comando lleva la matriz de su actor.
    std::uint32_t bound_model = UINT32_MAX;
    std::int32_t bound_shader = -1;
    for (std::uint32_t b = 0; b < draw_batches_.size(); ++b) {
        const DrawBatch& batch = draw_batches_[b];
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
        push.flags = 1u;
        bindMaterialPipeline(cmd, frame_index, material, bound_shader, push);
        cmd.pushConstants<GpuSkinnedPush>(
            *skinned_pass_.geometryLayout(),
            vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment, 0, push);

        cmd.drawIndexedIndirectCount(
            commands, static_cast<vk::DeviceSize>(batch.first_slot) * GpuCulling::kCommandSize, counts,
            static_cast<vk::DeviceSize>(b) * sizeof(std::uint32_t), batch.capacity,
            GpuCulling::kCommandSize);
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
    depth_barrier.oldLayout = vk::ImageLayout::eDepthAttachmentOptimal;
    depth_barrier.newLayout = vk::ImageLayout::eDepthReadOnlyOptimal;
    depth_barrier.image = *gbuffer_.depth().handle();
    depth_barrier.subresourceRange =
        vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1};

    // --- Y la imagen del SSAO a destino (el frame anterior la leyo) ---
    barriers[GBuffer::kColorAttachmentCount + 1] = discardToAttachment(*ssao_image_.handle());

    pipelineBarrier(cmd, barriers);

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

    GpuVolumetricPush push{};
    // Anisotropia 0.6: polvo y humo finos dispersan sobre todo hacia delante.
    // 60 m: mas alla el mapa de sombras ya es grueso y el efecto no aporta.
    push.params = Vec4{post_.volumetric_density, post_.volumetric_anisotropy, weather_time_, 60.0f};
    drawFullscreen(cmd, volumetric_pass_, &volumetric_sets_[frame_index], volumetric_image_,
                   &push);

    pipelineBarrier(cmd, writtenToSampled(*volumetric_image_.handle()));
}

void VulkanRenderer::recordSsgiPass(const vk::raii::CommandBuffer& cmd,
                                    std::uint32_t frame_index) {
    const bool ray_traced = rayTracingActive() && !isolated();

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
    push.extra.x = static_cast<float>(frame_count_ % 64);
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
        rt_push.params = Vec4{static_cast<float>(frame_count_ % 64),
                              scene_history_valid_ ? 1.0f : 0.0f, 0.0f, 0.0f};
        ray_tracing_.record(cmd, frame_index, RayTracing::Pass::Gi, gi_raw_.extent(), rt_push);

        raw_to_sampled = colorBarrier(*gi_raw_.handle(), vk::ImageLayout::eGeneral,
                                      vk::ImageLayout::eShaderReadOnlyOptimal,
                                      vk::PipelineStageFlagBits2::eComputeShader,
                                      vk::AccessFlagBits2::eShaderStorageWrite,
                                      vk::PipelineStageFlagBits2::eComputeShader,
                                      vk::AccessFlagBits2::eShaderSampledRead);
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
    GiTemporalPush temporal{};
    temporal.previous_view_projection = previous_view_projection_;
    temporal.params = Vec4{gi_filter_history_valid_ && !isolated() ? 1.0f : 0.0f, 0.0f, 0.0f,
                           0.0f};
    dispatch(gi_temporal_pass_, gi_temporal_sets_[frame_index], temporal);

    const std::uint32_t chain = (frame_index * 2 + (isolated() ? 1 : 0)) * kGiAtrousIterations;
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

    if (ray_traced) {
        pipelineBarrier(cmd, colorBarrier(*ssr_raw_.handle(), vk::ImageLayout::eUndefined,
                                          vk::ImageLayout::eGeneral,
                                          vk::PipelineStageFlagBits2::eFragmentShader,
                                          vk::AccessFlagBits2::eShaderSampledRead,
                                          vk::PipelineStageFlagBits2::eComputeShader,
                                          vk::AccessFlagBits2::eShaderStorageWrite));
        RayTracing::Push rt_push{};
        rt_push.previous_view_projection = previous_view_projection_;
        rt_push.params = Vec4{static_cast<float>(frame_count_ % 64),
                              ssr_history_ready_ ? 1.0f : 0.0f, 0.0f, 0.0f};
        ray_tracing_.record(cmd, frame_index, RayTracing::Pass::Reflections, ssr_raw_.extent(),
                            rt_push);
        raw_to_sampled = colorBarrier(*ssr_raw_.handle(), vk::ImageLayout::eGeneral,
                                      vk::ImageLayout::eShaderReadOnlyOptimal,
                                      vk::PipelineStageFlagBits2::eComputeShader,
                                      vk::AccessFlagBits2::eShaderStorageWrite,
                                      vk::PipelineStageFlagBits2::eFragmentShader,
                                      vk::AccessFlagBits2::eShaderSampledRead);
    } else {
        // Va despues de la GI: la imagen HDR ya esta como textura.
        pipelineBarrier(cmd, discardToAttachment(*ssr_raw_.handle()));

        GpuSsgiPush push{};
        push.previous_view_projection = previous_view_projection_;
        // Sin frame anterior o con el SSR apagado, el shader no refleja nada.
        // z: numero de frame, para que el ruido del primer paso cambie cada
        // frame.
        push.params = Vec4{post_.reflections && ssr_history_ready_ && !isolated() ? 1.0f : 0.0f,
                           1.0f, static_cast<float>(frame_count_ % 64), 0.0f};
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
    GpuSsgiPush resolve{};
    resolve.previous_view_projection = previous_view_projection_;
    resolve.params = Vec4{ssr_filter_history_valid_ && post_.reflections && !isolated() ? 1.0f : 0.0f,
                          kSsrHistoryWeight, 0.0f, 0.0f};
    drawFullscreen(cmd, ssr_resolve_pass_, &ssr_resolve_sets_[frame_index], ssr_image_,
                   &resolve);
}

void VulkanRenderer::recordFilterHistoryCopies(const vk::raii::CommandBuffer& cmd) {
    if (isolated()) return;  // la segunda vista no escribe las historias
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
    // (La GI la dejo lista su filtro, en compute.)
    pipelineBarrier(cmd, {writtenToSampled(*ssao_image_.handle()),
                          writtenToSampled(*ssr_image_.handle()),
                          colorBarrier(*target.handle(), vk::ImageLayout::eUndefined,
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

    cmd.beginRendering(rendering_info);

    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *lighting_pass_.pipeline());
    cmd.setViewport(0, vk::Viewport{0.0f, 0.0f, static_cast<float>(extent.width),
                                    static_cast<float>(extent.height), 0.0f, 1.0f});
    cmd.setScissor(0, vk::Rect2D{vk::Offset2D{0, 0}, extent});
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *lighting_pass_.layout(), 0,
                           *lighting_sets_[frame_index], nullptr);

    cmd.draw(3, 1, 0, 0);

    cmd.endRendering();
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
    depth_barrier.oldLayout = vk::ImageLayout::eDepthReadOnlyOptimal;
    depth_barrier.newLayout = vk::ImageLayout::eDepthReadOnlyOptimal;
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

    vk::RenderingAttachmentInfo color_attachment{};
    color_attachment.imageView = *scene_color_.view();
    color_attachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
    color_attachment.loadOp = vk::AttachmentLoadOp::eLoad;
    color_attachment.storeOp = vk::AttachmentStoreOp::eStore;
    vk::RenderingAttachmentInfo depth_attachment{};
    depth_attachment.imageView = *gbuffer_.depth().view();
    depth_attachment.imageLayout = vk::ImageLayout::eDepthReadOnlyOptimal;
    depth_attachment.loadOp = vk::AttachmentLoadOp::eLoad;
    depth_attachment.storeOp = vk::AttachmentStoreOp::eNone;
    vk::RenderingInfo rendering_info{};
    rendering_info.renderArea = vk::Rect2D{vk::Offset2D{0, 0}, extent};
    rendering_info.layerCount = 1;
    rendering_info.setColorAttachments(color_attachment);
    rendering_info.pDepthAttachment = &depth_attachment;

    cmd.beginRendering(rendering_info);
    cmd.setViewport(0, vk::Viewport{0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height),
                                    0.0f, 1.0f});
    cmd.setScissor(0, vk::Rect2D{vk::Offset2D{0, 0}, extent});
    water_pass_.record(cmd, frame_index, skin_sets_[frame_index], glass_sets_[frame_index]);
    cmd.endRendering();
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
    depth_attachment.imageLayout = vk::ImageLayout::eDepthReadOnlyOptimal;
    depth_attachment.loadOp = vk::AttachmentLoadOp::eLoad;
    depth_attachment.storeOp = vk::AttachmentStoreOp::eNone;

    vk::RenderingInfo rendering_info{};
    rendering_info.renderArea = vk::Rect2D{vk::Offset2D{0, 0}, extent};
    rendering_info.layerCount = 1;
    rendering_info.setColorAttachments(color_attachment);
    rendering_info.pDepthAttachment = &depth_attachment;

    cmd.beginRendering(rendering_info);
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
            cmd.pushConstants<GpuSkinnedPush>(
                *skinned_pass_.glassLayout(),
                vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment, 0, push);
            bound_material = submesh.material;
        }
        cmd.drawIndexed(submesh.index_count, 1, submesh.first_index, 0, 0);
    }

    cmd.endRendering();
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
    depth_barrier.oldLayout = vk::ImageLayout::eDepthReadOnlyOptimal;
    depth_barrier.newLayout = vk::ImageLayout::eDepthReadOnlyOptimal;
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
    depth_attachment.imageLayout = vk::ImageLayout::eDepthReadOnlyOptimal;
    depth_attachment.loadOp = vk::AttachmentLoadOp::eLoad;
    depth_attachment.storeOp = vk::AttachmentStoreOp::eNone;

    vk::RenderingInfo rendering_info{};
    rendering_info.renderArea = vk::Rect2D{vk::Offset2D{0, 0}, extent};
    rendering_info.layerCount = 1;
    rendering_info.setColorAttachments(mask_attachment);
    rendering_info.pDepthAttachment = &depth_attachment;

    cmd.beginRendering(rendering_info);
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
            cmd.pushConstants<GpuSkinnedPush>(
                *skinned_pass_.geometryLayout(),
                vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment, 0, push);
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
    cmd.endRendering();

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
    return it != ui_textures_.end() ? *it->second.view() : VK_NULL_HANDLE;
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
}

void VulkanRenderer::invalidateHistory() {
    taa_history_valid_ = false;
    scene_history_valid_ = false;
    ssr_history_ready_ = false;
    ssr_filter_history_valid_ = false;
    gi_filter_history_valid_ = false;
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
    gpu.base_color = data.base_color;
    gpu.emissive = core::Vec4{data.emissive.x, data.emissive.y, data.emissive.z, 0.0f};
    gpu.params = core::Vec4{data.metallic, data.roughness, data.occlusion_strength,
                            data.normal_map_directx ? -data.normal_scale : data.normal_scale};
    gpu.reflectance = data.reflectance;
    gpu.surface_shader = data.surface_shader;
    gpu.surface_params = data.surface_params;
}

std::int32_t VulkanRenderer::createSurfaceShader(const std::vector<std::uint32_t>& vertex_spirv,
                                                 const std::vector<std::uint32_t>& fragment_spirv,
                                                 std::string* error) {
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
        device_.handle().waitIdle();
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
    if (shader != bound_shader) {
        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics,
                         shader >= 0 ? *surface_pipelines_[static_cast<std::size_t>(shader)]
                                     : *skinned_pass_.geometryPipeline());
        bound_shader = shader;
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
    depth_barrier.oldLayout = vk::ImageLayout::eDepthReadOnlyOptimal;
    depth_barrier.newLayout = vk::ImageLayout::eDepthReadOnlyOptimal;
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
    depth_attachment.imageLayout = vk::ImageLayout::eDepthReadOnlyOptimal;
    depth_attachment.loadOp = vk::AttachmentLoadOp::eLoad;
    depth_attachment.storeOp = vk::AttachmentStoreOp::eNone;

    vk::RenderingInfo rendering_info{};
    rendering_info.renderArea = area;
    rendering_info.layerCount = 1;
    rendering_info.setColorAttachments(id_attachment);
    rendering_info.pDepthAttachment = &depth_attachment;

    cmd.beginRendering(rendering_info);
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
        cmd.pushConstants<GpuSkinnedPush>(*skinned_pass_.geometryLayout(),
                                          vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment, 0,
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
                cmd.pushConstants<std::uint32_t>(*skinned_pass_.geometryLayout(),
                                                 vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
                                                 offsetof(GpuSkinnedPush, pick_id), id);
            }
            cmd.drawIndexed(submeshes[i].index_count, 1, submeshes[i].first_index, 0, 0);
        }
    }
    cmd.endRendering();

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
    cmd.pipelineBarrier2(dependency);

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
    depth_barrier.oldLayout = vk::ImageLayout::eDepthReadOnlyOptimal;
    depth_barrier.newLayout = vk::ImageLayout::eDepthReadOnlyOptimal;
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
    depth_attachment.imageLayout = vk::ImageLayout::eDepthReadOnlyOptimal;
    depth_attachment.loadOp = vk::AttachmentLoadOp::eLoad;
    depth_attachment.storeOp = vk::AttachmentStoreOp::eNone;

    vk::RenderingInfo rendering_info{};
    rendering_info.renderArea = vk::Rect2D{vk::Offset2D{0, 0}, extent};
    rendering_info.layerCount = 1;
    rendering_info.setColorAttachments(color_attachment);
    rendering_info.pDepthAttachment = &depth_attachment;

    cmd.beginRendering(rendering_info);
    particle_pass_.record(cmd, frame_index, extent);
    cmd.endRendering();
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
    source.oldLayout = vk::ImageLayout::eDepthReadOnlyOptimal;
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
    source.newLayout = vk::ImageLayout::eDepthReadOnlyOptimal;
    target.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
    target.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
    target.dstStageMask = vk::PipelineStageFlagBits2::eEarlyFragmentTests | vk::PipelineStageFlagBits2::eLateFragmentTests;
    target.dstAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentRead;
    target.oldLayout = vk::ImageLayout::eTransferDstOptimal;
    target.newLayout = vk::ImageLayout::eDepthReadOnlyOptimal;
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
    depth_barrier.oldLayout = vk::ImageLayout::eDepthReadOnlyOptimal;
    depth_barrier.newLayout = vk::ImageLayout::eDepthReadOnlyOptimal;
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
    depth_attachment.imageLayout = vk::ImageLayout::eDepthReadOnlyOptimal;
    depth_attachment.loadOp = vk::AttachmentLoadOp::eLoad;
    depth_attachment.storeOp = vk::AttachmentStoreOp::eNone;

    vk::RenderingInfo rendering_info{};
    rendering_info.renderArea = vk::Rect2D{vk::Offset2D{0, 0}, extent};
    rendering_info.layerCount = 1;
    rendering_info.setColorAttachments(color_attachment);
    rendering_info.pDepthAttachment = &depth_attachment;

    cmd.beginRendering(rendering_info);
    overlay_pass_.record(cmd, frame_index, extent, overlay_geometry_);
    cmd.endRendering();
    pipelineBarrier(cmd, writtenToSampled(*ldr_color_.handle()));
}

// Escalado de la imagen HDR interna a la de pantalla, antes del bloom y la
// composicion: TAA/TAAU (temporal, con historia) o FSR 1 (EASU, espacial), y
// despues la nitidez (RCAS) a upscaled_color_, que es lo que lee el resto.
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

    if (graphics_.upscaler == Upscaler::Fsr1) {
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
        drawFullscreen(cmd, taa_pass_, &taa_sets_[0], upscale_target_, &push);
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
    const vk::Extent2D screen = swapchain_.extent();

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
    pipelineBarrier(cmd, discardToAttachment(*sky_lut_.handle()));
    drawFullscreen(cmd, sky_lut_pass_, nullptr, sky_lut_, &sky_push_);

    // La leen la iluminacion (fragment) y el IBL (compute).
    vk::ImageMemoryBarrier2 to_sampled = writtenToSampled(*sky_lut_.handle());
    to_sampled.dstStageMask |= vk::PipelineStageFlagBits2::eComputeShader;
    pipelineBarrier(cmd, to_sampled);

    // Suelo del entorno mojado: la pelicula de agua de la humedad y los
    // charcos (mismas cantidades que la pasada de geometria).
    const bool raining = rain_enabled_ && rainAvailable();
    const float ground_wet =
        raining ? std::min(0.25f * wetness_ + 0.45f * puddles_, 0.7f) : 0.0f;
    ibl_probe_.record(cmd, ibl_light_radiance_, ibl_to_light_, environmentActive(), ground_wet);
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
    to_attachment.newLayout = vk::ImageLayout::eDepthAttachmentOptimal;
    to_attachment.image = *rain_map_.handle();
    to_attachment.subresourceRange = range;
    pipelineBarrier(cmd, to_attachment);

    vk::RenderingAttachmentInfo depth_attachment{};
    depth_attachment.imageView = *rain_map_.view();
    depth_attachment.imageLayout = vk::ImageLayout::eDepthAttachmentOptimal;
    depth_attachment.loadOp = vk::AttachmentLoadOp::eClear;
    depth_attachment.storeOp = vk::AttachmentStoreOp::eStore;
    depth_attachment.clearValue = vk::ClearValue{vk::ClearDepthStencilValue{1.0f, 0}};
    const vk::Extent2D extent{kRainMapSize, kRainMapSize};
    vk::RenderingInfo rendering_info{};
    rendering_info.renderArea = vk::Rect2D{vk::Offset2D{0, 0}, extent};
    rendering_info.layerCount = 1;
    rendering_info.pDepthAttachment = &depth_attachment;
    cmd.beginRendering(rendering_info);
    cmd.setViewport(0, vk::Viewport{0.0f, 0.0f, static_cast<float>(kRainMapSize),
                                    static_cast<float>(kRainMapSize), 0.0f, 1.0f});
    cmd.setScissor(0, vk::Rect2D{vk::Offset2D{0, 0}, extent});
    recordActorShadows(cmd, frame_index, rain_view_projection_);
    cmd.endRendering();

    vk::ImageMemoryBarrier2 to_read = to_attachment;
    to_read.srcStageMask = vk::PipelineStageFlagBits2::eLateFragmentTests;
    to_read.srcAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
    to_read.dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader;
    to_read.dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead;
    to_read.oldLayout = vk::ImageLayout::eDepthAttachmentOptimal;
    to_read.newLayout = vk::ImageLayout::eDepthReadOnlyOptimal;
    pipelineBarrier(cmd, to_read);

    rain_map_ready_ = true;
    std::cout << "[Vulkan] Mapa de lluvia listo: " << kRainMapSize << "^2 sobre "
              << 2.0f * half << " m\n";
}

void VulkanRenderer::recordCloudPass(const vk::raii::CommandBuffer& cmd,
                                     std::uint32_t frame_index) {
    pipelineBarrier(cmd, discardToAttachment(*clouds_image_.handle()));
    // Apagadas no se dibujan (la iluminacion no las lee), pero la imagen
    // queda igualmente como textura para el descriptor.
    if (clouds_enabled_ && !environmentActive()) {
        drawFullscreen(cmd, clouds_pass_, &clouds_sets_[frame_index], clouds_image_,
                       &cloud_push_);
    }
    pipelineBarrier(cmd, writtenToSampled(*clouds_image_.handle()));
}

void VulkanRenderer::recordLightShaftPass(const vk::raii::CommandBuffer& cmd) {
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
    settings.tone = Vec4{p.light_shafts ? 0.6f * p.light_shaft_intensity : 0.0f,
                         static_cast<float>(static_cast<std::int32_t>(p.tonemapper)),
                         p.saturation, p.contrast};
    settings.look = Vec4{p.vibrance, p.vignette ? p.vignette_intensity : 0.0f,
                         p.vignette_smoothness, p.chromatic_aberration};
    settings.film = Vec4{p.film_grain, static_cast<float>(frame_count_ % 4096),
                         1.0f / static_cast<float>(std::max(screen.width, 1u)),
                         1.0f / static_cast<float>(std::max(screen.height, 1u))};
    settings.white_balance = toVec4(whiteBalanceLms(p.temperature, p.tint), 0.0f);
    settings.color_filter = toVec4(p.color_filter, 0.0f);
    settings.lift = toVec4(p.lift, 0.0f);
    settings.gamma = toVec4(p.gamma, 0.0f);
    settings.gain = toVec4(p.gain, 0.0f);
    settings.vignette_color = toVec4(p.vignette_color, 0.0f);
    settings.bloom_tint = toVec4(p.bloom_tint, 0.0f);
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

    cmd.beginRendering(rendering_info);

    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *post_process_pass_.pipeline());
    cmd.setViewport(0, vk::Viewport{0.0f, 0.0f, static_cast<float>(extent.width),
                                    static_cast<float>(extent.height), 0.0f, 1.0f});
    cmd.setScissor(0, vk::Rect2D{vk::Offset2D{0, 0}, extent});
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *post_process_pass_.layout(), 0,
                           *post_process_sets_[current_frame_], nullptr);

    GpuPostProcessPush push{};
    push.inverse_resolution = core::Vec2{1.0f / static_cast<float>(extent.width),
                                         1.0f / static_cast<float>(extent.height)};
    push.enabled = post_.fxaa ? 1.0f : 0.0f;
    cmd.pushConstants<GpuPostProcessPush>(*post_process_pass_.layout(),
                                          vk::ShaderStageFlagBits::eFragment, 0, push);

    cmd.draw(3, 1, 0, 0);

    // Lo que dibuje la herramienta (el editor) va encima, en el mismo pase.
    if (overlay_) {
        overlay_(*cmd);
    }

    cmd.endRendering();

    // --- La imagen queda lista para presentarse ---
    vk::ImageMemoryBarrier2 present_barrier = colorBarrier(
        swapchain_.images()[image_index], vk::ImageLayout::eColorAttachmentOptimal,
        vk::ImageLayout::ePresentSrcKHR, vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        vk::AccessFlagBits2::eColorAttachmentWrite, vk::PipelineStageFlagBits2::eBottomOfPipe,
        vk::AccessFlagBits2::eNone);

    vk::DependencyInfo present_dependency{};
    present_dependency.setImageMemoryBarriers(present_barrier);
    cmd.pipelineBarrier2(present_dependency);
}

bool VulkanRenderer::applyPendingResize() {
    if (!initialized_ || (!framebuffer_resized_ && !settings_dirty_ && swapchain_.isValid())) {
        return false;
    }
    settings_dirty_ = false;
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

}  // namespace cramion::gfx
