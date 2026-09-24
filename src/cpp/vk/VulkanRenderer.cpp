#include "vk/VulkanRenderer.h"

#include "scene/Scene.h"
#include "vk/GpuTypes.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace cramion::gfx {
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

    instance_.initialize(info);
    surface_.initialize(instance_, window);
    device_.initialize(instance_, surface_);
    swapchain_.initialize(device_, surface_, width, height);
    gbuffer_.create(device_, swapchain_.extent());
    gpu_culling_.create(device_);
    createRenderTargets();
    shadow_map_.create(device_);
    local_shadow_maps_.create(device_);

    // La iluminacion escribe en HDR; la composicion lo lleva a 8 bits y el
    // FXAA es quien escribe en la swapchain.
    lighting_pass_.create(device_, kHdrFormat);
    post_process_pass_.create(device_, swapchain_.imageFormat());
    skinned_pass_.create(device_, gbuffer_, shadow_map_.format());

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

        const std::array<Type, 4> composite_bindings = {
            Type::eCombinedImageSampler, Type::eCombinedImageSampler,
            Type::eCombinedImageSampler, Type::eStorageBuffer};
        FullscreenPassDesc composite{};
        composite.fragment_shader = "composite.frag.spv";
        composite.bindings = composite_bindings;
        composite.push_constant_size = sizeof(GpuCompositePush);
        composite.color_format = kLdrFormat;
        composite_pass_.create(device_, composite);

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

    createUniformBuffers();
    createDescriptors();
    createCommandObjects();
    createSyncObjects();

    initialized_ = true;
    std::cout << "[Vulkan] Renderizador diferido listo\n";
}

