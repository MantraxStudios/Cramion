#include "CramionCore/profiling/Profiler.h"

#include "CramionCore/cvar/CVar.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>

namespace cramion::prof {
namespace {

cvar::CVar<bool> g_enabled("prof.Enabled", true,
                           "Perfilador de CPU (Insights): mide las zonas de cada frame. Apagado no cuesta nada",
                           cvar::Saved);
cvar::CVar<float> g_hitch_ms("prof.HitchMs", 50.0f,
                             "Un frame mas largo que esto es un tiron: se guarda una captura .crtrace de los frames de "
                             "antes (0 = no capturar)",
                             cvar::Saved, 0.0f, 10000.0f);
cvar::CVar<int> g_hitch_frames("prof.HitchFrames", 120, "Frames que entran en una captura de tiron", cvar::Saved, 10,
                               2000);
cvar::CVar<float> g_hitch_cooldown("prof.HitchCooldown", 10.0f,
                                   "Segundos minimos entre dos capturas de tiron (los de cargar no llenan el disco)",
                                   cvar::Saved, 0.0f, 3600.0f);
cvar::CVar<int> g_hitch_keep("prof.HitchKeep", 20, "Capturas de tiron que se guardan como mucho (las viejas se borran)",
                             cvar::Saved, 1, 1000);

using Clock = std::chrono::steady_clock;

struct Event {
    const char* name = nullptr;
    std::int64_t start_ns = 0;
    std::int64_t end_ns = 0;
    std::uint32_t thread = 0;
    std::uint16_t depth = 0;
    std::uint16_t parent = 0xFFFF;  // indice del padre en el frame (mismo hilo) o 0xFFFF
};

struct Frame {
    std::uint64_t index = 0;
    std::int64_t start_ns = 0;
    std::int64_t end_ns = 0;
    std::vector<Event> events;
    std::vector<std::pair<const char*, double>> counters;
};

struct ThreadState {
    std::uint32_t id = 0;
    std::vector<std::uint32_t> stack;  // indices en current.events
};

constexpr std::size_t kHistory = 600;

struct State {
    std::mutex mutex;
    Frame current;
    bool in_frame = false;
    std::deque<Frame> history;
    std::uint64_t frame_index = 0;
    std::set<std::string> interned;
    std::filesystem::path folder;
    std::filesystem::path last_capture;
    double last_capture_time = -1e9;
    std::function<void(const std::string&)> log;
    std::atomic<std::uint32_t> next_thread{1};
    std::thread::id main_thread = std::this_thread::get_id();
};

State& S() {
    static State s;
    return s;
}

const Clock::time_point kEpoch = Clock::now();

std::int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - kEpoch).count();
}

ThreadState& thread_state() {
    thread_local ThreadState t{std::this_thread::get_id() == S().main_thread ? 0u : S().next_thread.fetch_add(1), {}};
    return t;
}

double ms(std::int64_t ns) { return static_cast<double>(ns) / 1.0e6; }

std::filesystem::path default_folder() {
    if (const char* local = std::getenv("LOCALAPPDATA")) {
        return std::filesystem::path(local) / "Cramion" / "Traces";
    }
    return std::filesystem::temp_directory_path() / "CramionTraces";
}

double percentile(std::vector<double> v, double p) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const double pos = p * static_cast<double>(v.size() - 1);
    const std::size_t i = static_cast<std::size_t>(pos);
    const double f = pos - static_cast<double>(i);
    return i + 1 < v.size() ? v[i] * (1.0 - f) + v[i + 1] * f : v[i];
}

std::string json_escape(const char* s) {
    std::string out;
    for (const char* c = s; c != nullptr && *c; ++c) {
        if (*c == '"' || *c == '\\') out += '\\';
        if (static_cast<unsigned char>(*c) < 0x20) continue;
        out += *c;
    }
    return out;
}

