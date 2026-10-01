#include "CramionFX/vk/PrecipitationPass.h"

#include "CramionFX/vk/VulkanDevice.h"
#include "CramionFX/vk/VulkanImage.h"
#include "CramionFX/vk/VulkanShader.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace cramion::gfx {

namespace {

// Debe coincidir con precipitation.vert/.frag.
struct GpuPrecipitation {
    core::Mat4 view_projection;
    core::Mat4 inverse_view_projection;
    core::Mat4 rain_view_projection;
    core::Vec4 camera;     // xyz = camara, w = segundos (ciclo de 1 h)
    core::Vec4 right;      // xyz = derecha de la camara, w = tan(fov / 2)
    core::Vec4 wind;       // xyz = viento (m/s), w = mapa de lluvia listo
    core::Vec4 drift;      // xyz = lo que ha empujado el viento (m), w = alto de la imagen (px)
    core::Vec4 amounts;    // x = lluvia, y = nieve, z = polvo, w = ancho de la imagen (px)
    core::Vec4 light;      // rgb = luz de las gotas (HDR lineal), w = destello del rayo
    core::Vec4 bolt_info;  // x = brillo del rayo, y = segmentos
    core::Vec4 bolt[2 * PrecipitationPass::kMaxBoltSegments];
};

struct PushConstants {
    std::int32_t mode = 0;  // 0 lluvia, 1 nieve, 2 polvo, 3 salpicaduras, 4 rayo
    std::int32_t count = 0;
    std::int32_t pad[2] = {0, 0};
};

constexpr std::uint32_t kMaxRain = 110000;
constexpr std::uint32_t kMaxSnow = 90000;
constexpr std::uint32_t kMaxDust = 60000;
constexpr std::uint32_t kMaxSplashes = 4000;

}  // namespace

