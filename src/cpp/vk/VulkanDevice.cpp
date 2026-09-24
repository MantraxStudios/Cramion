#include "vk/VulkanDevice.h"

#include "vk/VulkanInstance.h"
#include "vk/VulkanSurface.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <vector>

namespace cramion::gfx {
namespace {

// Extensiones que el dispositivo logico necesita para presentar en pantalla.
constexpr std::array<const char*, 1> kRequiredDeviceExtensions = {
    vk::KHRSwapchainExtensionName,
};

}  // namespace

void VulkanDevice::initialize(const VulkanInstance& instance, const VulkanSurface& surface) {
    pickPhysicalDevice(instance, surface);
    queue_families_ = findQueueFamilies(physical_device_, surface.handle());
    createLogicalDevice();
    selectDepthFormat();
}

// -----------------------------------------------------------------------------
// Dispositivo fisico
// -----------------------------------------------------------------------------

void VulkanDevice::pickPhysicalDevice(const VulkanInstance& instance, const VulkanSurface& surface) {
    vk::raii::PhysicalDevices candidates(instance.handle());
    if (candidates.empty()) {
        throw std::runtime_error("No se encontro ninguna GPU compatible con Vulkan.");
    }

    // Se ordenan por puntuacion y se toma la mejor valida.
    std::multimap<std::uint32_t, const vk::raii::PhysicalDevice*> ranked;
    for (const auto& candidate : candidates) {
        const std::uint32_t score = rateDevice(candidate, surface.handle());
        if (score > 0) {
            ranked.emplace(score, &candidate);
        }
    }

    if (ranked.empty()) {
        throw std::runtime_error(
            "Ninguna GPU cumple los requisitos (Vulkan 1.3, VK_KHR_swapchain, "
            "dynamic rendering, synchronization2 y presentacion en la ventana).");
    }

    physical_device_ = *ranked.rbegin()->second;

    const auto properties = physical_device_.getProperties();
    device_name_ = properties.deviceName.data();
    api_version_ = properties.apiVersion;

    std::cout << "[Vulkan] GPU seleccionada: " << device_name_ << " (API "
              << VK_API_VERSION_MAJOR(api_version_) << '.' << VK_API_VERSION_MINOR(api_version_)
              << '.' << VK_API_VERSION_PATCH(api_version_) << ")\n";
}

std::uint32_t VulkanDevice::rateDevice(const vk::raii::PhysicalDevice& candidate,
                                       const vk::raii::SurfaceKHR& surface) {
    const auto properties = candidate.getProperties();

    if (properties.apiVersion < kMinimumApiVersion) {
        return 0;
    }
    if (!supportsRequiredExtensions(candidate)) {
        return 0;
    }
    if (!supportsRequiredFeatures(candidate)) {
        return 0;
    }
    if (!findQueueFamilies(candidate, surface).isComplete()) {
        return 0;
    }
    if (candidate.getSurfaceFormatsKHR(*surface).empty() ||
        candidate.getSurfacePresentModesKHR(*surface).empty()) {
        return 0;
    }

    // Preferencia: GPU dedicada sobre integrada; a igualdad, la que admita
    // texturas mas grandes.
    std::uint32_t score = 1;
    switch (properties.deviceType) {
        case vk::PhysicalDeviceType::eDiscreteGpu:
            score += 100000;
            break;
        case vk::PhysicalDeviceType::eIntegratedGpu:
            score += 50000;
            break;
        default:
            break;
    }
    score += properties.limits.maxImageDimension2D;
    return score;
}

bool VulkanDevice::supportsRequiredExtensions(const vk::raii::PhysicalDevice& candidate) {
    const auto available = candidate.enumerateDeviceExtensionProperties();

    return std::all_of(
        kRequiredDeviceExtensions.begin(), kRequiredDeviceExtensions.end(),
        [&](const char* required) {
            return std::any_of(available.begin(), available.end(),
                               [&](const vk::ExtensionProperties& property) {
                                   return std::strcmp(property.extensionName.data(), required) == 0;
                               });
        });
}

bool VulkanDevice::supportsRequiredFeatures(const vk::raii::PhysicalDevice& candidate) {
    const auto chain =
        candidate.getFeatures2<vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceVulkan13Features>();
    const auto& features13 = chain.template get<vk::PhysicalDeviceVulkan13Features>();

    return features13.dynamicRendering && features13.synchronization2 &&
           features13.shaderDemoteToHelperInvocation;
}

QueueFamilyIndices VulkanDevice::findQueueFamilies(const vk::raii::PhysicalDevice& candidate,
                                                   const vk::raii::SurfaceKHR& surface) {
    QueueFamilyIndices indices{};
    const auto families = candidate.getQueueFamilyProperties();

    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(families.size()); ++i) {
        // La cola grafica tambien lanza los compute shaders (auto-exposicion):
        // se exigen ambas capacidades en la misma familia.
        const vk::QueueFlags required = vk::QueueFlagBits::eGraphics | vk::QueueFlagBits::eCompute;
        const bool graphics = (families[i].queueFlags & required) == required;
        const bool present = candidate.getSurfaceSupportKHR(i, *surface) == VK_TRUE;

        // Lo ideal es una unica familia que haga ambas cosas: menos sincronizacion.
        if (graphics && present) {
            indices.graphics = i;
            indices.present = i;
            break;
        }
        if (graphics && indices.graphics == UINT32_MAX) {
            indices.graphics = i;
        }
        if (present && indices.present == UINT32_MAX) {
            indices.present = i;
        }
    }

