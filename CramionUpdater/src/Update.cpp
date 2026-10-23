// Actualizaciones de Cramion (ver Update.h).

#include <CramionUpdater/Update.h>

#include "Inflate.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <winhttp.h>
#else
// Linux: HTTP(S) con el programa curl, SHA-256 propio, procesos con /proc.
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <cwctype>
#include <fstream>
#include <set>
#include <sstream>
#include <thread>

#ifndef CRAMION_VERSION_STRING
#define CRAMION_VERSION_STRING "0.0.0"
#endif

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace cramion::update {

namespace {

void setError(std::string* error, const std::string& text) {
    if (error != nullptr) *error = text;
}

// Texto del error de Windows en UTF-8 (ec.message() viene en la pagina de
// codigos del sistema y los acentos saldrian rotos en la interfaz).
#if defined(_WIN32)
std::string errorText(const std::error_code& ec) {
    wchar_t* buffer = nullptr;
    const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                   nullptr, static_cast<DWORD>(ec.value()), 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::wstring text = n > 0 && buffer != nullptr ? std::wstring(buffer, n) : L"error " + std::to_wstring(ec.value());
    if (buffer != nullptr) LocalFree(buffer);
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ' || text.back() == L'.')) text.pop_back();
    return narrow(text);
}
#else
// En Linux los mensajes del sistema ya vienen en UTF-8.
std::string errorText(const std::error_code& ec) { return ec.message(); }
#endif

std::string lowerAscii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

#if defined(_WIN32)
std::wstring lowerWide(std::wstring s) {
    for (wchar_t& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}
#endif

bool startsWith(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

std::string readTextFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool writeTextFile(const fs::path& path, const std::string& text) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    const fs::path temp = path.wstring() + L".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << text;
        if (!out) return false;
    }
    fs::rename(temp, path, ec);
    return !ec;
}

// "file:///C:/a%20b/x.zip" o "C:/x.zip" -> ruta; vacio si es http(s).
fs::path localPath(const std::string& url) {
    std::string rest;
    if (startsWith(url, "file:///")) {
#if defined(_WIN32)
        rest = url.substr(8);
#else
        rest = url.substr(7);  // file:///home/x: la ruta empieza en la tercera barra
#endif
    } else if (startsWith(url, "file://")) {
        rest = url.substr(7);
    } else if (url.find("://") == std::string::npos) {
        return fs::path(widen(url));
    } else {
        return {};
    }
    std::string decoded;
    for (std::size_t i = 0; i < rest.size(); ++i) {
        if (rest[i] == '%' && i + 2 < rest.size() && std::isxdigit(static_cast<unsigned char>(rest[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(rest[i + 2]))) {
            decoded += static_cast<char>(std::stoi(rest.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else {
            decoded += rest[i];
        }
    }
    return fs::path(widen(decoded));
}

// Nombre con el mutex del sistema: reopen.json lo pueden tocar varios editores.
#if defined(_WIN32)
class NamedLock {
public:
    explicit NamedLock(const wchar_t* name) : mutex_(CreateMutexW(nullptr, FALSE, name)) {
        if (mutex_ != nullptr) WaitForSingleObject(mutex_, 3000);
    }
    ~NamedLock() {
        if (mutex_ != nullptr) {
            ReleaseMutex(mutex_);
            CloseHandle(mutex_);
        }
    }
    NamedLock(const NamedLock&) = delete;
    NamedLock& operator=(const NamedLock&) = delete;

private:
    HANDLE mutex_;
};
#else
// Linux: flock sobre un archivo en /tmp (como el mutex: se espera hasta 3 s).
class NamedLock {
public:
    explicit NamedLock(const wchar_t* name) {
        const std::string path = "/tmp/" + narrow(name) + "-" + std::to_string(static_cast<unsigned long>(getuid())) + ".lock";
        fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (fd_ < 0) return;
        for (int i = 0; i < 300; ++i) {
            if (flock(fd_, LOCK_EX | LOCK_NB) == 0) return;
            usleep(10000);
        }
    }
    ~NamedLock() {
        if (fd_ >= 0) {
            flock(fd_, LOCK_UN);
            ::close(fd_);
        }
    }
    NamedLock(const NamedLock&) = delete;
    NamedLock& operator=(const NamedLock&) = delete;

private:
    int fd_ = -1;
};
#endif

// --- HTTP ----------------------------------------------------------------------------

#if defined(_WIN32)
struct Handle {
    HINTERNET h = nullptr;
    Handle() = default;
    explicit Handle(HINTERNET handle) : h(handle) {}
    ~Handle() {
        if (h != nullptr) WinHttpCloseHandle(h);
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    explicit operator bool() const { return h != nullptr; }
};

std::string winError(const char* what) {
    const DWORD code = GetLastError();
    std::string text = what;
    switch (code) {
        case ERROR_WINHTTP_NAME_NOT_RESOLVED: text += ": sin conexion a Internet (no se encuentra el servidor)"; break;
        case ERROR_WINHTTP_TIMEOUT: text += ": el servidor no responde (tiempo agotado)"; break;
        case ERROR_WINHTTP_CANNOT_CONNECT: text += ": no se pudo conectar con el servidor"; break;
        case ERROR_WINHTTP_SECURE_FAILURE: text += ": fallo la conexion segura (HTTPS)"; break;
        default: text += " (error " + std::to_string(code) + ")"; break;
    }
    return text;
}

#endif

using Sink = std::function<bool(const char* data, std::size_t size)>;

#if !defined(_WIN32)
// Lanza un programa sin shell (argv tal cual, se busca en el PATH si no lleva
// barra). `stdout_fd` >= 0: su salida va ahi (y la de error, a /dev/null).
// `detach`: sesion propia (no se cierra con el editor). -1 si falla.
pid_t spawnProgram(const std::vector<std::string>& args, int stdout_fd, const fs::path& cwd, bool detach) {
    if (args.empty()) return -1;
    std::vector<char*> argv;
    for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    if (stdout_fd >= 0) {
        posix_spawn_file_actions_adddup2(&actions, stdout_fd, 1);
        posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);
    }
    if (!cwd.empty()) posix_spawn_file_actions_addchdir_np(&actions, cwd.c_str());
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    sigset_t none;
    sigemptyset(&none);
    sigset_t defaults;
    sigemptyset(&defaults);
    sigaddset(&defaults, SIGPIPE);
    sigaddset(&defaults, SIGTERM);
    sigaddset(&defaults, SIGINT);
    posix_spawnattr_setsigmask(&attr, &none);
    posix_spawnattr_setsigdefault(&attr, &defaults);
    short flags = POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
    if (detach) flags |= POSIX_SPAWN_SETSID;
    posix_spawnattr_setflags(&attr, flags);
    pid_t pid = -1;
    const int rc = posix_spawnp(&pid, argv[0], &actions, &attr, argv.data(), environ);
    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&actions);
    return rc == 0 ? pid : -1;
}

// Un proceso lanzado y que no se espera: un hilo recoge su salida para que
// no quede zombi.
void reapLater(pid_t pid) {
    std::thread([pid] {
        int status = 0;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
    }).detach();
}

std::string curlError(int code) {
    std::string text = "No se pudo descargar";
    switch (code) {
        case 5:
        case 6: text += ": sin conexion a Internet (no se encuentra el servidor)"; break;
        case 7: text += ": no se pudo conectar con el servidor"; break;
        case 28: text += ": el servidor no responde (tiempo agotado)"; break;
        case 18:
        case 56: text = "Se corto la descarga"; break;
        case 35:
        case 51:
        case 53:
        case 54:
        case 58:
        case 59:
        case 60:
        case 77:
        case 80:
        case 83:
        case 90:
        case 91: text += ": fallo la conexion segura (HTTPS)"; break;
        case 3: text = "Direccion no valida"; break;
        default: text += " (curl, error " + std::to_string(code) + ")"; break;
    }
    return text;
}

// HTTP(S) con el programa curl: sin bibliotecas nuevas y con los
// certificados y el proxy del sistema. Con -D - las cabeceras salen por
// stdout delante del cuerpo (un bloque por redireccion); se leen a trozos para
// el progreso y se mata curl si se cancela.
bool curlTransfer(const std::string& url, const Sink& sink, const std::function<void(std::uint64_t)>& on_total,
                  std::string* error) {
    const std::string low = lowerAscii(url);
    if (!startsWith(low, "http://") && !startsWith(low, "https://")) {
        setError(error, "Direccion no valida: " + url);
        return false;
    }
    std::string host = low.substr(low.find("://") + 3);
    host = host.substr(0, host.find_first_of("/:?#"));
    std::vector<std::string> args = {"curl", "-s", "-L", "--max-redirs", "10", "--proto", "=http,https",
                                     "--connect-timeout", "10", "--speed-limit", "1", "--speed-time", "60",
                                     "-D", "-", "-A", "CramionUpdater/" + currentVersion().str()};
    if (host == "api.github.com") {
        args.insert(args.end(), {"-H", "Accept: application/vnd.github+json", "-H", "X-GitHub-Api-Version: 2022-11-28"});
    }
    args.insert(args.end(), {"--url", url});

    int fds[2] = {-1, -1};
    if (pipe2(fds, O_CLOEXEC) != 0) {
        setError(error, "No se pudo descargar (pipe: " + errorText(std::error_code(errno, std::generic_category())) + ")");
        return false;
    }
    const pid_t pid = spawnProgram(args, fds[1], {}, false);
    ::close(fds[1]);
    if (pid <= 0) {
        ::close(fds[0]);
        setError(error, "No se encuentra el programa curl (instalalo para buscar y descargar actualizaciones)");
        return false;
    }
    bool finished = false;
    int exit_status = 0;
    const auto finish = [&](bool stop) {
        if (finished) return exit_status;
        finished = true;
        if (stop) kill(pid, SIGTERM);
        ::close(fds[0]);
        while (waitpid(pid, &exit_status, 0) < 0 && errno == EINTR) {
        }
        return exit_status;
    };

    std::string head;  // cabeceras aun sin terminar
    bool in_headers = true;
    long status_code = 0;
    std::uint64_t length = 0;
    bool has_length = false;
    // Cabeceras leidas: se comprueba el codigo y empieza el cuerpo.
    const auto startBody = [&]() -> bool {
        in_headers = false;
        if (status_code != 200) {
            std::string text = "El servidor respondio " + std::to_string(status_code);
            if (status_code == 404) text += " (no encontrado)";
            if (status_code == 403 || status_code == 429) text += ": demasiadas consultas a GitHub, prueba dentro de un rato";
            finish(true);
            setError(error, text);
            return false;
        }
        if (on_total && has_length) on_total(length);
        return true;
    };
    std::vector<char> buffer(1 << 16);
    for (;;) {
        const ssize_t n = ::read(fds[0], buffer.data(), buffer.size());
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        const char* data = buffer.data();
        std::size_t size = static_cast<std::size_t>(n);
        if (in_headers) {
            head.append(data, size);
            // Bloques de cabeceras (redirecciones, proxy, 100 Continue) hasta
            // que lo siguiente ya no empieza por HTTP/.
            bool body = false;
            while (head.size() >= 5) {
                if (head.compare(0, 5, "HTTP/") != 0) {
                    body = true;
                    break;
                }
                std::size_t end = head.find("\r\n\r\n");
                std::size_t skip = 4;
                if (end == std::string::npos) {
                    end = head.find("\n\n");
                    skip = 2;
                }
                if (end == std::string::npos) break;
                std::istringstream lines(head.substr(0, end));
                std::string line;
                std::getline(lines, line);
                const std::size_t sp = line.find(' ');
                status_code = sp == std::string::npos ? 0 : std::strtol(line.c_str() + sp + 1, nullptr, 10);
                has_length = false;
                while (std::getline(lines, line)) {
                    if (startsWith(lowerAscii(line), "content-length:")) {
                        length = std::strtoull(line.c_str() + 15, nullptr, 10);
                        has_length = true;
                    }
                }
                head.erase(0, end + skip);
            }
            if (!body) continue;
            if (!startBody()) return false;
            data = head.data();
            size = head.size();
        }
        if (size > 0 && !sink(data, size)) {
            finish(true);
            setError(error, "Cancelado");
            return false;
        }
        head.clear();
    }
    const int status = finish(false);
    const int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (code != 0) {
        setError(error, code < 0 ? std::string("Se corto la descarga") : curlError(code));
        return false;
    }
    if (in_headers) {
        // Cuerpo corto (menos de 5 bytes) o vacio.
        if (status_code == 0) {
            setError(error, "El servidor no respondio");
            return false;
        }
        if (!startBody()) return false;
        if (!head.empty() && !sink(head.data(), head.size())) {
            setError(error, "Cancelado");
            return false;
        }
    }
    return true;
}
#endif

// Descarga `url` pasando los datos a `sink`. `total` = tamano si se conoce.
bool transfer(const std::string& url, const Sink& sink, const std::function<void(std::uint64_t)>& on_total,
              std::string* error) {
    if (const fs::path local = localPath(url); !local.empty()) {
        std::ifstream in(local, std::ios::binary);
        if (!in) {
            setError(error, "No se encuentra " + narrow(local.wstring()));
            return false;
        }
        std::error_code ec;
        if (on_total) on_total(static_cast<std::uint64_t>(fs::file_size(local, ec)));
        std::vector<char> buffer(1 << 16);
        while (in) {
            in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const std::streamsize got = in.gcount();
            if (got > 0 && !sink(buffer.data(), static_cast<std::size_t>(got))) {
                setError(error, "Cancelado");
                return false;
            }
        }
        return true;
    }

#if defined(_WIN32)
    const std::wstring wide_url = widen(url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wide_url.c_str(), 0, 0, &parts)) {
        setError(error, "Direccion no valida: " + url);
        return false;
    }
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.lpszExtraInfo != nullptr) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    if (path.empty()) path = L"/";
    const bool secure = parts.nScheme == INTERNET_SCHEME_HTTPS;

    const std::wstring agent = L"CramionUpdater/" + widen(currentVersion().str());
#ifdef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
    Handle session(WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                               WINHTTP_NO_PROXY_BYPASS, 0));
#else
    Handle session;
#endif
    if (!session) {
        session.h = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                WINHTTP_NO_PROXY_BYPASS, 0);
    }
    if (!session) {
        setError(error, winError("No se pudo iniciar WinHTTP"));
        return false;
    }
    WinHttpSetTimeouts(session.h, 10000, 15000, 20000, 60000);
    // TLS 1.2 y 1.3 (Windows 10 no activa la 1.3 por defecto).
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
    protocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
    WinHttpSetOption(session.h, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));

    Handle connection(WinHttpConnect(session.h, host.c_str(), parts.nPort, 0));
    if (!connection) {
        setError(error, winError("No se pudo conectar"));
        return false;
    }
    Handle request(WinHttpOpenRequest(connection.h, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                      WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0));
    if (!request) {
        setError(error, winError("No se pudo crear la peticion"));
        return false;
    }
    std::wstring headers;
    if (lowerWide(host) == L"api.github.com") {
        headers = L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
    }
    if (!WinHttpSendRequest(request.h, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                            headers.empty() ? 0 : static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.h, nullptr)) {
        setError(error, winError("No se pudo descargar"));
        return false;
    }
    DWORD status = 0;
    DWORD status_size = sizeof(status);
    WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &status_size, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) {
        std::string text = "El servidor respondio " + std::to_string(status);
        if (status == 404) text += " (no encontrado)";
        if (status == 403 || status == 429) text += ": demasiadas consultas a GitHub, prueba dentro de un rato";
        setError(error, text);
        return false;
    }
    if (on_total) {
        wchar_t length[32] = {};
        DWORD length_size = sizeof(length);
        if (WinHttpQueryHeaders(request.h, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, length,
                                &length_size, WINHTTP_NO_HEADER_INDEX)) {
            on_total(std::wcstoull(length, nullptr, 10));
        }
    }
    std::vector<char> buffer(1 << 16);
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.h, &available)) {
            setError(error, winError("Se corto la descarga"));
            return false;
        }
        if (available == 0) break;
        while (available > 0) {
            DWORD read = 0;
            const DWORD chunk = std::min<DWORD>(available, static_cast<DWORD>(buffer.size()));
            if (!WinHttpReadData(request.h, buffer.data(), chunk, &read)) {
                setError(error, winError("Se corto la descarga"));
                return false;
            }
            if (read == 0) break;
            if (!sink(buffer.data(), read)) {
                setError(error, "Cancelado");
                return false;
            }
            available -= read;
        }
    }
    return true;
