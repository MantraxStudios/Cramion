// Decodificador opcional con FFmpeg (Fase 2): RTSP, RTMP y DASH (.mpd) en
// todas las plataformas, HLS fuera de Windows y, en Linux (sin Media
// Foundation ni MediaCodec), tambien los archivos y https://.
//
// Solo se compila con la opcion de CMake CRAMION_WITH_FFMPEG (apagada por
// defecto: FFmpeg es LGPL/GPL segun como se compile y hay que repartir sus
// bibliotecas con el juego). Sin ella, CRAMION_HAS_FFMPEG no esta definido y
// este archivo queda vacio.
//
// - Decodificacion por software (los hilos de FFmpeg); salida NV12 (el compute
//   de la GPU convierte) y audio en float con el formato nativo de cada pista.
// - Red: protocol_whitelist por esquema (una lista DASH/HLS de https no puede
//   llevar a file:// ni a http:// de otro sitio) y tls_verify. interrupt()
//   corta lo que este bloqueado con el interrupt_callback de FFmpeg.
// - Necesita FFmpeg 5.1 o mas nuevo (AVChannelLayout).

#if defined(CRAMION_HAS_FFMPEG)

#include "DecoderInternal.h"
#include "FramePool.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/dict.h>
#include <libavutil/pixdesc.h>
#include <libavutil/samplefmt.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(59, 37, 100)
#error "Cramion: el backend de video FFmpeg necesita FFmpeg 5.1 o mas nuevo (CRAMION_WITH_FFMPEG)"
#endif

namespace cramion::video::detail {

namespace {

std::string avError(int code) {
    char text[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, text, sizeof(text));
    return text;
}

std::string lowerAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool startsWith(const std::string& text, const char* prefix) { return text.rfind(prefix, 0) == 0; }

// Los protocolos que FFmpeg puede abrir para este URI (tambien los que abra
// por dentro: segmentos de DASH/HLS, RTP de RTSP...).
std::string protocolWhitelist(const std::string& uri) {
    const std::string l = lowerAscii(uri.substr(0, 8));
    if (startsWith(l, "rtsps://")) return "tls,tcp,udp,rtp,srtp";
    if (startsWith(l, "rtsp://")) return "tcp,udp,rtp,srtp";
    if (startsWith(l, "rtmps://")) return "rtmps,rtmp,tls,tcp";
    if (startsWith(l, "rtmp://")) return "rtmp,tcp";
    if (startsWith(l, "https://")) return "https,tls,tcp,crypto,data";
    // http:// solo llega aqui hacia este PC (net::checkUrl).
    if (startsWith(l, "http://")) return "http,tcp,crypto,data";
    return "file,crypto,data";
}

std::string codecLabel(AVCodecID id) {
    switch (id) {
        case AV_CODEC_ID_H264: return "H.264";
        case AV_CODEC_ID_HEVC: return "H.265";
        case AV_CODEC_ID_VP8: return "VP8";
        case AV_CODEC_ID_VP9: return "VP9";
        case AV_CODEC_ID_AV1: return "AV1";
        case AV_CODEC_ID_MPEG4: return "MPEG-4";
        case AV_CODEC_ID_MPEG2VIDEO: return "MPEG-2";
        default: return avcodec_get_name(id);
    }
}

void initOnce() {
    static std::once_flag once;
    std::call_once(once, [] {
        av_log_set_level(AV_LOG_ERROR);
        avformat_network_init();
    });
}

int matrixOf(AVColorSpace space, int height) {
    switch (space) {
        case AVCOL_SPC_BT709:
        case AVCOL_SPC_BT2020_NCL:
        case AVCOL_SPC_BT2020_CL: return 709;  // 2020: lo mas parecido que convierte la GPU
        case AVCOL_SPC_BT470BG:
        case AVCOL_SPC_SMPTE170M:
        case AVCOL_SPC_SMPTE240M: return 601;
        default: return height >= 720 ? 709 : 601;
    }
}

bool isYuvj(AVPixelFormat format) {
    return format == AV_PIX_FMT_YUVJ420P || format == AV_PIX_FMT_YUVJ422P || format == AV_PIX_FMT_YUVJ444P ||
           format == AV_PIX_FMT_YUVJ440P;
}

struct Slot {
    int stream = -1;
    int track = -1;  // -1 = video; si no, pista de audio (orden de StreamInfo::audio)
    AVCodecContext* ctx = nullptr;
    bool eof = false;
};

struct Ready {
    DecodeEvent kind = DecodeEvent::Video;
    VideoFrame frame;
    AudioChunk chunk;
};

class FfmpegDecoder final : public IVideoDecoder {
public:
    FfmpegDecoder() = default;

