#ifndef CRAMION_EDITOR_AUTOSAVE_H
#define CRAMION_EDITOR_AUTOSAVE_H

// Autoguardado de la escena (ROADMAP 2 #3).
//
// Cada N minutos (Editar > Autoguardado, 2 por defecto), si la escena tiene
// cambios, la instantanea de deshacer (current_state_, que ya esta en memoria)
// se copia y un hilo propio la escribe en Library/Autosave/<escena>.crscene.autosave
// (temporal + rename: nunca queda a medias). El hilo principal solo copia el
// texto: no serializa nada.
//
// Tambien se guarda al cerrarse el editor por un error: excepcion de SEH,
// std::terminate, abort() (lo que hace terminate en los hilos de trabajo) y
// una excepcion que salga del bucle (ErrorDeviceLost), desde el destructor del
// editor. Al abrir la escena, si su autoguardado es mas nuevo que el archivo,
// el editor ofrece recuperarlo.
//
// Las preferencias de seguridad del editor (autoguardado y token del MCP) van
// en %LOCALAPPDATA%/Cramion/Settings/EditorSafety.ini (son del PC, no del proyecto).

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

namespace cramion::editor {

struct SafetyPreferences {
    int autosave_minutes = 2;       // 0 = sin autoguardado (ni al cerrarse por un error)
    bool mcp_require_token = true;  // el servidor MCP exige el token de la sesion
};
SafetyPreferences loadSafetyPreferences();
void saveSafetyPreferences(const SafetyPreferences& prefs);

class SceneAutosave {
public:
    SceneAutosave();
    ~SceneAutosave();  // escribe lo que quede en la cola y para el hilo
    SceneAutosave(const SceneAutosave&) = delete;
    SceneAutosave& operator=(const SceneAutosave&) = delete;

    // Library/Autosave/<ruta dentro de Assets, con '~' por '/'>.crscene.autosave;
    // `scene` vacia (escena sin guardar) = "Sin guardar.crscene.autosave".
    static std::filesystem::path fileFor(const std::filesystem::path& library, const std::filesystem::path& assets,
                                         const std::filesystem::path& scene);
    // El autoguardado existe y es mas nuevo que la escena (o la escena no existe).
    static bool newerThanScene(const std::filesystem::path& autosave, const std::filesystem::path& scene);

    struct Frame {
        const std::filesystem::path* file = nullptr;  // nullptr = este frame no se autoguarda (prefab, carga...)
        const std::string* state = nullptr;  // la instantanea de deshacer (current_state_)
        bool dirty = false;
        int minutes = 2;
    };
    // Cada frame en el hilo principal: comparaciones y, cada N minutos, una
    // copia del texto. Arma tambien el guardado de emergencia.
    void tick(const Frame& frame);

    // Borra un autoguardado (en la cola, despues de lo que haya pendiente).
    void discard(const std::filesystem::path& file);
    // Mientras se pregunta si se recupera, ese archivo no se pisa.
    void protect(const std::filesystem::path& file) { protected_ = file; }
    // El ultimo archivo escrito en esta sesion (vacio si ninguno).
    const std::filesystem::path& lastWritten() const { return last_written_; }

    // Escribe ya, en este hilo, lo armado (salida por una excepcion).
    void writeNow();
    // Sin guardado de emergencia (cierre normal).
    void disarm();

private:
    struct Job {
        bool remove = false;
        std::filesystem::path file;
        std::string data;
    };
    void workerLoop();
    void push(Job job);

    using Clock = std::chrono::steady_clock;
    std::filesystem::path target_;
    std::filesystem::path protected_;
    std::filesystem::path last_written_;
    Clock::time_point due_{};
    bool due_set_ = false;
    int minutes_ = -1;
    // Identidad de la instantanea ya escrita: cada commit deja un texto nuevo
    // (otro buffer), asi que puntero + tamano basta para no repetir.
    const char* written_data_ = nullptr;
    std::size_t written_size_ = 0;

    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable idle_;
    std::deque<Job> jobs_;
    bool busy_ = false;
    bool stop_ = false;
};

// Guardado de emergencia desde cualquier hilo (manejadores de crash o el de
// ErrorDeviceLost): escribe la ultima instantanea armada. Sin excepciones y
// sin memoria nueva; una sola vez por proceso. Devuelve si escribio.
bool autosaveEmergency();

}  // namespace cramion::editor

#endif  // CRAMION_EDITOR_AUTOSAVE_H
