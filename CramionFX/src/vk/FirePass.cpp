#include "CramionFX/vk/VulkanCompat.h"
#include "CramionFX/vk/FirePass.h"

#include "CramionFX/vk/VulkanDevice.h"
#include "CramionFX/vk/VulkanShader.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace cramion::gfx {

namespace {

using core::Vec3;
using core::Vec4;

// Lo que lee fire.frag (binding 1). Una fila por zona en cada array.
struct GpuFire {
    Vec4 box_min[kFireZoneSlots];      // xyz = caja de la zona (llamas + humo + deriva), w = activa
    Vec4 box_max[kFireZoneSlots];      // xyz, w = altura de referencia (las alturas del mapa son relativas)
    Vec4 rect[kFireZoneSlots];         // xy = esquina minima (x, z), z = lado (m), w = parte del mapa usada
    Vec4 flame[kFireZoneSlots];        // x = altura de las llamas, y = intensidad, z = altura del humo, w = densidad
    Vec4 wind[kFireZoneSlots];         // xy = viento (m/s), z = subida del humo (m/s), w = tamano de celda (m)
    Vec4 smoke_color[kFireZoneSlots];  // rgb
    Vec4 sun_direction;                // xyz = hacia el sol
    Vec4 sun_color;                    // rgb = color x intensidad
    Vec4 ambient;                      // rgb = cielo
    Vec4 params;                       // x = segundos, y = frame, z = zonas, w = 1 / kFireMapSize
};

constexpr vk::DeviceSize kMapBytes = static_cast<vk::DeviceSize>(kFireMapSize) * kFireMapSize * 4;
constexpr vk::DeviceSize kHeightBytes = static_cast<vk::DeviceSize>(kFireMapSize) * kFireMapSize * 2;
constexpr vk::DeviceSize kStagingBytes = (kMapBytes + kHeightBytes) * kFireZoneSlots;
constexpr std::uint32_t kNoiseSize = 256;

// float -> half (IEEE 754), con redondeo al mas cercano y sin denormales.
std::uint16_t toHalf(float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const std::uint32_t sign = (bits >> 16) & 0x8000u;
    const int exponent = static_cast<int>((bits >> 23) & 0xFFu) - 127 + 15;
    std::uint32_t mantissa = bits & 0x7FFFFFu;
    if (exponent <= 0) return static_cast<std::uint16_t>(sign);
    if (exponent >= 31) return static_cast<std::uint16_t>(sign | 0x7BFFu);
    mantissa += 0x1000u;  // redondeo
    if (mantissa & 0x800000u) {
        mantissa = 0;
        if (exponent + 1 >= 31) return static_cast<std::uint16_t>(sign | 0x7BFFu);
        return static_cast<std::uint16_t>(sign | (static_cast<std::uint32_t>(exponent + 1) << 10));
    }
    return static_cast<std::uint16_t>(sign | (static_cast<std::uint32_t>(exponent) << 10) | (mantissa >> 13));
}

std::uint32_t hash(std::uint32_t x) {
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

vk::ImageMemoryBarrier2 layerBarrier(vk::Image image, vk::ImageLayout from, vk::ImageLayout to,
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
    b.subresourceRange = vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, kFireZoneSlots};
    return b;
}

// Quien lee los mapas: la geometria (vertices de la hierba, fragmentos) y fire.frag.
constexpr vk::PipelineStageFlags2 kReaders =
    vk::PipelineStageFlagBits2::eVertexShader | vk::PipelineStageFlagBits2::eFragmentShader;

}  // namespace

