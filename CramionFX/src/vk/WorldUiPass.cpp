#include "CramionFX/vk/WorldUiPass.h"

#include "CramionFX/vk/VulkanDevice.h"
#include "CramionFX/vk/VulkanShader.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>

namespace cramion::gfx {

namespace {

// RGBA8 sin sRGB: guarda los colores con gamma, como la UI de la pantalla.
constexpr vk::Format kTextureFormat = vk::Format::eR8G8B8A8Unorm;

struct PaintPush {
    float scale[2];
    float translate[2];
};

struct QuadPush {
    core::Mat4 transform;
    core::Vec4 params;  // x = opacidad
};

vk::ImageMemoryBarrier2 imageBarrier(vk::Image image, vk::ImageLayout from, vk::ImageLayout to,
                                     vk::PipelineStageFlags2 src_stage, vk::AccessFlags2 src_access,
                                     vk::PipelineStageFlags2 dst_stage, vk::AccessFlags2 dst_access) {
    vk::ImageMemoryBarrier2 barrier{};
    barrier.srcStageMask = src_stage;
    barrier.srcAccessMask = src_access;
    barrier.dstStageMask = dst_stage;
    barrier.dstAccessMask = dst_access;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.image = image;
    barrier.subresourceRange = vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    return barrier;
}

void pipelineBarrier(const vk::raii::CommandBuffer& cmd, const vk::ImageMemoryBarrier2& barrier) {
    vk::DependencyInfo dependency{};
    dependency.setImageMemoryBarriers(barrier);
    cmd.pipelineBarrier2(dependency);
}

}  // namespace

void WorldUiPass::create(const VulkanDevice& device, vk::Format scene_format, vk::Format depth_format,
                         std::uint32_t frames_in_flight) {
    destroy();
    frames_in_flight_ = std::max(frames_in_flight, 1u);

    // Un combined image sampler en el binding 0 del fragment shader: igual
    // que el de ImGui, asi sus descriptor sets (letras, imagenes) sirven aqui.
    vk::DescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = vk::DescriptorType::eCombinedImageSampler;
    binding.descriptorCount = 1;
    binding.stageFlags = vk::ShaderStageFlagBits::eFragment;
    vk::DescriptorSetLayoutCreateInfo set_info{};
    set_info.setBindings(binding);
    set_layout_ = vk::raii::DescriptorSetLayout(device.handle(), set_info);

    vk::PushConstantRange paint_push{vk::ShaderStageFlagBits::eVertex, 0, sizeof(PaintPush)};
    vk::PipelineLayoutCreateInfo paint_info{};
    paint_info.setSetLayouts(*set_layout_);
    paint_info.setPushConstantRanges(paint_push);
    paint_layout_ = vk::raii::PipelineLayout(device.handle(), paint_info);

    vk::PushConstantRange quad_push{vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment, 0,
                                    sizeof(QuadPush)};
    vk::PipelineLayoutCreateInfo quad_info{};
    quad_info.setSetLayouts(*set_layout_);
    quad_info.setPushConstantRanges(quad_push);
    quad_layout_ = vk::raii::PipelineLayout(device.handle(), quad_info);

    paint_pipeline_ = createPipeline(device, false, kTextureFormat, vk::Format::eUndefined);
    quad_pipeline_ = createPipeline(device, true, scene_format, depth_format);

    vk::SamplerCreateInfo sampler_info{};
    sampler_info.magFilter = vk::Filter::eLinear;
    sampler_info.minFilter = vk::Filter::eLinear;
    sampler_info.mipmapMode = vk::SamplerMipmapMode::eLinear;
    sampler_info.addressModeU = vk::SamplerAddressMode::eClampToEdge;
    sampler_info.addressModeV = vk::SamplerAddressMode::eClampToEdge;
    sampler_info.addressModeW = vk::SamplerAddressMode::eClampToEdge;
    sampler_info.maxLod = 0.0f;
    sampler_ = vk::raii::Sampler(device.handle(), sampler_info);

    // Un set por canvas (su textura en el panel) y el blanco.
    constexpr std::uint32_t kMaxSets = 128;
    const vk::DescriptorPoolSize pool_size{vk::DescriptorType::eCombinedImageSampler, kMaxSets};
    vk::DescriptorPoolCreateInfo pool_info{};
    pool_info.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    pool_info.maxSets = kMaxSets;
    pool_info.setPoolSizes(pool_size);
    pool_ = vk::raii::DescriptorPool(device.handle(), pool_info);

    // La textura blanca de las formas lisas.
    white_.create(device, vk::Extent2D{1, 1}, kTextureFormat,
                  vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
                  vk::ImageAspectFlagBits::eColor);
    device.submitOneTime([&](const vk::raii::CommandBuffer& cmd) {
        pipelineBarrier(cmd, imageBarrier(*white_.handle(), vk::ImageLayout::eUndefined,
                                          vk::ImageLayout::eTransferDstOptimal, vk::PipelineStageFlagBits2::eTopOfPipe,
                                          vk::AccessFlagBits2::eNone, vk::PipelineStageFlagBits2::eClear,
                                          vk::AccessFlagBits2::eTransferWrite));
        cmd.clearColorImage(*white_.handle(), vk::ImageLayout::eTransferDstOptimal,
                            vk::ClearColorValue{1.0f, 1.0f, 1.0f, 1.0f},
                            vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1});
        pipelineBarrier(cmd, imageBarrier(*white_.handle(), vk::ImageLayout::eTransferDstOptimal,
                                          vk::ImageLayout::eShaderReadOnlyOptimal, vk::PipelineStageFlagBits2::eClear,
                                          vk::AccessFlagBits2::eTransferWrite,
                                          vk::PipelineStageFlagBits2::eFragmentShader,
                                          vk::AccessFlagBits2::eShaderSampledRead));
    });
    vk::DescriptorSetAllocateInfo alloc{};
    alloc.descriptorPool = *pool_;
    alloc.setSetLayouts(*set_layout_);
    white_set_ = std::move(vk::raii::DescriptorSets(device.handle(), alloc).front());
    vk::DescriptorImageInfo white_info{*sampler_, *white_.view(), vk::ImageLayout::eShaderReadOnlyOptimal};
    vk::WriteDescriptorSet write{};
    write.dstSet = *white_set_;
    write.dstBinding = 0;
    write.descriptorType = vk::DescriptorType::eCombinedImageSampler;
    write.setImageInfo(white_info);
    device.handle().updateDescriptorSets(write, nullptr);
    white_ready_ = true;

