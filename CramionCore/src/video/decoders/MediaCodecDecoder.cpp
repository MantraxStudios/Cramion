// Decodificador de video de Android (Fase 2) con el NDK: AMediaExtractor
// (demultiplexor) y un AMediaCodec por pista (por hardware si el movil lo
// tiene, que es lo normal).
//
// - Video: el codec dibuja en la ventana de un AImageReader (YUV_420_888) y
//   cada imagen se copia a NV12 (el compute de la GPU convierte, como en
//   Windows). Con AImageReader el formato es el mismo en todos los moviles;
//   con un bufer de salida cada fabricante da el suyo (NV12, I420, teselados
//   de Qualcomm...).
// - Audio: salida PCM de 16 bits (o float, si el codec la da) a float, con el
//   formato nativo de cada pista (AudioChunk dice la frecuencia y los canales).
// - Archivos: AMediaExtractor_setDataSourceFd (los videos del .crpack ya estan
//   descomprimidos en los datos de la app, PlayerMain.cpp).
// - https://: AMediaDataSource (Android 9+, se busca con dlsym: el juego
//   arranca desde Android 8) sobre net::httpRequest con peticiones Range, asi
//   se aplica la politica de Http.h (solo https; http solo a este PC; nada de
//   https -> http) e interrupt() corta la red. La pila HTTP del sistema no se usa.
// - HLS (.m3u8): AMediaExtractor no lo lee; con el backend FFmpeg compilado
//   va por el (DecoderFactory.cpp). Sin el, error claro.
// - Todo en el hilo del reproductor. El aviso del AImageReader llega en su
//   propio hilo y solo despierta a next().

#if defined(__ANDROID__)

#include "DecoderInternal.h"
#include "FramePool.h"

#include "CramionCore/net/Http.h"

#include <android/native_window.h>
#include <media/NdkImage.h>
#include <media/NdkImageReader.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaFormat.h>

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

// AMediaDataSource es de Android 9 (API 28): sus funciones se buscan con
// dlsym (ndkExtra). Declarada aqui por si la cabecera del NDK la oculta con
// el nivel de API minimo del juego (26).
struct AMediaDataSource;

namespace cramion::video::detail {

namespace {

constexpr const char* kNoHls =
    "Android no lee HLS (.m3u8) con MediaCodec: usa un .mp4 por https:// o compila el motor con "
    "CRAMION_WITH_FFMPEG";
constexpr const char* kNeedsAndroid9 = "El video por https:// en Android necesita Android 9 o mas nuevo";

// --- Funciones del NDK posteriores a Android 8 (dlsym) ---------------------------

struct NdkExtra {
    using ReadAt = ssize_t (*)(void*, off64_t, void*, size_t);
    using GetSize = ssize_t (*)(void*);
    using Close = void (*)(void*);
    AMediaDataSource* (*dataSourceNew)() = nullptr;
    void (*dataSourceDelete)(AMediaDataSource*) = nullptr;
    void (*dataSourceSetUserdata)(AMediaDataSource*, void*) = nullptr;
    void (*dataSourceSetReadAt)(AMediaDataSource*, ReadAt) = nullptr;
    void (*dataSourceSetGetSize)(AMediaDataSource*, GetSize) = nullptr;
    void (*dataSourceSetClose)(AMediaDataSource*, Close) = nullptr;
    media_status_t (*extractorSetDataSourceCustom)(AMediaExtractor*, AMediaDataSource*) = nullptr;
    media_status_t (*codecGetName)(AMediaCodec*, char**) = nullptr;
    void (*codecReleaseName)(AMediaCodec*, char*) = nullptr;

    bool customSource() const {
        return dataSourceNew != nullptr && dataSourceDelete != nullptr && dataSourceSetUserdata != nullptr &&
               dataSourceSetReadAt != nullptr && dataSourceSetGetSize != nullptr && dataSourceSetClose != nullptr &&
               extractorSetDataSourceCustom != nullptr;
    }
};

const NdkExtra& ndkExtra() {
    static const NdkExtra extra = [] {
        NdkExtra e;
        // Ya va enlazada (CMake: mediandk): dlopen solo devuelve su handle.
        // No se cierra nunca.
        void* lib = dlopen("libmediandk.so", RTLD_NOW);
        if (lib == nullptr) return e;
        const auto get = [lib](const char* name, auto& fn) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(lib, name));
        };
        get("AMediaDataSource_new", e.dataSourceNew);
        get("AMediaDataSource_delete", e.dataSourceDelete);
        get("AMediaDataSource_setUserdata", e.dataSourceSetUserdata);
        get("AMediaDataSource_setReadAt", e.dataSourceSetReadAt);
        get("AMediaDataSource_setGetSize", e.dataSourceSetGetSize);
        get("AMediaDataSource_setClose", e.dataSourceSetClose);
        get("AMediaExtractor_setDataSourceCustom", e.extractorSetDataSourceCustom);
        get("AMediaCodec_getName", e.codecGetName);
        get("AMediaCodec_releaseName", e.codecReleaseName);
        return e;
    }();
    return extra;
}

// --- https:// por trozos (peticiones Range, como el byte stream de MfDecoder) ----

class HttpSource {
public:
    static constexpr std::uint64_t kUnknownLength = ~0ull;

    explicit HttpSource(std::string url) : url_(std::move(url)) {}

    // Primera peticion: longitud y si el servidor admite rangos.
    bool start(std::string& error) {
        net::HttpResponse r;
        if (!fetch(0, kChunk - 1, r, error)) return false;
        if (r.status == 206) {
            length_ = parseTotal(r);
            std::lock_guard<std::mutex> lock(mutex_);
            cache_.emplace_front(0, std::make_shared<const std::string>(std::move(r.body)));
        } else {
            // El servidor no admite Range: el archivo entero (ya cabia en un trozo).
            length_ = r.body.size();
            whole_ = std::make_shared<const std::string>(std::move(r.body));
        }
        return true;
    }

