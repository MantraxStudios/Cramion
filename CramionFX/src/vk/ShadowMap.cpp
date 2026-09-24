#include "CramionFX/vk/ShadowMap.h"

#include "CramionFX/vk/VulkanDevice.h"

#include <iostream>

namespace cramion::gfx {

void ShadowMap::create(const VulkanDevice& device) {
    destroy();

    depth_.create(device, extent(), device.depthFormat(),
                  vk::ImageUsageFlagBits::eDepthStencilAttachment |
                      vk::ImageUsageFlagBits::eSampled,
                  vk::ImageAspectFlagBits::eDepth, scene::kShadowCascadeCount);

    vk::SamplerCreateInfo sampler_info{};
    // Filtrado lineal + comparacion = PCF de 2x2 hecho por la GPU.
    sampler_info.magFilter = vk::Filter::eLinear;
    sampler_info.minFilter = vk::Filter::eLinear;
    sampler_info.mipmapMode = vk::SamplerMipmapMode::eNearest;

    // Fuera del mapa no hay sombra: el borde blanco (profundidad maxima) hace
    // que la comparacion siempre salga "iluminado".
    sampler_info.addressModeU = vk::SamplerAddressMode::eClampToBorder;
    sampler_info.addressModeV = vk::SamplerAddressMode::eClampToBorder;
    sampler_info.addressModeW = vk::SamplerAddressMode::eClampToBorder;
    sampler_info.borderColor = vk::BorderColor::eFloatOpaqueWhite;

    sampler_info.compareEnable = VK_TRUE;
    sampler_info.compareOp = vk::CompareOp::eLessOrEqual;

    sampler_ = vk::raii::Sampler(device.handle(), sampler_info);

    std::cout << "[Vulkan] Mapa de sombras creado: " << scene::kShadowCascadeCount
              << " cascadas de " << kResolution << "x" << kResolution << " ("
              << vk::to_string(depth_.format()) << ")\n";
}

void ShadowMap::destroy() {
    sampler_ = nullptr;
    depth_.destroy();
}

}  // namespace cramion::gfx
