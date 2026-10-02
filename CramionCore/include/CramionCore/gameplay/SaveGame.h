#ifndef CRAMION_CORE_GAMEPLAY_SAVE_GAME_H
#define CRAMION_CORE_GAMEPLAY_SAVE_GAME_H

// Partidas guardadas (como el Save Game de Unreal o un sistema de guardado de
// Unity), con ranuras ("slot1", "autosave"...).
//
// Que se guarda:
//   - los objetos con el componente Saveable: posicion/rotacion/escala, si
//     estan activos, la velocidad de su cuerpo fisico, los componentes que se
//     elijan y el estado de su script (sus propiedades y lo que devuelva
//     OnSave(); al cargar se llama a OnLoad(datos));
//   - los objetos Saveable creados en el juego (Scene.instantiate) y los de la
//     escena que se destruyeron;
//   - los valores sueltos (Save.setValue) y las variables de los dialogos;
//   - la escena, la fecha, el tiempo jugado y una etiqueta.
//
// El archivo es JSON versionado (comprimido con zstd si se pide) y se
// escribe en un hilo aparte: el juego no se para al guardar.

#include "CramionCore/ecs/Reflection.h"
#include "CramionCore/ecs/World.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace cramion::physics {
class PhysicsSystem;
}

namespace cramion::gameplay {

inline constexpr const char* kSaveExtension = ".crsave";
inline constexpr int kSaveFormatVersion = 1;

// Marca un objeto para que entre en las partidas.
struct Saveable {
    // Identificador estable. Vacio = el UUID del objeto (vale para los de la
    // escena). Ponlo a mano si el objeto se puede borrar y volver a crear en
    // el editor y quieres que una partida vieja lo siga encontrando.
    std::string save_id;
    bool save_transform = true;
    bool save_active = true;
    bool save_physics = true;   // velocidades del Rigidbody
    bool save_script = true;    // propiedades del script + OnSave/OnLoad
    bool save_children = false; // tambien la posicion de sus hijos
    // Componentes enteros a guardar, por nombre del registro separados por
    // comas ("Light, AudioSource"); "*" = todos los que tenga.
    std::string components;
    void reflect(ecs::PropertyVisitor& v);
};

struct SaveSlotInfo {
    std::string slot;
    std::string label;
    std::string scene;
    std::string date;          // "2026-10-02 18:30"
    std::int64_t unix_time = 0;
    double playtime = 0.0;     // segundos
    std::uintmax_t size_bytes = 0;
    std::filesystem::path file;
};

class SaveSystem {
public:
    SaveSystem();
    ~SaveSystem();  // espera a que terminen las escrituras
    SaveSystem(const SaveSystem&) = delete;
    SaveSystem& operator=(const SaveSystem&) = delete;

    // --- Configuracion ---
    void setFolder(const std::filesystem::path& folder);
    const std::filesystem::path& folder() const { return folder_; }
    void setCompression(bool compress) { compress_ = compress; }
    bool compression() const { return compress_; }
    void setPhysics(physics::PhysicsSystem* physics) { physics_ = physics; }
    // El estado de los scripts (lo pone el ScriptSystem).
    struct ScriptHooks {
        std::function<nlohmann::json(ecs::Entity)> capture;
        std::function<void(ecs::Entity, const nlohmann::json&)> restore;
    };
    void setScriptHooks(ScriptHooks hooks) { hooks_ = std::move(hooks); }

    // --- Escena ---
    // Al empezar una escena: apunta que objetos Saveable trae (para saber
    // luego cuales se crearon o destruyeron en el juego).
    void beginScene(ecs::World& world, const std::string& scene_name, const std::string& scene_file);
    const std::string& sceneName() const { return scene_name_; }
    const std::string& sceneFile() const { return scene_file_; }
    // Cuenta el tiempo jugado y hace el autoguardado.
    void update(ecs::World& world, float dt);

