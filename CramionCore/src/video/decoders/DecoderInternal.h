#ifndef CRAMION_VIDEO_DECODER_INTERNAL_H
#define CRAMION_VIDEO_DECODER_INTERNAL_H

// Interno de src/video/decoders: lo que DecoderFactory.cpp necesita de cada
// backend. No es API publica (el contrato es VideoDecoder.h).

#include "CramionCore/video/VideoDecoder.h"

#include <memory>
#include <string>

namespace cramion::video::detail {

// cramion://test?... (SyntheticDecoder.cpp; todas las plataformas).
bool isSyntheticUri(const std::string& uri);
std::unique_ptr<IVideoDecoder> makeSyntheticDecoder();

// Sin backend (Linux, Android hasta la Fase 2): open() falla con `reason`.
std::unique_ptr<IVideoDecoder> makeNullDecoder(std::string reason);
// Motivo por el que esta plataforma no reproduce video.
std::string platformUnsupportedReason();

// Termina en .m3u8 (sin contar ?query ni #fragmento).
bool isHlsUri(const std::string& uri);
// https:// o http://
bool isNetworkUri(const std::string& uri);

#if defined(_WIN32)
// MfDecoder.cpp: archivo o http(s) con IMFSourceReader asincrono.
std::unique_ptr<IVideoDecoder> makeMfDecoder();
// Media Foundation se puede cargar (mfplat.dll y mfreadwrite.dll). Si no, `why`
// explica que instalar (Media Feature Pack en Windows N/KN).
bool mediaFoundationAvailable(std::string* why);
// MfEngineDecoder.cpp: HLS (.m3u8) con un byte stream propio sobre
// net::httpRequest que encadena los segmentos hacia el Source Reader.
std::unique_ptr<IVideoDecoder> makeHlsDecoder();
#endif

}  // namespace cramion::video::detail

#endif  // CRAMION_VIDEO_DECODER_INTERNAL_H
