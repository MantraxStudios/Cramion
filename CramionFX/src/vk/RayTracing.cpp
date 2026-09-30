#include "CramionFX/vk/RayTracing.h"

#include "CramionFX/asset/Model.h"
#include "CramionFX/vk/SkinnedModel.h"
#include "CramionFX/vk/VulkanBuffer.h"
#include "CramionFX/vk/VulkanDevice.h"
#include "CramionFX/vk/VulkanShader.h"
#include "CramionFX/vk/VulkanTexture.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string_view>

namespace cramion::gfx {
namespace {

// Deben coincidir con rt_common.glsl (std430).
struct RtVertex {
    float px, py, pz;
    float nx, ny, nz;
    float u, v;
};
static_assert(sizeof(RtVertex) == 32, "RtVertex debe coincidir con rt_common.glsl");

struct RtMaterial {
    core::Vec4 base_color{};
    core::Vec4 emissive{};
    core::Vec4 params{};
    std::uint32_t albedo_texture = 0;
    std::uint32_t metallic_roughness_texture = 0;
    std::uint32_t emissive_texture = 0;
    std::uint32_t flags = 0;
};
static_assert(sizeof(RtMaterial) == 64, "RtMaterial debe coincidir con rt_common.glsl");

constexpr std::uint32_t kGroupSize = 8;

// Buffer con direccion en la GPU (entradas de las estructuras de aceleracion
// y datos que leen los shaders). La memoria se declara antes que el buffer:
// se destruye despues.
struct DeviceBuffer {
    vk::raii::DeviceMemory memory{nullptr};
    vk::raii::Buffer buffer{nullptr};
    vk::DeviceAddress address = 0;
    vk::DeviceSize size = 0;
    void* mapped = nullptr;
};

DeviceBuffer createBuffer(const VulkanDevice& device, vk::DeviceSize size,
                          vk::BufferUsageFlags usage, bool host_visible,
                          const void* data = nullptr) {
    DeviceBuffer result;
    result.size = std::max<vk::DeviceSize>(size, 16);

    vk::BufferCreateInfo buffer_info{};
    buffer_info.size = result.size;
    buffer_info.usage = usage | vk::BufferUsageFlagBits::eShaderDeviceAddress |
                        (host_visible ? vk::BufferUsageFlags{}
                                      : vk::BufferUsageFlagBits::eTransferDst);
    buffer_info.sharingMode = vk::SharingMode::eExclusive;
    result.buffer = vk::raii::Buffer(device.handle(), buffer_info);

    const vk::MemoryRequirements requirements = result.buffer.getMemoryRequirements();
    vk::MemoryAllocateFlagsInfo flags_info{};
    flags_info.flags = vk::MemoryAllocateFlagBits::eDeviceAddress;
    vk::MemoryAllocateInfo allocate_info{};
    allocate_info.pNext = &flags_info;
    allocate_info.allocationSize = requirements.size;
    allocate_info.memoryTypeIndex = device.findMemoryType(
        requirements.memoryTypeBits,
        host_visible ? vk::MemoryPropertyFlagBits::eHostVisible |
                           vk::MemoryPropertyFlagBits::eHostCoherent
                     : vk::MemoryPropertyFlags{vk::MemoryPropertyFlagBits::eDeviceLocal});
    result.memory = vk::raii::DeviceMemory(device.handle(), allocate_info);
    result.buffer.bindMemory(*result.memory, 0);
    result.address = device.handle().getBufferAddress(vk::BufferDeviceAddressInfo{*result.buffer});

    if (host_visible) {
        result.mapped = result.memory.mapMemory(0, VK_WHOLE_SIZE);
        if (data != nullptr) {
            std::memcpy(result.mapped, data, static_cast<std::size_t>(size));
        }
    } else if (data != nullptr && size > 0) {
        VulkanBuffer staging;
        staging.create(device, size, vk::BufferUsageFlagBits::eTransferSrc,
                       vk::MemoryPropertyFlagBits::eHostVisible |
                           vk::MemoryPropertyFlagBits::eHostCoherent);
        staging.write(data, size);
        const vk::Buffer source = *staging.handle();
        const vk::Buffer target = *result.buffer;
        device.submitOneTime([&](const vk::raii::CommandBuffer& cmd) {
            cmd.copyBuffer(source, target, vk::BufferCopy{0, 0, size});
        });
        staging.destroy();
    }
    return result;
}

vk::DeviceAddress alignUp(vk::DeviceAddress value, vk::DeviceAddress alignment) {
    return alignment == 0 ? value : (value + alignment - 1) / alignment * alignment;
}

}  // namespace

// Estructura de aceleracion con su almacenamiento.
struct AccelerationStructure {
    DeviceBuffer storage;
    vk::raii::AccelerationStructureKHR handle{nullptr};
    vk::DeviceAddress address = 0;
};

struct RayTracing::Resources {
    // --- Escena ---
    DeviceBuffer vertices;
    DeviceBuffer indices;
    DeviceBuffer triangle_materials;
    DeviceBuffer materials;
    DeviceBuffer models;
    // Micromapa de opacidad del follaje (VK_EXT_opacity_micromap): uno para
    // todos los triangulos recortados por alfa. Las BLAS lo referencian: se
    // declara antes para que se destruya despues de ellas.
    DeviceBuffer micromap_storage;
    vk::raii::MicromapEXT micromap{nullptr};
    std::uint32_t micromap_level = 0;
    std::vector<AccelerationStructure> blas;  // uno por modelo (vacio si no tiene triangulos)
    AccelerationStructure tlas;
    DeviceBuffer instance_buffer;
    std::vector<Instance> instances;
    // El set de la escena actual ya tiene la TLAS escrita (al volver a subir la
    // escena el set es nuevo y hay que escribirla aunque las instancias no cambien).
    bool tlas_written = false;
    std::uint32_t texture_count = 0;
    vk::DeviceSize scratch_alignment = 0;

    vk::raii::Sampler texture_sampler{nullptr};

    // --- Descriptores y pipelines ---
    vk::raii::DescriptorSetLayout frame_layout{nullptr};
    vk::raii::DescriptorSetLayout scene_layout{nullptr};
    vk::raii::DescriptorPool frame_pool{nullptr};
    vk::raii::DescriptorPool scene_pool{nullptr};
    std::vector<vk::raii::DescriptorSet> frame_sets;  // [frame * kPassCount + pase]
    std::vector<vk::raii::DescriptorSet> scene_sets;  // uno
    vk::raii::PipelineLayout pipeline_layout{nullptr};
    std::array<vk::raii::Pipeline, kPassCount> pipelines{nullptr, nullptr, nullptr, nullptr, nullptr};
    // Etapas de los layouts: con pipeline de rayos, tambien raygen y any-hit.
    vk::ShaderStageFlags stages = vk::ShaderStageFlagBits::eCompute;