    vertex_buffers_.resize(frames_in_flight_);
    index_buffers_.resize(frames_in_flight_);
}

void WorldUiPass::destroy() {
    canvases_.clear();
    retired_.clear();
    textures_.clear();
    for (VulkanBuffer& buffer : vertex_buffers_) buffer.destroy();
    for (VulkanBuffer& buffer : index_buffers_) buffer.destroy();
    vertex_buffers_.clear();
    index_buffers_.clear();
    white_set_ = nullptr;
    white_.destroy();
    white_ready_ = false;
    pool_ = nullptr;
    sampler_ = nullptr;
    quad_pipeline_ = nullptr;
    paint_pipeline_ = nullptr;
    quad_layout_ = nullptr;
    paint_layout_ = nullptr;
    set_layout_ = nullptr;
    dirty_ = false;
}

vk::raii::Pipeline WorldUiPass::createPipeline(const VulkanDevice& device, bool quad, vk::Format color_format,
                                               vk::Format depth_format) const {
    const vk::raii::ShaderModule vertex_module =
        shaders::loadModule(device, quad ? "world_ui_quad.vert.spv" : "world_ui.vert.spv");
    const vk::raii::ShaderModule fragment_module =
        shaders::loadModule(device, quad ? "world_ui_quad.frag.spv" : "world_ui.frag.spv");
    std::array<vk::PipelineShaderStageCreateInfo, 2> stages{};
    stages[0].stage = vk::ShaderStageFlagBits::eVertex;
    stages[0].module = *vertex_module;
    stages[0].pName = "main";
    stages[1].stage = vk::ShaderStageFlagBits::eFragment;
    stages[1].module = *fragment_module;
    stages[1].pName = "main";

    // La textura: vertices de ImGui. El panel: sin buffer (gl_VertexIndex).
    const vk::VertexInputBindingDescription binding{0, sizeof(WorldUiVertex), vk::VertexInputRate::eVertex};
    const std::array<vk::VertexInputAttributeDescription, 3> attributes = {{
        {0, 0, vk::Format::eR32G32Sfloat, offsetof(WorldUiVertex, x)},
        {1, 0, vk::Format::eR32G32Sfloat, offsetof(WorldUiVertex, u)},
        {2, 0, vk::Format::eR8G8B8A8Unorm, offsetof(WorldUiVertex, color)},
    }};
    vk::PipelineVertexInputStateCreateInfo vertex_input{};
    if (!quad) {
        vertex_input.setVertexBindingDescriptions(binding);
        vertex_input.setVertexAttributeDescriptions(attributes);
    }

    vk::PipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.topology = vk::PrimitiveTopology::eTriangleList;

    vk::PipelineViewportStateCreateInfo viewport_state{};
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;

    vk::PipelineRasterizationStateCreateInfo rasterization{};
    rasterization.polygonMode = vk::PolygonMode::eFill;
    rasterization.cullMode = vk::CullModeFlagBits::eNone;  // el panel se ve por las dos caras (como uGUI)
    rasterization.frontFace = vk::FrontFace::eCounterClockwise;
    rasterization.lineWidth = 1.0f;

    vk::PipelineMultisampleStateCreateInfo multisample{};
    multisample.rasterizationSamples = vk::SampleCountFlagBits::e1;

    vk::PipelineDepthStencilStateCreateInfo depth_stencil{};
    depth_stencil.depthTestEnable = quad ? VK_TRUE : VK_FALSE;
    depth_stencil.depthWriteEnable = VK_FALSE;  // la escena conserva su depth
    depth_stencil.depthCompareOp = vk::CompareOp::eLessOrEqual;

    // La textura: alfa sobre transparente (queda premultiplicada). El panel:
    // premultiplicado sobre la escena, sin tocar su alfa.
    vk::PipelineColorBlendAttachmentState blend{};
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = quad ? vk::BlendFactor::eOne : vk::BlendFactor::eSrcAlpha;
    blend.dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
    blend.colorBlendOp = vk::BlendOp::eAdd;
    blend.srcAlphaBlendFactor = quad ? vk::BlendFactor::eZero : vk::BlendFactor::eOne;
    blend.dstAlphaBlendFactor = quad ? vk::BlendFactor::eOne : vk::BlendFactor::eOneMinusSrcAlpha;
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
    info.layout = quad ? *quad_layout_ : *paint_layout_;
    return vk::raii::Pipeline(device.handle(), nullptr, info);
}