bool write_trace_locked(State& s, const std::filesystem::path& file, int frames, std::string* error) {
    std::error_code ec;
    if (file.has_parent_path()) std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::binary);
    if (!out) {
        if (error) *error = "no se puede escribir " + file.string();
        return false;
    }
    const std::size_t n = std::min<std::size_t>(static_cast<std::size_t>(std::max(frames, 1)), s.history.size());
    out << "{\"displayTimeUnit\":\"ms\",\"otherData\":{\"engine\":\"Cramion\",\"frames\":" << n << "},\"traceEvents\":[\n";
    out << "{\"name\":\"thread_name\",\"ph\":\"M\",\"pid\":1,\"tid\":0,\"args\":{\"name\":\"Principal\"}}";
    std::set<std::uint32_t> threads;
    for (std::size_t k = s.history.size() - n; k < s.history.size(); ++k) {
        const Frame& f = s.history[k];
        char buf[256];
        std::snprintf(buf, sizeof(buf), ",\n{\"name\":\"Frame %llu\",\"cat\":\"frame\",\"ph\":\"X\",\"pid\":1,\"tid\":0,"
                                        "\"ts\":%.3f,\"dur\":%.3f}",
                      static_cast<unsigned long long>(f.index), static_cast<double>(f.start_ns) / 1000.0,
                      static_cast<double>(f.end_ns - f.start_ns) / 1000.0);
        out << buf;
        for (const Event& e : f.events) {
            if (e.end_ns <= e.start_ns) continue;
            threads.insert(e.thread);
            out << ",\n{\"name\":\"" << json_escape(e.name) << "\",\"cat\":\"cpu\",\"ph\":\"X\",\"pid\":1,\"tid\":"
                << e.thread;
            std::snprintf(buf, sizeof(buf), ",\"ts\":%.3f,\"dur\":%.3f}", static_cast<double>(e.start_ns) / 1000.0,
                          static_cast<double>(e.end_ns - e.start_ns) / 1000.0);
            out << buf;
        }
        for (const auto& [name, value] : f.counters) {
            std::snprintf(buf, sizeof(buf), ",\"ts\":%.3f,\"args\":{\"valor\":%.4f}}",
                          static_cast<double>(f.end_ns) / 1000.0, value);
            out << ",\n{\"name\":\"" << json_escape(name) << "\",\"ph\":\"C\",\"pid\":1,\"tid\":0" << buf;
        }
    }
    for (std::uint32_t t : threads) {
        if (t == 0) continue;
        out << ",\n{\"name\":\"thread_name\",\"ph\":\"M\",\"pid\":1,\"tid\":" << t << ",\"args\":{\"name\":\"Hilo " << t
            << "\"}}";
    }
    out << "\n]}\n";
    return static_cast<bool>(out);
}

void prune_captures(const std::filesystem::path& folder) {
    std::error_code ec;
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
        const std::string name = entry.path().filename().string();
        if (name.rfind("tiron_", 0) == 0 && entry.path().extension() == ".crtrace") files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    const std::size_t keep = static_cast<std::size_t>(std::max(g_hitch_keep.get(), 1));
    for (std::size_t i = 0; i + keep < files.size(); ++i) std::filesystem::remove(files[i], ec);
}

}  // namespace

const char* intern(const std::string& name) {
    State& s = S();
    std::lock_guard lock(s.mutex);
    return s.interned.insert(name).first->c_str();
}

bool enabled() { return g_enabled.get(); }
void setEnabled(bool on) { g_enabled.set(on); }
std::uint64_t frameIndex() { return S().frame_index; }

void begin(const char* name) {
    if (!g_enabled.get()) return;
    State& s = S();
    ThreadState& t = thread_state();
    const std::int64_t now = now_ns();
    std::lock_guard lock(s.mutex);
    if (!s.in_frame) return;
    Event e;
    e.name = name;
    e.start_ns = now;
    e.thread = t.id;
    e.depth = static_cast<std::uint16_t>(t.stack.size());
    e.parent = t.stack.empty() ? 0xFFFF : static_cast<std::uint16_t>(std::min<std::uint32_t>(t.stack.back(), 0xFFFE));
    if (s.current.events.size() >= 0xFFFE) return;  // demasiadas zonas en un frame
    t.stack.push_back(static_cast<std::uint32_t>(s.current.events.size()));
    s.current.events.push_back(e);
}

void end() {
    if (!g_enabled.get()) return;
    State& s = S();
    ThreadState& t = thread_state();
    const std::int64_t now = now_ns();
    std::lock_guard lock(s.mutex);
    if (t.stack.empty()) return;
    const std::uint32_t i = t.stack.back();
    t.stack.pop_back();
    if (i < s.current.events.size()) s.current.events[i].end_ns = now;
}

void counter(const char* name, double value) {
    if (!g_enabled.get()) return;
    State& s = S();
    std::lock_guard lock(s.mutex);
    if (s.in_frame) s.current.counters.emplace_back(name, value);
}

void beginFrame() {
    State& s = S();
    std::lock_guard lock(s.mutex);
    s.current = Frame{};
    s.current.index = ++s.frame_index;
    s.current.start_ns = now_ns();
    s.in_frame = g_enabled.get();
    thread_state().stack.clear();
}

