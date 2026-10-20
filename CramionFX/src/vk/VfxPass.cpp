#include "CramionFX/vk/VulkanCompat.h"
#include "CramionFX/vk/VfxPass.h"

#include "CramionFX/asset/ImageFile.h"
#include "CramionFX/vk/VulkanDevice.h"
#include "CramionFX/vk/VulkanShader.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>

namespace cramion::gfx {

namespace {

// Etapas de vfx_sim.comp.
enum Stage : std::uint32_t {
    kReset = 0,
    kBegin = 1,
    kEmit = 2,
    kUpdate = 3,
    kSortLocal = 4,
    kSortFlip = 5,
    kSortDisperse = 6,
    kSortLocalDisperse = 7,
    kEmitEvents = 8,
    kEmitChannel = 20,  // STAGE_EMIT_CHANNEL
};
// Bloque de la ordenacion en memoria compartida (2 x kGroup, vfx_sim.comp).
constexpr std::uint32_t kSortBlock = 128;

// 0 UBO, 1..7 storage, 8 depth, 9 texturas, 10..12 storage (mallas y eventos).
constexpr std::uint32_t kBindings = 13;
constexpr std::uint32_t kStorageBindings = 10;
// Luz real de "lit" (solo el fragmento): 13 luces (UBO), 14 cascadas (UBO),
// 15 mapa de sombras, 16 y 17 clusteres (storage). En el modo compatible
// solo la 13 (dentro de los recursos por shader de ese modo).
constexpr std::uint32_t kLightBindings = 18;
constexpr std::uint32_t kCompatLightBindings = 14;
constexpr vk::DeviceSize kDummyUniformBytes = 4096;  // >= GpuLights y GpuShadows en lo que lee vfx.frag
constexpr vk::DeviceSize kDummyStorageBytes = 256;   // cabecera de clusteres a cero (sin luces)
// Lo que declara vfx.frag de GpuLights (5 vec4, ivec4, 32 puntuales de 3 vec4
// y 8 focos de 4) y de GpuShadows (4 mat4 y 3 vec4).
constexpr vk::DeviceSize kVfxLightsRange = 6 * 16 + 32 * 48 + 8 * 64;
constexpr vk::DeviceSize kVfxShadowsRange = 4 * 64 + 3 * 16;
static_assert(kVfxLightsRange <= kDummyUniformBytes && kVfxShadowsRange <= kDummyUniformBytes);
constexpr std::uint32_t kCountersPerSlot = 8;
constexpr std::uint32_t kInitialPool = 65536;
constexpr std::uint32_t kGroup = 64;  // local_size_x de vfx_sim.comp

// Bits de flags.z (vfx_common.glsl).
constexpr std::uint32_t kWorldSpace = 1u;
constexpr std::uint32_t kColorOverLife = 2u;
constexpr std::uint32_t kSizeOverLife = 4u;
constexpr std::uint32_t kColorBySpeed = 8u;
constexpr std::uint32_t kLit = 16u;
constexpr std::uint32_t kInterpolate = 32u;
constexpr std::uint32_t kFlipBlend = 4096u;   // VFX_FLIP_BLEND
constexpr std::uint32_t kRibbonIds = 8192u;   // VFX_RIBBON_IDS
// Turbulencia con el volumen horneado (VFX_CURL_BAKED; un bit alto para no
// chocar con los que se anadan abajo).
constexpr std::uint32_t kCurlBaked = 1u << 27;
constexpr std::uint32_t kNoEvents = 0xFFFFFFFFu;
// Canales de datos (vfx_common.glsl): bit de flags.z (tiene bloques Emit*),
// bit de push.pad (alguien escucha) y su sitio en Counters (detras de las
// ranuras: 8 por canal y kMaxListens por ranura) y en Events (detras de las
// listas de los subemisores: 3 vec4 por evento).
constexpr std::uint32_t kEmitsChannel = 1u << 20;  // VFX_EMITS_CHANNEL
constexpr std::uint32_t kPadChannels = 2u;         // VFX_PAD_CHANNELS
constexpr std::uint32_t kChannelCounters = kCountersPerSlot * VfxPass::kMaxInstances;
constexpr std::uint32_t kListenCounters = kChannelCounters + 8 * VfxPass::kMaxChannels;
constexpr std::uint32_t kCounterInts = kListenCounters + VfxPass::kMaxInstances * VfxPass::kMaxListens;
constexpr vk::DeviceSize kChannelEventsBase = 2ull * VfxPass::kMaxEvents * 2 * VfxPass::kMaxEventLists;  // en vec4
constexpr vk::DeviceSize kChannelEventsVec4 = 3ull * VfxPass::kMaxChannelEvents * 2 * VfxPass::kMaxChannels;
static_assert(kChannelCounters == 2048 && kListenCounters == 2048 + 64 && kChannelEventsBase == 65536,
              "MISMOS numeros que VFX_CHANNEL_COUNTERS / VFX_LISTEN_COUNTERS / VFX_CHANNEL_EVENTS");
// Ruido rizado horneado: storage de solo la simulacion (vfx_sim.comp). Un
// numero alto, lejos de los de arriba.
constexpr std::uint32_t kCurlBinding = 24;
// Salida Luz (luces de particula): storage de solo la simulacion, uno por
// hueco de frame (vfx_sim.comp VfxLightOut): cabecera (uvec4: x candidatas
// apuntadas), un vec4 por ranura (x alcance, y intensidad (0 = sin luz),
// w parte de las vivas que dan luz) y 3 uvec4 por candidata.
constexpr std::uint32_t kParticleLightBinding = 26;
constexpr vk::DeviceSize kLightParamsOffset = 16;
constexpr vk::DeviceSize kLightItemsOffset = kLightParamsOffset + 16ull * VfxPass::kMaxInstances;
constexpr vk::DeviceSize kLightBufferBytes = kLightItemsOffset + 48ull * VfxPass::kMaxLightCandidates;
// Lo que se adelanta una luz con su velocidad como mucho (s): la lectura
// llega frames_in_flight frames tarde.
constexpr float kLightMaxLead = 0.1f;
// Pool de instancias: solo se aparcan las pequenas (impactos), para no
// guardar memoria de efectos grandes que ya no estan.
constexpr std::uint32_t kParkMaxCapacity = 16384;

// Constante de especializacion 0 de los shaders del VFX: ranuras de textura.
struct TextureSpecialization {
    std::uint32_t count = 16;
    vk::SpecializationMapEntry entry{0, 0, sizeof(std::uint32_t)};
    vk::SpecializationInfo info{};
    explicit TextureSpecialization(std::uint32_t n) : count(n) {
        info.mapEntryCount = 1;
        info.pMapEntries = &entry;
        info.dataSize = sizeof(count);
        info.pData = &count;
    }
    TextureSpecialization(const TextureSpecialization&) = delete;
    TextureSpecialization& operator=(const TextureSpecialization&) = delete;
};

// Ruido de valor periodico (kVfxCurlPeriod celdas) para hornear el volumen.
float curlHash(int x, int y, int z) {
    const auto wrap = [](int v) {
        const int p = static_cast<int>(kVfxCurlPeriod);
        return static_cast<std::uint32_t>(((v % p) + p) % p);
    };
    std::uint32_t h = wrap(x) * 73856093u ^ wrap(y) * 19349663u ^ wrap(z) * 83492791u;
    h = h * 747796405u + 2891336453u;
    h = ((h >> ((h >> 28u) + 4u)) ^ h) * 277803737u;
    h = (h >> 22u) ^ h;
    return static_cast<float>(h) * (2.0f / 4294967295.0f) - 1.0f;
}

float periodicNoise(const core::Vec3& p, int salt) {
    const float fx = std::floor(p.x), fy = std::floor(p.y), fz = std::floor(p.z);
    const int ix = static_cast<int>(fx) + salt * 3, iy = static_cast<int>(fy) + salt * 5, iz = static_cast<int>(fz) + salt * 7;
    const float tx = p.x - fx, ty = p.y - fy, tz = p.z - fz;
    const float ux = tx * tx * (3.0f - 2.0f * tx), uy = ty * ty * (3.0f - 2.0f * ty), uz = tz * tz * (3.0f - 2.0f * tz);
    const auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
    const float x00 = lerp(curlHash(ix, iy, iz), curlHash(ix + 1, iy, iz), ux);
    const float x10 = lerp(curlHash(ix, iy + 1, iz), curlHash(ix + 1, iy + 1, iz), ux);
    const float x01 = lerp(curlHash(ix, iy, iz + 1), curlHash(ix + 1, iy, iz + 1), ux);
    const float x11 = lerp(curlHash(ix, iy + 1, iz + 1), curlHash(ix + 1, iy + 1, iz + 1), ux);
    return lerp(lerp(x00, x10, uy), lerp(x01, x11, uy), uz);
}

core::Vec3 curlPotential(const core::Vec3& p) {
    return core::Vec3{periodicNoise(p, 0), periodicNoise(p, 1), periodicNoise(p, 2)};
}

// MISMO orden que vfx_common.glsl (VfxFrame, std140).
struct FrameGpu {
    core::Mat4 view;
    core::Mat4 projection;
    core::Mat4 view_projection;
    core::Mat4 inverse_view_projection;
    core::Mat4 inverse_projection;
    core::Vec4 camera;
    core::Vec4 right;
    core::Vec4 up;
    core::Vec4 viewport;
    core::Vec4 light;
    core::Vec4 sun;
    core::Vec4 ambient;  // rgb sin el sol, w = 1 con las luces del frame enlazadas
};
static_assert(sizeof(FrameGpu) == 5 * 64 + 7 * 16);

// MISMO orden que vfx_common.glsl (VfxInstance, std430).
struct InstanceGpu {
    core::Mat4 transform;
    core::Mat4 local_to_world;
    core::Mat4 world_to_local;
    std::uint32_t range[4];
    std::uint32_t blocks[4];
    core::Vec4 time;
    std::uint32_t flags[4];
    core::Vec4 output0;
    core::Vec4 output1;
    core::Vec4 speed_slow;
    core::Vec4 speed_fast;
    core::Vec4 speed_range;
    core::Vec4 shift;
    core::Vec4 emitter_velocity;
    core::Vec4 color_lut[kVfxLutSize];
    core::Vec4 size_lut[kVfxLutSize / 4];
    std::uint32_t extra[4];  // salida, primer indice de la malla, indices, lista de eventos
    core::Vec4 output2;      // cinta: repeticion (m), metros del emisor, cara, UV; malla: z orientacion
    core::Vec4 output3;      // malla: escala; cinta: x corte (s), yzw vector (CustomFacing / CustomSide)
    core::Vec4 ribbon0;      // cinta: tension, tramos (negativo = adaptativo), celda del atlas (-1 = entera)
    core::Vec4 ribbon1;      // cinta: UV escala xy, desplazamiento zw
    // Malla: variantes, LOD, giro de Fija, escala al azar y material; y la
    // mezcla Distorsion (vfx_common.glsl).
    std::uint32_t variant_first[4];
    std::uint32_t variant_count[4];
    core::Vec4 mesh0;          // variantes, m por nivel de LOD, distancia del sprite, distorsion por alturas
    core::Vec4 mesh_rotation;  // cuaternion de Fija
    core::Vec4 mesh_jitter;    // escala al azar por eje
    core::Vec4 tint;           // material: color base
    core::Vec4 emissive;       // material: emision; w distorsion (px a 1080p)
};
static_assert(sizeof(InstanceGpu) == 1088 + 112);

struct Push {
    std::uint32_t stage = 0;
    std::uint32_t slot = 0;
    std::uint32_t count = 0;
    std::uint32_t pad = 0;
};

constexpr vk::ShaderStageFlags kAllStages =
    vk::ShaderStageFlagBits::eCompute | vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment;

void memoryBarrier(const vk::raii::CommandBuffer& cmd, vk::PipelineStageFlags2 src_stage, vk::AccessFlags2 src_access,
                   vk::PipelineStageFlags2 dst_stage, vk::AccessFlags2 dst_access) {
    vk::MemoryBarrier2 barrier{};
    barrier.srcStageMask = src_stage;
    barrier.srcAccessMask = src_access;
    barrier.dstStageMask = dst_stage;
    barrier.dstAccessMask = dst_access;
    vk::DependencyInfo dependency{};
    dependency.setMemoryBarriers(barrier);
    compat::pipelineBarrier(cmd, dependency);
}

void computeBarrier(const vk::raii::CommandBuffer& cmd) {
    memoryBarrier(cmd, vk::PipelineStageFlagBits2::eComputeShader,
                  vk::AccessFlagBits2::eShaderStorageWrite | vk::AccessFlagBits2::eShaderStorageRead,
                  vk::PipelineStageFlagBits2::eComputeShader,
                  vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite);
}

std::uint32_t groups(std::uint32_t threads) {
    return std::max((threads + kGroup - 1) / kGroup, 1u);
}

}  // namespace

// -----------------------------------------------------------------------------
// Creacion
// -----------------------------------------------------------------------------

void VfxPass::create(const VulkanDevice& device, vk::Format color_format, vk::Format depth_format,
                     std::uint32_t frames_in_flight) {
    destroy();
    device_ = &device;
    frames_ = frames_in_flight;
    full_lighting_ = !device.compatMode();
    const auto& dev = device.handle();
    const std::uint32_t binding_count = full_lighting_ ? kLightBindings : kCompatLightBindings;
    // Ranuras de textura: las que admite el dispositivo por etapa (con sitio
    // para el depth, las sombras y algo mas), entre 16 y kMaxTextures.
    {
        const vk::PhysicalDeviceLimits& limits = device.limits();
        const std::uint32_t per_stage = std::min({limits.maxPerStageDescriptorSamplers,
                                                  limits.maxPerStageDescriptorSampledImages,
                                                  limits.maxDescriptorSetSamplers, limits.maxDescriptorSetSampledImages});
        texture_limit_ = std::clamp(per_stage > 8u ? per_stage - 8u : 0u, kMinTextures, kMaxTextures);
    }

    // --- Set 0 ---
    {
        using Type = vk::DescriptorType;
        const std::array<Type, kLightBindings> types = {
            Type::eUniformBuffer,        Type::eStorageBuffer,        Type::eStorageBuffer, Type::eStorageBuffer,
            Type::eStorageBuffer,        Type::eStorageBuffer,        Type::eStorageBuffer, Type::eStorageBuffer,
            Type::eCombinedImageSampler, Type::eCombinedImageSampler, Type::eStorageBuffer, Type::eStorageBuffer,
            Type::eStorageBuffer,        Type::eUniformBuffer,        Type::eUniformBuffer, Type::eCombinedImageSampler,
            Type::eStorageBuffer,        Type::eStorageBuffer};
        std::array<vk::DescriptorSetLayoutBinding, kLightBindings + 2> bindings{};
        for (std::uint32_t i = 0; i < binding_count; ++i) {
            // Las de la luz, solo el fragmento (no cuentan en la simulacion ni el vertice).
            const vk::ShaderStageFlags stages = i >= kBindings ? vk::ShaderStageFlags{vk::ShaderStageFlagBits::eFragment} : kAllStages;
            bindings[i] = vk::DescriptorSetLayoutBinding{i, types[i], i == 9 ? texture_limit_ : 1u, stages};
        }
        // El ruido rizado horneado: solo la simulacion.
        bindings[binding_count] = vk::DescriptorSetLayoutBinding{kCurlBinding, vk::DescriptorType::eStorageBuffer, 1u,
                                                                 vk::ShaderStageFlagBits::eCompute};
        // Salida Luz (parametros y candidatas de las luces de particula): solo la simulacion.
        bindings[binding_count + 1] = vk::DescriptorSetLayoutBinding{kParticleLightBinding, vk::DescriptorType::eStorageBuffer,
                                                                     1u, vk::ShaderStageFlagBits::eCompute};
        vk::DescriptorSetLayoutCreateInfo info{};
        info.bindingCount = binding_count + 2;
        info.pBindings = bindings.data();
        set_layout_ = vk::raii::DescriptorSetLayout(dev, info);
    }
    {
        const std::uint32_t light_uniforms = full_lighting_ ? 2u : 1u;
        const std::uint32_t light_storage = full_lighting_ ? 2u : 0u;
        const std::uint32_t light_images = full_lighting_ ? 1u : 0u;
        const std::array<vk::DescriptorPoolSize, 3> sizes = {{
            {vk::DescriptorType::eUniformBuffer, (1 + light_uniforms) * frames_in_flight},
            {vk::DescriptorType::eStorageBuffer, (kStorageBindings + light_storage + 2) * frames_in_flight},  // + el ruido y las luces de particula
            {vk::DescriptorType::eCombinedImageSampler, (1 + texture_limit_ + light_images) * frames_in_flight},
        }};
        vk::DescriptorPoolCreateInfo pool_info{};
        pool_info.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
        pool_info.maxSets = frames_in_flight;
        pool_info.setPoolSizes(sizes);
        pool_ = vk::raii::DescriptorPool(dev, pool_info);
        const std::vector<vk::DescriptorSetLayout> layouts(frames_in_flight, *set_layout_);
        vk::DescriptorSetAllocateInfo alloc{};
        alloc.descriptorPool = *pool_;
        alloc.setSetLayouts(layouts);
        vk::raii::DescriptorSets sets(dev, alloc);
        sets_.clear();
        for (auto& s : sets) sets_.push_back(std::move(s));
    }
    {
        vk::PushConstantRange push{kAllStages, 0, sizeof(Push)};
        vk::PipelineLayoutCreateInfo layout_info{};
        layout_info.setSetLayouts(*set_layout_);
        layout_info.setPushConstantRanges(push);
        layout_ = vk::raii::PipelineLayout(dev, layout_info);
    }
    {
        const vk::raii::ShaderModule module = shaders::loadModule(device, "vfx_sim.comp.spv");
        const TextureSpecialization specialization(texture_limit_);
        vk::ComputePipelineCreateInfo pipeline{};
        pipeline.stage = vk::PipelineShaderStageCreateInfo{{}, vk::ShaderStageFlagBits::eCompute, *module, "main",
                                                           &specialization.info};
        pipeline.layout = *layout_;
        sim_pipeline_ = vk::raii::Pipeline(dev, device.pipelineCache(), pipeline);
    }
    for (std::uint32_t mesh = 0; mesh < 2; ++mesh) {
        draw_pipelines_[mesh * 3 + 0] = createDrawPipeline(color_format, depth_format, VfxBlend::Additive, mesh != 0);
        draw_pipelines_[mesh * 3 + 1] = createDrawPipeline(color_format, depth_format, VfxBlend::Alpha, mesh != 0);
        draw_pipelines_[mesh * 3 + 2] = createDrawPipeline(color_format, depth_format, VfxBlend::Premultiplied, mesh != 0);
    }
    // Opaca y Distorsion: al primer uso (prepare).
    color_format_ = color_format;
    depth_format_ = depth_format;
    distortion_draws_.assign(frames_in_flight, {});

    {
        vk::SamplerCreateInfo info{};
        info.magFilter = vk::Filter::eNearest;
        info.minFilter = vk::Filter::eNearest;
        info.mipmapMode = vk::SamplerMipmapMode::eNearest;
        info.addressModeU = vk::SamplerAddressMode::eClampToEdge;
        info.addressModeV = vk::SamplerAddressMode::eClampToEdge;
        info.addressModeW = vk::SamplerAddressMode::eClampToEdge;
        depth_sampler_ = vk::raii::Sampler(dev, info);
    }
    {
        vk::SamplerCreateInfo info{};
        info.magFilter = vk::Filter::eLinear;
        info.minFilter = vk::Filter::eLinear;
        info.mipmapMode = vk::SamplerMipmapMode::eLinear;
        info.addressModeU = vk::SamplerAddressMode::eClampToEdge;
        info.addressModeV = vk::SamplerAddressMode::eClampToEdge;
        info.addressModeW = vk::SamplerAddressMode::eClampToEdge;
        info.maxLod = VK_LOD_CLAMP_NONE;
        texture_sampler_ = vk::raii::Sampler(dev, info);
    }

    const vk::MemoryPropertyFlags host = vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;
    frame_ubo_.resize(frames_in_flight);
    instance_buf_.resize(frames_in_flight);
    block_buf_.resize(frames_in_flight);
    point_buf_.resize(frames_in_flight);
    readback_.resize(frames_in_flight);
    for (std::uint32_t f = 0; f < frames_in_flight; ++f) {
        frame_ubo_[f].create(device, sizeof(FrameGpu), vk::BufferUsageFlagBits::eUniformBuffer, host);
        instance_buf_[f].create(device, sizeof(InstanceGpu) * kMaxInstances, vk::BufferUsageFlagBits::eStorageBuffer, host);
        block_buf_[f].create(device, sizeof(VfxBlock) * kMaxBlocks, vk::BufferUsageFlagBits::eStorageBuffer, host);
        point_buf_[f].create(device, sizeof(core::Vec4) * 2 * kMaxPoints, vk::BufferUsageFlagBits::eStorageBuffer, host);
        const vk::DeviceSize readback_bytes = sizeof(std::int32_t) * kCountersPerSlot * kMaxInstances;
        try {
            readback_[f].create(device, readback_bytes, vk::BufferUsageFlagBits::eTransferDst,
                                host | vk::MemoryPropertyFlagBits::eHostCached);
        } catch (const std::exception&) {
            readback_[f].create(device, readback_bytes, vk::BufferUsageFlagBits::eTransferDst, host);
        }
    }
    readback_ready_.assign(frames_in_flight, false);
    // Salida Luz: 50 KB por hueco, visibles desde la CPU (la GPU apunta pocas
    // candidatas y la CPU las lee sin copias), a cero (sin luces).
    light_buf_.resize(frames_in_flight);
    for (std::uint32_t f = 0; f < frames_in_flight; ++f) {
        try {
            light_buf_[f].create(device, kLightBufferBytes, vk::BufferUsageFlagBits::eStorageBuffer,
                                 host | vk::MemoryPropertyFlagBits::eHostCached);
        } catch (const std::exception&) {
            light_buf_[f].create(device, kLightBufferBytes, vk::BufferUsageFlagBits::eStorageBuffer, host);
        }
        const std::vector<std::uint8_t> zeros(static_cast<std::size_t>(kLightItemsOffset), 0);
        light_buf_[f].write(zeros.data(), zeros.size());
    }
    light_ready_.assign(frames_in_flight, false);
    light_params_dirty_.assign(frames_in_flight, false);
    light_written_clock_.assign(frames_in_flight, 0.0f);
    light_params_scratch_.assign(kMaxInstances, core::Vec4{});
    draws_.assign(frames_in_flight, {});
    // (Con los de los canales detras: 3 KB; la lectura solo copia las ranuras.)
    counters_.create(device, sizeof(std::int32_t) * kCounterInts,
                     vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eIndirectBuffer |
                         vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst,
                     vk::MemoryPropertyFlagBits::eDeviceLocal);
    // Mallas: visibles desde la CPU (se escriben una vez, en una parte que
    // ningun frame en vuelo lee: sin copias ni esperas).
    mesh_vertices_.create(device, sizeof(core::Vec4) * 2 * kMaxMeshVertices, vk::BufferUsageFlagBits::eStorageBuffer,
                          host);
    mesh_indices_.create(device, sizeof(std::uint32_t) * kMaxMeshIndices, vk::BufferUsageFlagBits::eStorageBuffer, host);
    mesh_vertex_count_ = 0;
    mesh_index_count_ = 0;
    meshes_ = {};
    mesh_slots_used_ = 0;
    // Eventos: solo la GPU (1 MB).
    events_.create(device, sizeof(core::Vec4) * 2 * kMaxEvents * 2 * kMaxEventLists,
                   vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst,
                   vk::MemoryPropertyFlagBits::eDeviceLocal);
    event_list_used_.assign(kMaxEventLists, false);
    channels_ready_ = false;  // los canales, al primer uso (ensureChannels)
    channels_active_ = false;
    channels_were_active_ = false;
    // Para cuando la vista no trae las luces (a cero: sin luces por clusteres).
    {
        dummy_uniform_.create(device, kDummyUniformBytes, vk::BufferUsageFlagBits::eUniformBuffer, host);
        dummy_storage_.create(device, kDummyStorageBytes, vk::BufferUsageFlagBits::eStorageBuffer, host);
        const std::vector<std::uint8_t> zeros(static_cast<std::size_t>(kDummyUniformBytes), 0);
        dummy_uniform_.write(zeros.data(), zeros.size());
        dummy_storage_.write(zeros.data(), static_cast<std::size_t>(kDummyStorageBytes));
    }
    // Ruido rizado horneado (512 KB en la GPU; se hornea una vez, ~10 ms).
    // Si algo falla, la turbulencia sigue con el calculado.
    VulkanBuffer curl_staging;
    curl_ready_ = false;
    try {
        std::vector<core::Vec4> volume;
        bakeVfxCurlVolume(volume);
        const vk::DeviceSize bytes = sizeof(core::Vec4) * volume.size();
        curl_staging.create(device, bytes, vk::BufferUsageFlagBits::eTransferSrc, host);
        curl_staging.write(volume.data(), static_cast<std::size_t>(bytes));
        curl_volume_.create(device, bytes, vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst,
                            vk::MemoryPropertyFlagBits::eDeviceLocal);
        curl_ready_ = true;
    } catch (const std::exception& e) {
        std::cerr << "[VFX] Sin ruido rizado horneado (" << e.what() << "): se calcula por particula\n";
        curl_staging.destroy();
        curl_volume_.destroy();
    }
    device.submitOneTime([&](const vk::raii::CommandBuffer& cmd) {
        cmd.fillBuffer(*counters_.handle(), 0, VK_WHOLE_SIZE, 0u);
        cmd.fillBuffer(*events_.handle(), 0, VK_WHOLE_SIZE, 0u);
        if (curl_ready_) {
            cmd.copyBuffer(*curl_staging.handle(), *curl_volume_.handle(),
                           vk::BufferCopy{0, 0, sizeof(core::Vec4) * kVfxCurlSize * kVfxCurlSize * kVfxCurlSize});
        }
    });
    curl_staging.destroy();
    // Sin volumen, el binding apunta a un buffer de relleno (no se lee).
    if (!curl_ready_) curl_volume_.create(device, kDummyStorageBytes, vk::BufferUsageFlagBits::eStorageBuffer, host);

    const std::uint8_t white[4] = {255, 255, 255, 255};
    white_.create(device, 1, 1, white);
    slot_used_.assign(kMaxInstances, false);
    createPool(kInitialPool);
}

vk::raii::Pipeline VfxPass::createDrawPipeline(vk::Format color_format, vk::Format depth_format, VfxBlend mode,
                                               bool mesh) const {
    const VulkanDevice& device = *device_;
    const vk::raii::ShaderModule vertex = shaders::loadModule(device, "vfx.vert.spv");
    // Distorsion: su propio fragmento (sin luz; escribe el desplazamiento).
    const vk::raii::ShaderModule fragment =
        shaders::loadModule(device, mode == VfxBlend::Distortion ? "vfx_distortion.frag.spv" : "vfx.frag.spv");
    const TextureSpecialization specialization(texture_limit_);  // ranuras de textura (vfx_common.glsl)
    const std::array<vk::PipelineShaderStageCreateInfo, 2> stages = {{
        {{}, vk::ShaderStageFlagBits::eVertex, *vertex, "main", &specialization.info},
        {{}, vk::ShaderStageFlagBits::eFragment, *fragment, "main", &specialization.info},
    }};
    vk::PipelineVertexInputStateCreateInfo vertex_input{};
    vk::PipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.topology = vk::PrimitiveTopology::eTriangleList;
    vk::PipelineViewportStateCreateInfo viewport{};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    vk::PipelineRasterizationStateCreateInfo raster{};
    raster.polygonMode = vk::PolygonMode::eFill;
    // Mallas: sin las caras traseras (no escriben profundidad: con ellas, lo de
    // detras de cada trozo se pintaba encima). Quads y cintas: las dos caras.
    raster.cullMode = mesh ? vk::CullModeFlagBits::eBack : vk::CullModeFlagBits::eNone;
    raster.frontFace = vk::FrontFace::eCounterClockwise;
    raster.lineWidth = 1.0f;
    vk::PipelineMultisampleStateCreateInfo multisample{};
    multisample.rasterizationSamples = vk::SampleCountFlagBits::e1;
    vk::PipelineDepthStencilStateCreateInfo depth{};
    depth.depthTestEnable = VK_TRUE;
    depth.depthWriteEnable = VK_FALSE;
    depth.depthCompareOp = vk::CompareOp::eLessOrEqual;

    // El alfa de la imagen HDR guarda la distancia de lo que se ve: no se toca.
    vk::PipelineColorBlendAttachmentState blend{};
    blend.blendEnable = VK_TRUE;
    switch (mode) {
        case VfxBlend::Additive:
            blend.srcColorBlendFactor = vk::BlendFactor::eOne;
            blend.dstColorBlendFactor = vk::BlendFactor::eOne;
            break;
        case VfxBlend::Alpha:
            blend.srcColorBlendFactor = vk::BlendFactor::eSrcAlpha;
            blend.dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
            break;
        case VfxBlend::Premultiplied:
            blend.srcColorBlendFactor = vk::BlendFactor::eOne;
            blend.dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
            break;
        case VfxBlend::Opaque:
            // Sin mezcla en el color (el alfa, la distancia, sigue igual).
            blend.srcColorBlendFactor = vk::BlendFactor::eOne;
            blend.dstColorBlendFactor = vk::BlendFactor::eZero;
            break;
        case VfxBlend::Distortion:
            // Los desplazamientos se suman (RG16F).
            blend.srcColorBlendFactor = vk::BlendFactor::eOne;
            blend.dstColorBlendFactor = vk::BlendFactor::eOne;
            break;
    }
    blend.colorBlendOp = vk::BlendOp::eAdd;
    blend.srcAlphaBlendFactor = vk::BlendFactor::eZero;
    blend.dstAlphaBlendFactor = vk::BlendFactor::eOne;
    blend.alphaBlendOp = vk::BlendOp::eAdd;
    blend.colorWriteMask = mode == VfxBlend::Distortion
                               ? vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG
                               : vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                                     vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
    vk::PipelineColorBlendStateCreateInfo color_blend{};
    color_blend.setAttachments(blend);
    const std::array<vk::DynamicState, 2> dynamic_states = {vk::DynamicState::eViewport, vk::DynamicState::eScissor};
    vk::PipelineDynamicStateCreateInfo dynamic{};
    dynamic.setDynamicStates(dynamic_states);
    vk::PipelineRenderingCreateInfo rendering{};
    rendering.setColorAttachmentFormats(color_format);
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
    info.pColorBlendState = &color_blend;
    info.pDynamicState = &dynamic;
    info.layout = *layout_;
    return compat::makeGraphicsPipeline(device, info);
}

bool VfxPass::ensurePipelines(VfxBlend blend) {
    if (device_ == nullptr || (blend != VfxBlend::Opaque && blend != VfxBlend::Distortion)) return true;
    std::array<vk::raii::Pipeline, 2>& pipelines = blend == VfxBlend::Opaque ? opaque_pipelines_ : distortion_pipelines_;
    if (*pipelines[0] && *pipelines[1]) return true;
    // Distorsion: el formato del buffer de distorsion (ParticlePass::kDistortionFormat).
    const vk::Format format = blend == VfxBlend::Distortion ? vk::Format::eR16G16Sfloat : color_format_;
    try {
        for (std::uint32_t mesh = 0; mesh < 2; ++mesh) {
            if (!*pipelines[mesh]) pipelines[mesh] = createDrawPipeline(format, depth_format_, blend, mesh != 0);
        }
    } catch (const std::exception& e) {
        std::cerr << "[VFX] No se pudo crear el pipeline " << (blend == VfxBlend::Opaque ? "opaco" : "de distorsion")
                  << ": " << e.what() << "\n";
        return false;
    }
    return *pipelines[0] && *pipelines[1];
}

void VfxPass::destroy() {
    sets_.clear();
    pool_ = nullptr;
    for (VulkanBuffer& b : frame_ubo_) b.destroy();
    for (VulkanBuffer& b : instance_buf_) b.destroy();
    for (VulkanBuffer& b : block_buf_) b.destroy();
    for (VulkanBuffer& b : point_buf_) b.destroy();
    for (VulkanBuffer& b : readback_) b.destroy();
    for (VulkanBuffer& b : light_buf_) b.destroy();
    light_buf_.clear();
    light_ready_.clear();
    light_params_dirty_.clear();
    light_written_clock_.clear();
    light_selector_.clear();
    lights_.clear();
    any_light_ = false;
    mesh_vertices_.destroy();
    mesh_indices_.destroy();
    events_.destroy();
    dummy_uniform_.destroy();
    dummy_storage_.destroy();
    curl_volume_.destroy();
    curl_ready_ = false;
    recycler_.clear();
    mesh_vertex_count_ = 0;
    mesh_index_count_ = 0;
    meshes_ = {};
    mesh_slots_used_ = 0;
    event_list_used_.clear();
    channels_ready_ = false;
    channels_active_ = false;
    channels_were_active_ = false;
    for (std::vector<VfxEventRecord>& list : script_events_) list.clear();
    upload_scratch_.clear();
    instance_scratch_.clear();
    block_scratch_.clear();
    point_scratch_.clear();
    frame_ubo_.clear();
    instance_buf_.clear();
    block_buf_.clear();
    point_buf_.clear();
    readback_.clear();
    readback_ready_.clear();
    draws_.clear();
    particles_.destroy();
    dead_.destroy();
    alive_.destroy();
    counters_.destroy();
    white_.destroy();
    for (VulkanTexture& t : textures_) t.destroy();
    texture_paths_ = {};
    sim_pipeline_ = nullptr;
    for (vk::raii::Pipeline& p : draw_pipelines_) p = nullptr;
    for (vk::raii::Pipeline& p : opaque_pipelines_) p = nullptr;
    for (vk::raii::Pipeline& p : distortion_pipelines_) p = nullptr;
    distortion_draws_.clear();
    layout_ = nullptr;
    set_layout_ = nullptr;
    depth_sampler_ = nullptr;
    texture_sampler_ = nullptr;
    instances_.clear();
    slots_.clear();
    slot_used_.clear();
    free_.clear();
    pool_size_ = 0;
    sim_pending_ = false;
    stats_ = VfxStats{};
    device_ = nullptr;
}

// -----------------------------------------------------------------------------
// Pool de particulas
// -----------------------------------------------------------------------------

void VfxPass::createPool(std::uint32_t size) {
    size = std::clamp(size, kInitialPool, kMaxPool);
    const vk::BufferUsageFlags storage = vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst;
    particles_.create(*device_, sizeof(core::Vec4) * 4 * static_cast<vk::DeviceSize>(size), storage,
                      vk::MemoryPropertyFlagBits::eDeviceLocal);
    dead_.create(*device_, sizeof(std::uint32_t) * static_cast<vk::DeviceSize>(size), storage,
                 vk::MemoryPropertyFlagBits::eDeviceLocal);
    alive_.create(*device_, sizeof(std::uint32_t) * static_cast<vk::DeviceSize>(size), storage,
                  vk::MemoryPropertyFlagBits::eDeviceLocal);
    pool_size_ = size;
    free_.clear();
    free_.push_back(Range{0, size});
    stats_.pool = size;
    stats_.memory_bytes = static_cast<std::uint64_t>(size) * (sizeof(core::Vec4) * 4 + sizeof(std::uint32_t) * 2);
}

bool VfxPass::allocate(std::uint32_t size, std::uint32_t& offset) {
    // Primer hueco que cabe.
    for (std::size_t i = 0; i < free_.size(); ++i) {
        if (free_[i].size < size) continue;
        offset = free_[i].offset;
        free_[i].offset += size;
        free_[i].size -= size;
        if (free_[i].size == 0) free_.erase(free_.begin() + static_cast<std::ptrdiff_t>(i));
        return true;
    }
    return false;
}

void VfxPass::release(std::uint32_t offset, std::uint32_t size) {
    if (size == 0) return;
    free_.push_back(Range{offset, size});
    std::sort(free_.begin(), free_.end(), [](const Range& a, const Range& b) { return a.offset < b.offset; });
    // Juntar los huecos seguidos.
    std::vector<Range> merged;
    for (const Range& r : free_) {
        if (!merged.empty() && merged.back().offset + merged.back().size == r.offset) {
            merged.back().size += r.size;
        } else {
            merged.push_back(r);
        }
    }
    free_ = std::move(merged);
}

void VfxPass::releaseParked(const ParkedSlot& parked) {
    release(parked.offset, parked.capacity);
    if (parked.index < slot_used_.size()) slot_used_[parked.index] = false;
}

bool VfxPass::placeSlot(Slot& slot, std::uint32_t capacity) {
    std::uint32_t offset = 0;
    if (allocate(capacity, offset)) {
        slot.offset = offset;
        slot.capacity = capacity;
        slot.reset = true;
        slot.simulated = false;
        return true;
    }
    // Antes de crecer: fuera las aparcadas (y, si al crecer, sus rangos ya no
    // valdrian).
    ParkedSlot parked;
    bool released_parked = false;
    while (recycler_.evictOldest(parked)) {
        releaseParked(parked);
        released_parked = true;
    }
    if (released_parked && allocate(capacity, offset)) {
        slot.offset = offset;
        slot.capacity = capacity;
        slot.reset = true;
        slot.simulated = false;
        return true;
    }
    // No cabe: pool mas grande (se pierden las particulas de todos; pasa pocas
    // veces y solo al crecer).
    std::uint32_t used = capacity;
    for (const auto& [id, s] : slots_) used += s.capacity;
    if (used > kMaxPool) {
        if (!warned_full_) {
            std::cerr << "[VFX] Sin memoria para mas particulas (maximo " << kMaxPool << " en total)\n";
            warned_full_ = true;
        }
        slot.capacity = 0;
        return false;
    }
    std::uint32_t size = std::max(pool_size_ * 2, kInitialPool);
    while (size < used + used / 4 && size < kMaxPool) size *= 2;
    device_->waitIdle();
    createPool(std::min(size, kMaxPool));
    for (auto& [id, s] : slots_) {
        if (&s == &slot || s.capacity == 0) continue;
        std::uint32_t o = 0;
        if (allocate(s.capacity, o)) {
            s.offset = o;
        } else {
            s.capacity = 0;
        }
        s.reset = true;
        s.simulated = false;
    }
    if (!allocate(capacity, offset)) {
        slot.capacity = 0;
        return false;
    }
    slot.offset = offset;
    slot.capacity = capacity;
    slot.reset = true;
    slot.simulated = false;
    return true;
}

// -----------------------------------------------------------------------------
// Datos de la CPU
// -----------------------------------------------------------------------------

void VfxPass::setInstances(std::vector<VfxInstanceDesc> instances) {
    if (device_ == nullptr) return;
    if (instances.size() > kMaxInstances) instances.resize(kMaxInstances);
    // Los que ya no estan: fuera (su rango y su ranura).
    for (auto it = slots_.begin(); it != slots_.end();) {
        const bool present = std::any_of(instances.begin(), instances.end(),
                                         [&](const VfxInstanceDesc& d) { return d.id == it->first; });
        if (present) {
            ++it;
            continue;
        }
        if (it->second.event_list < event_list_used_.size()) event_list_used_[it->second.event_list] = false;
        // Pool de instancias: las pequenas se aparcan (rango e indice) para
        // el proximo con la misma capacidad; si no caben, fuera.
        const ParkedSlot parked{it->second.index, it->second.offset, it->second.capacity};
        if (parked.capacity == 0 || parked.capacity > kParkMaxCapacity || !recycler_.park(parked.capacity, parked)) {
            releaseParked(parked);
        }
        it = slots_.erase(it);
    }
    // Los nuevos (o con otra capacidad).
    for (VfxInstanceDesc& d : instances) {
        d.capacity = std::clamp<std::uint32_t>(d.capacity, 1u, kMaxCapacity);
        auto it = slots_.find(d.id);
        ParkedSlot reused;
        if (it == slots_.end() && recycler_.take(d.capacity, reused)) {
            // Del pool: su indice y su rango tal cual (RESET lo vacia).
            Slot slot;
            slot.index = reused.index;
            slot.offset = reused.offset;
            slot.capacity = reused.capacity;
            slot.reset = true;
            slot.simulated = false;
            it = slots_.emplace(d.id, slot).first;
        }
        if (it == slots_.end()) {
            auto free_slot = std::find(slot_used_.begin(), slot_used_.end(), false);
            // Sin indices libres: el de una aparcada.
            ParkedSlot oldest;
            if (free_slot == slot_used_.end() && recycler_.evictOldest(oldest)) {
                releaseParked(oldest);
                free_slot = std::find(slot_used_.begin(), slot_used_.end(), false);
            }
            if (free_slot == slot_used_.end()) continue;
            Slot slot;
            slot.index = static_cast<std::uint32_t>(free_slot - slot_used_.begin());
            *free_slot = true;
            it = slots_.emplace(d.id, slot).first;
            placeSlot(it->second, d.capacity);
        } else if (it->second.capacity != d.capacity) {
            release(it->second.offset, it->second.capacity);
            it->second.capacity = 0;
            placeSlot(it->second, d.capacity);
        }
        if (d.reset) it->second.reset = true;
        // Lista de eventos (la conserva mientras viva: los de un frame se
        // leen en el siguiente).
        Slot& slot = it->second;
        if (d.event_target && slot.event_list == kNoEvents) {
            const auto free_list = std::find(event_list_used_.begin(), event_list_used_.end(), false);
            if (free_list != event_list_used_.end()) {
                *free_list = true;
                slot.event_list = static_cast<std::uint32_t>(free_list - event_list_used_.begin());
                slot.reset = true;  // sus contadores de eventos a cero
            } else if (!warned_events_) {
                std::cerr << "[VFX] Demasiados efectos que reciben eventos a la vez (maximo " << kMaxEventLists << ")\n";
                warned_events_ = true;
            }
        } else if (!d.event_target && slot.event_list != kNoEvents) {
            event_list_used_[slot.event_list] = false;
            slot.event_list = kNoEvents;
        }
    }
    // Canales de datos: solo si alguien escucha (si no, ni etapas ni
    // barreras, y lo que apuntaron los scripts se tira).
    bool listening = false;
    for (VfxInstanceDesc& d : instances) {
        if (d.listens.size() > kMaxListens) d.listens.resize(kMaxListens);
        listening = listening || !d.listens.empty();
    }
    if (!listening) {
        for (std::vector<VfxEventRecord>& list : script_events_) list.clear();
    }
    channels_active_ = listening;
    if (channels_active_) ensureChannels();
    instances_ = std::move(instances);
    // Salida Luz: la calidad puede apagarla por efecto (kVfxOutLight) y el
    // tope a 0 la apaga entera.
    any_light_ = false;
    for (VfxInstanceDesc& d : instances_) {
        if ((d.disabled_outputs & kVfxOutLight) != 0u || lightBudget() == 0) d.light = false;
        any_light_ = any_light_ || d.light;
    }
    sim_pending_ = true;
}

void VfxPass::clear() {
    for (auto& [id, slot] : slots_) slot.reset = true;
    for (std::vector<VfxEventRecord>& list : script_events_) list.clear();
}

void VfxPass::pushEvents(std::uint32_t channel, const VfxEventRecord* records, std::size_t count) {
    if (channel >= kMaxChannels || records == nullptr) return;
    std::vector<VfxEventRecord>& list = script_events_[channel];
    const std::size_t room = kMaxChannelEvents - std::min<std::size_t>(list.size(), kMaxChannelEvents);
    list.insert(list.end(), records, records + std::min(count, room));
}

void VfxPass::ensureChannels() {
    if (channels_ready_ || device_ == nullptr) return;
    // Una vez: events_ crece con el sitio de los canales (los eventos de los
    // subemisores de este frame se pierden; sus contadores siguen igual).
    device_->waitIdle();
    events_.create(*device_, sizeof(core::Vec4) * (kChannelEventsBase + kChannelEventsVec4),
                   vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst,
                   vk::MemoryPropertyFlagBits::eDeviceLocal);
    device_->submitOneTime([&](const vk::raii::CommandBuffer& cmd) { cmd.fillBuffer(*events_.handle(), 0, VK_WHOLE_SIZE, 0u); });
    channels_ready_ = true;
    channels_were_active_ = false;  // sus contadores, a cero al grabar
}

int VfxPass::loadTexture(const std::filesystem::path& file) {
    if (device_ == nullptr || file.empty()) return -1;
    std::error_code error;
    const std::filesystem::path key = std::filesystem::weakly_canonical(file, error);
    for (std::uint32_t slot = 0; slot < texture_limit_; ++slot) {
        if (!texture_paths_[slot].empty() && texture_paths_[slot] == key) return static_cast<int>(slot);
    }
    std::uint32_t slot = 0;
    while (slot < texture_limit_ && !texture_paths_[slot].empty()) ++slot;
    if (slot >= texture_limit_) {
        std::cerr << "[VFX] Sin ranuras para mas texturas (maximo " << texture_limit_ << " en este dispositivo)\n";
        return -1;
    }
    asset::ImageRgba8 image;
    if (!asset::loadImageRgba8(file, image, 2048) || image.width == 0 || image.height == 0) {
        std::cerr << "[VFX] No se pudo leer la textura " << file.string() << "\n";
        return -1;
    }
    // La ranura es nueva (no la usa ningun frame en vuelo), pero el set de
    // descriptores se reescribe en cada prepare: no hace falta esperar.
    textures_[slot].create(*device_, image.width, image.height, image.pixels.data());
    texture_paths_[slot] = key;
    return static_cast<int>(slot);
}

int VfxPass::loadMesh(std::uint64_t key, const VfxMeshData& mesh) {
    if (device_ == nullptr || mesh.vertices.empty() || mesh.indices.empty()) return -1;
    for (std::uint32_t slot = 0; slot < mesh_slots_used_; ++slot) {
        if (meshes_[slot].key == key) return static_cast<int>(slot);
    }
    const auto vertex_count = static_cast<std::uint32_t>(mesh.vertices.size() / 2);
    const auto index_count = static_cast<std::uint32_t>(mesh.indices.size() - mesh.indices.size() % 3);
    if (mesh_slots_used_ >= kMaxMeshes || mesh_vertex_count_ + vertex_count > kMaxMeshVertices ||
        mesh_index_count_ + index_count > kMaxMeshIndices || index_count == 0) {
        std::cerr << "[VFX] Sin sitio para la malla de las particulas (" << vertex_count << " vertices; maximo "
                  << kMaxMeshVertices << " entre " << kMaxMeshes << " mallas)\n";
        return -1;
    }
    // Los vertices, tal cual; los indices con el primer vertice sumado y
    // acotados (un indice fuera leeria otra malla).
    mesh_vertices_.write(mesh.vertices.data(), sizeof(core::Vec4) * 2 * vertex_count,
                         sizeof(core::Vec4) * 2 * mesh_vertex_count_);
    std::vector<std::uint32_t> indices(index_count);
    for (std::uint32_t i = 0; i < index_count; ++i) {
        indices[i] = mesh_vertex_count_ + std::min(mesh.indices[i], vertex_count - 1);
    }
    mesh_indices_.write(indices.data(), sizeof(std::uint32_t) * index_count, sizeof(std::uint32_t) * mesh_index_count_);
    MeshSlot& slot = meshes_[mesh_slots_used_];
    slot.key = key;
    slot.first_index = mesh_index_count_;
    slot.index_count = index_count;
    mesh_vertex_count_ += vertex_count;
    mesh_index_count_ += index_count;
    return static_cast<int>(mesh_slots_used_++);
}

// -----------------------------------------------------------------------------
// Grabacion
// -----------------------------------------------------------------------------

void VfxPass::readBack(std::uint32_t frame) {
    if (frame >= readback_.size() || !readback_ready_[frame]) return;
    readback_ready_[frame] = false;
    const auto* counters = static_cast<const std::int32_t*>(readback_[frame].mapped());
    if (counters == nullptr) return;
    std::uint32_t alive = 0;
    for (auto& [id, slot] : slots_) {
        if (!slot.simulated || slot.capacity == 0) continue;
        const std::int32_t value = counters[slot.index * kCountersPerSlot + 1];
        slot.alive = static_cast<std::uint32_t>(std::clamp<std::int32_t>(value, 0, static_cast<std::int32_t>(slot.capacity)));
        alive += slot.alive;
    }
    stats_.alive = alive;
}

bool VfxPass::prepare(std::uint32_t frame, const VfxView& view, bool simulate) {
    if (device_ == nullptr || frame >= frames_) return false;
    readBack(frame);
    draws_[frame].clear();
    if (frame < distortion_draws_.size()) distortion_draws_[frame].clear();
    if (instances_.empty()) {
        stats_.effects = 0;
        stats_.capacity = 0;
        return false;
    }
    simulate = simulate && sim_pending_;
    if (simulate) frame_shift_ = pending_shift_;

    // --- Camara ---
    FrameGpu f{};
    f.view = view.view;
    f.projection = view.projection;
    f.view_projection = view.view_projection;
    f.inverse_view_projection = core::inverse(view.view_projection);
    f.inverse_projection = core::inverse(view.projection);
    const bool has_depth = static_cast<VkImageView>(view.scene_depth) != VK_NULL_HANDLE;
    f.camera = core::Vec4{view.camera_position, has_depth ? 1.0f : 0.0f};
    // Ejes de la camara: las filas de la rotacion de la vista.
    const core::Vec3 right{view.view.m[0][0], view.view.m[1][0], view.view.m[2][0]};
    const core::Vec3 up{view.view.m[0][1], view.view.m[1][1], view.view.m[2][1]};
    f.right = core::Vec4{core::normalize(right), 0.0f};
    f.up = core::Vec4{core::normalize(up), 0.0f};
    const float w = static_cast<float>(std::max(view.extent.width, 1u));
    const float h = static_cast<float>(std::max(view.extent.height, 1u));
    f.viewport = core::Vec4{w, h, 1.0f / w, 1.0f / h};
    f.light = core::Vec4{view.light, 1.0f};
    f.sun = core::Vec4{core::length(view.to_sun) > 1e-6f ? core::normalize(view.to_sun) : core::Vec3{0.0f, 1.0f, 0.0f},
                       0.0f};
    // Luz real: hacen falta las luces y, fuera del modo compatible, tambien
    // las cascadas y su mapa (los clusteres pueden faltar: sin ellos, ninguna).
    const bool has_lights = static_cast<VkBuffer>(view.lights) != VK_NULL_HANDLE &&
                            (!full_lighting_ || (static_cast<VkBuffer>(view.shadows) != VK_NULL_HANDLE &&
                                                 static_cast<VkImageView>(view.shadow_map) != VK_NULL_HANDLE &&
                                                 static_cast<VkSampler>(view.shadow_sampler) != VK_NULL_HANDLE));
    f.ambient = core::Vec4{view.ambient, has_lights ? 1.0f : 0.0f};
    frame_ubo_[frame].write(&f, sizeof(f));

    // --- Efectos, bloques y puntos ---
    // (Los de la clase: sin reservar memoria en cada frame y vista.)
    instance_scratch_.assign(sizeof(InstanceGpu) * kMaxInstances, 0);
    std::vector<VfxBlock>& block_data = block_scratch_;
    std::vector<core::Vec4>& point_data = point_scratch_;
    block_data.clear();
    point_data.clear();
    std::uint32_t capacity_total = 0;
    std::uint32_t effects = 0;
    // Salida Luz: parametros por ranura (a cero: sin luz). Sin luces ni
    // restos de otro frame, ni se tocan.
    const bool light_params = any_light_ || (frame < light_params_dirty_.size() && light_params_dirty_[frame]);
    if (light_params) std::fill(light_params_scratch_.begin(), light_params_scratch_.end(), core::Vec4{});
    for (const VfxInstanceDesc& d : instances_) {
        const auto it = slots_.find(d.id);
        if (it == slots_.end() || it->second.capacity == 0) continue;
        const Slot& slot = it->second;
        // Salida Luz: alcance, intensidad y la parte estable de las vivas que
        // apuntan su luz (potencias de 2: unas 8 candidatas por luz de su
        // maximo; con muchas vivas, la mitad, la cuarta parte...).
        if (d.light && slot.index < light_params_scratch_.size()) {
            const std::uint32_t wanted = std::clamp<std::uint32_t>(d.light_max * 8u, 32u, 256u);
            std::uint32_t share = 1;
            while (share < 1024u && slot.alive > wanted * share) share <<= 1u;
            light_params_scratch_[slot.index] = core::Vec4{std::max(d.light_radius, 0.01f), std::max(d.light_intensity, 0.0f),
                                                           0.0f, 1.0f / static_cast<float>(share)};
        }
        InstanceGpu g{};
        g.local_to_world = d.transform;
        g.world_to_local = core::inverse(d.transform);
        g.transform = d.world_space ? core::Mat4::identity() : d.transform;
        // Puntos de la malla (antes de los bloques: les dan su primer indice).
        std::uint32_t first_point = static_cast<std::uint32_t>(point_data.size() / 2);
        std::uint32_t point_count = 0;
        if (d.points && !d.points->empty()) {
            const std::size_t pairs = d.points->size() / 2;
            const std::size_t room = kMaxPoints - std::min<std::size_t>(point_data.size() / 2, kMaxPoints);
            point_count = static_cast<std::uint32_t>(std::min(pairs, room));
            point_data.insert(point_data.end(), d.points->begin(),
                              d.points->begin() + static_cast<std::ptrdiff_t>(point_count * 2));
        }
        const auto push_blocks = [&](const std::vector<VfxBlock>& list, std::uint32_t& first, std::uint32_t& count) {
            first = static_cast<std::uint32_t>(block_data.size());
            count = 0;
            for (VfxBlock b : list) {
                if (block_data.size() >= kMaxBlocks) break;
                if (b.type == static_cast<std::uint32_t>(VfxBlockType::PositionMesh)) {
                    b.a.x = static_cast<float>(first_point);
                    b.a.y = static_cast<float>(point_count);
                }
                // Eventos: del indice en event_targets a la ranura y la lista
                // del destino (sin destino vivo, no apunta nada).
                if (b.type == static_cast<std::uint32_t>(VfxBlockType::TriggerOnDie) ||
                    b.type == static_cast<std::uint32_t>(VfxBlockType::TriggerOnCollide)) {
                    const std::uint32_t target = b.pad0;
                    b.pad0 = kNoEvents;
                    b.pad1 = kNoEvents;
                    if (target < d.event_targets.size()) {
                        const auto found = slots_.find(d.event_targets[target]);
                        if (found != slots_.end() && found->second.capacity > 0 && found->second.event_list != kNoEvents) {
                            b.pad0 = found->second.index;
                            b.pad1 = found->second.event_list;
                        }
                    }
                }
                block_data.push_back(b);
                ++count;
            }
        };
        push_blocks(d.initialize, g.blocks[0], g.blocks[1]);
        push_blocks(d.update, g.blocks[2], g.blocks[3]);
        // Bloques de escucha: justo detras de los Update (EMIT_CHANNEL los
        // busca ahi; si no caben, mira el tipo y no nace nada).
        bool emits_channel = false;
        for (const VfxBlock& b : d.update) emits_channel = emits_channel || vfxWritesChannel(b.type);
        if (!d.listens.empty()) {
            std::uint32_t listen_first = 0;
            std::uint32_t listen_count = 0;
            push_blocks(d.listens, listen_first, listen_count);
        }
        g.range[0] = slot.offset;
        g.range[1] = slot.capacity;
        g.range[2] = slot.index;
        g.range[3] = std::min(d.spawn_count, slot.capacity);
        g.time = core::Vec4{std::clamp(d.delta_seconds, 0.0f, 0.25f), d.time, 0.0f, 0.0f};
        std::uint32_t bits = 0;
        if (d.world_space) bits |= kWorldSpace;
        if (d.color_over_life) bits |= kColorOverLife;
        if (d.size_over_life) bits |= kSizeOverLife;
        if (d.color_by_speed) bits |= kColorBySpeed;
        if (d.lit) bits |= kLit;
        if (d.interpolate_spawn) bits |= kInterpolate;
        if (emits_channel) bits |= kEmitsChannel;
        if (d.flip_blend) bits |= kFlipBlend;
        if (d.ribbon_ids && d.output == VfxOutput::Ribbon) bits |= kRibbonIds;
        if (baked_curl_ && curl_ready_) bits |= kCurlBaked;
        g.flags[0] = d.seed;
        g.flags[1] = frame_counter_;
        g.flags[2] = bits;
        g.flags[3] = d.texture >= 0 && d.texture < static_cast<int>(texture_limit_) && !texture_paths_[static_cast<std::size_t>(d.texture)].empty()
                         ? static_cast<std::uint32_t>(d.texture)
                         : 0xFFFFFFFFu;
        g.output0 = core::Vec4{static_cast<float>(d.orient), static_cast<float>(d.blend), std::max(d.soft_distance, 0.0f),
                               std::max(d.intensity, 0.0f)};
        g.output1 = core::Vec4{std::max(d.stretch, 0.0f), static_cast<float>(std::max(d.flip_cols, 1u)),
                               static_cast<float>(std::max(d.flip_rows, 1u)), std::max(d.flip_fps, 0.0f)};
        g.speed_slow = d.speed_color_slow;
        g.speed_fast = d.speed_color_fast;
        g.speed_range = core::Vec4{d.speed_min, std::max(d.speed_max, d.speed_min + 1e-3f), std::clamp(d.alpha_clip, 0.0f, 1.0f), 0.0f};
        g.shift = simulate ? core::Vec4{frame_shift_, 0.0f} : core::Vec4{};
        g.emitter_velocity = core::Vec4{d.emitter_velocity, 0.0f};
        for (std::uint32_t i = 0; i < kVfxLutSize; ++i) g.color_lut[i] = d.color_lut[i];
        for (std::uint32_t i = 0; i < kVfxLutSize / 4; ++i) {
            g.size_lut[i] = core::Vec4{d.size_lut[i * 4], d.size_lut[i * 4 + 1], d.size_lut[i * 4 + 2], d.size_lut[i * 4 + 3]};
        }
        // Salida: cinta o malla (sin malla cargada, la malla no dibuja nada).
        const bool has_mesh = d.mesh >= 0 && static_cast<std::uint32_t>(d.mesh) < mesh_slots_used_;
        g.extra[0] = static_cast<std::uint32_t>(d.output);
        g.extra[1] = has_mesh ? meshes_[static_cast<std::size_t>(d.mesh)].first_index : 0u;
        g.extra[2] = has_mesh ? meshes_[static_cast<std::size_t>(d.mesh)].index_count : 0u;
        g.extra[3] = slot.event_list;
        const float tile = std::max(d.ribbon_tile, 1e-3f);
        // Los metros del emisor dan la vuelta cada 1024 repeticiones (la
        // precision de un float; vfx.vert arregla el salto).
        const float travelled = std::fmod(std::max(d.emitter_distance, 0.0f), tile * 1024.0f);
        if (d.output == VfxOutput::Mesh) {
            g.output2 = core::Vec4{0.0f, 0.0f, static_cast<float>(d.mesh_orient), 0.0f};
            g.output3 = core::Vec4{d.mesh_scale, 0.0f};
        } else {
            g.output2 = core::Vec4{tile, travelled, static_cast<float>(d.ribbon_facing), static_cast<float>(d.ribbon_uv)};
            g.output3 = core::Vec4{std::max(d.ribbon_gap, 0.0f), d.ribbon_vector.x, d.ribbon_vector.y, d.ribbon_vector.z};
        }
        // Cinta: suavizado (1 tramo = la cinta de siempre), celda y UV.
        const int pieces = std::clamp(d.ribbon_subdivisions, 1, 16);
        g.ribbon0 = core::Vec4{std::clamp(d.ribbon_tension, 0.0f, 1.0f),
                               static_cast<float>(d.ribbon_adaptive && pieces > 1 ? -pieces : pieces),
                               d.ribbon_atlas_cell >= 0 ? static_cast<float>(d.ribbon_atlas_cell) : -1.0f, 0.0f};
        g.ribbon1 = d.ribbon_uv_transform;
        // Malla: variantes (sin cargar = la principal), LOD, sprite lejano,
        // giro de Fija y escala al azar. Los vertices del dibujo indirecto
        // (BEGIN los toma de extra.z) son los de la variante mas grande: en
        // las demas sobran y vfx.vert los descarta.
        if (d.output == VfxOutput::Mesh && has_mesh) {
            const MeshSlot& main_mesh = meshes_[static_cast<std::size_t>(d.mesh)];
            const std::uint32_t variants = std::clamp<std::uint32_t>(d.mesh_variant_count, 1u, kVfxMeshVariants);
            std::uint32_t vertices = 0;
            for (std::uint32_t v = 0; v < kVfxMeshVariants; ++v) {
                const int s = v == 0 ? d.mesh : d.mesh_variants[v - 1];
                const bool loaded = v < variants && s >= 0 && static_cast<std::uint32_t>(s) < mesh_slots_used_;
                const MeshSlot& m = loaded ? meshes_[static_cast<std::size_t>(s)] : main_mesh;
                g.variant_first[v] = m.first_index;
                g.variant_count[v] = m.index_count;
                if (v < variants) vertices = std::max(vertices, m.index_count);
            }
            // El sprite lejano usa los 12 primeros (un quad por cada cara).
            if (d.mesh_sprite_distance > 0.0f) vertices = std::max(vertices, 12u);
            g.extra[2] = vertices;
            g.mesh0 = core::Vec4{static_cast<float>(variants), std::max(d.mesh_lod_distance, 0.0f),
                                 std::max(d.mesh_sprite_distance, 0.0f), 0.0f};
            g.mesh_rotation = d.mesh_rotation;
            g.mesh_jitter = core::Vec4{std::clamp(d.mesh_scale_jitter.x, 0.0f, 1.0f), std::clamp(d.mesh_scale_jitter.y, 0.0f, 1.0f),
                                       std::clamp(d.mesh_scale_jitter.z, 0.0f, 1.0f), 0.0f};
        }
        // Material (1 y 0 sin el) y la distorsion (px a 1080p; w de mesh0: mapa de alturas).
        g.tint = d.tint;
        g.emissive = core::Vec4{d.emissive, std::clamp(d.distortion, 0.0f, kVfxMaxDistortionPixels)};
        if (d.distortion_height) g.mesh0.w = 1.0f;
        std::memcpy(instance_scratch_.data() + sizeof(InstanceGpu) * slot.index, &g, sizeof(InstanceGpu));
        capacity_total += slot.capacity;
        ++effects;
        // Se dibujan las que ya tienen contadores validos (o los tendran tras
        // la simulacion de este mismo frame).
        if (d.visible && (simulate || (slot.simulated && !slot.reset))) {
            const core::Vec3 position{d.transform.m[3][0], d.transform.m[3][1], d.transform.m[3][2]};
            const bool mesh_draw = d.output == VfxOutput::Mesh;
            // La calidad puede apagar la salida Malla de este tipo de efecto.
            const bool mesh_off = mesh_draw && (d.disabled_outputs & kVfxOutMesh) != 0u;
            if ((!mesh_draw || has_mesh) && !mesh_off) {
                const DrawItem item{slot.index, d.blend, mesh_draw, core::length(position - view.camera_position),
                                    d.sort_order};
                if (d.blend == VfxBlend::Distortion) {
                    // Al buffer de distorsion, no a la imagen. Apagada por la
                    // calidad (o en este tipo de efecto): no se dibuja nada.
                    if (distortionAllowed() && (d.disabled_outputs & kVfxOutDistortion) == 0u &&
                        ensurePipelines(VfxBlend::Distortion)) {
                        distortion_draws_[frame].push_back(item);
                    }
                } else if (d.blend != VfxBlend::Opaque || ensurePipelines(VfxBlend::Opaque)) {
                    draws_[frame].push_back(item);
                }
            }
        }
    }
    instance_buf_[frame].write(instance_scratch_.data(), instance_scratch_.size());
    if (!block_data.empty()) block_buf_[frame].write(block_data.data(), sizeof(VfxBlock) * block_data.size());
    if (!point_data.empty()) point_buf_[frame].write(point_data.data(), sizeof(core::Vec4) * point_data.size());
    if (light_params && frame < light_buf_.size()) {
        light_buf_[frame].write(light_params_scratch_.data(), sizeof(core::Vec4) * light_params_scratch_.size(),
                                kLightParamsOffset);
        light_params_dirty_[frame] = any_light_;
    }
    stats_.effects = effects;
    stats_.capacity = capacity_total;

    // Por sort_order (menor debajo); a igual orden, transparentes de atras
    // adelante y luego las aditivas (vfxDrawBefore).
    std::stable_sort(draws_[frame].begin(), draws_[frame].end(), [](const DrawItem& a, const DrawItem& b) {
        return vfxDrawBefore(VfxDrawKey{a.sort_order, a.blend == VfxBlend::Additive, a.distance},
                             VfxDrawKey{b.sort_order, b.blend == VfxBlend::Additive, b.distance});
    });

    // --- Descriptores (todos, cada vez: el depth puede cambiar de tamano) ---
    const vk::DescriptorBufferInfo ubo{*frame_ubo_[frame].handle(), 0, sizeof(FrameGpu)};
    const std::array<vk::DescriptorBufferInfo, 7> storage = {{
        {*particles_.handle(), 0, VK_WHOLE_SIZE},
        {*dead_.handle(), 0, VK_WHOLE_SIZE},
        {*alive_.handle(), 0, VK_WHOLE_SIZE},
        {*counters_.handle(), 0, VK_WHOLE_SIZE},
        {*instance_buf_[frame].handle(), 0, VK_WHOLE_SIZE},
        {*block_buf_[frame].handle(), 0, VK_WHOLE_SIZE},
        {*point_buf_[frame].handle(), 0, VK_WHOLE_SIZE},
    }};
    const std::array<vk::DescriptorBufferInfo, 3> extra_storage = {{
        {*mesh_vertices_.handle(), 0, VK_WHOLE_SIZE},
        {*mesh_indices_.handle(), 0, VK_WHOLE_SIZE},
        {*events_.handle(), 0, VK_WHOLE_SIZE},
    }};
    const vk::DescriptorImageInfo depth_info =
        has_depth ? vk::DescriptorImageInfo{*depth_sampler_, view.scene_depth, compat::depthReadOnlyLayout()}
                  : vk::DescriptorImageInfo{*depth_sampler_, *white_.view(), vk::ImageLayout::eShaderReadOnlyOptimal};
    std::array<vk::DescriptorImageInfo, kMaxTextures> textures{};
    for (std::uint32_t i = 0; i < texture_limit_; ++i) {
        const bool loaded = !texture_paths_[i].empty();
        textures[i] = vk::DescriptorImageInfo{*texture_sampler_, loaded ? *textures_[i].view() : *white_.view(),
                                              vk::ImageLayout::eShaderReadOnlyOptimal};
    }
    std::array<vk::WriteDescriptorSet, kBindings> writes{};
    for (std::uint32_t i = 0; i < kBindings; ++i) {
        writes[i].dstSet = *sets_[frame];
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
    }
    writes[0].descriptorType = vk::DescriptorType::eUniformBuffer;
    writes[0].pBufferInfo = &ubo;
    for (std::uint32_t i = 0; i < 7; ++i) {
        writes[1 + i].descriptorType = vk::DescriptorType::eStorageBuffer;
        writes[1 + i].pBufferInfo = &storage[i];
    }
    writes[8].descriptorType = vk::DescriptorType::eCombinedImageSampler;
    writes[8].pImageInfo = &depth_info;
    writes[9].descriptorType = vk::DescriptorType::eCombinedImageSampler;
    writes[9].descriptorCount = texture_limit_;
    writes[9].pImageInfo = textures.data();
    for (std::uint32_t i = 0; i < 3; ++i) {
        writes[10 + i].descriptorType = vk::DescriptorType::eStorageBuffer;
        writes[10 + i].pBufferInfo = &extra_storage[i];
    }
    device_->handle().updateDescriptorSets(writes, nullptr);
    // Ruido rizado horneado (o el de relleno si no se pudo crear).
    {
        const vk::DescriptorBufferInfo curl_info{*curl_volume_.handle(), 0, VK_WHOLE_SIZE};
        vk::WriteDescriptorSet curl_write{};
        curl_write.dstSet = *sets_[frame];
        curl_write.dstBinding = kCurlBinding;
        curl_write.descriptorCount = 1;
        curl_write.descriptorType = vk::DescriptorType::eStorageBuffer;
        curl_write.pBufferInfo = &curl_info;
        device_->handle().updateDescriptorSets(curl_write, nullptr);
    }
    // Salida Luz: el buffer de este hueco (siempre: el set lo declara).
    if (frame < light_buf_.size()) {
        const vk::DescriptorBufferInfo light_info{*light_buf_[frame].handle(), 0, VK_WHOLE_SIZE};
        vk::WriteDescriptorSet light_write{};
        light_write.dstSet = *sets_[frame];
        light_write.dstBinding = kParticleLightBinding;
        light_write.descriptorCount = 1;
        light_write.descriptorType = vk::DescriptorType::eStorageBuffer;
        light_write.pBufferInfo = &light_info;
        device_->handle().updateDescriptorSets(light_write, nullptr);
    }

    // Luz real (13..17): lo de la vista o, sin ello, los de relleno a cero
    // (frame.ambient.w = 0 y vfx.frag no los lee).
    {
        const auto buffer_or = [](vk::Buffer buffer, const VulkanBuffer& fallback) {
            return static_cast<VkBuffer>(buffer) != VK_NULL_HANDLE ? buffer : *fallback.handle();
        };
        // Solo lo que lee vfx.frag (el principio de cada uno): GpuLights entero
        // podria pasar de maxUniformBufferRange con VK_WHOLE_SIZE.
        const vk::DescriptorBufferInfo lights_info{buffer_or(view.lights, dummy_uniform_), 0, kVfxLightsRange};
        const vk::DescriptorBufferInfo shadows_info{buffer_or(view.shadows, dummy_uniform_), 0, kVfxShadowsRange};
        const vk::DescriptorBufferInfo cluster_lights_info{buffer_or(view.cluster_lights, dummy_storage_), 0,
                                                           VK_WHOLE_SIZE};
        const vk::DescriptorBufferInfo cluster_grid_info{buffer_or(view.cluster_grid, dummy_storage_), 0, VK_WHOLE_SIZE};
        const vk::DescriptorImageInfo shadow_info{view.shadow_sampler, view.shadow_map, compat::depthReadOnlyLayout()};
        std::array<vk::WriteDescriptorSet, 5> light_writes{};
        std::uint32_t count = 0;
        const auto add_buffer = [&](std::uint32_t binding, vk::DescriptorType type, const vk::DescriptorBufferInfo& info) {
            vk::WriteDescriptorSet& w = light_writes[count++];
            w.dstSet = *sets_[frame];
            w.dstBinding = binding;
            w.descriptorCount = 1;
            w.descriptorType = type;
            w.pBufferInfo = &info;
        };
        add_buffer(13, vk::DescriptorType::eUniformBuffer, lights_info);
        if (full_lighting_) {
            add_buffer(14, vk::DescriptorType::eUniformBuffer, shadows_info);
            add_buffer(16, vk::DescriptorType::eStorageBuffer, cluster_lights_info);
            add_buffer(17, vk::DescriptorType::eStorageBuffer, cluster_grid_info);
            // El mapa de sombras no tiene relleno: el renderizador lo pasa siempre.
            if (static_cast<VkImageView>(view.shadow_map) != VK_NULL_HANDLE &&
                static_cast<VkSampler>(view.shadow_sampler) != VK_NULL_HANDLE) {
                vk::WriteDescriptorSet& w = light_writes[count++];
                w.dstSet = *sets_[frame];
                w.dstBinding = 15;
                w.descriptorCount = 1;
                w.descriptorType = vk::DescriptorType::eCombinedImageSampler;
                w.pImageInfo = &shadow_info;
            }
        }
        device_->handle().updateDescriptorSets(
            vk::ArrayProxy<const vk::WriteDescriptorSet>(count, light_writes.data()), nullptr);
    }
    return effects > 0;
}

void VfxPass::recordSimulate(const vk::raii::CommandBuffer& cmd, std::uint32_t frame) {
    if (device_ == nullptr || !sim_pending_ || frame >= frames_) return;
    sim_pending_ = false;
    pending_shift_ = pending_shift_ - frame_shift_;
    frame_shift_ = core::Vec3{};
    // Paridad de los eventos de este frame: cada frame simulado escribe en
    // una lista y lee la que se escribio en el anterior.
    const std::uint32_t event_parity = frame_counter_ & 1u;
    ++frame_counter_;

    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *sim_pipeline_);
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *layout_, 0, *sets_[frame], nullptr);
    // Lo que leyo el frame anterior (dibujo, lectura) antes de escribir.
    memoryBarrier(cmd,
                  vk::PipelineStageFlagBits2::eVertexShader | vk::PipelineStageFlagBits2::eFragmentShader |
                      vk::PipelineStageFlagBits2::eDrawIndirect | vk::PipelineStageFlagBits2::eCopy,
                  vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eIndirectCommandRead |
                      vk::AccessFlagBits2::eTransferRead,
                  vk::PipelineStageFlagBits2::eComputeShader,
                  vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite);
    // El depth de la escena (escrito por la geometria) se lee en las colisiones.
    memoryBarrier(cmd, vk::PipelineStageFlagBits2::eLateFragmentTests | vk::PipelineStageFlagBits2::eEarlyFragmentTests,
                  vk::AccessFlagBits2::eDepthStencilAttachmentWrite, vk::PipelineStageFlagBits2::eComputeShader,
                  vk::AccessFlagBits2::eShaderSampledRead);

    // Canales de datos: si vuelven a usarse tras un tiempo sin nadie que
    // escuche, sus contadores a cero (si no, se leerian eventos viejos).
    const bool channels = channels_active_ && channels_ready_;
    if (channels && !channels_were_active_) {
        memoryBarrier(cmd, vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eDrawIndirect,
                      vk::AccessFlagBits2::eShaderStorageWrite | vk::AccessFlagBits2::eShaderStorageRead |
                          vk::AccessFlagBits2::eIndirectCommandRead,
                      vk::PipelineStageFlagBits2::eAllTransfer, vk::AccessFlagBits2::eTransferWrite);
        cmd.fillBuffer(*counters_.handle(), sizeof(std::int32_t) * kChannelCounters,
                       sizeof(std::int32_t) * (kCounterInts - kChannelCounters), 0u);
        memoryBarrier(cmd, vk::PipelineStageFlagBits2::eAllTransfer, vk::AccessFlagBits2::eTransferWrite,
                      vk::PipelineStageFlagBits2::eComputeShader,
                      vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite);
    }
    channels_were_active_ = channels;

    // pad: la paridad de los eventos (BEGIN, EMIT_EVENTS, UPDATE) y el bit de
    // los canales. Va en la push constant y no en la instancia: una vista
    // secundaria que llama a prepare() despues reescribe el buffer de
    // instancias de este hueco.
    const auto dispatch = [&](std::uint32_t stage, std::uint32_t slot, std::uint32_t count) {
        Push push{};
        push.stage = stage;
        push.slot = slot;
        push.count = count;
        push.pad = event_parity | (channels ? kPadChannels : 0u);
        cmd.pushConstants<Push>(*layout_, kAllStages, 0, push);
        cmd.dispatch(groups(count), 1, 1);
    };

    // RESET de las nuevas o reiniciadas.
    bool any_reset = false;
    for (const VfxInstanceDesc& d : instances_) {
        auto it = slots_.find(d.id);
        if (it == slots_.end() || it->second.capacity == 0 || !it->second.reset) continue;
        dispatch(kReset, it->second.index, it->second.capacity);
        it->second.reset = false;
        any_reset = true;
    }
    if (any_reset) computeBarrier(cmd);
    // BEGIN: las vivas del dibujo a 0 (todas las ranuras) y los eventos de
    // este frame vacios.
    dispatch(kBegin, 0, kMaxInstances);
    computeBarrier(cmd);
    if (channels) {
        // El dispatch indirecto que escribio BEGIN, y los eventos de los
        // scripts al canal de este frame (detras van los de la GPU: UPDATE
        // suma con atomicos desde ese contador).
        memoryBarrier(cmd, vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderStorageWrite,
                      vk::PipelineStageFlagBits2::eDrawIndirect | vk::PipelineStageFlagBits2::eAllTransfer,
                      vk::AccessFlagBits2::eIndirectCommandRead | vk::AccessFlagBits2::eTransferWrite);
        bool uploaded = false;
        for (std::uint32_t c = 0; c < kMaxChannels; ++c) {
            std::vector<VfxEventRecord>& list = script_events_[c];
            if (list.empty()) continue;
            const auto n = static_cast<std::uint32_t>(std::min<std::size_t>(list.size(), kMaxChannelEvents));
            upload_scratch_.clear();
            for (std::uint32_t i = 0; i < n; ++i) {
                const VfxEventRecord& r = list[i];
                upload_scratch_.push_back(core::Vec4{r.position, r.type});
                upload_scratch_.push_back(core::Vec4{r.velocity, r.value});
                upload_scratch_.push_back(r.color);
            }
            // (updateBuffer: como mucho 64 KB por llamada.)
            const vk::DeviceSize base =
                sizeof(core::Vec4) * (kChannelEventsBase + 3ull * kMaxChannelEvents * (c * 2 + event_parity));
            const vk::DeviceSize bytes = sizeof(core::Vec4) * upload_scratch_.size();
            const auto* data = reinterpret_cast<const std::uint8_t*>(upload_scratch_.data());
            for (vk::DeviceSize done = 0; done < bytes;) {
                const vk::DeviceSize chunk = std::min<vk::DeviceSize>(bytes - done, 65536);
                cmd.updateBuffer<std::uint8_t>(*events_.handle(), base + done,
                                               vk::ArrayProxy<const std::uint8_t>(static_cast<std::uint32_t>(chunk), data + done));
                done += chunk;
            }
            const std::int32_t count = static_cast<std::int32_t>(n);
            cmd.updateBuffer<std::int32_t>(*counters_.handle(), sizeof(std::int32_t) * (kChannelCounters + c * 8 + event_parity),
                                           count);
            list.clear();
            uploaded = true;
        }
        if (uploaded) {
            memoryBarrier(cmd, vk::PipelineStageFlagBits2::eAllTransfer, vk::AccessFlagBits2::eTransferWrite,
                          vk::PipelineStageFlagBits2::eComputeShader,
                          vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite);
        }
    }
    // EMIT
    bool any_emit = false;
    for (const VfxInstanceDesc& d : instances_) {
        const auto it = slots_.find(d.id);
        if (it == slots_.end() || it->second.capacity == 0) continue;
        const std::uint32_t spawn = std::min(d.spawn_count, it->second.capacity);
        if (spawn == 0) continue;
        dispatch(kEmit, it->second.index, spawn);
        any_emit = true;
    }
    // EMIT_EVENTS: los que reciben eventos (los de su otra lista, del frame
    // anterior). Comparten la barrera con EMIT: sacan de la pila con atomicos.
    for (const VfxInstanceDesc& d : instances_) {
        const auto it = slots_.find(d.id);
        if (it == slots_.end() || it->second.capacity == 0 || it->second.event_list == kNoEvents) continue;
        dispatch(kEmitEvents, it->second.index, kMaxEvents);
        any_emit = true;
    }
    // EMIT_CHANNEL: los que escuchan un canal, un hilo por evento del frame
    // anterior (dispatch indirecto desde el contador del canal, sin leer
    // nada en la CPU). Tambien comparten la barrera.
    if (channels) {
        for (const VfxInstanceDesc& d : instances_) {
            if (d.listens.empty()) continue;
            const auto it = slots_.find(d.id);
            if (it == slots_.end() || it->second.capacity == 0) continue;
            for (std::uint32_t k = 0; k < d.listens.size() && k < kMaxListens; ++k) {
                const std::uint32_t channel = d.listens[k].pad0;
                if (channel >= kMaxChannels) continue;
                Push push{};
                push.stage = kEmitChannel;
                push.slot = it->second.index;
                push.count = k;
                push.pad = event_parity | kPadChannels;
                cmd.pushConstants<Push>(*layout_, kAllStages, 0, push);
                cmd.dispatchIndirect(*counters_.handle(), sizeof(std::int32_t) * (kChannelCounters + channel * 8 + 4));
                any_emit = true;
            }
        }
    }
    if (any_emit) computeBarrier(cmd);
    // UPDATE
    for (const VfxInstanceDesc& d : instances_) {
        auto it = slots_.find(d.id);
        if (it == slots_.end() || it->second.capacity == 0) continue;
        dispatch(kUpdate, it->second.index, it->second.capacity);
        it->second.simulated = true;
    }

    // SORT: dentro de cada efecto con mezcla alfa (o premultiplicada), las
    // vivas de atras adelante (entre efectos ya las ordena prepare). La
    // lista de UPDATE sale en el orden de los atomicos y cambia cada frame.
    // Tamano: las vivas leidas (un par de frames tarde) mas lo que puede
    // nacer mientras, sin pasar de la capacidad; si se queda corto, la cola
    // va sin ordenar ese frame. Los pasos de todos los efectos comparten
    // las barreras.
    std::vector<SortJob>& sorts = sort_scratch_;
    sorts.clear();
    std::uint32_t largest = 0;
    for (const VfxInstanceDesc& d : instances_) {
        // Las cintas siempre (sus vivas, por edad: es su forma) y enteras: una
        // cola sin ordenar la rompe. El resto, con mezcla alfa.
        const bool ribbon = d.output == VfxOutput::Ribbon;
        // (La distorsion suma: el orden no importa.)
        const bool unsorted = d.blend == VfxBlend::Additive || d.blend == VfxBlend::Distortion;
        if (!d.visible || (unsorted && !ribbon)) continue;
        const auto it = slots_.find(d.id);
        if (it == slots_.end() || it->second.capacity == 0) continue;
        const Slot& slot = it->second;
        const std::uint64_t wanted = static_cast<std::uint64_t>(slot.alive) +
                                     4ull * std::min(d.spawn_count, slot.capacity) + 256ull;
        const auto bound =
            ribbon ? slot.capacity : static_cast<std::uint32_t>(std::min<std::uint64_t>(wanted, slot.capacity));
        std::uint32_t size = kSortBlock;
        while (size < bound) size <<= 1u;
        sorts.push_back(SortJob{slot.index, size});
        largest = std::max(largest, size);
    }
    if (!sorts.empty()) {
        computeBarrier(cmd);  // las vivas de UPDATE
        const auto sort_step = [&](std::uint32_t stage, std::uint32_t height, const SortJob& job) {
            Push push{};
            push.stage = stage;
            push.slot = job.slot;
            push.count = height;
            push.pad = job.size;
            cmd.pushConstants<Push>(*layout_, kAllStages, 0, push);
            // Un hilo por pareja (size / 2); en los locales, 64 hilos por
            // bloque de 128: los mismos grupos.
            cmd.dispatch(job.size / kSortBlock, 1, 1);
        };
        for (const SortJob& job : sorts) sort_step(kSortLocal, 0, job);
        computeBarrier(cmd);
        for (std::uint32_t h = kSortBlock * 2; h <= largest; h <<= 1u) {
            for (const SortJob& job : sorts) {
                if (h <= job.size) sort_step(kSortFlip, h, job);
            }
            computeBarrier(cmd);
            for (std::uint32_t hh = h >> 1u; hh > kSortBlock; hh >>= 1u) {
                for (const SortJob& job : sorts) {
                    if (h <= job.size) sort_step(kSortDisperse, hh, job);
                }
                computeBarrier(cmd);
            }
            for (const SortJob& job : sorts) {
                if (h <= job.size) sort_step(kSortLocalDisperse, 0, job);
            }
            computeBarrier(cmd);
        }
    }

    // Lectura de los contadores (llega cuando este hueco de frame vuelva).
    memoryBarrier(cmd, vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderStorageWrite,
                  vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferRead);
    cmd.copyBuffer(*counters_.handle(), *readback_[frame].handle(),
                   vk::BufferCopy{0, 0, sizeof(std::int32_t) * kCountersPerSlot * kMaxInstances});
    memoryBarrier(cmd, vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferWrite,
                  vk::PipelineStageFlagBits2::eHost, vk::AccessFlagBits2::eHostRead);
    readback_ready_[frame] = true;
    // Salida Luz: las candidatas de UPDATE, para la CPU cuando vuelva este
    // hueco (gatherLights). El reloj de la simulacion las adelanta despues.
    {
        float step = 0.0f;
        for (const VfxInstanceDesc& d : instances_) step = std::max(step, std::clamp(d.delta_seconds, 0.0f, 0.25f));
        light_clock_ += step;
        light_step_ = step;
    }
    if (any_light_ && frame < light_buf_.size()) {
        memoryBarrier(cmd, vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderStorageWrite,
                      vk::PipelineStageFlagBits2::eHost, vk::AccessFlagBits2::eHostRead);
        light_ready_[frame] = true;
        light_written_clock_[frame] = light_clock_;
    }
    // Para dibujar: particulas y lista de vivas (vertices) y el contador (indirecto).
    memoryBarrier(cmd, vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderStorageWrite,
                  vk::PipelineStageFlagBits2::eVertexShader | vk::PipelineStageFlagBits2::eFragmentShader |
                      vk::PipelineStageFlagBits2::eDrawIndirect,
                  vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eIndirectCommandRead);
}