    // AMediaDataSource: bytes leidos, o -1 al final y si falla.
    ssize_t readAt(std::uint64_t position, void* out, std::size_t size) {
        if (size == 0) return 0;
        if (length_ != kUnknownLength && position >= length_) return -1;
        auto* dst = static_cast<std::uint8_t*>(out);
        std::size_t done = 0;
        while (done < size) {
            if (cancel_.load()) return -1;
            const std::uint64_t p = position + done;
            std::shared_ptr<const std::string> data;
            std::uint64_t base = 0;
            if (whole_) {
                data = whole_;
            } else {
                const std::uint64_t index = p / kChunk;
                base = index * kChunk;
                std::string error;
                data = chunk(index, error);
                if (!data) {
                    if (cancel_.load()) return -1;
                    if (done > 0) break;
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (error_.empty()) {
                        error_ = error;
                        std::cerr << "[Video] " << error << "\n";
                    }
                    return -1;
                }
            }
            const std::uint64_t offset = p - base;
            if (offset >= data->size()) break;  // fin
            const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(size - done, data->size() - offset));
            std::memcpy(dst + done, data->data() + offset, n);
            done += n;
            if (!whole_ && data->size() < kChunk && offset + n >= data->size()) break;  // ultimo trozo
        }
        return done > 0 ? static_cast<ssize_t>(done) : -1;
    }

    // -1 si no se sabe (o no cabe en ssize_t: armeabi-v7a con mas de 2 GB).
    ssize_t size() const {
        if (length_ == kUnknownLength || length_ > static_cast<std::uint64_t>(SSIZE_MAX)) return -1;
        return static_cast<ssize_t>(length_);
    }
    bool knownLength() const { return length_ != kUnknownLength; }
    void abort() { cancel_.store(true); }
    std::string error() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return error_;
    }

private:
    static constexpr std::uint64_t kChunk = 2ull << 20;  // 2 MB por peticion
    static constexpr std::size_t kMaxChunks = 16;        // 32 MB de cache como mucho

    static std::string header(const net::HttpResponse& r, const char* name) {
        for (const auto& [k, v] : r.headers) {
            if (k == name) return v;
        }
        return {};
    }

    static std::uint64_t parseTotal(const net::HttpResponse& r) {
        // "bytes 0-2097151/123456789" ("*" = desconocida).
        const std::string range = header(r, "content-range");
        const auto slash = range.rfind('/');
        if (slash == std::string::npos) return kUnknownLength;
        const std::string total = range.substr(slash + 1);
        if (total.empty() || total[0] == '*') return kUnknownLength;
        char* end = nullptr;
        const unsigned long long value = std::strtoull(total.c_str(), &end, 10);
        return end != total.c_str() ? static_cast<std::uint64_t>(value) : kUnknownLength;
    }

    bool fetch(std::uint64_t first, std::uint64_t last, net::HttpResponse& out, std::string& error) {
        net::HttpRequest request;
        request.url = url_;
        request.headers.emplace_back("Range", "bytes=" + std::to_string(first) + "-" + std::to_string(last));
        request.max_response_bytes = static_cast<std::size_t>(kChunk + (64u << 10));
        for (int attempt = 0; attempt < 3; ++attempt) {
            if (cancel_.load()) {
                error = "Interrumpido";
                return false;
            }
            out = net::httpRequest(request, &cancel_);
            // httpRequest deja el estado aunque corte el cuerpo por el maximo
            // (servidor que ignora Range): solo vale si no hubo error.
            const bool too_big = !out.error.empty() && out.error.find("maximo") != std::string::npos;
            if ((out.status == 206 || out.status == 200) && out.error.empty()) return true;
            if (out.status == 416) {  // pasado el final
                out.body.clear();
                return true;
            }
            if (too_big && first != 0) break;  // Range ignorado a mitad: no se arregla reintentando
            if (out.status >= 400 && out.status < 500) break;
            if (too_big && first == 0) {
                // Sin Range y mas grande que un trozo: se baja entero (con limite).
                request.headers.clear();
                request.max_response_bytes = 256u << 20;
                out = net::httpRequest(request, &cancel_);
                if (out.ok && out.error.empty()) return true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(150 * (attempt + 1)));
        }
        if (out.status != 0) error = "HTTP " + std::to_string(out.status) + " al leer " + url_;
        else error = out.error.empty() ? "No se pudo leer " + url_ : out.error;
        return false;
    }

    std::shared_ptr<const std::string> chunk(std::uint64_t index, std::string& error) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto it = cache_.begin(); it != cache_.end(); ++it) {
                if (it->first == index) {
                    cache_.splice(cache_.begin(), cache_, it);
                    return cache_.front().second;
                }
            }
        }
        const std::uint64_t first = index * kChunk;
        std::uint64_t last = first + kChunk - 1;
        if (length_ != kUnknownLength) {
            if (first >= length_) return std::make_shared<const std::string>();
            last = std::min(last, length_ - 1);
        }
        net::HttpResponse r;
        if (!fetch(first, last, r, error)) return nullptr;
        if (r.status == 200) {
            // Ignoro el Range a mitad: el cuerpo es el archivo entero.
            whole_ = std::make_shared<const std::string>(std::move(r.body));
            const std::string& all = *whole_;
            if (first >= all.size()) return std::make_shared<const std::string>();
            return std::make_shared<const std::string>(all.substr(first, static_cast<std::size_t>(last - first + 1)));
        }
        auto data = std::make_shared<const std::string>(std::move(r.body));
        std::lock_guard<std::mutex> lock(mutex_);
        cache_.emplace_front(index, data);
        if (cache_.size() > kMaxChunks) cache_.pop_back();
        return data;
    }

    std::string url_;
    std::uint64_t length_ = kUnknownLength;
    std::shared_ptr<const std::string> whole_;
    std::atomic<bool> cancel_{false};
    mutable std::mutex mutex_;
    std::list<std::pair<std::uint64_t, std::shared_ptr<const std::string>>> cache_;
    std::string error_;
};

