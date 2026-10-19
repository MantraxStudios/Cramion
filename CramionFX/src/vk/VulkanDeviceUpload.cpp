#include "CramionFX/vk/VulkanDeviceUpload.h"

#include "CramionFX/vk/VulkanBuffer.h"
#include "CramionFX/vk/VulkanCompat.h"
#include "CramionFX/vk/VulkanDevice.h"

#include <vk_mem_alloc.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace cramion::gfx {

namespace {

vk::DeviceSize alignUp(vk::DeviceSize value, vk::DeviceSize alignment) {
    if (alignment <= 1) return value;
    return (value + alignment - 1) / alignment * alignment;
}

}  // namespace

// -----------------------------------------------------------------------------
// StagingRing
// -----------------------------------------------------------------------------

StagingRing::StagingRing(const VulkanDevice& device, vk::DeviceSize capacity) : device_(&device), capacity_(capacity) {
    vk::BufferCreateInfo buffer_info{};
    buffer_info.size = capacity;
    buffer_info.usage = vk::BufferUsageFlagBits::eTransferSrc;
    buffer_info.sharingMode = vk::SharingMode::eExclusive;
    buffer_ = vk::raii::Buffer(device.handle(), buffer_info);

    // Memoria de la CPU visible y coherente (como los staging de antes),
    // mapeada para siempre y con su propia asignacion. (Los usos AUTO de VMA
    // no valen con vmaAllocateMemoryForBuffer: no conoce el buffer.)
    VmaAllocationCreateInfo allocation_info{};
    allocation_info.usage = VMA_MEMORY_USAGE_UNKNOWN;
    allocation_info.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    allocation_info.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
    VmaAllocation allocation = nullptr;
    VmaAllocationInfo info{};
    if (vmaAllocateMemoryForBuffer(device.allocator(), static_cast<VkBuffer>(*buffer_), &allocation_info, &allocation,
                                   &info) != VK_SUCCESS) {
        buffer_ = nullptr;
        throw std::runtime_error("No se pudo reservar el anillo de staging.");
    }
    if (vmaBindBufferMemory(device.allocator(), allocation, static_cast<VkBuffer>(*buffer_)) != VK_SUCCESS) {
        buffer_ = nullptr;
        vmaFreeMemory(device.allocator(), allocation);
        throw std::runtime_error("No se pudo enlazar el anillo de staging.");
    }
    allocation_ = allocation;
    mapped_ = info.pMappedData;
    memory_ticket_ = GpuMemoryTicket(GpuMemoryKind::Staging, info.size);
}

StagingRing::~StagingRing() {
    buffer_ = nullptr;
    if (allocation_ != nullptr) vmaFreeMemory(device_->allocator(), allocation_);
    allocation_ = nullptr;
    memory_ticket_.release();
}

bool StagingRing::allocate(vk::DeviceSize size, vk::DeviceSize alignment, Slice& out) {
    if (size == 0 || size > capacity_ || mapped_ == nullptr) return false;
    const std::lock_guard lock(mutex_);
    vk::DeviceSize begin = 0;
    if (entries_.empty()) {
        begin = 0;
    } else {
        const Entry& front = entries_.front();
        const Entry& back = entries_.back();
        const vk::DeviceSize head = alignUp(back.end, alignment);
        // Tras dar la vuelta, los tramos nuevos quedan por debajo del primero
        // vivo: el hueco libre es [head, front.begin).
        const bool wrapped = back.begin < front.begin;
        if (wrapped) {
            if (head + size > front.begin) return false;
            begin = head;
        } else if (head + size <= capacity_) {
            begin = head;
        } else if (size <= front.begin) {
            begin = 0;  // da la vuelta
        } else {
            return false;
        }
    }
    Entry entry;
    entry.id = next_id_++;
    entry.begin = begin;
    entry.end = begin + size;
    entries_.push_back(entry);
    out.buffer = *buffer_;
    out.offset = begin;
    out.mapped = static_cast<std::byte*>(mapped_) + begin;
    out.id = entry.id;
    return true;
}

void StagingRing::flush(const Slice& slice, vk::DeviceSize size) const {
    vmaFlushAllocation(device_->allocator(), allocation_, slice.offset, size);
}

void StagingRing::release(std::uint64_t id) {
    const std::lock_guard lock(mutex_);
    for (Entry& entry : entries_) {
        if (entry.id == id) {
            entry.released = true;
            break;
        }
    }
    // Avanza la cola por los primeros ya libres (los demas esperan su turno).
    while (!entries_.empty() && entries_.front().released) entries_.pop_front();
}

// -----------------------------------------------------------------------------
// UploadFence
// -----------------------------------------------------------------------------

void UploadFence::finish(std::shared_ptr<GpuTicket> ticket, std::vector<std::shared_ptr<UploadFence>> dependencies) {
    {
        const std::lock_guard lock(mutex_);
        ticket_ = std::move(ticket);
        dependencies_ = std::move(dependencies);
        submitted_ = true;
        if (!ticket_) finished_ = true;
    }
    cv_.notify_all();
}

