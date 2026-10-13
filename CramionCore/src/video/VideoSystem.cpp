// VideoSystem: los VideoPlayer de la escena en Play y la vista previa del
// editor (VideoSystem.h).
//
// Cada reproductor tiene un hilo de decodificacion (VideoWorker) y, en el
// hilo principal, su reloj:
//   - AudioClock: lo que la voz maestra ya ha sonado (AudioStream::playedSeconds);
//     si el audio se para (pista mas corta, sin dispositivo) mas de 0,2 s,
//     sigue con el tiempo de juego. Con velocidades fuera de 0,5..2 el audio
//     se calla y manda el tiempo de juego (como Unity).
//   - GameTime / UnscaledTime: dt (o sin escalar) por la velocidad.
// Cada frame se presenta el ultimo frame con timeline <= reloj (los que se
// saltan cuentan como FrameDropped) y se manda al VideoOutput; el audio se
// enruta con attachStream/updateStream/detachStream (Direct: desde el objeto;
// AudioSource: por las fuentes de cada pista; ApiOnly: lectores propios).
//
// Cambio de escena: World::sceneGeneration (como TerrainStreamer): si sube,
// todo se para sin eventos. Las entidades que desaparecen o se desactivan se
// sueltan; forget() lo hace al momento (hooks.destroying de los scripts).

#include "CramionCore/video/VideoSystem.h"

#include "CramionCore/audio/Audio.h"
#include "CramionCore/audio/AudioStream.h"
#include "CramionCore/video/VideoFiles.h"
#include "VideoWorker.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <map>
#include <unordered_map>
#include <utility>

namespace cramion::video {

namespace {

using detail::AudioTrackStream;
using detail::QueuedFrame;
using detail::VideoWorker;
using Workers = std::vector<std::unique_ptr<VideoWorker>>;

constexpr double kEps = 1e-4;
constexpr std::size_t kMaxEvents = 1024;
constexpr std::uint32_t kPreviewWidth = 960, kPreviewHeight = 540;
constexpr std::chrono::milliseconds kStopBudget{200};

std::uint32_t maxHeight(MaxResolution r) {
    switch (r) {
        case MaxResolution::P2160: return 2160;
        case MaxResolution::P1440: return 1440;
        case MaxResolution::P1080: return 1080;
        case MaxResolution::P720: return 720;
        case MaxResolution::P480: return 480;
        case MaxResolution::P360: return 360;
        default: return 0;
    }
}

// Tamano de la textura propia: el del video, reducido a la resolucion maxima.
void fitSize(std::uint32_t w, std::uint32_t h, MaxResolution r, std::uint32_t& ow, std::uint32_t& oh) {
    ow = w;
    oh = h;
    const std::uint32_t limit = maxHeight(r);
    if (limit > 0 && h > limit) {
        oh = limit;
        ow = std::max<std::uint32_t>(2, static_cast<std::uint32_t>(std::lround(static_cast<double>(w) * limit / h)));
    }
    ow = std::max<std::uint32_t>(ow & ~1u, 2);
    oh = std::max<std::uint32_t>(oh & ~1u, 2);
}

std::string sourceKey(const VideoPlayer& c) {
    return (c.source == VideoSource::Url ? "u:" + c.url : "c:" + c.clip) + "|" + std::to_string(c.start_time) + "|" +
           std::to_string(c.end_time);
}

std::string normalizedClip(std::string s) {
    std::replace(s.begin(), s.end(), '\\', '/');
    for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

bool sameVoice(const audio::StreamVoice& a, const audio::StreamVoice& b) {
    return a.emitter == b.emitter && a.use_source_settings == b.use_source_settings && a.spatial == b.spatial &&
           a.min_distance == b.min_distance && a.max_distance == b.max_distance && a.volume == b.volume && a.pitch == b.pitch;
}

bool sameOverride(const MaterialOverride& a, const MaterialOverride& b) {
    return a.slot == b.slot && a.property == b.property && a.texture == b.texture && a.emissive_intensity == b.emissive_intensity;
}

void finishAll(Workers& workers) {
    for (auto& w : workers) w->requestStop();
    const auto deadline = std::chrono::steady_clock::now() + kStopBudget;
    for (auto& w : workers) w->finish(deadline);
    workers.clear();
}

}  // namespace

struct VideoSystem::Impl {
    struct Voice {
        int track = -1;
        entt::entity emitter = entt::null;
        std::shared_ptr<audio::AudioStream> stream;
        int handle = -1;
        audio::StreamVoice params;
    };
    struct ApiReader {
        std::shared_ptr<audio::AudioStream> stream;
        int reader = -1;
    };

    struct Player {
        entt::entity handle = entt::null;
        bool preview = false;
        std::string key;  // fuente + seccion (si cambia, se reabre)
        std::unique_ptr<VideoWorker> worker;
        PlayerState state = PlayerState::Idle;
        bool want_play = false;
        bool prepared_seen = false;
        bool dirty = false;  // invalidateFile
        StreamInfo info;
        double start = 0.0;
        double fd = 1.0 / 30.0;
        double length = -1.0;
        double T = 0.0;  // reloj: linea de tiempo (s desde start_time, con las vueltas)
        int laps = 0;
        bool seeking = false;
        double audio_idle = 0.0;
        double last_audio = -1.0;
        bool has_frame = false;
        double last_tl = 0.0;
        double last_pts = 0.0;
        VideoFrame last_frame;  // para mandarla otra vez si la salida llega tarde
        std::uint64_t dropped = 0;
        std::string error;
        // Salida
        std::string output_key;
        bool output_ready = false;
        std::int32_t texture = -1;
        bool own_texture = false;
        entt::entity override_target = entt::null;
        MaterialOverride last_override;
        bool plane_set = false;
        bool plane_near = false;
        float plane_alpha = -1.0f;
        AspectMode plane_aspect = AspectMode::FitInside;
        // Audio
        std::uint64_t streams_version = ~0ull;
        std::vector<AudioTrackStream> streams;
        std::vector<Voice> voices;
        std::map<int, ApiReader> api_readers;
        std::shared_ptr<audio::AudioStream> clock_stream;
    };