// --- Codecs ---------------------------------------------------------------------

std::string codecLabel(const std::string& mime) {
    if (mime == "video/avc") return "H.264";
    if (mime == "video/hevc") return "H.265";
    if (mime == "video/x-vnd.on2.vp8") return "VP8";
    if (mime == "video/x-vnd.on2.vp9") return "VP9";
    if (mime == "video/av01") return "AV1";
    if (mime == "video/mp4v-es") return "MPEG-4";
    if (mime == "video/3gpp") return "H.263";
    return mime;
}

// Los de software de Android (DecodeMode::Software): Codec2 y, en moviles
// viejos, OMX.
std::vector<const char*> softwareNames(const std::string& mime) {
    if (mime == "video/avc") return {"c2.android.avc.decoder", "OMX.google.h264.decoder"};
    if (mime == "video/hevc") return {"c2.android.hevc.decoder", "OMX.google.hevc.decoder"};
    if (mime == "video/x-vnd.on2.vp8") return {"c2.android.vp8.decoder", "OMX.google.vp8.decoder"};
    if (mime == "video/x-vnd.on2.vp9") return {"c2.android.vp9.decoder", "OMX.google.vp9.decoder"};
    if (mime == "video/av01") return {"c2.android.av1.decoder"};
    if (mime == "video/mp4v-es") return {"c2.android.mpeg4.decoder", "OMX.google.mpeg4.decoder"};
    if (mime == "video/3gpp") return {"c2.android.h263.decoder", "OMX.google.h263.decoder"};
    return {};
}

std::string formatString(AMediaFormat* format, const char* key) {
    const char* value = nullptr;
    if (format == nullptr || !AMediaFormat_getString(format, key, &value) || value == nullptr) return {};
    return value;
}

// MediaFormat.COLOR_STANDARD_*: 1 = BT.709, 2-5 = BT.601 (PAL/NTSC), 6 = BT.2020.
void readColor(AMediaFormat* format, ColorInfo& color) {
    std::int32_t standard = 0, range = 0;
    if (AMediaFormat_getInt32(format, "color-standard", &standard)) {
        if (standard == 1 || standard == 6) color.matrix = 709;  // 2020: lo mas parecido que convierte la GPU
        else if (standard >= 2 && standard <= 5) color.matrix = 601;
    }
    if (AMediaFormat_getInt32(format, "color-range", &range)) color.full_range = range == 1;
}

struct Track {
    std::size_t index = 0;  // pista del extractor
    int audio = -1;         // pista de audio (orden de StreamInfo::audio); -1 = video
    AMediaCodec* codec = nullptr;
    bool input_eos = false;
    bool output_eos = false;
    std::uint32_t rate = 0, channels = 0;
    std::int32_t encoding = 2;  // pcm-encoding: 2 = 16 bits, 3 = 8 bits, 4 = float
};

struct Ready {
    DecodeEvent kind = DecodeEvent::Video;
    VideoFrame frame;
    AudioChunk chunk;
};

class MediaCodecDecoder final : public IVideoDecoder {
public:
    MediaCodecDecoder() = default;

    ~MediaCodecDecoder() override {
        // Sin avisos a un objeto que se va (nullptr quita el listener).
        if (reader_ != nullptr) AImageReader_setImageListener(reader_, nullptr);
        for (Track& t : tracks_) {
            if (t.codec == nullptr) continue;
            AMediaCodec_stop(t.codec);
            AMediaCodec_delete(t.codec);
        }
        tracks_.clear();
        ready_.clear();
        // El codec dibujaba en la ventana del lector: el lector despues.
        if (reader_ != nullptr) AImageReader_delete(reader_);
        if (extractor_ != nullptr) AMediaExtractor_delete(extractor_);
        // La fuente de datos despues del extractor (que la lee hasta el final).
        if (source_ != nullptr) ndkExtra().dataSourceDelete(source_);
        http_.reset();
        if (fd_ >= 0) ::close(fd_);
    }

