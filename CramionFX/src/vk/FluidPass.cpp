#include "CramionFX/vk/FluidPass.h"

#include "CramionFX/vk/VulkanDevice.h"
#include "CramionFX/vk/VulkanShader.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>

namespace cramion::gfx {

namespace {

constexpr float kPi = 3.14159265358979f;

// Etapas de fluid_sim.comp.
enum Stage : std::uint32_t {
    kSpawn = 0,
    kCommit = 1,
    kPredict = 2,
    kScanLocal = 3,
    kScanBlocks = 4,
    kScanAdd = 5,
    kScatter = 6,
    kCount = 7,
    kLambda = 8,
    kDelta = 9,
    kApply = 10,
    kVelocity = 11,
    kViscosity = 12,
    kFinal = 13,
    kClear = 14,
};

constexpr std::uint32_t kSimBindings = 22;
constexpr std::uint32_t kRenderBindings = 11;
constexpr std::uint32_t kMaxCells = (1u << 21) - 1u;  // + la de las muertas = 2^21 (4096 bloques de 512)
constexpr std::uint32_t kStateBytes = 64;
constexpr vk::Format kDepthFormat = vk::Format::eR32Sfloat;
constexpr vk::Format kTestFormat = vk::Format::eD32Sfloat;
constexpr vk::Format kThickFormat = vk::Format::eR16G16B16A16Sfloat;

// MISMO orden que fluid_common.glsl.
struct MaterialGpu {
    core::Vec4 absorb_visc;
    core::Vec4 scatter_cohes;
    core::Vec4 emission_vort;
    core::Vec4 params;
};
static_assert(sizeof(MaterialGpu) == 64);

struct SimParamsGpu {
    core::Vec4 gravity_dt;
    core::Vec4 kernel;
    core::Vec4 constants;
    core::Vec4 domain_min;
    core::Vec4 domain_max;
    std::int32_t grid[4];
    std::uint32_t counts[4];
    core::Vec4 height_origin;
    std::uint32_t height_size[4];
    core::Vec4 shift;
    core::Vec4 extra;
    MaterialGpu materials[FluidPass::kMaterials];
};
static_assert(sizeof(SimParamsGpu) == 176 + 64 * FluidPass::kMaterials);

struct RenderParamsGpu {
    core::Mat4 view;
    core::Mat4 projection;
    core::Mat4 inverse_projection;
    core::Mat4 inverse_view;
    core::Vec4 viewport;
    core::Vec4 params;
    core::Vec4 params2;
    MaterialGpu materials[FluidPass::kMaterials];
};

struct SimPush {
    std::uint32_t stage = 0;
    std::uint32_t extra = 0;
    std::uint32_t iteration = 0;
    std::uint32_t first = 0;
};

MaterialGpu toGpu(const FluidMaterial& m) {
    MaterialGpu g;
    g.absorb_visc = core::Vec4{m.absorption.x, m.absorption.y, m.absorption.z, std::clamp(m.viscosity, 0.0f, 1.0f)};
    g.scatter_cohes = core::Vec4{m.scatter.x, m.scatter.y, m.scatter.z, std::max(m.cohesion, 0.0f)};
    g.emission_vort = core::Vec4{m.emission.x, m.emission.y, m.emission.z, std::max(m.vorticity, 0.0f)};
    g.params = core::Vec4{m.foam, std::clamp(m.roughness, 0.0f, 1.0f), std::max(m.damping, 0.0f), std::clamp(m.crust, 0.0f, 1.0f)};
    return g;
}

void computeBarrier(const vk::raii::CommandBuffer& cmd) {
    vk::MemoryBarrier2 barrier{};
    barrier.srcStageMask = vk::PipelineStageFlagBits2::eComputeShader;
    barrier.srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite;
    barrier.dstStageMask = vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eDrawIndirect;
    barrier.dstAccessMask = vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite |
                            vk::AccessFlagBits2::eIndirectCommandRead;
    vk::DependencyInfo dependency{};
    dependency.setMemoryBarriers(barrier);
    cmd.pipelineBarrier2(dependency);
}

void memoryBarrier(const vk::raii::CommandBuffer& cmd, vk::PipelineStageFlags2 src_stage, vk::AccessFlags2 src_access,
                   vk::PipelineStageFlags2 dst_stage, vk::AccessFlags2 dst_access) {
    vk::MemoryBarrier2 barrier{};
    barrier.srcStageMask = src_stage;
    barrier.srcAccessMask = src_access;
    barrier.dstStageMask = dst_stage;
    barrier.dstAccessMask = dst_access;
    vk::DependencyInfo dependency{};
    dependency.setMemoryBarriers(barrier);
    cmd.pipelineBarrier2(dependency);
}

vk::ImageMemoryBarrier2 imageBarrier(vk::Image image, vk::ImageAspectFlags aspect, vk::ImageLayout from,
                                     vk::ImageLayout to, vk::PipelineStageFlags2 src_stage, vk::AccessFlags2 src_access,
                                     vk::PipelineStageFlags2 dst_stage, vk::AccessFlags2 dst_access) {
    vk::ImageMemoryBarrier2 b{};
    b.srcStageMask = src_stage;
    b.srcAccessMask = src_access;
    b.dstStageMask = dst_stage;
    b.dstAccessMask = dst_access;
    b.oldLayout = from;
    b.newLayout = to;
    b.image = image;
    b.subresourceRange = vk::ImageSubresourceRange{aspect, 0, 1, 0, 1};
    return b;
}

// Poly6 sin constante (para la suma de la red de reposo).
float poly6(float r2, float h) {
    const float h2 = h * h;
    if (r2 >= h2) return 0.0f;
    const float d = h2 - r2;
    return 315.0f / (64.0f * kPi * std::pow(h, 9.0f)) * d * d * d;
}

}  // namespace

// -----------------------------------------------------------------------------
// Creacion
// -----------------------------------------------------------------------------

void FluidPass::create(const VulkanDevice& device, const vk::raii::DescriptorSetLayout& glass_layout,
                       vk::Format color_format, std::uint32_t frames_in_flight) {
    destroy();
    device_ = &device;
    frames_ = frames_in_flight;

    // --- Sets ---
    {
        std::array<vk::DescriptorSetLayoutBinding, kSimBindings> bindings{};
        for (std::uint32_t i = 0; i < kSimBindings; ++i) {
            bindings[i] = vk::DescriptorSetLayoutBinding{
                i, i == 0 ? vk::DescriptorType::eUniformBuffer : vk::DescriptorType::eStorageBuffer, 1,
                vk::ShaderStageFlagBits::eCompute};
        }
        vk::DescriptorSetLayoutCreateInfo info{};
        info.setBindings(bindings);
        sim_set_layout_ = vk::raii::DescriptorSetLayout(device.handle(), info);
    }
    {
        const vk::ShaderStageFlags all = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment |
                                         vk::ShaderStageFlagBits::eCompute;
        using Type = vk::DescriptorType;
        const std::array<Type, kRenderBindings> types = {
            Type::eUniformBuffer,        Type::eStorageBuffer, Type::eStorageBuffer, Type::eStorageBuffer,
            Type::eCombinedImageSampler, Type::eStorageImage,  Type::eStorageImage,  Type::eStorageImage,
            Type::eCombinedImageSampler, Type::eCombinedImageSampler, Type::eCombinedImageSampler};
        std::array<vk::DescriptorSetLayoutBinding, kRenderBindings> bindings{};
        for (std::uint32_t i = 0; i < kRenderBindings; ++i) bindings[i] = vk::DescriptorSetLayoutBinding{i, types[i], 1, all};
        vk::DescriptorSetLayoutCreateInfo info{};
        info.setBindings(bindings);
        render_set_layout_ = vk::raii::DescriptorSetLayout(device.handle(), info);
    }
    const std::array<vk::DescriptorPoolSize, 4> sizes = {{
        {vk::DescriptorType::eUniformBuffer, 2 * frames_in_flight},
        {vk::DescriptorType::eStorageBuffer, (kSimBindings + 3) * frames_in_flight},
        {vk::DescriptorType::eStorageImage, 3 * frames_in_flight},
        {vk::DescriptorType::eCombinedImageSampler, 4 * frames_in_flight},
    }};
    vk::DescriptorPoolCreateInfo pool_info{};
    pool_info.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    pool_info.maxSets = 2 * frames_in_flight;
    pool_info.setPoolSizes(sizes);
    pool_ = vk::raii::DescriptorPool(device.handle(), pool_info);
    {
        std::vector<vk::DescriptorSetLayout> layouts(frames_in_flight, *sim_set_layout_);
        vk::DescriptorSetAllocateInfo alloc{};
        alloc.descriptorPool = *pool_;
        alloc.setSetLayouts(layouts);
        sim_sets_ = vk::raii::DescriptorSets(device.handle(), alloc);
    }
    {
        std::vector<vk::DescriptorSetLayout> layouts(frames_in_flight, *render_set_layout_);
        vk::DescriptorSetAllocateInfo alloc{};
        alloc.descriptorPool = *pool_;
        alloc.setSetLayouts(layouts);
        render_sets_ = vk::raii::DescriptorSets(device.handle(), alloc);
    }

    createSimulation();
    createRender(color_format);
    (void)glass_layout;
    {
        // set 0 = el del liquido, set 1 = el del vidrio (escena detras, cielo, luces).
        const std::array<vk::DescriptorSetLayout, 2> layouts = {*render_set_layout_, *glass_layout};
        vk::PipelineLayoutCreateInfo layout_info{};
        layout_info.setSetLayouts(layouts);
        shade_layout_ = vk::raii::PipelineLayout(device.handle(), layout_info);

        const vk::raii::ShaderModule vertex = shaders::loadModule(device, "fluid_fullscreen.vert.spv");
        const vk::raii::ShaderModule fragment = shaders::loadModule(device, "fluid_shade.frag.spv");
        const std::array<vk::PipelineShaderStageCreateInfo, 2> stages = {{
            {{}, vk::ShaderStageFlagBits::eVertex, *vertex, "main"},
            {{}, vk::ShaderStageFlagBits::eFragment, *fragment, "main"},
        }};
        vk::PipelineVertexInputStateCreateInfo vertex_input{};
        vk::PipelineInputAssemblyStateCreateInfo input_assembly{};
        input_assembly.topology = vk::PrimitiveTopology::eTriangleList;
        vk::PipelineViewportStateCreateInfo viewport{};
        viewport.viewportCount = 1;
        viewport.scissorCount = 1;
        vk::PipelineRasterizationStateCreateInfo raster{};
        raster.polygonMode = vk::PolygonMode::eFill;
        raster.cullMode = vk::CullModeFlagBits::eNone;
        raster.lineWidth = 1.0f;
        vk::PipelineMultisampleStateCreateInfo multisample{};
        multisample.rasterizationSamples = vk::SampleCountFlagBits::e1;
        vk::PipelineDepthStencilStateCreateInfo depth{};
        vk::PipelineColorBlendAttachmentState blend_attachment{};
        blend_attachment.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                                          vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
        vk::PipelineColorBlendStateCreateInfo blend{};
        blend.setAttachments(blend_attachment);
        const std::array<vk::DynamicState, 2> dynamic_states = {vk::DynamicState::eViewport, vk::DynamicState::eScissor};
        vk::PipelineDynamicStateCreateInfo dynamic{};
        dynamic.setDynamicStates(dynamic_states);
        vk::PipelineRenderingCreateInfo rendering{};
        rendering.setColorAttachmentFormats(color_format);
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
        info.layout = *shade_layout_;
        shade_pipeline_ = vk::raii::Pipeline(device.handle(), device.pipelineCache(), info);
    }

    params_.resize(frames_in_flight);
    shapes_buf_.resize(frames_in_flight);
    spawn_buf_.resize(frames_in_flight);
    render_params_.resize(frames_in_flight);
    readback_.resize(frames_in_flight);
    const vk::MemoryPropertyFlags host = vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;
    for (std::uint32_t f = 0; f < frames_in_flight; ++f) {
        params_[f].create(device, sizeof(SimParamsGpu), vk::BufferUsageFlagBits::eUniformBuffer, host);
        render_params_[f].create(device, sizeof(RenderParamsGpu), vk::BufferUsageFlagBits::eUniformBuffer, host);
        shapes_buf_[f].create(device, sizeof(core::Vec4) * 4 * (kMaxShapes + 16), vk::BufferUsageFlagBits::eStorageBuffer, host);
        spawn_buf_[f].create(device, sizeof(core::Vec4) * 2 * kMaxSpawnsPerFrame, vk::BufferUsageFlagBits::eStorageBuffer, host);
    }
    readback_ready_.assign(frames_in_flight, false);
    frame_spawns_.assign(frames_in_flight, 0);
    frame_shapes_.assign(frames_in_flight, 0);
    state_.create(device, kStateBytes,
                  vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eIndirectBuffer |
                      vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst,
                  vk::MemoryPropertyFlagBits::eDeviceLocal);
    heightfield_.create(device, sizeof(float) * kMaxHeightfield * kMaxHeightfield, vk::BufferUsageFlagBits::eStorageBuffer, host);
    device.submitOneTime([&](const vk::raii::CommandBuffer& cmd) { cmd.fillBuffer(*state_.handle(), 0, kStateBytes, 0u); });

    // Materiales por defecto: agua.
    materials_.fill(FluidMaterial{});
    setSettings(settings_);
}

void FluidPass::createSimulation() {
    const VulkanDevice& device = *device_;
    vk::PushConstantRange push{vk::ShaderStageFlagBits::eCompute, 0, sizeof(SimPush)};
    vk::PipelineLayoutCreateInfo layout_info{};
    layout_info.setSetLayouts(*sim_set_layout_);
    layout_info.setPushConstantRanges(push);
    sim_layout_ = vk::raii::PipelineLayout(device.handle(), layout_info);
    const vk::raii::ShaderModule module = shaders::loadModule(device, "fluid_sim.comp.spv");
    vk::ComputePipelineCreateInfo pipeline{};
    pipeline.stage = vk::PipelineShaderStageCreateInfo{{}, vk::ShaderStageFlagBits::eCompute, *module, "main"};
    pipeline.layout = *sim_layout_;
    sim_pipeline_ = vk::raii::Pipeline(device.handle(), device.pipelineCache(), pipeline);
}

void FluidPass::createRender(vk::Format color_format) {
    (void)color_format;
    const VulkanDevice& device = *device_;
    {
        vk::PipelineLayoutCreateInfo layout_info{};
        layout_info.setSetLayouts(*render_set_layout_);
        render_layout_ = vk::raii::PipelineLayout(device.handle(), layout_info);
    }
    {
        vk::PushConstantRange push{vk::ShaderStageFlagBits::eCompute, 0, sizeof(std::uint32_t)};
        vk::PipelineLayoutCreateInfo layout_info{};
        layout_info.setSetLayouts(*render_set_layout_);
        layout_info.setPushConstantRanges(push);
        smooth_layout_ = vk::raii::PipelineLayout(device.handle(), layout_info);
        const vk::raii::ShaderModule module = shaders::loadModule(device, "fluid_smooth.comp.spv");
        vk::ComputePipelineCreateInfo pipeline{};
        pipeline.stage = vk::PipelineShaderStageCreateInfo{{}, vk::ShaderStageFlagBits::eCompute, *module, "main"};
        pipeline.layout = *smooth_layout_;
        smooth_pipeline_ = vk::raii::Pipeline(device.handle(), device.pipelineCache(), pipeline);
    }

    const vk::raii::ShaderModule vertex = shaders::loadModule(device, "fluid_sphere.vert.spv");
    const vk::raii::ShaderModule depth_fragment = shaders::loadModule(device, "fluid_depth.frag.spv");
    const vk::raii::ShaderModule thick_fragment = shaders::loadModule(device, "fluid_thickness.frag.spv");
    vk::PipelineVertexInputStateCreateInfo vertex_input{};
    vk::PipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.topology = vk::PrimitiveTopology::eTriangleList;
    vk::PipelineViewportStateCreateInfo viewport{};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    vk::PipelineRasterizationStateCreateInfo raster{};
    raster.polygonMode = vk::PolygonMode::eFill;
    raster.cullMode = vk::CullModeFlagBits::eNone;
    raster.lineWidth = 1.0f;
    vk::PipelineMultisampleStateCreateInfo multisample{};
    multisample.rasterizationSamples = vk::SampleCountFlagBits::e1;
    const std::array<vk::DynamicState, 2> dynamic_states = {vk::DynamicState::eViewport, vk::DynamicState::eScissor};
    vk::PipelineDynamicStateCreateInfo dynamic{};
    dynamic.setDynamicStates(dynamic_states);
    const vk::ColorComponentFlags rgba = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                                         vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
    // --- Profundidad (la esfera mas cercana) ---
    {
        const std::array<vk::PipelineShaderStageCreateInfo, 2> stages = {{
            {{}, vk::ShaderStageFlagBits::eVertex, *vertex, "main"},
            {{}, vk::ShaderStageFlagBits::eFragment, *depth_fragment, "main"},
        }};
        vk::PipelineDepthStencilStateCreateInfo depth{};
        depth.depthTestEnable = VK_TRUE;
        depth.depthWriteEnable = VK_TRUE;
        depth.depthCompareOp = vk::CompareOp::eLess;
        vk::PipelineColorBlendAttachmentState attachment{};
        attachment.colorWriteMask = vk::ColorComponentFlagBits::eR;
        vk::PipelineColorBlendStateCreateInfo blend{};
        blend.setAttachments(attachment);
        vk::PipelineRenderingCreateInfo rendering{};
        rendering.setColorAttachmentFormats(kDepthFormat);
        rendering.depthAttachmentFormat = kTestFormat;
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
        info.layout = *render_layout_;
        depth_pipeline_ = vk::raii::Pipeline(device.handle(), device.pipelineCache(), info);
    }
    // --- Grosor (suma aditiva, media resolucion) ---
    {
        const std::array<vk::PipelineShaderStageCreateInfo, 2> stages = {{
            {{}, vk::ShaderStageFlagBits::eVertex, *vertex, "main"},
            {{}, vk::ShaderStageFlagBits::eFragment, *thick_fragment, "main"},
        }};
        vk::PipelineDepthStencilStateCreateInfo depth{};
        std::array<vk::PipelineColorBlendAttachmentState, 3> attachments{};
        for (auto& a : attachments) {
            a.blendEnable = VK_TRUE;
            a.srcColorBlendFactor = vk::BlendFactor::eOne;
            a.dstColorBlendFactor = vk::BlendFactor::eOne;
            a.colorBlendOp = vk::BlendOp::eAdd;
            a.srcAlphaBlendFactor = vk::BlendFactor::eOne;
            a.dstAlphaBlendFactor = vk::BlendFactor::eOne;
            a.alphaBlendOp = vk::BlendOp::eAdd;
            a.colorWriteMask = rgba;
        }
        vk::PipelineColorBlendStateCreateInfo blend{};
        blend.setAttachments(attachments);
        const std::array<vk::Format, 3> formats = {kThickFormat, kThickFormat, kThickFormat};
        vk::PipelineRenderingCreateInfo rendering{};
        rendering.setColorAttachmentFormats(formats);
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
        info.layout = *render_layout_;
        thickness_pipeline_ = vk::raii::Pipeline(device.handle(), device.pipelineCache(), info);
    }

    vk::SamplerCreateInfo sampler{};
    sampler.magFilter = vk::Filter::eLinear;
    sampler.minFilter = vk::Filter::eLinear;
    sampler.mipmapMode = vk::SamplerMipmapMode::eNearest;
    sampler.addressModeU = vk::SamplerAddressMode::eClampToEdge;
    sampler.addressModeV = vk::SamplerAddressMode::eClampToEdge;
    sampler.addressModeW = vk::SamplerAddressMode::eClampToEdge;
    sampler.maxLod = 0.0f;
    sampler_ = vk::raii::Sampler(device.handle(), sampler);
}

void FluidPass::destroy() {
    sim_sets_.clear();
    render_sets_.clear();
    for (VulkanBuffer& b : set_a_) b.destroy();
    for (VulkanBuffer& b : set_b_) b.destroy();
    delta_.destroy();
    scalar_.destroy();
    omega_.destroy();
    vel_tmp_.destroy();
    cell_of_.destroy();
    cell_count_.destroy();
    cell_start_.destroy();
    cell_offset_.destroy();
    block_sums_.destroy();
    state_.destroy();
    heightfield_.destroy();
    for (VulkanBuffer& b : params_) b.destroy();
    for (VulkanBuffer& b : shapes_buf_) b.destroy();
    for (VulkanBuffer& b : spawn_buf_) b.destroy();
    for (VulkanBuffer& b : readback_) b.destroy();
    for (VulkanBuffer& b : render_params_) b.destroy();
    params_.clear();
    shapes_buf_.clear();
    spawn_buf_.clear();
    readback_.clear();
    render_params_.clear();
    depth_raw_.destroy();
    depth_test_.destroy();
    depth_temp_.destroy();
    depth_smooth_.destroy();
    for (VulkanImage& t : thick_) t.destroy();
    sim_pipeline_ = nullptr;
    smooth_pipeline_ = nullptr;
    depth_pipeline_ = nullptr;
    thickness_pipeline_ = nullptr;
    shade_pipeline_ = nullptr;
    sampler_ = nullptr;
    pool_ = nullptr;
    sim_layout_ = nullptr;
    render_layout_ = nullptr;
    shade_layout_ = nullptr;
    smooth_layout_ = nullptr;
    sim_set_layout_ = nullptr;
    render_set_layout_ = nullptr;
    capacity_ = 0;
    cells_ = 1;
    extent_ = vk::Extent2D{};
    bound_depth_ = vk::ImageView{};
    targets_ready_ = false;
    snapshot_ = FluidSnapshot{};
    stats_ = FluidStats{};
    spawns_.clear();
    device_ = nullptr;
}

// -----------------------------------------------------------------------------
// Ajustes y memoria
// -----------------------------------------------------------------------------

void FluidPass::setSettings(const FluidSimSettings& in) {
    FluidSimSettings s = in;
    s.particle_radius = std::clamp(s.particle_radius, 0.005f, 1.0f);
    s.max_particles = std::clamp<std::uint32_t>(s.max_particles, 256u, kMaxParticles);
    s.iterations = std::clamp<std::uint32_t>(s.iterations, 1u, 10u);
    s.substep = std::clamp(s.substep, 1.0f / 1000.0f, 1.0f / 15.0f);
    for (int a = 0; a < 3; ++a) {
        float& lo = (&s.domain_min.x)[a];
        float& hi = (&s.domain_max.x)[a];
        if (hi < lo + 0.1f) hi = lo + 0.1f;
    }
    settings_ = s;
    if (device_ == nullptr) return;

    const float h = 4.0f * s.particle_radius;  // radio del nucleo (2 separaciones)
    // Rejilla envuelta: celdas = dominio / h, al menos 3 por eje, como mucho 2^21.
    std::array<std::int32_t, 3> grid{};
    for (int a = 0; a < 3; ++a) {
        const float size = (&s.domain_max.x)[a] - (&s.domain_min.x)[a];
        grid[static_cast<std::size_t>(a)] = std::clamp(static_cast<std::int32_t>(std::ceil(size / h)) + 1, 3, 512);
    }
    while (static_cast<std::uint64_t>(grid[0]) * static_cast<std::uint64_t>(grid[1]) * static_cast<std::uint64_t>(grid[2]) > kMaxCells) {
        auto largest = std::max_element(grid.begin(), grid.end());
        *largest = std::max(3, *largest * 3 / 4);
    }
    const auto cells = static_cast<std::uint32_t>(grid[0] * grid[1] * grid[2]);

    // Densidad de reposo y escala de la restriccion: sumas en una red cubica
    // con la separacion de reposo (2 * radio).
    const float d = 2.0f * s.particle_radius;
    const int reach = static_cast<int>(std::ceil(h / d));
    float density = 0.0f;
    float grad2 = 0.0f;
    const float spiky = 45.0f / (kPi * std::pow(h, 6.0f));
    for (int z = -reach; z <= reach; ++z) {
        for (int y = -reach; y <= reach; ++y) {
            for (int x = -reach; x <= reach; ++x) {
                const float r2 = (static_cast<float>(x * x + y * y + z * z)) * d * d;
                density += poly6(r2, h);
                const float r = std::sqrt(r2);
                if (r > 0.0f && r < h) {
                    const float g = spiky * (h - r) * (h - r);
                    grad2 += g * g;
                }
            }
        }
    }
    rest_density_ = std::max(density, 1e-6f);
    const float grad2_c = grad2 / (rest_density_ * rest_density_);
    constraint_epsilon_ = grad2_c * 0.1f;
    scorr_scale_ = 0.02f / std::max(grad2_c, 1e-12f);

    const bool cells_changed = cells != cells_ || !cell_count_.isValid();
    const bool capacity_changed = s.max_particles != capacity_;
    grid_ = grid;
    if (!cells_changed && !capacity_changed) return;

    device_->waitIdle();
    const vk::BufferUsageFlags storage = vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst |
                                         vk::BufferUsageFlagBits::eTransferSrc;
    if (cells_changed) {
        cells_ = cells;
        const vk::DeviceSize bytes = sizeof(std::uint32_t) * (static_cast<vk::DeviceSize>(cells) + 1);
        cell_count_.create(*device_, bytes, storage, vk::MemoryPropertyFlagBits::eDeviceLocal);
        cell_start_.create(*device_, bytes, storage, vk::MemoryPropertyFlagBits::eDeviceLocal);
        cell_offset_.create(*device_, bytes, storage, vk::MemoryPropertyFlagBits::eDeviceLocal);
        block_sums_.create(*device_, sizeof(std::uint32_t) * 4096, storage, vk::MemoryPropertyFlagBits::eDeviceLocal);
    }
    if (capacity_changed) allocateParticles(s.max_particles);
    writeSimSets();
    if (targets_ready_) writeRenderSets();
}

void FluidPass::allocateParticles(std::uint32_t capacity) {
    capacity_ = capacity;
    const vk::BufferUsageFlags storage = vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst |
                                         vk::BufferUsageFlagBits::eTransferSrc;
    const vk::DeviceSize vec4s = sizeof(core::Vec4) * static_cast<vk::DeviceSize>(capacity);
    for (VulkanBuffer& b : set_a_) b.create(*device_, vec4s, storage, vk::MemoryPropertyFlagBits::eDeviceLocal);
    for (VulkanBuffer& b : set_b_) b.create(*device_, vec4s, storage, vk::MemoryPropertyFlagBits::eDeviceLocal);
    delta_.create(*device_, vec4s, storage, vk::MemoryPropertyFlagBits::eDeviceLocal);
    scalar_.create(*device_, vec4s, storage, vk::MemoryPropertyFlagBits::eDeviceLocal);
    omega_.create(*device_, vec4s, storage, vk::MemoryPropertyFlagBits::eDeviceLocal);
    vel_tmp_.create(*device_, vec4s, storage, vk::MemoryPropertyFlagBits::eDeviceLocal);
    cell_of_.create(*device_, sizeof(std::uint32_t) * static_cast<vk::DeviceSize>(capacity), storage,
                    vk::MemoryPropertyFlagBits::eDeviceLocal);
    // Lectura: estado + posiciones + velocidades. Memoria con cache si la hay
    // (leer de la combinada de escritura es lentisimo).
    const vk::DeviceSize readback_bytes = kStateBytes + 2 * vec4s;
    for (VulkanBuffer& b : readback_) {
        try {
            b.create(*device_, readback_bytes, vk::BufferUsageFlagBits::eTransferDst,
                     vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent |
                         vk::MemoryPropertyFlagBits::eHostCached);
        } catch (const std::exception&) {
            b.create(*device_, readback_bytes, vk::BufferUsageFlagBits::eTransferDst,
                     vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
        }
    }
    readback_ready_.assign(frames_, false);
    // Las particulas que habia se pierden (otra capacidad).
    device_->submitOneTime([&](const vk::raii::CommandBuffer& cmd) { cmd.fillBuffer(*state_.handle(), 0, kStateBytes, 0u); });
    snapshot_ = FluidSnapshot{};
    stats_.memory_bytes = vec4s * 12 + readback_bytes * frames_ + sizeof(std::uint32_t) * capacity;
}

void FluidPass::writeSimSets() {
    if (sim_sets_.empty() || !set_a_[0].isValid() || !cell_count_.isValid()) return;
    for (std::uint32_t f = 0; f < frames_; ++f) {
        std::array<vk::DescriptorBufferInfo, kSimBindings> infos{};
        infos[0] = vk::DescriptorBufferInfo{*params_[f].handle(), 0, sizeof(SimParamsGpu)};
        const std::array<const VulkanBuffer*, kSimBindings - 1> buffers = {
            &set_a_[0], &set_a_[1], &set_a_[2], &set_a_[3], &set_b_[0], &set_b_[1], &set_b_[2], &set_b_[3],
            &delta_,    &scalar_,   &omega_,    &vel_tmp_,  &cell_of_,  &cell_count_, &cell_start_, &cell_offset_,
            &block_sums_, &state_,  &shapes_buf_[f], &heightfield_, &spawn_buf_[f]};
        for (std::uint32_t i = 1; i < kSimBindings; ++i) {
            infos[i] = vk::DescriptorBufferInfo{*buffers[i - 1]->handle(), 0, VK_WHOLE_SIZE};
        }
        std::array<vk::WriteDescriptorSet, kSimBindings> writes{};
        for (std::uint32_t i = 0; i < kSimBindings; ++i) {
            writes[i].dstSet = *sim_sets_[f];
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = i == 0 ? vk::DescriptorType::eUniformBuffer : vk::DescriptorType::eStorageBuffer;
            writes[i].pBufferInfo = &infos[i];
        }
        device_->handle().updateDescriptorSets(writes, nullptr);
    }
}

void FluidPass::setHeightfield(const core::Vec3& origin, float cell, std::uint32_t width, std::uint32_t height,
                               std::vector<float> heights) {
    if (width < 2 || height < 2 || heights.size() < static_cast<std::size_t>(width) * height) {
        if (height_w_ != 0) heights_dirty_ = true;
        height_w_ = height_h_ = 0;
        heights_.clear();
        return;
    }
    width = std::min(width, kMaxHeightfield);
    height = std::min(height, kMaxHeightfield);
    if (width == height_w_ && height == height_h_ && cell == height_cell_ && origin.x == height_origin_.x &&
        origin.z == height_origin_.z && heights == heights_) {
        return;
    }
    height_origin_ = origin;
    height_cell_ = std::max(cell, 1e-3f);
    height_w_ = width;
    height_h_ = height;
    heights_ = std::move(heights);
    heights_dirty_ = true;
}

void FluidPass::spawn(const std::vector<FluidSpawn>& particles) {
    const std::size_t room = kMaxSpawnsPerFrame - std::min<std::size_t>(spawns_.size(), kMaxSpawnsPerFrame);
    spawns_.insert(spawns_.end(), particles.begin(), particles.begin() + static_cast<std::ptrdiff_t>(std::min(room, particles.size())));
}

void FluidPass::clear() {
    clear_pending_ = true;
    spawns_.clear();
    snapshot_.count = 0;
    snapshot_.positions.clear();
    snapshot_.velocities.clear();
    ++snapshot_.frame;
    stats_.particles = 0;
}

// -----------------------------------------------------------------------------
// Simulacion
// -----------------------------------------------------------------------------

void FluidPass::readBack(std::uint32_t frame) {
    if (frame >= readback_.size() || !readback_ready_[frame] || !readback_[frame].isValid()) return;
    readback_ready_[frame] = false;
    if (clear_pending_) return;
    const auto* bytes = static_cast<const std::uint8_t*>(readback_[frame].mapped());
    if (bytes == nullptr) return;
    std::uint32_t state[16];
    std::memcpy(state, bytes, sizeof(state));
    const std::uint32_t count = std::min(state[3], capacity_);
    snapshot_.count = count;
    snapshot_.positions.resize(count);
    snapshot_.velocities.resize(count);
    const vk::DeviceSize vec4s = sizeof(core::Vec4) * static_cast<vk::DeviceSize>(capacity_);
    if (count > 0) {
        std::memcpy(snapshot_.positions.data(), bytes + kStateBytes, sizeof(core::Vec4) * count);
        std::memcpy(snapshot_.velocities.data(), bytes + kStateBytes + vec4s, sizeof(core::Vec4) * count);
    }
    ++snapshot_.frame;
    stats_.particles = count;
}

void FluidPass::uploadFrame(std::uint32_t frame) {
    const FluidSimSettings& s = settings_;
    const float h = 4.0f * s.particle_radius;
    SimParamsGpu p{};
    p.gravity_dt = core::Vec4{s.gravity.x, s.gravity.y, s.gravity.z, s.substep};
    p.kernel = core::Vec4{h, s.particle_radius, rest_density_, 1.0f / rest_density_};
    p.constants = core::Vec4{315.0f / (64.0f * kPi * std::pow(h, 9.0f)), 45.0f / (kPi * std::pow(h, 6.0f)),
                             constraint_epsilon_, scorr_scale_};
    p.domain_min = core::Vec4{s.domain_min.x, s.domain_min.y, s.domain_min.z, s.solid_walls ? 1.0f : 0.0f};
    p.domain_max = core::Vec4{s.domain_max.x, s.domain_max.y, s.domain_max.z, s.kill_outside ? 1.0f : 0.0f};
    p.grid[0] = grid_[0];
    p.grid[1] = grid_[1];
    p.grid[2] = grid_[2];
    p.grid[3] = static_cast<std::int32_t>(cells_);
    const auto shape_count = static_cast<std::uint32_t>(std::min<std::size_t>(shapes_.size(), kMaxShapes));
    const auto drain_count = static_cast<std::uint32_t>(std::min<std::size_t>(drains_.size(), 16));
    p.counts[0] = capacity_;
    p.counts[1] = shape_count;
    p.counts[2] = drain_count;
    p.counts[3] = height_w_ >= 2 ? 1u : 0u;
    p.height_origin = core::Vec4{height_origin_.x, height_cell_, height_origin_.z, s.max_speed};
    p.height_size[0] = height_w_;
    p.height_size[1] = height_h_;
    p.height_size[2] = (cells_ + 1 + 511) / 512;
    p.shift = core::Vec4{pending_shift_.x, pending_shift_.y, pending_shift_.z, std::clamp(s.friction, 0.0f, 1.0f)};
    const float dq = 0.2f * h;
    p.extra = core::Vec4{s.surface_tension, poly6(dq * dq, h), 0.0f, 0.0f};
    for (std::uint32_t m = 0; m < kMaterials; ++m) p.materials[m] = toGpu(materials_[m]);
    params_[frame].write(&p, sizeof(p));

    // Formas: colliders y luego desagues (4 vec4 por forma).
    std::vector<core::Vec4> packed;
    packed.reserve(static_cast<std::size_t>(shape_count + drain_count) * 4);
    const auto pack = [&](const FluidShape& shape) {
        const auto type = static_cast<float>(static_cast<std::uint32_t>(shape.type));
        packed.push_back(core::Vec4{shape.a.x, shape.a.y, shape.a.z, type});
        packed.push_back(core::Vec4{shape.b.x, shape.b.y, shape.b.z, shape.radius});
        packed.push_back(core::Vec4{shape.rotation.x, shape.rotation.y, shape.rotation.z, shape.rotation.w});
        packed.push_back(core::Vec4{});
    };
    for (std::uint32_t i = 0; i < shape_count; ++i) pack(shapes_[i]);
    for (std::uint32_t i = 0; i < drain_count; ++i) pack(drains_[i]);
    if (!packed.empty()) shapes_buf_[frame].write(packed.data(), packed.size() * sizeof(core::Vec4));
    frame_shapes_[frame] = shape_count;

    // Particulas nuevas.
    const auto spawn_count = static_cast<std::uint32_t>(std::min<std::size_t>(spawns_.size(), kMaxSpawnsPerFrame));
    if (spawn_count > 0) {
        std::vector<core::Vec4> data(static_cast<std::size_t>(spawn_count) * 2);
        for (std::uint32_t i = 0; i < spawn_count; ++i) {
            const FluidSpawn& sp = spawns_[i];
            data[i * 2] = core::Vec4{sp.position.x, sp.position.y, sp.position.z,
                                     static_cast<float>(std::min(sp.material, kMaterials - 1))};
            data[i * 2 + 1] = core::Vec4{sp.velocity.x, sp.velocity.y, sp.velocity.z, sp.lifetime};
        }
        spawn_buf_[frame].write(data.data(), data.size() * sizeof(core::Vec4));
    }
    frame_spawns_[frame] = spawn_count;
    spawns_.clear();

    if (heights_dirty_) {
        // Pocas veces (al cambiar el terreno): se espera a la GPU.
        device_->waitIdle();
        if (!heights_.empty()) {
            // Las filas se recortan si el campo era mas grande que el maximo.
            std::vector<float> rows(static_cast<std::size_t>(height_w_) * height_h_);
            for (std::uint32_t y = 0; y < height_h_; ++y) {
                for (std::uint32_t x = 0; x < height_w_; ++x) rows[y * height_w_ + x] = heights_[y * height_w_ + x];
            }
            heightfield_.write(rows.data(), rows.size() * sizeof(float));
        }
        heights_dirty_ = false;
    }
}

void FluidPass::dispatchStage(const vk::raii::CommandBuffer& cmd, std::uint32_t frame, std::uint32_t stage,
                              bool indirect, std::uint32_t groups, std::uint32_t extra, std::uint32_t iteration) const {
    (void)frame;
    SimPush push{};
    push.stage = stage;
    push.extra = extra;
    push.iteration = iteration;
    push.first = iteration;  // PREDICT: 1 = primer subpaso (desplazamiento del origen)
    cmd.pushConstants<SimPush>(*sim_layout_, vk::ShaderStageFlagBits::eCompute, 0, push);
    if (indirect) {
        cmd.dispatchIndirect(*state_.handle(), 0);
    } else {
        cmd.dispatch(std::max(groups, 1u), 1, 1);
    }
}

void FluidPass::recordSimulate(const vk::raii::CommandBuffer& cmd, std::uint32_t frame) {
    if (device_ == nullptr || !frame_pending_ || capacity_ == 0) return;
    frame_pending_ = false;
    readBack(frame);
    if (!active_) {
        spawns_.clear();
        return;
    }
    uploadFrame(frame);
    const std::uint32_t substeps = pending_substeps_;
    const std::uint32_t spawn_count = frame_spawns_[frame];
    stats_.substeps = substeps;
    stats_.capacity = capacity_;
    stats_.grid_cells = cells_;

    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *sim_pipeline_);
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *sim_layout_, 0, *sim_sets_[frame], nullptr);
    // Lo que leyo el frame anterior (dibujo, lectura) antes de escribir.
    memoryBarrier(cmd,
                  vk::PipelineStageFlagBits2::eVertexShader | vk::PipelineStageFlagBits2::eFragmentShader |
                      vk::PipelineStageFlagBits2::eCopy | vk::PipelineStageFlagBits2::eDrawIndirect,
                  vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eTransferRead |
                      vk::AccessFlagBits2::eIndirectCommandRead,
                  vk::PipelineStageFlagBits2::eComputeShader,
                  vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite);