void FirePass::create(const VulkanDevice& device, vk::Format color_format, std::uint32_t frames_in_flight) {
    destroy();
    device_ = &device;
    const vk::ImageUsageFlags usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst;
    map_.create(device, vk::Extent2D{kFireMapSize, kFireMapSize}, vk::Format::eR8G8B8A8Unorm, usage,
                vk::ImageAspectFlagBits::eColor, kFireZoneSlots);
    height_.create(device, vk::Extent2D{kFireMapSize, kFireMapSize}, vk::Format::eR16Sfloat, usage,
                   vk::ImageAspectFlagBits::eColor, kFireZoneSlots);
    // Vacios (nada quemado) y listos para leer desde el primer frame.
    const vk::Image map_image = *map_.handle();
    const vk::Image height_image = *height_.handle();
    device.submitOneTime([&](const vk::raii::CommandBuffer& cmd) {
        const std::array<vk::ImageMemoryBarrier2, 2> to_clear = {
            layerBarrier(map_image, vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
                         vk::PipelineStageFlagBits2::eTopOfPipe, {}, vk::PipelineStageFlagBits2::eTransfer,
                         vk::AccessFlagBits2::eTransferWrite),
            layerBarrier(height_image, vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
                         vk::PipelineStageFlagBits2::eTopOfPipe, {}, vk::PipelineStageFlagBits2::eTransfer,
                         vk::AccessFlagBits2::eTransferWrite)};
        compat::pipelineBarrier(cmd, vk::DependencyInfo{}.setImageMemoryBarriers(to_clear));
        const vk::ClearColorValue zero{std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f}};
        const vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, kFireZoneSlots};
        cmd.clearColorImage(map_image, vk::ImageLayout::eTransferDstOptimal, zero, range);
        cmd.clearColorImage(height_image, vk::ImageLayout::eTransferDstOptimal, zero, range);
        const std::array<vk::ImageMemoryBarrier2, 2> to_read = {
            layerBarrier(map_image, vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
                         vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite, kReaders,
                         vk::AccessFlagBits2::eShaderSampledRead),
            layerBarrier(height_image, vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
                         vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite, kReaders,
                         vk::AccessFlagBits2::eShaderSampledRead)};
        compat::pipelineBarrier(cmd, vk::DependencyInfo{}.setImageMemoryBarriers(to_read));
    });
    initialized_layout_ = true;

    vk::SamplerCreateInfo sampler{};
    sampler.magFilter = vk::Filter::eLinear;
    sampler.minFilter = vk::Filter::eLinear;
    sampler.mipmapMode = vk::SamplerMipmapMode::eNearest;
    sampler.addressModeU = vk::SamplerAddressMode::eClampToEdge;
    sampler.addressModeV = vk::SamplerAddressMode::eClampToEdge;
    sampler.addressModeW = vk::SamplerAddressMode::eClampToEdge;
    sampler.maxLod = 0.0f;
    map_sampler_ = vk::raii::Sampler(device.handle(), sampler);
    depth_sampler_ = vk::raii::Sampler(device.handle(), sampler);
    sampler.addressModeU = vk::SamplerAddressMode::eRepeat;
    sampler.addressModeV = vk::SamplerAddressMode::eRepeat;
    noise_sampler_ = vk::raii::Sampler(device.handle(), sampler);
    createNoise(device);

    const vk::MemoryPropertyFlags host_visible =
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;
    staging_.resize(frames_in_flight);
    uniforms_.resize(frames_in_flight);
    for (std::uint32_t i = 0; i < frames_in_flight; ++i) {
        staging_[i].create(device, kStagingBytes, vk::BufferUsageFlagBits::eTransferSrc, host_visible);
        uniforms_[i].create(device, sizeof(GpuFire), vk::BufferUsageFlagBits::eUniformBuffer, host_visible);
    }

    // Set: 0 camara, 1 fuego, 2 profundidad, 3 mapa, 4 alturas, 5 ruido.
    std::array<vk::DescriptorSetLayoutBinding, 6> bindings{};
    for (std::uint32_t i = 0; i < bindings.size(); ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType =
            i < 2 ? vk::DescriptorType::eUniformBuffer : vk::DescriptorType::eCombinedImageSampler;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = vk::ShaderStageFlagBits::eFragment;
    }
    vk::DescriptorSetLayoutCreateInfo layout_info{};
    layout_info.setBindings(bindings);
    set_layout_ = vk::raii::DescriptorSetLayout(device.handle(), layout_info);

    const std::array<vk::DescriptorPoolSize, 2> sizes = {
        vk::DescriptorPoolSize{vk::DescriptorType::eUniformBuffer, frames_in_flight * 2},
        vk::DescriptorPoolSize{vk::DescriptorType::eCombinedImageSampler, frames_in_flight * 4}};
    vk::DescriptorPoolCreateInfo pool_info{};
    pool_info.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    pool_info.maxSets = frames_in_flight;
    pool_info.setPoolSizes(sizes);
    pool_ = vk::raii::DescriptorPool(device.handle(), pool_info);
    const std::vector<vk::DescriptorSetLayout> layouts(frames_in_flight, *set_layout_);
    vk::DescriptorSetAllocateInfo alloc{};
    alloc.descriptorPool = *pool_;
    alloc.setSetLayouts(layouts);
    vk::raii::DescriptorSets sets(device.handle(), alloc);
    sets_.clear();
    for (auto& s : sets) sets_.push_back(std::move(s));

    createPipeline(device, color_format);
}