void WorldUiPass::setCanvases(std::vector<WorldUiCanvas> canvases) {
    canvases_ = std::move(canvases);
    dirty_ = true;
    // Las de canvas que ya no estan, a retirar (la GPU aun puede leerlas).
    for (auto it = textures_.begin(); it != textures_.end();) {
        const std::uint64_t id = (*it)->id;
        const bool used = std::any_of(canvases_.begin(), canvases_.end(),
                                      [&](const WorldUiCanvas& c) { return c.id == id; });
        if (used) {
            ++it;
            continue;
        }
        retire(std::move(*it));
        it = textures_.erase(it);
    }
    for (Retired& r : retired_) {
        if (r.frames_left > 0) --r.frames_left;
    }
    std::erase_if(retired_, [](const Retired& r) { return r.frames_left == 0; });
}

void WorldUiPass::retire(std::unique_ptr<Texture> texture) {
    retired_.push_back(Retired{std::move(texture), frames_in_flight_ + 2});
}

WorldUiPass::Texture* WorldUiPass::textureFor(const VulkanDevice& device, const WorldUiCanvas& canvas) {
    for (auto it = textures_.begin(); it != textures_.end(); ++it) {
        if ((*it)->id != canvas.id) continue;
        const vk::Extent2D extent = (*it)->image.extent();
        if (extent.width == canvas.width && extent.height == canvas.height) return it->get();
        retire(std::move(*it));  // otra resolucion: una nueva
        textures_.erase(it);
        break;
    }
    auto texture = std::make_unique<Texture>();
    texture->id = canvas.id;
    texture->image.create(device, vk::Extent2D{canvas.width, canvas.height}, kTextureFormat,
                          vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled,
                          vk::ImageAspectFlagBits::eColor);
    vk::DescriptorSetAllocateInfo alloc{};
    alloc.descriptorPool = *pool_;
    alloc.setSetLayouts(*set_layout_);
    texture->set = std::move(vk::raii::DescriptorSets(device.handle(), alloc).front());
    vk::DescriptorImageInfo image_info{*sampler_, *texture->image.view(), vk::ImageLayout::eShaderReadOnlyOptimal};
    vk::WriteDescriptorSet write{};
    write.dstSet = *texture->set;
    write.dstBinding = 0;
    write.descriptorType = vk::DescriptorType::eCombinedImageSampler;
    write.setImageInfo(image_info);
    device.handle().updateDescriptorSets(write, nullptr);
    textures_.push_back(std::move(texture));
    return textures_.back().get();
}

