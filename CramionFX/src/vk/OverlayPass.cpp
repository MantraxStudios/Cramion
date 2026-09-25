#include "CramionFX/vk/OverlayPass.h"

#include "CramionFX/vk/VulkanDevice.h"
#include "CramionFX/vk/VulkanShader.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace cramion::gfx {

namespace {

struct OverlayPush {
    float half_width = 1.0f;
    float alpha_scale = 1.0f;
};

core::Vec4 toClip(const core::Mat4& view_projection, const core::Vec3& p) {
    return view_projection * core::Vec4{p.x, p.y, p.z, 1.0f};
}

core::Vec4 lerp4(const core::Vec4& a, const core::Vec4& b, float t) {
    return core::Vec4{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t,
                      a.w + (b.w - a.w) * t};
}

}  // namespace

void OverlayPass::create(const VulkanDevice& device, vk::Format color_format,
                         vk::Format depth_format, std::uint32_t frames_in_flight) {
    destroy();

    vk::PushConstantRange push_range{};
    push_range.stageFlags = vk::ShaderStageFlagBits::eFragment;
    push_range.size = sizeof(OverlayPush);
    vk::PipelineLayoutCreateInfo layout_info{};
    layout_info.setPushConstantRanges(push_range);
    layout_ = vk::raii::PipelineLayout(device.handle(), layout_info);

    visible_pipeline_ =
        createPipeline(device, color_format, depth_format, vk::CompareOp::eLessOrEqual);
    occluded_pipeline_ = createPipeline(device, color_format, depth_format, vk::CompareOp::eGreater);

    buffers_.resize(frames_in_flight);
    vertex_counts_.assign(frames_in_flight, 0);
}

void OverlayPass::destroy() {
    for (VulkanBuffer& buffer : buffers_) {
        buffer.destroy();
    }
    buffers_.clear();
    vertex_counts_.clear();
    occluded_pipeline_ = nullptr;
    visible_pipeline_ = nullptr;
    layout_ = nullptr;
}

vk::raii::Pipeline OverlayPass::createPipeline(const VulkanDevice& device, vk::Format color_format,
                                               vk::Format depth_format,
                                               vk::CompareOp compare) const {
    const vk::raii::ShaderModule vertex_module = shaders::loadModule(device, "overlay.vert.spv");
    const vk::raii::ShaderModule fragment_module = shaders::loadModule(device, "overlay.frag.spv");
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
        {1, 0, vk::Format::eR8G8B8A8Unorm, offsetof(Vertex, color)},
        {2, 0, vk::Format::eR32Sfloat, offsetof(Vertex, edge)},
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
    rasterization.cullMode = vk::CullModeFlagBits::eNone;  // gizmos por las dos caras
    rasterization.frontFace = vk::FrontFace::eCounterClockwise;
    rasterization.lineWidth = 1.0f;

    vk::PipelineMultisampleStateCreateInfo multisample{};
    multisample.rasterizationSamples = vk::SampleCountFlagBits::e1;

    // Prueba contra la escena sin escribir: el depth sigue siendo el suyo.
    vk::PipelineDepthStencilStateCreateInfo depth_stencil{};
    depth_stencil.depthTestEnable = VK_TRUE;
    depth_stencil.depthWriteEnable = VK_FALSE;
    depth_stencil.depthCompareOp = compare;

    vk::PipelineColorBlendAttachmentState blend{};
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = vk::BlendFactor::eSrcAlpha;
    blend.dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
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
    return vk::raii::Pipeline(device.handle(), device.pipelineCache(), info);
}