void FirePass::createNoise(const VulkanDevice& device) {
    // Ruido de valor: R al azar y G = R desplazado (-37, -17). Con dos
    // lecturas filtradas se tiene un ruido 3D suave (el truco de las capas de
    // Inigo Quilez): cada "capa" z esta desplazada (37, 17) texeles. B y A,
    // otro ruido igual de independiente.
    std::vector<std::uint8_t> r(kNoiseSize * kNoiseSize);
    std::vector<std::uint8_t> b(kNoiseSize * kNoiseSize);
    for (std::uint32_t i = 0; i < r.size(); ++i) {
        r[i] = static_cast<std::uint8_t>(hash(i * 2654435761u + 17u) >> 24);
        b[i] = static_cast<std::uint8_t>(hash(i * 2246822519u + 101u) >> 24);
    }
    std::vector<std::uint8_t> rgba(kNoiseSize * kNoiseSize * 4);
    for (std::uint32_t y = 0; y < kNoiseSize; ++y) {
        for (std::uint32_t x = 0; x < kNoiseSize; ++x) {
            const std::uint32_t i = y * kNoiseSize + x;
            const std::uint32_t shifted = ((y - 17u) & (kNoiseSize - 1)) * kNoiseSize + ((x - 37u) & (kNoiseSize - 1));
            rgba[i * 4 + 0] = r[i];
            rgba[i * 4 + 1] = r[shifted];
            rgba[i * 4 + 2] = b[i];
            rgba[i * 4 + 3] = b[shifted];
        }
    }
    noise_.create(device, kNoiseSize, kNoiseSize, rgba.data());
}

void FirePass::createPipeline(const VulkanDevice& device, vk::Format color_format) {
    const vk::DescriptorSetLayout raw = *set_layout_;
    vk::PipelineLayoutCreateInfo layout_info{};
    layout_info.setSetLayouts(raw);
    layout_ = vk::raii::PipelineLayout(device.handle(), layout_info);

    const vk::raii::ShaderModule vertex_module = shaders::loadModule(device, "lighting.vert.spv");
    const vk::raii::ShaderModule fragment_module = shaders::loadModule(device, "fire.frag.spv");
    std::array<vk::PipelineShaderStageCreateInfo, 2> stages{};
    stages[0].stage = vk::ShaderStageFlagBits::eVertex;
    stages[0].module = *vertex_module;
    stages[0].pName = "main";
    stages[1].stage = vk::ShaderStageFlagBits::eFragment;
    stages[1].module = *fragment_module;
    stages[1].pName = "main";

    vk::PipelineVertexInputStateCreateInfo vertex_input{};
    vk::PipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.topology = vk::PrimitiveTopology::eTriangleList;
    vk::PipelineViewportStateCreateInfo viewport_state{};
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;
    vk::PipelineRasterizationStateCreateInfo rasterization{};
    rasterization.polygonMode = vk::PolygonMode::eFill;
    rasterization.cullMode = vk::CullModeFlagBits::eNone;
    rasterization.frontFace = vk::FrontFace::eCounterClockwise;
    rasterization.lineWidth = 1.0f;
    vk::PipelineMultisampleStateCreateInfo multisample{};
    multisample.rasterizationSamples = vk::SampleCountFlagBits::e1;
    vk::PipelineDepthStencilStateCreateInfo depth_stencil{};

    // Premultiplicado: color = luz del volumen + lo de detras x transmitancia (alfa).
    vk::PipelineColorBlendAttachmentState blend{};
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = vk::BlendFactor::eOne;
    blend.dstColorBlendFactor = vk::BlendFactor::eSrcAlpha;
    blend.colorBlendOp = vk::BlendOp::eAdd;
    blend.srcAlphaBlendFactor = vk::BlendFactor::eZero;
    blend.dstAlphaBlendFactor = vk::BlendFactor::eOne;
    blend.alphaBlendOp = vk::BlendOp::eAdd;
    blend.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                           vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
    vk::PipelineColorBlendStateCreateInfo color_blend{};
    color_blend.setAttachments(blend);

    const std::array<vk::DynamicState, 2> dynamic_states = {vk::DynamicState::eViewport, vk::DynamicState::eScissor};
    vk::PipelineDynamicStateCreateInfo dynamic_state{};
    dynamic_state.setDynamicStates(dynamic_states);
    vk::PipelineRenderingCreateInfo rendering_info{};
    rendering_info.setColorAttachmentFormats(color_format);

    vk::GraphicsPipelineCreateInfo pipeline_info{};
    pipeline_info.pNext = &rendering_info;
    pipeline_info.setStages(stages);
    pipeline_info.pVertexInputState = &vertex_input;
    pipeline_info.pInputAssemblyState = &input_assembly;
    pipeline_info.pViewportState = &viewport_state;
    pipeline_info.pRasterizationState = &rasterization;
    pipeline_info.pMultisampleState = &multisample;
    pipeline_info.pDepthStencilState = &depth_stencil;
    pipeline_info.pColorBlendState = &color_blend;
    pipeline_info.pDynamicState = &dynamic_state;
    pipeline_info.layout = *layout_;
    pipeline_ = compat::makeGraphicsPipeline(device, pipeline_info);
}

