#ifndef CRAMION_VIDEO_DECODER_H
#define CRAMION_VIDEO_DECODER_H

// Contrato A (plan del VideoPlayer, Fase 1). Lo implementa WP2
// (src/video/decoders/: Synthetic, MediaFoundation en Windows, Null) y la
// Fase 2 (MediaCodec en Android y FFmpeg opcional para RTSP/RTMP/DASH).
// Ningun backend se enlaza de forma que impida arrancar: Media Foundation se
// carga en diferido (Windows N/KN sin Media Feature Pack). decoderAvailable()
// lo comprueba antes de usarlo.

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace cramion::video {

enum class DecodeMode : int { Auto = 0, Hardware, Software };
enum class PixelLayout : int { Rgba8 = 0, Bgra8, Nv12 };

struct ColorInfo {
    int matrix = 709;        // 601 | 709
    bool full_range = false;
};

struct DecodeOptions {
    DecodeMode mode = DecodeMode::Auto;
    std::uint32_t max_width = 0, max_height = 0;  // 0 = nativo (pista; la GPU escala de todos modos)
    bool low_latency = false;
    // Pista opcional: 0 = formato nativo de cada pista (lo normal). El
    // decodificador no garantiza remuestrear; AudioChunk dice lo que sale y
    // el AudioStream / ma_data_source declaran ese formato (miniaudio convierte).
    std::uint32_t audio_rate = 0;
    std::uint32_t audio_channels = 0;
    std::vector<int> audio_tracks;                // pistas a decodificar; vacio = ninguna
    bool probe_only = false;                      // solo info() (+ un frame para la miniatura)
};

struct AudioTrackInfo {
    std::string name, language;
    std::uint32_t channels = 0, rate = 0;
};

struct StreamInfo {
    std::uint32_t width = 0, height = 0;
    double fps = 0.0;
    double duration = -1.0;                       // <0 = directo/desconocida
    bool seekable = false, is_live = false;
    std::vector<AudioTrackInfo> audio;
    std::string codec, backend;                   // "H.264", "MediaFoundation"
    bool hardware = false;
    ColorInfo color;
};

struct VideoFrame {
    double pts = 0.0;
    std::uint32_t width = 0, height = 0;
    PixelLayout layout = PixelLayout::Nv12;
    ColorInfo color;
    std::array<const std::uint8_t*, 2> planes{};  // Nv12: Y, UV; Rgba8/Bgra8: [0]
    std::array<std::uint32_t, 2> strides{};
    std::shared_ptr<const void> owner;            // mantiene vivos los planos (pool)
};

struct AudioChunk {
    int track = -1;
    double pts = 0.0;
    std::uint32_t rate = 0;                       // frecuencia real de `samples`
    std::uint32_t channels = 0;                   // canales reales de `samples`
    std::vector<float> samples;                   // entrelazado, rate x channels
};

enum class DecodeEvent : int { Video = 0, Audio, NeedMore, EndOfStream, Error };

class IVideoDecoder {
public:
    virtual ~IVideoDecoder() = default;
    // Bloqueante; solo en el hilo del reproductor (MTA en Windows: COM y
    // MFStartup se inician en ese hilo, nunca en el hilo principal del editor).
    virtual bool open(const std::string& uri, const DecodeOptions& options, std::string& error) = 0;
    virtual StreamInfo info() const = 0;
    // Bloqueante hasta producir algo (un frame o un bloque de audio).
    virtual DecodeEvent next(VideoFrame& video, AudioChunk& audio, std::string& error) = 0;
    // Al keyframe anterior y vacia lo interno; el hilo descarta hasta el objetivo.
    virtual bool seek(double seconds) = 0;
    // Desde cualquier hilo: despierta open()/next() bloqueados (red). Tiene que
    // hacer volver de verdad (lector asincrono o byte stream propio con E_ABORT).
    virtual void interrupt() = 0;
};

// uri: ruta absoluta (UTF-8), https://..., http://localhost..., .m3u8, cramion://test?...
// y, con FFmpeg, rtsp://, rtmp:// y .mpd (DASH).
std::unique_ptr<IVideoDecoder> createDecoder(const std::string& uri, const DecodeOptions& options);
// En Windows comprueba que mfplat.dll se puede cargar; `why` dice que falta
// (Media Feature Pack, extension HEVC/AV1...). En Android, que https://
// tenga Android 9 y que no sea HLS (sin FFmpeg).
bool decoderAvailable(const std::string& uri, std::string* why = nullptr);
// rtsp://, rtsps://, rtmp(s):// y DASH (.mpd) se pueden abrir: el motor se
// compilo con el backend FFmpeg (opcion de CMake CRAMION_WITH_FFMPEG, apagada
// por defecto).
bool streamingProtocolsAvailable();

// Decodificador sintetico (pruebas): cramion://test?w=64&h=36&fps=30&dur=2&tracks=2
//   w,h (>= 8)  fps  dur (s; <= 0 = directo sin fin)  tracks (pistas de audio)
//   rate, ch    formato del audio si DecodeOptions no lo pide (48000, 2)
//   fmt=rgba|bgra|nv12   delay=ms (open bloquea, interrupt() lo corta)
//   decode_ms=ms (cada next())   gop=N (seek al keyframe anterior)
//   live=1   fail=1 (open falla)   error_at=s (next() da Error)   hw=1
//   matrix=601|709   range=full   block=N (frames de audio por bloque, 1024)
// Pista k: seno de 440*(k+1) Hz, amplitud 0.25, en todos los canales.
// El numero de frame (round(pts*fps)) va en los primeros bytes del plano 0:
// [0..2] = indice (little endian, 24 bits), [3] = 0xFF, [4..6] = C7 1D E0, [7] = 0xFF.
// Devuelve el indice, o -1 si los bytes no llevan la marca.
std::int64_t syntheticFrameIndex(const std::uint8_t* plane0_first_bytes);
std::int64_t syntheticFrameIndex(const VideoFrame& frame);

}  // namespace cramion::video
#endif
