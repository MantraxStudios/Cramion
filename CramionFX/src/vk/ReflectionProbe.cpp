#include "CramionFX/vk/VulkanCompat.h"
#include "CramionFX/vk/ReflectionProbe.h"

#include "CramionFX/vk/VulkanDevice.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <vector>

namespace cramion::gfx {
namespace {

// Debe coincidir con el bloque de push constants de probe_prefilter.comp.
struct PrefilterPush {
    float roughness = 0.0f;
    std::uint32_t face_size = 0;
    std::uint32_t sample_count = 0;
    float capture_size = 0.0f;
};

constexpr std::uint32_t kPrefilterSamples = 64;

vk::ImageMemoryBarrier2 barrier(vk::Image image, std::uint32_t base_mip, std::uint32_t mips,
                                std::uint32_t base_layer, std::uint32_t layers,
                                vk::ImageLayout old_layout, vk::ImageLayout new_layout,
                                vk::PipelineStageFlags2 src_stage, vk::AccessFlags2 src_access,
                                vk::PipelineStageFlags2 dst_stage, vk::AccessFlags2 dst_access) {
    vk::ImageMemoryBarrier2 result{};
    result.srcStageMask = src_stage;
    result.srcAccessMask = src_access;
    result.dstStageMask = dst_stage;
    result.dstAccessMask = dst_access;
    result.oldLayout = old_layout;
    result.newLayout = new_layout;
    result.image = image;
    result.subresourceRange = vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, base_mip,
                                                        mips, base_layer, layers};
    return result;
}

void pipelineBarrier(const vk::raii::CommandBuffer& cmd,
                     vk::ArrayProxy<const vk::ImageMemoryBarrier2> barriers) {
    vk::DependencyInfo dependency{};
    dependency.imageMemoryBarrierCount = barriers.size();
    dependency.pImageMemoryBarriers = barriers.data();
    compat::pipelineBarrier(cmd, dependency);
}

}  // namespace

ReflectionProbe::Image ReflectionProbe::createImage(const VulkanDevice& device,
                                                    const vk::ImageCreateInfo& info) {
    Image result;
    result.image = vk::raii::Image(device.handle(), info);

    const vk::MemoryRequirements requirements = result.image.getMemoryRequirements();
    vk::MemoryAllocateInfo allocate_info{};
    allocate_info.allocationSize = requirements.size;
    allocate_info.memoryTypeIndex = device.findMemoryType(
        requirements.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal);
    result.memory = vk::raii::DeviceMemory(device.handle(), allocate_info);
    result.image.bindMemory(*result.memory, 0);
    return result;
}

