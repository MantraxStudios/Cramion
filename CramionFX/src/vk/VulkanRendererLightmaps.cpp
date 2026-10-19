// VulkanRenderer: lightmaps horneados por texel de UV2 (.crbake v4,
// PLAN-LIGHTMAPS.md Fase 2). Aparte para no engordar VulkanRenderer.cpp:
//
//   - el atlas (LightmapAtlas: binding 7 del set 0 de la geometria) y los
//     registros por instancia (binding 8, GpuLightmapRecord en el mismo hueco
//     que su matriz en el buffer de huesos, por frame en vuelo);
//   - de donde salen las sombras del sol (cascadas, el sol entero en las
//     sondas de un .crbake v1-3 o la visibilidad horneada del v4);
//   - el cielo del horneado (los armonicos de la irradiancia del IBL).

#include "CramionFX/vk/VulkanCompat.h"
#include "CramionFX/vk/VulkanRenderer.h"
#include "CramionFX/scene/Scene.h"
#include "CramionFX/scene/Scene.h"

#include <algorithm>
#include <cstring>
#include <iostream>

namespace cramion::gfx {

namespace {

constexpr std::size_t kInitialRecords = 256;

}  // namespace

void VulkanRenderer::createLightmapResources() {
    lightmap_atlas_.createDummy(device_);
    lightmaps_uploaded_ = false;
    lightmap_record_buffers_.resize(kMaxFramesInFlight);
    const std::vector<GpuLightmapRecord> none(kInitialRecords);
    for (VulkanBuffer& buffer : lightmap_record_buffers_) {
        buffer.create(device_, sizeof(GpuLightmapRecord) * kInitialRecords, vk::BufferUsageFlagBits::eStorageBuffer,
                      vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
        buffer.write(none.data(), sizeof(GpuLightmapRecord) * none.size());
    }
}

void VulkanRenderer::destroyLightmapResources() {
    for (VulkanBuffer& buffer : lightmap_record_buffers_) buffer.destroy();
    lightmap_record_buffers_.clear();
    lightmap_atlas_.destroy();
    lightmaps_uploaded_ = false;
}

void VulkanRenderer::writeLightmapDescriptors(std::uint32_t frame_index) {
    if (frame_index >= skin_sets_.size() || frame_index >= lightmap_record_buffers_.size()) return;
    if (!lightmap_record_buffers_[frame_index].isValid()) return;
    vk::DescriptorImageInfo atlas_info{};
    atlas_info.sampler = lightmap_atlas_.sampler();
    atlas_info.imageView = lightmap_atlas_.view();
    atlas_info.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
    vk::DescriptorBufferInfo records_info{};
    records_info.buffer = *lightmap_record_buffers_[frame_index].handle();
    records_info.range = VK_WHOLE_SIZE;
    std::array<vk::WriteDescriptorSet, 2> writes{};
    writes[0].dstSet = *skin_sets_[frame_index];
    writes[0].dstBinding = 7;
    writes[0].descriptorType = vk::DescriptorType::eCombinedImageSampler;
    writes[0].setImageInfo(atlas_info);
    writes[1].dstSet = *skin_sets_[frame_index];
    writes[1].dstBinding = 8;
    writes[1].descriptorType = vk::DescriptorType::eStorageBuffer;
    writes[1].setBufferInfo(records_info);
    device_.handle().updateDescriptorSets(writes, nullptr);
}

void VulkanRenderer::uploadLightmaps() {
    // (setBakedLighting ya espero a la GPU: nadie lee el atlas viejo.)
    if (!lightmap_atlas_.sampler()) return;  // sin crear (renderizador sin iniciar)
    lightmaps_uploaded_ = !baked_.lightmaps.empty() && lightmap_atlas_.upload(device_, baked_.lightmaps);
    // Las paginas ya estan en la GPU: fuera de la RAM (PLAN-LIGHTMAPS 4.3, punto 10).
    for (LightmapPage& page : baked_.lightmaps.pages) std::vector<std::uint16_t>().swap(page.texels);
    for (std::uint32_t i = 0; i < kMaxFramesInFlight && i < skin_sets_.size(); ++i) writeLightmapDescriptors(i);
    ++baked_generation_;
    if (lightmaps_uploaded_) {
        std::cout << "[Lightmap] " << lightmap_atlas_.layers() << " paginas de " << lightmap_atlas_.width() << " x "
                  << lightmap_atlas_.height() << " (" << lightmap_atlas_.bytes() / (1024 * 1024) << " MB), "
                  << baked_.lightmaps.records.size() << " objetos\n";
    }
}

bool VulkanRenderer::lightmapsActive() const {
    return lightmaps_uploaded_ && lighting_mode_ == LightingMode::Baked;
}

SunShadowSource VulkanRenderer::sunShadowSource(const core::Vec3& sun_direction) const {
    if (lighting_mode_ != LightingMode::Baked) return SunShadowSource::Cascades;
    if (baked_.version <= 3) {
        // Lo de siempre: el sol entero va en las sondas.
        return bakedGiActive() && bakedSunMatches(baked_, sun_direction) ? SunShadowSource::LegacyBaked
                                                                          : SunShadowSource::Cascades;
    }
    if (lightmapsActive() && baked_.lightmaps.sun_visibility && sun_bake_mode_ &&
        bakedSunDirectionMatches(baked_, sun_direction)) {
        return SunShadowSource::BakedVisibility;
    }
    return SunShadowSource::Cascades;
}

int VulkanRenderer::lightmapDebugView() const {
    switch (drawModeNow()) {
        case SceneDrawMode::LightmapIrradiance: return 1;
        case SceneDrawMode::SunVisibility: return 2;
        case SceneDrawMode::LightmapUv: return 3;
        default: return 0;
    }
}

core::Vec4 VulkanRenderer::lightmapUniform(const core::Vec3& sun_direction) const {
    core::Vec4 u{};
    const SceneDrawMode mode = drawModeNow();
    const int debug = lightmapDebugView();
    const bool flat = mode == SceneDrawMode::Unlit || mode == SceneDrawMode::Wireframe;
    if (lightmapsActive() && !flat && !pathTraceReplacesLighting()) {
        u.x = baked_.lightmaps.has_irradiance ? 2.0f : 1.0f;
        const SunShadowSource source = sunShadowSource(sun_direction);
        u.y = source == SunShadowSource::BakedVisibility && sunShadows() ? sun_shadow_strength_ : 0.0f;
    }
    // Las vistas de depuracion tambien en Tiempo real (con el atlas subido).
    if (debug > 0) {
        if (lightmaps_uploaded_ && u.x < 0.5f) u.x = baked_.lightmaps.has_irradiance ? 2.0f : 1.0f;
        u.z = static_cast<float>(debug);
    }
    return u;
}

void VulkanRenderer::writeLightmapRecords(const scene::Scene& scene, std::uint32_t frame_index) {
    if (frame_index >= lightmap_record_buffers_.size()) return;
    const std::size_t count = motion_offset_;  // huecos de este frame (la mitad de bone_staging_)
    if (count == 0) return;
    lightmap_record_staging_.assign(count, GpuLightmapRecord{});
    const bool active = lightmapsActive() || (lightmaps_uploaded_ && lightmapDebugView() > 0);
    if (active) {
        const std::vector<scene::Actor>& actors = scene.actors();
        for (const ActorDraw& draw : actor_draws_) {
            if (!draw.per_submesh || draw.bone_entries == 0 || draw.model >= skinned_models_.size()) continue;
            if (!skinned_models_[draw.model].hasLightmapUvs() || draw.scene_actor >= actors.size()) continue;
            const scene::Actor& actor = actors[draw.scene_actor];
            if (actor.lightmap_layer < 0 || static_cast<std::uint32_t>(actor.lightmap_layer) >= lightmap_atlas_.layers()) continue;
            // La matriz de la instancia va detras de sus huesos.
            const std::size_t slot = static_cast<std::size_t>(draw.bone_offset) + draw.bone_entries - 1;
            if (slot >= count) continue;
            GpuLightmapRecord& record = lightmap_record_staging_[slot];
            record.scale_offset = actor.lightmap_scale_offset;
            record.layer = static_cast<std::uint32_t>(actor.lightmap_layer);
        }
    }
    VulkanBuffer& buffer = lightmap_record_buffers_[frame_index];
    const std::size_t bytes = count * sizeof(GpuLightmapRecord);
    if (buffer.size() < bytes) {
        // Como los huesos: se dobla, y la fence de este frame garantiza que la
        // GPU ya no lo usa.
        const std::size_t capacity = std::max(count, static_cast<std::size_t>(buffer.size() / sizeof(GpuLightmapRecord)) * 2);
        buffer.create(device_, sizeof(GpuLightmapRecord) * capacity, vk::BufferUsageFlagBits::eStorageBuffer,
                      vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
        writeLightmapDescriptors(frame_index);
    }
    buffer.write(lightmap_record_staging_.data(), bytes);
}

std::array<core::Vec3, 9> VulkanRenderer::readSkyIrradianceSh() {
    std::array<core::Vec3, 9> out{};
    const VulkanBuffer& source = ibl_probe_.irradianceBuffer();
    if (!source.isValid()) return out;
    device_.waitIdle();
    constexpr vk::DeviceSize kBytes = sizeof(core::Vec4) * 9;
    VulkanBuffer readback;
    readback.create(device_, kBytes, vk::BufferUsageFlagBits::eTransferDst,
                    vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
    device_.submitOneTime([&](const vk::raii::CommandBuffer& cmd) {
        const vk::BufferCopy region{0, 0, kBytes};
        cmd.copyBuffer(*source.handle(), *readback.handle(), region);
    });
    core::Vec4 coefficients[9];
    std::memcpy(coefficients, readback.mapped(), sizeof(coefficients));
    for (std::size_t k = 0; k < 9; ++k) out[k] = core::Vec3{coefficients[k].x, coefficients[k].y, coefficients[k].z};
    return out;
}

}  // namespace cramion::gfx