#else
    return curlTransfer(url, sink, on_total, error);
#endif
}

// --- Zip -------------------------------------------------------------------------------

std::uint32_t crc32(const unsigned char* data, std::size_t size) {
    static const std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < size; ++i) crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

std::uint16_t le16(const unsigned char* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }
std::uint32_t le32(const unsigned char* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
std::uint64_t le64(const unsigned char* p) {
    return static_cast<std::uint64_t>(le32(p)) | (static_cast<std::uint64_t>(le32(p + 4)) << 32);
}

struct ZipEntry {
    std::string name;  // con '/'
    std::uint16_t method = 0;
    std::uint32_t crc = 0;
    std::uint64_t compressed = 0;
    std::uint64_t size = 0;
    std::uint64_t local_offset = 0;
    bool directory = false;
};

bool readZipDirectory(const std::vector<unsigned char>& zip, std::vector<ZipEntry>& entries, std::string* error) {
    const std::size_t n = zip.size();
    if (n < 22) {
        setError(error, "El zip esta vacio o roto");
        return false;
    }
    // Fin del directorio central: se busca hacia atras (puede haber comentario).
    std::size_t eocd = std::string::npos;
    const std::size_t lowest = n > 22 + 65535 ? n - 22 - 65535 : 0;
    for (std::size_t i = n - 22 + 1; i-- > lowest;) {
        if (le32(&zip[i]) == 0x06054b50u) {
            eocd = i;
            break;
        }
    }
    if (eocd == std::string::npos) {
        setError(error, "No es un zip valido (falta el directorio central)");
        return false;
    }
    std::uint64_t count = le16(&zip[eocd + 10]);
    std::uint64_t cd_offset = le32(&zip[eocd + 16]);
    // Zip64: el localizador esta justo antes.
    if ((count == 0xFFFFu || cd_offset == 0xFFFFFFFFu) && eocd >= 20 && le32(&zip[eocd - 20]) == 0x07064b50u) {
        const std::uint64_t z64 = le64(&zip[eocd - 20 + 8]);
        if (z64 + 56 <= n && le32(&zip[z64]) == 0x06064b50u) {
            count = le64(&zip[z64 + 32]);
            cd_offset = le64(&zip[z64 + 48]);
        }
    }
    std::size_t p = static_cast<std::size_t>(cd_offset);
    for (std::uint64_t i = 0; i < count; ++i) {
        if (p + 46 > n || le32(&zip[p]) != 0x02014b50u) {
            setError(error, "El directorio del zip esta roto");
            return false;
        }
        ZipEntry e;
        e.method = le16(&zip[p + 10]);
        e.crc = le32(&zip[p + 16]);
        e.compressed = le32(&zip[p + 20]);
        e.size = le32(&zip[p + 24]);
        const std::uint16_t name_len = le16(&zip[p + 28]);
        const std::uint16_t extra_len = le16(&zip[p + 30]);
        const std::uint16_t comment_len = le16(&zip[p + 32]);
        e.local_offset = le32(&zip[p + 42]);
        if (p + 46 + name_len + extra_len > n) {
            setError(error, "El directorio del zip esta roto");
            return false;
        }
        e.name.assign(reinterpret_cast<const char*>(&zip[p + 46]), name_len);
        std::replace(e.name.begin(), e.name.end(), '\\', '/');
        // Campo extra Zip64 (0x0001): solo los valores que valen 0xFFFFFFFF.
        std::size_t x = p + 46 + name_len;
        const std::size_t x_end = x + extra_len;
        while (x + 4 <= x_end) {
            const std::uint16_t id = le16(&zip[x]);
            const std::uint16_t len = le16(&zip[x + 2]);
            std::size_t q = x + 4;
            if (id == 0x0001u) {
                if (e.size == 0xFFFFFFFFu && q + 8 <= x_end) e.size = le64(&zip[q]), q += 8;
                if (e.compressed == 0xFFFFFFFFu && q + 8 <= x_end) e.compressed = le64(&zip[q]), q += 8;
                if (e.local_offset == 0xFFFFFFFFu && q + 8 <= x_end) e.local_offset = le64(&zip[q]);
            }
            x += 4u + len;
        }
        e.directory = !e.name.empty() && e.name.back() == '/';
        entries.push_back(std::move(e));
        p += 46u + name_len + extra_len + comment_len;
    }
    return true;
}

// Ruta segura dentro del destino (sin .., sin unidad ni raiz).
bool safeRelative(const std::string& name) {
    if (name.empty() || name[0] == '/' || name.find(':') != std::string::npos) return false;
    std::size_t start = 0;
    while (start <= name.size()) {
        const std::size_t end = name.find('/', start);
        const std::string part = name.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (part == "..") return false;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return true;
}

fs::path cleanupListFile() { return dataFolder() / "cleanup.txt"; }

}  // namespace

// --- Versiones -------------------------------------------------------------------------

Version Version::parse(std::string_view text) {
    Version v;
    std::size_t i = 0;
    while (i < text.size() && !std::isdigit(static_cast<unsigned char>(text[i]))) ++i;
    int* fields[3] = {&v.major, &v.minor, &v.patch};
    int parsed = 0;
    while (parsed < 3 && i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) {
        int value = 0;
        while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) {
            value = value * 10 + (text[i] - '0');
            if (value > 1000000) value = 1000000;
            ++i;
        }
        *fields[parsed++] = value;
        if (i < text.size() && text[i] == '.' && i + 1 < text.size() &&
            std::isdigit(static_cast<unsigned char>(text[i + 1]))) {
            ++i;
        } else {
            break;
        }
    }
    v.valid = parsed >= 2;
    // Sufijo de version previa: "-beta.1" (hasta un espacio).
    if (v.valid && i < text.size() && text[i] == '-') {
        std::size_t end = i + 1;
        while (end < text.size() && !std::isspace(static_cast<unsigned char>(text[end]))) ++end;
        v.pre = std::string(text.substr(i + 1, end - i - 1));
    }
    return v;
}