    Host host;
    bool running = false;
    bool paused = false;
    ecs::World* world = nullptr;
    std::uint64_t generation = 0;
    std::unordered_map<entt::entity, std::unique_ptr<Player>> players;
    std::vector<VideoEvent> events;

    std::unique_ptr<Player> preview;
    VideoPlayer preview_settings;
    entt::entity preview_entity = entt::null;
    ecs::World* preview_world = nullptr;
    std::int32_t preview_texture = -1;

    // --- Eventos ---
    void emit(const Player& p, VideoEventType type, double time, int count = 0, std::string message = {}) {
        if (p.preview || world == nullptr) return;
        if (events.size() >= kMaxEvents) events.erase(events.begin());
        VideoEvent ev;
        ev.entity = world->wrap(p.handle);
        ev.type = type;
        ev.time = time;
        ev.count = count;
        ev.message = std::move(message);
        events.push_back(std::move(ev));
    }

    double videoTime(const Player& p) const {
        double t = p.T;
        if (p.length > 0.0 && p.laps > 0) t -= p.laps * p.length;
        return p.start + std::max(0.0, t);
    }

    // --- Componentes ---
    VideoPlayer* settingsOf(Player& p, ecs::World* w) {
        if (p.preview) return &preview_settings;
        if (w == nullptr || !w->valid(p.handle)) return nullptr;
        return w->registry().try_get<VideoPlayer>(p.handle);
    }

    ecs::Entity selfOf(const Player& p, ecs::World* w) const {
        if (p.preview) return preview_world != nullptr && preview_entity != entt::null && preview_world->valid(preview_entity)
                                  ? preview_world->wrap(preview_entity)
                                  : ecs::Entity{};
        return w != nullptr ? w->wrap(p.handle) : ecs::Entity{};
    }

    // --- Abrir y soltar ---
    void open(Player& p, const VideoPlayer& c) {
        Workers old;
        release(p, old);
        finishAll(old);
        p.key = sourceKey(c);
        p.error.clear();
        std::string error;
        const std::string uri = resolveSource(c, host.assets_root, &error);
        if (uri.empty()) {
            fail(p, error.empty() ? std::string("Video: fuente vacia") : error);
            return;
        }
        detail::WorkerConfig cfg;
        cfg.uri = uri;
        cfg.options.mode = c.decode;
        if (const std::uint32_t h = maxHeight(c.max_resolution); h > 0) {
            cfg.options.max_height = h;
            cfg.options.max_width = (h * 16 + 8) / 9;
        }
        cfg.options.low_latency = c.low_latency;
        const bool wants_audio = c.audio_output == AudioOutput::ApiOnly || (host.audio != nullptr && c.audio_output != AudioOutput::None);
        if (wants_audio) {
            if (c.tracks.empty()) {
                cfg.options.audio_tracks.push_back(0);
            } else {
                for (std::size_t i = 0; i < c.tracks.size(); ++i) {
                    if (c.tracks[i].enabled) cfg.options.audio_tracks.push_back(static_cast<int>(i));
                }
            }
        }
        cfg.start_time = std::max(0.0f, c.start_time);
        cfg.end_time = c.end_time > c.start_time ? c.end_time : 0.0;
        cfg.loop = c.loop;
        cfg.skip_on_drop = c.skip_on_drop;
        cfg.max_queued = std::clamp(c.max_queued_frames, 2, 8);
        cfg.audio_seconds = std::clamp(static_cast<double>(c.buffer_seconds), 0.25, 10.0);
        p.start = cfg.start_time;
        p.worker = std::make_unique<VideoWorker>(std::move(cfg));
        p.state = PlayerState::Preparing;
        p.prepared_seen = false;
        p.T = 0.0;
        p.laps = 0;
        p.seeking = false;
        p.has_frame = false;
        p.audio_idle = 0.0;
        p.last_audio = -1.0;
        p.length = -1.0;
        p.dirty = false;
    }

    void fail(Player& p, const std::string& message) {
        p.error = message;
        if (p.state != PlayerState::Error) emit(p, VideoEventType::Error, videoTime(p), 0, message);
        p.state = PlayerState::Error;
        p.want_play = false;
    }

    void clearOutput(Player& p) {
        VideoOutput* out = host.output;
        if (out != nullptr) {
            if (p.override_target != entt::null) out->clearMaterialOverride(p.override_target);
            if (p.plane_set) out->setCameraPlane(p.plane_near, -1, 0.0f, p.plane_aspect);
            if (p.own_texture && p.texture >= 0) out->destroyTexture(p.texture);
            else if (p.texture >= 0) out->releaseTexture(p.texture);  // .crrt o vista previa: siguen vivas
        }
        p.override_target = entt::null;
        p.plane_set = false;
        p.plane_alpha = -1.0f;
        p.own_texture = false;
        p.texture = -1;
        p.output_ready = false;
        p.output_key.clear();
    }