void VfxPass::recordDraw(const vk::raii::CommandBuffer& cmd, std::uint32_t frame, vk::Extent2D viewport) const {
    if (device_ == nullptr || frame >= draws_.size() || draws_[frame].empty()) return;
    cmd.setViewport(0, vk::Viewport{0.0f, 0.0f, static_cast<float>(viewport.width), static_cast<float>(viewport.height),
                                    0.0f, 1.0f});
    cmd.setScissor(0, vk::Rect2D{vk::Offset2D{0, 0}, viewport});
    int bound = -1;
    for (const DrawItem& item : draws_[frame]) {
        // 0..5 las de siempre; 6/7 la opaca (quads y cintas / mallas).
        const bool opaque = item.blend == VfxBlend::Opaque;
        const auto pipeline = opaque ? 6 + (item.mesh ? 1 : 0) : static_cast<int>(item.blend) + (item.mesh ? 3 : 0);
        if (pipeline != bound) {
            const vk::raii::Pipeline& p = opaque ? opaque_pipelines_[item.mesh ? 1 : 0]
                                                 : draw_pipelines_[static_cast<std::size_t>(pipeline)];
            cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *p);
            cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *layout_, 0, *sets_[frame], nullptr);
            bound = pipeline;
        }
        Push push{};
        push.slot = item.slot;
        cmd.pushConstants<Push>(*layout_, kAllStages, 0, push);
        const vk::DeviceSize offset = sizeof(std::int32_t) * (static_cast<vk::DeviceSize>(item.slot) * kCountersPerSlot + 4);
        cmd.drawIndirect(*counters_.handle(), offset, 1, 16);
    }
}