void endFrame() {
    State& s = S();
    std::string message;
    {
        std::lock_guard lock(s.mutex);
        if (!s.in_frame) return;
        s.in_frame = false;
        s.current.end_ns = now_ns();
        // Zonas sin cerrar (un return a mitad): acaban con el frame.
        for (Event& e : s.current.events) {
            if (e.end_ns == 0) e.end_ns = s.current.end_ns;
        }
        thread_state().stack.clear();
        s.history.push_back(std::move(s.current));
        if (s.history.size() > kHistory) s.history.pop_front();
        // Tiron: captura de los frames de antes.
        const Frame& f = s.history.back();
        const double frame_ms = ms(f.end_ns - f.start_ns);
        const double seconds = static_cast<double>(f.end_ns) / 1.0e9;
        const float threshold = g_hitch_ms.get();
        if (threshold > 0.0f && frame_ms > threshold && s.history.size() > 30 &&
            seconds - s.last_capture_time > g_hitch_cooldown.get()) {
            s.last_capture_time = seconds;
            if (s.folder.empty()) s.folder = default_folder();
            const std::time_t tt = std::time(nullptr);
            std::tm tm{};
#ifdef _WIN32
            localtime_s(&tm, &tt);
#else
            localtime_r(&tt, &tm);
#endif
            char stamp[64];
            std::strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &tm);
            const std::filesystem::path file =
                s.folder / (std::string("tiron_") + stamp + "_" + std::to_string(static_cast<int>(frame_ms)) + "ms.crtrace");
            std::string error;
            if (write_trace_locked(s, file, g_hitch_frames.get(), &error)) {
                s.last_capture = file;
                prune_captures(s.folder);
                // La zona mas cara del frame del tiron (para el aviso).
                const char* worst = nullptr;
                std::int64_t worst_ns = 0;
                for (const Event& e : f.events) {
                    if (e.depth == 0 && e.thread == 0 && e.end_ns - e.start_ns > worst_ns) {
                        worst_ns = e.end_ns - e.start_ns;
                        worst = e.name;
                    }
                }
                message = "[Insights] Tiron de " + std::to_string(static_cast<int>(frame_ms)) + " ms" +
                          (worst ? std::string(" (sobre todo ") + worst + ": " +
                                       std::to_string(static_cast<int>(ms(worst_ns))) + " ms)"
                                 : std::string()) +
                          ". Captura: " + file.string();
            }
        }
    }
    if (!message.empty() && s.log) s.log(message);
}

std::vector<ZoneStats> stats(int frames) {
    State& s = S();
    std::lock_guard lock(s.mutex);
    const std::size_t n = std::min<std::size_t>(static_cast<std::size_t>(std::max(frames, 1)), s.history.size());
    struct Acc {
        std::string name;
        int depth = 0;
        std::uint32_t thread = 0;
        std::vector<double> per_frame;  // ms por frame (inclusivo)
        double self_total = 0.0;
        double calls = 0.0;
        double last = 0.0;
        std::size_t order = 0;
    };
    std::map<std::string, Acc> acc;
    std::size_t order = 0;
    for (std::size_t k = s.history.size() - n; k < s.history.size(); ++k) {
        const Frame& f = s.history[k];
        // Ruta de cada evento (por su padre).
        std::vector<std::string> paths(f.events.size());
        std::vector<double> child_ms(f.events.size(), 0.0);
        for (std::size_t i = 0; i < f.events.size(); ++i) {
            const Event& e = f.events[i];
            const std::string prefix = e.parent != 0xFFFF && e.parent < i ? paths[e.parent] + "/" : std::string();
            paths[i] = prefix + (e.thread != 0 ? "[" + std::to_string(e.thread) + "] " : std::string()) + e.name;
            if (e.parent != 0xFFFF && e.parent < i) child_ms[e.parent] += ms(e.end_ns - e.start_ns);
        }
        std::unordered_map<std::string, std::pair<double, double>> frame_sum;  // inclusivo, self
        std::unordered_map<std::string, int> frame_calls;
        for (std::size_t i = 0; i < f.events.size(); ++i) {
            const Event& e = f.events[i];
            const double d = ms(e.end_ns - e.start_ns);
            auto& fs = frame_sum[paths[i]];
            fs.first += d;
            fs.second += std::max(d - child_ms[i], 0.0);
            ++frame_calls[paths[i]];
            Acc& a = acc[paths[i]];
            if (a.name.empty()) {
                a.name = e.name;
                a.depth = e.depth;
                a.thread = e.thread;
                a.order = order++;
            }
        }
        for (auto& [path, a] : acc) {
            const auto it = frame_sum.find(path);
            const double v = it != frame_sum.end() ? it->second.first : 0.0;
            a.per_frame.push_back(v);
            if (it != frame_sum.end()) {
                a.self_total += it->second.second;
                a.calls += frame_calls[path];
            }
            if (k + 1 == s.history.size()) a.last = v;
        }
    }
    std::vector<ZoneStats> out;
    for (auto& [path, a] : acc) {
        ZoneStats z;
        z.path = path;
        z.name = a.name;
        z.depth = a.depth;
        z.thread = a.thread;
        double sum = 0.0, mx = 0.0;
        for (double v : a.per_frame) {
            sum += v;
            mx = std::max(mx, v);
        }
        const double frames_d = static_cast<double>(std::max<std::size_t>(n, 1));
        z.avg_ms = sum / frames_d;
        z.self_ms = a.self_total / frames_d;
        z.max_ms = mx;
        z.p95_ms = percentile(a.per_frame, 0.95);
        z.last_ms = a.last;
        z.calls = a.calls / frames_d;
        out.push_back(std::move(z));
    }
    // Orden de arbol: por ruta (los hijos quedan detras del padre).
    std::sort(out.begin(), out.end(), [](const ZoneStats& a, const ZoneStats& b) { return a.path < b.path; });
    return out;
}

