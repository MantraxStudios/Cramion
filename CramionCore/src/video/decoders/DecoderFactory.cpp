// Elige el backend de cada URI (contrato A, VideoDecoder.h):
//   cramion://test?...            SyntheticDecoder (todas las plataformas)
//   rtsp(s)://, rtmp(s)://, .mpd  FfmpegDecoder (con CRAMION_WITH_FFMPEG; sin el, error claro)
//   *.m3u8 (archivo o red)        Windows: HLS sobre el Source Reader de Media Foundation;
//                                 Android y Linux: FFmpeg (sin el, error claro)
//   archivo, https://             Windows: MfDecoder; Android: MediaCodecDecoder;
//                                 Linux: FFmpeg
//   el resto                      NullDecoder (servidor de Linux sin FFmpeg)

#include "DecoderInternal.h"

#include "CramionCore/net/Http.h"

#include <algorithm>
#include <cctype>

namespace cramion::video {

namespace detail {

namespace {
std::string lowerAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// La ruta (sin ?query ni #fragmento si es una URL) termina en `ext` (en minusculas).
bool hasExtension(const std::string& uri, const char* ext) {
    std::string path = uri;
    if (isNetworkUri(uri)) {
        if (const auto cut = path.find_first_of("?#"); cut != std::string::npos) path.resize(cut);
    }
    path = lowerAscii(path);
    const std::string e = ext;
    return path.size() >= e.size() && path.compare(path.size() - e.size(), e.size(), e) == 0;
}

[[maybe_unused]] std::string ffmpegMissingReason(const std::string& uri) {
    if (isStreamingUri(uri)) {
        return "RTSP y RTMP necesitan el backend FFmpeg (compila el motor con la opcion CRAMION_WITH_FFMPEG)";
    }
    if (isDashUri(uri)) {
        return "DASH (.mpd) necesita el backend FFmpeg (compila el motor con la opcion CRAMION_WITH_FFMPEG)";
    }
    return "Este video necesita el backend FFmpeg (compila el motor con la opcion CRAMION_WITH_FFMPEG)";
}
}  // namespace

bool isNetworkUri(const std::string& uri) {
    const std::string l = lowerAscii(uri.substr(0, 8));
    return l.rfind("https://", 0) == 0 || l.rfind("http://", 0) == 0;
}

bool isStreamingUri(const std::string& uri) {
    const std::string l = lowerAscii(uri.substr(0, 8));
    return l.rfind("rtsp://", 0) == 0 || l.rfind("rtsps://", 0) == 0 || l.rfind("rtmp://", 0) == 0 ||
           l.rfind("rtmps://", 0) == 0;
}

bool isHlsUri(const std::string& uri) { return hasExtension(uri, ".m3u8"); }

bool isDashUri(const std::string& uri) { return hasExtension(uri, ".mpd"); }

}  // namespace detail

bool streamingProtocolsAvailable() {
#if defined(CRAMION_HAS_FFMPEG)
    return true;
#else
    return false;
#endif
}

std::unique_ptr<IVideoDecoder> createDecoder(const std::string& uri, [[maybe_unused]] const DecodeOptions& options) {
    if (detail::isSyntheticUri(uri)) return detail::makeSyntheticDecoder();
    const bool needs_ffmpeg = detail::isStreamingUri(uri) || detail::isDashUri(uri);
#if defined(CRAMION_HAS_FFMPEG)
    if (needs_ffmpeg) return detail::makeFfmpegDecoder();
#if !defined(_WIN32)
    if (detail::isHlsUri(uri)) return detail::makeFfmpegDecoder();
#endif
#else
    if (needs_ffmpeg) return detail::makeNullDecoder(detail::ffmpegMissingReason(uri));
#endif
#if defined(_WIN32)
    if (detail::isHlsUri(uri)) return detail::makeHlsDecoder();
    return detail::makeMfDecoder();
#elif defined(__ANDROID__)
    return detail::makeMediaCodecDecoder();  // HLS sin FFmpeg: open() dice por que no
#elif defined(CRAMION_HAS_FFMPEG)
    return detail::makeFfmpegDecoder();
#else
    return detail::makeNullDecoder(detail::platformUnsupportedReason());
#endif
}

bool decoderAvailable(const std::string& uri, std::string* why) {
    const auto fail = [&](const std::string& reason) {
        if (why != nullptr) *why = reason;
        return false;
    };
    if (uri.empty()) return fail("No hay video (ruta vacia)");
    if (detail::isSyntheticUri(uri)) return true;
    if (detail::isNetworkUri(uri)) {
        std::string error;
        if (!net::checkUrl(uri, &error)) return fail(error);
    }
    if (detail::isStreamingUri(uri) || detail::isDashUri(uri)) {
#if defined(CRAMION_HAS_FFMPEG)
        return true;
#else
        return fail(detail::ffmpegMissingReason(uri));
#endif
    }
#if defined(_WIN32)
    std::string reason;
    if (!detail::mediaFoundationAvailable(&reason)) return fail(reason);
    return true;
#elif defined(__ANDROID__)
#if defined(CRAMION_HAS_FFMPEG)
    if (detail::isHlsUri(uri)) return true;
#endif
    std::string reason;
    if (!detail::mediaCodecAvailable(uri, &reason)) return fail(reason);
    return true;
#elif defined(CRAMION_HAS_FFMPEG)
    return true;
#else
    return fail(detail::platformUnsupportedReason());
#endif
}

}  // namespace cramion::video
