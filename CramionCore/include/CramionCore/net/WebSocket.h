#ifndef CRAMION_CORE_NET_WEBSOCKET_H
#define CRAMION_CORE_NET_WEBSOCKET_H

// Conexiones WebSocket (ws:// y wss://) con el servidor propio del juego
// (backend de un MMO, chat, lobby). Conectar, enviar y cerrar nunca paran el
// juego: cada conexion tiene su hilo de sesion y su hilo lector; los eventos
// se recogen con poll() en el hilo principal.
//
// Seguridad (como Http):
//   - wss:// siempre (TLS 1.2+, certificado y nombre comprobados).
//   - ws:// solo a este PC salvo net.WsInsecure (1 = red local, 2 = cualquiera, solo con trucos).
//   - Cabeceras validadas (validHeader) y sin las reservadas del protocolo.
//   - Tamano maximo por mensaje (al llegar cada trozo) y colas con tope.
// Motores: Windows WinHTTP; Linux/Android motor propio (RFC 6455) sobre TCP,
// OpenSSL (Linux) o javax.net.ssl.SSLSocket (Android, por JNI; sin Java propio).

#include "CramionCore/net/Http.h"  // HttpHeaders, validHeader, setJavaVM

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace cramion::net {

inline constexpr std::size_t kWsHardMaxMessage = 64u << 20;     // ninguna opcion pasa de aqui
inline constexpr std::size_t kWsHardMaxQueue = 256u << 20;
inline constexpr std::size_t kWsMaxHeaders = 32;                // y 32 KB en total
inline constexpr std::size_t kWsMaxProtocols = 8;               // tokens RFC 7230 de <= 64

struct WsReconnect {
    bool enabled = true;
    int min_delay_ms = 500;
    int max_delay_ms = 30000;
    int max_tries = -1;         // -1 = sin limite (se reinicia al abrir)
    bool on_app_codes = false;  // reconectar tambien tras cierres 4000-4999 del servidor
};

struct WebSocketOptions {
    std::string url;                              // ws://host[:puerto]/ruta o wss://...
    HttpHeaders headers;                          // Authorization, Cookie... (sin Host/Upgrade/Connection/Sec-WebSocket-*)
    std::vector<std::string> protocols;           // Sec-WebSocket-Protocol
    int connect_timeout_ms = 10000;               // DNS + TCP + TLS + saludo
    std::size_t max_message_bytes = 4u << 20;     // entrante; mayor -> cierre 1009 (tope kWsHardMaxMessage)
    std::size_t max_inbox_bytes = 16u << 20;      // eventos sin recoger; lleno -> se deja de leer (no se pierde nada)
    std::size_t max_send_queue_bytes = 16u << 20; // salida pendiente; send() falla si se pasa
    int ping_interval_ms = 20000;                 // motor propio: ping real; WinHTTP: keepalive (>= 15000)
    int idle_timeout_ms = -1;                     // -1 automatico (propio: max(2.5*ping, 45 s); WinHTTP: no, salvo heartbeat); 0 nunca
    int send_timeout_ms = 20000;                  // un envio bloqueado mas -> conexion muerta (1006)
    int close_timeout_ms = 3000;                  // espera del cierre limpio
    std::string heartbeat_text;                   // opcional: texto de latido cada ping_interval_ms
    bool resend_queued = false;                   // al reconectar, mandar lo que quedo sin enviar
    WsReconnect reconnect;
    int insecure_level = -1;                      // -1 = net.WsInsecure (se lee en open, hilo principal)
    bool force_rfc6455 = false;                   // pruebas: motor propio tambien en Windows (solo ws://)
};

enum class WsState : std::uint8_t { Connecting, Open, Closing, Closed, Reconnecting };
const char* wsStateName(WsState state);  // "connecting", "open", "closing", "closed", "reconnecting"

struct WsEvent {
    enum class Kind : std::uint8_t { Open, Text, Binary, Close, Error, Reconnecting };
    Kind kind = Kind::Error;
    std::string data;             // Text/Binary: mensaje (Binary en bruto); Close: motivo; Error: texto en espanol
    int code = 0;                 // Close: 1000..4999, 1005 sin codigo, 1006 cortada; Reconnecting: intento (1, 2...)
    bool clean = false;           // Close: hubo saludo de cierre
    bool will_reconnect = false;  // Close/Error: detras viene Reconnecting
    int delay_ms = 0;             // Reconnecting
    int http_status = 0;          // Error del saludo (401, 404...)
    std::string protocol;         // Open
    std::size_t dropped = 0;      // Close: mensajes salientes tirados
};
// Orden: Open al abrir. Error cuando algo falla. Close cuando termina una
// conexion abierta, y tambien como ultimo evento si termina del todo sin haber
// llegado a abrir (1006). Reconnecting despues de Close/Error si will_reconnect.

struct WsStats {
    WsState state = WsState::Closed;
    std::string protocol;
    std::uint64_t sent_messages = 0, received_messages = 0, sent_bytes = 0, received_bytes = 0;
    std::size_t queued_send_bytes = 0;
    std::size_t inbox_bytes = 0;
    int rtt_ms = -1;  // motor propio (ping/pong); WinHTTP -1
    int reconnects = 0;
};

// Politica sin red (sin DNS): esquema, puerto, ws:// segun el nivel. false + motivo.
bool checkWebSocketUrl(const std::string& url, int insecure_level, std::string* error);
// Cabeceras (validHeader, reservadas, limites) y subprotocolos (tokens, sin repetir).
bool checkWebSocketOptions(const WebSocketOptions& options, std::string* error);

class WebSocket {
public:
    // Valida y arranca en segundo plano. nullptr + error si algo no vale o hay
    // demasiadas (net.WsMaxConnections) o la red ya se cerro. Nunca bloquea.
    static std::shared_ptr<WebSocket> open(WebSocketOptions options, std::string* error);
    ~WebSocket();  // = close(1001) sin esperar; sus eventos se tiran

    WebSocket(const WebSocket&) = delete;
    WebSocket& operator=(const WebSocket&) = delete;

    // Encola; nunca bloquea. false + motivo: no abierta, texto no UTF-8, cola llena, ya cerrando.
    bool send(std::string data, bool binary, std::string* error);
    // Cierre limpio (1000, 1001 o 3000-4999; otro codigo pasa a 1000; motivo <= 123 bytes UTF-8). Sin reconexion.
    void close(int code = 1000, std::string reason = {});
    // Corta ya (sin saludo). Close 1006 si estaba abierta.
    void abort();
    // Cabecera para el proximo intento (renovar token). Valor vacio la quita.
    bool setHeader(std::string name, std::string value, std::string* error);

    WsState state() const;
    WsStats stats() const;
    const std::string& url() const;
    // Eventos nuevos (hilo principal). max 0 = todos. Libera la cola de entrada.
    std::vector<WsEvent> poll(std::size_t max = 0);
    // Cerrada del todo, sin reconexion pendiente y ya recogido su ultimo evento.
    bool finished() const;

    struct Conn;  // interno (WebSocket.cpp)

private:
    explicit WebSocket(std::shared_ptr<Conn> conn);
    std::shared_ptr<Conn> conn_;
};

// Las conexiones de un ScriptSystem (o de una prueba), por id y nombre. Solo hilo principal.
class WebSocketHub {
public:
    WebSocketHub() = default;
    ~WebSocketHub();  // closeAll(1001, "Fin") sin esperar
    WebSocketHub(const WebSocketHub&) = delete;
    WebSocketHub& operator=(const WebSocketHub&) = delete;

    // id > 0, o 0 + error. Con name: si hay una viva (no Closed) con ese nombre,
    // devuelve su id sin abrir otra (*reused = true).
    std::uint32_t open(WebSocketOptions options, const std::string& name, std::string* error, bool* reused = nullptr);
    std::shared_ptr<WebSocket> get(std::uint32_t id) const;  // nullptr si no existe
    std::uint32_t find(const std::string& name) const;       // 0 si no hay viva
    std::string name(std::uint32_t id) const;
    std::vector<std::uint32_t> ids() const;                  // en orden de apertura (incluye las Closed aun no recogidas)
    void close(std::uint32_t id, int code = 1000, std::string reason = {});
    void closeAll(int code, const std::string& reason, bool unnamed_only = false);
    // Quita las terminadas: finished(), o Closed hace mas de 30 s sin recoger.
    void collect();
    int count() const;  // no Closed

private:
    struct Item {
        std::shared_ptr<WebSocket> ws;
        std::string name;
    };
    std::vector<std::pair<std::uint32_t, Item>> items_;
    std::uint32_t next_id_ = 1;
};

// Al salir del programa (player, servidor, editor): cierra todo (1001), espera
// hasta wait_ms, corta lo que quede y espera 200 ms mas a los hilos. Despues,
// open() falla con "la red del motor ya se cerro".
void shutdownWebSockets(int wait_ms = 500);

}  // namespace cramion::net

#endif  // CRAMION_CORE_NET_WEBSOCKET_H
