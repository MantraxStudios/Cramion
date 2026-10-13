// HLS (.m3u8) en Windows.
//
// El plan proponia IMFMediaEngine como respaldo, pero Media Engine saca el
// audio por su propio renderer y no da el PCM: sin modos Direct/AudioSource/
// API, sin oclusion y sin reloj de audio (critica #5). En su lugar, un HLS
// ligero sobre el mismo Source Reader de MfDecoder:
//   - lee la lista maestra y elige la variante (la de mas ancho de banda que
//     cabe en la resolucion maxima);
//   - encadena los segmentos (MPEG-TS, que se pueden concatenar, o fMP4 con su
//     EXT-X-MAP) en un IMFByteStream propio que baja cada segmento con
//     net::httpRequest (politica de Http.h; interrupt() corta la descarga);
//   - en directo vuelve a leer la lista cada media duracion de segmento;
//   - seek (solo VOD): reabre el lector en el segmento que contiene ese tiempo
//     y corrige los pts para que cuenten desde el inicio del video.
// Fuera de la Fase 1: EXT-X-KEY (cifrado), EXT-X-BYTERANGE, audio en una
// lista aparte (EXT-X-MEDIA TYPE=AUDIO con URI) y cambio de calidad adaptativo.
// El nombre del archivo es el del plan; la clase es HlsDecoder.

#if defined(_WIN32)

#include "MfCommon.h"

#include "DecoderInternal.h"

#include "CramionCore/net/Http.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

