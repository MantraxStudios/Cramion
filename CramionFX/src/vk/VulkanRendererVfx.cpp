// VulkanRenderer: el pase del VFX Graph (VfxPass.h). Aparte para no engordar
// VulkanRenderer.cpp.

#include "CramionFX/vk/VulkanRenderer.h"

namespace cramion::gfx {

void VulkanRenderer::recordVfxPass(const vk::raii::CommandBuffer& cmd, std::uint32_t frame_index) {
    const vk::Extent2D extent = scene_color_.extent();
    if (extent.width == 0 || extent.height == 0) return;

    VfxView view;
    view.view = camera_view_;
    view.projection = camera_projection_;
    view.view_projection = camera_view_projection_;
    view.camera_position = camera_position_;
    view.light = precipitation_light_;
    view.extent = gbuffer_.depth().extent();
    view.scene_depth = *gbuffer_.depth().view();
    // Las vistas secundarias (vista Juego, sondas) solo dibujan: la
    // simulacion avanza una vez por frame con la vista principal.
    const bool simulate = !isolated();
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
    depth_barrier.oldLayout = vk::ImageLayout::eDepthReadOnlyOptimal;
    depth_barrier.newLayout = vk::ImageLayout::eDepthReadOnlyOptimal;
    depth_barrier.image = *gbuffer_.depth().handle();
    depth_barrier.subresourceRange = vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1};
    const std::array<vk::ImageMemoryBarrier2, 2> barriers = {color_barrier, depth_barrier};
    vk::DependencyInfo dependency{};
    dependency.setImageMemoryBarriers(barriers);
    cmd.pipelineBarrier2(dependency);

    vk::RenderingAttachmentInfo color_attachment{};
    color_attachment.imageView = *scene_color_.view();
    color_attachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
    color_attachment.loadOp = vk::AttachmentLoadOp::eLoad;
    color_attachment.storeOp = vk::AttachmentStoreOp::eStore;
    vk::RenderingAttachmentInfo depth_attachment{};
    depth_attachment.imageView = *gbuffer_.depth().view();
    depth_attachment.imageLayout = vk::ImageLayout::eDepthReadOnlyOptimal;
    depth_attachment.loadOp = vk::AttachmentLoadOp::eLoad;
    depth_attachment.storeOp = vk::AttachmentStoreOp::eNone;
    vk::RenderingInfo rendering_info{};
    rendering_info.renderArea = vk::Rect2D{vk::Offset2D{0, 0}, extent};
    rendering_info.layerCount = 1;
    rendering_info.setColorAttachments(color_attachment);
    rendering_info.pDepthAttachment = &depth_attachment;
    cmd.beginRendering(rendering_info);
    vfx_pass_.recordDraw(cmd, frame_index, extent);
    cmd.endRendering();
}

}  // namespace cramion::gfx
