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

    // `window` es la ventana (HWND o ANativeWindow); debe seguir viva
    // mientras exista la superficie.
    void initialize(const VulkanInstance& instance, NativeWindow window);
    void shutdown();

    const vk::raii::SurfaceKHR& handle() const { return surface_; }
    NativeWindow window() const { return window_; }

private:
    vk::raii::SurfaceKHR surface_{nullptr};
    NativeWindow window_ = nullptr;
};

}  // namespace cramion::gfx

#endif  // CRAMION_VK_VULKAN_SURFACE_H
