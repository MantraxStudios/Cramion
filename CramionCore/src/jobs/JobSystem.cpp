#include "CramionCore/jobs/JobSystem.h"

#include "CramionCore/cvar/CVar.h"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace cramion::jobs {

namespace {

cvar::CVar<int> g_job_threads("jobs.Threads", -1,
                              "Hilos del job system (-1 = nucleos - 2, 0 = todo en el hilo que llama)", cvar::Saved, -1,
                              64);

cvar::CVar<int> g_io_threads("stream.IoThreads", -1,
                             "Hilos de disco del streaming (-1 = automatico: 2, o 1 en equipos de 4 hilos o menos; "
                             "0 = en el hilo que llama)",
                             cvar::Saved, -1, 8);

std::string environment(const char* name) {
#ifdef _WIN32
    char* value = nullptr;
    std::size_t size = 0;
    std::string out;
    if (_dupenv_s(&value, &size, name) == 0 && value != nullptr) out = value;
    std::free(value);
    return out;
#else
    const char* value = std::getenv(name);
    return value != nullptr ? value : "";
#endif
}

struct Job {
    std::function<void()> fn;
    JobHandle counter;
    const char* name = nullptr;
};

// -----------------------------------------------------------------------------
// Fallos: avisos de tareas que lanzan e informe de std::terminate
// -----------------------------------------------------------------------------

std::atomic<FailureReport> g_failure_report{nullptr};
std::atomic<std::uint64_t> g_failed{0};

// Tarea en curso de este hilo (para el informe de terminate) y si es un hilo
// de disco.
thread_local const char* t_current_task = nullptr;
thread_local bool t_io_thread = false;

const char* taskName(const char* name) { return name != nullptr ? name : "sin nombre"; }

// Texto de una excepcion (what() si es std::exception).
std::string describe(const std::exception_ptr& error) {
    if (!error) return "sin excepcion activa";
    try {
        std::rethrow_exception(error);
    } catch (const std::exception& e) {
        return e.what();
    } catch (...) {
        return "excepcion desconocida (no deriva de std::exception)";
    }
}

void report(const std::string& line, bool fatal) {
    // Nada de lo que se haga aqui puede lanzar (sale del catch de una tarea
    // o de un manejador de terminate).
    try {
        // Una sola escritura: los avisos de varios hilos no se mezclan.
        std::string text = line;
        text += '\n';
        std::cerr.write(text.data(), static_cast<std::streamsize>(text.size()));
        std::cerr.flush();
    } catch (...) {
    }
    if (const FailureReport hook = g_failure_report.load(std::memory_order_acquire)) {
        try {
            hook(line, fatal);
        } catch (...) {
        }
    }
}

// Manejador de terminate que habia antes que el nuestro: en Windows el
// manejador es por hilo y un trabajador nuevo trae el de por defecto; con
// libstdc++/libc++ es global y aqui queda el de la aplicacion.
std::atomic<std::terminate_handler> g_previous_terminate{nullptr};

[[noreturn]] void onWorkerTerminate() {
    // Una sola vez aunque fallen varios hilos a la vez.
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true)) {
        std::string line = "[Jobs] std::terminate en un hilo ";
        try {
            line += t_io_thread ? "de disco" : "trabajador";
            line += " durante la tarea '";
            line += t_current_task != nullptr ? t_current_task : "ninguna";
            line += "': ";
            line += describe(std::current_exception());
        } catch (...) {
        }
        report(line, true);
        // Informe en un archivo por si no hay consola (el player sin ventana).
        try {
            const std::filesystem::path file = std::filesystem::temp_directory_path() / "cramion_jobs_terminate.txt";
            std::ofstream out(file, std::ios::trunc);
            out << line << '\n';
        } catch (...) {
        }
    }
    const std::terminate_handler previous = g_previous_terminate.load(std::memory_order_acquire);
    if (previous != nullptr && previous != &onWorkerTerminate) previous();  // minidump de la aplicacion
    std::abort();
}

void installWorkerTerminate(bool io) {
    t_io_thread = io;
    const std::terminate_handler current = std::get_terminate();
    if (current == &onWorkerTerminate) return;  // global y ya puesto por otro trabajador
    g_previous_terminate.store(current, std::memory_order_release);
    std::set_terminate(&onWorkerTerminate);
}

// Baja el contador del grupo pase lo que pase (RAII): quien espera en
// waitFor nunca se queda colgado aunque la tarea lance.
struct FinishJob {
    Job& job;
    std::atomic<std::uint64_t>& executed;
    ~FinishJob() {
        // Suelta lo capturado antes de avisar (quien espera puede destruirlo).
        job.fn = nullptr;
        executed.fetch_add(1, std::memory_order_relaxed);
        if (job.counter) job.counter->pending.fetch_sub(1, std::memory_order_acq_rel);
    }
};

