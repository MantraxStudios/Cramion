#ifndef CRAMION_VIDEO_WORKER_H
#define CRAMION_VIDEO_WORKER_H

// El hilo de decodificacion de un reproductor (privado de src/video).
//
// Abre el decodificador, deja el primer frame en la cola (Prepared) y sigue
// bombeando mientras haya sitio: frames en una cola corta (max_queued_frames,
// nunca segundos de frames decodificados) y audio en un AudioStream por pista
// (el anillo frena al hilo cuando la voz maestra no consume).
//
// Linea de tiempo: cada frame lleva su `timeline` = (pts - inicio) + vueltas *
// longitud de la seccion. Asi el bucle no se nota (el hilo vuelve al inicio
// con seek y sigue) y el reloj del hilo principal solo avanza.
//
// Busquedas por epoch: seek() (hilo principal) sube el epoch, vacia la cola y
// reinicia los anillos; el hilo ve el epoch nuevo, busca en el decodificador
// y descarta lo anterior al objetivo. Un frame de otro epoch nunca entra.
//
// Parar: requestStop() + interrupt() del decodificador; finish() espera hasta
// el plazo y si el hilo no contesta lo suelta (detach): el hilo solo toca su
// estado compartido (shared_ptr), nunca el World.

#include "CramionCore/audio/AudioStream.h"
#include "CramionCore/video/VideoDecoder.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace cramion::video::detail {

struct QueuedFrame {
    VideoFrame frame;
    double timeline = 0.0;      // segundos desde start_time, con las vueltas
    std::uint64_t epoch = 0;
};

struct WorkerConfig {
    std::string uri;
    DecodeOptions options;
    double start_time = 0.0;    // s del video
    double end_time = 0.0;      // s del video (0 = hasta el final)
    bool loop = false;
    bool skip_on_drop = true;
    int max_queued = 4;
    double audio_seconds = 1.0;  // capacidad de cada anillo de audio
};

struct AudioTrackStream {
    int track = -1;
    std::shared_ptr<audio::AudioStream> stream;
};

class VideoWorker {
public:
    explicit VideoWorker(WorkerConfig config);
    ~VideoWorker();  // requestStop + finish(200 ms)
    VideoWorker(const VideoWorker&) = delete;
    VideoWorker& operator=(const VideoWorker&) = delete;

    // --- Hilo principal ---
    bool prepared() const;
    bool failed(std::string* error = nullptr) const;
    bool endOfStream() const;   // el hilo llego al final (sin bucle) y no producira mas
    StreamInfo info() const;    // valida desde prepared()
    // Longitud de la seccion (start..end) en s; < 0 si aun no se sabe (directo).
    double segmentLength() const;
    double startTime() const;   // s del video donde empieza la seccion (la real, desde prepared())

    // Busca a `video_seconds` (s del video). Devuelve la linea de tiempo nueva.
    double seek(double video_seconds);
    std::uint64_t epoch() const;

    void setLoop(bool loop);
    void setSkipOnDrop(bool skip);
    void setClockHint(double timeline);   // para que el hilo salte lo que ya llega tarde
    void setStarving(bool starving);      // la cola de video esta vacia y hace falta
    void setMasterAttached(int track, bool attached);  // esa pista tiene voz maestra (el anillo frena)

    // Saca de la cola los frames con timeline <= t (del epoch actual): el
    // ultimo en `out` y cuantos se saltaron en `skipped`. only_one: solo el
    // siguiente (sin saltar). false si no habia ninguno.
    bool takeUpTo(double t, QueuedFrame& out, int& skipped, bool only_one);
    std::size_t queued() const;
    double queuedSeconds() const;  // de video en la cola
    // Frames descartados por el hilo (llegaban tarde) desde la ultima llamada.
    std::uint64_t takeWorkerDropped();

    // Anillos de audio creados hasta ahora (aparecen al conocerse su formato).
    std::vector<AudioTrackStream> streams() const;
    std::uint64_t streamsVersion() const;

    // --- Parar ---
    void requestStop();   // no bloquea: marca y despierta (interrupt)
    // Espera hasta `deadline`; true si el hilo termino (join), false si se solto.
    bool finish(std::chrono::steady_clock::time_point deadline);

    struct Shared;

private:
    std::shared_ptr<Shared> shared_;
    std::thread thread_;
    bool finished_ = false;
};

}  // namespace cramion::video::detail

#endif  // CRAMION_VIDEO_WORKER_H
