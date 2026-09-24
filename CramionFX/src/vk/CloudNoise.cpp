#include "CramionFX/vk/CloudNoise.h"

#include "CramionFX/vk/ComputePass.h"
#include "CramionFX/vk/VulkanDevice.h"

#include <array>
#include <iostream>

namespace cramion::gfx {

void CloudNoise::create(const VulkanDevice& device) {
    destroy();

    vk::ImageCreateInfo image_info{};
    image_info.imageType = vk::ImageType::e3D;
    image_info.format = kFormat;
    image_info.extent = vk::Extent3D{kSize, kSize, kSize};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = vk::SampleCountFlagBits::e1;
    image_info.tiling = vk::ImageTiling::eOptimal;
    image_info.usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled;
    image_info.initialLayout = vk::ImageLayout::eUndefined;
    image_ = vk::raii::Image(device.handle(), image_info);

    const vk::MemoryRequirements requirements = image_.getMemoryRequirements();
    vk::MemoryAllocateInfo allocate_info{};
    allocate_info.allocationSize = requirements.size;
    allocate_info.memoryTypeIndex = device.findMemoryType(
        requirements.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal);
    memory_ = vk::raii::DeviceMemory(device.handle(), allocate_info);
    image_.bindMemory(*memory_, 0);

    vk::ImageViewCreateInfo view_info{};
    view_info.image = *image_;
    view_info.viewType = vk::ImageViewType::e3D;
    view_info.format = kFormat;
    view_info.subresourceRange = vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    view_ = vk::raii::ImageView(device.handle(), view_info);

    // Repetir en los tres ejes: el ruido es periodico.
    vk::SamplerCreateInfo sampler_info{};
    sampler_info.magFilter = vk::Filter::eLinear;
    sampler_info.minFilter = vk::Filter::eLinear;
    sampler_info.mipmapMode = vk::SamplerMipmapMode::eNearest;
    sampler_info.addressModeU = vk::SamplerAddressMode::eRepeat;
    sampler_info.addressModeV = vk::SamplerAddressMode::eRepeat;
    sampler_info.addressModeW = vk::SamplerAddressMode::eRepeat;
    sampler_ = vk::raii::Sampler(device.handle(), sampler_info);

    // --- Generacion (una vez) ---
    const std::array<vk::DescriptorType, 1> bindings = {vk::DescriptorType::eStorageImage};
    ComputePassDesc desc{};
    desc.shader = "cloud_noise.comp.spv";
    desc.bindings = bindings;
    ComputePass pass;
    pass.create(device, desc);

    const vk::DescriptorPoolSize pool_size{vk::DescriptorType::eStorageImage, 1};
    vk::DescriptorPoolCreateInfo pool_info{};
    pool_info.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    pool_info.maxSets = 1;
    pool_info.setPoolSizes(pool_size);
    const vk::raii::DescriptorPool pool(device.handle(), pool_info);

    const vk::DescriptorSetLayout layout = *pass.descriptorSetLayout();
    vk::DescriptorSetAllocateInfo alloc{};
    alloc.descriptorPool = *pool;
    alloc.setSetLayouts(layout);
    vk::raii::DescriptorSets sets(device.handle(), alloc);

    vk::DescriptorImageInfo storage{};
    storage.imageView = *view_;
    storage.imageLayout = vk::ImageLayout::eGeneral;
    vk::WriteDescriptorSet write{};
    write.dstSet = *sets[0];
    write.dstBinding = 0;
    write.descriptorType = vk::DescriptorType::eStorageImage;
    write.setImageInfo(storage);
    device.handle().updateDescriptorSets(write, nullptr);

    const vk::Image image = *image_;
    device.submitOneTime([&](const vk::raii::CommandBuffer& cmd) {
        vk::ImageMemoryBarrier2 to_general{};
        to_general.srcStageMask = vk::PipelineStageFlagBits2::eNone;
        to_general.dstStageMask = vk::PipelineStageFlagBits2::eComputeShader;
        to_general.dstAccessMask = vk::AccessFlagBits2::eShaderStorageWrite;
        to_general.oldLayout = vk::ImageLayout::eUndefined;
        to_general.newLayout = vk::ImageLayout::eGeneral;
        to_general.image = image;
        to_general.subresourceRange = view_info.subresourceRange;
        vk::DependencyInfo dependency{};
        dependency.setImageMemoryBarriers(to_general);
        cmd.pipelineBarrier2(dependency);

        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *pass.pipeline());
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *pass.layout(), 0, *sets[0],
                               nullptr);
        cmd.dispatch(kSize / 4, kSize / 4, kSize / 4);

        vk::ImageMemoryBarrier2 to_read = to_general;
        to_read.srcStageMask = vk::PipelineStageFlagBits2::eComputeShader;
        to_read.srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite;
        to_read.dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader;
        to_read.dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead;
        to_read.oldLayout = vk::ImageLayout::eGeneral;
        to_read.newLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
        vk::DependencyInfo read_dependency{};
        read_dependency.setImageMemoryBarriers(to_read);
        cmd.pipelineBarrier2(read_dependency);
    });

    sets.clear();
    pass.destroy();
    std::cout << "[Vulkan] Ruido de las nubes generado: " << kSize << "^3\n";
}

void CloudNoise::destroy() {
    sampler_ = nullptr;
    view_ = nullptr;
    image_ = nullptr;
    memory_ = nullptr;
}

}  // namespace cramion::gfx
