#include "CramionFX/vk/WaterPass.h"

#include "CramionFX/vk/VulkanDevice.h"
#include "CramionFX/vk/VulkanShader.h"

#include <algorithm>
#include <cmath>

namespace cramion::gfx {

namespace {

struct GpuWaterUniform {
    core::Vec4 time_count{};  // x = tiempo, y = cuerpos
    GpuWaterBody bodies[kMaxWaterBodies];
};

struct GpuWaterPush {
    std::uint32_t body = 0;
    std::uint32_t mesh = 0;  // 0 lago, 1 oceano, 2 rio
    std::uint32_t pad[2] = {0, 0};
};

// Resolucion de las mallas.
constexpr std::uint32_t kGridCells = 192;
constexpr std::uint32_t kOceanRings = 180;
constexpr std::uint32_t kOceanSegments = 256;

}  // namespace

void WaterPass::create(const VulkanDevice& device, const vk::raii::DescriptorSetLayout& frame_layout,
                       const vk::raii::DescriptorSetLayout& scene_layout, vk::Format color_format,
                       vk::Format depth_format, std::uint32_t frames_in_flight) {
    destroy();
    device_ = &device;
    frames_ = frames_in_flight;

    const auto stages = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment;
    const vk::DescriptorSetLayoutBinding binding{0, vk::DescriptorType::eUniformBuffer, 1, stages};
    vk::DescriptorSetLayoutCreateInfo set_info{};
    set_info.setBindings(binding);
    set_layout_ = vk::raii::DescriptorSetLayout(device.handle(), set_info);

    const std::array<vk::DescriptorSetLayout, 3> set_layouts = {*frame_layout, *set_layout_, *scene_layout};
    vk::PushConstantRange push{};
    push.stageFlags = stages;
    push.size = sizeof(GpuWaterPush);
    vk::PipelineLayoutCreateInfo layout_info{};
    layout_info.setSetLayouts(set_layouts);
    layout_info.setPushConstantRanges(push);
    layout_ = vk::raii::PipelineLayout(device.handle(), layout_info);

    const vk::DescriptorPoolSize size{vk::DescriptorType::eUniformBuffer, frames_in_flight};
    vk::DescriptorPoolCreateInfo pool_info{};
    pool_info.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    pool_info.maxSets = frames_in_flight;
    pool_info.setPoolSizes(size);
    pool_ = vk::raii::DescriptorPool(device.handle(), pool_info);
    const std::vector<vk::DescriptorSetLayout> layouts(frames_in_flight, *set_layout_);
    vk::DescriptorSetAllocateInfo alloc{};
    alloc.descriptorPool = *pool_;
    alloc.setSetLayouts(layouts);
    sets_ = vk::raii::DescriptorSets(device.handle(), alloc);

    uniforms_.resize(frames_in_flight);
    for (std::uint32_t i = 0; i < frames_in_flight; ++i) {
        uniforms_[i].create(device, sizeof(GpuWaterUniform), vk::BufferUsageFlagBits::eUniformBuffer,
                            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
        vk::DescriptorBufferInfo info{};
        info.buffer = *uniforms_[i].handle();
        info.range = sizeof(GpuWaterUniform);
        vk::WriteDescriptorSet write{};
        write.dstSet = *sets_[i];
        write.dstBinding = 0;
        write.descriptorType = vk::DescriptorType::eUniformBuffer;
        write.setBufferInfo(info);
        device.handle().updateDescriptorSets(write, nullptr);
    }

    // --- Pipeline: mezcla sobre la imagen HDR, prueba de profundidad sin escribir ---
    const vk::raii::ShaderModule vertex = shaders::loadModule(device, "water.vert.spv");
    const vk::raii::ShaderModule fragment = shaders::loadModule(device, "water.frag.spv");
    const std::array<vk::PipelineShaderStageCreateInfo, 2> shader_stages = {{
        {{}, vk::ShaderStageFlagBits::eVertex, *vertex, "main"},
        {{}, vk::ShaderStageFlagBits::eFragment, *fragment, "main"},
    }};
    const vk::VertexInputBindingDescription vertex_binding{0, sizeof(WaterVertex), vk::VertexInputRate::eVertex};
    const std::array<vk::VertexInputAttributeDescription, 3> attributes = {{
        {0, 0, vk::Format::eR32G32B32Sfloat, offsetof(WaterVertex, position)},
        {1, 0, vk::Format::eR32G32Sfloat, offsetof(WaterVertex, uv)},
        {2, 0, vk::Format::eR32G32Sfloat, offsetof(WaterVertex, flow)},
    }};
    vk::PipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.setVertexBindingDescriptions(vertex_binding);
    vertex_input.setVertexAttributeDescriptions(attributes);
    vk::PipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.topology = vk::PrimitiveTopology::eTriangleList;
    vk::PipelineViewportStateCreateInfo viewport{};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    vk::PipelineRasterizationStateCreateInfo raster{};
    raster.polygonMode = vk::PolygonMode::eFill;
    raster.cullMode = vk::CullModeFlagBits::eNone;  // tambien se ve desde abajo
    raster.lineWidth = 1.0f;
    vk::PipelineMultisampleStateCreateInfo multisample{};
    multisample.rasterizationSamples = vk::SampleCountFlagBits::e1;
    vk::PipelineDepthStencilStateCreateInfo depth{};
    depth.depthTestEnable = VK_TRUE;
    depth.depthWriteEnable = VK_FALSE;
    depth.depthCompareOp = vk::CompareOp::eLessOrEqual;
    vk::PipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.blendEnable = VK_TRUE;
    blend_attachment.srcColorBlendFactor = vk::BlendFactor::eSrcAlpha;
    blend_attachment.dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
    blend_attachment.colorBlendOp = vk::BlendOp::eAdd;
    blend_attachment.srcAlphaBlendFactor = vk::BlendFactor::eZero;
    blend_attachment.dstAlphaBlendFactor = vk::BlendFactor::eOne;
    blend_attachment.alphaBlendOp = vk::BlendOp::eAdd;
    blend_attachment.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                                      vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
    vk::PipelineColorBlendStateCreateInfo blend{};
    blend.setAttachments(blend_attachment);
    const std::array<vk::DynamicState, 2> dynamic_states = {vk::DynamicState::eViewport, vk::DynamicState::eScissor};
    vk::PipelineDynamicStateCreateInfo dynamic{};
    dynamic.setDynamicStates(dynamic_states);
    vk::PipelineRenderingCreateInfo rendering{};
    rendering.setColorAttachmentFormats(color_format);
    rendering.depthAttachmentFormat = depth_format;
    vk::GraphicsPipelineCreateInfo info{};
    info.pNext = &rendering;
    info.setStages(shader_stages);
    info.pVertexInputState = &vertex_input;
    info.pInputAssemblyState = &input_assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depth;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = *layout_;
    pipeline_ = vk::raii::Pipeline(device.handle(), device.pipelineCache(), info);

    // --- Bajo el agua: triangulo a pantalla completa, sin profundidad ---
    {
        const vk::raii::ShaderModule fullscreen = shaders::loadModule(device, "lighting.vert.spv");
        const vk::raii::ShaderModule under = shaders::loadModule(device, "water_under.frag.spv");
        const std::array<vk::PipelineShaderStageCreateInfo, 2> under_stages = {{
            {{}, vk::ShaderStageFlagBits::eVertex, *fullscreen, "main"},
            {{}, vk::ShaderStageFlagBits::eFragment, *under, "main"},
        }};
        vk::PipelineVertexInputStateCreateInfo no_input{};
        vk::PipelineDepthStencilStateCreateInfo no_depth{};
        vk::PipelineColorBlendAttachmentState opaque{};
        opaque.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                                vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
        vk::PipelineColorBlendStateCreateInfo under_blend{};
        under_blend.setAttachments(opaque);
        vk::GraphicsPipelineCreateInfo under_info = info;
        under_info.setStages(under_stages);
        under_info.pVertexInputState = &no_input;
        under_info.pDepthStencilState = &no_depth;
        under_info.pColorBlendState = &under_blend;
        underwater_pipeline_ = vk::raii::Pipeline(device.handle(), device.pipelineCache(), under_info);
    }

    createMeshes(device);
}

void WaterPass::destroy() {
    bodies_.clear();
    garbage_.clear();
    for (Mesh& river : rivers_) river = Mesh{};
    grid_ = Mesh{};
    ocean_ = Mesh{};
    for (VulkanBuffer& buffer : uniforms_) buffer.destroy();
    uniforms_.clear();
    sets_.clear();
    pool_ = nullptr;
    pipeline_ = nullptr;
    underwater_pipeline_ = nullptr;
    layout_ = nullptr;
    set_layout_ = nullptr;
    device_ = nullptr;
}

void WaterPass::upload(const VulkanDevice& device, Mesh& mesh, const std::vector<WaterVertex>& vertices,
                       const std::vector<std::uint32_t>& indices) {
    mesh.vertices = VulkanBuffer::createDeviceLocal(device, vertices.data(), sizeof(WaterVertex) * vertices.size(),
                                                    vk::BufferUsageFlagBits::eVertexBuffer);
    mesh.indices = VulkanBuffer::createDeviceLocal(device, indices.data(), sizeof(std::uint32_t) * indices.size(),
                                                   vk::BufferUsageFlagBits::eIndexBuffer);
    mesh.index_count = static_cast<std::uint32_t>(indices.size());
}

void WaterPass::createMeshes(const VulkanDevice& device) {
    // --- Lago: cuadricula en 0..1 ---
    {
        constexpr std::uint32_t n = kGridCells + 1;
        std::vector<WaterVertex> vertices;
        vertices.reserve(n * n);
        for (std::uint32_t z = 0; z < n; ++z) {
            for (std::uint32_t x = 0; x < n; ++x) {
                const float u = static_cast<float>(x) / kGridCells;
                const float v = static_cast<float>(z) / kGridCells;
                vertices.push_back(WaterVertex{{u, 0.0f, v}, {u, v}, {0.0f, 0.0f}});
            }
        }
        std::vector<std::uint32_t> indices;
        indices.reserve(kGridCells * kGridCells * 6);
        for (std::uint32_t z = 0; z < kGridCells; ++z) {
            for (std::uint32_t x = 0; x < kGridCells; ++x) {
                const std::uint32_t a = z * n + x;
                indices.insert(indices.end(), {a, a + n, a + 1, a + 1, a + n, a + n + 1});
            }
        }
        upload(device, grid_, vertices, indices);
    }
    // --- Oceano: disco radial alrededor de la camara, cada vez mas espaciado ---
    {
        std::vector<WaterVertex> vertices;
        vertices.push_back(WaterVertex{{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}});
        for (std::uint32_t ring = 1; ring <= kOceanRings; ++ring) {
            // 0.35 m cerca de la camara; unos 12 km en el ultimo anillo.
            const float radius = 0.35f * (std::pow(1.06f, static_cast<float>(ring)) - 1.0f) / 0.06f;
            for (std::uint32_t s = 0; s < kOceanSegments; ++s) {
                const float angle = 6.28318530718f * static_cast<float>(s) / kOceanSegments;
                vertices.push_back(
                    WaterVertex{{std::cos(angle) * radius, 0.0f, std::sin(angle) * radius}, {0.0f, 0.0f}, {0.0f, 0.0f}});
            }
        }
        std::vector<std::uint32_t> indices;
        for (std::uint32_t s = 0; s < kOceanSegments; ++s) {
            indices.insert(indices.end(), {0u, 1 + (s + 1) % kOceanSegments, 1 + s});
        }
        for (std::uint32_t ring = 1; ring < kOceanRings; ++ring) {
            const std::uint32_t inner = 1 + (ring - 1) * kOceanSegments;
            const std::uint32_t outer = inner + kOceanSegments;
            for (std::uint32_t s = 0; s < kOceanSegments; ++s) {
                const std::uint32_t s1 = (s + 1) % kOceanSegments;
                indices.insert(indices.end(), {inner + s, inner + s1, outer + s, inner + s1, outer + s1, outer + s});
            }
        }
        upload(device, ocean_, vertices, indices);
    }
}

void WaterPass::setBodies(const std::vector<WaterBodyDesc>& bodies, float time, int underwater) {
    time_ = time;
    underwater_ = underwater < static_cast<int>(std::min<std::size_t>(bodies.size(), kMaxWaterBodies)) ? underwater : -1;
    bodies_.assign(bodies.begin(), bodies.begin() + std::min<std::size_t>(bodies.size(), kMaxWaterBodies));
}

void WaterPass::prepare(std::uint32_t frame) {
    if (device_ == nullptr || frame >= uniforms_.size()) return;
    // Mallas viejas de rios: se liberan cuando ningun frame en vuelo las usa.
    for (auto it = garbage_.begin(); it != garbage_.end();) {
        if (it->frames_left == 0) {
            it = garbage_.erase(it);
        } else {
            --it->frames_left;
            ++it;
        }
    }
    GpuWaterUniform data{};
    data.time_count = core::Vec4{time_, static_cast<float>(bodies_.size()), 0.0f, 0.0f};
    for (std::size_t i = 0; i < bodies_.size(); ++i) {
        const WaterBodyDesc& body = bodies_[i];
        data.bodies[i] = body.params;
        Mesh& river = rivers_[i];
        if (body.params.extent.z > 1.5f && river.version != body.mesh_version) {
            if (river.index_count > 0) garbage_.push_back(Garbage{std::move(river), frames_ + 1});
            river = Mesh{};
            if (!body.indices.empty()) upload(*device_, river, body.vertices, body.indices);
            river.version = body.mesh_version;
        }
    }
    uniforms_[frame].write(&data, sizeof(data));
}

void WaterPass::record(const vk::raii::CommandBuffer& cmd, std::uint32_t frame,
                       const vk::raii::DescriptorSet& frame_set, const vk::raii::DescriptorSet& scene_set) {
    if (bodies_.empty() || frame >= sets_.size()) return;
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *layout_, 0,
                           {*frame_set, *sets_[frame], *scene_set}, nullptr);
    // Primero lo que se ve bajo el agua (con la copia de la escena), despues
    // las superficies encima.
    if (underwater_ >= 0) {
        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *underwater_pipeline_);
        GpuWaterPush push{};
        push.body = static_cast<std::uint32_t>(underwater_);
        cmd.pushConstants<GpuWaterPush>(*layout_, vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
                                        0, push);
        cmd.draw(3, 1, 0, 0);
    }
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *pipeline_);
    for (std::uint32_t i = 0; i < bodies_.size(); ++i) {
        const float type = bodies_[i].params.extent.z;
        const Mesh* mesh = type < 0.5f ? &ocean_ : (type < 1.5f ? &grid_ : &rivers_[i]);
        if (mesh->index_count == 0) continue;
        GpuWaterPush push{};
        push.body = i;
        push.mesh = type < 0.5f ? 1u : (type < 1.5f ? 0u : 2u);
        cmd.pushConstants<GpuWaterPush>(*layout_, vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
                                        0, push);
        cmd.bindVertexBuffers(0, *mesh->vertices.handle(), {0});
        cmd.bindIndexBuffer(*mesh->indices.handle(), 0, vk::IndexType::eUint32);
        cmd.drawIndexed(mesh->index_count, 1, 0, 0, 0);
    }
}

}  // namespace cramion::gfx
