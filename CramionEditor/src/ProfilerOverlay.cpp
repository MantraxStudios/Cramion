#include "ProfilerOverlay.h"

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <cstdio>

namespace cramion::editor {

namespace {

std::uint64_t fileTimeTo100ns(const FILETIME& t) {
    return (static_cast<std::uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime;
}

// Verde si va bien, amarillo regular, rojo mal (umbral en ms por frame).
ImU32 gradeColor(float milliseconds) {
    if (milliseconds <= 16.8f) return IM_COL32(120, 220, 120, 255);
    if (milliseconds <= 33.4f) return IM_COL32(240, 200, 90, 255);
    return IM_COL32(245, 100, 90, 255);
}

}  // namespace

void ProfilerOverlay::update(float dt, const gfx::VulkanRenderer& renderer) {
    const float frame_ms = std::max(dt, 0.0f) * 1000.0f;
    frame_ms_[next_] = frame_ms;
    next_ = (next_ + 1) % kHistory;
    filled_ = std::min(filled_ + 1, kHistory);

    // CPU trabajando = el frame menos lo que espero a la GPU en este frame.
    const double fence_total = renderer.fenceWaitTotalMs();
    const float waited = last_fence_total_ < 0.0 ? 0.0f : static_cast<float>(fence_total - last_fence_total_);
    last_fence_total_ = fence_total;

    window_time_ += dt;
    ++window_frames_;
    window_worst_ms_ = std::max(window_worst_ms_, frame_ms);
    window_cpu_ms_ += std::max(frame_ms - waited, 0.0f);
    if (window_time_ < 0.5f) return;

    // --- Se publica cada medio segundo (numeros que se pueden leer) ---
    fps_ = static_cast<float>(window_frames_) / window_time_;
    frame_avg_ms_ = window_time_ * 1000.0f / static_cast<float>(window_frames_);
    worst_fps_ = window_worst_ms_ > 0.0f ? 1000.0f / window_worst_ms_ : 0.0f;
    cpu_ms_ = window_cpu_ms_ / static_cast<float>(window_frames_);
    gpu_ms_ = renderer.gpuProfiler().supported() ? renderer.gpuProfiler().totalMilliseconds() : 0.0f;
    gpu_percent_ = frame_avg_ms_ > 0.0f ? std::min(gpu_ms_ / frame_avg_ms_ * 100.0f, 100.0f) : 0.0f;
    window_time_ = 0.0f;
    window_frames_ = 0;
    window_worst_ms_ = 0.0f;
    window_cpu_ms_ = 0.0f;

    // % de CPU del proceso: tiempo de todos sus hilos / (tiempo real x nucleos).
    FILETIME created, exited, kernel, user, now;
    if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        GetSystemTimeAsFileTime(&now);
        const std::uint64_t process = fileTimeTo100ns(kernel) + fileTimeTo100ns(user);
        const std::uint64_t wall = fileTimeTo100ns(now);
        if (last_wall_time_ != 0 && wall > last_wall_time_) {
            SYSTEM_INFO info;
            GetSystemInfo(&info);
            const double cores = std::max<DWORD>(info.dwNumberOfProcessors, 1);
            cpu_percent_ = static_cast<float>(static_cast<double>(process - last_process_time_) /
                                              (static_cast<double>(wall - last_wall_time_) * cores) * 100.0);
        }
        last_process_time_ = process;
        last_wall_time_ = wall;
    }
    PROCESS_MEMORY_COUNTERS memory{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof(memory))) {
        ram_mb_ = static_cast<double>(memory.WorkingSetSize) / (1024.0 * 1024.0);
    }
}