    // --- Instantaneas ---
    nlohmann::json capture(ecs::World& world);
    // Aplica una instantanea a la escena ya cargada (la misma de la partida).
    bool apply(ecs::World& world, const nlohmann::json& snapshot, std::string* error = nullptr);
    // Objetos que hay que destruir al aplicar (los destruye quien llama,
    // con sus OnDestroy); apply() los destruye si no hay `destroy`.
    std::function<void(ecs::Entity)> destroy;
    // Objetos creados al aplicar (para crear sus scripts antes de OnLoad).
    std::function<void(ecs::Entity)> spawned;

    // --- Ranuras ---
    bool save(ecs::World& world, const std::string& slot, const std::string& label = {}, std::string* error = nullptr);
    bool read(const std::string& slot, nlohmann::json& snapshot, std::string* error = nullptr) const;
    bool remove(const std::string& slot);
    bool exists(const std::string& slot) const;
    std::vector<SaveSlotInfo> list() const;
    std::optional<SaveSlotInfo> info(const std::string& slot) const;
    std::filesystem::path slotFile(const std::string& slot) const;
    // Espera a que se escriban las partidas pendientes.
    void flush();
    bool writing() const { return pending_writes_.load() > 0; }
    // Error de la ultima escritura en segundo plano (y lo borra).
    std::string takeWriteError();

    // --- Carga diferida ---
    // Una partida de otra escena: se guarda aqui mientras se carga la escena
    // y se aplica al empezar (ScriptSystem::start).
    std::optional<nlohmann::json> pending_load;

    // --- Valores sueltos (van dentro de cada partida) ---
    nlohmann::json& values() { return values_; }
    const nlohmann::json& values() const { return values_; }
    // Datos extra que se anaden a cada instantanea (las variables de dialogo).
    std::function<nlohmann::json()> extra_capture;
    std::function<void(const nlohmann::json&)> extra_restore;

    // --- Tiempo jugado y autoguardado ---
    double playtime() const { return playtime_; }
    void setPlaytime(double seconds) { playtime_ = seconds; }
    // Cada `seconds` (0 = nunca) guarda en `slot`.
    void setAutosave(float seconds, const std::string& slot = "autosave");
    float autosaveInterval() const { return autosave_interval_; }

    // Clave estable de un objeto en las partidas.
    static std::string keyOf(ecs::Entity entity);
    // Carpeta por defecto de las partidas de un juego: %APPDATA%/<juego>/saves
    // en Windows; `fallback` en otros sistemas.
    static std::filesystem::path defaultFolder(const std::string& game, const std::filesystem::path& fallback);
    // Nombre de ranura valido como archivo ("Partida 1" -> "Partida 1", "a/b" -> "a_b").
    static std::string sanitizeSlot(const std::string& slot);

private:
    nlohmann::json captureEntity(ecs::World& world, ecs::Entity e, const Saveable& s);
    void applyEntity(ecs::World& world, ecs::Entity e, const nlohmann::json& state);

    std::filesystem::path folder_;
    bool compress_ = false;
    physics::PhysicsSystem* physics_ = nullptr;
    ScriptHooks hooks_;
    std::string scene_name_;
    std::string scene_file_;
    std::set<std::string> baseline_;  // claves Saveable de la escena al empezar
    nlohmann::json values_ = nlohmann::json::object();
    double playtime_ = 0.0;
    float autosave_interval_ = 0.0f;
    float autosave_timer_ = 0.0f;
    std::string autosave_slot_ = "autosave";

    // Escrituras en segundo plano: un hilo con una cola (en orden).
    struct WriteJob {
        std::filesystem::path file;
        std::string text;
        bool compress = false;
    };
    void enqueue(WriteJob job);
    void writerLoop();
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::condition_variable idle_cv_;
    std::vector<WriteJob> queue_;
    std::thread writer_;
    bool stopping_ = false;
    std::atomic<int> pending_writes_{0};
    std::string write_error_;
};

void registerSaveComponents();

}  // namespace cramion::gameplay

#endif  // CRAMION_CORE_GAMEPLAY_SAVE_GAME_H
