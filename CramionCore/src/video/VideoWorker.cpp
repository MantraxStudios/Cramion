// El hilo de decodificacion de un reproductor (VideoWorker.h).

#include "VideoWorker.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <limits>
#include <utility>

namespace cramion::video::detail {

namespace {
constexpr int kMaxTracks = 32;
constexpr double kAudioLowSeconds = 0.15;  // con menos audio en el anillo, la cola de video puede crecer
}  // namespace

struct VideoWorker::Shared {
    WorkerConfig cfg;
    mutable std::mutex m;
    std::condition_variable cv;       // el hilo espera sitio u ordenes
    std::condition_variable done_cv;  // el principal espera a que termine
    std::atomic<bool> stop{false};

    // Protegido por m.
    std::shared_ptr<IVideoDecoder> decoder;
    bool prepared = false;
    bool failed = false;
    bool eos = false;
    bool done = false;
    std::string error;
    StreamInfo info;
    double segment = -1.0;
    double frame_duration = 1.0 / 30.0;
    std::uint64_t epoch = 0;
    double seek_target = 0.0;  // s del video
    double start = 0.0;        // inicio real de la seccion (Run lo corrige al abrir)
    std::deque<QueuedFrame> queue;
    std::vector<AudioTrackStream> streams;
    std::uint64_t streams_version = 0;

    std::atomic<bool> loop{false};
    std::atomic<bool> skip{true};
    std::atomic<bool> starving{false};
    std::atomic<double> clock_hint{0.0};
    std::array<std::atomic<bool>, kMaxTracks> master{};
    std::atomic<std::uint64_t> worker_dropped{0};

    bool masterAttached(int track) const {
        return track >= 0 && track < kMaxTracks && master[static_cast<std::size_t>(track)].load(std::memory_order_relaxed);
    }

    // Con m tomado: alguna pista con voz maestra se esta quedando sin audio.
    bool audioLow() const {
        for (const AudioTrackStream& s : streams) {
            if (!s.stream || !masterAttached(s.track)) continue;
            const double capacity = std::max(cfg.audio_seconds, 0.01) * s.stream->rate();
            const double buffered = capacity - static_cast<double>(s.stream->freeFrames());
            if (buffered < kAudioLowSeconds * s.stream->rate()) return true;
        }
        return false;
    }
};

namespace {

using Shared = VideoWorker::Shared;

class Run {
public:
    explicit Run(std::shared_ptr<Shared> shared) : s_(std::move(shared)), cfg_(s_->cfg) {}

    void operator()() {
        try {
            body();
        } catch (const std::exception& e) {
            fail(std::string("Video: ") + e.what());
        } catch (...) {
            fail("Video: error en el hilo de decodificacion");
        }
        {
            std::lock_guard<std::mutex> lock(s_->m);
            s_->decoder.reset();
        }
        dec_.reset();  // el decodificador se destruye en su hilo (COM/MF)
        {
            std::lock_guard<std::mutex> lock(s_->m);
            s_->done = true;
            if (!s_->prepared && !s_->failed && !s_->stop) s_->eos = true;
        }
        s_->done_cv.notify_all();
        s_->cv.notify_all();
    }

private:
    std::shared_ptr<Shared> s_;
    const WorkerConfig& cfg_;
    std::shared_ptr<IVideoDecoder> dec_;
    StreamInfo info_;
    double start_ = 0.0;
    double cut_ = std::numeric_limits<double>::infinity();  // s del video donde acaba la seccion
    double length_ = -1.0;                                  // longitud de la seccion
    double fd_ = 1.0 / 30.0;
    std::uint64_t epoch_ = 0;
    double discard_before_ = 0.0;
    int lap_ = 0;
    bool epoch_has_frame_ = false;
    bool eos_ = false;
    double last_video_end_ = 0.0;
    double last_audio_end_ = 0.0;

    void fail(const std::string& message) {
        std::lock_guard<std::mutex> lock(s_->m);
        if (s_->stop) return;
        s_->failed = true;
        s_->error = message;
    }