    void releaseAudio(Player& p) {
        for (Voice& v : p.voices) {
            if (v.handle >= 0 && host.audio != nullptr) host.audio->detachStream(v.handle);
        }
        p.voices.clear();
        for (auto& [track, r] : p.api_readers) {
            if (r.stream && r.reader >= 0) r.stream->removeReader(r.reader);
        }
        p.api_readers.clear();
        if (p.worker) {
            for (const AudioTrackStream& s : p.streams) p.worker->setMasterAttached(s.track, false);
        }
        p.streams.clear();
        p.streams_version = ~0ull;
        p.clock_stream.reset();
    }

    // Suelta todo; el hilo va a `workers` (se espera a todos juntos).
    void release(Player& p, Workers& workers) {
        releaseAudio(p);
        clearOutput(p);
        if (p.worker) {
            p.worker->requestStop();
            workers.push_back(std::move(p.worker));
        }
        p.last_frame = VideoFrame{};
        p.prepared_seen = false;
        p.has_frame = false;
    }

    // --- Controles ---
    void play(Player& p, const VideoPlayer& c) {
        if (!p.worker || p.state == PlayerState::Error || p.state == PlayerState::Idle) {
            open(p, c);
            if (p.state == PlayerState::Error) return;
            p.want_play = true;
            return;
        }
        p.want_play = true;
        switch (p.state) {
            case PlayerState::Prepared:
            case PlayerState::Paused:
                p.state = PlayerState::Playing;
                emit(p, VideoEventType::Started, videoTime(p));
                break;
            case PlayerState::Ended:
                seek(p, c, p.start);
                p.state = PlayerState::Playing;
                emit(p, VideoEventType::Started, videoTime(p));
                break;
            default: break;  // Preparing: al prepararse; Playing/Buffering: ya
        }
    }

    void pause(Player& p) {
        p.want_play = false;
        if (p.state == PlayerState::Playing || p.state == PlayerState::Buffering) p.state = PlayerState::Paused;
        applyStreamPause(p);
    }

    void stopOne(Player& p) {
        Workers old;
        release(p, old);
        finishAll(old);
        p.state = PlayerState::Idle;
        p.want_play = false;
        p.T = 0.0;
        p.laps = 0;
        p.seeking = false;
    }

    void prepare(Player& p, const VideoPlayer& c) {
        if (!p.worker || p.state == PlayerState::Error || p.state == PlayerState::Idle) open(p, c);
    }

    void seek(Player& p, const VideoPlayer& c, double seconds) {
        if (!p.worker || p.state == PlayerState::Error || p.state == PlayerState::Idle) open(p, c);
        if (!p.worker) return;
        if (p.prepared_seen && (!p.info.seekable || p.info.is_live)) return;
        p.T = p.worker->seek(seconds);
        p.laps = 0;
        p.seeking = true;
        p.audio_idle = 0.0;
        p.last_audio = -1.0;
        if (p.state == PlayerState::Ended) p.state = p.want_play ? PlayerState::Playing : PlayerState::Paused;
        applyStreamPause(p);
    }

    void stepForward(Player& p, const VideoPlayer& c, int frames) {
        if (!p.worker || p.state == PlayerState::Idle || p.state == PlayerState::Error) {
            prepare(p, c);
            return;
        }
        if (p.state == PlayerState::Playing || p.state == PlayerState::Buffering) pause(p);
        p.want_play = false;
        p.T += std::max(1, frames) * p.fd;
        if (p.length > 0.0) p.T = std::min(p.T, (p.laps + 1) * p.length - 0.5 * p.fd);
    }

    // --- Salida (textura, material, planos) ---
    std::int32_t previewTex() {
        if (preview_texture < 0 && host.output != nullptr) preview_texture = host.output->createTexture(kPreviewWidth, kPreviewHeight);
        return preview_texture;
    }

    void syncOutput(Player& p, const VideoPlayer& c, ecs::World* w, ecs::Entity self) {
        VideoOutput* out = host.output;
        if (out == nullptr || !p.prepared_seen || p.info.width == 0 || p.info.height == 0) return;
        const RenderMode mode = p.preview ? RenderMode::ApiOnly : c.render_mode;
        std::string key = std::to_string(static_cast<int>(mode));
        if (mode == RenderMode::RenderTexture) key += "|" + c.target_texture.uuid.toString();
        if (mode == RenderMode::MaterialOverride) key += "|" + c.target_renderer.toString();
        if (!p.preview && mode != RenderMode::RenderTexture) key += "|" + std::to_string(static_cast<int>(c.max_resolution));
        if (key != p.output_key) {
            clearOutput(p);
            p.output_key = key;
        }
        if (!p.output_ready) {
            std::int32_t tex = -1;
            bool own = false;
            if (p.preview) {
                tex = previewTex();
            } else if (mode == RenderMode::RenderTexture) {
                if (c.target_texture.valid()) tex = out->textureForRenderTexture(c.target_texture.uuid);
            } else {
                std::uint32_t tw = 0, th = 0;
                fitSize(p.info.width, p.info.height, c.max_resolution, tw, th);
                tex = out->createTexture(tw, th);
                own = tex >= 0;
            }
            if (tex < 0) return;  // la .crrt aun no esta cargada: se reintenta
            p.texture = tex;
            p.own_texture = own;
            p.output_ready = true;
            if (p.has_frame && p.last_frame.planes[0] != nullptr) out->submitFrame(p.texture, p.last_frame, convert(p, c));
        }
        if (mode == RenderMode::MaterialOverride && w != nullptr) {
            entt::entity target = self.valid() ? self.handle() : entt::null;
            if (c.target_renderer.valid()) {
                const ecs::Entity e = w->find(c.target_renderer);
                target = e.valid() ? e.handle() : entt::null;
            }
            if (target != p.override_target && p.override_target != entt::null) {
                out->clearMaterialOverride(p.override_target);
                p.override_target = entt::null;
            }
            if (target != entt::null) {
                MaterialOverride o;
                o.slot = c.material_slot;
                o.property = c.material_property;
                o.texture = p.texture;
                o.emissive_intensity = c.emissive_intensity;
                if (target != p.override_target || !sameOverride(o, p.last_override)) {
                    out->setMaterialOverride(target, o);
                    p.override_target = target;
                    p.last_override = o;
                }
            }
        } else if ((mode == RenderMode::CameraFarPlane || mode == RenderMode::CameraNearPlane) && !p.preview) {
            const bool near_plane = mode == RenderMode::CameraNearPlane;
            const float alpha = std::clamp(c.camera_alpha, 0.0f, 1.0f);
            if (!p.plane_set || p.plane_near != near_plane || p.plane_alpha != alpha || p.plane_aspect != c.aspect) {
                out->setCameraPlane(near_plane, p.texture, alpha, c.aspect);
                p.plane_set = true;
                p.plane_near = near_plane;
                p.plane_alpha = alpha;
                p.plane_aspect = c.aspect;
            }
        }
    }