namespace cramion::video::mf {

namespace {

constexpr QWORD kUnknownLength = static_cast<QWORD>(-1);

struct Segment {
    std::string uri;
    double duration = 0.0;
    double start = 0.0;
    std::uint64_t sequence = 0;
};

struct MediaPlaylist {
    std::vector<Segment> segments;
    bool ended = false;
    double target = 6.0;
    std::string map_uri;  // EXT-X-MAP (fMP4)
};

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    std::size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\xEF' || s[i] == '\xBB' || s[i] == '\xBF')) ++i;
    return s.substr(i);
}

bool startsWith(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }

// Lista de atributos HLS: CLAVE=valor,CLAVE="valor, con comas"
std::vector<std::pair<std::string, std::string>> attributes(const std::string& list) {
    std::vector<std::pair<std::string, std::string>> out;
    std::size_t i = 0;
    while (i < list.size()) {
        const auto eq = list.find('=', i);
        if (eq == std::string::npos) break;
        std::string key = trim(list.substr(i, eq - i));
        std::string value;
        std::size_t j = eq + 1;
        if (j < list.size() && list[j] == '"') {
            const auto close = list.find('"', j + 1);
            value = list.substr(j + 1, (close == std::string::npos ? list.size() : close) - j - 1);
            j = close == std::string::npos ? list.size() : close + 1;
        } else {
            const auto comma = list.find(',', j);
            value = list.substr(j, (comma == std::string::npos ? list.size() : comma) - j);
            j = comma == std::string::npos ? list.size() : comma;
        }
        out.emplace_back(std::move(key), trim(value));
        const auto comma = list.find(',', j);
        if (comma == std::string::npos) break;
        i = comma + 1;
    }
    return out;
}

std::string attribute(const std::string& list, const char* key) {
    for (const auto& [k, v] : attributes(list)) {
        if (k == key) return v;
    }
    return {};
}

std::string resolveUri(const std::string& base, const std::string& ref) {
    if (ref.find("://") != std::string::npos) return ref;
    if (detail::isNetworkUri(base)) {
        const auto scheme_end = base.find("://");
        const std::string scheme = base.substr(0, scheme_end);
        if (startsWith(ref, "//")) return scheme + ":" + ref;
        const auto host_end = base.find('/', scheme_end + 3);
        const std::string origin = host_end == std::string::npos ? base : base.substr(0, host_end);
        if (startsWith(ref, "/")) return origin + ref;
        std::string path = base;
        if (const auto cut = path.find_first_of("?#"); cut != std::string::npos) path.resize(cut);
        const auto slash = path.rfind('/');
        return (slash == std::string::npos || slash < scheme_end + 3 ? path + "/" : path.substr(0, slash + 1)) + ref;
    }
    // Archivo local: absoluta o relativa a la carpeta de la lista.
    if (ref.size() > 1 && (ref[1] == ':' || ref[0] == '/' || ref[0] == '\\')) return ref;
    const auto slash = base.find_last_of("/\\");
    return (slash == std::string::npos ? std::string() : base.substr(0, slash + 1)) + ref;
}

bool fetch(const std::string& uri, std::size_t max_bytes, std::atomic<bool>* cancel, std::string& out, std::string& error) {
    if (detail::isNetworkUri(uri)) {
        if (!net::checkUrl(uri, &error)) return false;
        net::HttpRequest request;
        request.url = uri;
        request.max_response_bytes = max_bytes;
        net::HttpResponse r;
        for (int attempt = 0; attempt < 3; ++attempt) {
            if (cancel != nullptr && cancel->load()) {
                error = "Interrumpido";
                return false;
            }
            r = net::httpRequest(request, cancel);
            if (r.ok) {
                out = std::move(r.body);
                return true;
            }
            if (r.status >= 400 && r.status < 500) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(200 * (attempt + 1)));
        }
        error = r.status != 0 ? "HTTP " + std::to_string(r.status) + " al leer " + uri
                              : (r.error.empty() ? "No se pudo leer " + uri : r.error);
        return false;
    }
    std::ifstream file(std::filesystem::path(widen(uri)), std::ios::binary);
    if (!file) {
        error = "No existe el archivo: " + uri;
        return false;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    out = buffer.str();
    if (out.size() > max_bytes) {
        error = "El archivo es demasiado grande: " + uri;
        return false;
    }
    return true;
}

bool isMaster(const std::string& text) { return text.find("#EXT-X-STREAM-INF") != std::string::npos; }

// La variante de mas ancho de banda que cabe; si ninguna cabe, la mas pequena.
std::string chooseVariant(const std::string& text, const std::string& base, std::uint32_t max_w, std::uint32_t max_h) {
    struct Variant {
        std::string uri;
        std::uint64_t bandwidth = 0;
        std::uint32_t w = 0, h = 0;
    };
    std::vector<Variant> variants;
    std::istringstream in(text);
    std::string line, pending;
    bool expect_uri = false;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty()) continue;
        if (startsWith(line, "#EXT-X-STREAM-INF:")) {
            pending = line.substr(18);
            expect_uri = true;
        } else if (line[0] != '#' && expect_uri) {
            Variant v;
            v.uri = resolveUri(base, line);
            v.bandwidth = std::strtoull(attribute(pending, "BANDWIDTH").c_str(), nullptr, 10);
            const std::string res = attribute(pending, "RESOLUTION");
            if (const auto x = res.find('x'); x != std::string::npos) {
                v.w = static_cast<std::uint32_t>(std::strtoul(res.substr(0, x).c_str(), nullptr, 10));
                v.h = static_cast<std::uint32_t>(std::strtoul(res.substr(x + 1).c_str(), nullptr, 10));
            }
            variants.push_back(std::move(v));
            expect_uri = false;
        }
    }
    if (variants.empty()) return {};
    const auto fits = [&](const Variant& v) {
        return (max_w == 0 || v.w == 0 || v.w <= max_w) && (max_h == 0 || v.h == 0 || v.h <= max_h);
    };
    const Variant* best = nullptr;
    for (const Variant& v : variants) {
        if (fits(v) && (best == nullptr || v.bandwidth > best->bandwidth)) best = &v;
    }
    if (best == nullptr) {
        for (const Variant& v : variants) {
            if (best == nullptr || static_cast<std::uint64_t>(v.w) * v.h < static_cast<std::uint64_t>(best->w) * best->h) best = &v;
        }
    }
    return best->uri;
}

