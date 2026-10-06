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

#include <atomic>
#include <cstdint>
#include <cstddef>
#include <functional>
#include <memory>

namespace cramion::jobs {

// Contador de tareas pendientes de un grupo: done() cuando llega a 0.
struct JobCounter {
    std::atomic<int> pending{0};
    bool done() const { return pending.load(std::memory_order_acquire) == 0; }
};

using JobHandle = std::shared_ptr<JobCounter>;

// Lanza una tarea. Si `group` no es nulo, la tarea se suma a ese grupo (se
// esperan todas juntas).
JobHandle schedule(std::function<void()> job, JobHandle group = nullptr);

// Espera a que termine (ayudando con otras tareas mientras).
void wait(const JobHandle& handle);

// Reparte [0, count) en trozos de al menos `grain` elementos y espera.
void parallelFor(std::size_t count, std::size_t grain, const std::function<void(std::size_t, std::size_t)>& body);

// Hilos trabajadores (0 = sin hilos: todo en el que llama).
int workerCount();
// Reinicia los hilos con otro numero (-1 = automatico).
void setWorkerCount(int threads);
// Para los hilos (al cerrar). Se vuelven a crear solos si hace falta.
void shutdown();

// Estadisticas para el perfilador / Insights.
struct JobStats {
    std::uint64_t executed = 0;  // tareas ejecutadas desde el principio
    int queued = 0;              // en la cola ahora mismo
    int workers = 0;
};
JobStats stats();

}  // namespace cramion::jobs

#endif  // CRAMION_CORE_JOBS_JOB_SYSTEM_H