    if (clear_pending_) {
        dispatchStage(cmd, frame, kClear, false);
        computeBarrier(cmd);
        clear_pending_ = false;
    }
    if (spawn_count > 0) {
        dispatchStage(cmd, frame, kSpawn, false, (spawn_count + 255) / 256, spawn_count);
        computeBarrier(cmd);
        dispatchStage(cmd, frame, kCommit, false, 1, spawn_count);
        computeBarrier(cmd);
    }
    const std::uint32_t entries = cells_ + 1;
    const std::uint32_t blocks = (entries + 511) / 512;
    for (std::uint32_t step = 0; step < substeps; ++step) {
        cmd.fillBuffer(*cell_count_.handle(), 0, VK_WHOLE_SIZE, 0u);
        memoryBarrier(cmd, vk::PipelineStageFlagBits2::eClear | vk::PipelineStageFlagBits2::eTransfer,
                      vk::AccessFlagBits2::eTransferWrite, vk::PipelineStageFlagBits2::eComputeShader,
                      vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite);
        dispatchStage(cmd, frame, kPredict, true, 1, 0, step == 0 ? 1u : 0u);
        computeBarrier(cmd);
        dispatchStage(cmd, frame, kScanLocal, false, blocks);
        computeBarrier(cmd);
        dispatchStage(cmd, frame, kScanBlocks, false, 1);
        computeBarrier(cmd);
        dispatchStage(cmd, frame, kScanAdd, false, (entries + 255) / 256);
        computeBarrier(cmd);
        dispatchStage(cmd, frame, kScatter, true);
        computeBarrier(cmd);
        dispatchStage(cmd, frame, kCount, false, 1);
        computeBarrier(cmd);
        for (std::uint32_t it = 0; it < settings_.iterations; ++it) {
            dispatchStage(cmd, frame, kLambda, true);
            computeBarrier(cmd);
            dispatchStage(cmd, frame, kDelta, true);
            computeBarrier(cmd);
            dispatchStage(cmd, frame, kApply, true);
            computeBarrier(cmd);
        }
        dispatchStage(cmd, frame, kVelocity, true);
        computeBarrier(cmd);
        dispatchStage(cmd, frame, kViscosity, true);
        computeBarrier(cmd);
        dispatchStage(cmd, frame, kFinal, true);
        computeBarrier(cmd);
    }
    if (substeps > 0) pending_shift_ = core::Vec3{};
    time_ += static_cast<float>(substeps) * settings_.substep;

