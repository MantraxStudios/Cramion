#ifndef CRAMION_VIDEO_PLAYER_H
#define CRAMION_VIDEO_PLAYER_H

// Contrato C: el componente VideoPlayer (como el de Unity). Lo implementa WP1.

#include "CramionCore/ecs/Reflection.h"
#include "CramionCore/asset/AssetTypes.h"  // assets::AssetRef
#include "CramionCore/Uuid.h"
#include "CramionCore/video/VideoDecoder.h"
#include <filesystem>
#include <string>
#include <vector>

namespace cramion::video {

enum class VideoSource      : int { Clip = 0, Url };
enum class RenderMode       : int { RenderTexture = 0, MaterialOverride, CameraFarPlane, CameraNearPlane, ApiOnly };
enum class MaterialProperty : int { Emissive = 0, Albedo, Both };
enum class AspectMode       : int { Stretch = 0, FitInside, FitOutside, FitHorizontally, FitVertically, NoScaling };
enum class AudioOutput      : int { None = 0, Direct, AudioSource, ApiOnly };
enum class TimeSource       : int { AudioClock = 0, GameTime, UnscaledTime };
enum class MaxResolution    : int { Native = 0, P2160, P1440, P1080, P720, P480, P360 };
// Linear solo se aplica con RenderMode::ApiOnly; el resto sale siempre en sRGB.
enum class ColorOutput      : int { Srgb = 0, Linear };

struct VideoAudioTrack {               // "Pista de audio"
    bool enabled = true;               // "Activa"
    float volume = 1.0f;               // "Volumen"
    bool mute = false;                 // "Silenciar"
    std::vector<Uuid> targets;         // "Fuentes de audio" (entidades con AudioSource; varias = mismo sonido en N)
};

struct VideoPlayer {
    // Fuente
    VideoSource source = VideoSource::Clip;   // "Fuente"
    std::string clip;                         // "Clip de video" (ruta en Assets)
    std::string url;                          // "URL"
    // Reproduccion
    bool play_on_awake = true;
    bool wait_for_first_frame = true;
    bool loop = false;
    bool skip_on_drop = true;
    float playback_speed = 1.0f;              // 0.1..4
    float start_time = 0.0f;                  // s
    float end_time = 0.0f;                    // s (0 = hasta el final)
    TimeSource time_source = TimeSource::AudioClock;
    bool send_frame_ready = false;            // OnVideoFrameReady (coste)
    // Imagen
    RenderMode render_mode = RenderMode::MaterialOverride;
    assets::AssetRef target_texture;          // RenderTexture (.crrt)
    Uuid target_renderer;                     // vacio = esta entidad
    int material_slot = -1;
    MaterialProperty material_property = MaterialProperty::Emissive;
    float emissive_intensity = 1.0f;
    float camera_alpha = 1.0f;
    AspectMode aspect = AspectMode::FitInside;
    // Audio
    AudioOutput audio_output = AudioOutput::Direct;
    float volume = 1.0f;
    bool direct_spatial = true;
    float direct_min_distance = 2.0f;
    float direct_max_distance = 40.0f;
    std::vector<VideoAudioTrack> tracks;      // se rellena con los metadatos
    // Calidad
    MaxResolution max_resolution = MaxResolution::Native;
    DecodeMode decode = DecodeMode::Auto;
    float buffer_seconds = 1.0f;              // bytes comprimidos/red y anillo de audio; nunca frames decodificados
    int max_queued_frames = 4;                // 2..8
    bool low_latency = false;
    ColorOutput color = ColorOutput::Srgb;

    void reflect(ecs::PropertyVisitor& v);    // enums por nombre; grupos Fuente/Reproduccion/Imagen/Audio/Calidad
};

void registerVideoComponents();               // ("VideoPlayer", "Reproductor de video", "Video")
bool isVideoFile(const std::filesystem::path& file);

}  // namespace cramion::video
#endif
