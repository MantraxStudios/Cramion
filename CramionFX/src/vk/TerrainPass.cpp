#include "CramionFX/vk/VulkanCompat.h"
#include "CramionFX/vk/TerrainPass.h"
#include "CramionFX/vk/GBuffer.h"

#include "CramionFX/asset/ImageFile.h"
#include "CramionFX/core/Frustum.h"
#include "CramionFX/vk/VulkanDevice.h"
#include "CramionFX/vk/VulkanShader.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

namespace cramion::gfx {

namespace {

constexpr std::uint32_t kPatchCells = 32;      // celdas por lado de un trozo
constexpr std::uint32_t kLayerSize = 512;      // texturas de capa (se reescalan)
constexpr vk::Format kHeightFormat = vk::Format::eR32Sfloat;
constexpr vk::Format kSplatFormat = vk::Format::eR8G8B8A8Unorm;
constexpr vk::Format kLayerFormat = vk::Format::eR8G8B8A8Unorm;

// Debe coincidir con TerrainParams de terrain.vert / terrain.frag.
struct GpuTerrainParams {
    core::Vec4 origin_size;  // xyz origen, w tamano
    core::Vec4 info;         // x altura maxima, y resolucion, z 1/resolucion, w capas
    core::Vec4 layer_params[kMaxTerrainLayers];  // x repeticion (m), y rugosidad, z metal, w fuerza normal
    core::Vec4 layer_tint[kMaxTerrainLayers];    // rgb tinte, a = tiene color
    core::Vec4 layer_extra[kMaxTerrainLayers];   // x = tiene normal map
    // Suelo bajo la hierba (terrain.frag): x = capa de la hierba (-1 = sin
    // hierba), y = fuerza, z = empieza lo lejano (m), w = distancia maxima.
    core::Vec4 grass_floor;
    core::Vec4 grass_floor_color;  // rgb = cesped visto de arriba (sRGB), a = umbral de la capa
};
static_assert(sizeof(GpuTerrainParams) == 16 * (2 + 3 * kMaxTerrainLayers + 2),
              "GpuTerrainParams debe coincidir con TerrainParams (std140)");

// Debe coincidir con el push de terrain.vert.
struct GpuTerrainPush {
    core::Vec4 chunk;                 // xy = esquina (0..1), z = tamano (0..1), w = faldon (m)
    core::Mat4 light_view_projection;
    std::uint32_t shadow = 0;
    std::uint32_t pad[3] = {0, 0, 0};
};

void barrier(const vk::raii::CommandBuffer& cmd, vk::Image image, vk::ImageLayout from, vk::ImageLayout to,
             vk::PipelineStageFlags2 src_stage, vk::AccessFlags2 src_access, vk::PipelineStageFlags2 dst_stage,
             vk::AccessFlags2 dst_access, std::uint32_t base_mip = 0, std::uint32_t mips = 1,
             std::uint32_t layers = 1) {
    vk::ImageMemoryBarrier2 b{};
    b.srcStageMask = src_stage;
    b.srcAccessMask = src_access;
    b.dstStageMask = dst_stage;
    b.dstAccessMask = dst_access;
    b.oldLayout = from;
    b.newLayout = to;
    b.image = image;
    b.subresourceRange = vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, base_mip, mips, 0, layers};
    vk::DependencyInfo dependency{};
    dependency.setImageMemoryBarriers(b);
    compat::pipelineBarrier(cmd, dependency);
}

// Imagen RGBA8 (cualquier tamano) a kLayerSize x kLayerSize (bilineal).
std::vector<std::uint8_t> resizeSquare(const asset::ImageRgba8& image) {
    std::vector<std::uint8_t> out(static_cast<std::size_t>(kLayerSize) * kLayerSize * 4);
    for (std::uint32_t y = 0; y < kLayerSize; ++y) {
        const float fy = (static_cast<float>(y) + 0.5f) / kLayerSize * image.height - 0.5f;
        const auto y0 = static_cast<std::uint32_t>(std::clamp(std::floor(fy), 0.0f, static_cast<float>(image.height - 1)));
        const std::uint32_t y1 = std::min(y0 + 1, image.height - 1);
        const float ty = std::clamp(fy - static_cast<float>(y0), 0.0f, 1.0f);
        for (std::uint32_t x = 0; x < kLayerSize; ++x) {
            const float fx = (static_cast<float>(x) + 0.5f) / kLayerSize * image.width - 0.5f;
            const auto x0 = static_cast<std::uint32_t>(std::clamp(std::floor(fx), 0.0f, static_cast<float>(image.width - 1)));
            const std::uint32_t x1 = std::min(x0 + 1, image.width - 1);
            const float tx = std::clamp(fx - static_cast<float>(x0), 0.0f, 1.0f);
            for (int c = 0; c < 4; ++c) {
                const auto at = [&](std::uint32_t px, std::uint32_t py) {
                    return static_cast<float>(image.pixels[(static_cast<std::size_t>(py) * image.width + px) * 4 + c]);
                };
                const float top = at(x0, y0) + (at(x1, y0) - at(x0, y0)) * tx;
                const float bottom = at(x0, y1) + (at(x1, y1) - at(x0, y1)) * tx;
                out[(static_cast<std::size_t>(y) * kLayerSize + x) * 4 + c] =
                    static_cast<std::uint8_t>(std::clamp(top + (bottom - top) * ty + 0.5f, 0.0f, 255.0f));
            }
        }
    }
    return out;
}

std::vector<std::uint8_t> solidLayer(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a) {
    std::vector<std::uint8_t> out(static_cast<std::size_t>(kLayerSize) * kLayerSize * 4);
    for (std::size_t i = 0; i < out.size(); i += 4) {
        out[i] = r;
        out[i + 1] = g;
        out[i + 2] = b;
        out[i + 3] = a;
    }
    return out;
}

// Frames que se guarda un juego de capas que ya no usa ningun terreno (~10 s
// a 60 FPS): las teselas que vuelven a entrar lo encuentran cargado.
constexpr std::uint32_t kLayerKeepFrames = 600;

// Lo que leen las imagenes del terreno: vertices, fragmentos y el compute de
// la hierba.
constexpr vk::PipelineStageFlags2 kTerrainReadStages = vk::PipelineStageFlagBits2::eVertexShader |
                                                       vk::PipelineStageFlagBits2::eFragmentShader |
                                                       vk::PipelineStageFlagBits2::eComputeShader;

std::string layerKey(const std::array<std::filesystem::path, kMaxTerrainLayers>& albedo,
                     const std::array<std::filesystem::path, kMaxTerrainLayers>& normal) {
    std::string key;
    for (std::uint32_t i = 0; i < kMaxTerrainLayers; ++i) {
        key += albedo[i].string() + "|" + normal[i].string() + "|";
    }
    return key;
}

// Copia propia (escribible) de unos datos de la CPU compartidos: si nadie mas
// los tiene y son nuestros, los mismos; si no, una copia (o de `size`
// valores `fill` si no habia).
template <typename T>
std::vector<T>& writableCopy(std::shared_ptr<const std::vector<T>>& data, bool& owned, std::size_t size, T fill) {
    if (!data || !owned || data.use_count() != 1 || data->size() != size) {
        auto copy = data && data->size() == size ? std::make_shared<std::vector<T>>(*data)
                                                 : std::make_shared<std::vector<T>>(size, fill);
        data = copy;
        owned = true;
        return *copy;
    }
    // Creado aqui como no-const (owned) y sin nadie mas: se puede escribir.
    return const_cast<std::vector<T>&>(*data);
}

}  // namespace

// Un terreno preparado en un hilo de trabajo: sus imagenes (copiandose en la
// GPU hasta que `ticket` termine) y sus parametros por frame.
struct PreparedTerrain {
    std::uint32_t resolution = 0;
    std::uint32_t splat_resolution = 0;
    VulkanImage heights;
    VulkanImage splat0;
    VulkanImage splat1;
    std::vector<VulkanBuffer> params;
    TerrainPixels pixels;  // la copia de la CPU para los rayos (compartida, sin copiar)
    std::shared_ptr<GpuTicket> ticket;
    bool adopted = false;
};

// -----------------------------------------------------------------------------
// Creacion
// -----------------------------------------------------------------------------

void TerrainPass::create(const VulkanDevice& device, const vk::raii::DescriptorSetLayout& frame_layout,
                         std::array<vk::Format, GBuffer::kColorAttachmentCount> gbuffer_formats, vk::Format depth_format,
                         vk::Format shadow_format, std::uint32_t frames_in_flight) {
    destroy();
    device_ = &device;
    frames_ = frames_in_flight;

    // Set 1: alturas y parametros (vertice y fragmento), pesos y capas.
    // (La hierba lee alturas, parametros y pesos en su compute.)
    const auto stage_all =
        vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment | vk::ShaderStageFlagBits::eCompute;
    const auto stage_splat = vk::ShaderStageFlagBits::eFragment | vk::ShaderStageFlagBits::eCompute;
    const std::array<vk::DescriptorSetLayoutBinding, 6> bindings = {{
        {0, vk::DescriptorType::eCombinedImageSampler, 1, stage_all},
        {1, vk::DescriptorType::eUniformBuffer, 1, stage_all},
        {2, vk::DescriptorType::eCombinedImageSampler, 1, stage_splat},
        {3, vk::DescriptorType::eCombinedImageSampler, 1, stage_splat},
        {4, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eFragment},
        {5, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eFragment},
    }};
    vk::DescriptorSetLayoutCreateInfo set_info{};
    set_info.setBindings(bindings);
    set_layout_ = vk::raii::DescriptorSetLayout(device.handle(), set_info);

    const std::array<vk::DescriptorSetLayout, 2> set_layouts = {*frame_layout, *set_layout_};
    vk::PushConstantRange push{};
    push.stageFlags = vk::ShaderStageFlagBits::eVertex;
    push.size = sizeof(GpuTerrainPush);
    vk::PipelineLayoutCreateInfo layout_info{};
    layout_info.setSetLayouts(set_layouts);
    layout_info.setPushConstantRanges(push);
    layout_ = vk::raii::PipelineLayout(device.handle(), layout_info);

    // Los sets de los terrenos salen de pools que crecen bajo demanda (ver
    // allocateSets): el primero se crea ya.
    pools_.clear();
    pool_warned_ = false;
    addPool();

    vk::SamplerCreateInfo clamp{};
    clamp.magFilter = vk::Filter::eLinear;
    clamp.minFilter = vk::Filter::eLinear;
    clamp.mipmapMode = vk::SamplerMipmapMode::eNearest;
    clamp.addressModeU = clamp.addressModeV = clamp.addressModeW = vk::SamplerAddressMode::eClampToEdge;
    clamp_sampler_ = vk::raii::Sampler(device.handle(), clamp);

    createRepeatSampler();

    createPipelines(device, gbuffer_formats, depth_format, shadow_format);
    createGrassPipelines(device, frame_layout, gbuffer_formats, depth_format);
    createPatchMesh(device);
    staging_.resize(frames_in_flight);
}

// Capas (color y normal): repetidas, trilineal y con el filtrado de la
// configuracion grafica (anisotropico y sesgo de los mips).
void TerrainPass::createRepeatSampler() {
    vk::SamplerCreateInfo repeat{};
    repeat.magFilter = vk::Filter::eLinear;
    repeat.minFilter = vk::Filter::eLinear;
    repeat.mipmapMode = vk::SamplerMipmapMode::eLinear;
    repeat.addressModeU = repeat.addressModeV = repeat.addressModeW = vk::SamplerAddressMode::eRepeat;
    repeat.maxLod = VK_LOD_CLAMP_NONE;
    applyTextureFiltering(repeat, device_->physicalDevice(), filtering_);
    repeat_sampler_ = vk::raii::Sampler(device_->handle(), repeat);
}

void TerrainPass::setTextureFiltering(const TextureFiltering& filtering) {
    if (filtering == filtering_) return;
    filtering_ = filtering;
    if (device_ == nullptr || !*repeat_sampler_) return;
    createRepeatSampler();
    // Los sets que apuntaban al de antes (los retirados ya no se dibujan).
    for (auto& [id, terrain] : terrains_) {
        if (terrain && terrain->layer_set && !terrain->sets.empty()) writeDescriptors(*terrain);
    }
}

void TerrainPass::destroy() {
    // Las capas que se estaban leyendo en otro hilo crean recursos con el
    // dispositivo: que terminen antes de soltar nada.
    if (device_ != nullptr) device_->drainTasks();
    pending_layers_.clear();
    terrains_.clear();
    retired_.clear();
    layer_sets_.clear();
    grass_blocks_.clear();
    retired_grass_.clear();
    grass_pipeline_ = nullptr;
    grass_draw_layout_ = nullptr;
    grass_cull_pipeline_ = nullptr;
    grass_cull_layout_ = nullptr;
    grass_pool_ = nullptr;
    grass_set_layout_ = nullptr;
    grass_interactors_.clear();
    uploads_.clear();
    for (VulkanBuffer& buffer : staging_) buffer.destroy();
    staging_.clear();
    patch_vertices_.destroy();
    patch_indices_.destroy();
    shadow_pipeline_ = nullptr;
    gbuffer_pipeline_ = nullptr;
    gbuffer_wire_pipeline_ = nullptr;
    repeat_sampler_ = nullptr;
    clamp_sampler_ = nullptr;
    pools_.clear();
    layout_ = nullptr;
    set_layout_ = nullptr;
    device_ = nullptr;
}