bool parseMedia(const std::string& text, const std::string& base, MediaPlaylist& out, std::string& error) {
    MediaPlaylist p;
    std::istringstream in(text);
    std::string line;
    double pending_duration = -1.0;
    std::uint64_t sequence = 0;
    double start = 0.0;
    bool header = false;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty()) continue;
        if (startsWith(line, "#EXTM3U")) {
            header = true;
        } else if (startsWith(line, "#EXT-X-TARGETDURATION:")) {
            p.target = std::max(1.0, std::strtod(line.c_str() + 22, nullptr));
        } else if (startsWith(line, "#EXT-X-MEDIA-SEQUENCE:")) {
            sequence = std::strtoull(line.c_str() + 22, nullptr, 10);
        } else if (startsWith(line, "#EXTINF:")) {
            pending_duration = std::strtod(line.c_str() + 8, nullptr);
        } else if (startsWith(line, "#EXT-X-ENDLIST")) {
            p.ended = true;
        } else if (startsWith(line, "#EXT-X-MAP:")) {
            if (!attribute(line.substr(11), "BYTERANGE").empty()) {
                error = "HLS con EXT-X-MAP por rangos (BYTERANGE) no esta soportado todavia";
                return false;
            }
            p.map_uri = resolveUri(base, attribute(line.substr(11), "URI"));
        } else if (startsWith(line, "#EXT-X-KEY:")) {
            const std::string method = attribute(line.substr(11), "METHOD");
            if (!method.empty() && method != "NONE") {
                error = "HLS cifrado (EXT-X-KEY " + method + ") no esta soportado todavia";
                return false;
            }
        } else if (startsWith(line, "#EXT-X-BYTERANGE")) {
            error = "HLS con EXT-X-BYTERANGE no esta soportado todavia";
            return false;
        } else if (line[0] != '#') {
            Segment s;
            s.uri = resolveUri(base, line);
            s.duration = pending_duration > 0.0 ? pending_duration : p.target;
            s.start = start;
            s.sequence = sequence++;
            start += s.duration;
            p.segments.push_back(std::move(s));
            pending_duration = -1.0;
        }
    }
    if (!header) {
        error = "No es una lista HLS (#EXTM3U)";
        return false;
    }
    if (p.segments.empty() && p.ended) {
        error = "La lista HLS no tiene segmentos";
        return false;
    }
    out = std::move(p);
    return true;
}

// Lista de medios compartida entre el decodificador y su byte stream (en
// directo el byte stream la recarga).
struct Playlist {
    std::string uri;
    std::mutex mutex;
    MediaPlaylist media;
    bool fmp4 = false;

    // Directo: anade los segmentos nuevos (por numero de secuencia).
    bool reload(std::atomic<bool>* cancel, std::string& error) {
        std::string text;
        if (!fetch(uri, 4u << 20, cancel, text, error)) return false;
        MediaPlaylist fresh;
        if (!parseMedia(text, uri, fresh, error)) return false;
        std::lock_guard<std::mutex> lock(mutex);
        const std::uint64_t last = media.segments.empty() ? 0 : media.segments.back().sequence;
        double start = media.segments.empty() ? 0.0 : media.segments.back().start + media.segments.back().duration;
        for (Segment& s : fresh.segments) {
            if (!media.segments.empty() && s.sequence <= last) continue;
            s.start = start;
            start += s.duration;
            media.segments.push_back(std::move(s));
        }
        // Lo muy viejo no hace falta (en directo la lista crece sin fin).
        if (media.segments.size() > 512) media.segments.erase(media.segments.begin(), media.segments.end() - 256);
        media.ended = fresh.ended;
        media.target = fresh.target;
        return true;
    }
};

// Byte stream secuencial: [init fMP4] + segmentos a partir de `sequence`.
// Guarda los ultimos trozos para que el analizador pueda volver un poco atras.
class HlsByteStream final : public ByteStreamBase {
public:
    HlsByteStream(std::shared_ptr<Playlist> playlist, std::uint64_t sequence)
        : playlist_(std::move(playlist)), next_sequence_(sequence) {
        need_init_ = !playlist_->media.map_uri.empty();
    }

