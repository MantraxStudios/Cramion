#include "CramionFX/vk/SkinnedModel.h"

#include "CramionFX/asset/Dds.h"
#include "CramionFX/vk/GpuTypes.h"
#include "CramionFX/vk/SkinnedPass.h"
#include "CramionFX/vk/VulkanDevice.h"

#include <meshoptimizer.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <unordered_map>

namespace cramion::gfx {
namespace {

static_assert(sizeof(asset::SkinnedVertex) == 80, "meshlet_common.glsl lee los vertices como 20 floats");

// ¿Cada arista (con los vertices soldados por posicion) la comparten
// exactamente dos triangulos? Entonces la malla es cerrada: lo que mira hacia
// atras siempre queda detras de algo que mira hacia delante.
bool isClosedMesh(const asset::ModelData& model, const std::vector<asset::SubMesh>& submeshes,
                  const std::vector<std::uint32_t>& indices, const std::vector<bool>& transparent) {
    std::size_t triangles = 0;
    for (const asset::SubMesh& submesh : submeshes) triangles += submesh.index_count / 3;
    if (triangles == 0 || triangles > 2'000'000) return false;  // enorme: no se comprueba (tiempo de carga)

    struct PositionHash {
        std::size_t operator()(const std::array<std::uint32_t, 3>& p) const {
            return (static_cast<std::size_t>(p[0]) * 73856093u) ^ (static_cast<std::size_t>(p[1]) * 19349663u) ^
                   (static_cast<std::size_t>(p[2]) * 83492791u);
        }
    };
    // Soldado con tolerancia (1/100000 del tamano del modelo): en las costuras
    // de UV las copias de un vertice pueden diferir en el ultimo bit
    // (cos(2 pi) != 1) y la malla parecia abierta.
    core::Vec3 low = model.vertices[0].position;
    core::Vec3 high = low;
    for (const asset::SkinnedVertex& v : model.vertices) {
        low = core::Vec3{std::min(low.x, v.position.x), std::min(low.y, v.position.y), std::min(low.z, v.position.z)};
        high = core::Vec3{std::max(high.x, v.position.x), std::max(high.y, v.position.y), std::max(high.z, v.position.z)};
    }
    const float extent = std::max({high.x - low.x, high.y - low.y, high.z - low.z, 1e-6f});
    const float cell = extent * 1e-5f;
    std::unordered_map<std::array<std::uint32_t, 3>, std::uint32_t, PositionHash> welded;
    welded.reserve(triangles * 2);
    const auto weld = [&](std::uint32_t vertex) {
        const core::Vec3& p = model.vertices[vertex].position;
        const std::array<std::uint32_t, 3> key = {static_cast<std::uint32_t>(std::lround((p.x - low.x) / cell)),
                                                  static_cast<std::uint32_t>(std::lround((p.y - low.y) / cell)),
                                                  static_cast<std::uint32_t>(std::lround((p.z - low.z) / cell))};
        return welded.emplace(key, static_cast<std::uint32_t>(welded.size())).first->second;
    };
    std::unordered_map<std::uint64_t, std::uint32_t> edges;
    edges.reserve(triangles * 3);
    for (const asset::SubMesh& submesh : submeshes) {
        if (submesh.material < transparent.size() && transparent[submesh.material]) continue;
        for (std::uint32_t t = 0; t + 2 < submesh.index_count; t += 3) {
            const std::uint32_t v[3] = {weld(indices[submesh.first_index + t]), weld(indices[submesh.first_index + t + 1]),
                                        weld(indices[submesh.first_index + t + 2])};
            if (v[0] == v[1] || v[1] == v[2] || v[0] == v[2]) continue;  // degenerado
            for (int e = 0; e < 3; ++e) {
                const std::uint32_t a = std::min(v[e], v[(e + 1) % 3]);
                const std::uint32_t b = std::max(v[e], v[(e + 1) % 3]);
                ++edges[(static_cast<std::uint64_t>(a) << 32) | b];
            }
        }
    }
    if (edges.empty()) return false;
    for (const auto& [edge, count] : edges) {
        if (count != 2) return false;
    }
    return true;
}

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

// El color base tiene zonas recortadas por alfa (el raster las descarta con
// alfa < 0.5). DXT1 se toma como opaco; DXT3/5 y BC7 casi siempre llevan alfa
// cuando se eligen; RGBA8 se comprueba.
bool hasAlpha(const asset::TextureData& texture) {
    switch (texture.format) {
        case asset::TextureFormat::Rgba8:
            for (std::size_t i = 3; i < texture.pixels.size(); i += 4) {
                if (texture.pixels[i] < 128) {
                    return true;
                }
            }
            return false;
        case asset::TextureFormat::Bc2:
        case asset::TextureFormat::Bc3:
        case asset::TextureFormat::Bc7:
            return true;
        default:
            return false;
    }
}

}  // namespace

std::uint32_t SkinnedModel::shadingBits(const asset::MaterialData& material) {
    const auto byte = [](float value) {
        return static_cast<std::uint32_t>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    int model = std::clamp(material.shading_model, 0, 5);
    // La transmision es del vidrio (pasada forward); en lo opaco, estandar.
    if (model == 5 && !material.transparent) model = 0;
    float p0 = material.specular_tint;
    float p1 = 0.0f;
    float p2 = 0.0f;
    switch (model) {
        case 1: p0 = material.clearcoat; p1 = material.clearcoat_roughness; p2 = material.specular_tint; break;
        case 2: p0 = material.sheen; p1 = material.sheen_tint; p2 = material.specular_tint; break;
        case 3:
            p0 = material.subsurface;
            p1 = material.translucency;
            p2 = (material.subsurface_thickness - 0.01f) / 0.29f;
            break;
        case 4: p0 = material.anisotropy; p1 = material.anisotropy_rotation / 180.0f; p2 = material.specular_tint; break;
        case 5: p0 = (material.ior - 1.0f) / 1.5f; p1 = material.transmission_thickness / 0.2f; break;
        default: break;
    }
    return static_cast<std::uint32_t>(model) | (byte(p0) << 8) | (byte(p1) << 16) | (byte(p2) << 24);
}

std::uint32_t SkinnedModel::reliefFlags(std::uint32_t flags, const asset::MaterialData& material, bool has_height,
                                        bool tessellation) {
    flags &= ~(GpuSkinnedPush::kFlagHeightMap | GpuSkinnedPush::kFlagTessellation |
               GpuSkinnedPush::kFlagParallaxShadow | GpuSkinnedPush::kTessFactorMask);
    if (!has_height) return flags;
    if (material.tessellation && tessellation) {
        const auto factor = static_cast<std::uint32_t>(std::clamp(material.tessellation_density, 1.0f, 64.0f) + 0.5f);
        return flags | GpuSkinnedPush::kFlagTessellation | (factor << GpuSkinnedPush::kTessFactorShift);
    }
    // Parallax (tambien si se pidio teselacion y la GPU no la tiene).
    flags |= GpuSkinnedPush::kFlagHeightMap;
    if (material.parallax_shadows) flags |= GpuSkinnedPush::kFlagParallaxShadow;
    return flags;
}

void SkinnedModel::create(const VulkanDevice& device, const asset::ModelData& model,
                          const SkinnedPass& pass) {
    destroy();

    // (Tambien storage: los mesh shaders leen los vertices de los meshlets.)
    vertices_ = VulkanBuffer::createDeviceLocal(
        device, model.vertices.data(), sizeof(asset::SkinnedVertex) * model.vertices.size(),
        vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eStorageBuffer);
    // Indices de LOD0 y detras los de los LODs (sus submallas se desplazan).
    if (model.lod_indices.empty()) {
        indices_ = VulkanBuffer::createDeviceLocal(device, model.indices.data(),
                                                   sizeof(std::uint32_t) * model.indices.size(),
                                                   vk::BufferUsageFlagBits::eIndexBuffer);
    } else {
        std::vector<std::uint32_t> all;
        all.reserve(model.indices.size() + model.lod_indices.size());
        all.insert(all.end(), model.indices.begin(), model.indices.end());
        all.insert(all.end(), model.lod_indices.begin(), model.lod_indices.end());
        indices_ = VulkanBuffer::createDeviceLocal(device, all.data(), sizeof(std::uint32_t) * all.size(),
                                                   vk::BufferUsageFlagBits::eIndexBuffer);
    }
    index_count_ = static_cast<std::uint32_t>(model.indices.size());
    submeshes_ = model.submeshes;
    lods_.clear();
    for (const asset::MeshLod& lod : model.lods) {
        Lod gpu{};
        gpu.submeshes = lod.submeshes;
        for (asset::SubMesh& submesh : gpu.submeshes) submesh.first_index += index_count_;
        gpu.error = lod.error;
        lods_.push_back(std::move(gpu));
    }
    rigid_ = model.animations.empty();

    // --- Texturas, con los texeles por defecto al final ---
    textures_.reserve(model.textures.size() + 3);
    for (const asset::TextureData& texture : model.textures) {
        if (texture.format == asset::TextureFormat::Rgba8) {
            textures_.emplace_back().create(device, texture.width, texture.height,
                                            texture.pixels.data());
        } else if (!device.textureCompressionBcSupported()) {
            // GPU sin BC (casi todas las de movil): se descomprime aqui (el
            // nivel 0; los mips los hace la GPU). Antes el modelo entero
            // fallaba y el juego no arrancaba en esos moviles.
            const std::vector<std::uint8_t> rgba = asset::decodeBlockCompressed(texture);
            if (rgba.empty()) throw std::runtime_error("Textura comprimida con datos incompletos: " + texture.name);
            textures_.emplace_back().create(device, texture.width, texture.height, rgba.data());
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
    white_index_ = white_index;
    black_index_ = black_index;
    render_texture_refs_.clear();

    // --- Un descriptor set por material ---
    const auto material_count = static_cast<std::uint32_t>(model.materials.size());

    vk::DescriptorPoolSize pool_size{vk::DescriptorType::eCombinedImageSampler,
                                     material_count * SkinnedPass::kMaterialBindingCount};
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
        const bool parallax = material.height_scale > 0.0f && material.occlusion_texture >= 0;
        gpu.emissive = core::Vec4{material.emissive.x, material.emissive.y, material.emissive.z,
                                  parallax ? material.height_scale : 0.0f};
        gpu.shader_flags = material.specular_map && material.metallic_roughness_texture >= 0
                               ? GpuSkinnedPush::kFlagSpecularMap
                               : 0u;
        gpu.shader_flags = reliefFlags(gpu.shader_flags, material, parallax, pass.tessellationEnabled());
        gpu.shading = shadingBits(material);
        // El signo de la escala del normal map dice su convenio (ver
        // skinned.frag): positivo = OpenGL, negativo = DirectX.
        gpu.params = core::Vec4{material.metallic, material.roughness, material.occlusion_strength,
                                material.normal_map_directx ? -material.normal_scale
                                                            : material.normal_scale};
        gpu.reflectance = material.reflectance;
        gpu.transparent = material.transparent;
        gpu.surface_shader = material.surface_shader;
        gpu.surface_params = material.surface_params;

        // Sin textura emisiva, el factor multiplica al blanco: un material
        // emisivo liso (glTF sin mapa) sigue brillando.
        const bool emits =
            material.emissive.x > 0.0f || material.emissive.y > 0.0f || material.emissive.z > 0.0f;

        // Mismo orden que los bindings del set 1 (ver SkinnedPass).
        // (+ las 4 del shader de superficie del usuario, blancas si no hay.)
        const std::array<std::size_t, SkinnedPass::kMaterialBindingCount> textures = {
            pick(material.albedo_texture, white_index),
            pick(material.metallic_roughness_texture, white_index),
            pick(material.normal_texture, flat_normal_index),
            pick(material.occlusion_texture, white_index),
            pick(material.emissive_texture, emits ? white_index : black_index),
            pick(material.surface_textures[0], white_index),
            pick(material.surface_textures[1], white_index),
            pick(material.surface_textures[2], white_index),
            pick(material.surface_textures[3], white_index),
        };
        gpu.albedo_texture = static_cast<std::uint32_t>(textures[0]);
        gpu.metallic_roughness_texture = static_cast<std::uint32_t>(textures[1]);
        gpu.emissive_texture = static_cast<std::uint32_t>(textures[4]);
        if (material.albedo_texture >= 0) {
            gpu.alpha_masked =
                hasAlpha(model.textures[static_cast<std::size_t>(material.albedo_texture)]);
        }
        materials_.push_back(gpu);
        if (material.albedo_render_texture >= 0) render_texture_refs_.push_back({m, 0, material.albedo_render_texture});
        if (material.emissive_render_texture >= 0) render_texture_refs_.push_back({m, 4, material.emissive_render_texture});

        std::array<vk::DescriptorImageInfo, SkinnedPass::kMaterialBindingCount> infos{};
        for (std::uint32_t t = 0; t < SkinnedPass::kMaterialBindingCount; ++t) {
            infos[t].sampler = *pass.sampler();
            infos[t].imageView = *textures_[textures[t]].view();
            infos[t].imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
        }

        std::array<vk::WriteDescriptorSet, SkinnedPass::kMaterialBindingCount> writes{};
        for (std::uint32_t t = 0; t < SkinnedPass::kMaterialBindingCount; ++t) {
            writes[t].dstSet = *material_sets_[m];
            writes[t].dstBinding = t;
            writes[t].descriptorType = vk::DescriptorType::eCombinedImageSampler;
            writes[t].setImageInfo(infos[t]);
        }
        // Modo compatible: el set solo tiene las texturas PBR.
        device.handle().updateDescriptorSets(
            vk::ArrayProxy<const vk::WriteDescriptorSet>(SkinnedPass::materialBindingCount(), writes.data()), nullptr);
    }

    // --- Grupos de dibujo (un grupo por material con submallas opacas) ---
    draw_groups_.clear();
    submesh_groups_.assign(submeshes_.size(), kNoGroup);
    std::vector<std::uint32_t> material_group(materials_.size(), kNoGroup);
    for (std::uint32_t i = 0; i < submeshes_.size(); ++i) {
        const std::uint32_t material = submeshes_[i].material;
        if (materials_[material].transparent) {
            continue;
        }
        if (material_group[material] == kNoGroup) {
            material_group[material] = static_cast<std::uint32_t>(draw_groups_.size());
            draw_groups_.push_back(DrawGroup{material, 0, 0});
        }
        submesh_groups_[i] = material_group[material];
        ++draw_groups_[material_group[material]].capacity;
    }
    // Las submallas de los LODs, al grupo de su material (un material que
    // solo aparece en un LOD no pasa: generateLods no crea materiales).
    for (Lod& lod : lods_) {
        lod.groups.resize(lod.submeshes.size());
        for (std::size_t i = 0; i < lod.submeshes.size(); ++i) {
            const std::uint32_t material = lod.submeshes[i].material;
            lod.groups[i] = material < material_group.size() ? material_group[material] : kNoGroup;
        }
    }
    slot_count_ = 0;
    for (DrawGroup& group : draw_groups_) {
        group.first_slot = slot_count_;
        slot_count_ += group.capacity;
    }

    buildMeshlets(device, model, pass);

    std::cout << "[Vulkan] Modelo " << model.name << " subido: " << model.textures.size()
              << " texturas con mipmaps, " << material_count << " materiales\n";
}

void SkinnedModel::buildMeshlets(const VulkanDevice& device, const asset::ModelData& model, const SkinnedPass& pass) {
    meshlet_ranges_.clear();
    closed_.clear();
    max_cluster_meshlets_ = 0;
    // Solo escenarios (rigidos): los animados se deforman y sus esferas y conos no valdrian.
    if (!pass.meshShadersEnabled() || !rigid_ || model.vertices.empty()) return;

    constexpr std::size_t kMaxVertices = 64;
    constexpr std::size_t kMaxTriangles = 124;
    constexpr float kConeWeight = 0.25f;
    const float* positions = &model.vertices[0].position.x;
    const std::size_t vertex_count = model.vertices.size();
    constexpr std::size_t kStride = sizeof(asset::SkinnedVertex);

    std::vector<bool> transparent(materials_.size());
    for (std::size_t m = 0; m < materials_.size(); ++m) transparent[m] = materials_[m].transparent;

    std::vector<GpuMeshlet> gpu;
    std::vector<std::uint32_t> all_vertices;
    std::vector<std::uint8_t> all_triangles;
    std::vector<meshopt_Meshlet> built;
    std::vector<unsigned int> built_vertices;
    std::vector<unsigned char> built_triangles;
    for (std::uint32_t lod = 0; lod < lodCount(); ++lod) {
        const std::vector<asset::SubMesh>& submeshes = lod == 0 ? model.submeshes : model.lods[lod - 1].submeshes;
        const std::vector<std::uint32_t>& indices = lod == 0 ? model.indices : model.lod_indices;
        std::vector<MeshletRange> ranges(submeshes.size());
        for (std::size_t s = 0; s < submeshes.size(); ++s) {
            const asset::SubMesh& submesh = submeshes[s];
            if (submesh.index_count < 3 || (submesh.material < transparent.size() && transparent[submesh.material])) {
                continue;
            }
            const std::size_t bound = meshopt_buildMeshletsBound(submesh.index_count, kMaxVertices, kMaxTriangles);
            built.resize(bound);
            built_vertices.resize(bound * kMaxVertices);
            built_triangles.resize(bound * kMaxTriangles * 3);
            const std::size_t count = meshopt_buildMeshlets(
                built.data(), built_vertices.data(), built_triangles.data(), indices.data() + submesh.first_index,
                submesh.index_count, positions, vertex_count, kStride, kMaxVertices, kMaxTriangles, kConeWeight);
            ranges[s] = MeshletRange{static_cast<std::uint32_t>(gpu.size()), static_cast<std::uint32_t>(count)};
            max_cluster_meshlets_ = std::max(max_cluster_meshlets_, static_cast<std::uint32_t>(count));
            for (std::size_t i = 0; i < count; ++i) {
                const meshopt_Meshlet& m = built[i];
                const meshopt_Bounds bounds =
                    meshopt_computeMeshletBounds(&built_vertices[m.vertex_offset], &built_triangles[m.triangle_offset],
                                                 m.triangle_count, positions, vertex_count, kStride);
                GpuMeshlet out{};
                out.center_radius = core::Vec4{bounds.center[0], bounds.center[1], bounds.center[2], bounds.radius};
                out.cone_axis_cutoff =
                    core::Vec4{bounds.cone_axis[0], bounds.cone_axis[1], bounds.cone_axis[2], bounds.cone_cutoff};
                out.cone_apex = core::Vec4{bounds.cone_apex[0], bounds.cone_apex[1], bounds.cone_apex[2], 0.0f};
                out.vertex_offset = static_cast<std::uint32_t>(all_vertices.size());
                out.triangle_offset = static_cast<std::uint32_t>(all_triangles.size());
                out.vertex_count = m.vertex_count;
                out.triangle_count = m.triangle_count;
                all_vertices.insert(all_vertices.end(), built_vertices.begin() + m.vertex_offset,
                                    built_vertices.begin() + m.vertex_offset + m.vertex_count);
                all_triangles.insert(all_triangles.end(), built_triangles.begin() + m.triangle_offset,
                                     built_triangles.begin() + m.triangle_offset + m.triangle_count * 3);
                gpu.push_back(out);
            }
        }
        meshlet_ranges_.push_back(std::move(ranges));
        // LOD0 se comprueba; los simplificados heredan (el simplificador no abre mallas cerradas).
        closed_.push_back(lod == 0 ? isClosedMesh(model, submeshes, indices, transparent) : closed_[0]);
    }
    if (gpu.empty()) {
        meshlet_ranges_.clear();
        closed_.clear();
        return;
    }
    while (all_triangles.size() % 4 != 0) all_triangles.push_back(0);

    const auto storage = vk::BufferUsageFlagBits::eStorageBuffer;
    meshlets_ = VulkanBuffer::createDeviceLocal(device, gpu.data(), sizeof(GpuMeshlet) * gpu.size(), storage);
    meshlet_vertices_ = VulkanBuffer::createDeviceLocal(device, all_vertices.data(),
                                                        sizeof(std::uint32_t) * all_vertices.size(), storage);
    meshlet_triangles_ =
        VulkanBuffer::createDeviceLocal(device, all_triangles.data(), all_triangles.size(), storage);

    const vk::DescriptorPoolSize pool_size{vk::DescriptorType::eStorageBuffer, 4};
    vk::DescriptorPoolCreateInfo pool_info{};
    pool_info.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    pool_info.maxSets = 1;
    pool_info.setPoolSizes(pool_size);
    meshlet_pool_ = vk::raii::DescriptorPool(device.handle(), pool_info);
    const vk::DescriptorSetLayout layout = *pass.meshletSetLayout();
    vk::DescriptorSetAllocateInfo alloc{};
    alloc.descriptorPool = *meshlet_pool_;
    alloc.setSetLayouts(layout);
    meshlet_sets_ = vk::raii::DescriptorSets(device.handle(), alloc);
    const std::array<vk::DescriptorBufferInfo, 4> infos = {
        vk::DescriptorBufferInfo{*vertices_.handle(), 0, VK_WHOLE_SIZE},
        vk::DescriptorBufferInfo{*meshlets_.handle(), 0, VK_WHOLE_SIZE},
        vk::DescriptorBufferInfo{*meshlet_vertices_.handle(), 0, VK_WHOLE_SIZE},
        vk::DescriptorBufferInfo{*meshlet_triangles_.handle(), 0, VK_WHOLE_SIZE}};
    std::array<vk::WriteDescriptorSet, 4> writes{};
    for (std::uint32_t i = 0; i < 4; ++i) {
        writes[i].dstSet = *meshlet_sets_[0];
        writes[i].dstBinding = i;
        writes[i].descriptorType = vk::DescriptorType::eStorageBuffer;
        writes[i].setBufferInfo(infos[i]);
    }
    device.handle().updateDescriptorSets(writes, nullptr);
    std::cout << "[Vulkan] " << model.name << ": " << gpu.size() << " meshlets en " << lodCount() << " LODs"
              << (closed_[0] ? " (malla cerrada)" : "") << "\n";
}

void SkinnedModel::setMaterialImage(const VulkanDevice& device, const SkinnedPass& pass, std::uint32_t material,
                                    std::uint32_t binding, vk::ImageView view) {
    if (material >= material_sets_.size()) return;
    if (!view) view = *textures_[binding == 4 ? black_index_ : white_index_].view();
    vk::DescriptorImageInfo info{};
    info.sampler = *pass.sampler();
    info.imageView = view;
    info.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
    vk::WriteDescriptorSet write{};
    write.dstSet = *material_sets_[material];
    write.dstBinding = binding;
    write.descriptorType = vk::DescriptorType::eCombinedImageSampler;
    write.setImageInfo(info);
    device.handle().updateDescriptorSets(write, nullptr);
}

void SkinnedModel::destroy() {
    meshlet_sets_.clear();
    meshlet_pool_ = nullptr;
    meshlets_.destroy();
    meshlet_vertices_.destroy();
    meshlet_triangles_.destroy();
    meshlet_ranges_.clear();
    closed_.clear();
    material_sets_.clear();
    pool_ = nullptr;
    textures_.clear();
    materials_.clear();
    submeshes_.clear();
    lods_.clear();
    indices_.destroy();
    vertices_.destroy();
    index_count_ = 0;
}

}  // namespace cramion::gfx
