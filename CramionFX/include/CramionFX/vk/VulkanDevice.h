#ifndef CRAMION_VK_VULKAN_DEVICE_H
#define CRAMION_VK_VULKAN_DEVICE_H

#include "CramionFX/vk/VulkanCommon.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// Vulkan Memory Allocator (vendor/vma): tipo opaco, la cabecera solo la ven
// los .cpp de CramionFX.
struct VmaAllocator_T;

namespace cramion::gfx {

class VulkanInstance;
class VulkanSurface;
class StagingRing;

// Trabajo enviado a la GPU sin esperarlo (VulkanDevice::submitAsync). Se
// consulta su fence sin bloquear: el hilo principal adopta el resultado
// cuando done() da true. El command buffer y lo que haya que mantener vivo
// (staging) los suelta VulkanDevice::collectAsync cuando la GPU termina.
class GpuTicket {
public:
    // true cuando la GPU termino. No bloquea. Desde cualquier hilo.
    bool done() const;
    // Bloquea hasta que termine. Solo para los caminos sincronos de reserva
    // (CVars de streaming apagadas), nunca en el camino normal del frame.
    void wait() const;

private:
    friend class VulkanDevice;
    const vk::raii::Device* device_ = nullptr;
    vk::raii::Fence fence_{nullptr};
    mutable std::atomic<bool> signaled_{false};
};

// Quien ejecuta las tareas en segundo plano de CramionFX (decodificar las
// texturas de las capas, la malla del terreno para los rayos, la vegetacion).
// CramionCore le pasa su sistema de jobs; sin el, un hilo propio.
using TaskScheduler = std::function<void(std::function<void()>)>;

// Paso 3 de la inicializacion: eleccion del dispositivo fisico y creacion del
// dispositivo logico con sus colas.
//
// Requisitos que debe cumplir una GPU para ser aceptada:
//   - Vulkan (cualquier version) con VK_KHR_swapchain.
//   - Una familia de colas con graficos y otra capaz de presentar en la
//     superficie (pueden ser la misma).
//   - Al menos un formato y un modo de presentacion en la superficie.
// Con Vulkan 1.3, dynamic rendering, synchronization2 y limites de escritorio
// va por el camino completo; si no (moviles, GPU antiguas), por el modo
// compatible (VulkanCompat.h): mismas imagenes con menos recursos por shader.
class VulkanDevice {
public:
    VulkanDevice() = default;
    ~VulkanDevice();

    VulkanDevice(const VulkanDevice&) = delete;
    VulkanDevice& operator=(const VulkanDevice&) = delete;

    // `required`: la GPU que pide OpenXR (la del casco); `extra_extensions`:
    // las extensiones de dispositivo que pide. `optional_extensions`: se
    // activan si la GPU las tiene, sin avisar (compartir memoria con un
    // runtime de VR: el casco se puede conectar despues).
    void initialize(const VulkanInstance& instance, const VulkanSurface& surface, VkPhysicalDevice required = VK_NULL_HANDLE,
                    const std::vector<std::string>& extra_extensions = {},
                    const std::vector<std::string>& optional_extensions = {});
    void shutdown();

    const std::vector<std::string>& enabledExtensions() const { return enabled_extensions_; }
    bool extensionAvailable(const std::string& name) const;  // la GPU la tiene (activada o no)

    const vk::raii::PhysicalDevice& physicalDevice() const { return physical_device_; }
    const vk::raii::Device& handle() const { return device_; }
    const vk::raii::Queue& graphicsQueue() const { return graphics_queue_; }
    const vk::raii::Queue& presentQueue() const { return present_queue_; }
    const QueueFamilyIndices& queueFamilies() const { return queue_families_; }
    // La cola se usa desde varios hilos (las subidas del streaming): todo lo
    // que la toque (submit, present, waitIdle, OpenXR, ImGui) con esto puesto.
    std::mutex& queueMutex() const { return queue_mutex_; }

    const std::string& name() const { return device_name_; }
    std::uint32_t apiVersion() const { return api_version_; }

    // Espera a que la GPU termine todo el trabajo pendiente.
    void waitIdle() const;

