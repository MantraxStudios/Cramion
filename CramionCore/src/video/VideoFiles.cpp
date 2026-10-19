// Archivos y URLs de video, y la informacion + miniatura de la cache del
// editor (VideoFiles.h).

#include "CramionCore/video/VideoFiles.h"

#include "CramionCore/net/Http.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <system_error>

namespace cramion::video {

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// FNV-1a de 64 bits: nombre estable del archivo de cache.
std::uint64_t fnv1a(const std::string& text) {
    std::uint64_t h = 1469598103934665603ull;
    for (const unsigned char c : text) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

std::filesystem::path pathFromUtf8(const std::string& text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

bool fileStamp(const std::filesystem::path& file, std::uint64_t& size, std::int64_t& time) {
    std::error_code e;
    size = static_cast<std::uint64_t>(std::filesystem::file_size(file, e));
    if (e) return false;
    const auto stamp = std::filesystem::last_write_time(file, e);
    if (e) return false;
    time = static_cast<std::int64_t>(stamp.time_since_epoch().count());
    return true;
}

nlohmann::json infoToJson(const VideoInfo& info) {
    nlohmann::json j;
    j["version"] = 1;
    j["ok"] = info.ok;
    j["error"] = info.error;
    j["size"] = info.file_size;
    j["time"] = info.file_time;
    const StreamInfo& s = info.stream;
    j["width"] = s.width;
    j["height"] = s.height;
    j["fps"] = s.fps;
    j["duration"] = s.duration;
    j["seekable"] = s.seekable;
    j["live"] = s.is_live;
    j["codec"] = s.codec;
    j["backend"] = s.backend;
    j["hardware"] = s.hardware;
    j["matrix"] = s.color.matrix;
    j["full_range"] = s.color.full_range;
    nlohmann::json tracks = nlohmann::json::array();
    for (const AudioTrackInfo& t : s.audio) {
        tracks.push_back({{"name", t.name}, {"language", t.language}, {"channels", t.channels}, {"rate", t.rate}});
    }
    j["audio"] = std::move(tracks);
    j["thumbnail"] = info.has_thumbnail;
    return j;
}

bool infoFromJson(const nlohmann::json& j, VideoInfo& info) {
    if (!j.is_object() || j.value("version", 0) != 1) return false;
    info.ok = j.value("ok", false);
    info.error = j.value("error", std::string());
    info.file_size = j.value("size", std::uint64_t{0});
    info.file_time = j.value("time", std::int64_t{0});
    StreamInfo& s = info.stream;
    s.width = j.value("width", 0u);
    s.height = j.value("height", 0u);
    s.fps = j.value("fps", 0.0);
    s.duration = j.value("duration", -1.0);
    s.seekable = j.value("seekable", false);
    s.is_live = j.value("live", false);
    s.codec = j.value("codec", std::string());
    s.backend = j.value("backend", std::string());
    s.hardware = j.value("hardware", false);
    s.color.matrix = j.value("matrix", 709);
    s.color.full_range = j.value("full_range", false);
    s.audio.clear();
    if (const auto it = j.find("audio"); it != j.end() && it->is_array()) {
        for (const nlohmann::json& t : *it) {
            AudioTrackInfo a;
            a.name = t.value("name", std::string());
            a.language = t.value("language", std::string());
            a.channels = t.value("channels", 0u);
            a.rate = t.value("rate", 0u);
            s.audio.push_back(std::move(a));
        }
    }
    info.has_thumbnail = j.value("thumbnail", false);
    return true;
}

std::uint8_t clamp8(float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0f, 255.0f) + 0.5f); }

// Abre, lee la info y un frame (un poco despues del principio si se puede).
bool probe(const std::string& uri, VideoInfo& out, asset::ImageRgba8* image, std::uint32_t thumbnail_size) {
    out.ok = false;
    out.error.clear();
    out.has_thumbnail = false;
    std::string why;
    if (!decoderAvailable(uri, &why)) {
        out.error = why.empty() ? std::string("No hay decodificador de video para este archivo") : why;
        return false;
    }
    DecodeOptions options;
    options.probe_only = true;
    std::unique_ptr<IVideoDecoder> decoder = createDecoder(uri, options);
    if (!decoder) {
        out.error = "No hay decodificador de video para este archivo";
        return false;
    }
    std::string error;
    if (!decoder->open(uri, options, error)) {
        out.error = error.empty() ? std::string("No se pudo abrir el video") : error;
        return false;
    }
    out.stream = decoder->info();
    out.ok = true;
    if (image == nullptr || out.stream.width == 0 || out.stream.height == 0) return true;
    // El primer frame suele ser negro (fundido): un poco mas adelante.
    if (out.stream.seekable && out.stream.duration > 2.0) decoder->seek(std::min(1.0, out.stream.duration * 0.1));
    // Con plazo: un decodificador que devuelve NeedMore (~100 ms cada uno) no
    // deja el hilo de importacion minutos aqui.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    for (int i = 0; i < 2000 && std::chrono::steady_clock::now() < deadline; ++i) {
        VideoFrame frame;
        AudioChunk chunk;
        const DecodeEvent ev = decoder->next(frame, chunk, error);
        if (ev == DecodeEvent::Video) {
            out.has_thumbnail = frameToRgba(frame, thumbnail_size, *image);
            break;
        }
        if (ev == DecodeEvent::EndOfStream || ev == DecodeEvent::Error) break;
    }
    return true;
}

}  // namespace

