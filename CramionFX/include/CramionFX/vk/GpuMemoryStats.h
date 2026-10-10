#ifndef CRAMIONFX_VK_GPU_MEMORY_STATS_H
#define CRAMIONFX_VK_GPU_MEMORY_STATS_H

// Cuenta de la memoria de video reservada, por tipo: el renderizador reserva
// cada recurso por separado (sin un asignador comun) y, con la VRAM llena, no
// habia forma de saber que la ocupaba. Lo lleva un "ticket" dentro de cada
// recurso: suma al reservar y resta al destruirse (sigue al recurso al
// moverlo).

#include <array>
#include <atomic>
#include <cstdint>
#include <utility>

namespace cramion::gfx {

enum class GpuMemoryKind : std::uint8_t {
    Texture,  // VulkanTexture: texturas de modelos, iconos...
    Image,    // VulkanImage: destinos de render, sombras, G-buffer...
    Buffer,   // VulkanBuffer en la GPU: mallas, BLAS, culling...
    Staging,  // VulkanBuffer visible desde la CPU (uniformes, copias)
    Count,
};

inline std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(GpuMemoryKind::Count)>& gpuMemoryCounters() {
    static std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(GpuMemoryKind::Count)> counters{};
    return counters;
}

inline std::uint64_t gpuMemoryBytes(GpuMemoryKind kind) {
    return gpuMemoryCounters()[static_cast<std::size_t>(kind)].load(std::memory_order_relaxed);
}

class GpuMemoryTicket {
public:
    GpuMemoryTicket() = default;
    GpuMemoryTicket(GpuMemoryKind kind, std::uint64_t bytes) : kind_(kind), bytes_(bytes) {
        gpuMemoryCounters()[static_cast<std::size_t>(kind_)].fetch_add(bytes_, std::memory_order_relaxed);
    }
    ~GpuMemoryTicket() { release(); }
    GpuMemoryTicket(const GpuMemoryTicket&) = delete;
    GpuMemoryTicket& operator=(const GpuMemoryTicket&) = delete;
    GpuMemoryTicket(GpuMemoryTicket&& other) noexcept
        : kind_(other.kind_), bytes_(std::exchange(other.bytes_, 0)) {}
    GpuMemoryTicket& operator=(GpuMemoryTicket&& other) noexcept {
        if (this != &other) {
            release();
            kind_ = other.kind_;
            bytes_ = std::exchange(other.bytes_, 0);
        }
        return *this;
    }
    void release() {
        if (bytes_ == 0) return;
        gpuMemoryCounters()[static_cast<std::size_t>(kind_)].fetch_sub(bytes_, std::memory_order_relaxed);
        bytes_ = 0;
    }

private:
    GpuMemoryKind kind_ = GpuMemoryKind::Texture;
    std::uint64_t bytes_ = 0;
};

}  // namespace cramion::gfx

#endif  // CRAMIONFX_VK_GPU_MEMORY_STATS_H