// Avisa y guarda la primera excepcion del grupo (antes de bajar el contador).
void recordFailure(const Job& job, std::exception_ptr error, bool io) {
    g_failed.fetch_add(1, std::memory_order_relaxed);
    std::string line = "[Jobs] La tarea '";
    try {
        line += taskName(job.name);
        line += io ? "' (disco)" : "'";
        line += " lanzo una excepcion: ";
        line += describe(error);
    } catch (...) {
    }
    report(line, false);
    if (job.counter && !job.counter->failed_.exchange(true, std::memory_order_acq_rel)) {
        job.counter->error = std::move(error);
    }
}

// Vuelve a lanzar en quien espera la primera excepcion del grupo.
void rethrowIfFailed(const JobHandle& handle) {
    if (handle && handle->failed() && handle->error) std::rethrow_exception(handle->error);
}

constexpr std::size_t kPriorities = 3;

class Pool {
public:
    explicit Pool(bool io) : io_(io) {}
    ~Pool() { stop(); }

    void ensureStarted() {
        if (started_.load(std::memory_order_acquire)) return;
        std::lock_guard<std::mutex> lock(start_mutex_);
        if (started_.load(std::memory_order_relaxed)) return;
        start(desiredThreads());
    }

    int desiredThreads() const {
        const int cores = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
        if (io_) {
            const int requested = g_io_threads;
            if (requested >= 0) return std::min(requested, 8);
            // Con jobs.Threads = 0 (depurar) tambien el disco va en el que llama.
            int compute = g_job_threads;
            if (const std::string env = environment("CRAMION_JOB_THREADS"); !env.empty()) compute = std::atoi(env.c_str());
            if (compute == 0) return 0;
            return cores <= 4 ? 1 : 2;
        }
        int requested = g_job_threads;
        if (const std::string env = environment("CRAMION_JOB_THREADS"); !env.empty()) requested = std::atoi(env.c_str());
        if (requested >= 0) return std::min(requested, 64);
        return std::clamp(cores - 2, 1, 16);
    }

    void start(int threads) {
        quit_ = false;
        for (int i = 0; i < threads; ++i) workers_.emplace_back([this] { workerLoop(); });
        worker_count_ = threads;
        started_.store(true, std::memory_order_release);
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            quit_ = true;
        }
        cv_.notify_all();
        for (std::thread& t : workers_) {
            if (t.joinable()) t.join();
        }
        workers_.clear();
        worker_count_ = 0;
        // Lo que quede en la cola se ejecuta aqui (nadie se queda esperando).
        std::array<std::deque<Job>, kPriorities> rest;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (std::size_t i = 0; i < kPriorities; ++i) rest[i].swap(queues_[i]);
            queued_ = 0;
        }
        for (auto& q : rest) {
            for (Job& j : q) run(j);
        }
        started_.store(false, std::memory_order_release);
    }

    void restart(int threads) {
        std::lock_guard<std::mutex> lock(start_mutex_);
        stop();
        start(threads < 0 ? desiredThreads() : std::min(threads, 64));
    }

    void push(Job job, Priority p) {
        ensureStarted();
        if (worker_count_ == 0) {
            run(job);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queues_[index(p)].push_back(std::move(job));
            ++queued_;
        }
        cv_.notify_one();
    }

    // Ejecuta una tarea del grupo `group` si hay en la cola; false si no.
    // Solo las del grupo: quien espera no se queda atascado en una tarea larga
    // de otro (el hilo principal esperando un parallelFor, por ejemplo).
    bool tryRunOneOf(const JobCounter* group) {
        Job job;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            bool found = false;
            for (auto& q : queues_) {
                for (auto it = q.begin(); it != q.end(); ++it) {
                    if (it->counter.get() != group) continue;
                    job = std::move(*it);
                    q.erase(it);
                    --queued_;
                    found = true;
                    break;
                }
                if (found) break;
            }
            if (!found) return false;
        }
        run(job);
        return true;
    }

    void waitFor(const JobHandle& h) {
        if (!h) return;
        const auto start = std::chrono::steady_clock::now();
        while (!h->done()) {
            if (tryRunOneOf(h.get())) continue;
            // Las del grupo ya estan en otros hilos (el contador baja siempre,
            // tambien si una lanza: esto no se cuelga). Esperas cortas (un
            // parallelFor): yield, como antes. Largas (disco, cerrar escena):
            // dormir un poco para no quemar un nucleo. En Windows sin
            // timeBeginPeriod un sleep_for puede durar ~15 ms: solo pasados
            // 50 ms (nunca en una espera normal de un frame).
            if (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(50)) {
                std::this_thread::yield();
            } else {
                std::this_thread::sleep_for(std::chrono::microseconds(500));
            }
        }
    }

    int workers() const { return worker_count_; }
    std::uint64_t executed() const { return executed_.load(std::memory_order_relaxed); }
    int queued() {
        std::lock_guard<std::mutex> lock(mutex_);
        return queued_;
    }