void PrecipitationPass::create(const VulkanDevice& device, vk::Format color_format, vk::Format depth_format,
                               std::uint32_t frames_in_flight) {
    destroy();
    const auto& dev = device.handle();

    std::array<vk::DescriptorSetLayoutBinding, 3> bindings{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = vk::DescriptorType::eUniformBuffer;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment;
    for (std::uint32_t i = 1; i < 3; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = vk::DescriptorType::eCombinedImageSampler;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = vk::ShaderStageFlagBits::eVertex;
    }
    vk::DescriptorSetLayoutCreateInfo set_info{};
    set_info.setBindings(bindings);
    set_layout_ = vk::raii::DescriptorSetLayout(dev, set_info);

    const std::array<vk::DescriptorPoolSize, 2> sizes = {{
        {vk::DescriptorType::eUniformBuffer, frames_in_flight},
        {vk::DescriptorType::eCombinedImageSampler, frames_in_flight * 2},
    }};
    vk::DescriptorPoolCreateInfo pool_info{};
    pool_info.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    pool_info.maxSets = frames_in_flight;
    pool_info.setPoolSizes(sizes);
    pool_ = vk::raii::DescriptorPool(dev, pool_info);
    const std::vector<vk::DescriptorSetLayout> layouts(frames_in_flight, *set_layout_);
    vk::DescriptorSetAllocateInfo alloc{};
    alloc.descriptorPool = *pool_;
    alloc.setSetLayouts(layouts);
    vk::raii::DescriptorSets sets(dev, alloc);
    sets_.clear();
    for (auto& s : sets) sets_.push_back(std::move(s));

    vk::PushConstantRange push{};
    push.stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment;
    push.size = sizeof(PushConstants);
    vk::PipelineLayoutCreateInfo layout_info{};
    layout_info.setSetLayouts(*set_layout_);
    layout_info.setPushConstantRanges(push);
    layout_ = vk::raii::PipelineLayout(dev, layout_info);

    alpha_pipeline_ = createPipeline(device, color_format, depth_format, false);
    additive_pipeline_ = createPipeline(device, color_format, depth_format, true);

    vk::SamplerCreateInfo sampler_info{};
    sampler_info.magFilter = vk::Filter::eNearest;
    sampler_info.minFilter = vk::Filter::eNearest;
    sampler_info.mipmapMode = vk::SamplerMipmapMode::eNearest;
    sampler_info.addressModeU = vk::SamplerAddressMode::eClampToEdge;
    sampler_info.addressModeV = vk::SamplerAddressMode::eClampToEdge;
    sampler_info.addressModeW = vk::SamplerAddressMode::eClampToEdge;
    depth_sampler_ = vk::raii::Sampler(dev, sampler_info);

    buffers_.resize(frames_in_flight);
    for (VulkanBuffer& buffer : buffers_) {
        buffer.create(device, sizeof(GpuPrecipitation), vk::BufferUsageFlagBits::eUniformBuffer,
                      vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
    }
    counts_.assign(frames_in_flight, Counts{});
}

void PrecipitationPass::destroy() {
    for (VulkanBuffer& buffer : buffers_) buffer.destroy();
    buffers_.clear();
    counts_.clear();
    sets_.clear();
    pool_ = nullptr;
    alpha_pipeline_ = nullptr;
    additive_pipeline_ = nullptr;
    layout_ = nullptr;
    set_layout_ = nullptr;
    depth_sampler_ = nullptr;
}

vk::raii::Pipeline PrecipitationPass::createPipeline(const VulkanDevice& device, vk::Format color_format,
                                                     vk::Format depth_format, bool additive) const {
    const vk::raii::ShaderModule vertex_module = shaders::loadModule(device, "precipitation.vert.spv");
    const vk::raii::ShaderModule fragment_module = shaders::loadModule(device, "precipitation.frag.spv");
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
    depth_stencil.depthTestEnable = VK_TRUE;
    depth_stencil.depthWriteEnable = VK_FALSE;
    depth_stencil.depthCompareOp = vk::CompareOp::eLessOrEqual;

    // El alfa de la imagen HDR guarda la distancia de lo que se ve: no se toca.
    vk::PipelineColorBlendAttachmentState blend{};
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = additive ? vk::BlendFactor::eOne : vk::BlendFactor::eSrcAlpha;
    blend.dstColorBlendFactor = additive ? vk::BlendFactor::eOne : vk::BlendFactor::eOneMinusSrcAlpha;
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
    rendering_info.depthAttachmentFormat = depth_format;

    vk::GraphicsPipelineCreateInfo info{};
    info.pNext = &rendering_info;
    info.setStages(stages);
    info.pVertexInputState = &vertex_input;
    info.pInputAssemblyState = &input_assembly;
    info.pViewportState = &viewport_state;
    info.pRasterizationState = &rasterization;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depth_stencil;
    info.pColorBlendState = &color_blend;
    info.pDynamicState = &dynamic_state;
    info.layout = *layout_;
    return vk::raii::Pipeline(device.handle(), device.pipelineCache(), info);
}

bool PrecipitationPass::prepare(const VulkanDevice& device, std::uint32_t frame, const PrecipitationSettings& s,
                                const PrecipitationFrame& view, const VulkanImage& rain_map, vk::Sampler rain_sampler,
                                const VulkanImage& scene_depth) {
    if (frame >= buffers_.size()) return false;
    Counts& counts = counts_[frame];
    counts = Counts{};
    last_count_ = 0;

    // El viento empuja todo lo que cae (acumulado: cambiar el viento no hace
    // saltar las particulas).
    const float dt = std::clamp(view.delta_seconds, 0.0f, 0.25f);
    seconds_ = std::fmod(seconds_ + dt, 3600.0);
    constexpr double kDriftPeriod = 4000.0;
    drift_[0] = std::fmod(drift_[0] + s.wind.x * dt, kDriftPeriod);
    drift_[1] = std::fmod(drift_[1] + s.wind.y * dt, kDriftPeriod);
    drift_[2] = std::fmod(drift_[2] + s.wind.z * dt, kDriftPeriod);

    if (!s.drawsSomething()) return false;

    const float density = std::clamp(s.density, 0.0f, 4.0f);
    const auto scaled = [&](float amount, std::uint32_t max) {
        return static_cast<std::uint32_t>(std::clamp(amount, 0.0f, 1.0f) * density * static_cast<float>(max) * 0.65f);
    };
    counts.rain = std::min(scaled(s.rain, kMaxRain), kMaxRain);
    counts.snow = std::min(scaled(s.snow, kMaxSnow), kMaxSnow);
    counts.dust = std::min(scaled(s.dust, kMaxDust), kMaxDust);
    counts.splashes = s.splashes ? std::min(scaled(s.rain, kMaxSplashes), kMaxSplashes) : 0u;
    counts.bolt = s.bolt_brightness > 0.001f
                      ? static_cast<std::uint32_t>(std::min<std::size_t>(s.bolt.size() / 2, kMaxBoltSegments))
                      : 0u;

    GpuPrecipitation data{};
    data.view_projection = view.view_projection;
    data.inverse_view_projection = view.inverse_view_projection;
    data.rain_view_projection = view.rain_view_projection;
    data.camera = core::Vec4{view.camera_position.x, view.camera_position.y, view.camera_position.z,
                             static_cast<float>(seconds_)};
    data.right = core::Vec4{view.camera_right.x, view.camera_right.y, view.camera_right.z, view.tan_half_fov};
    data.wind = core::Vec4{s.wind.x, s.wind.y, s.wind.z, view.rain_map_ready ? 1.0f : 0.0f};
    data.drift = core::Vec4{static_cast<float>(drift_[0]), static_cast<float>(drift_[1]), static_cast<float>(drift_[2]),
                            static_cast<float>(view.extent.height)};
    data.amounts = core::Vec4{s.rain, s.snow, s.dust, static_cast<float>(view.extent.width)};
    data.light = core::Vec4{view.light.x, view.light.y, view.light.z, s.flash};
    data.bolt_info = core::Vec4{s.bolt_brightness, static_cast<float>(counts.bolt), 0.0f, 0.0f};
    for (std::uint32_t i = 0; i < counts.bolt * 2; ++i) data.bolt[i] = s.bolt[i];
    buffers_[frame].write(&data, sizeof(data));

    const vk::DescriptorBufferInfo buffer_info{*buffers_[frame].handle(), 0, sizeof(GpuPrecipitation)};
    const vk::DescriptorImageInfo rain_info{rain_sampler, *rain_map.view(), vk::ImageLayout::eDepthReadOnlyOptimal};
    const vk::DescriptorImageInfo depth_info{*depth_sampler_, *scene_depth.view(), vk::ImageLayout::eDepthReadOnlyOptimal};
    const std::array<vk::WriteDescriptorSet, 3> writes = {{
        {*sets_[frame], 0, 0, 1, vk::DescriptorType::eUniformBuffer, nullptr, &buffer_info},
        {*sets_[frame], 1, 0, 1, vk::DescriptorType::eCombinedImageSampler, &rain_info},
        {*sets_[frame], 2, 0, 1, vk::DescriptorType::eCombinedImageSampler, &depth_info},
    }};
    device.handle().updateDescriptorSets(writes, nullptr);

    last_count_ = counts.rain + counts.snow + counts.dust + counts.splashes;
    return counts.rain + counts.snow + counts.dust + counts.splashes + counts.bolt > 0;
}

void PrecipitationPass::record(const vk::raii::CommandBuffer& cmd, std::uint32_t frame, vk::Extent2D viewport) const {
    if (frame >= counts_.size()) return;
    const Counts& counts = counts_[frame];
    cmd.setViewport(0, vk::Viewport{0.0f, 0.0f, static_cast<float>(viewport.width),
                                    static_cast<float>(viewport.height), 0.0f, 1.0f});
    cmd.setScissor(0, vk::Rect2D{vk::Offset2D{0, 0}, viewport});
    const auto draw = [&](const vk::raii::Pipeline& pipeline, int mode, std::uint32_t count) {
        if (count == 0) return;
        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *pipeline);
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *layout_, 0, *sets_[frame], nullptr);
        PushConstants push{};
        push.mode = mode;
        push.count = static_cast<std::int32_t>(count);
        cmd.pushConstants<PushConstants>(*layout_, vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
                                         0, push);
        cmd.draw(6, count, 0, 0);
    };
    // El rayo primero (queda detras de la lluvia, en el cielo).
    draw(additive_pipeline_, 4, counts.bolt);
    draw(alpha_pipeline_, 2, counts.dust);
    draw(alpha_pipeline_, 1, counts.snow);
    draw(alpha_pipeline_, 0, counts.rain);
    draw(alpha_pipeline_, 3, counts.splashes);
}

}  // namespace cramion::gfx
