// Android: assets del APK, carpeta de datos, logs a logcat, pantalla de
// carga y crashes (lo que en Windows hace LoadingScreen.cpp).

#include "AndroidSupport.h"

#include "LoadingScreen.h"

#include <CramionCore/platform/MemoryPressure.h>

#include <android/asset_manager.h>
#include <android/log.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <jni.h>
#include <signal.h>
#include <ucontext.h>
#include <unistd.h>
#include <unwind.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <exception>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <mutex>
#include <streambuf>
#include <utility>
#include <vector>

namespace cramion::android {

namespace {

std::filesystem::path g_root = "/data/local/tmp";
AAssetManager* g_assets = nullptr;

// Un streambuf que manda cada linea a logcat y al archivo de log.
class LogBuffer final : public std::streambuf {
public:
    LogBuffer(int priority, std::ofstream* file) : priority_(priority), file_(file) {}

protected:
    int overflow(int c) override {
        if (c == traits_type::eof()) return 0;
        std::lock_guard lock(mutex());
        if (c == '\n') {
            flushLine();
        } else {
            line_ += static_cast<char>(c);
            if (line_.size() > 900) flushLine();  // logcat corta las lineas largas
        }
        return c;
    }
    // cerr hace sync tras cada <<: la linea se manda entera al llegar el salto
    // (si no, logcat la parte en trozos).
    int sync() override { return 0; }

private:
    static std::mutex& mutex() {
        static std::mutex m;
        return m;
    }
    void flushLine() {
        __android_log_write(priority_, "Cramion", line_.c_str());
        editor::crashBreadcrumb(line_);  // las ultimas lineas van al informe de un cierre
        if (file_ != nullptr && *file_) {
            *file_ << line_ << '\n';
            file_->flush();
        }
        line_.clear();
    }
    int priority_;
    std::ofstream* file_;
    std::string line_;
};

}  // namespace

void setDataRoot(const std::filesystem::path& folder) { g_root = folder; }
const std::filesystem::path& dataRoot() { return g_root; }

void redirectLogs(const std::filesystem::path& file) {
    static std::ofstream log_file;
    std::error_code e;
    std::filesystem::create_directories(file.parent_path(), e);
    log_file.open(file, std::ios::trunc);
    static LogBuffer out(ANDROID_LOG_INFO, &log_file);
    static LogBuffer err(ANDROID_LOG_WARN, &log_file);
    std::cout.rdbuf(&out);
    std::cerr.rdbuf(&err);
}

void setAssetManager(AAssetManager* manager) { g_assets = manager; }

bool assetExists(const std::string& name) {
    if (g_assets == nullptr) return false;
    AAsset* asset = AAssetManager_open(g_assets, name.c_str(), AASSET_MODE_UNKNOWN);
    if (asset == nullptr) return false;
    AAsset_close(asset);
    return true;
}

std::uint64_t assetSize(const std::string& name) {
    if (g_assets == nullptr) return 0;
    AAsset* asset = AAssetManager_open(g_assets, name.c_str(), AASSET_MODE_UNKNOWN);
    if (asset == nullptr) return 0;
    const std::uint64_t size = static_cast<std::uint64_t>(AAsset_getLength64(asset));
    AAsset_close(asset);
    return size;
}

std::vector<std::string> assetFolder(const std::string& folder) {
    std::vector<std::string> files;
    if (g_assets == nullptr) return files;
    AAssetDir* dir = AAssetManager_openDir(g_assets, folder.c_str());
    if (dir == nullptr) return files;
    while (const char* name = AAssetDir_getNextFileName(dir)) files.emplace_back(name);
    AAssetDir_close(dir);
    return files;
}

bool extractAsset(const std::string& name, const std::filesystem::path& to, const CopyProgress& progress,
                  std::string* error) {
    if (g_assets == nullptr) {
        if (error != nullptr) *error = "sin AssetManager";
        return false;
    }
    AAsset* asset = AAssetManager_open(g_assets, name.c_str(), AASSET_MODE_STREAMING);
    if (asset == nullptr) {
        if (error != nullptr) *error = "no esta en el APK: " + name;
        return false;
    }
    const std::uint64_t total = static_cast<std::uint64_t>(AAsset_getLength64(asset));
    std::error_code e;
    std::filesystem::create_directories(to.parent_path(), e);
    std::filesystem::path temporary = to;
    temporary += ".part";
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    if (!out) {
        AAsset_close(asset);
        if (error != nullptr) *error = "no se pudo escribir " + to.string();
        return false;
    }
    std::vector<char> buffer(1u << 20);
    std::uint64_t done = 0;
    int read = 0;
    while ((read = AAsset_read(asset, buffer.data(), buffer.size())) > 0) {
        out.write(buffer.data(), read);
        done += static_cast<std::uint64_t>(read);
        if (progress) progress(done, total);
    }
    AAsset_close(asset);
    out.close();
    if (read < 0 || !out) {
        std::filesystem::remove(temporary, e);
        if (error != nullptr) *error = "no se pudo copiar " + name + " (sin espacio?)";
        return false;
    }
    std::filesystem::rename(temporary, to, e);
    if (e) {
        if (error != nullptr) *error = "no se pudo renombrar " + to.string();
        return false;
    }
    return true;
}

bool openAssetRange(const std::string& name, AssetRange& range) {
    if (g_assets == nullptr) return false;
    AAsset* asset = AAssetManager_open(g_assets, name.c_str(), AASSET_MODE_UNKNOWN);
    if (asset == nullptr) return false;
    off64_t start = 0;
    off64_t length = 0;
    const int fd = AAsset_openFileDescriptor64(asset, &start, &length);
    AAsset_close(asset);
    if (fd < 0) return false;
    range.fd = fd;
    range.offset = static_cast<std::uint64_t>(start);
    range.length = static_cast<std::uint64_t>(length);
    range.path = "/proc/self/fd/" + std::to_string(fd);
    return true;
}

void closeAssetRange(AssetRange& range) {
    if (range.fd >= 0) close(range.fd);
    range.fd = -1;
}

// NativeActivity no recibe onTrimMemory (es de Java): se lee el ultimo nivel
// que el sistema le mando al proceso con
// ActivityManager.getMyMemoryState(RunningAppProcessInfo).lastTrimLevel
// (API 16+). Cada 2 s como mucho; si sube (o cambia a otro aviso), se avisa a
// MemoryPressure. onLowMemory llega por la glue (APP_CMD_LOW_MEMORY).
int pollMemoryPressure(void* java_vm) {
    static auto last_poll = std::chrono::steady_clock::time_point{};
    static int last_level = 0;
    static jclass manager_class = nullptr;
    static jclass info_class = nullptr;
    static jmethodID info_ctor = nullptr;
    static jmethodID get_state = nullptr;
    static jfieldID trim_field = nullptr;
    static bool broken = false;
    const auto now = std::chrono::steady_clock::now();
    if (java_vm == nullptr || broken || now - last_poll < std::chrono::seconds(2)) return 0;
    last_poll = now;
    JavaVM* vm = static_cast<JavaVM*>(java_vm);
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        // El hilo del juego se queda enganchado (sale con _exit).
        if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK || env == nullptr) {
            broken = true;
            return 0;
        }
    }
    const auto fail = [&] {
        if (env->ExceptionCheck()) env->ExceptionClear();
        broken = true;
        std::cerr << "[Memoria] No se pueden leer los avisos de memoria del sistema (lastTrimLevel)\n";
        return 0;
    };
    if (manager_class == nullptr) {
        jclass manager = env->FindClass("android/app/ActivityManager");
        jclass info = env->FindClass("android/app/ActivityManager$RunningAppProcessInfo");
        if (manager == nullptr || info == nullptr) return fail();
        manager_class = static_cast<jclass>(env->NewGlobalRef(manager));
        info_class = static_cast<jclass>(env->NewGlobalRef(info));
        env->DeleteLocalRef(manager);
        env->DeleteLocalRef(info);
        info_ctor = env->GetMethodID(info_class, "<init>", "()V");
        get_state = env->GetStaticMethodID(manager_class, "getMyMemoryState",
                                           "(Landroid/app/ActivityManager$RunningAppProcessInfo;)V");
        trim_field = env->GetFieldID(info_class, "lastTrimLevel", "I");
        if (info_ctor == nullptr || get_state == nullptr || trim_field == nullptr) return fail();
    }
    jobject info = env->NewObject(info_class, info_ctor);
    if (info == nullptr) return fail();
    env->CallStaticVoidMethod(manager_class, get_state, info);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        env->DeleteLocalRef(info);
        return 0;
    }
    const int level = env->GetIntField(info, trim_field);
    env->DeleteLocalRef(info);
    if (level != last_level) {
        last_level = level;
        if (level > 0) {
            memory::notifyTrimMemory(level);
            return level;
        }
    }
    return 0;
}

}  // namespace cramion::android

