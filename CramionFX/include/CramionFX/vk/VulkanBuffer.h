#ifndef CRAMION_VK_VULKAN_BUFFER_H
#define CRAMION_VK_VULKAN_BUFFER_H

#include "CramionFX/vk/GpuMemoryStats.h"
#include "CramionFX/vk/VulkanCommon.h"

#include <memory>

struct VmaAllocation_T;

namespace cramion::gfx {

class UploadBatch;
class VulkanDevice;

// Buffer de Vulkan con su memoria asociada.
//
// La memoria la reparte VMA (VulkanDevice::allocator): un bloque grande para
// muchos buffers en lugar de una vkAllocateMemory por buffer (lenta y con el
// limite maxMemoryAllocationCount, 4096 en muchos drivers).
class VulkanBuffer {
public:
    VulkanBuffer() = default;
    ~VulkanBuffer() { destroy(); }

    VulkanBuffer(const VulkanBuffer&) = delete;
    VulkanBuffer& operator=(const VulkanBuffer&) = delete;
    VulkanBuffer(VulkanBuffer&& other) noexcept;
    VulkanBuffer& operator=(VulkanBuffer&& other) noexcept;

    // Crea el buffer y le asigna memoria con las propiedades indicadas. Si la
    // memoria es visible desde la CPU queda mapeada de forma permanente.
    // `name` (opcional): nombre para RenderDoc/Nsight/validacion
    // (VulkanDevice::setObjectName; no hace nada sin la validacion).
    void create(const VulkanDevice& device, vk::DeviceSize size, vk::BufferUsageFlags usage,
                vk::MemoryPropertyFlags properties, const char* name = nullptr);
    // Pone (o cambia) el nombre de depuracion de un buffer ya creado.
    void setName(const char* name) const;

    // Crea un buffer en memoria de la GPU y copia `data` mediante staging.
    // Para datos que no cambian, como las mallas. Espera a la GPU.
    static VulkanBuffer createDeviceLocal(const VulkanDevice& device, const void* data,
                                          vk::DeviceSize size, vk::BufferUsageFlags usage);
    // Igual, pero la copia se graba en `batch` (sin esperar). El lote mantiene
    // vivo el buffer hasta que la GPU termina la copia.
    static std::shared_ptr<VulkanBuffer> createDeviceLocal(const VulkanDevice& device, const void* data,
                                                           vk::DeviceSize size, vk::BufferUsageFlags usage,
                                                           UploadBatch& batch);

    // Copia datos en un buffer visible desde la CPU.
    void write(const void* data, vk::DeviceSize size, vk::DeviceSize offset = 0) const;

    void destroy();

    const vk::raii::Buffer& handle() const { return buffer_; }
    vk::DeviceSize size() const { return size_; }
    void* mapped() const { return mapped_; }
    bool isValid() const { return *buffer_ != VK_NULL_HANDLE; }

private:
    GpuMemoryTicket memory_ticket_;  // cuenta de VRAM (GpuMemoryStats.h)
    const VulkanDevice* device_ = nullptr;  // su asignador (VMA)
    VmaAllocation_T* allocation_ = nullptr;
    vk::raii::Buffer buffer_{nullptr};

    vk::DeviceSize size_ = 0;
    void* mapped_ = nullptr;
};

}  // namespace cramion::gfx

#endif  // CRAMION_VK_VULKAN_BUFFER_H
