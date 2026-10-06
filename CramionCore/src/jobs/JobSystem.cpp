#include "CramionCore/jobs/JobSystem.h"

#include "CramionCore/cvar/CVar.h"

#include <algorithm>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace cramion::jobs {

namespace {

cvar::CVar<int> g_job_threads("jobs.Threads", -1,
                              "Hilos del job system (-1 = nucleos - 2, 0 = todo en el hilo que llama)", cvar::Saved, -1,
                              64);

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
};

class Pool {
public:
    ~Pool() { stop(); }

    void ensureStarted() {
        if (started_.load(std::memory_order_acquire)) return;
        std::lock_guard<std::mutex> lock(start_mutex_);
        if (started_.load(std::memory_order_relaxed)) return;
        start(desiredThreads());
    }

    int desiredThreads() const {
        int requested = g_job_threads;
        if (const std::string env = environment("CRAMION_JOB_THREADS"); !env.empty()) requested = std::atoi(env.c_str());
        if (requested >= 0) return std::min(requested, 64);
        const int cores = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
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
        std::deque<Job> rest;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            rest.swap(queue_);
        }
        for (Job& j : rest) run(j);
        started_.store(false, std::memory_order_release);
    }

    void restart(int threads) {
        std::lock_guard<std::mutex> lock(start_mutex_);
        stop();
        start(threads < 0 ? desiredThreads() : std::min(threads, 64));
    }

    void push(Job job) {
        ensureStarted();
        if (worker_count_ == 0) {
            run(job);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push_back(std::move(job));
        }
        cv_.notify_one();
    }

    // Ejecuta una tarea de la cola si hay; false si estaba vacia.
    bool tryRunOne() {
        Job job;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (queue_.empty()) return false;
            job = std::move(queue_.front());
            queue_.pop_front();
        }
        run(job);
        return true;
    }

    void waitFor(const JobHandle& h) {
        if (!h) return;
        while (!h->done()) {
            if (!tryRunOne()) std::this_thread::yield();
        }
    }

    int workers() const { return worker_count_; }
    std::uint64_t executed() const { return executed_.load(std::memory_order_relaxed); }
    int queued() {
        std::lock_guard<std::mutex> lock(mutex_);
        return static_cast<int>(queue_.size());
    }

private:
    void run(Job& job) {
        if (job.fn) job.fn();
        executed_.fetch_add(1, std::memory_order_relaxed);
        if (job.counter) job.counter->pending.fetch_sub(1, std::memory_order_acq_rel);
    }

    void workerLoop() {
        for (;;) {
            Job job;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this] { return quit_ || !queue_.empty(); });
                if (quit_ && queue_.empty()) return;
                job = std::move(queue_.front());
                queue_.pop_front();
            }
            run(job);
        }
    }

    std::mutex mutex_;
    std::mutex start_mutex_;
    std::condition_variable cv_;
    std::deque<Job> queue_;
    std::vector<std::thread> workers_;
    std::atomic<bool> started_{false};
    std::atomic<std::uint64_t> executed_{0};
    int worker_count_ = 0;
    bool quit_ = false;
};

Pool& pool() {
    static Pool p;
    return p;
}

}  // namespace

JobHandle schedule(std::function<void()> job, JobHandle group) {
    JobHandle counter = group ? std::move(group) : std::make_shared<JobCounter>();
    counter->pending.fetch_add(1, std::memory_order_acq_rel);
    pool().push(Job{std::move(job), counter});
    return counter;
}

void wait(const JobHandle& handle) { pool().waitFor(handle); }

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
        schedule([&body, begin, end] { body(begin, end); }, group);
    }
    body(0, std::min(chunk, count));
    wait(group);
}

int workerCount() {
    pool().ensureStarted();
    return pool().workers();
}

void setWorkerCount(int threads) { pool().restart(threads); }

void shutdown() { pool().stop(); }

JobStats stats() {
    JobStats s;
    s.executed = pool().executed();
    s.queued = pool().queued();
    s.workers = pool().workers();
    return s;
}

}  // namespace cramion::jobs
