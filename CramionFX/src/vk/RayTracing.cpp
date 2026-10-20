#include "CramionFX/vk/VulkanCompat.h"
#include "CramionFX/vk/RayTracing.h"

#include "CramionFX/asset/Model.h"
#include "CramionFX/vk/SkinnedModel.h"
#include "CramionFX/vk/VulkanBuffer.h"
#include "CramionFX/vk/VulkanDevice.h"
#include "CramionFX/vk/VulkanShader.h"
#include "CramionFX/vk/VulkanTexture.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <unordered_map>

namespace cramion::gfx {
namespace {

// Deben coincidir con rt_common.glsl (std430).
using RtVertex = RayTracing::Vertex;
static_assert(sizeof(RtVertex) == 32, "RtVertex debe coincidir con rt_common.glsl");

struct RtMaterial {
    core::Vec4 base_color{};
    core::Vec4 emissive{};
    core::Vec4 params{};
    std::uint32_t albedo_texture = 0;
    std::uint32_t metallic_roughness_texture = 0;
    std::uint32_t emissive_texture = 0;
    std::uint32_t flags = 0;
    // Tiling de las UV: xy = repeticion, zw = desplazamiento.
    core::Vec4 uv_transform{1.0f, 1.0f, 0.0f, 0.0f};
};
static_assert(sizeof(RtMaterial) == 80, "RtMaterial debe coincidir con rt_common.glsl");

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

// `data_size`: cuantos bytes de `data` se copian (VK_WHOLE_SIZE = `size`; los
// buffers de la escena se crean con hueco al final para lo que entre despues).
DeviceBuffer createBuffer(const VulkanDevice& device, vk::DeviceSize size,
                          vk::BufferUsageFlags usage, bool host_visible,
                          const void* data = nullptr, vk::DeviceSize data_size = VK_WHOLE_SIZE) {
    DeviceBuffer result;
    result.size = std::max<vk::DeviceSize>(size, 16);
    if (data_size == VK_WHOLE_SIZE) data_size = size;
    size = std::min(data_size, size);  // (desde aqui, lo que se copia)

    vk::BufferCreateInfo buffer_info{};
    buffer_info.size = result.size;
    // (Origen de copias: al crecer, lo de antes pasa al buffer nuevo en la GPU.)
    buffer_info.usage = usage | vk::BufferUsageFlagBits::eShaderDeviceAddress |
                        (host_visible ? vk::BufferUsageFlags{}
                                      : vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eTransferSrc);
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

void memoryBarrier(const vk::raii::CommandBuffer& cmd, vk::PipelineStageFlags2 src_stage,
                   vk::AccessFlags2 src_access, vk::PipelineStageFlags2 dst_stage, vk::AccessFlags2 dst_access) {
    vk::MemoryBarrier2 barrier{};
    barrier.srcStageMask = src_stage;
    barrier.srcAccessMask = src_access;
    barrier.dstStageMask = dst_stage;
    barrier.dstAccessMask = dst_access;
    vk::DependencyInfo dependency{};
    dependency.setMemoryBarriers(barrier);
    compat::pipelineBarrier(cmd, dependency);
}

void imageBarrier(const vk::raii::CommandBuffer& cmd, vk::Image image, std::uint32_t mips, vk::ImageLayout from,
                  vk::ImageLayout to, vk::PipelineStageFlags2 src_stage, vk::AccessFlags2 src_access,
                  vk::PipelineStageFlags2 dst_stage, vk::AccessFlags2 dst_access) {
    vk::ImageMemoryBarrier2 barrier{};
    barrier.srcStageMask = src_stage;
    barrier.srcAccessMask = src_access;
    barrier.dstStageMask = dst_stage;
    barrier.dstAccessMask = dst_access;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.image = image;
    barrier.subresourceRange = vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, mips, 0, 1};
    vk::DependencyInfo dependency{};
    dependency.setImageMemoryBarriers(barrier);
    compat::pipelineBarrier(cmd, dependency);
}

// Etapas que leen la escena de rayos (compute con ray queries y, con SER,
// el pipeline de rayos).
constexpr vk::PipelineStageFlags2 kRayStages =
    vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eRayTracingShaderKHR;

}  // namespace

// Estructura de aceleracion con su almacenamiento.
struct AccelerationStructure {
    DeviceBuffer storage;
    vk::raii::AccelerationStructureKHR handle{nullptr};
    vk::DeviceAddress address = 0;
};

struct RayTracing::Resources {
    const VulkanDevice* device = nullptr;  // para el staging de las subidas por frame

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
    // Modelo cuya BLAS usa cada uno (el mismo, el primero de su grupo de
    // geometria compartida o UINT32_MAX si no tiene).
    std::vector<std::uint32_t> blas_owner;
    std::uint32_t texture_count = 0;
    vk::DeviceSize scratch_alignment = 0;

    // --- TLAS: se reconstruye en la GPU dentro del frame (recordUpdates) ---
    // Una sola: las barreras al principio de cada frame esperan a que los
    // frames anteriores dejen de leerla. Las instancias van en un buffer por
    // frame en vuelo (la CPU escribe el de este frame mientras la GPU puede
    // estar leyendo el del anterior).
    AccelerationStructure tlas;
    std::uint32_t tlas_capacity = 0;
    // La mayor que se llego a crear: sobrevive a releaseScene. Rehacer la
    // escena empezaba otra vez en 256 y crecia x1,5 en los frames siguientes
    // (cada paso, un waitIdle: 4-6 seguidos tras cada reconstruccion con
    // Ashbound). Ahora se crea una vez con lo que ya hizo falta.
    std::uint32_t tlas_capacity_hint = 0;
    DeviceBuffer tlas_scratch;
    std::array<DeviceBuffer, kMaxFramesInFlight> instance_buffers;
    std::vector<vk::AccelerationStructureInstanceKHR> gpu_instances;
    bool tlas_dirty = false;
    bool tlas_built = false;

    // --- Mallas extra (terreno, arboles) ---
    struct ExtraModel {
        std::uint32_t model = 0;  // indice global (despues de los modelos)
        std::uint32_t vertex_base = 0;
        std::uint32_t vertex_count = 0;
        bool updatable = false;
        // Para rehacer su BLAS en la GPU (las que se pueden reescribir).
        std::vector<vk::AccelerationStructureGeometryKHR> geometries;
        std::vector<vk::AccelerationStructureBuildRangeInfoKHR> ranges;
        DeviceBuffer scratch;
    };
    std::vector<ExtraModel> extra_models;
    // Textura de una malla extra: propia (memoria + imagen + vista) o solo la
    // vista de una capa de la imagen de otro pase.
    struct OwnedTexture {
        vk::raii::DeviceMemory memory{nullptr};
        vk::raii::Image image{nullptr};
        vk::raii::ImageView view{nullptr};
        std::uint32_t size = 0;
        std::uint32_t mips = 0;
    };
    std::vector<OwnedTexture> extra_textures;
    OwnedTexture white;  // 1x1 blanca: metal/rugosidad y emision de las mallas extra
    struct PendingVertices {
        std::uint32_t extra = 0;
        std::vector<RtVertex> vertices;
    };
    struct PendingTexture {
        std::uint32_t texture = 0;
        std::vector<std::vector<std::uint8_t>> mips;
    };
    std::vector<PendingVertices> pending_vertices;
    std::vector<PendingTexture> pending_textures;
    // Staging de las subidas de cada frame en vuelo (se libera cuando vuelve
    // a tocar ese hueco: su frame ya termino).
    std::array<std::vector<DeviceBuffer>, kMaxFramesInFlight> staging;
    std::uint32_t model_count = 0;

    // --- Escena incremental (addModel) ---
    // Filas de rt_models reservadas para modelos: los que no estaban al
    // construirla entran en su fila sin rehacerla; las mallas extra van detras.
    std::uint32_t model_slots = 0;
    // Lo ocupado de cada buffer (los buffers tienen hueco al final): lo que
    // entra se escribe detras y, si no cabe, el buffer crece en la GPU.
    std::uint32_t vertex_used = 0;
    std::uint32_t triangle_used = 0;
    std::uint32_t material_used = 0;
    std::vector<std::uint32_t> model_info;  // copia en la CPU de rt_models (uvec4 por fila)
    std::vector<vk::DescriptorImageInfo> texture_infos;  // binding 7, en orden
    std::unordered_map<VkImageView, std::uint32_t> texture_slots;
    std::uint32_t white_index = 0;
    // Geometria ya subida (clave de build): {modelo con la BLAS, primer material}.
    std::unordered_map<std::string, std::pair<std::uint32_t, std::uint32_t>> shared_geometry;
    vk::Buffer irradiance{};
    struct PendingModel {
        std::uint32_t model = 0;
        bool owns_blas = false;
        std::uint32_t vertex_offset = 0;
        std::vector<RtVertex> vertices;
        std::uint32_t triangle_offset = 0;
        std::vector<std::uint32_t> indices;
        std::vector<std::uint32_t> triangle_materials;
        std::uint32_t material_offset = 0;
        std::vector<RtMaterial> materials;
        struct Range {
            std::uint32_t first_triangle = 0;
            std::uint32_t triangle_count = 0;
            bool opaque = true;
        };
        std::vector<Range> ranges;
    };
    std::vector<PendingModel> pending_models;
    // Los sets de la escena: uno por hueco de frame. Lo que cambia sin parar
    // la GPU (TLAS nueva, buffers que crecen, texturas nuevas) sube
    // scene_version; cada set se reescribe antes de su siguiente uso, cuando su
    // frame anterior ya termino (refreshSceneSet).
    std::uint64_t scene_version = 1;
    std::array<std::uint64_t, kMaxFramesInFlight> set_version{};
    std::array<std::uint32_t, kMaxFramesInFlight> set_textures{};  // texturas ya escritas en cada set

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
    // Sondas dinamicas (rt_probes.comp): sets frame + escena + el del renderizador.
    vk::raii::PipelineLayout probe_pipeline_layout{nullptr};
    vk::raii::Pipeline probe_pipeline{nullptr};
    VkDescriptorSetLayout probe_set_layout = VK_NULL_HANDLE;
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

    // Texturas que caben en el set de la escena (array fijo, partially bound).
    std::uint32_t max_textures = 0;

    // Layout del set de la escena (fijo: no depende del numero de texturas).
    void ensureLayouts(const VulkanDevice& device) {
        if (*scene_layout != nullptr) return;
        using Type = vk::DescriptorType;
        std::array<vk::DescriptorSetLayoutBinding, 11> scene_bindings{};
        std::array<vk::DescriptorBindingFlags, 11> binding_flags{};
        for (std::uint32_t i = 0; i < scene_bindings.size(); ++i) {
            scene_bindings[i].binding = i;
            scene_bindings[i].descriptorCount = 1;
            scene_bindings[i].stageFlags = stages;
            scene_bindings[i].descriptorType = Type::eStorageBuffer;
        }
        scene_bindings[0].descriptorType = Type::eAccelerationStructureKHR;
        scene_bindings[7].descriptorType = Type::eCombinedImageSampler;
        scene_bindings[7].descriptorCount = max_textures;
        binding_flags[7] = vk::DescriptorBindingFlagBits::ePartiallyBound;
        vk::DescriptorSetLayoutBindingFlagsCreateInfo flags_info{};
        flags_info.setBindingFlags(binding_flags);
        vk::DescriptorSetLayoutCreateInfo scene_layout_info{};
        scene_layout_info.pNext = &flags_info;
        scene_layout_info.setBindings(scene_bindings);
        scene_layout = vk::raii::DescriptorSetLayout(device.handle(), scene_layout_info);
    }

