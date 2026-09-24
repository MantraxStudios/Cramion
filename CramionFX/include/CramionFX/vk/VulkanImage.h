#ifndef CRAMION_VK_VULKAN_IMAGE_H
#define CRAMION_VK_VULKAN_IMAGE_H

#include "CramionFX/vk/VulkanCommon.h"

#include <vector>

namespace cramion::gfx {

class VulkanDevice;

// Imagen 2D (o array 2D) con su memoria y sus vistas. Se usa para los destinos
// del G-buffer, el buffer de profundidad y el array de mapas de sombra.
//
// Cuando `layers` > 1 se crea ademas una vista por capa, necesaria para
// renderizar en una sola cascada cada vez.
class VulkanImage {
public:
    VulkanImage() = default;
    ~VulkanImage() = default;

    VulkanImage(const VulkanImage&) = delete;
    VulkanImage& operator=(const VulkanImage&) = delete;
    VulkanImage(VulkanImage&&) = default;
    VulkanImage& operator=(VulkanImage&&) = default;

    // Crea imagen + memoria + vista. `aspect` distingue color de profundidad.
    // Con `layers` > 1 la vista principal es un array 2D y se generan tambien
    // las vistas individuales de cada capa.
    void create(const VulkanDevice& device, vk::Extent2D extent, vk::Format format,
                vk::ImageUsageFlags usage, vk::ImageAspectFlags aspect,
                std::uint32_t layers = 1);

    void destroy();

    const vk::raii::Image& handle() const { return image_; }
    // Vista del conjunto: 2D si hay una capa, array 2D si hay varias.
    const vk::raii::ImageView& view() const { return view_; }

    // Vista de una sola capa, para usarla como destino de render.
    const vk::raii::ImageView& layerView(std::uint32_t layer) const {
        return layer_views_[layer];
    }

    std::uint32_t layers() const { return layers_; }
    vk::Format format() const { return format_; }
    vk::Extent2D extent() const { return extent_; }
    vk::ImageAspectFlags aspect() const { return aspect_; }
    bool isValid() const { return *image_ != VK_NULL_HANDLE; }

private:
    // La vista se destruye antes que la imagen, y la imagen antes que su
    // memoria: de ahi este orden de declaracion.
    vk::raii::DeviceMemory memory_{nullptr};
    vk::raii::Image image_{nullptr};
    vk::raii::ImageView view_{nullptr};
    std::vector<vk::raii::ImageView> layer_views_;

    vk::Format format_ = vk::Format::eUndefined;
    vk::Extent2D extent_{0, 0};
    vk::ImageAspectFlags aspect_ = vk::ImageAspectFlagBits::eColor;
    std::uint32_t layers_ = 1;
};

}  // namespace cramion::gfx

#endif  // CRAMION_VK_VULKAN_IMAGE_H
