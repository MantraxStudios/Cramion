#ifndef CRAMION_VK_GPU_PROFILER_H
#define CRAMION_VK_GPU_PROFILER_H

#include "CramionFX/vk/VulkanCommon.h"

#include <cstdint>
#include <string>
#include <vector>

namespace cramion::gfx {

class VulkanDevice;

// Cuanto tarda en la GPU cada pasada del frame, con timestamps de Vulkan.
//
//   begin(cmd, frame)          al empezar a grabar el frame
//   mark(cmd, frame, "SSAO")   al terminar cada pasada: el tiempo desde la
//                              marca anterior se atribuye a "SSAO"
//   collect(frame)             despues de esperar la fence de ese hueco:
//                              lee los tiempos (ya terminados) y los promedia
//
// Cada hueco de frame en vuelo tiene su tramo de consultas, asi que leer no
// bloquea nunca a la GPU. Los tiempos son una media exponencial (~1 s) para
// que se puedan leer sin que salten.
struct GpuTiming {
    std::string name;
    float milliseconds = 0.0f;
};

class GpuProfiler {
public:
    static constexpr std::uint32_t kMaxMarks = 48;

    void create(const VulkanDevice& device, std::uint32_t frames_in_flight);
    void destroy();

    bool supported() const { return supported_; }

    void begin(const vk::raii::CommandBuffer& cmd, std::uint32_t frame);
    void mark(const vk::raii::CommandBuffer& cmd, std::uint32_t frame, const char* name);
    void collect(std::uint32_t frame);

    // Pasadas en el orden del frame, en milisegundos (medias).
    const std::vector<GpuTiming>& timings() const { return timings_; }
    float totalMilliseconds() const { return total_ms_; }

private:
    struct FrameMarks {
        std::vector<const char*> names;  // una por mark() (la primera marca es begin)
        bool recorded = false;
    };

    vk::raii::QueryPool pool_{nullptr};
    std::vector<FrameMarks> frames_;
    std::vector<GpuTiming> timings_;
    float total_ms_ = 0.0f;
    double period_ns_ = 1.0;
    std::uint64_t valid_mask_ = ~0ull;
    bool supported_ = false;
};

}  // namespace cramion::gfx

#endif  // CRAMION_VK_GPU_PROFILER_H