    bool open(const std::string& uri, const DecodeOptions& options, std::string& error) override {
        options_ = options;
        if (isHlsUri(uri)) {
            error = kNoHls;
            return false;
        }
        extractor_ = AMediaExtractor_new();
        if (extractor_ == nullptr) {
            error = "Android no pudo crear el lector de video (AMediaExtractor)";
            return false;
        }
        media_status_t status = AMEDIA_OK;
        bool remote = false;
        if (isNetworkUri(uri)) {
            remote = true;
            std::string why;
            if (!net::checkUrl(uri, &why)) {
                error = why;
                return false;
            }
            const NdkExtra& x = ndkExtra();
            if (!x.customSource()) {
                error = kNeedsAndroid9;
                return false;
            }
            auto http = std::make_shared<HttpSource>(uri);
            {
                std::lock_guard<std::mutex> lock(abort_mutex_);
                http_ = http;
                if (interrupted_.load()) http->abort();
            }
            if (!http->start(error)) return false;
            source_ = x.dataSourceNew();
            if (source_ == nullptr) {
                error = "Android no pudo crear la fuente de datos del video";
                return false;
            }
            x.dataSourceSetUserdata(source_, http.get());
            x.dataSourceSetReadAt(source_, [](void* user, off64_t offset, void* buffer, size_t size) -> ssize_t {
                if (offset < 0) return -1;
                return static_cast<HttpSource*>(user)->readAt(static_cast<std::uint64_t>(offset), buffer, size);
            });
            x.dataSourceSetGetSize(source_, [](void* user) -> ssize_t { return static_cast<HttpSource*>(user)->size(); });
            x.dataSourceSetClose(source_, [](void*) {});
            status = x.extractorSetDataSourceCustom(extractor_, source_);
        } else {
            fd_ = ::open(uri.c_str(), O_RDONLY | O_CLOEXEC);
            if (fd_ < 0) {
                error = "No existe el archivo de video: " + uri;
                return false;
            }
            struct stat st {};
            if (fstat(fd_, &st) != 0 || st.st_size <= 0) {
                error = "No se pudo leer el archivo de video: " + uri;
                return false;
            }
            status = AMediaExtractor_setDataSourceFd(extractor_, fd_, 0, static_cast<off64_t>(st.st_size));
        }
        if (interrupted_.load()) {
            error = "Apertura interrumpida";
            return false;
        }
        if (status != AMEDIA_OK) {
            const std::string net_error = http_ ? http_->error() : std::string();
            error = !net_error.empty() ? net_error
                                       : "Android no reconoce el formato del video (" + std::to_string(status) + ")";
            return false;
        }

        // Pistas: el primer video y todas las de audio.
        StreamInfo si;
        si.backend = "MediaCodec";
        const std::size_t count = AMediaExtractor_getTrackCount(extractor_);
        int video_index = -1;
        AMediaFormat* video_format = nullptr;
        std::string video_mime;
        std::vector<std::size_t> audio_indices;
        std::int64_t duration_us = -1;
        for (std::size_t i = 0; i < count; ++i) {
            AMediaFormat* format = AMediaExtractor_getTrackFormat(extractor_, i);
            if (format == nullptr) continue;
            const std::string mime = formatString(format, "mime");
            std::int64_t d = 0;
            if (AMediaFormat_getInt64(format, "durationUs", &d) && d > 0) duration_us = std::max(duration_us, d);
            if (mime.rfind("video/", 0) == 0 && video_index < 0) {
                video_index = static_cast<int>(i);
                video_format = format;  // se usa para configurar el codec
                video_mime = mime;
                continue;
            }
            if (mime.rfind("audio/", 0) == 0) {
                AudioTrackInfo t;
                std::int32_t channels = 0, rate = 0;
                AMediaFormat_getInt32(format, "channel-count", &channels);
                AMediaFormat_getInt32(format, "sample-rate", &rate);
                t.channels = static_cast<std::uint32_t>(std::max(channels, 0));
                t.rate = static_cast<std::uint32_t>(std::max(rate, 0));
                t.language = formatString(format, "language");
                if (t.language == "und") t.language.clear();
                t.name = "Pista " + std::to_string(si.audio.size() + 1);
                si.audio.push_back(t);
                audio_indices.push_back(i);
            }
            AMediaFormat_delete(format);
        }
        if (video_index < 0) {
            error = "El archivo no tiene pista de video";
            return false;
        }
        struct FormatGuard {
            AMediaFormat* f;
            ~FormatGuard() { AMediaFormat_delete(f); }
        } video_guard{video_format};

        std::int32_t width = 0, height = 0;
        AMediaFormat_getInt32(video_format, "width", &width);
        AMediaFormat_getInt32(video_format, "height", &height);
        if (width <= 0 || height <= 0) {
            error = "El video no dice su tamano (" + codecLabel(video_mime) + ")";
            return false;
        }
        std::int32_t fps_i = 0;
        float fps_f = 0.0f;
        if (AMediaFormat_getInt32(video_format, "frame-rate", &fps_i) && fps_i > 0) si.fps = fps_i;
        else if (AMediaFormat_getFloat(video_format, "frame-rate", &fps_f) && fps_f > 0.0f) si.fps = fps_f;
        si.width = static_cast<std::uint32_t>(width);
        si.height = static_cast<std::uint32_t>(height);
        si.codec = codecLabel(video_mime);
        color_.matrix = height >= 720 ? 709 : 601;
        color_.full_range = false;
        readColor(video_format, color_);
        si.color = color_;

        // Video: codec -> ventana del AImageReader.
        AMediaCodec* codec = createCodec(video_mime, options.mode);
        if (codec == nullptr) {
            error = "Este movil no tiene decodificador de " + codecLabel(video_mime);
            return false;
        }
        Track video;
        video.index = static_cast<std::size_t>(video_index);
        video.codec = codec;
        tracks_.push_back(video);  // el destructor lo suelta aunque falle lo de abajo
        si.hardware = isHardware(codec, options.mode);
        if (AImageReader_new(width, height, AIMAGE_FORMAT_YUV_420_888, kMaxImages, &reader_) != AMEDIA_OK ||
            reader_ == nullptr) {
            error = "Android no pudo crear el lector de imagenes del video (AImageReader " + std::to_string(width) + "x" +
                    std::to_string(height) + ")";
            return false;
        }
        AImageReader_ImageListener listener{this, &MediaCodecDecoder::onImageAvailable};
        AImageReader_setImageListener(reader_, &listener);
        ANativeWindow* window = nullptr;
        if (AImageReader_getWindow(reader_, &window) != AMEDIA_OK || window == nullptr) {
            error = "Android no pudo crear la superficie del video";
            return false;
        }
        if (options.low_latency) AMediaFormat_setInt32(video_format, "low-latency", 1);
        if (AMediaCodec_configure(codec, video_format, window, nullptr, 0) != AMEDIA_OK ||
            AMediaCodec_start(codec) != AMEDIA_OK) {
            error = "No se pudo preparar el decodificador de " + codecLabel(video_mime) + " (" +
                    std::to_string(width) + "x" + std::to_string(height) + "; puede que el movil no llegue a ese tamano)";
            return false;
        }
        AMediaExtractor_selectTrack(extractor_, video.index);

        if (!options.probe_only) {
            for (const int t : options.audio_tracks) {
                if (t < 0 || t >= static_cast<int>(audio_indices.size())) continue;
                if (std::any_of(tracks_.begin(), tracks_.end(), [&](const Track& k) { return k.audio == t; })) continue;
                if (!openAudio(audio_indices[static_cast<std::size_t>(t)], t)) {
                    std::cerr << "[Video] La pista de audio " << (t + 1) << " no se puede decodificar; se omite\n";
                }
            }
        }

        if (duration_us > 0) si.duration = static_cast<double>(duration_us) / 1e6;
        // Por red solo se busca si se sabe la longitud (Range).
        si.seekable = si.duration > 0.0 && (!remote || (http_ && http_->knownLength()));
        si.is_live = false;
        last_image_ = std::chrono::steady_clock::now();
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
        if (extractor_ == nullptr || tracks_.empty()) {
            error = "El decodificador no esta abierto";
            return DecodeEvent::Error;
        }
        using clock = std::chrono::steady_clock;
        // Sin nada que entregar en este tiempo: NeedMore (el hilo mira si debe
        // parar y marca Buffering).
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
            if (!error_.empty()) {
                error = error_;
                return DecodeEvent::Error;
            }
            bool progress = acquireImages();
            progress = drainOutputs() || progress;
            if (ready_.size() < kMaxReady) progress = feedInput() || progress;
            if (!ready_.empty() || !error_.empty()) continue;
            if (finished()) return DecodeEvent::EndOfStream;
            if (clock::now() >= deadline) return DecodeEvent::NeedMore;
            if (!progress) {
                // Esperando al codec o a la imagen del AImageReader (o a interrupt()).
                std::unique_lock<std::mutex> lock(wake_mutex_);
                wake_.wait_for(lock, std::chrono::milliseconds(2), [&] { return image_signal_ || interrupted_.load(); });
                image_signal_ = false;
            }
        }
    }

    bool seek(double seconds) override {
        if (extractor_ == nullptr || tracks_.empty()) return false;
        {
            std::lock_guard<std::mutex> lock(info_mutex_);
            if (!info_.seekable) return false;
        }
        if (interrupted_.load()) return false;  // parando: sin buscar
        for (Track& t : tracks_) {
            AMediaCodec_flush(t.codec);
            t.input_eos = false;
            t.output_eos = false;
        }
        extractor_eos_ = false;
        const auto us = static_cast<std::int64_t>(std::llround(std::max(0.0, seconds) * 1e6));
        const media_status_t status = AMediaExtractor_seekTo(extractor_, us, AMEDIAEXTRACTOR_SEEK_PREVIOUS_SYNC);
        ready_.clear();
        // Lo que el AImageReader ya tenia es de antes de buscar.
        for (;;) {
            AImage* image = nullptr;
            if (AImageReader_acquireNextImage(reader_, &image) != AMEDIA_OK || image == nullptr) break;
            AImage_delete(image);
        }
        rendered_pts_.clear();
        rendered_since_image_ = 0;
        last_image_ = std::chrono::steady_clock::now();
        return status == AMEDIA_OK;
    }

    void interrupt() override {
        {
            std::lock_guard<std::mutex> lock(abort_mutex_);
            interrupted_.store(true);
            if (http_) http_->abort();
        }
        {
            std::lock_guard<std::mutex> lock(wake_mutex_);
            image_signal_ = true;
        }
        wake_.notify_all();
    }