    // --- Path tracing con pipeline de rayos + SER (path_trace.rgen) ---
    vk::raii::PipelineLayout ser_layout{nullptr};
    vk::raii::Pipeline ser_pipeline{nullptr};
    DeviceBuffer sbt;  // tabla de shaders: raygen, miss, grupo de impacto
    vk::StridedDeviceAddressRegionKHR sbt_raygen{};
    vk::StridedDeviceAddressRegionKHR sbt_miss{};
    vk::StridedDeviceAddressRegionKHR sbt_hit{};

    void createSerPipeline(const VulkanDevice& device) {
        const std::array<vk::DescriptorSetLayout, 2> set_layouts = {*frame_layout, *scene_layout};
        const vk::PushConstantRange push_range{vk::ShaderStageFlagBits::eRaygenKHR, 0, sizeof(Push)};
        vk::PipelineLayoutCreateInfo layout_info{};
        layout_info.setSetLayouts(set_layouts);
        layout_info.setPushConstantRanges(push_range);
        ser_layout = vk::raii::PipelineLayout(device.handle(), layout_info);

        const vk::raii::ShaderModule raygen = shaders::loadModule(device, "path_trace.rgen.spv");
        const vk::raii::ShaderModule miss = shaders::loadModule(device, "path_trace.rmiss.spv");
        const vk::raii::ShaderModule any_hit = shaders::loadModule(device, "path_trace.rahit.spv");
        std::array<vk::PipelineShaderStageCreateInfo, 3> stage_infos{};
        stage_infos[0] = vk::PipelineShaderStageCreateInfo{{}, vk::ShaderStageFlagBits::eRaygenKHR, *raygen, "main"};
        stage_infos[1] = vk::PipelineShaderStageCreateInfo{{}, vk::ShaderStageFlagBits::eMissKHR, *miss, "main"};
        stage_infos[2] = vk::PipelineShaderStageCreateInfo{{}, vk::ShaderStageFlagBits::eAnyHitKHR, *any_hit, "main"};
        std::array<vk::RayTracingShaderGroupCreateInfoKHR, 3> groups{};
        for (auto& group : groups) {
            group.generalShader = VK_SHADER_UNUSED_KHR;
            group.closestHitShader = VK_SHADER_UNUSED_KHR;
            group.anyHitShader = VK_SHADER_UNUSED_KHR;
            group.intersectionShader = VK_SHADER_UNUSED_KHR;
        }
        groups[0].type = vk::RayTracingShaderGroupTypeKHR::eGeneral;
        groups[0].generalShader = 0;
        groups[1].type = vk::RayTracingShaderGroupTypeKHR::eGeneral;
        groups[1].generalShader = 1;
        groups[2].type = vk::RayTracingShaderGroupTypeKHR::eTrianglesHitGroup;
        groups[2].anyHitShader = 2;
        vk::RayTracingPipelineCreateInfoKHR info{};
        info.setStages(stage_infos);
        info.setGroups(groups);
        info.maxPipelineRayRecursionDepth = 1;
        info.layout = *ser_layout;
        ser_pipeline = vk::raii::Pipeline(device.handle(), nullptr, device.pipelineCache(), info);

        // Tabla de shaders: un registro por grupo, cada region alineada.
        const auto properties =
            device.physicalDevice()
                .getProperties2<vk::PhysicalDeviceProperties2, vk::PhysicalDeviceRayTracingPipelinePropertiesKHR>()
                .get<vk::PhysicalDeviceRayTracingPipelinePropertiesKHR>();
        const std::uint32_t handle_size = properties.shaderGroupHandleSize;
        const vk::DeviceSize stride = alignUp(handle_size, properties.shaderGroupHandleAlignment);
        const vk::DeviceSize base = properties.shaderGroupBaseAlignment;
        const std::vector<std::uint8_t> handles =
            ser_pipeline.getRayTracingShaderGroupHandlesKHR<std::uint8_t>(0, 3, 3 * handle_size);
        const vk::DeviceSize miss_offset = alignUp(stride, base);
        const vk::DeviceSize hit_offset = alignUp(miss_offset + stride, base);
        sbt = createBuffer(device, hit_offset + stride + base,
                           vk::BufferUsageFlagBits::eShaderBindingTableKHR, true);
        // La tabla empieza alineada a base dentro del buffer.
        const vk::DeviceAddress start = alignUp(sbt.address, base);
        auto* bytes = static_cast<std::uint8_t*>(sbt.mapped) + (start - sbt.address);
        std::memcpy(bytes, handles.data(), handle_size);
        std::memcpy(bytes + miss_offset, handles.data() + handle_size, handle_size);
        std::memcpy(bytes + hit_offset, handles.data() + 2 * handle_size, handle_size);
        sbt_raygen = vk::StridedDeviceAddressRegionKHR{start, stride, stride};
        sbt_miss = vk::StridedDeviceAddressRegionKHR{start + miss_offset, stride, stride};
        sbt_hit = vk::StridedDeviceAddressRegionKHR{start + hit_offset, stride, stride};
        std::cout << "[Vulkan] Path tracing con pipeline de rayos y Shader Execution Reordering\n";
    }

    // --- Cache de radiancia en el mundo (rt_common.glsl) ---
    DeviceBuffer cache_keys;      // uint por entrada
    DeviceBuffer cache_accum;     // CacheEntry (32 bytes)
    DeviceBuffer cache_resolved;  // vec4
    bool cache_reset = true;

    // Construye una estructura de aceleracion (y espera a que termine).
    AccelerationStructure buildStructure(
        const VulkanDevice& device, vk::AccelerationStructureTypeKHR type,
        const std::vector<vk::AccelerationStructureGeometryKHR>& geometries,
        const std::vector<vk::AccelerationStructureBuildRangeInfoKHR>& ranges) const {
        vk::AccelerationStructureBuildGeometryInfoKHR info{};
        info.type = type;
        info.flags = vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace;
        info.mode = vk::BuildAccelerationStructureModeKHR::eBuild;
        info.setGeometries(geometries);

        std::vector<std::uint32_t> primitive_counts;
        for (const auto& range : ranges) {
            primitive_counts.push_back(range.primitiveCount);
        }
        const vk::AccelerationStructureBuildSizesInfoKHR sizes =
            device.handle().getAccelerationStructureBuildSizesKHR(
                vk::AccelerationStructureBuildTypeKHR::eDevice, info, primitive_counts);

        AccelerationStructure result;
        result.storage = createBuffer(device, sizes.accelerationStructureSize,
                                      vk::BufferUsageFlagBits::eAccelerationStructureStorageKHR,
                                      false);
        vk::AccelerationStructureCreateInfoKHR create_info{};
        create_info.buffer = *result.storage.buffer;
        create_info.size = sizes.accelerationStructureSize;
        create_info.type = type;
        result.handle = vk::raii::AccelerationStructureKHR(device.handle(), create_info);

        const DeviceBuffer scratch =
            createBuffer(device, sizes.buildScratchSize + scratch_alignment,
                         vk::BufferUsageFlagBits::eStorageBuffer, false);
        info.dstAccelerationStructure = *result.handle;
        info.scratchData.deviceAddress = alignUp(scratch.address, scratch_alignment);

        const vk::AccelerationStructureBuildRangeInfoKHR* range_pointer = ranges.data();
        device.submitOneTime([&](const vk::raii::CommandBuffer& cmd) {
            cmd.buildAccelerationStructuresKHR(info, range_pointer);
        });

        result.address = device.handle().getAccelerationStructureAddressKHR(
            vk::AccelerationStructureDeviceAddressInfoKHR{*result.handle});
        return result;
    }

