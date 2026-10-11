#include "CramionFX/vk/VulkanCompat.h"
#include "CramionFX/vk/WaterPass.h"

#include "CramionFX/vk/VulkanDevice.h"
#include "CramionFX/vk/VulkanShader.h"

#include <algorithm>
#include <cmath>

namespace cramion::gfx {

namespace {

struct GpuWaterUniform {
    core::Vec4 time_count{};     // x = tiempo, y = cuerpos, z = cuerpo con la camara dentro (-1), w = tiempo del oceano
    core::Vec4 ripple{};         // olas interactivas: x, z de la esquina, celda (m), lado (0 = no hay)
    core::Vec4 cascade_size{};   // lado de cada cascada del oceano (m)
    core::Vec4 cascade_slope{};  // varianza de la pendiente de cada cascada
    core::Vec4 ocean{};          // x choppiness, y altura significativa, z niveles del clipmap, w -
    GpuWaterBody bodies[kMaxWaterBodies];
};

// La rejilla de olas mas grande que se sube (water::RippleSimulation::kSize).
constexpr std::uint32_t kMaxRippleSize = 256;

struct GpuWaterPush {
    std::uint32_t body = 0;
    std::uint32_t mesh = 0;  // 0 lago, 1 oceano, 2 rio
    std::uint32_t pad[2] = {0, 0};
};

struct GpuFftPush {
    std::uint32_t mode = 0;
    float time = 0.0f;
    float dt = 0.0f;
    float chop = 1.0f;
    float foam_threshold = 0.5f;
    float foam_decay = 0.25f;
    std::uint32_t reset = 1;
    float foam_amount = 1.0f;
};

// Resolucion de las mallas.
constexpr std::uint32_t kGridCells = 192;
// Clipmap del oceano: 128 x 128 celdas por nivel (el centro de 62 x 62 lo
// cubre el nivel anterior), 10 niveles: con 0.2 m en el primero, unos 6.5 km
// a cada lado en el ultimo, y su borde se estira hasta el horizonte.
constexpr std::int32_t kClipHalf = 64;
constexpr std::int32_t kClipHole = 31;
constexpr std::uint32_t kOceanLevels = 10;
constexpr vk::Format kFftFormat = vk::Format::eR32G32B32A32Sfloat;
constexpr vk::Format kOceanFormat = vk::Format::eR16G16B16A16Sfloat;

vk::ImageMemoryBarrier2 imageBarrier(vk::Image image, vk::ImageLayout from, vk::ImageLayout to,
                                     vk::PipelineStageFlags2 src_stage, vk::AccessFlags2 src_access,
                                     vk::PipelineStageFlags2 dst_stage, vk::AccessFlags2 dst_access,
                                     std::uint32_t base_mip, std::uint32_t mips, std::uint32_t layers) {
    vk::ImageMemoryBarrier2 b{};
    b.srcStageMask = src_stage;
    b.srcAccessMask = src_access;
    b.dstStageMask = dst_stage;
    b.dstAccessMask = dst_access;
    b.oldLayout = from;
    b.newLayout = to;
    b.image = image;
    b.subresourceRange = vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, base_mip, mips, 0, layers};
    return b;
}

void barriers(const vk::raii::CommandBuffer& cmd, const std::vector<vk::ImageMemoryBarrier2>& images,
              bool compute_memory = false) {
    vk::DependencyInfo dependency{};
    dependency.setImageMemoryBarriers(images);
    vk::MemoryBarrier2 memory{};
    if (compute_memory) {
        memory.srcStageMask = vk::PipelineStageFlagBits2::eComputeShader;
        memory.srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite;
        memory.dstStageMask = vk::PipelineStageFlagBits2::eComputeShader;
        memory.dstAccessMask = vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite;
        dependency.setMemoryBarriers(memory);
    }
    compat::pipelineBarrier(cmd, dependency);
}

}  // namespace