void TerrainPass::createPipelines(const VulkanDevice& device, std::array<vk::Format, GBuffer::kColorAttachmentCount> gbuffer_formats,
                                  vk::Format depth_format, vk::Format shadow_format) {
    const vk::raii::ShaderModule vertex = shaders::loadModule(device, "terrain.vert.spv");
    const vk::raii::ShaderModule fragment = shaders::loadModule(device, "terrain.frag.spv");

    const vk::VertexInputBindingDescription binding{0, sizeof(float) * 3, vk::VertexInputRate::eVertex};
    const vk::VertexInputAttributeDescription attribute{0, 0, vk::Format::eR32G32B32Sfloat, 0};
    vk::PipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.setVertexBindingDescriptions(binding);
    vertex_input.setVertexAttributeDescriptions(attribute);
    vk::PipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.topology = vk::PrimitiveTopology::eTriangleList;
    vk::PipelineViewportStateCreateInfo viewport{};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    vk::PipelineMultisampleStateCreateInfo multisample{};
    multisample.rasterizationSamples = vk::SampleCountFlagBits::e1;
    const std::array<vk::DynamicState, 2> dynamic_states = {vk::DynamicState::eViewport, vk::DynamicState::eScissor};
    vk::PipelineDynamicStateCreateInfo dynamic{};
    dynamic.setDynamicStates(dynamic_states);

    // --- G-buffer ---
    {
        const std::array<vk::PipelineShaderStageCreateInfo, 2> stages = {{
            {{}, vk::ShaderStageFlagBits::eVertex, *vertex, "main"},
            {{}, vk::ShaderStageFlagBits::eFragment, *fragment, "main"},
        }};
        vk::PipelineRasterizationStateCreateInfo raster{};
        raster.polygonMode = vk::PolygonMode::eFill;
        raster.cullMode = vk::CullModeFlagBits::eNone;  // faldones y vistas por debajo
        raster.lineWidth = 1.0f;
        vk::PipelineDepthStencilStateCreateInfo depth{};
        depth.depthTestEnable = VK_TRUE;
        depth.depthWriteEnable = VK_TRUE;
        depth.depthCompareOp = vk::CompareOp::eLessOrEqual;
        std::array<vk::PipelineColorBlendAttachmentState, GBuffer::kColorAttachmentCount> blends{};
        for (auto& b : blends) {
            b.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                               vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
        }
        vk::PipelineColorBlendStateCreateInfo blend{};
        blend.setAttachments(blends);
        vk::PipelineRenderingCreateInfo rendering{};
        rendering.setColorAttachmentFormats(gbuffer_formats);
        // Modo compatible: 4 destinos (GBuffer::activeColorAttachments).
        rendering.colorAttachmentCount = GBuffer::activeColorAttachments();
        blend.attachmentCount = GBuffer::activeColorAttachments();
        rendering.depthAttachmentFormat = depth_format;
        vk::GraphicsPipelineCreateInfo info{};
        info.pNext = &rendering;
        info.setStages(stages);
        info.pVertexInputState = &vertex_input;
        info.pInputAssemblyState = &input_assembly;
        info.pViewportState = &viewport;
        info.pRasterizationState = &raster;
        info.pMultisampleState = &multisample;
        info.pDepthStencilState = &depth;
        info.pColorBlendState = &blend;
        info.pDynamicState = &dynamic;
        info.layout = *layout_;
        gbuffer_pipeline_ = compat::makeGraphicsPipeline(device, info);
        if (device.fillModeNonSolidSupported()) {
            raster.polygonMode = vk::PolygonMode::eLine;
            gbuffer_wire_pipeline_ = compat::makeGraphicsPipeline(device, info);
            raster.polygonMode = vk::PolygonMode::eFill;
        }
    }
    // --- Sombras (solo profundidad) ---
    {
        const vk::PipelineShaderStageCreateInfo stage{{}, vk::ShaderStageFlagBits::eVertex, *vertex, "main"};
        vk::PipelineRasterizationStateCreateInfo raster{};
        raster.polygonMode = vk::PolygonMode::eFill;
        raster.cullMode = vk::CullModeFlagBits::eNone;
        raster.lineWidth = 1.0f;
        raster.depthClampEnable = device.depthClampSupported() ? VK_TRUE : VK_FALSE;
        raster.depthBiasEnable = VK_TRUE;
        raster.depthBiasConstantFactor = 0.8f;
        raster.depthBiasSlopeFactor = 1.5f;
        vk::PipelineDepthStencilStateCreateInfo depth{};
        depth.depthTestEnable = VK_TRUE;
        depth.depthWriteEnable = VK_TRUE;
        depth.depthCompareOp = vk::CompareOp::eLessOrEqual;
        vk::PipelineColorBlendStateCreateInfo blend{};
        vk::PipelineRenderingCreateInfo rendering{};
        rendering.depthAttachmentFormat = shadow_format;
        vk::GraphicsPipelineCreateInfo info{};
        info.pNext = &rendering;
        info.setStages(stage);
        info.pVertexInputState = &vertex_input;
        info.pInputAssemblyState = &input_assembly;
        info.pViewportState = &viewport;
        info.pRasterizationState = &raster;
        info.pMultisampleState = &multisample;
        info.pDepthStencilState = &depth;
        info.pColorBlendState = &blend;
        info.pDynamicState = &dynamic;
        info.layout = *layout_;
        shadow_pipeline_ = compat::makeGraphicsPipeline(device, info);
    }
}

// -----------------------------------------------------------------------------
// Hierba
// -----------------------------------------------------------------------------

namespace {

// Debe coincidir con GrassParams (grass_common.glsl).
constexpr std::uint32_t kMaxGrassInteractors = 32;
struct GpuGrassParams {
    core::Vec4 grid;
    core::Vec4 origin;
    core::Vec4 shape;
    core::Vec4 base_color;
    core::Vec4 tip_color;
    core::Vec4 dry_color;
    core::Vec4 wind;
    core::Vec4 camera_pos;
    std::int32_t layers[4];
    core::Mat4 view_projection;
    core::Vec4 interactors[kMaxGrassInteractors];
    core::Vec4 sun;          // xyz = hacia el sol, w = extincion del cesped (1/m)
    core::Vec4 rings;        // x = fin del anillo 0, y = separacion del anillo 1, z = sus celdas, w = empieza a desvanecerse
    core::Vec4 ring_origin;  // xy = esquina de la rejilla del anillo 1, z = proporcion que queda lejos, w = 0
};
static_assert(sizeof(GpuGrassParams) == 768, "GpuGrassParams debe coincidir con GrassParams (std140)");
struct GpuGrassBlade {
    core::Vec4 position;
    core::Vec4 push;
};
static_assert(sizeof(GpuGrassBlade) == 32, "GpuGrassBlade debe coincidir con grass_common.glsl");
struct DrawIndirect {
    std::uint32_t vertex_count;
    std::uint32_t instance_count;
    std::uint32_t first_vertex;
    std::uint32_t first_instance;
};
constexpr std::uint32_t kGrassLists = 3;  // cerca (todo el detalle), media, lejos
// La rejilla de un anillo: como mucho 4096 celdas por lado (si no cabe, las
// celdas crecen y hay menos briznas).
constexpr float kMaxGrassCells = 4096.0f;

// Lo que cambia con el nivel del hardware. La referencia es Alto: la densidad
// y las distancias del usuario (la de detalle, como mucho 12 m).
//
//   Nivel   densidad  cerca / media / maxima (m)      tramos (listas 0/1/2)  lejos queda
//   Bajo    x0.35     6 / 14 / 45                     4 / 2 / 1              10 %
//   Medio   x0.6      8 / 18 / 60                     5 / 3 / 1              10 %
//   Alto    x1        12 / 22 / la del usuario        7 / 3 / 1              12 %
//   Ultra   x1.6      14 / 28 / la del usuario x1.3   7 / 4 / 2              12 %
//
// (En los niveles bajos, como mucho las del usuario.) Con menos briznas, algo
// mas anchas (1/sqrt de la escala de densidad): cubren lo mismo.
struct GrassRings {
    float density = 1.0f;     // briznas por m2 de cerca
    float width = 0.007f;     // m (en la base)
    float near_distance = 12.0f;  // lista 0 (todo el detalle) hasta aqui
    float mid_distance = 22.0f;   // fin del anillo 0 (la rejilla fina)
    float max_distance = 80.0f;   // fin del anillo 1 (la gruesa)
    float far_ratio = 0.12f;  // proporcion de briznas que queda lejos
    std::array<std::uint32_t, kGrassLists> segments{7, 3, 1};
};

GrassRings grassRingsFor(HardwareTier tier, const GrassDesc& g) {
    const float user_max = std::clamp(g.max_distance, 2.0f, 400.0f);
    const float user_near = std::clamp(g.near_distance, 0.5f, user_max);
    GrassRings r;
    float density_scale = 1.0f;
    switch (tier) {
        case HardwareTier::Low:
            density_scale = 0.35f;
            r.near_distance = std::min(user_near, 6.0f);
            r.mid_distance = 14.0f;
            r.max_distance = std::min(user_max, 45.0f);
            r.segments = {4, 2, 1};
            r.far_ratio = 0.10f;
            break;
        case HardwareTier::Medium:
            density_scale = 0.6f;
            r.near_distance = std::min(user_near, 8.0f);
            r.mid_distance = 18.0f;
            r.max_distance = std::min(user_max, 60.0f);
            r.segments = {5, 3, 1};
            r.far_ratio = 0.10f;
            break;
        case HardwareTier::High:
            r.near_distance = std::min(user_near, 12.0f);
            r.mid_distance = 22.0f;
            r.max_distance = user_max;
            r.segments = {7, 3, 1};
            r.far_ratio = 0.12f;
            break;
        case HardwareTier::Ultra:
            density_scale = 1.6f;
            r.near_distance = std::min(user_near * 1.2f, 14.0f);
            r.mid_distance = 28.0f;
            r.max_distance = std::min(user_max * 1.3f, 400.0f);
            r.segments = {7, 4, 2};
            r.far_ratio = 0.12f;
            break;
    }
    r.density = std::max(g.density, 0.01f) * density_scale;
    r.width = std::max(g.width, 0.0005f) / std::sqrt(density_scale);
    r.max_distance = std::max(r.max_distance, 2.0f);
    r.mid_distance = std::min(r.mid_distance, r.max_distance);
    // Siempre cerca < media: los smoothstep(near, mid) de los shaders no
    // admiten los dos bordes iguales (con la distancia maxima del usuario corta).
    r.near_distance = std::min(r.near_distance, r.mid_distance * 0.95f);
    return r;
}

// Rejilla de un anillo que cubre +-`reach` con celdas de `spacing` (que crece
// si no caben).
void grassGrid(float reach, float& spacing, float& cells) {
    cells = std::ceil(reach * 2.0f / spacing) + 1.0f;
    if (cells > kMaxGrassCells) {
        spacing = reach * 2.0f / (kMaxGrassCells - 1.0f);
        cells = kMaxGrassCells;
    }
}

}  // namespace