    ConvertParams convert(const Player& p, const VideoPlayer& c) const {
        ConvertParams params;
        const RenderMode mode = p.preview ? RenderMode::ApiOnly : c.render_mode;
        // Los planos aplican la proporcion al dibujar (su textura ya tiene la del video).
        params.aspect = p.preview ? AspectMode::FitInside
                        : (mode == RenderMode::CameraFarPlane || mode == RenderMode::CameraNearPlane) ? AspectMode::Stretch
                                                                                                        : c.aspect;
        params.alpha = 1.0f;
        params.linear_out = !p.preview && mode == RenderMode::ApiOnly && c.color == ColorOutput::Linear;
        return params;
    }

    void submit(Player& p, const VideoPlayer& c, const VideoFrame& frame) {
        p.last_frame = frame;
        if (host.output != nullptr && p.output_ready && p.texture >= 0) host.output->submitFrame(p.texture, frame, convert(p, c));
    }

    // --- Audio ---
    void applyStreamPause(Player& p) {
        const bool audible = !paused && !p.seeking && p.state == PlayerState::Playing;
        for (const AudioTrackStream& s : p.streams) {
            if (s.stream) s.stream->paused.store(!audible);
        }
    }

    void syncAudio(Player& p, const VideoPlayer& c, ecs::World* w, ecs::Entity self) {
        if (!p.worker) return;
        const std::uint64_t version = p.worker->streamsVersion();
        if (version != p.streams_version) {
            p.streams = p.worker->streams();
            p.streams_version = version;
        }
        const float speed = std::clamp(c.playback_speed, 0.1f, 4.0f);
        const bool out_of_range = speed < 0.5f || speed > 2.0f;
        AudioOutput mode = c.audio_output;
        if (p.preview && mode == AudioOutput::AudioSource) mode = AudioOutput::Direct;  // vista previa: 2D

        // Voces que deberian estar.
        std::vector<Voice> desired;
        if (host.audio != nullptr && (mode == AudioOutput::Direct || mode == AudioOutput::AudioSource)) {
            for (const AudioTrackStream& s : p.streams) {
                if (!s.stream) continue;
                const VideoAudioTrack* tr =
                    s.track >= 0 && static_cast<std::size_t>(s.track) < c.tracks.size() ? &c.tracks[static_cast<std::size_t>(s.track)] : nullptr;
                const bool silent = out_of_range || (tr != nullptr && (tr->mute || !tr->enabled));
                const float volume = silent ? 0.0f : std::max(0.0f, c.volume * (tr != nullptr ? tr->volume : 1.0f));
                if (mode == AudioOutput::Direct) {
                    Voice v;
                    v.track = s.track;
                    v.stream = s.stream;
                    v.emitter = self.valid() ? self.handle() : entt::null;
                    v.params.emitter = self;
                    v.params.use_source_settings = false;
                    v.params.spatial = c.direct_spatial && !p.preview;
                    v.params.min_distance = c.direct_min_distance;
                    v.params.max_distance = std::max(c.direct_max_distance, c.direct_min_distance + 0.01f);
                    v.params.volume = volume;
                    v.params.pitch = speed;
                    desired.push_back(std::move(v));
                } else if (tr != nullptr && w != nullptr) {
                    for (const Uuid& id : tr->targets) {
                        const ecs::Entity e = w->find(id);
                        if (!e.valid() || !e.has<audio::AudioSource>()) continue;
                        const bool dup = std::any_of(desired.begin(), desired.end(),
                                                     [&](const Voice& d) { return d.track == s.track && d.emitter == e.handle(); });
                        if (dup) continue;
                        Voice v;
                        v.track = s.track;
                        v.stream = s.stream;
                        v.emitter = e.handle();
                        v.params.emitter = e;
                        v.params.use_source_settings = true;
                        v.params.spatial = true;
                        v.params.volume = volume;
                        v.params.pitch = speed;
                        desired.push_back(std::move(v));
                    }
                }
            }
        }
        // Fuera las que ya no estan (o cuyo anillo cambio).
        for (auto it = p.voices.begin(); it != p.voices.end();) {
            const bool keep = std::any_of(desired.begin(), desired.end(), [&](const Voice& d) {
                return d.track == it->track && d.emitter == it->emitter && d.stream == it->stream;
            });
            if (keep) {
                ++it;
                continue;
            }
            if (it->handle >= 0 && host.audio != nullptr) host.audio->detachStream(it->handle);
            it = p.voices.erase(it);
        }
        // Nuevas y cambios.
        for (Voice& d : desired) {
            auto it = std::find_if(p.voices.begin(), p.voices.end(), [&](const Voice& v) {
                return v.track == d.track && v.emitter == d.emitter && v.stream == d.stream;
            });
            if (it != p.voices.end()) {
                if (it->handle >= 0 && !sameVoice(it->params, d.params)) {
                    d.params.master = it->params.master;
                    host.audio->updateStream(it->handle, d.params);
                    it->params = d.params;
                }
                continue;
            }
            const bool first = std::none_of(p.voices.begin(), p.voices.end(),
                                            [&](const Voice& v) { return v.stream == d.stream && v.handle >= 0; });
            d.params.master = first;
            if (first) d.stream->setOutputLatency(host.audio->outputLatencySeconds());
            d.handle = host.audio->attachStream(d.stream, d.params);
            p.voices.push_back(std::move(d));
        }
        // El anillo frena al hilo solo si alguien lo consume; el reloj, la primera pista con voz.
        p.clock_stream.reset();
        for (const AudioTrackStream& s : p.streams) {
            const bool attached = std::any_of(p.voices.begin(), p.voices.end(),
                                              [&](const Voice& v) { return v.stream == s.stream && v.handle >= 0; });
            p.worker->setMasterAttached(s.track, attached);
            if (attached && !p.clock_stream && !out_of_range) p.clock_stream = s.stream;
        }
        // ApiOnly: un lector propio por pista (no maestro: si el script no lee, no frena el video).
        if (mode == AudioOutput::ApiOnly) {
            for (const AudioTrackStream& s : p.streams) {
                ApiReader& r = p.api_readers[s.track];
                if (r.stream == s.stream && r.reader >= 0) continue;
                if (r.stream && r.reader >= 0) r.stream->removeReader(r.reader);
                r.stream = s.stream;
                r.reader = s.stream ? s.stream->addReader(false) : -1;
            }
        } else if (!p.api_readers.empty()) {
            for (auto& [track, r] : p.api_readers) {
                if (r.stream && r.reader >= 0) r.stream->removeReader(r.reader);
            }
            p.api_readers.clear();
        }
        applyStreamPause(p);
    }