void WaterPass::create(const VulkanDevice& device, const vk::raii::DescriptorSetLayout& frame_layout,
                       const vk::raii::DescriptorSetLayout& scene_layout, vk::Format color_format,
                       vk::Format depth_format, std::uint32_t frames_in_flight) {
    destroy();
    device_ = &device;
    frames_ = frames_in_flight;

    const auto stages = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment;
    const std::array<vk::DescriptorSetLayoutBinding, 4> bindings = {{
        {0, vk::DescriptorType::eUniformBuffer, 1, stages},
        {1, vk::DescriptorType::eStorageBuffer, 1, stages},         // alturas de las olas interactivas
        {2, vk::DescriptorType::eCombinedImageSampler, 1, stages},  // oceano: desplazamiento + espuma
        {3, vk::DescriptorType::eCombinedImageSampler, 1, stages},  // oceano: derivadas (normal)
    }};
    vk::DescriptorSetLayoutCreateInfo set_info{};
    set_info.setBindings(bindings);
    set_layout_ = vk::raii::DescriptorSetLayout(device.handle(), set_info);

    const std::array<vk::DescriptorSetLayout, 3> set_layouts = {*frame_layout, *set_layout_, *scene_layout};
    vk::PushConstantRange push{};
    push.stageFlags = stages;
    push.size = sizeof(GpuWaterPush);
    vk::PipelineLayoutCreateInfo layout_info{};
    layout_info.setSetLayouts(set_layouts);
    layout_info.setPushConstantRanges(push);
    layout_ = vk::raii::PipelineLayout(device.handle(), layout_info);

    const std::array<vk::DescriptorPoolSize, 3> sizes = {{
        {vk::DescriptorType::eUniformBuffer, frames_in_flight},
        {vk::DescriptorType::eStorageBuffer, frames_in_flight},
        {vk::DescriptorType::eCombinedImageSampler, frames_in_flight * 2},
    }};
    vk::DescriptorPoolCreateInfo pool_info{};
    pool_info.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    pool_info.maxSets = frames_in_flight;
    pool_info.setPoolSizes(sizes);
    pool_ = vk::raii::DescriptorPool(device.handle(), pool_info);
    const std::vector<vk::DescriptorSetLayout> layouts(frames_in_flight, *set_layout_);
    vk::DescriptorSetAllocateInfo alloc{};
    alloc.descriptorPool = *pool_;
    alloc.setSetLayouts(layouts);
    sets_ = vk::raii::DescriptorSets(device.handle(), alloc);

    createOcean(device);

    uniforms_.resize(frames_in_flight);
    ripple_buffers_.resize(frames_in_flight);
    for (std::uint32_t i = 0; i < frames_in_flight; ++i) {
        ripple_buffers_[i].create(device, sizeof(float) * kMaxRippleSize * kMaxRippleSize,
                                  vk::BufferUsageFlagBits::eStorageBuffer,
                                  vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
        vk::DescriptorBufferInfo ripple_info{};
        ripple_info.buffer = *ripple_buffers_[i].handle();
        ripple_info.range = VK_WHOLE_SIZE;
        vk::WriteDescriptorSet ripple_write{};
        ripple_write.dstSet = *sets_[i];
        ripple_write.dstBinding = 1;
        ripple_write.descriptorType = vk::DescriptorType::eStorageBuffer;
        ripple_write.setBufferInfo(ripple_info);
        device.handle().updateDescriptorSets(ripple_write, nullptr);
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
        const std::array<vk::DescriptorImageInfo, 2> ocean_infos = {{
            {*ocean_sampler_, *displacement_.sampled, vk::ImageLayout::eShaderReadOnlyOptimal},
            {*ocean_sampler_, *derivatives_.sampled, vk::ImageLayout::eShaderReadOnlyOptimal},
        }};
        for (std::uint32_t b = 0; b < 2; ++b) {
            vk::WriteDescriptorSet image_write{};
            image_write.dstSet = *sets_[i];
            image_write.dstBinding = 2 + b;
            image_write.descriptorType = vk::DescriptorType::eCombinedImageSampler;
            image_write.setImageInfo(ocean_infos[b]);
            device.handle().updateDescriptorSets(image_write, nullptr);
        }
    }

    // --- Pipeline: mezcla sobre la imagen HDR con su propia profundidad ---
    // Escribe profundidad (en una copia de la de la escena, ver
    // VulkanRenderer::recordWaterPass): sin ella las olas no se tapaban entre
    // si y una cresta de detras se veia delante de la de delante.
    const vk::raii::ShaderModule vertex = shaders::loadModule(device, "water.vert.spv");
    const vk::raii::ShaderModule fragment = shaders::loadModule(device, "water.frag.spv");
    const std::array<vk::PipelineShaderStageCreateInfo, 2> shader_stages = {{
        {{}, vk::ShaderStageFlagBits::eVertex, *vertex, "main"},
        {{}, vk::ShaderStageFlagBits::eFragment, *fragment, "main"},
    }};
    const vk::VertexInputBindingDescription vertex_binding{0, sizeof(WaterVertex), vk::VertexInputRate::eVertex};
    const std::array<vk::VertexInputAttributeDescription, 4> attributes = {{
        {0, 0, vk::Format::eR32G32B32Sfloat, offsetof(WaterVertex, position)},
        {1, 0, vk::Format::eR32G32Sfloat, offsetof(WaterVertex, uv)},
        {2, 0, vk::Format::eR32G32Sfloat, offsetof(WaterVertex, flow)},
        {3, 0, vk::Format::eR32Sfloat, offsetof(WaterVertex, slope)},
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
    depth.depthWriteEnable = VK_TRUE;
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
    pipeline_ = compat::makeGraphicsPipeline(device, info);

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
        underwater_pipeline_ = compat::makeGraphicsPipeline(device, under_info);
    }

    createMeshes(device);
}

void WaterPass::createOceanImage(const VulkanDevice& device, OceanImage& out, vk::Format format,
                                 std::uint32_t layers, std::uint32_t mips, vk::ImageUsageFlags usage) {
    vk::ImageCreateInfo image_info{};
    image_info.imageType = vk::ImageType::e2D;
    image_info.format = format;
    image_info.extent = vk::Extent3D{kOceanResolution, kOceanResolution, 1};
    image_info.mipLevels = mips;
    image_info.arrayLayers = layers;
    image_info.samples = vk::SampleCountFlagBits::e1;
    image_info.tiling = vk::ImageTiling::eOptimal;
    image_info.usage = usage;
    image_info.initialLayout = vk::ImageLayout::eUndefined;
    out.image = vk::raii::Image(device.handle(), image_info);
    const vk::MemoryRequirements requirements = out.image.getMemoryRequirements();
    vk::MemoryAllocateInfo allocate_info{};
    allocate_info.allocationSize = requirements.size;
    allocate_info.memoryTypeIndex =
        device.findMemoryType(requirements.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal);
    out.memory = vk::raii::DeviceMemory(device.handle(), allocate_info);
    out.image.bindMemory(*out.memory, 0);

    vk::ImageViewCreateInfo view_info{};
    view_info.image = *out.image;
    view_info.viewType = vk::ImageViewType::e2DArray;
    view_info.format = format;
    view_info.subresourceRange = vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, mips, 0, layers};
    if (usage & vk::ImageUsageFlagBits::eSampled) out.sampled = vk::raii::ImageView(device.handle(), view_info);
    view_info.subresourceRange.levelCount = 1;
    out.storage = vk::raii::ImageView(device.handle(), view_info);
}

void WaterPass::createOcean(const VulkanDevice& device) {
    ocean_mips_ = 0;
    for (std::uint32_t s = kOceanResolution; s > 0; s >>= 1) ++ocean_mips_;
    createOceanImage(device, fft_image_, kFftFormat, kOceanCascades * 2, 1, vk::ImageUsageFlagBits::eStorage);
    const auto usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
                       vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst;
    createOceanImage(device, displacement_, kOceanFormat, kOceanCascades, ocean_mips_, usage);
    createOceanImage(device, derivatives_, kOceanFormat, kOceanCascades, ocean_mips_, usage);

    // Repetir (cada cascada es periodica), trilineal y anisotropico: de cerca
    // nitido y a lo lejos la media (sin parpadeo).
    vk::SamplerCreateInfo sampler_info{};
    sampler_info.magFilter = vk::Filter::eLinear;
    sampler_info.minFilter = vk::Filter::eLinear;
    sampler_info.mipmapMode = vk::SamplerMipmapMode::eLinear;
    sampler_info.addressModeU = vk::SamplerAddressMode::eRepeat;
    sampler_info.addressModeV = vk::SamplerAddressMode::eRepeat;
    sampler_info.addressModeW = vk::SamplerAddressMode::eClampToEdge;
    sampler_info.maxLod = static_cast<float>(ocean_mips_);
    if (device.physicalDevice().getFeatures().samplerAnisotropy) {
        sampler_info.anisotropyEnable = VK_TRUE;
        sampler_info.maxAnisotropy =
            std::min(8.0f, device.physicalDevice().getProperties().limits.maxSamplerAnisotropy);
    }
    ocean_sampler_ = vk::raii::Sampler(device.handle(), sampler_info);

    // Computo: un solo shader con cuatro modos (espectro, filas, columnas, montaje).
    const std::array<vk::DescriptorType, 4> fft_bindings = {
        vk::DescriptorType::eStorageBuffer, vk::DescriptorType::eStorageImage, vk::DescriptorType::eStorageImage,
        vk::DescriptorType::eStorageImage};
    ComputePassDesc desc{};
    desc.shader = "water_fft.comp.spv";
    desc.bindings = fft_bindings;
    desc.push_constant_size = sizeof(GpuFftPush);
    fft_pass_.create(device, desc);

    const std::array<vk::DescriptorPoolSize, 2> pool_sizes = {{
        {vk::DescriptorType::eStorageBuffer, frames_},
        {vk::DescriptorType::eStorageImage, frames_ * 3},
    }};
    vk::DescriptorPoolCreateInfo pool_info{};
    pool_info.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    pool_info.maxSets = frames_;
    pool_info.setPoolSizes(pool_sizes);
    fft_pool_ = vk::raii::DescriptorPool(device.handle(), pool_info);
    const std::vector<vk::DescriptorSetLayout> layouts(frames_, *fft_pass_.descriptorSetLayout());
    vk::DescriptorSetAllocateInfo alloc{};
    alloc.descriptorPool = *fft_pool_;
    alloc.setSetLayouts(layouts);
    fft_sets_ = vk::raii::DescriptorSets(device.handle(), alloc);

    const vk::DeviceSize spectrum_size =
        sizeof(float) * 8ull * kOceanCascades * kOceanResolution * kOceanResolution;
    spectrum_buffers_.resize(frames_);
    spectrum_uploaded_.assign(frames_, 0);
    for (std::uint32_t i = 0; i < frames_; ++i) {
        spectrum_buffers_[i].create(device, spectrum_size, vk::BufferUsageFlagBits::eStorageBuffer,
                                    vk::MemoryPropertyFlagBits::eHostVisible |
                                        vk::MemoryPropertyFlagBits::eHostCoherent);
        vk::DescriptorBufferInfo buffer_info{};
        buffer_info.buffer = *spectrum_buffers_[i].handle();
        buffer_info.range = VK_WHOLE_SIZE;
        const std::array<vk::DescriptorImageInfo, 3> images = {{
            {nullptr, *fft_image_.storage, vk::ImageLayout::eGeneral},
            {nullptr, *displacement_.storage, vk::ImageLayout::eGeneral},
            {nullptr, *derivatives_.storage, vk::ImageLayout::eGeneral},
        }};
        std::array<vk::WriteDescriptorSet, 4> writes{};
        writes[0].dstSet = *fft_sets_[i];
        writes[0].dstBinding = 0;
        writes[0].descriptorType = vk::DescriptorType::eStorageBuffer;
        writes[0].setBufferInfo(buffer_info);
        for (std::uint32_t b = 0; b < 3; ++b) {
            writes[b + 1].dstSet = *fft_sets_[i];
            writes[b + 1].dstBinding = b + 1;
            writes[b + 1].descriptorType = vk::DescriptorType::eStorageImage;
            writes[b + 1].setImageInfo(images[b]);
        }
        device.handle().updateDescriptorSets(writes, nullptr);
    }

    // Las imagenes del FFT siempre en General; las del oceano empiezan
    // legibles (en calma) hasta el primer frame.
    const vk::Image fft = *fft_image_.image;
    const vk::Image disp = *displacement_.image;
    const vk::Image deriv = *derivatives_.image;
    const std::uint32_t mips = ocean_mips_;
    device.submitOneTime([&](const vk::raii::CommandBuffer& cmd) {
        using Stage = vk::PipelineStageFlagBits2;
        using Access = vk::AccessFlagBits2;
        barriers(cmd, {imageBarrier(fft, vk::ImageLayout::eUndefined, vk::ImageLayout::eGeneral, Stage::eNone,
                                    Access::eNone, Stage::eComputeShader, Access::eShaderStorageWrite, 0, 1,
                                    kOceanCascades * 2),
                       imageBarrier(disp, vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
                                    Stage::eNone, Access::eNone, Stage::eClear, Access::eTransferWrite, 0, mips,
                                    kOceanCascades),
                       imageBarrier(deriv, vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
                                    Stage::eNone, Access::eNone, Stage::eClear, Access::eTransferWrite, 0, mips,
                                    kOceanCascades)});
        const vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor, 0, mips, 0, kOceanCascades};
        const vk::ClearColorValue zero{std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f}};
        cmd.clearColorImage(disp, vk::ImageLayout::eTransferDstOptimal, zero, range);
        cmd.clearColorImage(deriv, vk::ImageLayout::eTransferDstOptimal, zero, range);
        barriers(cmd, {imageBarrier(disp, vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
                                    Stage::eClear, Access::eTransferWrite, Stage::eVertexShader | Stage::eFragmentShader,
                                    Access::eShaderSampledRead, 0, mips, kOceanCascades),
                       imageBarrier(deriv, vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
                                    Stage::eClear, Access::eTransferWrite, Stage::eVertexShader | Stage::eFragmentShader,
                                    Access::eShaderSampledRead, 0, mips, kOceanCascades)});
    });
    ocean_ready_ = false;
}