    // Lectura para la CPU (llega cuando este frame vuelva a usarse).
    memoryBarrier(cmd, vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderStorageWrite,
                  vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferRead);
    const vk::DeviceSize vec4s = sizeof(core::Vec4) * static_cast<vk::DeviceSize>(capacity_);
    cmd.copyBuffer(*state_.handle(), *readback_[frame].handle(), vk::BufferCopy{0, 0, kStateBytes});
    cmd.copyBuffer(*set_a_[0].handle(), *readback_[frame].handle(), vk::BufferCopy{0, kStateBytes, vec4s});
    cmd.copyBuffer(*set_a_[1].handle(), *readback_[frame].handle(), vk::BufferCopy{0, kStateBytes + vec4s, vec4s});
    memoryBarrier(cmd, vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferWrite,
                  vk::PipelineStageFlagBits2::eHost, vk::AccessFlagBits2::eHostRead);
    readback_ready_[frame] = true;
    // Para dibujar: las posiciones (vertices) y el contador (indirecto).
    memoryBarrier(cmd, vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderStorageWrite,
                  vk::PipelineStageFlagBits2::eVertexShader | vk::PipelineStageFlagBits2::eFragmentShader |
                      vk::PipelineStageFlagBits2::eDrawIndirect,
                  vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eIndirectCommandRead);
}

