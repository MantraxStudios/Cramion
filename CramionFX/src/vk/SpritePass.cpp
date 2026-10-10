#include "CramionFX/vk/VulkanCompat.h"
#include "CramionFX/vk/SpritePass.h"

#include "CramionFX/asset/ImageFile.h"
#include "CramionFX/vk/VulkanDevice.h"
#include "CramionFX/vk/VulkanShader.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <iostream>

namespace cramion::gfx {

namespace {

// Frames sin usarse antes de liberar una textura (mucho mas que los frames en
// vuelo: nunca se libera una que la GPU pueda estar leyendo).
constexpr std::uint64_t kEvictFrames = 600;

std::string textureKey(const SpriteTexture& desc) {
    const std::u8string path = desc.file.u8string();
    return std::string(path.begin(), path.end()) + (desc.point_filter ? "|p" : "|l");
}

}  // namespace

void SpritePass::create(const VulkanDevice& device, vk::Format color_format, vk::Format depth_format,
                        std::uint32_t frames_in_flight) {
    destroy();
    frames_in_flight_ = frames_in_flight;

    // set 0: la textura de la tanda; set 1: las luces 2D del frame.
    const vk::DescriptorSetLayoutBinding texture_binding{0, vk::DescriptorType::eCombinedImageSampler, 1,
                                                         vk::ShaderStageFlagBits::eFragment};
    vk::DescriptorSetLayoutCreateInfo texture_info{};
    texture_info.setBindings(texture_binding);
    texture_layout_ = vk::raii::DescriptorSetLayout(device.handle(), texture_info);

    const vk::DescriptorSetLayoutBinding light_binding{0, vk::DescriptorType::eStorageBuffer, 1,
                                                       vk::ShaderStageFlagBits::eFragment};
    vk::DescriptorSetLayoutCreateInfo light_info{};
    light_info.setBindings(light_binding);
    light_layout_ = vk::raii::DescriptorSetLayout(device.handle(), light_info);

    const std::array<vk::DescriptorSetLayout, 2> set_layouts = {*texture_layout_, *light_layout_};
    vk::PushConstantRange push{};
    push.stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment;
    push.size = sizeof(Push);
    vk::PipelineLayoutCreateInfo layout_info{};
    layout_info.setSetLayouts(set_layouts);
    layout_info.setPushConstantRanges(push);
    layout_ = vk::raii::PipelineLayout(device.handle(), layout_info);

    // Texturas (+ las dos blancas) y un buffer de luces por frame.
    const std::array<vk::DescriptorPoolSize, 2> sizes = {{
        {vk::DescriptorType::eCombinedImageSampler, kMaxTextures + 2},
        {vk::DescriptorType::eStorageBuffer, frames_in_flight},
    }};
    vk::DescriptorPoolCreateInfo pool_info{};
    pool_info.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    pool_info.maxSets = kMaxTextures + 2 + frames_in_flight;
    pool_info.setPoolSizes(sizes);
    pool_ = vk::raii::DescriptorPool(device.handle(), pool_info);

    vk::SamplerCreateInfo linear{};
    linear.magFilter = vk::Filter::eLinear;
    linear.minFilter = vk::Filter::eLinear;
    linear.mipmapMode = vk::SamplerMipmapMode::eLinear;
    linear.addressModeU = linear.addressModeV = linear.addressModeW = vk::SamplerAddressMode::eClampToEdge;
    linear.maxLod = VK_LOD_CLAMP_NONE;
    linear_sampler_ = vk::raii::Sampler(device.handle(), linear);

    // Pixel art: el texel mas cercano y solo el nivel 0 (los mips lo emborronan).
    vk::SamplerCreateInfo point{};
    point.magFilter = vk::Filter::eNearest;
    point.minFilter = vk::Filter::eNearest;
    point.mipmapMode = vk::SamplerMipmapMode::eNearest;
    point.addressModeU = point.addressModeV = point.addressModeW = vk::SamplerAddressMode::eClampToEdge;
    point.maxLod = 0.0f;
    point_sampler_ = vk::raii::Sampler(device.handle(), point);

    pipeline_ = createPipeline(device, color_format, depth_format);

    vertex_buffers_.resize(frames_in_flight);
    light_buffers_.resize(frames_in_flight);
    batches_.assign(frames_in_flight, {});
    pushes_.assign(frames_in_flight, Push{});
    light_sets_.clear();
    for (std::uint32_t i = 0; i < frames_in_flight; ++i) {
        light_buffers_[i].create(device, sizeof(GpuLight) * kMaxLights, vk::BufferUsageFlagBits::eStorageBuffer,
                                 vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
        vk::DescriptorSetAllocateInfo allocate{};
        allocate.descriptorPool = *pool_;
        allocate.setSetLayouts(*light_layout_);
        light_sets_.push_back(std::move(vk::raii::DescriptorSets(device.handle(), allocate).front()));
        const vk::DescriptorBufferInfo buffer_info{*light_buffers_[i].handle(), 0, sizeof(GpuLight) * kMaxLights};
        vk::WriteDescriptorSet write{};
        write.dstSet = *light_sets_.back();
        write.dstBinding = 0;
        write.descriptorType = vk::DescriptorType::eStorageBuffer;
        write.setBufferInfo(buffer_info);
        device.handle().updateDescriptorSets(write, nullptr);
    }

    const std::array<std::uint8_t, 4> white = {255, 255, 255, 255};
    white_.reset(createEntry(device, white.data(), 1, 1, false));
    white_point_.reset(createEntry(device, white.data(), 1, 1, true));
}

void SpritePass::destroy() {
    textures_.clear();
    white_.reset();
    white_point_.reset();
    light_sets_.clear();
    for (VulkanBuffer& buffer : vertex_buffers_) buffer.destroy();
    for (VulkanBuffer& buffer : light_buffers_) buffer.destroy();
    vertex_buffers_.clear();
    light_buffers_.clear();
    batches_.clear();
    pushes_.clear();
    pipeline_ = nullptr;
    layout_ = nullptr;
    point_sampler_ = nullptr;
    linear_sampler_ = nullptr;
    pool_ = nullptr;
    light_layout_ = nullptr;
    texture_layout_ = nullptr;
}

vk::raii::Pipeline SpritePass::createPipeline(const VulkanDevice& device, vk::Format color_format,
                                              vk::Format depth_format) const {
    const vk::raii::ShaderModule vertex_module = shaders::loadModule(device, "sprite.vert.spv");
    const vk::raii::ShaderModule fragment_module = shaders::loadModule(device, "sprite.frag.spv");
    std::array<vk::PipelineShaderStageCreateInfo, 2> stages{};
    stages[0].stage = vk::ShaderStageFlagBits::eVertex;
    stages[0].module = *vertex_module;
    stages[0].pName = "main";
    stages[1].stage = vk::ShaderStageFlagBits::eFragment;
    stages[1].module = *fragment_module;
    stages[1].pName = "main";

    const vk::VertexInputBindingDescription binding{0, sizeof(Vertex), vk::VertexInputRate::eVertex};
    const std::array<vk::VertexInputAttributeDescription, 4> attributes = {{
        {0, 0, vk::Format::eR32G32B32Sfloat, offsetof(Vertex, position)},
        {1, 0, vk::Format::eR32G32Sfloat, offsetof(Vertex, uv)},
        {2, 0, vk::Format::eR32G32B32A32Sfloat, offsetof(Vertex, color)},
        {3, 0, vk::Format::eR32G32Sfloat, offsetof(Vertex, params)},
    }};
    vk::PipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.setVertexBindingDescriptions(binding);
    vertex_input.setVertexAttributeDescriptions(attributes);

    vk::PipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.topology = vk::PrimitiveTopology::eTriangleList;

    vk::PipelineViewportStateCreateInfo viewport_state{};
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;

    // Sin descarte de caras: los sprites volteados (escala negativa) se ven igual.
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
    return compat::makeGraphicsPipeline(device, info);
}

SpritePass::TextureEntry* SpritePass::createEntry(const VulkanDevice& device, const std::uint8_t* rgba,
                                                  std::uint32_t width, std::uint32_t height, bool point) {
    auto entry = std::make_unique<TextureEntry>();
    entry->texture.create(device, width, height, rgba);
    vk::DescriptorSetAllocateInfo allocate{};
    allocate.descriptorPool = *pool_;
    allocate.setSetLayouts(*texture_layout_);
    entry->set = std::move(vk::raii::DescriptorSets(device.handle(), allocate).front());
    const vk::DescriptorImageInfo image_info{point ? *point_sampler_ : *linear_sampler_, *entry->texture.view(),
                                             vk::ImageLayout::eShaderReadOnlyOptimal};
    vk::WriteDescriptorSet write{};
    write.dstSet = *entry->set;
    write.dstBinding = 0;
    write.descriptorType = vk::DescriptorType::eCombinedImageSampler;
    write.setImageInfo(image_info);
    device.handle().updateDescriptorSets(write, nullptr);
    return entry.release();
}

SpritePass::TextureEntry* SpritePass::texture(const VulkanDevice& device, const SpriteTexture& desc) {
    TextureEntry* fallback = desc.point_filter ? white_point_.get() : white_.get();
    if (desc.file.empty()) return fallback;
    const std::string key = textureKey(desc);
    if (const auto it = textures_.find(key); it != textures_.end()) {
        return it->second ? it->second.get() : fallback;
    }
    if (textures_.size() >= kMaxTextures) {
        evictUnused();
        if (textures_.size() >= kMaxTextures) return fallback;
    }
    asset::ImageRgba8 image;
    if (!asset::loadImageRgba8(desc.file, image) || image.width == 0 || image.height == 0) {
        std::cerr << "[Sprites] No se pudo leer la imagen " << desc.file.string() << "\n";
        textures_[key] = nullptr;  // no se vuelve a intentar cada frame
        return fallback;
    }
    TextureEntry* entry = createEntry(device, image.pixels.data(), image.width, image.height, desc.point_filter);
    textures_[key].reset(entry);
    return entry;
}

void SpritePass::evictUnused() {
    for (auto it = textures_.begin(); it != textures_.end();) {
        if (!it->second || it->second->last_used + kEvictFrames < frame_counter_) {
            it = textures_.erase(it);
        } else {
            ++it;
        }
    }
}

bool SpritePass::prepare(const VulkanDevice& device, std::uint32_t frame, const SpriteDrawList& sprites,
                         const core::Mat4& view_projection) {
    if (frame >= vertex_buffers_.size()) return false;
    batches_[frame].clear();
    if (sprites.empty()) return false;
    ++frame_counter_;
    if (reload_requested_) {
        // Las imagenes cambiaron en disco: todas fuera (rara vez; se espera a la GPU).
        device.waitIdle();
        textures_.clear();
        reload_requested_ = false;
    }
    if (frame_counter_ % 300 == 0 && textures_.size() > kMaxTextures / 2) evictUnused();

    // Una entrada por textura de la lista.
    std::vector<TextureEntry*> entries;
    entries.reserve(sprites.textures.size());
    for (const SpriteTexture& desc : sprites.textures) {
        TextureEntry* entry = texture(device, desc);
        entry->last_used = frame_counter_;
        entries.push_back(entry);
    }

    scratch_.clear();
    scratch_.reserve(sprites.quads.size() * 6);
    std::vector<Batch>& batches = batches_[frame];
    static constexpr int kOrder[6] = {0, 1, 2, 0, 2, 3};
    for (const SpriteQuad& q : sprites.quads) {
        if (q.color.w <= 0.0f) continue;
        TextureEntry* entry = q.texture < entries.size() ? entries[q.texture] : white_.get();
        const VkDescriptorSet set = static_cast<VkDescriptorSet>(*entry->set);
        if (batches.empty() || batches.back().set != set) {
            batches.push_back(Batch{static_cast<std::uint32_t>(scratch_.size()), 0, set});
        }
        for (const int i : kOrder) {
            Vertex v{};
            v.position[0] = q.corners[i].x;
            v.position[1] = q.corners[i].y;
            v.position[2] = q.corners[i].z;
            v.uv[0] = q.uvs[i].x;
            v.uv[1] = q.uvs[i].y;
            v.color[0] = q.color.x;
            v.color[1] = q.color.y;
            v.color[2] = q.color.z;
            v.color[3] = q.color.w;
            v.params[0] = q.lit ? 1.0f : 0.0f;
            v.params[1] = q.alpha_cutoff;
            scratch_.push_back(v);
        }
        batches.back().count += 6;
    }
    if (scratch_.empty()) {
        batches.clear();
        return false;
    }

    const vk::DeviceSize bytes = sizeof(Vertex) * scratch_.size();
    VulkanBuffer& buffer = vertex_buffers_[frame];
    if (!buffer.isValid() || buffer.size() < bytes) {
        buffer.destroy();
        buffer.create(device, std::max<vk::DeviceSize>(bytes * 2, 256 * 1024), vk::BufferUsageFlagBits::eVertexBuffer,
                      vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
    }
    buffer.write(scratch_.data(), bytes);

    // Luces 2D.
    std::array<GpuLight, kMaxLights> lights{};
    const std::uint32_t light_count = static_cast<std::uint32_t>(std::min<std::size_t>(sprites.lights.size(), kMaxLights));
    for (std::uint32_t i = 0; i < light_count; ++i) {
        const SpriteLight& l = sprites.lights[i];
        lights[i].position_radius[0] = l.position.x;
        lights[i].position_radius[1] = l.position.y;
        lights[i].position_radius[2] = l.position.z;
        lights[i].position_radius[3] = std::max(l.radius, 0.001f);
        lights[i].color_intensity[0] = l.color.x;
        lights[i].color_intensity[1] = l.color.y;
        lights[i].color_intensity[2] = l.color.z;
        lights[i].color_intensity[3] = l.intensity;
        lights[i].params[0] = std::max(l.falloff, 0.01f);
    }
    if (light_count > 0) light_buffers_[frame].write(lights.data(), sizeof(GpuLight) * light_count);

    Push& push = pushes_[frame];
    push.view_projection = view_projection;
    push.ambient[0] = sprites.ambient.x;
    push.ambient[1] = sprites.ambient.y;
    push.ambient[2] = sprites.ambient.z;
    push.ambient[3] = 1.0f;
    push.light_count = light_count;
    return true;
}

void SpritePass::record(const vk::raii::CommandBuffer& cmd, std::uint32_t frame, vk::Extent2D viewport) const {
    if (frame >= batches_.size() || batches_[frame].empty()) return;
    cmd.setViewport(0, vk::Viewport{0.0f, 0.0f, static_cast<float>(viewport.width),
                                    static_cast<float>(viewport.height), 0.0f, 1.0f});
    cmd.setScissor(0, vk::Rect2D{vk::Offset2D{0, 0}, viewport});
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *pipeline_);
    cmd.bindVertexBuffers(0, *vertex_buffers_[frame].handle(), {0});
    cmd.pushConstants<Push>(*layout_, vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment, 0,
                            pushes_[frame]);
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *layout_, 1, *light_sets_[frame], nullptr);
    for (const Batch& batch : batches_[frame]) {
        const vk::DescriptorSet set{batch.set};
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *layout_, 0, set, nullptr);
        cmd.draw(batch.count, 1, batch.first, 0);
    }
}

}  // namespace cramion::gfx
