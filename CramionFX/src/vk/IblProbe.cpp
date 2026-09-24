#include "CramionFX/vk/IblProbe.h"

#include "CramionFX/vk/VulkanDevice.h"
#include "CramionFX/vk/VulkanImage.h"

#include <algorithm>
#include <array>
#include <iostream>

namespace cramion::gfx {
namespace {

// Debe coincidir con el bloque de push constants de ibl_prefilter.comp e
// ibl_sh.comp.
struct IblPush {
    core::Vec4 light_radiance{};  // a = rugosidad del nivel
    core::Vec4 to_light{};
    std::uint32_t face_size = 0;
    std::uint32_t sample_count = 0;
    float hdr = 0.0f;  // 1 = el entorno es el mapa HDR
    float wet = 0.0f;  // fraccion del suelo cubierta de agua
};

// Muestras GGX por texel: el cielo es suave (el disco del sol no esta en la
// LUT), asi que con pocas basta y no aparecen destellos.
constexpr std::uint32_t kPrefilterSamples = 64;

void imageBarrier(const vk::raii::CommandBuffer& cmd, vk::Image image, std::uint32_t mips,
                  std::uint32_t layers, vk::ImageLayout old_layout, vk::ImageLayout new_layout,
                  vk::PipelineStageFlags2 src_stage, vk::AccessFlags2 src_access,
                  vk::PipelineStageFlags2 dst_stage, vk::AccessFlags2 dst_access) {
    vk::ImageMemoryBarrier2 barrier{};
    barrier.srcStageMask = src_stage;
    barrier.srcAccessMask = src_access;
    barrier.dstStageMask = dst_stage;
    barrier.dstAccessMask = dst_access;
    barrier.oldLayout = old_layout;
    barrier.newLayout = new_layout;
    barrier.image = image;
    barrier.subresourceRange =
        vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, mips, 0, layers};

    vk::DependencyInfo dependency{};
    dependency.setImageMemoryBarriers(barrier);
    cmd.pipelineBarrier2(dependency);
}

}  // namespace

