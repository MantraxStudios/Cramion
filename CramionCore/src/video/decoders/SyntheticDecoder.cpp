// Decodificador sintetico: cramion://test?... (ver VideoDecoder.h).
//
// Determinista y sin codecs ni archivos: barras de color con el numero de
// frame escrito en los primeros bytes del plano 0, y un seno por pista de
// audio (440 Hz * (pista + 1)). Video y audio salen ordenados por pts, como
// los entregaria un demultiplexor real. Sirve para las pruebas (CramionVideoTests)
// en todas las plataformas, tambien en el servidor de Linux.

#include "DecoderInternal.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <limits>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace cramion::video {

namespace {

constexpr std::uint8_t kMagic[3] = {0xC7, 0x1D, 0xE0};
constexpr double kPi = 3.14159265358979323846;

struct SyntheticParams {
    std::uint32_t width = 64, height = 36;
    double fps = 30.0;
    double duration = 2.0;  // <= 0: sin fin
    int tracks = 0;
    std::uint32_t rate = 48000, channels = 2;
    PixelLayout layout = PixelLayout::Rgba8;
    int delay_ms = 0, decode_ms = 0;
    int gop = 1;
    bool live = false, fail = false, hardware = false;
    double error_at = -1.0;
    int matrix = 709;
    bool full_range = false;
    std::uint32_t block = 1024;
};

std::map<std::string, std::string> parseQuery(const std::string& uri) {
    std::map<std::string, std::string> out;
    const auto q = uri.find('?');
    if (q == std::string::npos) return out;
    std::string rest = uri.substr(q + 1);
    if (const auto hash = rest.find('#'); hash != std::string::npos) rest.resize(hash);
    std::size_t start = 0;
    while (start <= rest.size()) {
        auto end = rest.find('&', start);
        if (end == std::string::npos) end = rest.size();
        const std::string pair = rest.substr(start, end - start);
        if (!pair.empty()) {
            const auto eq = pair.find('=');
            if (eq == std::string::npos) out[pair] = "1";
            else out[pair.substr(0, eq)] = pair.substr(eq + 1);
        }
        start = end + 1;
    }
    return out;
}

double num(const std::map<std::string, std::string>& q, const char* key, double fallback) {
    const auto it = q.find(key);
    if (it == q.end() || it->second.empty()) return fallback;
    char* end = nullptr;
    const double v = std::strtod(it->second.c_str(), &end);
    return (end == it->second.c_str() || !std::isfinite(v)) ? fallback : v;
}

bool parseParams(const std::string& uri, SyntheticParams& p, std::string& error) {
    const auto q = parseQuery(uri);
    p.width = static_cast<std::uint32_t>(std::clamp(num(q, "w", 64), 8.0, 8192.0));
    p.height = static_cast<std::uint32_t>(std::clamp(num(q, "h", 36), 2.0, 8192.0));
    p.fps = std::clamp(num(q, "fps", 30), 1.0, 480.0);
    p.duration = num(q, "dur", 2.0);
    p.tracks = static_cast<int>(std::clamp(num(q, "tracks", 0), 0.0, 16.0));
    p.rate = static_cast<std::uint32_t>(std::clamp(num(q, "rate", 48000), 8000.0, 192000.0));
    p.channels = static_cast<std::uint32_t>(std::clamp(num(q, "ch", 2), 1.0, 8.0));
    p.delay_ms = static_cast<int>(std::clamp(num(q, "delay", 0), 0.0, 600000.0));
    p.decode_ms = static_cast<int>(std::clamp(num(q, "decode_ms", 0), 0.0, 60000.0));
    p.gop = static_cast<int>(std::clamp(num(q, "gop", 1), 1.0, 1000.0));
    p.live = num(q, "live", 0) != 0.0 || p.duration <= 0.0;
    p.fail = num(q, "fail", 0) != 0.0;
    p.hardware = num(q, "hw", 0) != 0.0;
    p.error_at = num(q, "error_at", -1.0);
    p.matrix = num(q, "matrix", 709) == 601.0 ? 601 : 709;
    if (const auto it = q.find("range"); it != q.end()) p.full_range = it->second == "full";
    p.block = static_cast<std::uint32_t>(std::clamp(num(q, "block", 1024), 16.0, 65536.0));
    if (const auto it = q.find("fmt"); it != q.end()) {
        if (it->second == "rgba") p.layout = PixelLayout::Rgba8;
        else if (it->second == "bgra") p.layout = PixelLayout::Bgra8;
        else if (it->second == "nv12") p.layout = PixelLayout::Nv12;
        else {
            error = "Formato sintetico desconocido: " + it->second + " (rgba, bgra o nv12)";
            return false;
        }
    }
    if (p.layout == PixelLayout::Nv12) {
        p.width = (p.width + 1u) & ~1u;
        p.height = (p.height + 1u) & ~1u;
    }
    return true;
}

// Barras SMPTE simplificadas (RGB 0..255).
constexpr std::uint8_t kBars[8][3] = {
    {235, 235, 235}, {235, 235, 16}, {16, 235, 235}, {16, 235, 16},
    {235, 16, 235},  {235, 16, 16},  {16, 16, 235},  {16, 16, 16},
};

void rgbToYuv(const std::uint8_t rgb[3], int matrix, bool full, std::uint8_t& y, std::uint8_t& u, std::uint8_t& v) {
    const double r = rgb[0] / 255.0, g = rgb[1] / 255.0, b = rgb[2] / 255.0;
    const double kr = matrix == 601 ? 0.299 : 0.2126;
    const double kb = matrix == 601 ? 0.114 : 0.0722;
    const double kg = 1.0 - kr - kb;
    const double yy = kr * r + kg * g + kb * b;
    const double pb = (b - yy) / (2.0 * (1.0 - kb));
    const double pr = (r - yy) / (2.0 * (1.0 - kr));
    const auto clamp8 = [](double x) { return static_cast<std::uint8_t>(std::clamp(std::lround(x), 0L, 255L)); };
    if (full) {
        y = clamp8(yy * 255.0);
        u = clamp8(128.0 + pb * 255.0);
        v = clamp8(128.0 + pr * 255.0);
    } else {
        y = clamp8(16.0 + yy * 219.0);
        u = clamp8(128.0 + pb * 224.0);
        v = clamp8(128.0 + pr * 224.0);
    }
}

void writeMarker(std::uint8_t* p, std::int64_t index) {
    const auto i = static_cast<std::uint32_t>(std::max<std::int64_t>(0, index));
    p[0] = static_cast<std::uint8_t>(i & 0xFF);
    p[1] = static_cast<std::uint8_t>((i >> 8) & 0xFF);
    p[2] = static_cast<std::uint8_t>((i >> 16) & 0xFF);
    p[3] = 0xFF;
    p[4] = kMagic[0];
    p[5] = kMagic[1];
    p[6] = kMagic[2];
    p[7] = 0xFF;
}

class SyntheticDecoder final : public IVideoDecoder {
public:
    bool open(const std::string& uri, const DecodeOptions& options, std::string& error) override {
        if (!parseParams(uri, params_, error)) return false;
        options_ = options;
        // Resolucion maxima: escala conservando la proporcion (como el procesador de MF).
        out_w_ = params_.width;
        out_h_ = params_.height;
        if ((options.max_width != 0 && out_w_ > options.max_width) ||
            (options.max_height != 0 && out_h_ > options.max_height)) {
            double s = 1.0;
            if (options.max_width != 0) s = std::min(s, static_cast<double>(options.max_width) / out_w_);
            if (options.max_height != 0) s = std::min(s, static_cast<double>(options.max_height) / out_h_);
            out_w_ = std::max<std::uint32_t>(8, static_cast<std::uint32_t>(out_w_ * s));
            out_h_ = std::max<std::uint32_t>(2, static_cast<std::uint32_t>(out_h_ * s));
            if (params_.layout == PixelLayout::Nv12) {
                out_w_ = (out_w_ + 1u) & ~1u;
                out_h_ = (out_h_ + 1u) & ~1u;
            }
        }
        rate_ = options.audio_rate != 0 ? options.audio_rate : params_.rate;
        channels_ = options.audio_channels != 0 ? options.audio_channels : params_.channels;

        if (params_.delay_ms > 0 && !sleepInterruptible(params_.delay_ms)) {
            error = "Apertura interrumpida";
            return false;
        }
        if (interrupted_.load()) {
            error = "Apertura interrumpida";
            return false;
        }
        if (params_.fail) {
            error = "Fallo sintetico al abrir (fail=1)";
            return false;
        }

        tracks_.clear();
        if (!options.probe_only) {
            for (const int t : options.audio_tracks) {
                if (t < 0 || t >= params_.tracks) continue;
                if (std::any_of(tracks_.begin(), tracks_.end(), [t](const Track& k) { return k.index == t; })) continue;
                tracks_.push_back({t, 0});
            }
        }
        next_frame_ = 0;
        video_ended_ = false;
        opened_ = true;
        return true;
    }