private:
    // Imagenes del AImageReader: las que el codec ya dibujo y aun no se leyeron
    // (con mas, el codec espera: su salida se queda en el codec).
    static constexpr std::int32_t kMaxImages = 5;
    static constexpr std::size_t kMaxPending = 3;
    static constexpr std::size_t kMaxReady = 4;

    static void onImageAvailable(void* context, AImageReader*) {
        auto* self = static_cast<MediaCodecDecoder*>(context);
        {
            std::lock_guard<std::mutex> lock(self->wake_mutex_);
            self->image_signal_ = true;
        }
        self->wake_.notify_all();
    }

    static AMediaCodec* createCodec(const std::string& mime, DecodeMode mode) {
        if (mode == DecodeMode::Software) {
            for (const char* name : softwareNames(mime)) {
                if (AMediaCodec* codec = AMediaCodec_createCodecByName(name)) return codec;
            }
        }
        return AMediaCodec_createDecoderByType(mime.c_str());
    }

    static bool isHardware(AMediaCodec* codec, DecodeMode mode) {
        bool hardware = mode != DecodeMode::Software;
        const NdkExtra& x = ndkExtra();
        if (x.codecGetName == nullptr) return hardware;  // Android 8: no se sabe el nombre
        char* name = nullptr;
        if (x.codecGetName(codec, &name) == AMEDIA_OK && name != nullptr) {
            const std::string n = name;
            hardware = !(n.rfind("OMX.google.", 0) == 0 || n.rfind("c2.android.", 0) == 0 || n.find(".sw.") != std::string::npos);
            if (x.codecReleaseName != nullptr) x.codecReleaseName(codec, name);
        }
        return hardware;
    }

    bool openAudio(std::size_t index, int track) {
        AMediaFormat* format = AMediaExtractor_getTrackFormat(extractor_, index);
        if (format == nullptr) return false;
        const std::string mime = formatString(format, "mime");
        Track t;
        t.index = index;
        t.audio = track;
        std::int32_t channels = 0, rate = 0;
        AMediaFormat_getInt32(format, "channel-count", &channels);
        AMediaFormat_getInt32(format, "sample-rate", &rate);
        t.channels = static_cast<std::uint32_t>(std::max(channels, 0));
        t.rate = static_cast<std::uint32_t>(std::max(rate, 0));
        t.codec = mime.empty() ? nullptr : AMediaCodec_createDecoderByType(mime.c_str());
        bool ok = t.codec != nullptr && AMediaCodec_configure(t.codec, format, nullptr, nullptr, 0) == AMEDIA_OK &&
                  AMediaCodec_start(t.codec) == AMEDIA_OK;
        AMediaFormat_delete(format);
        if (!ok) {
            if (t.codec != nullptr) AMediaCodec_delete(t.codec);
            return false;
        }
        AMediaExtractor_selectTrack(extractor_, index);
        tracks_.push_back(t);
        return true;
    }

    Track* trackFor(int extractor_index) {
        for (Track& t : tracks_) {
            if (static_cast<int>(t.index) == extractor_index) return &t;
        }
        return nullptr;
    }

    // Muestras del extractor a la entrada de su codec (en el orden del archivo).
    bool feedInput() {
        bool fed = false;
        for (int n = 0; n < 8; ++n) {
            if (extractor_eos_) {
                // Fin del archivo: cada codec recibe su fin para soltar lo que le queda.
                for (Track& t : tracks_) {
                    if (t.input_eos) continue;
                    const ssize_t i = AMediaCodec_dequeueInputBuffer(t.codec, 0);
                    if (i < 0) continue;
                    AMediaCodec_queueInputBuffer(t.codec, static_cast<std::size_t>(i), 0, 0, 0,
                                                 AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
                    t.input_eos = true;
                    fed = true;
                }
                return fed;
            }
            const int index = AMediaExtractor_getSampleTrackIndex(extractor_);
            if (index < 0) {
                if (interrupted_.load()) return fed;
                if (!markReadError()) extractor_eos_ = true;
                fed = true;
                continue;
            }
            Track* t = trackFor(index);
            if (t == nullptr || t->input_eos) {
                AMediaExtractor_advance(extractor_);
                fed = true;
                continue;
            }
            const ssize_t i = AMediaCodec_dequeueInputBuffer(t->codec, 0);
            if (i < 0) return fed;  // entrada llena: primero hay que vaciar su salida
            std::size_t capacity = 0;
            std::uint8_t* buffer = AMediaCodec_getInputBuffer(t->codec, static_cast<std::size_t>(i), &capacity);
            const ssize_t size = buffer != nullptr ? AMediaExtractor_readSampleData(extractor_, buffer, capacity) : -1;
            if (size < 0) {
                if (interrupted_.load()) {
                    AMediaCodec_queueInputBuffer(t->codec, static_cast<std::size_t>(i), 0, 0, 0, 0);
                    return fed;
                }
                // Sin mas datos (o fallo la red): fin de todas las pistas.
                AMediaCodec_queueInputBuffer(t->codec, static_cast<std::size_t>(i), 0, 0, 0,
                                             AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
                t->input_eos = true;
                if (!markReadError()) extractor_eos_ = true;
                fed = true;
                continue;
            }
            const std::int64_t time = AMediaExtractor_getSampleTime(extractor_);
            AMediaCodec_queueInputBuffer(t->codec, static_cast<std::size_t>(i), 0, static_cast<std::size_t>(size),
                                         static_cast<std::uint64_t>(std::max<std::int64_t>(time, 0)), 0);
            AMediaExtractor_advance(extractor_);
            fed = true;
        }
        return fed;
    }

    // Un fallo de la red no es el final del video: error claro. true si lo hubo.
    bool markReadError() {
        if (!http_) return false;
        const std::string e = http_->error();
        if (e.empty()) return false;
        error_ = e;
        return true;
    }

    // Salidas de los codecs: el video se dibuja en el AImageReader; el audio,
    // a float.
    bool drainOutputs() {
        bool any = false;
        for (Track& t : tracks_) {
            for (int guard = 0; guard < 16 && !t.output_eos; ++guard) {
                if (t.audio < 0 && rendered_pts_.size() >= kMaxPending) break;
                AMediaCodecBufferInfo info{};
                const ssize_t i = AMediaCodec_dequeueOutputBuffer(t.codec, &info, 0);
                if (i == AMEDIACODEC_INFO_TRY_AGAIN_LATER) break;
                if (i == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
                    readOutputFormat(t);
                    any = true;
                    continue;
                }
                if (i == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) continue;
                if (i < 0) break;  // otro aviso o un error del codec: se reintenta luego
                any = true;
                const bool eos = (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0;
                if (t.audio < 0) {
                    const bool render = info.size > 0 || !eos;
                    if (render) {
                        rendered_pts_.push_back(info.presentationTimeUs);
                        ++rendered_since_image_;
                    }
                    AMediaCodec_releaseOutputBuffer(t.codec, static_cast<std::size_t>(i), render);
                    if (eos) {
                        t.output_eos = true;
                        video_eos_at_ = std::chrono::steady_clock::now();
                    }
                } else {
                    if (info.size > 0 && info.offset >= 0) {
                        std::size_t capacity = 0;
                        const std::uint8_t* data = AMediaCodec_getOutputBuffer(t.codec, static_cast<std::size_t>(i), &capacity);
                        const auto end = static_cast<std::size_t>(info.offset) + static_cast<std::size_t>(info.size);
                        if (data != nullptr && end <= capacity) {
                            pushAudio(t, data + info.offset, static_cast<std::size_t>(info.size), info.presentationTimeUs);
                        }
                    }
                    AMediaCodec_releaseOutputBuffer(t.codec, static_cast<std::size_t>(i), false);
                    if (eos) t.output_eos = true;
                }
            }
        }
        return any;
    }

    void readOutputFormat(Track& t) {
        AMediaFormat* format = AMediaCodec_getOutputFormat(t.codec);
        if (format == nullptr) return;
        if (t.audio >= 0) {
            std::int32_t value = 0;
            if (AMediaFormat_getInt32(format, "sample-rate", &value) && value > 0) t.rate = static_cast<std::uint32_t>(value);
            if (AMediaFormat_getInt32(format, "channel-count", &value) && value > 0) t.channels = static_cast<std::uint32_t>(value);
            if (AMediaFormat_getInt32(format, "pcm-encoding", &value)) t.encoding = value;
        } else {
            readColor(format, color_);
        }
        AMediaFormat_delete(format);
    }

    void pushAudio(const Track& t, const std::uint8_t* data, std::size_t size, std::int64_t pts_us) {
        if (t.rate == 0 || t.channels == 0) return;
        Ready r;
        r.kind = DecodeEvent::Audio;
        AudioChunk& c = r.chunk;
        c.track = t.audio;
        c.pts = static_cast<double>(pts_us) / 1e6;
        c.rate = t.rate;
        c.channels = t.channels;
        if (t.encoding == 4) {  // float
            const std::size_t n = (size / sizeof(float)) / t.channels * t.channels;
            c.samples.resize(n);
            if (n > 0) std::memcpy(c.samples.data(), data, n * sizeof(float));
        } else if (t.encoding == 2 || t.encoding == 0) {  // 16 bits (lo normal)
            const std::size_t n = (size / 2) / t.channels * t.channels;
            c.samples.resize(n);
            for (std::size_t k = 0; k < n; ++k) {
                std::int16_t s;
                std::memcpy(&s, data + k * 2, 2);
                c.samples[k] = static_cast<float>(s) / 32768.0f;
            }
        } else if (t.encoding == 3) {  // 8 bits sin signo
            const std::size_t n = size / t.channels * t.channels;
            c.samples.resize(n);
            for (std::size_t k = 0; k < n; ++k) c.samples[k] = (static_cast<float>(data[k]) - 128.0f) / 128.0f;
        } else {
            if (!warned_audio_) {
                std::cerr << "[Video] Formato de audio de Android no soportado (pcm-encoding " << t.encoding << "); se omite\n";
                warned_audio_ = true;
            }
            return;
        }
        if (c.samples.empty()) return;
        ready_.push_back(std::move(r));
    }

    // El pts de una imagen: el que se dio al dibujarla (el AImageReader la
    // marca con presentationTimeUs * 1000). Una que no cuadra con ninguno es
    // de antes de una busqueda y se tira. Si el movil marca las imagenes con
    // otro reloj (nunca cuadra), se toman en orden.
    bool matchPts(std::int64_t image_us, std::int64_t& pts_us) {
        for (auto it = rendered_pts_.begin(); it != rendered_pts_.end(); ++it) {
            if (std::llabs(*it - image_us) <= 1) {
                pts_us = *it;
                rendered_pts_.erase(rendered_pts_.begin(), it + 1);  // las de antes no llegaron
                pts_trusted_ = true;
                return true;
            }
        }
        if (pts_trusted_ || rendered_pts_.empty()) return false;
        pts_us = rendered_pts_.front();
        rendered_pts_.pop_front();
        return true;
    }

    bool acquireImages() {
        using clock = std::chrono::steady_clock;
        bool any = false;
        for (;;) {
            AImage* image = nullptr;
            if (AImageReader_acquireNextImage(reader_, &image) != AMEDIA_OK || image == nullptr) break;
            any = true;
            std::int64_t timestamp = 0;
            AImage_getTimestamp(image, &timestamp);
            std::int64_t pts_us = 0;
            if (matchPts(timestamp / 1000, pts_us)) {
                Ready r;
                r.kind = DecodeEvent::Video;
                if (convertImage(image, pts_us, r.frame)) {
                    ready_.push_back(std::move(r));
                } else if (!warned_frame_) {
                    std::cerr << "[Video] Una imagen del decodificador de Android no se pudo leer; se salta\n";
                    warned_frame_ = true;
                }
            }
            AImage_delete(image);
            last_image_ = clock::now();
            rendered_since_image_ = 0;
        }
        // El codec dibuja y no llega ninguna imagen: este movil no da YUV_420_888
        // al AImageReader con este decodificador.
        if (!any && rendered_since_image_ >= static_cast<int>(kMaxPending) &&
            clock::now() - last_image_ > std::chrono::seconds(3)) {
            error_ = "El decodificador de video de este movil no entrega las imagenes (AImageReader YUV_420_888)";
        }
        return any;
    }

    bool finished() const {
        for (const Track& t : tracks_) {
            if (!t.output_eos) return false;
        }
        // Las ultimas imagenes del AImageReader llegan un poco despues del fin.
        return rendered_pts_.empty() ||
               std::chrono::steady_clock::now() - video_eos_at_ > std::chrono::milliseconds(500);
    }

    // YUV_420_888 (planos con su paso de fila y de pixel) -> NV12 recortado.
    bool convertImage(AImage* image, std::int64_t pts_us, VideoFrame& frame) {
        std::int32_t format = 0, w = 0, h = 0;
        if (AImage_getFormat(image, &format) != AMEDIA_OK || format != AIMAGE_FORMAT_YUV_420_888) return false;
        AImage_getWidth(image, &w);
        AImage_getHeight(image, &h);
        if (w <= 0 || h <= 0) return false;
        AImageCropRect crop{0, 0, w, h};
        if (AImage_getCropRect(image, &crop) != AMEDIA_OK || crop.right <= crop.left || crop.bottom <= crop.top) {
            crop = AImageCropRect{0, 0, w, h};
        }
        const std::int32_t left = std::clamp(crop.left, 0, w) & ~1;
        const std::int32_t top = std::clamp(crop.top, 0, h) & ~1;
        const std::int32_t cw = (std::clamp(crop.right, 0, w) - left) & ~1;
        const std::int32_t ch = (std::clamp(crop.bottom, 0, h) - top) & ~1;
        if (cw <= 0 || ch <= 0) return false;

        std::uint8_t* plane[3] = {};
        int length[3] = {};
        std::int32_t row[3] = {}, pixel[3] = {};
        for (int p = 0; p < 3; ++p) {
            if (AImage_getPlaneData(image, p, &plane[p], &length[p]) != AMEDIA_OK || plane[p] == nullptr ||
                AImage_getPlaneRowStride(image, p, &row[p]) != AMEDIA_OK || row[p] <= 0) {
                return false;
            }
            pixel[p] = 1;
            if (p > 0 && (AImage_getPlanePixelStride(image, p, &pixel[p]) != AMEDIA_OK || pixel[p] <= 0)) return false;
        }
        // Que todo lo que se lee este dentro de cada plano.
        const auto last_y = static_cast<std::int64_t>(top + ch - 1) * row[0] + left + cw;
        if (last_y > length[0]) return false;
        const std::int32_t cx = left / 2, cy = top / 2, cw2 = cw / 2, ch2 = ch / 2;
        for (int p = 1; p < 3; ++p) {
            const auto last = static_cast<std::int64_t>(cy + ch2 - 1) * row[p] + static_cast<std::int64_t>(cx + cw2 - 1) * pixel[p] + 1;
            if (last > length[p]) return false;
        }

        const std::size_t y_size = static_cast<std::size_t>(cw) * static_cast<std::size_t>(ch);
        auto data = pool_->get(y_size + y_size / 2);
        std::uint8_t* dst = data->data();
        for (std::int32_t r = 0; r < ch; ++r) {
            std::memcpy(dst + static_cast<std::size_t>(r) * cw,
                        plane[0] + static_cast<std::ptrdiff_t>(top + r) * row[0] + left, static_cast<std::size_t>(cw));
        }
        std::uint8_t* dst_uv = dst + y_size;
        const bool interleaved = pixel[1] == 2 && pixel[2] == 2 && plane[2] == plane[1] + 1 && row[1] == row[2];
        for (std::int32_t r = 0; r < ch2; ++r) {
            std::uint8_t* out = dst_uv + static_cast<std::size_t>(r) * cw;
            const std::uint8_t* u = plane[1] + static_cast<std::ptrdiff_t>(cy + r) * row[1] + static_cast<std::ptrdiff_t>(cx) * pixel[1];
            const std::uint8_t* v = plane[2] + static_cast<std::ptrdiff_t>(cy + r) * row[2] + static_cast<std::ptrdiff_t>(cx) * pixel[2];
            if (interleaved) {
                // Ya es NV12 (U y V alternos): la fila de una vez (cw - 1 bytes
                // desde U llegan al ultimo V, dentro del plano V).
                std::memcpy(out, u, static_cast<std::size_t>(cw - 1));
                out[cw - 1] = v[static_cast<std::ptrdiff_t>(cw2 - 1) * 2];
            } else {
                for (std::int32_t x = 0; x < cw2; ++x) {
                    out[x * 2] = u[static_cast<std::ptrdiff_t>(x) * pixel[1]];
                    out[x * 2 + 1] = v[static_cast<std::ptrdiff_t>(x) * pixel[2]];
                }
            }
        }
        frame.pts = static_cast<double>(pts_us) / 1e6;
        frame.width = static_cast<std::uint32_t>(cw);
        frame.height = static_cast<std::uint32_t>(ch);
        frame.layout = PixelLayout::Nv12;
        frame.color = color_;
        frame.planes = {dst, dst_uv};
        frame.strides = {static_cast<std::uint32_t>(cw), static_cast<std::uint32_t>(cw)};
        frame.owner = std::shared_ptr<const void>(data, data->data());
        return true;
    }

    DecodeOptions options_;
    mutable std::mutex info_mutex_;
    StreamInfo info_;
    AMediaExtractor* extractor_ = nullptr;
    AMediaDataSource* source_ = nullptr;
    std::shared_ptr<HttpSource> http_;  // vive mientras el extractor la lea
    int fd_ = -1;
    AImageReader* reader_ = nullptr;
    std::vector<Track> tracks_;  // [0] = video
    bool extractor_eos_ = false;
    std::deque<Ready> ready_;
    std::shared_ptr<FramePool> pool_ = std::make_shared<FramePool>();
    ColorInfo color_;
    std::deque<std::int64_t> rendered_pts_;  // us de lo dibujado que aun no llego
    bool pts_trusted_ = false;
    int rendered_since_image_ = 0;
    std::chrono::steady_clock::time_point last_image_{};
    std::chrono::steady_clock::time_point video_eos_at_{};
    std::string error_;
    bool warned_frame_ = false;
    bool warned_audio_ = false;
    std::atomic<bool> interrupted_{false};
    std::mutex abort_mutex_;
    std::mutex wake_mutex_;
    std::condition_variable wake_;
    bool image_signal_ = false;
};

}  // namespace

std::unique_ptr<IVideoDecoder> makeMediaCodecDecoder() { return std::make_unique<MediaCodecDecoder>(); }

bool mediaCodecAvailable(const std::string& uri, std::string* why) {
    if (isHlsUri(uri)) {
        if (why != nullptr) *why = kNoHls;
        return false;
    }
    if (isNetworkUri(uri) && !ndkExtra().customSource()) {
        if (why != nullptr) *why = kNeedsAndroid9;
        return false;
    }
    return true;
}

}  // namespace cramion::video::detail

#endif  // __ANDROID__
