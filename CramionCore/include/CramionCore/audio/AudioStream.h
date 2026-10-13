#ifndef CRAMION_AUDIO_STREAM_H
#define CRAMION_AUDIO_STREAM_H

// Contrato E (WP3). PCM float32 entrelazado en el formato NATIVO de la pista
// del decodificador (rate/channels de AudioChunk); el ma_data_source lo
// declara y ma_sound convierte y aplica el pitch.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace cramion::audio {

// Un escritor (hilo del video) y hasta 8 lectores (voces), cada uno con su
// cursor. El escritor solo respeta al lector maestro (la voz del reloj); los
// demas pueden quedar sobrescritos y saltan a la cabeza (se resincronizan).
// read() no usa mutex ni reserva memoria (corre en el hilo de audio).
class AudioStream {
public:
    AudioStream(std::uint32_t rate, std::uint32_t channels, double capacity_seconds);
    ~AudioStream();
    AudioStream(const AudioStream&) = delete;
    AudioStream& operator=(const AudioStream&) = delete;
    std::size_t write(const float* interleaved, std::size_t frames);  // devuelve los escritos
    std::size_t freeFrames() const;                                   // segun el lector maestro
    void reset(double base_pts);                                      // seek / bucle
    int addReader(bool master = false);                               // -1 si no caben mas
    void removeReader(int reader);                                    // despues de ma_sound_uninit
    std::size_t read(int reader, float* out, std::size_t frames);     // rellena con silencio si falta
    // base_pts + leidos por el maestro / rate - latencia de salida, interpolado
    // con steady_clock entre callbacks.
    double playedSeconds() const;
    void setOutputLatency(double seconds);
    std::uint32_t rate() const;
    std::uint32_t channels() const;
    std::atomic<bool> paused{false};                                  // read() da silencio sin avanzar
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace cramion::audio
#endif