    // Los pipelines de rayos: se compilan la primera vez que se construye una
    // escena y se reutilizan en las siguientes (el layout ya no cambia).
    void ensurePipelines(const VulkanDevice& device) {
        if (*pipelines[0] != nullptr) return;
        const auto started = std::chrono::steady_clock::now();
        const std::array<vk::DescriptorSetLayout, 2> set_layouts = {*frame_layout, *scene_layout};
        const vk::PushConstantRange push_range{vk::ShaderStageFlagBits::eCompute, 0, sizeof(Push)};
        vk::PipelineLayoutCreateInfo layout_info{};
        layout_info.setSetLayouts(set_layouts);
        layout_info.setPushConstantRanges(push_range);
        pipeline_layout = vk::raii::PipelineLayout(device.handle(), layout_info);

        const std::array<const char*, kPassCount> shaders = {"rt_gi.comp.spv", "rt_reflections.comp.spv",
                                                             "rt_cache_resolve.comp.spv", "path_trace.comp.spv",
                                                             "rt_shadows.comp.spv"};
        for (std::size_t i = 0; i < shaders.size(); ++i) {
            const vk::raii::ShaderModule module = shaders::loadModule(device, shaders[i]);
            vk::ComputePipelineCreateInfo pipeline_info{};
            pipeline_info.stage.stage = vk::ShaderStageFlagBits::eCompute;
            pipeline_info.stage.module = *module;
            pipeline_info.stage.pName = "main";
            pipeline_info.layout = *pipeline_layout;
            pipelines[i] = vk::raii::Pipeline(device.handle(), device.pipelineCache(), pipeline_info);
        }
        // Path tracing con SER: opcional (CRAMION_SER=1). Medido en una RTX
        // 4060 Ti (pueblo de la plantilla, 8 rebotes): compute con ray queries
        // 0.31 ms por muestra, pipeline de rayos 0.37, pipeline + SER 0.48. El
        // sombreado del motor es ligero (pocas texturas por impacto): reordenar
        // cuesta mas de lo que ahorra. Queda para materiales pesados.
        ser_pipeline = nullptr;
        const char* ser = std::getenv("CRAMION_SER");
        if (device.invocationReorderSupported() && ser != nullptr && std::string_view(ser) != "0") {
            createSerPipeline(device);
        }
        std::cout << "[Vulkan] Pipelines de rayos compilados en "
                  << std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - started).count()
                  << " ms\n";
    }

    // --- Cache de radiancia en el mundo (rt_common.glsl) ---
    DeviceBuffer cache_keys;      // uint por entrada
    DeviceBuffer cache_accum;     // CacheEntry (32 bytes)
    DeviceBuffer cache_resolved;  // vec4
    bool cache_reset = true;

    // Lo que hace falta para construir la BLAS de un modelo.
    struct BlasBuild {
        std::uint32_t model = 0;
        std::vector<vk::AccelerationStructureGeometryKHR> geometries;
        std::vector<vk::AccelerationStructureBuildRangeInfoKHR> ranges;
        // Enlaces de las geometrias recortadas con el micromapa (deben vivir
        // hasta construir la BLAS; los punteros de `geometries` apuntan aqui).
        std::vector<vk::AccelerationStructureTrianglesOpacityMicromapEXT> omm_links;
        std::vector<vk::MicromapUsageEXT> omm_usages;
        bool compact = true;  // las que se reescriben no se compactan
        vk::AccelerationStructureBuildSizesInfoKHR sizes{};
    };

    // Construye todas las BLAS en un solo envio (antes, uno por modelo:
    // muchos modelos tardaban segundos) y despues las compacta (suelen quedar
    // en la mitad de memoria). Deja `blas` con una por modelo.
    void buildBottomLevels(const VulkanDevice& device, std::vector<BlasBuild>& builds) {
        const vk::raii::Device& handle = device.handle();
        vk::DeviceSize scratch_size = 0;
        std::vector<vk::AccelerationStructureBuildGeometryInfoKHR> infos(builds.size());
        for (std::size_t i = 0; i < builds.size(); ++i) {
            BlasBuild& b = builds[i];
            vk::AccelerationStructureBuildGeometryInfoKHR& info = infos[i];
            info.type = vk::AccelerationStructureTypeKHR::eBottomLevel;
            info.flags = vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace;
            if (b.compact) info.flags |= vk::BuildAccelerationStructureFlagBitsKHR::eAllowCompaction;
            info.mode = vk::BuildAccelerationStructureModeKHR::eBuild;
            info.setGeometries(b.geometries);
            std::vector<std::uint32_t> counts;
            for (const auto& range : b.ranges) counts.push_back(range.primitiveCount);
            b.sizes = handle.getAccelerationStructureBuildSizesKHR(vk::AccelerationStructureBuildTypeKHR::eDevice,
                                                                   info, counts);
            scratch_size = std::max(scratch_size, b.sizes.buildScratchSize);

            AccelerationStructure& as = blas[b.model];
            as.storage = createBuffer(device, b.sizes.accelerationStructureSize,
                                      vk::BufferUsageFlagBits::eAccelerationStructureStorageKHR, false);
            vk::AccelerationStructureCreateInfoKHR create_info{};
            create_info.buffer = *as.storage.buffer;
            create_info.size = b.sizes.accelerationStructureSize;
            create_info.type = vk::AccelerationStructureTypeKHR::eBottomLevel;
            as.handle = vk::raii::AccelerationStructureKHR(handle, create_info);
            info.dstAccelerationStructure = *as.handle;
        }
        if (builds.empty()) return;

        std::vector<vk::AccelerationStructureKHR> compactable;
        std::vector<std::uint32_t> compactable_models;
        for (const BlasBuild& b : builds) {
            if (!b.compact) continue;
            compactable.push_back(*blas[b.model].handle);
            compactable_models.push_back(b.model);
        }
        vk::raii::QueryPool queries{nullptr};
        if (!compactable.empty()) {
            vk::QueryPoolCreateInfo query_info{};
            query_info.queryType = vk::QueryType::eAccelerationStructureCompactedSizeKHR;
            query_info.queryCount = static_cast<std::uint32_t>(compactable.size());
            queries = vk::raii::QueryPool(handle, query_info);
        }

        const DeviceBuffer scratch =
            createBuffer(device, scratch_size + scratch_alignment, vk::BufferUsageFlagBits::eStorageBuffer, false);
        const vk::DeviceAddress scratch_address = alignUp(scratch.address, scratch_alignment);
        device.submitOneTime([&](const vk::raii::CommandBuffer& cmd) {
            if (!compactable.empty()) cmd.resetQueryPool(*queries, 0, static_cast<std::uint32_t>(compactable.size()));
            for (std::size_t i = 0; i < builds.size(); ++i) {
                infos[i].scratchData.deviceAddress = scratch_address;
                const vk::AccelerationStructureBuildRangeInfoKHR* ranges = builds[i].ranges.data();
                cmd.buildAccelerationStructuresKHR(infos[i], ranges);
                // El mismo scratch para la siguiente (y la consulta de tamano).
                memoryBarrier(cmd, vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR,
                              vk::AccessFlagBits2::eAccelerationStructureWriteKHR,
                              vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR,
                              vk::AccessFlagBits2::eAccelerationStructureReadKHR |
                                  vk::AccessFlagBits2::eAccelerationStructureWriteKHR);
            }
            if (!compactable.empty()) {
                cmd.writeAccelerationStructuresPropertiesKHR(
                    compactable, vk::QueryType::eAccelerationStructureCompactedSizeKHR, *queries, 0);
            }
        });
        if (compactable.empty()) return;

        // --- Compactacion: una copia de cada una en su tamano justo ---
        const auto count = static_cast<std::uint32_t>(compactable.size());
        const auto [result, sizes] = queries.getResults<vk::DeviceSize>(
            0, count, sizeof(vk::DeviceSize) * count, sizeof(vk::DeviceSize),
            vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWait);
        if (result != vk::Result::eSuccess) return;  // sin compactar: siguen valiendo
        std::vector<AccelerationStructure> compacted(count);
        vk::DeviceSize before = 0;
        vk::DeviceSize after = 0;
        for (std::uint32_t i = 0; i < count; ++i) {
            const vk::DeviceSize size = std::max<vk::DeviceSize>(sizes[i], 256);
            before += blas[compactable_models[i]].storage.size;
            after += size;
            AccelerationStructure& as = compacted[i];
            as.storage = createBuffer(device, size, vk::BufferUsageFlagBits::eAccelerationStructureStorageKHR, false);
            vk::AccelerationStructureCreateInfoKHR create_info{};
            create_info.buffer = *as.storage.buffer;
            create_info.size = size;
            create_info.type = vk::AccelerationStructureTypeKHR::eBottomLevel;
            as.handle = vk::raii::AccelerationStructureKHR(handle, create_info);
        }
        device.submitOneTime([&](const vk::raii::CommandBuffer& cmd) {
            for (std::uint32_t i = 0; i < count; ++i) {
                vk::CopyAccelerationStructureInfoKHR copy{};
                copy.src = compactable[i];
                copy.dst = *compacted[i].handle;
                copy.mode = vk::CopyAccelerationStructureModeKHR::eCompact;
                cmd.copyAccelerationStructureKHR(copy);
            }
        });
        for (std::uint32_t i = 0; i < count; ++i) blas[compactable_models[i]] = std::move(compacted[i]);
        std::cout << "[Vulkan] BLAS compactadas: " << (before >> 20) << " MB -> " << (after >> 20) << " MB\n";
    }

    // Direcciones de todas las BLAS (despues de construirlas o compactarlas).
    void resolveAddresses(const VulkanDevice& device) {
        for (AccelerationStructure& as : blas) {
            if (*as.handle == nullptr) continue;
            as.address = device.handle().getAccelerationStructureAddressKHR(
                vk::AccelerationStructureDeviceAddressInfoKHR{*as.handle});
        }
    }

    // TLAS para `capacity` instancias (la escena del set 1, binding 0).
    void createTopLevel(const VulkanDevice& device, std::uint32_t capacity) {
        tlas = AccelerationStructure{};
        tlas_scratch = DeviceBuffer{};
        for (DeviceBuffer& buffer : instance_buffers) {
            buffer = createBuffer(device, sizeof(vk::AccelerationStructureInstanceKHR) * capacity,
                                  vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR, true);
        }
        vk::AccelerationStructureGeometryInstancesDataKHR instances_data{};
        instances_data.arrayOfPointers = VK_FALSE;
        instances_data.data.deviceAddress = instance_buffers[0].address;
        vk::AccelerationStructureGeometryKHR geometry{};
        geometry.geometryType = vk::GeometryTypeKHR::eInstances;
        geometry.geometry.instances = instances_data;
        vk::AccelerationStructureBuildGeometryInfoKHR info{};
        info.type = vk::AccelerationStructureTypeKHR::eTopLevel;
        info.flags = vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace;
        info.mode = vk::BuildAccelerationStructureModeKHR::eBuild;
        info.setGeometries(geometry);
        const vk::AccelerationStructureBuildSizesInfoKHR sizes =
            device.handle().getAccelerationStructureBuildSizesKHR(vk::AccelerationStructureBuildTypeKHR::eDevice,
                                                                  info, capacity);
        tlas.storage = createBuffer(device, sizes.accelerationStructureSize,
                                    vk::BufferUsageFlagBits::eAccelerationStructureStorageKHR, false);
        vk::AccelerationStructureCreateInfoKHR create_info{};
        create_info.buffer = *tlas.storage.buffer;
        create_info.size = sizes.accelerationStructureSize;
        create_info.type = vk::AccelerationStructureTypeKHR::eTopLevel;
        tlas.handle = vk::raii::AccelerationStructureKHR(device.handle(), create_info);
        tlas.address = device.handle().getAccelerationStructureAddressKHR(
            vk::AccelerationStructureDeviceAddressInfoKHR{*tlas.handle});
        tlas_scratch = createBuffer(device, sizes.buildScratchSize + scratch_alignment,
                                    vk::BufferUsageFlagBits::eStorageBuffer, false);
        tlas_capacity = capacity;

        // El descriptor (binding 0) lo pone cada set antes de su siguiente uso:
        // los frames en vuelo siguen con la vieja (retireTopLevel).
        ++scene_version;
        tlas_dirty = true;
        tlas_built = false;
    }

