#ifndef CRAMION_VIDEO_SYSTEM_H
#define CRAMION_VIDEO_SYSTEM_H

// Contrato D. Lo implementa WP1; lo usan el editor, el player y los scripts.
// Orden de parada: VideoSystem::stop() SIEMPRE antes de AudioSystem::stop()
// (aqui se sueltan los handles de stream). El cambio de escena tambien se
// detecta en update() con World::sceneGeneration().

#include "CramionCore/ecs/World.h"
#include "CramionCore/video/VideoOutput.h"
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace cramion::audio { class AudioSystem; }

namespace cramion::video {

enum class PlayerState : int { Idle = 0, Preparing, Prepared, Playing, Paused, Buffering, Ended, Error };
enum class VideoEventType : int { Prepared = 0, Started, LoopPointReached, FrameReady, SeekCompleted,
                                  Error, FrameDropped, Buffering, Resumed };

struct VideoEvent {
    ecs::Entity entity;
    VideoEventType type = VideoEventType::Prepared;
    double time = 0.0;
    int count = 0;
    std::string message;
};

struct PlayerStatus {
    PlayerState state = PlayerState::Idle;
    double time = 0.0, length = -1.0, fps = 0.0;
    std::int64_t frame = 0, frame_count = 0;
    std::uint32_t width = 0, height = 0;
    bool is_live = false, hardware = false;
    std::uint64_t dropped = 0;
    double buffered_seconds = 0.0;
    std::string codec, backend, error;
    int audio_tracks = 0;
    std::uint32_t audio_rate = 0, audio_channels = 0;  // formato de readSamples (nativo de la pista)
    int audio_voices = 0;                              // voces de stream enganchadas (Direct/AudioSource)
};

class VideoSystem {
public:
    struct Host {
        audio::AudioSystem* audio = nullptr;
        VideoOutput* output = nullptr;
        std::filesystem::path assets_root;
    };
    VideoSystem();
    ~VideoSystem();
    VideoSystem(const VideoSystem&) = delete;
    VideoSystem& operator=(const VideoSystem&) = delete;
    void setHost(const Host& host);

    void start(ecs::World& world);                                  // Play
    void update(ecs::World& world, float dt, float unscaled_dt);    // hilo principal, antes de RenderSync::sync
    void stop();                                                    // fin del Play / cambio de escena (antes de audio.stop)
    void setPaused(bool paused);

    // Controles por entidad. En Play, el reproductor de esa entidad. Fuera de
    // Play (editor), la vista previa: la de previewEntity(e) o, con
    // ecs::Entity{} (invalida), la de previewFile.
    void play(ecs::Entity e);
    void pause(ecs::Entity e);
    void stop(ecs::Entity e);
    void prepare(ecs::Entity e);
    void seek(ecs::Entity e, double seconds);
    void stepForward(ecs::Entity e, int frames = 1);
    // La entidad se va a destruir (hooks.destroying): corta su decodificacion ya.
    void forget(ecs::Entity e);
    PlayerStatus status(ecs::Entity e) const;
    void setTrackVolume(ecs::Entity e, int track, float volume);
    // AudioOutput::ApiOnly: bloques de ~20-50 ms desde el hilo principal (no es
    // DSP en tiempo real; los scripts C++ van por IPC). Formato en status().audio_*.
    std::size_t readSamples(ecs::Entity e, int track, float* out, std::size_t frames);
    std::int32_t textureOf(ecs::Entity e) const;
    std::vector<VideoEvent> takeEvents();

    // Editor sin Play (audio en 2D; textura de vista previa persistente).
    // previewEntity/previewFile preparan (primer frame, en pausa); play/pause/
    // seek/stepForward/status con la entidad (o Entity{}) la controlan.
    // setHost({}) o destruir el sistema ANTES que el VideoOutput (suelta sus texturas).
    void previewEntity(ecs::World& world, ecs::Entity e);
    void previewFile(const std::filesystem::path& file);
    void updatePreview(float dt);
    void stopPreview();
    bool previewing() const;
    std::int32_t previewTexture() const;
    PlayerStatus previewStatus() const;
    void invalidateFile(const std::string& relative);  // el archivo cambio en disco

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace cramion::video
#endif
