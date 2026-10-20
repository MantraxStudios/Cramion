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

// Half float de un valor positivo (la escala de la emision por instancia:
// unpackHalf2x16 en lightmap_common.glsl). Sin subnormales: por debajo de
// 6e-5, 0. Redondeo al mas cercano.
std::uint32_t packPositiveHalf(float value) {
    value = std::clamp(value, 0.0f, 65504.0f);
    if (value < 6.1035e-5f) return 0u;
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    bits += 0x1000u;  // redondeo de los 13 bits que se pierden
    const std::uint32_t exponent = ((bits >> 23) & 0xFFu) - 127u + 15u;
    return std::min((exponent << 10) | ((bits >> 13) & 0x3FFu), 0x7BFFu);
}

std::uint32_t packUnorm8(const core::Vec4& v) {
    const auto byte = [](float c) { return static_cast<std::uint32_t>(std::clamp(c, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return byte(v.x) | (byte(v.y) << 8) | (byte(v.z) << 16) | (byte(v.w) << 24);
}

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
    const std::vector<scene::Actor>& actors = scene.actors();
    // Parametros por instancia (tinte, opacidad, escala de la emision, sin
    // decals, INSTANCE_DATA): en el
    // primer hueco de cada dibujo (lo que lee la GPU con bone_offset) y en el
    // de su matriz (gl_InstanceIndex en los lotes). Solo los que no son los
    // de por defecto: casi ninguno.
    for (const ActorDraw& draw : actor_draws_) {
        if (draw.bone_entries == 0 || draw.scene_actor >= actors.size()) continue;
        const scene::Actor& actor = actors[draw.scene_actor];
        if (!actor.hasInstanceParams()) continue;
        const std::uint32_t tint = packUnorm8(actor.tint);
        const std::uint32_t emission = packPositiveHalf(actor.emission_scale);
        const std::size_t first = draw.bone_offset;
        const std::size_t last = static_cast<std::size_t>(draw.bone_offset) + draw.bone_entries - 1;
        for (const std::size_t slot : {first, last}) {
            if (slot >= count) continue;
            lightmap_record_staging_[slot].tint = tint;
            lightmap_record_staging_[slot].emission = emission;
            lightmap_record_staging_[slot].flags = actor.receive_decals ? 0u : GpuLightmapRecord::kFlagNoDecals;
            // INSTANCE_DATA(i) de los .crshader.
            static_assert(sizeof(scene::Actor::instance_data) == sizeof(core::Vec4) * kInstanceDataCount);
            for (std::uint32_t k = 0; k < kInstanceDataCount; ++k) {
                lightmap_record_staging_[slot].data[k] = actor.instance_data[k];
            }
        }
    }
    const bool active = lightmapsActive() || (lightmaps_uploaded_ && lightmapDebugView() > 0);
    if (active) {
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
    // --- Fundido de LOD (dither crossfade) ---
    // 16 bits altos de `emission` (los bajos son la escala de la emision):
    // 0 = sin fundido; bits 0..14 = avance (0..1) y bit 15 = este hueco es el
    // nivel que sale (lodFadeDiscard en lightmap_common.glsl). El hueco del
    // que sale es una copia del de la instancia (tinte, lightmap...).
    for (const ActorDraw& draw : actor_draws_) {
        if (draw.lod_fade_slot == UINT32_MAX || draw.bone_entries == 0) continue;
        const std::size_t incoming = static_cast<std::size_t>(draw.bone_offset) + draw.bone_entries - 1;
        const std::size_t outgoing = draw.lod_fade_slot;
        if (incoming >= count || outgoing >= count) continue;
        const auto progress = static_cast<std::uint32_t>(std::clamp(draw.lod_fade, 0.0f, 1.0f) * 32767.0f + 0.5f);
        const std::uint32_t fade = std::clamp(progress, 1u, 32767u);
        lightmap_record_staging_[outgoing] = lightmap_record_staging_[incoming];
        GpuLightmapRecord& in = lightmap_record_staging_[incoming];
        GpuLightmapRecord& out = lightmap_record_staging_[outgoing];
        in.emission = (in.emission & 0xFFFFu) | (fade << 16);
        out.emission = (out.emission & 0xFFFFu) | ((fade | 0x8000u) << 16);
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