void FirePass::destroy() {
    pipeline_ = nullptr;
    layout_ = nullptr;
    sets_.clear();
    pool_ = nullptr;
    set_layout_ = nullptr;
    staging_.clear();
    uniforms_.clear();
    noise_.destroy();
    map_sampler_ = nullptr;
    noise_sampler_ = nullptr;
    depth_sampler_ = nullptr;
    map_.destroy();
    height_.destroy();
    slots_ = {};
    active_count_ = 0;
    initialized_layout_ = false;
    device_ = nullptr;
}

void FirePass::setZones(const std::vector<FireZone>& zones) {
    active_count_ = 0;
    for (std::uint32_t i = 0; i < kFireZoneSlots; ++i) {
        Slot& slot = slots_[i];
        const bool valid = i < zones.size() && zones[i].cells && zones[i].heights && zones[i].resolution > 0 &&
                           zones[i].resolution <= kFireMapSize &&
                           zones[i].cells->size() >= static_cast<std::size_t>(zones[i].resolution) * zones[i].resolution * 4 &&
                           zones[i].heights->size() >= static_cast<std::size_t>(zones[i].resolution) * zones[i].resolution;
        slot.active = valid;
        if (!valid) {
            slot.zone.cells.reset();
            slot.zone.heights.reset();
            // Otro mapa podria acabar en la misma direccion: se olvida lo subido.
            slot.uploaded_cells = nullptr;
            slot.uploaded_heights = nullptr;
            slot.uploaded_resolution = 0;
            continue;
        }
        slot.zone = zones[i];
        ++active_count_;
    }
}

std::array<Vec4, kFireZoneSlots> FirePass::zoneRects() const {
    std::array<Vec4, kFireZoneSlots> out{};
    for (std::uint32_t i = 0; i < kFireZoneSlots; ++i) {
        const Slot& s = slots_[i];
        // Solo cuando lo de la GPU es de esta zona (se sube antes de la geometria).
        if (!s.active || s.uploaded_resolution != s.zone.resolution) continue;
        out[i] = Vec4{s.zone.origin.x, s.zone.origin.z, s.zone.size,
                      static_cast<float>(s.zone.resolution) / static_cast<float>(kFireMapSize)};
    }
    return out;
}

