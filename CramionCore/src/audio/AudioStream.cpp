#include "CramionCore/audio/AudioStream.h"

// Anillo de audio de un video (contrato E, WP3).
//
// Posiciones absolutas (frames escritos/leidos desde que se creo el stream,
// nunca vuelven atras): el escritor avanza write_pos y cada lector su pos.
// reset() no pone nada a cero; mueve la base (frame y pts) a la cabeza y lleva
// a todos los lectores ahi. Asi un read() que corre a la vez nunca ve un lector
// por delante del escritor.
//
//   - Solo el lector maestro frena al escritor (freeFrames). Sin maestro, el
//     escritor no espera a nadie.
//   - Un lector no maestro que se queda atras mas que la capacidad (sobrescrito)
//     salta a la cabeza.
//   - read() corre en el hilo de audio: sin mutex ni memoria nueva. Avanza con
//     compare_exchange para que un reset() simultaneo gane.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <mutex>
#include <vector>

namespace cramion::audio {

namespace {
constexpr int kMaxReaders = 8;

std::int64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
}  // namespace

struct AudioStream::Impl {
    std::uint32_t rate = 48000;
    std::uint32_t channels = 2;
    std::uint64_t capacity = 1;  // frames
    std::vector<float> ring;     // capacity * channels

    std::atomic<std::uint64_t> write_pos{0};

    struct Reader {
        std::atomic<bool> used{false};
        std::atomic<std::uint64_t> pos{0};
    };
    std::array<Reader, kMaxReaders> readers;
    std::atomic<int> master{-1};
    std::mutex readers_mutex;  // solo addReader/removeReader (hilo principal)

    // Base del reloj (reset): se leen juntas con un seqlock.
    std::atomic<std::uint32_t> seq{0};
    std::atomic<std::uint64_t> base_frame{0};
    std::atomic<double> base_pts{0.0};

    // Interpolacion entre callbacks (solo el maestro).
    std::atomic<std::int64_t> last_read_ns{0};
    std::atomic<std::uint32_t> last_read_frames{0};
    std::atomic<double> latency{0.0};

    void copyOut(std::uint64_t from, float* out, std::size_t frames) const {
        std::size_t done = 0;
        while (done < frames) {
            const std::uint64_t index = (from + done) % capacity;
            const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(frames - done, capacity - index));
            std::memcpy(out + done * channels, ring.data() + index * channels, n * channels * sizeof(float));
            done += n;
        }
    }

    void copyIn(std::uint64_t to, const float* in, std::size_t frames) {
        std::size_t done = 0;
        while (done < frames) {
            const std::uint64_t index = (to + done) % capacity;
            const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(frames - done, capacity - index));
            std::memcpy(ring.data() + index * channels, in + done * channels, n * channels * sizeof(float));
            done += n;
        }
    }

    void readBase(std::uint64_t& frame, double& pts) const {
        for (;;) {
            const std::uint32_t s1 = seq.load(std::memory_order_acquire);
            frame = base_frame.load(std::memory_order_relaxed);
            pts = base_pts.load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if ((s1 & 1u) == 0 && seq.load(std::memory_order_relaxed) == s1) return;
        }
    }
};

AudioStream::AudioStream(std::uint32_t rate, std::uint32_t channels, double capacity_seconds)
    : impl_(std::make_unique<Impl>()) {
    Impl& d = *impl_;
    d.rate = std::max<std::uint32_t>(rate, 1);
    d.channels = std::max<std::uint32_t>(channels, 1);
    const double frames = std::max(capacity_seconds, 0.01) * static_cast<double>(d.rate);
    d.capacity = std::max<std::uint64_t>(static_cast<std::uint64_t>(frames), 64);
    d.ring.assign(static_cast<std::size_t>(d.capacity * d.channels), 0.0f);
}

AudioStream::~AudioStream() = default;

std::uint32_t AudioStream::rate() const { return impl_->rate; }
std::uint32_t AudioStream::channels() const { return impl_->channels; }
void AudioStream::setOutputLatency(double seconds) { impl_->latency.store(std::max(seconds, 0.0), std::memory_order_relaxed); }

std::size_t AudioStream::freeFrames() const {
    const Impl& d = *impl_;
    const int m = d.master.load(std::memory_order_acquire);
    if (m < 0) return static_cast<std::size_t>(d.capacity);
    const std::uint64_t w = d.write_pos.load(std::memory_order_relaxed);
    const std::uint64_t r = d.readers[static_cast<std::size_t>(m)].pos.load(std::memory_order_acquire);
    const std::uint64_t used = w > r ? w - r : 0;
    return used >= d.capacity ? 0 : static_cast<std::size_t>(d.capacity - used);
}

std::size_t AudioStream::write(const float* interleaved, std::size_t frames) {
    Impl& d = *impl_;
    if (interleaved == nullptr || frames == 0) return 0;
    const std::size_t n = std::min(frames, freeFrames());
    if (n == 0) return 0;
    const std::uint64_t w = d.write_pos.load(std::memory_order_relaxed);
    d.copyIn(w, interleaved, n);
    d.write_pos.store(w + n, std::memory_order_release);
    return n;
}

