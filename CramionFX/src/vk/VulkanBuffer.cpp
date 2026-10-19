#include "CramionFX/vk/VulkanBuffer.h"

#include "CramionFX/vk/VulkanDevice.h"
#include "CramionFX/vk/VulkanDeviceUpload.h"

#include <vk_mem_alloc.h>

#include <cstring>
#include <stdexcept>
#include <utility>

namespace cramion::gfx {

VulkanBuffer::VulkanBuffer(VulkanBuffer&& other) noexcept
    : memory_ticket_(std::move(other.memory_ticket_)),
      device_(std::exchange(other.device_, nullptr)),
      allocation_(std::exchange(other.allocation_, nullptr)),
      buffer_(std::move(other.buffer_)),
      size_(std::exchange(other.size_, 0)),
      mapped_(std::exchange(other.mapped_, nullptr)) {}

VulkanBuffer& VulkanBuffer::operator=(VulkanBuffer&& other) noexcept {
    if (this != &other) {
        destroy();
        memory_ticket_ = std::move(other.memory_ticket_);
        device_ = std::exchange(other.device_, nullptr);
        allocation_ = std::exchange(other.allocation_, nullptr);
        buffer_ = std::move(other.buffer_);
        size_ = std::exchange(other.size_, 0);
        mapped_ = std::exchange(other.mapped_, nullptr);
    }
    return *this;
}

void VulkanBuffer::create(const VulkanDevice& device, vk::DeviceSize size,
                          vk::BufferUsageFlags usage, vk::MemoryPropertyFlags properties) {
    if (size == 0) {
        throw std::runtime_error("No se puede crear un buffer de tamano 0.");
    }

    destroy();

    vk::BufferCreateInfo buffer_info{};
    buffer_info.size = size;
    buffer_info.usage = usage;
    buffer_info.sharingMode = vk::SharingMode::eExclusive;

    buffer_ = vk::raii::Buffer(device.handle(), buffer_info);

    // Las mismas propiedades de antes como requisito: VMA elige el tipo de
    // memoria (el primero que las cumple, como findMemoryType) y un hueco en
    // uno de sus bloques. Visible desde la CPU: mapeada para siempre (evita
    // mapear y desmapear en cada actualizacion de los uniform buffers).
    const bool host_visible = static_cast<bool>(properties & vk::MemoryPropertyFlagBits::eHostVisible);
    VmaAllocationCreateInfo allocation_info{};
    allocation_info.usage = VMA_MEMORY_USAGE_UNKNOWN;
    allocation_info.requiredFlags = static_cast<VkMemoryPropertyFlags>(properties);
    if (host_visible) allocation_info.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;

    VmaAllocation allocation = nullptr;
    VmaAllocationInfo info{};
    const VkResult result = vmaAllocateMemoryForBuffer(device.allocator(), static_cast<VkBuffer>(*buffer_),
                                                       &allocation_info, &allocation, &info);
    if (result != VK_SUCCESS) {
        buffer_ = nullptr;
        throw std::runtime_error("No hay memoria de la GPU para un buffer (" + vk::to_string(static_cast<vk::Result>(result)) + ").");
    }
    if (vmaBindBufferMemory(device.allocator(), allocation, static_cast<VkBuffer>(*buffer_)) != VK_SUCCESS) {
        buffer_ = nullptr;
        vmaFreeMemory(device.allocator(), allocation);
        throw std::runtime_error("No se pudo enlazar la memoria de un buffer.");
    }
    device_ = &device;
    allocation_ = allocation;
    memory_ticket_ = GpuMemoryTicket(host_visible ? GpuMemoryKind::Staging : GpuMemoryKind::Buffer, info.size);
    size_ = size;
    mapped_ = host_visible ? info.pMappedData : nullptr;
}

std::shared_ptr<VulkanBuffer> VulkanBuffer::createDeviceLocal(const VulkanDevice& device, const void* data,
                                                              vk::DeviceSize size, vk::BufferUsageFlags usage,
                                                              UploadBatch& batch) {
    auto result = std::make_shared<VulkanBuffer>();
    result->create(device, size, usage | vk::BufferUsageFlagBits::eTransferDst,
                   vk::MemoryPropertyFlagBits::eDeviceLocal);

    const UploadBatch::Staging staging = batch.stage(data, size);
    vk::BufferCopy region{};
    region.srcOffset = staging.offset;
    region.size = size;
    batch.copyCommands().copyBuffer(staging.buffer, *result->handle(), region);
    batch.handOffBuffer(*result->handle());
    batch.keepAlive(result);
    return result;
}

VulkanBuffer VulkanBuffer::createDeviceLocal(const VulkanDevice& device, const void* data,
                                             vk::DeviceSize size, vk::BufferUsageFlags usage) {
    UploadBatch batch(device);
    std::shared_ptr<VulkanBuffer> result = createDeviceLocal(device, data, size, usage, batch);
    batch.submitAndWait();
    // El lote aun guarda una referencia: se mueve el contenido, no el puntero.
    return std::move(*result);
}

void VulkanBuffer::write(const void* data, vk::DeviceSize size, vk::DeviceSize offset) const {
    if (mapped_ == nullptr) {
        throw std::runtime_error("write() sobre un buffer que no es visible desde la CPU.");
    }
    if (offset + size > size_) {
        throw std::runtime_error("write() fuera de los limites del buffer.");
    }

    std::memcpy(static_cast<std::byte*>(mapped_) + offset, data, static_cast<std::size_t>(size));
    // Las propiedades pedidas suelen incluir eHostCoherent (entonces no hace
    // nada); si no, la escritura se hace visible a la GPU aqui.
    vmaFlushAllocation(device_->allocator(), allocation_, offset, size);
}

void VulkanBuffer::destroy() {
    // El buffer antes que la memoria a la que esta enlazado.
    buffer_ = nullptr;
    // (Sin asignador: el dispositivo ya se cerro con esto vivo; se deja.)
    if (allocation_ != nullptr && device_ != nullptr && device_->allocator() != nullptr) {
        vmaFreeMemory(device_->allocator(), allocation_);
    }
    allocation_ = nullptr;
    device_ = nullptr;
    mapped_ = nullptr;
    memory_ticket_.release();
    size_ = 0;
}

}  // namespace cramion::gfx