void VfxPass::recordDistortion(const vk::raii::CommandBuffer& cmd, std::uint32_t frame, vk::Extent2D viewport) const {
    if (device_ == nullptr || !hasDistortion(frame)) return;
    cmd.setViewport(0, vk::Viewport{0.0f, 0.0f, static_cast<float>(viewport.width), static_cast<float>(viewport.height),
                                    0.0f, 1.0f});
    cmd.setScissor(0, vk::Rect2D{vk::Offset2D{0, 0}, viewport});
    int bound = -1;
    for (const DrawItem& item : distortion_draws_[frame]) {
        const int pipeline = item.mesh ? 1 : 0;
        if (pipeline != bound) {
            cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *distortion_pipelines_[static_cast<std::size_t>(pipeline)]);
            cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *layout_, 0, *sets_[frame], nullptr);
            bound = pipeline;
        }
        Push push{};
        push.slot = item.slot;
        cmd.pushConstants<Push>(*layout_, kAllStages, 0, push);
        const vk::DeviceSize offset = sizeof(std::int32_t) * (static_cast<vk::DeviceSize>(item.slot) * kCountersPerSlot + 4);
        cmd.drawIndirect(*counters_.handle(), offset, 1, 16);
    }
}

std::uint32_t VfxPass::aliveCount(std::uint64_t id) const {
    const auto it = slots_.find(id);
    return it != slots_.end() ? it->second.alive : 0u;
}