// -----------------------------------------------------------------------------
// Render
// -----------------------------------------------------------------------------

void FluidPass::ensureTargets(vk::Extent2D extent, vk::ImageView scene_depth) {
    if (targets_ready_ && extent == extent_ && scene_depth == bound_depth_) return;
    device_->waitIdle();
    extent_ = extent;
    bound_depth_ = scene_depth;
    const vk::Extent2D half{std::max(extent.width / 2, 1u), std::max(extent.height / 2, 1u)};
    depth_raw_.create(*device_, extent, kDepthFormat,
                      vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eStorage,
                      vk::ImageAspectFlagBits::eColor);
    depth_temp_.create(*device_, extent, kDepthFormat, vk::ImageUsageFlagBits::eStorage, vk::ImageAspectFlagBits::eColor);
    depth_smooth_.create(*device_, extent, kDepthFormat, vk::ImageUsageFlagBits::eStorage, vk::ImageAspectFlagBits::eColor);
    depth_test_.create(*device_, extent, kTestFormat, vk::ImageUsageFlagBits::eDepthStencilAttachment,
                       vk::ImageAspectFlagBits::eDepth);
    for (VulkanImage& t : thick_) {
        t.create(*device_, half, kThickFormat, vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled,
                 vk::ImageAspectFlagBits::eColor);
    }
    targets_ready_ = true;
    writeRenderSets();
}

