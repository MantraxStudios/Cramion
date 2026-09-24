#ifndef CRAMION_VK_VULKAN_BUFFER_H
#define CRAMION_VK_VULKAN_BUFFER_H

#include "vk/VulkanCommon.h"

namespace cramion::gfx {

class VulkanDevice;

// Buffer de Vulkan con su memoria asociada.
//
// Es un asignador deliberadamente simple: una asignacion de memoria por buffer.
// Sirve de sobra para esta escena (un buffer de vertices y otro de indices
// por modelo y unos pocos uniform buffers); para muchos objetos pequenos el
// siguiente paso seria un sub-asignador o VMA.
class VulkanBuffer {
public:
    VulkanBuffer() = default;
    ~VulkanBuffer() = default;

    VulkanBuffer(const VulkanBuffer&) = delete;
    VulkanBuffer& operator=(const VulkanBuffer&) = delete;
    VulkanBuffer(VulkanBuffer&&) = default;
    VulkanBuffer& operator=(VulkanBuffer&&) = default;

    // Crea el buffer y le asigna memoria con las propiedades indicadas. Si la
    // memoria es visible desde la CPU queda mapeada de forma permanente.
    void create(const VulkanDevice& device, vk::DeviceSize size, vk::BufferUsageFlags usage,
                vk::MemoryPropertyFlags properties);

    // Crea un buffer en memoria de la GPU y copia `data` mediante un buffer
    // intermedio (staging). Para datos que no cambian, como las mallas.
    static VulkanBuffer createDeviceLocal(const VulkanDevice& device, const void* data,
                                          vk::DeviceSize size, vk::BufferUsageFlags usage);

    // Copia datos en un buffer visible desde la CPU.
    void write(const void* data, vk::DeviceSize size, vk::DeviceSize offset = 0) const;

    void destroy();

    const vk::raii::Buffer& handle() const { return buffer_; }
    vk::DeviceSize size() const { return size_; }
    void* mapped() const { return mapped_; }
    bool isValid() const { return *buffer_ != VK_NULL_HANDLE; }

private:
    // Orden de declaracion inverso al de destruccion: el buffer debe destruirse
    // antes de liberar la memoria a la que esta enlazado.
    vk::raii::DeviceMemory memory_{nullptr};
    vk::raii::Buffer buffer_{nullptr};

    vk::DeviceSize size_ = 0;
    void* mapped_ = nullptr;
};

}  // namespace cramion::gfx

#endif  // CRAMION_VK_VULKAN_BUFFER_H