void TerrainPass::createGrassPipelines(const VulkanDevice& device, const vk::raii::DescriptorSetLayout& frame_layout,
                                       std::array<vk::Format, GBuffer::kColorAttachmentCount> gbuffer_formats,
                                       vk::Format depth_format) {
    const auto compute = vk::ShaderStageFlagBits::eCompute;
    const vk::ShaderStageFlags all = vk::ShaderStageFlagBits::eCompute | vk::ShaderStageFlagBits::eVertex |
                                     vk::ShaderStageFlagBits::eFragment;
    const std::array<vk::DescriptorSetLayoutBinding, 3> bindings = {{
        {0, vk::DescriptorType::eUniformBuffer, 1, all},
        {1, vk::DescriptorType::eStorageBuffer, 1, compute | vk::ShaderStageFlagBits::eVertex},
        {2, vk::DescriptorType::eStorageBuffer, 1, compute},
    }};
    vk::DescriptorSetLayoutCreateInfo set_info{};
    set_info.setBindings(bindings);
    grass_set_layout_ = vk::raii::DescriptorSetLayout(device.handle(), set_info);

    const std::uint32_t max_sets = 160 * frames_;
    const std::array<vk::DescriptorPoolSize, 2> sizes = {{
        {vk::DescriptorType::eUniformBuffer, max_sets},
        {vk::DescriptorType::eStorageBuffer, max_sets * 2},
    }};
    vk::DescriptorPoolCreateInfo pool_info{};
    pool_info.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    pool_info.maxSets = max_sets;
    pool_info.setPoolSizes(sizes);
    grass_pool_ = vk::raii::DescriptorPool(device.handle(), pool_info);

    // --- Compute: set 0 el del terreno, set 1 el de la hierba ---
    {
        const std::array<vk::DescriptorSetLayout, 2> sets = {*set_layout_, *grass_set_layout_};
        // Push: el anillo (0 = rejilla fina, 1 = gruesa).
        const vk::PushConstantRange push{compute, 0, sizeof(std::uint32_t)};
        vk::PipelineLayoutCreateInfo layout_info{};
        layout_info.setSetLayouts(sets);
        layout_info.setPushConstantRanges(push);
        grass_cull_layout_ = vk::raii::PipelineLayout(device.handle(), layout_info);
        const vk::raii::ShaderModule module = shaders::loadModule(device, "grass_cull.comp.spv");
        vk::ComputePipelineCreateInfo info{};
        info.stage = vk::PipelineShaderStageCreateInfo{{}, compute, *module, "main"};
        info.layout = *grass_cull_layout_;
        grass_cull_pipeline_ = vk::raii::Pipeline(device.handle(), device.pipelineCache(), info);
    }
    // --- Dibujo: set 0 el del frame, 1 el del terreno, 2 el de la hierba ---
    {
        const std::array<vk::DescriptorSetLayout, 3> sets = {*frame_layout, *set_layout_, *grass_set_layout_};
        vk::PushConstantRange push{vk::ShaderStageFlagBits::eVertex, 0, sizeof(std::uint32_t)};
        vk::PipelineLayoutCreateInfo layout_info{};
        layout_info.setSetLayouts(sets);
        layout_info.setPushConstantRanges(push);
        grass_draw_layout_ = vk::raii::PipelineLayout(device.handle(), layout_info);

        const vk::raii::ShaderModule vertex = shaders::loadModule(device, "grass.vert.spv");
        const vk::raii::ShaderModule fragment = shaders::loadModule(device, "grass.frag.spv");
        const std::array<vk::PipelineShaderStageCreateInfo, 2> stages = {{
            {{}, vk::ShaderStageFlagBits::eVertex, *vertex, "main"},
            {{}, vk::ShaderStageFlagBits::eFragment, *fragment, "main"},
        }};
        vk::PipelineVertexInputStateCreateInfo vertex_input{};  // sin vertices: salen del indice
        vk::PipelineInputAssemblyStateCreateInfo input_assembly{};
        input_assembly.topology = vk::PrimitiveTopology::eTriangleList;
        vk::PipelineViewportStateCreateInfo viewport{};
        viewport.viewportCount = 1;
        viewport.scissorCount = 1;
        vk::PipelineRasterizationStateCreateInfo raster{};
        raster.polygonMode = vk::PolygonMode::eFill;
        raster.cullMode = vk::CullModeFlagBits::eNone;  // las briznas se ven por las dos caras
        raster.lineWidth = 1.0f;
        vk::PipelineMultisampleStateCreateInfo multisample{};
        multisample.rasterizationSamples = vk::SampleCountFlagBits::e1;
        vk::PipelineDepthStencilStateCreateInfo depth{};
        depth.depthTestEnable = VK_TRUE;
        depth.depthWriteEnable = VK_TRUE;
        depth.depthCompareOp = vk::CompareOp::eLessOrEqual;
        std::array<vk::PipelineColorBlendAttachmentState, GBuffer::kColorAttachmentCount> blends{};
        for (auto& b : blends) {
            b.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                               vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
        }
        vk::PipelineColorBlendStateCreateInfo blend{};
        blend.setAttachments(blends);
        const std::array<vk::DynamicState, 2> dynamic_states = {vk::DynamicState::eViewport, vk::DynamicState::eScissor};
        vk::PipelineDynamicStateCreateInfo dynamic{};
        dynamic.setDynamicStates(dynamic_states);
        vk::PipelineRenderingCreateInfo rendering{};
        rendering.setColorAttachmentFormats(gbuffer_formats);
        // Modo compatible: 4 destinos (GBuffer::activeColorAttachments).
        rendering.colorAttachmentCount = GBuffer::activeColorAttachments();
        blend.attachmentCount = GBuffer::activeColorAttachments();
        rendering.depthAttachmentFormat = depth_format;
        vk::GraphicsPipelineCreateInfo info{};
        info.pNext = &rendering;
        info.setStages(stages);
        info.pVertexInputState = &vertex_input;
        info.pInputAssemblyState = &input_assembly;
        info.pViewportState = &viewport;
        info.pRasterizationState = &raster;
        info.pMultisampleState = &multisample;
        info.pDepthStencilState = &depth;
        info.pColorBlendState = &blend;
        info.pDynamicState = &dynamic;
        info.layout = *grass_draw_layout_;
        grass_pipeline_ = compat::makeGraphicsPipeline(device, info);
    }
}

// Bloques de hierba: un juego de buffers de briznas (~96 MB con los valores
// por defecto) que se presta a las teselas mas cercanas. Antes cada tesela
// tenia el suyo (y lo creaba con un waitIdle): con muchas teselas vivas era
// mucha VRAM para hierba que solo se ve hasta max_distance.
bool TerrainPass::ensureGrass(Terrain& t, std::uint32_t id) {
    const GrassDesc& g = t.desc.grass;
    // Tres listas del mismo tamano (la lista N empieza en N x capacity).
    const std::uint32_t capacity = std::clamp<std::uint32_t>(g.max_blades / kGrassLists, 1024, 4000000);
    if (t.grass && t.grass->capacity == capacity) return true;
    if (t.grass) releaseGrass(t);
    // Uno libre (y que ya no use ningun frame en vuelo) de su capacidad...
    for (GrassBlock& block : grass_blocks_) {
        if (block.owner == 0 && block.cooldown == 0 && block.grass && block.grass->capacity == capacity) {
            block.owner = id;
            t.grass = block.grass.get();
            return true;
        }
    }
    // ...uno nuevo si cabe...
    if (grass_pool_size_ == 0 || grass_blocks_.size() < grass_pool_size_) {
        std::unique_ptr<Grass> grass = createGrassBlock(capacity);
        if (!grass) return false;
        t.grass = grass.get();
        grass_blocks_.push_back(GrassBlock{std::move(grass), id, 0});
        return true;
    }
    // ...o uno libre de otra capacidad, rehecho (el viejo se retira sin parar la GPU).
    for (GrassBlock& block : grass_blocks_) {
        if (block.owner != 0 || block.cooldown != 0) continue;
        std::unique_ptr<Grass> grass = createGrassBlock(capacity);
        if (!grass) return false;
        if (block.grass) retired_grass_.push_back(RetiredGrass{std::move(block.grass), frames_ + 1});
        block.grass = std::move(grass);
        block.owner = id;
        t.grass = block.grass.get();
        return true;
    }
    return false;  // todos prestados: esta tesela (la mas lejana) sin hierba
}

void TerrainPass::releaseGrass(Terrain& t) {
    if (t.grass == nullptr) return;
    for (GrassBlock& block : grass_blocks_) {
        if (block.grass.get() == t.grass) {
            block.owner = 0;
            block.cooldown = frames_ + 1;  // algun frame en vuelo aun lo usa
            break;
        }
    }
    t.grass = nullptr;
}