    // Hornea (rt_omm_bake.comp, con las texturas del set de la escena) y
    // construye el micromapa de opacidad de los triangulos `masked` (indices
    // globales, en el orden del micromapa). Deja `micromap` y `micromap_level`.
    void buildOpacityMicromap(const VulkanDevice& device, const std::vector<std::uint32_t>& masked) {
        const auto count = static_cast<std::uint32_t>(masked.size());
        // 256 microtriangulos (nivel 4, 64 bytes) por triangulo; con mucho
        // follaje, 64 (nivel 3, 16 bytes).
        micromap_level = count > 400000 ? 3u : 4u;
        const std::uint32_t words = (1u << (2u * micromap_level)) / 16u;
        const std::uint32_t bytes_per_triangle = words * 4u;

        // --- Horneado en la GPU ---
        const DeviceBuffer triangle_list =
            createBuffer(device, sizeof(std::uint32_t) * count, vk::BufferUsageFlagBits::eStorageBuffer, false,
                         masked.data());
        // Visible desde la CPU: se cuentan los estados para el registro.
        const DeviceBuffer data = createBuffer(
            device, static_cast<vk::DeviceSize>(bytes_per_triangle) * count,
            vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eMicromapBuildInputReadOnlyEXT, true);

        using Type = vk::DescriptorType;
        std::array<vk::DescriptorSetLayoutBinding, 2> bindings{};
        for (std::uint32_t i = 0; i < 2; ++i) {
            bindings[i].binding = i;
            bindings[i].descriptorType = Type::eStorageBuffer;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = vk::ShaderStageFlagBits::eCompute;
        }
        vk::DescriptorSetLayoutCreateInfo layout_info{};
        layout_info.setBindings(bindings);
        const vk::raii::DescriptorSetLayout bake_layout(device.handle(), layout_info);
        const vk::DescriptorPoolSize pool_size{Type::eStorageBuffer, 2};
        vk::DescriptorPoolCreateInfo pool_info{};
        pool_info.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
        pool_info.maxSets = 1;
        pool_info.setPoolSizes(pool_size);
        const vk::raii::DescriptorPool pool(device.handle(), pool_info);
        const vk::DescriptorSetLayout raw_layout = *bake_layout;
        vk::DescriptorSetAllocateInfo alloc{};
        alloc.descriptorPool = *pool;
        alloc.setSetLayouts(raw_layout);
        vk::raii::DescriptorSets sets(device.handle(), alloc);
        const std::array<vk::DescriptorBufferInfo, 2> infos = {
            vk::DescriptorBufferInfo{*triangle_list.buffer, 0, VK_WHOLE_SIZE},
            vk::DescriptorBufferInfo{*data.buffer, 0, VK_WHOLE_SIZE}};
        std::array<vk::WriteDescriptorSet, 2> writes{};
        for (std::uint32_t i = 0; i < 2; ++i) {
            writes[i].dstSet = *sets[0];
            writes[i].dstBinding = i;
            writes[i].descriptorType = Type::eStorageBuffer;
            writes[i].setBufferInfo(infos[i]);
        }
        device.handle().updateDescriptorSets(writes, nullptr);

        struct BakePush {
            std::uint32_t triangle_count;
            std::uint32_t level;
            std::uint32_t words_per_triangle;
            std::uint32_t pad;
        };
        const std::array<vk::DescriptorSetLayout, 3> set_layouts = {*frame_layout, *scene_layout, *bake_layout};
        const vk::PushConstantRange push_range{vk::ShaderStageFlagBits::eCompute, 0, sizeof(BakePush)};
        vk::PipelineLayoutCreateInfo pipeline_layout_info{};
        pipeline_layout_info.setSetLayouts(set_layouts);
        pipeline_layout_info.setPushConstantRanges(push_range);
        const vk::raii::PipelineLayout bake_pipeline_layout(device.handle(), pipeline_layout_info);
        const vk::raii::ShaderModule module = shaders::loadModule(device, "rt_omm_bake.comp.spv");
        vk::ComputePipelineCreateInfo pipeline_info{};
        pipeline_info.stage.stage = vk::ShaderStageFlagBits::eCompute;
        pipeline_info.stage.module = *module;
        pipeline_info.stage.pName = "main";
        pipeline_info.layout = *bake_pipeline_layout;
        const vk::raii::Pipeline bake_pipeline(device.handle(), device.pipelineCache(), pipeline_info);

        const BakePush push{count, micromap_level, words, 0};
        const std::uint64_t threads = static_cast<std::uint64_t>(count) * words;
        device.submitOneTime([&](const vk::raii::CommandBuffer& cmd) {
            cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *bake_pipeline);
            const std::array<vk::DescriptorSet, 2> bound = {*scene_sets[0], *sets[0]};
            cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *bake_pipeline_layout, 1, bound, nullptr);
            cmd.pushConstants<BakePush>(*bake_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0, push);
            cmd.dispatch(static_cast<std::uint32_t>((threads + 63) / 64), 1, 1);
        });

        // Cuantos microtriangulos quedaron resueltos (sin shader).
        std::array<std::uint64_t, 4> histogram{};
        const auto* states = static_cast<const std::uint32_t*>(data.mapped);
        const std::uint64_t total_words = static_cast<std::uint64_t>(count) * words;
        for (std::uint64_t w = 0; w < total_words; ++w) {
            for (std::uint32_t j = 0; j < 16; ++j) ++histogram[(states[w] >> (2u * j)) & 3u];
        }
        const std::uint64_t resolved = histogram[0] + histogram[1];
        std::cout << "[Vulkan] Micromapa: transparentes " << histogram[0] << ", opacos " << histogram[1]
                  << ", dudosos " << histogram[2] + histogram[3] << " (primera palabra 0x" << std::hex
                  << (total_words > 0 ? states[0] : 0u) << std::dec << ")\n";

        // --- Construccion del micromapa ---
        const auto format = static_cast<std::uint16_t>(vk::OpacityMicromapFormatEXT::e4State);
        std::vector<vk::MicromapTriangleEXT> triangles(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            triangles[i].dataOffset = i * bytes_per_triangle;
            triangles[i].subdivisionLevel = static_cast<std::uint16_t>(micromap_level);
            triangles[i].format = format;
        }
        const DeviceBuffer triangle_array =
            createBuffer(device, sizeof(vk::MicromapTriangleEXT) * count,
                         vk::BufferUsageFlagBits::eMicromapBuildInputReadOnlyEXT, false, triangles.data());