// -----------------------------------------------------------------------------
// Salida Luz: luces de particula
// -----------------------------------------------------------------------------

namespace {

float lightLuminance(const core::Vec3& c) {
    return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z;
}

VfxLight toLight(const VfxLightCandidate& c) {
    VfxLight l;
    l.position = c.position;
    l.radius = c.radius;
    l.color = c.color;
    l.exponent = c.exponent;
    l.affects_translucent = c.affects_translucent;
    return l;
}

}  // namespace

void VfxLightSelector::select(const std::vector<VfxLightCandidate>& candidates, const std::vector<Emitter>& emitters,
                              const core::Vec3& camera, std::uint32_t budget, float dt, std::vector<VfxLight>& out) {
    out.clear();
    if (budget == 0) {
        tracked_.clear();
        full_ = false;
        return;
    }
    constexpr float kFadeSeconds = 0.1f;
    constexpr float kKeepBonus = 1.5f;  // las que ya lucen: no cambian por un empate
    const float step = std::clamp(dt, 0.0f, 0.25f) / kFadeSeconds;
    const auto lit = [&](std::uint64_t key) {
        const auto it = tracked_.find(key);
        return it != tracked_.end() && !it->second.leaving;
    };

    // Las que ya estaban: posicion y color al dia.
    for (auto& [key, t] : tracked_) t.wanted = false;
    for (const VfxLightCandidate& c : candidates) {
        const auto it = tracked_.find(c.key);
        if (it != tracked_.end()) it->second.light = toLight(c);
    }

    // --- 1) Por efecto: su maximo, por su prioridad (con la clave de
    // desempate: el mismo orden cada frame) ---
    order_.clear();
    for (std::uint32_t i = 0; i < candidates.size(); ++i) {
        const VfxLightCandidate& c = candidates[i];
        if (c.emitter >= emitters.size()) continue;
        const float distance = core::length(c.position - camera);
        float score = emitters[c.emitter].priority == VfxLightPriority::CameraDistance
                          ? 1.0f / (1.0f + distance)
                          : lightLuminance(c.color) * c.radius;
        if (lit(c.key)) score *= kKeepBonus;
        order_.emplace_back(score, i);
    }
    std::sort(order_.begin(), order_.end(), [&](const auto& a, const auto& b) {
        const VfxLightCandidate& ca = candidates[a.second];
        const VfxLightCandidate& cb = candidates[b.second];
        if (ca.emitter != cb.emitter) return ca.emitter < cb.emitter;
        if (a.first != b.first) return a.first > b.first;
        return ca.key < cb.key;
    });
    per_emitter_.assign(emitters.size(), 0u);
    std::size_t kept = 0;
    for (const auto& entry : order_) {
        const VfxLightCandidate& c = candidates[entry.second];
        if (per_emitter_[c.emitter] >= std::max(emitters[c.emitter].max_lights, 1u)) continue;
        ++per_emitter_[c.emitter];
        // --- 2) Entre todos: lo que aporta a la vista (intensidad x alcance^2
        // a su distancia), con la misma ventaja para las que ya lucen ---
        const float distance = std::max(core::length(c.position - camera), std::max(c.radius, 0.01f));
        float score = lightLuminance(c.color) * c.radius * c.radius / (distance * distance);
        if (lit(c.key)) score *= kKeepBonus;
        order_[kept++] = {score, entry.second};
    }
    order_.resize(kept);
    std::sort(order_.begin(), order_.end(), [&](const auto& a, const auto& b) {
        if (a.first != b.first) return a.first > b.first;
        return candidates[a.second].key < candidates[b.second].key;
    });

    // --- 3) Huecos: las del tope siguen o suben; las demas bajan y lo dejan
    // al llegar a 0 (sin su particula, con lo ultimo que se supo). Una nueva
    // entra si hay hueco: de golpe si el tope no estaba lleno (un impacto),
    // subiendo si sustituye a otra ---
    chosen_.clear();
    const std::size_t wanted = std::min<std::size_t>(order_.size(), budget);
    for (std::size_t n = 0; n < wanted; ++n) {
        const std::uint64_t key = candidates[order_[n].second].key;
        const auto it = tracked_.find(key);
        if (it != tracked_.end()) {
            it->second.wanted = true;
        } else {
            chosen_.push_back(order_[n].second);
        }
    }
    for (auto it = tracked_.begin(); it != tracked_.end();) {
        Tracked& t = it->second;
        if (t.wanted) {
            t.leaving = false;
            t.fade = std::min(t.fade + step, 1.0f);
        } else {
            t.leaving = true;
            t.fade -= step;
        }
        if (t.leaving && t.fade <= 0.0f) {
            it = tracked_.erase(it);
        } else {
            ++it;
        }
    }
    const bool was_full = full_;
    for (const std::uint32_t index : chosen_) {
        if (tracked_.size() >= budget) break;
        Tracked t;
        t.fade = was_full ? step : 1.0f;
        t.wanted = true;
        t.light = toLight(candidates[index]);
        tracked_.emplace(candidates[index].key, t);
    }
    full_ = tracked_.size() >= budget;

    for (const auto& [key, t] : tracked_) {
        const float f = std::clamp(t.fade, 0.0f, 1.0f);
        const float k = f * f * (3.0f - 2.0f * f);
        if (k <= 0.0f) continue;
        VfxLight l = t.light;
        l.color = l.color * k;
        out.push_back(l);
    }
}

