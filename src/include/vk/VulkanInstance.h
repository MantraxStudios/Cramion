#ifndef CRAMION_VK_VULKAN_INSTANCE_H
#define CRAMION_VK_VULKAN_INSTANCE_H

#include "vk/VulkanCommon.h"

#include <string>
#include <vector>

namespace cramion::gfx {

// Paso 1 de la inicializacion: instancia de Vulkan + mensajero de depuracion.
//
// Encapsula el cargador (vk::raii::Context), la seleccion de capas y
// extensiones de instancia, y la creacion del VK_EXT_debug_utils cuando la
// validacion esta disponible.
class VulkanInstance {
public:
    VulkanInstance() = default;
    ~VulkanInstance() = default;

    VulkanInstance(const VulkanInstance&) = delete;
    VulkanInstance& operator=(const VulkanInstance&) = delete;

    // Crea la instancia. Lanza std::runtime_error si falta algo obligatorio.
    void initialize(const EngineInfo& info);

    // Destruye el mensajero y la instancia (en ese orden).
    void shutdown();

    const vk::raii::Instance& handle() const { return instance_; }
    std::uint32_t apiVersion() const { return api_version_; }
    bool validationEnabled() const { return validation_enabled_; }

private:
    // Extensiones de instancia obligatorias (superficie + plataforma) mas las
    // opcionales de depuracion si estan disponibles.
    std::vector<const char*> selectExtensions();
    std::vector<const char*> selectLayers();
    void createDebugMessenger();

    // Callback de las capas de validacion.
    static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
        vk::DebugUtilsMessageSeverityFlagBitsEXT severity,
        vk::DebugUtilsMessageTypeFlagsEXT types,
        const vk::DebugUtilsMessengerCallbackDataEXT* data,
        void* user_data);

    // El orden de declaracion importa: los miembros se destruyen en orden
    // inverso, asi el mensajero muere antes que la instancia.
    vk::raii::Context context_{};
    vk::raii::Instance instance_{nullptr};
    vk::raii::DebugUtilsMessengerEXT debug_messenger_{nullptr};

    std::uint32_t api_version_ = VK_API_VERSION_1_0;
    bool validation_enabled_ = false;
};

}  // namespace cramion::gfx

#endif  // CRAMION_VK_VULKAN_INSTANCE_H