IblProbe::Image IblProbe::createImage(const VulkanDevice& device,
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

void IblProbe::create(const VulkanDevice& device, const VulkanImage& sky_lut) {
    destroy();

    const vk::ImageUsageFlags usage =
        vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled;

    // --- Cubo de entorno con mips ---
    vk::ImageCreateInfo environment_info{};
    environment_info.flags = vk::ImageCreateFlagBits::eCubeCompatible;
    environment_info.imageType = vk::ImageType::e2D;
    environment_info.format = kFormat;
    environment_info.extent = vk::Extent3D{kEnvironmentSize, kEnvironmentSize, 1};
    environment_info.mipLevels = kEnvironmentMips;
    environment_info.arrayLayers = 6;
    environment_info.samples = vk::SampleCountFlagBits::e1;
    environment_info.tiling = vk::ImageTiling::eOptimal;
    environment_info.usage = usage;
    environment_info.initialLayout = vk::ImageLayout::eUndefined;
    environment_ = createImage(device, environment_info);

    vk::ImageViewCreateInfo cube_view{};
    cube_view.image = *environment_.image;
    cube_view.viewType = vk::ImageViewType::eCube;
    cube_view.format = kFormat;
    cube_view.subresourceRange =
        vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, kEnvironmentMips, 0, 6};
    environment_view_ = vk::raii::ImageView(device.handle(), cube_view);

    for (std::uint32_t mip = 0; mip < kEnvironmentMips; ++mip) {
        vk::ImageViewCreateInfo mip_view{};
        mip_view.image = *environment_.image;
        mip_view.viewType = vk::ImageViewType::e2DArray;
        mip_view.format = kFormat;
        mip_view.subresourceRange =
            vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, mip, 1, 0, 6};
        environment_mip_views_.emplace_back(device.handle(), mip_view);
    }

    // --- LUT de la BRDF ---
    vk::ImageCreateInfo brdf_info = environment_info;
    brdf_info.flags = {};
    brdf_info.extent = vk::Extent3D{kBrdfLutSize, kBrdfLutSize, 1};
    brdf_info.mipLevels = 1;
    brdf_info.arrayLayers = 1;
    brdf_ = createImage(device, brdf_info);

    vk::ImageViewCreateInfo brdf_view{};
    brdf_view.image = *brdf_.image;
    brdf_view.viewType = vk::ImageViewType::e2D;
    brdf_view.format = kFormat;
    brdf_view.subresourceRange =
        vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    brdf_view_ = vk::raii::ImageView(device.handle(), brdf_view);

    // --- Irradiancia (9 coeficientes vec4) ---
    const std::array<core::Vec4, 9> zero{};
    irradiance_ = VulkanBuffer::createDeviceLocal(device, zero.data(), sizeof(zero),
                                                  vk::BufferUsageFlagBits::eStorageBuffer);

    // Trilineal: la rugosidad elige un mip fraccionario del cubo.
    vk::SamplerCreateInfo sampler_info{};
    sampler_info.magFilter = vk::Filter::eLinear;
    sampler_info.minFilter = vk::Filter::eLinear;
    sampler_info.mipmapMode = vk::SamplerMipmapMode::eLinear;
    sampler_info.addressModeU = vk::SamplerAddressMode::eClampToEdge;
    sampler_info.addressModeV = vk::SamplerAddressMode::eClampToEdge;
    sampler_info.addressModeW = vk::SamplerAddressMode::eClampToEdge;
    sampler_info.maxLod = static_cast<float>(kEnvironmentMips);
    sampler_ = vk::raii::Sampler(device.handle(), sampler_info);

    // --- Pipelines ---
    using Type = vk::DescriptorType;
    // (Binding 2: el mapa de entorno HDR, si la escena trae uno.)
    const std::array<Type, 3> prefilter_bindings = {Type::eCombinedImageSampler,
                                                    Type::eStorageImage,
                                                    Type::eCombinedImageSampler};
    const std::array<Type, 3> sh_bindings = {Type::eCombinedImageSampler, Type::eStorageBuffer,
                                             Type::eCombinedImageSampler};
    const std::array<Type, 1> brdf_bindings = {Type::eStorageImage};

    ComputePassDesc prefilter{};
    prefilter.shader = "ibl_prefilter.comp.spv";
    prefilter.bindings = prefilter_bindings;
    prefilter.push_constant_size = sizeof(IblPush);
    prefilter_pass_.create(device, prefilter);

    ComputePassDesc sh{};
    sh.shader = "ibl_sh.comp.spv";
    sh.bindings = sh_bindings;
    sh.push_constant_size = sizeof(IblPush);
    sh_pass_.create(device, sh);

    ComputePassDesc brdf{};
    brdf.shader = "brdf_lut.comp.spv";
    brdf.bindings = brdf_bindings;
    brdf_pass_.create(device, brdf);

    // --- Descriptores ---
    const std::array<vk::DescriptorPoolSize, 3> pool_sizes = {
        vk::DescriptorPoolSize{Type::eCombinedImageSampler, (kEnvironmentMips + 1) * 2},
        vk::DescriptorPoolSize{Type::eStorageImage, kEnvironmentMips + 1},
        vk::DescriptorPoolSize{Type::eStorageBuffer, 1}};
    vk::DescriptorPoolCreateInfo pool_info{};
    pool_info.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    pool_info.maxSets = kEnvironmentMips + 2;
    pool_info.setPoolSizes(pool_sizes);
    pool_ = vk::raii::DescriptorPool(device.handle(), pool_info);

    const auto allocate = [&](const ComputePass& pass, std::uint32_t count) {
        const std::vector<vk::DescriptorSetLayout> layouts(count, *pass.descriptorSetLayout());
        vk::DescriptorSetAllocateInfo alloc{};
        alloc.descriptorPool = *pool_;
        alloc.setSetLayouts(layouts);
        return vk::raii::DescriptorSets(device.handle(), alloc);
    };
    prefilter_sets_ = allocate(prefilter_pass_, kEnvironmentMips);
    sh_sets_ = allocate(sh_pass_, 1);
    brdf_sets_ = allocate(brdf_pass_, 1);

    vk::DescriptorImageInfo sky_info{};
    sky_info.sampler = *prefilter_pass_.sampler();
    sky_info.imageView = *sky_lut.view();
    sky_info.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;

    const auto write_image = [&](const vk::raii::DescriptorSet& set, std::uint32_t binding,
                                 Type type, const vk::DescriptorImageInfo& info) {
        vk::WriteDescriptorSet write{};
        write.dstSet = *set;
        write.dstBinding = binding;
        write.descriptorType = type;
        write.setImageInfo(info);
        device.handle().updateDescriptorSets(write, nullptr);
    };

    for (std::uint32_t mip = 0; mip < kEnvironmentMips; ++mip) {
        write_image(prefilter_sets_[mip], 0, Type::eCombinedImageSampler, sky_info);
        // Hasta que haya un HDR, la LUT del cielo ocupa su hueco (no se lee).
        write_image(prefilter_sets_[mip], 2, Type::eCombinedImageSampler, sky_info);

        vk::DescriptorImageInfo storage{};
        storage.imageView = *environment_mip_views_[mip];
        storage.imageLayout = vk::ImageLayout::eGeneral;
        write_image(prefilter_sets_[mip], 1, Type::eStorageImage, storage);
    }

    write_image(sh_sets_[0], 0, Type::eCombinedImageSampler, sky_info);
    write_image(sh_sets_[0], 2, Type::eCombinedImageSampler, sky_info);
    vk::DescriptorBufferInfo irradiance_info{};
    irradiance_info.buffer = *irradiance_.handle();
    irradiance_info.range = VK_WHOLE_SIZE;
    vk::WriteDescriptorSet sh_write{};
    sh_write.dstSet = *sh_sets_[0];
    sh_write.dstBinding = 1;
    sh_write.descriptorType = Type::eStorageBuffer;
    sh_write.setBufferInfo(irradiance_info);
    device.handle().updateDescriptorSets(sh_write, nullptr);

    vk::DescriptorImageInfo brdf_storage{};
    brdf_storage.imageView = *brdf_view_;
    brdf_storage.imageLayout = vk::ImageLayout::eGeneral;
    write_image(brdf_sets_[0], 0, Type::eStorageImage, brdf_storage);

    brdf_ready_ = false;
    std::cout << "[Vulkan] IBL listo: cubo de " << kEnvironmentSize << " px con "
              << kEnvironmentMips << " niveles de rugosidad\n";
}