void WaterPass::destroy() {
    ripple_buffers_.clear();
    bodies_.clear();
    garbage_.clear();
    for (Mesh& river : rivers_) river = Mesh{};
    grid_ = Mesh{};
    ocean_full_ = Mesh{};
    ocean_ring_ = Mesh{};
    for (VulkanBuffer& buffer : uniforms_) buffer.destroy();
    uniforms_.clear();
    sets_.clear();
    pool_ = nullptr;
    pipeline_ = nullptr;
    underwater_pipeline_ = nullptr;
    layout_ = nullptr;
    set_layout_ = nullptr;
    fft_sets_.clear();
    fft_pool_ = nullptr;
    fft_pass_.destroy();
    spectrum_buffers_.clear();
    spectrum_uploaded_.clear();
    fft_image_ = OceanImage{};
    displacement_ = OceanImage{};
    derivatives_ = OceanImage{};
    ocean_sampler_ = nullptr;
    ocean_ready_ = false;
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
    // --- Oceano: rejilla de 128 x 128 celdas en coordenadas de celda
    // (-64..64); el nivel 0 entera, los demas con el hueco del centro.
    // Diagonales alternas (en rombo): sin direccion preferida en las olas.
    {
        constexpr std::int32_t n = kClipHalf * 2 + 1;
        std::vector<WaterVertex> vertices;
        vertices.reserve(static_cast<std::size_t>(n) * n);
        for (std::int32_t z = -kClipHalf; z <= kClipHalf; ++z) {
            for (std::int32_t x = -kClipHalf; x <= kClipHalf; ++x) {
                vertices.push_back(
                    WaterVertex{{static_cast<float>(x), 0.0f, static_cast<float>(z)}, {0.0f, 0.0f}, {0.0f, 0.0f}});
            }
        }
        std::vector<std::uint32_t> full;
        std::vector<std::uint32_t> ring;
        for (std::int32_t z = -kClipHalf; z < kClipHalf; ++z) {
            for (std::int32_t x = -kClipHalf; x < kClipHalf; ++x) {
                const std::uint32_t a = static_cast<std::uint32_t>((z + kClipHalf) * n + (x + kClipHalf));
                const std::uint32_t b = a + 1;
                const std::uint32_t c = a + static_cast<std::uint32_t>(n);
                const std::uint32_t d = c + 1;
                const bool flip = ((x + z) & 1) != 0;
                const std::array<std::uint32_t, 6> quad =
                    flip ? std::array<std::uint32_t, 6>{a, c, b, b, c, d} : std::array<std::uint32_t, 6>{a, c, d, a, d, b};
                full.insert(full.end(), quad.begin(), quad.end());
                const bool hole = x >= -kClipHole && x + 1 <= kClipHole && z >= -kClipHole && z + 1 <= kClipHole;
                if (!hole) ring.insert(ring.end(), quad.begin(), quad.end());
            }
        }
        upload(device, ocean_full_, vertices, full);
        upload(device, ocean_ring_, vertices, ring);
    }
}

