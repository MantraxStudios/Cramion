#ifndef CRAMION_VIDEO_FILES_H
#define CRAMION_VIDEO_FILES_H

// Archivos y URLs de video (WP1): que es un video, que URL se puede abrir, de
// donde sale el URI del decodificador y la informacion + miniatura que el
// editor guarda al abrir el proyecto en Library/Cache/Video/<hash>.json|.png.
//
// isVideoFile esta en VideoPlayer.h (.mp4 .m4v .mov .webm .mkv .avi .wmv).
//
// probeVideo / probeVideoUri bloquean (abren el decodificador): nunca en el
// hilo principal con una URL; el editor las llama en 1-2 hilos de importacion.
// Cada hilo inicia COM / Media Foundation dentro del decodificador (MTA).

#include "CramionCore/video/VideoDecoder.h"
#include "CramionCore/video/VideoPlayer.h"

#include <CramionFX/asset/ImageFile.h>

#include <cstdint>
#include <filesystem>
#include <string>

namespace cramion::video {

// "https://...", "http://...", "cramion://...", "rtsp://"... (tiene esquema).
bool isVideoUrl(const std::string& text);

// La URL se puede abrir: https, http solo a este PC (la politica de
// net::checkUrl, Http.h) o el decodificador de pruebas cramion://test.
// false + motivo legible si no.
bool checkVideoUrl(const std::string& url, std::string* error);

// Texto UTF-8 de una ruta (lo que reciben los decodificadores).
std::string pathUtf8(const std::filesystem::path& path);

// Ruta de Assets ("Videos/intro.mp4"; tambien absoluta o una URL
// cramion://test) -> URI del decodificador (ruta absoluta UTF-8). Vacio +
// error si no existe o sale de Assets.
std::string resolveClip(const std::string& clip, const std::filesystem::path& assets_root, std::string* error);

// El URI del componente segun su Fuente (Clip o URL). Vacio + error si no vale.
std::string resolveSource(const VideoPlayer& player, const std::filesystem::path& assets_root, std::string* error);

// Lo que se sabe de un video (Inspector, navegador, Video.info).
struct VideoInfo {
    bool ok = false;
    std::string error;            // por que no se pudo abrir (codec que falta, Media Feature Pack...)
    StreamInfo stream;
    bool has_thumbnail = false;
    std::filesystem::path thumbnail;  // el .png de la cache (si se escribio)
    std::uint64_t file_size = 0;      // archivo local: para saber si la cache sigue valiendo
    std::int64_t file_time = 0;
};

// Rutas de la cache de un archivo: <cache_dir>/<hash>.json y .png (hash de
// la ruta absoluta; el .json guarda tamano y fecha).
struct VideoCachePaths {
    std::filesystem::path json;
    std::filesystem::path png;
};
VideoCachePaths videoCachePaths(const std::filesystem::path& cache_dir, const std::filesystem::path& file);

// La cache del archivo si existe y coincide con su tamano y fecha.
bool readCachedVideoInfo(const std::filesystem::path& cache_dir, const std::filesystem::path& file, VideoInfo& out);

// Archivo local: usa la cache si vale; si no, abre el video, lee la info y
// un frame y escribe <hash>.json y <hash>.png en cache_dir (vacio = sin
// cache). Devuelve out.ok. Tambien se escribe el .json de un error (no se
// reintenta cada vez que se abre el proyecto hasta que cambie el archivo).
bool probeVideo(const std::filesystem::path& file, const std::filesystem::path& cache_dir, VideoInfo& out,
                std::uint32_t thumbnail_size = 256);

// Cualquier URI (URL, "Probar URL" del Inspector): info y, si se pide, la
// miniatura en memoria. Sin cache. Devuelve out.ok.
bool probeVideoUri(const std::string& uri, VideoInfo& out, asset::ImageRgba8* thumbnail = nullptr,
                   std::uint32_t thumbnail_size = 256);

// Un frame decodificado a RGBA8 (sRGB tal cual: NV12 con la matriz y el
// rango del frame). max_size > 0 reduce el lado mayor. false si no se pudo.
bool frameToRgba(const VideoFrame& frame, std::uint32_t max_size, asset::ImageRgba8& out);

}  // namespace cramion::video

#endif  // CRAMION_VIDEO_FILES_H