void FirePass::recordUpload(const vk::raii::CommandBuffer& cmd, std::uint32_t frame) {
    if (device_ == nullptr || frame >= staging_.size()) return;
    std::vector<vk::BufferImageCopy> map_regions;
    std::vector<vk::BufferImageCopy> height_regions;
    auto* staging = static_cast<std::uint8_t*>(staging_[frame].mapped());
    if (staging == nullptr) return;
    for (std::uint32_t i = 0; i < kFireZoneSlots; ++i) {
        Slot& slot = slots_[i];
        if (!slot.active) continue;
        const std::uint32_t res = slot.zone.resolution;
        const vk::DeviceSize texels = static_cast<vk::DeviceSize>(res) * res;
        if (slot.zone.cells.get() != slot.uploaded_cells || res != slot.uploaded_resolution) {
            const vk::DeviceSize offset = kMapBytes * i;
            std::memcpy(staging + offset, slot.zone.cells->data(), static_cast<std::size_t>(texels * 4));
            vk::BufferImageCopy region{};
            region.bufferOffset = offset;
            region.imageSubresource = vk::ImageSubresourceLayers{vk::ImageAspectFlagBits::eColor, 0, i, 1};
            region.imageExtent = vk::Extent3D{res, res, 1};
            map_regions.push_back(region);
            slot.uploaded_cells = slot.zone.cells.get();
        }
        if (slot.zone.heights.get() != slot.uploaded_heights || res != slot.uploaded_resolution) {
            const vk::DeviceSize offset = kMapBytes * kFireZoneSlots + kHeightBytes * i;
            auto* out = reinterpret_cast<std::uint16_t*>(staging + offset);
            const std::vector<float>& h = *slot.zone.heights;
            for (vk::DeviceSize t = 0; t < texels; ++t) out[t] = toHalf(h[t]);
            vk::BufferImageCopy region{};
            region.bufferOffset = offset;
            region.imageSubresource = vk::ImageSubresourceLayers{vk::ImageAspectFlagBits::eColor, 0, i, 1};
            region.imageExtent = vk::Extent3D{res, res, 1};
            height_regions.push_back(region);
            slot.uploaded_heights = slot.zone.heights.get();
        }
        slot.uploaded_resolution = res;
    }
    if (map_regions.empty() && height_regions.empty()) return;
    const vk::Image map_image = *map_.handle();
    const vk::Image height_image = *height_.handle();
    std::vector<vk::ImageMemoryBarrier2> before;
    std::vector<vk::ImageMemoryBarrier2> after;
    const auto add = [&](vk::Image image) {
        before.push_back(layerBarrier(image, vk::ImageLayout::eShaderReadOnlyOptimal, vk::ImageLayout::eTransferDstOptimal,
                                      kReaders, {}, vk::PipelineStageFlagBits2::eTransfer,
                                      vk::AccessFlagBits2::eTransferWrite));
        after.push_back(layerBarrier(image, vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
                                     vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite, kReaders,
                                     vk::AccessFlagBits2::eShaderSampledRead));
    };
    if (!map_regions.empty()) add(map_image);
    if (!height_regions.empty()) add(height_image);
    compat::pipelineBarrier(cmd, vk::DependencyInfo{}.setImageMemoryBarriers(before));
    if (!map_regions.empty()) {
        cmd.copyBufferToImage(*staging_[frame].handle(), map_image, vk::ImageLayout::eTransferDstOptimal, map_regions);
    }
    if (!height_regions.empty()) {
        cmd.copyBufferToImage(*staging_[frame].handle(), height_image, vk::ImageLayout::eTransferDstOptimal,
                              height_regions);
    }
    compat::pipelineBarrier(cmd, vk::DependencyInfo{}.setImageMemoryBarriers(after));
}