std::vector<float> frameTimes() {
    State& s = S();
    std::lock_guard lock(s.mutex);
    std::vector<float> out;
    out.reserve(s.history.size());
    for (const Frame& f : s.history) out.push_back(static_cast<float>(ms(f.end_ns - f.start_ns)));
    return out;
}

std::vector<CounterStats> counters(int frames) {
    State& s = S();
    std::lock_guard lock(s.mutex);
    const std::size_t n = std::min<std::size_t>(static_cast<std::size_t>(std::max(frames, 1)), s.history.size());
    std::map<std::string, CounterStats> acc;
    std::map<std::string, int> count;
    for (std::size_t k = s.history.size() - n; k < s.history.size(); ++k) {
        for (const auto& [name, value] : s.history[k].counters) {
            CounterStats& c = acc[name];
            c.name = name;
            c.last = value;
            c.avg += value;
            c.max = std::max(c.max, value);
            ++count[name];
        }
    }
    std::vector<CounterStats> out;
    for (auto& [name, c] : acc) {
        c.avg /= std::max(count[name], 1);
        out.push_back(c);
    }
    return out;
}

FrameSummary summary(int frames) {
    std::vector<float> times = frameTimes();
    FrameSummary r;
    const std::size_t n = std::min<std::size_t>(static_cast<std::size_t>(std::max(frames, 1)), times.size());
    if (n == 0) return r;
    std::vector<double> v(times.end() - static_cast<std::ptrdiff_t>(n), times.end());
    double sum = 0.0;
    for (double x : v) {
        sum += x;
        r.max_ms = std::max(r.max_ms, x);
        if (g_hitch_ms.get() > 0.0f && x > g_hitch_ms.get()) ++r.hitches;
    }
    r.avg_ms = sum / static_cast<double>(n);
    r.p50_ms = percentile(v, 0.50);
    r.p95_ms = percentile(v, 0.95);
    r.p99_ms = percentile(v, 0.99);
    return r;
}

bool saveTrace(const std::filesystem::path& file, int frames, std::string* error) {
    State& s = S();
    std::lock_guard lock(s.mutex);
    if (s.history.empty()) {
        if (error) *error = "no hay frames medidos (prof.Enabled apagado?)";
        return false;
    }
    return write_trace_locked(s, file, frames, error);
}

std::filesystem::path captureFolder() {
    State& s = S();
    std::lock_guard lock(s.mutex);
    if (s.folder.empty()) s.folder = default_folder();
    return s.folder;
}

void setCaptureFolder(const std::filesystem::path& folder) {
    State& s = S();
    std::lock_guard lock(s.mutex);
    s.folder = folder;
}

std::filesystem::path lastHitchCapture() {
    State& s = S();
    std::lock_guard lock(s.mutex);
    return s.last_capture;
}

void setLog(std::function<void(const std::string&)> log) {
    State& s = S();
    std::lock_guard lock(s.mutex);
    s.log = std::move(log);
}

void reset() {
    State& s = S();
    std::lock_guard lock(s.mutex);
    s.history.clear();
    s.current = Frame{};
    s.in_frame = false;
    s.frame_index = 0;
    s.last_capture.clear();
    s.last_capture_time = -1e9;
    thread_state().stack.clear();
}

}  // namespace cramion::prof
