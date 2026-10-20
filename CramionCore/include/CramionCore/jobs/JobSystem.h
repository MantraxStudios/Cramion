#ifndef CRAMION_CORE_JOBS_JOB_SYSTEM_H
#define CRAMION_CORE_JOBS_JOB_SYSTEM_H

// Sistema de tareas (job system) como el C# Job System de Unity / el
// TaskGraph de Unreal: un grupo de hilos trabajadores que reparten trabajo
// corto entre todos los nucleos.
//
//   jobs::JobHandle h = jobs::schedule([] { ... });       // una tarea
//   jobs::parallelFor(count, 64, [&](std::size_t begin, std::size_t end) {
//       for (std::size_t i = begin; i < end; ++i) ...       // por trozos
//   });                                                     // espera al final
//   jobs::wait(h);
//
// Quien espera no se queda parado: ejecuta tareas de la cola mientras tanto
// (asi una tarea puede lanzar y esperar otras sin bloquear los hilos). Los
// hilos son nucleos - 2 (minimo 1, maximo 16); CVar jobs.Threads o la variable
// de entorno CRAMION_JOB_THREADS lo cambian (0 = todo en el hilo que llama,
// util para depurar).
//
// Prioridades: cada hilo coge primero High, luego Normal y por ultimo
// Background. Las lecturas y escrituras de disco van a otro grupo pequeno de
// hilos (scheduleIo, CVar stream.IoThreads) para que una lectura lenta no deje
// sin hilos al resto.
//
// Quien espera (wait, parallelFor) solo ejecuta tareas de SU grupo mientras
// espera: el hilo principal no se queda atascado en una tarea larga de otro
// (una forma de colision, el follaje...).
//
// Excepciones: una tarea que lanza no tumba el proceso. Se registra en el log
// con el nombre de la tarea (el de schedule), el contador del grupo baja igual
// y wait() / parallelFor() la vuelven a lanzar en el hilo que espera (la
// primera del grupo). Una tarea sin nadie que espere solo deja el aviso; quien
// sondea done() puede mirar failed(). Si aun asi un trabajador acaba en
// std::terminate (una excepcion en un destructor noexcept...), su manejador
// escribe un informe (tarea en curso y motivo) antes de seguir con el
// manejador de la aplicacion (minidump del editor y del player).

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <string>

namespace cramion::jobs {

// Contador de tareas pendientes de un grupo: done() cuando llega a 0.
// `error` guarda la primera excepcion del grupo (la escribe solo quien gana
// `failed_`, antes de bajar `pending`: tras done() ya se puede leer).
struct JobCounter {
    std::atomic<int> pending{0};
    std::atomic<bool> failed_{false};
    std::exception_ptr error;
    bool done() const { return pending.load(std::memory_order_acquire) == 0; }
    // Alguna tarea del grupo lanzo una excepcion (fiable tras done()).
    bool failed() const { return failed_.load(std::memory_order_acquire); }
};

using JobHandle = std::shared_ptr<JobCounter>;

enum class Priority { High, Normal, Background };

// Lanza una tarea. Si `group` no es nulo, la tarea se suma a ese grupo (se
// esperan todas juntas). `name` sale en el log y en el informe si la tarea
// falla; debe vivir siempre (un literal).
JobHandle schedule(std::function<void()> job, JobHandle group = nullptr, Priority p = Priority::Normal,
                   const char* name = nullptr);

// Una tarea de disco (leer, descomprimir, escribir) en el grupo de hilos de
// IO (stream.IoThreads). No debe esperar a tareas de schedule() dentro.
JobHandle scheduleIo(std::function<void()> job, Priority p = Priority::Normal, const char* name = nullptr);

// Cancelacion cooperativa: quien lanza la tarea pone `cancelled` y la tarea
// lo mira entre etapas (y descarta su resultado). `generation` sirve para
// reconocer resultados viejos.
struct CancelToken {
    std::atomic<bool> cancelled{false};
    std::uint64_t generation = 0;
};
using Cancel = std::shared_ptr<CancelToken>;

inline bool isCancelled(const Cancel& c) { return c && c->cancelled.load(std::memory_order_acquire); }

// Presupuesto de tiempo del hilo principal (integrar teselas, recrear
// objetos...). spend() devuelve true mientras quede tiempo; la primera vez
// siempre true (algo avanza cada frame aunque el presupuesto sea 0).
class FrameBudget {
public:
    explicit FrameBudget(double ms);
    bool spend();
    // Sin gastar: queda tiempo (o aun no se gasto nada).
    bool hasTime() const;
    double elapsedMs() const;
    int spent() const { return spent_; }

private:
    std::chrono::steady_clock::time_point start_;
    double ms_ = 0.0;
    int spent_ = 0;
};

// Espera a que termine (ayudando con otras tareas mientras). Si alguna tarea
// del grupo lanzo, vuelve a lanzar la primera excepcion aqui.
void wait(const JobHandle& handle);

// Reparte [0, count) en trozos de al menos `grain` elementos y espera. Si un
// trozo lanza, se esperan los demas igualmente (usan `body` por referencia) y
// la primera excepcion sale de aqui.
void parallelFor(std::size_t count, std::size_t grain, const std::function<void(std::size_t, std::size_t)>& body);

// Destino extra de los avisos de tareas que fallan y del informe de
// std::terminate en un trabajador (las migas de pan del informe de fallos del
// editor, por ejemplo). Siempre salen tambien por std::cerr. `fatal` = el
// informe de terminate (el proceso se cierra justo despues). Nulo = ninguno.
using FailureReport = void (*)(const std::string& line, bool fatal);
void setFailureReport(FailureReport report);

// Hilos trabajadores (0 = sin hilos: todo en el que llama).
int workerCount();
// Hilos de IO (0 = sin hilos: scheduleIo ejecuta en el que llama).
int ioWorkerCount();
// Reinicia los hilos con otro numero (-1 = automatico).
void setWorkerCount(int threads);
// Para los hilos (al cerrar). Se vuelven a crear solos si hace falta.
void shutdown();

// Estadisticas para el perfilador / Insights.
struct JobStats {
    std::uint64_t executed = 0;  // tareas ejecutadas desde el principio
    int queued = 0;              // en la cola ahora mismo
    int workers = 0;
    int io_queued = 0;           // tareas de disco en cola
    int io_workers = 0;
    std::uint64_t failed = 0;    // tareas que lanzaron una excepcion
};
JobStats stats();

}  // namespace cramion::jobs

#endif  // CRAMION_CORE_JOBS_JOB_SYSTEM_H