        const vk::MicromapUsageEXT usage{count, micromap_level, format};
        vk::MicromapBuildInfoEXT info{};
        info.type = vk::MicromapTypeEXT::eOpacityMicromap;
        info.flags = vk::BuildMicromapFlagBitsEXT::ePreferFastTrace;
        info.mode = vk::BuildMicromapModeEXT::eBuild;
        info.usageCountsCount = 1;
        info.pUsageCounts = &usage;
        const vk::MicromapBuildSizesInfoEXT sizes =
            device.handle().getMicromapBuildSizesEXT(vk::AccelerationStructureBuildTypeKHR::eDevice, info);

        micromap_storage = createBuffer(device, sizes.micromapSize, vk::BufferUsageFlagBits::eMicromapStorageEXT, false);
        vk::MicromapCreateInfoEXT create_info{};
        create_info.buffer = *micromap_storage.buffer;
        create_info.size = sizes.micromapSize;
        create_info.type = vk::MicromapTypeEXT::eOpacityMicromap;
        micromap = vk::raii::MicromapEXT(device.handle(), create_info);

        const DeviceBuffer scratch = createBuffer(device, sizes.buildScratchSize + scratch_alignment,
                                                  vk::BufferUsageFlagBits::eStorageBuffer, false);
        info.dstMicromap = *micromap;
        info.scratchData.deviceAddress = alignUp(scratch.address, scratch_alignment);
        info.data.deviceAddress = data.address;
        info.triangleArray.deviceAddress = triangle_array.address;
        info.triangleArrayStride = sizeof(vk::MicromapTriangleEXT);
        device.submitOneTime([&](const vk::raii::CommandBuffer& cmd) { cmd.buildMicromapsEXT(info); });

        const double percent = total_words > 0 ? 100.0 * static_cast<double>(resolved) /
                                                     static_cast<double>(total_words * 16) : 0.0;
        std::cout << "[Vulkan] Micromapa de opacidad: " << count << " triangulos de follaje, nivel " << micromap_level
                  << " (" << (1u << (2u * micromap_level)) << " microtriangulos c/u), "
                  << static_cast<int>(percent + 0.5) << "% resueltos sin shader\n";
    }
};

RayTracing::RayTracing() = default;
RayTracing::~RayTracing() = default;

void RayTracing::create(const VulkanDevice& device) {
    destroy();
    resources_ = std::make_unique<Resources>();
    Resources& r = *resources_;

    const auto properties =
        device.physicalDevice()
            .getProperties2<vk::PhysicalDeviceProperties2,
                            vk::PhysicalDeviceAccelerationStructurePropertiesKHR>();
    r.scratch_alignment = properties.get<vk::PhysicalDeviceAccelerationStructurePropertiesKHR>()
                              .minAccelerationStructureScratchOffsetAlignment;

    // --- Set 0: lo de cada frame ---
    using Type = vk::DescriptorType;
    // 7, 8, 9: albedo y material del G-buffer y la acumulacion (path tracing).
    // 10: el modelo de sombreado de Disney del G-buffer.
    const std::array<Type, 11> frame_types = {
        Type::eUniformBuffer,        Type::eCombinedImageSampler, Type::eCombinedImageSampler,
        Type::eCombinedImageSampler, Type::eStorageImage,         Type::eUniformBuffer,
        Type::eCombinedImageSampler, Type::eCombinedImageSampler, Type::eCombinedImageSampler,
        Type::eStorageImage,         Type::eCombinedImageSampler};
    // Con pipeline de rayos, los sets tambien los leen el raygen y el any-hit.
    if (device.rayTracingPipelineSupported()) {
        r.stages |= vk::ShaderStageFlagBits::eRaygenKHR | vk::ShaderStageFlagBits::eAnyHitKHR;
    }
    std::array<vk::DescriptorSetLayoutBinding, 11> frame_bindings{};
    for (std::uint32_t i = 0; i < frame_bindings.size(); ++i) {
        frame_bindings[i].binding = i;
        frame_bindings[i].descriptorType = frame_types[i];
        frame_bindings[i].descriptorCount = 1;
        frame_bindings[i].stageFlags = r.stages;
    }
    vk::DescriptorSetLayoutCreateInfo frame_layout_info{};
    frame_layout_info.setBindings(frame_bindings);
    r.frame_layout = vk::raii::DescriptorSetLayout(device.handle(), frame_layout_info);

    constexpr std::uint32_t kFrameSets = kMaxFramesInFlight * kPassCount;
    const std::array<vk::DescriptorPoolSize, 3> frame_sizes = {
        vk::DescriptorPoolSize{Type::eUniformBuffer, kFrameSets * 2},
        vk::DescriptorPoolSize{Type::eCombinedImageSampler, kFrameSets * 7},
        vk::DescriptorPoolSize{Type::eStorageImage, kFrameSets * 2}};
    vk::DescriptorPoolCreateInfo frame_pool_info{};
    frame_pool_info.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    frame_pool_info.maxSets = kFrameSets;
    frame_pool_info.setPoolSizes(frame_sizes);
    r.frame_pool = vk::raii::DescriptorPool(device.handle(), frame_pool_info);

    const std::vector<vk::DescriptorSetLayout> layouts(kFrameSets, *r.frame_layout);
    vk::DescriptorSetAllocateInfo alloc{};
    alloc.descriptorPool = *r.frame_pool;
    alloc.setSetLayouts(layouts);
    r.frame_sets = vk::raii::DescriptorSets(device.handle(), alloc);

    // Texturas de los materiales: con mips y repeticion (UV fuera de [0, 1]).
    vk::SamplerCreateInfo sampler_info{};
    sampler_info.magFilter = vk::Filter::eLinear;
    sampler_info.minFilter = vk::Filter::eLinear;
    sampler_info.mipmapMode = vk::SamplerMipmapMode::eLinear;
    sampler_info.addressModeU = vk::SamplerAddressMode::eRepeat;
    sampler_info.addressModeV = vk::SamplerAddressMode::eRepeat;
    sampler_info.addressModeW = vk::SamplerAddressMode::eRepeat;
    sampler_info.maxLod = VK_LOD_CLAMP_NONE;
    r.texture_sampler = vk::raii::Sampler(device.handle(), sampler_info);

    // Cache de radiancia: 2 + 16 + 8 MB, a cero.
    const auto storage = vk::BufferUsageFlagBits::eStorageBuffer;
    r.cache_keys = createBuffer(device, sizeof(std::uint32_t) * kCacheEntries, storage, false);
    r.cache_accum = createBuffer(device, 32ull * kCacheEntries, storage, false);
    r.cache_resolved = createBuffer(device, 16ull * kCacheEntries, storage, false);
    device.submitOneTime([&](const vk::raii::CommandBuffer& cmd) {
        cmd.fillBuffer(*r.cache_keys.buffer, 0, VK_WHOLE_SIZE, 0u);
        cmd.fillBuffer(*r.cache_accum.buffer, 0, VK_WHOLE_SIZE, 0u);
        cmd.fillBuffer(*r.cache_resolved.buffer, 0, VK_WHOLE_SIZE, 0u);
    });
}

