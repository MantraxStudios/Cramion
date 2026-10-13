// Video: el VideoPlayer de los objetos desde los scripts (video/VideoSystem.h).
//
//   entity:videoPlay() / videoPause() / videoStop() / videoPrepare()
//   entity:videoSeek(segundos) / videoSeekFrame(frame) / videoStepForward(n)
//   entity:videoSetClip("Videos/intro.mp4") / videoSetUrl("https://...")
//   entity:videoSetTrackVolume(pista, volumen) / videoSetTrackMute(pista, true)
//   entity:videoSetTrackTargets(pista, {fuente1, fuente2})   (pistas desde 1)
//   entity:videoReadSamples(pista, frames)   Salida de audio ApiOnly
//   entity.videoTime, videoFrame, videoLength, videoIsPlaying, videoState...
//   Video.isSupported(uri) / Video.info("Videos/intro.mp4")
//
// Eventos (fase Events): a los Visual Scripts del objeto (Custom Event con
// ese nombre y el nodo "On Video Event") y a sus scripts de C++ (onMessage):
//   OnVideoPrepared, OnVideoStarted, OnVideoLoopPointReached,
//   OnVideoFrameReady (solo con "Enviar OnVideoFrameReady"), OnVideoSeekCompleted,
//   OnVideoError, OnVideoFrameDropped, OnVideoBuffering, OnVideoResumed
// con {type, time, count, message}. Sin VideoSystem (servidor) los metodos no
// hacen nada y las propiedades dan sus valores por defecto.

#include "Modules.h"

#include "CramionCore/video/VideoFiles.h"
#include "CramionCore/video/VideoSystem.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <exception>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace cramion::scripting::native {

namespace {

using video::PlayerState;
using video::VideoEventType;

const char* eventName(VideoEventType type) {
    switch (type) {
        case VideoEventType::Prepared: return "OnVideoPrepared";
        case VideoEventType::Started: return "OnVideoStarted";
        case VideoEventType::LoopPointReached: return "OnVideoLoopPointReached";
        case VideoEventType::FrameReady: return "OnVideoFrameReady";
        case VideoEventType::SeekCompleted: return "OnVideoSeekCompleted";
        case VideoEventType::Error: return "OnVideoError";
        case VideoEventType::FrameDropped: return "OnVideoFrameDropped";
        case VideoEventType::Buffering: return "OnVideoBuffering";
        case VideoEventType::Resumed: return "OnVideoResumed";
    }
    return "OnVideoEvent";
}

const char* stateName(PlayerState state) {
    switch (state) {
        case PlayerState::Idle: return "Idle";
        case PlayerState::Preparing: return "Preparing";
        case PlayerState::Prepared: return "Prepared";
        case PlayerState::Playing: return "Playing";
        case PlayerState::Paused: return "Paused";
        case PlayerState::Buffering: return "Buffering";
        case PlayerState::Ended: return "Ended";
        case PlayerState::Error: return "Error";
    }
    return "Idle";
}

void method(Runtime& rt, const char* name, api::Function fn, api::Doc doc) {
    rt.native.method("Entity", name, std::move(fn), std::move(doc));
}

void property(Runtime& rt, const char* name, api::Function get, api::Function set, api::Doc doc) {
    rt.native.property("Entity", name, std::move(get), std::move(set), std::move(doc), true);
}

// El VideoPlayer del objeto (error si no tiene).
video::VideoPlayer& player(Runtime& rt, const api::Call& c, const char* who) {
    const ecs::Entity e = rt.selfEntity(c);
    video::VideoPlayer* p = e.tryGet<video::VideoPlayer>();
    if (p == nullptr) throw api::Error(std::string(who) + ": el objeto no tiene VideoPlayer");
    return *p;
}

// Pista desde 1 (como las listas de la API) -> indice desde 0.
int trackArg(const api::Call& c, std::size_t i, const char* who) {
    const long long t = c.integer(i, 1);
    if (t < 1 || t > 32) throw api::Error(std::string(who) + ": pista fuera de rango (desde 1)");
    return static_cast<int>(t - 1);
}

video::VideoAudioTrack& track(video::VideoPlayer& p, int index) {
    if (p.tracks.size() <= static_cast<std::size_t>(index)) p.tracks.resize(static_cast<std::size_t>(index) + 1);
    return p.tracks[static_cast<std::size_t>(index)];
}

video::PlayerStatus status(Runtime& rt, const api::Call& c) {
    const ecs::Entity e = rt.selfEntity(c);
    return rt.video != nullptr ? rt.video->status(e) : video::PlayerStatus{};
}

api::Value infoValue(const video::VideoInfo& info) {
    api::Value t = api::Value::object();
    const video::StreamInfo& s = info.stream;
    t.set("width", s.width);
    t.set("height", s.height);
    t.set("fps", s.fps);
    t.set("duration", s.duration);
    t.set("frameCount", s.duration > 0.0 && s.fps > 0.0 ? static_cast<double>(std::llround(s.duration * s.fps)) : 0.0);
    t.set("seekable", s.seekable);
    t.set("live", s.is_live);
    t.set("codec", s.codec);
    t.set("backend", s.backend);
    t.set("hardware", s.hardware);
    api::Value::Array tracks;
    for (const video::AudioTrackInfo& a : s.audio) {
        api::Value tr = api::Value::object();
        tr.set("name", a.name);
        tr.set("language", a.language);
        tr.set("channels", a.channels);
        tr.set("rate", a.rate);
        tracks.push_back(std::move(tr));
    }
    t.set("audioTracks", api::Value(std::move(tracks)));
    return t;
}

}  // namespace