    // La TLAS de antes y sus buffers, a la cola de retirada: los sueltan los
    // frames en vuelo que la usan (antes, un waitIdle cada vez que crecia).
    struct RetiredTopLevel {
        AccelerationStructure tlas;
        DeviceBuffer scratch;
        std::array<DeviceBuffer, kMaxFramesInFlight> instances;
    };
    void retireTopLevel() {
        if (*tlas.handle == nullptr) return;
        RetiredTopLevel old{std::move(tlas), std::move(tlas_scratch), std::move(instance_buffers)};
        tlas = AccelerationStructure{};
        tlas_scratch = DeviceBuffer{};
        for (DeviceBuffer& buffer : instance_buffers) buffer = DeviceBuffer{};
        device->retireAfterGpu({VulkanDevice::keepAlive(std::move(old))});
    }

    // Escribe en el set del hueco `slot` lo que cambio desde su ultimo uso
    // (todo si cambio la version; si no, solo las texturas nuevas). Su frame
    // anterior ya termino (la fence del hueco): se puede reescribir.
    void refreshSceneSet(std::uint32_t slot) {
        if (slot >= scene_sets.size()) return;
        const bool full = set_version[slot] != scene_version;
        const auto textures = static_cast<std::uint32_t>(texture_infos.size());
        if (!full && set_textures[slot] >= textures) return;
        using Type = vk::DescriptorType;
        const vk::DescriptorSet set = *scene_sets[slot];
        std::vector<vk::WriteDescriptorSet> writes;
        const std::array<vk::DescriptorBufferInfo, 9> buffers = {
            vk::DescriptorBufferInfo{*vertices.buffer, 0, VK_WHOLE_SIZE},
            vk::DescriptorBufferInfo{*indices.buffer, 0, VK_WHOLE_SIZE},
            vk::DescriptorBufferInfo{*triangle_materials.buffer, 0, VK_WHOLE_SIZE},
            vk::DescriptorBufferInfo{*materials.buffer, 0, VK_WHOLE_SIZE},
            vk::DescriptorBufferInfo{*models.buffer, 0, VK_WHOLE_SIZE},
            vk::DescriptorBufferInfo{irradiance, 0, VK_WHOLE_SIZE},
            vk::DescriptorBufferInfo{*cache_keys.buffer, 0, VK_WHOLE_SIZE},
            vk::DescriptorBufferInfo{*cache_accum.buffer, 0, VK_WHOLE_SIZE},
            vk::DescriptorBufferInfo{*cache_resolved.buffer, 0, VK_WHOLE_SIZE}};
        const vk::AccelerationStructureKHR tlas_handle = *tlas.handle;
        vk::WriteDescriptorSetAccelerationStructureKHR tlas_info{};
        tlas_info.accelerationStructureCount = 1;
        tlas_info.pAccelerationStructures = &tlas_handle;
        if (full) {
            // 1-6: escena e irradiancia; 8-10: la cache de radiancia.
            for (std::uint32_t i = 0; i < buffers.size(); ++i) {
                vk::WriteDescriptorSet write{};
                write.dstSet = set;
                write.dstBinding = i < 6 ? 1 + i : 2 + i;
                write.descriptorCount = 1;
                write.descriptorType = Type::eStorageBuffer;
                write.pBufferInfo = &buffers[i];
                writes.push_back(write);
            }
            if (tlas_handle) {
                vk::WriteDescriptorSet write{};
                write.pNext = &tlas_info;
                write.dstSet = set;
                write.dstBinding = 0;
                write.descriptorCount = 1;
                write.descriptorType = Type::eAccelerationStructureKHR;
                writes.push_back(write);
            }
        }
        if (set_textures[slot] < textures) {
            vk::WriteDescriptorSet write{};
            write.dstSet = set;
            write.dstBinding = 7;
            write.dstArrayElement = set_textures[slot];
            write.descriptorCount = textures - set_textures[slot];
            write.descriptorType = Type::eCombinedImageSampler;
            write.pImageInfo = texture_infos.data() + set_textures[slot];
            writes.push_back(write);
        }
        device->handle().updateDescriptorSets(writes, nullptr);
        set_version[slot] = scene_version;
        set_textures[slot] = textures;
    }

    // Que quepan `needed` bytes: si no, un buffer mayor (x1,5) con lo de antes
    // copiado en la GPU en este command buffer. El viejo va al staging de este
    // hueco (lo sueltan los frames en vuelo). Las mallas extra que se
    // reescriben guardan las direcciones de sus vertices e indices: se corrigen.
    bool growBuffer(const vk::raii::CommandBuffer& cmd, std::uint32_t slot, DeviceBuffer& buffer,
                    vk::DeviceSize needed, vk::BufferUsageFlags usage) {
        if (needed <= buffer.size) return false;
        DeviceBuffer grown = createBuffer(*device, needed + needed / 2, usage, false);
        cmd.copyBuffer(*buffer.buffer, *grown.buffer, vk::BufferCopy{0, 0, buffer.size});
        const vk::DeviceAddress delta = grown.address - buffer.address;  // (sin signo: al sumarlo vuelve al sitio)
        const bool vertex_buffer = &buffer == &vertices;
        const bool index_buffer = &buffer == &indices;
        for (ExtraModel& e : extra_models) {
            for (vk::AccelerationStructureGeometryKHR& geometry : e.geometries) {
                if (vertex_buffer) geometry.geometry.triangles.vertexData.deviceAddress += delta;
                if (index_buffer) geometry.geometry.triangles.indexData.deviceAddress += delta;
            }
        }
        staging[slot].push_back(std::move(buffer));
        buffer = std::move(grown);
        ++scene_version;  // los sets, al buffer nuevo
        return true;
    }

    // Los modelos que entraron con addModel: sus datos al final de los buffers
    // (que crecen si hace falta) y sus BLAS, en el command buffer del frame. Sin
    // compactar ni micromapa de opacidad (el any-hit prueba el alfa): hasta la
    // siguiente reconstruccion entera ocupan algo mas y trazan algo mas lento.
    void flushPendingModels(const vk::raii::CommandBuffer& cmd, std::uint32_t slot) {
        const VulkanDevice& gpu = *device;
        const auto input = vk::BufferUsageFlagBits::eStorageBuffer |
                           vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR;
        // Lo que los frames anteriores aun leen o escriben de estos buffers.
        memoryBarrier(cmd,
                      kRayStages | vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR |
                          vk::PipelineStageFlagBits2::eTransfer,
                      vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eAccelerationStructureReadKHR |
                          vk::AccessFlagBits2::eTransferWrite,
                      vk::PipelineStageFlagBits2::eTransfer,
                      vk::AccessFlagBits2::eTransferRead | vk::AccessFlagBits2::eTransferWrite);
        // (Las cuatro, sin cortocircuito.)
        const bool grew_vertices =
            growBuffer(cmd, slot, vertices, sizeof(RtVertex) * static_cast<vk::DeviceSize>(vertex_used), input);
        const bool grew_indices = growBuffer(cmd, slot, indices, 12ull * triangle_used, input);
        const bool grew_triangles =
            growBuffer(cmd, slot, triangle_materials, 4ull * triangle_used, vk::BufferUsageFlagBits::eStorageBuffer);
        const bool grew_materials = growBuffer(cmd, slot, materials,
                                               sizeof(RtMaterial) * static_cast<vk::DeviceSize>(material_used),
                                               vk::BufferUsageFlagBits::eStorageBuffer);
        if (grew_vertices || grew_indices || grew_triangles || grew_materials) {
            // La copia de lo de antes pisa el hueco donde va lo nuevo: primero ella.
            memoryBarrier(cmd, vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite,
                          vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite);
        }

        // --- Los datos, en un solo staging ---
        struct Copy {
            vk::Buffer target;
            vk::DeviceSize offset = 0;
            vk::DeviceSize source = 0;
            vk::DeviceSize bytes = 0;
        };
        std::vector<std::uint8_t> packed;
        std::vector<Copy> copies;
        const auto add = [&](const DeviceBuffer& target, vk::DeviceSize offset, const void* data, std::size_t bytes) {
            if (bytes == 0) return;
            copies.push_back(Copy{*target.buffer, offset, packed.size(), bytes});
            const auto* begin = static_cast<const std::uint8_t*>(data);
            packed.insert(packed.end(), begin, begin + bytes);
        };
        for (const PendingModel& p : pending_models) {
            add(vertices, sizeof(RtVertex) * static_cast<vk::DeviceSize>(p.vertex_offset), p.vertices.data(),
                sizeof(RtVertex) * p.vertices.size());
            add(indices, 12ull * p.triangle_offset, p.indices.data(), sizeof(std::uint32_t) * p.indices.size());
            add(triangle_materials, 4ull * p.triangle_offset, p.triangle_materials.data(),
                sizeof(std::uint32_t) * p.triangle_materials.size());
            add(materials, sizeof(RtMaterial) * static_cast<vk::DeviceSize>(p.material_offset), p.materials.data(),
                sizeof(RtMaterial) * p.materials.size());
            add(models, 16ull * p.model, model_info.data() + static_cast<std::size_t>(p.model) * 4, 16);
        }
        if (!copies.empty()) {
            DeviceBuffer upload =
                createBuffer(gpu, packed.size(), vk::BufferUsageFlagBits::eTransferSrc, true, packed.data());
            for (const Copy& copy : copies) {
                cmd.copyBuffer(*upload.buffer, copy.target, vk::BufferCopy{copy.source, copy.offset, copy.bytes});
            }
            staging[slot].push_back(std::move(upload));
        }
        // (Tambien antes de las copias del terreno que vienen detras: van al
        // mismo buffer que la copia de cuando crece.)
        memoryBarrier(cmd, vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite,
                      vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR | kRayStages |
                          vk::PipelineStageFlagBits2::eTransfer,
                      vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eAccelerationStructureReadKHR |
                          vk::AccessFlagBits2::eTransferWrite);

        // --- Sus BLAS (una por geometria nueva; las variantes usan la de su grupo) ---
        struct Build {
            std::uint32_t model = 0;
            std::vector<vk::AccelerationStructureGeometryKHR> geometries;
            std::vector<vk::AccelerationStructureBuildRangeInfoKHR> ranges;
            vk::AccelerationStructureBuildGeometryInfoKHR info{};
        };
        std::vector<Build> builds;
        vk::DeviceSize scratch_size = 0;
        for (const PendingModel& p : pending_models) {
            if (!p.owns_blas || p.ranges.empty() || p.model >= blas.size() || *blas[p.model].handle != nullptr) continue;
            Build& b = builds.emplace_back();
            b.model = p.model;
            std::vector<std::uint32_t> counts;
            for (const PendingModel::Range& range : p.ranges) {
                vk::AccelerationStructureGeometryTrianglesDataKHR triangles{};
                triangles.vertexFormat = vk::Format::eR32G32B32Sfloat;
                triangles.vertexData.deviceAddress = vertices.address;
                triangles.vertexStride = sizeof(RtVertex);
                triangles.maxVertex = vertex_used > 0 ? vertex_used - 1 : 0;
                triangles.indexType = vk::IndexType::eUint32;
                triangles.indexData.deviceAddress =
                    indices.address + static_cast<vk::DeviceAddress>(range.first_triangle) * 12;
                vk::AccelerationStructureGeometryKHR geometry{};
                geometry.geometryType = vk::GeometryTypeKHR::eTriangles;
                geometry.geometry.triangles = triangles;
                geometry.flags = range.opaque ? vk::GeometryFlagBitsKHR::eOpaque : vk::GeometryFlagsKHR{};
                b.geometries.push_back(geometry);
                vk::AccelerationStructureBuildRangeInfoKHR build_range{};
                build_range.primitiveCount = range.triangle_count;
                b.ranges.push_back(build_range);
                counts.push_back(range.triangle_count);
            }
            b.info.type = vk::AccelerationStructureTypeKHR::eBottomLevel;
            b.info.flags = vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace;
            b.info.mode = vk::BuildAccelerationStructureModeKHR::eBuild;
            b.info.setGeometries(b.geometries);
            const vk::AccelerationStructureBuildSizesInfoKHR sizes = gpu.handle().getAccelerationStructureBuildSizesKHR(
                vk::AccelerationStructureBuildTypeKHR::eDevice, b.info, counts);
            scratch_size = std::max(scratch_size, sizes.buildScratchSize);
            AccelerationStructure& as = blas[p.model];
            as.storage = createBuffer(gpu, sizes.accelerationStructureSize,
                                      vk::BufferUsageFlagBits::eAccelerationStructureStorageKHR, false);
            vk::AccelerationStructureCreateInfoKHR create_info{};
            create_info.buffer = *as.storage.buffer;
            create_info.size = sizes.accelerationStructureSize;
            create_info.type = vk::AccelerationStructureTypeKHR::eBottomLevel;
            as.handle = vk::raii::AccelerationStructureKHR(gpu.handle(), create_info);
            b.info.dstAccelerationStructure = *as.handle;
        }
        if (!builds.empty()) {
            // Un scratch para todas, una detras de otra (como buildBottomLevels).
            DeviceBuffer scratch =
                createBuffer(gpu, scratch_size + scratch_alignment, vk::BufferUsageFlagBits::eStorageBuffer, false);
            const vk::DeviceAddress scratch_address = alignUp(scratch.address, scratch_alignment);
            for (Build& b : builds) {
                b.info.scratchData.deviceAddress = scratch_address;
                const vk::AccelerationStructureBuildRangeInfoKHR* ranges = b.ranges.data();
                cmd.buildAccelerationStructuresKHR(b.info, ranges);
                memoryBarrier(cmd, vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR,
                              vk::AccessFlagBits2::eAccelerationStructureWriteKHR,
                              vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR | kRayStages,
                              vk::AccessFlagBits2::eAccelerationStructureReadKHR |
                                  vk::AccessFlagBits2::eAccelerationStructureWriteKHR);
            }
            staging[slot].push_back(std::move(scratch));
            // Con direccion, las instancias las usan desde el siguiente
            // setInstances (la TLAS de este frame ya se eligio sin ellas).
            for (const Build& b : builds) {
                AccelerationStructure& as = blas[b.model];
                as.address = gpu.handle().getAccelerationStructureAddressKHR(
                    vk::AccelerationStructureDeviceAddressInfoKHR{*as.handle});
            }
        }
        pending_models.clear();
    }