std::string Version::str() const {
    std::string s = std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
    if (!pre.empty()) s += "-" + pre;
    return s;
}

std::strong_ordering Version::operator<=>(const Version& other) const {
    if (major != other.major) return major <=> other.major;
    if (minor != other.minor) return minor <=> other.minor;
    if (patch != other.patch) return patch <=> other.patch;
    // La estable va despues de sus previas.
    if (pre.empty() || other.pre.empty()) return pre.empty() <=> other.pre.empty();
    // Partes separadas por '.': numeros como numeros, texto como texto.
    std::size_t a = 0, b = 0;
    while (a <= pre.size() && b <= other.pre.size()) {
        const std::size_t ea = std::min(pre.find('.', a), pre.size());
        const std::size_t eb = std::min(other.pre.find('.', b), other.pre.size());
        const std::string pa = pre.substr(a, ea - a), pb = other.pre.substr(b, eb - b);
        const bool na = !pa.empty() && std::all_of(pa.begin(), pa.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); });
        const bool nb = !pb.empty() && std::all_of(pb.begin(), pb.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); });
        std::strong_ordering order = std::strong_ordering::equal;
        if (na && nb) {
            order = std::stoll(pa) <=> std::stoll(pb);
        } else if (na != nb) {
            order = na ? std::strong_ordering::less : std::strong_ordering::greater;
        } else {
            const int c = pa.compare(pb);
            order = c < 0 ? std::strong_ordering::less : c > 0 ? std::strong_ordering::greater : std::strong_ordering::equal;
        }
        if (order != 0) return order;
        const bool end_a = ea >= pre.size(), end_b = eb >= other.pre.size();
        if (end_a || end_b) return end_b <=> end_a;  // mas partes = mas alta
        a = ea + 1;
        b = eb + 1;
    }
    return std::strong_ordering::equal;
}