bool isVideoFile(const std::filesystem::path& file) {
    static const char* const kExtensions[] = {".mp4", ".m4v", ".mov", ".webm", ".mkv", ".avi", ".wmv"};
    const std::u8string ext8 = file.extension().u8string();
    const std::string ext = lower(std::string(ext8.begin(), ext8.end()));
    for (const char* e : kExtensions) {
        if (ext == e) return true;
    }
    return false;
}

bool isVideoUrl(const std::string& text) {
    const std::size_t colon = text.find("://");
    if (colon == std::string::npos || colon == 0) return false;
    for (std::size_t i = 0; i < colon; ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (!std::isalnum(c) && c != '+' && c != '-' && c != '.') return false;
    }
    return true;
}

bool checkVideoUrl(const std::string& url, std::string* error) {
    const std::string l = lower(url);
    if (l.rfind("cramion://test", 0) == 0) return true;  // decodificador sintetico (pruebas)
    // Directos (camaras IP, servidores RTMP): solo con el backend FFmpeg. Sin
    // politica de dominio: casi siempre son de la red local.
    if (l.rfind("rtsp://", 0) == 0 || l.rfind("rtsps://", 0) == 0 || l.rfind("rtmp://", 0) == 0 ||
        l.rfind("rtmps://", 0) == 0) {
        if (!streamingProtocolsAvailable()) {
            if (error != nullptr) {
                *error = "Video: RTSP y RTMP necesitan el backend FFmpeg (opcion CRAMION_WITH_FFMPEG al compilar el motor)";
            }
            return false;
        }
        if (url.find("://") + 3 >= url.size()) {
            if (error != nullptr) *error = "Video: falta el servidor en la URL";
            return false;
        }
        return true;
    }
    if (l.rfind("https://", 0) != 0 && l.rfind("http://", 0) != 0) {
        if (error != nullptr) {
            *error = isVideoUrl(url) ? "Video: solo URL https:// (o http:// a este PC); rtsp:// y rtmp:// con el backend FFmpeg"
                                     : "Video: la URL tiene que empezar por https://";
        }
        return false;
    }
    std::string why;
    if (!net::checkUrl(url, &why)) {
        if (error != nullptr) *error = "Video: " + (why.empty() ? std::string("URL no permitida (solo https, o http a este PC)") : why);
        return false;
    }
    return true;
}