void ProfilerOverlay::draw(ImDrawList* draw, const ImVec2& origin, const ImVec2& size, const ecs::Profiler& settings,
                           const std::string& gpu_name) const {
    const float scale = std::clamp(settings.scale, 0.5f, 3.0f);
    ImFont* font = ImGui::GetFont();
    const float font_size = ImGui::GetStyle().FontSizeBase * scale;
    const float line = font_size + 3.0f * scale;
    const float pad = 8.0f * scale;
    const float width = 250.0f * scale;
    const float graph_height = 44.0f * scale;

    struct Line {
        std::string text;
        ImU32 color;
    };
    std::array<Line, 6> lines;
    std::size_t count = 0;
    char buffer[160];
    if (settings.show_fps) {
        std::snprintf(buffer, sizeof(buffer), "FPS  %.0f   (min %.0f)   %.2f ms", fps_, worst_fps_, frame_avg_ms_);
        lines[count++] = {buffer, gradeColor(frame_avg_ms_)};
    }
    if (settings.show_cpu) {
        std::snprintf(buffer, sizeof(buffer), "CPU  %.2f ms   %.0f %%", cpu_ms_, cpu_percent_);
        lines[count++] = {buffer, gradeColor(cpu_ms_)};
    }
    if (settings.show_gpu) {
        if (gpu_ms_ > 0.0f) {
            std::snprintf(buffer, sizeof(buffer), "GPU  %.2f ms   %.0f %%", gpu_ms_, gpu_percent_);
            lines[count++] = {buffer, gradeColor(gpu_ms_)};
        } else {
            lines[count++] = {"GPU  (sin timestamps)", IM_COL32(170, 175, 190, 255)};
        }
        if (!gpu_name.empty()) lines[count++] = {gpu_name, IM_COL32(150, 158, 175, 255)};
    }
    if (settings.show_memory) {
        std::snprintf(buffer, sizeof(buffer), "RAM  %.0f MB", ram_mb_);
        lines[count++] = {buffer, IM_COL32(200, 205, 220, 255)};
    }
    const bool graph = settings.show_graph && filled_ > 1;
    if (count == 0 && !graph) return;

    const float height = pad * 2.0f + line * static_cast<float>(count) + (graph ? graph_height + (count > 0 ? pad : 0.0f) : 0.0f);
    const float margin = 10.0f * scale;
    const bool right = settings.corner == ecs::ProfilerCorner::TopRight || settings.corner == ecs::ProfilerCorner::BottomRight;
    const bool bottom = settings.corner == ecs::ProfilerCorner::BottomRight || settings.corner == ecs::ProfilerCorner::BottomLeft;
    const ImVec2 a{right ? origin.x + size.x - width - margin : origin.x + margin,
                   bottom ? origin.y + size.y - height - margin : origin.y + margin};
    const ImVec2 b{a.x + width, a.y + height};

    draw->PushClipRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), true);
    const int alpha = static_cast<int>(std::clamp(settings.opacity, 0.0f, 1.0f) * 255.0f);
    draw->AddRectFilled(a, b, IM_COL32(10, 12, 18, alpha), 6.0f * scale);
    float y = a.y + pad;
    for (std::size_t i = 0; i < count; ++i) {
        draw->AddText(font, font_size, ImVec2(a.x + pad, y), lines[i].color, lines[i].text.c_str());
        y += line;
    }
    if (graph) {
        if (count > 0) y += pad - 3.0f * scale;
        const ImVec2 ga{a.x + pad, y};
        const ImVec2 gb{b.x - pad, y + graph_height};
        draw->AddRectFilled(ga, gb, IM_COL32(255, 255, 255, 14), 3.0f * scale);
        // Escala: al menos 33 ms, o el frame mas lento que se ve.
        float top = 33.4f;
        for (std::size_t i = 0; i < filled_; ++i) top = std::max(top, frame_ms_[i]);
        const float bar = (gb.x - ga.x) / static_cast<float>(kHistory);
        for (std::size_t i = 0; i < filled_; ++i) {
            // Del mas viejo (izquierda) al mas nuevo (derecha).
            const std::size_t index = (next_ + kHistory - filled_ + i) % kHistory;
            const float ms = frame_ms_[index];
            const float h = std::min(ms / top, 1.0f) * (gb.y - ga.y);
            const float x = ga.x + bar * static_cast<float>(kHistory - filled_ + i);
            draw->AddRectFilled(ImVec2(x, gb.y - h), ImVec2(x + std::max(bar - 0.5f, 1.0f), gb.y), gradeColor(ms));
        }
        // Linea de 60 FPS.
        const float y60 = gb.y - std::min(16.67f / top, 1.0f) * (gb.y - ga.y);
        draw->AddLine(ImVec2(ga.x, y60), ImVec2(gb.x, y60), IM_COL32(255, 255, 255, 90), 1.0f);
    }
    draw->PopClipRect();
}

const ecs::Profiler* findProfiler(const ecs::World& world) {
    const ecs::Profiler* found = nullptr;
    world.forEachDepthFirst([&](ecs::Entity e) {
        if (found == nullptr && e.activeInHierarchy()) found = e.tryGet<ecs::Profiler>();
    });
    return found;
}

}  // namespace cramion::editor
