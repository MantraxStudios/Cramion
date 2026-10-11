#include "CramionFX/vk/VulkanSwapchain.h"

#include "CramionFX/vk/VulkanCompat.h"
#include "CramionFX/vk/VulkanDevice.h"
#include "CramionFX/vk/VulkanSurface.h"

#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>

namespace cramion::gfx {

void VulkanSwapchain::initialize(const VulkanDevice& device, const VulkanSurface& surface,
                                 std::uint32_t width, std::uint32_t height) {
    device_ = &device;
    surface_ = &surface;
    create(width, height);
}

bool VulkanSwapchain::recreate(std::uint32_t width, std::uint32_t height) {
    if (device_ == nullptr || surface_ == nullptr) {
        throw std::runtime_error("VulkanSwapchain::recreate llamado antes de initialize.");
    }

    // Ventana minimizada: se libera la swapchain y se espera a que vuelva a
    // tener area.
    if (width == 0 || height == 0) {
        device_->waitIdle();
        image_views_.clear();
        images_.clear();
        swapchain_ = nullptr;
        extent_ = vk::Extent2D{0, 0};
        return false;
    }

    device_->waitIdle();
    create(width, height);
    return true;
}

void VulkanSwapchain::create(std::uint32_t width, std::uint32_t height) {
    const auto& physical = device_->physicalDevice();
    const vk::SurfaceKHR surface = *surface_->handle();

    const auto capabilities = physical.getSurfaceCapabilitiesKHR(surface);
    const auto formats = physical.getSurfaceFormatsKHR(surface);
    const auto present_modes = physical.getSurfacePresentModesKHR(surface);

    const vk::SurfaceFormatKHR surface_format = chooseSurfaceFormat(formats);
    const vk::PresentModeKHR present_mode = choosePresentMode(present_modes);
    vk::Extent2D extent = chooseExtent(capabilities, width, height);
    vk::SurfaceTransformFlagBitsKHR transform = capabilities.currentTransform;
#if defined(__ANDROID__)
    // Movil girado: el compositor rota la imagen (preTransform = identidad).
    // currentExtent ya viene en la orientacion de la ventana: se usa tal
    // cual (girarlo daba una imagen vertical en la pantalla horizontal).
    // Si el driver no lo fija, el tamano de la ventana.
    if (capabilities.supportedTransforms & vk::SurfaceTransformFlagBitsKHR::eIdentity) {
        transform = vk::SurfaceTransformFlagBitsKHR::eIdentity;
    }
    if (width > 0 && height > 0 && capabilities.currentExtent.width != UINT32_MAX) {
        const vk::Extent2D surface = capabilities.currentExtent;
        // La ventana ya giro y la superficie aun no: manda la ventana.
        const bool rotated = (surface.width > surface.height) != (width > height);
        // La ventana pide menos pixeles que la pantalla (Window::
        // setMaxShortSide): la swapchain se crea a ese tamano y Android la
        // escala a la ventana (SCALE_TO_WINDOW, en el compositor: sin coste
        // para la GPU). Admite cualquier tamano entre minImageExtent (1x1) y
        // maxImageExtent.
        const bool reduced = static_cast<std::uint64_t>(width) * height <
                             static_cast<std::uint64_t>(surface.width) * surface.height;
        if (rotated || reduced) {
            extent = vk::Extent2D{
                std::clamp(width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width),
                std::clamp(height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height)};
        }
    }
#endif
    // Opaco si se puede (Android suele dar solo "heredado").
    vk::CompositeAlphaFlagBitsKHR alpha = vk::CompositeAlphaFlagBitsKHR::eOpaque;
    for (const vk::CompositeAlphaFlagBitsKHR candidate :
         {vk::CompositeAlphaFlagBitsKHR::eOpaque, vk::CompositeAlphaFlagBitsKHR::eInherit,
          vk::CompositeAlphaFlagBitsKHR::ePreMultiplied, vk::CompositeAlphaFlagBitsKHR::ePostMultiplied}) {
        if (capabilities.supportedCompositeAlpha & candidate) {
            alpha = candidate;
            break;
        }
    }

    // Una imagen mas que el minimo para no quedarse esperando al driver.
    std::uint32_t image_count = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount > 0 && image_count > capabilities.maxImageCount) {
        image_count = capabilities.maxImageCount;
    }

    vk::SwapchainCreateInfoKHR create_info{};
    create_info.surface = surface;
    create_info.minImageCount = image_count;
    create_info.imageFormat = surface_format.format;
    create_info.imageColorSpace = surface_format.colorSpace;
    create_info.imageExtent = extent;
    create_info.imageArrayLayers = 1;
    create_info.imageUsage = vk::ImageUsageFlagBits::eColorAttachment;
    create_info.preTransform = transform;
    create_info.compositeAlpha = alpha;
    create_info.presentMode = present_mode;
    create_info.clipped = VK_TRUE;