// -----------------------------------------------------------------------------
// Lo de LoadingScreen.h en Android
// -----------------------------------------------------------------------------

namespace cramion::editor {

namespace {

std::function<void(float, const char*)>& painter() {
    static std::function<void(float, const char*)> p;
    return p;
}

std::string g_app_name = "Cramion";
std::filesystem::path g_crash_folder;
struct sigaction g_previous[32];

// Lo que la senal lee ya preparado (sin memoria dinamica en la senal).
char g_crash_dir[1024] = "";
char g_app_name_c[128] = "Cramion";
char g_log_path[1024] = "";
char g_terminate_reason[512] = "";
std::atomic<int> g_entered{0};

// Ultimas lineas del registro (migas) y datos del contexto: la senal solo
// los lee si puede coger el mutex (un cierre con el mutex cogido no se cuelga).
constexpr std::size_t kBreadcrumbs = 80;
std::mutex g_crumb_mutex;
std::array<std::string, kBreadcrumbs> g_crumbs;
std::size_t g_crumb_next = 0;
std::size_t g_crumb_count = 0;
std::mutex g_context_mutex;
std::vector<std::pair<std::string, std::string>> g_context;

struct Backtrace {
    void* frames[64];
    int count = 0;
};

_Unwind_Reason_Code unwindFrame(_Unwind_Context* context, void* arg) {
    Backtrace* trace = static_cast<Backtrace*>(arg);
    const uintptr_t pc = _Unwind_GetIP(context);
    if (pc != 0 && trace->count < 64) trace->frames[trace->count++] = reinterpret_cast<void*>(pc);
    return trace->count < 64 ? _URC_NO_REASON : _URC_END_OF_STACK;
}

// Escritura sin buffer (de la senal).
void put(int fd, const char* text) {
    if (fd < 0 || text == nullptr) return;
    std::size_t size = std::strlen(text);
    while (size > 0) {
        const ssize_t done = ::write(fd, text, size);
        if (done < 0 && errno == EINTR) continue;
        if (done <= 0) return;
        text += done;
        size -= static_cast<std::size_t>(done);
    }
}

void putHex(int fd, std::uintptr_t v) {
    char b[2 + 16 + 1];
    char* p = b + sizeof(b) - 1;
    *p = '\0';
    do {
        *--p = "0123456789abcdef"[v & 0xF];
        v >>= 4;
    } while (v != 0);
    *--p = 'x';
    *--p = '0';
    put(fd, p);
}

void putNumber(int fd, long long v) {
    char b[24];
    char* p = b + sizeof(b) - 1;
    *p = '\0';
    const bool negative = v < 0;
    unsigned long long u = negative ? 0ull - static_cast<unsigned long long>(v) : static_cast<unsigned long long>(v);
    do {
        *--p = static_cast<char>('0' + u % 10);
        u /= 10;
    } while (u != 0);
    if (negative) *--p = '-';
    put(fd, p);
}

// Copia las lineas de un archivo de /proc que empiezan por alguno de los prefijos.
void putProcLines(int fd, const char* file, std::initializer_list<const char*> prefixes) {
    const int in = ::open(file, O_RDONLY | O_CLOEXEC);
    if (in < 0) return;
    char buffer[4096];
    const ssize_t n = ::read(in, buffer, sizeof(buffer) - 1);
    ::close(in);
    if (n <= 0) return;
    buffer[n] = '\0';
    char* line = buffer;
    while (*line != '\0') {
        char* end = std::strchr(line, '\n');
        if (end != nullptr) *end = '\0';
        for (const char* prefix : prefixes) {
            if (std::strncmp(line, prefix, std::strlen(prefix)) == 0) {
                put(fd, line);
                put(fd, "\n");
            }
        }
        if (end == nullptr) break;
        line = end + 1;
    }
}

const char* signalName(int signal, int code) {
    switch (signal) {
        case SIGSEGV: return "acceso a memoria no valida (SIGSEGV)";
        case SIGBUS: return "error de bus (SIGBUS: archivo mapeado o acceso desalineado)";
        case SIGILL: return "instruccion ilegal (SIGILL)";
        case SIGFPE: return code == FPE_INTDIV ? "division entera por cero (SIGFPE)" : "error aritmetico (SIGFPE)";
        case SIGABRT: return "abort() (SIGABRT)";
        default: return "senal";
    }
}

// Un marco de la pila: modulo + desplazamiento (+ funcion si la exporta). Con
// llvm-symbolizer y la libmain.so con simbolos (la de la carpeta de
// compilacion) da la linea.
void putFrame(int fd, int index, void* address) {
    Dl_info dl{};
    put(fd, "  #");
    putNumber(fd, index);
    put(fd, " ");
    if (address != nullptr && dladdr(address, &dl) != 0 && dl.dli_fname != nullptr) {
        const char* slash = std::strrchr(dl.dli_fname, '/');
        put(fd, slash != nullptr ? slash + 1 : dl.dli_fname);
        put(fd, " + ");
        putHex(fd, reinterpret_cast<std::uintptr_t>(address) - reinterpret_cast<std::uintptr_t>(dl.dli_fbase));
        if (dl.dli_sname != nullptr) {
            put(fd, " (");
            put(fd, dl.dli_sname);
            put(fd, ")");
        }
    } else {
        putHex(fd, reinterpret_cast<std::uintptr_t>(address));
    }
    put(fd, "\n");
}

// Informe: motivo, donde fallo, contexto, memoria, la pila (modulo +
// desplazamiento) y las ultimas lineas del registro, en Crashes/ (y la marca
// <app>.pending para el siguiente arranque). En la senal solo open/write/
// dladdr y lo preparado al instalar; las migas y el contexto si se coge el
// mutex. Despues, el del sistema (tombstone).
void onSignal(int signal, siginfo_t* info, void* context) {
    if (g_entered.exchange(1) == 0) {
        Backtrace trace;
        _Unwind_Backtrace(unwindFrame, &trace);
        // Donde fallo: el contador de programa del contexto.
        void* pc = nullptr;
#if defined(__aarch64__)
        if (context != nullptr) pc = reinterpret_cast<void*>(static_cast<ucontext_t*>(context)->uc_mcontext.pc);
#elif defined(__arm__)
        if (context != nullptr) pc = reinterpret_cast<void*>(static_cast<ucontext_t*>(context)->uc_mcontext.arm_pc);
#elif defined(__x86_64__)
        if (context != nullptr) pc = reinterpret_cast<void*>(static_cast<ucontext_t*>(context)->uc_mcontext.gregs[REG_RIP]);
#else
        (void)context;
#endif
        if (pc == nullptr && trace.count > 0) pc = trace.frames[0];
        const std::time_t now = std::time(nullptr);
        std::tm local{};
        localtime_r(&now, &local);
        char stamp[32];
        std::strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &local);
        char report[1400];
        std::snprintf(report, sizeof(report), "%s/%s_%s.txt", g_crash_dir, g_app_name_c, stamp);
        const char* reason = signal == SIGABRT && g_terminate_reason[0] != '\0'
                                 ? g_terminate_reason
                                 : signalName(signal, info != nullptr ? info->si_code : 0);
        const int fd = g_crash_dir[0] != '\0' ? ::open(report, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644) : -1;
        if (fd >= 0) {
            put(fd, g_app_name_c);
            put(fd, " se cerro por un error\nFecha: ");
            put(fd, stamp);
            put(fd, "\nMotivo: ");
            put(fd, reason);
            put(fd, "\nCodigo: senal ");
            putNumber(fd, signal);
            put(fd, "\nPlataforma: android\nDonde:\n");
            putFrame(fd, 0, pc);
            if ((signal == SIGSEGV || signal == SIGBUS) && info != nullptr) {
                const auto address = reinterpret_cast<std::uintptr_t>(info->si_addr);
                put(fd, "Acceso: ");
                putHex(fd, address);
                if (address < 0x10000) put(fd, " (puntero nulo)");
                put(fd, "\n");
            }
            put(fd, "\n--- Contexto ---\n");
            if (g_context_mutex.try_lock()) {
                for (const auto& [k, v] : g_context) {
                    put(fd, k.c_str());
                    put(fd, ": ");
                    put(fd, v.c_str());
                    put(fd, "\n");
                }
                g_context_mutex.unlock();
            }
            putProcLines(fd, "/proc/meminfo", {"MemTotal:", "MemAvailable:"});
            putProcLines(fd, "/proc/self/status", {"VmRSS:", "VmHWM:", "Threads:"});
            put(fd, "\n--- Pila ---\n");
            for (int i = 0; i < trace.count; ++i) putFrame(fd, i, trace.frames[i]);
            put(fd, "\n--- Ultimas lineas del registro ---\n");
            bool crumbs = false;
            if (g_crumb_mutex.try_lock()) {
                const std::size_t first = (g_crumb_next + kBreadcrumbs - g_crumb_count) % kBreadcrumbs;
                for (std::size_t i = 0; i < g_crumb_count; ++i) {
                    put(fd, g_crumbs[(first + i) % kBreadcrumbs].c_str());
                    put(fd, "\n");
                }
                crumbs = g_crumb_count > 0;
                g_crumb_mutex.unlock();
            }
            if (!crumbs && g_log_path[0] != '\0') {
                // El final del registro (6000 bytes).
                const int log = ::open(g_log_path, O_RDONLY | O_CLOEXEC);
                if (log >= 0) {
                    const off_t size = ::lseek(log, 0, SEEK_END);
                    ::lseek(log, size > 6000 ? size - 6000 : 0, SEEK_SET);
                    char buffer[6001];
                    const ssize_t n = ::read(log, buffer, 6000);
                    if (n > 0) {
                        buffer[n] = '\0';
                        put(fd, buffer);
                    }
                    ::close(log);
                }
            }
            ::close(fd);
            // Marca para avisar (y enviarlo, si el juego lo pide) en el siguiente arranque.
            char marker[1400];
            std::snprintf(marker, sizeof(marker), "%s/%s.pending", g_crash_dir, g_app_name_c);
            const int mfd = ::open(marker, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
            if (mfd >= 0) {
                put(mfd, report);
                ::close(mfd);
            }
        }
        // logcat: el motivo y la pila (como antes).
        char line[1600];
        std::snprintf(line, sizeof(line), "%s se cerro por un error: %s (senal %d) en %p; informe: %s", g_app_name_c,
                      reason, signal, info != nullptr ? info->si_addr : nullptr, fd >= 0 ? report : "(no se pudo guardar)");
        __android_log_write(ANDROID_LOG_FATAL, "Cramion", line);
        for (int i = 0; i < trace.count; ++i) {
            Dl_info dl{};
            const char* module = "?";
            uintptr_t offset = reinterpret_cast<uintptr_t>(trace.frames[i]);
            if (dladdr(trace.frames[i], &dl) != 0 && dl.dli_fname != nullptr) {
                module = std::strrchr(dl.dli_fname, '/') != nullptr ? std::strrchr(dl.dli_fname, '/') + 1 : dl.dli_fname;
                offset -= reinterpret_cast<uintptr_t>(dl.dli_fbase);
            }
            std::snprintf(line, sizeof(line), "  #%d %s + 0x%lx", i, module, static_cast<unsigned long>(offset));
            __android_log_write(ANDROID_LOG_FATAL, "Cramion", line);
        }
    }
    // El manejador del sistema (tombstone) despues del nuestro.
    if (signal > 0 && signal < 32) sigaction(signal, &g_previous[signal], nullptr);
    raise(signal);
}

// std::terminate: el motivo (la excepcion) y abort(), que entra por SIGABRT.
void onTerminate() {
    std::string reason = "std::terminate (excepcion de C++ sin capturar)";
    if (const std::exception_ptr current = std::current_exception()) {
        try {
            std::rethrow_exception(current);
        } catch (const std::exception& e) {
            reason += ": ";
            reason += e.what();
        } catch (...) {
        }
    }
    std::snprintf(g_terminate_reason, sizeof(g_terminate_reason), "%s", reason.c_str());
    std::abort();
}

}  // namespace

void setLoadingPainter(std::function<void(float fraction, const char* status)> p) { painter() = std::move(p); }

LoadingScreen::LoadingScreen(HWND window, const std::filesystem::path&, bool banner_only)
    : window_(window), banner_only_(banner_only) {}
LoadingScreen::~LoadingScreen() = default;

void LoadingScreen::show(float fraction, const char* status) {
    // Como mucho ~30 veces por segundo (cada llamada es un frame entero).
    const auto now = std::chrono::steady_clock::now();
    if (painted_ && now - last_paint_ < std::chrono::milliseconds(33) && fraction < 1.0f) return;
    last_paint_ = now;
    painted_ = true;
    if (painter()) painter()(fraction, status != nullptr ? status : "");
}

void LoadingScreen::paint(float, const std::wstring&) {}

std::filesystem::path localDataFolder(const std::filesystem::path& sub) {
    const std::filesystem::path folder = android::dataRoot() / sub;
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    return folder;
}

void installCrashHandler(const std::string& app_name) {
    g_app_name = app_name.empty() ? std::string("Cramion") : app_name;
    for (char& c : g_app_name) {
        if (c == '/' || c == '\\' || c == '\0') c = '_';
    }
    g_crash_folder = localDataFolder("Crashes");
    std::snprintf(g_crash_dir, sizeof(g_crash_dir), "%s", g_crash_folder.c_str());
    std::snprintf(g_app_name_c, sizeof(g_app_name_c), "%s", g_app_name.c_str());
    // La primera vuelta de _Unwind_Backtrace prepara lo suyo (con memoria):
    // ahora, no en la senal.
    {
        Backtrace warm;
        _Unwind_Backtrace(unwindFrame, &warm);
    }
    // Pila aparte para el manejador en este hilo (el del juego): un
    // desbordamiento de pila tambien llega.
    static std::vector<char> alternate(256 * 1024);
    stack_t stack{};
    stack.ss_sp = alternate.data();
    stack.ss_size = alternate.size();
    sigaltstack(&stack, nullptr);
    std::set_terminate(&onTerminate);
    struct sigaction action {};
    action.sa_sigaction = onSignal;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    for (int signal : {SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL}) sigaction(signal, &action, &g_previous[signal]);
    setCrashContext("Aplicacion", g_app_name);
    setCrashContext("Plataforma", "android");
}

void crashBreadcrumb(const std::string& line) {
    std::lock_guard<std::mutex> lock(g_crumb_mutex);
    g_crumbs[g_crumb_next] = line.size() > 400 ? line.substr(0, 400) + "..." : line;
    g_crumb_next = (g_crumb_next + 1) % kBreadcrumbs;
    g_crumb_count = std::min(g_crumb_count + 1, kBreadcrumbs);
}

void setCrashContext(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lock(g_context_mutex);
    for (auto& [k, v] : g_context) {
        if (k == key) {
            v = value;
            return;
        }
    }
    g_context.emplace_back(key, value);
}

void setCrashLogFile(const std::filesystem::path& file) {
    std::snprintf(g_log_path, sizeof(g_log_path), "%s", file.c_str());  // lo lee la senal
}

std::filesystem::path pendingCrashReport(const std::string& app_name) {
    const std::string name = app_name.empty() ? g_app_name : app_name;
    const std::filesystem::path marker = localDataFolder("Crashes") / (name + ".pending");
    std::ifstream in(marker);
    if (!in) return {};
    std::string line;
    std::getline(in, line);
    in.close();
    std::error_code ec;
    std::filesystem::remove(marker, ec);
    const std::filesystem::path report(line);
    return std::filesystem::exists(report, ec) ? report : std::filesystem::path{};
}

std::string readCrashReport(const std::filesystem::path& report) {
    std::ifstream in(report, std::ios::binary);
    if (!in) return {};
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    const std::streamoff max = 64 * 1024;
    const std::streamoff from = size > max ? size - max : 0;
    in.seekg(from);
    std::string out(static_cast<std::size_t>(size - from), '\0');
    in.read(out.data(), static_cast<std::streamsize>(out.size()));
    return out;
}

void testCrash(int kind) {
    switch (kind) {
        case 1: std::abort();
        case 2: std::terminate();
        default: {
            volatile int* p = nullptr;
            *p = 42;  // acceso no valido
            break;
        }
    }
}

}  // namespace cramion::editor
