#ifndef CRAMION_VK_VULKAN_SWAPCHAIN_H
#define CRAMION_VK_VULKAN_SWAPCHAIN_H

#include "CramionFX/vk/VulkanCommon.h"

#include <vector>

namespace cramion::gfx {

class VulkanDevice;
class VulkanSurface;

// Paso 4 de la inicializacion: cadena de intercambio (swapchain) y sus vistas
// de imagen.
//
// La swapchain depende del tamano de la ventana, asi que se recrea cada vez que
// esta cambia de tamano o el driver la marca como obsoleta.
class VulkanSwapchain {
public:
    VulkanSwapchain() = default;
    ~VulkanSwapchain() = default;

    VulkanSwapchain(const VulkanSwapchain&) = delete;
    VulkanSwapchain& operator=(const VulkanSwapchain&) = delete;

    void initialize(const VulkanDevice& device, const VulkanSurface& surface, std::uint32_t width,
                    std::uint32_t height);

    // Vuelve a crear la swapchain con el tamano indicado reutilizando la
    // anterior. Devuelve false si la ventana esta minimizada (area cero), caso
    // en el que no hay nada que crear.
    bool recreate(std::uint32_t width, std::uint32_t height);
    // Sincronia vertical (FIFO) o mailbox; se aplica al recrearla.
    void setVsync(bool vsync) { vsync_ = vsync; }

    void shutdown();

    const vk::raii::SwapchainKHR& handle() const { return swapchain_; }
    vk::Format imageFormat() const { return image_format_; }
    vk::Extent2D extent() const { return extent_; }
    std::uint32_t imageCount() const { return static_cast<std::uint32_t>(images_.size()); }

    const std::vector<vk::Image>& images() const { return images_; }
    const std::vector<vk::raii::ImageView>& imageViews() const { return image_views_; }

    // true si hay una swapchain utilizable (ventana con area > 0).
    bool isValid() const { return *swapchain_ != VK_NULL_HANDLE; }

private:
    void create(std::uint32_t width, std::uint32_t height);
    void createImageViews();

    static vk::SurfaceFormatKHR chooseSurfaceFormat(
        const std::vector<vk::SurfaceFormatKHR>& available);
    vk::PresentModeKHR choosePresentMode(const std::vector<vk::PresentModeKHR>& available) const;
    bool vsync_ = false;
    static vk::Extent2D chooseExtent(const vk::SurfaceCapabilitiesKHR& capabilities,
                                     std::uint32_t width, std::uint32_t height);

    // Referencias a los objetos de los que depende; propiedad del renderizador.
    const VulkanDevice* device_ = nullptr;
    const VulkanSurface* surface_ = nullptr;

    // Orden de declaracion = orden de creacion; la destruccion es inversa, por
    // lo que las vistas mueren antes que la swapchain.
    vk::raii::SwapchainKHR swapchain_{nullptr};
    std::vector<vk::Image> images_;
    std::vector<vk::raii::ImageView> image_views_;

    vk::Format image_format_ = vk::Format::eUndefined;
    vk::Extent2D extent_{0, 0};
};

}  // namespace cramion::gfx

#endif  // CRAMION_VK_VULKAN_SWAPCHAIN_H