    // Imagen RGBA8 UNORM (color sRGB, como las de los modelos) con sus mips.
    OwnedTexture createOwnedTexture(const VulkanDevice& device, std::uint32_t size,
                                    const std::vector<std::vector<std::uint8_t>>& mips) {
        OwnedTexture t;
        t.size = size;
        t.mips = static_cast<std::uint32_t>(mips.size());
        vk::ImageCreateInfo info{};
        info.imageType = vk::ImageType::e2D;
        info.format = vk::Format::eR8G8B8A8Unorm;
        info.extent = vk::Extent3D{size, size, 1};
        info.mipLevels = t.mips;
        info.arrayLayers = 1;
        info.samples = vk::SampleCountFlagBits::e1;
        info.tiling = vk::ImageTiling::eOptimal;
        info.usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst;
        info.initialLayout = vk::ImageLayout::eUndefined;
        t.image = vk::raii::Image(device.handle(), info);
        const vk::MemoryRequirements requirements = t.image.getMemoryRequirements();
        vk::MemoryAllocateInfo allocate{};
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex =
            device.findMemoryType(requirements.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal);
        t.memory = vk::raii::DeviceMemory(device.handle(), allocate);
        t.image.bindMemory(*t.memory, 0);

        vk::DeviceSize total = 0;
        for (const auto& level : mips) total += level.size();
        VulkanBuffer staging;
        staging.create(device, std::max<vk::DeviceSize>(total, 16), vk::BufferUsageFlagBits::eTransferSrc,
                       vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
        std::vector<vk::BufferImageCopy> regions;
        vk::DeviceSize offset = 0;
        std::uint32_t level_size = size;
        for (std::uint32_t m = 0; m < t.mips; ++m) {
            staging.write(mips[m].data(), mips[m].size(), offset);
            vk::BufferImageCopy region{};
            region.bufferOffset = offset;
            region.imageSubresource = vk::ImageSubresourceLayers{vk::ImageAspectFlagBits::eColor, m, 0, 1};
            region.imageExtent = vk::Extent3D{level_size, level_size, 1};
            regions.push_back(region);
            offset += mips[m].size();
            level_size = std::max(level_size / 2, 1u);
        }
        const vk::Image image = *t.image;
        device.submitOneTime([&](const vk::raii::CommandBuffer& cmd) {
            imageBarrier(cmd, image, t.mips, vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
                         vk::PipelineStageFlagBits2::eTopOfPipe, vk::AccessFlagBits2::eNone,
                         vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite);
            cmd.copyBufferToImage(*staging.handle(), image, vk::ImageLayout::eTransferDstOptimal, regions);
            imageBarrier(cmd, image, t.mips, vk::ImageLayout::eTransferDstOptimal,
                         vk::ImageLayout::eShaderReadOnlyOptimal, vk::PipelineStageFlagBits2::eTransfer,
                         vk::AccessFlagBits2::eTransferWrite, kRayStages, vk::AccessFlagBits2::eShaderSampledRead);
        });
        staging.destroy();
        vk::ImageViewCreateInfo view{};
        view.image = image;
        view.viewType = vk::ImageViewType::e2D;
        view.format = vk::Format::eR8G8B8A8Unorm;
        view.subresourceRange = vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, t.mips, 0, 1};
        t.view = vk::raii::ImageView(device.handle(), view);
        return t;
    }