void IblProbe::destroy() {
    brdf_sets_.clear();
    sh_sets_.clear();
    prefilter_sets_.clear();
    pool_ = nullptr;
    brdf_pass_.destroy();
    sh_pass_.destroy();
    prefilter_pass_.destroy();
    sampler_ = nullptr;
    irradiance_.destroy();
    brdf_view_ = nullptr;
    brdf_ = Image{};
    environment_mip_views_.clear();
    environment_view_ = nullptr;
    environment_ = Image{};
    brdf_ready_ = false;
}

void IblProbe::setEnvironment(const VulkanDevice& device, vk::ImageView view,
                              vk::Sampler sampler) {
    vk::DescriptorImageInfo info{};
    info.sampler = sampler;
    info.imageView = view;
    info.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;

    std::vector<vk::WriteDescriptorSet> writes;
    for (const vk::raii::DescriptorSet& set : prefilter_sets_) {
        vk::WriteDescriptorSet write{};
        write.dstSet = *set;
        write.dstBinding = 2;
        write.descriptorType = vk::DescriptorType::eCombinedImageSampler;
        write.setImageInfo(info);
        writes.push_back(write);
    }
    vk::WriteDescriptorSet sh_write{};
    sh_write.dstSet = *sh_sets_[0];
    sh_write.dstBinding = 2;
    sh_write.descriptorType = vk::DescriptorType::eCombinedImageSampler;
    sh_write.setImageInfo(info);
    writes.push_back(sh_write);
    device.handle().updateDescriptorSets(writes, nullptr);
}

