// VulkanRenderer: las sondas de reflexion colocadas (componente
// ReflectionProbeBox; ReflectionProbeSet.h). Aparte para no engordar
// VulkanRenderer.cpp:
//   - fillPlacedProbes: las de cada vista en el UBO de las luces (al final de
//     GpuLights; lighting.frag mezcla las 2 que mas pesan en cada pixel).
//   - setBakedReflectionCubes: los cubos horneados al array de cubos.
//   - bakeReflectionCube: el horneado de una sonda (las seis caras como la
//     sonda de la camara, el prefiltrado y la lectura a la CPU).
// Y si el GTAO va en lugar del SSAO (gtaoActive).

#include "CramionFX/vk/VulkanCompat.h"
#include "CramionFX/vk/VulkanRenderer.h"

#include "CramionFX/scene/Scene.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <utility>
#include <vector>

namespace cramion::gfx {
namespace {

using core::Vec3;

void pipelineBarrier(const vk::raii::CommandBuffer& cmd, const vk::ImageMemoryBarrier2& barrier) {
    vk::DependencyInfo dependency{};
    dependency.setImageMemoryBarriers(barrier);
    compat::pipelineBarrier(cmd, dependency);
}

vk::ImageMemoryBarrier2 colorBarrier(vk::Image image, vk::ImageLayout old_layout, vk::ImageLayout new_layout,
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
    barrier.subresourceRange = vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    return barrier;
}

}  // namespace

bool VulkanRenderer::gtaoActive() const {
    // En gama baja sigue el SSAO de siempre (mas barato); en el modo
    // compatible no hay GTAO (su imagen es RG16F y lighting.frag no lo lee).
    // (Y con la imagen en RGBA16F: ssaoFormat.)
    return gtao_requested_ && *gtao_pass_.pipeline() != VK_NULL_HANDLE && !device_.compatMode() &&
           hardware_.tier != HardwareTier::Low && ssaoFormat() == kGtaoFormat;
}

void VulkanRenderer::fillPlacedProbes(GpuLights& light_data, const core::Vec3& eye) {
    light_data.placed_info[0] = 0;
    // Mientras se hornea una, ninguna: la captura no se ve a si misma (ni a
    // las demas a medio hornear).
    if (!placed_probes_enabled_ || baking_reflection_ || !reflection_probe_.placedSupported() ||
        placed_probes_.empty() || placed_cubes_.empty()) {
        if (!isolated()) placed_active_count_ = 0;
        return;
    }
    const std::vector<SelectedPlacedProbe> picked = selectPlacedProbes(placed_probes_, placed_cube_ids_, eye);
    for (std::size_t i = 0; i < picked.size(); ++i) {
        const PlacedReflectionProbe& probe = placed_probes_[picked[i].probe];
        // El paralaje desde donde esta ahora la sonda (se mueve con el origen
        // flotante; si se movio despues de hornear, hay que volver a hornear).
        light_data.placed[i] = packPlacedProbe(probe, picked[i].layer, probe.position);
    }
    light_data.placed_info[0] = static_cast<std::uint32_t>(picked.size());
    if (!isolated()) placed_active_count_ = static_cast<std::uint32_t>(picked.size());
}

void VulkanRenderer::writePlacedProbeDescriptors() {
    if (!lighting_pass_.hasBinding(31) || !reflection_probe_.placedSupported()) return;
    for (std::uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
        vk::DescriptorImageInfo info{};
        info.sampler = *reflection_probe_.sampler();
        info.imageView = *reflection_probe_.placedView();
        info.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
        vk::WriteDescriptorSet write{};
        write.dstSet = *lighting_sets_[i];
        write.dstBinding = 31;
        write.descriptorType = vk::DescriptorType::eCombinedImageSampler;
        write.setImageInfo(info);
        device_.handle().updateDescriptorSets(write, nullptr);
    }
}

void VulkanRenderer::setBakedReflectionCubes(std::vector<BakedReflectionCube> cubes) {
    if (cubes.size() > kMaxPlacedProbeCubes) {
        std::cerr << "[Vulkan] Sondas de reflexion: " << cubes.size() << " horneadas, se usan las primeras "
                  << kMaxPlacedProbeCubes << "\n";
        cubes.resize(kMaxPlacedProbeCubes);
    }
    placed_cubes_ = std::move(cubes);
    // El id de cada capa; las que no valen (otro tamano, danadas) no se
    // eligen nunca (id vacio).
    placed_cube_ids_.assign(placed_cubes_.size(), PlacedProbeId{});
    std::vector<const BakedReflectionCube*> uploads(placed_cubes_.size(), nullptr);
    for (std::size_t i = 0; i < placed_cubes_.size(); ++i) {
        const BakedReflectionCube& cube = placed_cubes_[i];
        if (cube.size == ReflectionProbe::kSize && cube.mips == ReflectionProbe::kMips && bakedCubeValid(cube)) {
            placed_cube_ids_[i] = cube.id;
            uploads[i] = &cube;
        }
    }
    if (!reflection_probe_.placedSupported()) return;
    // El array viejo puede estar en uso en los frames en vuelo.
    waitIdle();
    reflection_probe_.uploadPlacedCubes(device_, uploads);
    writePlacedProbeDescriptors();
}

bool VulkanRenderer::bakeReflectionCube(const scene::Scene& scene, const core::Vec3& position,
                                        BakedReflectionCube& out, std::string* error) {
    const auto fail = [&](const char* why) {
        if (error != nullptr) *error = why;
        return false;
    };
    if (device_.compatMode()) return fail("el modo compatible no tiene sondas de reflexion");
    if (!reflection_probe_.placedSupported()) return fail("la GPU no admite arrays de cubos (imageCubeArray)");
    if (rayTracingActive()) {
        return fail("con el trazado de rayos no se capturan sondas: apagalo (render.rt) para hornear");
    }
    if (game_unlit_) return fail("en el modo Unlit no se capturan sondas");
    if (!scene_history_valid_ || !local_shadow_layout_ready_ || render_extent_.width == 0 ||
        render_extent_.height == 0) {
        return fail("aun no se ha dibujado ningun frame (abre la vista de la escena y vuelve a probar)");
    }

    // Nada en vuelo: ni frames ni una cara de la sonda de la camara (la que
    // estuviera terminada se recoge; una a medias se empieza de nuevo, porque
    // comparten la imagen de captura).
    waitIdle();
    finishProbeCapture();
    if (probe_face_ > 0 && !probe_finish_pending_) probe_face_ = 0;

    // Mismas caras y convencion que captureProbeFace (+X, -X, +Y, -Y, +Z, -Z).
    const std::array<std::pair<Vec3, Vec3>, 6> faces = {{
        {Vec3{1.0f, 0.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f}},
        {Vec3{-1.0f, 0.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f}},
        {Vec3{0.0f, 1.0f, 0.0f}, Vec3{0.0f, 0.0f, -1.0f}},
        {Vec3{0.0f, -1.0f, 0.0f}, Vec3{0.0f, 0.0f, 1.0f}},
        {Vec3{0.0f, 0.0f, 1.0f}, Vec3{0.0f, 1.0f, 0.0f}},
        {Vec3{0.0f, 0.0f, -1.0f}, Vec3{0.0f, 1.0f, 0.0f}},
    }};
    const vk::Extent2D extent = render_extent_;
    const std::uint32_t side = std::min(extent.width, extent.height);
    const vk::Rect2D square{vk::Offset2D{static_cast<std::int32_t>((extent.width - side) / 2),
                                         static_cast<std::int32_t>((extent.height - side) / 2)},
                            vk::Extent2D{side, side}};

    const core::Mat4 view_projection = camera_view_projection_;
    const core::Mat4 previous_view_projection = previous_view_projection_;
    const std::uint32_t screen_frame = current_frame_;
    current_frame_ = kProbeFrameSlot;
    capturing_ = true;
    baking_reflection_ = true;
    const vk::raii::Fence& fence = in_flight_fences_[current_frame_];

    for (std::uint32_t face = 0; face < faces.size(); ++face) {
        scene::Camera camera = scene.camera();
        camera.setPosition(position);
        camera.setOrientation(faces[face].first, faces[face].second);
        const float aspect = static_cast<float>(extent.width) / static_cast<float>(extent.height);
        camera.setAspectRatio(aspect);
        camera.setFovY(extent.width >= extent.height ? core::kPi * 0.5f : 2.0f * std::atan(1.0f / aspect));
        // Sin el tope de distancia de la sonda de la camara: se hornea una vez.
        device_.waitFence(*fence, "horneado de una sonda de reflexion");
        if (current_frame_ < surface_param_used_.size()) surface_param_used_[current_frame_] = 0;
        if ((decal_sets_dirty_ & (1u << current_frame_)) != 0) {
            writeDecalTextureDescriptors(static_cast<int>(current_frame_));
            decal_sets_dirty_ &= ~(1u << current_frame_);
        }
        // Los actores del ultimo frame a los buffers de este hueco (como
        // captureProbeFace).
        {
            const std::uint32_t slot_count =
                draw_batches_.empty() ? 0u : draw_batches_.back().first_slot + draw_batches_.back().capacity;
            gpu_culling_.setClusters(device_, current_frame_, gpu_clusters_,
                                     static_cast<std::uint32_t>(draw_batches_.size()), slot_count, camera_buffers_);
            if (!bone_staging_.empty()) {
                ensureBoneCapacity(current_frame_, bone_staging_.size());
                bone_buffers_[current_frame_].write(bone_staging_.data(), sizeof(core::Mat4) * bone_staging_.size());
                writeLightmapRecords(scene, current_frame_);
            }
        }
        updateUniforms(scene, camera, current_frame_);
        terrain_pass_.prepareView(current_frame_, position, camera_view_projection_, probe_limits_.terrain);

        device_.handle().resetFences(*fence);
        const vk::raii::CommandBuffer& cmd = command_buffers_[current_frame_];
        cmd.reset();
        cmd.begin(vk::CommandBufferBeginInfo{});
        foliage_pass_.recordCull(cmd, current_frame_, position, camera_view_projection_, 0.0f,
                                 /*update_stats=*/false);
        recordShadowPass(cmd, current_frame_);
        recordSkyLutPass(cmd);
        recordCloudPass(cmd, current_frame_);
        recordGeometryPass(cmd, current_frame_);
        recordSsaoPass(cmd, current_frame_);
        recordSsgiPass(cmd, current_frame_);
        recordSsrPass(cmd, current_frame_);
        recordLightingPass(cmd, current_frame_, probe_capture_);
        pipelineBarrier(cmd, colorBarrier(*probe_capture_.handle(), vk::ImageLayout::eColorAttachmentOptimal,
                                          vk::ImageLayout::eTransferSrcOptimal,
                                          vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                          vk::AccessFlagBits2::eColorAttachmentWrite, vk::PipelineStageFlagBits2::eBlit,
                                          vk::AccessFlagBits2::eTransferRead));
        reflection_probe_.recordFaceCopy(cmd, *probe_capture_.handle(), square, face);
        if (face + 1 == faces.size()) reflection_probe_.recordBakePrefilter(cmd);
        cmd.end();

        const vk::CommandBuffer command_buffer = *cmd;
        vk::SubmitInfo submit_info{};
        submit_info.commandBufferCount = 1;
        submit_info.pCommandBuffers = &command_buffer;
        {
            const std::lock_guard queue_lock(device_.queueMutex());
            device_.graphicsQueue().submit(submit_info, *fence);
        }
    }
    device_.waitFence(*fence, "horneado de una sonda de reflexion");

    current_frame_ = screen_frame;
    capturing_ = false;
    baking_reflection_ = false;
    camera_view_projection_ = view_projection;
    previous_view_projection_ = previous_view_projection;

    out = BakedReflectionCube{};
    out.position = position;
    out.size = ReflectionProbe::kSize;
    out.mips = ReflectionProbe::kMips;
    if (!reflection_probe_.readBake(out.texels) || !bakedCubeValid(out)) {
        return fail("no se pudo leer el cubo horneado");
    }
    return true;
}

}  // namespace cramion::gfx