    // --- Memoria (VMA) ---
    // Asignador comun de buffers y texturas: reparte bloques grandes entre los
    // recursos en lugar de una vkAllocateMemory por recurso (lenta y con el
    // limite maxMemoryAllocationCount, 4096 en muchos drivers).
    VmaAllocator_T* allocator() const { return allocator_; }
    // Anillo de staging persistente (mapeado) de las subidas (UploadBatch).
    // Nulo antes de initialize y despues de shutdown.
    StagingRing* stagingRing() const { return staging_ring_.get(); }

    // --- Cola de transferencia ---
    // Familia de colas solo de copias (sin graficos), si la GPU la tiene y no
    // se esta en modo compatible. Las subidas de UploadBatch copian alli y
    // pasan la propiedad de los recursos a la cola de graficos.
    // CRAMION_VK_NO_TRANSFER_QUEUE=1 la desactiva (pruebas).
    bool hasTransferQueue() const { return transfer_family_ != UINT32_MAX; }
    std::uint32_t transferFamily() const { return transfer_family_; }
    std::uint32_t graphicsFamily() const { return queue_families_.graphics; }

    // Envia un lote de subida sin esperar (UploadBatch::submit). `transfer`:
    // las copias en la cola de transferencia (puede ser nulo); `graphics`: lo
    // de la cola de graficos (recibir la propiedad, mips...), que espera a las
    // copias con un semaforo. Si solo hay `graphics`, va todo ahi. El ticket
    // es la fence del ultimo envio; `keep_alive` vive hasta que la GPU termina
    // (lo suelta collectAsync).
    std::shared_ptr<GpuTicket> submitUpload(vk::CommandBuffer transfer, vk::CommandBuffer graphics,
                                            std::shared_ptr<const void> keep_alive) const;

    // Mensajes de detalle (cada modelo subido, cada textura...): solo con
    // CRAMION_LOG_LEVEL=debug (o CRAMION_VK_VERBOSE=1). std::cout sincroniza
    // con stdio y desde los hilos de subida eran cientos de lineas por minuto.
    static bool debugLog();

    // Indice del primer tipo de memoria compatible con `type_bits` que tenga
    // las propiedades pedidas. Lanza si la GPU no ofrece ninguno.
    std::uint32_t findMemoryType(std::uint32_t type_bits,
                                 vk::MemoryPropertyFlags properties) const;

    // Graba y ejecuta un command buffer de usar y tirar, y espera a que
    // termine. Para copias de staging y transiciones puntuales de layout.
    // Se puede llamar desde cualquier hilo.
    void submitOneTime(const std::function<void(const vk::raii::CommandBuffer&)>& record) const;

    // Como submitOneTime pero SIN esperar: graba, envia (cola de graficos,
    // con queueMutex) y devuelve un ticket con su fence. `keep_alive` (el
    // staging, las imagenes de destino...) vive hasta que la GPU termina.
    // Desde cualquier hilo. Nunca bloquea en la GPU.
    std::shared_ptr<GpuTicket> submitAsync(const std::function<void(const vk::raii::CommandBuffer&)>& record,
                                           std::vector<std::shared_ptr<const void>> keep_alive = {}) const;
    // Hilo principal, una vez por frame: suelta los command buffers y lo
    // mantenido vivo de los envios que ya terminaron. Devuelve los que siguen
    // en vuelo.
    std::size_t collectAsync() const;
    std::size_t asyncInFlight() const;

    // Tareas en segundo plano (ver TaskScheduler). setTaskScheduler al
    // arrancar (nullptr = el hilo propio). runTask desde cualquier hilo;
    // drainTasks espera a que terminen todas (al destruir pasadas).
    void setTaskScheduler(TaskScheduler scheduler);
    void runTask(std::function<void()> task) const;
    void drainTasks() const;
    std::uint32_t tasksInFlight() const { return static_cast<std::uint32_t>(tasks_in_flight_.load()); }
    // Tareas largas de streaming (subir un modelo entero: decodificar sus
    // texturas y grabar sus copias). Van a un grupo pequeno de hilos propio
    // (CRAMION_STREAM_THREADS, 3 por defecto), no al planificador de las
    // tareas cortas: un modelo de varios segundos no deja sin hilos al
    // terreno ni a la vegetacion. Antes, un std::async (un hilo del sistema
    // creado y destruido) por modelo. drainTasks tambien las espera.
    void runStreamingTask(std::function<void()> task) const;

