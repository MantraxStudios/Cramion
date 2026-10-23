#include "SystemUtil.h"

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
#endif

namespace cramion::editor::sys {

namespace {

#if defined(_WIN32)
std::wstring widen(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
    return out;
}

std::wstring quote(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos) return arg;
    std::wstring out = L"\"";
    for (const wchar_t c : arg) {
        if (c == L'"') out += L'\\';
        out += c;
    }
    return out + L"\"";
}
#endif

}  // namespace

std::filesystem::path executablePath() {
#if defined(_WIN32)
    wchar_t buffer[MAX_PATH * 4] = {};
    GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    return std::filesystem::path(buffer);
#else
    std::error_code ec;
    return std::filesystem::read_symlink("/proc/self/exe", ec);
#endif
}

std::filesystem::path executableDir() { return executablePath().parent_path(); }

bool openWithSystem(const std::string& path_or_url) {
#if defined(_WIN32)
    return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", widen(path_or_url).c_str(), nullptr, nullptr,
                                                   SW_SHOWNORMAL)) > 32;
#else
    return spawnDetached("xdg-open", {path_or_url}) != 0;
#endif
}

bool openWithSystem(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return openWithSystem(std::string(text.begin(), text.end()));
}

bool revealInFileManager(const std::filesystem::path& path) {
#if defined(_WIN32)
    const std::wstring args = L"/select," + quote(path.wstring());
    return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr,
                                                   SW_SHOWNORMAL)) > 32;
#else
    // FileManager1 (Nautilus, Dolphin, Nemo...) selecciona el archivo; si no
    // responde, se abre la carpeta.
    std::error_code ec;
    if (std::filesystem::is_regular_file(path, ec) && !findInPath("dbus-send").empty()) {
        const std::u8string text = std::filesystem::absolute(path, ec).u8string();
        std::string uri = "file://";
        for (const char8_t c : text) {
            const auto u = static_cast<unsigned char>(c);
            if (std::isalnum(u) || u == '/' || u == '-' || u == '_' || u == '.' || u == '~') {
                uri += static_cast<char>(u);
            } else {
                char hex[4];
                std::snprintf(hex, sizeof(hex), "%%%02X", u);
                uri += hex;
            }
        }
        std::string out;
        if (runCapture({"dbus-send", "--session", "--print-reply", "--dest=org.freedesktop.FileManager1",
                        "/org/freedesktop/FileManager1", "org.freedesktop.FileManager1.ShowItems",
                        "array:string:" + uri, "string:"},
                       out) == 0) {
            return true;
        }
    }
    return openWithSystem(std::filesystem::is_directory(path, ec) ? path : path.parent_path());
#endif
}

unsigned long spawnDetached(const std::filesystem::path& exe, const std::vector<std::string>& args,
                            const std::filesystem::path& working_dir) {
#if defined(_WIN32)
    std::wstring command = quote(exe.wstring());
    for (const std::string& arg : args) command += L" " + quote(widen(arg));
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const std::wstring dir = working_dir.wstring();
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS, nullptr,
                        dir.empty() ? nullptr : dir.c_str(), &startup, &process)) {
        return 0;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return process.dwProcessId;
#else
    // Doble fork: el nieto no queda como zombi del editor.
    int pid_pipe[2];
    if (pipe2(pid_pipe, O_CLOEXEC) != 0) return 0;
    const pid_t child = fork();
    if (child < 0) {
        close(pid_pipe[0]);
        close(pid_pipe[1]);
        return 0;
    }
    if (child == 0) {
        setsid();
        const pid_t grandchild = fork();
        if (grandchild == 0) {
            if (!working_dir.empty() && chdir(working_dir.c_str()) != 0) _exit(127);
            const int null_fd = open("/dev/null", O_RDWR);
            if (null_fd >= 0) {
                dup2(null_fd, 0);
                dup2(null_fd, 1);
                dup2(null_fd, 2);
            }
            std::vector<std::string> storage;
            storage.push_back(exe.string());
            storage.insert(storage.end(), args.begin(), args.end());
            std::vector<char*> argv;
            for (std::string& s : storage) argv.push_back(s.data());
            argv.push_back(nullptr);
            execvp(argv[0], argv.data());
            _exit(127);
        }
        const auto value = static_cast<long>(grandchild);
        (void)!write(pid_pipe[1], &value, sizeof(value));
        _exit(0);
    }
    close(pid_pipe[1]);
    long value = 0;
    const bool got = read(pid_pipe[0], &value, sizeof(value)) == static_cast<ssize_t>(sizeof(value));
    close(pid_pipe[0]);
    int status = 0;
    waitpid(child, &status, 0);
    return got && value > 0 ? static_cast<unsigned long>(value) : 0;
#endif
}