    bool start(std::string& error) {
        if (!initAttributes()) {
            error = "Media Foundation: no se pudo crear el almacen de atributos";
            return false;
        }
        SetString(MF_BYTESTREAM_CONTENT_TYPE, playlist_->fmp4 ? L"video/mp4" : L"video/mp2t");
        SetString(MF_BYTESTREAM_ORIGIN_NAME, playlist_->fmp4 ? L"cramion-hls.mp4" : L"cramion-hls.ts");
        return true;
    }

protected:
    HRESULT readAt(QWORD position, BYTE* pb, ULONG cb, ULONG* read) override {
        *read = 0;
        if (!pieces_.empty() && position < pieces_.front().offset) return E_INVALIDARG;  // ya descartado
        ULONG done = 0;
        while (done < cb) {
            if (cancel_.load()) return E_ABORT;
            const QWORD p = position + done;
            if (p >= end_) {
                if (finished_) break;
                std::string error;
                if (!appendNext(error)) {
                    if (cancel_.load()) return E_ABORT;
                    if (finished_) break;
                    if (done > 0) break;
                    std::cerr << "[Video] HLS: " << error << "\n";
                    return MF_E_NET_READ;
                }
                continue;
            }
            bool found = false;
            for (const Piece& piece : pieces_) {
                if (p >= piece.offset && p < piece.offset + piece.data.size()) {
                    const QWORD offset = p - piece.offset;
                    const ULONG n = static_cast<ULONG>(std::min<QWORD>(cb - done, piece.data.size() - offset));
                    std::memcpy(pb + done, piece.data.data() + offset, n);
                    done += n;
                    found = true;
                    break;
                }
            }
            if (!found) {  // ya descartado (lectura muy atras tras un segmento enorme)
                if (done > 0) break;
                return E_INVALIDARG;
            }
        }
        *read = done;
        return S_OK;
    }
    DWORD capabilities() const override { return MFBYTESTREAM_IS_READABLE | MFBYTESTREAM_IS_REMOTE; }
    QWORD length() const override { return kUnknownLength; }
    bool canSeekTo(QWORD position) const override { return pieces_.empty() || position >= pieces_.front().offset; }

private:
    struct Piece {
        QWORD offset = 0;
        std::string data;
    };

    bool appendNext(std::string& error) {
        std::string uri;
        if (need_init_) {
            uri = playlist_->media.map_uri;
        } else {
            // El siguiente segmento; en directo, esperar a que aparezca.
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            for (;;) {
                double wait = 1.0;
                {
                    std::lock_guard<std::mutex> lock(playlist_->mutex);
                    for (const Segment& s : playlist_->media.segments) {
                        if (s.sequence >= next_sequence_) {
                            uri = s.uri;
                            next_sequence_ = s.sequence + 1;
                            break;
                        }
                    }
                    if (uri.empty() && playlist_->media.ended) {
                        finished_ = true;
                        return false;
                    }
                    wait = std::clamp(playlist_->media.target * 0.5, 0.5, 5.0);
                }
                if (!uri.empty()) break;
                if (std::chrono::steady_clock::now() > deadline) {
                    error = "El directo HLS no da segmentos nuevos";
                    return false;
                }
                const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(static_cast<int>(wait * 1000));
                while (std::chrono::steady_clock::now() < until) {
                    if (cancel_.load()) return false;
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
                if (!playlist_->reload(&cancel_, error)) return false;
            }
        }
        std::string data;
        if (!fetch(uri, 128u << 20, &cancel_, data, error)) return false;
        need_init_ = false;
        Piece piece;
        piece.offset = end_;
        end_ += data.size();
        piece.data = std::move(data);
        pieces_.push_back(std::move(piece));
        while (pieces_.size() > 3) pieces_.pop_front();
        return true;
    }

    std::shared_ptr<Playlist> playlist_;
    std::uint64_t next_sequence_ = 0;
    bool need_init_ = false;
    bool finished_ = false;
    std::deque<Piece> pieces_;
    QWORD end_ = 0;
};

class HlsDecoder final : public IVideoDecoder {
public:
    ~HlsDecoder() override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stream_) stream_->abort();
        }
        inner_.reset();
        stream_.Reset();
    }

    bool open(const std::string& uri, const DecodeOptions& options, std::string& error) override {
        options_ = options;
        if (!session_.acquire(&error)) return false;
        std::string text;
        if (!fetch(uri, 4u << 20, &cancel_, text, error)) return false;
        std::string media_uri = uri;
        if (isMaster(text)) {
            media_uri = chooseVariant(text, uri, options.max_width, options.max_height);
            if (media_uri.empty()) {
                error = "La lista maestra HLS no tiene variantes";
                return false;
            }
            if (!fetch(media_uri, 4u << 20, &cancel_, text, error)) return false;
        }
        playlist_ = std::make_shared<Playlist>();
        playlist_->uri = media_uri;
        if (!parseMedia(text, media_uri, playlist_->media, error)) return false;
        const MediaPlaylist& m = playlist_->media;
        live_ = !m.ended;
        if (m.segments.empty() && live_ && !playlist_->reload(&cancel_, error)) return false;
        if (m.segments.empty()) {
            error = "La lista HLS no tiene segmentos";
            return false;
        }
        std::string first = m.segments.front().uri;
        if (const auto cut = first.find_first_of("?#"); cut != std::string::npos) first.resize(cut);
        std::transform(first.begin(), first.end(), first.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const auto ends = [&](const char* ext) {
            const std::size_t n = std::strlen(ext);
            return first.size() >= n && first.compare(first.size() - n, n, ext) == 0;
        };
        playlist_->fmp4 = !m.map_uri.empty() || ends(".m4s") || ends(".mp4") || ends(".m4v") || ends(".cmfv");
        total_ = 0.0;
        for (const Segment& s : m.segments) total_ += s.duration;
        // Directo: empezar cerca del final (3 segmentos), como los reproductores.
        const std::size_t index = live_ && m.segments.size() > 3 ? m.segments.size() - 3 : 0;
        return openAt(index, error);
    }

    StreamInfo info() const override {
        StreamInfo s;
        if (inner_) s = inner_->info();
        s.backend = "MediaFoundation (HLS)";
        s.is_live = live_;
        s.seekable = !live_;
        s.duration = live_ ? -1.0 : total_;
        return s;
    }

    DecodeEvent next(VideoFrame& video, AudioChunk& audio, std::string& error) override {
        if (!inner_) {
            error = "El decodificador HLS no esta abierto";
            return DecodeEvent::Error;
        }
        const DecodeEvent e = inner_->next(video, audio, error);
        if (e == DecodeEvent::Video || e == DecodeEvent::Audio) {
            double& pts = e == DecodeEvent::Video ? video.pts : audio.pts;
            // Los pts del TS/fMP4 no tienen por que empezar en el inicio del
            // segmento: se fija el desfase con la primera muestra de la sesion.
            if (offset_pending_) {
                offset_ = session_start_ - pts;
                offset_pending_ = false;
            }
            pts += offset_;
        }
        return e;
    }

    bool seek(double seconds) override {
        if (live_ || !playlist_) return false;
        std::size_t index = 0;
        {
            std::lock_guard<std::mutex> lock(playlist_->mutex);
            const auto& segs = playlist_->media.segments;
            for (std::size_t i = 0; i < segs.size(); ++i) {
                if (segs[i].start <= seconds) index = i;
                else break;
            }
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stream_) stream_->abort();
        }
        std::unique_ptr<MfDecoder> old;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            old = std::move(inner_);
            stream_.Reset();
        }
        old.reset();
        std::string error;
        if (!openAt(index, error)) {
            std::cerr << "[Video] HLS: no se pudo saltar a " << seconds << " s: " << error << "\n";
            return false;
        }
        return true;
    }

    void interrupt() override {
        std::lock_guard<std::mutex> lock(mutex_);
        interrupted_ = true;
        cancel_.store(true);
        if (stream_) stream_->abort();
        if (inner_) inner_->interrupt();
    }