void AudioStream::reset(double base_pts) {
    Impl& d = *impl_;
    // Lo que quedaba en el anillo se descarta: todos a la cabeza.
    const std::uint64_t w = d.write_pos.load(std::memory_order_acquire);
    const std::uint32_t s = d.seq.load(std::memory_order_relaxed);
    d.seq.store(s + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    d.base_frame.store(w, std::memory_order_relaxed);
    d.base_pts.store(base_pts, std::memory_order_relaxed);
    d.seq.store(s + 2, std::memory_order_release);
    for (Impl::Reader& reader : d.readers) {
        if (reader.used.load(std::memory_order_acquire)) reader.pos.store(w, std::memory_order_release);
    }
    d.last_read_frames.store(0, std::memory_order_relaxed);
}

int AudioStream::addReader(bool master) {
    Impl& d = *impl_;
    std::lock_guard<std::mutex> lock(d.readers_mutex);
    for (int i = 0; i < kMaxReaders; ++i) {
        Impl::Reader& reader = d.readers[static_cast<std::size_t>(i)];
        if (reader.used.load(std::memory_order_relaxed)) continue;
        const std::uint64_t w = d.write_pos.load(std::memory_order_acquire);
        const int current = d.master.load(std::memory_order_relaxed);
        std::uint64_t start = w;
        if (current >= 0) {
            // Al compas del maestro (mismo sonido en N altavoces).
            start = d.readers[static_cast<std::size_t>(current)].pos.load(std::memory_order_acquire);
        } else if (master) {
            // Sin maestro el escritor no esperaba: lo mas antiguo que sigue en el anillo.
            std::uint64_t base = 0;
            double pts = 0.0;
            d.readBase(base, pts);
            start = std::max(base, w > d.capacity ? w - d.capacity : 0);
        }
        reader.pos.store(start, std::memory_order_relaxed);
        reader.used.store(true, std::memory_order_release);
        if (master && current < 0) {
            d.last_read_frames.store(0, std::memory_order_relaxed);
            d.master.store(i, std::memory_order_release);
        }
        return i;
    }
    return -1;
}

void AudioStream::removeReader(int reader) {
    Impl& d = *impl_;
    if (reader < 0 || reader >= kMaxReaders) return;
    std::lock_guard<std::mutex> lock(d.readers_mutex);
    d.readers[static_cast<std::size_t>(reader)].used.store(false, std::memory_order_release);
    if (d.master.load(std::memory_order_relaxed) == reader) {
        // Otro lector pasa a marcar el reloj (misma escala de frames: sin salto).
        int next = -1;
        for (int i = 0; i < kMaxReaders; ++i) {
            if (d.readers[static_cast<std::size_t>(i)].used.load(std::memory_order_relaxed)) {
                next = i;
                break;
            }
        }
        d.last_read_frames.store(0, std::memory_order_relaxed);
        d.master.store(next, std::memory_order_release);
    }
}

std::size_t AudioStream::read(int reader, float* out, std::size_t frames) {
    Impl& d = *impl_;
    if (out == nullptr || frames == 0) return 0;
    const std::size_t ch = d.channels;
    const auto silence = [&](std::size_t from) {
        std::fill(out + from * ch, out + frames * ch, 0.0f);
    };
    if (reader < 0 || reader >= kMaxReaders) {
        silence(0);
        return 0;
    }
    Impl::Reader& slot = d.readers[static_cast<std::size_t>(reader)];
    const bool is_master = d.master.load(std::memory_order_acquire) == reader;
    if (!slot.used.load(std::memory_order_acquire) || paused.load(std::memory_order_relaxed)) {
        silence(0);  // en pausa no avanza (ni el reloj se interpola)
        if (is_master) d.last_read_frames.store(0, std::memory_order_relaxed);
        return 0;
    }
    const std::uint64_t original = slot.pos.load(std::memory_order_acquire);
    const std::uint64_t w = d.write_pos.load(std::memory_order_acquire);
    std::uint64_t r = original;
    if (r > w) {  // no deberia pasar (reset a la vez): silencio y se reintenta
        silence(0);
        return 0;
    }
    if (w - r > d.capacity) r = w;  // sobrescrito (no maestro): salta a la cabeza
    std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(frames, w - r));
    if (n > 0) {
        d.copyOut(r, out, n);
        if (!is_master) {
            // El escritor no espera a este lector: si piso lo copiado, fuera.
            const std::uint64_t w2 = d.write_pos.load(std::memory_order_acquire);
            if (w2 - r > d.capacity) {
                r = w2;
                n = 0;
            }
        }
    }
    silence(n);
    std::uint64_t expected = original;
    slot.pos.compare_exchange_strong(expected, r + n, std::memory_order_acq_rel);  // falla si hubo reset: gana el reset
    if (is_master) {
        d.last_read_ns.store(nowNs(), std::memory_order_relaxed);
        d.last_read_frames.store(static_cast<std::uint32_t>(n), std::memory_order_release);
    }
    return n;
}

double AudioStream::playedSeconds() const {
    const Impl& d = *impl_;
    std::uint64_t base = 0;
    double pts = 0.0;
    d.readBase(base, pts);
    const int m = d.master.load(std::memory_order_acquire);
    if (m < 0) return pts;
    const std::uint64_t pos = d.readers[static_cast<std::size_t>(m)].pos.load(std::memory_order_acquire);
    const double rate = static_cast<double>(d.rate);
    double t = pts + static_cast<double>(pos > base ? pos - base : 0) / rate;
    // Entre callbacks el dispositivo sigue sonando: se interpola, como mucho el
    // ultimo bloque (si el callback se retrasa, el reloj no se adelanta).
    const std::uint32_t last = d.last_read_frames.load(std::memory_order_acquire);
    if (last > 0 && !paused.load(std::memory_order_relaxed)) {
        const double elapsed = static_cast<double>(nowNs() - d.last_read_ns.load(std::memory_order_relaxed)) * 1e-9;
        t += std::clamp(elapsed, 0.0, static_cast<double>(last) / rate);
    }
    // Lo leido aun no se oye: va por delante la latencia de salida.
    t -= d.latency.load(std::memory_order_relaxed);
    return std::max(t, pts);
}

}  // namespace cramion::audio