void RayTracing::resetCache() {
    if (resources_) resources_->cache_reset = true;
}

void RayTracing::destroy() {
    resources_.reset();
}

void RayTracing::build(const VulkanDevice& device,
                       const std::vector<const asset::ModelData*>& models,
                       const std::vector<SkinnedModel>& gpu_models,
                       const VulkanBuffer& irradiance) {
    Resources& r = *resources_;

    // --- Datos de la escena: todos los modelos en los mismos buffers ---
    std::vector<RtVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<std::uint32_t> triangle_materials;
    std::vector<RtMaterial> materials;
    std::vector<std::uint32_t> model_info;  // uvec4 por modelo
    std::vector<vk::DescriptorImageInfo> textures;

    // Por modelo: triangulos de cada geometria (opaca, recortada).
    struct GeometryRange {
        std::uint32_t first_triangle = 0;
        std::uint32_t triangle_count = 0;
        bool opaque = true;
    };
    std::vector<std::vector<GeometryRange>> model_geometries;

    for (std::size_t m = 0; m < models.size(); ++m) {
        const asset::ModelData& model = *models[m];
        const SkinnedModel& gpu = gpu_models[m];
        const auto vertex_base = static_cast<std::uint32_t>(vertices.size());
        const auto material_base = static_cast<std::uint32_t>(materials.size());
        const auto texture_base = static_cast<std::uint32_t>(textures.size());

        for (const asset::SkinnedVertex& v : model.vertices) {
            vertices.push_back(RtVertex{v.position.x, v.position.y, v.position.z, v.normal.x,
                                        v.normal.y, v.normal.z, v.uv.x, v.uv.y});
        }

        for (const VulkanTexture& texture : gpu.textures()) {
            vk::DescriptorImageInfo info{};
            info.sampler = *r.texture_sampler;
            info.imageView = *texture.view();
            info.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
            textures.push_back(info);
        }

        for (const SkinnedModel::Material& material : gpu.materials()) {
            RtMaterial rt{};
            rt.base_color = material.base_color;
            rt.emissive = material.emissive;
            rt.params = core::Vec4{material.params.x, material.params.y, 0.0f, 0.0f};
            rt.albedo_texture = texture_base + material.albedo_texture;
            rt.metallic_roughness_texture = texture_base + material.metallic_roughness_texture;
            rt.emissive_texture = texture_base + material.emissive_texture;
            rt.flags = material.alpha_masked ? 1u : 0u;
            materials.push_back(rt);
        }

        // Triangulos: primero los opacos, luego los recortados por alfa.
        std::vector<GeometryRange> geometries;
        for (const bool masked : {false, true}) {
            GeometryRange range{};
            range.first_triangle = static_cast<std::uint32_t>(indices.size() / 3);
            range.opaque = !masked;
            for (const asset::SubMesh& submesh : model.submeshes) {
                const SkinnedModel::Material& material = gpu.materials()[submesh.material];
                if (material.transparent || material.alpha_masked != masked) {
                    continue;
                }
                for (std::uint32_t i = 0; i < submesh.index_count; ++i) {
                    indices.push_back(vertex_base + model.indices[submesh.first_index + i]);
                }
                for (std::uint32_t t = 0; t < submesh.index_count / 3; ++t) {
                    triangle_materials.push_back(material_base + submesh.material);
                }
            }
            range.triangle_count =
                static_cast<std::uint32_t>(indices.size() / 3) - range.first_triangle;
            if (range.triangle_count > 0) {
                geometries.push_back(range);
            }
        }
        model_info.push_back(geometries.size() > 0 ? geometries[0].first_triangle : 0);
        model_info.push_back(geometries.size() > 1 ? geometries[1].first_triangle : 0);
        model_info.push_back(0);
        model_info.push_back(0);
        model_geometries.push_back(std::move(geometries));
    }

    if (textures.empty() || indices.empty()) {
        std::cout << "[Vulkan] Trazado de rayos: la escena no tiene geometria\n";
        return;
    }
    r.texture_count = static_cast<std::uint32_t>(textures.size());

    const auto input = vk::BufferUsageFlagBits::eStorageBuffer |
                       vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR;
    r.vertices = createBuffer(device, sizeof(RtVertex) * vertices.size(), input, false,
                              vertices.data());
    r.indices = createBuffer(device, sizeof(std::uint32_t) * indices.size(), input, false,
                             indices.data());
    r.triangle_materials =
        createBuffer(device, sizeof(std::uint32_t) * triangle_materials.size(),
                     vk::BufferUsageFlagBits::eStorageBuffer, false, triangle_materials.data());
    r.materials = createBuffer(device, sizeof(RtMaterial) * materials.size(),
                               vk::BufferUsageFlagBits::eStorageBuffer, false, materials.data());
    r.models = createBuffer(device, sizeof(std::uint32_t) * model_info.size(),
                            vk::BufferUsageFlagBits::eStorageBuffer, false, model_info.data());

    // (Las BLAS se construyen al final: el micromapa de opacidad del follaje
    // se hornea con el set de la escena, que lleva las texturas.)
    r.blas.clear();

    // --- Set 1: la escena ---
    using Type = vk::DescriptorType;
    std::array<vk::DescriptorSetLayoutBinding, 11> scene_bindings{};
    for (std::uint32_t i = 0; i < scene_bindings.size(); ++i) {
        scene_bindings[i].binding = i;
        scene_bindings[i].descriptorCount = 1;
        scene_bindings[i].stageFlags = r.stages;
        scene_bindings[i].descriptorType = Type::eStorageBuffer;
    }
    scene_bindings[0].descriptorType = Type::eAccelerationStructureKHR;
    scene_bindings[7].descriptorType = Type::eCombinedImageSampler;
    scene_bindings[7].descriptorCount = r.texture_count;
    vk::DescriptorSetLayoutCreateInfo scene_layout_info{};
    scene_layout_info.setBindings(scene_bindings);
    r.scene_layout = vk::raii::DescriptorSetLayout(device.handle(), scene_layout_info);

    const std::array<vk::DescriptorPoolSize, 3> scene_sizes = {
        vk::DescriptorPoolSize{Type::eAccelerationStructureKHR, 1},
        vk::DescriptorPoolSize{Type::eStorageBuffer, 9},
        vk::DescriptorPoolSize{Type::eCombinedImageSampler, r.texture_count}};
    vk::DescriptorPoolCreateInfo scene_pool_info{};
    scene_pool_info.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    scene_pool_info.maxSets = 1;
    scene_pool_info.setPoolSizes(scene_sizes);
    // Al reconstruir (cada uploadModels: el editor lo llama al añadir
    // modelos) el set viejo se libera ANTES de sustituir su pool: si no, al
    // asignar scene_sets despues se liberaba sobre un pool ya destruido.
    r.scene_sets.clear();
    r.scene_pool = vk::raii::DescriptorPool(device.handle(), scene_pool_info);

    const vk::DescriptorSetLayout scene_layout = *r.scene_layout;
    vk::DescriptorSetAllocateInfo alloc{};
    alloc.descriptorPool = *r.scene_pool;
    alloc.setSetLayouts(scene_layout);
    r.scene_sets = vk::raii::DescriptorSets(device.handle(), alloc);
    r.tlas_written = false;

    const std::array<vk::DescriptorBufferInfo, 6> buffers = {
        vk::DescriptorBufferInfo{*r.vertices.buffer, 0, VK_WHOLE_SIZE},
        vk::DescriptorBufferInfo{*r.indices.buffer, 0, VK_WHOLE_SIZE},
        vk::DescriptorBufferInfo{*r.triangle_materials.buffer, 0, VK_WHOLE_SIZE},
        vk::DescriptorBufferInfo{*r.materials.buffer, 0, VK_WHOLE_SIZE},
        vk::DescriptorBufferInfo{*r.models.buffer, 0, VK_WHOLE_SIZE},
        vk::DescriptorBufferInfo{*irradiance.handle(), 0, VK_WHOLE_SIZE}};
    const std::array<vk::DescriptorBufferInfo, 3> cache_buffers = {
        vk::DescriptorBufferInfo{*r.cache_keys.buffer, 0, VK_WHOLE_SIZE},
        vk::DescriptorBufferInfo{*r.cache_accum.buffer, 0, VK_WHOLE_SIZE},
        vk::DescriptorBufferInfo{*r.cache_resolved.buffer, 0, VK_WHOLE_SIZE}};
    std::array<vk::WriteDescriptorSet, 10> writes{};
    for (std::uint32_t i = 0; i < buffers.size(); ++i) {
        writes[i].dstSet = *r.scene_sets[0];
        writes[i].dstBinding = 1 + i;
        writes[i].descriptorType = Type::eStorageBuffer;
        writes[i].setBufferInfo(buffers[i]);
    }
    writes[6].dstSet = *r.scene_sets[0];
    writes[6].dstBinding = 7;
    writes[6].descriptorType = Type::eCombinedImageSampler;
    writes[6].setImageInfo(textures);
    for (std::uint32_t i = 0; i < cache_buffers.size(); ++i) {
        writes[7 + i].dstSet = *r.scene_sets[0];
        writes[7 + i].dstBinding = 8 + i;
        writes[7 + i].descriptorType = Type::eStorageBuffer;
        writes[7 + i].setBufferInfo(cache_buffers[i]);
    }
    device.handle().updateDescriptorSets(writes, nullptr);
    // Escena nueva: lo guardado era de otros modelos.
    r.cache_reset = true;

    // --- Pipelines ---
    const std::array<vk::DescriptorSetLayout, 2> set_layouts = {*r.frame_layout, *r.scene_layout};
    const vk::PushConstantRange push_range{vk::ShaderStageFlagBits::eCompute, 0, sizeof(Push)};
    vk::PipelineLayoutCreateInfo layout_info{};
    layout_info.setSetLayouts(set_layouts);
    layout_info.setPushConstantRanges(push_range);
    r.pipeline_layout = vk::raii::PipelineLayout(device.handle(), layout_info);

    const std::array<const char*, kPassCount> shaders = {"rt_gi.comp.spv", "rt_reflections.comp.spv",
                                                         "rt_cache_resolve.comp.spv", "path_trace.comp.spv",
                                                         "rt_shadows.comp.spv"};
    for (std::size_t i = 0; i < shaders.size(); ++i) {
        const vk::raii::ShaderModule module = shaders::loadModule(device, shaders[i]);
        vk::ComputePipelineCreateInfo pipeline_info{};
        pipeline_info.stage.stage = vk::ShaderStageFlagBits::eCompute;
        pipeline_info.stage.module = *module;
        pipeline_info.stage.pName = "main";
        pipeline_info.layout = *r.pipeline_layout;
        r.pipelines[i] = vk::raii::Pipeline(device.handle(), device.pipelineCache(), pipeline_info);
    }
    // Path tracing con SER: opcional (CRAMION_SER=1). Medido en una RTX 4060
    // Ti (pueblo de la plantilla, 8 rebotes): compute con ray queries 0.31 ms
    // por muestra, pipeline de rayos 0.37, pipeline + SER 0.48. El sombreado
    // del motor es ligero (pocas texturas por impacto): reordenar cuesta mas
    // de lo que ahorra. Queda para materiales pesados.
    r.ser_pipeline = nullptr;
    const char* ser = std::getenv("CRAMION_SER");
    if (device.invocationReorderSupported() && ser != nullptr && std::string_view(ser) != "0") {
        r.createSerPipeline(device);
    }

    // --- Micromapa de opacidad del follaje (VK_EXT_opacity_micromap) ---
    // Los triangulos recortados por alfa, en orden: el micromapa tiene uno por
    // cada uno y cada geometria recortada empieza en su `base`.
    std::vector<std::uint32_t> masked;
    std::vector<std::vector<std::uint32_t>> micromap_base(model_geometries.size());
    for (std::size_t m = 0; m < model_geometries.size(); ++m) {
        for (const GeometryRange& range : model_geometries[m]) {
            micromap_base[m].push_back(range.opaque ? UINT32_MAX : static_cast<std::uint32_t>(masked.size()));
            if (range.opaque) continue;
            for (std::uint32_t t = 0; t < range.triangle_count; ++t) masked.push_back(range.first_triangle + t);
        }
    }
    r.micromap = nullptr;
    r.micromap_storage = DeviceBuffer{};
    r.micromap_level = 0;
    const bool omm_disabled = std::getenv("CRAMION_NO_OMM") != nullptr;
    if (device.opacityMicromapSupported() && !masked.empty() && !omm_disabled) {
        r.buildOpacityMicromap(device, masked);
    }

    // --- Una BLAS por modelo ---
    const auto omm_format = static_cast<std::uint32_t>(vk::OpacityMicromapFormatEXT::e4State);
    for (std::size_t m = 0; m < model_geometries.size(); ++m) {
        const std::vector<GeometryRange>& geometries = model_geometries[m];
        if (geometries.empty()) {
            r.blas.emplace_back();
            continue;
        }
        std::vector<vk::AccelerationStructureGeometryKHR> geometry_infos;
        std::vector<vk::AccelerationStructureBuildRangeInfoKHR> ranges;
        // Lo que enlaza cada geometria recortada con el micromapa (debe vivir
        // hasta que se construye la BLAS).
        std::vector<vk::AccelerationStructureTrianglesOpacityMicromapEXT> omm_links(geometries.size());
        std::vector<vk::MicromapUsageEXT> omm_usages(geometries.size());
        for (std::size_t g = 0; g < geometries.size(); ++g) {
            const GeometryRange& range = geometries[g];
            vk::AccelerationStructureGeometryTrianglesDataKHR triangles{};
            triangles.vertexFormat = vk::Format::eR32G32B32Sfloat;
            triangles.vertexData.deviceAddress = r.vertices.address;
            triangles.vertexStride = sizeof(RtVertex);
            triangles.maxVertex = static_cast<std::uint32_t>(vertices.size() - 1);
            triangles.indexType = vk::IndexType::eUint32;
            triangles.indexData.deviceAddress =
                r.indices.address + static_cast<vk::DeviceAddress>(range.first_triangle) * 12;
            if (!range.opaque && *r.micromap != nullptr) {
                omm_usages[g] = vk::MicromapUsageEXT{range.triangle_count, r.micromap_level, omm_format};
                vk::AccelerationStructureTrianglesOpacityMicromapEXT& link = omm_links[g];
                link.indexType = vk::IndexType::eNoneKHR;  // triangulo i -> base + i
                link.baseTriangle = micromap_base[m][g];
                link.micromap = *r.micromap;
                link.usageCountsCount = 1;
                link.pUsageCounts = &omm_usages[g];
                triangles.pNext = &link;
            }

            vk::AccelerationStructureGeometryKHR geometry{};
            geometry.geometryType = vk::GeometryTypeKHR::eTriangles;
            geometry.geometry.triangles = triangles;
            // Opacos: el hardware acepta el impacto sin preguntar al shader.
            geometry.flags = range.opaque ? vk::GeometryFlagBitsKHR::eOpaque
                                          : vk::GeometryFlagsKHR{};
            geometry_infos.push_back(geometry);

            vk::AccelerationStructureBuildRangeInfoKHR build_range{};
            build_range.primitiveCount = range.triangle_count;
            ranges.push_back(build_range);
        }
        r.blas.push_back(r.buildStructure(device, vk::AccelerationStructureTypeKHR::eBottomLevel,
                                          geometry_infos, ranges));
    }

    std::cout << "[Vulkan] Trazado de rayos listo: " << indices.size() / 3 << " triangulos, "
              << materials.size() << " materiales, " << r.texture_count << " texturas\n";
}

