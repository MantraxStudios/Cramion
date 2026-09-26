#ifndef CRAMION_EDITOR_PROFILER_OVERLAY_H
#define CRAMION_EDITOR_PROFILER_OVERLAY_H

// Lo que dibuja el componente Profiler (ecs::Profiler): FPS, CPU, GPU y
// memoria en una esquina de la pantalla del juego, con la grafica del tiempo
// de cada frame. Lo usan el juego exportado (CramionPlayer) y la vista Juego
// del editor.
//
//   update(dt, renderer)  una vez por frame (aunque no haya Profiler: asi
//                         al anadirlo ya tiene datos)
//   draw(...)             si la escena tiene un Profiler (findProfiler)
//
// De donde sale cada numero:
//   FPS   frames contados en el ultimo medio segundo; "min" = el frame mas
//         lento de ese tramo
//   CPU   ms de trabajo por frame = tiempo del frame - lo que la CPU espero a
//         la GPU (fence); % = tiempo de CPU del proceso (todos sus hilos)
//         entre el tiempo real y los nucleos
//   GPU   ms medidos en la GPU con timestamps de Vulkan (GpuProfiler); % =
//         ms de GPU / ms del frame (cuanto del frame estuvo ocupada)
//   RAM   memoria del proceso (working set)

#include <CramionCore/ecs/Components.h>
#include <CramionCore/ecs/World.h>
#include <CramionFX/vk/VulkanRenderer.h>

#include <imgui.h>

#include <array>
#include <cstdint>
#include <string>

namespace cramion::editor {

class ProfilerOverlay {
public:
    void update(float dt, const gfx::VulkanRenderer& renderer);
    void draw(ImDrawList* draw, const ImVec2& origin, const ImVec2& size, const ecs::Profiler& settings,
              const std::string& gpu_name) const;

    float fps() const { return fps_; }
    float cpuMilliseconds() const { return cpu_ms_; }
    float gpuMilliseconds() const { return gpu_ms_; }

private:
    static constexpr std::size_t kHistory = 120;
    std::array<float, kHistory> frame_ms_{};  // circular
    std::size_t next_ = 0;
    std::size_t filled_ = 0;

    // Tramo actual (se publica cada medio segundo).
    float window_time_ = 0.0f;
    int window_frames_ = 0;
    float window_worst_ms_ = 0.0f;
    float window_cpu_ms_ = 0.0f;
    double last_fence_total_ = -1.0;

    // Publicado.
    float fps_ = 0.0f;
    float worst_fps_ = 0.0f;
    float frame_avg_ms_ = 0.0f;
    float cpu_ms_ = 0.0f;
    float cpu_percent_ = 0.0f;
    float gpu_ms_ = 0.0f;
    float gpu_percent_ = 0.0f;
    double ram_mb_ = 0.0;

    // Tiempos del proceso (GetProcessTimes) para el % de CPU.
    std::uint64_t last_process_time_ = 0;
    std::uint64_t last_wall_time_ = 0;
};

// El primer Profiler activo de la escena, o nullptr.
const ecs::Profiler* findProfiler(const ecs::World& world);

}  // namespace cramion::editor

#endif  // CRAMION_EDITOR_PROFILER_OVERLAY_H
