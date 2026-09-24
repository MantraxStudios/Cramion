#include "vk/VulkanSurface.h"

#include "vk/VulkanInstance.h"

#include <iostream>
#include <stdexcept>

namespace cramion::gfx {

void VulkanSurface::initialize(const VulkanInstance& instance, HWND window) {
    if (window == nullptr) {
        throw std::runtime_error("No se puede crear la superficie: HWND nulo.");
    }

    vk::Win32SurfaceCreateInfoKHR create_info{};
    create_info.hinstance = GetModuleHandleW(nullptr);
    create_info.hwnd = window;

    surface_ = vk::raii::SurfaceKHR(instance.handle(), create_info);
    window_ = window;

    std::cout << "[Vulkan] Superficie Win32 creada\n";
}

void VulkanSurface::shutdown() {
    surface_ = nullptr;
    window_ = nullptr;
}

}  // namespace cramion::gfx