void RayTracing::setInstances(const VulkanDevice& device, const std::vector<Instance>& instances) {
    Resources& r = *resources_;
    if (r.scene_sets.empty()) {
        return;  // Escena sin subir.
    }

    // Solo los modelos que tienen BLAS.
    std::vector<Instance> usable;
    for (const Instance& instance : instances) {
        if (instance.model < r.blas.size() && *r.blas[instance.model].handle != nullptr) {
            usable.push_back(instance);
        }
    }
    const bool same =
        usable.size() == r.instances.size() &&
        std::equal(usable.begin(), usable.end(), r.instances.begin(),
                   [](const Instance& a, const Instance& b) {
                       return a.model == b.model &&
                              std::memcmp(&a.transform, &b.transform, sizeof(core::Mat4)) == 0;
                   });
    if (same && *r.tlas.handle != nullptr && r.tlas_written) {
        return;
    }
    r.instances = usable;

    // Puede estar en uso por un frame en vuelo: se rehace (o se destruye) con
    // la GPU parada (solo pasa al cargar o si un escenario se mueve). Tambien
    // al quedarse sin instancias: destruirla sin esperar era el error
    // "vkDestroyAccelerationStructureKHR ... in use by VkCommandBuffer" al
    // borrar el ultimo objeto o cambiar de escena en el editor.
    device.waitIdle();
    if (usable.empty()) {
        r.tlas = AccelerationStructure{};
        return;
    }

    std::vector<vk::AccelerationStructureInstanceKHR> gpu_instances;
    for (const Instance& instance : usable) {
        // 3x4 por filas; core::Mat4 esta por columnas (m[columna][fila]).
        vk::TransformMatrixKHR transform{};
        for (std::uint32_t row = 0; row < 3; ++row) {
            for (std::uint32_t column = 0; column < 4; ++column) {
                transform.matrix[row][column] = instance.transform.m[column][row];
            }
        }
        vk::AccelerationStructureInstanceKHR gpu{};
        gpu.setTransform(transform);
        gpu.setInstanceCustomIndex(instance.model);
        gpu.setMask(0xFF);
        gpu.setFlags(vk::GeometryInstanceFlagBitsKHR::eTriangleFacingCullDisable);
        gpu.setAccelerationStructureReference(r.blas[instance.model].address);
        gpu_instances.push_back(gpu);
    }
    r.instance_buffer = createBuffer(
        device, sizeof(vk::AccelerationStructureInstanceKHR) * gpu_instances.size(),
        vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR, true,
        gpu_instances.data());

    vk::AccelerationStructureGeometryInstancesDataKHR instances_data{};
    instances_data.arrayOfPointers = VK_FALSE;
    instances_data.data.deviceAddress = r.instance_buffer.address;
    vk::AccelerationStructureGeometryKHR geometry{};
    geometry.geometryType = vk::GeometryTypeKHR::eInstances;
    geometry.geometry.instances = instances_data;
    vk::AccelerationStructureBuildRangeInfoKHR range{};
    range.primitiveCount = static_cast<std::uint32_t>(gpu_instances.size());
    r.tlas = r.buildStructure(device, vk::AccelerationStructureTypeKHR::eTopLevel, {geometry},
                              {range});

    const vk::AccelerationStructureKHR tlas = *r.tlas.handle;
    vk::WriteDescriptorSetAccelerationStructureKHR tlas_info{};
    tlas_info.accelerationStructureCount = 1;
    tlas_info.pAccelerationStructures = &tlas;
    vk::WriteDescriptorSet write{};
    write.pNext = &tlas_info;
    write.dstSet = *r.scene_sets[0];
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = vk::DescriptorType::eAccelerationStructureKHR;
    device.handle().updateDescriptorSets(write, nullptr);
    r.tlas_written = true;

    std::cout << "[Vulkan] TLAS construida: " << gpu_instances.size() << " instancia(s)\n";
}

