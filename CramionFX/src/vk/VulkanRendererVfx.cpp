// VulkanRenderer: el pase del VFX Graph (VfxPass.h). Aparte para no engordar
// VulkanRenderer.cpp.

#include "CramionFX/vk/VulkanCompat.h"
#include "CramionFX/vk/VulkanRenderer.h"

namespace cramion::gfx {

// vfx.frag declara el principio de GpuLights (sol, ambiente, 32 + 8 luces) y
// de GpuShadows; VfxPass enlaza solo ese tramo.
static_assert(sizeof(GpuLights) >= 6 * 16 + 32 * 48 + 8 * 64, "vfx.frag lee mas de lo que tiene GpuLights");
static_assert(sizeof(GpuShadows) >= 4 * 64 + 3 * 16, "vfx.frag lee mas de lo que tiene GpuShadows");
static_assert(scene::kMaxPointLights == 32 && scene::kMaxSpotLights == 8, "vfx.frag declara 32 puntuales y 8 focos");

void VulkanRenderer::recordVfxPass(const vk::raii::CommandBuffer& cmd, std::uint32_t frame_index) {
    const vk::Extent2D extent = scene_color_.extent();
    if (extent.width == 0 || extent.height == 0) return;

    VfxView view;
    view.view = camera_view_;
    view.projection = camera_projection_;
    view.view_projection = camera_view_projection_;
    view.camera_position = camera_position_;
    view.light = precipitation_light_;
    view.to_sun = ibl_to_light_;  // (las mallas iluminadas)
    view.extent = gbuffer_.depth().extent();
    view.scene_depth = *gbuffer_.depth().view();
    // Luz real de los "lit": las luces, las cascadas y los clusteres de este
    // hueco de frame (VfxPass reescribe los descriptores en cada prepare, asi
    // que valen aunque los clusteres hayan crecido).
    view.ambient = vfx_ambient_light_;
    if (frame_index < light_buffers_.size() && light_buffers_[frame_index].isValid()) {
        view.lights = *light_buffers_[frame_index].handle();
    }
    if (frame_index < shadow_buffers_.size() && shadow_buffers_[frame_index].isValid()) {
        view.shadows = *shadow_buffers_[frame_index].handle();
    }
    view.shadow_map = *shadow_map_.image().view();
    view.shadow_sampler = *shadow_map_.sampler();
    if (frame_index < cluster_light_buffers_.size() && cluster_light_buffers_[frame_index].isValid() &&
        frame_index < cluster_grid_buffers_.size() && cluster_grid_buffers_[frame_index].isValid()) {
        view.cluster_lights = *cluster_light_buffers_[frame_index].handle();
        view.cluster_grid = *cluster_grid_buffers_[frame_index].handle();
    }
    // Las vistas secundarias (vista Juego, sondas) solo dibujan: la
    // simulacion avanza una vez por frame con la vista principal.
    const bool simulate = !isolated() && !xrSecondEye();  // una vez por frame (en VR, el primer ojo)
    if (!vfx_pass_.prepare(frame_index, view, simulate)) return;
    if (simulate) vfx_pass_.recordSimulate(cmd, frame_index);

    // La imagen HDR sigue como destino de color: solo hay que ordenar las
    // escrituras. El depth, en solo lectura (prueba y particulas suaves).
    vk::ImageMemoryBarrier2 color_barrier{};
    color_barrier.srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput;
    color_barrier.srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite;
    color_barrier.dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput;
    color_barrier.dstAccessMask = vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite;
    color_barrier.oldLayout = vk::ImageLayout::eColorAttachmentOptimal;
    color_barrier.newLayout = vk::ImageLayout::eColorAttachmentOptimal;
    color_barrier.image = *scene_color_.handle();
    color_barrier.subresourceRange = vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    vk::ImageMemoryBarrier2 depth_barrier{};
    depth_barrier.srcStageMask = vk::PipelineStageFlagBits2::eLateFragmentTests | vk::PipelineStageFlagBits2::eFragmentShader;
    depth_barrier.srcAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
    depth_barrier.dstStageMask = vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                                 vk::PipelineStageFlagBits2::eLateFragmentTests | vk::PipelineStageFlagBits2::eFragmentShader;
    depth_barrier.dstAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentRead | vk::AccessFlagBits2::eShaderSampledRead;
    depth_barrier.oldLayout = compat::depthReadOnlyLayout();
    depth_barrier.newLayout = compat::depthReadOnlyLayout();
    depth_barrier.image = *gbuffer_.depth().handle();
    depth_barrier.subresourceRange = vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1};
    const std::array<vk::ImageMemoryBarrier2, 2> barriers = {color_barrier, depth_barrier};
    vk::DependencyInfo dependency{};
    dependency.setImageMemoryBarriers(barriers);
    compat::pipelineBarrier(cmd, dependency);

    vk::RenderingAttachmentInfo color_attachment{};
    color_attachment.imageView = *scene_color_.view();
    color_attachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
    color_attachment.loadOp = vk::AttachmentLoadOp::eLoad;
    color_attachment.storeOp = vk::AttachmentStoreOp::eStore;
    vk::RenderingAttachmentInfo depth_attachment{};
    depth_attachment.imageView = *gbuffer_.depth().view();
    depth_attachment.imageLayout = compat::depthReadOnlyLayout();
    depth_attachment.loadOp = vk::AttachmentLoadOp::eLoad;
    depth_attachment.storeOp = vk::AttachmentStoreOp::eNone;
    vk::RenderingInfo rendering_info{};
    rendering_info.renderArea = vk::Rect2D{vk::Offset2D{0, 0}, extent};
    rendering_info.layerCount = 1;
    rendering_info.setColorAttachments(color_attachment);
    rendering_info.pDepthAttachment = &depth_attachment;
    compat::beginRendering(cmd, rendering_info);
    vfx_pass_.recordDraw(cmd, frame_index, extent);
    compat::endRendering(cmd);
}

}  // namespace cramion::gfx