void ReflectionProbe::create(const VulkanDevice& device) {
    destroy();

    // --- Captura: cubo con todos los mips (destino y origen de copias) ---
    vk::ImageCreateInfo capture_info{};
    capture_info.flags = vk::ImageCreateFlagBits::eCubeCompatible;
    capture_info.imageType = vk::ImageType::e2D;
    capture_info.format = kFormat;
    capture_info.extent = vk::Extent3D{kCaptureSize, kCaptureSize, 1};
    capture_info.mipLevels = kCaptureMips;
    capture_info.arrayLayers = 6;
    capture_info.samples = vk::SampleCountFlagBits::e1;
    capture_info.tiling = vk::ImageTiling::eOptimal;
    capture_info.usage = vk::ImageUsageFlagBits::eTransferDst |
                         vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eSampled;
    capture_info.initialLayout = vk::ImageLayout::eUndefined;
    capture_ = createImage(device, capture_info);

    vk::ImageViewCreateInfo capture_view{};
    capture_view.image = *capture_.image;
    capture_view.viewType = vk::ImageViewType::eCube;
    capture_view.format = kFormat;
    capture_view.subresourceRange =
        vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, kCaptureMips, 0, 6};
    capture_view_ = vk::raii::ImageView(device.handle(), capture_view);

    // --- Cubos prefiltrados ---
    vk::ImageCreateInfo probe_info = capture_info;
    probe_info.extent = vk::Extent3D{kSize, kSize, 1};
    probe_info.mipLevels = kMips;
    // (Origen de copia: sus distancias a la CPU, recordDistanceReadback.)
    probe_info.usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
                       vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eTransferSrc;
    for (Cube& cube : cubes_) {
        cube.image = createImage(device, probe_info);

        vk::ImageViewCreateInfo probe_view = capture_view;
        probe_view.image = *cube.image.image;
        probe_view.subresourceRange.levelCount = kMips;
        cube.view = vk::raii::ImageView(device.handle(), probe_view);

        for (std::uint32_t mip = 0; mip < kMips; ++mip) {
            vk::ImageViewCreateInfo mip_view{};
            mip_view.image = *cube.image.image;
            mip_view.viewType = vk::ImageViewType::e2DArray;
            mip_view.format = kFormat;
            mip_view.subresourceRange =
                vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, mip, 1, 0, 6};
            cube.mip_views.emplace_back(device.handle(), mip_view);
        }
    }

    // Trilineal con todos los mips: la rugosidad (iluminacion) o el angulo
    // solido de cada muestra (prefiltrado) eligen un mip fraccionario.
    vk::SamplerCreateInfo sampler_info{};
    sampler_info.magFilter = vk::Filter::eLinear;
    sampler_info.minFilter = vk::Filter::eLinear;
    sampler_info.mipmapMode = vk::SamplerMipmapMode::eLinear;
    sampler_info.addressModeU = vk::SamplerAddressMode::eClampToEdge;
    sampler_info.addressModeV = vk::SamplerAddressMode::eClampToEdge;
    sampler_info.addressModeW = vk::SamplerAddressMode::eClampToEdge;
    sampler_info.maxLod = static_cast<float>(kCaptureMips);
    sampler_ = vk::raii::Sampler(device.handle(), sampler_info);

    // --- Prefiltrado ---
    using Type = vk::DescriptorType;
    const std::array<Type, 2> bindings = {Type::eCombinedImageSampler, Type::eStorageImage};
    ComputePassDesc prefilter{};
    prefilter.shader = "probe_prefilter.comp.spv";
    prefilter.bindings = bindings;
    prefilter.push_constant_size = sizeof(PrefilterPush);
    prefilter_pass_.create(device, prefilter);

    const std::array<vk::DescriptorPoolSize, 2> pool_sizes = {
        vk::DescriptorPoolSize{Type::eCombinedImageSampler, kMips * kCubeCount},
        vk::DescriptorPoolSize{Type::eStorageImage, kMips * kCubeCount}};
    vk::DescriptorPoolCreateInfo pool_info{};
    pool_info.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    pool_info.maxSets = kMips * kCubeCount;
    pool_info.setPoolSizes(pool_sizes);
    pool_ = vk::raii::DescriptorPool(device.handle(), pool_info);

    const std::vector<vk::DescriptorSetLayout> layouts(kMips,
                                                       *prefilter_pass_.descriptorSetLayout());
    for (Cube& cube : cubes_) {
        vk::DescriptorSetAllocateInfo alloc{};
        alloc.descriptorPool = *pool_;
        alloc.setSetLayouts(layouts);
        cube.prefilter_sets = vk::raii::DescriptorSets(device.handle(), alloc);

        for (std::uint32_t mip = 0; mip < kMips; ++mip) {
            vk::DescriptorImageInfo capture_image{};
            capture_image.sampler = *sampler_;
            capture_image.imageView = *capture_view_;
            capture_image.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;

            vk::DescriptorImageInfo storage{};
            storage.imageView = *cube.mip_views[mip];
            storage.imageLayout = vk::ImageLayout::eGeneral;

            std::array<vk::WriteDescriptorSet, 2> writes{};
            writes[0].dstSet = *cube.prefilter_sets[mip];
            writes[0].dstBinding = 0;
            writes[0].descriptorType = Type::eCombinedImageSampler;
            writes[0].setImageInfo(capture_image);
            writes[1].dstSet = *cube.prefilter_sets[mip];
            writes[1].dstBinding = 1;
            writes[1].descriptorType = Type::eStorageImage;
            writes[1].setImageInfo(storage);
            device.handle().updateDescriptorSets(writes, nullptr);
        }
    }

    // Los cubos se enlazan a la iluminacion desde el principio, antes de la
    // primera captura (que no los usara hasta estar listos): en negro y como
    // textura.
    device.submitOneTime([&](const vk::raii::CommandBuffer& cmd) {
        using Stage = vk::PipelineStageFlagBits2;
        using Access = vk::AccessFlagBits2;
        for (const Cube& cube : cubes_) {
            const vk::Image probe_image = *cube.image.image;
            pipelineBarrier(cmd,
                            barrier(probe_image, 0, kMips, 0, 6, vk::ImageLayout::eUndefined,
                                    vk::ImageLayout::eTransferDstOptimal, Stage::eNone,
                                    Access::eNone, Stage::eTransfer, Access::eTransferWrite));
            const vk::ClearColorValue black{0.0f, 0.0f, 0.0f, 0.0f};
            const vk::ImageSubresourceRange all{vk::ImageAspectFlagBits::eColor, 0, kMips, 0, 6};
            cmd.clearColorImage(probe_image, vk::ImageLayout::eTransferDstOptimal, black, all);
            pipelineBarrier(cmd, barrier(probe_image, 0, kMips, 0, 6,
                                         vk::ImageLayout::eTransferDstOptimal,
                                         vk::ImageLayout::eShaderReadOnlyOptimal,
                                         Stage::eTransfer, Access::eTransferWrite,
                                         Stage::eFragmentShader, Access::eShaderSampledRead));
        }
    });

    // Distancias de una captura para la CPU: un texel RGBA16F por direccion.
    distance_readback_.create(device, 6ull * kDistanceSize * kDistanceSize * 4 * sizeof(std::uint16_t),
                              vk::BufferUsageFlagBits::eTransferDst,
                              vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);

    std::cout << "[Vulkan] Sonda de reflexion lista: captura de " << kCaptureSize << " px, "
              << kCubeCount << " cubos de " << kSize << " px con " << kMips << " niveles\n";
}