void WorldUiPass::recordPaint(const VulkanDevice& device, const vk::raii::CommandBuffer& cmd, std::uint32_t frame) {
    dirty_ = false;
    if (canvases_.empty() || !white_ready_ || frame >= vertex_buffers_.size()) return;

    // Todos los vertices e indices del frame en sus buffers (por frame en vuelo).
    std::size_t vertex_count = 0;
    std::size_t index_count = 0;
    for (const WorldUiCanvas& c : canvases_) {
        vertex_count += c.vertices.size();
        index_count += c.indices.size();
    }
    VulkanBuffer& vertices = vertex_buffers_[frame];
    VulkanBuffer& indices = index_buffers_[frame];
    const vk::DeviceSize vertex_bytes = std::max<vk::DeviceSize>(vertex_count * sizeof(WorldUiVertex), 64);
    const vk::DeviceSize index_bytes = std::max<vk::DeviceSize>(index_count * sizeof(std::uint32_t), 64);
    const vk::MemoryPropertyFlags host = vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;
    if (!vertices.isValid() || vertices.size() < vertex_bytes) {
        vertices.create(device, vertex_bytes * 2, vk::BufferUsageFlagBits::eVertexBuffer, host);
    }
    if (!indices.isValid() || indices.size() < index_bytes) {
        indices.create(device, index_bytes * 2, vk::BufferUsageFlagBits::eIndexBuffer, host);
    }
    std::vector<std::size_t> first_vertex(canvases_.size());
    std::vector<std::size_t> first_index(canvases_.size());
    {
        auto* vertex_out = static_cast<std::uint8_t*>(vertices.mapped());
        auto* index_out = static_cast<std::uint8_t*>(indices.mapped());
        std::size_t v = 0;
        std::size_t i = 0;
        for (std::size_t c = 0; c < canvases_.size(); ++c) {
            const WorldUiCanvas& canvas = canvases_[c];
            first_vertex[c] = v;
            first_index[c] = i;
            if (!canvas.vertices.empty()) {
                std::memcpy(vertex_out + v * sizeof(WorldUiVertex), canvas.vertices.data(),
                            canvas.vertices.size() * sizeof(WorldUiVertex));
            }
            if (!canvas.indices.empty()) {
                std::memcpy(index_out + i * sizeof(std::uint32_t), canvas.indices.data(),
                            canvas.indices.size() * sizeof(std::uint32_t));
            }
            v += canvas.vertices.size();
            i += canvas.indices.size();
        }
    }

    for (std::size_t c = 0; c < canvases_.size(); ++c) {
        const WorldUiCanvas& canvas = canvases_[c];
        if (canvas.width == 0 || canvas.height == 0) continue;
        Texture* texture = textureFor(device, canvas);
        const vk::Image image = *texture->image.handle();
        pipelineBarrier(cmd, imageBarrier(image,
                                          texture->painted ? vk::ImageLayout::eShaderReadOnlyOptimal
                                                           : vk::ImageLayout::eUndefined,
                                          vk::ImageLayout::eColorAttachmentOptimal,
                                          vk::PipelineStageFlagBits2::eFragmentShader,
                                          vk::AccessFlagBits2::eShaderSampledRead,
                                          vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                          vk::AccessFlagBits2::eColorAttachmentWrite));
        vk::RenderingAttachmentInfo color{};
        color.imageView = *texture->image.view();
        color.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
        color.loadOp = vk::AttachmentLoadOp::eClear;
        color.storeOp = vk::AttachmentStoreOp::eStore;
        color.clearValue = vk::ClearValue{vk::ClearColorValue{0.0f, 0.0f, 0.0f, 0.0f}};
        vk::RenderingInfo rendering{};
        rendering.renderArea = vk::Rect2D{vk::Offset2D{0, 0}, vk::Extent2D{canvas.width, canvas.height}};
        rendering.layerCount = 1;
        rendering.setColorAttachments(color);
        cmd.beginRendering(rendering);
        if (!canvas.indices.empty() && !canvas.batches.empty()) {
            const float w = static_cast<float>(canvas.width);
            const float h = static_cast<float>(canvas.height);
            cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *paint_pipeline_);
            cmd.setViewport(0, vk::Viewport{0.0f, 0.0f, w, h, 0.0f, 1.0f});
            const PaintPush push{{2.0f / w, 2.0f / h}, {-1.0f, -1.0f}};
            cmd.pushConstants<PaintPush>(*paint_layout_, vk::ShaderStageFlagBits::eVertex, 0, push);
            cmd.bindVertexBuffers(0, *vertices.handle(), vk::DeviceSize{first_vertex[c] * sizeof(WorldUiVertex)});
            cmd.bindIndexBuffer(*indices.handle(), first_index[c] * sizeof(std::uint32_t), vk::IndexType::eUint32);
            for (const WorldUiBatch& batch : canvas.batches) {
                if (batch.index_count == 0) continue;
                const float x0 = std::clamp(batch.clip[0], 0.0f, w);
                const float y0 = std::clamp(batch.clip[1], 0.0f, h);
                const float x1 = std::clamp(batch.clip[2], 0.0f, w);
                const float y1 = std::clamp(batch.clip[3], 0.0f, h);
                if (x1 <= x0 || y1 <= y0) continue;
                cmd.setScissor(0, vk::Rect2D{vk::Offset2D{static_cast<std::int32_t>(x0), static_cast<std::int32_t>(y0)},
                                             vk::Extent2D{static_cast<std::uint32_t>(x1 - x0),
                                                          static_cast<std::uint32_t>(y1 - y0)}});
                const vk::DescriptorSet set =
                    batch.texture != VK_NULL_HANDLE ? vk::DescriptorSet(batch.texture) : *white_set_;
                cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *paint_layout_, 0, set, nullptr);
                cmd.drawIndexed(batch.index_count, 1, batch.first_index, static_cast<std::int32_t>(batch.vertex_offset), 0);
            }
        }
        cmd.endRendering();
        pipelineBarrier(cmd, imageBarrier(image, vk::ImageLayout::eColorAttachmentOptimal,
                                          vk::ImageLayout::eShaderReadOnlyOptimal,
                                          vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                          vk::AccessFlagBits2::eColorAttachmentWrite,
                                          vk::PipelineStageFlagBits2::eFragmentShader,
                                          vk::AccessFlagBits2::eShaderSampledRead));
        texture->painted = true;
    }
}

