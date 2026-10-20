#include "CramionFX/vk/DecalAtlas.h"

#include "CramionFX/vk/VulkanBuffer.h"
#include "CramionFX/vk/VulkanCompat.h"
#include "CramionFX/vk/VulkanDevice.h"

#include <algorithm>
#include <bit>
#include <iostream>
#include <vector>

namespace cramion::gfx {

namespace {

constexpr vk::Format kFormat = vk::Format::eR8G8B8A8Unorm;
constexpr std::uint32_t kFirstCapacity = 4;

vk::ImageMemoryBarrier2 imageBarrier(vk::Image image, vk::ImageSubresourceRange range, vk::ImageLayout from,
                                     vk::ImageLayout to, vk::PipelineStageFlags2 src_stage, vk::AccessFlags2 src_access,
                                     vk::PipelineStageFlags2 dst_stage, vk::AccessFlags2 dst_access) {
    vk::ImageMemoryBarrier2 barrier{};
    barrier.srcStageMask = src_stage;
    barrier.srcAccessMask = src_access;
    barrier.dstStageMask = dst_stage;
    barrier.dstAccessMask = dst_access;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.image = image;
    barrier.subresourceRange = range;
    return barrier;
}

void barrier(const vk::raii::CommandBuffer& cmd, const vk::ImageMemoryBarrier2& b) {
    vk::DependencyInfo dependency{};
    dependency.setImageMemoryBarriers(b);
    compat::pipelineBarrier(cmd, dependency);
}

vk::ImageSubresourceRange colorRange(std::uint32_t first_level, std::uint32_t levels, std::uint32_t first_layer,
                                     std::uint32_t layers) {
    return vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, first_level, levels, first_layer, layers};
}

}  // namespace

std::shared_ptr<DecalAtlas::Storage> DecalAtlas::createStorage(const VulkanDevice& device, std::uint32_t size,
                                                               std::uint32_t levels, std::uint32_t layers) const {
    auto storage = std::make_shared<Storage>();
    vk::ImageCreateInfo info{};
    info.imageType = vk::ImageType::e2D;
    info.format = kFormat;
    info.extent = vk::Extent3D{size, size, 1};
    info.mipLevels = levels;
    info.arrayLayers = layers;
    info.samples = vk::SampleCountFlagBits::e1;
    info.tiling = vk::ImageTiling::eOptimal;
    // Origen y destino: los mips se hacen por blit y al crecer se copian las capas.
    info.usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst |
                 vk::ImageUsageFlagBits::eTransferSrc;
    info.sharingMode = vk::SharingMode::eExclusive;
    info.initialLayout = vk::ImageLayout::eUndefined;
    storage->image = vk::raii::Image(device.handle(), info);
    const vk::MemoryRequirements requirements = storage->image.getMemoryRequirements();
    vk::MemoryAllocateInfo allocate{};
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = device.findMemoryType(requirements.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal);
    storage->memory = vk::raii::DeviceMemory(device.handle(), allocate);
    storage->ticket = GpuMemoryTicket(GpuMemoryKind::Texture, requirements.size);
    storage->image.bindMemory(*storage->memory, 0);
    vk::ImageViewCreateInfo view{};
    view.image = *storage->image;
    view.viewType = vk::ImageViewType::e2DArray;  // siempre array (sampler2DArray)
    view.format = kFormat;
    view.subresourceRange = colorRange(0, levels, 0, layers);
    storage->view = vk::raii::ImageView(device.handle(), view);
    return storage;
}