const std::vector<VfxLight>& VfxPass::gatherLights(std::uint32_t frame, const core::Vec3& camera) {
    // Sin efectos con luz: nada (y sin lo que quedara de antes).
    if (!any_light_ || lightBudget() == 0) {
        if (!lights_.empty() || light_selector_.tracked() > 0) {
            lights_.clear();
            light_selector_.clear();
        }
        return lights_;
    }
    if (frame >= light_buf_.size() || !light_ready_[frame]) return lights_;
    light_ready_[frame] = false;
    const auto* base = static_cast<const std::uint8_t*>(light_buf_[frame].mapped());
    if (base == nullptr) return lights_;

    // Que efecto con luz hay ahora en cada ranura (uno que ya no esta no da luz).
    std::array<std::int32_t, kMaxInstances> emitter_of{};
    emitter_of.fill(-1);
    std::array<const VfxInstanceDesc*, kMaxInstances> descs{};
    light_emitters_.clear();
    for (const VfxInstanceDesc& d : instances_) {
        if (!d.light) continue;
        const auto it = slots_.find(d.id);
        if (it == slots_.end() || it->second.capacity == 0 || it->second.index >= kMaxInstances) continue;
        emitter_of[it->second.index] = static_cast<std::int32_t>(light_emitters_.size());
        descs[light_emitters_.size()] = &d;
        light_emitters_.push_back(VfxLightSelector::Emitter{std::max(d.light_max, 1u), d.light_priority});
    }

    // Las candidatas (vfx_sim.comp VfxLightOut: 3 uvec4 cada una), adelantadas
    // con su velocidad lo que ha simulado el mundo desde que se apuntaron.
    std::uint32_t count = 0;
    std::memcpy(&count, base, sizeof(count));
    count = std::min(count, kMaxLightCandidates);
    const float lead = std::clamp(light_clock_ - light_written_clock_[frame] + light_step_, 0.0f, kLightMaxLead);
    const auto as_float = [](std::uint32_t bits) {
        float value = 0.0f;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    };
    std::array<std::uint32_t, 12> q{};
    light_candidates_.clear();
    for (std::uint32_t n = 0; n < count; ++n) {
        std::memcpy(q.data(), base + kLightItemsOffset + 48ull * n, sizeof(q));
        const std::uint32_t local = q[7] & 0x1FFFFFu;
        const std::uint32_t slot = q[7] >> 21u;
        if (slot >= kMaxInstances || emitter_of[slot] < 0) continue;
        const VfxInstanceDesc& d = *descs[static_cast<std::size_t>(emitter_of[slot])];
        VfxLightCandidate c;
        c.key = (d.id << 21u) | local;
        c.emitter = static_cast<std::uint32_t>(emitter_of[slot]);
        const core::Vec3 velocity{as_float(q[8]), as_float(q[9]), as_float(q[10])};
        c.position = core::Vec3{as_float(q[0]), as_float(q[1]), as_float(q[2])} + velocity * lead;
        c.radius = as_float(q[3]);
        c.color = core::Vec3{as_float(q[4]), as_float(q[5]), as_float(q[6])};
        if (!std::isfinite(c.position.x) || !std::isfinite(c.position.y) || !std::isfinite(c.position.z) ||
            !std::isfinite(c.radius) || !std::isfinite(c.color.x + c.color.y + c.color.z) || c.radius <= 0.0f) {
            continue;
        }
        c.exponent = d.light_falloff == VfxLightFalloff::Linear ? std::max(d.light_exponent, 0.01f) : 0.0f;
        c.affects_translucent = d.light_translucent;
        light_candidates_.push_back(c);
    }
    const float dt = light_clock_ - light_gather_clock_;
    light_gather_clock_ = light_clock_;
    light_selector_.select(light_candidates_, light_emitters_, camera, lightBudget(), dt, lights_);
    return lights_;
}

