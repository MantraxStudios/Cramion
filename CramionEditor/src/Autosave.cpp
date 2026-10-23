// Autoguardado de la escena (ver Autosave.h).

#include "Autosave.h"

#include "Dialogs.h"
#include "LoadingScreen.h"  // localDataFolder

#if defined(_WIN32)
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <exception>
#include <fstream>
#include <iostream>
#include <mutex>

namespace cramion::editor {

namespace {

#if defined(_WIN32)
constexpr const wchar_t* kTempSuffix = L".tmp";
constexpr const wchar_t* kCrashTempSuffix = L".crash.tmp";  // otro temporal: el hilo puede estar escribiendo
constexpr std::size_t kPathChars = 2048;

std::filesystem::path preferencesFile() { return localDataFolder("Settings") / "EditorSafety.ini"; }

// Escritura atomica con Win32 puro (vale tambien dentro de un manejador de
// crash: sin excepciones ni memoria nueva). Temporal, FlushFileBuffers y
// MoveFileEx con reemplazo: o queda el archivo anterior o el nuevo entero.
bool writeAtomic(const wchar_t* file, const wchar_t* suffix, const char* data, std::size_t size) {
    wchar_t temporary[kPathChars + 16];
    if (std::swprintf(temporary, sizeof(temporary) / sizeof(temporary[0]), L"%ls%ls", file, suffix) < 0) return false;
    const HANDLE handle = CreateFileW(temporary, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    bool ok = true;
    std::size_t written = 0;
    while (ok && written < size) {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(size - written, 16u << 20));
        DWORD done = 0;
        ok = WriteFile(handle, data + written, chunk, &done, nullptr) != 0 && done == chunk;
        written += done;
    }
    ok = ok && FlushFileBuffers(handle) != 0;
    CloseHandle(handle);
    if (ok) ok = MoveFileExW(temporary, file, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    if (!ok) DeleteFileW(temporary);
    return ok;
}
#else
constexpr const char* kTempSuffix = ".tmp";
constexpr const char* kCrashTempSuffix = ".crash.tmp";  // otro temporal: el hilo puede estar escribiendo
constexpr std::size_t kPathChars = 4096;

std::filesystem::path preferencesFile() { return localDataFolder("Settings") / "EditorSafety.ini"; }

// Escritura atomica con POSIX puro (open/write/fsync/rename: vale tambien
// dentro de un manejador de senal). O queda el archivo anterior o el nuevo entero.
bool writeAtomic(const char* file, const char* suffix, const char* data, std::size_t size) {
    char temporary[kPathChars + 16];
    const std::size_t file_len = std::strlen(file);
    const std::size_t suffix_len = std::strlen(suffix);
    if (file_len + suffix_len + 1 > sizeof(temporary)) return false;
    std::memcpy(temporary, file, file_len);
    std::memcpy(temporary + file_len, suffix, suffix_len + 1);
    const int fd = ::open(temporary, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return false;
    bool ok = true;
    std::size_t written = 0;
    while (ok && written < size) {
        const ssize_t done = ::write(fd, data + written, std::min<std::size_t>(size - written, 16u << 20));
        if (done < 0 && errno == EINTR) continue;
        ok = done > 0;
        if (ok) written += static_cast<std::size_t>(done);
    }
    ok = ok && ::fsync(fd) == 0;
    ::close(fd);
    if (ok) ok = ::rename(temporary, file) == 0;
    if (!ok) ::unlink(temporary);
    return ok;
}
#endif

// --- Guardado de emergencia ---------------------------------------------------
// Lo arma tick() en el hilo principal: la instantanea (current_state_ del
// editor, que vive lo que el editor) y su archivo. Al cambiar de archivo se
// desarma mientras se copia la ruta: un crash justo entonces no guarda.

std::atomic<const std::string*> g_state{nullptr};
std::atomic<bool> g_armed{false};
std::atomic<bool> g_emergency_done{false};
#if !defined(_WIN32)
char g_file[kPathChars] = {};

std::terminate_handler g_prev_terminate = nullptr;
constexpr int kCrashSignals[] = {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT};
struct sigaction g_prev_actions[64] = {};

// La ruta solo cambia con la escena (no cada frame).
void setEmergencyFile(const std::filesystem::path& file) {
    g_armed = false;
    const std::string native = file.native();
    if (native.size() + 16 >= kPathChars) {  // ruta demasiado larga: sin emergencia
        g_file[0] = '\0';
        return;
    }
    std::memcpy(g_file, native.c_str(), native.size() + 1);
}

void arm(const std::string* state) {
    g_state = state;
    g_armed = g_file[0] != '\0';
}

void onTerminate() {
    autosaveEmergency();
    if (g_prev_terminate != nullptr) g_prev_terminate();
    std::abort();
}

// Delante del manejador de crash (LoadingScreen.cpp): primero se guarda la
// escena y luego se pasa la senal al anterior (el informe), o al del sistema.
void onCrashSignal(int signal, siginfo_t* info, void* context) {
    autosaveEmergency();
    const struct sigaction& previous = g_prev_actions[signal];
    if ((previous.sa_flags & SA_SIGINFO) != 0 && previous.sa_sigaction != nullptr) {
        previous.sa_sigaction(signal, info, context);
        return;
    }
    if (previous.sa_handler != SIG_DFL && previous.sa_handler != SIG_IGN && previous.sa_handler != nullptr) {
        previous.sa_handler(signal);
        return;
    }
    struct sigaction fallback {};
    fallback.sa_handler = SIG_DFL;
    sigemptyset(&fallback.sa_mask);
    sigaction(signal, &fallback, nullptr);
    raise(signal);
}

void installEmergencyHooks() {
    static std::once_flag once;
    std::call_once(once, [] {
        g_prev_terminate = std::set_terminate(&onTerminate);
        struct sigaction action {};
        action.sa_sigaction = &onCrashSignal;
        action.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigemptyset(&action.sa_mask);
        for (const int signal : kCrashSignals) sigaction(signal, &action, &g_prev_actions[signal]);
    });
}
#else
wchar_t g_file[kPathChars] = {};

LPTOP_LEVEL_EXCEPTION_FILTER g_prev_filter = nullptr;
std::terminate_handler g_prev_terminate = nullptr;
using SignalHandler = void (*)(int);
SignalHandler g_prev_abort = nullptr;

// La ruta solo cambia con la escena (no cada frame).
void setEmergencyFile(const std::filesystem::path& file) {
    g_armed = false;
    const std::wstring wide = file.wstring();
    if (wide.size() + 16 >= kPathChars) {  // ruta demasiado larga: sin emergencia
        g_file[0] = L'\0';
        return;
    }
    std::wmemcpy(g_file, wide.c_str(), wide.size() + 1);
}

void arm(const std::string* state) {
    g_state = state;
    g_armed = g_file[0] != L'\0';
}

LONG WINAPI onUnhandledException(EXCEPTION_POINTERS* info) {
    autosaveEmergency();
    return g_prev_filter != nullptr ? g_prev_filter(info) : EXCEPTION_CONTINUE_SEARCH;
}

void onTerminate() {
    autosaveEmergency();
    if (g_prev_terminate != nullptr) g_prev_terminate();
    std::abort();
}

void onAbort(int signal) {
    autosaveEmergency();
    if (g_prev_abort != nullptr && g_prev_abort != SIG_DFL && g_prev_abort != SIG_IGN) g_prev_abort(signal);
}

// Delante del manejador de crash (LoadingScreen.cpp), que sigue igual
// despues: primero se guarda la escena y luego el minidump y el mensaje.
// set_terminate es por hilo en el CRT de Microsoft: en los hilos de trabajo
// terminate acaba en abort(), que entra por SIGABRT (global).
void installEmergencyHooks() {
    static std::once_flag once;
    std::call_once(once, [] {
        g_prev_filter = SetUnhandledExceptionFilter(&onUnhandledException);
        g_prev_terminate = std::set_terminate(&onTerminate);
        const SignalHandler previous = std::signal(SIGABRT, &onAbort);
        g_prev_abort = previous == SIG_ERR ? nullptr : previous;
    });
}
#endif

}  // namespace

bool autosaveEmergency() {
    if (!g_armed.load() || g_emergency_done.exchange(true)) return false;
    const std::string* state = g_state.load();
    if (state == nullptr) return false;
    // Lectura de mejor esfuerzo: si el hilo principal cayo cambiando la
    // instantanea, el texto puede estar vacio o a medias (al recuperarlo, el
    // lector de escenas lo rechaza y la escena del disco sigue).
    const char* data = state->data();
    const std::size_t size = state->size();
    if (data == nullptr || size == 0 || size > (std::size_t{1} << 32)) return false;
    return writeAtomic(g_file, kCrashTempSuffix, data, size);
}

// --- Preferencias -------------------------------------------------------------

SafetyPreferences loadSafetyPreferences() {
    SafetyPreferences prefs;
    std::ifstream in(preferencesFile());
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("autosave_minutes=", 0) == 0) prefs.autosave_minutes = std::clamp(std::atoi(line.c_str() + 17), 0, 120);
        if (line.rfind("mcp_require_token=", 0) == 0) prefs.mcp_require_token = std::atoi(line.c_str() + 18) != 0;
    }
    return prefs;
}

void saveSafetyPreferences(const SafetyPreferences& prefs) {
    std::ofstream out(preferencesFile(), std::ios::trunc);
    out << "autosave_minutes=" << prefs.autosave_minutes << "\n";
    out << "mcp_require_token=" << (prefs.mcp_require_token ? 1 : 0) << "\n";
}

// --- SceneAutosave ------------------------------------------------------------

SceneAutosave::SceneAutosave() {
    installEmergencyHooks();
    worker_ = std::thread([this] { workerLoop(); });
}

SceneAutosave::~SceneAutosave() {
    disarm();
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
}

std::filesystem::path SceneAutosave::fileFor(const std::filesystem::path& library, const std::filesystem::path& assets,
                                             const std::filesystem::path& scene) {
#if defined(_WIN32)
    std::wstring name;
    if (scene.empty()) {
        name = L"Sin guardar.crscene";
    } else {
        // Ruta dentro de Assets (dos escenas con el mismo nombre en carpetas
        // distintas no se pisan); fuera de Assets, solo el nombre.
        std::filesystem::path relative = scene.lexically_normal().lexically_relative(assets.lexically_normal());
        if (relative.empty() || *relative.begin() == "..") relative = scene.filename();
        name = relative.wstring();
        for (wchar_t& c : name) {
            if (c == L'/' || c == L'\\' || c == L':') c = L'~';
        }
    }
    return library / "Autosave" / (name + L".autosave");
#else
    // Linux: la ruta nativa es UTF-8 (wstring dependeria de la locale).
    std::string name;
    if (scene.empty()) {
        name = "Sin guardar.crscene";
    } else {
        std::filesystem::path relative = scene.lexically_normal().lexically_relative(assets.lexically_normal());
        if (relative.empty() || *relative.begin() == "..") relative = scene.filename();
        name = relative.native();
        for (char& c : name) {
            if (c == '/' || c == '\\' || c == ':') c = '~';
        }
    }
    return library / "Autosave" / (name + ".autosave");
#endif
}

bool SceneAutosave::newerThanScene(const std::filesystem::path& autosave, const std::filesystem::path& scene) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(autosave, ec)) return false;
    const auto saved = std::filesystem::last_write_time(autosave, ec);
    if (ec) return false;
    if (scene.empty()) return true;
    const auto original = std::filesystem::last_write_time(scene, ec);
    if (ec) return true;  // la escena ya no esta: el autoguardado es lo que queda
    // Margen de 2 s: guardar la escena y autoguardar en el mismo segundo.
    return saved > original + std::chrono::seconds(2);
}

