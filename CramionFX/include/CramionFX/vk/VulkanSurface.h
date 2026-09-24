#ifndef CRAMION_VK_VULKAN_SURFACE_H
#define CRAMION_VK_VULKAN_SURFACE_H

#include "CramionFX/vk/VulkanCommon.h"

namespace cramion::gfx {

class VulkanInstance;

// Paso 2 de la inicializacion: superficie de presentacion asociada al HWND de
// la ventana Win32 creada por CramionDM.
class VulkanSurface {
public:
    VulkanSurface() = default;
    ~VulkanSurface() = default;

    VulkanSurface(const VulkanSurface&) = delete;
    VulkanSurface& operator=(const VulkanSurface&) = delete;

    // `window` es el HWND de la ventana; debe seguir vivo mientras exista la
    // superficie.
    void initialize(const VulkanInstance& instance, HWND window);
    void shutdown();

    const vk::raii::SurfaceKHR& handle() const { return surface_; }
    HWND window() const { return window_; }

private:
    vk::raii::SurfaceKHR surface_{nullptr};
    HWND window_ = nullptr;
};

}  // namespace cramion::gfx

#endif  // CRAMION_VK_VULKAN_SURFACE_H