void VulkanRenderer::shutdown() {
    if (!initialized_) {
        return;
    }

    device_.waitIdle();

    skinned_models_.clear();
    actor_draws_.clear();
    gpu_culling_.destroy();

    render_finished_.clear();
    in_flight_fences_.clear();
    image_available_.clear();
    command_buffers_.clear();
    command_pool_ = nullptr;

    exposure_average_sets_.clear();
    histogram_sets_.clear();
    light_shaft_sets_.clear();
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
    post_pool_ = nullptr;
    post_process_sets_.clear();
    lighting_sets_.clear();
    descriptor_pool_ = nullptr;
    skin_sets_.clear();
    skin_pool_ = nullptr;

    for (VulkanBuffer& buffer : bone_buffers_) {
        buffer.destroy();
    }
    bone_buffers_.clear();
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
    local_shadow_buffers_.clear();
    shadow_buffers_.clear();
    light_buffers_.clear();
    camera_buffers_.clear();

    exposure_average_pass_.destroy();
    histogram_pass_.destroy();
    light_shaft_pass_.destroy();
    clouds_pass_.destroy();
    gi_atrous_pass_.destroy();
    gi_temporal_pass_.destroy();
    ssr_resolve_pass_.destroy();
    ssr_pass_.destroy();
    ssgi_pass_.destroy();
    sky_lut_pass_.destroy();
    composite_pass_.destroy();
    bloom_up_pass_.destroy();
    bloom_down_pass_.destroy();
    ssao_pass_.destroy();
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
    ldr_color_.destroy();
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
    pool_sizes[1].descriptorCount = kMaxFramesInFlight * (GBuffer::kColorAttachmentCount + 15);

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
        vk::DescriptorPoolSize{vk::DescriptorType::eStorageBuffer, kMaxFramesInFlight},
        vk::DescriptorPoolSize{vk::DescriptorType::eCombinedImageSampler, kMaxFramesInFlight}};

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
    }

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
        vk::DescriptorPoolSize{vk::DescriptorType::eUniformBuffer, kMaxFramesInFlight * 6},
        vk::DescriptorPoolSize{vk::DescriptorType::eCombinedImageSampler,
                               kMaxFramesInFlight * 18 + kBloomSets + 3 + 2 + 1},
        vk::DescriptorPoolSize{vk::DescriptorType::eStorageBuffer, 4}};

    vk::DescriptorPoolCreateInfo post_pool_info{};
    post_pool_info.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    post_pool_info.maxSets = kMaxFramesInFlight * 6 + kBloomSets + 4;
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
    bloom_down_sets_ = allocate(bloom_down_pass_, kBloomLevels);
    bloom_up_sets_ = allocate(bloom_up_pass_, kBloomLevels - 1);
    composite_sets_ = allocate(composite_pass_, 1);
    light_shaft_sets_ = allocate(light_shaft_pass_, 1);
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
    actor_draws_.clear();
    bone_staging_.clear();
    submesh_bounds_.clear();
    gpu_clusters_.clear();
    std::uint32_t group_count = 0;
    std::uint32_t slot_count = 0;

    for (const scene::Actor& actor : scene.actors()) {
        if (actor.model >= skinned_models_.size()) {
            continue;  // Modelo sin subir a la GPU.
        }

        const std::vector<core::Mat4>& bones = actor.animator.boneMatrices();
        ActorDraw draw{};
        draw.model = actor.model;
        draw.transform = actor.transform;
        draw.bone_offset = static_cast<std::uint32_t>(bone_staging_.size());
        draw.bounds_center = actor.bounds_center;
        draw.bounds_radius = actor.bounds_radius;

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

            // Sus clusteres los descarta la GPU: caja en el mundo, grupo de
            // dibujo (su material) y hueco de comando.
            draw.first_group = group_count;
            draw.first_slot = slot_count;
            const auto& groups = model.drawGroups();
            for (std::uint32_t i = 0; i < model.submeshes().size(); ++i) {
                const std::uint32_t group = model.submeshGroup(i);
                if (group == SkinnedModel::kNoGroup) {
                    continue;  // Transparente: no se dibuja.
                }
                const asset::SubMesh& submesh = model.submeshes()[i];
                const core::Aabb& box = submesh_bounds_[draw.first_bounds + i];
                GpuCluster cluster{};
                cluster.bounds_min = toVec4(box.min, 0.0f);
                cluster.bounds_max = toVec4(box.max, 0.0f);
                cluster.first_index = submesh.first_index;
                cluster.index_count = submesh.index_count;
                cluster.group = group_count + group;
                cluster.first_slot = slot_count + groups[group].first_slot;
                gpu_clusters_.push_back(cluster);
            }
            group_count += static_cast<std::uint32_t>(groups.size());
            slot_count += model.slotCount();
        }

        actor_draws_.push_back(draw);
        bone_staging_.insert(bone_staging_.end(), bones.begin(), bones.end());
    }

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

template <typename Draw>
std::uint32_t VulkanRenderer::forEachVisibleSubmesh(const ActorDraw& actor,
                                                    const core::Frustum& frustum,
                                                    Draw&& draw) const {
    const SkinnedModel& model = skinned_models_[actor.model];
    const auto& submeshes = model.submeshes();

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
        if (actor.per_submesh && !frustum.intersects(submesh_bounds_[actor.first_bounds + i])) {
            continue;
        }
        draw(i);
        ++drawn;
    }
    return drawn;
}

