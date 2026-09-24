#include "CramionFX/vk/VulkanInstance.h"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

namespace cramion::gfx {
namespace {

constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";

// Comprueba si un nombre esta presente en una lista de propiedades.
template <typename Properties, typename NameGetter>
bool contains(const std::vector<Properties>& list, const char* name, NameGetter get_name) {
    return std::any_of(list.begin(), list.end(), [&](const Properties& item) {
        return std::strcmp(get_name(item), name) == 0;
    });
}

}  // namespace

void VulkanInstance::initialize(const EngineInfo& info) {
    validation_enabled_ = info.enable_validation;

    // 1) Version de la API: se usa la mas alta que soporte el loader, con tope
    //    en Vulkan 1.4.
    const std::uint32_t loader_version = context_.enumerateInstanceVersion();
    if (loader_version < kMinimumApiVersion) {
        throw std::runtime_error(
            "El loader de Vulkan instalado es anterior a 1.3; actualiza los drivers.");
    }
    api_version_ = std::min(loader_version, static_cast<std::uint32_t>(VK_API_VERSION_1_4));

    // 2) Capas y extensiones.
    const std::vector<const char*> layers = selectLayers();
    const std::vector<const char*> extensions = selectExtensions();

    // 3) Descripcion de la aplicacion.
    vk::ApplicationInfo app_info{};
    app_info.pApplicationName = info.app_name;
    app_info.applicationVersion = info.app_version;
    app_info.pEngineName = info.engine_name;
    app_info.engineVersion = info.engine_version;
    app_info.apiVersion = api_version_;

    vk::InstanceCreateInfo create_info{};
    create_info.pApplicationInfo = &app_info;
    create_info.setPEnabledLayerNames(layers);
    create_info.setPEnabledExtensionNames(extensions);

    instance_ = vk::raii::Instance(context_, create_info);

    // 4) Mensajero de depuracion (solo si la validacion sigue activa).
    if (validation_enabled_) {
        createDebugMessenger();
    }

    std::cout << "[Vulkan] Instancia creada (API "
              << VK_API_VERSION_MAJOR(api_version_) << '.'
              << VK_API_VERSION_MINOR(api_version_) << '.'
              << VK_API_VERSION_PATCH(api_version_) << ", validacion "
              << (validation_enabled_ ? "ON" : "OFF") << ")\n";
}

std::vector<const char*> VulkanInstance::selectLayers() {
    std::vector<const char*> layers;
    if (!validation_enabled_) {
        return layers;
    }

    const auto available = context_.enumerateInstanceLayerProperties();
    const bool has_validation = contains(available, kValidationLayer,
                                         [](const vk::LayerProperties& p) { return p.layerName.data(); });

    if (has_validation) {
        layers.push_back(kValidationLayer);
    } else {
        // No es un error fatal: simplemente se continua sin validacion.
        std::cout << "[Vulkan] Capa de validacion no disponible; se desactiva.\n";
        validation_enabled_ = false;
    }
    return layers;
}

std::vector<const char*> VulkanInstance::selectExtensions() {
    const auto available = context_.enumerateInstanceExtensionProperties();
    const auto has = [&](const char* name) {
        return contains(available, name,
                        [](const vk::ExtensionProperties& p) { return p.extensionName.data(); });
    };

    // Obligatorias para poder presentar en una ventana Win32.
    const char* required[] = {vk::KHRSurfaceExtensionName, vk::KHRWin32SurfaceExtensionName};

    std::vector<const char*> extensions;
    for (const char* name : required) {
        if (!has(name)) {
            throw std::runtime_error(std::string("Extension de instancia obligatoria ausente: ") + name);
        }
        extensions.push_back(name);
    }

    // Opcional: mensajes de depuracion legibles.
    if (validation_enabled_) {
        if (has(vk::EXTDebugUtilsExtensionName)) {
            extensions.push_back(vk::EXTDebugUtilsExtensionName);
        } else {
            std::cout << "[Vulkan] VK_EXT_debug_utils no disponible; se desactiva la validacion.\n";
            validation_enabled_ = false;
        }
    }

    return extensions;
}

void VulkanInstance::createDebugMessenger() {
    using Severity = vk::DebugUtilsMessageSeverityFlagBitsEXT;
    using Type = vk::DebugUtilsMessageTypeFlagBitsEXT;

    vk::DebugUtilsMessengerCreateInfoEXT create_info{};
    create_info.messageSeverity = Severity::eWarning | Severity::eError;
    create_info.messageType = Type::eGeneral | Type::eValidation | Type::ePerformance;
    create_info.pfnUserCallback = &VulkanInstance::debugCallback;

    debug_messenger_ = vk::raii::DebugUtilsMessengerEXT(instance_, create_info);
}

VKAPI_ATTR VkBool32 VKAPI_CALL VulkanInstance::debugCallback(
    vk::DebugUtilsMessageSeverityFlagBitsEXT severity,
    vk::DebugUtilsMessageTypeFlagsEXT types,
    const vk::DebugUtilsMessengerCallbackDataEXT* data,
    void* user_data) {
    (void)types;
    (void)user_data;

    const char* level =
        (severity == vk::DebugUtilsMessageSeverityFlagBitsEXT::eError) ? "ERROR" : "AVISO";

    std::cerr << "[Vulkan/" << level << "] " << (data->pMessage ? data->pMessage : "(sin mensaje)")
              << '\n';

    // VK_FALSE: no abortar la llamada que genero el mensaje.
    return VK_FALSE;
}

void VulkanInstance::shutdown() {
    debug_messenger_ = nullptr;
    instance_ = nullptr;
    validation_enabled_ = false;
}

}  // namespace cramion::gfx
