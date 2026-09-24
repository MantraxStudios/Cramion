#include "CramionFX/vk/VulkanBuffer.h"

#include "CramionFX/vk/VulkanDevice.h"

#include <cstring>
#include <stdexcept>

namespace cramion::gfx {

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

    const vk::MemoryRequirements requirements = buffer_.getMemoryRequirements();

    vk::MemoryAllocateInfo allocate_info{};
    allocate_info.allocationSize = requirements.size;
    allocate_info.memoryTypeIndex = device.findMemoryType(requirements.memoryTypeBits, properties);

    memory_ = vk::raii::DeviceMemory(device.handle(), allocate_info);
    buffer_.bindMemory(*memory_, 0);

    size_ = size;

    // La memoria accesible desde la CPU se deja mapeada: evita mapear y
    // desmapear en cada actualizacion de los uniform buffers.
    if (properties & vk::MemoryPropertyFlagBits::eHostVisible) {
        mapped_ = memory_.mapMemory(0, VK_WHOLE_SIZE);
    }
}

VulkanBuffer VulkanBuffer::createDeviceLocal(const VulkanDevice& device, const void* data,
                                             vk::DeviceSize size, vk::BufferUsageFlags usage) {
    VulkanBuffer staging;
    staging.create(device, size, vk::BufferUsageFlagBits::eTransferSrc,
                   vk::MemoryPropertyFlagBits::eHostVisible |
                       vk::MemoryPropertyFlagBits::eHostCoherent);
    staging.write(data, size);

    VulkanBuffer result;
    result.create(device, size, usage | vk::BufferUsageFlagBits::eTransferDst,
                  vk::MemoryPropertyFlagBits::eDeviceLocal);

    device.submitOneTime([&](const vk::raii::CommandBuffer& cmd) {
        vk::BufferCopy region{};
        region.size = size;
        cmd.copyBuffer(*staging.handle(), *result.handle(), region);
    });

    staging.destroy();
    return result;
}

void VulkanBuffer::write(const void* data, vk::DeviceSize size, vk::DeviceSize offset) const {
    if (mapped_ == nullptr) {
        throw std::runtime_error("write() sobre un buffer que no es visible desde la CPU.");
    }
    if (offset + size > size_) {
        throw std::runtime_error("write() fuera de los limites del buffer.");
    }

    std::memcpy(static_cast<std::byte*>(mapped_) + offset, data, static_cast<std::size_t>(size));
}

void VulkanBuffer::destroy() {
    if (mapped_ != nullptr) {
        memory_.unmapMemory();
        mapped_ = nullptr;
    }

    buffer_ = nullptr;
    memory_ = nullptr;
    size_ = 0;
}

}  // namespace cramion::gfx
