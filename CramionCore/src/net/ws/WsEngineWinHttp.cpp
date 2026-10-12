// Motor WebSocket de Windows: WinHTTP sincrono (WinHttpWebSocket*). Un envio y
// una recepcion pueden ir a la vez en hilos distintos; abort() cierra los
// handles (la forma documentada de cortar una llamada sincrona bloqueada).

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include "../NetCommon.h"
#include "../WinHttpCommon.h"
#include "CramionCore/net/Encoding.h"
#include "WsEngine.h"
#include "WsProtocol.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#endif
#ifndef WINHTTP_OPTION_WEB_SOCKET_KEEPALIVE_INTERVAL
#define WINHTTP_OPTION_WEB_SOCKET_KEEPALIVE_INTERVAL 116
#endif

namespace cramion::net::ws {

// En WebSocket.cpp: copia de net.WsLog (se pone en el hilo principal) y una
// linea de consola segura desde cualquier hilo.
int wsLogLevel();
void wsLogLine(const std::string& line);

namespace {

using detail::widen;
using detail::winError;

void winsockOnce() {
    static const bool ready = [] {
        WSADATA data{};
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;  // nunca WSACleanup
    }();
    (void)ready;
}

std::string ipText(const sockaddr* addr) {
    char buffer[64] = {};
    if (addr->sa_family == AF_INET) {
        const auto* in = reinterpret_cast<const sockaddr_in*>(addr);
        const auto* b = reinterpret_cast<const unsigned char*>(&in->sin_addr);
        std::snprintf(buffer, sizeof(buffer), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
        return buffer;
    }
    if (addr->sa_family == AF_INET6) {
        const auto* in6 = reinterpret_cast<const sockaddr_in6*>(addr);
        const auto* b = reinterpret_cast<const unsigned char*>(&in6->sin6_addr);
        std::string out;
        for (int i = 0; i < 8; ++i) {
            std::snprintf(buffer, sizeof(buffer), "%x", (b[i * 2] << 8) | b[i * 2 + 1]);
            if (i > 0) out += ':';
            out += buffer;
        }
        return out;
    }
    return {};
}

// net.WsInsecure = 1: todas las direcciones del nombre tienen que ser locales.
bool checkPrivate(const std::string& host, int port, Failure& failure) {
    winsockOnce();
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* list = nullptr;
    const std::string service = std::to_string(port);
    if (getaddrinfo(host.c_str(), service.c_str(), &hints, &list) != 0 || list == nullptr) {
        failure.text = detail::kErrNoHost;
        return false;
    }
    bool ok = true;
    for (addrinfo* a = list; a != nullptr; a = a->ai_next) {
        if (a->ai_addr == nullptr || !detail::isPrivateIp(ipText(a->ai_addr))) ok = false;
    }
    freeaddrinfo(list);
    if (!ok) {
        failure.text = detail::kErrNotLocal;
        failure.fatal = true;
    }
    return ok;
}

bool isSecureError(DWORD code) {
    switch (code) {
        case ERROR_WINHTTP_SECURE_FAILURE:
        case ERROR_WINHTTP_SECURE_INVALID_CERT:
        case ERROR_WINHTTP_SECURE_CERT_CN_INVALID:
        case ERROR_WINHTTP_SECURE_CERT_DATE_INVALID:
        case ERROR_WINHTTP_SECURE_INVALID_CA:
        case ERROR_WINHTTP_SECURE_CERT_REV_FAILED:
        case ERROR_WINHTTP_SECURE_CERT_REVOKED:
        case ERROR_WINHTTP_SECURE_CERT_WRONG_USAGE:
        case ERROR_WINHTTP_SECURE_CHANNEL_ERROR:
            return true;
        default:
            return false;
    }
}

// Texto del error de conexion (los de NetCommon, sin prefijo).
std::string connectErrorText(DWORD code) {
    switch (code) {
        case ERROR_WINHTTP_NAME_NOT_RESOLVED: return detail::kErrNoHost;
        case ERROR_WINHTTP_TIMEOUT: return detail::kErrTimeout;
        case ERROR_WINHTTP_CANNOT_CONNECT: return detail::kErrRefused;
        case ERROR_WINHTTP_CONNECTION_ERROR: return detail::kErrCut;
        default: break;
    }
    if (isSecureError(code)) return detail::kErrCert;
    SetLastError(code);
    return winError("no se pudo conectar");
}

class WinHttpEngine final : public Engine {
public:
    ~WinHttpEngine() override { closeHandles(); }

    bool connect(const ConnectInfo& info, std::string& protocol, Failure& failure) override {
        max_message_ = info.max_message_bytes;
        const auto aborted = [&] {
            if (aborted_.load()) {
                failure = Failure{};
                return true;
            }
            return false;
        };
        const auto fail_last = [&] {
            const DWORD code = GetLastError();
            if (aborted() || code == ERROR_WINHTTP_OPERATION_CANCELLED || code == ERROR_INVALID_HANDLE) {
                failure.text = aborted_.load() ? std::string() : connectErrorText(code);
                return false;
            }
            failure.text = connectErrorText(code);
            failure.fatal = isSecureError(code);
            return false;
        };
        const auto gaveUp = [&] {
            aborted();
            return false;
        };
        if (aborted()) return false;

        if (info.policy == AddrPolicy::PrivateOnly && !detail::isLoopbackHost(info.url.host)) {
            if (!checkPrivate(info.url.host, info.url.port, failure)) return false;
            if (aborted()) return false;
        }

        // ws:// sin cifrar a este PC o a la red local: directo, nunca por el proxy
        // del sistema (la comprobacion de la direccion no valdria nada).
        const bool direct = info.policy != AddrPolicy::Any;
        HINTERNET session = WinHttpOpen(L"Cramion", direct ? WINHTTP_ACCESS_TYPE_NO_PROXY : WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (session == nullptr && !direct && GetLastError() == ERROR_INVALID_PARAMETER) {
            // Antes de Windows 8.1 no existe AUTOMATIC_PROXY.
            session = WinHttpOpen(L"Cramion", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                  WINHTTP_NO_PROXY_BYPASS, 0);
        }
        if (session == nullptr) {
            failure.text = winError("no se pudo iniciar WinHTTP");
            return false;
        }
        if (!publish(session_, session)) return gaveUp();

        const int t = std::clamp(info.timeout_ms, 1000, 300000);
        WinHttpSetTimeouts(session, t, t, t, t);
        DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
        protocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
        if (!WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols))) {
            // Windows sin TLS 1.3 rechaza la opcion entera: al menos TLS 1.2.
            protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
            WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
        }
        DWORD keepalive = static_cast<DWORD>(std::max(15000, info.keepalive_ms));
        const bool keepalive_on_session =
            WinHttpSetOption(session, WINHTTP_OPTION_WEB_SOCKET_KEEPALIVE_INTERVAL, &keepalive, sizeof(keepalive)) != FALSE;

        // IPv6 literal: WinHttpConnect lo quiere entre corchetes (tambien para Host:).
        const std::string server =
            info.url.host.find(':') != std::string::npos ? "[" + info.url.host + "]" : info.url.host;
        HINTERNET connection =
            WinHttpConnect(session, widen(server).c_str(), static_cast<INTERNET_PORT>(info.url.port), 0);
        if (connection == nullptr) return fail_last();
        if (!publish(connect_, connection)) return gaveUp();

        HINTERNET request = WinHttpOpenRequest(connection, L"GET", widen(info.url.path).c_str(), nullptr, WINHTTP_NO_REFERER,
                                               WINHTTP_DEFAULT_ACCEPT_TYPES, info.url.secure ? WINHTTP_FLAG_SECURE : 0);
        if (request == nullptr) return fail_last();
        if (!publish(request_, request)) return gaveUp();

        if (!keepalive_on_session &&
            !WinHttpSetOption(request, WINHTTP_OPTION_WEB_SOCKET_KEEPALIVE_INTERVAL, &keepalive, sizeof(keepalive))) {
            static std::once_flag warned;
            if (wsLogLevel() >= 1) {
                std::call_once(warned, [] { wsLogLine("[WebSocket] WinHTTP no acepta el intervalo de keepalive"); });
            }
        }
        if (!WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0)) return fail_last();
        DWORD disable = WINHTTP_DISABLE_REDIRECTS;
        WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE, &disable, sizeof(disable));