void IblProbe::record(const vk::raii::CommandBuffer& cmd, const core::Vec3& light_radiance,
                      const core::Vec3& to_light, bool hdr, float wet) {
    using Stage = vk::PipelineStageFlagBits2;
    using Access = vk::AccessFlagBits2;

    // --- LUT de la BRDF: solo el primer frame ---
    if (!brdf_ready_) {
        imageBarrier(cmd, *brdf_.image, 1, 1, vk::ImageLayout::eUndefined,
                     vk::ImageLayout::eGeneral, Stage::eTopOfPipe, Access::eNone,
                     Stage::eComputeShader, Access::eShaderStorageWrite);

        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *brdf_pass_.pipeline());
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *brdf_pass_.layout(), 0,
                               *brdf_sets_[0], nullptr);
        cmd.dispatch((kBrdfLutSize + 7) / 8, (kBrdfLutSize + 7) / 8, 1);

        imageBarrier(cmd, *brdf_.image, 1, 1, vk::ImageLayout::eGeneral,
                     vk::ImageLayout::eShaderReadOnlyOptimal, Stage::eComputeShader,
                     Access::eShaderStorageWrite, Stage::eFragmentShader,
                     Access::eShaderSampledRead);
        brdf_ready_ = true;
    }

    // --- El cubo se reescribe entero (el frame anterior lo leyo) ---
    imageBarrier(cmd, *environment_.image, kEnvironmentMips, 6, vk::ImageLayout::eUndefined,
                 vk::ImageLayout::eGeneral, Stage::eFragmentShader, Access::eShaderSampledRead,
                 Stage::eComputeShader, Access::eShaderStorageWrite);

    // Los armonicos los leyo tambien el frame anterior.
    {
        vk::MemoryBarrier2 barrier{};
        barrier.srcStageMask = Stage::eFragmentShader;
        barrier.srcAccessMask = Access::eShaderStorageRead;
        barrier.dstStageMask = Stage::eComputeShader;
        barrier.dstAccessMask = Access::eShaderStorageWrite;
        vk::DependencyInfo dependency{};
        dependency.setMemoryBarriers(barrier);
        cmd.pipelineBarrier2(dependency);
    }

    IblPush push{};
    push.light_radiance = core::Vec4{light_radiance.x, light_radiance.y, light_radiance.z, 0.0f};
    push.to_light = core::Vec4{to_light.x, to_light.y, to_light.z, 0.0f};
    push.sample_count = kPrefilterSamples;
    push.hdr = hdr ? 1.0f : 0.0f;
    push.wet = wet;

    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *prefilter_pass_.pipeline());
    for (std::uint32_t mip = 0; mip < kEnvironmentMips; ++mip) {
        const std::uint32_t size = std::max(kEnvironmentSize >> mip, 1u);
        push.light_radiance.w =
            static_cast<float>(mip) / static_cast<float>(kEnvironmentMips - 1);
        push.face_size = size;

        cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *prefilter_pass_.layout(), 0,
                               *prefilter_sets_[mip], nullptr);
        cmd.pushConstants<IblPush>(*prefilter_pass_.layout(), vk::ShaderStageFlagBits::eCompute,
                                   0, push);
        cmd.dispatch((size + 7) / 8, (size + 7) / 8, 6);
    }

    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *sh_pass_.pipeline());
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *sh_pass_.layout(), 0, *sh_sets_[0],
                           nullptr);
    cmd.pushConstants<IblPush>(*sh_pass_.layout(), vk::ShaderStageFlagBits::eCompute, 0, push);
    cmd.dispatch(1, 1, 1);

    // --- Todo pasa a leerse en la pasada de iluminacion ---
    imageBarrier(cmd, *environment_.image, kEnvironmentMips, 6, vk::ImageLayout::eGeneral,
                 vk::ImageLayout::eShaderReadOnlyOptimal, Stage::eComputeShader,
                 Access::eShaderStorageWrite, Stage::eFragmentShader,
                 Access::eShaderSampledRead);

    vk::MemoryBarrier2 barrier{};
    barrier.srcStageMask = Stage::eComputeShader;
    barrier.srcAccessMask = Access::eShaderStorageWrite;
    barrier.dstStageMask = Stage::eFragmentShader;
    barrier.dstAccessMask = Access::eShaderStorageRead;
    vk::DependencyInfo dependency{};
    dependency.setMemoryBarriers(barrier);
    cmd.pipelineBarrier2(dependency);
}

}  // namespace cramion::gfx
