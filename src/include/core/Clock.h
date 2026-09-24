#ifndef CRAMION_CORE_CLOCK_H
#define CRAMION_CORE_CLOCK_H

#include <chrono>
#include <cstdint>

namespace cramion::core {

// Reloj del bucle principal: mide el tiempo entre frames (delta time) y lleva
// la cuenta de frames y de FPS.
//
// Uso:
//   Clock clock;
//   while (running) {
//       const float dt = clock.tick();   // segundos desde el frame anterior
//       update(dt);
//       render();
//   }
//
// El delta se limita a kMaxDeltaSeconds para que una pausa larga (arrastrar la
// ventana, un breakpoint, el equipo suspendido) no produzca un salto enorme que
// rompa la simulacion.
class Clock {
public:
    // Tope del delta devuelto por tick(), en segundos.
    static constexpr float kMaxDeltaSeconds = 0.25f;

    // Intervalo de refresco de la media de FPS, en segundos.
    static constexpr double kFpsRefreshSeconds = 0.5;

    Clock();

    // Reinicia el reloj: tiempo total, contador de frames y medias a cero.
    void reset();

    // Cierra el frame actual y abre el siguiente. Devuelve el delta en segundos
    // (ya limitado). El primer tick() devuelve 0.
    float tick();

    // Delta del ultimo tick(), en segundos.
    float deltaSeconds() const { return delta_seconds_; }

    // Delta del ultimo tick(), en milisegundos.
    float deltaMilliseconds() const { return delta_seconds_ * 1000.0f; }

    // Tiempo transcurrido desde reset(), en segundos. Es el tiempo real, sin
    // limitar, asi que no coincide con la suma de los deltas si hubo pausas.
    double totalSeconds() const { return total_seconds_; }

    // Frames completados desde reset().
    std::uint64_t frameCount() const { return frame_count_; }

    // Media de FPS del ultimo intervalo de kFpsRefreshSeconds.
    float fps() const { return fps_; }

    // Media de milisegundos por frame del ultimo intervalo.
    float averageFrameMilliseconds() const { return average_frame_ms_; }

    // true solo durante el frame en el que se acaba de recalcular la media;
    // util para refrescar el titulo de la ventana sin hacerlo cada frame.
    bool fpsUpdated() const { return fps_updated_; }

private:
    using SteadyClock = std::chrono::steady_clock;
    using TimePoint = SteadyClock::time_point;

    TimePoint start_{};
    TimePoint last_tick_{};

    float delta_seconds_ = 0.0f;
    double total_seconds_ = 0.0;
    std::uint64_t frame_count_ = 0;

    // Acumuladores del intervalo en curso para la media de FPS.
    double fps_accumulated_seconds_ = 0.0;
    std::uint32_t fps_accumulated_frames_ = 0;

    float fps_ = 0.0f;
    float average_frame_ms_ = 0.0f;
    bool fps_updated_ = false;
};

}  // namespace cramion::core

#endif  // CRAMION_CORE_CLOCK_H
