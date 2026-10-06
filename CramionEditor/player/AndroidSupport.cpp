// Android: assets del APK, carpeta de datos, logs a logcat, pantalla de
// carga y crashes (lo que en Windows hace LoadingScreen.cpp).

#include "AndroidSupport.h"

#include "LoadingScreen.h"

#include <android/asset_manager.h>
#include <android/log.h>
#include <dlfcn.h>
#include <signal.h>
#include <unistd.h>
#include <unwind.h>

#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <mutex>
#include <streambuf>

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

struct Backtrace {
    void* frames[48];
    int count = 0;
};

_Unwind_Reason_Code unwindFrame(_Unwind_Context* context, void* arg) {
    Backtrace* trace = static_cast<Backtrace*>(arg);
    const uintptr_t pc = _Unwind_GetIP(context);
    if (pc != 0 && trace->count < 48) trace->frames[trace->count++] = reinterpret_cast<void*>(pc);
    return trace->count < 48 ? _URC_NO_REASON : _URC_END_OF_STACK;
}

void onSignal(int signal, siginfo_t* info, void* context) {
    // Informe: senal, direccion y la pila (modulo + desplazamiento; con
    // llvm-symbolizer y la libmain.so con simbolos da las lineas).
    Backtrace trace;
    _Unwind_Backtrace(unwindFrame, &trace);
    char report[8192];
    int used = std::snprintf(report, sizeof(report), "%s se cerro por un error: senal %d (%s) en %p\nPila:\n",
                             g_app_name.c_str(), signal, strsignal(signal), info != nullptr ? info->si_addr : nullptr);
    for (int i = 0; i < trace.count && used < static_cast<int>(sizeof(report)) - 200; ++i) {
        Dl_info dl{};
        const char* module = "?";
        uintptr_t offset = reinterpret_cast<uintptr_t>(trace.frames[i]);
        if (dladdr(trace.frames[i], &dl) != 0 && dl.dli_fname != nullptr) {
            module = std::strrchr(dl.dli_fname, '/') != nullptr ? std::strrchr(dl.dli_fname, '/') + 1 : dl.dli_fname;
            offset -= reinterpret_cast<uintptr_t>(dl.dli_fbase);
        }
        used += std::snprintf(report + used, sizeof(report) - static_cast<std::size_t>(used), "  #%d %s + 0x%lx\n", i,
                              module, static_cast<unsigned long>(offset));
    }
    __android_log_write(ANDROID_LOG_FATAL, "Cramion", report);
    if (!g_crash_folder.empty()) {
        const std::string file = (g_crash_folder / (g_app_name + "_" + std::to_string(std::time(nullptr)) + ".txt")).string();
        if (FILE* f = std::fopen(file.c_str(), "w")) {
            std::fputs(report, f);
            std::fclose(f);
        }
    }
    // El manejador del sistema (tombstone) despues del nuestro.
    sigaction(signal, &g_previous[signal], nullptr);
    raise(signal);
    (void)context;
}

}  // namespace

void setLoadingPainter(std::function<void(float fraction, const char* status)> p) { painter() = std::move(p); }

LoadingScreen::LoadingScreen(HWND window, const std::filesystem::path&) : window_(window) {}
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
    g_crash_folder = localDataFolder("Crashes");
    struct sigaction action {};
    action.sa_sigaction = onSignal;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    for (int signal : {SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL}) sigaction(signal, &action, &g_previous[signal]);
}

// En Android el informe es el de onSignal (sin migas ni contexto por ahora).
void crashBreadcrumb(const std::string&) {}
void setCrashContext(const std::string&, const std::string&) {}
void setCrashLogFile(const std::filesystem::path&) {}
std::filesystem::path pendingCrashReport(const std::string&) { return {}; }
std::string readCrashReport(const std::filesystem::path&) { return {}; }
void testCrash(int) {
    volatile int* p = nullptr;
    *p = 42;
}

}  // namespace cramion::editor