    // Vista 2D UNORM de una capa de la imagen de otro pase (la imagen debe
    // tener formato mutable si es SRGB). BC7 (los arboles ya comprimidos):
    // su UNORM.
    static OwnedTexture createLayerView(const VulkanDevice& device, const ExtraTexture& source) {
        OwnedTexture t;
        t.mips = source.mip_count;
        vk::ImageViewCreateInfo view{};
        view.image = source.image;
        view.viewType = vk::ImageViewType::e2D;
        view.format = source.format == vk::Format::eBc7SrgbBlock || source.format == vk::Format::eBc7UnormBlock
                          ? vk::Format::eBc7UnormBlock
                          : vk::Format::eR8G8B8A8Unorm;
        view.subresourceRange =
            vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, source.mip_count, source.layer, 1};
        t.view = vk::raii::ImageView(device.handle(), view);
        return t;
    }

    // Cache de radiancia: 2 + 16 + 8 MB, a cero (solo con la escena subida).
    void ensureCache(const VulkanDevice& device) {
        if (*cache_keys.buffer != nullptr) return;
        const auto storage = vk::BufferUsageFlagBits::eStorageBuffer;
        cache_keys = createBuffer(device, sizeof(std::uint32_t) * kCacheEntries, storage, false);
        cache_accum = createBuffer(device, 32ull * kCacheEntries, storage, false);
        cache_resolved = createBuffer(device, 16ull * kCacheEntries, storage, false);
        device.submitOneTime([&](const vk::raii::CommandBuffer& cmd) {
            cmd.fillBuffer(*cache_keys.buffer, 0, VK_WHOLE_SIZE, 0u);
            cmd.fillBuffer(*cache_accum.buffer, 0, VK_WHOLE_SIZE, 0u);
            cmd.fillBuffer(*cache_resolved.buffer, 0, VK_WHOLE_SIZE, 0u);
        });
        cache_reset = true;
    }

    // Lo de la escena que aun pueden usar los frames en vuelo, junto (el orden
    // de los miembros es el de destruccion, al reves: los sets antes que su
    // pool, la TLAS antes que las BLAS y estas antes que el micromapa).
    struct RetiredScene {
        DeviceBuffer vertices;
        DeviceBuffer indices;
        DeviceBuffer triangle_materials;
        DeviceBuffer materials;
        DeviceBuffer models;
        DeviceBuffer cache_keys;
        DeviceBuffer cache_accum;
        DeviceBuffer cache_resolved;
        DeviceBuffer micromap_storage;
        vk::raii::MicromapEXT micromap{nullptr};
        std::vector<AccelerationStructure> blas;
        std::vector<ExtraModel> extra_models;
        std::vector<OwnedTexture> extra_textures;
        OwnedTexture white;
        std::array<std::vector<DeviceBuffer>, kMaxFramesInFlight> staging;
        vk::raii::DescriptorPool scene_pool{nullptr};
        std::vector<vk::raii::DescriptorSet> scene_sets;
    };

    // Todo lo de la escena. Sin parar la GPU: lo que puedan usar los frames en
    // vuelo pasa a la cola de retirada (VulkanDevice::retireAfterGpu); antes
    // habia que llamarla con la GPU parada (un waitIdle en cada reconstruccion).
    // La cache de radiancia esta en coordenadas del mundo: al rehacer la escena
    // del mismo nivel sigue valiendo (keep_cache) y no se vacia.
    void releaseScene(bool keep_cache = false) {
        retireTopLevel();
        if (device != nullptr && (!scene_sets.empty() || *vertices.buffer != nullptr || !blas.empty() ||
                                  (!keep_cache && *cache_keys.buffer != nullptr))) {
            auto old = std::make_shared<RetiredScene>();
            old->vertices = std::move(vertices);
            old->indices = std::move(indices);
            old->triangle_materials = std::move(triangle_materials);
            old->materials = std::move(materials);
            old->models = std::move(models);
            if (!keep_cache) {
                old->cache_keys = std::move(cache_keys);
                old->cache_accum = std::move(cache_accum);
                old->cache_resolved = std::move(cache_resolved);
            }
            old->micromap_storage = std::move(micromap_storage);
            old->micromap = std::move(micromap);
            old->blas = std::move(blas);
            old->extra_models = std::move(extra_models);
            old->extra_textures = std::move(extra_textures);
            old->white = std::move(white);
            old->staging = std::move(staging);
            old->scene_pool = std::move(scene_pool);
            old->scene_sets = std::move(scene_sets);
            device->retireAfterGpu({std::shared_ptr<const void>(std::move(old))});
        }
        tlas = AccelerationStructure{};
        tlas_scratch = DeviceBuffer{};
        for (DeviceBuffer& buffer : instance_buffers) buffer = DeviceBuffer{};
        tlas_capacity = 0;
        gpu_instances.clear();
        tlas_dirty = false;
        tlas_built = false;
        // (Los pipelines y el layout de la escena se quedan: no dependen de ella.)
        scene_sets.clear();
        scene_pool = nullptr;
        blas.clear();
        blas_owner.clear();
        extra_models.clear();
        micromap = nullptr;
        micromap_storage = DeviceBuffer{};
        micromap_level = 0;
        extra_textures.clear();
        white = OwnedTexture{};
        vertices = DeviceBuffer{};
        indices = DeviceBuffer{};
        triangle_materials = DeviceBuffer{};
        materials = DeviceBuffer{};
        models = DeviceBuffer{};
        if (!keep_cache) {
            cache_keys = DeviceBuffer{};
            cache_accum = DeviceBuffer{};
            cache_resolved = DeviceBuffer{};
        }
        pending_vertices.clear();
        pending_textures.clear();
        for (auto& list : staging) list.clear();
        texture_count = 0;
        model_count = 0;
        model_slots = 0;
        vertex_used = 0;
        triangle_used = 0;
        material_used = 0;
        model_info.clear();
        texture_infos.clear();
        texture_slots.clear();
        shared_geometry.clear();
        pending_models.clear();
        ++scene_version;
        set_textures.fill(0);
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
    r.device = &device;

    const auto properties =
        device.physicalDevice()
            .getProperties2<vk::PhysicalDeviceProperties2,
                            vk::PhysicalDeviceAccelerationStructurePropertiesKHR>();
    r.scratch_alignment = properties.get<vk::PhysicalDeviceAccelerationStructurePropertiesKHR>()
                              .minAccelerationStructureScratchOffsetAlignment;
    // Texturas del set de la escena: 16384 o lo que admita la GPU (con margen
    // para las demas texturas de los shaders).
    const vk::PhysicalDeviceLimits& limits = properties.get<vk::PhysicalDeviceProperties2>().properties.limits;
    r.max_textures = std::max(64u, std::min({16384u, limits.maxPerStageDescriptorSampledImages - 32u,
                                             limits.maxDescriptorSetSampledImages - 64u,
                                             limits.maxPerStageDescriptorSamplers - 32u,
                                             limits.maxDescriptorSetSamplers - 64u}));

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
    // La escena, la cache de radiancia y la TLAS se crean en build(): con el
    // trazado apagado no ocupan memoria.
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
                       const VulkanBuffer& irradiance, const ExtraScene& extra,
                       const std::vector<bool>& used, std::uint32_t model_slots) {
    Resources& r = *resources_;
    // Filas de modelos: las de los que hay y las reservadas para los que
    // lleguen (addModel); las mallas extra, detras.
    const auto slots = std::max(model_slots, static_cast<std::uint32_t>(models.size()));
    // La cache de radiancia se queda (se rehace la escena del mismo nivel: un
    // modelo nuevo o cambiado). Vaciarla dejaba la GI y los reflejos sin luz
    // rebotada unos frames en un momento cualquiera. Solo se crea (vacia) si
    // no existia.
    r.releaseScene(/*keep_cache=*/true);
    r.ensureCache(device);

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
    // rt_models (rt_common.glsl): x, y = primer triangulo de cada geometria;
    // z = lo que se suma al material de cada triangulo (un modelo que usa los
    // triangulos de otro tiene sus materiales en otro sitio; en uint, si resta
    // da la vuelta y la suma del shader vuelve al sitio).
    const auto add_model = [&](std::vector<GeometryRange> geometries) {
        model_info.push_back(geometries.size() > 0 ? geometries[0].first_triangle : 0);
        model_info.push_back(geometries.size() > 1 ? geometries[1].first_triangle : 0);
        model_info.push_back(0);
        model_info.push_back(0);
        model_geometries.push_back(std::move(geometries));
    };
    // De que modelo es la BLAS de cada uno (UINT32_MAX: sin BLAS).
    r.blas_owner.assign(slots, UINT32_MAX);

    // Cada textura una vez: las variantes de un modelo y los de un mismo
    // pueblo comparten muchas (y las tres de por defecto todos). Antes cada
    // modelo ponia las suyas y el array se llenaba de repetidas.
    std::unordered_map<VkImageView, std::uint32_t> texture_slots;
    // Geometria compartida: las variantes de material (.crmat) usan los mismos
    // vertices e indices que su original (SkinnedModel los comparte en la GPU
    // por contenido; en la RAM, core::SharedVector). Una sola copia y una sola
    // BLAS por grupo: cada modelo pone sus materiales (rt_models[modelo].z) y
    // la instancia dice de que modelo es (instanceCustomIndex).
    struct SharedGeometry {
        std::uint32_t owner = 0;          // el modelo con los triangulos y la BLAS
        std::uint32_t material_base = 0;  // donde empiezan sus materiales
    };
    std::unordered_map<std::string, SharedGeometry> shared_geometry;
    std::unordered_map<const void*, std::uint32_t> vertex_bases;  // vertices ya copiados (por buffer)
    std::uint32_t skipped_models = 0;
    std::uint32_t shared_models = 0;
    std::string key;
    const auto append = [&key](const auto& value) {
        key.append(reinterpret_cast<const char*>(&value), sizeof(value));
    };

    for (std::size_t m = 0; m < models.size(); ++m) {
        const asset::ModelData& model = *models[m];
        const SkinnedModel& gpu = gpu_models[m];
        // Sin instancias (el original de una variante que nadie dibuja, lo
        // que no esta en la escena): ni triangulos, ni materiales, ni BLAS.
        // Si despues aparece, el renderizador rehace la escena.
        const bool wanted = used.empty() || (m < used.size() && used[m]);
        // (Ni un modelo sin nada en la GPU: el hueco vacio de una subida que
        // fallo o de un "solo base de variantes" no tiene materiales.)
        if (!wanted || model.indices.empty() || model.vertices.empty() || gpu.materials().empty()) {
            if (!wanted) ++skipped_models;
            add_model({});
            continue;
        }
        const auto material_base = static_cast<std::uint32_t>(materials.size());

        std::vector<std::uint32_t> texture_map(gpu.textures().size(), 0);
        for (std::size_t t = 0; t < gpu.textures().size(); ++t) {
            const vk::ImageView view = *gpu.textures()[t]->view();
            const auto [it, inserted] =
                texture_slots.try_emplace(static_cast<VkImageView>(view), static_cast<std::uint32_t>(textures.size()));
            if (inserted) {
                textures.push_back(
                    vk::DescriptorImageInfo{*r.texture_sampler, view, vk::ImageLayout::eShaderReadOnlyOptimal});
            }
            texture_map[t] = it->second;
        }
        const auto texture_slot = [&](std::uint32_t local) {
            return local < texture_map.size() ? texture_map[local] : 0u;
        };

        for (const SkinnedModel::Material& material : gpu.materials()) {
            RtMaterial rt{};
            rt.base_color = material.base_color;
            rt.emissive = material.emissive;
            rt.params = core::Vec4{material.params.x, material.params.y, 0.0f, 0.0f};
            rt.albedo_texture = texture_slot(material.albedo_texture);
            rt.metallic_roughness_texture = texture_slot(material.metallic_roughness_texture);
            rt.emissive_texture = texture_slot(material.emissive_texture);
            rt.flags = material.alpha_masked ? 1u : 0u;
            rt.uv_transform = material.uv_transform;
            materials.push_back(rt);
        }

        // Clave de la geometria: los buffers de la GPU (compartidos por
        // contenido) o, sin ellos, los datos de la RAM (SharedVector), las
        // submallas y que es cada una para los rayos (opaca, recortada o
        // fuera). Las recortadas, ademas, con lo que decide su alfa: el
        // micromapa de opacidad se hornea con el material del primero del grupo.
        const void* vertex_identity = gpu.vertices().isValid() ? static_cast<const void*>(&gpu.vertices())
                                                               : static_cast<const void*>(model.vertices.data());
        const void* index_identity = gpu.indices().isValid() ? static_cast<const void*>(&gpu.indices())
                                                             : static_cast<const void*>(model.indices.data());
        key.clear();
        append(vertex_identity);
        append(index_identity);
        append(model.vertices.size());
        append(model.indices.size());
        for (const asset::SubMesh& submesh : model.submeshes) {
            const SkinnedModel::Material& material = gpu.materials()[submesh.material];
            const std::uint8_t kind = material.transparent ? 0 : material.alpha_masked ? 2 : 1;
            append(submesh.first_index);
            append(submesh.index_count);
            append(submesh.material);
            append(kind);
            if (kind == 2) {
                append(texture_slot(material.albedo_texture));
                append(material.base_color.w);
                append(material.uv_transform);
            }
        }
        if (const auto group = shared_geometry.find(key); group != shared_geometry.end()) {
            const std::uint32_t owner = group->second.owner;
            const std::uint32_t first_opaque = model_info[static_cast<std::size_t>(owner) * 4];
            const std::uint32_t first_masked = model_info[static_cast<std::size_t>(owner) * 4 + 1];
            model_info.push_back(first_opaque);
            model_info.push_back(first_masked);
            model_info.push_back(material_base - group->second.material_base);
            model_info.push_back(0);
            model_geometries.emplace_back();
            r.blas_owner[m] = owner;
            ++shared_models;
            continue;
        }
        shared_geometry.emplace(key, SharedGeometry{static_cast<std::uint32_t>(m), material_base});
        r.blas_owner[m] = static_cast<std::uint32_t>(m);

        // Los vertices, una vez por buffer (otra clave con los mismos
        // vertices: una variante que cambia que submallas son recortadas).
        std::uint32_t vertex_base = 0;
        if (const auto it = vertex_bases.find(vertex_identity); it != vertex_bases.end()) {
            vertex_base = it->second;
        } else {
            vertex_base = static_cast<std::uint32_t>(vertices.size());
            vertex_bases.emplace(vertex_identity, vertex_base);
            for (const asset::SkinnedVertex& v : model.vertices) {
                vertices.push_back(RtVertex{v.position.x, v.position.y, v.position.z, v.normal.x,
                                            v.normal.y, v.normal.z, v.uv.x, v.uv.y});
            }
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
        add_model(std::move(geometries));
    }
    // Las filas reservadas, vacias (sin BLAS: sus instancias se ignoran).
    for (std::size_t m = models.size(); m < slots; ++m) add_model({});

    // --- Mallas extra: terreno y especies de arboles ---
    r.white = r.createOwnedTexture(device, 1, {{255, 255, 255, 255}});
    const auto white_index = static_cast<std::uint32_t>(textures.size());
    textures.push_back(vk::DescriptorImageInfo{*r.texture_sampler, *r.white.view,
                                               vk::ImageLayout::eShaderReadOnlyOptimal});
    const auto extra_texture_base = static_cast<std::uint32_t>(textures.size());
    for (const ExtraTexture& source : extra.textures) {
        Resources::OwnedTexture texture = source.mips.empty()
                                              ? Resources::createLayerView(device, source)
                                              : r.createOwnedTexture(device, source.size, source.mips);
        textures.push_back(vk::DescriptorImageInfo{*r.texture_sampler, *texture.view,
                                                   vk::ImageLayout::eShaderReadOnlyOptimal});
        r.extra_textures.push_back(std::move(texture));
    }
    for (std::size_t e = 0; e < extra.meshes.size(); ++e) {
        const ExtraMesh& mesh = extra.meshes[e];
        Resources::ExtraModel info;
        info.model = static_cast<std::uint32_t>(slots + e);
        info.vertex_base = static_cast<std::uint32_t>(vertices.size());
        info.vertex_count = static_cast<std::uint32_t>(mesh.vertices.size());
        info.updatable = mesh.updatable;
        std::vector<GeometryRange> geometries;
        if (!mesh.materials.empty()) {
            vertices.insert(vertices.end(), mesh.vertices.begin(), mesh.vertices.end());
            const auto material_base = static_cast<std::uint32_t>(materials.size());
            for (const ExtraMaterial& m : mesh.materials) {
                RtMaterial rt{};
                rt.base_color = m.base_color;
                rt.params = core::Vec4{0.0f, m.roughness, 0.0f, 0.0f};
                rt.albedo_texture = m.texture >= 0 && static_cast<std::size_t>(m.texture) < extra.textures.size()
                                        ? extra_texture_base + static_cast<std::uint32_t>(m.texture)
                                        : white_index;
                rt.metallic_roughness_texture = white_index;
                rt.emissive_texture = white_index;
                rt.flags = m.alpha_masked ? 1u : 0u;
                materials.push_back(rt);
            }
            const std::size_t triangles = mesh.indices.size() / 3;
            for (const bool masked : {false, true}) {
                GeometryRange range{};
                range.first_triangle = static_cast<std::uint32_t>(indices.size() / 3);
                range.opaque = !masked;
                for (std::size_t t = 0; t < triangles; ++t) {
                    std::uint32_t local = t < mesh.triangle_materials.size() ? mesh.triangle_materials[t] : 0u;
                    if (local >= mesh.materials.size()) local = 0;
                    if (mesh.materials[local].alpha_masked != masked) continue;
                    for (std::size_t k = 0; k < 3; ++k) indices.push_back(info.vertex_base + mesh.indices[t * 3 + k]);
                    triangle_materials.push_back(material_base + local);
                }
                range.triangle_count = static_cast<std::uint32_t>(indices.size() / 3) - range.first_triangle;
                if (range.triangle_count > 0) geometries.push_back(range);
            }
        }
        add_model(std::move(geometries));
        r.blas_owner.push_back(info.model);
        r.extra_models.push_back(std::move(info));
    }

    if (indices.empty()) {
        std::cout << "[Vulkan] Trazado de rayos: la escena no tiene geometria\n";
        r.releaseScene();
        return;
    }
    r.texture_count = static_cast<std::uint32_t>(textures.size());
    r.model_count = static_cast<std::uint32_t>(model_geometries.size());

    const auto input = vk::BufferUsageFlagBits::eStorageBuffer |
                       vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR;
    // Con hueco al final (un octavo y algo) para lo que entre por addModel;
    // si se llena, crece en la GPU (Resources::growBuffer).
    const auto room = [](std::size_t count, std::size_t minimum) {
        return static_cast<vk::DeviceSize>(count + std::max(count / 8, minimum));
    };
    r.vertices = createBuffer(device, sizeof(RtVertex) * room(vertices.size(), 65536), input, false,
                              vertices.data(), sizeof(RtVertex) * vertices.size());
    r.indices = createBuffer(device, sizeof(std::uint32_t) * room(indices.size(), 3 * 65536), input, false,
                             indices.data(), sizeof(std::uint32_t) * indices.size());
    r.triangle_materials =
        createBuffer(device, sizeof(std::uint32_t) * room(triangle_materials.size(), 65536),
                     vk::BufferUsageFlagBits::eStorageBuffer, false, triangle_materials.data(),
                     sizeof(std::uint32_t) * triangle_materials.size());
    r.materials = createBuffer(device, sizeof(RtMaterial) * room(materials.size(), 1024),
                               vk::BufferUsageFlagBits::eStorageBuffer, false, materials.data(),
                               sizeof(RtMaterial) * materials.size());
    r.models = createBuffer(device, sizeof(std::uint32_t) * model_info.size(),
                            vk::BufferUsageFlagBits::eStorageBuffer, false, model_info.data());

    // (Las BLAS se construyen al final: el micromapa de opacidad del follaje
    // se hornea con el set de la escena, que lleva las texturas.)
    r.blas.clear();

    // --- Set 1: la escena ---
    // El layout (y con el los pipelines) se crea una vez: las texturas van en
    // un array de tamano fijo con huecos sin escribir (partially bound). Antes
    // el layout dependia del numero de texturas y cada reconstruccion
    // recompilaba los pipelines de rayos (mas de 1 s).
    using Type = vk::DescriptorType;
    if (textures.size() > r.max_textures) {
        std::cout << "[Vulkan] Trazado de rayos: " << textures.size() << " texturas, solo caben " << r.max_textures
                  << " (las demas se ven blancas)\n";
        textures.resize(r.max_textures);
        for (RtMaterial& material : materials) {
            if (material.albedo_texture >= r.max_textures) material.albedo_texture = white_index;
            if (material.metallic_roughness_texture >= r.max_textures) material.metallic_roughness_texture = white_index;
            if (material.emissive_texture >= r.max_textures) material.emissive_texture = white_index;
        }
        r.materials = createBuffer(device, sizeof(RtMaterial) * room(materials.size(), 1024),
                                   vk::BufferUsageFlagBits::eStorageBuffer, false, materials.data(),
                                   sizeof(RtMaterial) * materials.size());
        r.texture_count = r.max_textures;
    }
    r.ensureLayouts(device);

    // Un set por hueco de frame: lo que cambia despues (addModel, la TLAS que
    // crece) se escribe en cada uno cuando su frame anterior ya termino.
    const std::array<vk::DescriptorPoolSize, 3> scene_sizes = {
        vk::DescriptorPoolSize{Type::eAccelerationStructureKHR, kMaxFramesInFlight},
        vk::DescriptorPoolSize{Type::eStorageBuffer, 9 * kMaxFramesInFlight},
        vk::DescriptorPoolSize{Type::eCombinedImageSampler, r.max_textures * kMaxFramesInFlight}};
    vk::DescriptorPoolCreateInfo scene_pool_info{};
    scene_pool_info.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    scene_pool_info.maxSets = kMaxFramesInFlight;
    scene_pool_info.setPoolSizes(scene_sizes);
    // Al reconstruir (cada uploadModels: el editor lo llama al añadir
    // modelos) el set viejo se libera ANTES de sustituir su pool: si no, al
    // asignar scene_sets despues se liberaba sobre un pool ya destruido.
    r.scene_sets.clear();
    r.scene_pool = vk::raii::DescriptorPool(device.handle(), scene_pool_info);

    const std::vector<vk::DescriptorSetLayout> scene_layouts(kMaxFramesInFlight, *r.scene_layout);
    vk::DescriptorSetAllocateInfo alloc{};
    alloc.descriptorPool = *r.scene_pool;
    alloc.setSetLayouts(scene_layouts);
    r.scene_sets = vk::raii::DescriptorSets(device.handle(), alloc);

    // Lo que necesita addModel despues (y refreshSceneSet para escribir los sets).
    r.model_slots = slots;
    r.vertex_used = static_cast<std::uint32_t>(vertices.size());
    r.triangle_used = static_cast<std::uint32_t>(indices.size() / 3);
    r.material_used = static_cast<std::uint32_t>(materials.size());
    r.model_info = model_info;
    r.texture_infos = std::move(textures);
    r.texture_slots = std::move(texture_slots);
    r.white_index = white_index;
    r.shared_geometry.clear();
    for (const auto& [geometry_key, group] : shared_geometry) {
        r.shared_geometry.emplace(geometry_key, std::make_pair(group.owner, group.material_base));
    }
    r.irradiance = *irradiance.handle();
    ++r.scene_version;
    r.set_version.fill(0);
    r.set_textures.fill(0);
    for (std::uint32_t slot = 0; slot < kMaxFramesInFlight; ++slot) r.refreshSceneSet(slot);
    // (La cache no se vacia: esta en el mundo y se adapta sola a lo que
    // cambio. Al cambiar de escena la vacia RenderSync::clearSceneStreaming
    // con VulkanRenderer::resetRayTracingCache.)

    // --- Pipelines: la primera vez (despues se reutilizan) ---
    r.ensurePipelines(device);

    // --- Micromapa de opacidad del follaje (VK_EXT_opacity_micromap) ---
    // Los triangulos recortados por alfa, en orden: el micromapa tiene uno por
    // cada uno y cada geometria recortada empieza en su `base`. Las mallas que
    // se reescriben (terreno) no lo usan.
    const auto updatable = [&](std::size_t model) {
        return model >= slots && r.extra_models[model - slots].updatable;
    };
    std::vector<std::uint32_t> masked;
    std::vector<std::vector<std::uint32_t>> micromap_base(model_geometries.size());
    for (std::size_t m = 0; m < model_geometries.size(); ++m) {
        for (const GeometryRange& range : model_geometries[m]) {
            const bool linked = !range.opaque && !updatable(m);
            micromap_base[m].push_back(linked ? static_cast<std::uint32_t>(masked.size()) : UINT32_MAX);
            if (!linked) continue;
            for (std::uint32_t t = 0; t < range.triangle_count; ++t) masked.push_back(range.first_triangle + t);
        }
    }
    const bool omm_disabled = std::getenv("CRAMION_NO_OMM") != nullptr;
    if (device.opacityMicromapSupported() && !masked.empty() && !omm_disabled) {
        r.buildOpacityMicromap(device, masked);
    }

    // --- Una BLAS por modelo (todas en un envio y compactadas) ---
    r.blas.resize(model_geometries.size());
    const auto omm_format = static_cast<std::uint32_t>(vk::OpacityMicromapFormatEXT::e4State);
    std::vector<Resources::BlasBuild> builds;
    for (std::size_t m = 0; m < model_geometries.size(); ++m) {
        const std::vector<GeometryRange>& geometries = model_geometries[m];
        if (geometries.empty()) continue;
        Resources::BlasBuild build;
        build.model = static_cast<std::uint32_t>(m);
        build.compact = !updatable(m);
        build.omm_links.resize(geometries.size());
        build.omm_usages.resize(geometries.size());
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
            if (micromap_base[m][g] != UINT32_MAX && *r.micromap != nullptr) {
                build.omm_usages[g] = vk::MicromapUsageEXT{range.triangle_count, r.micromap_level, omm_format};
                vk::AccelerationStructureTrianglesOpacityMicromapEXT& link = build.omm_links[g];
                link.indexType = vk::IndexType::eNoneKHR;  // triangulo i -> base + i
                link.baseTriangle = micromap_base[m][g];
                link.micromap = *r.micromap;
                link.usageCountsCount = 1;
                link.pUsageCounts = &build.omm_usages[g];
                triangles.pNext = &link;
            }

            vk::AccelerationStructureGeometryKHR geometry{};
            geometry.geometryType = vk::GeometryTypeKHR::eTriangles;
            geometry.geometry.triangles = triangles;
            // Opacos: el hardware acepta el impacto sin preguntar al shader.
            geometry.flags = range.opaque ? vk::GeometryFlagBitsKHR::eOpaque
                                          : vk::GeometryFlagsKHR{};
            build.geometries.push_back(geometry);

            vk::AccelerationStructureBuildRangeInfoKHR build_range{};
            build_range.primitiveCount = range.triangle_count;
            build.ranges.push_back(build_range);
        }
        builds.push_back(std::move(build));
    }
    r.buildBottomLevels(device, builds);
    r.resolveAddresses(device);
    // Las que se reescriben guardan como se construyen (y su scratch).
    for (const Resources::BlasBuild& build : builds) {
        if (!updatable(build.model)) continue;
        Resources::ExtraModel& e = r.extra_models[build.model - slots];
        e.geometries = build.geometries;
        e.ranges = build.ranges;
        e.scratch = createBuffer(device, build.sizes.buildScratchSize + r.scratch_alignment,
                                 vk::BufferUsageFlagBits::eStorageBuffer, false);
    }

    std::cout << "[Vulkan] Trazado de rayos listo: " << indices.size() / 3 << " triangulos, "
              << materials.size() << " materiales, " << r.texture_count << " texturas ("
              << extra.meshes.size() << " mallas extra; " << builds.size() << " BLAS, " << shared_models
              << " modelos con la geometria de otro, " << skipped_models << " sin instancias)\n";
}

