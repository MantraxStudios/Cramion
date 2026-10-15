#ifndef CRAMION_CORE_PROFILING_PROFILER_H
#define CRAMION_CORE_PROFILING_PROFILER_H

// Cramion Insights: perfilador de CPU por zonas (como Unreal Insights / el
// Profiler de Unity, en pequeno).
//
//   CR_PROFILE_SCOPE("Fisica");          // mide hasta el final del bloque
//   CR_PROFILE_COUNTER("Particulas", n); // un numero por frame (grafica)
//
// Cada frame (beginFrame/endFrame en el hilo principal) se guardan las zonas
// con su anidamiento (arbol) y su hilo. Se puede:
//   - ver el arbol con media, p95, maximo y llamadas de los ultimos frames
//     (ventana Insights del editor, MCP `profiler`, scripts `Profiler::`);
//   - capturar los ultimos N frames a un .crtrace (formato Chrome Trace
//     Event: se abre en ui.perfetto.dev o chrome://tracing);
//   - capturar solos los tirones: si un frame pasa de prof.HitchMs (y de
//     prof.HitchRelative veces la mediana de los anteriores: en un PC lento
//     no todo es un tiron), se guardan los frames de antes en la carpeta de
//     capturas (una cada pocos segundos como mucho; se escribe en un hilo de
//     disco, sin parar el frame) y se avisa por el registro.
// Coste: unas decenas de nanosegundos por zona; con prof.Enabled = false
// las zonas no hacen nada.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace cramion::prof {

// Zonas. `name` debe vivir siempre (un literal o intern()).
void begin(const char* name);
void end();
// Para nombres que no son literales (el archivo de un script): devuelve un
// puntero estable a una copia.
const char* intern(const std::string& name);

class Scope {
public:
    explicit Scope(const char* name) { begin(name); }
    ~Scope() { end(); }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};

void counter(const char* name, double value);

// Frame (hilo principal).
void beginFrame();
void endFrame();
bool enabled();
void setEnabled(bool on);
std::uint64_t frameIndex();
// El juego carga una escena (true desde que empieza hasta "Escena lista"):
// esos frames (de cientos de ms, que no son tirones del juego) no cuentan en
// summary() ni en topZones() y no guardan capturas de tiron. El frame en el
// que se pasa a true, o en el que se vuelve a false, cuenta como de carga.
void setLoading(bool loading);

// Estadisticas de las zonas en los ultimos `frames` frames (arbol aplanado en
// profundidad: cada nodo lleva su profundidad y la ruta de sus padres).
struct ZoneStats {
    std::string path;   // "Frame/Fisica/Jolt"
    std::string name;   // "Jolt"
    int depth = 0;
    std::uint32_t thread = 0;  // 0 = principal
    double avg_ms = 0.0;       // media por frame (inclusivo)
    double self_ms = 0.0;      // media por frame sin sus hijos
    double p95_ms = 0.0;
    double max_ms = 0.0;
    double last_ms = 0.0;
    double calls = 0.0;        // media de llamadas por frame
};
std::vector<ZoneStats> stats(int frames = 120);
// Duracion de los ultimos frames (ms), del mas viejo al mas nuevo.
std::vector<float> frameTimes();
struct CounterStats {
    std::string name;
    double last = 0.0;
    double avg = 0.0;
    double max = 0.0;
};
std::vector<CounterStats> counters(int frames = 120);
// Percentiles de la duracion del frame (p50, p95, p99) y el maximo, en los
// ultimos `frames` frames sin los de carga (setLoading).
struct FrameSummary {
    double avg_ms = 0.0, p50_ms = 0.0, p95_ms = 0.0, p99_ms = 0.0, max_ms = 0.0;
    // Frames de la ventana por encima de prof.HitchMs y de prof.HitchRelative
    // veces su mediana (como las capturas de tiron).
    int hitches = 0;
};
FrameSummary summary(int frames = 300);
// Las zonas del hilo principal con mas tiempo propio (sin sus zonas hijas):
// media por frame en los ultimos `frames` frames sin los de carga, de mas a
// menos y como mucho `count`. Agrupa por nombre, sin rutas: mucho mas barata
// que stats() (la usa la linea de rendimiento del juego cada pocos segundos).
struct ZoneCost {
    std::string name;
    double self_ms = 0.0;
};
std::vector<ZoneCost> topZones(int frames = 300, int count = 5);

// Capturas (.crtrace = JSON de Chrome Trace Event).
bool saveTrace(const std::filesystem::path& file, int frames = 300, std::string* error = nullptr);
std::filesystem::path captureFolder();
void setCaptureFolder(const std::filesystem::path& folder);
// La ultima captura de tiron ya escrita (se escriben en un hilo de disco: la
// del ultimo tiron puede tardar un poco; flushCaptures() la espera).
std::filesystem::path lastHitchCapture();
// Espera a que se terminen de escribir las capturas de tiron pendientes.
void flushCaptures();
// Aviso al guardar una captura de tiron (para la consola / el registro). Se
// llama desde el hilo de disco que la escribio.
void setLog(std::function<void(const std::string&)> log);

// Pruebas: borra todo.
void reset();

}  // namespace cramion::prof

#define CR_PROFILE_CONCAT2(a, b) a##b
#define CR_PROFILE_CONCAT(a, b) CR_PROFILE_CONCAT2(a, b)
#define CR_PROFILE_SCOPE(name) ::cramion::prof::Scope CR_PROFILE_CONCAT(cr_profile_scope_, __LINE__)(name)
#define CR_PROFILE_COUNTER(name, value) ::cramion::prof::counter(name, static_cast<double>(value))

#endif  // CRAMION_CORE_PROFILING_PROFILER_H
