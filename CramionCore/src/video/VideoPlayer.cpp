// El componente VideoPlayer (VideoPlayer.h): sus propiedades para el
// Inspector y la escena, y su registro. Lo reproduce VideoSystem.

#include "CramionCore/video/VideoPlayer.h"

#include "CramionCore/ecs/World.h"  // la definicion de ComponentRegistry::registerComponent<T>

#include <array>

namespace cramion::video {

namespace {

using ecs::FloatRange;
using ecs::Meta;

constexpr std::array<const char*, 2> kSources = {"Clip", "Url"};
constexpr std::array<const char*, 5> kRenderModes = {"RenderTexture", "MaterialOverride", "CameraFarPlane", "CameraNearPlane",
                                                      "ApiOnly"};
constexpr std::array<const char*, 3> kProperties = {"Emissive", "Albedo", "Both"};
constexpr std::array<const char*, 6> kAspects = {"Stretch", "FitInside", "FitOutside", "FitHorizontally", "FitVertically",
                                                 "NoScaling"};
constexpr std::array<const char*, 4> kAudioOutputs = {"None", "Direct", "AudioSource", "ApiOnly"};
constexpr std::array<const char*, 3> kTimeSources = {"AudioClock", "GameTime", "UnscaledTime"};
constexpr std::array<const char*, 7> kResolutions = {"Native", "P2160", "P1440", "P1080", "P720", "P480", "P360"};
constexpr std::array<const char*, 3> kDecodeModes = {"Auto", "Hardware", "Software"};
constexpr std::array<const char*, 2> kColors = {"Srgb", "Linear"};

// Un grupo con cabecera: f() solo si la UI lo tiene abierto.
template <typename Fn>
void group(ecs::PropertyVisitor& v, const char* label, bool open, Fn&& f) {
    if (v.beginGroup(label, open)) {
        f();
        v.endGroup();
    }
}

}  // namespace

void VideoPlayer::reflect(ecs::PropertyVisitor& v) {
    const bool all = v.wantsAllFields();
    group(v, "Fuente", true, [&] {
        ecs::enumField(v, {"source", "Fuente", "Clip: un video de Assets. URL: https (o http a este PC); HLS .m3u8 en Windows"},
                       source, kSources);
        if (all || source == VideoSource::Clip) {
            v.field({"clip", "Clip de video", "Archivo de Assets (.mp4 .m4v .mov .webm .mkv .avi .wmv). Recomendado: H.264 + AAC"},
                    clip);
        }
        if (all || source == VideoSource::Url) v.field({"url", "URL", "https://... (o http://localhost...)"}, url);
    });
    group(v, "Reproduccion", true, [&] {
        v.field({"play_on_awake", "Reproducir al empezar"}, play_on_awake);
        v.field({"wait_for_first_frame", "Esperar al primer frame", "El reloj no avanza hasta tener la primera imagen"},
                wait_for_first_frame);
        v.field({"loop", "Bucle"}, loop);
        v.field({"skip_on_drop", "Saltar frames", "Si el decodificador se retrasa, salta frames para no ir por detras"},
                skip_on_drop);
        v.field({"playback_speed", "Velocidad"}, playback_speed, FloatRange{0.1f, 4.0f, 0.01f, "%.2fx", true});
        v.field({"start_time", "Empezar en", "Segundos"}, start_time, FloatRange{0.0f, 86400.0f, 0.05f, "%.2f s"});
        v.field({"end_time", "Fin", "Segundos (0 = hasta el final). Con Bucle, repite la seccion"}, end_time,
                FloatRange{0.0f, 86400.0f, 0.05f, "%.2f s"});
        ecs::enumField(v,
                       {"time_source", "Reloj",
                        "AudioClock: el audio marca el tiempo (labios a tiempo). GameTime: tiempo del juego (se para "
                        "con la pausa y la escala). UnscaledTime: sin escala"},
                       time_source, kTimeSources);
        v.field({"send_frame_ready", "Enviar OnVideoFrameReady", "Un evento por frame (cuesta)"}, send_frame_ready);
    });
    group(v, "Imagen", true, [&] {
        ecs::enumField(v,
                       {"render_mode", "Salida",
                        "RenderTexture: escribe en una .crrt. MaterialOverride: la textura de un material del objeto. "
                        "Planos: detras o delante de la vista del juego. ApiOnly: solo para scripts"},
                       render_mode, kRenderModes);
        if (all || render_mode == RenderMode::RenderTexture) {
            v.asset({"target_texture", "Render Texture"}, target_texture, assets::AssetType::RenderTexture);
        }
        if (all || render_mode == RenderMode::MaterialOverride) {
            v.entity({"target_renderer", "Objeto", "Vacio = este objeto"}, target_renderer);
            v.field({"material_slot", "Hueco de material", "-1 = todos"}, material_slot, -1, 64);
            ecs::enumField(v, {"material_property", "Propiedad"}, material_property, kProperties);
            v.field({"emissive_intensity", "Intensidad"}, emissive_intensity, FloatRange{0.0f, 20.0f, 0.01f, "%.2f"});
        }
        if (all || render_mode == RenderMode::CameraFarPlane || render_mode == RenderMode::CameraNearPlane) {
            v.field({"camera_alpha", "Opacidad"}, camera_alpha, FloatRange{0.0f, 1.0f, 0.01f, "%.2f", true});
        }
        ecs::enumField(v, {"aspect", "Proporcion"}, aspect, kAspects);
    });
    group(v, "Audio", true, [&] {
        ecs::enumField(v,
                       {"audio_output", "Salida de audio",
                        "Direct: suena desde este objeto. AudioSource: por las fuentes de cada pista (con sus efectos "
                        "y oclusion). ApiOnly: los scripts leen las muestras"},
                       audio_output, kAudioOutputs);
        v.field({"volume", "Volumen"}, volume, FloatRange{0.0f, 2.0f, 0.01f, "%.2f", true});
        if (all || audio_output == AudioOutput::Direct) {
            v.field({"direct_spatial", "3D"}, direct_spatial);
            v.field({"direct_min_distance", "Distancia minima"}, direct_min_distance, FloatRange{0.0f, 1000.0f, 0.1f, "%.1f m"});
            v.field({"direct_max_distance", "Distancia maxima"}, direct_max_distance, FloatRange{0.1f, 5000.0f, 0.1f, "%.1f m"});
        }
        ecs::listField(v, {"tracks", "Pistas de audio"}, tracks, [all, this](VideoAudioTrack& t, ecs::PropertyVisitor& tv) {
            tv.field({"enabled", "Activa"}, t.enabled);
            tv.field({"volume", "Volumen"}, t.volume, FloatRange{0.0f, 2.0f, 0.01f, "%.2f", true});
            tv.field({"mute", "Silenciar"}, t.mute);
            if (all || audio_output == AudioOutput::AudioSource) {
                ecs::listField(tv, {"targets", "Fuentes de audio", "Objetos con AudioSource: varios = el mismo sonido en N altavoces"},
                               t.targets, [](Uuid& target, ecs::PropertyVisitor& ev) { ev.entity({"entity", "Fuente"}, target); });
            }
        });
    });
    group(v, "Calidad", false, [&] {
        ecs::enumField(v, {"max_resolution", "Resolucion maxima"}, max_resolution, kResolutions);
        ecs::enumField(v, {"decode", "Decodificacion", "Auto: por hardware si se puede"}, decode, kDecodeModes);
        v.field({"buffer_seconds", "Bufer", "Segundos de datos comprimidos/red y de audio (nunca frames decodificados)"},
                buffer_seconds, FloatRange{0.1f, 30.0f, 0.05f, "%.2f s"});
        v.field({"max_queued_frames", "Frames en cola"}, max_queued_frames, 2, 8);
        v.field({"low_latency", "Baja latencia"}, low_latency);
        ecs::enumField(v, {"color", "Color", "Linear solo con Salida ApiOnly (el resto va siempre en sRGB)"}, color, kColors);
    });
}

void registerVideoComponents() {
    ecs::ComponentRegistry& registry = ecs::ComponentRegistry::instance();
    if (registry.find("VideoPlayer") == nullptr) {
        registry.registerComponent<VideoPlayer>("VideoPlayer", "Reproductor de video", "Video");
    }
}

}  // namespace cramion::video