void SceneAutosave::tick(const Frame& frame) {
    if (frame.file == nullptr || frame.file->empty()) {
        // Otra pestana (prefab, Blueprint) o cargando: current_state_ no es
        // el de la escena. El plazo sigue; se escribe al volver.
        g_armed = false;
        return;
    }
    const std::filesystem::path& file = *frame.file;
    if (file != target_) {
        target_ = file;
        written_data_ = nullptr;
        written_size_ = 0;
        due_set_ = false;
        setEmergencyFile(target_);
        std::error_code ec;
        std::filesystem::create_directories(target_.parent_path(), ec);  // para la emergencia
    }
    const bool active =
        frame.minutes > 0 && frame.dirty && frame.state != nullptr && !frame.state->empty() && file != protected_;
    if (!active) {
        g_armed = false;
        due_set_ = false;  // el plazo cuenta desde el primer cambio
        return;
    }
    arm(frame.state);
    const Clock::time_point now = Clock::now();
    if (frame.minutes != minutes_) {
        minutes_ = frame.minutes;
        due_set_ = false;
    }
    if (!due_set_) {
        due_ = now + std::chrono::minutes(minutes_);
        due_set_ = true;
        return;
    }
    if (now < due_) return;
    due_ = now + std::chrono::minutes(minutes_);
    if (frame.state->data() == written_data_ && frame.state->size() == written_size_) return;  // nada nuevo
    written_data_ = frame.state->data();
    written_size_ = frame.state->size();
    // La unica parte en el frame: copiar el texto (memcpy; ~1 ms por cada
    // 5-10 MB de escena). Serializar no hace falta: es la instantanea de deshacer.
    push(Job{false, file, *frame.state});
    last_written_ = file;
}