// -----------------------------------------------------------------------------
// Ruido rizado horneado
// -----------------------------------------------------------------------------

core::Vec3 vfxCurlReference(const core::Vec3& q) {
    // Rotacional por diferencias centradas (como curlNoise de vfx_sim.comp).
    constexpr float e = 0.05f;
    const core::Vec3 dx = curlPotential(q + core::Vec3{e, 0.0f, 0.0f}) - curlPotential(q - core::Vec3{e, 0.0f, 0.0f});
    const core::Vec3 dy = curlPotential(q + core::Vec3{0.0f, e, 0.0f}) - curlPotential(q - core::Vec3{0.0f, e, 0.0f});
    const core::Vec3 dz = curlPotential(q + core::Vec3{0.0f, 0.0f, e}) - curlPotential(q - core::Vec3{0.0f, 0.0f, e});
    const float k = 1.0f / (2.0f * e);
    return core::Vec3{(dy.z - dz.y) * k, (dz.x - dx.z) * k, (dx.y - dy.x) * k};
}

void bakeVfxCurlVolume(std::vector<core::Vec4>& out) {
    const std::uint32_t n = kVfxCurlSize;
    out.assign(static_cast<std::size_t>(n) * n * n, core::Vec4{});
    const float step = 1.0f / kVfxCurlTexelsPerUnit;
    for (std::uint32_t z = 0; z < n; ++z) {
        for (std::uint32_t y = 0; y < n; ++y) {
            for (std::uint32_t x = 0; x < n; ++x) {
                const core::Vec3 q{static_cast<float>(x) * step, static_cast<float>(y) * step, static_cast<float>(z) * step};
                out[(static_cast<std::size_t>(z) * n + y) * n + x] = core::Vec4{vfxCurlReference(q), 0.0f};
            }
        }
    }
}