void ReflectionProbe::destroy() {
    distance_readback_.destroy();
    for (Cube& cube : cubes_) {
        cube.prefilter_sets.clear();
        cube.mip_views.clear();
        cube.view = nullptr;
        cube.image = Image{};
    }
    pool_ = nullptr;
    prefilter_pass_.destroy();
    sampler_ = nullptr;
    capture_view_ = nullptr;
    capture_ = Image{};
}

void ReflectionProbe::recordFaceCopy(const vk::raii::CommandBuffer& cmd, vk::Image source,
                                     const vk::Rect2D& region, std::uint32_t face) const {
    using Stage = vk::PipelineStageFlagBits2;
    using Access = vk::AccessFlagBits2;
    const vk::Image capture = *capture_.image;

    // La cara se reescribe entera; la leyo el ultimo prefiltrado.
    pipelineBarrier(cmd, barrier(capture, 0, 1, face, 1, vk::ImageLayout::eUndefined,
                                 vk::ImageLayout::eTransferDstOptimal, Stage::eComputeShader,
                                 Access::eShaderSampledRead, Stage::eBlit,
                                 Access::eTransferWrite));

    // X invertida: la base de la vista (derecha = forward x arriba) es la
    // contraria a la de las caras de un cubo de Vulkan.
    const auto x0 = region.offset.x;
    const auto y0 = region.offset.y;
    const auto x1 = x0 + static_cast<std::int32_t>(region.extent.width);
    const auto y1 = y0 + static_cast<std::int32_t>(region.extent.height);

    vk::ImageBlit blit{};
    blit.srcSubresource = vk::ImageSubresourceLayers{vk::ImageAspectFlagBits::eColor, 0, 0, 1};
    blit.srcOffsets[0] = vk::Offset3D{x1, y0, 0};
    blit.srcOffsets[1] = vk::Offset3D{x0, y1, 1};
    blit.dstSubresource = vk::ImageSubresourceLayers{vk::ImageAspectFlagBits::eColor, 0, face, 1};
    blit.dstOffsets[0] = vk::Offset3D{0, 0, 0};
    blit.dstOffsets[1] = vk::Offset3D{static_cast<std::int32_t>(kCaptureSize),
                                      static_cast<std::int32_t>(kCaptureSize), 1};
    cmd.blitImage(source, vk::ImageLayout::eTransferSrcOptimal, capture,
                  vk::ImageLayout::eTransferDstOptimal, blit, vk::Filter::eLinear);
}