void WaterPass::setBodies(const std::vector<WaterBodyDesc>& bodies, float time, int underwater) {
    time_ = time;
    underwater_ = underwater < static_cast<int>(std::min<std::size_t>(bodies.size(), kMaxWaterBodies)) ? underwater : -1;
    bodies_.assign(bodies.begin(), bodies.begin() + std::min<std::size_t>(bodies.size(), kMaxWaterBodies));
}

void WaterPass::setSpectrum(const WaterSpectrumDesc& spectrum) {
    if (spectrum.modes == nullptr) {
        // Sin oceano: nada que simular (el oceano, si aparece, empieza en calma).
        spectrum_modes_.clear();
        spectrum_ = WaterSpectrumDesc{};
        ocean_ready_ = false;
        return;
    }
    const bool changed = spectrum.key != spectrum_.key || spectrum_modes_.empty();
    spectrum_ = spectrum;
    if (changed && spectrum.modes != nullptr &&
        spectrum.modes->size() == 8ull * kOceanCascades * kOceanResolution * kOceanResolution) {
        spectrum_modes_ = *spectrum.modes;
    }
    spectrum_.modes = nullptr;
}

void WaterPass::setRipples(const std::vector<float>& heights, std::uint32_t size, float origin_x, float origin_z,
                           float cell) {
    if (heights.empty() || size == 0 || size > kMaxRippleSize || heights.size() < static_cast<std::size_t>(size) * size) {
        ripple_params_ = core::Vec4{};
        ripple_size_ = 0;
        return;
    }
    ripples_ = heights;
    ripple_size_ = size;
    ripple_params_ = core::Vec4{origin_x, origin_z, cell, static_cast<float>(size)};
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
    const float period = std::max(spectrum_.period, 1.0f);
    data.time_count = core::Vec4{time_, static_cast<float>(bodies_.size()), static_cast<float>(underwater_),
                                 std::fmod(std::max(time_, 0.0f), period)};
    data.ripple = ripple_params_;
    if (!spectrum_modes_.empty()) {
        data.cascade_size = spectrum_.sizes;
        data.cascade_slope = spectrum_.slope_variance;
    }
    data.ocean = core::Vec4{spectrum_.choppiness, spectrum_.significant_height, static_cast<float>(kOceanLevels), 0.0f};
    if (ripple_size_ > 0 && frame < ripple_buffers_.size()) {
        ripple_buffers_[frame].write(ripples_.data(), sizeof(float) * ripple_size_ * ripple_size_);
    }
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

void WaterPass::recordSimulation(const vk::raii::CommandBuffer& cmd, std::uint32_t frame) {
    if (bodies_.empty() || spectrum_modes_.empty() || frame >= fft_sets_.size()) return;
    if (spectrum_uploaded_[frame] != spectrum_.key) {
        spectrum_buffers_[frame].write(spectrum_modes_.data(), sizeof(float) * spectrum_modes_.size());
        spectrum_uploaded_[frame] = spectrum_.key;
    }
    using Stage = vk::PipelineStageFlagBits2;
    using Access = vk::AccessFlagBits2;
    const std::uint32_t mips = ocean_mips_;
    const vk::Image disp = *displacement_.image;
    const vk::Image deriv = *derivatives_.image;
    const auto readers = Stage::eVertexShader | Stage::eFragmentShader;

    GpuFftPush push{};
    const float period = std::max(spectrum_.period, 1.0f);
    push.time = std::fmod(std::max(time_, 0.0f), period);
    float dt = time_ - last_simulated_;
    if (!ocean_ready_ || dt < 0.0f || dt > 1.0f) dt = 0.0f;
    last_simulated_ = time_;
    push.dt = dt;
    push.chop = spectrum_.choppiness;
    // Mas espuma = umbral de compresion mas alto (rompen olas menos empinadas).
    push.foam_threshold = 0.25f + 0.22f * std::clamp(spectrum_.foam, 0.0f, 3.0f);
    push.foam_decay = 1.0f / std::max(spectrum_.foam_persistence, 0.05f);
    push.reset = ocean_ready_ ? 0u : 1u;
    push.foam_amount = std::clamp(spectrum_.foam, 0.0f, 1.0f);

    // Mip 0 para escribir (General, conserva la espuma), el resto para copiar.
    const vk::ImageLayout previous = vk::ImageLayout::eShaderReadOnlyOptimal;
    barriers(cmd, {imageBarrier(disp, previous, vk::ImageLayout::eGeneral, readers, Access::eShaderSampledRead,
                                Stage::eComputeShader, Access::eShaderStorageRead | Access::eShaderStorageWrite, 0, 1,
                                kOceanCascades),
                   imageBarrier(deriv, previous, vk::ImageLayout::eGeneral, readers, Access::eShaderSampledRead,
                                Stage::eComputeShader, Access::eShaderStorageWrite, 0, 1, kOceanCascades),
                   imageBarrier(disp, previous, vk::ImageLayout::eTransferDstOptimal, readers,
                                Access::eShaderSampledRead, Stage::eBlit, Access::eTransferWrite, 1, mips - 1,
                                kOceanCascades),
                   imageBarrier(deriv, previous, vk::ImageLayout::eTransferDstOptimal, readers,
                                Access::eShaderSampledRead, Stage::eBlit, Access::eTransferWrite, 1, mips - 1,
                                kOceanCascades),
                   imageBarrier(*fft_image_.image, vk::ImageLayout::eGeneral, vk::ImageLayout::eGeneral,
                                Stage::eComputeShader, Access::eShaderStorageRead | Access::eShaderStorageWrite,
                                Stage::eComputeShader, Access::eShaderStorageRead | Access::eShaderStorageWrite, 0, 1,
                                kOceanCascades * 2)});

    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *fft_pass_.pipeline());
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *fft_pass_.layout(), 0, *fft_sets_[frame], nullptr);
    const auto run = [&](std::uint32_t mode, std::uint32_t layers) {
        push.mode = mode;
        cmd.pushConstants<GpuFftPush>(*fft_pass_.layout(), vk::ShaderStageFlagBits::eCompute, 0, push);
        cmd.dispatch(1, kOceanResolution, layers);
    };
    run(0, kOceanCascades);
    barriers(cmd, {}, true);
    run(1, kOceanCascades * 2);
    barriers(cmd, {}, true);
    run(2, kOceanCascades * 2);
    barriers(cmd, {}, true);
    run(3, kOceanCascades);

    // Mipmaps (media 2x2 por nivel): a lo lejos el oleaje se promedia.
    for (const vk::Image image : {disp, deriv}) {
        barriers(cmd, {imageBarrier(image, vk::ImageLayout::eGeneral, vk::ImageLayout::eTransferSrcOptimal,
                                    Stage::eComputeShader, Access::eShaderStorageWrite, Stage::eBlit,
                                    Access::eTransferRead, 0, 1, kOceanCascades)});
        for (std::uint32_t m = 1; m < mips; ++m) {
            const std::int32_t src = static_cast<std::int32_t>(kOceanResolution >> (m - 1));
            const std::int32_t dst = std::max(src / 2, 1);
            vk::ImageBlit2 region{};
            region.srcSubresource = vk::ImageSubresourceLayers{vk::ImageAspectFlagBits::eColor, m - 1, 0, kOceanCascades};
            region.srcOffsets[1] = vk::Offset3D{src, src, 1};
            region.dstSubresource = vk::ImageSubresourceLayers{vk::ImageAspectFlagBits::eColor, m, 0, kOceanCascades};
            region.dstOffsets[1] = vk::Offset3D{dst, dst, 1};
            vk::BlitImageInfo2 blit{};
            blit.srcImage = image;
            blit.srcImageLayout = vk::ImageLayout::eTransferSrcOptimal;
            blit.dstImage = image;
            blit.dstImageLayout = vk::ImageLayout::eTransferDstOptimal;
            blit.filter = vk::Filter::eLinear;
            blit.setRegions(region);
            cmd.blitImage2(blit);
            barriers(cmd, {imageBarrier(image, vk::ImageLayout::eTransferDstOptimal,
                                        vk::ImageLayout::eTransferSrcOptimal, Stage::eBlit, Access::eTransferWrite,
                                        Stage::eBlit, Access::eTransferRead, m, 1, kOceanCascades)});
        }
        barriers(cmd, {imageBarrier(image, vk::ImageLayout::eTransferSrcOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
                                    Stage::eBlit, Access::eTransferRead | Access::eTransferWrite, readers,
                                    Access::eShaderSampledRead, 0, mips, kOceanCascades)});
    }
    ocean_ready_ = true;
}