int runCapture(const std::vector<std::string>& argv_in, std::string& output, bool merge_stderr,
               const std::filesystem::path& working_dir) {
    output.clear();
    if (argv_in.empty()) return -1;
#if defined(_WIN32)
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE read_end = nullptr;
    HANDLE write_end = nullptr;
    if (!CreatePipe(&read_end, &write_end, &sa, 0)) return -1;
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
    std::wstring command;
    for (const std::string& arg : argv_in) command += (command.empty() ? L"" : L" ") + quote(widen(arg));
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = write_end;
    startup.hStdError = merge_stderr ? write_end : GetStdHandle(STD_ERROR_HANDLE);
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION process{};
    const std::wstring dir = working_dir.wstring();
    const BOOL ok = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                   dir.empty() ? nullptr : dir.c_str(), &startup, &process);
    CloseHandle(write_end);
    if (!ok) {
        CloseHandle(read_end);
        return -1;
    }
    char buffer[4096];
    DWORD read = 0;
    while (ReadFile(read_end, buffer, sizeof(buffer), &read, nullptr) && read > 0) output.append(buffer, read);
    CloseHandle(read_end);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return static_cast<int>(code);
#else
    int out_pipe[2];
    if (pipe2(out_pipe, O_CLOEXEC) != 0) return -1;
    const pid_t pid = fork();
    if (pid < 0) {
        close(out_pipe[0]);
        close(out_pipe[1]);
        return -1;
    }
    if (pid == 0) {
        if (!working_dir.empty() && chdir(working_dir.c_str()) != 0) _exit(127);
        dup2(out_pipe[1], 1);
        if (merge_stderr) dup2(out_pipe[1], 2);
        std::vector<std::string> storage = argv_in;
        std::vector<char*> argv;
        for (std::string& s : storage) argv.push_back(s.data());
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }
    close(out_pipe[1]);
    char buffer[4096];
    ssize_t n = 0;
    while ((n = read(out_pipe[0], buffer, sizeof(buffer))) > 0 || (n < 0 && errno == EINTR)) {
        if (n > 0) output.append(buffer, static_cast<std::size_t>(n));
    }
    close(out_pipe[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

std::filesystem::path findInPath(const std::string& program) {
#if defined(_WIN32)
    wchar_t buffer[MAX_PATH] = {};
    if (SearchPathW(nullptr, widen(program).c_str(), L".exe", MAX_PATH, buffer, nullptr) > 0) return buffer;
    return {};
#else
    const std::string path = env("PATH");
    std::size_t begin = 0;
    while (begin <= path.size()) {
        std::size_t end = path.find(':', begin);
        if (end == std::string::npos) end = path.size();
        const std::filesystem::path candidate = std::filesystem::path(path.substr(begin, end - begin)) / program;
        if (end > begin && access(candidate.c_str(), X_OK) == 0) return candidate;
        begin = end + 1;
    }
    return {};
#endif
}

std::string env(const char* name) {
#if defined(_WIN32)
    const DWORD n = GetEnvironmentVariableW(widen(name).c_str(), nullptr, 0);
    if (n == 0) return {};
    std::wstring value(n, L'\0');
    GetEnvironmentVariableW(widen(name).c_str(), value.data(), n);
    value.resize(n - 1);
    const std::filesystem::path p(value);
    const std::u8string text = p.u8string();
    return std::string(text.begin(), text.end());
#else
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string{};
#endif
}

}  // namespace cramion::editor::sys
