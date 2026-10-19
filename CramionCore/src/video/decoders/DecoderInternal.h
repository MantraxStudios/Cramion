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

// Sin backend (Linux sin FFmpeg, DASH sin FFmpeg...): open() falla con `reason`.
std::unique_ptr<IVideoDecoder> makeNullDecoder(std::string reason);
// Motivo por el que esta plataforma no reproduce video.
std::string platformUnsupportedReason();

// Termina en .m3u8 (sin contar ?query ni #fragmento).
bool isHlsUri(const std::string& uri);
// Termina en .mpd (DASH; sin contar ?query ni #fragmento).
bool isDashUri(const std::string& uri);
// https:// o http://
bool isNetworkUri(const std::string& uri);
// rtsp://, rtsps://, rtmp:// o rtmps:// (en directo; solo con FFmpeg).
bool isStreamingUri(const std::string& uri);

#if defined(__ANDROID__)
// MediaCodecDecoder.cpp: archivo o https:// (Android 9+) con AMediaExtractor y
// AMediaCodec. HLS no (sin FFmpeg, open() falla con un motivo claro).
std::unique_ptr<IVideoDecoder> makeMediaCodecDecoder();
// Se puede abrir `uri` en este movil (HLS no; https:// necesita Android 9).
bool mediaCodecAvailable(const std::string& uri, std::string* why);
#endif

#if defined(CRAMION_HAS_FFMPEG)
// FfmpegDecoder.cpp (opcion CRAMION_WITH_FFMPEG): RTSP/RTMP, DASH, HLS fuera
// de Windows y, en Linux, todo.
std::unique_ptr<IVideoDecoder> makeFfmpegDecoder();
#endif

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
