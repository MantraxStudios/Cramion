#ifndef CRAMION_CORE_NET_WS_STREAM_H
#define CRAMION_CORE_NET_WS_STREAM_H

// Flujo de bytes del motor propio: TCP (Winsock/POSIX), TLS con OpenSSL (Linux)
// o TLS con javax.net.ssl.SSLSocket (Android). read() y write() pueden ir a la
// vez en hilos distintos (lector y sesion). abort() desde cualquier hilo.

#include <cstddef>
#include <memory>
#include <string>

namespace cramion::net::ws {

enum class AddrPolicy : unsigned char { Any, LoopbackOnly, PrivateOnly };

class ByteStream {
public:
    virtual ~ByteStream() = default;
    // Resuelve, comprueba `policy` con TODAS las direcciones, conecta (y hace TLS
    // con SNI + comprobacion del nombre). Bloquea hasta conectar, fallar,
    // timeout_ms o abort(). fatal = no reconectar (politica, certificado, permiso).
    virtual bool connect(const std::string& host, int port, int timeout_ms, AddrPolicy policy, std::string& error,
                         bool& fatal) = 0;
    virtual long read(char* buffer, std::size_t n, std::string& error) = 0;        // >0 bytes, 0 fin, -1 error/abort
    virtual bool write(const char* data, std::size_t n, std::string& error) = 0;   // todo, o false
    virtual void abort() = 0;                                                       // idempotente
};

std::unique_ptr<ByteStream> makePlainStream();
// nullptr + error si esta compilacion no tiene TLS para el motor propio
// (Windows: lo hace WinHTTP; Linux sin OpenSSL).
std::unique_ptr<ByteStream> makeTlsStream(std::string* error);

}  // namespace cramion::net::ws

#endif