void VulkanRenderer::recordActorShadows(const vk::raii::CommandBuffer& cmd,
                                        std::uint32_t frame_index,
                                        const vk::raii::Pipeline& pipeline,
                                        const core::Mat4& light_view_projection,
                                        const Vec3& light_position, float range) {
    bool bound = false;

    // Las cascadas se dibujan con depth clamp (range == 0): lo que queda entre
    // el sol y la cascada tambien proyecta sombra dentro, asi que no se
    // descarta por el plano cercano.
    const core::Frustum frustum(light_view_projection, /*ignore_near=*/range <= 0.0f);

    for (const ActorDraw& draw : actor_draws_) {
        if (range > 0.0f) {
            const float reach = range + draw.bounds_radius;
            const Vec3 offset = draw.bounds_center - light_position;
            if (core::dot(offset, offset) > reach * reach) {
                continue;
            }
        }

        if (!bound) {
            cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *pipeline);
            cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *skinned_pass_.shadowLayout(),
                                   0, *skin_sets_[frame_index], nullptr);
            bound = true;
        }

        const SkinnedModel& model = skinned_models_[draw.model];

        GpuSkinnedShadowPush push{};
        push.light_model_view_projection = light_view_projection * draw.transform;
        push.bone_offset = draw.bone_offset;
        cmd.pushConstants<GpuSkinnedShadowPush>(*skinned_pass_.shadowLayout(),
                                                vk::ShaderStageFlagBits::eVertex, 0, push);

        cmd.bindVertexBuffers(0, *model.vertices().handle(), {0});
        cmd.bindIndexBuffer(*model.indices().handle(), 0, vk::IndexType::eUint32);

        // Por submalla: la textura del material recorta por alfa (las hojas
        // proyectan la sombra de las hojas, no la de su rectangulo). Solo las
        // que tocan el volumen de la luz; el material se vincula solo al
        // cambiar (las submallas de un material van seguidas).
        std::uint32_t bound_material = UINT32_MAX;
        shadow_submeshes_ += forEachVisibleSubmesh(draw, frustum, [&](std::uint32_t i) {
            const asset::SubMesh& submesh = model.submeshes()[i];
            if (submesh.material != bound_material) {
                cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
                                       *skinned_pass_.shadowLayout(), 1,
                                       *model.materialSet(submesh.material), nullptr);
                bound_material = submesh.material;
            }
            cmd.drawIndexed(submesh.index_count, 1, submesh.first_index, 0, 0);
        });
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

        std::array<vk::WriteDescriptorSet, 22> writes{};

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
        const VulkanImage& source = (level == 0) ? scene_color_ : bloom_levels_[level - 1];
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

    write_texture(composite_sets_[0], 0, composite_pass_.sampler(), scene_color_, kRead);
    write_texture(composite_sets_[0], 1, composite_pass_.sampler(), bloom_levels_[0], kRead);
    write_texture(composite_sets_[0], 2, composite_pass_.sampler(), light_shafts_, kRead);
    write_storage(composite_sets_[0], 3, exposure_buffer_);

    write_texture(light_shaft_sets_[0], 0, light_shaft_pass_.sampler(), gbuffer_.depth(),
                  vk::ImageLayout::eDepthReadOnlyOptimal);
    write_texture(light_shaft_sets_[0], 1, light_shaft_pass_.sampler(), scene_color_, kRead);

    write_texture(histogram_sets_[0], 0, histogram_pass_.sampler(), scene_color_, kRead);
    write_storage(histogram_sets_[0], 1, histogram_buffer_);

    write_storage(exposure_average_sets_[0], 0, histogram_buffer_);
    write_storage(exposure_average_sets_[0], 1, exposure_buffer_);
}