    // --- Un frame de un reproductor ---
    void present(Player& p, const VideoPlayer& c, bool game_clock) {
        double target = p.T + kEps;
        if (p.state == PlayerState::Ended && p.length > 0.0) target = (p.laps + 1) * p.length - 1e-6;
        QueuedFrame q;
        int skipped = 0;
        const bool only_one = !c.skip_on_drop && p.has_frame;
        if (p.worker->takeUpTo(target, q, skipped, only_one)) {
            submit(p, c, q.frame);
            p.has_frame = true;
            p.last_tl = q.timeline;
            p.last_pts = q.frame.pts;
            // Sin saltar frames: el reloj de juego espera al video.
            if (only_one && game_clock && p.T > q.timeline + 2.0 * p.fd) p.T = q.timeline + p.fd;
            if (p.seeking) {
                p.seeking = false;
                emit(p, VideoEventType::SeekCompleted, videoTime(p));
                applyStreamPause(p);
            }
            if (c.send_frame_ready) {
                emit(p, VideoEventType::FrameReady, q.frame.pts, static_cast<int>(std::llround(q.frame.pts * (p.info.fps > 0 ? p.info.fps : 30.0))));
            }
        }
        const std::uint64_t total = static_cast<std::uint64_t>(skipped) + p.worker->takeWorkerDropped();
        if (total > 0) {
            p.dropped += total;
            emit(p, VideoEventType::FrameDropped, videoTime(p), static_cast<int>(std::min<std::uint64_t>(total, 1u << 30)));
        }
    }

