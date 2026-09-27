#ifndef CRAMION_CORE_AUDIO_H
#define CRAMION_CORE_AUDIO_H

// Audio como el de Unity (miniaudio por debajo):
//
//   AudioSource    un sonido en una entidad: clip (WAV, MP3, FLAC u OGG en
//                  Assets), volumen, tono, bucle, sonar al empezar y 2D o 3D.
//                  En 3D se atenua con la distancia (min/max, logaritmica o
//                  lineal), se situa a izquierda/derecha y tiene efecto Doppler.
//   AudioListener  los oidos (normalmente en la camara). Sin ninguno, la
//                  camara que se esta viendo. Activa la OCLUSION: un sonido
//                  con paredes (colliders) entre el y el oyente se oye tapado
//                  (mas bajo y sin agudos), como detras de una puerta cerrada
//                  o dentro de una casa. Tambien un paso bajo general (bajo
//                  el agua, menus) y el volumen general.
//   Efectos        cada AudioSource: paso bajo, paso alto, eco y cuanto
//                  manda a la reverberacion.
//   AudioReverbZone  una esfera (como la de Unity): con el oyente dentro,
//                  los sonidos reverberan como en esa sala o cueva.
//   AudioSystem    lo reproduce en Play; los scripts lo usan (play, stop,
//                  playOneShot) y el editor para escuchar un clip.

#include "CramionCore/ecs/Reflection.h"
#include "CramionCore/ecs/World.h"

#include <CramionFX/core/Math.h>

#include <filesystem>
#include <cstdint>
#include <functional>
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
    // --- Efectos ---
    bool occlusion = true;         // se tapa con paredes (si el AudioListener tiene la oclusion)
    bool low_pass = false;         // paso bajo: quita agudos (radio, telefono, lejos)
    float low_pass_cutoff = 5000.0f;   // Hz
    bool high_pass = false;        // paso alto: quita graves (radio pequena, altavoz)
    float high_pass_cutoff = 300.0f;   // Hz
    bool echo = false;             // eco
    float echo_delay = 0.35f;      // segundos (hasta 2)
    float echo_decay = 0.45f;      // cada repeticion (0..0.95)
    float echo_wet = 0.5f;         // cuanto eco se oye
    float reverb_send = 1.0f;      // cuanto va a la reverberacion de las AudioReverbZone

    void reflect(ecs::PropertyVisitor& v);
};

struct AudioListener {
    float volume = 1.0f;  // volumen general
    // Oclusion: paredes entre el sonido y el oyente (raycast de fisica).
    bool occlusion = true;
    float occlusion_cutoff = 900.0f;   // Hz: agudos que quedan con una pared
    float occlusion_volume = 0.45f;    // volumen que pasa por cada pared (0..1)
    int occlusion_max_walls = 3;       // mas paredes no tapan mas
    float occlusion_speed = 8.0f;      // rapidez del cambio (abrir/cerrar una puerta)
    // Paso bajo de todo lo que se oye (bajo el agua, menu de pausa).
    bool low_pass = false;
    float low_pass_cutoff = 800.0f;
    void reflect(ecs::PropertyVisitor& v);
};

// Reverberacion por zonas: dentro de min_distance, entera; hasta
// max_distance se mezcla con lo de fuera.
enum class ReverbPreset : int { Custom = 0, Room, Bathroom, Hall, Cave, Arena, Forest, Underwater };

struct AudioReverbZone {
    float min_distance = 5.0f;
    float max_distance = 12.0f;
    ReverbPreset preset = ReverbPreset::Room;
    float room_size = 0.6f;  // 0..1 (Custom): largo de la cola
    float damping = 0.5f;    // 0..1 (Custom): cuanto se apagan los agudos
    float level = 0.5f;      // 0..1: cuanta reverberacion se oye

    void reflect(ecs::PropertyVisitor& v);
};
// Tamano, amortiguacion y nivel de un preset (Custom = los de la zona).
void reverbPresetValues(const AudioReverbZone& zone, float& room_size, float& damping, float& level);

class AudioSystem {
public:
    // Cuantas paredes hay entre `from` (oyente) y `to` (sonido). La pone el
    // juego/editor con la fisica (physicsOcclusionQuery); sin ella, ninguna.
    using OcclusionQuery = std::function<float(const core::Vec3& from, const core::Vec3& to, ecs::Entity source,
                                               ecs::Entity listener)>;
    AudioSystem();
    // Pruebas: sin altavoces; el sonido se saca con renderOffline.
    struct Offline {};
    explicit AudioSystem(Offline);
    ~AudioSystem();
    // Solo sin dispositivo: mezcla `frames` frames (entrelazados, 2 canales).
    void renderOffline(float* out, std::uint32_t frames);
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

    void setOcclusionQuery(OcclusionQuery query);
    // Paredes (suavizado) que tapan el sonido de la entidad ahora; -1 si no suena.
    float occlusionOf(ecs::Entity entity) const;
    // Reverberacion que se oye ahora (0 fuera de toda zona).
    float reverbLevel() const;

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

namespace cramion::physics {
class PhysicsSystem;
}

namespace cramion::audio {

// Oclusion con los colliders de la fisica: cada cuerpo solido que corta la
// linea oyente-sonido es una pared (sin contar el propio sonido, el oyente ni
// sus padres o hijos: el cuerpo del jugador, la carcasa de la radio).
AudioSystem::OcclusionQuery physicsOcclusionQuery(const physics::PhysicsSystem& physics);

}  // namespace cramion::audio

#endif  // CRAMION_CORE_AUDIO_H
