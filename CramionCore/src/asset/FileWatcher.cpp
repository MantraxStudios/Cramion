#include "CramionCore/asset/FileWatcher.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <unistd.h>

#include <cerrno>
#endif

namespace cramion::assets {

namespace {

using Clock = std::chrono::steady_clock;
using Kind = FileWatcher::Kind;

// Dos avisos seguidos de la misma ruta en uno: lo que importa es como queda
// al final respecto a como estaba antes de la rafaga.
Kind merge(Kind before, Kind now) {
    if (now == Kind::Removed) return Kind::Removed;
    if (now == Kind::Added) return before == Kind::Added ? Kind::Added : Kind::Modified;  // borrado y vuelto a crear
    return before == Kind::Added ? Kind::Added : Kind::Modified;
}

}  // namespace

struct FileWatcher::Impl {
    std::filesystem::path root;
    std::chrono::milliseconds debounce{300};
    std::chrono::milliseconds max_delay{2000};
    std::thread thread;
    std::atomic<bool> running{false};

    // Del hilo: lo que se esta juntando (por ruta).
    struct Pending {
        std::filesystem::path path;
        Kind kind = Kind::Modified;
        Clock::time_point first{};
        Clock::time_point last{};
    };
    std::unordered_map<std::filesystem::path::string_type, Pending> pending;
    bool pending_overflow = false;

    // Compartido con poll() (con el mutex).
    std::mutex mutex;
    Batch ready;

#if defined(_WIN32)
    HANDLE stop_event = nullptr;
#elif defined(__linux__)
    int wake_fd = -1;
#endif

    void note(const std::filesystem::path& path, Kind kind, Clock::time_point now) {
        const auto [it, inserted] = pending.try_emplace(path.native(), Pending{path, kind, now, now});
        if (!inserted) {
            it->second.kind = merge(it->second.kind, kind);
            it->second.last = now;
        }
    }

    // Pasa a `ready` lo que lleva `debounce` quieto (o `max_delay` esperando).
    // Devuelve cuanto falta para el siguiente (o -1 si no queda nada).
    long long flush(Clock::time_point now) {
        if (pending_overflow) {
            pending.clear();
            pending_overflow = false;
            const std::lock_guard lock(mutex);
            ready.overflow = true;
            ready.changes.clear();  // el recorrido completo ya lo cubre
            return -1;
        }
        long long next = -1;
        std::vector<Change> out;
        for (auto it = pending.begin(); it != pending.end();) {
            const Pending& p = it->second;
            const auto quiet = std::chrono::duration_cast<std::chrono::milliseconds>(now - p.last);
            const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(now - p.first);
            if (quiet >= debounce || waited >= max_delay) {
                out.push_back(Change{p.path, p.kind});
                it = pending.erase(it);
                continue;
            }
            const long long left = std::min((debounce - quiet).count(), (max_delay - waited).count());
            next = next < 0 ? left : std::min(next, left);
            ++it;
        }
        if (!out.empty()) {
            const std::lock_guard lock(mutex);
            if (!ready.overflow) {
                ready.changes.insert(ready.changes.end(), std::make_move_iterator(out.begin()),
                                     std::make_move_iterator(out.end()));
            }
        }
        return next;
    }

    void overflow(const char* why) {
        std::cerr << "[Assets] Vigilancia de " << root.string() << ": " << why << " (se recorre todo)\n";
        pending_overflow = true;
    }

#if defined(_WIN32)
    HANDLE directory = INVALID_HANDLE_VALUE;

