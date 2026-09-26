#ifndef CRAMION_VK_VULKAN_DEVICE_H
#define CRAMION_VK_VULKAN_DEVICE_H

#include "CramionFX/vk/VulkanCommon.h"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace cramion::gfx {

class VulkanInstance;
class VulkanSurface;

// Paso 3 de la inicializacion: eleccion del dispositivo fisico y creacion del
// dispositivo logico con sus colas.
//
// Requisitos que debe cumplir una GPU para ser aceptada:
//   - API >= 1.3 (dynamic rendering y synchronization2 en el nucleo).
//   - Extension VK_KHR_swapchain.
//   - Una familia de colas con graficos y otra capaz de presentar en la
//     superficie (pueden ser la misma).
//   - Al menos un formato y un modo de presentacion en la superficie.
class VulkanDevice {
public:
    VulkanDevice() = default;
    ~VulkanDevice() = default;

    VulkanDevice(const VulkanDevice&) = delete;
    VulkanDevice& operator=(const VulkanDevice&) = delete;

    void initialize(const VulkanInstance& instance, const VulkanSurface& surface);
    void shutdown();

    const vk::raii::PhysicalDevice& physicalDevice() const { return physical_device_; }
    const vk::raii::Device& handle() const { return device_; }
    const vk::raii::Queue& graphicsQueue() const { return graphics_queue_; }
    const vk::raii::Queue& presentQueue() const { return present_queue_; }
    const QueueFamilyIndices& queueFamilies() const { return queue_families_; }

    const std::string& name() const { return device_name_; }
    std::uint32_t apiVersion() const { return api_version_; }

    // Espera a que la GPU termine todo el trabajo pendiente.
    void waitIdle() const;

    // Indice del primer tipo de memoria compatible con `type_bits` que tenga
    // las propiedades pedidas. Lanza si la GPU no ofrece ninguno.
    std::uint32_t findMemoryType(std::uint32_t type_bits,
                                 vk::MemoryPropertyFlags properties) const;

    // Graba y ejecuta un command buffer de usar y tirar, y espera a que
    // termine. Para copias de staging y transiciones puntuales de layout.
    void submitOneTime(const std::function<void(const vk::raii::CommandBuffer&)>& record) const;

    // Formato de profundidad soportado por la GPU, elegido al inicializar.
    vk::Format depthFormat() const { return depth_format_; }

    // ¿Se activo depthClamp? La pasada de sombras lo usa para no perder los
    // objetos que quedan por delante del plano cercano de una cascada.
    bool depthClampSupported() const { return depth_clamp_supported_; }
    bool textureCompressionBcSupported() const { return texture_compression_bc_supported_; }

    // Trazado de rayos por hardware (ray queries + estructuras de
    // aceleracion, con direcciones de buffer y texturas indexadas en los
    // shaders). Opcional: sin el, la GI y los reflejos son de pantalla.
    bool rayTracingSupported() const { return ray_tracing_supported_; }

    // Memoria de video real (VK_EXT_memory_budget): lo que usa este proceso
    // y lo que el sistema le deja usar ahora mismo (baja si otras apps, como
    // el editor abierto a la vez, ocupan la GPU). false si el driver no lo
    // informa.
    bool videoMemory(std::uint64_t& used_bytes, std::uint64_t& budget_bytes) const;

    // Cache de pipelines en disco: el primer arranque compila los shaders para
    // esta GPU y los siguientes los leen ya compilados. Todos los pipelines se
    // crean con pipelineCache(), que ademas los cuenta (progreso de la carga).
    void loadPipelineCache(const std::filesystem::path& file);
    void savePipelineCache() const;
    const vk::raii::PipelineCache& pipelineCache() const;
    std::uint32_t pipelinesCreated() const { return pipelines_created_; }
    void setPipelineCallback(std::function<void(std::uint32_t)> callback) { pipeline_callback_ = std::move(callback); }

private:
    void pickPhysicalDevice(const VulkanInstance& instance, const VulkanSurface& surface);
    void createLogicalDevice();

    // Devuelve 0 si la GPU no sirve; cuanto mayor la puntuacion, mejor.
    static std::uint32_t rateDevice(const vk::raii::PhysicalDevice& candidate,
                                    const vk::raii::SurfaceKHR& surface);
    static QueueFamilyIndices findQueueFamilies(const vk::raii::PhysicalDevice& candidate,
                                                const vk::raii::SurfaceKHR& surface);
    static bool supportsRequiredExtensions(const vk::raii::PhysicalDevice& candidate);
    static bool supportsRequiredFeatures(const vk::raii::PhysicalDevice& candidate);
    static bool supportsRayTracing(const vk::raii::PhysicalDevice& candidate);

    // Primer formato de profundidad de la lista de preferencias que la GPU
    // admita como attachment.
    void selectDepthFormat();

    vk::raii::PhysicalDevice physical_device_{nullptr};
    vk::raii::Device device_{nullptr};
    vk::raii::Queue graphics_queue_{nullptr};
    vk::raii::Queue present_queue_{nullptr};

    // Pool aparte para los comandos de usar y tirar (copias de staging).
    vk::raii::CommandPool transient_pool_{nullptr};

    QueueFamilyIndices queue_families_{};
    vk::Format depth_format_ = vk::Format::eUndefined;
    bool depth_clamp_supported_ = false;
    bool texture_compression_bc_supported_ = false;
    bool ray_tracing_supported_ = false;
    bool memory_budget_supported_ = false;
    std::string device_name_;
    std::uint32_t api_version_ = 0;

    vk::raii::PipelineCache pipeline_cache_{nullptr};
    std::filesystem::path pipeline_cache_file_;
    mutable std::uint32_t pipelines_created_ = 0;
    std::function<void(std::uint32_t)> pipeline_callback_;
};

}  // namespace cramion::gfx

#endif  // CRAMION_VK_VULKAN_DEVICE_H
