#include "CramionCore/net/Http.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace cramion::net {
namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string narrow(const std::wstring& s) {
    if (s.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

struct Handle {
    HINTERNET h = nullptr;
    explicit Handle(HINTERNET handle = nullptr) : h(handle) {}
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
        case ERROR_WINHTTP_NAME_NOT_RESOLVED: text += ": no se encuentra el servidor (sin Internet o nombre mal escrito)"; break;
        case ERROR_WINHTTP_TIMEOUT: text += ": el servidor no responde (tiempo agotado)"; break;
        case ERROR_WINHTTP_CANNOT_CONNECT: text += ": no se pudo conectar con el servidor"; break;
        case ERROR_WINHTTP_CONNECTION_ERROR: text += ": se corto la conexion"; break;
        case ERROR_WINHTTP_SECURE_FAILURE:
        case ERROR_WINHTTP_SECURE_INVALID_CERT:
        case ERROR_WINHTTP_SECURE_CERT_CN_INVALID:
        case ERROR_WINHTTP_SECURE_CERT_DATE_INVALID:
        case ERROR_WINHTTP_SECURE_INVALID_CA:
            text += ": conexion segura rechazada (el certificado del servidor no es valido)";
            break;
        case ERROR_WINHTTP_REDIRECT_FAILED: text += ": redireccion no permitida (de https a http)"; break;
        case ERROR_WINHTTP_OPERATION_CANCELLED: text += ": cancelada"; break;
        default: text += " (error " + std::to_string(code) + ")"; break;
    }
    return text;
}

bool isLocalHost(const std::string& host) {
    const std::string h = lower(host);
    return h == "localhost" || h == "127.0.0.1" || h == "::1" || h == "[::1]";
}

struct UrlParts {
    bool secure = false;
    std::wstring host;
    INTERNET_PORT port = 0;
    std::wstring path;
};

bool crack(const std::string& url, UrlParts& out, std::string* error) {
    const auto fail = [&](const std::string& why) {
        if (error != nullptr) *error = why;
        return false;
    };
    if (url.empty()) return fail("Direccion vacia");
    for (const unsigned char c : url) {
        if (c < 0x21 || c == 0x7F) return fail("La direccion no puede llevar espacios ni caracteres de control (usa Http.urlEncode)");
    }
    const std::wstring wide = widen(url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wide.c_str(), 0, 0, &parts) || parts.dwHostNameLength == 0) {
        return fail("Direccion no valida: " + url);
    }
    out.host.assign(parts.lpszHostName, parts.dwHostNameLength);
    out.path.assign(parts.lpszUrlPath != nullptr ? parts.lpszUrlPath : L"", parts.dwUrlPathLength);
    if (parts.lpszExtraInfo != nullptr) out.path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    if (out.path.empty()) out.path = L"/";
    out.port = parts.nPort;
    if (parts.nScheme == INTERNET_SCHEME_HTTPS) {
        out.secure = true;
    } else if (parts.nScheme == INTERNET_SCHEME_HTTP) {
        if (!isLocalHost(narrow(out.host))) {
            return fail("Solo se permite https:// (http:// sin cifrar solo a localhost para pruebas): " + url);
        }
    } else {
        return fail("Solo se permite https://: " + url);
    }
    return true;
}

bool validMethod(const std::string& m) {
    if (m.empty() || m.size() > 16) return false;
    return std::all_of(m.begin(), m.end(), [](unsigned char c) { return std::isupper(c) != 0; });
}

HttpHeaders parseHeaders(const std::wstring& raw) {
    HttpHeaders out;
    const std::string text = narrow(raw);
    std::size_t at = text.find("\r\n");  // la primera linea es el estado
    while (at != std::string::npos) {
        const std::size_t start = at + 2;
        at = text.find("\r\n", start);
        const std::string line = text.substr(start, at == std::string::npos ? std::string::npos : at - start);
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos || colon == 0) continue;
        std::string value = line.substr(colon + 1);
        const std::size_t first = value.find_first_not_of(" \t");
        value = first == std::string::npos ? std::string() : value.substr(first);
        out.emplace_back(lower(line.substr(0, colon)), value);
    }
    return out;
}

}  // namespace

bool checkUrl(const std::string& url, std::string* error) {
    UrlParts parts;
    return crack(url, parts, error);
}

bool validHeader(const std::string& name, const std::string& value) {
    if (name.empty() || name.size() > 256 || value.size() > 8192) return false;
    for (const unsigned char c : name) {
        if (c <= 0x20 || c >= 0x7F || c == ':') return false;
    }
    for (const unsigned char c : value) {
        if ((c < 0x20 && c != '\t') || c == 0x7F) return false;
    }
    return true;
}

