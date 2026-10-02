#ifndef CRAMION_CORE_REPLAY_REPLAY_H
#define CRAMION_CORE_REPLAY_REPLAY_H

// Repeticiones (como el Replay System de Unreal o las repeticiones de Rocket
// League): graba la partida y la vuelve a ver con una linea de tiempo.
//
//   Grabar   cada muestra (30 por segundo por defecto) guarda el Transform
//            local de los objetos que se mueven (o solo los de una etiqueta),
//            si estan activos, el estado y el tiempo de su Animator o de su
//            Motion Matching, y los sonidos que empiezan (Audio Source y
//            Audio.playOneShot). Los objetos que aparecen durante la partida
//            (balas, enemigos) guardan una copia para poder verlos aunque ya
//            no existan.
//   Comprimir cada 30 muestras un fotograma completo; entre medias solo lo que
//            cambia: la posicion como diferencia en enteros de 16 bits (medio
//            milimetro), el giro como cuaternion de "los tres pequenos" en
//            16 bits y la escala solo si cambia. Un objeto quieto no ocupa
//            nada. Se compara con lo que reconstruira el lector: el error no
//            se acumula.
//   Ultimos N segundos: con "max_seconds" la grabacion es circular (instant
//            replay, killcam).
//   Reproducir la fisica y la logica se paran (el juego decide, con
//            Replay.isPlaying()), los objetos se colocan interpolando entre
//            muestras, a cualquier velocidad, hacia delante o saltando
//            (seek); con camara libre (WASD + boton derecho) o la grabada. Al
//            parar, todo vuelve a como estaba.
//   Archivo  .crreplay (binario): cabecera JSON (objetos, eventos, opciones)
//            + tabla de fotogramas + flujo comprimido. Se puede ver despues
//            con la misma escena cargada (los objetos se buscan por UUID).

#include "CramionCore/Uuid.h"

#include <CramionFX/core/Math.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cramion::dm {
class Input;
}
namespace cramion::ecs {
class World;
class Entity;
}
namespace cramion::audio {
class AudioSystem;
}

namespace cramion::replay {

struct ReplayOptions {
    float rate = 30.0f;           // muestras por segundo
    float max_seconds = 0.0f;     // 0 = sin limite; > 0 = solo los ultimos N segundos
    std::string tag;              // solo objetos con esta etiqueta (o hijos de uno); vacio = los que se mueven
    bool record_audio = true;
    bool record_animation = true;
    int keyframe_interval = 30;   // muestras entre fotogramas completos
};

// Eventos con tiempo: sonidos y marcadores (Replay.mark).
struct ReplayEvent {
    enum class Kind : int { Marker = 0, OneShot = 1, SourcePlay = 2, SourceStop = 3 };
    Kind kind = Kind::Marker;
    float time = 0.0f;
    std::string name;   // marcador: su nombre; one-shot: el clip
    std::string data;   // marcador: texto libre
    Uuid entity;        // fuente de audio
    core::Vec3 position{};
    float volume = 1.0f;
    bool spatial = true;
};

struct ReplayInfo {
    float duration = 0.0f;
    int frames = 0;
    int tracks = 0;
    int events = 0;
    std::size_t bytes = 0;  // flujo comprimido
    std::size_t raw_bytes = 0;  // lo que ocuparia sin comprimir
    std::string scene;
};

class ReplaySystem {
public:
    ReplaySystem();
    ~ReplaySystem();
    ReplaySystem(const ReplaySystem&) = delete;
    ReplaySystem& operator=(const ReplaySystem&) = delete;

    // --- Grabar ---
    bool startRecording(ecs::World& world, const ReplayOptions& options = {});
    void stopRecording();
    bool recording() const { return recording_; }
    const ReplayOptions& options() const { return options_; }

    // --- Reproducir ---
    // Guarda como esta todo para dejarlo igual al parar. `from` en segundos
    // desde el principio de lo grabado (negativo = desde el final: -5 = los
    // ultimos 5 segundos).
    bool play(ecs::World& world, float from = 0.0f, float speed = 1.0f);
    void stop(ecs::World& world);
    bool playing() const { return playing_; }
    void setPaused(bool paused) { paused_ = paused; }
    bool paused() const { return paused_; }
    void setSpeed(float speed) { speed_ = speed; }
    float speed() const { return speed_; }
    void setLoop(bool loop) { loop_ = loop; }
    bool loop() const { return loop_; }
    void seek(float seconds);
    float time() const { return time_; }  // segundos desde el principio
    float duration() const;
    // Camara libre: la camara principal deja de seguir lo grabado y se
    // mueve con WASD/QE y el boton derecho del raton (Shift = rapido).
    void setFreeCamera(bool free) { free_camera_ = free; }
    bool freeCamera() const { return free_camera_; }
    void setInput(const dm::Input* input) { input_ = input; }

    // Cada frame, DESPUES de los scripts: graba una muestra o coloca la
    // repeticion. dt = segundos del frame.
    void update(ecs::World& world, float delta_seconds);