        std::wstring headers;
        for (const auto& [name, value] : info.headers) headers += widen(name + ": " + value + "\r\n");
        if (!info.protocols.empty()) {
            std::string list;
            for (const auto& p : info.protocols) {
                if (!list.empty()) list += ", ";
                list += p;
            }
            headers += widen("Sec-WebSocket-Protocol: " + list + "\r\n");
        }
        if (aborted()) return false;
        if (!WinHttpSendRequest(request, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                                headers.empty() ? 0 : static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
            return fail_last();
        }
        if (!WinHttpReceiveResponse(request, nullptr)) return fail_last();
        if (aborted()) return false;

        DWORD status = 0;
        DWORD size = sizeof(status);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &status, &size, WINHTTP_NO_HEADER_INDEX);
        if (status != 101) {
            const int code = static_cast<int>(status);
            failure.http_status = code;
            if (code == 401 || code == 403) {
                failure.text = "el servidor rechazo la conexion (HTTP " + std::to_string(code) + "): revisa el token o la sesion";
                failure.fatal = true;
            } else {
                failure.text = "el servidor no acepta WebSocket en esa ruta (respondio HTTP " + std::to_string(code) + ")";
                failure.fatal = !(code >= 500 || code == 408 || code == 429);
            }
            return false;
        }