std::string urlEncode(const std::string& text) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(text.size() * 3);
    for (const unsigned char c : text) {
        if (std::isalnum(c) != 0 || c == '-' || c == '.' || c == '_' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

HttpResponse httpRequest(const HttpRequest& request, const std::atomic<bool>* cancel) {
    HttpResponse response;
    const auto start = std::chrono::steady_clock::now();
    const auto finish = [&](std::string error) {
        response.error = std::move(error);
        response.ok = false;
        response.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        return response;
    };
    const auto cancelled = [&] { return cancel != nullptr && cancel->load(); };

    UrlParts url;
    std::string error;
    if (!crack(request.url, url, &error)) return finish(error);
    const std::string method = request.method.empty() ? std::string("GET") : request.method;
    if (!validMethod(method)) return finish("Metodo no valido: " + method);
    std::wstring headers;
    for (const auto& [name, value] : request.headers) {
        if (!validHeader(name, value)) return finish("Cabecera no valida (sin saltos de linea ni caracteres raros): " + name);
        headers += widen(name + ": " + value + "\r\n");
    }

    Handle session(WinHttpOpen(L"Cramion", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                               WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) return finish(winError("No se pudo iniciar WinHTTP"));
    const int timeout = std::clamp(request.timeout_ms, 1000, 300000);
    WinHttpSetTimeouts(session.h, timeout, timeout, timeout, timeout);
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
    protocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
    WinHttpSetOption(session.h, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
    DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    WinHttpSetOption(session.h, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect));
#ifdef WINHTTP_OPTION_DECOMPRESSION
    DWORD decompression = WINHTTP_DECOMPRESSION_FLAG_ALL;
    WinHttpSetOption(session.h, WINHTTP_OPTION_DECOMPRESSION, &decompression, sizeof(decompression));
#endif

    Handle connection(WinHttpConnect(session.h, url.host.c_str(), url.port, 0));
    if (!connection) return finish(winError("No se pudo conectar"));
    const HINTERNET req = WinHttpOpenRequest(connection.h, widen(method).c_str(), url.path.c_str(), nullptr,
                                             WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                             url.secure ? WINHTTP_FLAG_SECURE : 0);
    if (req == nullptr) return finish(winError("No se pudo crear la peticion"));

    // Vigilante: los tiempos de WinHTTP no cubren todas las esperas (un
    // servidor que acepta y no contesta), asi que un hilo cierra la peticion
    // al pasar el tiempo total o al cancelarla; la llamada bloqueada vuelve
    // con error. Cerrar el handle desde otro hilo es la forma documentada de
    // cortar una peticion sincrona.
    struct Watchdog {
        std::mutex mutex;
        std::condition_variable wake;
        bool done = false;
        std::atomic<bool> fired{false};
        bool timed_out = false;
        std::thread thread;
        HINTERNET handle = nullptr;
        ~Watchdog() {
            {
                std::lock_guard<std::mutex> lock(mutex);
                done = true;
            }
            wake.notify_all();
            if (thread.joinable()) thread.join();
            if (!fired) WinHttpCloseHandle(handle);
        }
    } watchdog;
    watchdog.handle = req;
    const auto deadline = start + std::chrono::milliseconds(timeout);
    watchdog.thread = std::thread([&watchdog, deadline, cancel] {
        std::unique_lock<std::mutex> lock(watchdog.mutex);
        while (!watchdog.done) {
            watchdog.wake.wait_for(lock, std::chrono::milliseconds(50));
            if (watchdog.done) break;
            const bool late = std::chrono::steady_clock::now() >= deadline;
            if (late || (cancel != nullptr && cancel->load())) {
                watchdog.timed_out = late;
                watchdog.fired = true;
                WinHttpCloseHandle(watchdog.handle);
                break;
            }
        }
    });
    const auto cut = [&](const std::string& fallback) {
        if (watchdog.fired) {
            std::lock_guard<std::mutex> lock(watchdog.mutex);
            return finish(watchdog.timed_out ? "El servidor no responde (tiempo agotado: " + std::to_string(timeout / 1000) + " s)"
                                             : std::string("Peticion cancelada"));
        }
        return finish(fallback);
    };

    // La espera de las cabeceras de la respuesta tiene su propio tiempo (90 s
    // por defecto): el mismo que el resto.
    DWORD response_timeout = static_cast<DWORD>(timeout);
    WinHttpSetOption(req, WINHTTP_OPTION_RECEIVE_RESPONSE_TIMEOUT, &response_timeout, sizeof(response_timeout));
    if (!url.secure) {
        // http a localhost: sin redirecciones (podrian llevar fuera, sin cifrar).
        DWORD disable = WINHTTP_DISABLE_REDIRECTS;
        WinHttpSetOption(req, WINHTTP_OPTION_DISABLE_FEATURE, &disable, sizeof(disable));
    }
    if (cancelled()) return finish("Peticion cancelada");

    const DWORD body_size = static_cast<DWORD>(request.body.size());
    if (!WinHttpSendRequest(req, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                            headers.empty() ? 0 : static_cast<DWORD>(-1),
                            body_size > 0 ? const_cast<char*>(request.body.data()) : WINHTTP_NO_REQUEST_DATA, body_size,
                            body_size, 0) ||
        watchdog.fired || !WinHttpReceiveResponse(req, nullptr)) {
        return cut(winError("La peticion fallo"));
    }
    if (watchdog.fired) return cut("");

    DWORD status = 0;
    DWORD size = sizeof(status);
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &size, WINHTTP_NO_HEADER_INDEX);
    response.status = static_cast<int>(status);
    DWORD raw_size = 0;
    WinHttpQueryHeaders(req, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER,
                        &raw_size, WINHTTP_NO_HEADER_INDEX);
    if (raw_size > 0) {
        std::wstring raw(raw_size / sizeof(wchar_t), L'\0');
        if (WinHttpQueryHeaders(req, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, raw.data(),
                                &raw_size, WINHTTP_NO_HEADER_INDEX)) {
            raw.resize(raw_size / sizeof(wchar_t));
            response.headers = parseHeaders(raw);
        }
    }

    std::vector<char> buffer(1 << 16);
    for (;;) {
        if (watchdog.fired) return cut("");
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(req, &available)) return cut(winError("Se corto la respuesta"));
        if (available == 0) break;
        DWORD read = 0;
        const DWORD chunk = std::min<DWORD>(available, static_cast<DWORD>(buffer.size()));
        if (!WinHttpReadData(req, buffer.data(), chunk, &read)) return cut(winError("Se corto la respuesta"));
        if (read == 0) break;
        if (response.body.size() + read > request.max_response_bytes) {
            return finish("La respuesta pasa del maximo (" + std::to_string(request.max_response_bytes / 1024) + " KB)");
        }
        response.body.append(buffer.data(), read);
    }
    response.ok = response.status >= 200 && response.status < 300;
    response.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    return response;
}

// --- HttpClient ---------------------------------------------------------------------

struct HttpClient::Shared {
    std::mutex mutex;
    std::deque<std::pair<std::uint32_t, HttpRequest>> queued;
    std::vector<std::pair<std::uint32_t, HttpResponse>> done;
    int running = 0;
    int max_parallel = 6;
    std::uint32_t next_id = 1;
    std::uint64_t generation = 0;
    std::shared_ptr<std::atomic<bool>> cancel = std::make_shared<std::atomic<bool>>(false);
};

HttpClient::HttpClient(int max_parallel) : shared_(std::make_shared<Shared>()) {
    shared_->max_parallel = std::max(1, max_parallel);
}

HttpClient::~HttpClient() { cancelAll(); }

std::uint32_t HttpClient::send(HttpRequest request) {
    std::uint32_t id = 0;
    {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        id = shared_->next_id++;
        shared_->queued.emplace_back(id, std::move(request));
    }
    startQueued();
    return id;
}

void HttpClient::startQueued() {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    while (shared_->running < shared_->max_parallel && !shared_->queued.empty()) {
        auto job = std::move(shared_->queued.front());
        shared_->queued.pop_front();
        ++shared_->running;
        // El hilo solo toca `Shared` (compartido): puede terminar despues de
        // que se destruya el cliente sin problema.
        std::thread([shared = shared_, cancel = shared_->cancel, generation = shared_->generation,
                     job = std::move(job)]() mutable {
            HttpResponse response = httpRequest(job.second, cancel.get());
            std::lock_guard<std::mutex> done_lock(shared->mutex);
            --shared->running;
            if (generation == shared->generation) shared->done.emplace_back(job.first, std::move(response));
        }).detach();
    }
}

std::vector<std::pair<std::uint32_t, HttpResponse>> HttpClient::poll() {
    std::vector<std::pair<std::uint32_t, HttpResponse>> out;
    {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        out.swap(shared_->done);
    }
    startQueued();
    return out;
}

void HttpClient::cancelAll() {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    shared_->queued.clear();
    shared_->done.clear();
    shared_->cancel->store(true);
    shared_->cancel = std::make_shared<std::atomic<bool>>(false);
    ++shared_->generation;
}

int HttpClient::pending() const {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    return static_cast<int>(shared_->queued.size()) + shared_->running;
}

}  // namespace cramion::net