void FluidPass::writeRenderSets() {
    if (!targets_ready_ || !set_a_[0].isValid()) return;
    for (std::uint32_t f = 0; f < frames_; ++f) {
        const vk::DescriptorBufferInfo params{*render_params_[f].handle(), 0, sizeof(RenderParamsGpu)};
        const std::array<vk::DescriptorBufferInfo, 3> particles = {{
            {*set_a_[0].handle(), 0, VK_WHOLE_SIZE},
            {*set_a_[1].handle(), 0, VK_WHOLE_SIZE},
            {*set_a_[2].handle(), 0, VK_WHOLE_SIZE},
        }};
        const vk::DescriptorImageInfo scene_depth{*sampler_, bound_depth_, vk::ImageLayout::eDepthReadOnlyOptimal};
        const std::array<vk::DescriptorImageInfo, 3> storage = {{
            {nullptr, *depth_raw_.view(), vk::ImageLayout::eGeneral},
            {nullptr, *depth_temp_.view(), vk::ImageLayout::eGeneral},
            {nullptr, *depth_smooth_.view(), vk::ImageLayout::eGeneral},
        }};
        const std::array<vk::DescriptorImageInfo, 3> thick = {{
            {*sampler_, *thick_[0].view(), vk::ImageLayout::eShaderReadOnlyOptimal},
            {*sampler_, *thick_[1].view(), vk::ImageLayout::eShaderReadOnlyOptimal},
            {*sampler_, *thick_[2].view(), vk::ImageLayout::eShaderReadOnlyOptimal},
        }};
        std::array<vk::WriteDescriptorSet, kRenderBindings> writes{};
        for (std::uint32_t i = 0; i < kRenderBindings; ++i) {
            writes[i].dstSet = *render_sets_[f];
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
        }
        writes[0].descriptorType = vk::DescriptorType::eUniformBuffer;
        writes[0].pBufferInfo = &params;
        for (std::uint32_t i = 0; i < 3; ++i) {
            writes[1 + i].descriptorType = vk::DescriptorType::eStorageBuffer;
            writes[1 + i].pBufferInfo = &particles[i];
            writes[5 + i].descriptorType = vk::DescriptorType::eStorageImage;
            writes[5 + i].pImageInfo = &storage[i];
            writes[8 + i].descriptorType = vk::DescriptorType::eCombinedImageSampler;
            writes[8 + i].pImageInfo = &thick[i];
        }
        writes[4].descriptorType = vk::DescriptorType::eCombinedImageSampler;
        writes[4].pImageInfo = &scene_depth;
        device_->handle().updateDescriptorSets(writes, nullptr);
    }
}