void FirePass::record(const vk::raii::CommandBuffer& cmd, std::uint32_t frame, vk::Extent2D extent,
                      vk::Buffer camera_buffer, vk::DeviceSize camera_size, vk::ImageView depth_view,
                      const FireLighting& lighting, float seconds, std::uint32_t frame_counter) {
    if (device_ == nullptr || !active() || frame >= sets_.size()) return;
    GpuFire data{};
    std::uint32_t count = 0;
    for (std::uint32_t i = 0; i < kFireZoneSlots; ++i) {
        const Slot& s = slots_[i];
        if (!s.active || s.uploaded_resolution != s.zone.resolution) continue;
        const FireZone& z = s.zone;
        ++count;
        const float top = z.ground_max + std::max(z.smoke_height, z.flame_height * 1.6f) + 2.0f;
        // El humo se va con el viento mientras sube: la caja se alarga hacia alli.
        const float rise = std::max(z.smoke_rise, 0.2f);
        const float drift_x = z.wind.x * (z.smoke_height / rise);
        const float drift_z = z.wind.y * (z.smoke_height / rise);
        const float pad = z.smoke_height * 0.35f + 4.0f;  // la columna se ensancha
        const float x0 = z.origin.x, z0 = z.origin.z, x1 = z.origin.x + z.size, z1 = z.origin.z + z.size;
        data.box_min[i] = Vec4{std::min(x0, x0 + drift_x) - pad, z.ground_min - 1.0f, std::min(z0, z0 + drift_z) - pad, 1.0f};
        data.box_max[i] = Vec4{std::max(x1, x1 + drift_x) + pad, top, std::max(z1, z1 + drift_z) + pad, z.origin.y};
        data.rect[i] = Vec4{z.origin.x, z.origin.z, z.size,
                            static_cast<float>(z.resolution) / static_cast<float>(kFireMapSize)};
        data.flame[i] = Vec4{std::max(z.flame_height, 0.1f), std::max(z.flame_intensity, 0.0f),
                             std::max(z.smoke_height, 1.0f), std::max(z.smoke_density, 0.0f)};
        data.wind[i] = Vec4{z.wind.x, z.wind.y, rise, z.size / static_cast<float>(std::max(z.resolution, 1u))};
        data.smoke_color[i] = Vec4{z.smoke_color.x, z.smoke_color.y, z.smoke_color.z, 0.0f};
    }
    if (count == 0) return;
    data.sun_direction = Vec4{lighting.to_sun.x, lighting.to_sun.y, lighting.to_sun.z, 0.0f};
    data.sun_color = Vec4{lighting.sun_color.x, lighting.sun_color.y, lighting.sun_color.z, 0.0f};
    data.ambient = Vec4{lighting.ambient.x, lighting.ambient.y, lighting.ambient.z, 0.0f};
    data.params = Vec4{seconds, static_cast<float>(frame_counter % 1024u), static_cast<float>(count),
                       1.0f / static_cast<float>(kFireMapSize)};
    uniforms_[frame].write(&data, sizeof(data));

    const vk::DescriptorSet set = *sets_[frame];
    vk::DescriptorBufferInfo camera_info{camera_buffer, 0, camera_size};
    vk::DescriptorBufferInfo fire_info{*uniforms_[frame].handle(), 0, sizeof(GpuFire)};
    vk::DescriptorImageInfo depth_info{*depth_sampler_, depth_view, compat::depthReadOnlyLayout()};
    vk::DescriptorImageInfo map_info{*map_sampler_, *map_.view(), vk::ImageLayout::eShaderReadOnlyOptimal};
    vk::DescriptorImageInfo height_info{*map_sampler_, *height_.view(), vk::ImageLayout::eShaderReadOnlyOptimal};
    vk::DescriptorImageInfo noise_info{*noise_sampler_, *noise_.view(), vk::ImageLayout::eShaderReadOnlyOptimal};
    std::array<vk::WriteDescriptorSet, 6> writes{};
    for (std::uint32_t i = 0; i < writes.size(); ++i) {
        writes[i].dstSet = set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = i < 2 ? vk::DescriptorType::eUniformBuffer : vk::DescriptorType::eCombinedImageSampler;
    }
    writes[0].pBufferInfo = &camera_info;
    writes[1].pBufferInfo = &fire_info;
    writes[2].pImageInfo = &depth_info;
    writes[3].pImageInfo = &map_info;
    writes[4].pImageInfo = &height_info;
    writes[5].pImageInfo = &noise_info;
    device_->handle().updateDescriptorSets(writes, nullptr);

    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *pipeline_);
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *layout_, 0, set, nullptr);
    cmd.setViewport(0, vk::Viewport{0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0f,
                                    1.0f});
    cmd.setScissor(0, vk::Rect2D{vk::Offset2D{0, 0}, extent});
    cmd.draw(3, 1, 0, 0);
}

}  // namespace cramion::gfx