bool UploadFence::ownDone() const {
    std::shared_ptr<GpuTicket> ticket;
    {
        const std::lock_guard lock(mutex_);
        if (finished_) return true;
        if (!submitted_) return false;
        ticket = ticket_;
    }
    if (ticket && !ticket->done()) return false;
    const std::lock_guard lock(mutex_);
    finished_ = true;
    ticket_.reset();  // la fence ya no hace falta (la suelta collectAsync)
    return true;
}

void UploadFence::ownWait() const {
    std::shared_ptr<GpuTicket> ticket;
    {
        std::unique_lock lock(mutex_);
        // Otro hilo lo esta grabando: se envia sin esperar a nadie, asi que
        // llega (no hay ciclos de espera).
        cv_.wait(lock, [this]() { return submitted_; });
        if (finished_) return;
        ticket = ticket_;
    }
    if (ticket) ticket->wait();
    const std::lock_guard lock(mutex_);
    finished_ = true;
    ticket_.reset();
}

bool UploadFence::done() const {
    if (!ownDone()) return false;
    std::vector<std::shared_ptr<UploadFence>> dependencies;
    {
        const std::lock_guard lock(mutex_);
        dependencies = dependencies_;
    }
    // Solo las directas (cada recurso esta en un unico lote): sin recursion,
    // asi dos lotes que dependen el uno del otro no se buscan sin fin.
    for (const std::shared_ptr<UploadFence>& other : dependencies) {
        if (other && !other->ownDone()) return false;
    }
    return true;
}

void UploadFence::wait() const {
    ownWait();
    std::vector<std::shared_ptr<UploadFence>> dependencies;
    {
        const std::lock_guard lock(mutex_);
        dependencies = dependencies_;
    }
    for (const std::shared_ptr<UploadFence>& other : dependencies) {
        if (other) other->ownWait();
    }
}

// -----------------------------------------------------------------------------
// UploadBatch
// -----------------------------------------------------------------------------

UploadBatch::UploadBatch(const VulkanDevice& device)
    : device_(&device), cross_queue_(device.hasTransferQueue()) {}

UploadBatch::~UploadBatch() {
    if (recorded_ && !submitted_) {
        try {
            submitAndWait();
        } catch (const std::exception& e) {
            std::cerr << "[Vulkan] No se pudo enviar un lote de subida: " << e.what() << "\n";
            fence_->finish(nullptr, {});  // abandonado: que nadie lo espere para siempre
        } catch (...) {
            fence_->finish(nullptr, {});
        }
    } else if (!submitted_) {
        fence_->finish(nullptr, {});  // vacio
    }
    if (StagingRing* ring = device_->stagingRing()) {
        for (const std::uint64_t id : ring_slices_) ring->release(id);
    }
}

void UploadBatch::begin(vk::raii::CommandPool& pool, std::optional<vk::raii::CommandBuffers>& commands,
                        std::uint32_t family) {
    vk::CommandPoolCreateInfo pool_info{};
    pool_info.flags = vk::CommandPoolCreateFlagBits::eTransient;
    pool_info.queueFamilyIndex = family;
    pool = vk::raii::CommandPool(device_->handle(), pool_info);

    vk::CommandBufferAllocateInfo allocate_info{};
    allocate_info.commandPool = *pool;
    allocate_info.level = vk::CommandBufferLevel::ePrimary;
    allocate_info.commandBufferCount = 1;
    commands.emplace(device_->handle(), allocate_info);

    vk::CommandBufferBeginInfo begin_info{};
    begin_info.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
    commands->front().begin(begin_info);
}

const vk::raii::CommandBuffer& UploadBatch::copyCommands() {
    if (submitted_) throw std::runtime_error("UploadBatch: grabar despues de enviar.");
    if (!copy_commands_) {
        begin(copy_pool_, copy_commands_, cross_queue_ ? device_->transferFamily() : device_->graphicsFamily());
    }
    recorded_ = true;
    return copy_commands_->front();
}

const vk::raii::CommandBuffer& UploadBatch::graphicsCommands() {
    if (!cross_queue_) return copyCommands();
    if (submitted_) throw std::runtime_error("UploadBatch: grabar despues de enviar.");
    if (!graphics_commands_) begin(graphics_pool_, graphics_commands_, device_->graphicsFamily());
    recorded_ = true;
    graphics_used_ = true;
    return graphics_commands_->front();
}