void DecalAtlas::create(const VulkanDevice& device, std::uint32_t layer_size, std::uint32_t max_layers) {
    destroy();
    const vk::PhysicalDeviceLimits limits = device.physicalDevice().getProperties().limits;
    layer_size_ = std::clamp(std::bit_floor(std::max(layer_size, 1u)), 1u, limits.maxImageDimension2D);
    levels_ = static_cast<std::uint32_t>(std::bit_width(layer_size_));
    max_layers_ = std::clamp(max_layers, 1u, limits.maxImageArrayLayers);

    // El ficticio: 1x1x1 blanco.
    storage_ = createStorage(device, 1, 1, 1);
    capacity_ = 0;
    used_ = 0;
    const std::uint8_t white[4] = {255, 255, 255, 255};
    VulkanBuffer staging;
    staging.create(device, sizeof(white), vk::BufferUsageFlagBits::eTransferSrc,
                   vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
    staging.write(white, sizeof(white));
    const vk::Image image = *storage_->image;
    device.submitOneTime([&](const vk::raii::CommandBuffer& cmd) {
        barrier(cmd, imageBarrier(image, colorRange(0, 1, 0, 1), vk::ImageLayout::eUndefined,
                                  vk::ImageLayout::eTransferDstOptimal, vk::PipelineStageFlagBits2::eNone,
                                  vk::AccessFlagBits2::eNone, vk::PipelineStageFlagBits2::eTransfer,
                                  vk::AccessFlagBits2::eTransferWrite));
        vk::BufferImageCopy region{};
        region.imageSubresource = vk::ImageSubresourceLayers{vk::ImageAspectFlagBits::eColor, 0, 0, 1};
        region.imageExtent = vk::Extent3D{1, 1, 1};
        cmd.copyBufferToImage(*staging.handle(), image, vk::ImageLayout::eTransferDstOptimal, region);
        barrier(cmd, imageBarrier(image, colorRange(0, 1, 0, 1), vk::ImageLayout::eTransferDstOptimal,
                                  vk::ImageLayout::eShaderReadOnlyOptimal, vk::PipelineStageFlagBits2::eTransfer,
                                  vk::AccessFlagBits2::eTransferWrite, vk::PipelineStageFlagBits2::eFragmentShader,
                                  vk::AccessFlagBits2::eShaderSampledRead));
    });
}

void DecalAtlas::destroy() {
    storage_.reset();
    capacity_ = 0;
    used_ = 0;
}

vk::ImageView DecalAtlas::view() const {
    return storage_ ? *storage_->view : vk::ImageView{};
}

int DecalAtlas::add(const VulkanDevice& device, std::uint32_t width, std::uint32_t height, const std::uint8_t* rgba) {
    if (!storage_ || rgba == nullptr || width == 0 || height == 0) return -1;
    if (used_ >= max_layers_) {
        std::cerr << "[Vulkan] Atlas de decals lleno (" << max_layers_ << " imagenes de " << layer_size_ << " px)\n";
        return -1;
    }
    const std::uint32_t layer = used_;

    // Crece si no cabe: la imagen nueva copia las capas ya subidas (todos sus
    // mips). La anterior (tambien el ficticio) la mantiene viva el propio
    // envio: va detras en la cola de todo frame que la tenga en su set, y los
    // sets se reescriben antes de su siguiente frame.
    std::shared_ptr<Storage> previous;
    std::uint32_t copy_layers = 0;
    bool fresh = false;  // imagen recien creada: todas sus capas sin layout
    if (layer >= capacity_) {
        const std::uint32_t capacity = std::min(capacity_ == 0 ? kFirstCapacity : capacity_ * 2, max_layers_);
        std::shared_ptr<Storage> grown;
        try {
            grown = createStorage(device, layer_size_, levels_, capacity);
        } catch (const std::exception& error) {
            std::cerr << "[Vulkan] No se pudo agrandar el atlas de decals a " << capacity << " capas: " << error.what()
                      << "\n";
            return -1;
        }
        previous = storage_;
        copy_layers = capacity_ > 0 ? used_ : 0u;
        storage_ = std::move(grown);
        capacity_ = capacity;
        fresh = true;
    }

    auto staging = std::make_shared<VulkanBuffer>();
    const vk::DeviceSize bytes = static_cast<vk::DeviceSize>(width) * height * 4;
    staging->create(device, bytes, vk::BufferUsageFlagBits::eTransferSrc,
                    vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
    staging->write(rgba, static_cast<std::size_t>(bytes));

    // La imagen tal cual llega (un nivel): origen del blit que la escala a la capa.
    std::shared_ptr<Storage> source;
    {
        source = std::make_shared<Storage>();
        vk::ImageCreateInfo info{};
        info.imageType = vk::ImageType::e2D;
        info.format = kFormat;
        info.extent = vk::Extent3D{width, height, 1};
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = vk::SampleCountFlagBits::e1;
        info.tiling = vk::ImageTiling::eOptimal;
        info.usage = vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eTransferSrc;
        info.sharingMode = vk::SharingMode::eExclusive;
        info.initialLayout = vk::ImageLayout::eUndefined;
        source->image = vk::raii::Image(device.handle(), info);
        const vk::MemoryRequirements requirements = source->image.getMemoryRequirements();
        vk::MemoryAllocateInfo allocate{};
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex =
            device.findMemoryType(requirements.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal);
        source->memory = vk::raii::DeviceMemory(device.handle(), allocate);
        source->image.bindMemory(*source->memory, 0);
    }

    const vk::Image atlas = *storage_->image;
    const vk::Image src = *source->image;
    const vk::Image old = previous ? *previous->image : vk::Image{};
    const std::uint32_t levels = levels_;
    const std::uint32_t size = layer_size_;
    const bool grew = fresh;
    const std::uint32_t capacity = capacity_;
    // Sin esperar (el hilo principal no se para): la cola lo pone detras de los
    // frames ya enviados y delante del que va a grabarse; el staging, la
    // temporal y la imagen anterior viven hasta que acaba (collectAsync).
    std::vector<std::shared_ptr<const void>> keep_alive = {staging, source};
    if (previous) keep_alive.push_back(previous);
    device.submitAsync([&](const vk::raii::CommandBuffer& cmd) {
        using Stage = vk::PipelineStageFlagBits2;
        using Access = vk::AccessFlagBits2;
        // Imagen nueva: todo a destino de copia (las capas libres tambien, para
        // que la vista entera tenga un layout valido al leerla).
        if (grew) {
            barrier(cmd, imageBarrier(atlas, colorRange(0, levels, 0, capacity), vk::ImageLayout::eUndefined,
                                      vk::ImageLayout::eTransferDstOptimal, Stage::eNone, Access::eNone,
                                      Stage::eTransfer, Access::eTransferWrite));
        } else {
            // La capa nueva (nadie la lee aun: ningun decal la usa).
            barrier(cmd, imageBarrier(atlas, colorRange(0, levels, layer, 1), vk::ImageLayout::eUndefined,
                                      vk::ImageLayout::eTransferDstOptimal, Stage::eNone, Access::eNone,
                                      Stage::eTransfer, Access::eTransferWrite));
        }
        // Las capas que ya tenia: de la imagen anterior (los frames que la leen
        // van antes en la cola; la barrera espera a todo lo anterior).
        if (copy_layers > 0) {
            barrier(cmd, imageBarrier(old, colorRange(0, levels, 0, copy_layers), vk::ImageLayout::eShaderReadOnlyOptimal,
                                      vk::ImageLayout::eTransferSrcOptimal, Stage::eAllCommands,
                                      Access::eShaderSampledRead, Stage::eTransfer, Access::eTransferRead));
            std::vector<vk::ImageCopy> copies;
            for (std::uint32_t level = 0; level < levels; ++level) {
                const std::uint32_t extent = std::max(size >> level, 1u);
                vk::ImageCopy copy{};
                copy.srcSubresource = vk::ImageSubresourceLayers{vk::ImageAspectFlagBits::eColor, level, 0, copy_layers};
                copy.dstSubresource = copy.srcSubresource;
                copy.extent = vk::Extent3D{extent, extent, 1};
                copies.push_back(copy);
            }
            cmd.copyImage(old, vk::ImageLayout::eTransferSrcOptimal, atlas, vk::ImageLayout::eTransferDstOptimal, copies);
        }

        // La imagen original a la temporal y de ahi, escalada, al nivel 0 de la capa.
        barrier(cmd, imageBarrier(src, colorRange(0, 1, 0, 1), vk::ImageLayout::eUndefined,
                                  vk::ImageLayout::eTransferDstOptimal, Stage::eNone, Access::eNone, Stage::eTransfer,
                                  Access::eTransferWrite));
        vk::BufferImageCopy region{};
        region.imageSubresource = vk::ImageSubresourceLayers{vk::ImageAspectFlagBits::eColor, 0, 0, 1};
        region.imageExtent = vk::Extent3D{width, height, 1};
        cmd.copyBufferToImage(*staging->handle(), src, vk::ImageLayout::eTransferDstOptimal, region);
        barrier(cmd, imageBarrier(src, colorRange(0, 1, 0, 1), vk::ImageLayout::eTransferDstOptimal,
                                  vk::ImageLayout::eTransferSrcOptimal, Stage::eTransfer, Access::eTransferWrite,
                                  Stage::eTransfer, Access::eTransferRead));
        {
            vk::ImageBlit blit{};
            blit.srcSubresource = vk::ImageSubresourceLayers{vk::ImageAspectFlagBits::eColor, 0, 0, 1};
            blit.srcOffsets[1] = vk::Offset3D{static_cast<std::int32_t>(width), static_cast<std::int32_t>(height), 1};
            blit.dstSubresource = vk::ImageSubresourceLayers{vk::ImageAspectFlagBits::eColor, 0, layer, 1};
            blit.dstOffsets[1] = vk::Offset3D{static_cast<std::int32_t>(size), static_cast<std::int32_t>(size), 1};
            cmd.blitImage(src, vk::ImageLayout::eTransferSrcOptimal, atlas, vk::ImageLayout::eTransferDstOptimal, blit,
                          vk::Filter::eLinear);
        }
        // Los mips de la capa (cada nivel, origen del siguiente).
        for (std::uint32_t level = 1; level < levels; ++level) {
            barrier(cmd, imageBarrier(atlas, colorRange(level - 1, 1, layer, 1), vk::ImageLayout::eTransferDstOptimal,
                                      vk::ImageLayout::eTransferSrcOptimal, Stage::eTransfer, Access::eTransferWrite,
                                      Stage::eTransfer, Access::eTransferRead));
            const auto from = static_cast<std::int32_t>(std::max(size >> (level - 1), 1u));
            const auto to = static_cast<std::int32_t>(std::max(size >> level, 1u));
            vk::ImageBlit blit{};
            blit.srcSubresource = vk::ImageSubresourceLayers{vk::ImageAspectFlagBits::eColor, level - 1, layer, 1};
            blit.srcOffsets[1] = vk::Offset3D{from, from, 1};
            blit.dstSubresource = vk::ImageSubresourceLayers{vk::ImageAspectFlagBits::eColor, level, layer, 1};
            blit.dstOffsets[1] = vk::Offset3D{to, to, 1};
            cmd.blitImage(atlas, vk::ImageLayout::eTransferSrcOptimal, atlas, vk::ImageLayout::eTransferDstOptimal, blit,
                          vk::Filter::eLinear);
            barrier(cmd, imageBarrier(atlas, colorRange(level - 1, 1, layer, 1), vk::ImageLayout::eTransferSrcOptimal,
                                      vk::ImageLayout::eTransferDstOptimal, Stage::eTransfer, Access::eTransferRead,
                                      Stage::eTransfer, Access::eTransferWrite));
        }
        // Todo lo tocado, listo para los shaders: la imagen entera si es nueva,
        // si no solo la capa.
        const vk::ImageSubresourceRange done = grew ? colorRange(0, levels, 0, capacity) : colorRange(0, levels, layer, 1);
        barrier(cmd, imageBarrier(atlas, done, vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
                                  Stage::eTransfer, Access::eTransferWrite, Stage::eFragmentShader,
                                  Access::eShaderSampledRead));
    }, std::move(keep_alive));
    ++used_;
    return static_cast<int>(layer);
}

}  // namespace cramion::gfx
