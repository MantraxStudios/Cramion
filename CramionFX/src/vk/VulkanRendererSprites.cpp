// VulkanRenderer: el pase de los sprites y tilemaps 2D (SpritePass.h). Aparte
// para no engordar VulkanRenderer.cpp.

#include "CramionFX/vk/VulkanCompat.h"
#include "CramionFX/vk/VulkanRenderer.h"

#include <array>

namespace cramion::gfx {

void VulkanRenderer::recordSpritePass(const vk::raii::CommandBuffer& cmd, std::uint32_t frame_index) {
    const vk::Extent2D extent = scene_color_.extent();
    if (extent.width == 0 || extent.height == 0) return;
    if (!sprite_pass_.prepare(device_, frame_index, sprites_, camera_view_projection_)) return;

    // Como las particulas: la imagen HDR sigue como destino de color (solo hay
    // que ordenar las escrituras) y el depth de la escena en solo lectura.
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
    depth_barrier.srcStageMask =
        vk::PipelineStageFlagBits2::eLateFragmentTests | vk::PipelineStageFlagBits2::eFragmentShader;
    depth_barrier.srcAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
    depth_barrier.dstStageMask =
        vk::PipelineStageFlagBits2::eEarlyFragmentTests | vk::PipelineStageFlagBits2::eLateFragmentTests;
    depth_barrier.dstAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentRead;
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
    sprite_pass_.record(cmd, frame_index, extent);
    compat::endRendering(cmd);
}

}  // namespace cramion::gfx
