// Elige el backend de cada URI (contrato A, VideoDecoder.h):
//   cramion://test?...      SyntheticDecoder (todas las plataformas)
//   *.m3u8 (archivo o red)  HLS sobre el Source Reader de Media Foundation (Windows)
//   archivo, https://       MfDecoder (Windows)
//   el resto                NullDecoder (Linux; Android en la Fase 2)

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
}  // namespace

bool isNetworkUri(const std::string& uri) {
    const std::string l = lowerAscii(uri.substr(0, 8));
    return l.rfind("https://", 0) == 0 || l.rfind("http://", 0) == 0;
}

bool isHlsUri(const std::string& uri) {
    std::string path = uri;
    if (isNetworkUri(uri)) {
        if (const auto cut = path.find_first_of("?#"); cut != std::string::npos) path.resize(cut);
    }
    path = lowerAscii(path);
    return path.size() >= 5 && path.compare(path.size() - 5, 5, ".m3u8") == 0;
}

}  // namespace detail

std::unique_ptr<IVideoDecoder> createDecoder(const std::string& uri, [[maybe_unused]] const DecodeOptions& options) {
    if (detail::isSyntheticUri(uri)) return detail::makeSyntheticDecoder();
#if defined(_WIN32)
    if (detail::isHlsUri(uri)) return detail::makeHlsDecoder();
    return detail::makeMfDecoder();
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
#if defined(_WIN32)
    std::string reason;
    if (!detail::mediaFoundationAvailable(&reason)) return fail(reason);
    return true;
#else
    return fail(detail::platformUnsupportedReason());
#endif
}

}  // namespace cramion::video