    StreamInfo info() const override {
        StreamInfo s;
        s.width = out_w_;
        s.height = out_h_;
        s.fps = params_.fps;
        s.duration = params_.live ? -1.0 : params_.duration;
        s.seekable = !params_.live;
        s.is_live = params_.live;
        s.codec = "Synthetic";
        s.backend = "Synthetic";
        s.hardware = params_.hardware;
        s.color.matrix = params_.matrix;
        s.color.full_range = params_.full_range;
        for (int t = 0; t < params_.tracks; ++t) {
            AudioTrackInfo a;
            a.name = "Pista " + std::to_string(t + 1);
            a.language = "und";
            a.channels = channels_;
            a.rate = rate_;
            s.audio.push_back(a);
        }
        return s;
    }

    DecodeEvent next(VideoFrame& video, AudioChunk& audio, std::string& error) override {
        if (!opened_) {
            error = "El decodificador no esta abierto";
            return DecodeEvent::Error;
        }
        if (interrupted_.load()) return DecodeEvent::NeedMore;
        if (params_.decode_ms > 0 && !sleepInterruptible(params_.decode_ms)) return DecodeEvent::NeedMore;

        const double end = params_.live ? std::numeric_limits<double>::infinity() : params_.duration;
        const double video_pts = static_cast<double>(next_frame_) / params_.fps;
        if (video_pts >= end) video_ended_ = true;

        // El siguiente por pts: video o la pista de audio mas atrasada.
        int best_track = -1;
        double best_pts = video_ended_ ? std::numeric_limits<double>::infinity() : video_pts;
        for (std::size_t i = 0; i < tracks_.size(); ++i) {
            const double pts = static_cast<double>(tracks_[i].sample) / rate_;
            if (pts >= end) continue;
            if (pts < best_pts) {
                best_pts = pts;
                best_track = static_cast<int>(i);
            }
        }
        if (!std::isfinite(best_pts)) return DecodeEvent::EndOfStream;
        if (params_.error_at >= 0.0 && best_pts >= params_.error_at) {
            error = "Error sintetico de decodificacion (error_at)";
            return DecodeEvent::Error;
        }

        if (best_track < 0) {
            makeFrame(video, next_frame_);
            ++next_frame_;
            return DecodeEvent::Video;
        }

        Track& tr = tracks_[static_cast<std::size_t>(best_track)];
        std::uint64_t frames = params_.block;
        if (!params_.live) {
            const auto total = static_cast<std::uint64_t>(std::llround(params_.duration * rate_));
            frames = std::min<std::uint64_t>(frames, total > tr.sample ? total - tr.sample : 0);
        }
        if (frames == 0) frames = 1;
        audio.track = tr.index;
        audio.pts = static_cast<double>(tr.sample) / rate_;
        audio.rate = rate_;
        audio.channels = channels_;
        audio.samples.resize(static_cast<std::size_t>(frames) * channels_);
        const double freq = 440.0 * (tr.index + 1);
        for (std::uint64_t f = 0; f < frames; ++f) {
            // Fase por muestra absoluta: igual tras un seek, sin acumular error.
            const double phase = std::fmod(static_cast<double>(tr.sample + f) * freq / rate_, 1.0);
            const float s = static_cast<float>(0.25 * std::sin(2.0 * kPi * phase));
            for (std::uint32_t c = 0; c < channels_; ++c) audio.samples[static_cast<std::size_t>(f) * channels_ + c] = s;
        }
        tr.sample += frames;
        return DecodeEvent::Audio;
    }

