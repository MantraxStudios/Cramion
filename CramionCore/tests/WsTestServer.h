#ifndef CRAMION_TESTS_WS_TEST_SERVER_H
#define CRAMION_TESTS_WS_TEST_SERVER_H

// Servidor WebSocket de prueba (solo para las pruebas, header-only): escucha en
// 127.0.0.1 con un puerto libre, un hilo por conexion, y apunta lo que ve
// (cabeceras de cada peticion, mensajes, codigos de cierre, pongs). Exige que
// el cliente mande las tramas con mascara: una sin mascara cuenta como fallo
// (unmaskedFrames). El Sec-WebSocket-Accept sale de net::sha1 + base64Encode y,
// en Windows, se compara con el SHA-1 de BCrypt (acceptMismatches).
//
// Rutas:
//   /eco          eco de texto y binario; responde a los ping
//   /frag         manda "uno dos tres" en 3 trozos con un ping "pp" en medio
//   /cerrar4001   cierra con 4001 "baneado"
//   /cortar       corta el TCP a los 200 ms sin trama de cierre
//   /401          responde HTTP 401
//   /proto        elige el primer subprotocolo pedido
//   /proto?mal    elige uno que no se pidio ("otro")
//   /cabeceras    manda las cabeceras recibidas como JSON (nombres en minusculas)
//   /lento        nunca responde al saludo
//   /mudo         tras el saludo no manda nada (ni pong)
//   /grande?n=N   manda un mensaje binario de N bytes
//   /rafaga       manda 1000 mensajes de texto numerados ("0000...", 1000 bytes cada uno)
// Todas (menos /lento, /401, /cortar y /mudo) siguen despues como /eco.
//
// En el CMake de las pruebas va antes que cualquier cabecera que meta
// <windows.h> (winsock2.h tiene que ir primero).

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include "CramionCore/net/Encoding.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace cramion::wstest {

#if defined(_WIN32)
using Socket = SOCKET;
inline constexpr Socket kNoSocket = INVALID_SOCKET;
inline void closeSocket(Socket s) { closesocket(s); }
inline void shutdownBoth(Socket s) { shutdown(s, SD_BOTH); }
inline void shutdownSend(Socket s) { shutdown(s, SD_SEND); }
inline constexpr int kSendFlags = 0;
#else
using Socket = int;
inline constexpr Socket kNoSocket = -1;
inline void closeSocket(Socket s) { ::close(s); }
inline void shutdownBoth(Socket s) { ::shutdown(s, SHUT_RDWR); }
inline void shutdownSend(Socket s) { ::shutdown(s, SHUT_WR); }
inline constexpr int kSendFlags = MSG_NOSIGNAL;
#endif

inline constexpr const char* kWsGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

// Trama del servidor (sin mascara).
inline std::string serverFrame(int opcode, std::string_view payload, bool fin = true) {
    std::string out;
    out += static_cast<char>((fin ? 0x80 : 0x00) | (opcode & 0x0F));
    const std::uint64_t n = payload.size();
    if (n < 126) {
        out += static_cast<char>(n);
    } else if (n <= 0xFFFF) {
        out += static_cast<char>(126);
        out += static_cast<char>((n >> 8) & 0xFF);
        out += static_cast<char>(n & 0xFF);
    } else {
        out += static_cast<char>(127);
        for (int i = 7; i >= 0; --i) out += static_cast<char>((n >> (8 * i)) & 0xFF);
    }
    out.append(payload.data(), payload.size());
    return out;
}

inline std::string serverClose(int code, std::string_view reason = {}) {
    std::string payload;
    if (code != 0) {
        payload += static_cast<char>((code >> 8) & 0xFF);
        payload += static_cast<char>(code & 0xFF);
        payload.append(reason.data(), reason.size());
    }
    return serverFrame(0x8, payload);
}

inline std::string acceptKey(const std::string& key) {
    const std::array<std::uint8_t, 20> digest = net::sha1(key + kWsGuid);
    return net::base64Encode(std::string_view(reinterpret_cast<const char*>(digest.data()), digest.size()));
}