void FluidPass::recordPrepare(const vk::raii::CommandBuffer& cmd, std::uint32_t frame, const View& view) {
    if (device_ == nullptr || capacity_ == 0 || view.extent.width == 0 || view.extent.height == 0) return;
    ensureTargets(view.extent, view.scene_depth);

    const float radius = settings_.particle_radius;
    const bool debug = render_settings_.debug_particles;
    RenderParamsGpu p{};
    p.view = view.view;
    p.projection = view.projection;
    p.inverse_projection = core::inverse(view.projection);
    p.inverse_view = core::inverse(view.view);
    const auto w = static_cast<float>(view.extent.width);
    const auto h = static_cast<float>(view.extent.height);
    p.viewport = core::Vec4{w, h, 1.0f / w, 1.0f / h};
    p.params = core::Vec4{radius * (debug ? 1.0f : std::clamp(render_settings_.render_radius, 0.5f, 3.0f)),
                          std::max(render_settings_.thickness, 0.0f),
                          debug ? 0.0f : radius * 4.0f * std::clamp(render_settings_.smoothing, 0.0f, 3.0f), 96.0f};
    p.params2 = core::Vec4{render_settings_.refraction, time_, radius, debug ? 1.0f : 0.0f};
    for (std::uint32_t m = 0; m < kMaterials; ++m) p.materials[m] = toGpu(materials_[m]);
    render_params_[frame].write(&p, sizeof(p));

    using Stage = vk::PipelineStageFlagBits2;
    using Access = vk::AccessFlagBits2;
    using Layout = vk::ImageLayout;
    const vk::ImageAspectFlags color = vk::ImageAspectFlagBits::eColor;
    {
        std::vector<vk::ImageMemoryBarrier2> barriers = {
            imageBarrier(*depth_raw_.handle(), color, Layout::eUndefined, Layout::eColorAttachmentOptimal,
                         Stage::eComputeShader, Access::eShaderStorageRead, Stage::eColorAttachmentOutput,
                         Access::eColorAttachmentWrite),
            imageBarrier(*depth_test_.handle(), vk::ImageAspectFlagBits::eDepth, Layout::eUndefined,
                         Layout::eDepthAttachmentOptimal, Stage::eLateFragmentTests,
                         Access::eDepthStencilAttachmentWrite,
                         Stage::eEarlyFragmentTests | Stage::eLateFragmentTests,
                         Access::eDepthStencilAttachmentRead | Access::eDepthStencilAttachmentWrite),
        };
        for (VulkanImage& t : thick_) {
            barriers.push_back(imageBarrier(*t.handle(), color, Layout::eUndefined, Layout::eColorAttachmentOptimal,
                                            Stage::eFragmentShader, Access::eShaderSampledRead,
                                            Stage::eColorAttachmentOutput, Access::eColorAttachmentWrite));
        }
        cmd.pipelineBarrier2(vk::DependencyInfo{}.setImageMemoryBarriers(barriers));
    }

    // --- Profundidad de las esferas ---
    {
        vk::RenderingAttachmentInfo color_attachment{};
        color_attachment.imageView = *depth_raw_.view();
        color_attachment.imageLayout = Layout::eColorAttachmentOptimal;
        color_attachment.loadOp = vk::AttachmentLoadOp::eClear;
        color_attachment.storeOp = vk::AttachmentStoreOp::eStore;
        color_attachment.clearValue.color = vk::ClearColorValue{std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f}};
        vk::RenderingAttachmentInfo depth_attachment{};
        depth_attachment.imageView = *depth_test_.view();
        depth_attachment.imageLayout = Layout::eDepthAttachmentOptimal;
        depth_attachment.loadOp = vk::AttachmentLoadOp::eClear;
        depth_attachment.storeOp = vk::AttachmentStoreOp::eDontCare;
        depth_attachment.clearValue.depthStencil = vk::ClearDepthStencilValue{1.0f, 0};
        vk::RenderingInfo info{};
        info.renderArea = vk::Rect2D{vk::Offset2D{0, 0}, view.extent};
        info.layerCount = 1;
        info.setColorAttachments(color_attachment);
        info.pDepthAttachment = &depth_attachment;
        cmd.beginRendering(info);
        cmd.setViewport(0, vk::Viewport{0.0f, 0.0f, w, h, 0.0f, 1.0f});
        cmd.setScissor(0, vk::Rect2D{vk::Offset2D{0, 0}, view.extent});
        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *depth_pipeline_);
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *render_layout_, 0, *render_sets_[frame], nullptr);
        cmd.drawIndirect(*state_.handle(), 16, 1, 16);
        cmd.endRendering();
    }
    // --- Grosor y material (media resolucion) ---
    {
        const vk::Extent2D half = thick_[0].extent();
        std::array<vk::RenderingAttachmentInfo, 3> attachments{};
        for (std::size_t i = 0; i < 3; ++i) {
            attachments[i].imageView = *thick_[i].view();
            attachments[i].imageLayout = Layout::eColorAttachmentOptimal;
            attachments[i].loadOp = vk::AttachmentLoadOp::eClear;
            attachments[i].storeOp = vk::AttachmentStoreOp::eStore;
            attachments[i].clearValue.color = vk::ClearColorValue{std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f}};
        }
        vk::RenderingInfo info{};
        info.renderArea = vk::Rect2D{vk::Offset2D{0, 0}, half};
        info.layerCount = 1;
        info.setColorAttachments(attachments);
        cmd.beginRendering(info);
        cmd.setViewport(0, vk::Viewport{0.0f, 0.0f, static_cast<float>(half.width), static_cast<float>(half.height), 0.0f, 1.0f});
        cmd.setScissor(0, vk::Rect2D{vk::Offset2D{0, 0}, half});
        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *thickness_pipeline_);
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *render_layout_, 0, *render_sets_[frame], nullptr);
        cmd.drawIndirect(*state_.handle(), 16, 1, 16);
        cmd.endRendering();
    }
    // --- Suavizado (computo) ---
    {
        std::vector<vk::ImageMemoryBarrier2> barriers = {
            imageBarrier(*depth_raw_.handle(), color, Layout::eColorAttachmentOptimal, Layout::eGeneral,
                         Stage::eColorAttachmentOutput, Access::eColorAttachmentWrite, Stage::eComputeShader,
                         Access::eShaderStorageRead),
            imageBarrier(*depth_temp_.handle(), color, Layout::eUndefined, Layout::eGeneral, Stage::eComputeShader,
                         Access::eShaderStorageRead, Stage::eComputeShader,
                         Access::eShaderStorageWrite | Access::eShaderStorageRead),
            imageBarrier(*depth_smooth_.handle(), color, Layout::eUndefined, Layout::eGeneral,
                         Stage::eFragmentShader, Access::eShaderStorageRead, Stage::eComputeShader,
                         Access::eShaderStorageWrite),
        };
        for (VulkanImage& t : thick_) {
            barriers.push_back(imageBarrier(*t.handle(), color, Layout::eColorAttachmentOptimal,
                                            Layout::eShaderReadOnlyOptimal, Stage::eColorAttachmentOutput,
                                            Access::eColorAttachmentWrite, Stage::eFragmentShader,
                                            Access::eShaderSampledRead));
        }
        cmd.pipelineBarrier2(vk::DependencyInfo{}.setImageMemoryBarriers(barriers));
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *smooth_pipeline_);
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *smooth_layout_, 0, *render_sets_[frame], nullptr);
        const std::uint32_t gx = (view.extent.width + 15) / 16;
        const std::uint32_t gy = (view.extent.height + 15) / 16;
        // Dos rondas separables (H, V, H, V): raw -> temp -> smooth -> temp ->
        // smooth. Una sola dejaba ver cada esfera de cerca y rayas verticales.
        vk::MemoryBarrier2 between{};
        between.srcStageMask = Stage::eComputeShader;
        between.srcAccessMask = Access::eShaderStorageWrite | Access::eShaderStorageRead;
        between.dstStageMask = Stage::eComputeShader;
        between.dstAccessMask = Access::eShaderStorageRead | Access::eShaderStorageWrite;
        for (std::uint32_t pass = 0; pass < 4; ++pass) {
            if (pass > 0) cmd.pipelineBarrier2(vk::DependencyInfo{}.setMemoryBarriers(between));
            cmd.pushConstants<std::uint32_t>(*smooth_layout_, vk::ShaderStageFlagBits::eCompute, 0, pass);
            cmd.dispatch(gx, gy, 1);
        }
        const vk::ImageMemoryBarrier2 smooth_barrier =
            imageBarrier(*depth_smooth_.handle(), color, Layout::eGeneral, Layout::eGeneral, Stage::eComputeShader,
                         Access::eShaderStorageWrite, Stage::eFragmentShader, Access::eShaderStorageRead);
        cmd.pipelineBarrier2(vk::DependencyInfo{}.setImageMemoryBarriers(smooth_barrier));
    }
}

void FluidPass::recordShade(const vk::raii::CommandBuffer& cmd, std::uint32_t frame,
                            const vk::raii::DescriptorSet& glass_set, vk::Extent2D extent) const {
    if (!targets_ready_) return;
    cmd.setViewport(0, vk::Viewport{0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0f, 1.0f});
    cmd.setScissor(0, vk::Rect2D{vk::Offset2D{0, 0}, extent});
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *shade_pipeline_);
    const std::array<vk::DescriptorSet, 2> sets = {*render_sets_[frame], *glass_set};
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *shade_layout_, 0, sets, nullptr);
    cmd.draw(3, 1, 0, 0);
}

}  // namespace cramion::gfx