void ReflectionProbe::recordPrefilter(const vk::raii::CommandBuffer& cmd,
                                      std::uint32_t cube) const {
    using Stage = vk::PipelineStageFlagBits2;
    using Access = vk::AccessFlagBits2;
    const vk::Image capture = *capture_.image;
    const vk::Image probe = *cubes_[cube].image.image;

    // --- Mips de la captura, cada uno a partir del anterior ---
    pipelineBarrier(cmd, barrier(capture, 0, 1, 0, 6, vk::ImageLayout::eTransferDstOptimal,
                                 vk::ImageLayout::eTransferSrcOptimal, Stage::eBlit,
                                 Access::eTransferWrite, Stage::eBlit, Access::eTransferRead));
    for (std::uint32_t mip = 1; mip < kCaptureMips; ++mip) {
        pipelineBarrier(cmd, barrier(capture, mip, 1, 0, 6, vk::ImageLayout::eUndefined,
                                     vk::ImageLayout::eTransferDstOptimal, Stage::eComputeShader,
                                     Access::eShaderSampledRead, Stage::eBlit,
                                     Access::eTransferWrite));

        const auto source_size = static_cast<std::int32_t>(std::max(kCaptureSize >> (mip - 1), 1u));
        const auto target_size = static_cast<std::int32_t>(std::max(kCaptureSize >> mip, 1u));
        vk::ImageBlit blit{};
        blit.srcSubresource =
            vk::ImageSubresourceLayers{vk::ImageAspectFlagBits::eColor, mip - 1, 0, 6};
        blit.srcOffsets[1] = vk::Offset3D{source_size, source_size, 1};
        blit.dstSubresource = vk::ImageSubresourceLayers{vk::ImageAspectFlagBits::eColor, mip, 0, 6};
        blit.dstOffsets[1] = vk::Offset3D{target_size, target_size, 1};
        cmd.blitImage(capture, vk::ImageLayout::eTransferSrcOptimal, capture,
                      vk::ImageLayout::eTransferDstOptimal, blit, vk::Filter::eLinear);

        pipelineBarrier(cmd, barrier(capture, mip, 1, 0, 6, vk::ImageLayout::eTransferDstOptimal,
                                     vk::ImageLayout::eTransferSrcOptimal, Stage::eBlit,
                                     Access::eTransferWrite, Stage::eBlit,
                                     Access::eTransferRead));
    }

    // --- La captura pasa a textura y el cubo a destino del compute ---
    pipelineBarrier(
        cmd, {barrier(capture, 0, kCaptureMips, 0, 6, vk::ImageLayout::eTransferSrcOptimal,
                      vk::ImageLayout::eShaderReadOnlyOptimal, Stage::eBlit,
                      Access::eTransferRead | Access::eTransferWrite, Stage::eComputeShader,
                      Access::eShaderSampledRead),
              barrier(probe, 0, kMips, 0, 6, vk::ImageLayout::eUndefined,
                      vk::ImageLayout::eGeneral, Stage::eFragmentShader,
                      Access::eShaderSampledRead, Stage::eComputeShader,
                      Access::eShaderStorageWrite)});

    PrefilterPush push{};
    push.sample_count = kPrefilterSamples;
    push.capture_size = static_cast<float>(kCaptureSize);

    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *prefilter_pass_.pipeline());
    for (std::uint32_t mip = 0; mip < kMips; ++mip) {
        const std::uint32_t size = std::max(kSize >> mip, 1u);
        push.roughness = static_cast<float>(mip) / static_cast<float>(kMips - 1);
        push.face_size = size;

        cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *prefilter_pass_.layout(), 0,
                               *cubes_[cube].prefilter_sets[mip], nullptr);
        cmd.pushConstants<PrefilterPush>(*prefilter_pass_.layout(),
                                         vk::ShaderStageFlagBits::eCompute, 0, push);
        cmd.dispatch((size + 7) / 8, (size + 7) / 8, 6);
    }

    // --- Listo para la iluminacion ---
    pipelineBarrier(cmd, barrier(probe, 0, kMips, 0, 6, vk::ImageLayout::eGeneral,
                                 vk::ImageLayout::eShaderReadOnlyOptimal, Stage::eComputeShader,
                                 Access::eShaderStorageWrite, Stage::eFragmentShader,
                                 Access::eShaderSampledRead));
}