    // --- Eventos ---
    void mark(const std::string& name, const std::string& data = {});
    const std::vector<ReplayEvent>& events() const { return events_; }
    // Sonidos: se graban los Audio Source que empiezan/paran y los one-shots
    // (el juego conecta audio::setOneShotListener con notifyOneShot). En la
    // repeticion suenan otra vez.
    void setAudio(audio::AudioSystem* audio) { audio_ = audio; }
    void notifyOneShot(const std::string& clip, const core::Vec3& position, float volume, bool spatial);

    // --- Archivos ---
    // Carpeta de Replay.save("nombre") / load("nombre").
    void setFolder(const std::filesystem::path& folder) { folder_ = folder; }
    const std::filesystem::path& folder() const { return folder_; }
    std::filesystem::path fileFor(const std::string& name) const;
    bool save(const std::filesystem::path& file, std::string* error = nullptr) const;
    bool load(const std::filesystem::path& file, std::string* error = nullptr);
    std::vector<std::string> list() const;  // nombres en la carpeta

    bool empty() const { return frames_.empty(); }
    ReplayInfo info() const;
    void clear();
    // Nombre de un objeto grabado (para la ventana del editor).
    std::size_t trackCount() const { return tracks_.size(); }
    const std::string& trackName(std::size_t i) const { return tracks_[i].name; }

    // Juego con origen flotante: no hace falta nada (cada muestra guarda el
    // origen del mundo).

private:
    struct Track {
        Uuid uuid;
        Uuid parent;
        std::string name;
        std::string snapshot;  // serializeEntity si aparecio durante la grabacion
        bool spawned = false;
    };
    struct TrackState {
        bool alive = false;
        core::Vec3 position{};
        core::Quat rotation{};
        core::Vec3 scale{1.0f, 1.0f, 1.0f};
        bool active = true;
        int anim_kind = 0;  // 0 nada, 1 Animator, 2 Motion Matching
        int anim_state = -1;  // estado del controlador / entrada de la base
        float anim_time = 0.0f;
        std::vector<std::pair<int, float>> params;  // parametros del Animator (indice en param_names_)
    };
    struct Frame {
        float time = 0.0f;
        double origin[3] = {0.0, 0.0, 0.0};
        std::uint32_t offset = 0;  // en stream_
        bool keyframe = false;
    };
    struct Watch {
        int track = -1;
        std::uint64_t version = 0;
        bool active = true;
        bool spawned = false;
    };
    struct Restore {
        Uuid uuid;
        core::Vec3 position{};
        core::Quat rotation{};
        core::Vec3 scale{1.0f, 1.0f, 1.0f};
        bool active = true;
        bool has_animator = false;
        bool animator_playing = true;
        float animator_time = 0.0f;
        int animator_state = -1;
        bool has_motion = false;
    };

    void recordSample(ecs::World& world);
    void trim();
    // Estado de todos los objetos en el fotograma `index` (decodificando desde
    // el fotograma completo anterior o desde el ultimo decodificado).
    bool decodeTo(int index);
    void decodeFrame(int index, std::vector<TrackState>& states) const;
    void applyAt(ecs::World& world, float seconds);
    void bindEntities(ecs::World& world);
    void updateFreeCamera(ecs::World& world, float delta_seconds);
    void fireEvents(float from, float to);

    ReplayOptions options_;
    bool recording_ = false;
    float record_time_ = 0.0f;
    float sample_accumulator_ = 0.0f;
    int samples_since_key_ = 0;
    std::string scene_name_;

    std::vector<Track> tracks_;
    std::vector<Frame> frames_;
    std::vector<std::uint8_t> stream_;
    std::vector<ReplayEvent> events_;
    std::vector<std::string> param_names_;  // parametros del Animator grabados
    std::size_t raw_bytes_ = 0;
    // Lo que reconstruira el lector (para codificar diferencias sin deriva).
    std::vector<TrackState> encoded_;
    std::unordered_map<std::uint32_t, Watch> watched_;  // por entt::entity
    std::unordered_map<std::uint32_t, bool> audio_playing_;

    // Reproduccion.
    bool playing_ = false;
    bool paused_ = false;
    bool loop_ = false;
    float speed_ = 1.0f;
    float time_ = 0.0f;
    bool free_camera_ = false;
    const dm::Input* input_ = nullptr;
    float camera_yaw_ = 0.0f;
    float camera_pitch_ = 0.0f;
    bool camera_angles_valid_ = false;
    std::vector<std::uint32_t> bound_;  // entidad (entt) de cada pista (0xFFFFFFFF = ninguna)
    std::vector<std::uint32_t> ghosts_; // copias creadas para objetos que ya no existen
    std::vector<Restore> restore_;
    int decoded_index_ = -1;
    std::vector<TrackState> decoded_;
    std::vector<TrackState> next_state_;

    audio::AudioSystem* audio_ = nullptr;
    std::filesystem::path folder_;
};

// El sistema de la partida en curso (Lua: Replay.*). nullptr fuera de Play.
ReplaySystem* activeSystem();
void setActiveSystem(ReplaySystem* system);

}  // namespace cramion::replay

#endif  // CRAMION_CORE_REPLAY_REPLAY_H