std::string pathUtf8(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

std::string resolveClip(const std::string& clip, const std::filesystem::path& assets_root, std::string* error) {
    if (clip.empty()) {
        if (error != nullptr) *error = "Video: no hay clip";
        return {};
    }
    if (isVideoUrl(clip)) {
        // Un clip "cramion://test?..." (pruebas) o una URL pegada en el campo.
        return checkVideoUrl(clip, error) ? clip : std::string();
    }
    std::filesystem::path file = pathFromUtf8(clip);
    if (file.is_relative()) {
        // Nunca fuera de Assets ("../../secreto.mp4").
        for (const auto& part : file) {
            if (part == "..") {
                if (error != nullptr) *error = "Video: el clip tiene que estar dentro de Assets (" + clip + ")";
                return {};
            }
        }
        file = assets_root / file;
    }
    std::error_code e;
    if (!std::filesystem::is_regular_file(file, e)) {
        if (error != nullptr) *error = "Video: no existe el archivo " + clip;
        return {};
    }
    const std::filesystem::path absolute = std::filesystem::absolute(file, e);
    return pathUtf8((e ? file : absolute).lexically_normal());
}

std::string resolveSource(const VideoPlayer& player, const std::filesystem::path& assets_root, std::string* error) {
    if (player.source == VideoSource::Url) {
        if (player.url.empty()) {
            if (error != nullptr) *error = "Video: no hay URL";
            return {};
        }
        return checkVideoUrl(player.url, error) ? player.url : std::string();
    }
    return resolveClip(player.clip, assets_root, error);
}

VideoCachePaths videoCachePaths(const std::filesystem::path& cache_dir, const std::filesystem::path& file) {
    std::error_code e;
    std::filesystem::path absolute = std::filesystem::absolute(file, e);
    if (e) absolute = file;
    const std::u8string generic = absolute.lexically_normal().generic_u8string();
    std::string key(generic.begin(), generic.end());
#if defined(_WIN32)
    key = lower(key);  // C:/A.mp4 y c:/a.mp4 son el mismo archivo
#endif
    char name[32];
    std::snprintf(name, sizeof(name), "%016llx", static_cast<unsigned long long>(fnv1a(key)));
    VideoCachePaths paths;
    paths.json = cache_dir / (std::string(name) + ".json");
    paths.png = cache_dir / (std::string(name) + ".png");
    return paths;
}

bool readCachedVideoInfo(const std::filesystem::path& cache_dir, const std::filesystem::path& file, VideoInfo& out) {
    if (cache_dir.empty()) return false;
    std::uint64_t size = 0;
    std::int64_t time = 0;
    if (!fileStamp(file, size, time)) return false;
    const VideoCachePaths paths = videoCachePaths(cache_dir, file);
    std::ifstream in(paths.json, std::ios::binary);
    if (!in) return false;
    std::stringstream text;
    text << in.rdbuf();
    const nlohmann::json j = nlohmann::json::parse(text.str(), nullptr, false);
    VideoInfo cached;
    if (j.is_discarded() || !infoFromJson(j, cached)) return false;
    if (cached.file_size != size || cached.file_time != time) return false;
    std::error_code e;
    if (cached.has_thumbnail && std::filesystem::is_regular_file(paths.png, e)) {
        cached.thumbnail = paths.png;
    } else {
        cached.has_thumbnail = false;
    }
    out = std::move(cached);
    return true;
}

bool probeVideo(const std::filesystem::path& file, const std::filesystem::path& cache_dir, VideoInfo& out,
                std::uint32_t thumbnail_size) {
    if (readCachedVideoInfo(cache_dir, file, out)) return out.ok;
    VideoInfo info;
    fileStamp(file, info.file_size, info.file_time);
    asset::ImageRgba8 image;
    std::error_code e;
    const std::filesystem::path absolute = std::filesystem::absolute(file, e);
    probe(pathUtf8((e ? file : absolute).lexically_normal()), info, &image, thumbnail_size);
    if (!cache_dir.empty()) {
        std::filesystem::create_directories(cache_dir, e);
        const VideoCachePaths paths = videoCachePaths(cache_dir, file);
        if (info.has_thumbnail) {
            info.has_thumbnail = asset::saveImagePng(paths.png, image);
            if (info.has_thumbnail) info.thumbnail = paths.png;
        }
        // Escritura atomica: otro hilo (o el editor) nunca lee un .json a medias.
        const std::filesystem::path temp = paths.json.string() + ".tmp";
        {
            std::ofstream outf(temp, std::ios::binary | std::ios::trunc);
            outf << infoToJson(info).dump(1);
        }
        std::filesystem::rename(temp, paths.json, e);
        if (e) std::filesystem::remove(temp, e);
    }
    out = std::move(info);
    return out.ok;
}

bool probeVideoUri(const std::string& uri, VideoInfo& out, asset::ImageRgba8* thumbnail, std::uint32_t thumbnail_size) {
    out = VideoInfo{};
    if (isVideoUrl(uri) && !checkVideoUrl(uri, &out.error)) return false;
    return probe(uri, out, thumbnail, thumbnail_size);
}

bool frameToRgba(const VideoFrame& frame, std::uint32_t max_size, asset::ImageRgba8& out) {
    const std::uint32_t w = frame.width, h = frame.height;
    if (w == 0 || h == 0 || frame.planes[0] == nullptr) return false;
    if (frame.layout == PixelLayout::Nv12 && frame.planes[1] == nullptr) return false;
    std::uint32_t ow = w, oh = h;
    if (max_size > 0 && std::max(w, h) > max_size) {
        if (w >= h) {
            ow = max_size;
            oh = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::lround(static_cast<double>(h) * max_size / w)));
        } else {
            oh = max_size;
            ow = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::lround(static_cast<double>(w) * max_size / h)));
        }
    }
    out.width = ow;
    out.height = oh;
    out.pixels.assign(static_cast<std::size_t>(ow) * oh * 4, 255);
    // YCbCr -> RGB (BT.601 o BT.709, rango limitado o completo).
    const bool bt601 = frame.color.matrix == 601;
    const float kr = bt601 ? 0.299f : 0.2126f, kb = bt601 ? 0.114f : 0.0722f;
    const float kg = 1.0f - kr - kb;
    const bool full = frame.color.full_range;
    for (std::uint32_t y = 0; y < oh; ++y) {
        const std::uint32_t sy = std::min(h - 1, static_cast<std::uint32_t>((static_cast<std::uint64_t>(y) * h + h / 2) / oh));
        for (std::uint32_t x = 0; x < ow; ++x) {
            const std::uint32_t sx = std::min(w - 1, static_cast<std::uint32_t>((static_cast<std::uint64_t>(x) * w + w / 2) / ow));
            std::uint8_t* d = out.pixels.data() + (static_cast<std::size_t>(y) * ow + x) * 4;
            if (frame.layout == PixelLayout::Nv12) {
                const std::uint8_t* yrow = frame.planes[0] + static_cast<std::size_t>(sy) * frame.strides[0];
                const std::uint8_t* uvrow = frame.planes[1] + static_cast<std::size_t>(sy / 2) * frame.strides[1];
                float Y = yrow[sx];
                float Cb = uvrow[(sx / 2) * 2];
                float Cr = uvrow[(sx / 2) * 2 + 1];
                if (full) {
                    Y /= 255.0f;
                    Cb = (Cb - 128.0f) / 255.0f;
                    Cr = (Cr - 128.0f) / 255.0f;
                } else {
                    Y = (Y - 16.0f) / 219.0f;
                    Cb = (Cb - 128.0f) / 224.0f;
                    Cr = (Cr - 128.0f) / 224.0f;
                }
                const float r = Y + 2.0f * (1.0f - kr) * Cr;
                const float b = Y + 2.0f * (1.0f - kb) * Cb;
                const float g = (Y - kr * r - kb * b) / kg;
                d[0] = clamp8(r * 255.0f);
                d[1] = clamp8(g * 255.0f);
                d[2] = clamp8(b * 255.0f);
            } else {
                const std::uint8_t* s = frame.planes[0] + static_cast<std::size_t>(sy) * frame.strides[0] + static_cast<std::size_t>(sx) * 4;
                const bool bgra = frame.layout == PixelLayout::Bgra8;
                d[0] = bgra ? s[2] : s[0];
                d[1] = s[1];
                d[2] = bgra ? s[0] : s[2];
            }
            d[3] = 255;
        }
    }
    return true;
}

}  // namespace cramion::video