void RayTracing::releaseScene() {
    if (resources_) resources_->releaseScene();
}

bool RayTracing::sceneBuilt() const {
    return resources_ != nullptr && !resources_->scene_sets.empty();
}

std::uint32_t RayTracing::modelCount() const {
    return resources_ != nullptr ? resources_->model_count : 0u;
}

std::uint32_t RayTracing::modelSlots() const {
    return resources_ != nullptr ? resources_->model_slots : 0u;
}

bool RayTracing::hasModel(std::uint32_t model) const {
    if (!sceneBuilt()) return false;
    const Resources& r = *resources_;
    return model < r.model_slots && model < r.blas_owner.size() && r.blas_owner[model] != UINT32_MAX;
}

bool RayTracing::addModel(std::uint32_t index, const asset::ModelData& model,
                          const std::vector<SkinnedModel>& gpu_models) {
    if (!sceneBuilt() || index >= gpu_models.size()) return true;
    Resources& r = *resources_;
    if (index >= r.model_slots) return false;  // sin fila: hay que rehacerla entera
    if (r.blas_owner[index] != UINT32_MAX) return true;  // ya esta
    const SkinnedModel& gpu = gpu_models[index];
    // (Un hueco vacio o fuera de la GPU: nada que trazar, de momento.)
    if (model.indices.empty() || model.vertices.empty() || gpu.materials().empty()) return true;

    Resources::PendingModel pending;
    pending.model = index;
    pending.material_offset = r.material_used;
    const std::uint32_t material_base = r.material_used;

    // Texturas: las que ya estan en el set, por su vista; las nuevas, detras
    // (si no caben, blancas, como en build).
    std::vector<std::uint32_t> texture_map(gpu.textures().size(), r.white_index);
    for (std::size_t t = 0; t < gpu.textures().size(); ++t) {
        const vk::ImageView view = *gpu.textures()[t]->view();
        auto it = r.texture_slots.find(static_cast<VkImageView>(view));
        if (it == r.texture_slots.end()) {
            if (r.texture_infos.size() >= r.max_textures) continue;
            it = r.texture_slots.emplace(static_cast<VkImageView>(view), static_cast<std::uint32_t>(r.texture_infos.size()))
                     .first;
            r.texture_infos.push_back(
                vk::DescriptorImageInfo{*r.texture_sampler, view, vk::ImageLayout::eShaderReadOnlyOptimal});
        }
        texture_map[t] = it->second < r.texture_infos.size() ? it->second : r.white_index;
    }
    r.texture_count = static_cast<std::uint32_t>(r.texture_infos.size());
    const auto texture_slot = [&](std::uint32_t local) {
        return local < texture_map.size() ? texture_map[local] : r.white_index;
    };
    for (const SkinnedModel::Material& material : gpu.materials()) {
        RtMaterial rt{};
        rt.base_color = material.base_color;
        rt.emissive = material.emissive;
        rt.params = core::Vec4{material.params.x, material.params.y, 0.0f, 0.0f};
        rt.albedo_texture = texture_slot(material.albedo_texture);
        rt.metallic_roughness_texture = texture_slot(material.metallic_roughness_texture);
        rt.emissive_texture = texture_slot(material.emissive_texture);
        rt.flags = material.alpha_masked ? 1u : 0u;
        rt.uv_transform = material.uv_transform;
        pending.materials.push_back(rt);
    }
    r.material_used += static_cast<std::uint32_t>(pending.materials.size());

    // Geometria compartida (variantes de material): la misma clave que build.
    const void* vertex_identity = gpu.vertices().isValid() ? static_cast<const void*>(&gpu.vertices())
                                                           : static_cast<const void*>(model.vertices.data());
    const void* index_identity = gpu.indices().isValid() ? static_cast<const void*>(&gpu.indices())
                                                         : static_cast<const void*>(model.indices.data());
    std::string key;
    const auto append = [&key](const auto& value) {
        key.append(reinterpret_cast<const char*>(&value), sizeof(value));
    };
    append(vertex_identity);
    append(index_identity);
    append(model.vertices.size());
    append(model.indices.size());
    for (const asset::SubMesh& submesh : model.submeshes) {
        const SkinnedModel::Material& material = gpu.materials()[submesh.material];
        const std::uint8_t kind = material.transparent ? 0 : material.alpha_masked ? 2 : 1;
        append(submesh.first_index);
        append(submesh.index_count);
        append(submesh.material);
        append(kind);
        if (kind == 2) {
            append(texture_slot(material.albedo_texture));
            append(material.base_color.w);
            append(material.uv_transform);
        }
    }
    std::uint32_t* row = r.model_info.data() + static_cast<std::size_t>(index) * 4;
    if (const auto group = r.shared_geometry.find(key); group != r.shared_geometry.end()) {
        // Solo si el dueno sigue con esos mismos buffers en la GPU (vivos: su
        // direccion no puede ser la de otros nuevos) y con su BLAS.
        const std::uint32_t owner = group->second.first;
        const bool alive = gpu.vertices().isValid() && gpu.indices().isValid() && owner < gpu_models.size() &&
                           owner < r.model_slots && r.blas_owner[owner] == owner &&
                           &gpu_models[owner].vertices() == &gpu.vertices() &&
                           &gpu_models[owner].indices() == &gpu.indices();
        if (alive) {
            row[0] = r.model_info[static_cast<std::size_t>(owner) * 4];
            row[1] = r.model_info[static_cast<std::size_t>(owner) * 4 + 1];
            row[2] = material_base - group->second.second;
            row[3] = 0;
            r.blas_owner[index] = owner;
            r.pending_models.push_back(std::move(pending));
            return true;
        }
        r.shared_geometry.erase(group);
    }
    r.shared_geometry.emplace(key, std::make_pair(index, material_base));
    r.blas_owner[index] = index;

    // Sus vertices e indices, al final (primero los triangulos opacos, luego
    // los recortados por alfa, como en build).
    pending.owns_blas = true;
    pending.vertex_offset = r.vertex_used;
    pending.triangle_offset = r.triangle_used;
    const std::uint32_t vertex_base = r.vertex_used;
    pending.vertices.reserve(model.vertices.size());
    for (const asset::SkinnedVertex& v : model.vertices) {
        pending.vertices.push_back(
            RtVertex{v.position.x, v.position.y, v.position.z, v.normal.x, v.normal.y, v.normal.z, v.uv.x, v.uv.y});
    }
    r.vertex_used += static_cast<std::uint32_t>(model.vertices.size());
    for (const bool masked : {false, true}) {
        Resources::PendingModel::Range range{};
        range.first_triangle = r.triangle_used + static_cast<std::uint32_t>(pending.indices.size() / 3);
        range.opaque = !masked;
        for (const asset::SubMesh& submesh : model.submeshes) {
            const SkinnedModel::Material& material = gpu.materials()[submesh.material];
            if (material.transparent || material.alpha_masked != masked) continue;
            for (std::uint32_t i = 0; i < submesh.index_count; ++i) {
                pending.indices.push_back(vertex_base + model.indices[submesh.first_index + i]);
            }
            for (std::uint32_t t = 0; t < submesh.index_count / 3; ++t) {
                pending.triangle_materials.push_back(material_base + submesh.material);
            }
        }
        range.triangle_count =
            r.triangle_used + static_cast<std::uint32_t>(pending.indices.size() / 3) - range.first_triangle;
        if (range.triangle_count > 0) pending.ranges.push_back(range);
    }
    r.triangle_used += static_cast<std::uint32_t>(pending.indices.size() / 3);
    // Su fila de rt_models (x, y = primer triangulo de cada geometria).
    row[0] = pending.ranges.size() > 0 ? pending.ranges[0].first_triangle : 0;
    row[1] = pending.ranges.size() > 1 ? pending.ranges[1].first_triangle : 0;
    row[2] = 0;
    row[3] = 0;
    r.pending_models.push_back(std::move(pending));
    return true;
}