    bool seek(double seconds) override {
        if (!opened_ || params_.live) return false;
        seconds = std::clamp(seconds, 0.0, params_.duration);
        auto frame = static_cast<std::int64_t>(std::floor(seconds * params_.fps + 1e-9));
        frame -= frame % params_.gop;  // keyframe anterior
        next_frame_ = frame;
        video_ended_ = false;
        const double t = static_cast<double>(frame) / params_.fps;
        for (Track& tr : tracks_) tr.sample = static_cast<std::uint64_t>(std::llround(t * rate_));
        return true;
    }

    void interrupt() override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            interrupted_.store(true);
        }
        cv_.notify_all();
    }

private:
    struct Track {
        int index = 0;
        std::uint64_t sample = 0;
    };

    // false si interrupt() la corto.
    bool sleepInterruptible(int ms) {
        std::unique_lock<std::mutex> lock(mutex_);
        return !cv_.wait_for(lock, std::chrono::milliseconds(ms), [this] { return interrupted_.load(); });
    }

    void makeFrame(VideoFrame& f, std::int64_t index) const {
        const std::uint32_t w = out_w_, h = out_h_;
        f.pts = static_cast<double>(index) / params_.fps;
        f.width = w;
        f.height = h;
        f.layout = params_.layout;
        f.color.matrix = params_.matrix;
        f.color.full_range = params_.full_range;
        const std::uint32_t moving = static_cast<std::uint32_t>(index % w);

        if (params_.layout == PixelLayout::Nv12) {
            const std::size_t y_size = static_cast<std::size_t>(w) * h;
            auto buffer = std::make_shared<std::vector<std::uint8_t>>(y_size + y_size / 2);
            std::uint8_t* yp = buffer->data();
            std::uint8_t* uvp = yp + y_size;
            std::uint8_t bar_y[8], bar_u[8], bar_v[8], white_y, white_u, white_v;
            for (int b = 0; b < 8; ++b) rgbToYuv(kBars[b], params_.matrix, params_.full_range, bar_y[b], bar_u[b], bar_v[b]);
            const std::uint8_t white[3] = {255, 255, 255};
            rgbToYuv(white, params_.matrix, params_.full_range, white_y, white_u, white_v);
            for (std::uint32_t y = 0; y < h; ++y) {
                for (std::uint32_t x = 0; x < w; ++x) {
                    const int b = static_cast<int>(x * 8 / w);
                    yp[static_cast<std::size_t>(y) * w + x] = x == moving ? white_y : bar_y[b];
                }
            }
            for (std::uint32_t y = 0; y < h / 2; ++y) {
                for (std::uint32_t x = 0; x < w / 2; ++x) {
                    const int b = static_cast<int>(x * 2 * 8 / w);
                    const bool m = x * 2 == (moving & ~1u);
                    std::uint8_t* p = uvp + static_cast<std::size_t>(y) * w + x * 2;
                    p[0] = m ? white_u : bar_u[b];
                    p[1] = m ? white_v : bar_v[b];
                }
            }
            writeMarker(yp, index);
            f.planes = {yp, uvp};
            f.strides = {w, w};
            f.owner = std::shared_ptr<const void>(buffer, buffer.get());
            return;
        }

        const bool bgra = params_.layout == PixelLayout::Bgra8;
        auto buffer = std::make_shared<std::vector<std::uint8_t>>(static_cast<std::size_t>(w) * h * 4);
        std::uint8_t* px = buffer->data();
        for (std::uint32_t y = 0; y < h; ++y) {
            for (std::uint32_t x = 0; x < w; ++x) {
                const std::uint8_t* c = kBars[x * 8 / w];
                static constexpr std::uint8_t kWhite[3] = {255, 255, 255};
                if (x == moving) c = kWhite;
                std::uint8_t* p = px + (static_cast<std::size_t>(y) * w + x) * 4;
                p[0] = bgra ? c[2] : c[0];
                p[1] = c[1];
                p[2] = bgra ? c[0] : c[2];
                p[3] = 0xFF;
            }
        }
        writeMarker(px, index);
        f.planes = {px, nullptr};
        f.strides = {w * 4, 0};
        f.owner = std::shared_ptr<const void>(buffer, buffer.get());
    }

    SyntheticParams params_;
    DecodeOptions options_;
    std::uint32_t out_w_ = 0, out_h_ = 0;
    std::uint32_t rate_ = 48000, channels_ = 2;
    std::vector<Track> tracks_;
    std::int64_t next_frame_ = 0;
    bool video_ended_ = false;
    bool opened_ = false;
    std::atomic<bool> interrupted_{false};
    std::mutex mutex_;
    std::condition_variable cv_;
};

}  // namespace

std::int64_t syntheticFrameIndex(const std::uint8_t* p) {
    if (p == nullptr) return -1;
    if (p[3] != 0xFF || p[4] != kMagic[0] || p[5] != kMagic[1] || p[6] != kMagic[2] || p[7] != 0xFF) return -1;
    return static_cast<std::int64_t>(p[0]) | (static_cast<std::int64_t>(p[1]) << 8) |
           (static_cast<std::int64_t>(p[2]) << 16);
}

std::int64_t syntheticFrameIndex(const VideoFrame& frame) {
    const std::uint32_t bytes_row0 = frame.layout == PixelLayout::Nv12 ? frame.width : frame.width * 4;
    if (bytes_row0 < 8) return -1;
    return syntheticFrameIndex(frame.planes[0]);
}

namespace detail {

bool isSyntheticUri(const std::string& uri) { return uri.rfind("cramion://test", 0) == 0; }

std::unique_ptr<IVideoDecoder> makeSyntheticDecoder() { return std::make_unique<SyntheticDecoder>(); }

}  // namespace detail

}  // namespace cramion::video