void RayTracing::updateFrameSet(const VulkanDevice& device, std::uint32_t frame_index,
                                const FrameInputs& inputs) {
    Resources& r = *resources_;
    for (std::uint32_t pass = 0; pass < kPassCount; ++pass) {
        const vk::DescriptorSet set = *r.frame_sets[frame_index * kPassCount + pass];

        const vk::DescriptorBufferInfo camera{*inputs.camera->handle(), 0, VK_WHOLE_SIZE};
        const vk::DescriptorBufferInfo lights{*inputs.lights->handle(), 0, VK_WHOLE_SIZE};
        const vk::DescriptorImageInfo depth{inputs.sampler, inputs.depth,
                                            vk::ImageLayout::eDepthReadOnlyOptimal};
        const vk::DescriptorImageInfo normal{inputs.sampler, inputs.normal,
                                             vk::ImageLayout::eShaderReadOnlyOptimal};
        const vk::DescriptorImageInfo previous{inputs.sampler, inputs.previous_color,
                                               vk::ImageLayout::eShaderReadOnlyOptimal};
        // La resolucion de la cache no escribe imagen: se le pone la de la GI.
        const vk::ImageView output_view = pass == 1   ? inputs.reflection_output
                                          : pass == 3 ? inputs.path_output
                                          : pass == 4 ? inputs.shadow_output
                                                      : inputs.gi_output;
        const vk::DescriptorImageInfo output{nullptr, output_view, vk::ImageLayout::eGeneral};
        const vk::DescriptorImageInfo albedo{inputs.sampler, inputs.albedo,
                                             vk::ImageLayout::eShaderReadOnlyOptimal};
        const vk::DescriptorImageInfo material{inputs.sampler, inputs.material,
                                               vk::ImageLayout::eShaderReadOnlyOptimal};
        const vk::DescriptorImageInfo accumulation{nullptr, inputs.accumulation, vk::ImageLayout::eGeneral};
        const vk::DescriptorImageInfo shading{inputs.sampler, inputs.shading,
                                              vk::ImageLayout::eShaderReadOnlyOptimal};
        const vk::DescriptorImageInfo environment{inputs.environment_sampler, inputs.environment,
                                                  vk::ImageLayout::eShaderReadOnlyOptimal};

        std::array<vk::WriteDescriptorSet, 11> writes{};
        for (std::uint32_t i = 0; i < writes.size(); ++i) {
            writes[i].dstSet = set;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
        }
        writes[0].descriptorType = vk::DescriptorType::eUniformBuffer;
        writes[0].pBufferInfo = &camera;
        writes[1].descriptorType = vk::DescriptorType::eCombinedImageSampler;
        writes[1].pImageInfo = &depth;
        writes[2].descriptorType = vk::DescriptorType::eCombinedImageSampler;
        writes[2].pImageInfo = &normal;
        writes[3].descriptorType = vk::DescriptorType::eCombinedImageSampler;
        writes[3].pImageInfo = &previous;
        writes[4].descriptorType = vk::DescriptorType::eStorageImage;
        writes[4].pImageInfo = &output;
        writes[5].descriptorType = vk::DescriptorType::eUniformBuffer;
        writes[5].pBufferInfo = &lights;
        writes[6].descriptorType = vk::DescriptorType::eCombinedImageSampler;
        writes[6].pImageInfo = &environment;
        writes[7].descriptorType = vk::DescriptorType::eCombinedImageSampler;
        writes[7].pImageInfo = &albedo;
        writes[8].descriptorType = vk::DescriptorType::eCombinedImageSampler;
        writes[8].pImageInfo = &material;
        writes[9].descriptorType = vk::DescriptorType::eStorageImage;
        writes[9].pImageInfo = &accumulation;
        writes[10].descriptorType = vk::DescriptorType::eCombinedImageSampler;
        writes[10].pImageInfo = &shading;
        device.handle().updateDescriptorSets(writes, nullptr);
    }
}

