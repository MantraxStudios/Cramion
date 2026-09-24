#include "CramionFX/vk/ParticlePass.h"

#include "CramionFX/vk/VulkanDevice.h"
#include "CramionFX/vk/VulkanShader.h"

#include <algorithm>
#include <array>
#include <cstddef>

namespace cramion::gfx {

void ParticlePass::create(const VulkanDevice& device, vk::Format color_format,
                          vk::Format depth_format, std::uint32_t frames_in_flight) {
    destroy();

    vk::PipelineLayoutCreateInfo layout_info{};
    layout_ = vk::raii::PipelineLayout(device.handle(), layout_info);

    alpha_pipeline_ = createPipeline(device, color_format, depth_format, false);
    additive_pipeline_ = createPipeline(device, color_format, depth_format, true);

    buffers_.resize(frames_in_flight);
    alpha_counts_.assign(frames_in_flight, 0);
    additive_counts_.assign(frames_in_flight, 0);
}

void ParticlePass::destroy() {
    for (VulkanBuffer& buffer : buffers_) {
        buffer.destroy();
    }
    buffers_.clear();
    alpha_counts_.clear();
    additive_counts_.clear();
    additive_pipeline_ = nullptr;
    alpha_pipeline_ = nullptr;
    layout_ = nullptr;
}

vk::raii::Pipeline ParticlePass::createPipeline(const VulkanDevice& device, vk::Format color_format,
                                                vk::Format depth_format, bool additive) const {
    const vk::raii::ShaderModule vertex_module = shaders::loadModule(device, "particle.vert.spv");
    const vk::raii::ShaderModule fragment_module = shaders::loadModule(device, "particle.frag.spv");
    std::array<vk::PipelineShaderStageCreateInfo, 2> stages{};
    stages[0].stage = vk::ShaderStageFlagBits::eVertex;
    stages[0].module = *vertex_module;
    stages[0].pName = "main";
    stages[1].stage = vk::ShaderStageFlagBits::eFragment;
    stages[1].module = *fragment_module;
    stages[1].pName = "main";

    const vk::VertexInputBindingDescription binding{0, sizeof(Vertex), vk::VertexInputRate::eVertex};
    const std::array<vk::VertexInputAttributeDescription, 3> attributes = {{
        {0, 0, vk::Format::eR32G32B32A32Sfloat, offsetof(Vertex, clip)},
        {1, 0, vk::Format::eR32G32B32A32Sfloat, offsetof(Vertex, color)},
        {2, 0, vk::Format::eR32G32Sfloat, offsetof(Vertex, uv)},
    }};
    vk::PipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.setVertexBindingDescriptions(binding);
    vertex_input.setVertexAttributeDescriptions(attributes);

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

    vk::PipelineColorBlendAttachmentState blend{};
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = vk::BlendFactor::eSrcAlpha;
    blend.dstColorBlendFactor = additive ? vk::BlendFactor::eOne : vk::BlendFactor::eOneMinusSrcAlpha;
    blend.colorBlendOp = vk::BlendOp::eAdd;
    blend.srcAlphaBlendFactor = vk::BlendFactor::eZero;
    blend.dstAlphaBlendFactor = vk::BlendFactor::eOne;
    blend.alphaBlendOp = vk::BlendOp::eAdd;
    blend.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                           vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
    vk::PipelineColorBlendStateCreateInfo color_blend{};
    color_blend.setAttachments(blend);

    const std::array<vk::DynamicState, 2> dynamic_states = {vk::DynamicState::eViewport,
                                                            vk::DynamicState::eScissor};
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
    return vk::raii::Pipeline(device.handle(), nullptr, info);
}

bool ParticlePass::prepare(const VulkanDevice& device, std::uint32_t frame,
                           const ParticleDrawList& particles, const core::Mat4& view,
                           const core::Mat4& view_projection) {
    if (frame >= buffers_.size()) {
        return false;
    }
    alpha_counts_[frame] = 0;
    additive_counts_[frame] = 0;
    if (particles.empty()) {
        return false;
    }

    // Ejes derecho y arriba de la camara (filas de la matriz de vista).
    const core::Vec3 right = core::normalize(core::Vec3{view.m[0][0], view.m[1][0], view.m[2][0]});
    const core::Vec3 up = core::normalize(core::Vec3{view.m[0][1], view.m[1][1], view.m[2][1]});

    scratch_.clear();
    scratch_.reserve(particles.size() * 6);
    const auto expand = [&](const std::vector<ParticleInstance>& list) {
        static constexpr float kCorners[6][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, -1}, {1, 1}, {-1, 1}};
        for (const ParticleInstance& p : list) {
            if (p.color.w <= 0.0f || p.size <= 0.0f) {
                continue;
            }
            const float half = p.size * 0.5f;
            for (const auto& corner : kCorners) {
                const core::Vec3 world = p.position + right * (corner[0] * half) + up * (corner[1] * half);
                const core::Vec4 clip = view_projection * core::Vec4{world.x, world.y, world.z, 1.0f};
                scratch_.push_back(Vertex{{clip.x, clip.y, clip.z, clip.w},
                                          {p.color.x, p.color.y, p.color.z, p.color.w},
                                          {corner[0], corner[1]}});
            }
        }
    };
    expand(particles.alpha);
    const auto alpha_vertices = static_cast<std::uint32_t>(scratch_.size());
    expand(particles.additive);
    const auto additive_vertices = static_cast<std::uint32_t>(scratch_.size()) - alpha_vertices;
    if (scratch_.empty()) {
        return false;
    }

    const vk::DeviceSize bytes = sizeof(Vertex) * scratch_.size();
    VulkanBuffer& buffer = buffers_[frame];
    if (!buffer.isValid() || buffer.size() < bytes) {
        buffer.destroy();
        buffer.create(device, std::max<vk::DeviceSize>(bytes * 2, 256 * 1024),
                      vk::BufferUsageFlagBits::eVertexBuffer,
                      vk::MemoryPropertyFlagBits::eHostVisible |
                          vk::MemoryPropertyFlagBits::eHostCoherent);
    }
    buffer.write(scratch_.data(), bytes);
    alpha_counts_[frame] = alpha_vertices;
    additive_counts_[frame] = additive_vertices;
    return true;
}

void ParticlePass::record(const vk::raii::CommandBuffer& cmd, std::uint32_t frame,
                          vk::Extent2D viewport) const {
    if (frame >= buffers_.size() || alpha_counts_[frame] + additive_counts_[frame] == 0) {
        return;
    }
    cmd.setViewport(0, vk::Viewport{0.0f, 0.0f, static_cast<float>(viewport.width),
                                    static_cast<float>(viewport.height), 0.0f, 1.0f});
    cmd.setScissor(0, vk::Rect2D{vk::Offset2D{0, 0}, viewport});
    cmd.bindVertexBuffers(0, *buffers_[frame].handle(), {0});
    if (alpha_counts_[frame] > 0) {
        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *alpha_pipeline_);
        cmd.draw(alpha_counts_[frame], 1, 0, 0);
    }
    if (additive_counts_[frame] > 0) {
        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *additive_pipeline_);
        cmd.draw(additive_counts_[frame], 1, alpha_counts_[frame], 0);
    }
}

}  // namespace cramion::gfx