private:
    bool openAt(std::size_t index, std::string& error) {
        Segment segment;
        {
            std::lock_guard<std::mutex> lock(playlist_->mutex);
            if (index >= playlist_->media.segments.size()) {
                error = "Segmento HLS fuera de la lista";
                return false;
            }
            segment = playlist_->media.segments[index];
        }
        ComPtr<HlsByteStream> stream;
        stream.Attach(new HlsByteStream(playlist_, segment.sequence));
        if (!stream->start(error)) return false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (interrupted_) {
                error = "Apertura interrumpida";
                return false;
            }
            stream_ = stream;
        }
        auto inner = std::make_unique<MfDecoder>();
        if (!inner->openStream(stream.Get(), stream.Get(), options_, error)) return false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            inner_ = std::move(inner);
            if (interrupted_) inner_->interrupt();
        }
        session_start_ = live_ ? 0.0 : segment.start;
        offset_pending_ = true;
        return true;
    }

    Session session_;
    DecodeOptions options_;
    std::shared_ptr<Playlist> playlist_;
    bool live_ = false;
    double total_ = 0.0;
    double session_start_ = 0.0;
    double offset_ = 0.0;
    bool offset_pending_ = true;

    std::mutex mutex_;  // stream_, inner_ e interrupted_ frente a interrupt()
    ComPtr<HlsByteStream> stream_;
    std::unique_ptr<MfDecoder> inner_;
    bool interrupted_ = false;
    std::atomic<bool> cancel_{false};
};

}  // namespace

}  // namespace cramion::video::mf

namespace cramion::video::detail {

std::unique_ptr<IVideoDecoder> makeHlsDecoder() { return std::make_unique<mf::HlsDecoder>(); }

}  // namespace cramion::video::detail

#endif  // _WIN32