void WaterPass::record(const vk::raii::CommandBuffer& cmd, std::uint32_t frame,
                       const vk::raii::DescriptorSet& frame_set, const vk::raii::DescriptorSet& scene_set) {
    if (bodies_.empty() || frame >= sets_.size()) return;
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *layout_, 0,
                           {*frame_set, *sets_[frame], *scene_set}, nullptr);
    const auto stages = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment;
    // Primero lo que se ve bajo el agua (con la copia de la escena), despues
    // las superficies encima.
    if (underwater_ >= 0) {
        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *underwater_pipeline_);
        GpuWaterPush push{};
        push.body = static_cast<std::uint32_t>(underwater_);
        // Mismo codigo que la superficie (0 lago, 1 oceano, 2 rio); el tipo del
        // cuerpo va al reves (0 oceano, 1 lago).
        const float type = bodies_[static_cast<std::size_t>(underwater_)].params.extent.z;
        push.mesh = type < 0.5f ? 1u : (type < 1.5f ? 0u : 2u);
        cmd.pushConstants<GpuWaterPush>(*layout_, stages, 0, push);
        cmd.draw(3, 1, 0, 0);
    }
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *pipeline_);
    for (std::uint32_t i = 0; i < bodies_.size(); ++i) {
        const float type = bodies_[i].params.extent.z;
        GpuWaterPush push{};
        push.body = i;
        push.mesh = type < 0.5f ? 1u : (type < 1.5f ? 0u : 2u);
        cmd.pushConstants<GpuWaterPush>(*layout_, stages, 0, push);
        if (type < 0.5f) {
            // Oceano: nivel 0 (instancia 0) y los anillos (instancias 1..).
            cmd.bindVertexBuffers(0, *ocean_full_.vertices.handle(), {0});
            cmd.bindIndexBuffer(*ocean_full_.indices.handle(), 0, vk::IndexType::eUint32);
            cmd.drawIndexed(ocean_full_.index_count, 1, 0, 0, 0);
            cmd.bindIndexBuffer(*ocean_ring_.indices.handle(), 0, vk::IndexType::eUint32);
            cmd.drawIndexed(ocean_ring_.index_count, kOceanLevels - 1, 0, 0, 1);
            continue;
        }
        const Mesh* mesh = type < 1.5f ? &grid_ : &rivers_[i];
        if (mesh->index_count == 0) continue;
        cmd.bindVertexBuffers(0, *mesh->vertices.handle(), {0});
        cmd.bindIndexBuffer(*mesh->indices.handle(), 0, vk::IndexType::eUint32);
        cmd.drawIndexed(mesh->index_count, 1, 0, 0, 0);
    }
}

}  // namespace cramion::gfx
