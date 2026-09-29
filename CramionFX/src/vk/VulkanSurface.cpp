#include "CramionFX/vk/VulkanSurface.h"

#include "CramionFX/vk/VulkanInstance.h"

#include <iostream>
#include <stdexcept>

namespace cramion::gfx {

void VulkanSurface::initialize(const VulkanInstance& instance, NativeWindow window) {
    if (window == nullptr) {
        throw std::runtime_error("No se puede crear la superficie: ventana nula.");
    }

#if defined(__ANDROID__)
    vk::AndroidSurfaceCreateInfoKHR create_info{};
    create_info.window = window;
    surface_ = vk::raii::SurfaceKHR(instance.handle(), create_info);
    std::cout << "[Vulkan] Superficie Android creada\n";
#else
    vk::Win32SurfaceCreateInfoKHR create_info{};
    create_info.hinstance = GetModuleHandleW(nullptr);
    create_info.hwnd = window;
    surface_ = vk::raii::SurfaceKHR(instance.handle(), create_info);
    std::cout << "[Vulkan] Superficie Win32 creada\n";
#endif
    window_ = window;
}

void VulkanSurface::shutdown() {
    surface_ = nullptr;
    window_ = nullptr;
}

}  // namespace cramion::gfx