        std::string chosen;
        const bool has_protocol = queryCustom(request, L"Sec-WebSocket-Protocol", chosen);
        std::string extensions;
        if (queryCustom(request, L"Sec-WebSocket-Extensions", extensions) && !extensions.empty()) {
            failure.text = "el servidor pidio extensiones no soportadas (compresion)";
            failure.fatal = true;
            return false;
        }
        if (has_protocol && !chosen.empty()) {
            if (std::find(info.protocols.begin(), info.protocols.end(), chosen) == info.protocols.end()) {
                failure.text = "el servidor eligio un subprotocolo que no se pidio (" + chosen + ")";
                failure.fatal = true;
                return false;
            }
        } else if (!info.protocols.empty()) {
            failure.text = "el servidor no acepto ningun subprotocolo";
            failure.fatal = true;
            return false;
        }

        // La espera de datos la vigila el watchdog (idle), no WinHTTP.
        DWORD zero = 0;
        WinHttpSetOption(request, WINHTTP_OPTION_RECEIVE_TIMEOUT, &zero, sizeof(zero));
        HINTERNET socket = WinHttpWebSocketCompleteUpgrade(request, 0);
        if (socket == nullptr) {
            if (aborted()) return false;
            failure.text = "respuesta del servidor no valida (no es un saludo WebSocket correcto)";
            failure.fatal = true;
            return false;
        }
        if (!publish(ws_, socket)) return gaveUp();
        WinHttpSetOption(socket, WINHTTP_OPTION_RECEIVE_TIMEOUT, &zero, sizeof(zero));
        WinHttpSetOption(socket, WINHTTP_OPTION_SEND_TIMEOUT, &zero, sizeof(zero));  // lo vigila el watchdog
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (request_ != nullptr) {
                WinHttpCloseHandle(request_);
                request_ = nullptr;
            }
        }
        protocol = has_protocol ? chosen : std::string();
        return !aborted_.load();
    }

    RxItem receive() override {
        RxItem item;
        if (peer_closed_) {
            // Tras el cierre del servidor solo queda esperar a que la sesion corte.
            std::unique_lock<std::mutex> lock(mu_);
            cv_.wait(lock, [&] { return aborted_.load(); });
            item.kind = RxItem::Kind::Ended;
            return item;
        }
        if (buffer_.empty()) buffer_.resize(64u << 10);
        for (;;) {
            HINTERNET socket = currentSocket();
            if (socket == nullptr || aborted_.load()) {
                item.kind = RxItem::Kind::Aborted;
                return item;
            }
            DWORD n = 0;
            WINHTTP_WEB_SOCKET_BUFFER_TYPE type{};
            const DWORD e = WinHttpWebSocketReceive(socket, buffer_.data(), static_cast<DWORD>(buffer_.size()), &n, &type);
            if (e != NO_ERROR) {
                if (aborted_.load()) {
                    item.kind = RxItem::Kind::Aborted;
                    return item;
                }
                item.kind = RxItem::Kind::Failed;
                item.failure.text = detail::kErrCut;
                if (wsLogLevel() >= 1) {
                    SetLastError(e);
                    item.failure.text += " (" + winError("WinHTTP") + ")";
                }
                return item;
            }
            if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
                USHORT code = 0;
                char reason[WINHTTP_WEB_SOCKET_MAX_CLOSE_REASON_LENGTH] = {};
                DWORD length = 0;
                item.kind = RxItem::Kind::PeerClose;
                if (WinHttpWebSocketQueryCloseStatus(socket, &code, reason, sizeof(reason), &length) == NO_ERROR) {
                    item.code = code;
                    item.data.assign(reason, std::min<DWORD>(length, sizeof(reason)));
                } else {
                    item.code = 1005;
                }
                peer_closed_ = true;
                return item;
            }
            const bool binary = type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE ||
                                type == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE;
            const bool last = type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE ||
                              type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE;
            if (discarding_) {
                // Resto de un mensaje demasiado grande: se tira.
                if (last) discarding_ = false;
                continue;
            }
            if (msg_.size() + n > max_message_) {
                msg_.clear();
                msg_.shrink_to_fit();
                discarding_ = !last;
                item.kind = RxItem::Kind::ProtocolError;
                item.code = 1009;
                item.failure.text = "mensaje demasiado grande (mas de " + std::to_string(max_message_) + " bytes, maxSize)";
                return item;
            }
            msg_.append(buffer_.data(), n);
            if (!last) continue;
            item.binary = binary;
            item.data.swap(msg_);
            msg_.clear();
            if (!binary && !net::validUtf8(item.data)) {
                item.data.clear();
                item.kind = RxItem::Kind::ProtocolError;
                item.code = 1007;
                item.failure.text = "el servidor mando texto que no es UTF-8 valido";
                return item;
            }
            item.kind = RxItem::Kind::Message;
            return item;
        }
    }

    bool sendMessage(std::string_view data, bool binary, std::string& error) override {
        HINTERNET socket = currentSocket();
        if (socket == nullptr || aborted_.load()) {
            error = detail::kErrCut;
            return false;
        }
        const DWORD e = WinHttpWebSocketSend(socket,
                                             binary ? WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE
                                                    : WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                                             data.empty() ? nullptr : const_cast<char*>(data.data()),
                                             static_cast<DWORD>(data.size()));
        if (e != NO_ERROR) {
            error = sendErrorText(e);
            return false;
        }
        return true;
    }

    // WinHTTP contesta los ping solo y no deja mandarlos (usa keepalive).
    bool sendPing(std::string_view, std::string&) override { return true; }
    bool sendPong(std::string_view, std::string&) override { return true; }

    bool sendClose(int code, std::string_view reason, std::string& error) override {
        HINTERNET socket = currentSocket();
        if (socket == nullptr || aborted_.load()) {
            error = detail::kErrCut;
            return false;
        }
        USHORT status = static_cast<USHORT>(code);
        std::string_view why = reason;
        if (code == 0) {
            status = WINHTTP_WEB_SOCKET_EMPTY_CLOSE_STATUS;  // 1005: sin codigo
            why = {};
        }
        if (why.size() > kMaxCloseReason) why = why.substr(0, kMaxCloseReason);
        const DWORD e = WinHttpWebSocketShutdown(socket, status, why.empty() ? nullptr : const_cast<char*>(why.data()),
                                                 static_cast<DWORD>(why.size()));
        if (e != NO_ERROR) {
            error = sendErrorText(e);
            return false;
        }
        return true;
    }

    void abort() override {
        if (aborted_.exchange(true)) return;
        closeHandles();
        cv_.notify_all();
    }

    bool supportsPing() const override { return false; }
    bool waitsForEof() const override { return false; }