std::unique_ptr<TerrainPass::Grass> TerrainPass::createGrassBlock(std::uint32_t capacity) {
    auto grass = std::make_unique<Grass>();
    grass->capacity = capacity;
    grass->blades.create(*device_, static_cast<vk::DeviceSize>(capacity) * kGrassLists * sizeof(GpuGrassBlade),
                         vk::BufferUsageFlagBits::eStorageBuffer, vk::MemoryPropertyFlagBits::eDeviceLocal);
    grass->args.create(*device_, sizeof(DrawIndirect) * kGrassLists,
                       vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eIndirectBuffer |
                           vk::BufferUsageFlagBits::eTransferDst,
                       vk::MemoryPropertyFlagBits::eDeviceLocal);
    grass->params.resize(frames_);
    for (VulkanBuffer& b : grass->params) {
        b.create(*device_, sizeof(GpuGrassParams), vk::BufferUsageFlagBits::eUniformBuffer,
                 vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
    }
    const std::vector<vk::DescriptorSetLayout> layouts(frames_, *grass_set_layout_);
    vk::DescriptorSetAllocateInfo alloc{};
    alloc.descriptorPool = *grass_pool_;
    alloc.setSetLayouts(layouts);
    try {
        grass->sets = vk::raii::DescriptorSets(device_->handle(), alloc);
    } catch (const std::exception&) {
        return nullptr;  // sin descriptores (demasiados bloques)
    }
    for (std::uint32_t f = 0; f < frames_; ++f) {
        const vk::DescriptorBufferInfo params{*grass->params[f].handle(), 0, sizeof(GpuGrassParams)};
        const vk::DescriptorBufferInfo blades{*grass->blades.handle(), 0, VK_WHOLE_SIZE};
        const vk::DescriptorBufferInfo args{*grass->args.handle(), 0, VK_WHOLE_SIZE};
        const std::array<vk::WriteDescriptorSet, 3> writes = {{
            {*grass->sets[f], 0, 0, 1, vk::DescriptorType::eUniformBuffer, nullptr, &params},
            {*grass->sets[f], 1, 0, 1, vk::DescriptorType::eStorageBuffer, nullptr, &blades},
            {*grass->sets[f], 2, 0, 1, vk::DescriptorType::eStorageBuffer, nullptr, &args},
        }};
        device_->handle().updateDescriptorSets(writes, nullptr);
    }
    return grass;
}

void TerrainPass::setGrassLighting(const core::Vec3& to_sun) {
    const float length = std::sqrt(to_sun.x * to_sun.x + to_sun.y * to_sun.y + to_sun.z * to_sun.z);
    if (!(length > 1e-6f)) return;  // tambien NaN
    grass_to_sun_ = core::Vec3{to_sun.x / length, to_sun.y / length, to_sun.z / length};
}

void TerrainPass::setGrassInteractors(const std::vector<core::Vec4>& spheres) {
    grass_interactors_.assign(spheres.begin(), spheres.begin() + std::min<std::size_t>(spheres.size(), kMaxGrassInteractors));
}

void TerrainPass::recordGrassCull(const vk::raii::CommandBuffer& cmd, std::uint32_t frame,
                                  const core::Vec3& camera_position, const core::Mat4& view_projection,
                                  float delta_seconds) {
    if (device_ != nullptr && !device_->drawIndirectFirstInstanceSupported()) return;  // sin firstInstance (algunos moviles)
    grass_previous_seconds_ = grass_seconds_;
    grass_seconds_ += std::clamp(delta_seconds, 0.0f, 0.1f);
    if (!*grass_cull_pipeline_) return;

    // Bloques: los soltados vuelven a estar libres cuando ningun frame en
    // vuelo los usa; los retirados se destruyen entonces.
    for (GrassBlock& block : grass_blocks_) {
        if (block.grass) block.grass->culled = false;
        if (block.owner == 0 && block.cooldown > 0) --block.cooldown;
    }
    for (RetiredGrass& r : retired_grass_) --r.frames;
    std::erase_if(retired_grass_, [](const RetiredGrass& r) { return r.frames == 0; });

    // Solo las teselas que tocan el circulo de la hierba (distancia en XZ de
    // la camara a su rectangulo <= distancia maxima): las demas ni lanzan el
    // compute (cada una recorria toda la rejilla de la camara) ni ocupan un
    // bloque. Las mas cercanas primero: si no hay bloques para todas, se
    // quedan sin hierba las lejanas.
    struct Candidate {
        float distance = 0.0f;
        std::uint32_t id = 0;
        Terrain* terrain = nullptr;
    };
    std::vector<Candidate> candidates;
    for (auto& [id, terrain] : terrains_) {
        Terrain& t = *terrain;
        const GrassDesc& g = t.desc.grass;
        bool wanted = g.enabled && t.desc.visible && frame < t.sets.size() && t.layer_set && !t.needs_clear;
        float distance = 0.0f;
        if (wanted) {
            const float x0 = t.desc.origin.x;
            const float z0 = t.desc.origin.z;
            const float x1 = x0 + t.desc.size;
            const float z1 = z0 + t.desc.size;
            const float dx = std::max({x0 - camera_position.x, 0.0f, camera_position.x - x1});
            const float dz = std::max({z0 - camera_position.z, 0.0f, camera_position.z - z1});
            distance = std::sqrt(dx * dx + dz * dz);
            // +1 m de margen: el fundido de las briznas llega justo al maximo.
            wanted = distance <= grassRingsFor(grass_tier_, g).max_distance + 1.0f;
        }
        if (wanted) {
            candidates.push_back(Candidate{distance, id, &t});
        } else if (t.grass) {
            releaseGrass(t);
        }
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) { return a.distance < b.distance; });
    if (grass_pool_size_ > 0 && candidates.size() > grass_pool_size_) {
        for (std::size_t i = grass_pool_size_; i < candidates.size(); ++i) releaseGrass(*candidates[i].terrain);
        candidates.resize(grass_pool_size_);
    }
    // Pool reducido (CVar): fuera los bloques libres que sobran.
    while (grass_pool_size_ > 0 && grass_blocks_.size() > grass_pool_size_) {
        const auto free_block = std::find_if(grass_blocks_.begin(), grass_blocks_.end(),
                                             [](const GrassBlock& b) { return b.owner == 0; });
        if (free_block == grass_blocks_.end()) break;
        if (free_block->grass) retired_grass_.push_back(RetiredGrass{std::move(free_block->grass), frames_ + 1});
        grass_blocks_.erase(free_block);
    }

    for (const Candidate& candidate : candidates) {
        const std::uint32_t id = candidate.id;
        Terrain& t = *candidate.terrain;
        const GrassDesc& g = t.desc.grass;
        if (!ensureGrass(t, id)) continue;
        Grass& grass = *t.grass;

        // Dos rejillas: una celda por brizna posible. El anillo 0 (hasta
        // `mid`) con la densidad de cerca; el 1 (la corona hasta `max`) ya con
        // la de lejos: con una sola rejilla fina casi todos los hilos lejanos
        // se descartaban.
        const GrassRings r = grassRingsFor(grass_tier_, g);
        const float max_distance = r.max_distance;
        const float mid_distance = r.mid_distance;
        float spacing = 1.0f / std::sqrt(r.density);
        float cells = 0.0f;
        grassGrid(mid_distance, spacing, cells);
        float far_spacing = 1.0f / std::sqrt(r.density * r.far_ratio);
        float far_cells = 0.0f;
        if (mid_distance < max_distance) {
            grassGrid(max_distance, far_spacing, far_cells);
        }
        GpuGrassParams p{};
        p.grid = core::Vec4{spacing, cells, max_distance, r.near_distance};
        p.origin = core::Vec4{camera_position.x - mid_distance, camera_position.z - mid_distance,
                              std::clamp(g.threshold, 0.0f, 0.99f), static_cast<float>(grass.capacity)};
        p.shape = core::Vec4{g.height, g.height_variation, r.width, g.bend};
        p.base_color = core::Vec4{g.base_color.x, g.base_color.y, g.base_color.z, g.color_variation};
        p.tip_color = core::Vec4{g.tip_color.x, g.tip_color.y, g.tip_color.z, g.wind};
        p.dry_color = core::Vec4{g.dry_color.x, g.dry_color.y, g.dry_color.z, g.interaction};
        const float wind_angle = g.wind_direction * 3.14159265f / 180.0f;
        p.wind = core::Vec4{std::cos(wind_angle), std::sin(wind_angle), grass_seconds_, grass_previous_seconds_};
        p.camera_pos = core::Vec4{camera_position.x, camera_position.y, camera_position.z, static_cast<float>(id % 97)};
        p.layers[0] = g.layer;
        p.layers[1] = g.dry_layer;
        p.layers[2] = static_cast<std::int32_t>(grass_interactors_.size());
        p.view_projection = view_projection;
        for (std::size_t i = 0; i < grass_interactors_.size(); ++i) p.interactors[i] = grass_interactors_[i];
        // Extincion del cesped (1/m): cuanta hoja se cruza por metro (~2 con
        // los valores por defecto). La auto-sombra y la oclusion de grass.frag.
        const float sigma = std::clamp(r.density * r.width * 0.6f, 0.5f, 12.0f);
        p.sun = core::Vec4{grass_to_sun_.x, grass_to_sun_.y, grass_to_sun_.z, sigma};
        // El desvanecido empieza antes del maximo (smoothstep con bordes iguales: indefinido).
        const float fade_start = std::min(std::max(mid_distance, max_distance * 0.7f), max_distance * 0.98f);
        p.rings = core::Vec4{mid_distance, far_spacing, far_cells, fade_start};
        p.ring_origin = core::Vec4{camera_position.x - max_distance, camera_position.z - max_distance, r.far_ratio, 0.0f};
        grass.params[frame].write(&p, sizeof(p));

        // Contadores a cero (despues de que el frame anterior dibujara).
        vk::MemoryBarrier2 before{};
        before.srcStageMask = vk::PipelineStageFlagBits2::eDrawIndirect | vk::PipelineStageFlagBits2::eVertexShader;
        before.srcAccessMask = vk::AccessFlagBits2::eIndirectCommandRead | vk::AccessFlagBits2::eShaderStorageRead;
        before.dstStageMask = vk::PipelineStageFlagBits2::eTransfer | vk::PipelineStageFlagBits2::eComputeShader;
        before.dstAccessMask = vk::AccessFlagBits2::eTransferWrite | vk::AccessFlagBits2::eShaderStorageWrite;
        compat::pipelineBarrier(cmd, vk::DependencyInfo{}.setMemoryBarriers(before));
        const std::array<DrawIndirect, kGrassLists> reset = {{{r.segments[0] * 6 + 3, 0, 0, 0},
                                                              {r.segments[1] * 6 + 3, 0, 0, grass.capacity},
                                                              {r.segments[2] * 6 + 3, 0, 0, grass.capacity * 2}}};
        cmd.updateBuffer<DrawIndirect>(*grass.args.handle(), 0, reset);
        vk::MemoryBarrier2 cleared{};
        cleared.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
        cleared.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
        cleared.dstStageMask = vk::PipelineStageFlagBits2::eComputeShader;
        cleared.dstAccessMask = vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite;
        compat::pipelineBarrier(cmd, vk::DependencyInfo{}.setMemoryBarriers(cleared));

        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *grass_cull_pipeline_);
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *grass_cull_layout_, 0,
                               {*t.sets[frame], *grass.sets[frame]}, nullptr);
        const auto groups = static_cast<std::uint32_t>((cells + 7.0f) / 8.0f);
        cmd.pushConstants<std::uint32_t>(*grass_cull_layout_, vk::ShaderStageFlagBits::eCompute, 0, 0u);
        cmd.dispatch(groups, groups, 1);
        if (far_cells > 0.0f) {
            // Los dos anillos suman en los mismos contadores.
            vk::MemoryBarrier2 ring{};
            ring.srcStageMask = vk::PipelineStageFlagBits2::eComputeShader;
            ring.srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite;
            ring.dstStageMask = vk::PipelineStageFlagBits2::eComputeShader;
            ring.dstAccessMask = vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite;
            compat::pipelineBarrier(cmd, vk::DependencyInfo{}.setMemoryBarriers(ring));
            const auto far_groups = static_cast<std::uint32_t>((far_cells + 7.0f) / 8.0f);
            cmd.pushConstants<std::uint32_t>(*grass_cull_layout_, vk::ShaderStageFlagBits::eCompute, 0, 1u);
            cmd.dispatch(far_groups, far_groups, 1);
        }

        vk::MemoryBarrier2 done{};
        done.srcStageMask = vk::PipelineStageFlagBits2::eComputeShader;
        done.srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite;
        done.dstStageMask = vk::PipelineStageFlagBits2::eDrawIndirect | vk::PipelineStageFlagBits2::eVertexShader;
        done.dstAccessMask = vk::AccessFlagBits2::eIndirectCommandRead | vk::AccessFlagBits2::eShaderStorageRead;
        compat::pipelineBarrier(cmd, vk::DependencyInfo{}.setMemoryBarriers(done));
        grass.culled = true;
    }
}

void TerrainPass::recordGrassGBuffer(const vk::raii::CommandBuffer& cmd, std::uint32_t frame,
                                     const vk::raii::DescriptorSet& frame_set) const {
    if (device_ != nullptr && !device_->drawIndirectFirstInstanceSupported()) return;  // sin firstInstance (algunos moviles)
    bool bound = false;
    for (const auto& [id, terrain] : terrains_) {
        const Terrain& t = *terrain;
        if (!t.grass || !t.grass->culled || frame >= t.grass->sets.size()) continue;
        if (!bound) {
            cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *grass_pipeline_);
            cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *grass_draw_layout_, 0, *frame_set, nullptr);
            bound = true;
        }
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *grass_draw_layout_, 1,
                               {*t.sets[frame], *t.grass->sets[frame]}, nullptr);
        // Los tramos de cada lista: los mismos que puso recordGrassCull en
        // vertex_count (el nivel no cambia dentro del frame).
        const GrassRings r = grassRingsFor(grass_tier_, t.desc.grass);
        for (std::uint32_t list = 0; list < kGrassLists; ++list) {
            cmd.pushConstants<std::uint32_t>(*grass_draw_layout_, vk::ShaderStageFlagBits::eVertex, 0, r.segments[list]);
            cmd.drawIndirect(*t.grass->args.handle(), sizeof(DrawIndirect) * list, 1, sizeof(DrawIndirect));
        }
    }
}

// Malla de un trozo: (kPatchCells + 1)^2 vertices (x, z en 0..1, faldon 0)
// y un anillo de faldon (faldon 1) que baja desde el borde.
void TerrainPass::createPatchMesh(const VulkanDevice& device) {
    constexpr std::uint32_t n = kPatchCells + 1;
    std::vector<float> vertices;
    vertices.reserve(n * n * 3 + n * 4 * 3);
    for (std::uint32_t z = 0; z < n; ++z) {
        for (std::uint32_t x = 0; x < n; ++x) {
            vertices.push_back(static_cast<float>(x) / kPatchCells);
            vertices.push_back(static_cast<float>(z) / kPatchCells);
            vertices.push_back(0.0f);
        }
    }
    std::vector<std::uint32_t> indices;
    for (std::uint32_t z = 0; z < kPatchCells; ++z) {
        for (std::uint32_t x = 0; x < kPatchCells; ++x) {
            const std::uint32_t a = z * n + x;
            const std::uint32_t b = a + 1;
            const std::uint32_t c = a + n;
            const std::uint32_t d = c + 1;
            indices.insert(indices.end(), {a, c, b, b, c, d});
        }
    }
    // Faldones: por cada lado, una tira del borde hacia abajo.
    const auto border = [&](std::uint32_t i, int side) {
        switch (side) {
            case 0: return i;                      // z = 0
            case 1: return (n - 1) * n + i;        // z = 1
            case 2: return i * n;                  // x = 0
            default: return i * n + (n - 1);       // x = 1
        }
    };
    for (int side = 0; side < 4; ++side) {
        const auto first = static_cast<std::uint32_t>(vertices.size() / 3);
        for (std::uint32_t i = 0; i < n; ++i) {
            const std::uint32_t top = border(i, side);
            vertices.push_back(vertices[top * 3]);
            vertices.push_back(vertices[top * 3 + 1]);
            vertices.push_back(1.0f);
        }
        for (std::uint32_t i = 0; i + 1 < n; ++i) {
            const std::uint32_t t0 = border(i, side);
            const std::uint32_t t1 = border(i + 1, side);
            const std::uint32_t s0 = first + i;
            const std::uint32_t s1 = first + i + 1;
            indices.insert(indices.end(), {t0, s0, t1, t1, s0, s1});
        }
    }
    patch_vertices_ = VulkanBuffer::createDeviceLocal(device, vertices.data(), vertices.size() * sizeof(float),
                                                      vk::BufferUsageFlagBits::eVertexBuffer);
    patch_indices_ = VulkanBuffer::createDeviceLocal(device, indices.data(), indices.size() * sizeof(std::uint32_t),
                                                     vk::BufferUsageFlagBits::eIndexBuffer);
    patch_index_count_ = static_cast<std::uint32_t>(indices.size());
}

// -----------------------------------------------------------------------------
// Terrenos
// -----------------------------------------------------------------------------

void TerrainPass::addPool() {
    // 64 terrenos (teselas de un TerrainGrid) por pool, con un set por frame en vuelo.
    const std::uint32_t max_sets = 64 * frames_;
    const std::array<vk::DescriptorPoolSize, 2> sizes = {{
        {vk::DescriptorType::eCombinedImageSampler, max_sets * 5},
        {vk::DescriptorType::eUniformBuffer, max_sets},
    }};
    vk::DescriptorPoolCreateInfo pool_info{};
    pool_info.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    pool_info.maxSets = max_sets;
    pool_info.setPoolSizes(sizes);
    pools_.emplace_back(device_->handle(), pool_info);
}

