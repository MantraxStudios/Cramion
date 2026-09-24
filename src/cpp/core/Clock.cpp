#include "core/Clock.h"

#include <algorithm>

namespace cramion::core {

Clock::Clock() {
    reset();
}

void Clock::reset() {
    start_ = SteadyClock::now();
    last_tick_ = start_;

    delta_seconds_ = 0.0f;
    total_seconds_ = 0.0;
    frame_count_ = 0;

    fps_accumulated_seconds_ = 0.0;
    fps_accumulated_frames_ = 0;
    fps_ = 0.0f;
    average_frame_ms_ = 0.0f;
    fps_updated_ = false;
}

float Clock::tick() {
    const TimePoint now = SteadyClock::now();

    // Tiempo real transcurrido desde el tick anterior.
    const double elapsed = std::chrono::duration<double>(now - last_tick_).count();
    last_tick_ = now;

    total_seconds_ = std::chrono::duration<double>(now - start_).count();
    ++frame_count_;

    // El delta que ve la simulacion va limitado; la media de FPS usa el tiempo
    // real para que siga reflejando el rendimiento de verdad.
    delta_seconds_ = std::min(static_cast<float>(elapsed), kMaxDeltaSeconds);

    fps_accumulated_seconds_ += elapsed;
    ++fps_accumulated_frames_;
    fps_updated_ = false;

    if (fps_accumulated_seconds_ >= kFpsRefreshSeconds) {
        fps_ = static_cast<float>(fps_accumulated_frames_ / fps_accumulated_seconds_);
        average_frame_ms_ =
            static_cast<float>(fps_accumulated_seconds_ * 1000.0 / fps_accumulated_frames_);
        fps_updated_ = true;

        fps_accumulated_seconds_ = 0.0;
        fps_accumulated_frames_ = 0;
    }

    return delta_seconds_;
}

}  // namespace cramion::core
