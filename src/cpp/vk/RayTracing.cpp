#include "vk/RayTracing.h"

#include "asset/Model.h"
#include "vk/SkinnedModel.h"
#include "vk/VulkanBuffer.h"
#include "vk/VulkanDevice.h"
#include "vk/VulkanShader.h"
#include "vk/VulkanTexture.h"

#include <algorithm>
#include <cstring>
#include <iostream>

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
    std::vector<AccelerationStructure> blas;  // uno por modelo (vacio si no tiene triangulos)
    AccelerationStructure tlas;
    DeviceBuffer instance_buffer;
    std::vector<Instance> instances;
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
    std::array<vk::raii::Pipeline, kPassCount> pipelines{nullptr, nullptr};

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
    const std::array<Type, 7> frame_types = {
        Type::eUniformBuffer,        Type::eCombinedImageSampler, Type::eCombinedImageSampler,
        Type::eCombinedImageSampler, Type::eStorageImage,         Type::eUniformBuffer,
        Type::eCombinedImageSampler};
    std::array<vk::DescriptorSetLayoutBinding, 7> frame_bindings{};
    for (std::uint32_t i = 0; i < frame_bindings.size(); ++i) {
        frame_bindings[i].binding = i;
        frame_bindings[i].descriptorType = frame_types[i];
        frame_bindings[i].descriptorCount = 1;
        frame_bindings[i].stageFlags = vk::ShaderStageFlagBits::eCompute;
    }
    vk::DescriptorSetLayoutCreateInfo frame_layout_info{};
    frame_layout_info.setBindings(frame_bindings);
    r.frame_layout = vk::raii::DescriptorSetLayout(device.handle(), frame_layout_info);

    constexpr std::uint32_t kFrameSets = kMaxFramesInFlight * kPassCount;
    const std::array<vk::DescriptorPoolSize, 3> frame_sizes = {
        vk::DescriptorPoolSize{Type::eUniformBuffer, kFrameSets * 2},
        vk::DescriptorPoolSize{Type::eCombinedImageSampler, kFrameSets * 4},
        vk::DescriptorPoolSize{Type::eStorageImage, kFrameSets}};
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

    // --- Una BLAS por modelo ---
    r.blas.clear();
    for (const std::vector<GeometryRange>& geometries : model_geometries) {
        if (geometries.empty()) {
            r.blas.emplace_back();
            continue;
        }
        std::vector<vk::AccelerationStructureGeometryKHR> geometry_infos;
        std::vector<vk::AccelerationStructureBuildRangeInfoKHR> ranges;
        for (const GeometryRange& range : geometries) {
            vk::AccelerationStructureGeometryTrianglesDataKHR triangles{};
            triangles.vertexFormat = vk::Format::eR32G32B32Sfloat;
            triangles.vertexData.deviceAddress = r.vertices.address;
            triangles.vertexStride = sizeof(RtVertex);
            triangles.maxVertex = static_cast<std::uint32_t>(vertices.size() - 1);
            triangles.indexType = vk::IndexType::eUint32;
            triangles.indexData.deviceAddress =
                r.indices.address + static_cast<vk::DeviceAddress>(range.first_triangle) * 12;

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

    // --- Set 1: la escena ---
    using Type = vk::DescriptorType;
    std::array<vk::DescriptorSetLayoutBinding, 8> scene_bindings{};
    for (std::uint32_t i = 0; i < scene_bindings.size(); ++i) {
        scene_bindings[i].binding = i;
        scene_bindings[i].descriptorCount = 1;
        scene_bindings[i].stageFlags = vk::ShaderStageFlagBits::eCompute;
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
        vk::DescriptorPoolSize{Type::eStorageBuffer, 6},
        vk::DescriptorPoolSize{Type::eCombinedImageSampler, r.texture_count}};
    vk::DescriptorPoolCreateInfo scene_pool_info{};
    scene_pool_info.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    scene_pool_info.maxSets = 1;
    scene_pool_info.setPoolSizes(scene_sizes);
    r.scene_pool = vk::raii::DescriptorPool(device.handle(), scene_pool_info);

    const vk::DescriptorSetLayout scene_layout = *r.scene_layout;
    vk::DescriptorSetAllocateInfo alloc{};
    alloc.descriptorPool = *r.scene_pool;
    alloc.setSetLayouts(scene_layout);
    r.scene_sets = vk::raii::DescriptorSets(device.handle(), alloc);

    const std::array<vk::DescriptorBufferInfo, 6> buffers = {
        vk::DescriptorBufferInfo{*r.vertices.buffer, 0, VK_WHOLE_SIZE},
        vk::DescriptorBufferInfo{*r.indices.buffer, 0, VK_WHOLE_SIZE},
        vk::DescriptorBufferInfo{*r.triangle_materials.buffer, 0, VK_WHOLE_SIZE},
        vk::DescriptorBufferInfo{*r.materials.buffer, 0, VK_WHOLE_SIZE},
        vk::DescriptorBufferInfo{*r.models.buffer, 0, VK_WHOLE_SIZE},
        vk::DescriptorBufferInfo{*irradiance.handle(), 0, VK_WHOLE_SIZE}};
    std::array<vk::WriteDescriptorSet, 7> writes{};
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
    device.handle().updateDescriptorSets(writes, nullptr);

    // --- Pipelines ---
    const std::array<vk::DescriptorSetLayout, 2> set_layouts = {*r.frame_layout, *r.scene_layout};
    const vk::PushConstantRange push_range{vk::ShaderStageFlagBits::eCompute, 0, sizeof(Push)};
    vk::PipelineLayoutCreateInfo layout_info{};
    layout_info.setSetLayouts(set_layouts);
    layout_info.setPushConstantRanges(push_range);
    r.pipeline_layout = vk::raii::PipelineLayout(device.handle(), layout_info);

    const std::array<const char*, kPassCount> shaders = {"rt_gi.comp.spv",
                                                         "rt_reflections.comp.spv"};
    for (std::size_t i = 0; i < shaders.size(); ++i) {
        const vk::raii::ShaderModule module = shaders::loadModule(device, shaders[i]);
        vk::ComputePipelineCreateInfo pipeline_info{};
        pipeline_info.stage.stage = vk::ShaderStageFlagBits::eCompute;
        pipeline_info.stage.module = *module;
        pipeline_info.stage.pName = "main";
        pipeline_info.layout = *r.pipeline_layout;
        r.pipelines[i] = vk::raii::Pipeline(device.handle(), nullptr, pipeline_info);
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
    if (same && *r.tlas.handle != nullptr) {
        return;
    }
    r.instances = usable;
    if (usable.empty()) {
        r.tlas = AccelerationStructure{};
        return;
    }

    // Puede estar en uso por un frame en vuelo: se rehace con la GPU parada
    // (solo pasa al cargar o si un escenario se mueve).
    device.waitIdle();

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
        const vk::DescriptorImageInfo output{
            nullptr, pass == 0 ? inputs.gi_output : inputs.reflection_output,
            vk::ImageLayout::eGeneral};
        const vk::DescriptorImageInfo environment{inputs.environment_sampler, inputs.environment,
                                                  vk::ImageLayout::eShaderReadOnlyOptimal};

        std::array<vk::WriteDescriptorSet, 7> writes{};
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
        device.handle().updateDescriptorSets(writes, nullptr);
    }
}

bool RayTracing::ready() const {
    return resources_ != nullptr && !resources_->scene_sets.empty() &&
           *resources_->tlas.handle != nullptr && *resources_->pipelines[0] != nullptr;
}

void RayTracing::record(const vk::raii::CommandBuffer& cmd, std::uint32_t frame_index, Pass pass,
                        vk::Extent2D extent, const Push& push) const {
    const Resources& r = *resources_;
    const auto index = static_cast<std::uint32_t>(pass);
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *r.pipelines[index]);
    const std::array<vk::DescriptorSet, 2> sets = {*r.frame_sets[frame_index * kPassCount + index],
                                                   *r.scene_sets[0]};
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *r.pipeline_layout, 0, sets, nullptr);
    cmd.pushConstants<Push>(*r.pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0, push);
    cmd.dispatch((extent.width + kGroupSize - 1) / kGroupSize,
                 (extent.height + kGroupSize - 1) / kGroupSize, 1);
}

}  // namespace cramion::gfx