bool OverlayPass::prepare(const VulkanDevice& device, std::uint32_t frame,
                          const OverlayGeometry& geometry, const core::Mat4& view_projection,
                          vk::Extent2D viewport) {
    if (frame >= buffers_.size()) {
        return false;
    }
    vertex_counts_[frame] = 0;
    if (geometry.empty() || viewport.width == 0 || viewport.height == 0) {
        return false;
    }

    scratch_.clear();
    scratch_.reserve(geometry.triangles.size() + geometry.lines.size() * 3);
    const auto push = [&](const core::Vec4& clip, std::uint32_t color, float edge) {
        scratch_.push_back(Vertex{{clip.x, clip.y, clip.z, clip.w}, color, edge});
    };

    // --- Triangulos: en espacio de recorte; la GPU los recorta sola ---
    const std::size_t triangle_vertices = geometry.triangles.size() - geometry.triangles.size() % 3;
    for (std::size_t i = 0; i < triangle_vertices; ++i) {
        const OverlayVertex& v = geometry.triangles[i];
        push(toClip(view_projection, v.position), v.color, 0.0f);
    }

    // --- Lineas: recorte contra el plano cercano (z >= 0 en Vulkan) y
    // expansion a un quad de grosor constante en pixeles ---
    const float half_width = std::max(geometry.line_width * 0.5f, 0.5f);
    const float extent = half_width + 1.0f;  // medio pixel de mas para el antialias
    const float sx = static_cast<float>(viewport.width) * 0.5f;
    const float sy = static_cast<float>(viewport.height) * 0.5f;
    constexpr float kMinW = 1e-5f;
    for (std::size_t i = 0; i + 1 < geometry.lines.size(); i += 2) {
        const OverlayVertex& a = geometry.lines[i];
        const OverlayVertex& b = geometry.lines[i + 1];
        core::Vec4 ca = toClip(view_projection, a.position);
        core::Vec4 cb = toClip(view_projection, b.position);
        if (ca.z < 0.0f && cb.z < 0.0f) {
            continue;  // entera detras del plano cercano
        }
        if (ca.z < 0.0f) {
            ca = lerp4(ca, cb, ca.z / (ca.z - cb.z));
        } else if (cb.z < 0.0f) {
            cb = lerp4(cb, ca, cb.z / (cb.z - ca.z));
        }
        ca.w = std::max(ca.w, kMinW);
        cb.w = std::max(cb.w, kMinW);

        // Direccion en pixeles y su perpendicular.
        const float ax = ca.x / ca.w * sx;
        const float ay = ca.y / ca.w * sy;
        const float bx = cb.x / cb.w * sx;
        const float by = cb.y / cb.w * sy;
        float dx = bx - ax;
        float dy = by - ay;
        const float length = std::sqrt(dx * dx + dy * dy);
        if (length < 1e-4f) {
            dx = 1.0f;
            dy = 0.0f;
        } else {
            dx /= length;
            dy /= length;
        }
        // Desplazamiento en NDC de `extent` pixeles, perpendicular (y un poco
        // a lo largo, para que los extremos no queden cortados en seco).
        const float nx = -dy * extent / sx;
        const float ny = dx * extent / sy;
        const float tx = dx * 0.5f / sx;
        const float ty = dy * 0.5f / sy;
        const auto offset = [](const core::Vec4& c, float ox, float oy) {
            return core::Vec4{c.x + ox * c.w, c.y + oy * c.w, c.z, c.w};
        };
        const core::Vec4 a0 = offset(ca, nx - tx, ny - ty);
        const core::Vec4 a1 = offset(ca, -nx - tx, -ny - ty);
        const core::Vec4 b0 = offset(cb, nx + tx, ny + ty);
        const core::Vec4 b1 = offset(cb, -nx + tx, -ny + ty);
        push(a0, a.color, extent);
        push(a1, a.color, -extent);
        push(b0, b.color, extent);
        push(b0, b.color, extent);
        push(a1, a.color, -extent);
        push(b1, b.color, -extent);
    }

    if (scratch_.empty()) {
        return false;
    }

    // Buffer de este frame en vuelo: crece al doble si no cabe.
    const vk::DeviceSize bytes = sizeof(Vertex) * scratch_.size();
    VulkanBuffer& buffer = buffers_[frame];
    if (!buffer.isValid() || buffer.size() < bytes) {
        buffer.destroy();
        buffer.create(device, std::max<vk::DeviceSize>(bytes * 2, 64 * 1024),
                      vk::BufferUsageFlagBits::eVertexBuffer,
                      vk::MemoryPropertyFlagBits::eHostVisible |
                          vk::MemoryPropertyFlagBits::eHostCoherent);
    }
    buffer.write(scratch_.data(), bytes);
    vertex_counts_[frame] = static_cast<std::uint32_t>(scratch_.size());
    return true;
}

void OverlayPass::record(const vk::raii::CommandBuffer& cmd, std::uint32_t frame,
                         vk::Extent2D viewport, const OverlayGeometry& geometry) const {
    if (frame >= buffers_.size() || vertex_counts_[frame] == 0) {
        return;
    }
    cmd.setViewport(0, vk::Viewport{0.0f, 0.0f, static_cast<float>(viewport.width),
                                    static_cast<float>(viewport.height), 0.0f, 1.0f});
    cmd.setScissor(0, vk::Rect2D{vk::Offset2D{0, 0}, viewport});
    cmd.bindVertexBuffers(0, *buffers_[frame].handle(), {0});

    OverlayPush push{};
    push.half_width = std::max(geometry.line_width * 0.5f, 0.5f);

    // Primero lo tapado (atenuado) y luego lo visible encima.
    if (geometry.occluded_alpha > 0.0f) {
        push.alpha_scale = geometry.occluded_alpha;
        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *occluded_pipeline_);
        cmd.pushConstants<OverlayPush>(*layout_, vk::ShaderStageFlagBits::eFragment, 0, push);
        cmd.draw(vertex_counts_[frame], 1, 0, 0);
    }
    push.alpha_scale = 1.0f;
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *visible_pipeline_);
    cmd.pushConstants<OverlayPush>(*layout_, vk::ShaderStageFlagBits::eFragment, 0, push);
    cmd.draw(vertex_counts_[frame], 1, 0, 0);
}

}  // namespace cramion::gfx