    ~FfmpegDecoder() override {
        for (Slot& s : slots_) avcodec_free_context(&s.ctx);
        slots_.clear();
        if (ctx_ != nullptr) avformat_close_input(&ctx_);
        if (sws_ != nullptr) sws_freeContext(sws_);
        av_frame_free(&frame_);
        av_packet_free(&packet_);
    }

    bool open(const std::string& uri, const DecodeOptions& options, std::string& error) override {
        initOnce();
        options_ = options;
        frame_ = av_frame_alloc();
        packet_ = av_packet_alloc();
        ctx_ = avformat_alloc_context();
        if (frame_ == nullptr || packet_ == nullptr || ctx_ == nullptr) {
            error = "FFmpeg: sin memoria";
            return false;
        }
        ctx_->interrupt_callback.callback = &FfmpegDecoder::onInterrupt;
        ctx_->interrupt_callback.opaque = this;

        const bool remote = isNetworkUri(uri) || isStreamingUri(uri);
        AVDictionary* opts = nullptr;
        av_dict_set(&opts, "protocol_whitelist", protocolWhitelist(uri).c_str(), 0);
        if (remote) {
            av_dict_set(&opts, "rw_timeout", "15000000", 0);  // us: una red muerta no deja el hilo colgado
            av_dict_set(&opts, "tls_verify", "1", 0);
        }
        if (options.low_latency) av_dict_set(&opts, "fflags", "nobuffer", 0);
        // avformat_open_input libera el contexto si falla (y lo deja a nullptr).
        int rc = avformat_open_input(&ctx_, uri.c_str(), nullptr, &opts);
        av_dict_free(&opts);
        if (interrupted_.load()) {
            error = "Apertura interrumpida";
            return false;
        }
        if (rc < 0) {
            error = "FFmpeg no pudo abrir el video: " + avError(rc);
            return false;
        }
        rc = avformat_find_stream_info(ctx_, nullptr);
        if (interrupted_.load()) {
            error = "Apertura interrumpida";
            return false;
        }
        if (rc < 0) {
            error = "FFmpeg no reconoce el formato del video: " + avError(rc);
            return false;
        }

        // Pistas: el mejor video y todas las de audio.
        StreamInfo si;
        si.backend = "FFmpeg";
        const int video_stream = av_find_best_stream(ctx_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (video_stream < 0) {
            error = "El video no tiene pista de video";
            return false;
        }
        std::vector<int> audio_streams;
        for (unsigned i = 0; i < ctx_->nb_streams; ++i) {
            AVStream* s = ctx_->streams[i];
            s->discard = AVDISCARD_ALL;  // lo elegido se activa abajo
            if (s->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) continue;
            AudioTrackInfo t;
            t.channels = static_cast<std::uint32_t>(std::max(s->codecpar->ch_layout.nb_channels, 0));
            t.rate = static_cast<std::uint32_t>(std::max(s->codecpar->sample_rate, 0));
            if (const AVDictionaryEntry* e = av_dict_get(s->metadata, "language", nullptr, 0)) t.language = e->value;
            if (t.language == "und") t.language.clear();
            if (const AVDictionaryEntry* e = av_dict_get(s->metadata, "title", nullptr, 0)) t.name = e->value;
            if (t.name.empty()) t.name = "Pista " + std::to_string(si.audio.size() + 1);
            si.audio.push_back(t);
            audio_streams.push_back(static_cast<int>(i));
        }

        AVStream* vs = ctx_->streams[video_stream];
        Slot video;
        video.stream = video_stream;
        if (!openCodec(vs, video, error)) return false;
        vs->discard = AVDISCARD_DEFAULT;
        slots_.push_back(video);
        if (!options.probe_only) {
            for (const int t : options.audio_tracks) {
                if (t < 0 || t >= static_cast<int>(audio_streams.size())) continue;
                if (std::any_of(slots_.begin(), slots_.end(), [&](const Slot& s) { return s.track == t; })) continue;
                Slot s;
                s.stream = audio_streams[static_cast<std::size_t>(t)];
                s.track = t;
                std::string why;
                if (!openCodec(ctx_->streams[s.stream], s, why)) {
                    std::cerr << "[Video] La pista de audio " << (t + 1) << " no se puede decodificar (" << why << "); se omite\n";
                    continue;
                }
                ctx_->streams[s.stream]->discard = AVDISCARD_DEFAULT;
                slots_.push_back(s);
            }
        }

        si.width = static_cast<std::uint32_t>(std::max(vs->codecpar->width, 0));
        si.height = static_cast<std::uint32_t>(std::max(vs->codecpar->height, 0));
        const AVRational rate = av_guess_frame_rate(ctx_, vs, nullptr);
        if (rate.num > 0 && rate.den > 0) si.fps = av_q2d(rate);
        si.codec = codecLabel(vs->codecpar->codec_id);
        si.hardware = false;
        si.color.matrix = matrixOf(vs->codecpar->color_space, vs->codecpar->height);
        si.color.full_range = vs->codecpar->color_range == AVCOL_RANGE_JPEG;
        const bool streaming = isStreamingUri(uri);  // rtsp/rtmp: siempre en directo
        if (!streaming && ctx_->duration != AV_NOPTS_VALUE && ctx_->duration > 0) {
            si.duration = static_cast<double>(ctx_->duration) / AV_TIME_BASE;
        }
        si.is_live = streaming || si.duration <= 0.0;
        si.seekable = !si.is_live;
        start_time_ = ctx_->start_time != AV_NOPTS_VALUE ? static_cast<double>(ctx_->start_time) / AV_TIME_BASE : 0.0;
        frame_step_ = si.fps > 0.0 ? 1.0 / si.fps : 1.0 / 30.0;
        {
            std::lock_guard<std::mutex> lock(info_mutex_);
            info_ = si;
        }
        return true;
    }

    StreamInfo info() const override {
        std::lock_guard<std::mutex> lock(info_mutex_);
        return info_;
    }

    DecodeEvent next(VideoFrame& video, AudioChunk& audio, std::string& error) override {
        if (ctx_ == nullptr || slots_.empty()) {
            error = "El decodificador no esta abierto";
            return DecodeEvent::Error;
        }
        using clock = std::chrono::steady_clock;
        const auto deadline = clock::now() + std::chrono::milliseconds(100);
        for (;;) {
            if (interrupted_.load()) return DecodeEvent::NeedMore;
            if (!ready_.empty()) {
                Ready r = std::move(ready_.front());
                ready_.pop_front();
                if (r.kind == DecodeEvent::Video) video = std::move(r.frame);
                else audio = std::move(r.chunk);
                return r.kind;
            }
            // 1. Lo que los decodificadores ya tienen.
            receiveAll();
            if (!error_.empty()) {
                error = error_;
                return DecodeEvent::Error;
            }
            if (!ready_.empty()) continue;
            if (std::all_of(slots_.begin(), slots_.end(), [](const Slot& s) { return s.eof; })) return DecodeEvent::EndOfStream;
            if (clock::now() >= deadline) return DecodeEvent::NeedMore;
            if (input_eof_) continue;  // vaciando: cada decodificador acaba en AVERROR_EOF

            // 2. Un paquete mas (bloquea en la red; interrupt() lo corta).
            const int rc = av_read_frame(ctx_, packet_);
            if (interrupted_.load()) {
                av_packet_unref(packet_);
                return DecodeEvent::NeedMore;
            }
            if (rc == AVERROR(EAGAIN)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            if (rc == AVERROR_EOF) {
                // Fin: cada decodificador suelta lo que le queda.
                input_eof_ = true;
                for (Slot& s : slots_) avcodec_send_packet(s.ctx, nullptr);
                continue;
            }
            if (rc < 0) {
                error = "FFmpeg: no se pudo leer el video (" + avError(rc) + ")";
                return DecodeEvent::Error;
            }
            for (Slot& s : slots_) {
                if (s.stream != packet_->stream_index || s.eof) continue;
                const int sent = avcodec_send_packet(s.ctx, packet_);
                // Un paquete roto (red con perdidas en RTSP) se salta.
                if (sent < 0 && sent != AVERROR(EAGAIN) && sent != AVERROR_INVALIDDATA && !warned_packet_) {
                    std::cerr << "[Video] FFmpeg rechazo un paquete (" << avError(sent) << "); se salta\n";
                    warned_packet_ = true;
                }
                break;
            }
            av_packet_unref(packet_);
        }
    }

    bool seek(double seconds) override {
        if (ctx_ == nullptr) return false;
        {
            std::lock_guard<std::mutex> lock(info_mutex_);
            if (!info_.seekable) return false;
        }
        if (interrupted_.load()) return false;
        std::int64_t target = static_cast<std::int64_t>(std::llround(std::max(0.0, seconds) * AV_TIME_BASE));
        if (ctx_->start_time != AV_NOPTS_VALUE) target += ctx_->start_time;
        const int rc = av_seek_frame(ctx_, -1, target, AVSEEK_FLAG_BACKWARD);
        for (Slot& s : slots_) {
            avcodec_flush_buffers(s.ctx);
            s.eof = false;
        }
        input_eof_ = false;
        ready_.clear();
        return rc >= 0;
    }

    void interrupt() override { interrupted_.store(true); }

private:
    static int onInterrupt(void* opaque) { return static_cast<FfmpegDecoder*>(opaque)->interrupted_.load() ? 1 : 0; }

    bool openCodec(AVStream* stream, Slot& slot, std::string& error) {
        const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
        if (codec == nullptr) {
            error = std::string("FFmpeg no tiene decodificador de ") + codecLabel(stream->codecpar->codec_id);
            return false;
        }
        slot.ctx = avcodec_alloc_context3(codec);
        if (slot.ctx == nullptr) {
            error = "FFmpeg: sin memoria";
            return false;
        }
        int rc = avcodec_parameters_to_context(slot.ctx, stream->codecpar);
        slot.ctx->pkt_timebase = stream->time_base;
        slot.ctx->thread_count = 0;  // los que FFmpeg vea (por frames y por cortes)
        if (options_.low_latency) slot.ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
        if (rc >= 0) rc = avcodec_open2(slot.ctx, codec, nullptr);
        if (rc < 0) {
            avcodec_free_context(&slot.ctx);
            error = "FFmpeg no pudo abrir el decodificador de " + codecLabel(stream->codecpar->codec_id) + ": " + avError(rc);
            return false;
        }
        return true;
    }

    double secondsOf(const AVFrame* f, const AVStream* stream, double fallback) const {
        std::int64_t ts = f->best_effort_timestamp;
        if (ts == AV_NOPTS_VALUE) ts = f->pts;
        if (ts == AV_NOPTS_VALUE) return fallback;
        return static_cast<double>(ts) * av_q2d(stream->time_base) - start_time_;
    }

    void receiveAll() {
        for (Slot& s : slots_) {
            while (!s.eof) {
                const int rc = avcodec_receive_frame(s.ctx, frame_);
                if (rc == AVERROR(EAGAIN)) break;
                if (rc == AVERROR_EOF) {
                    s.eof = true;
                    break;
                }
                if (rc < 0) {
                    error_ = "FFmpeg no pudo decodificar el video: " + avError(rc);
                    return;
                }
                Ready r;
                if (s.track < 0) {
                    r.kind = DecodeEvent::Video;
                    if (convertVideo(frame_, ctx_->streams[s.stream], r.frame)) {
                        ready_.push_back(std::move(r));
                    } else if (!warned_frame_) {
                        std::cerr << "[Video] FFmpeg dio un frame que no se pudo convertir; se salta\n";
                        warned_frame_ = true;
                    }
                } else {
                    r.kind = DecodeEvent::Audio;
                    if (convertAudio(frame_, s, r.chunk)) ready_.push_back(std::move(r));
                }
                av_frame_unref(frame_);
            }
        }
    }

    bool convertVideo(const AVFrame* f, const AVStream* stream, VideoFrame& out) {
        const int w = f->width & ~1, h = f->height & ~1;
        if (w <= 0 || h <= 0 || f->data[0] == nullptr) return false;
        const auto format = static_cast<AVPixelFormat>(f->format);
        const std::size_t y_size = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
        auto data = pool_->get(y_size + y_size / 2);
        std::uint8_t* dst_y = data->data();
        std::uint8_t* dst_uv = dst_y + y_size;
        int matrix = matrixOf(f->colorspace, f->height);
        bool full_range = f->color_range == AVCOL_RANGE_JPEG || isYuvj(format);
        if (format == AV_PIX_FMT_NV12) {
            for (int r = 0; r < h; ++r) std::memcpy(dst_y + static_cast<std::size_t>(r) * w, f->data[0] + static_cast<std::ptrdiff_t>(r) * f->linesize[0], static_cast<std::size_t>(w));
            for (int r = 0; r < h / 2; ++r) std::memcpy(dst_uv + static_cast<std::size_t>(r) * w, f->data[1] + static_cast<std::ptrdiff_t>(r) * f->linesize[1], static_cast<std::size_t>(w));
        } else if (format == AV_PIX_FMT_YUV420P || format == AV_PIX_FMT_YUVJ420P) {
            for (int r = 0; r < h; ++r) std::memcpy(dst_y + static_cast<std::size_t>(r) * w, f->data[0] + static_cast<std::ptrdiff_t>(r) * f->linesize[0], static_cast<std::size_t>(w));
            for (int r = 0; r < h / 2; ++r) {
                const std::uint8_t* u = f->data[1] + static_cast<std::ptrdiff_t>(r) * f->linesize[1];
                const std::uint8_t* v = f->data[2] + static_cast<std::ptrdiff_t>(r) * f->linesize[2];
                std::uint8_t* o = dst_uv + static_cast<std::size_t>(r) * w;
                for (int x = 0; x < w / 2; ++x) {
                    o[x * 2] = u[x];
                    o[x * 2 + 1] = v[x];
                }
            }
        } else {
            // El resto (10 bits, 4:2:2, 4:4:4, RGB...): swscale a NV12 de 8 bits.
            sws_ = sws_getCachedContext(sws_, f->width, f->height, format, w, h, AV_PIX_FMT_NV12, SWS_BILINEAR,
                                        nullptr, nullptr, nullptr);
            if (sws_ == nullptr) return false;
            std::uint8_t* dst[4] = {dst_y, dst_uv, nullptr, nullptr};
            int dst_stride[4] = {w, w, 0, 0};
            sws_scale(sws_, f->data, f->linesize, 0, f->height, dst, dst_stride);
            // swscale pasa los YUVJ a rango limitado y el RGB con BT.601.
            const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(format);
            if (desc != nullptr && (desc->flags & AV_PIX_FMT_FLAG_RGB) != 0) {
                matrix = 601;
                full_range = false;
            } else if (isYuvj(format)) {
                full_range = false;
            }
        }
        const double pts = secondsOf(f, stream, last_video_pts_ + frame_step_);
        last_video_pts_ = pts;
        out.pts = pts;
        out.width = static_cast<std::uint32_t>(w);
        out.height = static_cast<std::uint32_t>(h);
        out.layout = PixelLayout::Nv12;
        out.color.matrix = matrix;
        out.color.full_range = full_range;
        out.planes = {dst_y, dst_uv};
        out.strides = {static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(w)};
        out.owner = std::shared_ptr<const void>(data, data->data());
        return true;
    }

    bool convertAudio(const AVFrame* f, const Slot& slot, AudioChunk& out) {
        const int channels = f->ch_layout.nb_channels;
        const int n = f->nb_samples;
        if (channels <= 0 || n <= 0 || f->sample_rate <= 0) return false;
        const auto format = static_cast<AVSampleFormat>(f->format);
        const int bytes = av_get_bytes_per_sample(format);
        if (bytes <= 0) return false;
        const bool planar = av_sample_fmt_is_planar(format) != 0;
        const AVSampleFormat packed = av_get_packed_sample_fmt(format);
        out.track = slot.track;
        out.rate = static_cast<std::uint32_t>(f->sample_rate);
        out.channels = static_cast<std::uint32_t>(channels);
        out.pts = secondsOf(f, ctx_->streams[slot.stream], 0.0);
        out.samples.resize(static_cast<std::size_t>(n) * static_cast<std::size_t>(channels));
        for (int i = 0; i < n; ++i) {
            for (int c = 0; c < channels; ++c) {
                const std::uint8_t* p = planar ? f->extended_data[c] + static_cast<std::ptrdiff_t>(i) * bytes
                                               : f->extended_data[0] + (static_cast<std::ptrdiff_t>(i) * channels + c) * bytes;
                float v = 0.0f;
                switch (packed) {
                    case AV_SAMPLE_FMT_U8: v = (static_cast<float>(*p) - 128.0f) / 128.0f; break;
                    case AV_SAMPLE_FMT_S16: {
                        std::int16_t s;
                        std::memcpy(&s, p, sizeof(s));
                        v = static_cast<float>(s) / 32768.0f;
                        break;
                    }
                    case AV_SAMPLE_FMT_S32: {
                        std::int32_t s;
                        std::memcpy(&s, p, sizeof(s));
                        v = static_cast<float>(static_cast<double>(s) / 2147483648.0);
                        break;
                    }
                    case AV_SAMPLE_FMT_S64: {
                        std::int64_t s;
                        std::memcpy(&s, p, sizeof(s));
                        v = static_cast<float>(static_cast<double>(s) / 9223372036854775808.0);
                        break;
                    }
                    case AV_SAMPLE_FMT_FLT: std::memcpy(&v, p, sizeof(v)); break;
                    case AV_SAMPLE_FMT_DBL: {
                        double d;
                        std::memcpy(&d, p, sizeof(d));
                        v = static_cast<float>(d);
                        break;
                    }
                    default: return false;
                }
                out.samples[static_cast<std::size_t>(i) * static_cast<std::size_t>(channels) + static_cast<std::size_t>(c)] = v;
            }
        }
        return true;
    }

    DecodeOptions options_;
    mutable std::mutex info_mutex_;
    StreamInfo info_;
    AVFormatContext* ctx_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVPacket* packet_ = nullptr;
    SwsContext* sws_ = nullptr;
    std::vector<Slot> slots_;  // [0] = video
    std::deque<Ready> ready_;
    std::shared_ptr<FramePool> pool_ = std::make_shared<FramePool>();
    double start_time_ = 0.0;
    double frame_step_ = 1.0 / 30.0;
    double last_video_pts_ = 0.0;
    bool input_eof_ = false;
    bool warned_frame_ = false;
    bool warned_packet_ = false;
    std::string error_;
    std::atomic<bool> interrupted_{false};
};

}  // namespace

std::unique_ptr<IVideoDecoder> makeFfmpegDecoder() { return std::make_unique<FfmpegDecoder>(); }

}  // namespace cramion::video::detail

#endif  // CRAMION_HAS_FFMPEG
