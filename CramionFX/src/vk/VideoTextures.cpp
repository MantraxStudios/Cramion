#include "CramionFX/vk/VideoTextures.h"

#include "CramionFX/vk/VulkanCompat.h"
#include "CramionFX/vk/VulkanDevice.h"

#include <algorithm>
#include <cstring>
#include <iostream>

namespace cramion::gfx {

namespace {

using Stage = vk::PipelineStageFlagBits2;
using Access = vk::AccessFlagBits2;

constexpr std::uint32_t kMaxVideoSlots = 64;  // texturas con video a la vez (sets por frame)
constexpr std::uint32_t kMaxSide = 8192;

struct ConvertPush {
    float rect[4];
    std::int32_t mode[4];
    float params[4];
};
static_assert(sizeof(ConvertPush) == 48);

struct PlanePush {
    float rect[4];
    float params[4];
};
static_assert(sizeof(PlanePush) == 32);

vk::ImageMemoryBarrier2 imageBarrier(vk::Image image, vk::ImageLayout old_layout, vk::ImageLayout new_layout,
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

void barriers(const vk::raii::CommandBuffer& cmd, vk::ArrayProxy<const vk::ImageMemoryBarrier2> list) {
    vk::DependencyInfo dependency{};
    dependency.imageMemoryBarrierCount = list.size();
    dependency.pImageMemoryBarriers = list.data();
    compat::pipelineBarrier(cmd, dependency);
}

// Bytes por fila de cada plano (sin relleno) y su numero de filas.
struct PlaneShape {
    std::uint32_t row_bytes = 0;
    std::uint32_t rows = 0;
};
std::array<PlaneShape, 2> planeShapes(const VideoUpload& u) {
    if (u.layout == VideoUpload::Layout::Nv12) {
        return {PlaneShape{u.width, u.height}, PlaneShape{((u.width + 1) / 2) * 2, (u.height + 1) / 2}};
    }
    return {PlaneShape{u.width * 4, u.height}, PlaneShape{}};
}

std::size_t align16(std::size_t v) { return (v + 15) & ~static_cast<std::size_t>(15); }

}  // namespace

VideoRect videoContentRect(int aspect, float src_w, float src_h, float dst_w, float dst_h) {
    VideoRect r{0.0f, 0.0f, dst_w, dst_h};
    if (src_w <= 0.0f || src_h <= 0.0f || dst_w <= 0.0f || dst_h <= 0.0f) return r;
    float scale_x = dst_w / src_w;
    float scale_y = dst_h / src_h;
    switch (aspect) {
        case 0:  // Stretch
            return r;
        case 2:  // FitOutside: llena y recorta
            scale_x = scale_y = std::max(scale_x, scale_y);
            break;
        case 3:  // FitHorizontally
            scale_y = scale_x;
            break;
        case 4:  // FitVertically
            scale_x = scale_y;
            break;
        case 5:  // NoScaling: pixel a pixel
            scale_x = scale_y = 1.0f;
            break;
        default:  // 1 FitInside: entero, con bandas
            scale_x = scale_y = std::min(scale_x, scale_y);
            break;
    }
    r.w = src_w * scale_x;
    r.h = src_h * scale_y;
    r.x = (dst_w - r.w) * 0.5f;
    r.y = (dst_h - r.h) * 0.5f;
    return r;
}

void VideoTextures::create(const VulkanDevice& device, vk::Format ldr_format, std::uint32_t frames_in_flight) {
    destroy();
    frames_in_flight_ = std::max(frames_in_flight, 1u);
    try {
        using Type = vk::DescriptorType;
        const std::array<Type, 3> convert_bindings = {Type::eCombinedImageSampler, Type::eCombinedImageSampler,
                                                      Type::eStorageImage};
        ComputePassDesc convert{};
        convert.shader = "video_yuv.comp.spv";
        convert.bindings = convert_bindings;
        convert.push_constant_size = sizeof(ConvertPush);
        convert_.create(device, convert);

        const std::array<Type, 2> plane_bindings = {Type::eCombinedImageSampler, Type::eCombinedImageSampler};
        FullscreenPassDesc plane{};
        plane.fragment_shader = "video_plane.frag.spv";
        plane.bindings = plane_bindings;
        plane.push_constant_size = sizeof(PlanePush);
        plane.color_format = ldr_format;
        plane.alpha_blend = true;
        plane_pass_.create(device, plane);

        vk::SamplerCreateInfo sampler_info{};
        sampler_info.magFilter = vk::Filter::eLinear;
        sampler_info.minFilter = vk::Filter::eLinear;
        sampler_info.mipmapMode = vk::SamplerMipmapMode::eNearest;
        sampler_info.addressModeU = vk::SamplerAddressMode::eClampToEdge;
        sampler_info.addressModeV = vk::SamplerAddressMode::eClampToEdge;
        sampler_info.addressModeW = vk::SamplerAddressMode::eClampToEdge;
        linear_sampler_ = vk::raii::Sampler(device.handle(), sampler_info);
        // La profundidad se lee con texelFetch: sin filtrar (no todos los
        // formatos de profundidad admiten filtro lineal).
        sampler_info.magFilter = vk::Filter::eNearest;
        sampler_info.minFilter = vk::Filter::eNearest;
        nearest_sampler_ = vk::raii::Sampler(device.handle(), sampler_info);

        const std::uint32_t convert_sets = kMaxVideoSlots * frames_in_flight_;
        const std::uint32_t plane_sets = 2 * frames_in_flight_;
        const std::array<vk::DescriptorPoolSize, 2> sizes = {
            vk::DescriptorPoolSize{Type::eCombinedImageSampler, convert_sets * 2 + plane_sets * 2},
            vk::DescriptorPoolSize{Type::eStorageImage, convert_sets}};
        vk::DescriptorPoolCreateInfo pool_info{};
        pool_info.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
        pool_info.maxSets = convert_sets + plane_sets;
        pool_info.setPoolSizes(sizes);
        pool_ = vk::raii::DescriptorPool(device.handle(), pool_info);

        std::vector<vk::DescriptorSetLayout> layouts(plane_sets, *plane_pass_.descriptorSetLayout());
        vk::DescriptorSetAllocateInfo alloc{};
        alloc.descriptorPool = *pool_;
        alloc.setSetLayouts(layouts);
        plane_sets_ = vk::raii::DescriptorSets(device.handle(), alloc);
        created_ = true;
    } catch (const std::exception& e) {
        std::cerr << "[Video] No se pudieron crear las pasadas de video: " << e.what() << "\n";
        destroy();
    }
}

void VideoTextures::destroy() {
    created_ = false;
    {
        const std::lock_guard lock(mutex_);
        slots_.clear();
    }
    retired_.clear();
    planes_ = {};
    plane_sets_.clear();
    pool_ = nullptr;
    linear_sampler_ = nullptr;
    nearest_sampler_ = nullptr;
    plane_pass_.destroy();
    convert_.destroy();
}

VideoTextures::Slot& VideoTextures::slotFor(std::int32_t id) {
    std::unique_ptr<Slot>& slot = slots_[id];
    if (!slot) slot = std::make_unique<Slot>();
    return *slot;
}

bool VideoTextures::submit(std::int32_t id, const VideoUpload& upload) {
    if (!created_ || id < 0) return false;
    if (upload.width == 0 || upload.height == 0 || upload.width > kMaxSide || upload.height > kMaxSide) return false;
    if (upload.planes[0] == nullptr) return false;
    if (upload.layout == VideoUpload::Layout::Nv12 && upload.planes[1] == nullptr) return false;
    const std::array<PlaneShape, 2> shapes = planeShapes(upload);

    const std::lock_guard lock(mutex_);
    Slot& slot = slotFor(id);
    const std::size_t size0 = static_cast<std::size_t>(shapes[0].row_bytes) * shapes[0].rows;
    const std::size_t size1 = static_cast<std::size_t>(shapes[1].row_bytes) * shapes[1].rows;
    slot.offset1 = align16(size0);
    // Solo el ultimo frame: el anterior sin grabar se pisa (nunca se acumulan).
    slot.data.resize(slot.offset1 + size1);
    for (int p = 0; p < 2; ++p) {
        const PlaneShape& shape = shapes[p];
        if (shape.rows == 0) continue;
        const std::uint32_t stride = std::max(upload.strides[p], shape.row_bytes);
        std::uint8_t* dst = slot.data.data() + (p == 0 ? 0 : slot.offset1);
        const std::uint8_t* src = upload.planes[p];
        if (stride == shape.row_bytes) {
            std::memcpy(dst, src, static_cast<std::size_t>(shape.row_bytes) * shape.rows);
        } else {
            for (std::uint32_t row = 0; row < shape.rows; ++row) {
                std::memcpy(dst + static_cast<std::size_t>(row) * shape.row_bytes,
                            src + static_cast<std::size_t>(row) * stride, shape.row_bytes);
            }
        }
    }
    slot.params = upload;
    slot.params.planes[0] = slot.params.planes[1] = nullptr;
    slot.dirty = true;
    return true;
}

void VideoTextures::forget(std::int32_t id) {
    std::unique_ptr<Slot> slot;
    {
        const std::lock_guard lock(mutex_);
        const auto it = slots_.find(id);
        if (it != slots_.end()) {
            slot = std::move(it->second);
            slots_.erase(it);
        }
    }
    if (slot) retire(std::move(slot));
    for (Plane& plane : planes_) {
        if (plane.id == id) plane = Plane{};
    }
}

bool VideoTextures::hasPending() const {
    const std::lock_guard lock(mutex_);
    for (const auto& [id, slot] : slots_) {
        if (slot->dirty) return true;
    }
    return false;
}

void VideoTextures::retire(std::unique_ptr<Slot> slot) {
    Retired retired;
    retired.slot = std::move(slot);
    retired.frames_left = frames_in_flight_ + 1;
    retired_.push_back(std::move(retired));
}

void VideoTextures::retireImage(VulkanImage& image) {
    if (!image.isValid()) return;
    Retired retired;
    retired.image = std::move(image);
    retired.frames_left = frames_in_flight_ + 1;
    retired_.push_back(std::move(retired));
    image = VulkanImage{};
}

void VideoTextures::collectRetired() {
    for (Retired& retired : retired_) {
        if (retired.frames_left > 0) --retired.frames_left;
    }
    std::erase_if(retired_, [](const Retired& r) { return r.frames_left == 0; });
}

void VideoTextures::recordUploads(const VulkanDevice& device, const vk::raii::CommandBuffer& cmd, std::uint32_t frame,
                                  const Resolve& resolve) {
    if (!created_) return;
    const std::lock_guard lock(mutex_);
    frame %= frames_in_flight_;
    for (auto& [id, slot_ptr] : slots_) {
        Slot& slot = *slot_ptr;
        if (!slot.dirty) continue;
        slot.dirty = false;
        const VideoTarget target = resolve(id);
        if (!target.valid()) continue;  // la Render Texture ya no existe
        const VideoUpload& u = slot.params;
        try {
            // Staging del hueco de frame: su fence ya se espero (el command
            // buffer que lo leyo termino), asi que se puede pisar o rehacer.
            if (slot.staging.size() != frames_in_flight_) slot.staging.resize(frames_in_flight_);
            VulkanBuffer& staging = slot.staging[frame];
            if (!staging.isValid() || staging.size() < slot.data.size()) {
                staging = VulkanBuffer{};
                staging.create(device, std::max<vk::DeviceSize>(slot.data.size(), 4096),
                               vk::BufferUsageFlagBits::eTransferSrc,
                               vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
            }
            std::memcpy(staging.mapped(), slot.data.data(), slot.data.size());

            const bool direct = u.layout == VideoUpload::Layout::Rgba8 && u.width == target.extent.width &&
                                u.height == target.extent.height && !u.linear_out && u.alpha >= 1.0f;
            if (direct) {
                // RGBA del mismo tamano: directo a la Render Texture.
                barriers(cmd, imageBarrier(target.image, vk::ImageLayout::eShaderReadOnlyOptimal,
                                           vk::ImageLayout::eTransferDstOptimal,
                                           Stage::eFragmentShader | Stage::eComputeShader | Stage::eTransfer,
                                           Access::eShaderSampledRead | Access::eTransferRead, Stage::eTransfer,
                                           Access::eTransferWrite));
                vk::BufferImageCopy copy{};
                copy.imageSubresource = vk::ImageSubresourceLayers{vk::ImageAspectFlagBits::eColor, 0, 0, 1};
                copy.imageExtent = vk::Extent3D{u.width, u.height, 1};
                cmd.copyBufferToImage(*staging.handle(), target.image, vk::ImageLayout::eTransferDstOptimal, copy);
                barriers(cmd, imageBarrier(target.image, vk::ImageLayout::eTransferDstOptimal,
                                           vk::ImageLayout::eShaderReadOnlyOptimal, Stage::eTransfer,
                                           Access::eTransferWrite, Stage::eFragmentShader | Stage::eComputeShader,
                                           Access::eShaderSampledRead));
                continue;
            }

            // Planos internos (se rehacen si cambia el formato o el tamano del
            // video: p. ej. otra variante de un stream). No estan en ningun
            // material: nada que reenlazar, solo retirarlos unos frames.
            const bool nv12 = u.layout == VideoUpload::Layout::Nv12;
            if (!slot.plane0.isValid() || slot.plane_layout != u.layout || slot.plane_width != u.width ||
                slot.plane_height != u.height) {
                retireImage(slot.plane0);
                retireImage(slot.plane1);
                const vk::ImageUsageFlags usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst;
                slot.plane0.create(device, vk::Extent2D{u.width, u.height},
                                   nv12 ? vk::Format::eR8Unorm : vk::Format::eR8G8B8A8Unorm, usage,
                                   vk::ImageAspectFlagBits::eColor);
                if (nv12) {
                    slot.plane1.create(device, vk::Extent2D{(u.width + 1) / 2, (u.height + 1) / 2},
                                       vk::Format::eR8G8Unorm, usage, vk::ImageAspectFlagBits::eColor);
                }
                slot.plane_layout = u.layout;
                slot.plane_width = u.width;
                slot.plane_height = u.height;
            }
            // Imagen de salida: el tamano actual de la Render Texture (una .crrt
            // editada cambia de tamano).
            if (!slot.output.isValid() || slot.output.extent() != target.extent) {
                retireImage(slot.output);
                slot.output.create(device, target.extent, vk::Format::eR8G8B8A8Unorm,
                                   vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferSrc,
                                   vk::ImageAspectFlagBits::eColor);
            }
            if (slot.sets.size() != frames_in_flight_) {
                slot.sets.clear();
                std::vector<vk::DescriptorSetLayout> layouts(frames_in_flight_, *convert_.descriptorSetLayout());
                vk::DescriptorSetAllocateInfo alloc{};
                alloc.descriptorPool = *pool_;
                alloc.setSetLayouts(layouts);
                slot.sets = vk::raii::DescriptorSets(device.handle(), alloc);
            }

            // Planos: se sobrescriben enteros (layout indefinido), despues de
            // que el compute del frame anterior los leyera.
            std::vector<vk::ImageMemoryBarrier2> to_dst = {
                imageBarrier(*slot.plane0.handle(), vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
                             Stage::eComputeShader, Access::eShaderSampledRead, Stage::eTransfer, Access::eTransferWrite)};
            if (nv12) {
                to_dst.push_back(imageBarrier(*slot.plane1.handle(), vk::ImageLayout::eUndefined,
                                              vk::ImageLayout::eTransferDstOptimal, Stage::eComputeShader,
                                              Access::eShaderSampledRead, Stage::eTransfer, Access::eTransferWrite));
            }
            barriers(cmd, to_dst);
            vk::BufferImageCopy copy{};
            copy.imageSubresource = vk::ImageSubresourceLayers{vk::ImageAspectFlagBits::eColor, 0, 0, 1};
            copy.imageExtent = vk::Extent3D{u.width, u.height, 1};
            cmd.copyBufferToImage(*staging.handle(), *slot.plane0.handle(), vk::ImageLayout::eTransferDstOptimal, copy);
            if (nv12) {
                copy.bufferOffset = slot.offset1;
                copy.imageExtent = vk::Extent3D{(u.width + 1) / 2, (u.height + 1) / 2, 1};
                cmd.copyBufferToImage(*staging.handle(), *slot.plane1.handle(), vk::ImageLayout::eTransferDstOptimal,
                                      copy);
            }
            std::vector<vk::ImageMemoryBarrier2> to_read = {
                imageBarrier(*slot.plane0.handle(), vk::ImageLayout::eTransferDstOptimal,
                             vk::ImageLayout::eShaderReadOnlyOptimal, Stage::eTransfer, Access::eTransferWrite,
                             Stage::eComputeShader, Access::eShaderSampledRead),
                imageBarrier(*slot.output.handle(), vk::ImageLayout::eUndefined, vk::ImageLayout::eGeneral,
                             Stage::eTransfer, Access::eTransferRead, Stage::eComputeShader,
                             Access::eShaderStorageWrite)};
            if (nv12) {
                to_read.push_back(imageBarrier(*slot.plane1.handle(), vk::ImageLayout::eTransferDstOptimal,
                                               vk::ImageLayout::eShaderReadOnlyOptimal, Stage::eTransfer,
                                               Access::eTransferWrite, Stage::eComputeShader,
                                               Access::eShaderSampledRead));
            }
            barriers(cmd, to_read);

            // Conversion y escalado.
            const vk::raii::DescriptorSet& set = slot.sets[frame];
            const vk::DescriptorImageInfo plane0_info{*linear_sampler_, *slot.plane0.view(),
                                                      vk::ImageLayout::eShaderReadOnlyOptimal};
            const vk::DescriptorImageInfo plane1_info{*linear_sampler_,
                                                      nv12 ? *slot.plane1.view() : *slot.plane0.view(),
                                                      vk::ImageLayout::eShaderReadOnlyOptimal};
            const vk::DescriptorImageInfo output_info{nullptr, *slot.output.view(), vk::ImageLayout::eGeneral};
            std::array<vk::WriteDescriptorSet, 3> writes{};
            for (std::uint32_t b = 0; b < 3; ++b) {
                writes[b].dstSet = *set;
                writes[b].dstBinding = b;
                writes[b].descriptorCount = 1;
                writes[b].descriptorType = b < 2 ? vk::DescriptorType::eCombinedImageSampler
                                                 : vk::DescriptorType::eStorageImage;
            }
            writes[0].pImageInfo = &plane0_info;
            writes[1].pImageInfo = &plane1_info;
            writes[2].pImageInfo = &output_info;
            device.handle().updateDescriptorSets(writes, nullptr);

            const VideoRect rect = videoContentRect(u.aspect, static_cast<float>(u.width), static_cast<float>(u.height),
                                                    static_cast<float>(target.extent.width),
                                                    static_cast<float>(target.extent.height));
            ConvertPush push{};
            push.rect[0] = rect.x;
            push.rect[1] = rect.y;
            push.rect[2] = rect.w;
            push.rect[3] = rect.h;
            push.mode[0] = nv12 ? 0 : (u.layout == VideoUpload::Layout::Bgra8 ? 2 : 1);
            push.mode[1] = u.matrix == 601 ? 601 : 709;
            push.mode[2] = u.full_range ? 1 : 0;
            push.mode[3] = u.linear_out ? 1 : 0;
            push.params[0] = u.alpha;
            cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *convert_.pipeline());
            cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *convert_.layout(), 0, *set, nullptr);
            cmd.pushConstants<ConvertPush>(*convert_.layout(), vk::ShaderStageFlagBits::eCompute, 0, push);
            cmd.dispatch((target.extent.width + 7) / 8, (target.extent.height + 7) / 8, 1);

            // A la Render Texture (misma imagen y formato que antes: los
            // materiales y la UI que la leen no se enteran).
            barriers(cmd, {imageBarrier(*slot.output.handle(), vk::ImageLayout::eGeneral,
                                        vk::ImageLayout::eTransferSrcOptimal, Stage::eComputeShader,
                                        Access::eShaderStorageWrite, Stage::eTransfer, Access::eTransferRead),
                           imageBarrier(target.image, vk::ImageLayout::eShaderReadOnlyOptimal,
                                        vk::ImageLayout::eTransferDstOptimal,
                                        Stage::eFragmentShader | Stage::eComputeShader | Stage::eTransfer,
                                        Access::eShaderSampledRead | Access::eTransferRead, Stage::eTransfer,
                                        Access::eTransferWrite)});
            vk::ImageCopy region{};
            region.srcSubresource = vk::ImageSubresourceLayers{vk::ImageAspectFlagBits::eColor, 0, 0, 1};
            region.dstSubresource = region.srcSubresource;
            region.extent = vk::Extent3D{target.extent.width, target.extent.height, 1};
            cmd.copyImage(*slot.output.handle(), vk::ImageLayout::eTransferSrcOptimal, target.image,
                          vk::ImageLayout::eTransferDstOptimal, region);
            barriers(cmd, imageBarrier(target.image, vk::ImageLayout::eTransferDstOptimal,
                                       vk::ImageLayout::eShaderReadOnlyOptimal, Stage::eTransfer, Access::eTransferWrite,
                                       Stage::eFragmentShader | Stage::eComputeShader, Access::eShaderSampledRead));
        } catch (const std::exception& e) {
            std::cerr << "[Video] No se pudo subir el frame de la textura " << id << ": " << e.what() << "\n";
        }
    }
}

void VideoTextures::setPlane(int plane, std::int32_t id, float alpha, int aspect) {
    if (plane < 0 || plane > 1) return;
    planes_[plane] = id < 0 ? Plane{} : Plane{id, std::clamp(alpha, 0.0f, 1.0f), aspect};
}

void VideoTextures::recordPlanes(const VulkanDevice& device, const vk::raii::CommandBuffer& cmd, std::uint32_t frame,
                                 const VulkanImage& color, const VulkanImage& depth, vk::ImageLayout depth_layout,
                                 const Resolve& resolve) {
    if (!created_ || !hasPlanes() || !color.isValid() || !depth.isValid()) return;
    frame %= frames_in_flight_;
    struct Draw {
        int plane = 0;
        VideoTarget target;
        float alpha = 1.0f;
        int aspect = 1;
    };
    std::array<Draw, 2> draws{};
    int count = 0;
    for (int p = 0; p < 2; ++p) {  // primero el lejano
        const Plane& plane = planes_[p];
        if (plane.id < 0 || plane.alpha <= 0.0f) continue;
        const VideoTarget target = resolve(plane.id);
        if (!target.valid() || !target.view) continue;
        draws[count++] = Draw{p, target, plane.alpha, plane.aspect};
    }
    if (count == 0) return;

    const vk::Extent2D extent = color.extent();
    vk::ImageMemoryBarrier2 depth_barrier{};
    depth_barrier.srcStageMask = Stage::eLateFragmentTests | Stage::eFragmentShader | Stage::eComputeShader;
    depth_barrier.srcAccessMask = Access::eDepthStencilAttachmentWrite;
    depth_barrier.dstStageMask = Stage::eFragmentShader;
    depth_barrier.dstAccessMask = Access::eShaderSampledRead;
    depth_barrier.oldLayout = depth_layout;
    depth_barrier.newLayout = depth_layout;
    depth_barrier.image = *depth.handle();
    depth_barrier.subresourceRange = vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1};
    barriers(cmd, {imageBarrier(*color.handle(), vk::ImageLayout::eShaderReadOnlyOptimal,
                                vk::ImageLayout::eColorAttachmentOptimal, Stage::eFragmentShader,
                                Access::eShaderSampledRead, Stage::eColorAttachmentOutput,
                                Access::eColorAttachmentRead | Access::eColorAttachmentWrite),
                   depth_barrier});