    return indices;
}

// -----------------------------------------------------------------------------
// Dispositivo logico
// -----------------------------------------------------------------------------

void VulkanDevice::createLogicalDevice() {
    if (!queue_families_.isComplete()) {
        throw std::runtime_error("La GPU seleccionada no expone las familias de colas necesarias.");
    }

    // Una DeviceQueueCreateInfo por familia distinta.
    const std::set<std::uint32_t> unique_families = {queue_families_.graphics,
                                                     queue_families_.present};
    const float priority = 1.0f;

    std::vector<vk::DeviceQueueCreateInfo> queue_infos;
    queue_infos.reserve(unique_families.size());
    for (const std::uint32_t family : unique_families) {
        vk::DeviceQueueCreateInfo info{};
        info.queueFamilyIndex = family;
        info.queueCount = 1;
        info.pQueuePriorities = &priority;
        queue_infos.push_back(info);
    }

    // Caracteristicas de Vulkan 1.3 encadenadas al DeviceCreateInfo.
    vk::StructureChain<vk::DeviceCreateInfo, vk::PhysicalDeviceFeatures2,
                       vk::PhysicalDeviceVulkan13Features>
        chain{};

    auto& create_info = chain.get<vk::DeviceCreateInfo>();
    create_info.setQueueCreateInfos(queue_infos);
    create_info.setPEnabledExtensionNames(kRequiredDeviceExtensions);

    auto& features13 = chain.get<vk::PhysicalDeviceVulkan13Features>();
    features13.dynamicRendering = VK_TRUE;
    features13.synchronization2 = VK_TRUE;
    // Con --target-env=vulkan1.3, glslc traduce `discard` a
    // OpDemoteToHelperInvocation (lo usa el recorte por alfa de los modelos).
    features13.shaderDemoteToHelperInvocation = VK_TRUE;

    // depthClamp es opcional: si la GPU no lo tiene, la pasada de sombras
    // simplemente prescinde de el.
    depth_clamp_supported_ =
        physical_device_.getFeatures().depthClamp == VK_TRUE;

    // Texturas comprimidas por bloques (BC1-BC7, las de los DDS): todas las
    // GPU de escritorio las tienen.
    texture_compression_bc_supported_ =
        physical_device_.getFeatures().textureCompressionBC == VK_TRUE;

    auto& features = chain.get<vk::PhysicalDeviceFeatures2>();
    features.features.depthClamp = depth_clamp_supported_ ? VK_TRUE : VK_FALSE;
    features.features.textureCompressionBC =
        texture_compression_bc_supported_ ? VK_TRUE : VK_FALSE;

    device_ = vk::raii::Device(physical_device_, create_info);

    graphics_queue_ = vk::raii::Queue(device_, queue_families_.graphics, 0);
    present_queue_ = vk::raii::Queue(device_, queue_families_.present, 0);

    // Pool aparte para los comandos de usar y tirar (copias de staging).
    vk::CommandPoolCreateInfo pool_info{};
    pool_info.flags = vk::CommandPoolCreateFlagBits::eTransient;
    pool_info.queueFamilyIndex = queue_families_.graphics;
    transient_pool_ = vk::raii::CommandPool(device_, pool_info);

    std::cout << "[Vulkan] Dispositivo logico creado (familia graficos = "
              << queue_families_.graphics << ", presentacion = " << queue_families_.present
              << ")\n";
}