void ReflectionProbe::recordDistanceReadback(const vk::raii::CommandBuffer& cmd, std::uint32_t cube) const {
    if (!distance_readback_.isValid()) return;
    using Stage = vk::PipelineStageFlagBits2;
    using Access = vk::AccessFlagBits2;
    const vk::Image probe = *cubes_[cube].image.image;

    // El nivel pequeno, de textura (lo dejo asi recordPrefilter) a origen de
    // la copia, y de vuelta.
    pipelineBarrier(cmd, barrier(probe, kDistanceMip, 1, 0, 6, vk::ImageLayout::eShaderReadOnlyOptimal,
                                 vk::ImageLayout::eTransferSrcOptimal,
                                 Stage::eComputeShader | Stage::eFragmentShader,
                                 Access::eShaderStorageWrite | Access::eShaderSampledRead, Stage::eTransfer,
                                 Access::eTransferRead));
    vk::BufferImageCopy region{};
    region.imageSubresource = vk::ImageSubresourceLayers{vk::ImageAspectFlagBits::eColor, kDistanceMip, 0, 6};
    region.imageExtent = vk::Extent3D{kDistanceSize, kDistanceSize, 1};
    cmd.copyImageToBuffer(probe, vk::ImageLayout::eTransferSrcOptimal, *distance_readback_.handle(), region);
    pipelineBarrier(cmd, barrier(probe, kDistanceMip, 1, 0, 6, vk::ImageLayout::eTransferSrcOptimal,
                                 vk::ImageLayout::eShaderReadOnlyOptimal, Stage::eTransfer, Access::eTransferRead,
                                 Stage::eFragmentShader, Access::eShaderSampledRead));

    // La copia, visible para la CPU cuando termine (la espera la fence de la
    // captura).
    vk::MemoryBarrier2 to_host{};
    to_host.srcStageMask = Stage::eTransfer;
    to_host.srcAccessMask = Access::eTransferWrite;
    to_host.dstStageMask = Stage::eHost;
    to_host.dstAccessMask = Access::eHostRead;
    vk::DependencyInfo dependency{};
    dependency.setMemoryBarriers(to_host);
    compat::pipelineBarrier(cmd, dependency);
}

namespace {

float halfToFloat(std::uint16_t value) {
    const std::uint32_t exponent = (value >> 10) & 0x1Fu;
    const std::uint32_t mantissa = value & 0x3FFu;
    float result = 0.0f;
    if (exponent == 0) {
        result = std::ldexp(static_cast<float>(mantissa), -24);
    } else if (exponent == 31) {
        result = mantissa == 0 ? std::numeric_limits<float>::infinity() : std::numeric_limits<float>::quiet_NaN();
    } else {
        result = std::ldexp(static_cast<float>(mantissa | 0x400u), static_cast<int>(exponent) - 25);
    }
    return (value & 0x8000u) != 0 ? -result : result;
}

}  // namespace