    // Reutilizar la swapchain antigua acelera el redimensionado.
    create_info.oldSwapchain = *swapchain_;

    // Si graficos y presentacion son familias distintas, las imagenes deben ser
    // compartidas (o habria que hacer transferencias de propiedad explicitas).
    const QueueFamilyIndices& families = device_->queueFamilies();
    const std::array<std::uint32_t, 2> family_indices = {families.graphics, families.present};
    if (families.graphics != families.present) {
        create_info.imageSharingMode = vk::SharingMode::eConcurrent;
        create_info.setQueueFamilyIndices(family_indices);
    } else {
        create_info.imageSharingMode = vk::SharingMode::eExclusive;
    }

    vk::raii::SwapchainKHR new_swapchain(device_->handle(), create_info);

    // Las vistas antiguas apuntan a la swapchain antigua: se liberan antes de
    // reemplazarla.
    image_views_.clear();
    images_.clear();
    swapchain_ = std::move(new_swapchain);

    image_format_ = surface_format.format;
    extent_ = extent;
    images_ = swapchain_.getImages();
    createImageViews();

    std::cout << "[Vulkan] Swapchain creada: " << extent_.width << "x" << extent_.height << ", "
              << images_.size() << " imagenes, formato " << vk::to_string(image_format_) << ", modo "
              << vk::to_string(present_mode) << "\n";
}

void VulkanSwapchain::createImageViews() {
    image_views_.reserve(images_.size());

    for (const vk::Image image : images_) {
        vk::ImageViewCreateInfo create_info{};
        create_info.image = image;
        create_info.viewType = vk::ImageViewType::e2D;
        create_info.format = image_format_;
        create_info.components = vk::ComponentMapping{};
        create_info.subresourceRange =
            vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};

        image_views_.emplace_back(device_->handle(), create_info);
        compat::registerView(static_cast<VkImageView>(*image_views_.back()), create_info.format);
    }
}

vk::SurfaceFormatKHR VulkanSwapchain::chooseSurfaceFormat(
    const std::vector<vk::SurfaceFormatKHR>& available) {
    if (available.empty()) {
        throw std::runtime_error("La superficie no expone ningun formato de color.");
    }

    // Un formato UNORM: la composicion ya aplica la curva sRGB a mano. Uno
    // _SRGB la aplicaria otra vez (imagen lavada). Preferencia: BGRA8 (el
    // habitual en Windows), luego RGBA8 (Android) y luego el de 10 bits.
    const std::array<vk::Format, 4> preferred = {vk::Format::eB8G8R8A8Unorm, vk::Format::eR8G8B8A8Unorm,
                                                 vk::Format::eA2B10G10R10UnormPack32,
                                                 vk::Format::eA2R10G10B10UnormPack32};
    for (const vk::Format wanted : preferred) {
        for (const auto& format : available) {
            if (format.format == wanted && format.colorSpace == vk::ColorSpaceKHR::eSrgbNonlinear) {
                return format;
            }
        }
    }
    // Como ultimo recurso cualquiera que no sea _SRGB.
    for (const auto& format : available) {
        const bool srgb = format.format == vk::Format::eB8G8R8A8Srgb || format.format == vk::Format::eR8G8B8A8Srgb ||
                          format.format == vk::Format::eA8B8G8R8SrgbPack32;
        if (!srgb && format.colorSpace == vk::ColorSpaceKHR::eSrgbNonlinear) return format;
    }
    return available.front();
}

vk::PresentModeKHR VulkanSwapchain::choosePresentMode(
    const std::vector<vk::PresentModeKHR>& available) const {
    // VSync: FIFO (siempre esta garantizado).
    if (vsync_) return vk::PresentModeKHR::eFifo;
    // Mailbox = triple buffer sin tearing.
    const bool has_mailbox = std::find(available.begin(), available.end(),
                                       vk::PresentModeKHR::eMailbox) != available.end();

    return has_mailbox ? vk::PresentModeKHR::eMailbox : vk::PresentModeKHR::eFifo;
}

vk::Extent2D VulkanSwapchain::chooseExtent(const vk::SurfaceCapabilitiesKHR& capabilities,
                                           std::uint32_t width, std::uint32_t height) {
    // Si el driver fija el tamano, se respeta tal cual.
    if (capabilities.currentExtent.width != UINT32_MAX) {
        return capabilities.currentExtent;
    }

    vk::Extent2D extent{width, height};
    extent.width = std::clamp(extent.width, capabilities.minImageExtent.width,
                              capabilities.maxImageExtent.width);
    extent.height = std::clamp(extent.height, capabilities.minImageExtent.height,
                               capabilities.maxImageExtent.height);
    return extent;
}

void VulkanSwapchain::shutdown() {
    image_views_.clear();
    images_.clear();
    swapchain_ = nullptr;

    image_format_ = vk::Format::eUndefined;
    extent_ = vk::Extent2D{0, 0};
    device_ = nullptr;
    surface_ = nullptr;
}

}  // namespace cramion::gfx
