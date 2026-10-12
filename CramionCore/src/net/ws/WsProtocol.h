#ifndef CRAMION_CORE_NET_WS_PROTOCOL_H
#define CRAMION_CORE_NET_WS_PROTOCOL_H

// RFC 6455 sin E/S: saludo, tramas, codigos de cierre. Lo usan el motor propio
// (Linux, Android, Windows con force_rfc6455) y el servidor de pruebas.

#include "CramionCore/net/Http.h"  // HttpHeaders

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace cramion::net::ws {

inline constexpr const char* kGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
inline constexpr std::size_t kMaxHandshakeBytes = 16u << 10;
inline constexpr std::size_t kMaxControlPayload = 125;
inline constexpr std::size_t kMaxCloseReason = 123;

enum class Opcode : std::uint8_t { Continuation = 0x0, Text = 0x1, Binary = 0x2, Close = 0x8, Ping = 0x9, Pong = 0xA };

std::string makeKey();                        // base64 de 16 bytes de secureRandom
std::string acceptFor(std::string_view key);  // base64(sha1(key + kGuid))

struct HandshakeRequest {
    std::string host;
    int port = 0;
    bool secure = false;
    std::string path = "/";  // con la query
    HttpHeaders headers;     // ya validadas
    std::vector<std::string> protocols;
    std::string key;
};
// "GET path HTTP/1.1", Host (puerto solo si no es 80/443), Upgrade, Connection,
// Sec-WebSocket-Version: 13, Sec-WebSocket-Key, Sec-WebSocket-Protocol (si hay), cabeceras.
std::string buildHandshake(const HandshakeRequest& request);
// Posicion justo despues de "\r\n\r\n", o npos.
std::size_t headerEnd(std::string_view buffer);

struct HandshakeResult {
    bool ok = false;
    int status = 0;        // estado HTTP de la respuesta
    std::string protocol;  // el elegido por el servidor
    std::string error;     // texto en espanol
    bool fatal = false;    // no reconectar (401/403/404/426/otros 4xx menos 408/429, subprotocolo, extensiones, accept mal)
};
// head = respuesta hasta "\r\n\r\n" incluido. Exige 101, Upgrade: websocket,
// "upgrade" en Connection, Sec-WebSocket-Accept correcto, sin Sec-WebSocket-Extensions,
// y un subprotocolo de los pedidos (o ninguno si no se pidio).
HandshakeResult verifyHandshake(std::string_view head, std::string_view key, const std::vector<std::string>& protocols);

// Cliente -> servidor: siempre con mascara (mask). fin = false en los trozos.
std::string encodeFrame(Opcode op, std::string_view payload, bool fin, std::uint32_t mask);
// code 0 = cierre sin codigo (payload vacio). Motivo cortado a 123 bytes (UTF-8).
std::string closePayload(int code, std::string_view reason);
bool validReceivedCloseCode(int code);  // 1000-1003, 1007-1014, 3000-4999
bool validSendCloseCode(int code);      // 1000 y 3000-4999 (lo que puede pedir un script)

// Mascaras rapidas (xorshift64*, semilla de secureRandom).
class Masker {
public:
    Masker();
    std::uint32_t next();

private:
    std::uint64_t s_;
};

// Tramas del servidor, incremental (cualquier corte de TCP).
class FrameParser {
public:
    explicit FrameParser(std::size_t max_message_bytes);
    struct Item {
        enum class Kind : std::uint8_t { Text, Binary, Ping, Pong, Close, Error };
        Kind kind = Kind::Error;
        std::string data;  // mensaje / payload de ping-pong / motivo (Close) / texto del error (Error)
        int code = 0;      // Close: el recibido (1005 si vino vacio); Error: el que hay que mandar (1002, 1007, 1009)
    };
    // Tras un Error no saca nada mas. Reglas: servidor sin mascara, RSV=0,
    // opcodes conocidos, control con FIN y <= 125 (puede ir entre trozos),
    // orden de continuaciones, tamano comprobado ANTES de reservar (1009),
    // UTF-8 del mensaje entero (1007), cierre de 1 byte o codigo no valido (1002).
    void feed(const char* data, std::size_t n, std::vector<Item>& out);
    bool failed() const;

private:
    void fail(int code, std::string text, std::vector<Item>& out);
    bool startFrame(std::vector<Item>& out);  // cabecera completa en hdr_; false si fallo
    void endFrame(std::vector<Item>& out);

    std::size_t max_ = 0;
    bool failed_ = false;
    bool closed_ = false;           // llego el cierre: lo que venga despues se ignora
    unsigned char hdr_[14] = {};    // cabecera en curso (2 + hasta 8 de longitud)
    std::size_t hdr_len_ = 0;
    bool in_payload_ = false;
    std::uint64_t remaining_ = 0;   // bytes de payload que faltan de la trama actual
    Opcode frame_op_ = Opcode::Continuation;
    bool frame_fin_ = false;
    bool in_message_ = false;       // hay un mensaje de datos a medias (trozos)
    Opcode msg_op_ = Opcode::Text;
    std::string msg_;               // mensaje de datos que se va juntando
    std::string ctrl_;              // payload de la trama de control actual
};

}  // namespace cramion::net::ws

#endif