    // Formato de profundidad soportado por la GPU, elegido al inicializar.
    vk::Format depthFormat() const { return depth_format_; }

    // ¿Se activo depthClamp? La pasada de sombras lo usa para no perder los
    // objetos que quedan por delante del plano cercano de una cascada.
    bool depthClampSupported() const { return depth_clamp_supported_; }
    // vkCmdDrawIndexedIndirectCount y varios comandos por llamada (no en
    // muchas GPU de movil).
    bool indirectCountSupported() const { return indirect_count_supported_; }
    // firstInstance en los comandos de dibujo indirecto (vegetacion, hierba):
    // obligatorio en el camino completo; algunos moviles no lo tienen.
    bool drawIndirectFirstInstanceSupported() const { return draw_indirect_first_instance_; }
    bool textureCompressionBcSupported() const { return texture_compression_bc_supported_; }
    // Poligonos como lineas (vista Wireframe).
    bool fillModeNonSolidSupported() const { return fill_mode_non_solid_supported_; }
    // Shaders de teselacion (materiales con relieve teselado).
    bool tessellationSupported() const { return tessellation_supported_; }
    // Modo compatible (VulkanCompat.h): moviles y GPU sin lo del camino de
    // escritorio. Shaders de shaders/compat, recursos por shader dentro de
    // los minimos de Vulkan, G-buffer de 4 destinos, culling en la CPU.
    bool compatMode() const { return compat_mode_; }
    const std::string& compatReason() const { return compat_reason_; }
    const vk::PhysicalDeviceLimits& limits() const { return limits_; }
    // Texturas comprimidas de movil (para elegir el formato de las texturas).
    bool textureCompressionAstcSupported() const { return texture_compression_astc_supported_; }
    bool textureCompressionEtc2Supported() const { return texture_compression_etc2_supported_; }

    // Trazado de rayos por hardware (ray queries + estructuras de
    // aceleracion, con direcciones de buffer y texturas indexadas en los
    // shaders). Opcional: sin el, la GI y los reflejos son de pantalla.
    bool rayTracingSupported() const { return ray_tracing_supported_; }
    // Opcionales de las GPUs actuales (RTX 40/50, RDNA 3+...).
    bool opacityMicromapSupported() const { return opacity_micromap_supported_; }
    bool rayTracingPipelineSupported() const { return ray_tracing_pipeline_supported_; }
    bool invocationReorderSupported() const { return invocation_reorder_supported_; }
    bool meshShaderSupported() const { return mesh_shader_supported_; }

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
    void pickPhysicalDevice(const VulkanInstance& instance, const VulkanSurface& surface, VkPhysicalDevice required);
    void createLogicalDevice();

    // Devuelve 0 si la GPU no sirve; cuanto mayor la puntuacion, mejor.
    static std::uint32_t rateDevice(const vk::raii::PhysicalDevice& candidate,
                                    const vk::raii::SurfaceKHR& surface);
    static QueueFamilyIndices findQueueFamilies(const vk::raii::PhysicalDevice& candidate,
                                                const vk::raii::SurfaceKHR& surface);
    static bool supportsRequiredExtensions(const vk::raii::PhysicalDevice& candidate);
    static bool supportsRayTracing(const vk::raii::PhysicalDevice& candidate);
    // Modo compatible: dispositivo con las extensiones y caracteristicas que
    // tenga (Vulkan 1.0-1.2, moviles), sin trazado de rayos ni mesh shaders.
    void createCompatLogicalDevice(const char* reason);
    // Familia de colas solo de transferencia (UINT32_MAX si no hay o no sirve).
    static std::uint32_t findTransferFamily(const vk::raii::PhysicalDevice& candidate, std::uint32_t graphics,
                                            std::uint32_t present);
    // VMA y el anillo de staging, con el dispositivo ya creado.
    void createAllocator(const VulkanInstance& instance);
    void destroyAllocator();

