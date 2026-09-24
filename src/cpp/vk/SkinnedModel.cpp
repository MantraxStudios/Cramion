#include "vk/SkinnedModel.h"

#include "vk/SkinnedPass.h"
#include "vk/VulkanDevice.h"

#include <array>
#include <iostream>

namespace cramion::gfx {
namespace {

// Formato de Vulkan de una textura comprimida del DDS. Variantes UNORM: el
// shader linealiza el color base a mano.
vk::Format blockFormat(asset::TextureFormat format) {
    switch (format) {
        case asset::TextureFormat::Bc1: return vk::Format::eBc1RgbaUnormBlock;
        case asset::TextureFormat::Bc2: return vk::Format::eBc2UnormBlock;
        case asset::TextureFormat::Bc3: return vk::Format::eBc3UnormBlock;
        case asset::TextureFormat::Bc4: return vk::Format::eBc4UnormBlock;
        case asset::TextureFormat::Bc5: return vk::Format::eBc5UnormBlock;
        case asset::TextureFormat::Bc7: return vk::Format::eBc7UnormBlock;
        case asset::TextureFormat::Rgba8: break;
    }
    return vk::Format::eR8G8B8A8Unorm;
}

}  // namespace

void SkinnedModel::create(const VulkanDevice& device, const asset::ModelData& model,
                          const SkinnedPass& pass) {
    destroy();

    vertices_ = VulkanBuffer::createDeviceLocal(
        device, model.vertices.data(), sizeof(asset::SkinnedVertex) * model.vertices.size(),
        vk::BufferUsageFlagBits::eVertexBuffer);
    indices_ = VulkanBuffer::createDeviceLocal(device, model.indices.data(),
                                               sizeof(std::uint32_t) * model.indices.size(),
                                               vk::BufferUsageFlagBits::eIndexBuffer);
    index_count_ = static_cast<std::uint32_t>(model.indices.size());
    submeshes_ = model.submeshes;
    rigid_ = model.animations.empty();

    // --- Texturas, con los texeles por defecto al final ---
    textures_.reserve(model.textures.size() + 3);
    for (const asset::TextureData& texture : model.textures) {
        if (texture.format == asset::TextureFormat::Rgba8) {
            textures_.emplace_back().create(device, texture.width, texture.height,
                                            texture.pixels.data());
        } else {
            textures_.emplace_back().createCompressed(
                device, texture.width, texture.height, blockFormat(texture.format),
                asset::blockBytes(texture.format), texture.mip_levels, texture.pixels.data(),
                texture.pixels.size());
        }
    }
    const auto add_texel = [&](std::uint8_t r, std::uint8_t g, std::uint8_t b) {
        const std::array<std::uint8_t, 4> texel = {r, g, b, 255};
        textures_.emplace_back().create(device, 1, 1, texel.data());
        return textures_.size() - 1;
    };
    const std::size_t white_index = add_texel(255, 255, 255);
    // (0.5, 0.5, 1) en espacio tangente = la normal de la malla sin cambios.
    const std::size_t flat_normal_index = add_texel(128, 128, 255);
    const std::size_t black_index = add_texel(0, 0, 0);

    // --- Un descriptor set por material ---
    const auto material_count = static_cast<std::uint32_t>(model.materials.size());

    vk::DescriptorPoolSize pool_size{vk::DescriptorType::eCombinedImageSampler,
                                     material_count * SkinnedPass::kMaterialTextureCount};
    vk::DescriptorPoolCreateInfo pool_info{};
    pool_info.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    pool_info.maxSets = material_count;
    pool_info.setPoolSizes(pool_size);
    pool_ = vk::raii::DescriptorPool(device.handle(), pool_info);

    const std::vector<vk::DescriptorSetLayout> layouts(material_count, *pass.materialSetLayout());
    vk::DescriptorSetAllocateInfo alloc_info{};
    alloc_info.descriptorPool = *pool_;
    alloc_info.setSetLayouts(layouts);
    material_sets_ = vk::raii::DescriptorSets(device.handle(), alloc_info);

    const auto pick = [](std::int32_t texture, std::size_t fallback) {
        return (texture >= 0) ? static_cast<std::size_t>(texture) : fallback;
    };

    materials_.reserve(material_count);
    for (std::uint32_t m = 0; m < material_count; ++m) {
        const asset::MaterialData& material = model.materials[m];

        Material gpu{};
        gpu.base_color = material.base_color;
        gpu.emissive = core::Vec4{material.emissive.x, material.emissive.y, material.emissive.z,
                                  0.0f};
        // El signo de la escala del normal map dice su convenio (ver
        // skinned.frag): positivo = OpenGL, negativo = DirectX.
        gpu.params = core::Vec4{material.metallic, material.roughness, material.occlusion_strength,
                                material.normal_map_directx ? -material.normal_scale
                                                            : material.normal_scale};
        gpu.reflectance = material.reflectance;
        gpu.transparent = material.transparent;
        materials_.push_back(gpu);

        // Sin textura emisiva, el factor multiplica al blanco: un material
        // emisivo liso (glTF sin mapa) sigue brillando.
        const bool emits =
            material.emissive.x > 0.0f || material.emissive.y > 0.0f || material.emissive.z > 0.0f;

        // Mismo orden que los bindings del set 1 (ver SkinnedPass).
        const std::array<std::size_t, SkinnedPass::kMaterialTextureCount> textures = {
            pick(material.albedo_texture, white_index),
            pick(material.metallic_roughness_texture, white_index),
            pick(material.normal_texture, flat_normal_index),
            pick(material.occlusion_texture, white_index),
            pick(material.emissive_texture, emits ? white_index : black_index),
        };

        std::array<vk::DescriptorImageInfo, SkinnedPass::kMaterialTextureCount> infos{};
        for (std::uint32_t t = 0; t < SkinnedPass::kMaterialTextureCount; ++t) {
            infos[t].sampler = *pass.sampler();
            infos[t].imageView = *textures_[textures[t]].view();
            infos[t].imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
        }

        std::array<vk::WriteDescriptorSet, SkinnedPass::kMaterialTextureCount> writes{};
        for (std::uint32_t t = 0; t < SkinnedPass::kMaterialTextureCount; ++t) {
            writes[t].dstSet = *material_sets_[m];
            writes[t].dstBinding = t;
            writes[t].descriptorType = vk::DescriptorType::eCombinedImageSampler;
            writes[t].setImageInfo(infos[t]);
        }
        device.handle().updateDescriptorSets(writes, nullptr);
    }

    std::cout << "[Vulkan] Modelo " << model.name << " subido: " << model.textures.size()
              << " texturas con mipmaps, " << material_count << " materiales\n";
}

void SkinnedModel::destroy() {
    material_sets_.clear();
    pool_ = nullptr;
    textures_.clear();
    materials_.clear();
    submeshes_.clear();
    indices_.destroy();
    vertices_.destroy();
    index_count_ = 0;
}

}  // namespace cramion::gfx