void RayTracing::updateExtraVertices(std::uint32_t extra_index, std::vector<Vertex> vertices) {
    if (!sceneBuilt()) return;
    Resources& r = *resources_;
    if (extra_index >= r.extra_models.size()) return;
    const Resources::ExtraModel& e = r.extra_models[extra_index];
    if (!e.updatable || vertices.size() != e.vertex_count || e.geometries.empty()) return;
    for (Resources::PendingVertices& pending : r.pending_vertices) {
        if (pending.extra == extra_index) {
            pending.vertices = std::move(vertices);
            return;
        }
    }
    r.pending_vertices.push_back(Resources::PendingVertices{extra_index, std::move(vertices)});
}

void RayTracing::updateExtraTexture(std::uint32_t texture_index, std::vector<std::vector<std::uint8_t>> mips) {
    if (!sceneBuilt()) return;
    Resources& r = *resources_;
    if (texture_index >= r.extra_textures.size()) return;
    const Resources::OwnedTexture& texture = r.extra_textures[texture_index];
    if (*texture.image == nullptr || mips.size() != texture.mips) return;
    for (Resources::PendingTexture& pending : r.pending_textures) {
        if (pending.texture == texture_index) {
            pending.mips = std::move(mips);
            return;
        }
    }
    r.pending_textures.push_back(Resources::PendingTexture{texture_index, std::move(mips)});
}

void RayTracing::setInstances(const VulkanDevice& device, const std::vector<Instance>& instances) {
    if (!sceneBuilt()) return;
    Resources& r = *resources_;

    // Solo los modelos que tienen BLAS (la suya o la del grupo con su misma
    // geometria: las variantes de material). El indice propio de la instancia
    // sigue siendo el modelo: sus materiales.
    std::vector<vk::AccelerationStructureInstanceKHR> gpu_instances;
    gpu_instances.reserve(instances.size());
    for (const Instance& instance : instances) {
        const std::uint32_t owner = instance.model < r.blas_owner.size() ? r.blas_owner[instance.model] : UINT32_MAX;
        if (owner >= r.blas.size() || r.blas[owner].address == 0) continue;
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
        gpu.setMask(instance.mask);
        gpu.setFlags(vk::GeometryInstanceFlagBitsKHR::eTriangleFacingCullDisable);
        gpu.setAccelerationStructureReference(r.blas[owner].address);
        gpu_instances.push_back(gpu);
    }

    const auto count = static_cast<std::uint32_t>(gpu_instances.size());
    if (*r.tlas.handle == nullptr || count > r.tlas_capacity) {
        // La TLAS (y su descriptor) cambia sin parar la GPU: la vieja la
        // sueltan los frames en vuelo (retireTopLevel) y cada set apunta a la
        // nueva antes de su siguiente uso. Crece con margen: pocas veces.
        r.retireTopLevel();
        const std::uint32_t capacity = std::max({256u, count + count / 2, r.tlas_capacity_hint});
        r.tlas_capacity_hint = capacity;
        r.createTopLevel(device, capacity);
        std::cout << "[Vulkan] TLAS para " << capacity << " instancias\n";
    }
    const bool same = gpu_instances.size() == r.gpu_instances.size() &&
                      (gpu_instances.empty() ||
                       std::memcmp(gpu_instances.data(), r.gpu_instances.data(),
                                   gpu_instances.size() * sizeof(vk::AccelerationStructureInstanceKHR)) == 0);
    if (!same) {
        r.gpu_instances = std::move(gpu_instances);
        r.tlas_dirty = true;
    }
}