void ReflectionProbe::readDistances(std::vector<float>& out) const {
    out.clear();
    if (!distance_readback_.isValid() || distance_readback_.mapped() == nullptr) return;
    constexpr std::size_t kTexels = 6u * kDistanceSize * kDistanceSize;
    out.resize(kTexels);
    // RGBA16F por texel, caras una tras otra y filas de arriba a abajo: el
    // alfa (la distancia) es el cuarto.
    const auto* texels = static_cast<const std::uint16_t*>(distance_readback_.mapped());
    for (std::size_t i = 0; i < kTexels; ++i) {
        const float distance = halfToFloat(texels[i * 4 + 3]);
        out[i] = std::isfinite(distance) ? distance : 0.0f;
    }
}

namespace {

// Distancia de la tabla en una direccion (no hace falta normalizarla): cara y
// texel como en la especificacion de Vulkan (tabla de seleccion de caras de
// los cubos); igual que cubeDirection de probe_prefilter.comp.
float texelDistance(const std::vector<float>& distances, std::uint32_t size, float x, float y, float z) {
    const float ax = std::abs(x);
    const float ay = std::abs(y);
    const float az = std::abs(z);
    std::uint32_t face = 0;
    float sc = 0.0f;
    float tc = 0.0f;
    float ma = 0.0f;
    if (ax >= ay && ax >= az) {
        face = x >= 0.0f ? 0u : 1u;
        sc = x >= 0.0f ? -z : z;
        tc = -y;
        ma = ax;
    } else if (ay >= az) {
        face = y >= 0.0f ? 2u : 3u;
        sc = x;
        tc = y >= 0.0f ? z : -z;
        ma = ay;
    } else {
        face = z >= 0.0f ? 4u : 5u;
        sc = z >= 0.0f ? x : -x;
        tc = -y;
        ma = az;
    }
    if (ma <= 0.0f) return -1.0f;
    const auto texel = [size](float t) {
        return std::min(static_cast<std::uint32_t>(std::max(t, 0.0f) * static_cast<float>(size)), size - 1);
    };
    const std::uint32_t u = texel(0.5f * (sc / ma + 1.0f));
    const std::uint32_t v = texel(0.5f * (tc / ma + 1.0f));
    return distances[(face * size + v) * size + u];
}

}  // namespace

float ReflectionProbe::distanceToward(const std::vector<float>& distances, float x, float y, float z,
                                      int half_window) {
    if (distances.size() != 6u * kDistanceSize * kDistanceSize) return -1.0f;
    const float length = std::sqrt(x * x + y * y + z * z);
    if (!(length > 0.0f)) return -1.0f;
    x /= length;
    y /= length;
    z /= length;
    // Lo mas lejos que ve alrededor: con half_window 2, 5 x 5 direcciones
    // separadas un texel del centro de una cara (~3.6 grados; ~14 en total),
    // tambien al otro lado de un borde del cubo. Una persona, un poste u hojas
    // delante de la sonda no son "otra habitacion" (si no, se recapturaba sin
    // parar al andar detras de alguien).
    // Base alrededor de la direccion: t = normalize(cross(eje, d)), b = cross(d, t).
    const bool vertical = std::abs(y) > 0.9f;
    const float ex = vertical ? 1.0f : 0.0f;
    const float ey = vertical ? 0.0f : 1.0f;
    float tx = ey * z;
    float ty = -ex * z;
    float tz = ex * y - ey * x;
    const float t_length = std::sqrt(tx * tx + ty * ty + tz * tz);
    tx /= t_length;
    ty /= t_length;
    tz /= t_length;
    const float bx = y * tz - z * ty;
    const float by = z * tx - x * tz;
    const float bz = x * ty - y * tx;
    const float step = 2.0f / static_cast<float>(kDistanceSize);
    float seen = -1.0f;
    for (int i = -half_window; i <= half_window; ++i) {
        for (int j = -half_window; j <= half_window; ++j) {
            const float a = static_cast<float>(i) * step;
            const float c = static_cast<float>(j) * step;
            seen = std::max(seen, texelDistance(distances, kDistanceSize, x + a * tx + c * bx, y + a * ty + c * by,
                                                z + a * tz + c * bz));
        }
    }
    return seen;
}

}  // namespace cramion::gfx
