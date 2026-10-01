// VulkanRenderer: el pase de los liquidos (FluidPass.h). Aparte para no
// engordar VulkanRenderer.cpp.

#include "CramionFX/vk/VulkanRenderer.h"

namespace cramion::gfx {

void VulkanRenderer::recordFluidPass(const vk::raii::CommandBuffer& cmd, std::uint32_t frame_index) {
    const vk::Extent2D extent = render_extent_;
    if (extent.width == 0 || extent.height == 0) return;

    // La misma proyeccion (con jitter) que la escena: el liquido se prueba
    // contra su profundidad pixel a pixel.
    FluidPass::View view;
    view.view = camera_view_;
    const bool temporal = upscaling_ && graphics_.upscaler != Upscaler::Fsr1 && !isolated();
    view.projection = temporal ? core::translate(core::Vec3{jitter_ndc_.x, jitter_ndc_.y, 0.0f}) * camera_projection_
                               : camera_projection_;
    view.extent = extent;
    view.scene_depth = *gbuffer_.depth().view();
    fluid_pass_.recordPrepare(cmd, frame_index, view);

    // Lo que hay detras (refraccion) y el sombreado sobre la imagen HDR.
    copySceneForTransparency(cmd);
    vk::RenderingAttachmentInfo color_attachment{};
    color_attachment.imageView = *scene_color_.view();
    color_attachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
    color_attachment.loadOp = vk::AttachmentLoadOp::eLoad;
    color_attachment.storeOp = vk::AttachmentStoreOp::eStore;
    vk::RenderingInfo rendering_info{};
    rendering_info.renderArea = vk::Rect2D{vk::Offset2D{0, 0}, extent};
    rendering_info.layerCount = 1;
    rendering_info.setColorAttachments(color_attachment);
    cmd.beginRendering(rendering_info);
    fluid_pass_.recordShade(cmd, frame_index, glass_sets_[frame_index], extent);
    cmd.endRendering();
}

}  // namespace cramion::gfx