bool RayTracing::serActive() const {
    return resources_ != nullptr && *resources_->ser_pipeline != nullptr;
}

bool RayTracing::ready() const {
    return resources_ != nullptr && !resources_->scene_sets.empty() &&
           *resources_->tlas.handle != nullptr && resources_->tlas_written &&
           *resources_->pipelines[0] != nullptr;
}

void RayTracing::record(const vk::raii::CommandBuffer& cmd, std::uint32_t frame_index, Pass pass,
                        vk::Extent2D extent, const Push& push) const {
    Resources& r = *resources_;
    const auto index = static_cast<std::uint32_t>(pass);
    // La cache de radiancia se escribe (GI, resolucion) y se lee (GI,
    // reflejos) en pases seguidos: cada uno espera a que el anterior acabe.
    vk::MemoryBarrier2 cache_barrier{};
    cache_barrier.srcStageMask = vk::PipelineStageFlagBits2::eComputeShader;
    cache_barrier.srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite;
    cache_barrier.dstStageMask = vk::PipelineStageFlagBits2::eComputeShader;
    cache_barrier.dstAccessMask = vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite;
    vk::DependencyInfo dependency{};
    dependency.setMemoryBarriers(cache_barrier);
    cmd.pipelineBarrier2(dependency);

    Push data = push;
    if (pass == Pass::PathTrace && *r.ser_pipeline != nullptr) {
        // Lo que escribieron el raster y los compute (G-buffer, imagen de la
        // escena) lo leen ahora shaders de rayos.
        vk::MemoryBarrier2 to_rays{};
        to_rays.srcStageMask = vk::PipelineStageFlagBits2::eAllCommands;
        to_rays.srcAccessMask = vk::AccessFlagBits2::eMemoryWrite;
        to_rays.dstStageMask = vk::PipelineStageFlagBits2::eRayTracingShaderKHR;
        to_rays.dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
        vk::DependencyInfo to_rays_dependency{};
        to_rays_dependency.setMemoryBarriers(to_rays);
        cmd.pipelineBarrier2(to_rays_dependency);
        cmd.bindPipeline(vk::PipelineBindPoint::eRayTracingKHR, *r.ser_pipeline);
        const std::array<vk::DescriptorSet, 2> rt_sets = {*r.frame_sets[frame_index * kPassCount + index],
                                                          *r.scene_sets[0]};
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eRayTracingKHR, *r.ser_layout, 0, rt_sets, nullptr);
        cmd.pushConstants<Push>(*r.ser_layout, vk::ShaderStageFlagBits::eRaygenKHR, 0, data);
        cmd.traceRaysKHR(r.sbt_raygen, r.sbt_miss, r.sbt_hit, vk::StridedDeviceAddressRegionKHR{}, extent.width,
                         extent.height, 1);
        // Y lo que escribio el path tracing lo leen despues los compute y el raster.
        vk::MemoryBarrier2 from_rays{};
        from_rays.srcStageMask = vk::PipelineStageFlagBits2::eRayTracingShaderKHR;
        from_rays.srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite;
        from_rays.dstStageMask = vk::PipelineStageFlagBits2::eAllCommands;
        from_rays.dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
        vk::DependencyInfo from_rays_dependency{};
        from_rays_dependency.setMemoryBarriers(from_rays);
        cmd.pipelineBarrier2(from_rays_dependency);
        return;
    }
    if (pass == Pass::CacheResolve) {
        data.params.z = r.cache_reset ? 1.0f : 0.0f;
        r.cache_reset = false;
    }
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *r.pipelines[index]);
    const std::array<vk::DescriptorSet, 2> sets = {*r.frame_sets[frame_index * kPassCount + index],
                                                   *r.scene_sets[0]};
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *r.pipeline_layout, 0, sets, nullptr);
    cmd.pushConstants<Push>(*r.pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0, data);
    cmd.dispatch((extent.width + kGroupSize - 1) / kGroupSize,
                 (extent.height + kGroupSize - 1) / kGroupSize, 1);
}

}  // namespace cramion::gfx