bool TerrainPass::allocateSets(std::vector<vk::raii::DescriptorSet>& sets) {
    std::vector<vk::DescriptorSetLayout> layouts(frames_, *set_layout_);
    vk::DescriptorSetAllocateInfo allocate{};
    allocate.setSetLayouts(layouts);
    // Primero los pools que ya hay (los mas nuevos suelen tener sitio)...
    for (auto it = pools_.rbegin(); it != pools_.rend(); ++it) {
        allocate.descriptorPool = **it;
        try {
            sets = vk::raii::DescriptorSets(device_->handle(), allocate);
            return true;
        } catch (const std::exception&) {
            // lleno o fragmentado: el siguiente
        }
    }
    // ...y si todos estan llenos, uno nuevo.
    try {
        addPool();
        allocate.descriptorPool = *pools_.back();
        sets = vk::raii::DescriptorSets(device_->handle(), allocate);
        return true;
    } catch (const std::exception& e) {
        if (!pool_warned_) {
            std::cerr << "[TerrainPass] no se pudieron reservar descriptores para un terreno ("
                      << terrains_.size() << " vivos, " << retired_.size() << " retirados): " << e.what() << '\n';
            pool_warned_ = true;
        }
        return false;
    }
}

std::uint32_t TerrainPass::createTerrain(std::uint32_t resolution, std::uint32_t splat_resolution) {
    if (device_ == nullptr || resolution < 3 || splat_resolution < 2) return 0;
    auto terrain = std::make_unique<Terrain>();
    // Los descriptores antes que las imagenes: si no hay, no se crea nada (0).
    if (!allocateSets(terrain->sets)) return 0;
    const std::uint32_t id = next_id_++;
    terrain->resolution = resolution;
    terrain->splat_resolution = splat_resolution;
    const auto usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst;
    terrain->heights.create(*device_, vk::Extent2D{resolution, resolution}, kHeightFormat, usage,
                            vk::ImageAspectFlagBits::eColor);
    terrain->splat0.create(*device_, vk::Extent2D{splat_resolution, splat_resolution}, kSplatFormat, usage,
                           vk::ImageAspectFlagBits::eColor);
    terrain->splat1.create(*device_, vk::Extent2D{splat_resolution, splat_resolution}, kSplatFormat, usage,
                           vk::ImageAspectFlagBits::eColor);
    // Contenido valido (plano, capa 0) con un clear en el command buffer del
    // siguiente frame (recordUploads), antes que sus datos: ni submitOneTime
    // (esperaba a la GPU) ni una subida plana que luego se repetia con los
    // datos de verdad. Hasta entonces no se dibuja.
    terrain->needs_clear = true;
    terrain->params.resize(frames_);
    for (VulkanBuffer& buffer : terrain->params) {
        buffer.create(*device_, sizeof(GpuTerrainParams), vk::BufferUsageFlagBits::eUniformBuffer,
                      vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
    }
    terrains_.emplace(id, std::move(terrain));
    // Las capas llegan con el primer setDesc (hasta entonces no se dibuja).
    return id;
}

std::shared_ptr<PreparedTerrain> TerrainPass::prepareTerrainAsync(const TerrainPixels& pixels) const {
    // Solo lee lo fijo desde create() (dispositivo, frames en vuelo, copia de
    // la CPU): se puede llamar desde cualquier hilo.
    const VulkanDevice* device = device_;
    const std::uint32_t res = pixels.resolution;
    const std::uint32_t splat_res = pixels.splat_resolution;
    if (device == nullptr || res < 3 || splat_res < 2) return nullptr;
    const std::size_t height_count = static_cast<std::size_t>(res) * res;
    const std::size_t splat_bytes = static_cast<std::size_t>(splat_res) * splat_res * 4;
    if (!pixels.heights || pixels.heights->size() < height_count || !pixels.splat0 ||
        pixels.splat0->size() < splat_bytes || !pixels.splat1 || pixels.splat1->size() < splat_bytes) {
        return nullptr;
    }
    try {
        auto prepared = std::make_shared<PreparedTerrain>();
        prepared->resolution = res;
        prepared->splat_resolution = splat_res;
        const auto usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst;
        prepared->heights.create(*device, vk::Extent2D{res, res}, kHeightFormat, usage, vk::ImageAspectFlagBits::eColor);
        prepared->splat0.create(*device, vk::Extent2D{splat_res, splat_res}, kSplatFormat, usage,
                                vk::ImageAspectFlagBits::eColor);
        prepared->splat1.create(*device, vk::Extent2D{splat_res, splat_res}, kSplatFormat, usage,
                                vk::ImageAspectFlagBits::eColor);
        prepared->params.resize(frames_);
        for (VulkanBuffer& buffer : prepared->params) {
            buffer.create(*device, sizeof(GpuTerrainParams), vk::BufferUsageFlagBits::eUniformBuffer,
                          vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
        }
        // Staging de usar y tirar (vive hasta que la GPU termine la copia).
        const vk::DeviceSize height_bytes = height_count * sizeof(float);
        auto staging = std::make_shared<VulkanBuffer>();
        staging->create(*device, height_bytes + splat_bytes * 2, vk::BufferUsageFlagBits::eTransferSrc,
                        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
        staging->write(pixels.heights->data(), height_bytes, 0);
        staging->write(pixels.splat0->data(), splat_bytes, height_bytes);
        staging->write(pixels.splat1->data(), splat_bytes, height_bytes + splat_bytes);

        const vk::Buffer source = *staging->handle();
        const std::array<std::pair<vk::Image, vk::DeviceSize>, 3> targets = {{
            {*prepared->heights.handle(), 0},
            {*prepared->splat0.handle(), height_bytes},
            {*prepared->splat1.handle(), height_bytes + splat_bytes},
        }};
        // Lo envia y no espera: el hilo principal lo adopta cuando la fence
        // ya esta (preparedTerrainReady). Si nadie lo adopta, las imagenes
        // viven en el ticket hasta que la GPU termina.
        prepared->ticket = device->submitAsync(
            [&](const vk::raii::CommandBuffer& cmd) {
                for (std::size_t i = 0; i < targets.size(); ++i) {
                    barrier(cmd, targets[i].first, vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
                            vk::PipelineStageFlagBits2::eTopOfPipe, vk::AccessFlagBits2::eNone,
                            vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite);
                    const std::uint32_t side = i == 0 ? res : splat_res;
                    vk::BufferImageCopy region{};
                    region.bufferOffset = targets[i].second;
                    region.imageSubresource = vk::ImageSubresourceLayers{vk::ImageAspectFlagBits::eColor, 0, 0, 1};
                    region.imageExtent = vk::Extent3D{side, side, 1};
                    cmd.copyBufferToImage(source, targets[i].first, vk::ImageLayout::eTransferDstOptimal, region);
                    barrier(cmd, targets[i].first, vk::ImageLayout::eTransferDstOptimal,
                            vk::ImageLayout::eShaderReadOnlyOptimal, vk::PipelineStageFlagBits2::eTransfer,
                            vk::AccessFlagBits2::eTransferWrite, kTerrainReadStages,
                            vk::AccessFlagBits2::eShaderSampledRead);
                }
            },
            {staging, prepared});
        if (keep_cpu_copy_) prepared->pixels = pixels;  // para los rayos: los mismos buffers, sin copiar
        prepared->pixels.resolution = res;
        prepared->pixels.splat_resolution = splat_res;
        return prepared;
    } catch (const std::exception& e) {
        std::cerr << "[TerrainPass] No se pudo preparar un terreno de " << res << "x" << res << ": " << e.what() << "\n";
        return nullptr;
    }
}

bool TerrainPass::preparedTerrainReady(const std::shared_ptr<PreparedTerrain>& prepared) {
    return prepared && !prepared->adopted && prepared->ticket && prepared->ticket->done();
}

std::uint32_t TerrainPass::adoptTerrain(const std::shared_ptr<PreparedTerrain>& prepared) {
    if (device_ == nullptr || !preparedTerrainReady(prepared)) return 0;
    PreparedTerrain& p = *prepared;
    if (p.params.size() != frames_) return 0;
    auto terrain = std::make_unique<Terrain>();
    // Sin descriptores ahora: se reintenta (el preparado sigue valido).
    if (!allocateSets(terrain->sets)) return 0;
    const std::uint32_t id = next_id_++;
    terrain->resolution = p.resolution;
    terrain->splat_resolution = p.splat_resolution;
    terrain->heights = std::move(p.heights);
    terrain->splat0 = std::move(p.splat0);
    terrain->splat1 = std::move(p.splat1);
    terrain->params = std::move(p.params);
    if (keep_cpu_copy_ && p.pixels.heights && p.pixels.splat0 && p.pixels.splat1) {
        // Los mismos buffers del snapshot (sin copiar): solo se copian si se
        // esculpe mientras alguien mas los tiene.
        terrain->cpu_heights = std::move(p.pixels.heights);
        terrain->cpu_splat0 = std::move(p.pixels.splat0);
        terrain->cpu_splat1 = std::move(p.pixels.splat1);
        terrain->height_revision = 1;
        terrain->look_revision = 1;
    }
    p.pixels = TerrainPixels{};
    p.adopted = true;
    terrains_.emplace(id, std::move(terrain));
    // Las capas llegan con el primer setDesc (hasta entonces no se dibuja).
    return id;
}

void TerrainPass::setUploadBudget(std::size_t bytes_per_frame) {
    // Como poco 256 KB: una fila de cualquier terreno cabe de sobra.
    const std::size_t budget = bytes_per_frame == 0 ? 0 : std::max<std::size_t>(bytes_per_frame, std::size_t{256} << 10);
    if (budget == upload_budget_) return;
    upload_budget_ = budget;
    // Los staging se rehacen a su tamano en el siguiente recordUploads de
    // cada hueco (el de un frame en vuelo no se toca hasta que vuelve a tocarle).
}

std::size_t TerrainPass::pendingUploadBytes() const {
    std::size_t total = 0;
    for (const Upload& u : uploads_) {
        const std::size_t row = static_cast<std::size_t>(u.w) * (u.kind == 0 ? sizeof(float) : 8);
        total += row * (u.h - u.done_rows);
    }
    return total;
}

void TerrainPass::destroyTerrain(std::uint32_t id) {
    const auto it = terrains_.find(id);
    if (it == terrains_.end()) return;
    // Su bloque de hierba vuelve al pool (cuando ningun frame en vuelo lo use).
    releaseGrass(*it->second);
    // Algun frame en vuelo puede estar usandolo: se borra unos frames despues.
    retired_.push_back(Retired{std::move(it->second), nullptr, frames_ + 1});
    terrains_.erase(it);
    uploads_.erase(std::remove_if(uploads_.begin(), uploads_.end(), [&](const Upload& u) { return u.id == id; }),
                   uploads_.end());
}

void TerrainPass::setDesc(std::uint32_t id, const TerrainDesc& desc) {
    const auto it = terrains_.find(id);
    if (it == terrains_.end()) return;
    Terrain& t = *it->second;
    t.desc = desc;
    if (t.desc.layers.size() > kMaxTerrainLayers) t.desc.layers.resize(kMaxTerrainLayers);
    // Las texturas de las capas solo se recargan si cambian sus rutas.
    bool changed = !t.layer_set;
    for (std::uint32_t i = 0; i < kMaxTerrainLayers; ++i) {
        const std::filesystem::path albedo = i < t.desc.layers.size() ? t.desc.layers[i].albedo : std::filesystem::path{};
        const std::filesystem::path normal = i < t.desc.layers.size() ? t.desc.layers[i].normal : std::filesystem::path{};
        changed = changed || albedo != t.albedo_paths[i] || normal != t.normal_paths[i];
    }
    // Sin parar la GPU: si ya habia capas, el cambio pone sets nuevos y retira
    // los de antes (un frame en vuelo puede estar usandolos).
    if (changed) requestLayerTextures(t);
    // Lo que cambia el color del terreno para los rayos: capas, tintes y hierba.
    std::uint64_t signature = 1469598103934665603ull ^ t.desc.layers.size();
    const auto mix_float = [&](float f) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &f, sizeof(bits));
        signature = (signature ^ bits) * 1099511628211ull;
    };
    for (const TerrainLayerDesc& layer : t.desc.layers) {
        for (const float f : {layer.tint.x, layer.tint.y, layer.tint.z}) mix_float(f);
    }
    const GrassDesc& grass = t.desc.grass;
    for (const float f : {grass.enabled ? 1.0f : 0.0f, static_cast<float>(grass.layer), static_cast<float>(grass.dry_layer),
                          grass.threshold, grass.base_color.x, grass.base_color.y, grass.base_color.z, grass.tip_color.x,
                          grass.tip_color.y, grass.tip_color.z, grass.dry_color.x, grass.dry_color.y,
                          grass.dry_color.z}) {
        mix_float(f);
    }
    if (signature != t.look_signature) {
        t.look_signature = signature;
        ++t.look_revision;
    }
}

std::shared_ptr<GpuTicket> TerrainPass::createArrayTexture(const VulkanDevice& device, ArrayTexture& texture,
                                                           const std::vector<std::vector<std::uint8_t>>& layers,
                                                           std::shared_ptr<const void> owner) {
    const auto count = static_cast<std::uint32_t>(layers.size());
    const std::uint32_t mips = static_cast<std::uint32_t>(std::bit_width(kLayerSize));
    texture = ArrayTexture{};
    vk::ImageCreateInfo info{};
    info.imageType = vk::ImageType::e2D;
    info.format = kLayerFormat;
    info.extent = vk::Extent3D{kLayerSize, kLayerSize, 1};
    info.mipLevels = mips;
    info.arrayLayers = count;
    info.samples = vk::SampleCountFlagBits::e1;
    info.tiling = vk::ImageTiling::eOptimal;
    info.usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst |
                 vk::ImageUsageFlagBits::eTransferSrc;
    info.initialLayout = vk::ImageLayout::eUndefined;
    texture.image = vk::raii::Image(device.handle(), info);
    const vk::MemoryRequirements requirements = texture.image.getMemoryRequirements();
    vk::MemoryAllocateInfo allocate{};
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = device.findMemoryType(requirements.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal);
    texture.memory = vk::raii::DeviceMemory(device.handle(), allocate);
    texture.image.bindMemory(*texture.memory, 0);

    const vk::DeviceSize layer_bytes = static_cast<vk::DeviceSize>(kLayerSize) * kLayerSize * 4;
    auto staging = std::make_shared<VulkanBuffer>();
    staging->create(device, layer_bytes * count, vk::BufferUsageFlagBits::eTransferSrc,
                    vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
    for (std::uint32_t i = 0; i < count; ++i) staging->write(layers[i].data(), layer_bytes, layer_bytes * i);

    const vk::Image image = *texture.image;
    const vk::Buffer source = *staging->handle();
    // Sin esperar: el staging vive en el ticket hasta que la GPU termina; las
    // capas no se ponen a ningun terreno hasta entonces (pollAsync).
    std::shared_ptr<GpuTicket> ticket = device.submitAsync(
        [&](const vk::raii::CommandBuffer& cmd) {
            barrier(cmd, image, vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
                    vk::PipelineStageFlagBits2::eTopOfPipe, vk::AccessFlagBits2::eNone, vk::PipelineStageFlagBits2::eTransfer,
                    vk::AccessFlagBits2::eTransferWrite, 0, mips, count);
            std::vector<vk::BufferImageCopy> regions;
            for (std::uint32_t i = 0; i < count; ++i) {
                vk::BufferImageCopy region{};
                region.bufferOffset = layer_bytes * i;
                region.imageSubresource = vk::ImageSubresourceLayers{vk::ImageAspectFlagBits::eColor, 0, i, 1};
                region.imageExtent = vk::Extent3D{kLayerSize, kLayerSize, 1};
                regions.push_back(region);
            }
            cmd.copyBufferToImage(source, image, vk::ImageLayout::eTransferDstOptimal, regions);
            // Mipmaps: cada nivel desde el anterior (blit lineal), todas las capas.
            std::int32_t size = static_cast<std::int32_t>(kLayerSize);
            for (std::uint32_t level = 1; level < mips; ++level) {
                barrier(cmd, image, vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eTransferSrcOptimal,
                        vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite,
                        vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead, level - 1, 1, count);
                vk::ImageBlit blit{};
                blit.srcSubresource = vk::ImageSubresourceLayers{vk::ImageAspectFlagBits::eColor, level - 1, 0, count};
                blit.srcOffsets[1] = vk::Offset3D{size, size, 1};
                const std::int32_t next = std::max(size / 2, 1);
                blit.dstSubresource = vk::ImageSubresourceLayers{vk::ImageAspectFlagBits::eColor, level, 0, count};
                blit.dstOffsets[1] = vk::Offset3D{next, next, 1};
                cmd.blitImage(image, vk::ImageLayout::eTransferSrcOptimal, image, vk::ImageLayout::eTransferDstOptimal,
                              blit, vk::Filter::eLinear);
                barrier(cmd, image, vk::ImageLayout::eTransferSrcOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
                        vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead,
                        vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderSampledRead, level - 1,
                        1, count);
                size = next;
            }
            barrier(cmd, image, vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
                    vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite,
                    vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderSampledRead, mips - 1, 1,
                    count);
        },
        {staging, std::move(owner)});

    vk::ImageViewCreateInfo view{};
    view.image = image;
    view.viewType = vk::ImageViewType::e2DArray;
    view.format = kLayerFormat;
    view.subresourceRange = vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, mips, 0, count};
    texture.view = vk::raii::ImageView(device.handle(), view);
    return ticket;
}

std::shared_ptr<TerrainPass::LayerSet> TerrainPass::buildLayerSet(const VulkanDevice& device,
                                                                  const LayerPaths& albedo_paths,
                                                                  const LayerPaths& normal_paths,
                                                                  std::vector<std::shared_ptr<GpuTicket>>& tickets) {
    auto set = std::make_shared<LayerSet>();
    std::vector<std::vector<std::uint8_t>> albedo(kMaxTerrainLayers);
    std::vector<std::vector<std::uint8_t>> normal(kMaxTerrainLayers);
    for (std::uint32_t i = 0; i < kMaxTerrainLayers; ++i) {
        set->albedo_paths[i] = albedo_paths[i];
        set->normal_paths[i] = normal_paths[i];
        asset::ImageRgba8 image;
        if (!set->albedo_paths[i].empty() && asset::loadImageRgba8(set->albedo_paths[i], image, 2048)) {
            albedo[i] = resizeSquare(image);
        } else {
            if (!set->albedo_paths[i].empty()) {
                std::cerr << "[Terreno] No se pudo leer " << set->albedo_paths[i].string() << "\n";
                set->albedo_paths[i].clear();
            }
            albedo[i] = solidLayer(255, 255, 255, 255);
        }
        if (!set->normal_paths[i].empty() && asset::loadImageRgba8(set->normal_paths[i], image, 2048)) {
            normal[i] = resizeSquare(image);
        } else {
            set->normal_paths[i].clear();
            normal[i] = solidLayer(128, 128, 255, 255);
        }
    }
    // `set` vive en los tickets hasta que la GPU termina: si algo falla
    // despues, sus imagenes no se destruyen con la copia en vuelo.
    tickets.push_back(createArrayTexture(device, set->albedo, albedo, set));
    tickets.push_back(createArrayTexture(device, set->normal, normal, set));
    // Color medio de cada capa (lo que se ve de lejos, y lo que ven los rayos).
    for (std::uint32_t i = 0; i < kMaxTerrainLayers; ++i) {
        double sum[3] = {0.0, 0.0, 0.0};
        const std::vector<std::uint8_t>& pixels = albedo[i];
        const std::size_t count = pixels.size() / 4;
        for (std::size_t p = 0; p < count; ++p) {
            for (std::size_t c = 0; c < 3; ++c) sum[c] += pixels[p * 4 + c];
        }
        const double scale = count > 0 ? 1.0 / (255.0 * static_cast<double>(count)) : 0.0;
        set->average[i] = core::Vec3{static_cast<float>(sum[0] * scale), static_cast<float>(sum[1] * scale),
                                     static_cast<float>(sum[2] * scale)};
    }
    return set;
}

void TerrainPass::requestLayerTextures(Terrain& terrain) {
    for (std::uint32_t i = 0; i < kMaxTerrainLayers; ++i) {
        const TerrainLayerDesc* layer = i < terrain.desc.layers.size() ? &terrain.desc.layers[i] : nullptr;
        terrain.albedo_paths[i] = layer != nullptr ? layer->albedo : std::filesystem::path{};
        terrain.normal_paths[i] = layer != nullptr ? layer->normal : std::filesystem::path{};
    }
    const std::string key = layerKey(terrain.albedo_paths, terrain.normal_paths);
    // Otro terreno con las mismas texturas (las teselas de una rejilla): se
    // comparten, ni se vuelven a leer ni ocupan VRAM otra vez.
    if (const auto found = layer_sets_.find(key); found != layer_sets_.end()) {
        found->second.unused_frames = 0;
        if (terrain.layer_set == found->second.set || swapLayerSet(terrain, found->second.set)) {
            terrain.pending_layers.clear();
        } else {
            terrain.pending_layers = key;  // sin descriptores ahora: se reintenta en pollAsync
        }
        return;
    }
    if (terrain.pending_layers == key && pending_layers_.contains(key)) return;  // ya se estan leyendo
    terrain.pending_layers = key;
    if (pending_layers_.contains(key)) return;

    if (!async_layers_) {
        // Como antes: leer y subir ahora mismo (espera a la copia, no a toda la GPU).
        std::vector<std::shared_ptr<GpuTicket>> tickets;
        std::shared_ptr<LayerSet> set = buildLayerSet(*device_, terrain.albedo_paths, terrain.normal_paths, tickets);
        for (const auto& ticket : tickets) ticket->wait();
        layer_sets_[key] = LayerEntry{set, 0};
        if (swapLayerSet(terrain, set)) terrain.pending_layers.clear();
        return;
    }

    // En segundo plano: leer del disco, reescalar y crear/subir las texturas
    // (submitAsync). Solo ve sus rutas (copiadas) y su PendingLayers.
    auto pending = std::make_shared<PendingLayers>();
    pending_layers_.emplace(key, pending);
    const VulkanDevice* device = device_;
    device_->runTask([device, pending, albedo = terrain.albedo_paths, normal = terrain.normal_paths]() {
        try {
            std::vector<std::shared_ptr<GpuTicket>> tickets;
            std::shared_ptr<LayerSet> set = buildLayerSet(*device, albedo, normal, tickets);
            pending->set = std::move(set);
            pending->tickets = std::move(tickets);
            pending->state.store(1, std::memory_order_release);
        } catch (const std::exception& e) {
            std::cerr << "[Terreno] No se pudieron cargar las capas: " << e.what() << "\n";
            pending->state.store(2, std::memory_order_release);
        }
    });
}

bool TerrainPass::swapLayerSet(Terrain& terrain, std::shared_ptr<LayerSet> set) {
    if (!set) return false;
    if (terrain.layer_set == set) return true;
    if (terrain.layer_set) {
        // Ya se dibujaba: sus sets pueden estar en un frame en vuelo y no se
        // pueden reescribir. Sets nuevos, y los de antes (con sus capas) se
        // retiran unos frames (antes: waitIdle).
        std::vector<vk::raii::DescriptorSet> fresh;
        if (!allocateSets(fresh)) return false;
        std::swap(terrain.sets, fresh);
        retired_.push_back(Retired{nullptr, terrain.layer_set, frames_ + 1, std::move(fresh)});
    }
    // (Sin capas aun no se dibujo nunca: sus sets no estan en ningun frame.)
    terrain.layer_set = std::move(set);
    ++terrain.look_revision;
    writeDescriptors(terrain);
    return true;
}

void TerrainPass::pollAsync() {
    // Capas leidas en otro hilo: listas cuando su copia termino en la GPU.
    for (auto it = pending_layers_.begin(); it != pending_layers_.end();) {
        PendingLayers& pending = *it->second;
        const int state = pending.state.load(std::memory_order_acquire);
        if (state == 0) {
            ++it;
            continue;
        }
        if (state == 2) {
            // Fallo (sin memoria...): los que no tenian capas lo vuelven a
            // pedir con su siguiente setDesc; los que si, siguen con las suyas.
            for (auto& [id, terrain] : terrains_) {
                if (terrain->pending_layers == it->first) terrain->pending_layers.clear();
            }
            it = pending_layers_.erase(it);
            continue;
        }
        bool done = true;
        for (const auto& ticket : pending.tickets) done = done && (!ticket || ticket->done());
        if (!done) {
            ++it;
            continue;
        }
        layer_sets_[it->first] = LayerEntry{pending.set, 0};
        it = pending_layers_.erase(it);
    }
    // Terrenos esperando unas capas que ya estan.
    for (auto& [id, terrain] : terrains_) {
        if (terrain->pending_layers.empty()) continue;
        const auto found = layer_sets_.find(terrain->pending_layers);
        if (found == layer_sets_.end()) continue;
        if (swapLayerSet(*terrain, found->second.set)) terrain->pending_layers.clear();
    }
}

void TerrainPass::writeDescriptors(Terrain& terrain) {
    for (std::uint32_t frame = 0; frame < terrain.sets.size(); ++frame) {
        const vk::DescriptorImageInfo heights{*clamp_sampler_, *terrain.heights.view(), vk::ImageLayout::eShaderReadOnlyOptimal};
        const vk::DescriptorBufferInfo params{*terrain.params[frame].handle(), 0, sizeof(GpuTerrainParams)};
        const vk::DescriptorImageInfo splat0{*clamp_sampler_, *terrain.splat0.view(), vk::ImageLayout::eShaderReadOnlyOptimal};
        const vk::DescriptorImageInfo splat1{*clamp_sampler_, *terrain.splat1.view(), vk::ImageLayout::eShaderReadOnlyOptimal};
        const vk::DescriptorImageInfo albedo{*repeat_sampler_, *terrain.layer_set->albedo.view, vk::ImageLayout::eShaderReadOnlyOptimal};
        const vk::DescriptorImageInfo normal{*repeat_sampler_, *terrain.layer_set->normal.view, vk::ImageLayout::eShaderReadOnlyOptimal};
        const vk::DescriptorSet set = *terrain.sets[frame];
        const std::array<vk::WriteDescriptorSet, 6> writes = {{
            {set, 0, 0, 1, vk::DescriptorType::eCombinedImageSampler, &heights},
            {set, 1, 0, 1, vk::DescriptorType::eUniformBuffer, nullptr, &params},
            {set, 2, 0, 1, vk::DescriptorType::eCombinedImageSampler, &splat0},
            {set, 3, 0, 1, vk::DescriptorType::eCombinedImageSampler, &splat1},
            {set, 4, 0, 1, vk::DescriptorType::eCombinedImageSampler, &albedo},
            {set, 5, 0, 1, vk::DescriptorType::eCombinedImageSampler, &normal},
        }};
        device_->handle().updateDescriptorSets(writes, nullptr);
    }
}

// -----------------------------------------------------------------------------
// Subidas
// -----------------------------------------------------------------------------

void TerrainPass::updateHeights(std::uint32_t id, const float* heights, std::uint32_t x, std::uint32_t y,
                                std::uint32_t w, std::uint32_t h) {
    const auto it = terrains_.find(id);
    if (it == terrains_.end() || heights == nullptr || w == 0 || h == 0) return;
    const std::uint32_t res = it->second->resolution;
    x = std::min(x, res - 1);
    y = std::min(y, res - 1);
    w = std::min(w, res - x);
    h = std::min(h, res - y);
    Upload upload{id, 0, x, y, w, h, {}};
    upload.bytes.resize(static_cast<std::size_t>(w) * h * sizeof(float));
    for (std::uint32_t row = 0; row < h; ++row) {
        std::memcpy(upload.bytes.data() + static_cast<std::size_t>(row) * w * sizeof(float),
                    heights + static_cast<std::size_t>(y + row) * res + x, w * sizeof(float));
    }
    uploads_.push_back(std::move(upload));
    if (keep_cpu_copy_) {
        Terrain& t = *it->second;
        // Solo la region (la copia entera solo si otro la comparte: el
        // snapshot de la tesela o un hilo haciendo la malla de los rayos).
        std::vector<float>& cpu = writableCopy(t.cpu_heights, t.cpu_heights_owned, static_cast<std::size_t>(res) * res, 0.0f);
        for (std::uint32_t row = 0; row < h; ++row) {
            const std::size_t at = static_cast<std::size_t>(y + row) * res + x;
            std::memcpy(cpu.data() + at, heights + at, w * sizeof(float));
        }
        ++t.height_revision;
    }
}

void TerrainPass::updateSplat(std::uint32_t id, const std::uint8_t* splat0, const std::uint8_t* splat1,
                              std::uint32_t x, std::uint32_t y, std::uint32_t w, std::uint32_t h) {
    const auto it = terrains_.find(id);
    if (it == terrains_.end() || splat0 == nullptr || splat1 == nullptr || w == 0 || h == 0) return;
    const std::uint32_t res = it->second->splat_resolution;
    x = std::min(x, res - 1);
    y = std::min(y, res - 1);
    w = std::min(w, res - x);
    h = std::min(h, res - y);
    Upload upload{id, 1, x, y, w, h, {}};
    const std::size_t plane = static_cast<std::size_t>(w) * h * 4;
    upload.bytes.resize(plane * 2);
    for (std::uint32_t row = 0; row < h; ++row) {
        const std::size_t source = (static_cast<std::size_t>(y + row) * res + x) * 4;
        std::memcpy(upload.bytes.data() + static_cast<std::size_t>(row) * w * 4, splat0 + source, w * 4);
        std::memcpy(upload.bytes.data() + plane + static_cast<std::size_t>(row) * w * 4, splat1 + source, w * 4);
    }
    uploads_.push_back(std::move(upload));
    if (keep_cpu_copy_) {
        Terrain& t = *it->second;
        const std::size_t splat_size = static_cast<std::size_t>(res) * res * 4;
        // Los dos mapas comparten la bandera: si hay que copiar uno, los dos.
        bool owned0 = t.cpu_splat_owned;
        bool owned1 = t.cpu_splat_owned;
        std::vector<std::uint8_t>& cpu0 = writableCopy(t.cpu_splat0, owned0, splat_size, std::uint8_t{0});
        std::vector<std::uint8_t>& cpu1 = writableCopy(t.cpu_splat1, owned1, splat_size, std::uint8_t{0});
        t.cpu_splat_owned = owned0 && owned1;
        for (std::uint32_t row = 0; row < h; ++row) {
            const std::size_t at = (static_cast<std::size_t>(y + row) * res + x) * 4;
            std::memcpy(cpu0.data() + at, splat0 + at, w * 4);
            std::memcpy(cpu1.data() + at, splat1 + at, w * 4);
        }
        ++t.look_revision;
    }
}

std::vector<TerrainPass::RayTracingSource> TerrainPass::rayTracingSources() const {
    std::vector<RayTracingSource> out;
    for (const auto& [id, terrain] : terrains_) {
        const Terrain& t = *terrain;
        const std::size_t heights = static_cast<std::size_t>(t.resolution) * t.resolution;
        const std::size_t splat = static_cast<std::size_t>(t.splat_resolution) * t.splat_resolution * 4;
        if (!t.cpu_heights || !t.cpu_splat0 || !t.cpu_splat1 || t.cpu_heights->size() < heights ||
            t.cpu_splat0->size() < splat || t.cpu_splat1->size() < splat) {
            continue;
        }
        RayTracingSource s;
        s.id = id;
        s.resolution = t.resolution;
        s.splat_resolution = t.splat_resolution;
        s.origin = t.desc.origin;
        s.size = t.desc.size;
        s.max_height = t.desc.max_height;
        s.visible = t.desc.visible;
        s.heights = t.cpu_heights->data();
        s.splat0 = t.cpu_splat0->data();
        s.splat1 = t.cpu_splat1->data();
        s.heights_data = t.cpu_heights;
        s.splat0_data = t.cpu_splat0;
        s.splat1_data = t.cpu_splat1;
        s.layer_count = static_cast<std::uint32_t>(std::min<std::size_t>(t.desc.layers.size(), kMaxTerrainLayers));
        for (std::uint32_t i = 0; i < kMaxTerrainLayers; ++i) {
            // Sin textura, terrain.frag pinta su tinte con manchas de media ~1.
            const core::Vec3 tint = i < t.desc.layers.size() ? t.desc.layers[i].tint : core::Vec3{1.0f, 1.0f, 1.0f};
            const core::Vec3 base = t.layer_set ? t.layer_set->average[i] : core::Vec3{1.0f, 1.0f, 1.0f};
            s.layer_color[i] = core::Vec3{base.x * tint.x, base.y * tint.y, base.z * tint.z};
        }
        s.height_revision = t.height_revision;
        s.look_revision = t.look_revision;
        const GrassDesc& grass = t.desc.grass;
        if (grass.enabled && grass.layer >= 0 && static_cast<std::uint32_t>(grass.layer) < kMaxTerrainLayers) {
            s.grass = true;
            s.grass_layer = static_cast<std::uint32_t>(grass.layer);
            s.grass_dry_layer = grass.dry_layer;
            s.grass_threshold = grass.threshold;
            // La brizna es casi toda del color de la punta (grass.vert); la
            // base solo asoma entre ellas.
            s.grass_color = grass.base_color * 0.15f + grass.tip_color * 0.85f;
            s.grass_dry_color = grass.dry_color;
        }
        out.push_back(s);
    }
    return out;
}

bool TerrainPass::recordUploads(const vk::raii::CommandBuffer& cmd, std::uint32_t frame) {
    last_upload_bytes_ = 0;
    bool heights_changed = false;

    // Terrenos nuevos de createTerrain: contenido valido (plano, capa 0) antes
    // que nada. Un clear en la GPU, sin staging ni esperas.
    for (auto& [id, terrain] : terrains_) {
        Terrain& t = *terrain;
        if (!t.needs_clear) continue;
        const auto clear = [&](const VulkanImage& image, const std::array<float, 4>& value) {
            const vk::Image handle = *image.handle();
            barrier(cmd, handle, vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
                    vk::PipelineStageFlagBits2::eTopOfPipe, vk::AccessFlagBits2::eNone,
                    vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite);
            cmd.clearColorImage(handle, vk::ImageLayout::eTransferDstOptimal, vk::ClearColorValue{value},
                                vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1});
            barrier(cmd, handle, vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
                    vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite, kTerrainReadStages,
                    vk::AccessFlagBits2::eShaderSampledRead);
        };
        clear(t.heights, {0.0f, 0.0f, 0.0f, 0.0f});
        clear(t.splat0, {1.0f, 0.0f, 0.0f, 0.0f});
        clear(t.splat1, {0.0f, 0.0f, 0.0f, 0.0f});
        t.needs_clear = false;
        heights_changed = true;
    }
    if (uploads_.empty() || frame >= staging_.size()) return heights_changed;

    // Cada plano empieza alineado a 16 bytes en el staging.
    const auto align = [](vk::DeviceSize bytes) { return (bytes + 15) & ~static_cast<vk::DeviceSize>(15); };
    const auto rowBytes = [](const Upload& u) { return static_cast<vk::DeviceSize>(u.w) * 4; };  // por plano
    const auto planesOf = [](const Upload& u) { return static_cast<vk::DeviceSize>(u.kind == 0 ? 1 : 2); };

    // Staging de este hueco: con tope, de tamano fijo (el tope) y lo que no
    // quepa sigue el frame siguiente (bandas de filas); sin tope (0), como
    // antes: crece hasta caber todo.
    const bool limited = upload_budget_ > 0;
    vk::DeviceSize wanted = 0;
    if (limited) {
        wanted = align(upload_budget_);
        // Una fila de la primera subida siempre cabe (como poco una por frame).
        const Upload& first = uploads_.front();
        wanted = std::max(wanted, planesOf(first) * align(rowBytes(first)));
    } else {
        for (const Upload& u : uploads_) wanted += planesOf(u) * align(rowBytes(u) * (u.h - u.done_rows));
    }
    VulkanBuffer& staging = staging_[frame];
    // (La fence de este hueco ya se espero: su staging no esta en vuelo.)
    // (Con tope se rehace si no cabe o si sobra mucho: el tope bajo.)
    const bool recreate = !staging.isValid() || staging.size() < wanted || (limited && staging.size() > wanted * 2);
    if (recreate) {
        staging.destroy();
        staging.create(*device_, limited ? wanted : std::max<vk::DeviceSize>(wanted * 2, 1 << 20),
                       vk::BufferUsageFlagBits::eTransferSrc,
                       vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
    }
    const vk::DeviceSize capacity = limited ? wanted : staging.size();

    vk::DeviceSize offset = 0;
    std::size_t finished = 0;  // subidas completas (prefijo de uploads_)
    for (Upload& u : uploads_) {
        const auto it = terrains_.find(u.id);
        if (it == terrains_.end()) {
            ++finished;
            continue;
        }
        Terrain& t = *it->second;
        const vk::DeviceSize row = rowBytes(u);
        const vk::DeviceSize planes = planesOf(u);
        const std::uint32_t remaining = u.h - u.done_rows;
        // Filas que caben en lo que queda del staging de este frame.
        std::uint32_t rows = remaining;
        if (limited) {
            const vk::DeviceSize space = capacity > offset ? capacity - offset : 0;
            rows = static_cast<std::uint32_t>(std::min<vk::DeviceSize>(remaining, space / planes / row));
            while (rows > 0 && planes * align(row * rows) > space) --rows;
            if (rows == 0) {
                if (offset > 0) break;  // el resto, el frame siguiente
                rows = 1;               // como poco una fila por frame (cabe: ver `wanted`)
            }
        }
        const vk::DeviceSize band = row * rows;
        const vk::DeviceSize plane_total = row * u.h;  // pesos: splat0 entero y luego splat1
        for (vk::DeviceSize p = 0; p < planes; ++p) {
            staging.write(u.bytes.data() + p * plane_total + static_cast<vk::DeviceSize>(u.done_rows) * row, band,
                          offset + p * align(band));
        }
        const auto copy = [&](const VulkanImage& image, vk::DeviceSize at) {
            const vk::Image handle = *image.handle();
            barrier(cmd, handle, vk::ImageLayout::eShaderReadOnlyOptimal, vk::ImageLayout::eTransferDstOptimal,
                    kTerrainReadStages, vk::AccessFlagBits2::eShaderSampledRead, vk::PipelineStageFlagBits2::eTransfer,
                    vk::AccessFlagBits2::eTransferWrite);
            vk::BufferImageCopy region{};
            region.bufferOffset = at;
            region.imageSubresource = vk::ImageSubresourceLayers{vk::ImageAspectFlagBits::eColor, 0, 0, 1};
            region.imageOffset = vk::Offset3D{static_cast<std::int32_t>(u.x), static_cast<std::int32_t>(u.y + u.done_rows), 0};
            region.imageExtent = vk::Extent3D{u.w, rows, 1};
            cmd.copyBufferToImage(*staging.handle(), handle, vk::ImageLayout::eTransferDstOptimal, region);
            barrier(cmd, handle, vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
                    vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite, kTerrainReadStages,
                    vk::AccessFlagBits2::eShaderSampledRead);
        };
        if (u.kind == 0) {
            copy(t.heights, offset);
            heights_changed = true;
        } else {
            copy(t.splat0, offset);
            copy(t.splat1, offset + align(band));
        }
        offset += planes * align(band);
        last_upload_bytes_ += static_cast<std::size_t>(planes * band);
        u.done_rows += rows;
        if (u.done_rows < u.h) break;  // tope de este frame
        ++finished;
    }
    uploads_.erase(uploads_.begin(), uploads_.begin() + static_cast<std::ptrdiff_t>(finished));
    return heights_changed;
}

// -----------------------------------------------------------------------------
// Trozos (LOD)
// -----------------------------------------------------------------------------

namespace {

// Recorre el arbol de trozos de un terreno: el LOD lo decide solo la
// distancia a `camera`, y `keep` recorta (una caja por nodo). Asi el G-buffer
// y los mapas de las luces locales eligen el MISMO detalle para un mismo
// sitio aunque recorten con volumenes distintos.
template <typename Keep, typename Visit>
void walkChunks(const TerrainDesc& d, std::uint32_t resolution, const core::Vec3& camera, Keep&& keep,
                Visit&& visit) {
    if (!d.visible || resolution < 3) return;
    // El trozo mas pequeno: kPatchCells texeles de alturas por lado.
    const float min_size = std::min(1.0f, static_cast<float>(kPatchCells) / static_cast<float>(resolution - 1));
    struct Node {
        float u, v, size;
    };
    std::vector<Node> stack = {{0.0f, 0.0f, 1.0f}};
    while (!stack.empty()) {
        const Node node = stack.back();
        stack.pop_back();
        const core::Vec3 lo{d.origin.x + node.u * d.size, d.origin.y, d.origin.z + node.v * d.size};
        const core::Vec3 hi{lo.x + node.size * d.size, d.origin.y + d.max_height, lo.z + node.size * d.size};
        if (!keep(core::Aabb{lo, hi})) continue;
        // Distancia de la camara a la caja del trozo.
        const core::Vec3 closest{std::clamp(camera.x, lo.x, hi.x), std::clamp(camera.y, lo.y, hi.y),
                                 std::clamp(camera.z, lo.z, hi.z)};
        const float distance = core::length(closest - camera);
        const float world_size = node.size * d.size;
        if (node.size > min_size * 1.001f && distance < world_size * std::max(d.lod_distance, 0.5f)) {
            const float half = node.size * 0.5f;
            stack.push_back({node.u, node.v, half});
            stack.push_back({node.u + half, node.v, half});
            stack.push_back({node.u, node.v + half, half});
            stack.push_back({node.u + half, node.v + half, half});
            continue;
        }
        // Faldon: lo bastante para tapar el salto con el vecino de otro LOD.
        const float cell = world_size / kPatchCells;
        visit(node.u, node.v, node.size, cell * 2.0f + d.max_height * 0.02f + 0.5f);
    }
}

}  // namespace

void TerrainPass::selectChunks(Terrain& terrain, const core::Vec3& camera, const core::Mat4& view_projection) {
    terrain.chunks.clear();
    const core::Frustum frustum(view_projection);
    walkChunks(
        terrain.desc, terrain.resolution, camera, [&](const core::Aabb& box) { return frustum.intersects(box); },
        [&](float u, float v, float size, float skirt) { terrain.chunks.push_back(Chunk{u, v, size, skirt}); });
}

std::uint64_t TerrainPass::localSignature(const core::Vec3& position, float range) const {
    // FNV-1a de los trozos (con su LOD) que tocan la caja de la luz.
    std::uint64_t hash = 1469598103934665603ull;
    const auto mix = [&hash](std::uint64_t value) {
        hash ^= value;
        hash *= 1099511628211ull;
    };
    const core::Vec3 r{range, range, range};
    const core::Vec3 light_lo = position - r;
    const core::Vec3 light_hi = position + r;
    for (const auto& [id, terrain] : terrains_) {
        if (!terrain->desc.cast_shadows) continue;
        mix(id);
        walkChunks(
            terrain->desc, terrain->resolution, lod_camera_,
            [&](const core::Aabb& box) {
                // El faldon cuelga por debajo del origen (como mucho el ancho
                // del trozo).
                const float skirt_bottom = box.min.y - (box.max.x - box.min.x);
                return box.min.x <= light_hi.x && box.max.x >= light_lo.x && skirt_bottom <= light_hi.y &&
                       box.max.y >= light_lo.y && box.min.z <= light_hi.z && box.max.z >= light_lo.z;
            },
            [&](float u, float v, float size, float) {
                mix(std::bit_cast<std::uint32_t>(u));
                mix(std::bit_cast<std::uint32_t>(v));
                mix(std::bit_cast<std::uint32_t>(size));
            });
    }
    return hash;
}

void TerrainPass::prepare(std::uint32_t frame, const core::Vec3& camera_position, const core::Mat4& view_projection) {
    lod_camera_ = camera_position;
    // Lo quitado hace frames_ + 1 frames ya no lo usa ningun frame en vuelo.
    for (Retired& r : retired_) --r.frames;
    std::erase_if(retired_, [](const Retired& r) { return r.frames == 0; });
    // Capas que terminaron de leerse/subirse en otro hilo.
    pollAsync();
    // Las capas que ya no usa nadie se guardan un rato (teselas que vuelven).
    std::erase_if(layer_sets_, [](auto& entry) {
        LayerEntry& e = entry.second;
        if (!e.set || e.set.use_count() > 1) {
            e.unused_frames = 0;
            return !e.set;
        }
        return ++e.unused_frames > kLayerKeepFrames;
    });
    for (auto& [id, terrain] : terrains_) {
        if (!terrain->layer_set || terrain->needs_clear) {
            terrain->chunks.clear();  // sin capas o sin contenido aun: no se dibuja
            continue;
        }
        selectChunks(*terrain, camera_position, view_projection);
        if (frame >= terrain->params.size()) continue;
        const TerrainDesc& d = terrain->desc;
        GpuTerrainParams params{};
        params.origin_size = core::Vec4{d.origin.x, d.origin.y, d.origin.z, d.size};
        params.info = core::Vec4{d.max_height, static_cast<float>(terrain->resolution),
                                 1.0f / static_cast<float>(terrain->resolution),
                                 static_cast<float>(std::max<std::size_t>(d.layers.size(), 1))};
        for (std::uint32_t i = 0; i < kMaxTerrainLayers; ++i) {
            const TerrainLayerDesc layer = i < d.layers.size() ? d.layers[i] : TerrainLayerDesc{};
            params.layer_params[i] = core::Vec4{std::max(layer.tiling, 0.01f), layer.roughness, layer.metallic,
                                                layer.normal_strength};
            params.layer_tint[i] = core::Vec4{layer.tint.x, layer.tint.y, layer.tint.z,
                                              terrain->layer_set->albedo_paths[i].empty() ? 0.0f : 1.0f};
            params.layer_extra[i] =
                core::Vec4{terrain->layer_set->normal_paths[i].empty() ? 0.0f : 1.0f, 0.0f, 0.0f, 0.0f};
        }
        // Suelo del cesped: donde crece la hierba, el suelo toma su color
        // (oscuro bajo las briznas de cerca, el de las puntas lejos).
        params.grass_floor = core::Vec4{-1.0f, 0.0f, 0.0f, 0.0f};
        const GrassDesc& grass = d.grass;
        const std::size_t layer_count = std::max<std::size_t>(d.layers.size(), 1);
        if (grass.enabled && grass.layer >= 0 && static_cast<std::size_t>(grass.layer) < layer_count &&
            static_cast<std::uint32_t>(grass.layer) < kMaxTerrainLayers) {
            const GrassRings r = grassRingsFor(grass_tier_, grass);
            // z = donde empiezan a desvanecerse las briznas (lo mismo que en
            // recordGrassCull): el tinte del suelo se apaga con ellas.
            const float fade_start =
                std::min(std::max(r.mid_distance, r.max_distance * 0.7f), r.max_distance * 0.98f);
            params.grass_floor = core::Vec4{static_cast<float>(grass.layer), 0.85f, fade_start, r.max_distance};
            const core::Vec3 canopy = grass.base_color * 0.25f + grass.tip_color * 0.75f;
            params.grass_floor_color = core::Vec4{canopy.x, canopy.y, canopy.z, std::clamp(grass.threshold, 0.0f, 0.99f)};
        }
        terrain->params[frame].write(&params, sizeof(params));
    }
}

void TerrainPass::prepareView(const core::Vec3& camera_position, const core::Mat4& view_projection,
                              bool include) {
    for (auto& [id, terrain] : terrains_) {
        if (!include || !terrain->layer_set || terrain->needs_clear) {
            terrain->chunks.clear();
            continue;
        }
        selectChunks(*terrain, camera_position, view_projection);
    }
}

void TerrainPass::recordGBuffer(const vk::raii::CommandBuffer& cmd, std::uint32_t frame,
                                const vk::raii::DescriptorSet& frame_set) const {
    bool bound = false;
    for (const auto& [id, terrain] : terrains_) {
        if (terrain->chunks.empty() || frame >= terrain->sets.size()) continue;
        if (!bound) {
            cmd.bindPipeline(vk::PipelineBindPoint::eGraphics,
                     wireframe_ && *gbuffer_wire_pipeline_ ? *gbuffer_wire_pipeline_ : *gbuffer_pipeline_);
            cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *layout_, 0, *frame_set, nullptr);
            cmd.bindVertexBuffers(0, *patch_vertices_.handle(), {0});
            cmd.bindIndexBuffer(*patch_indices_.handle(), 0, vk::IndexType::eUint32);
            bound = true;
        }
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *layout_, 1, *terrain->sets[frame], nullptr);
        for (const Chunk& chunk : terrain->chunks) {
            GpuTerrainPush push{};
            push.chunk = core::Vec4{chunk.u, chunk.v, chunk.size, chunk.skirt};
            cmd.pushConstants<GpuTerrainPush>(*layout_, vk::ShaderStageFlagBits::eVertex, 0, push);
            cmd.drawIndexed(patch_index_count_, 1, 0, 0, 0);
        }
    }
}