void VulkanRenderer::createRenderTargets() {
    const vk::Extent2D extent = swapchain_.extent();
    const vk::ImageUsageFlags target_usage =
        vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled;

    scene_color_.create(device_, extent, kHdrFormat, target_usage,
                        vk::ImageAspectFlagBits::eColor);
    ldr_color_.create(device_, extent, kLdrFormat, target_usage,
                      vk::ImageAspectFlagBits::eColor);
    ssao_image_.create(device_, extent, kSsaoFormat, target_usage,
                       vk::ImageAspectFlagBits::eColor);

    for (std::uint32_t level = 0; level < kBloomLevels; ++level) {
        bloom_levels_[level].create(device_, bloomLevelExtent(extent, level), kHdrFormat,
                                    target_usage, vk::ImageAspectFlagBits::eColor);
    }

    // Los rayos son un desenfoque muy amplio: media resolucion basta.
    light_shafts_.create(device_, bloomLevelExtent(extent, 0), kHdrFormat, target_usage,
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

    device_.waitIdle();
    skinned_models_.clear();
    skinned_models_.reserve(scene.models().size());

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
    gbuffer_.create(device_, swapchain_.extent());
    createRenderTargets();
    updateLightingDescriptors();
    updatePostDescriptors();

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
    GpuCamera camera_data{};
    camera_data.view = camera.view();
    camera_data.projection = camera.projection();
    camera_data.view_projection = camera_data.projection * camera_data.view;
    previous_view_projection_ = camera_view_projection_;
    camera_view_projection_ = camera_data.view_projection;
    // Se invierte una vez aqui para que el shader de iluminacion no tenga que
    // hacerlo por pixel al reconstruir la posicion del mundo.
    camera_data.inverse_view_projection = core::inverse(camera_data.view_projection);
    camera_data.position = toVec4(camera.position(), 1.0f);

    camera_buffers_[frame_index].write(&camera_data, sizeof(camera_data));

    const scene::LightSet& lights = scene.lights();

    // --- Sombras de las luces locales ---
    // Va antes que las luces porque decide que hueco de sombra tiene cada una.
    // Con las sombras apagadas no se toca la cache: al volver a encenderlas
    // solo se redibuja lo que haya cambiado entretanto. Las caras de la sonda
    // usan los mapas tal como estan: su camara no debe decidir los huecos.
    if (shadows_enabled_ && !capturing_) {
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
    light_data.ssao_enabled = ssao_enabled_ ? 1 : 0;
    light_data.gi_enabled = gi_enabled_ ? 1 : 0;
    // --- Lluvia ---
    if (!capturing_) {
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
    weather_buffers_[frame_index].write(&weather, sizeof(weather));

    // --- Nubes ---
    // Con el cielo fotografiado no hay nubes volumetricas: la foto trae las
    // suyas.
    light_data.clouds =
        Vec4{clouds_enabled_ && !environmentActive() ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f};
    light_data.environment = Vec4{environmentActive() ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f};
    if (!capturing_) {
        cloud_time_ += frame_delta_seconds_;
    }

    // Sonda de reflexion: el cubo nuevo entra fundiendose con el anterior.
    // No se refleja a si misma mientras se captura.
    if (!capturing_) {
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
    cloud_push_.params =
        Vec4{static_cast<float>(frame_count_ % 64), kCloudDensity, 0.0f, 0.0f};

    // --- Rayos de luz: el sol proyectado en pantalla ---
    // Con w = 0 se proyecta la direccion (un punto en el infinito).
    light_shaft_push_ = GpuLightShaftPush{};
    const vk::Extent2D extent = swapchain_.extent();
    light_shaft_push_.aspect =
        static_cast<float>(extent.width) / static_cast<float>(std::max(extent.height, 1u));
    const Vec4 sun_clip = camera_data.view_projection * toVec4(lights.sky.to_sun, 0.0f);
    if (light_shafts_enabled_ && sun_clip.w > 0.0f) {
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

    GpuShadows shadow_data{};
    std::array<float, scene::kShadowCascadeCount> splits{};
    std::array<float, scene::kShadowCascadeCount> texels{};

    for (std::uint32_t i = 0; i < scene::kShadowCascadeCount; ++i) {
        const scene::ShadowCascade& cascade = cascades_.cascade(i);
        shadow_data.light_view_projection[i] = cascade.light_view_projection;
        splits[i] = cascade.split_distance;
        texels[i] = cascade.texel_world_size;
    }

    shadow_data.split_distances = Vec4{splits[0], splits[1], splits[2], splits[3]};
    shadow_data.texel_world_sizes = Vec4{texels[0], texels[1], texels[2], texels[3]};
    shadow_data.params = Vec4{static_cast<float>(ShadowMap::kResolution),
                              shadows_enabled_ ? 1.0f : 0.0f, cascade_debug_ ? 1.0f : 0.0f,
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

void VulkanRenderer::drawFrame(const scene::Scene& scene) {
    if (!initialized_) {
        return;
    }

    if (framebuffer_resized_ || !swapchain_.isValid()) {
        recreateSwapchain();
        if (!swapchain_.isValid()) {
            return;
        }
    }

    const vk::raii::Device& device = device_.handle();
    const vk::raii::Fence& fence = in_flight_fences_[current_frame_];

    if (device.waitForFences(*fence, VK_TRUE, std::numeric_limits<std::uint64_t>::max()) !=
        vk::Result::eSuccess) {
        throw std::runtime_error("Tiempo de espera agotado esperando la fence del frame.");
    }

    // Resultado del culling en GPU de la ultima vez que se uso este hueco.
    const GpuCulling::Stats culling = gpu_culling_.readStats(current_frame_);
    gpu_visible_submeshes_ = culling.early + culling.late;
    occluded_submeshes_ = culling.occluded;

    // Huesos de este frame (tambien los usa la captura de la sonda).
    updateActors(scene, current_frame_);

    if (probeCaptureDue(scene)) {
        captureProbeFace(scene);
    }

    const auto [acquire_result, image_index] = swapchain_.handle().acquireNextImage(
        std::numeric_limits<std::uint64_t>::max(), *image_available_[current_frame_], nullptr);

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
        displayed_exposure_ = auto_exposure_enabled_ ? exposure_state.exposure : exposure_;
        displayed_luminance_ = exposure_state.average_luminance;
    }

    const auto now = std::chrono::steady_clock::now();
    frame_delta_seconds_ =
        (frame_count_ == 0) ? 0.0f : std::chrono::duration<float>(now - last_frame_time_).count();
    last_frame_time_ = now;

    // Los uniform buffers de este frame no estan en uso: la fence lo garantiza.
    updateUniforms(scene, scene.camera(), current_frame_);

    device.resetFences(*fence);

    const vk::raii::CommandBuffer& cmd = command_buffers_[current_frame_];
    cmd.reset();
    recordCommandBuffer(cmd, image_index, current_frame_);

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
    signature.push_back(gi_enabled_ ? 1.0f : 0.0f);
    signature.push_back(ssao_enabled_ ? 1.0f : 0.0f);
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
    const vk::Extent2D extent = swapchain_.extent();

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

void VulkanRenderer::recordCommandBuffer(const vk::raii::CommandBuffer& cmd,
                                         std::uint32_t image_index, std::uint32_t frame_index) {
    cmd.begin(vk::CommandBufferBeginInfo{});

    // Los clusteres de escenario los cuenta la GPU (frame reciente); los
    // animados se suman al dibujarlos.
    visible_submeshes_ = gpu_visible_submeshes_;
    shadow_submeshes_ = 0;
    total_submeshes_ = 0;
    for (const ActorDraw& draw : actor_draws_) {
        total_submeshes_ +=
            static_cast<std::uint32_t>(skinned_models_[draw.model].submeshes().size());
    }

    if (!rain_map_ready_ && (rainAvailable() || waterAvailable()) && !actor_draws_.empty()) {
        recordRainMap(cmd, frame_index);
    }
    recordShadowPass(cmd, frame_index);
    recordLocalShadowPass(cmd, frame_index);
    recordSkyLutPass(cmd);
    recordCloudPass(cmd, frame_index);
    recordGeometryPass(cmd, frame_index);
    recordSsaoPass(cmd, frame_index);
    recordSsgiPass(cmd, frame_index);
    recordSsrPass(cmd, frame_index);
    recordLightingPass(cmd, frame_index, scene_color_);
    recordFilterHistoryCopies(cmd);
    recordBloomPass(cmd);
    recordLightShaftPass(cmd);
    recordAutoExposurePass(cmd);
    recordCompositePass(cmd, frame_index);
    recordPostProcessPass(cmd, image_index);

    cmd.end();
}

void VulkanRenderer::recordShadowPass(const vk::raii::CommandBuffer& cmd,
                                      std::uint32_t frame_index) {
    const vk::Extent2D extent = shadow_map_.extent();
    const vk::ImageSubresourceRange all_cascades{vk::ImageAspectFlagBits::eDepth, 0, 1, 0,
                                                 scene::kShadowCascadeCount};

    // --- Todas las capas pasan a destino de profundidad ---
    // El frame anterior las dejo como textura de la pasada de iluminacion.
    vk::ImageMemoryBarrier2 to_attachment{};
    to_attachment.srcStageMask = vk::PipelineStageFlagBits2::eFragmentShader;
    to_attachment.srcAccessMask = vk::AccessFlagBits2::eShaderSampledRead;
    to_attachment.dstStageMask = vk::PipelineStageFlagBits2::eEarlyFragmentTests;
    to_attachment.dstAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
    to_attachment.oldLayout = vk::ImageLayout::eUndefined;
    to_attachment.newLayout = vk::ImageLayout::eDepthAttachmentOptimal;
    to_attachment.image = *shadow_map_.image().handle();
    to_attachment.subresourceRange = all_cascades;

    vk::DependencyInfo to_attachment_dependency{};
    to_attachment_dependency.setImageMemoryBarriers(to_attachment);
    cmd.pipelineBarrier2(to_attachment_dependency);

    // --- Una pasada por cascada ---
    for (std::uint32_t cascade = 0; cascade < scene::kShadowCascadeCount; ++cascade) {
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
        if (shadows_enabled_) {
            cmd.setViewport(0, vk::Viewport{0.0f, 0.0f, static_cast<float>(extent.width),
                                            static_cast<float>(extent.height), 0.0f, 1.0f});
            cmd.setScissor(0, vk::Rect2D{vk::Offset2D{0, 0}, extent});

            recordActorShadows(cmd, frame_index, skinned_pass_.shadowPipeline(),
                               cascades_.cascade(cascade).light_view_projection);
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
    const auto needs_render = [this](bool active, bool dirty, const Vec3& position, float range,
                                     bool& had_actor) {
        const bool touches = active && actorsTouch(position, range);
        const bool render = active && (dirty || touches || had_actor);
        had_actor = touches;
        return render;
    };

    std::vector<ShadowJob> jobs;
    if (shadows_enabled_) {
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

        recordActorShadows(cmd, frame_index, skinned_pass_.localShadowPipeline(),
                           job.light_view_projection, job.light_position, job.range);

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
    const bool occlusion = occlusion_culling_enabled_ && !capturing_;

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
    const core::Frustum frustum(camera_view_projection_);

    for (const ActorDraw& draw : actor_draws_) {
        if (draw.per_submesh) {
            continue;  // Escenario: lo dibuja drawGpuClusters.
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
        visible_submeshes_ += forEachVisibleSubmesh(draw, frustum, [&](std::uint32_t i) {
            const asset::SubMesh& submesh = model.submeshes()[i];
            if (submesh.material != bound_material) {
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
                cmd.pushConstants<GpuSkinnedPush>(
                    *skinned_pass_.geometryLayout(),
                    vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment, 0,
                    push);
                bound_material = submesh.material;
            }
            cmd.drawIndexed(submesh.index_count, 1, submesh.first_index, 0, 0);
        });
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

    for (const ActorDraw& draw : actor_draws_) {
        if (!draw.per_submesh) {
            continue;
        }
        const SkinnedModel& model = skinned_models_[draw.model];
        cmd.bindVertexBuffers(0, *model.vertices().handle(), {0});
        cmd.bindIndexBuffer(*model.indices().handle(), 0, vk::IndexType::eUint32);

        // Una llamada por material: la GPU decide cuantas submallas (y cuales)
        // dibuja, leyendo el contador del grupo.
        const auto& groups = model.drawGroups();
        for (std::uint32_t g = 0; g < groups.size(); ++g) {
            const SkinnedModel::DrawGroup& group = groups[g];
            const SkinnedModel::Material& material = model.materials()[group.material];
            cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
                                   *skinned_pass_.geometryLayout(), 1,
                                   *model.materialSet(group.material), nullptr);

            GpuSkinnedPush push{};
            push.model = draw.transform;
            push.base_color = material.base_color;
            push.emissive = material.emissive;
            push.material = material.params;
            push.bone_offset = draw.bone_offset;
            push.reflectance = material.reflectance;
            cmd.pushConstants<GpuSkinnedPush>(
                *skinned_pass_.geometryLayout(),
                vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment, 0, push);

            cmd.drawIndexedIndirectCount(
                commands,
                static_cast<vk::DeviceSize>(draw.first_slot + group.first_slot) *
                    GpuCulling::kCommandSize,
                counts, static_cast<vk::DeviceSize>(draw.first_group + g) * sizeof(std::uint32_t),
                group.capacity, GpuCulling::kCommandSize);
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

void VulkanRenderer::recordSsgiPass(const vk::raii::CommandBuffer& cmd,
                                    std::uint32_t frame_index) {
    const bool ray_traced = rayTracingActive() && !capturing_;

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
    if (!scene_history_valid_ && !capturing_) {
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
    push.params = Vec4{scene_history_valid_ && !capturing_ ? 1.0f : 0.0f, 1.0f, 0.0f, 0.0f};
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
    temporal.params = Vec4{gi_filter_history_valid_ && !capturing_ ? 1.0f : 0.0f, 0.0f, 0.0f,
                           0.0f};
    dispatch(gi_temporal_pass_, gi_temporal_sets_[frame_index], temporal);

    const std::uint32_t chain = (frame_index * 2 + (capturing_ ? 1 : 0)) * kGiAtrousIterations;
    for (std::uint32_t iteration = 0; iteration < kGiAtrousIterations; ++iteration) {
        GiAtrousPush atrous{};
        atrous.step = 1 << iteration;
        dispatch(gi_atrous_pass_, gi_atrous_sets_[chain + iteration], atrous);
    }

    if (!capturing_) {
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

    if (capturing_) {
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
    const bool ray_traced = rayTracingActive() && !capturing_ && ssr_enabled_;
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
        push.params = Vec4{ssr_enabled_ && ssr_history_ready_ && !capturing_ ? 1.0f : 0.0f,
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
    resolve.params = Vec4{ssr_filter_history_valid_ && ssr_enabled_ && !capturing_ ? 1.0f : 0.0f,
                          kSsrHistoryWeight, 0.0f, 0.0f};
    drawFullscreen(cmd, ssr_resolve_pass_, &ssr_resolve_sets_[frame_index], ssr_image_,
                   &resolve);
}

void VulkanRenderer::recordFilterHistoryCopies(const vk::raii::CommandBuffer& cmd) {
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
    const vk::Extent2D extent = swapchain_.extent();

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

void VulkanRenderer::recordBloomPass(const vk::raii::CommandBuffer& cmd) {
    const vk::Extent2D screen = swapchain_.extent();

    // La imagen HDR pasa a textura; todos los niveles se reescriben enteros.
    // La imagen HDR la leen tambien los rayos de luz y el histograma de la
    // exposicion (compute shader).
    std::array<vk::ImageMemoryBarrier2, kBloomLevels + 1> barriers{};
    barriers[0] = writtenToSampled(*scene_color_.handle());
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
        push.radius = 1.0f;
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

    ibl_probe_.record(cmd, ibl_light_radiance_, ibl_to_light_, environmentActive());
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
    recordActorShadows(cmd, frame_index, skinned_pass_.shadowPipeline(), rain_view_projection_);
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
    const vk::Extent2D extent = scene_color_.extent();
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

    GpuCompositePush push{};
    push.exposure = exposure_;
    push.bloom_strength = bloom_enabled_ ? 0.06f : 0.0f;
    push.auto_exposure_enabled = auto_exposure_enabled_ ? 1.0f : 0.0f;
    push.exposure_compensation = exposure_compensation_;
    push.light_shaft_strength = light_shafts_enabled_ ? 0.6f : 0.0f;
    push.tonemapper = aces_tonemapper_ ? 1.0f : 0.0f;
    // Mas saturacion con el tonemapper neutro: conserva el color de los
    // materiales, y un poco de viveza lo acerca a una foto de exteriores.
    push.saturation = aces_tonemapper_ ? 1.05f : 1.12f;
    drawFullscreen(cmd, composite_pass_, &composite_sets_[0], ldr_color_, &push);

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
    push.enabled = antialiasing_enabled_ ? 1.0f : 0.0f;
    cmd.pushConstants<GpuPostProcessPush>(*post_process_pass_.layout(),
                                          vk::ShaderStageFlagBits::eFragment, 0, push);

    cmd.draw(3, 1, 0, 0);

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

void VulkanRenderer::waitIdle() const {
    if (initialized_) {
        device_.waitIdle();
    }
}

}  // namespace cramion::gfx