void VulkanDevice::selectDepthFormat() {
    // De mas a menos preciso; D32 es el habitual en GPU de escritorio.
    constexpr std::array<vk::Format, 3> kCandidates = {
        vk::Format::eD32Sfloat,
        vk::Format::eD32SfloatS8Uint,
        vk::Format::eD24UnormS8Uint,
    };

    for (const vk::Format candidate : kCandidates) {
        const auto properties = physical_device_.getFormatProperties(candidate);

        // Ademas de servir de attachment, la pasada de iluminacion lo muestrea
        // para reconstruir la posicion del mundo.
        const vk::FormatFeatureFlags required =
            vk::FormatFeatureFlagBits::eDepthStencilAttachment |
            vk::FormatFeatureFlagBits::eSampledImage;

        if ((properties.optimalTilingFeatures & required) == required) {
            depth_format_ = candidate;
            std::cout << "[Vulkan] Formato de profundidad: " << vk::to_string(depth_format_)
                      << "\n";
            return;
        }
    }

    throw std::runtime_error("La GPU no admite ningun formato de profundidad conocido.");
}

std::uint32_t VulkanDevice::findMemoryType(std::uint32_t type_bits,
                                           vk::MemoryPropertyFlags properties) const {
    const auto memory_properties = physical_device_.getMemoryProperties();

    for (std::uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
        const bool type_allowed = (type_bits & (1u << i)) != 0;
        const bool has_properties =
            (memory_properties.memoryTypes[i].propertyFlags & properties) == properties;

        if (type_allowed && has_properties) {
            return i;
        }
    }

    throw std::runtime_error("No hay ningun tipo de memoria compatible con lo solicitado.");
}

void VulkanDevice::submitOneTime(
    const std::function<void(const vk::raii::CommandBuffer&)>& record) const {
    vk::CommandBufferAllocateInfo allocate_info{};
    allocate_info.commandPool = *transient_pool_;
    allocate_info.level = vk::CommandBufferLevel::ePrimary;
    allocate_info.commandBufferCount = 1;

    vk::raii::CommandBuffers buffers(device_, allocate_info);
    const vk::raii::CommandBuffer& cmd = buffers.front();

    vk::CommandBufferBeginInfo begin_info{};
    begin_info.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
    cmd.begin(begin_info);

    record(cmd);

    cmd.end();

    const vk::CommandBuffer raw = *cmd;
    vk::SubmitInfo submit_info{};
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &raw;

    // Una fence propia evita bloquear toda la cola con un waitIdle.
    vk::raii::Fence fence(device_, vk::FenceCreateInfo{});
    graphics_queue_.submit(submit_info, *fence);

    if (device_.waitForFences(*fence, VK_TRUE, UINT64_MAX) != vk::Result::eSuccess) {
        throw std::runtime_error("Tiempo agotado esperando un comando de un solo uso.");
    }
}

void VulkanDevice::waitIdle() const {
    if (*device_ != VK_NULL_HANDLE) {
        device_.waitIdle();
    }
}

void VulkanDevice::shutdown() {
    waitIdle();

    transient_pool_ = nullptr;
    present_queue_ = nullptr;
    graphics_queue_ = nullptr;
    device_ = nullptr;
    physical_device_ = nullptr;
    queue_families_ = {};
    device_name_.clear();
    depth_format_ = vk::Format::eUndefined;
    depth_clamp_supported_ = false;
    api_version_ = 0;
}

}  // namespace cramion::gfx