private:
    // Guarda el handle; si ya se aborto, lo cierra y devuelve false.
    bool publish(HINTERNET& slot, HINTERNET handle) {
        std::lock_guard<std::mutex> lock(mu_);
        if (aborted_.load()) {
            WinHttpCloseHandle(handle);
            return false;
        }
        slot = handle;
        return true;
    }

    HINTERNET currentSocket() {
        std::lock_guard<std::mutex> lock(mu_);
        return ws_;
    }

    void closeHandles() {
        std::lock_guard<std::mutex> lock(mu_);
        for (HINTERNET* h : {&ws_, &request_, &connect_, &session_}) {
            if (*h != nullptr) {
                WinHttpCloseHandle(*h);
                *h = nullptr;
            }
        }
    }

    std::string sendErrorText(DWORD e) {
        if (aborted_.load()) return detail::kErrCut;
        std::string text = detail::kErrCut;
        if (wsLogLevel() >= 1) {
            SetLastError(e);
            text += " (" + winError("WinHTTP") + ")";
        }
        return text;
    }

    // Cabecera de la respuesta por nombre. false si no esta.
    static bool queryCustom(HINTERNET request, const wchar_t* name, std::string& out) {
        out.clear();
        DWORD size = 0;
        if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CUSTOM, name, WINHTTP_NO_OUTPUT_BUFFER, &size,
                                WINHTTP_NO_HEADER_INDEX)) {
            return true;  // vacia
        }
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0) return false;
        std::wstring value(size / sizeof(wchar_t) + 1, L'\0');
        DWORD bytes = static_cast<DWORD>(value.size() * sizeof(wchar_t));
        if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_CUSTOM, name, value.data(), &bytes, WINHTTP_NO_HEADER_INDEX)) {
            return false;
        }
        value.resize(bytes / sizeof(wchar_t));
        out = detail::narrow(value);
        const std::size_t first = out.find_first_not_of(" \t");
        const std::size_t last = out.find_last_not_of(" \t");
        out = first == std::string::npos ? std::string() : out.substr(first, last - first + 1);
        return true;
    }

    std::mutex mu_;
    std::condition_variable cv_;
    std::atomic<bool> aborted_{false};
    HINTERNET session_ = nullptr;
    HINTERNET connect_ = nullptr;
    HINTERNET request_ = nullptr;
    HINTERNET ws_ = nullptr;
    // Solo el hilo lector.
    std::vector<char> buffer_;
    std::string msg_;
    bool discarding_ = false;
    bool peer_closed_ = false;
    std::size_t max_message_ = 4u << 20;
};

}  // namespace

std::shared_ptr<Engine> makeWinHttpEngine() { return std::make_shared<WinHttpEngine>(); }

}  // namespace cramion::net::ws

#endif  // _WIN32