Version currentVersion() {
    static const Version v = Version::parse(CRAMION_VERSION_STRING);
    return v;
}

// --- Releases --------------------------------------------------------------------------

std::string feedUrl() {
#if defined(_WIN32)
    wchar_t env[2048] = {};
    if (GetEnvironmentVariableW(L"CRAMION_UPDATE_FEED", env, 2048) > 0) return narrow(env);
#else
    if (const char* env = std::getenv("CRAMION_UPDATE_FEED"); env != nullptr && env[0] != '\0') return env;
#endif
    const Settings s = loadSettings();
    if (!s.feed.empty()) return s.feed;
    return s.channel == "beta" ? std::string(kBetaFeed) : std::string(kDefaultFeed);
}

bool betaChannel() { return loadSettings().channel == "beta"; }

bool parseRelease(const std::string& text, Release& out, std::string* error, const std::string& feed_url,
                  bool allow_prerelease) {
    json doc = json::parse(text, nullptr, false);
    if (doc.is_discarded()) {
        setError(error, "La respuesta del servidor no es JSON");
        return false;
    }
    // Lista de releases: la version mas alta que no sea borrador (ni previa,
    // salvo en el canal beta).
    if (doc.is_array()) {
        json best;
        Version best_version;
        for (const json& r : doc) {
            if (!r.is_object() || r.value("draft", false) || (!allow_prerelease && r.value("prerelease", false))) continue;
            const Version v = Version::parse(r.value("tag_name", r.value("version", std::string{})));
            if (v.valid && (!best_version.valid || v > best_version)) {
                best = r;
                best_version = v;
            }
        }
        if (best.is_null()) {
            setError(error, "No hay ninguna version publicada");
            return false;
        }
        doc = best;
    }
    if (!doc.is_object()) {
        setError(error, "Respuesta inesperada del servidor");
        return false;
    }
    if (doc.contains("message") && !doc.contains("tag_name")) {
        setError(error, "GitHub: " + doc.value("message", std::string{}));
        return false;
    }
    Release r;
    r.tag = doc.value("tag_name", doc.value("version", std::string{}));
    r.version = Version::parse(r.tag);
    if (!r.version.valid) {
        setError(error, "La version publicada no tiene numero (" + r.tag + ")");
        return false;
    }
    r.name = doc.value("name", std::string{});
    if (r.name.empty()) r.name = "Cramion " + r.version.str();
    const json& body = doc.contains("body") ? doc["body"] : doc.contains("notes") ? doc["notes"] : json();
    if (body.is_string()) r.notes = body.get<std::string>();
    r.page_url = kReleasesPage;
    r.published_at = doc.value("published_at", std::string{});
    r.prerelease = doc.value("prerelease", !r.version.pre.empty());
    const auto hexOf = [](std::string text) {
        // "sha256:abc..." o "abc...  Cramion-win64.zip": los 64 hex.
        if (text.starts_with("sha256:")) text = text.substr(7);
        std::string hex;
        for (const char c : text) {
            if (!std::isxdigit(static_cast<unsigned char>(c))) break;
            hex += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return hex.size() == 64 ? hex : std::string{};
    };
    if (doc.contains("assets") && doc["assets"].is_array()) {
        const json* pick = nullptr;
        for (const json& a : doc["assets"]) {
            const std::string name = a.value("name", std::string{});
            if (name == kPackageAsset) {
                pick = &a;
                break;
            }
            const std::string low = lowerAscii(name);
#if defined(_WIN32)
            if (pick == nullptr && low.size() > 4 && low.ends_with(".zip") && low.find("win64") != std::string::npos) pick = &a;
#else
            // Linux: solo el paquete de Linux (el de Windows no sirve aqui).
            if (pick == nullptr && low.size() > 4 && low.ends_with(".zip") && low.find("linux") != std::string::npos) pick = &a;
#endif
        }
        if (pick != nullptr) {
            r.zip_url = pick->value("browser_download_url", pick->value("url", std::string{}));
            r.zip_size = pick->value("size", std::uint64_t{0});
            if (pick->contains("digest") && (*pick)["digest"].is_string()) r.sha256 = hexOf((*pick)["digest"].get<std::string>());
            const std::string sidecar = pick->value("name", std::string{}) + ".sha256";
            for (const json& a : doc["assets"]) {
                if (a.value("name", std::string{}) == sidecar) {
                    r.sha256_url = a.value("browser_download_url", a.value("url", std::string{}));
                }
            }
        }
    } else if (doc.contains("zip_url")) {
        r.zip_url = doc.value("zip_url", std::string{});
        r.zip_size = doc.value("zip_size", std::uint64_t{0});
    }
    if (r.sha256.empty() && doc.contains("sha256") && doc["sha256"].is_string()) r.sha256 = hexOf(doc["sha256"].get<std::string>());
    // Zip (y su .sha256) con ruta relativa (feeds locales de prueba): junto al feed.
    const auto resolve = [&](std::string& url) {
        if (url.empty() || url.find("://") != std::string::npos || fs::path(widen(url)).is_absolute() || feed_url.empty()) return;
        if (const fs::path feed_file = localPath(feed_url); !feed_file.empty()) {
            url = narrow((feed_file.parent_path() / widen(url)).wstring());
        } else if (const std::size_t slash = feed_url.rfind('/'); slash != std::string::npos) {
            url = feed_url.substr(0, slash + 1) + url;
        }
    };
    resolve(r.zip_url);
    resolve(r.sha256_url);
    out = std::move(r);
    return true;
}

bool fetchLatest(const std::string& feed, Release& out, std::string* error, bool allow_prerelease) {
    std::string body;
    if (!httpGet(feed, body, error)) return false;
    return parseRelease(body, out, error, feed, allow_prerelease);
}

// --- SHA-256 (BCrypt de Windows) --------------------------------------------------------

namespace {

#if defined(_WIN32)
class Sha256 {
public:
    Sha256() {
        if (BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0) {
            BCryptCreateHash(algorithm_, &hash_, nullptr, 0, nullptr, 0, 0);
        }
    }
    ~Sha256() {
        if (hash_ != nullptr) BCryptDestroyHash(hash_);
        if (algorithm_ != nullptr) BCryptCloseAlgorithmProvider(algorithm_, 0);
    }
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;
    bool ok() const { return hash_ != nullptr; }
    void add(const void* data, std::size_t size) {
        const auto* p = static_cast<const unsigned char*>(data);
        while (size > 0) {
            const ULONG chunk = static_cast<ULONG>(std::min<std::size_t>(size, 1u << 30));
            BCryptHashData(hash_, const_cast<PUCHAR>(p), chunk, 0);
            p += chunk;
            size -= chunk;
        }
    }
    std::string hex() {
        unsigned char digest[32] = {};
        BCryptFinishHash(hash_, digest, sizeof(digest), 0);
        static constexpr char kHex[] = "0123456789abcdef";
        std::string out;
        for (const unsigned char b : digest) {
            out += kHex[b >> 4];
            out += kHex[b & 15];
        }
        return out;
    }

private:
    BCRYPT_ALG_HANDLE algorithm_ = nullptr;
    BCRYPT_HASH_HANDLE hash_ = nullptr;
};
#else
// SHA-256 propio (FIPS 180-4): en Linux no hay BCrypt y no se anade OpenSSL.
class Sha256 {
public:
    bool ok() const { return true; }
    void add(const void* data, std::size_t size) {
        const auto* p = static_cast<const unsigned char*>(data);
        bytes_ += size;
        while (size > 0) {
            const std::size_t take = std::min(size, std::size_t{64} - used_);
            std::memcpy(block_ + used_, p, take);
            used_ += take;
            p += take;
            size -= take;
            if (used_ == 64) {
                compress(block_);
                used_ = 0;
            }
        }
    }
    std::string hex() {
        const std::uint64_t bits = bytes_ * 8u;
        block_[used_++] = 0x80;
        if (used_ > 56) {
            std::memset(block_ + used_, 0, 64 - used_);
            compress(block_);
            used_ = 0;
        }
        std::memset(block_ + used_, 0, 56 - used_);
        for (int i = 0; i < 8; ++i) block_[56 + i] = static_cast<unsigned char>(bits >> (56 - 8 * i));
        compress(block_);
        used_ = 0;
        static constexpr char kHex[] = "0123456789abcdef";
        std::string out;
        for (const std::uint32_t word : h_) {
            for (int shift = 28; shift >= 0; shift -= 4) out += kHex[(word >> shift) & 15u];
        }
        return out;
    }

private:
    static std::uint32_t rotr(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
    void compress(const unsigned char* p) {
        static constexpr std::uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<std::uint32_t>(p[4 * i]) << 24) | (static_cast<std::uint32_t>(p[4 * i + 1]) << 16) |
                   (static_cast<std::uint32_t>(p[4 * i + 2]) << 8) | static_cast<std::uint32_t>(p[4 * i + 3]);
        }
        for (int i = 16; i < 64; ++i) {
            const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
        for (int i = 0; i < 64; ++i) {
            const std::uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            const std::uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h_[0] += a;
        h_[1] += b;
        h_[2] += c;
        h_[3] += d;
        h_[4] += e;
        h_[5] += f;
        h_[6] += g;
        h_[7] += h;
    }

    std::uint32_t h_[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    unsigned char block_[64] = {};
    std::size_t used_ = 0;
    std::uint64_t bytes_ = 0;
};
#endif

}  // namespace

std::string sha256Hex(const void* data, std::size_t size) {
    Sha256 sha;
    if (!sha.ok()) return {};
    sha.add(data, size);
    return sha.hex();
}

std::string sha256File(const fs::path& file, std::string* error) {
    std::ifstream in(file, std::ios::binary);
    Sha256 sha;
    if (!in || !sha.ok()) {
        setError(error, "No se puede leer " + narrow(file.wstring()));
        return {};
    }
    std::vector<char> buffer(1 << 20);
    while (in) {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        if (in.gcount() > 0) sha.add(buffer.data(), static_cast<std::size_t>(in.gcount()));
    }
    return sha.hex();
}

bool verifyPackage(const Release& release, const fs::path& zip, bool* verified, std::string* error) {
    if (verified != nullptr) *verified = false;
    std::string expected = release.sha256;
    if (expected.empty() && !release.sha256_url.empty()) {
        std::string text;
        if (!httpGet(release.sha256_url, text, error)) {
            setError(error, "No se pudo descargar el SHA-256 del paquete: " + (error != nullptr ? *error : std::string{}));
            return false;
        }
        for (const char c : text) {
            if (!std::isxdigit(static_cast<unsigned char>(c))) break;
            expected += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (expected.size() != 64) {
            setError(error, "El archivo .sha256 publicado no es valido");
            return false;
        }
    }
    if (expected.empty()) return true;  // la release no publica SHA-256
    const std::string actual = sha256File(zip, error);
    if (actual.empty()) return false;
    if (actual != expected) {
        setError(error, "El paquete descargado no coincide con su SHA-256 publicado (esperado " + expected.substr(0, 12) +
                            "..., llego " + actual.substr(0, 12) + "...). Se ha borrado; no se instala nada.");
        return false;
    }
    if (verified != nullptr) *verified = true;
    return true;
}

std::vector<NoteLine> parseNotes(const std::string& markdown) {
    std::vector<NoteLine> lines;
    std::istringstream in(markdown);
    std::string raw;
    const auto clean = [](std::string s) {
        // [texto](url) -> texto
        for (std::size_t open = s.find('['); open != std::string::npos; open = s.find('[', open + 1)) {
            const std::size_t close = s.find("](", open);
            const std::size_t end = close == std::string::npos ? close : s.find(')', close);
            if (close == std::string::npos || end == std::string::npos) break;
            s = s.substr(0, open) + s.substr(open + 1, close - open - 1) + s.substr(end + 1);
        }
        std::string out;
        for (std::size_t i = 0; i < s.size(); ++i) {
            if ((s[i] == '*' && i + 1 < s.size() && s[i + 1] == '*') || (s[i] == '_' && i + 1 < s.size() && s[i + 1] == '_')) {
                ++i;
                continue;
            }
            if (s[i] == '`' || s[i] == '*') continue;  // *cursiva* tambien
            out += s[i];
        }
        while (!out.empty() && (out.back() == ' ' || out.back() == '\t')) out.pop_back();
        return out;
    };
    while (std::getline(in, raw)) {
        if (!raw.empty() && raw.back() == '\r') raw.pop_back();
        std::size_t indent = 0;
        while (indent < raw.size() && (raw[indent] == ' ' || raw[indent] == '\t')) ++indent;
        const std::string body = raw.substr(indent);
        NoteLine line;
        if (body.empty()) {
            if (lines.empty() || lines.back().kind == NoteLine::Kind::Blank) continue;
            line.kind = NoteLine::Kind::Blank;
        } else if (body[0] == '#') {
            std::size_t level = 0;
            while (level < body.size() && body[level] == '#') ++level;
            line.kind = NoteLine::Kind::Heading;
            line.level = static_cast<int>(level);
            line.text = clean(body.substr(std::min(body.size(), level + (level < body.size() && body[level] == ' ' ? 1 : 0))));
        } else if (body.size() >= 2 && (body[0] == '-' || body[0] == '*' || body[0] == '+') && body[1] == ' ') {
            line.kind = NoteLine::Kind::Bullet;
            line.level = static_cast<int>(indent / 2);
            line.text = clean(body.substr(2));
        } else {
            line.kind = NoteLine::Kind::Text;
            line.text = clean(body);
        }
        lines.push_back(std::move(line));
    }
    while (!lines.empty() && lines.back().kind == NoteLine::Kind::Blank) lines.pop_back();
    return lines;
}

std::string formatDate(const std::string& iso) {
    if (iso.size() < 10) return iso;
    return iso.substr(8, 2) + "/" + iso.substr(5, 2) + "/" + iso.substr(0, 4);
}

std::string formatBytes(std::uint64_t bytes) {
    char text[64];
    if (bytes >= 1024ull * 1024ull * 1024ull) {
        std::snprintf(text, sizeof(text), "%.1f GB", static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
    } else if (bytes >= 1024ull * 1024ull) {
        std::snprintf(text, sizeof(text), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    } else if (bytes >= 1024ull) {
        std::snprintf(text, sizeof(text), "%.0f KB", static_cast<double>(bytes) / 1024.0);
    } else {
        std::snprintf(text, sizeof(text), "%llu B", static_cast<unsigned long long>(bytes));
    }
    std::string s = text;
    std::replace(s.begin(), s.end(), '.', ',');
    return s;
}

// --- Descargas -------------------------------------------------------------------------

bool httpGet(const std::string& url, std::string& body, std::string* error) {
    body.clear();
    return transfer(
        url,
        [&](const char* data, std::size_t size) {
            body.append(data, size);
            return body.size() < 64u * 1024u * 1024u;
        },
        {}, error);
}

bool downloadFile(const std::string& url, const fs::path& destination, const Progress& progress, std::string* error) {
    std::error_code ec;
    fs::create_directories(destination.parent_path(), ec);
    const fs::path part = destination.wstring() + L".part";
    std::uint64_t total = 0;
    std::uint64_t done = 0;
    bool ok = false;
    {
        std::ofstream out(part, std::ios::binary | std::ios::trunc);
        if (!out) {
            setError(error, "No se puede escribir en " + narrow(part.parent_path().wstring()));
            return false;
        }
        ok = transfer(
            url,
            [&](const char* data, std::size_t size) {
                out.write(data, static_cast<std::streamsize>(size));
                if (!out) return false;
                done += size;
                return !progress || progress(done, total);
            },
            [&](std::uint64_t t) {
                total = t;
                if (progress) progress(0, total);
            },
            error);
        if (ok && !out) {
            setError(error, "No hay espacio en el disco para la descarga");
            ok = false;
        }
    }
    if (ok && total > 0 && done != total) {
        setError(error, "La descarga llego incompleta (" + formatBytes(done) + " de " + formatBytes(total) + ")");
        ok = false;
    }
    if (!ok) {
        fs::remove(part, ec);
        return false;
    }
    fs::remove(destination, ec);
    fs::rename(part, destination, ec);
    if (ec) {
        setError(error, "No se pudo guardar la descarga: " + errorText(ec));
        return false;
    }
    return true;
}

// --- Zip e instalacion -----------------------------------------------------------------

bool extractZip(const fs::path& zip_path, const fs::path& destination, const Progress& progress, std::string* error) {
    std::vector<unsigned char> zip;
    {
        std::ifstream in(zip_path, std::ios::binary);
        if (!in) {
            setError(error, "No se puede abrir " + narrow(zip_path.wstring()));
            return false;
        }
        in.seekg(0, std::ios::end);
        zip.resize(static_cast<std::size_t>(in.tellg()));
        in.seekg(0);
        in.read(reinterpret_cast<char*>(zip.data()), static_cast<std::streamsize>(zip.size()));
        if (!in) {
            setError(error, "No se pudo leer el zip");
            return false;
        }
    }
    std::vector<ZipEntry> entries;
    if (!readZipDirectory(zip, entries, error)) return false;

    // Carpeta comun de primer nivel (Cramion-0.6.2-win64/): se quita.
    std::string prefix;
    {
        bool first = true;
        for (const ZipEntry& e : entries) {
            const std::size_t slash = e.name.find('/');
            const std::string top = slash == std::string::npos ? std::string{} : e.name.substr(0, slash + 1);
            if (first) {
                prefix = top;
                first = false;
            } else if (top != prefix) {
                prefix.clear();
                break;
            }
            if (top.empty()) {
                prefix.clear();
                break;
            }
        }
    }

    std::error_code ec;
    fs::remove_all(destination, ec);
    fs::create_directories(destination, ec);
    if (ec) {
        setError(error, "No se puede crear " + narrow(destination.wstring()));
        return false;
    }
    std::uint64_t total = 0;
    for (const ZipEntry& e : entries) total += e.size;
    std::uint64_t done = 0;
    std::vector<unsigned char> data;
    for (const ZipEntry& e : entries) {
        const std::string name = e.name.substr(prefix.size());
        if (name.empty()) continue;
        if (!safeRelative(name)) {
            setError(error, "El zip tiene una ruta no permitida: " + e.name);
            return false;
        }
        const fs::path target = destination / fs::path(widen(name));
        if (e.directory) {
            fs::create_directories(target, ec);
            continue;
        }
        const std::size_t local = static_cast<std::size_t>(e.local_offset);
        if (local + 30 > zip.size() || le32(&zip[local]) != 0x04034b50u) {
            setError(error, "El zip esta roto (" + e.name + ")");
            return false;
        }
        const std::size_t start = local + 30u + le16(&zip[local + 26]) + le16(&zip[local + 28]);
        if (start + e.compressed > zip.size()) {
            setError(error, "El zip esta incompleto (" + e.name + ")");
            return false;
        }
        data.resize(static_cast<std::size_t>(e.size));
        if (e.method == 0) {
            if (e.compressed != e.size) {
                setError(error, "El zip esta roto (" + e.name + ")");
                return false;
            }
            if (e.size > 0) std::memcpy(data.data(), &zip[start], static_cast<std::size_t>(e.size));
        } else if (e.method == 8) {
            if (!detail::inflateRaw(&zip[start], static_cast<std::size_t>(e.compressed), data.data(), data.size())) {
                setError(error, "No se pudo descomprimir " + e.name);
                return false;
            }
        } else {
            setError(error, "Compresion no soportada en " + e.name + " (metodo " + std::to_string(e.method) + ")");
            return false;
        }
        if (crc32(data.data(), data.size()) != e.crc) {
            setError(error, "El archivo " + e.name + " llego danado (CRC). Vuelve a descargar.");
            return false;
        }
        fs::create_directories(target.parent_path(), ec);
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!out) {
            setError(error, "No se pudo escribir " + narrow(target.wstring()));
            return false;
        }
        done += e.size;
        if (progress && !progress(done, total)) {
            setError(error, "Cancelado");
            return false;
        }
    }
    return true;
}

bool looksLikeEnginePackage(const fs::path& folder) {
    std::error_code ec;
#if defined(_WIN32)
    return fs::exists(folder / kEditorExe, ec) && fs::exists(folder / "shaders", ec);
#else
    return (fs::exists(folder / "CramionEditor", ec) || fs::exists(folder / kEditorExe, ec)) && fs::exists(folder / "shaders", ec);
#endif
}

bool installFiles(const fs::path& staged, const fs::path& install_dir, const Progress& progress, std::string* error) {
    struct Step {
        fs::path target;
        fs::path old;  // vacio si no existia
        bool copied = false;
    };
    std::vector<fs::path> files;
    std::uint64_t total = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(staged, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        files.push_back(fs::relative(it->path(), staged, ec));
        total += it->file_size(ec);
    }
    if (ec || files.empty()) {
        setError(error, "La carpeta descomprimida esta vacia");
        return false;
    }
    std::vector<Step> steps;
    const auto rollback = [&] {
        for (auto it = steps.rbegin(); it != steps.rend(); ++it) {
            std::error_code e;
            if (it->copied) fs::remove(it->target, e);
            if (!it->old.empty()) fs::rename(it->old, it->target, e);
        }
    };
    std::uint64_t done = 0;
    for (const fs::path& rel : files) {
        const fs::path source = staged / rel;
        const fs::path target = install_dir / rel;
        fs::create_directories(target.parent_path(), ec);
        Step step;
        step.target = target;
        if (fs::exists(target, ec)) {
            // Un nombre libre para el viejo (el de una instalacion anterior
            // puede seguir en uso).
            fs::path old = target.wstring() + kOldSuffix;
            for (int i = 2; fs::exists(old, ec); ++i) {
                std::error_code e;
                if (fs::remove(old, e)) break;
                old = target.wstring() + kOldSuffix + std::to_wstring(i);
            }
            fs::rename(target, old, ec);
            if (ec) {
                rollback();
                setError(error, "No se puede reemplazar " + narrow(rel.wstring()) +
                                    ": esta en uso o sin permiso (" + errorText(ec) + ")");
                return false;
            }
            step.old = old;
        }
        fs::copy_file(source, target, fs::copy_options::overwrite_existing, ec);
        step.copied = !ec;
        steps.push_back(step);
        if (ec) {
            rollback();
            setError(error, "No se pudo copiar " + narrow(rel.wstring()) + ": " + errorText(ec));
            return false;
        }
        done += fs::file_size(source, ec);
        if (progress && !progress(done, total)) {
            rollback();
            setError(error, "Cancelado");
            return false;
        }
    }
    // Los viejos se borran ahora si se puede; los que siguen en uso (el propio
    // actualizador y sus .dll), al volver a abrir el motor.
    std::string pending = readTextFile(cleanupListFile());
    for (const Step& s : steps) {
        if (s.old.empty()) continue;
        std::error_code e;
        if (!fs::remove(s.old, e)) pending += narrow(s.old.wstring()) + "\n";
    }
    writeTextFile(cleanupListFile(), pending);
    return true;
}

int cleanupOldFiles() {
    NamedLock lock(L"CramionUpdateCleanup");
    const std::string list = readTextFile(cleanupListFile());
    if (list.empty()) return 0;
    std::istringstream in(list);
    std::string line;
    std::string remaining;
    int left = 0;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        const fs::path path(widen(line));
        // Solo lo que puso el actualizador.
        if (path.filename().wstring().find(kOldSuffix) == std::wstring::npos) continue;
        std::error_code ec;
        if (!fs::exists(path, ec)) continue;
        if (!fs::remove(path, ec)) {
            remaining += line + "\n";
            ++left;
        }
    }
    std::error_code ec;
    if (remaining.empty()) {
        fs::remove(cleanupListFile(), ec);
    } else {
        writeTextFile(cleanupListFile(), remaining);
    }
    return left;
}

// --- Ajustes y carpetas ----------------------------------------------------------------

fs::path dataFolder() {
#if defined(_WIN32)
    wchar_t env[1024] = {};
    if (GetEnvironmentVariableW(L"CRAMION_UPDATE_DATA", env, 1024) > 0) return fs::path(env);
    PWSTR local = nullptr;
    fs::path folder;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) folder = fs::path(local) / "Cramion";
    CoTaskMemFree(local);
    if (folder.empty()) folder = fs::temp_directory_path() / "Cramion";
    return folder;
#else
    // $XDG_DATA_HOME/Cramion o ~/.local/share/Cramion.
    if (const char* env = std::getenv("CRAMION_UPDATE_DATA"); env != nullptr && env[0] != '\0') return fs::path(env);
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg != nullptr && xdg[0] == '/') return fs::path(xdg) / "Cramion";
    if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
        return fs::path(home) / ".local" / "share" / "Cramion";
    }
    return fs::temp_directory_path() / "Cramion";
#endif
}

fs::path downloadsFolder() { return dataFolder() / "Updates"; }

fs::path installFolder() {
#if !defined(_WIN32)
    std::error_code ec;
    const fs::path exe = fs::read_symlink("/proc/self/exe", ec);
    return ec ? fs::current_path(ec) : exe.parent_path();
#else
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (n < buffer.size()) {
            buffer.resize(n);
            break;
        }
        buffer.resize(buffer.size() * 2);
    }
    return fs::path(buffer).parent_path();
#endif
}