private:
    static std::size_t index(Priority p) {
        switch (p) {
            case Priority::High: return 0;
            case Priority::Normal: return 1;
            case Priority::Background: return 2;
        }
        return 1;
    }

    // Ejecuta la tarea; una excepcion se registra y se guarda en su grupo
    // (wait la vuelve a lanzar), nunca sale de aqui.
    void run(Job& job) {
        FinishJob finish{job, executed_};
        const char* outer = t_current_task;  // run anidado (quien espera ayuda)
        t_current_task = taskName(job.name);
        try {
            if (job.fn) job.fn();
        } catch (...) {
            recordFailure(job, std::current_exception(), io_);
        }
        t_current_task = outer;
    }

    bool popLocked(Job& out) {
        for (auto& q : queues_) {
            if (q.empty()) continue;
            out = std::move(q.front());
            q.pop_front();
            --queued_;
            return true;
        }
        return false;
    }

    void workerLoop() {
        installWorkerTerminate(io_);
        for (;;) {
            Job job;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this] { return quit_ || queued_ > 0; });
                if (!popLocked(job)) {
                    if (quit_) return;
                    continue;
                }
            }
            run(job);
        }
    }

    bool io_ = false;
    std::mutex mutex_;
    std::mutex start_mutex_;
    std::condition_variable cv_;
    std::array<std::deque<Job>, kPriorities> queues_;
    int queued_ = 0;
    std::vector<std::thread> workers_;
    std::atomic<bool> started_{false};
    std::atomic<std::uint64_t> executed_{0};
    int worker_count_ = 0;
    bool quit_ = false;
};

Pool& pool() {
    static Pool p(false);
    return p;
}

Pool& ioPool() {
    static Pool p(true);
    return p;
}

}  // namespace

JobHandle schedule(std::function<void()> job, JobHandle group, Priority p, const char* name) {
    JobHandle counter = group ? std::move(group) : std::make_shared<JobCounter>();
    counter->pending.fetch_add(1, std::memory_order_acq_rel);
    pool().push(Job{std::move(job), counter, name}, p);
    return counter;
}

JobHandle scheduleIo(std::function<void()> job, Priority p, const char* name) {
    JobHandle counter = std::make_shared<JobCounter>();
    counter->pending.fetch_add(1, std::memory_order_acq_rel);
    ioPool().push(Job{std::move(job), counter, name}, p);
    return counter;
}

void wait(const JobHandle& handle) {
    if (!handle) return;
    // Una tarea de IO en cola puede ayudarse desde aqui igual (mismo grupo).
    if (!handle->done()) ioPool().tryRunOneOf(handle.get());
    if (!handle->done()) pool().waitFor(handle);
    rethrowIfFailed(handle);
}

void parallelFor(std::size_t count, std::size_t grain, const std::function<void(std::size_t, std::size_t)>& body) {
    if (count == 0) return;
    grain = std::max<std::size_t>(grain, 1);
    pool().ensureStarted();
    const int workers = pool().workers();
    if (workers == 0 || count <= grain) {
        body(0, count);
        return;
    }
    // Unos 4 trozos por hilo (reparte bien si unos tardan mas que otros).
    const std::size_t target_chunks = static_cast<std::size_t>(workers + 1) * 4;
    const std::size_t chunk = std::max(grain, (count + target_chunks - 1) / target_chunks);
    JobHandle group = std::make_shared<JobCounter>();
    std::size_t begin = chunk;  // el primer trozo lo hace quien llama
    for (; begin < count; begin += chunk) {
        const std::size_t end = std::min(count, begin + chunk);
        // High: quien llama espera (no debe quedar detras del streaming).
        schedule([&body, begin, end] { body(begin, end); }, group, Priority::High, "parallelFor");
    }
    // Si el trozo propio lanza, hay que esperar a los demas antes de salir:
    // usan `body` por referencia y quien llama lo destruiria al desenrollar.
    std::exception_ptr own;
    try {
        body(0, std::min(chunk, count));
    } catch (...) {
        own = std::current_exception();
    }
    if (own) {
        try {
            wait(group);
        } catch (...) {
            // Va primero la del trozo propio; la de los otros ya salio en el log.
        }
        std::rethrow_exception(own);
    }
    wait(group);
}

int workerCount() {
    pool().ensureStarted();
    return pool().workers();
}

int ioWorkerCount() {
    ioPool().ensureStarted();
    return ioPool().workers();
}

void setWorkerCount(int threads) { pool().restart(threads); }

void setFailureReport(FailureReport report) { g_failure_report.store(report, std::memory_order_release); }

void shutdown() {
    ioPool().stop();
    pool().stop();
}

JobStats stats() {
    JobStats s;
    s.executed = pool().executed() + ioPool().executed();
    s.queued = pool().queued();
    s.workers = pool().workers();
    s.io_queued = ioPool().queued();
    s.io_workers = ioPool().workers();
    s.failed = g_failed.load(std::memory_order_relaxed);
    return s;
}

// -----------------------------------------------------------------------------
// FrameBudget
// -----------------------------------------------------------------------------

FrameBudget::FrameBudget(double ms) : start_(std::chrono::steady_clock::now()), ms_(ms) {}

double FrameBudget::elapsedMs() const {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start_).count();
}

bool FrameBudget::hasTime() const { return spent_ == 0 || elapsedMs() < ms_; }

bool FrameBudget::spend() {
    if (!hasTime()) return false;
    ++spent_;
    return true;
}

}  // namespace cramion::jobs