core::Vec3 sampleVfxCurlVolume(const std::vector<core::Vec4>& volume, const core::Vec3& q) {
    const std::uint32_t n = kVfxCurlSize;
    if (volume.size() != static_cast<std::size_t>(n) * n * n) return core::Vec3{};
    const core::Vec3 t = q * kVfxCurlTexelsPerUnit;
    const float fx = std::floor(t.x), fy = std::floor(t.y), fz = std::floor(t.z);
    const int ix = static_cast<int>(fx), iy = static_cast<int>(fy), iz = static_cast<int>(fz);
    const float ux = t.x - fx, uy = t.y - fy, uz = t.z - fz;
    const auto at = [&](int x, int y, int z) {
        const auto w = [n](int v) { return static_cast<std::size_t>(static_cast<std::uint32_t>(v) & (n - 1u)); };
        const core::Vec4& v = volume[(w(z) * n + w(y)) * n + w(x)];
        return core::Vec3{v.x, v.y, v.z};
    };
    const auto mix = [](const core::Vec3& a, const core::Vec3& b, float k) { return a + (b - a) * k; };
    const core::Vec3 x00 = mix(at(ix, iy, iz), at(ix + 1, iy, iz), ux);
    const core::Vec3 x10 = mix(at(ix, iy + 1, iz), at(ix + 1, iy + 1, iz), ux);
    const core::Vec3 x01 = mix(at(ix, iy, iz + 1), at(ix + 1, iy, iz + 1), ux);
    const core::Vec3 x11 = mix(at(ix, iy + 1, iz + 1), at(ix + 1, iy + 1, iz + 1), ux);
    return mix(mix(x00, x10, uy), mix(x01, x11, uy), uz);
}

}  // namespace cramion::gfx