    void body() {
        std::unique_ptr<IVideoDecoder> created = createDecoder(cfg_.uri, cfg_.options);
        if (!created) {
            std::string why;
            decoderAvailable(cfg_.uri, &why);
            fail(why.empty() ? std::string("Video: no hay decodificador para ") + cfg_.uri : why);
            return;
        }
        dec_ = std::shared_ptr<IVideoDecoder>(std::move(created));
        {
            std::lock_guard<std::mutex> lock(s_->m);
            if (s_->stop) return;
            // epoch_ sigue en 0: una busqueda pedida antes de abrir se hace al abrir.
            s_->decoder = dec_;
        }
        std::string error;
        const bool opened = dec_->open(cfg_.uri, cfg_.options, error);
        if (s_->stop) return;
        if (!opened) {
            fail(error.empty() ? std::string("Video: no se pudo abrir ") + cfg_.uri : error);
            return;
        }
        info_ = dec_->info();
        fd_ = info_.fps > 0.0 ? 1.0 / info_.fps : 1.0 / 30.0;
        start_ = std::max(0.0, cfg_.start_time);
        if (info_.duration > 0.0 && start_ >= info_.duration) start_ = 0.0;
        double seg_end = -1.0;
        if (cfg_.end_time > start_) {
            cut_ = cfg_.end_time;
            seg_end = info_.duration > 0.0 ? std::min(cfg_.end_time, info_.duration) : cfg_.end_time;
        } else if (info_.duration > 0.0) {
            seg_end = info_.duration;
        }
        if (seg_end > start_) length_ = seg_end - start_;
        discard_before_ = start_;
        last_video_end_ = last_audio_end_ = start_;
        {
            std::lock_guard<std::mutex> lock(s_->m);
            s_->info = info_;
            s_->frame_duration = fd_;
            s_->segment = length_;
            s_->start = start_;  // start_time mas alla del final: 0
            // Sin pista de video (solo audio): preparado al abrir.
            if (info_.width == 0 || info_.height == 0) s_->prepared = true;
        }
        if (start_ > 0.0 && info_.seekable) dec_->seek(start_);

        while (!s_->stop) {
            bool seek_now = false;
            double target = 0.0;
            {
                std::unique_lock<std::mutex> lock(s_->m);
                if (s_->epoch != epoch_) {
                    epoch_ = s_->epoch;
                    target = s_->seek_target;
                    seek_now = true;
                } else if (eos_) {
                    // Al final (sin bucle): espera una busqueda o parar.
                    s_->cv.wait_for(lock, std::chrono::milliseconds(50),
                                    [&] { return s_->stop.load() || s_->epoch != epoch_; });
                    continue;
                }
            }
            if (seek_now) {
                if (info_.seekable) dec_->seek(target);
                discard_before_ = target;
                lap_ = 0;
                eos_ = false;
                epoch_has_frame_ = false;
                last_video_end_ = last_audio_end_ = target;
                continue;
            }
            VideoFrame frame;
            AudioChunk chunk;
            error.clear();
            const DecodeEvent ev = dec_->next(frame, chunk, error);
            if (s_->stop) break;
            switch (ev) {
                case DecodeEvent::Video: onVideo(std::move(frame)); break;
                case DecodeEvent::Audio: onAudio(chunk); break;
                case DecodeEvent::NeedMore: std::this_thread::yield(); break;
                case DecodeEvent::EndOfStream: reachedEnd(); break;
                case DecodeEvent::Error:
                    fail(error.empty() ? std::string("Video: error al decodificar") : error);
                    return;
            }
        }
    }

    // Fin de la seccion (o del archivo): otra vuelta o se queda al final.
    void reachedEnd() {
        if (length_ <= 0.0) {
            length_ = std::max(last_video_end_, last_audio_end_) - start_;
            std::lock_guard<std::mutex> lock(s_->m);
            s_->segment = length_ > 0.0 ? length_ : -1.0;
        }
        if (s_->loop.load() && info_.seekable && length_ > 0.0 && !info_.is_live) {
            dec_->seek(start_);
            discard_before_ = start_;
            ++lap_;
            last_video_end_ = last_audio_end_ = start_;
            return;
        }
        eos_ = true;
        std::lock_guard<std::mutex> lock(s_->m);
        if (s_->epoch == epoch_) {
            s_->eos = true;
            s_->prepared = true;  // un video sin frames tambien termina de prepararse
        }
    }