void SceneAutosave::discard(const std::filesystem::path& file) {
    if (file.empty()) return;
    if (file == target_) {
        written_data_ = nullptr;
        written_size_ = 0;
        due_set_ = false;
    }
    if (file == protected_) protected_.clear();
    if (file == last_written_) last_written_.clear();
    push(Job{true, file, {}});
}

void SceneAutosave::writeNow() {
    // Lo que el hilo este escribiendo, primero (si no, su rename podria
    // dejar una version mas vieja encima de esta).
    {
        std::unique_lock lock(mutex_);
        jobs_.clear();
        idle_.wait_for(lock, std::chrono::seconds(5), [this] { return !busy_; });
    }
    autosaveEmergency();
}

void SceneAutosave::disarm() { g_armed = false; }

void SceneAutosave::push(Job job) {
    {
        std::lock_guard lock(mutex_);
        // Una escritura del mismo archivo que aun no empezo se sustituye.
        for (Job& queued : jobs_) {
            if (queued.file == job.file && !queued.remove && !job.remove) {
                queued.data = std::move(job.data);
                return;
            }
        }
        jobs_.push_back(std::move(job));
    }
    wake_.notify_one();
}

void SceneAutosave::workerLoop() {
    for (;;) {
        Job job;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
            if (jobs_.empty()) return;  // stop_ y nada pendiente
            job = std::move(jobs_.front());
            jobs_.pop_front();
            busy_ = true;
        }
        try {
            if (job.remove) {
                std::error_code ec;
                std::filesystem::remove(job.file, ec);
            } else {
                const auto start = Clock::now();
#if defined(_WIN32)
                const std::wstring wide = job.file.wstring();
#else
                const std::string wide = job.file.native();
#endif
                std::error_code ec;
                std::filesystem::create_directories(job.file.parent_path(), ec);
                if (wide.size() + 16 < kPathChars && writeAtomic(wide.c_str(), kTempSuffix, job.data.data(), job.data.size())) {
                    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
                    std::cout << "[Autoguardado] " << dialogs::utf8(job.file.filename()) << " ("
                              << (job.data.size() + 512 * 1024) / (1024 * 1024) << " MB, " << ms << " ms)\n";
                } else {
                    std::cerr << "[Autoguardado] No se pudo escribir " << dialogs::utf8(job.file) << " (error "
#if defined(_WIN32)
                              << GetLastError()
#else
                              << errno
#endif
                              << ")\n";
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "[Autoguardado] " << e.what() << "\n";
        }
        {
            std::lock_guard lock(mutex_);
            busy_ = false;
        }
        idle_.notify_all();
    }
}

}  // namespace cramion::editor