UploadBatch::Staging UploadBatch::stage(const void* data, vk::DeviceSize size, vk::DeviceSize alignment) {
    Staging result;
    // copyBufferToImage pide desplazamientos multiplo de 4 (y del texel).
    alignment = std::max<vk::DeviceSize>(alignment, 4);
    StagingRing::Slice slice;
    StagingRing* ring = device_->stagingRing();
    if (ring != nullptr && ring->allocate(size, alignment, slice)) {
        ring_slices_.push_back(slice.id);
        std::memcpy(slice.mapped, data, static_cast<std::size_t>(size));
        ring->flush(slice, size);
        result.buffer = slice.buffer;
        result.offset = slice.offset;
        return result;
    }
    // No cabe en el anillo (lleno o dato enorme): un staging propio, como antes.
    auto own = std::make_shared<VulkanBuffer>();
    own->create(*device_, size, vk::BufferUsageFlagBits::eTransferSrc,
                vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
    own->write(data, size);
    result.buffer = *own->handle();
    result.offset = 0;
    own_staging_.push_back(std::move(own));
    return result;
}

void UploadBatch::handOffBuffer(vk::Buffer buffer) {
    if (!cross_queue_) return;
    vk::BufferMemoryBarrier2 release{};
    release.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
    release.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
    release.srcQueueFamilyIndex = device_->transferFamily();
    release.dstQueueFamilyIndex = device_->graphicsFamily();
    release.buffer = buffer;
    release.offset = 0;
    release.size = VK_WHOLE_SIZE;
    vk::DependencyInfo release_dependency{};
    release_dependency.setBufferMemoryBarriers(release);
    compat::pipelineBarrier(copyCommands(), release_dependency);

    vk::BufferMemoryBarrier2 acquire = release;
    acquire.srcStageMask = vk::PipelineStageFlagBits2::eNone;
    acquire.srcAccessMask = vk::AccessFlagBits2::eNone;
    acquire.dstStageMask = vk::PipelineStageFlagBits2::eAllCommands;
    acquire.dstAccessMask = vk::AccessFlagBits2::eMemoryRead;
    vk::DependencyInfo acquire_dependency{};
    acquire_dependency.setBufferMemoryBarriers(acquire);
    compat::pipelineBarrier(graphicsCommands(), acquire_dependency);
}

void UploadBatch::handOffImage(const vk::ImageMemoryBarrier2& barrier) {
    if (!cross_queue_) {
        vk::DependencyInfo dependency{};
        dependency.setImageMemoryBarriers(barrier);
        compat::pipelineBarrier(copyCommands(), dependency);
        return;
    }
    // Release en la cola de copias (con el cambio de layout) y acquire en la de
    // graficos con el mismo layout viejo y nuevo, como pide la especificacion.
    vk::ImageMemoryBarrier2 release = barrier;
    release.dstStageMask = vk::PipelineStageFlagBits2::eNone;
    release.dstAccessMask = vk::AccessFlagBits2::eNone;
    release.srcQueueFamilyIndex = device_->transferFamily();
    release.dstQueueFamilyIndex = device_->graphicsFamily();
    vk::DependencyInfo release_dependency{};
    release_dependency.setImageMemoryBarriers(release);
    compat::pipelineBarrier(copyCommands(), release_dependency);

    vk::ImageMemoryBarrier2 acquire = barrier;
    acquire.srcStageMask = vk::PipelineStageFlagBits2::eNone;
    acquire.srcAccessMask = vk::AccessFlagBits2::eNone;
    acquire.srcQueueFamilyIndex = device_->transferFamily();
    acquire.dstQueueFamilyIndex = device_->graphicsFamily();
    vk::DependencyInfo acquire_dependency{};
    acquire_dependency.setImageMemoryBarriers(acquire);
    compat::pipelineBarrier(graphicsCommands(), acquire_dependency);
}

void UploadBatch::keepAlive(std::shared_ptr<const void> object) {
    if (object) keep_alive_.push_back(std::move(object));
}

void UploadBatch::dependOn(std::shared_ptr<UploadFence> other) {
    if (!other || other == fence_) return;
    if (std::find(dependencies_.begin(), dependencies_.end(), other) != dependencies_.end()) return;
    dependencies_.push_back(std::move(other));
}

void UploadBatch::send(std::shared_ptr<const void> self) {
    if (submitted_) return;
    submitted_ = true;
    if (!recorded_) {
        fence_->finish(nullptr, std::move(dependencies_));
        return;
    }
    vk::CommandBuffer copy{};
    vk::CommandBuffer graphics{};
    if (copy_commands_) {
        copy_commands_->front().end();
        copy = *copy_commands_->front();
    }
    if (graphics_commands_) {
        graphics_commands_->front().end();
        if (graphics_used_) graphics = *graphics_commands_->front();
    }
    std::shared_ptr<GpuTicket> ticket;
    if (cross_queue_) {
        // Copias en la cola de transferencia; la de graficos espera al
        // semaforo y recibe la propiedad (y hace los mips).
        ticket = device_->submitUpload(copy, graphics, std::move(self));
    } else {
        // Todo en un command buffer de la cola de graficos.
        ticket = device_->submitUpload(vk::CommandBuffer{}, copy, std::move(self));
    }
    fence_->finish(std::move(ticket), std::move(dependencies_));
}

std::shared_ptr<UploadFence> UploadBatch::submit() {
    std::shared_ptr<UploadBatch> self = weak_from_this().lock();
    if (!self) {
        submitAndWait();
        return fence_;
    }
    // El lote (command buffers, staging y destinos) vive hasta que la GPU
    // termina: lo suelta VulkanDevice::collectAsync.
    send(std::move(self));
    return fence_;
}

void UploadBatch::submitAndWait() {
    send(nullptr);
    fence_->wait();
}

}  // namespace cramion::gfx