    void tick(Player& p, VideoPlayer& c, ecs::World* w, float dt, float unscaled_dt) {
        if (!p.worker) return;
        std::string error;
        if (p.worker->failed(&error)) {
            Workers old;
            release(p, old);
            finishAll(old);
            fail(p, error);
            return;
        }
        p.worker->setLoop(c.loop);
        p.worker->setSkipOnDrop(c.skip_on_drop);
        const ecs::Entity self = selfOf(p, w);
        const float speed = std::clamp(c.playback_speed, 0.1f, 4.0f);
        const TimeSource source = p.preview ? TimeSource::GameTime : c.time_source;
        const double step = (source == TimeSource::UnscaledTime ? unscaled_dt : dt) * static_cast<double>(speed);

        if (!p.prepared_seen) {
            if (!p.worker->prepared()) {
                // Sin esperar al primer frame, el reloj ya corre (se saltara lo que llegue tarde).
                if (p.want_play && !c.wait_for_first_frame && !paused) {
                    if (p.state != PlayerState::Playing) {
                        p.state = PlayerState::Playing;
                        emit(p, VideoEventType::Started, videoTime(p));
                    }
                    p.T += step;
                    p.worker->setClockHint(p.T);
                } else if (p.state != PlayerState::Playing) {
                    p.state = PlayerState::Preparing;
                }
                return;
            }
            p.prepared_seen = true;
            p.info = p.worker->info();
            p.start = p.worker->startTime();  // "Empezar en" mas alla del final: el hilo empieza en 0
            p.fd = p.info.fps > 0.0 ? 1.0 / p.info.fps : 1.0 / 30.0;
            emit(p, VideoEventType::Prepared, videoTime(p));
            if (p.want_play) {
                if (p.state != PlayerState::Playing) emit(p, VideoEventType::Started, videoTime(p));
                p.state = PlayerState::Playing;
            } else {
                p.state = PlayerState::Prepared;
            }
        }
        p.length = p.worker->segmentLength();
        syncOutput(p, c, w, self);
        syncAudio(p, c, w, self);

        // Reloj.
        const bool clock_runs = p.state == PlayerState::Playing && !paused && !p.seeking;
        bool game_clock = true;
        if (clock_runs) {
            if (source == TimeSource::AudioClock && p.clock_stream) {
                const double a = p.clock_stream->playedSeconds();
                if (a > p.last_audio + 1e-6) {
                    p.audio_idle = 0.0;
                    p.T = std::max(p.T, a);
                    game_clock = false;
                } else {
                    // El audio no avanza (pista mas corta, sin datos): un rato, se espera; despues, tiempo de juego.
                    p.audio_idle += dt;
                    if (p.audio_idle > 0.2) p.T += step;
                    else game_clock = false;
                }
                p.last_audio = std::max(p.last_audio, a);
            } else {
                p.T += step;
            }
        }
        p.worker->setClockHint(p.T);

        // Fin de la seccion: otra vuelta o se queda al final.
        if (p.length > 0.0 && (p.state == PlayerState::Playing || p.state == PlayerState::Buffering)) {
            if (c.loop) {
                while (p.T >= (p.laps + 1) * p.length) {
                    ++p.laps;
                    emit(p, VideoEventType::LoopPointReached, p.start + p.length, p.laps);
                }
            } else if (p.T >= (p.laps + 1) * p.length) {
                p.T = (p.laps + 1) * p.length;
                p.state = PlayerState::Ended;
                p.want_play = false;
                emit(p, VideoEventType::LoopPointReached, p.start + p.length, p.laps + 1);
            }
        }
        // Un video sin longitud conocida (directo que termina): al acabarse los frames.
        if (p.length <= 0.0 && p.state == PlayerState::Playing && p.worker->endOfStream() && p.worker->queued() == 0 && p.has_frame &&
            p.T > p.last_tl + p.fd) {
            p.state = PlayerState::Ended;
            p.want_play = false;
            emit(p, VideoEventType::LoopPointReached, videoTime(p), 1);
        }

        present(p, c, game_clock);

        // Buffering: sin frames y el reloj ya va bastante por delante.
        if (p.state == PlayerState::Playing && !paused) {
            const bool empty = p.worker->queued() == 0 && !p.worker->endOfStream();
            p.worker->setStarving(empty && p.has_frame);
            if (empty && p.has_frame && !p.seeking && p.T - p.last_tl > std::max(0.5, 3.0 * p.fd)) {
                p.state = PlayerState::Buffering;
                emit(p, VideoEventType::Buffering, videoTime(p));
            }
        } else if (p.state == PlayerState::Buffering) {
            const std::size_t enough = std::min<std::size_t>(2, static_cast<std::size_t>(std::clamp(c.max_queued_frames, 2, 8)));
            if (p.worker->queued() >= enough || p.worker->endOfStream()) {
                p.state = PlayerState::Playing;
                p.worker->setStarving(false);
                emit(p, VideoEventType::Resumed, videoTime(p));
            }
        } else {
            p.worker->setStarving(false);
        }
        applyStreamPause(p);
    }

    PlayerStatus statusOf(const Player& p) const {
        PlayerStatus s;
        s.state = p.state;
        s.time = videoTime(p);
        s.length = p.info.duration;
        s.fps = p.info.fps;
        const double fps = p.info.fps > 0.0 ? p.info.fps : 0.0;
        s.frame = p.has_frame ? static_cast<std::int64_t>(std::llround(p.last_pts * fps)) : static_cast<std::int64_t>(std::llround(s.time * fps));
        s.frame_count = p.info.duration > 0.0 ? static_cast<std::int64_t>(std::llround(p.info.duration * fps)) : 0;
        s.width = p.info.width;
        s.height = p.info.height;
        s.is_live = p.info.is_live;
        s.hardware = p.info.hardware;
        s.dropped = p.dropped;
        s.buffered_seconds = p.worker ? p.worker->queuedSeconds() : 0.0;
        s.codec = p.info.codec;
        s.backend = p.info.backend;
        s.error = p.error;
        s.audio_tracks = static_cast<int>(p.info.audio.size());
        for (const AudioTrackStream& t : p.streams) {
            if (t.stream) {
                s.audio_rate = t.stream->rate();
                s.audio_channels = t.stream->channels();
                break;
            }
        }
        s.audio_voices = static_cast<int>(std::count_if(p.voices.begin(), p.voices.end(), [](const Voice& v) { return v.handle >= 0; }));
        return s;
    }

    // --- Busqueda de reproductores ---
    // El de la entidad en Play (se crea si aun no lo vio update) o la vista previa.
    Player* find(ecs::Entity e, bool create) {
        if (running && world != nullptr && e.valid() && e.world() == world) {
            if (const auto it = players.find(e.handle()); it != players.end()) return it->second.get();
            if (create && e.has<VideoPlayer>() && e.activeInHierarchy()) return add(e);
        }
        if (preview && (preview_entity == (e.valid() ? e.handle() : entt::entity{entt::null}))) return preview.get();
        return nullptr;
    }

    Player* add(ecs::Entity e) {
        auto p = std::make_unique<Player>();
        p->handle = e.handle();
        Player* raw = p.get();
        players[e.handle()] = std::move(p);
        VideoPlayer& c = e.get<VideoPlayer>();
        raw->key = sourceKey(c);
        if (c.play_on_awake) play(*raw, c);
        return raw;
    }

    const VideoPlayer* settingsFor(Player& p, ecs::Entity e) {
        if (p.preview) return &preview_settings;
        return e.valid() ? e.tryGet<VideoPlayer>() : nullptr;
    }

