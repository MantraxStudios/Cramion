#ifndef CRAMION_VK_VULKAN_DEVICE_UPLOAD_H
#define CRAMION_VK_VULKAN_DEVICE_UPLOAD_H

// Subidas a la GPU por lotes.
//
// Antes cada buffer y cada textura se subia con su propio command buffer, su
// fence y una espera completa en la cola de graficos (VulkanDevice::
// submitOneTime): un modelo con 30 texturas eran ~32 esperas que competian con
// el frame. Ahora:
//   - Todas las copias de un modelo van en un UploadBatch: un command buffer
//     (dos con cola de transferencia) y una sola fence.
//   - Si la GPU tiene una cola solo de copias, las copias van alli y los
//     recursos pasan a la cola de graficos con barreras de propiedad
//     (release en la de transferencia, acquire en la de graficos, unidas por
//     un semaforo). Los mips (blits) se hacen en la de graficos.
//   - No se espera: el lote devuelve un UploadFence que se consulta sin
//     bloquear (VulkanRenderer::uploadNewModels instala el modelo cuando esta).
//   - Los datos pasan por un anillo de staging persistente y mapeado
//     (StagingRing) en lugar de un buffer de staging nuevo por recurso.

#include "CramionFX/vk/GpuMemoryStats.h"
#include "CramionFX/vk/VulkanCommon.h"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

struct VmaAllocation_T;

namespace cramion::gfx {

class GpuTicket;
class VulkanBuffer;
class VulkanDevice;

// Anillo de staging: un buffer visible desde la CPU, mapeado para siempre,
// del que cada lote toma tramos. Un tramo se devuelve cuando la GPU termina
// el lote que lo uso (al destruirse el lote); los tramos se liberan en
// cualquier orden y el anillo avanza por la cola en cuanto los primeros estan
// libres. Si no cabe (anillo lleno o dato mas grande que el anillo), el lote
// usa un buffer de staging propio: nunca se espera.
class StagingRing {
public:
    struct Slice {
        vk::Buffer buffer{};
        vk::DeviceSize offset = 0;
        void* mapped = nullptr;
        std::uint64_t id = 0;  // para release()
    };

    StagingRing(const VulkanDevice& device, vk::DeviceSize capacity);
    ~StagingRing();
    StagingRing(const StagingRing&) = delete;
    StagingRing& operator=(const StagingRing&) = delete;

    // Reserva `size` bytes alineados a `alignment`. false si no cabe ahora.
    bool allocate(vk::DeviceSize size, vk::DeviceSize alignment, Slice& out);
    // Hace visibles a la GPU los bytes escritos (no-op si la memoria es coherente).
    void flush(const Slice& slice, vk::DeviceSize size) const;
    void release(std::uint64_t id);
    vk::DeviceSize capacity() const { return capacity_; }

private:
    struct Entry {
        std::uint64_t id = 0;
        vk::DeviceSize begin = 0;
        vk::DeviceSize end = 0;
        bool released = false;
    };

    const VulkanDevice* device_ = nullptr;
    vk::raii::Buffer buffer_{nullptr};
    VmaAllocation_T* allocation_ = nullptr;
    GpuMemoryTicket memory_ticket_;
    void* mapped_ = nullptr;
    vk::DeviceSize capacity_ = 0;

    std::mutex mutex_;
    std::deque<Entry> entries_;  // en orden de reserva
    std::uint64_t next_id_ = 1;
};

// Fin de un lote, conocido ANTES de enviarlo: otro modelo que reutiliza una
// textura o un buffer compartido (caches por contenido de SkinnedModel) puede
// depender de un lote que otro hilo aun esta grabando.
class UploadFence {
public:
    // true cuando el lote y los lotes de los que depende terminaron en la
    // GPU. No bloquea. Desde cualquier hilo.
    bool done() const;
    // Bloquea hasta entonces (caminos sincronos).
    void wait() const;

private:
    friend class UploadBatch;
    bool ownDone() const;
    void ownWait() const;
    // Enviado (`ticket` nulo: no habia nada que esperar o se abandono).
    void finish(std::shared_ptr<GpuTicket> ticket, std::vector<std::shared_ptr<UploadFence>> dependencies);