Settings loadSettings() {
    Settings s;
    const json doc = json::parse(readTextFile(dataFolder() / "update.json"), nullptr, false);
    if (!doc.is_object()) return s;
    s.check_on_startup = doc.value("check_on_startup", true);
    s.skipped_version = doc.value("skipped_version", std::string{});
    s.feed = doc.value("feed", std::string{});
    s.last_check = doc.value("last_check", std::int64_t{0});
    s.last_seen_version = doc.value("last_seen_version", std::string{});
    s.channel = doc.value("channel", std::string("estable"));
    if (s.channel != "beta") s.channel = "estable";
    return s;
}

void saveSettings(const Settings& s) {
    json doc;
    doc["check_on_startup"] = s.check_on_startup;
    doc["skipped_version"] = s.skipped_version;
    doc["feed"] = s.feed;
    doc["last_check"] = s.last_check;
    doc["last_seen_version"] = s.last_seen_version;
    doc["channel"] = s.channel;
    writeTextFile(dataFolder() / "update.json", doc.dump(2));
}

namespace {

json readReopen() {
    json doc = json::parse(readTextFile(dataFolder() / "reopen.json"), nullptr, false);
    if (!doc.is_object()) doc = json::object();
    if (!doc.contains("projects") || !doc["projects"].is_array()) doc["projects"] = json::array();
    return doc;
}

}  // namespace

