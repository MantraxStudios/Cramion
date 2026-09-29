#include "CramionFX/vk/FoliagePass.h"

#include "CramionFX/vk/GBuffer.h"
#include "CramionFX/vk/VulkanDevice.h"
#include "CramionFX/vk/VulkanShader.h"

#include <algorithm>
#include <cmath>

namespace cramion::gfx {

namespace {

constexpr float kPi = 3.14159265f;

// Debe coincidir con foliage.vert (vertice de 32 bytes).
struct FoliageVertex {
    float px, py, pz;
    float nx, ny, nz;
    std::uint32_t color;  // RGBA8: rgb color (sRGB), a oclusion
    float wind;
};
static_assert(sizeof(FoliageVertex) == 32);

// VkDrawIndexedIndirectCommand (20 bytes), igual que en foliage_cull.comp.
struct DrawCommand {
    std::uint32_t index_count = 0;
    std::uint32_t instance_count = 0;
    std::uint32_t first_index = 0;
    std::int32_t vertex_offset = 0;
    std::uint32_t first_instance = 0;
};
static_assert(sizeof(DrawCommand) == 20);

struct CullPush {
    core::Mat4 view_projection;
    core::Vec4 camera;   // xyz, w distancia maxima
    core::Vec4 lod;      // x nivel 1, y nivel 2, z sombras, w proyectan (0/1)
    std::uint32_t counts[4] = {0, 0, 0, 0};
    core::Vec4 offset;
};
static_assert(sizeof(CullPush) == 128, "el minimo que garantiza Vulkan");

struct DrawPush {
    core::Mat4 light_view_projection;
    core::Vec4 params;  // x segundos, y sombra, z viento
    core::Vec4 offset;
};

std::uint32_t rgba(float r, float g, float b, float a) {
    const auto c = [](float v) { return static_cast<std::uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return c(r) | (c(g) << 8) | (c(b) << 16) | (c(a) << 24);
}

// Constructor de mallas de arboles (color por vertice).
struct TreeMesh {
    std::vector<FoliageVertex> vertices;
    std::vector<std::uint32_t> indices;
    float height = 10.0f;  // para el peso del viento

    std::uint32_t add(core::Vec3 p, core::Vec3 n, core::Vec3 color, float ao, float wind) {
        n = core::normalize(n);
        vertices.push_back({p.x, p.y, p.z, n.x, n.y, n.z, rgba(color.x, color.y, color.z, ao), wind});
        return static_cast<std::uint32_t>(vertices.size() - 1);
    }
    float windAt(float y, float strength) const { return strength * std::pow(std::clamp(y / height, 0.0f, 1.0f), 1.5f); }

    // Tronco: cilindro (o tronco de cono) sin tapas.
    void trunk(float r0, float r1, float y0, float y1, int sides, core::Vec3 color) {
        const std::uint32_t first = static_cast<std::uint32_t>(vertices.size());
        const float slope = (r0 - r1) / std::max(y1 - y0, 1e-3f);
        for (int k = 0; k <= sides; ++k) {
            const float a = 2.0f * kPi * static_cast<float>(k) / static_cast<float>(sides);
            const float c = std::cos(a), s = std::sin(a);
            add(core::Vec3{c * r0, y0, s * r0}, core::Vec3{c, slope, s}, color, 0.55f, windAt(y0, 0.15f));
            add(core::Vec3{c * r1, y1, s * r1}, core::Vec3{c, slope, s}, color * 1.08f, 0.9f, windAt(y1, 0.15f));
        }
        for (int k = 0; k < sides; ++k) {
            const std::uint32_t b0 = first + static_cast<std::uint32_t>(k) * 2, t0 = b0 + 1, b1 = b0 + 2, t1 = b0 + 3;
            indices.insert(indices.end(), {b0, t0, b1, b1, t0, t1});
        }
    }

    // Cono de hojas (pino) con su base.
    void cone(float radius, float y0, float y1, int sides, core::Vec3 color) {
        const float h = y1 - y0;
        const std::uint32_t apex_first = static_cast<std::uint32_t>(vertices.size());
        for (int k = 0; k <= sides; ++k) {
            const float a = 2.0f * kPi * (static_cast<float>(k) + 0.5f) / static_cast<float>(sides);
            add(core::Vec3{0.0f, y1, 0.0f}, core::Vec3{std::cos(a) * h, radius, std::sin(a) * h}, color * 1.15f, 1.0f,
                windAt(y1, 1.0f));
        }
        const std::uint32_t ring_first = static_cast<std::uint32_t>(vertices.size());
        for (int k = 0; k <= sides; ++k) {
            const float a = 2.0f * kPi * static_cast<float>(k) / static_cast<float>(sides);
            const float c = std::cos(a), s = std::sin(a);
            add(core::Vec3{c * radius, y0, s * radius}, core::Vec3{c * h, radius, s * h}, color * 0.85f, 0.62f,
                windAt(y0, 1.0f));
        }
        for (int k = 0; k < sides; ++k) {
            indices.insert(indices.end(), {ring_first + static_cast<std::uint32_t>(k), apex_first + static_cast<std::uint32_t>(k),
                                           ring_first + static_cast<std::uint32_t>(k) + 1});
        }
        // Base (hacia abajo, oscura: dentro de la copa).
        const std::uint32_t center = add(core::Vec3{0.0f, y0 + h * 0.08f, 0.0f}, core::Vec3{0, -1, 0}, color * 0.6f, 0.35f,
                                         windAt(y0, 1.0f));
        const std::uint32_t cap_first = static_cast<std::uint32_t>(vertices.size());
        for (int k = 0; k <= sides; ++k) {
            const float a = 2.0f * kPi * static_cast<float>(k) / static_cast<float>(sides);
            add(core::Vec3{std::cos(a) * radius, y0, std::sin(a) * radius}, core::Vec3{0, -1, 0}, color * 0.6f, 0.4f,
                windAt(y0, 1.0f));
        }
        for (int k = 0; k < sides; ++k) {
            indices.insert(indices.end(), {center, cap_first + static_cast<std::uint32_t>(k) + 1, cap_first + static_cast<std::uint32_t>(k)});
        }
    }

    // Copa redondeada: elipsoide de anillos (rings >= 2).
    void blob(core::Vec3 center, core::Vec3 radii, int rings, int sides, core::Vec3 color) {
        const std::uint32_t first = static_cast<std::uint32_t>(vertices.size());
        for (int r = 0; r <= rings; ++r) {
            const float lat = -kPi * 0.5f + kPi * static_cast<float>(r) / static_cast<float>(rings);
            const float cy = std::sin(lat), cr = std::cos(lat);
            const float up = static_cast<float>(r) / static_cast<float>(rings);  // 0 abajo, 1 arriba
            for (int k = 0; k <= sides; ++k) {
                const float a = 2.0f * kPi * static_cast<float>(k) / static_cast<float>(sides) + (r % 2) * kPi / sides;
                const core::Vec3 unit{std::cos(a) * cr, cy, std::sin(a) * cr};
                const core::Vec3 p = center + core::Vec3{unit.x * radii.x, unit.y * radii.y, unit.z * radii.z};
                const core::Vec3 n{unit.x / radii.x, unit.y / radii.y, unit.z / radii.z};
                add(p, n, color * (0.72f + 0.4f * up), 0.45f + 0.55f * up, windAt(p.y, 1.0f));
            }
        }
        const std::uint32_t row = static_cast<std::uint32_t>(sides) + 1;
        for (int r = 0; r < rings; ++r) {
            for (int k = 0; k < sides; ++k) {
                const std::uint32_t a = first + static_cast<std::uint32_t>(r) * row + static_cast<std::uint32_t>(k);
                const std::uint32_t b = a + row;
                indices.insert(indices.end(), {a, b, a + 1, a + 1, b, b + 1});
            }
        }
    }
};

// Especie x nivel (0 cerca, 1 media, 2 lejos).
TreeMesh makeTree(std::uint32_t species, std::uint32_t level) {
    TreeMesh m;
    if (species == 0) {  // Pino: conos apilados
        const core::Vec3 bark{0.36f, 0.25f, 0.16f}, needles{0.14f, 0.31f, 0.15f};
        m.height = 10.2f;
        if (level == 0) {
            m.trunk(0.28f, 0.12f, 0.0f, 4.0f, 8, bark);
            m.cone(2.4f, 1.8f, 5.8f, 10, needles);
            m.cone(1.9f, 3.8f, 7.6f, 10, needles);
            m.cone(1.35f, 5.6f, 9.2f, 9, needles);
            m.cone(0.8f, 7.4f, 10.2f, 8, needles);
        } else if (level == 1) {
            m.trunk(0.28f, 0.14f, 0.0f, 3.0f, 5, bark);
            m.cone(2.4f, 1.8f, 6.5f, 6, needles);
            m.cone(1.6f, 4.8f, 10.0f, 6, needles);
        } else {
            m.cone(2.3f, 1.0f, 10.0f, 4, needles);
        }
    } else if (species == 1) {  // Roble: copa ancha de varias bolas
        const core::Vec3 bark{0.40f, 0.29f, 0.19f}, leaves{0.21f, 0.39f, 0.15f};
        m.height = 8.6f;
        if (level == 0) {
            m.trunk(0.45f, 0.30f, 0.0f, 4.0f, 8, bark);
            m.blob({0.0f, 5.3f, 0.0f}, {3.2f, 2.6f, 3.2f}, 6, 10, leaves);
            m.blob({1.3f, 6.4f, 0.8f}, {2.0f, 1.8f, 2.0f}, 5, 9, leaves);
            m.blob({-1.2f, 6.2f, -1.0f}, {2.1f, 1.7f, 2.1f}, 5, 9, leaves);
            m.blob({0.2f, 7.2f, -0.4f}, {1.6f, 1.4f, 1.6f}, 4, 8, leaves);
        } else if (level == 1) {
            m.trunk(0.45f, 0.30f, 0.0f, 3.5f, 5, bark);
            m.blob({0.0f, 5.6f, 0.0f}, {3.4f, 2.9f, 3.4f}, 4, 7, leaves);
        } else {
            m.blob({0.0f, 5.4f, 0.0f}, {3.3f, 3.0f, 3.3f}, 2, 5, leaves);
        }
    } else {  // Abedul: tronco claro y copa alta
        const core::Vec3 bark{0.86f, 0.84f, 0.78f}, leaves{0.38f, 0.54f, 0.19f};
        m.height = 10.2f;
        if (level == 0) {
            m.trunk(0.20f, 0.11f, 0.0f, 7.5f, 7, bark);
            m.blob({0.0f, 6.6f, 0.0f}, {1.9f, 3.0f, 1.9f}, 6, 9, leaves);
            m.blob({0.5f, 8.6f, 0.3f}, {1.2f, 1.6f, 1.2f}, 4, 8, leaves);
        } else if (level == 1) {
            m.trunk(0.20f, 0.12f, 0.0f, 5.0f, 4, bark);
            m.blob({0.0f, 7.0f, 0.0f}, {2.0f, 3.2f, 2.0f}, 4, 6, leaves);
        } else {
            m.blob({0.0f, 6.8f, 0.0f}, {2.0f, 3.4f, 2.0f}, 2, 4, leaves);
        }
    }
    return m;
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

}  // namespace

FoliageInstance FoliageInstance::make(const core::Vec3& position, float yaw_radians, float scale, std::uint32_t species,
                                      float tint) {
    FoliageInstance f;
    f.x = position.x;
    f.y = position.y;
    f.z = position.z;
    float turn = std::fmod(yaw_radians / (2.0f * kPi), 1.0f);
    if (turn < 0.0f) turn += 1.0f;
    const auto yaw = static_cast<std::uint32_t>(turn * 1024.0f) & 1023u;
    const auto sc = static_cast<std::uint32_t>(std::clamp((scale - 0.25f) / 3.75f * 255.0f + 0.5f, 0.0f, 255.0f));
    const auto tn = static_cast<std::uint32_t>(std::clamp(tint, 0.0f, 1.0f) * 255.0f + 0.5f);
    f.packed = yaw | (sc << 10) | ((species & 3u) << 18) | (tn << 20);
    return f;
}

// -----------------------------------------------------------------------------
// Creacion
// -----------------------------------------------------------------------------

void FoliagePass::create(const VulkanDevice& device, const vk::raii::DescriptorSetLayout& frame_layout,
                         std::array<vk::Format, 4> gbuffer_formats, vk::Format depth_format, vk::Format shadow_format,
                         std::uint32_t frames_in_flight) {
    destroy();
    device_ = &device;
    frames_ = frames_in_flight;

    // Computo: instancias, visibles, comandos.
    {
        const std::array<vk::DescriptorSetLayoutBinding, 3> bindings = {{
            {0, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute},
            {1, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute},
            {2, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute},
        }};
        vk::DescriptorSetLayoutCreateInfo info{};
        info.setBindings(bindings);
        cull_set_layout_ = vk::raii::DescriptorSetLayout(device.handle(), info);
        vk::PushConstantRange push{vk::ShaderStageFlagBits::eCompute, 0, sizeof(CullPush)};
        vk::PipelineLayoutCreateInfo layout_info{};
        layout_info.setSetLayouts(*cull_set_layout_);
        layout_info.setPushConstantRanges(push);
        cull_layout_ = vk::raii::PipelineLayout(device.handle(), layout_info);
        const vk::raii::ShaderModule module = shaders::loadModule(device, "foliage_cull.comp.spv");
        vk::ComputePipelineCreateInfo pipeline{};
        pipeline.stage = vk::PipelineShaderStageCreateInfo{{}, vk::ShaderStageFlagBits::eCompute, *module, "main"};
        pipeline.layout = *cull_layout_;
        cull_pipeline_ = vk::raii::Pipeline(device.handle(), device.pipelineCache(), pipeline);
    }
    // Dibujo: set 0 = el de la geometria (camara, lluvia, decals), set 1 = instancias y visibles.
    {
        const std::array<vk::DescriptorSetLayoutBinding, 2> bindings = {{
            {0, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eVertex},
            {1, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eVertex},
        }};
        vk::DescriptorSetLayoutCreateInfo info{};
        info.setBindings(bindings);
        draw_set_layout_ = vk::raii::DescriptorSetLayout(device.handle(), info);
        const std::array<vk::DescriptorSetLayout, 2> layouts = {*frame_layout, *draw_set_layout_};
        vk::PushConstantRange push{vk::ShaderStageFlagBits::eVertex, 0, sizeof(DrawPush)};
        vk::PipelineLayoutCreateInfo layout_info{};
        layout_info.setSetLayouts(layouts);
        layout_info.setPushConstantRanges(push);
        draw_layout_ = vk::raii::PipelineLayout(device.handle(), layout_info);
    }
    const vk::DescriptorPoolSize size{vk::DescriptorType::eStorageBuffer, 5 * frames_in_flight};
    vk::DescriptorPoolCreateInfo pool_info{};
    pool_info.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    pool_info.maxSets = 2 * frames_in_flight;
    pool_info.setPoolSizes(size);
    pool_ = vk::raii::DescriptorPool(device.handle(), pool_info);

    createPipelines(device, gbuffer_formats, depth_format, shadow_format);
    buildMeshes();
    readback_ready_.assign(frames_in_flight, false);
}

void FoliagePass::destroy() {
    cull_sets_.clear();
    draw_sets_.clear();
    instances_.destroy();
    for (VulkanBuffer& b : visible_) b.destroy();
    for (VulkanBuffer& b : commands_) b.destroy();
    for (VulkanBuffer& b : readback_) b.destroy();
    visible_.clear();
    commands_.clear();
    readback_.clear();
    vertices_.destroy();
    indices_.destroy();
    gbuffer_pipeline_ = nullptr;
    gbuffer_wire_pipeline_ = nullptr;
    shadow_pipeline_ = nullptr;
    cull_pipeline_ = nullptr;
    pool_ = nullptr;
    draw_layout_ = nullptr;
    cull_layout_ = nullptr;
    draw_set_layout_ = nullptr;
    cull_set_layout_ = nullptr;
    instance_count_ = 0;
    stats_ = FoliageStats{};
    device_ = nullptr;
}

void FoliagePass::createPipelines(const VulkanDevice& device, std::array<vk::Format, 4> gbuffer_formats,
                                  vk::Format depth_format, vk::Format shadow_format) {
    const vk::raii::ShaderModule vertex = shaders::loadModule(device, "foliage.vert.spv");
    const vk::raii::ShaderModule fragment = shaders::loadModule(device, "foliage.frag.spv");

    const vk::VertexInputBindingDescription binding{0, sizeof(FoliageVertex), vk::VertexInputRate::eVertex};
    const std::array<vk::VertexInputAttributeDescription, 4> attributes = {{
        {0, 0, vk::Format::eR32G32B32Sfloat, 0},
        {1, 0, vk::Format::eR32G32B32Sfloat, 12},
        {2, 0, vk::Format::eR8G8B8A8Unorm, 24},
        {3, 0, vk::Format::eR32Sfloat, 28},
    }};
    vk::PipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.setVertexBindingDescriptions(binding);
    vertex_input.setVertexAttributeDescriptions(attributes);
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
    vk::PipelineDepthStencilStateCreateInfo depth{};
    depth.depthTestEnable = VK_TRUE;
    depth.depthWriteEnable = VK_TRUE;
    depth.depthCompareOp = vk::CompareOp::eLessOrEqual;

    // --- G-buffer (las dos caras: la normal se da la vuelta en el fragmento) ---
    {
        const std::array<vk::PipelineShaderStageCreateInfo, 2> stages = {{
            {{}, vk::ShaderStageFlagBits::eVertex, *vertex, "main"},
            {{}, vk::ShaderStageFlagBits::eFragment, *fragment, "main"},
        }};
        vk::PipelineRasterizationStateCreateInfo raster{};
        raster.polygonMode = vk::PolygonMode::eFill;
        raster.cullMode = vk::CullModeFlagBits::eNone;
        raster.lineWidth = 1.0f;
        std::array<vk::PipelineColorBlendAttachmentState, GBuffer::kColorAttachmentCount> blends{};
        for (auto& b : blends) {
            b.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                               vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
        }
        vk::PipelineColorBlendStateCreateInfo blend{};
        blend.setAttachments(blends);
        vk::PipelineRenderingCreateInfo rendering{};
        rendering.setColorAttachmentFormats(gbuffer_formats);
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
        info.layout = *draw_layout_;
        gbuffer_pipeline_ = vk::raii::Pipeline(device.handle(), device.pipelineCache(), info);
        if (device.fillModeNonSolidSupported()) {
            raster.polygonMode = vk::PolygonMode::eLine;
            gbuffer_wire_pipeline_ = vk::raii::Pipeline(device.handle(), device.pipelineCache(), info);
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
        raster.depthBiasConstantFactor = 1.0f;
        raster.depthBiasSlopeFactor = 1.75f;
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
        info.layout = *draw_layout_;
        shadow_pipeline_ = vk::raii::Pipeline(device.handle(), device.pipelineCache(), info);
    }
}

void FoliagePass::buildMeshes() {
    std::vector<FoliageVertex> vertices;
    std::vector<std::uint32_t> indices;
    for (std::uint32_t s = 0; s < kSpecies; ++s) {
        for (std::uint32_t l = 0; l < 3; ++l) {
            const TreeMesh m = makeTree(s, l);
            Mesh& mesh = meshes_[s * 3 + l];
            mesh.first_index = static_cast<std::uint32_t>(indices.size());
            mesh.index_count = static_cast<std::uint32_t>(m.indices.size());
            mesh.vertex_offset = static_cast<std::int32_t>(vertices.size());
            triangles_[s * 3 + l] = mesh.index_count / 3;
            vertices.insert(vertices.end(), m.vertices.begin(), m.vertices.end());
            indices.insert(indices.end(), m.indices.begin(), m.indices.end());
        }
    }
    vertices_ = VulkanBuffer::createDeviceLocal(*device_, vertices.data(), vertices.size() * sizeof(FoliageVertex),
                                                vk::BufferUsageFlagBits::eVertexBuffer);
    indices_ = VulkanBuffer::createDeviceLocal(*device_, indices.data(), indices.size() * sizeof(std::uint32_t),
                                               vk::BufferUsageFlagBits::eIndexBuffer);
}

// -----------------------------------------------------------------------------
// Instancias
// -----------------------------------------------------------------------------

void FoliagePass::setInstances(const std::vector<FoliageInstance>& input) {
    if (device_ == nullptr) return;
    device_->waitIdle();
    cull_sets_.clear();
    draw_sets_.clear();
    instances_.destroy();
    for (VulkanBuffer& b : visible_) b.destroy();
    for (VulkanBuffer& b : commands_) b.destroy();
    for (VulkanBuffer& b : readback_) b.destroy();
    visible_.clear();
    commands_.clear();
    readback_.clear();
    readback_ready_.assign(frames_, false);
    species_count_.fill(0);
    stats_ = FoliageStats{};

    const std::size_t count = std::min<std::size_t>(input.size(), kMaxInstances);
    instance_count_ = static_cast<std::uint32_t>(count);
    if (count == 0) return;
    for (std::size_t i = 0; i < count; ++i) ++species_count_[std::min<std::uint32_t>((input[i].packed >> 18) & 3u, kSpecies - 1)];

    instances_ = VulkanBuffer::createDeviceLocal(*device_, input.data(), count * sizeof(FoliageInstance),
                                                 vk::BufferUsageFlagBits::eStorageBuffer);
    // Cada lista cabe todas las de su especie: 4 listas por especie.
    const vk::DeviceSize visible_bytes = static_cast<vk::DeviceSize>(count) * kLists * sizeof(std::uint32_t);
    std::uint32_t offset = 0;
    for (std::uint32_t s = 0; s < kSpecies; ++s) {
        for (std::uint32_t l = 0; l < kLists; ++l) {
            template_first_instance_[s * kLists + l] = offset * kLists + l * species_count_[s];
        }
        offset += species_count_[s];
    }
    for (std::uint32_t f = 0; f < frames_; ++f) {
        VulkanBuffer visible;
        visible.create(*device_, visible_bytes, vk::BufferUsageFlagBits::eStorageBuffer, vk::MemoryPropertyFlagBits::eDeviceLocal);
        visible_.push_back(std::move(visible));
        VulkanBuffer commands;
        commands.create(*device_, sizeof(DrawCommand) * kSpecies * kLists,
                        vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eIndirectBuffer |
                            vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eTransferSrc,
                        vk::MemoryPropertyFlagBits::eDeviceLocal);
        commands_.push_back(std::move(commands));
        VulkanBuffer readback;
        readback.create(*device_, sizeof(DrawCommand) * kSpecies * kLists, vk::BufferUsageFlagBits::eTransferDst,
                        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
        readback_.push_back(std::move(readback));
    }
    stats_.instances = count;
    stats_.memory_bytes = count * sizeof(FoliageInstance) + visible_bytes * frames_;
    writeSets();
}

void FoliagePass::writeSets() {
    const vk::raii::Device& device = device_->handle();
    std::vector<vk::DescriptorSetLayout> cull_layouts(frames_, *cull_set_layout_);
    std::vector<vk::DescriptorSetLayout> draw_layouts(frames_, *draw_set_layout_);
    vk::DescriptorSetAllocateInfo cull_info{};
    cull_info.descriptorPool = *pool_;
    cull_info.setSetLayouts(cull_layouts);
    cull_sets_ = vk::raii::DescriptorSets(device, cull_info);
    vk::DescriptorSetAllocateInfo draw_info{};
    draw_info.descriptorPool = *pool_;
    draw_info.setSetLayouts(draw_layouts);
    draw_sets_ = vk::raii::DescriptorSets(device, draw_info);
    for (std::uint32_t f = 0; f < frames_; ++f) {
        const vk::DescriptorBufferInfo instances{*instances_.handle(), 0, VK_WHOLE_SIZE};
        const vk::DescriptorBufferInfo visible{*visible_[f].handle(), 0, VK_WHOLE_SIZE};
        const vk::DescriptorBufferInfo commands{*commands_[f].handle(), 0, VK_WHOLE_SIZE};
        std::array<vk::WriteDescriptorSet, 5> writes{};
        const auto write = [&](vk::WriteDescriptorSet& w, const vk::raii::DescriptorSet& set, std::uint32_t binding,
                               const vk::DescriptorBufferInfo& info) {
            w.dstSet = *set;
            w.dstBinding = binding;
            w.descriptorType = vk::DescriptorType::eStorageBuffer;
            w.setBufferInfo(info);
        };
        write(writes[0], cull_sets_[f], 0, instances);
        write(writes[1], cull_sets_[f], 1, visible);
        write(writes[2], cull_sets_[f], 2, commands);
        write(writes[3], draw_sets_[f], 0, instances);
        write(writes[4], draw_sets_[f], 1, visible);
        device.updateDescriptorSets(writes, nullptr);
    }
}

// -----------------------------------------------------------------------------
// Por frame
// -----------------------------------------------------------------------------

void FoliagePass::recordCull(const vk::raii::CommandBuffer& cmd, std::uint32_t frame, const core::Vec3& camera_position,
                             const core::Mat4& view_projection, float delta_seconds) {
    using Stage = vk::PipelineStageFlagBits2;
    using Access = vk::AccessFlagBits2;
    time_ += std::clamp(delta_seconds, 0.0f, 0.25f);
    if (instance_count_ == 0 || frame >= commands_.size()) return;

    // Lo que conto la GPU la ultima vez que uso este frame (ya termino).
    if (readback_ready_[frame]) {
        const auto* done = static_cast<const DrawCommand*>(readback_[frame].mapped());
        stats_.visible[0] = stats_.visible[1] = stats_.visible[2] = 0;
        stats_.shadow_casters = 0;
        stats_.triangles = 0;
        for (std::uint32_t s = 0; s < kSpecies; ++s) {
            for (std::uint32_t l = 0; l < 3; ++l) {
                const std::uint32_t n = done[s * kLists + l].instance_count;
                stats_.visible[l] += n;
                stats_.triangles += static_cast<std::uint64_t>(n) * triangles_[s * 3 + l];
            }
            stats_.shadow_casters += done[s * kLists + 3].instance_count;
        }
    }

    // Comandos con 0 instancias (el computo las cuenta).
    std::array<DrawCommand, kSpecies * kLists> commands{};
    for (std::uint32_t s = 0; s < kSpecies; ++s) {
        for (std::uint32_t l = 0; l < kLists; ++l) {
            const Mesh& mesh = meshes_[s * 3 + (l < 3 ? l : 1)];
            DrawCommand& c = commands[s * kLists + l];
            c.index_count = mesh.index_count;
            c.first_index = mesh.first_index;
            c.vertex_offset = mesh.vertex_offset;
            c.first_instance = template_first_instance_[s * kLists + l];
        }
    }
    memoryBarrier(cmd, Stage::eDrawIndirect | Stage::eVertexShader | Stage::eTransfer,
                  Access::eIndirectCommandRead | Access::eShaderRead | Access::eTransferRead, Stage::eTransfer,
                  Access::eTransferWrite);
    cmd.updateBuffer<DrawCommand>(*commands_[frame].handle(), 0, commands);
    memoryBarrier(cmd, Stage::eTransfer, Access::eTransferWrite, Stage::eComputeShader, Access::eShaderRead | Access::eShaderWrite);

    CullPush push{};
    push.view_projection = view_projection;
    push.camera = core::Vec4{camera_position.x, camera_position.y, camera_position.z, settings_.max_distance};
    push.lod = core::Vec4{settings_.lod1_distance, std::max(settings_.lod2_distance, settings_.lod1_distance),
                          settings_.shadow_distance, settings_.cast_shadows ? 1.0f : 0.0f};
    push.counts[0] = instance_count_;
    push.offset = core::Vec4{origin_offset_.x, origin_offset_.y, origin_offset_.z, 0.0f};
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *cull_pipeline_);
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *cull_layout_, 0, *cull_sets_[frame], nullptr);
    cmd.pushConstants<CullPush>(*cull_layout_, vk::ShaderStageFlagBits::eCompute, 0, push);
    cmd.dispatch((instance_count_ + 255) / 256, 1, 1);

    memoryBarrier(cmd, Stage::eComputeShader, Access::eShaderWrite, Stage::eDrawIndirect | Stage::eVertexShader | Stage::eTransfer,
                  Access::eIndirectCommandRead | Access::eShaderRead | Access::eTransferRead);
    // Copia para las estadisticas (se lee la proxima vez que toque este frame).
    const vk::BufferCopy region{0, 0, sizeof(DrawCommand) * kSpecies * kLists};
    cmd.copyBuffer(*commands_[frame].handle(), *readback_[frame].handle(), region);
    memoryBarrier(cmd, Stage::eTransfer, Access::eTransferWrite, Stage::eHost, Access::eHostRead);
    readback_ready_[frame] = true;
}

void FoliagePass::draw(const vk::raii::CommandBuffer& cmd, std::uint32_t frame, bool shadow) const {
    cmd.bindVertexBuffers(0, *vertices_.handle(), {0});
    cmd.bindIndexBuffer(*indices_.handle(), 0, vk::IndexType::eUint32);
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *draw_layout_, 1, *draw_sets_[frame], nullptr);
    for (std::uint32_t s = 0; s < kSpecies; ++s) {
        if (species_count_[s] == 0) continue;
        if (shadow) {
            cmd.drawIndexedIndirect(*commands_[frame].handle(), (s * kLists + 3) * sizeof(DrawCommand), 1, sizeof(DrawCommand));
        } else {
            // Tres niveles de detalle, uno por llamada (sin multiDrawIndirect en muchos moviles).
            for (std::uint32_t level = 0; level < 3; ++level) {
                cmd.drawIndexedIndirect(*commands_[frame].handle(), (s * kLists + level) * sizeof(DrawCommand), 1,
                                        sizeof(DrawCommand));
            }
        }
    }
}

void FoliagePass::recordGBuffer(const vk::raii::CommandBuffer& cmd, std::uint32_t frame,
                                const vk::raii::DescriptorSet& frame_set) const {
    if (instance_count_ == 0 || frame >= draw_sets_.size()) return;
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics,
                     wireframe_ && *gbuffer_wire_pipeline_ ? *gbuffer_wire_pipeline_ : *gbuffer_pipeline_);
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *draw_layout_, 0, *frame_set, nullptr);
    DrawPush push{};
    push.params = core::Vec4{time_, 0.0f, settings_.wind, 0.0f};
    push.offset = core::Vec4{origin_offset_.x, origin_offset_.y, origin_offset_.z, 0.0f};
    cmd.pushConstants<DrawPush>(*draw_layout_, vk::ShaderStageFlagBits::eVertex, 0, push);
    draw(cmd, frame, false);
}

void FoliagePass::recordShadow(const vk::raii::CommandBuffer& cmd, std::uint32_t frame,
                               const vk::raii::DescriptorSet& frame_set, const core::Mat4& light_view_projection) const {
    if (instance_count_ == 0 || !settings_.cast_shadows || frame >= draw_sets_.size()) return;
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *shadow_pipeline_);
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *draw_layout_, 0, *frame_set, nullptr);
    DrawPush push{};
    push.light_view_projection = light_view_projection;
    push.params = core::Vec4{time_, 1.0f, settings_.wind, 0.0f};
    push.offset = core::Vec4{origin_offset_.x, origin_offset_.y, origin_offset_.z, 0.0f};
    cmd.pushConstants<DrawPush>(*draw_layout_, vk::ShaderStageFlagBits::eVertex, 0, push);
    draw(cmd, frame, true);
}

}  // namespace cramion::gfx