    double timeline(double pts) const { return (pts - start_) + (lap_ > 0 ? lap_ * length_ : 0.0); }

    void onVideo(VideoFrame&& frame) {
        const double pts = frame.pts;
        if (pts + 0.5 * fd_ < discard_before_) return;  // antes del objetivo de la busqueda
        if (pts >= cut_ - 1e-6) {
            reachedEnd();
            return;
        }
        last_video_end_ = std::max(last_video_end_, pts + fd_);
        const double tl = timeline(pts);
        // Llega tarde (el reloj ya paso): ni se encola.
        if (s_->skip.load() && epoch_has_frame_ && tl < s_->clock_hint.load() - 1.5 * fd_) {
            s_->worker_dropped.fetch_add(1);
            return;
        }
        // Lo que ocupa un frame decodificado (4K NV12 = 12,4 MB; RGBA = 33 MB).
        const std::size_t frame_bytes = std::max<std::size_t>(
            1, static_cast<std::size_t>(frame.width) * frame.height * (frame.layout == PixelLayout::Nv12 ? 3 : 8) / 2);
        std::unique_lock<std::mutex> lock(s_->m);
        for (;;) {
            if (s_->stop || s_->epoch != epoch_) return;
            std::size_t cap = static_cast<std::size_t>(std::max(cfg_.max_queued, 1));
            // Si el audio maestro se queda sin datos (el contenedor los trae
            // mas adelante), la cola puede crecer un poco: si no, el reloj de
            // audio y la cola llena se esperarian el uno al otro. Con un tope
            // de memoria (a 4K, 2*8+8 frames serian 300 MB); si no basta, el
            // reloj pasa al tiempo de juego (audio parado 0,2 s) y la cola se vacia.
            if (s_->audioLow()) {
                constexpr std::size_t kExtraBytes = 64u << 20;
                cap = std::min(cap * 2 + 8, cap + std::max<std::size_t>(2, kExtraBytes / frame_bytes));
            }
            if (s_->queue.size() < cap) break;
            s_->cv.wait_for(lock, std::chrono::milliseconds(5));
        }
        QueuedFrame q;
        q.frame = std::move(frame);
        q.timeline = tl;
        q.epoch = epoch_;
        s_->queue.push_back(std::move(q));
        s_->prepared = true;
        epoch_has_frame_ = true;
    }

    // base: linea de tiempo de la primera muestra que se va a escribir.
    // nullptr si llego una busqueda mientras se decodificaba: el bloque es del
    // epoch viejo y un anillo creado ahora quedaria con esa base (seek() del
    // principal solo reinicia los anillos que ya existian).
    std::shared_ptr<audio::AudioStream> streamFor(int track, std::uint32_t rate, std::uint32_t channels, double base) {
        std::lock_guard<std::mutex> lock(s_->m);
        if (s_->epoch != epoch_) return nullptr;
        for (AudioTrackStream& s : s_->streams) {
            if (s.track != track) continue;
            if (s.stream && s.stream->rate() == rate && s.stream->channels() == channels) return s.stream;
            // Otro formato (otra variante HLS): anillo nuevo; el principal reengancha las voces.
            s.stream = std::make_shared<audio::AudioStream>(rate, channels, std::max(cfg_.audio_seconds, 0.25));
            s.stream->paused.store(true);
            s.stream->reset(base);  // donde va el audio ahora, no el ultimo objetivo de busqueda
            ++s_->streams_version;
            return s.stream;
        }
        AudioTrackStream s;
        s.track = track;
        s.stream = std::make_shared<audio::AudioStream>(rate, channels, std::max(cfg_.audio_seconds, 0.25));
        s.stream->paused.store(true);  // suena cuando el principal lo diga (Playing)
        s.stream->reset(base);
        s_->streams.push_back(s);
        ++s_->streams_version;
        return s.stream;
    }