void RayTracing::recordUpdates(const vk::raii::CommandBuffer& cmd, std::uint32_t frame_index) {
    if (!sceneBuilt()) return;
    Resources& r = *resources_;
    const std::uint32_t slot = frame_index % kMaxFramesInFlight;
    // El frame anterior de este hueco ya termino (su fence): su staging sobra
    // (y lo que se retiro en el: buffers que crecieron, scratch de BLAS).
    r.staging[slot].clear();

    // --- Modelos que entraron (addModel): sus datos y sus BLAS ---
    if (!r.pending_models.empty()) r.flushPendingModels(cmd, slot);
    // El set de este hueco, al dia (TLAS nueva, buffers que crecieron, texturas).
    r.refreshSceneSet(slot);

    // --- Vertices y texturas nuevos (terreno esculpido o pintado) ---
    std::vector<std::uint32_t> rebuild;
    if (!r.pending_vertices.empty() || !r.pending_textures.empty()) {
        // Lo que los frames anteriores aun leen (rayos y construcciones).
        memoryBarrier(cmd, kRayStages | vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR,
                      vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eAccelerationStructureReadKHR,
                      vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite);
        for (Resources::PendingVertices& pending : r.pending_vertices) {
            const Resources::ExtraModel& e = r.extra_models[pending.extra];
            const vk::DeviceSize bytes = sizeof(RtVertex) * pending.vertices.size();
            DeviceBuffer staging = createBuffer(*r.device, bytes, vk::BufferUsageFlagBits::eTransferSrc,
                                                true, pending.vertices.data());
            cmd.copyBuffer(*staging.buffer, *r.vertices.buffer,
                           vk::BufferCopy{0, sizeof(RtVertex) * static_cast<vk::DeviceSize>(e.vertex_base), bytes});
            r.staging[slot].push_back(std::move(staging));
            rebuild.push_back(pending.extra);
        }
        for (Resources::PendingTexture& pending : r.pending_textures) {
            const Resources::OwnedTexture& texture = r.extra_textures[pending.texture];
            vk::DeviceSize total = 0;
            for (const auto& level : pending.mips) total += level.size();
            std::vector<std::uint8_t> packed;
            packed.reserve(static_cast<std::size_t>(total));
            std::vector<vk::BufferImageCopy> regions;
            std::uint32_t size = texture.size;
            for (std::uint32_t m = 0; m < texture.mips; ++m) {
                vk::BufferImageCopy region{};
                region.bufferOffset = packed.size();
                region.imageSubresource = vk::ImageSubresourceLayers{vk::ImageAspectFlagBits::eColor, m, 0, 1};
                region.imageExtent = vk::Extent3D{size, size, 1};
                regions.push_back(region);
                packed.insert(packed.end(), pending.mips[m].begin(), pending.mips[m].end());
                size = std::max(size / 2, 1u);
            }
            DeviceBuffer staging = createBuffer(*r.device, packed.size(),
                                                vk::BufferUsageFlagBits::eTransferSrc, true, packed.data());
            const vk::Image image = *texture.image;
            imageBarrier(cmd, image, texture.mips, vk::ImageLayout::eShaderReadOnlyOptimal,
                         vk::ImageLayout::eTransferDstOptimal, kRayStages, vk::AccessFlagBits2::eShaderSampledRead,
                         vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite);
            cmd.copyBufferToImage(*staging.buffer, image, vk::ImageLayout::eTransferDstOptimal, regions);
            imageBarrier(cmd, image, texture.mips, vk::ImageLayout::eTransferDstOptimal,
                         vk::ImageLayout::eShaderReadOnlyOptimal, vk::PipelineStageFlagBits2::eTransfer,
                         vk::AccessFlagBits2::eTransferWrite, kRayStages, vk::AccessFlagBits2::eShaderSampledRead);
            r.staging[slot].push_back(std::move(staging));
        }
        r.pending_vertices.clear();
        r.pending_textures.clear();
        memoryBarrier(cmd, vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite,
                      vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR | kRayStages,
                      vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eAccelerationStructureReadKHR);
    }

    // --- Sus BLAS, de nuevo (en el mismo sitio: la direccion no cambia) ---
    if (!rebuild.empty()) {
        for (const std::uint32_t extra : rebuild) {
            Resources::ExtraModel& e = r.extra_models[extra];
            vk::AccelerationStructureBuildGeometryInfoKHR info{};
            info.type = vk::AccelerationStructureTypeKHR::eBottomLevel;
            info.flags = vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace;
            info.mode = vk::BuildAccelerationStructureModeKHR::eBuild;
            info.setGeometries(e.geometries);
            info.dstAccelerationStructure = *r.blas[e.model].handle;
            info.scratchData.deviceAddress = alignUp(e.scratch.address, r.scratch_alignment);
            const vk::AccelerationStructureBuildRangeInfoKHR* ranges = e.ranges.data();
            cmd.buildAccelerationStructuresKHR(info, ranges);
        }
        memoryBarrier(cmd, vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR,
                      vk::AccessFlagBits2::eAccelerationStructureWriteKHR,
                      vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR | kRayStages,
                      vk::AccessFlagBits2::eAccelerationStructureReadKHR);
        // La TLAS guarda las cajas de sus BLAS: hay que rehacerla.
        r.tlas_dirty = true;
    }

    // --- TLAS ---
    if (r.tlas_dirty && *r.tlas.handle != nullptr) {
        DeviceBuffer& instances = r.instance_buffers[slot];
        const auto count = static_cast<std::uint32_t>(std::min<std::size_t>(r.gpu_instances.size(), r.tlas_capacity));
        if (count > 0) {
            std::memcpy(instances.mapped, r.gpu_instances.data(), sizeof(vk::AccelerationStructureInstanceKHR) * count);
        }
        vk::AccelerationStructureGeometryInstancesDataKHR instances_data{};
        instances_data.arrayOfPointers = VK_FALSE;
        instances_data.data.deviceAddress = instances.address;
        vk::AccelerationStructureGeometryKHR geometry{};
        geometry.geometryType = vk::GeometryTypeKHR::eInstances;
        geometry.geometry.instances = instances_data;
        vk::AccelerationStructureBuildGeometryInfoKHR info{};
        info.type = vk::AccelerationStructureTypeKHR::eTopLevel;
        info.flags = vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace;
        info.mode = vk::BuildAccelerationStructureModeKHR::eBuild;
        info.setGeometries(geometry);
        info.dstAccelerationStructure = *r.tlas.handle;
        info.scratchData.deviceAddress = alignUp(r.tlas_scratch.address, r.scratch_alignment);
        vk::AccelerationStructureBuildRangeInfoKHR range{};
        range.primitiveCount = count;
        const vk::AccelerationStructureBuildRangeInfoKHR* range_pointer = &range;
        // Los frames anteriores dejan de leerla (y de construirla) antes.
        memoryBarrier(cmd, kRayStages | vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR,
                      vk::AccessFlagBits2::eAccelerationStructureReadKHR |
                          vk::AccessFlagBits2::eAccelerationStructureWriteKHR,
                      vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR,
                      vk::AccessFlagBits2::eAccelerationStructureReadKHR |
                          vk::AccessFlagBits2::eAccelerationStructureWriteKHR);
        cmd.buildAccelerationStructuresKHR(info, range_pointer);
        memoryBarrier(cmd, vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR,
                      vk::AccessFlagBits2::eAccelerationStructureWriteKHR, kRayStages,
                      vk::AccessFlagBits2::eAccelerationStructureReadKHR);
        r.tlas_dirty = false;
        r.tlas_built = true;
    }
}

void RayTracing::updateFrameSet(const VulkanDevice& device, std::uint32_t frame_index,
                                const FrameInputs& inputs) {
    Resources& r = *resources_;
    for (std::uint32_t pass = 0; pass < kPassCount; ++pass) {
        const vk::DescriptorSet set = *r.frame_sets[frame_index * kPassCount + pass];

        const vk::DescriptorBufferInfo camera{*inputs.camera->handle(), 0, VK_WHOLE_SIZE};
        const vk::DescriptorBufferInfo lights{*inputs.lights->handle(), 0, VK_WHOLE_SIZE};
        const vk::DescriptorImageInfo depth{inputs.sampler, inputs.depth,
                                            compat::depthReadOnlyLayout()};
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
    if (resources_ == nullptr) return false;
    const Resources& r = *resources_;
    // La TLAS ya esta construida o se construye al principio de este frame
    // (recordUpdates, antes de cualquier pase de rayos).
    return !r.scene_sets.empty() && *r.tlas.handle != nullptr && (r.tlas_built || r.tlas_dirty) &&
           *r.pipelines[0] != nullptr;
}

void RayTracing::record(const vk::raii::CommandBuffer& cmd, std::uint32_t frame_index, Pass pass,
                        vk::Extent2D extent, const Push& push) const {
    Resources& r = *resources_;
    const auto index = static_cast<std::uint32_t>(pass);
    // El set de la escena de este hueco (ya al dia si paso por recordUpdates).
    const std::uint32_t slot = frame_index % kMaxFramesInFlight;
    r.refreshSceneSet(slot);
    // La cache de radiancia se escribe (GI, resolucion) y se lee (GI,
    // reflejos) en pases seguidos: cada uno espera a que el anterior acabe.
    vk::MemoryBarrier2 cache_barrier{};
    cache_barrier.srcStageMask = vk::PipelineStageFlagBits2::eComputeShader;
    cache_barrier.srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite;
    cache_barrier.dstStageMask = vk::PipelineStageFlagBits2::eComputeShader;
    cache_barrier.dstAccessMask = vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite;
    vk::DependencyInfo dependency{};
    dependency.setMemoryBarriers(cache_barrier);
    compat::pipelineBarrier(cmd, dependency);

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
        compat::pipelineBarrier(cmd, to_rays_dependency);
        cmd.bindPipeline(vk::PipelineBindPoint::eRayTracingKHR, *r.ser_pipeline);
        const std::array<vk::DescriptorSet, 2> rt_sets = {*r.frame_sets[frame_index * kPassCount + index],
                                                          *r.scene_sets[slot]};
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
        compat::pipelineBarrier(cmd, from_rays_dependency);
        return;
    }
    if (pass == Pass::CacheResolve) {
        data.params.z = r.cache_reset ? 1.0f : 0.0f;
        r.cache_reset = false;
    }
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *r.pipelines[index]);
    const std::array<vk::DescriptorSet, 2> sets = {*r.frame_sets[frame_index * kPassCount + index],
                                                   *r.scene_sets[slot]};
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *r.pipeline_layout, 0, sets, nullptr);
    cmd.pushConstants<Push>(*r.pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0, data);
    cmd.dispatch((extent.width + kGroupSize - 1) / kGroupSize,
                 (extent.height + kGroupSize - 1) / kGroupSize, 1);
}

void RayTracing::recordProbeUpdate(const vk::raii::CommandBuffer& cmd, std::uint32_t frame_index,
                                   vk::DescriptorSetLayout probe_layout, vk::DescriptorSet probe_set, const Push& push,
                                   std::uint32_t probe_count) {
    if (!resources_ || !ready() || probe_count == 0 || resources_->scene_sets.empty() || resources_->frame_sets.empty()) {
        return;
    }
    Resources& r = *resources_;
    // La TLAS ya construida en un frame anterior: la captura de la sonda
    // (kProbeFrameSlot) no pasa por recordUpdates y una recien creada
    // (setInstances) aun no tiene nada.
    if (!r.tlas_built) return;
    const std::uint32_t slot = frame_index % kMaxFramesInFlight;
    r.refreshSceneSet(slot);
    const VulkanDevice& device = *r.device;
    if (*r.probe_pipeline == nullptr || r.probe_set_layout != static_cast<VkDescriptorSetLayout>(probe_layout)) {
        const std::array<vk::DescriptorSetLayout, 3> set_layouts = {*r.frame_layout, *r.scene_layout, probe_layout};
        const vk::PushConstantRange push_range{vk::ShaderStageFlagBits::eCompute, 0, sizeof(Push)};
        vk::PipelineLayoutCreateInfo layout_info{};
        layout_info.setSetLayouts(set_layouts);
        layout_info.setPushConstantRanges(push_range);
        r.probe_pipeline_layout = vk::raii::PipelineLayout(device.handle(), layout_info);
        const vk::raii::ShaderModule module = shaders::loadModule(device, "rt_probes.comp.spv");
        vk::ComputePipelineCreateInfo pipeline_info{};
        pipeline_info.stage.stage = vk::ShaderStageFlagBits::eCompute;
        pipeline_info.stage.module = *module;
        pipeline_info.stage.pName = "main";
        pipeline_info.layout = *r.probe_pipeline_layout;
        r.probe_pipeline = vk::raii::Pipeline(device.handle(), device.pipelineCache(), pipeline_info);
        r.probe_set_layout = static_cast<VkDescriptorSetLayout>(probe_layout);
    }
    // Lo que escribio el frame anterior (sondas leidas por la GI) antes de escribir.
    vk::MemoryBarrier2 before{};
    before.srcStageMask = vk::PipelineStageFlagBits2::eAllCommands;
    before.srcAccessMask = vk::AccessFlagBits2::eMemoryWrite | vk::AccessFlagBits2::eMemoryRead;
    before.dstStageMask = vk::PipelineStageFlagBits2::eComputeShader;
    before.dstAccessMask = vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite;
    vk::DependencyInfo before_dep{};
    before_dep.setMemoryBarriers(before);
    compat::pipelineBarrier(cmd, before_dep);

    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *r.probe_pipeline);
    const std::array<vk::DescriptorSet, 3> sets = {
        *r.frame_sets[frame_index * kPassCount + static_cast<std::uint32_t>(Pass::Gi)], *r.scene_sets[slot], probe_set};
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *r.probe_pipeline_layout, 0, sets, nullptr);
    cmd.pushConstants<Push>(*r.probe_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0, push);
    cmd.dispatch((probe_count + 63) / 64, 1, 1);

    // Las sondas nuevas las lee la GI horneada (fragment) de este frame.
    vk::MemoryBarrier2 after{};
    after.srcStageMask = vk::PipelineStageFlagBits2::eComputeShader;
    after.srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite;
    after.dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader | vk::PipelineStageFlagBits2::eComputeShader;
    after.dstAccessMask = vk::AccessFlagBits2::eShaderStorageRead;
    vk::DependencyInfo after_dep{};
    after_dep.setMemoryBarriers(after);
    compat::pipelineBarrier(cmd, after_dep);
}

}  // namespace cramion::gfx
