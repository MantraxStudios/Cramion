#include "CramionFX/vk/EnvironmentMap.h"

#include "CramionFX/vk/VulkanBuffer.h"
#include "CramionFX/vk/VulkanDevice.h"

#include <stb_image.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

namespace cramion::gfx {
namespace {

// Radiancia media que se busca para el cielo de la foto: la del cielo fisico
// del motor, que ilumina la sombra con ~1/6 del sol (intensidad 2.4).
constexpr float kTargetSkyRadiance = 0.4f;
// El sol de la foto se recorta a este multiplo del brillo medio del cielo:
// sigue viendose como un disco muy brillante (y hace bloom), pero su luz
// directa la pone la luz direccional.
constexpr float kMaxRadianceOverSky = 60.0f;

float luminance(const float* rgb) {
    return 0.2126f * rgb[0] + 0.7152f * rgb[1] + 0.0722f * rgb[2];
}

// float de 32 bits -> half (con redondeo, sin NaN especiales).
std::uint16_t toHalf(float value) {
    const auto bits = std::bit_cast<std::uint32_t>(value);
    const std::uint32_t sign = (bits >> 16) & 0x8000u;
    const std::int32_t exponent = static_cast<std::int32_t>((bits >> 23) & 0xFFu) - 127 + 15;
    std::uint32_t mantissa = bits & 0x7FFFFFu;
    if (exponent <= 0) {
        if (exponent < -10) {
            return static_cast<std::uint16_t>(sign);
        }
        mantissa |= 0x800000u;
        const auto shift = static_cast<std::uint32_t>(14 - exponent);
        return static_cast<std::uint16_t>(sign | ((mantissa + (1u << (shift - 1))) >> shift));
    }
    if (exponent >= 31) {
        return static_cast<std::uint16_t>(sign | 0x7BFFu);  // el mayor finito
    }
    const std::uint32_t rounded = mantissa + 0x1000u;
    if (rounded & 0x800000u) {
        return static_cast<std::uint16_t>(sign | (static_cast<std::uint32_t>(exponent + 1) << 10));
    }
    return static_cast<std::uint16_t>(sign | (static_cast<std::uint32_t>(exponent) << 10) |
                                      (rounded >> 13));
}

}  // namespace

bool EnvironmentMap::load(const VulkanDevice& device, const std::filesystem::path& path) {
    destroy();

    // --- Lectura (a memoria: asi valen rutas con cualquier caracter) ---
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        std::cerr << "[Entorno] No se pudo abrir " << path.string() << "\n";
        return false;
    }
    const std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(file)),
                                           std::istreambuf_iterator<char>());
    int source_width = 0;
    int source_height = 0;
    int channels = 0;
    float* pixels = stbi_loadf_from_memory(bytes.data(), static_cast<int>(bytes.size()),
                                           &source_width, &source_height, &channels, 3);
    if (pixels == nullptr) {
        std::cerr << "[Entorno] No se pudo leer " << path.string() << ": "
                  << stbi_failure_reason() << "\n";
        return false;
    }

    // --- Reduccion a kMaxWidth (media de bloques) ---
    const int factor = std::max(1, source_width / static_cast<int>(kMaxWidth));
    const int width = source_width / factor;
    const int height = source_height / factor;
    std::vector<float> radiance(static_cast<std::size_t>(width) * height * 3, 0.0f);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            float* out = &radiance[(static_cast<std::size_t>(y) * width + x) * 3];
            for (int dy = 0; dy < factor; ++dy) {
                for (int dx = 0; dx < factor; ++dx) {
                    const float* in =
                        &pixels[(static_cast<std::size_t>(y * factor + dy) * source_width +
                                 (x * factor + dx)) *
                                3];
                    out[0] += in[0];
                    out[1] += in[1];
                    out[2] += in[2];
                }
            }
            const float inverse = 1.0f / static_cast<float>(factor * factor);
            out[0] *= inverse;
            out[1] *= inverse;
            out[2] *= inverse;
        }
    }
    stbi_image_free(pixels);

    const auto direction_of = [&](float x, float y) {
        const float phi = ((x + 0.5f) / static_cast<float>(width) - 0.5f) * 2.0f * core::kPi;
        const float theta = (y + 0.5f) / static_cast<float>(height) * core::kPi;
        return core::Vec3{std::sin(theta) * std::cos(phi), std::cos(theta),
                          std::sin(theta) * std::sin(phi)};
    };

    // --- Brillo del cielo (hemisferio superior, sin el sol) y el sol ---
    float max_luminance = 0.0f;
    for (int y = 0; y < height / 2; ++y) {
        for (int x = 0; x < width; ++x) {
            max_luminance = std::max(
                max_luminance, luminance(&radiance[(static_cast<std::size_t>(y) * width + x) * 3]));
        }
    }
    double sky_sum = 0.0;
    double sky_weight = 0.0;
    core::Vec3 sun_sum{};
    for (int y = 0; y < height / 2; ++y) {
        // Angulo solido de la fila (sin theta).
        const float solid_angle =
            std::sin((static_cast<float>(y) + 0.5f) / static_cast<float>(height) * core::kPi);
        for (int x = 0; x < width; ++x) {
            const float l = luminance(&radiance[(static_cast<std::size_t>(y) * width + x) * 3]);
            if (l > 0.5f * max_luminance) {
                sun_sum += direction_of(static_cast<float>(x), static_cast<float>(y)) *
                           (l * solid_angle);
            } else {
                sky_sum += static_cast<double>(l * solid_angle);
                sky_weight += solid_angle;
            }
        }
    }
    const float sky_average = static_cast<float>(sky_sum / std::max(sky_weight, 1e-6));
    sun_direction_ = core::dot(sun_sum, sun_sum) > 0.0f ? core::normalize(sun_sum)
                                                        : core::Vec3{0.0f, 1.0f, 0.0f};
    // Que no quede rasante: las cascadas de sombra se estirarian sin fin.
    sun_direction_.y = std::max(sun_direction_.y, 0.1f);
    sun_direction_ = core::normalize(sun_direction_);

    const float scale = kTargetSkyRadiance / std::max(sky_average, 1e-6f);
    const float max_radiance = kMaxRadianceOverSky * sky_average;

    // --- A half, escalado y con el sol recortado ---
    std::vector<std::uint16_t> texels(static_cast<std::size_t>(width) * height * 4);
    for (std::size_t i = 0; i < static_cast<std::size_t>(width) * height; ++i) {
        float rgb[3] = {radiance[i * 3], radiance[i * 3 + 1], radiance[i * 3 + 2]};
        const float l = luminance(rgb);
        const float clamp = l > max_radiance ? max_radiance / l : 1.0f;
        for (int c = 0; c < 3; ++c) {
            texels[i * 4 + c] = toHalf(std::max(rgb[c], 0.0f) * clamp * scale);
        }
        texels[i * 4 + 3] = toHalf(1.0f);
    }

    // --- Imagen con todos los mips ---
    width_ = static_cast<std::uint32_t>(width);
    const auto mip_levels = static_cast<std::uint32_t>(
        std::bit_width(static_cast<std::uint32_t>(std::max(width, height))));

    vk::ImageCreateInfo image_info{};
    image_info.imageType = vk::ImageType::e2D;
    image_info.format = kFormat;
    image_info.extent = vk::Extent3D{static_cast<std::uint32_t>(width),
                                     static_cast<std::uint32_t>(height), 1};
    image_info.mipLevels = mip_levels;
    image_info.arrayLayers = 1;
    image_info.samples = vk::SampleCountFlagBits::e1;
    image_info.tiling = vk::ImageTiling::eOptimal;
    image_info.usage = vk::ImageUsageFlagBits::eTransferSrc |
                       vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled;
    image_info.initialLayout = vk::ImageLayout::eUndefined;
    image_ = vk::raii::Image(device.handle(), image_info);

    const vk::MemoryRequirements requirements = image_.getMemoryRequirements();
    vk::MemoryAllocateInfo allocate_info{};
    allocate_info.allocationSize = requirements.size;
    allocate_info.memoryTypeIndex = device.findMemoryType(
        requirements.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal);
    memory_ = vk::raii::DeviceMemory(device.handle(), allocate_info);
    image_.bindMemory(*memory_, 0);

    VulkanBuffer staging;
    const vk::DeviceSize size = texels.size() * sizeof(std::uint16_t);
    staging.create(device, size, vk::BufferUsageFlagBits::eTransferSrc,
                   vk::MemoryPropertyFlagBits::eHostVisible |
                       vk::MemoryPropertyFlagBits::eHostCoherent);
    staging.write(texels.data(), size);

    const vk::Image image = *image_;
    device.submitOneTime([&](const vk::raii::CommandBuffer& cmd) {
        const auto barrier = [&](std::uint32_t mip, std::uint32_t count, vk::ImageLayout from,
                                 vk::ImageLayout to, vk::PipelineStageFlags2 src_stage,
                                 vk::AccessFlags2 src_access, vk::PipelineStageFlags2 dst_stage,
                                 vk::AccessFlags2 dst_access) {
            vk::ImageMemoryBarrier2 b{};
            b.srcStageMask = src_stage;
            b.srcAccessMask = src_access;
            b.dstStageMask = dst_stage;
            b.dstAccessMask = dst_access;
            b.oldLayout = from;
            b.newLayout = to;
            b.image = image;
            b.subresourceRange =
                vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, mip, count, 0, 1};
            vk::DependencyInfo dependency{};
            dependency.setImageMemoryBarriers(b);
            cmd.pipelineBarrier2(dependency);
        };
        using Stage = vk::PipelineStageFlagBits2;
        using Access = vk::AccessFlagBits2;

        barrier(0, mip_levels, vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
                Stage::eNone, Access::eNone, Stage::eTransfer, Access::eTransferWrite);
        vk::BufferImageCopy region{};
        region.imageSubresource = vk::ImageSubresourceLayers{vk::ImageAspectFlagBits::eColor, 0, 0, 1};
        region.imageExtent = image_info.extent;
        cmd.copyBufferToImage(*staging.handle(), image, vk::ImageLayout::eTransferDstOptimal,
                              region);

        // Mips: cada nivel, la mitad del anterior (filtro lineal).
        auto level_width = static_cast<std::int32_t>(width);
        auto level_height = static_cast<std::int32_t>(height);
        for (std::uint32_t mip = 1; mip < mip_levels; ++mip) {
            barrier(mip - 1, 1, vk::ImageLayout::eTransferDstOptimal,
                    vk::ImageLayout::eTransferSrcOptimal, Stage::eTransfer, Access::eTransferWrite,
                    Stage::eTransfer, Access::eTransferRead);
            const std::int32_t next_width = std::max(level_width / 2, 1);
            const std::int32_t next_height = std::max(level_height / 2, 1);
            vk::ImageBlit blit{};
            blit.srcSubresource =
                vk::ImageSubresourceLayers{vk::ImageAspectFlagBits::eColor, mip - 1, 0, 1};
            blit.srcOffsets[1] = vk::Offset3D{level_width, level_height, 1};
            blit.dstSubresource = vk::ImageSubresourceLayers{vk::ImageAspectFlagBits::eColor, mip, 0, 1};
            blit.dstOffsets[1] = vk::Offset3D{next_width, next_height, 1};
            cmd.blitImage(image, vk::ImageLayout::eTransferSrcOptimal, image,
                          vk::ImageLayout::eTransferDstOptimal, blit, vk::Filter::eLinear);
            level_width = next_width;
            level_height = next_height;
        }
        barrier(0, mip_levels - 1, vk::ImageLayout::eTransferSrcOptimal,
                vk::ImageLayout::eShaderReadOnlyOptimal, Stage::eTransfer, Access::eTransferRead,
                Stage::eFragmentShader | Stage::eComputeShader, Access::eShaderSampledRead);
        barrier(mip_levels - 1, 1, vk::ImageLayout::eTransferDstOptimal,
                vk::ImageLayout::eShaderReadOnlyOptimal, Stage::eTransfer, Access::eTransferWrite,
                Stage::eFragmentShader | Stage::eComputeShader, Access::eShaderSampledRead);
    });
    staging.destroy();

    vk::ImageViewCreateInfo view_info{};
    view_info.image = image;
    view_info.viewType = vk::ImageViewType::e2D;
    view_info.format = kFormat;
    view_info.subresourceRange =
        vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, mip_levels, 0, 1};
    view_ = vk::raii::ImageView(device.handle(), view_info);

    // Repite en horizontal (el acimut da la vuelta), fijo en vertical.
    vk::SamplerCreateInfo sampler_info{};
    sampler_info.magFilter = vk::Filter::eLinear;
    sampler_info.minFilter = vk::Filter::eLinear;
    sampler_info.mipmapMode = vk::SamplerMipmapMode::eLinear;
    sampler_info.addressModeU = vk::SamplerAddressMode::eRepeat;
    sampler_info.addressModeV = vk::SamplerAddressMode::eClampToEdge;
    sampler_info.addressModeW = vk::SamplerAddressMode::eClampToEdge;
    sampler_info.maxLod = VK_LOD_CLAMP_NONE;
    sampler_ = vk::raii::Sampler(device.handle(), sampler_info);

    std::cout << "[Entorno] " << path.filename().string() << ": " << width << "x" << height
              << ", " << mip_levels << " mips, escala " << scale << ", sol hacia ("
              << sun_direction_.x << ", " << sun_direction_.y << ", " << sun_direction_.z
              << ")\n";
    return true;
}

void EnvironmentMap::destroy() {
    sampler_ = nullptr;
    view_ = nullptr;
    image_ = nullptr;
    memory_ = nullptr;
    width_ = 0;
}

}  // namespace cramion::gfx