void WorldUiPass::recordQuads(const vk::raii::CommandBuffer& cmd, const core::Mat4& view_projection,
                              vk::Extent2D viewport) const {
    bool bound = false;
    for (const WorldUiCanvas& canvas : canvases_) {
        const auto it = std::find_if(textures_.begin(), textures_.end(),
                                     [&](const std::unique_ptr<Texture>& t) { return t->id == canvas.id; });
        if (it == textures_.end() || !(*it)->painted || canvas.size.x <= 0.0f || canvas.size.y <= 0.0f) continue;
        if (!bound) {
            cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *quad_pipeline_);
            cmd.setViewport(0, vk::Viewport{0.0f, 0.0f, static_cast<float>(viewport.width),
                                            static_cast<float>(viewport.height), 0.0f, 1.0f});
            cmd.setScissor(0, vk::Rect2D{vk::Offset2D{0, 0}, viewport});
            bound = true;
        }
        core::Mat4 size = core::Mat4::identity();
        size.m[0][0] = canvas.size.x;
        size.m[1][1] = canvas.size.y;
        QuadPush push{};
        push.transform = view_projection * canvas.transform * size;
        push.params = core::Vec4{std::clamp(canvas.opacity, 0.0f, 1.0f), 0.0f, 0.0f, 0.0f};
        cmd.pushConstants<QuadPush>(*quad_layout_, vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
                                    0, push);
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *quad_layout_, 0, *(*it)->set, nullptr);
        cmd.draw(6, 1, 0, 0);
    }
}

}  // namespace cramion::gfx