    mutable std::mutex mutex_;
    mutable std::condition_variable cv_;
    bool submitted_ = false;
    mutable bool finished_ = false;  // la GPU termino (ya no hace falta el ticket)
    mutable std::shared_ptr<GpuTicket> ticket_;
    std::vector<std::shared_ptr<UploadFence>> dependencies_;
};

// Un lote de subidas: se graban las copias de varios recursos y se envian
// juntas. Lo usa un solo hilo a la vez (tiene sus propios command pools).
//
//   auto batch = std::make_shared<UploadBatch>(device);
//   auto vb = VulkanBuffer::createDeviceLocal(device, data, size, usage, *batch);
//   texture.create(device, w, h, pixels, *batch);
//   std::shared_ptr<UploadFence> ready = batch->submit();  // sin esperar
//   ... if (ready->done()) usar
//
// Para enviar sin esperar el lote tiene que estar en un shared_ptr (se
// mantiene vivo, con sus command buffers y su staging, hasta que la GPU
// termina). En la pila, submitAndWait().
class UploadBatch : public std::enable_shared_from_this<UploadBatch> {
public:
    explicit UploadBatch(const VulkanDevice& device);
    // Si se graba algo y no se envia (una excepcion a mitad), se envia y se
    // espera aqui: lo grabado puede ser de recursos compartidos que otros
    // modelos ya esperan.
    ~UploadBatch();
    UploadBatch(const UploadBatch&) = delete;
    UploadBatch& operator=(const UploadBatch&) = delete;

    const VulkanDevice& device() const { return *device_; }
    // Las copias van en la cola de transferencia (hay que pasar la propiedad).
    bool crossQueue() const { return cross_queue_; }

    // Copia `size` bytes a staging. El tramo vale hasta que la GPU termina.
    struct Staging {
        vk::Buffer buffer{};
        vk::DeviceSize offset = 0;
    };
    Staging stage(const void* data, vk::DeviceSize size, vk::DeviceSize alignment = 16);

    // Donde grabar las copias (cola de transferencia o, sin ella, graficos).
    const vk::raii::CommandBuffer& copyCommands();
    // Donde grabar lo que necesita la cola de graficos (blits de mips) una vez
    // recibida la propiedad. Sin cola de transferencia, el mismo de las copias.
    const vk::raii::CommandBuffer& graphicsCommands();

    // Deja un buffer recien copiado listo para la cola de graficos: con cola
    // de transferencia, release + acquire; sin ella no hace falta nada (la
    // fence ordena la copia con los frames siguientes, como antes).
    void handOffBuffer(vk::Buffer buffer);
    // Lo mismo para una imagen. `barrier` es la barrera que se pondria con una
    // sola cola (src = copia, dst = quien la usa, con su cambio de layout); con
    // cola de transferencia se parte en release (copias) y acquire (graficos).
    void handOffImage(const vk::ImageMemoryBarrier2& barrier);

    // Vive hasta que la GPU termina el lote (destinos de las copias, staging).
    void keepAlive(std::shared_ptr<const void> object);
    // El lote no esta listo hasta que `other` lo este (recurso compartido que
    // subio otro lote). Nulo: nada.
    void dependOn(std::shared_ptr<UploadFence> other);
    // La fence de este lote (valida antes de enviarlo).
    const std::shared_ptr<UploadFence>& fence() const { return fence_; }
    bool empty() const { return !recorded_; }

    // Envia sin esperar (el lote debe estar en un shared_ptr; si no, espera).
    std::shared_ptr<UploadFence> submit();
    // Envia y espera a la GPU (y a los lotes de los que depende).
    void submitAndWait();

private:
    void begin(vk::raii::CommandPool& pool, std::optional<vk::raii::CommandBuffers>& commands,
               std::uint32_t family);
    // Termina de grabar y envia. `self`: lo que se mantiene vivo (nulo si se
    // espera aqui mismo).
    void send(std::shared_ptr<const void> self);

    const VulkanDevice* device_ = nullptr;
    bool cross_queue_ = false;
    bool recorded_ = false;
    bool submitted_ = false;

    // Los pools antes que los command buffers: se destruyen despues.
    vk::raii::CommandPool copy_pool_{nullptr};
    vk::raii::CommandPool graphics_pool_{nullptr};
    std::optional<vk::raii::CommandBuffers> copy_commands_;
    std::optional<vk::raii::CommandBuffers> graphics_commands_;
    bool graphics_used_ = false;  // algo grabado en el de graficos (con cola aparte)

    std::vector<std::uint64_t> ring_slices_;                 // tramos del anillo a devolver
    std::vector<std::shared_ptr<VulkanBuffer>> own_staging_;  // lo que no cupo en el anillo
    std::vector<std::shared_ptr<const void>> keep_alive_;
    std::vector<std::shared_ptr<UploadFence>> dependencies_;
    std::shared_ptr<UploadFence> fence_ = std::make_shared<UploadFence>();
};

}  // namespace cramion::gfx

#endif  // CRAMION_VK_VULKAN_DEVICE_UPLOAD_H