void addReopenProject(const fs::path& project) {
    NamedLock lock(L"CramionReopenList");
    json doc = readReopen();
    const std::string path = narrow(project.wstring());
    for (const json& p : doc["projects"]) {
        if (p.is_string() && p.get<std::string>() == path) return;
    }
    doc["projects"].push_back(path);
    writeTextFile(dataFolder() / "reopen.json", doc.dump(2));
}

void markReopenHub() {
    NamedLock lock(L"CramionReopenList");
    json doc = readReopen();
    doc["hub"] = true;
    writeTextFile(dataFolder() / "reopen.json", doc.dump(2));
}

ReopenList takeReopenList() {
    NamedLock lock(L"CramionReopenList");
    ReopenList list;
    const json doc = readReopen();
    list.hub = doc.value("hub", false);
    for (const json& p : doc["projects"]) {
        if (p.is_string()) list.projects.emplace_back(widen(p.get<std::string>()));
    }
    std::error_code ec;
    fs::remove(dataFolder() / "reopen.json", ec);
    return list;
}

// --- Procesos --------------------------------------------------------------------------

#if defined(_WIN32)
std::vector<RunningApp> runningEngineApps(const fs::path& install_dir, unsigned long exclude_pid) {
    std::vector<RunningApp> apps;
    std::error_code ec;
    const std::wstring folder = lowerWide(fs::weakly_canonical(install_dir, ec).wstring());
    static const std::set<std::wstring> kApps = {L"cramioneditor.exe", L"cramionplayer.exe", L"cramion.exe"};
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return apps;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL ok = Process32FirstW(snapshot, &entry); ok; ok = Process32NextW(snapshot, &entry)) {
        if (entry.th32ProcessID == exclude_pid) continue;
        const std::wstring exe = lowerWide(entry.szExeFile);
        if (!kApps.contains(exe)) continue;
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
        if (process == nullptr) continue;
        wchar_t path[MAX_PATH * 2] = {};
        DWORD size = MAX_PATH * 2;
        const BOOL got = QueryFullProcessImageNameW(process, 0, path, &size);
        CloseHandle(process);
        if (!got) continue;
        const std::wstring dir = lowerWide(fs::weakly_canonical(fs::path(path).parent_path(), ec).wstring());
        if (dir != folder) continue;
        apps.push_back({entry.th32ProcessID, entry.szExeFile, exe == L"cramioneditor.exe"});
    }
    CloseHandle(snapshot);
    return apps;
}