void registerVideoApi(Runtime& rt) {
    // --- Controles ---
    const auto control = [&rt](const char* name, void (video::VideoSystem::*fn)(ecs::Entity), const char* doc) {
        method(rt, name,
               [&rt, fn](api::Call& c) -> api::Value {
                   const ecs::Entity e = rt.selfEntity(c);
                   if (rt.video != nullptr) (rt.video->*fn)(e);
                   return {};
               },
               {"", doc});
    };
    control("videoPlay", &video::VideoSystem::play, "reproduce el video (lo prepara si hace falta)");
    control("videoPause", &video::VideoSystem::pause, "pausa el video");
    control("videoStop", static_cast<void (video::VideoSystem::*)(ecs::Entity)>(&video::VideoSystem::stop),
            "para el video y suelta el decodificador (vuelve al principio)");
    control("videoPrepare", &video::VideoSystem::prepare, "abre el video y deja el primer frame listo (OnVideoPrepared)");
    method(rt, "videoSeek",
           [&rt](api::Call& c) -> api::Value {
               const ecs::Entity e = rt.selfEntity(c);
               if (rt.video != nullptr) rt.video->seek(e, c.number(0));
               return {};
           },
           {"segundos", "salta a ese segundo del video (OnVideoSeekCompleted al llegar)"});
    method(rt, "videoSeekFrame",
           [&rt](api::Call& c) -> api::Value {
               const ecs::Entity e = rt.selfEntity(c);
               if (rt.video == nullptr) return {};
               const video::PlayerStatus s = rt.video->status(e);
               const double fps = s.fps > 0.0 ? s.fps : 30.0;
               rt.video->seek(e, static_cast<double>(c.integer(0)) / fps);
               return {};
           },
           {"frame", "salta a ese frame (desde 0)"});
    method(rt, "videoStepForward",
           [&rt](api::Call& c) -> api::Value {
               const ecs::Entity e = rt.selfEntity(c);
               if (rt.video != nullptr) rt.video->stepForward(e, static_cast<int>(std::max<long long>(1, c.integer(0, 1))));
               return {};
           },
           {"frames", "pausa y avanza N frames (1 por defecto)"});
    method(rt, "videoSetClip",
           [&rt](api::Call& c) -> api::Value {
               video::VideoPlayer& p = player(rt, c, "videoSetClip");
               p.source = video::VideoSource::Clip;
               p.clip = c.string(0);
               return {};
           },
           {"\"Videos/intro.mp4\"", "cambia el clip (ruta en Assets); si estaba sonando, sigue con el nuevo"});
    method(rt, "videoSetUrl",
           [&rt](api::Call& c) -> api::Value {
               video::VideoPlayer& p = player(rt, c, "videoSetUrl");
               const std::string url = c.string(0);
               std::string error;
               if (!video::checkVideoUrl(url, &error)) throw api::Error("videoSetUrl: " + error);
               p.source = video::VideoSource::Url;
               p.url = url;
               return {};
           },
           {"\"https://...\"", "cambia la fuente a una URL (https, o http a este PC)"});
    method(rt, "videoSetTrackVolume",
           [&rt](api::Call& c) -> api::Value {
               video::VideoPlayer& p = player(rt, c, "videoSetTrackVolume");
               track(p, trackArg(c, 0, "videoSetTrackVolume")).volume = std::max(0.0f, static_cast<float>(c.number(1)));
               return {};
           },
           {"pista, volumen", "volumen de una pista de audio (desde 1)"});
    method(rt, "videoSetTrackMute",
           [&rt](api::Call& c) -> api::Value {
               video::VideoPlayer& p = player(rt, c, "videoSetTrackMute");
               track(p, trackArg(c, 0, "videoSetTrackMute")).mute = c.boolean(1, true);
               return {};
           },
           {"pista, true", "silencia (o no) una pista de audio (desde 1)"});
    method(rt, "videoSetTrackTargets",
           [&rt](api::Call& c) -> api::Value {
               video::VideoPlayer& p = player(rt, c, "videoSetTrackTargets");
               video::VideoAudioTrack& t = track(p, trackArg(c, 0, "videoSetTrackTargets"));
               std::vector<Uuid> targets;
               const api::Value& list = c.arg(1);
               const auto add = [&](const api::Value& v) {
                   const ecs::Entity e = rt.entity(v.asEntity());
                   if (e.valid()) targets.push_back(e.uuid());
               };
               if (list.isEntity()) {
                   add(list);
               } else if (list.isArray()) {
                   for (const api::Value& v : list.items()) add(v);
               }
               t.targets = std::move(targets);
               return {};
           },
           {"pista, {fuente1, fuente2}", "objetos con AudioSource por los que suena la pista (Salida de audio AudioSource)"});
    method(rt, "videoReadSamples",
           [&rt](api::Call& c) -> api::Value {
               const ecs::Entity e = rt.selfEntity(c);
               if (rt.video == nullptr) return api::Value(api::Value::Array{});
               const int tr = trackArg(c, 0, "videoReadSamples");
               const video::PlayerStatus s = rt.video->status(e);
               const std::size_t channels = std::max<std::uint32_t>(s.audio_channels, 1);
               const std::size_t limit = std::max<std::size_t>(s.audio_rate > 0 ? s.audio_rate / 10 : 4800, 1);  // 100 ms
               const std::size_t frames = std::clamp<std::size_t>(static_cast<std::size_t>(std::max<long long>(1, c.integer(1, 1024))), 1, limit);
               std::vector<float> buffer(frames * channels);
               const std::size_t got = rt.video->readSamples(e, tr, buffer.data(), frames);
               api::Value::Array out;
               out.reserve(got * channels);
               for (std::size_t i = 0; i < got * channels; ++i) out.emplace_back(buffer[i]);
               return api::Value(std::move(out));
           },
           {"pista, frames", "muestras entrelazadas de la pista (Salida de audio ApiOnly; bloques de 20-50 ms, no es tiempo real)",
            "lista de numeros"});

    // --- Propiedades ---
    property(rt, "videoTime", [&rt](api::Call& c) -> api::Value { return status(rt, c).time; },
             [&rt](api::Call& c) -> api::Value {
                 const ecs::Entity e = rt.selfEntity(c);
                 if (rt.video != nullptr) rt.video->seek(e, c.number(0));
                 return {};
             },
             {"", "segundo actual del video (escribirlo busca)", "numero"});
    property(rt, "videoFrame", [&rt](api::Call& c) -> api::Value { return static_cast<double>(status(rt, c).frame); },
             [&rt](api::Call& c) -> api::Value {
                 const ecs::Entity e = rt.selfEntity(c);
                 if (rt.video == nullptr) return {};
                 const double fps = rt.video->status(e).fps;
                 rt.video->seek(e, static_cast<double>(c.integer(0)) / (fps > 0.0 ? fps : 30.0));
                 return {};
             },
             {"", "frame que se ve (escribirlo busca)", "numero"});
    property(rt, "videoLength", [&rt](api::Call& c) -> api::Value { return status(rt, c).length; }, {},
             {"", "duracion en segundos (-1 en directo o si aun no se sabe)", "numero"});
    property(rt, "videoFrameCount", [&rt](api::Call& c) -> api::Value { return static_cast<double>(status(rt, c).frame_count); }, {},
             {"", "frames del video", "numero"});
    property(rt, "videoFrameRate", [&rt](api::Call& c) -> api::Value { return status(rt, c).fps; }, {},
             {"", "frames por segundo", "numero"});
    property(rt, "videoWidth", [&rt](api::Call& c) -> api::Value { return status(rt, c).width; }, {}, {"", "ancho en pixeles", "numero"});
    property(rt, "videoHeight", [&rt](api::Call& c) -> api::Value { return status(rt, c).height; }, {}, {"", "alto en pixeles", "numero"});
    property(rt, "videoIsPlaying",
             [&rt](api::Call& c) -> api::Value {
                 const PlayerState s = status(rt, c).state;
                 return s == PlayerState::Playing || s == PlayerState::Buffering;
             },
             {}, {"", "esta sonando?", "bool"});
    property(rt, "videoIsPrepared",
             [&rt](api::Call& c) -> api::Value {
                 const PlayerState s = status(rt, c).state;
                 return s != PlayerState::Idle && s != PlayerState::Preparing && s != PlayerState::Error;
             },
             {}, {"", "tiene el primer frame listo?", "bool"});
    property(rt, "videoIsLive", [&rt](api::Call& c) -> api::Value { return status(rt, c).is_live; }, {}, {"", "es un directo?", "bool"});
    property(rt, "videoState", [&rt](api::Call& c) -> api::Value { return stateName(status(rt, c).state); }, {},
             {"", "Idle, Preparing, Prepared, Playing, Paused, Buffering, Ended o Error", "texto"});
    property(rt, "videoPlaybackSpeed", [&rt](api::Call& c) -> api::Value { return player(rt, c, "videoPlaybackSpeed").playback_speed; },
             [&rt](api::Call& c) -> api::Value {
                 player(rt, c, "videoPlaybackSpeed").playback_speed = std::clamp(static_cast<float>(c.number(0)), 0.1f, 4.0f);
                 return {};
             },
             {"", "velocidad (0.1..4; fuera de 0.5..2 el audio se calla)", "numero"});
    property(rt, "videoLoop", [&rt](api::Call& c) -> api::Value { return player(rt, c, "videoLoop").loop; },
             [&rt](api::Call& c) -> api::Value {
                 player(rt, c, "videoLoop").loop = c.boolean(0);
                 return {};
             },
             {"", "bucle", "bool"});
    property(rt, "videoVolume", [&rt](api::Call& c) -> api::Value { return player(rt, c, "videoVolume").volume; },
             [&rt](api::Call& c) -> api::Value {
                 player(rt, c, "videoVolume").volume = std::max(0.0f, static_cast<float>(c.number(0)));
                 return {};
             },
             {"", "volumen general del video", "numero"});
    property(rt, "videoDroppedFrames", [&rt](api::Call& c) -> api::Value { return static_cast<double>(status(rt, c).dropped); }, {},
             {"", "frames saltados desde que empezo", "numero"});
    property(rt, "videoTexture",
             [&rt](api::Call& c) -> api::Value {
                 const ecs::Entity e = rt.selfEntity(c);
                 return rt.video != nullptr ? rt.video->textureOf(e) : -1;
             },
             {}, {"", "id de la textura del video (para otras llamadas de la API; -1 si no hay)", "numero"});

    // --- Tabla Video ---
    rt.native.function(
        "Video.isSupported",
        [&rt](api::Call& c) -> api::Value {
            const std::string uri = c.string(0);
            std::string resolved;
            if (video::isVideoUrl(uri)) {
                if (!video::checkVideoUrl(uri, nullptr)) return false;
                resolved = uri;
            } else {
                if (!video::isVideoFile(pathFromUtf8(uri))) return false;
                resolved = video::resolveClip(uri, rt.root, nullptr);
                if (resolved.empty()) return false;
            }
            return video::decoderAvailable(resolved, nullptr);
        },
        {"\"Videos/intro.mp4\"", "se puede reproducir este archivo (o URL) en esta plataforma?", "bool"});
    rt.native.function(
        "Video.info",
        [&rt](api::Call& c) -> api::Value {
            const std::string path = c.string(0);
            video::VideoInfo info;
            if (video::isVideoUrl(path)) {
                // Abrir una URL bloquea hasta conectar: nunca en el hilo del juego.
                rt.write(1, "Video.info: solo archivos de Assets; con una URL usa videoPrepare y OnVideoPrepared");
                return {};
            } else {
                std::string error;
                const std::string resolved = video::resolveClip(path, rt.root, &error);
                if (resolved.empty()) {
                    rt.write(2, "Video.info: " + error);
                    return {};
                }
                // La misma cache que el editor (Library/Cache/Video junto a Assets).
                // En un hilo aparte y se espera (archivo local: poco): Media
                // Foundation inicia COM en MTA en el hilo que lo usa, y el hilo
                // principal del editor debe seguir en STA (OLE, dialogos).
                const std::filesystem::path file = pathFromUtf8(resolved);
                const std::filesystem::path cache = rt.root.parent_path() / "Library" / "Cache" / "Video";
                std::thread([&] {
                    try {
                        video::probeVideo(file, cache, info);
                    } catch (const std::exception& e) {
                        info.ok = false;
                        info.error = e.what();
                    }
                }).join();
            }
            if (!info.ok) {
                rt.write(2, "Video.info: " + (info.error.empty() ? std::string("no se pudo abrir ") + path : info.error));
                return {};
            }
            return infoValue(info);
        },
        {"\"Videos/intro.mp4\"", "{width, height, fps, duration, frameCount, seekable, live, codec, backend, hardware, audioTracks} o nil",
         "objeto o nil"});

    // --- Eventos ---
    rt.onFrame(Phase::Events, [&rt](float) {
        if (rt.video == nullptr) return;
        std::vector<video::VideoEvent> events = rt.video->takeEvents();
        if (events.empty() || rt.world == nullptr) return;
        for (const video::VideoEvent& ev : events) {
            const ecs::Entity e = rt.entity(ev.entity.handle());
            if (!e.valid()) continue;  // ya no existe (o de otra escena)
            const char* name = eventName(ev.type);
            api::Value value = api::Value::object();
            value.set("type", std::string(name).substr(7));
            value.set("time", ev.time);
            value.set("count", ev.count);
            value.set("message", ev.message);
            for (const auto& fn : rt.hooks.event) fn(e, name, value);
            if (rt.message_listener) {
                rt.message_listener(e, name, rt.native.toJson(value).dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
            }
        }
    });
    // Una entidad que se destruye deja de decodificar al momento.
    rt.onDestroying([&rt](entt::entity h) {
        if (rt.video != nullptr && rt.world != nullptr) rt.video->forget(rt.world->wrap(h));
    });
}

}  // namespace cramion::scripting::native
