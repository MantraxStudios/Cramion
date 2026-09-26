#ifndef CRAMION_CORE_AUDIO_H
#define CRAMION_CORE_AUDIO_H

// Audio como el de Unity (miniaudio por debajo):
//
//   AudioSource    un sonido en una entidad: clip (WAV, MP3, FLAC u OGG en
//                  Assets), volumen, tono, bucle, sonar al empezar y 2D o 3D.
//                  En 3D se atenua con la distancia (min/max, logaritmica o
//                  lineal), se situa a izquierda/derecha y tiene efecto Doppler.
//   AudioListener  los oidos (normalmente en la camara). Sin ninguno, la
//                  camara que se esta viendo.
//   AudioSystem    lo reproduce en Play; los scripts lo usan (play, stop,
//                  playOneShot) y el editor para escuchar un clip.

#include "CramionCore/ecs/Reflection.h"
#include "CramionCore/ecs/World.h"

#include <CramionFX/core/Math.h>

#include <filesystem>
#include <memory>
#include <string>

namespace cramion::audio {

enum class Rolloff : int { Logarithmic = 0, Linear = 1 };

struct AudioSource {
    std::string clip;  // ruta dentro de Assets
    float volume = 1.0f;
    float pitch = 1.0f;
    bool loop = false;
    bool play_on_awake = true;
    bool mute = false;
    bool spatial = true;       // 3D (false = 2D: musica, interfaz)
    float pan = 0.0f;          // 2D: -1 izquierda .. 1 derecha
    float min_distance = 1.0f;   // 3D: hasta aqui, volumen completo
    float max_distance = 50.0f;  // 3D: desde aqui ya no baja mas
    Rolloff rolloff = Rolloff::Logarithmic;
    float doppler = 1.0f;

    void reflect(ecs::PropertyVisitor& v);
};

struct AudioListener {
    float volume = 1.0f;  // volumen general
    void reflect(ecs::PropertyVisitor& v);
};

class AudioSystem {
public:
    AudioSystem();
    ~AudioSystem();
    AudioSystem(const AudioSystem&) = delete;
    AudioSystem& operator=(const AudioSystem&) = delete;

    // Hay dispositivo de audio (si no, todo funciona en silencio).
    bool available() const;
    void setAssetsRoot(const std::filesystem::path& root);

    // Play: suenan los que tienen play_on_awake.
    void start(ecs::World& world);
    // Cada frame: posiciones, oyente, fuentes nuevas o quitadas y los ajustes
    // del Inspector. Sin AudioListener, el oyente es la camara dada.
    void update(ecs::World& world, float delta_seconds, const core::Vec3& camera_position,
                const core::Vec3& camera_forward);
    // Origen flotante (ecs/FloatingOrigin.h): el mundo se desplazo -offset;
    // la ultima posicion de cada fuente y del oyente (si no, el efecto Doppler veria un salto de un kilometro).
    void shiftOrigin(const core::Vec3& offset);
    // Fin del Play: silencio.
    void stop();
    bool running() const;

    void play(ecs::Entity entity);
    void stop(ecs::Entity entity);
    void pause(ecs::Entity entity);
    bool isPlaying(ecs::Entity entity) const;
    // Un sonido suelto (disparo, golpe): en `position` (3D) o sin posicion (2D).
    void playOneShot(const std::string& clip, const core::Vec3& position, float volume = 1.0f, bool spatial = true);

    // Editor: escuchar un archivo (2D) sin Play.
    void preview(const std::filesystem::path& file);
    void stopPreview();
    bool previewing() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Extensiones de audio que se pueden reproducir.
bool isAudioFile(const std::filesystem::path& file);

void registerAudioComponents();

}  // namespace cramion::audio

#endif  // CRAMION_CORE_AUDIO_H