    bool open() {
        directory = CreateFileW(root.wstring().c_str(), FILE_LIST_DIRECTORY,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
        if (directory == INVALID_HANDLE_VALUE) return false;
        stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        return stop_event != nullptr;
    }

    void close() {
        if (directory != INVALID_HANDLE_VALUE) CloseHandle(directory);
        if (stop_event != nullptr) CloseHandle(stop_event);
        directory = INVALID_HANDLE_VALUE;
        stop_event = nullptr;
    }

    void run() {
        // 64 KB: el maximo que admite una carpeta compartida por red; en local
        // sobra (unos 1500 nombres por lectura). Alineado a DWORD.
        std::vector<DWORD> buffer(64 * 1024 / sizeof(DWORD));
        const DWORD filter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE |
                             FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION;
        HANDLE io_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        OVERLAPPED overlapped{};
        overlapped.hEvent = io_event;
        const auto issue = [&]() {
            ResetEvent(io_event);
            return ReadDirectoryChangesW(directory, buffer.data(), static_cast<DWORD>(buffer.size() * sizeof(DWORD)),
                                         TRUE, filter, nullptr, &overlapped, nullptr) != FALSE;
        };
        bool reading = io_event != nullptr && issue();
        if (!reading) overflow("no se pudo leer la carpeta");
        while (running.load(std::memory_order_acquire)) {
            const long long next = flush(Clock::now());
            HANDLE handles[2] = {stop_event, io_event};
            const DWORD wait = WaitForMultipleObjects(reading ? 2 : 1, handles, FALSE,
                                                      next < 0 ? INFINITE : static_cast<DWORD>(std::max(next, 1LL)));
            if (wait == WAIT_OBJECT_0) break;  // stop()
            if (wait != WAIT_OBJECT_0 + 1) continue;  // tiempo: toca entregar
            DWORD bytes = 0;
            if (!GetOverlappedResult(directory, &overlapped, &bytes, FALSE)) {
                // ERROR_NOTIFY_ENUM_DIR: el buffer se lleno. Otro error: la
                // carpeta ya no esta (borrada o movida); no se vuelve a pedir.
                const DWORD error = GetLastError();
                overflow(error == ERROR_NOTIFY_ENUM_DIR ? "demasiados cambios a la vez" : "la carpeta ya no responde");
                reading = error == ERROR_NOTIFY_ENUM_DIR && issue();
                continue;
            }
            const Clock::time_point now = Clock::now();
            if (bytes == 0) {
                overflow("demasiados cambios a la vez");
            } else {
                const auto* base = reinterpret_cast<const std::uint8_t*>(buffer.data());
                std::size_t offset = 0;
                while (true) {
                    const auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(base + offset);
                    const std::wstring name(info->FileName, info->FileNameLength / sizeof(WCHAR));
                    const std::filesystem::path path = root / name;
                    switch (info->Action) {
                        case FILE_ACTION_ADDED:
                        case FILE_ACTION_RENAMED_NEW_NAME: note(path, Kind::Added, now); break;
                        case FILE_ACTION_REMOVED:
                        case FILE_ACTION_RENAMED_OLD_NAME: note(path, Kind::Removed, now); break;
                        default: {
                            // Una carpeta "cambia" cuando cambia algo dentro, y eso
                            // ya llega con su nombre: recorrerla entera sobraria.
                            std::error_code error;
                            if (!std::filesystem::is_directory(path, error)) note(path, Kind::Modified, now);
                            break;
                        }
                    }
                    if (info->NextEntryOffset == 0) break;
                    offset += info->NextEntryOffset;
                }
            }
            reading = issue();
            if (!reading) overflow("no se pudo seguir leyendo la carpeta");
        }
        if (reading) {
            // La lectura en marcha escribe en `buffer`: se cancela y se espera
            // a que el sistema la suelte antes de liberarlo.
            CancelIoEx(directory, &overlapped);
            DWORD bytes = 0;
            GetOverlappedResult(directory, &overlapped, &bytes, TRUE);
        }
        if (io_event != nullptr) CloseHandle(io_event);
    }

    void wake() {
        if (stop_event != nullptr) SetEvent(stop_event);
    }

#elif defined(__linux__)
    int inotify_fd = -1;
    std::unordered_map<int, std::filesystem::path> watches;  // wd -> carpeta
    static constexpr std::uint32_t kMask = IN_CREATE | IN_DELETE | IN_MODIFY | IN_CLOSE_WRITE | IN_MOVED_FROM |
                                           IN_MOVED_TO | IN_DELETE_SELF | IN_MOVE_SELF;

    void addWatch(const std::filesystem::path& folder) {
        const int wd = inotify_add_watch(inotify_fd, folder.c_str(), kMask);
        if (wd >= 0) watches[wd] = folder;
    }

    // La carpeta y todas sus subcarpetas (inotify no es recursivo).
    void addTree(const std::filesystem::path& folder) {
        addWatch(folder);
        std::error_code error;
        for (std::filesystem::recursive_directory_iterator it(
                 folder, std::filesystem::directory_options::skip_permission_denied, error), end;
             !error && it != end; it.increment(error)) {
            if (it->is_directory(error)) addWatch(it->path());
        }
    }

    // Una carpeta que se fue (movida o borrada): fuera sus watches.
    void removeTree(const std::filesystem::path& folder) {
        const std::string prefix = folder.native() + "/";
        for (auto it = watches.begin(); it != watches.end();) {
            const std::string& p = it->second.native();
            if (p == folder.native() || p.compare(0, prefix.size(), prefix) == 0) {
                inotify_rm_watch(inotify_fd, it->first);
                it = watches.erase(it);
            } else {
                ++it;
            }
        }
    }

    bool open() {
        inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (inotify_fd < 0 || wake_fd < 0) return false;
        addTree(root);
        return !watches.empty();
    }

    void close() {
        if (inotify_fd >= 0) ::close(inotify_fd);
        if (wake_fd >= 0) ::close(wake_fd);
        inotify_fd = wake_fd = -1;
        watches.clear();
    }

    void run() {
        alignas(inotify_event) char buffer[64 * 1024];
        while (running.load(std::memory_order_acquire)) {
            const long long next = flush(Clock::now());
            pollfd fds[2] = {{inotify_fd, POLLIN, 0}, {wake_fd, POLLIN, 0}};
            const int timeout = next < 0 ? -1 : static_cast<int>(std::max(next, 1LL));
            const int ready_count = ::poll(fds, 2, timeout);
            if (ready_count < 0 && errno != EINTR) break;
            if (fds[1].revents & POLLIN) break;  // stop()
            if (!(fds[0].revents & POLLIN)) continue;
            const Clock::time_point now = Clock::now();
            while (true) {
                const ssize_t length = ::read(inotify_fd, buffer, sizeof(buffer));
                if (length <= 0) break;  // EAGAIN: ya no queda nada
                for (ssize_t offset = 0; offset < length;) {
                    const auto* event = reinterpret_cast<const inotify_event*>(buffer + offset);
                    offset += static_cast<ssize_t>(sizeof(inotify_event) + event->len);
                    if (event->mask & IN_Q_OVERFLOW) {
                        overflow("demasiados cambios a la vez");
                        continue;
                    }
                    const auto watch = watches.find(event->wd);
                    if (watch == watches.end()) continue;
                    if (event->mask & IN_IGNORED) {
                        watches.erase(watch);
                        continue;
                    }
                    if (event->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) {
                        if (watch->second == root) overflow("la carpeta raiz se borro o se movio");
                        continue;  // su padre ya aviso con el nombre
                    }
                    const std::filesystem::path path =
                        event->len > 0 ? watch->second / event->name : watch->second;
                    const bool folder = (event->mask & IN_ISDIR) != 0;
                    if (event->mask & (IN_CREATE | IN_MOVED_TO)) {
                        // Carpeta nueva: se vigila (y quien recibe el aviso la
                        // recorre entera: lo creado antes del watch tambien).
                        if (folder) addTree(path);
                        note(path, Kind::Added, now);
                    } else if (event->mask & (IN_DELETE | IN_MOVED_FROM)) {
                        if (folder) removeTree(path);
                        note(path, Kind::Removed, now);
                    } else if (!folder) {
                        note(path, Kind::Modified, now);
                    }
                }
            }
        }
    }

    void wake() {
        if (wake_fd >= 0) {
            const std::uint64_t one = 1;
            [[maybe_unused]] const ssize_t written = ::write(wake_fd, &one, sizeof(one));
        }
    }

#else
    bool open() { return false; }
    void close() {}
    void run() {}
    void wake() {}
#endif
};

FileWatcher::FileWatcher() : impl_(std::make_unique<Impl>()) {}

FileWatcher::~FileWatcher() { stop(); }

bool FileWatcher::start(const std::filesystem::path& root, std::chrono::milliseconds debounce,
                        std::chrono::milliseconds max_delay) {
    stop();
    impl_->root = root;
    impl_->debounce = std::max(debounce, std::chrono::milliseconds(10));
    impl_->max_delay = std::max(max_delay, impl_->debounce);
    impl_->pending.clear();
    impl_->pending_overflow = false;
    {
        const std::lock_guard lock(impl_->mutex);
        impl_->ready = Batch{};
    }
    if (!impl_->open()) {
        impl_->close();
        return false;
    }
    impl_->running.store(true, std::memory_order_release);
    try {
        impl_->thread = std::thread([impl = impl_.get()]() {
            try {
                impl->run();
            } catch (const std::exception& e) {
                std::cerr << "[Assets] La vigilancia de carpetas se paro: " << e.what() << "\n";
            }
            impl->running.store(false, std::memory_order_release);
        });
    } catch (const std::exception&) {
        impl_->running.store(false, std::memory_order_release);
        impl_->close();
        return false;
    }
    return true;
}

void FileWatcher::stop() {
    if (impl_->thread.joinable()) {
        impl_->running.store(false, std::memory_order_release);
        impl_->wake();
        impl_->thread.join();
    }
    impl_->running.store(false, std::memory_order_release);
    impl_->close();
}

bool FileWatcher::running() const { return impl_->running.load(std::memory_order_acquire); }

const std::filesystem::path& FileWatcher::root() const { return impl_->root; }

bool FileWatcher::poll(Batch& out) {
    out = Batch{};
    const std::lock_guard lock(impl_->mutex);
    if (impl_->ready.empty()) return false;
    std::swap(out, impl_->ready);
    return true;
}

}  // namespace cramion::assets