#if defined(_WIN32)
// El mismo SHA-1 con BCrypt (para comprobar net::sha1 contra el del sistema).
inline bool bcryptSha1(std::string_view data, std::array<std::uint8_t, 20>& out) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = false;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA1_ALGORITHM, nullptr, 0) == 0) {
        if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
            ok = BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())),
                                static_cast<ULONG>(data.size()), 0) == 0 &&
                 BCryptFinishHash(hash, out.data(), static_cast<ULONG>(out.size()), 0) == 0;
            BCryptDestroyHash(hash);
        }
        BCryptCloseAlgorithmProvider(algorithm, 0);
    }
    return ok;
}
#endif

class WsTestServer {
public:
    WsTestServer() {
#if defined(_WIN32)
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);  // sin WSACleanup: el motor tambien usa winsock
#endif
        listener_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (listener_ == kNoSocket || ::bind(listener_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            std::printf("WsTestServer: no se pudo abrir el puerto\n");
            return;
        }
        socklen_t len = sizeof(addr);
        getsockname(listener_, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ntohs(addr.sin_port);
        ::listen(listener_, 64);
        accept_thread_ = std::thread([this] { acceptLoop(); });
    }
    ~WsTestServer() { stop(); }
    WsTestServer(const WsTestServer&) = delete;
    WsTestServer& operator=(const WsTestServer&) = delete;

    int port() const { return port_; }
    std::string url(const std::string& path) const { return "ws://127.0.0.1:" + std::to_string(port_) + path; }

    // Cierra todo y espera a sus hilos (el destructor lo hace solo).
    void stop() {
        if (stop_.exchange(true)) return;
        if (listener_ != kNoSocket) {
            shutdownBoth(listener_);  // Linux: despierta accept
            closeSocket(listener_);   // Windows: despierta accept
        }
        if (accept_thread_.joinable()) accept_thread_.join();
        std::vector<std::thread> workers;
        {
            std::lock_guard<std::mutex> lock(mu_);
            for (const Socket s : live_) shutdownBoth(s);
            workers.swap(workers_);
        }
        for (std::thread& t : workers) {
            if (t.joinable()) t.join();
        }
    }

    // --- Lo que vio (copias, con el candado) ---
    // Conexiones TCP aceptadas con esa ruta (con la query); "" = todas.
    int connections(const std::string& target = {}) const {
        std::lock_guard<std::mutex> lock(rec_mu_);
        if (target.empty()) return total_connections_;
        const auto it = connections_.find(target);
        return it == connections_.end() ? 0 : it->second;
    }
    // Mensajes completos que mando el cliente (texto y binario).
    std::vector<std::string> messages() const {
        std::lock_guard<std::mutex> lock(rec_mu_);
        return messages_;
    }
    // Codigos de los cierres que mando el cliente (0 = cierre sin codigo).
    std::vector<int> closeCodes() const {
        std::lock_guard<std::mutex> lock(rec_mu_);
        return close_codes_;
    }
    bool sawClose(int code) const {
        const std::vector<int> codes = closeCodes();
        return std::find(codes.begin(), codes.end(), code) != codes.end();
    }
    // Contenido de los pong que mando el cliente.
    std::vector<std::string> pongs() const {
        std::lock_guard<std::mutex> lock(rec_mu_);
        return pongs_;
    }
    bool sawPong(const std::string& payload) const {
        const std::vector<std::string> list = pongs();
        return std::find(list.begin(), list.end(), payload) != list.end();
    }
    // La peticion entera (linea + cabeceras) de la ultima conexion a esa ruta.
    std::string lastRequest(const std::string& target) const {
        std::lock_guard<std::mutex> lock(rec_mu_);
        const auto it = last_request_.find(target);
        return it == last_request_.end() ? std::string() : it->second;
    }
    // Una cabecera (nombre en minusculas) de la ultima peticion a esa ruta.
    std::string lastHeader(const std::string& target, const std::string& lower_name) const {
        std::lock_guard<std::mutex> lock(rec_mu_);
        const auto it = last_headers_.find(target);
        if (it == last_headers_.end()) return {};
        const auto h = it->second.find(lower_name);
        return h == it->second.end() ? std::string() : h->second;
    }
    // Tramas del cliente sin mascara (debe ser 0).
    int unmaskedFrames() const {
        std::lock_guard<std::mutex> lock(rec_mu_);
        return unmasked_;
    }
    // Windows: saludos en que net::sha1 y BCrypt no dieron lo mismo (debe ser 0).
    int acceptMismatches() const {
        std::lock_guard<std::mutex> lock(rec_mu_);
        return accept_mismatches_;
    }
    // Olvida lo apuntado (entre bloques de pruebas).
    void clear() {
        std::lock_guard<std::mutex> lock(rec_mu_);
        connections_.clear();
        total_connections_ = 0;
        messages_.clear();
        close_codes_.clear();
        pongs_.clear();
        last_request_.clear();
        last_headers_.clear();
    }

private:
    struct Peer {
        Socket s = kNoSocket;
        std::string buf;  // leido y aun sin usar

        bool readMore() {
            char tmp[65536];
            const auto got = ::recv(s, tmp, static_cast<int>(sizeof(tmp)), 0);
            if (got <= 0) return false;
            buf.append(tmp, static_cast<std::size_t>(got));
            return true;
        }
        bool need(std::size_t n) {
            while (buf.size() < n) {
                if (!readMore()) return false;
            }
            return true;
        }
        bool sendAll(std::string_view data) {
            std::size_t sent = 0;
            while (sent < data.size()) {
                const std::size_t chunk = std::min<std::size_t>(data.size() - sent, std::size_t(1) << 20);
                const auto n = ::send(s, data.data() + sent, static_cast<int>(chunk), kSendFlags);
                if (n <= 0) return false;
                sent += static_cast<std::size_t>(n);
            }
            return true;
        }
    };

    struct Frame {
        bool fin = false;
        int opcode = 0;
        bool masked = false;
        std::string payload;
    };

    Socket listener_ = kNoSocket;
    int port_ = 0;
    std::atomic<bool> stop_{false};
    std::thread accept_thread_;

    std::mutex mu_;  // live_ y workers_
    std::vector<Socket> live_;
    std::vector<std::thread> workers_;

    mutable std::mutex rec_mu_;  // lo apuntado
    std::map<std::string, int> connections_;
    int total_connections_ = 0;
    std::vector<std::string> messages_;
    std::vector<int> close_codes_;
    std::vector<std::string> pongs_;
    std::map<std::string, std::string> last_request_;
    std::map<std::string, std::map<std::string, std::string>> last_headers_;
    int unmasked_ = 0;
    int accept_mismatches_ = 0;

    void acceptLoop() {
        while (!stop_) {
            const Socket c = ::accept(listener_, nullptr, nullptr);
            if (c == kNoSocket) break;
            std::lock_guard<std::mutex> lock(mu_);
            if (stop_) {
                closeSocket(c);
                break;
            }
            live_.push_back(c);
            workers_.emplace_back([this, c] { serve(c); });
        }
    }

    void serve(Socket c) {
        Peer peer;
        peer.s = c;
        handle(peer);
        std::lock_guard<std::mutex> lock(mu_);
        live_.erase(std::remove(live_.begin(), live_.end(), c), live_.end());
        shutdownSend(c);
        closeSocket(c);
    }

    // Espera ms (o hasta stop()).
    void pause(int ms) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (!stop_ && std::chrono::steady_clock::now() < end) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    static std::string lower(std::string s) {
        for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return s;
    }
    static std::string trim(const std::string& s) {
        std::size_t a = 0;
        std::size_t b = s.size();
        while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
        while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) --b;
        return s.substr(a, b - a);
    }
    static std::string jsonEscape(const std::string& s) {
        std::string out;
        for (const char ch : s) {
            if (ch == '"' || ch == '\\') {
                out += '\\';
                out += ch;
            } else if (static_cast<unsigned char>(ch) < 0x20) {
                char tmp[8];
                std::snprintf(tmp, sizeof(tmp), "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(ch)));
                out += tmp;
            } else {
                out += ch;
            }
        }
        return out;
    }

    // Una trama del cliente (false si se corto la conexion).
    static bool readFrame(Peer& p, Frame& f) {
        if (!p.need(2)) return false;
        const auto b0 = static_cast<std::uint8_t>(p.buf[0]);
        const auto b1 = static_cast<std::uint8_t>(p.buf[1]);
        f.fin = (b0 & 0x80) != 0;
        f.opcode = b0 & 0x0F;
        f.masked = (b1 & 0x80) != 0;
        std::uint64_t len = b1 & 0x7F;
        std::size_t at = 2;
        if (len == 126) {
            if (!p.need(4)) return false;
            len = (std::uint64_t(static_cast<std::uint8_t>(p.buf[2])) << 8) | static_cast<std::uint8_t>(p.buf[3]);
            at = 4;
        } else if (len == 127) {
            if (!p.need(10)) return false;
            len = 0;
            for (int i = 0; i < 8; ++i) len = (len << 8) | static_cast<std::uint8_t>(p.buf[2 + i]);
            at = 10;
        }
        if (len > (std::uint64_t(64) << 20)) return false;  // en las pruebas nada es tan grande
        std::uint8_t mask[4] = {0, 0, 0, 0};
        if (f.masked) {
            if (!p.need(at + 4)) return false;
            for (int i = 0; i < 4; ++i) mask[i] = static_cast<std::uint8_t>(p.buf[at + i]);
            at += 4;
        }
        const auto n = static_cast<std::size_t>(len);
        if (!p.need(at + n)) return false;
        f.payload.assign(p.buf, at, n);
        if (f.masked) {
            for (std::size_t i = 0; i < n; ++i) f.payload[i] = static_cast<char>(f.payload[i] ^ mask[i & 3]);
        }
        p.buf.erase(0, at + n);
        return true;
    }

    // Lee mensajes hasta el cierre o el corte. echo: devuelve los mensajes y
    // responde a los ping. we_closed: el servidor ya mando su cierre (no lo repite).
    void messageLoop(Peer& p, bool echo, bool we_closed = false) {
        std::string message;
        int message_op = 0;
        Frame f;
        while (!stop_ && readFrame(p, f)) {
            if (!f.masked) {
                {
                    std::lock_guard<std::mutex> lock(rec_mu_);
                    ++unmasked_;
                }
                p.sendAll(serverClose(1002, "sin mascara"));
                return;
            }
            if (f.opcode == 0x9) {  // ping
                if (echo) p.sendAll(serverFrame(0xA, f.payload));
                continue;
            }
            if (f.opcode == 0xA) {  // pong
                std::lock_guard<std::mutex> lock(rec_mu_);
                pongs_.push_back(f.payload);
                continue;
            }
            if (f.opcode == 0x8) {  // cierre
                int code = 0;
                std::string reason;
                if (f.payload.size() >= 2) {
                    code = (static_cast<std::uint8_t>(f.payload[0]) << 8) | static_cast<std::uint8_t>(f.payload[1]);
                    reason = f.payload.substr(2);
                }
                {
                    std::lock_guard<std::mutex> lock(rec_mu_);
                    close_codes_.push_back(code);
                }
                if (!we_closed) p.sendAll(serverClose(code, reason));  // eco del cierre (mismo codigo)
                return;  // el servidor corta el TCP (serve)
            }
            if (f.opcode == 0x1 || f.opcode == 0x2) {
                message_op = f.opcode;
                message = f.payload;
            } else if (f.opcode == 0x0) {
                message += f.payload;
            } else {
                p.sendAll(serverClose(1002, "opcode"));
                return;
            }
            if (f.fin) {
                {
                    std::lock_guard<std::mutex> lock(rec_mu_);
                    messages_.push_back(message);
                }
                if (echo && !we_closed && !p.sendAll(serverFrame(message_op, message))) return;
                message.clear();
            }
        }
    }

    void handle(Peer& p) {
        // Saludo: hasta "\r\n\r\n".
        std::size_t end = std::string::npos;
        while ((end = p.buf.find("\r\n\r\n")) == std::string::npos) {
            if (p.buf.size() > (16u << 10) || !p.readMore()) return;
        }
        const std::string head = p.buf.substr(0, end + 2);
        p.buf.erase(0, end + 4);
        const std::size_t line_end = head.find("\r\n");
        const std::string line = head.substr(0, line_end);
        const std::size_t sp1 = line.find(' ');
        const std::size_t sp2 = line.rfind(' ');
        const std::string target = (sp1 != std::string::npos && sp2 > sp1) ? line.substr(sp1 + 1, sp2 - sp1 - 1) : "/";
        std::map<std::string, std::string> headers;
        for (std::size_t at = line_end + 2; at < head.size();) {
            const std::size_t next = head.find("\r\n", at);
            const std::string h = head.substr(at, next - at);
            at = next == std::string::npos ? head.size() : next + 2;
            const std::size_t colon = h.find(':');
            if (colon == std::string::npos) continue;
            const std::string name = lower(trim(h.substr(0, colon)));
            const std::string value = trim(h.substr(colon + 1));
            std::string& slot = headers[name];
            slot = slot.empty() ? value : slot + ", " + value;
        }
        {
            std::lock_guard<std::mutex> lock(rec_mu_);
            ++connections_[target];
            ++total_connections_;
            last_request_[target] = head;
            last_headers_[target] = headers;
        }

        if (target == "/lento") {  // nunca responde: espera a que el cliente corte
            while (!stop_ && p.readMore()) p.buf.clear();
            return;
        }
        if (target == "/401") {
            p.sendAll("HTTP/1.1 401 Unauthorized\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            return;
        }
        const bool known = target == "/eco" || target == "/frag" || target == "/cerrar4001" || target == "/cortar" ||
                           target == "/proto" || target == "/proto?mal" || target == "/cabeceras" || target == "/mudo" ||
                           target == "/rafaga" || target.rfind("/grande?n=", 0) == 0;
        const std::string key = headers["sec-websocket-key"];
        if (!known || key.empty() || lower(headers["upgrade"]) != "websocket") {
            p.sendAll("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            return;
        }
        const std::string accept = acceptKey(key);
#if defined(_WIN32)
        std::array<std::uint8_t, 20> check{};
        if (!bcryptSha1(key + kWsGuid, check) ||
            net::base64Encode(std::string_view(reinterpret_cast<const char*>(check.data()), check.size())) != accept) {
            std::lock_guard<std::mutex> lock(rec_mu_);
            ++accept_mismatches_;
        }
#endif
        std::string response = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                                "Sec-WebSocket-Accept: " +
                                accept + "\r\n";
        if (target == "/proto") {
            const std::string requested = headers["sec-websocket-protocol"];
            const std::string first = trim(requested.substr(0, requested.find(',')));
            if (!first.empty()) response += "Sec-WebSocket-Protocol: " + first + "\r\n";
        } else if (target == "/proto?mal") {
            response += "Sec-WebSocket-Protocol: otro\r\n";
        }
        response += "\r\n";
        if (!p.sendAll(response)) return;

        if (target == "/frag") {
            p.sendAll(serverFrame(0x1, "uno ", false) + serverFrame(0x9, "pp") + serverFrame(0x0, "dos ", false) +
                      serverFrame(0x0, "tres", true));
            messageLoop(p, true);
        } else if (target == "/cerrar4001") {
            p.sendAll(serverClose(4001, "baneado"));
            messageLoop(p, false, true);
        } else if (target == "/cortar") {
            pause(200);  // y fuera, sin trama de cierre
        } else if (target == "/cabeceras") {
            std::string json = "{";
            bool first = true;
            for (const auto& [name, value] : headers) {
                if (!first) json += ",";
                first = false;
                json += "\"" + jsonEscape(name) + "\":\"" + jsonEscape(value) + "\"";
            }
            json += "}";
            p.sendAll(serverFrame(0x1, json));
            messageLoop(p, true);
        } else if (target == "/mudo") {
            messageLoop(p, false);
        } else if (target.rfind("/grande?n=", 0) == 0) {
            const std::size_t n = static_cast<std::size_t>(std::strtoull(target.c_str() + 10, nullptr, 10));
            p.sendAll(serverFrame(0x2, std::string(n, 'g')));
            messageLoop(p, true);
        } else if (target == "/rafaga") {
            for (int i = 0; i < 1000 && !stop_; ++i) {
                char number[8];
                std::snprintf(number, sizeof(number), "%04d", i);
                std::string text(1000, '.');
                text.replace(0, 4, number);
                if (!p.sendAll(serverFrame(0x1, text))) return;
            }
            messageLoop(p, true);
        } else {
            messageLoop(p, true);  // /eco, /proto
        }
    }
};

}  // namespace cramion::wstest

#endif  // CRAMION_TESTS_WS_TEST_SERVER_H