    void onAudio(const AudioChunk& chunk) {
        if (chunk.samples.empty() || chunk.track < 0 || chunk.track >= kMaxTracks) return;
        std::uint32_t rate = chunk.rate, channels = chunk.channels;
        if (static_cast<std::size_t>(chunk.track) < info_.audio.size()) {
            if (rate == 0) rate = info_.audio[static_cast<std::size_t>(chunk.track)].rate;
            if (channels == 0) channels = info_.audio[static_cast<std::size_t>(chunk.track)].channels;
        }
        if (rate == 0) rate = 48000;
        if (channels == 0) channels = 2;
        const std::size_t frames = chunk.samples.size() / channels;
        if (frames == 0) return;
        const double t0 = chunk.pts;
        std::size_t from = 0, to = frames;
        if (t0 < discard_before_) {
            from = std::min(frames, static_cast<std::size_t>(std::llround((discard_before_ - t0) * rate)));
        }
        if (std::isfinite(cut_) && t0 + static_cast<double>(frames) / rate > cut_) {
            const double keep = (cut_ - t0) * rate;
            to = keep <= 0.0 ? 0 : std::min(frames, static_cast<std::size_t>(std::llround(keep)));
        }
        if (from >= to) return;
        last_audio_end_ = std::max(last_audio_end_, t0 + static_cast<double>(to) / rate);
        const std::shared_ptr<audio::AudioStream> stream =
            streamFor(chunk.track, rate, channels, timeline(t0 + static_cast<double>(from) / rate));
        if (!stream) return;
        const float* p = chunk.samples.data() + from * channels;
        std::size_t left = to - from;
        while (left > 0 && !s_->stop) {
            std::size_t n = 0;
            {
                // Con m: una busqueda (reset del anillo) no se cruza con esta escritura.
                std::unique_lock<std::mutex> lock(s_->m);
                if (s_->epoch != epoch_) return;
                n = stream->write(p, left);
                p += n * channels;
                left -= n;
                if (left == 0) break;
                // Nadie marca el paso (sin voz maestra) o el video se ha quedado
                // sin frames: lo que no cabe se pierde (nunca se bloquea).
                if (!s_->masterAttached(chunk.track) || s_->starving.load()) return;
                s_->cv.wait_for(lock, std::chrono::milliseconds(5));
            }
        }
    }
};

}  // namespace

VideoWorker::VideoWorker(WorkerConfig config) : shared_(std::make_shared<Shared>()) {
    shared_->cfg = std::move(config);
    shared_->loop.store(shared_->cfg.loop);
    shared_->skip.store(shared_->cfg.skip_on_drop);
    shared_->start = std::max(0.0, shared_->cfg.start_time);
    thread_ = std::thread(Run(shared_));
}

VideoWorker::~VideoWorker() {
    if (!finished_) {
        requestStop();
        finish(std::chrono::steady_clock::now() + std::chrono::milliseconds(200));
    }
}

bool VideoWorker::prepared() const {
    std::lock_guard<std::mutex> lock(shared_->m);
    return shared_->prepared;
}

bool VideoWorker::failed(std::string* error) const {
    std::lock_guard<std::mutex> lock(shared_->m);
    if (shared_->failed && error != nullptr) *error = shared_->error;
    return shared_->failed;
}

bool VideoWorker::endOfStream() const {
    std::lock_guard<std::mutex> lock(shared_->m);
    return shared_->eos;
}

StreamInfo VideoWorker::info() const {
    std::lock_guard<std::mutex> lock(shared_->m);
    return shared_->info;
}

double VideoWorker::segmentLength() const {
    std::lock_guard<std::mutex> lock(shared_->m);
    return shared_->segment;
}

double VideoWorker::seek(double video_seconds) {
    double timeline = 0.0;
    {
        std::lock_guard<std::mutex> lock(shared_->m);
        Shared& s = *shared_;
        double target = std::max(video_seconds, s.start);
        if (s.segment > 0.0) target = std::min(target, s.start + std::max(0.0, s.segment - s.frame_duration * 0.5));
        ++s.epoch;
        s.seek_target = target;
        s.queue.clear();
        s.eos = false;
        timeline = target - s.start;
        for (AudioTrackStream& t : s.streams) {
            if (t.stream) t.stream->reset(timeline);
        }
    }
    shared_->cv.notify_all();
    return timeline;
}

double VideoWorker::startTime() const {
    std::lock_guard<std::mutex> lock(shared_->m);
    return shared_->start;
}

std::uint64_t VideoWorker::epoch() const {
    std::lock_guard<std::mutex> lock(shared_->m);
    return shared_->epoch;
}

void VideoWorker::setLoop(bool loop) { shared_->loop.store(loop); }
void VideoWorker::setSkipOnDrop(bool skip) { shared_->skip.store(skip); }
void VideoWorker::setClockHint(double timeline) { shared_->clock_hint.store(timeline); }

void VideoWorker::setStarving(bool starving) {
    if (shared_->starving.exchange(starving) != starving && starving) shared_->cv.notify_all();
}

void VideoWorker::setMasterAttached(int track, bool attached) {
    if (track < 0 || track >= kMaxTracks) return;
    shared_->master[static_cast<std::size_t>(track)].store(attached);
    if (!attached) shared_->cv.notify_all();
}

bool VideoWorker::takeUpTo(double t, QueuedFrame& out, int& skipped, bool only_one) {
    skipped = 0;
    bool got = false;
    {
        std::lock_guard<std::mutex> lock(shared_->m);
        std::deque<QueuedFrame>& q = shared_->queue;
        while (!q.empty() && q.front().epoch == shared_->epoch && q.front().timeline <= t) {
            if (got) ++skipped;
            out = std::move(q.front());
            q.pop_front();
            got = true;
            if (only_one) break;
        }
        // De otro epoch (no deberia quedar ninguno: seek vacia la cola).
        while (!q.empty() && q.front().epoch != shared_->epoch) q.pop_front();
    }
    if (got) shared_->cv.notify_all();
    return got;
}

std::size_t VideoWorker::queued() const {
    std::lock_guard<std::mutex> lock(shared_->m);
    return shared_->queue.size();
}

double VideoWorker::queuedSeconds() const {
    std::lock_guard<std::mutex> lock(shared_->m);
    if (shared_->queue.empty()) return 0.0;
    return shared_->queue.back().timeline - shared_->queue.front().timeline + shared_->frame_duration;
}

std::uint64_t VideoWorker::takeWorkerDropped() { return shared_->worker_dropped.exchange(0); }

std::vector<AudioTrackStream> VideoWorker::streams() const {
    std::lock_guard<std::mutex> lock(shared_->m);
    return shared_->streams;
}

std::uint64_t VideoWorker::streamsVersion() const {
    std::lock_guard<std::mutex> lock(shared_->m);
    return shared_->streams_version;
}

void VideoWorker::requestStop() {
    shared_->stop.store(true);
    {
        std::lock_guard<std::mutex> lock(shared_->m);
        // interrupt() no bloquea; el hilo nunca tiene m mientras decodifica.
        if (shared_->decoder) shared_->decoder->interrupt();
    }
    shared_->cv.notify_all();
}

bool VideoWorker::finish(std::chrono::steady_clock::time_point deadline) {
    if (finished_) return true;
    finished_ = true;
    if (!thread_.joinable()) return true;
    bool done = false;
    {
        std::unique_lock<std::mutex> lock(shared_->m);
        done = shared_->done_cv.wait_until(lock, deadline, [&] { return shared_->done; });
    }
    if (done) {
        thread_.join();
        return true;
    }
    // No contesta (red, driver): se suelta. Solo toca su Shared (shared_ptr).
    thread_.detach();
    return false;
}

}  // namespace cramion::video::detail