int askAppsToSaveAndClose(const std::vector<RunningApp>& apps) {
    struct Context {
        const std::vector<RunningApp>* apps;
        UINT message;
        int sent;
    } context{&apps, RegisterWindowMessageW(kPrepareMessage), 0};
    EnumWindows(
        [](HWND hwnd, LPARAM param) -> BOOL {
            auto* c = reinterpret_cast<Context*>(param);
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;
            for (const RunningApp& app : *c->apps) {
                if (app.pid != pid) continue;
                // El editor guarda todo; el resto (juegos, demo) se cierra.
                if (PostMessageW(hwnd, app.editor ? c->message : WM_CLOSE, 0, 0)) ++c->sent;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&context));
    return context.sent;
}
#else
// Linux: /proc/<pid>/exe de cada proceso (solo los del usuario se pueden leer).
std::vector<RunningApp> runningEngineApps(const fs::path& install_dir, unsigned long exclude_pid) {
    std::vector<RunningApp> apps;
    std::error_code ec;
    const fs::path folder = fs::weakly_canonical(install_dir, ec);
    static const std::set<std::string> kApps = {"cramioneditor", "cramionplayer", "cramion",
                                                "cramioneditor.exe", "cramionplayer.exe", "cramion.exe"};
    for (fs::directory_iterator it("/proc", ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (name.empty() || name.find_first_not_of("0123456789") != std::string::npos) continue;
        const unsigned long pid = std::strtoul(name.c_str(), nullptr, 10);
        if (pid == exclude_pid) continue;
        std::error_code e;
        const fs::path exe = fs::read_symlink(it->path() / "exe", e);
        if (e || exe.empty()) continue;
        const std::string file = exe.filename().string();
        const std::string low = lowerAscii(file);
        if (!kApps.contains(low)) continue;
        if (fs::weakly_canonical(exe.parent_path(), e) != folder) continue;
        apps.push_back({pid, widen(file), low.starts_with("cramioneditor")});
    }
    return apps;
}

// Linux: no hay un mensaje de ventana entre procesos para "guarda y
// cierrate", y cerrar un editor con SIGTERM perderia lo que no esta guardado.
// No se pide nada (0): quien llama debe avisar de que se cierren a mano.
int askAppsToSaveAndClose(const std::vector<RunningApp>& apps) {
    (void)apps;
    return 0;
}
#endif

std::wstring quoteArgument(const std::wstring& argument) {
    if (!argument.empty() && argument.find_first_of(L" \t\"") == std::wstring::npos) return argument;
    std::wstring out = L"\"";
    int backslashes = 0;
    for (const wchar_t c : argument) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        if (c == L'"') {
            out.append(static_cast<std::size_t>(backslashes * 2 + 1), L'\\');
        } else {
            out.append(static_cast<std::size_t>(backslashes), L'\\');
        }
        backslashes = 0;
        out += c;
    }
    out.append(static_cast<std::size_t>(backslashes * 2), L'\\');
    out += L'"';
    return out;
}

#if defined(_WIN32)
unsigned long launch(const fs::path& exe, const std::wstring& arguments, const fs::path& working_dir) {
    std::wstring command = quoteArgument(exe.wstring());
    if (!arguments.empty()) command += L" " + arguments;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    const fs::path cwd = working_dir.empty() ? exe.parent_path() : working_dir;
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, cwd.c_str(), &startup, &info)) {
        return 0;
    }
    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);
    return info.dwProcessId;
}

