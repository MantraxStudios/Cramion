#ifndef CRAMION_CORE_NET_WS_ENGINE_H
#define CRAMION_CORE_NET_WS_ENGINE_H

// Un intento de conexion WebSocket. Lo usa WebSocket.cpp; lo implementan
// WsEngineWinHttp.cpp (Windows) y WsEngineRfc.cpp (todas).

#include "CramionCore/net/Http.h"
#include "WsStream.h"

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace cramion::net::ws {

struct WsUrl {
    bool secure = false;
    std::string host;  // sin corchetes en IPv6
    int port = 0;
    std::string path = "/";  // con la query
};
// ws:// o wss://, sin usuario:clave@, puerto 1-65535. En WebSocket.cpp.
bool parseWsUrl(const std::string& url, WsUrl& out, std::string* error);

struct ConnectInfo {
    WsUrl url;
    HttpHeaders headers;  // ya validadas (sin las reservadas)
    std::vector<std::string> protocols;
    int timeout_ms = 10000;
    AddrPolicy policy = AddrPolicy::Any;
    std::size_t max_message_bytes = 4u << 20;
    int keepalive_ms = 20000;  // WinHTTP: WEB_SOCKET_KEEPALIVE_INTERVAL (>= 15000)
};

struct Failure {
    std::string text;  // espanol
    int http_status = 0;
    bool fatal = false;  // no reconectar
};

struct RxItem {
    enum class Kind : unsigned char { Message, Ping, Pong, PeerClose, ProtocolError, Ended, Failed, Aborted };
    Kind kind = Kind::Failed;
    std::string data;  // Message / Ping / Pong: payload; PeerClose: motivo
    bool binary = false;
    int code = 0;      // PeerClose: codigo (1005 sin codigo); ProtocolError: el que hay que mandar (1002/1007/1009)
    Failure failure;   // ProtocolError / Failed
};

class Engine {
public:
    virtual ~Engine() = default;
    // Hilo de sesion. Bloquea hasta abrir, fallar o abort().
    virtual bool connect(const ConnectInfo& info, std::string& protocol, Failure& failure) = 0;
    // Hilo lector. Tras ProtocolError se sigue llamando hasta PeerClose/Ended/Failed/Aborted.
    virtual RxItem receive() = 0;
    // Hilo de sesion (uno a la vez).
    virtual bool sendMessage(std::string_view data, bool binary, std::string& error) = 0;
    virtual bool sendPing(std::string_view payload, std::string& error) = 0;  // solo si supportsPing()
    virtual bool sendPong(std::string_view payload, std::string& error) = 0;
    virtual bool sendClose(int code, std::string_view reason, std::string& error) = 0;  // una vez
    // Cualquier hilo, idempotente: desbloquea connect/receive/send.
    virtual void abort() = 0;
    virtual bool supportsPing() const = 0;  // RFC true, WinHTTP false
    virtual bool waitsForEof() const = 0;   // RFC true (tras el cierre espera a que el servidor corte TCP), WinHTTP false
};

std::shared_ptr<Engine> makeRfcEngine();  // WsEngineRfc.cpp (todas las plataformas)
#if defined(_WIN32)
std::shared_ptr<Engine> makeWinHttpEngine();  // WsEngineWinHttp.cpp
#endif

}  // namespace cramion::net::ws

#endif