    vk::RenderingAttachmentInfo color_attachment{};
    color_attachment.imageView = *color.view();
    color_attachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
    color_attachment.loadOp = vk::AttachmentLoadOp::eLoad;
    color_attachment.storeOp = vk::AttachmentStoreOp::eStore;
    vk::RenderingInfo rendering_info{};
    rendering_info.renderArea = vk::Rect2D{vk::Offset2D{0, 0}, extent};
    rendering_info.layerCount = 1;
    rendering_info.setColorAttachments(color_attachment);

    compat::beginRendering(cmd, rendering_info);
    cmd.setViewport(0, vk::Viewport{0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height),
                                    0.0f, 1.0f});
    cmd.setScissor(0, vk::Rect2D{vk::Offset2D{0, 0}, extent});
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *plane_pass_.pipeline());
    for (int i = 0; i < count; ++i) {
        const Draw& draw = draws[i];
        const vk::raii::DescriptorSet& set = plane_sets_[frame * 2 + static_cast<std::uint32_t>(draw.plane)];
        const vk::DescriptorImageInfo video_info{*linear_sampler_, draw.target.view,
                                                 vk::ImageLayout::eShaderReadOnlyOptimal};
        const vk::DescriptorImageInfo depth_info{*nearest_sampler_, *depth.view(), depth_layout};
        std::array<vk::WriteDescriptorSet, 2> writes{};
        for (std::uint32_t b = 0; b < 2; ++b) {
            writes[b].dstSet = *set;
            writes[b].dstBinding = b;
            writes[b].descriptorCount = 1;
            writes[b].descriptorType = vk::DescriptorType::eCombinedImageSampler;
        }
        writes[0].pImageInfo = &video_info;
        writes[1].pImageInfo = &depth_info;
        device.handle().updateDescriptorSets(writes, nullptr);

        const VideoRect rect =
            videoContentRect(draw.aspect, static_cast<float>(draw.target.extent.width),
                             static_cast<float>(draw.target.extent.height), static_cast<float>(extent.width),
                             static_cast<float>(extent.height));
        PlanePush push{};
        push.rect[0] = rect.x / static_cast<float>(extent.width);
        push.rect[1] = rect.y / static_cast<float>(extent.height);
        push.rect[2] = rect.w / static_cast<float>(extent.width);
        push.rect[3] = rect.h / static_cast<float>(extent.height);
        push.params[0] = draw.alpha;
        push.params[1] = draw.plane == 0 ? 1.0f : 0.0f;
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *plane_pass_.layout(), 0, *set, nullptr);
        cmd.pushConstants<PlanePush>(*plane_pass_.layout(), vk::ShaderStageFlagBits::eFragment, 0, push);
        cmd.draw(3, 1, 0, 0);
    }
    compat::endRendering(cmd);
    barriers(cmd, imageBarrier(*color.handle(), vk::ImageLayout::eColorAttachmentOptimal,
                               vk::ImageLayout::eShaderReadOnlyOptimal, Stage::eColorAttachmentOutput,
                               Access::eColorAttachmentWrite, Stage::eFragmentShader, Access::eShaderSampledRead));
}

}  // namespace cramion::gfx