std::wstring widen(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
    return out;
}

std::string narrow(const std::wstring& wide) {
    if (wide.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), n, nullptr, nullptr);
    return out;
}

bool openUrl(const std::string& url) {
    return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
}
#else
namespace {

// Los argumentos vienen entrecomillados como en Windows (quoteArgument): se
// separan con las mismas reglas para pasarlos como argv.
std::vector<std::string> splitArguments(const std::wstring& line) {
    std::vector<std::string> out;
    std::size_t i = 0;
    const std::size_t n = line.size();
    while (i < n) {
        while (i < n && (line[i] == L' ' || line[i] == L'\t')) ++i;
        if (i >= n) break;
        std::wstring arg;
        bool quoted = false;
        while (i < n && (quoted || (line[i] != L' ' && line[i] != L'\t'))) {
            std::size_t backslashes = 0;
            while (i < n && line[i] == L'\\') {
                ++backslashes;
                ++i;
            }
            if (i < n && line[i] == L'"') {
                arg.append(backslashes / 2, L'\\');
                if (backslashes % 2 == 1) {
                    arg += L'"';
                } else {
                    quoted = !quoted;
                }
                ++i;
            } else {
                arg.append(backslashes, L'\\');
                if (i < n && (quoted || (line[i] != L' ' && line[i] != L'\t'))) arg += line[i++];
            }
        }
        out.push_back(narrow(arg));
    }
    return out;
}

}  // namespace

unsigned long launch(const fs::path& exe, const std::wstring& arguments, const fs::path& working_dir) {
    std::vector<std::string> args = {exe.string()};
    for (std::string& a : splitArguments(arguments)) args.push_back(std::move(a));
    const fs::path cwd = working_dir.empty() ? exe.parent_path() : working_dir;
    const pid_t pid = spawnProgram(args, -1, cwd, true);
    if (pid <= 0) return 0;
    reapLater(pid);
    return static_cast<unsigned long>(pid);
}

// UTF-8 <-> wchar_t (UTF-32 en Linux). Lo que no es UTF-8 valido sale como U+FFFD.
std::wstring widen(const std::string& utf8) {
    std::wstring out;
    out.reserve(utf8.size());
    const auto* p = reinterpret_cast<const unsigned char*>(utf8.data());
    const std::size_t n = utf8.size();
    for (std::size_t i = 0; i < n;) {
        const unsigned char c = p[i];
        std::uint32_t cp = 0;
        int extra = 0;
        if (c < 0x80) {
            cp = c;
        } else if ((c & 0xE0) == 0xC0) {
            cp = c & 0x1Fu;
            extra = 1;
        } else if ((c & 0xF0) == 0xE0) {
            cp = c & 0x0Fu;
            extra = 2;
        } else if ((c & 0xF8) == 0xF0) {
            cp = c & 0x07u;
            extra = 3;
        } else {
            out += static_cast<wchar_t>(0xFFFD);
            ++i;
            continue;
        }
        bool ok = i + static_cast<std::size_t>(extra) < n;
        for (int k = 1; ok && k <= extra; ++k) {
            if ((p[i + k] & 0xC0) != 0x80) {
                ok = false;
            } else {
                cp = (cp << 6) | (p[i + k] & 0x3Fu);
            }
        }
        if (!ok || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            out += static_cast<wchar_t>(0xFFFD);
            ++i;
            continue;
        }
        out += static_cast<wchar_t>(cp);
        i += 1u + static_cast<std::size_t>(extra);
    }
    return out;
}

std::string narrow(const std::wstring& wide) {
    std::string out;
    out.reserve(wide.size());
    for (const wchar_t w : wide) {
        std::uint32_t cp = static_cast<std::uint32_t>(w);
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    return out;
}

// xdg-open (el navegador o la aplicacion del escritorio).
bool openUrl(const std::string& url) {
    const pid_t pid = spawnProgram({"xdg-open", url}, -1, {}, true);
    if (pid <= 0) return false;
    reapLater(pid);
    return true;
}
#endif

// --- Busqueda en segundo plano ---------------------------------------------------------

std::shared_ptr<CheckJob> startCheck(const std::string& feed) {
    auto job = std::make_shared<CheckJob>();
    const std::string url = feed.empty() ? feedUrl() : feed;
    const bool beta = betaChannel();
    std::thread([job, url, beta] {
        Release release;
        std::string error;
        if (fetchLatest(url, release, &error, beta)) {
            Settings s = loadSettings();
            s.last_check = static_cast<std::int64_t>(std::time(nullptr));
            s.last_seen_version = release.version.str();
            saveSettings(s);
            job->release = std::move(release);
            job->state = CheckJob::State::Done;
        } else {
            job->error = error;
            job->state = CheckJob::State::Failed;
        }
    }).detach();
    return job;
}

}  // namespace cramion::update