    // Primer formato de profundidad de la lista de preferencias que la GPU
    // admita como attachment.
    void selectDepthFormat();

    vk::raii::PhysicalDevice physical_device_{nullptr};
    vk::raii::Device device_{nullptr};
    vk::raii::Queue graphics_queue_{nullptr};
    vk::raii::Queue present_queue_{nullptr};
    vk::raii::Queue transfer_queue_{nullptr};  // cola de transferencia dedicada (si hay)
    std::uint32_t transfer_family_ = UINT32_MAX;
    mutable std::mutex transfer_queue_mutex_;

    VmaAllocator_T* allocator_ = nullptr;
    std::shared_ptr<StagingRing> staging_ring_;  // (shared_ptr: tipo incompleto aqui)

    // Pool aparte para los comandos de usar y tirar (copias de staging).
    vk::raii::CommandPool transient_pool_{nullptr};
    mutable std::mutex transient_mutex_;
    mutable std::mutex queue_mutex_;

    QueueFamilyIndices queue_families_{};
    vk::Format depth_format_ = vk::Format::eUndefined;
    bool depth_clamp_supported_ = false;
    bool indirect_count_supported_ = true;
    bool draw_indirect_first_instance_ = true;
    bool texture_compression_bc_supported_ = false;
    bool fill_mode_non_solid_supported_ = false;
    bool tessellation_supported_ = false;
    bool ray_tracing_supported_ = false;
    bool opacity_micromap_supported_ = false;
    bool ray_tracing_pipeline_supported_ = false;
    bool invocation_reorder_supported_ = false;
    bool mesh_shader_supported_ = false;
    bool memory_budget_supported_ = false;
    bool compat_mode_ = false;
    std::string compat_reason_;
    vk::PhysicalDeviceLimits limits_{};
    bool texture_compression_astc_supported_ = false;
    bool texture_compression_etc2_supported_ = false;
    std::string device_name_;
    std::uint32_t api_version_ = 0;
    std::vector<std::string> extra_extensions_;     // OpenXR
    std::vector<std::string> optional_extensions_;  // compartir memoria con un runtime de VR
    std::vector<std::string> enabled_extensions_;

    vk::raii::PipelineCache pipeline_cache_{nullptr};
    std::filesystem::path pipeline_cache_file_;
    mutable std::uint32_t pipelines_created_ = 0;
    std::function<void(std::uint32_t)> pipeline_callback_;

    // --- Envios sin espera (submitAsync). Al final: se destruyen antes que
    // el pool y el dispositivo. ---
    struct AsyncSubmit {
        std::shared_ptr<GpuTicket> ticket;
        std::optional<vk::raii::CommandBuffers> commands;  // se libera con transient_mutex_
        std::vector<std::shared_ptr<const void>> keep_alive;
    };
    void releaseAsync(std::vector<AsyncSubmit>& finished) const;
    mutable std::mutex async_mutex_;
    mutable std::vector<AsyncSubmit> async_;

    // --- Tareas en segundo plano ---
    void stopTaskThread();
    mutable std::mutex task_mutex_;
    mutable std::condition_variable task_cv_;
    mutable std::condition_variable task_done_cv_;
    mutable std::deque<std::function<void()>> tasks_;
    mutable std::thread task_thread_;
    mutable bool task_stop_ = false;
    mutable std::atomic<int> tasks_in_flight_{0};
    TaskScheduler scheduler_;  // con task_mutex_

    // --- Hilos de streaming (runStreamingTask) ---
    void stopStreamingThreads();
    mutable std::mutex stream_mutex_;
    mutable std::condition_variable stream_cv_;
    mutable std::deque<std::function<void()>> stream_tasks_;
    mutable std::vector<std::thread> stream_threads_;
    mutable bool stream_stop_ = false;
};

}  // namespace cramion::gfx

#endif  // CRAMION_VK_VULKAN_DEVICE_H