    void stopPlayers() {
        Workers old;
        for (auto& [h, p] : players) release(*p, old);
        players.clear();
        events.clear();
        finishAll(old);
    }

    void stopPreviewNow() {
        if (!preview) return;
        Workers old;
        release(*preview, old);
        preview.reset();
        preview_entity = entt::null;
        preview_world = nullptr;
        finishAll(old);
    }
};

VideoSystem::VideoSystem() : impl_(std::make_unique<Impl>()) {}

VideoSystem::~VideoSystem() {
    stop();
    if (impl_->host.output != nullptr && impl_->preview_texture >= 0) impl_->host.output->destroyTexture(impl_->preview_texture);
}

void VideoSystem::setHost(const Host& host) {
    Impl& d = *impl_;
    if (host.output != d.host.output) {
        // Las texturas son de la salida anterior: se sueltan con ella.
        for (auto& [h, p] : d.players) d.clearOutput(*p);
        if (d.preview) d.clearOutput(*d.preview);
        if (d.host.output != nullptr && d.preview_texture >= 0) d.host.output->destroyTexture(d.preview_texture);
        d.preview_texture = -1;
    }
    if (host.audio != d.host.audio) {
        for (auto& [h, p] : d.players) d.releaseAudio(*p);
        if (d.preview) d.releaseAudio(*d.preview);
    }
    d.host = host;
}

void VideoSystem::start(ecs::World& world) {
    Impl& d = *impl_;
    d.stopPreviewNow();  // la vista previa no sigue en Play (memoria: otra instancia del video)
    d.stopPlayers();
    d.running = true;
    d.paused = false;
    d.world = &world;
    d.generation = world.sceneGeneration();
}

void VideoSystem::update(ecs::World& world, float dt, float unscaled_dt) {
    Impl& d = *impl_;
    if (!d.running) return;
    if (&world != d.world || world.sceneGeneration() != d.generation) {
        // Otra escena: lo de la anterior se para sin eventos.
        d.stopPlayers();
        d.world = &world;
        d.generation = world.sceneGeneration();
    }
    if (d.paused) return;
    Workers old;
    // Entidades que ya no estan (destruidas, sin componente o desactivadas).
    for (auto it = d.players.begin(); it != d.players.end();) {
        const entt::entity h = it->first;
        const bool alive = world.valid(h) && world.registry().all_of<VideoPlayer>(h) && world.wrap(h).activeInHierarchy();
        if (alive) {
            ++it;
            continue;
        }
        d.release(*it->second, old);
        it = d.players.erase(it);
    }
    // Las nuevas (Play On Awake al aparecer, como OnEnable).
    std::vector<entt::entity> fresh;
    for (const entt::entity h : world.registry().view<VideoPlayer>()) {
        if (d.players.find(h) == d.players.end() && world.wrap(h).activeInHierarchy()) fresh.push_back(h);
    }
    for (const entt::entity h : fresh) d.add(world.wrap(h));
    for (auto& [h, ptr] : d.players) {
        Impl::Player& p = *ptr;
        VideoPlayer* c = world.registry().try_get<VideoPlayer>(h);
        if (c == nullptr) continue;
        // Otra fuente (o el archivo cambio): se reabre como estaba.
        if (p.worker && (p.dirty || sourceKey(*c) != p.key)) {
            const bool was_playing = p.want_play || p.state == PlayerState::Playing || p.state == PlayerState::Buffering;
            d.open(p, *c);
            p.want_play = was_playing && p.state != PlayerState::Error;
        } else if (!p.worker && sourceKey(*c) != p.key) {
            p.key = sourceKey(*c);
            if (p.state == PlayerState::Error) p.state = PlayerState::Idle;
        }
        d.tick(p, *c, &world, dt, unscaled_dt);
    }
    finishAll(old);
}

void VideoSystem::stop() {
    Impl& d = *impl_;
    d.stopPlayers();
    d.stopPreviewNow();
    d.events.clear();
    d.running = false;
    d.paused = false;
    d.world = nullptr;
}

void VideoSystem::setPaused(bool paused) {
    Impl& d = *impl_;
    d.paused = paused;
    for (auto& [h, p] : d.players) d.applyStreamPause(*p);
}

void VideoSystem::play(ecs::Entity e) {
    Impl& d = *impl_;
    if (Impl::Player* p = d.find(e, true)) {
        if (const VideoPlayer* c = d.settingsFor(*p, e)) d.play(*p, *c);
    }
}

void VideoSystem::pause(ecs::Entity e) {
    Impl& d = *impl_;
    if (Impl::Player* p = d.find(e, true)) d.pause(*p);
}

void VideoSystem::stop(ecs::Entity e) {
    Impl& d = *impl_;
    if (Impl::Player* p = d.find(e, false)) d.stopOne(*p);
}

void VideoSystem::prepare(ecs::Entity e) {
    Impl& d = *impl_;
    if (Impl::Player* p = d.find(e, true)) {
        if (const VideoPlayer* c = d.settingsFor(*p, e)) d.prepare(*p, *c);
    }
}

void VideoSystem::seek(ecs::Entity e, double seconds) {
    Impl& d = *impl_;
    if (Impl::Player* p = d.find(e, true)) {
        if (const VideoPlayer* c = d.settingsFor(*p, e)) d.seek(*p, *c, seconds);
    }
}

void VideoSystem::stepForward(ecs::Entity e, int frames) {
    Impl& d = *impl_;
    if (Impl::Player* p = d.find(e, true)) {
        if (const VideoPlayer* c = d.settingsFor(*p, e)) d.stepForward(*p, *c, frames);
    }
}

void VideoSystem::forget(ecs::Entity e) {
    Impl& d = *impl_;
    if (const auto it = d.players.find(e.handle()); it != d.players.end() && (e.world() == nullptr || e.world() == d.world)) {
        Workers old;
        d.release(*it->second, old);
        d.players.erase(it);
        finishAll(old);
    }
    if (d.preview && d.preview_entity != entt::null && d.preview_entity == e.handle()) d.stopPreviewNow();
}

PlayerStatus VideoSystem::status(ecs::Entity e) const {
    Impl& d = *impl_;
    if (Impl::Player* p = d.find(e, false)) return d.statusOf(*p);
    return {};
}

void VideoSystem::setTrackVolume(ecs::Entity e, int track, float volume) {
    Impl& d = *impl_;
    if (track < 0 || track >= 32) return;
    VideoPlayer* c = nullptr;
    if (Impl::Player* p = d.find(e, false); p != nullptr && p->preview) {
        c = &d.preview_settings;
    } else if (e.valid()) {
        c = e.tryGet<VideoPlayer>();
    }
    if (c == nullptr) return;
    if (c->tracks.size() <= static_cast<std::size_t>(track)) c->tracks.resize(static_cast<std::size_t>(track) + 1);
    c->tracks[static_cast<std::size_t>(track)].volume = std::max(0.0f, volume);
}

std::size_t VideoSystem::readSamples(ecs::Entity e, int track, float* out, std::size_t frames) {
    Impl& d = *impl_;
    Impl::Player* p = d.find(e, false);
    if (p == nullptr || out == nullptr || frames == 0) return 0;
    const auto it = p->api_readers.find(track);
    if (it == p->api_readers.end() || !it->second.stream || it->second.reader < 0) return 0;
    return it->second.stream->read(it->second.reader, out, frames);
}

std::int32_t VideoSystem::textureOf(ecs::Entity e) const {
    Impl& d = *impl_;
    if (Impl::Player* p = d.find(e, false); p != nullptr && p->output_ready) return p->texture;
    return -1;
}

std::vector<VideoEvent> VideoSystem::takeEvents() {
    std::vector<VideoEvent> out;
    out.swap(impl_->events);
    return out;
}

void VideoSystem::previewEntity(ecs::World& world, ecs::Entity e) {
    Impl& d = *impl_;
    d.stopPreviewNow();
    if (!e.valid() || !e.has<VideoPlayer>()) return;
    d.preview_settings = e.get<VideoPlayer>();
    if (d.preview_settings.audio_output == AudioOutput::AudioSource) d.preview_settings.audio_output = AudioOutput::Direct;
    d.preview_settings.direct_spatial = false;
    d.preview_entity = e.handle();
    d.preview_world = &world;
    d.preview = std::make_unique<Impl::Player>();
    d.preview->preview = true;
    d.preview->handle = e.handle();
    d.open(*d.preview, d.preview_settings);
}

void VideoSystem::previewFile(const std::filesystem::path& file) {
    Impl& d = *impl_;
    d.stopPreviewNow();
    d.preview_settings = VideoPlayer{};
    d.preview_settings.source = VideoSource::Clip;
    std::error_code ec;
    const std::filesystem::path absolute = std::filesystem::absolute(file, ec);
    d.preview_settings.clip = pathUtf8(ec ? file : absolute);
    d.preview_settings.render_mode = RenderMode::ApiOnly;
    d.preview_settings.audio_output = AudioOutput::Direct;
    d.preview_settings.direct_spatial = false;
    d.preview_settings.time_source = TimeSource::GameTime;
    d.preview = std::make_unique<Impl::Player>();
    d.preview->preview = true;
    d.open(*d.preview, d.preview_settings);
}

void VideoSystem::updatePreview(float dt) {
    Impl& d = *impl_;
    if (!d.preview) return;
    Impl::Player& p = *d.preview;
    if (p.dirty && p.worker) {
        const bool was_playing = p.want_play || p.state == PlayerState::Playing;
        d.open(p, d.preview_settings);
        p.want_play = was_playing && p.state != PlayerState::Error;
    }
    ecs::World* w = d.preview_world;
    if (w != nullptr && (d.preview_entity == entt::null || !w->valid(d.preview_entity))) w = nullptr;
    d.tick(p, d.preview_settings, w, dt, dt);
}

void VideoSystem::stopPreview() { impl_->stopPreviewNow(); }

bool VideoSystem::previewing() const { return impl_->preview != nullptr; }

std::int32_t VideoSystem::previewTexture() const { return impl_->previewTex(); }

PlayerStatus VideoSystem::previewStatus() const {
    if (!impl_->preview) return {};
    return impl_->statusOf(*impl_->preview);
}

void VideoSystem::invalidateFile(const std::string& relative) {
    Impl& d = *impl_;
    const std::string rel = normalizedClip(relative);
    if (rel.empty()) return;
    const auto matches = [&](const std::string& clip) {
        const std::string c = normalizedClip(clip);
        return c == rel || (c.size() > rel.size() && c.compare(c.size() - rel.size(), rel.size(), rel) == 0 &&
                            c[c.size() - rel.size() - 1] == '/');
    };
    for (auto& [h, p] : d.players) {
        // La clave guarda "c:<clip>|inicio|fin".
        if (p->key.rfind("c:", 0) == 0) {
            const std::string clip = p->key.substr(2, p->key.find('|') - 2);
            if (matches(clip)) p->dirty = true;
        }
    }
    if (d.preview && d.preview_settings.source == VideoSource::Clip && matches(d.preview_settings.clip)) d.preview->dirty = true;
}

}  // namespace cramion::video