void TerrainPass::recordShadow(const vk::raii::CommandBuffer& cmd, std::uint32_t frame,
                               const vk::raii::DescriptorSet& frame_set, const core::Mat4& light_view_projection,
                               bool local) const {
    bool bound = false;
    const core::Frustum light_frustum(light_view_projection, /*ignore_near=*/!local);
    for (const auto& [id, terrain] : terrains_) {
        if (terrain->chunks.empty() || !terrain->desc.cast_shadows || frame >= terrain->sets.size()) continue;
        if (!bound) {
            cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *shadow_pipeline_);
            cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *layout_, 0, *frame_set, nullptr);
            cmd.bindVertexBuffers(0, *patch_vertices_.handle(), {0});
            cmd.bindIndexBuffer(*patch_indices_.handle(), 0, vk::IndexType::eUint32);
            bound = true;
        }
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *layout_, 1, *terrain->sets[frame], nullptr);
        const auto draw = [&](float u, float v, float size, float skirt) {
            GpuTerrainPush push{};
            push.chunk = core::Vec4{u, v, size, skirt};
            push.light_view_projection = light_view_projection;
            push.shadow = 1;
            cmd.pushConstants<GpuTerrainPush>(*layout_, vk::ShaderStageFlagBits::eVertex, 0, push);
            cmd.drawIndexed(patch_index_count_, 1, 0, 0, 0);
        };
        if (local) {
            // Luz local: sus propios trozos, recortados por SU volumen pero con
            // el LOD de la camara. Con los trozos de la camara faltaba lo que
            // queda fuera de pantalla y, como el mapa se cachea, al moverse la
            // camara el terreno cambiaba de LOD y el del mapa no: el terreno se
            // sombreaba a si mismo a manchas (ruido). Ver localSignature().
            walkChunks(
                terrain->desc, terrain->resolution, lod_camera_,
                [&](const core::Aabb& box) {
                    // El faldon cuelga por debajo del origen (como mucho el
                    // ancho del trozo).
                    const core::Vec3 skirt{0.0f, box.max.x - box.min.x, 0.0f};
                    return light_frustum.intersects(core::Aabb{box.min - skirt, box.max});
                },
                draw);
        } else {
            for (const Chunk& chunk : terrain->chunks) draw(chunk.u, chunk.v, chunk.size, chunk.skirt);
        }
    }
}

std::uint32_t TerrainPass::chunkCount() const {
    std::uint32_t count = 0;
    for (const auto& [id, terrain] : terrains_) count += static_cast<std::uint32_t>(terrain->chunks.size());
    return count;
}

}  // namespace cramion::gfx
