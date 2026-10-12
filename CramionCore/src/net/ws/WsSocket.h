#ifndef CRAMION_CORE_NET_WS_SOCKET_H
#define CRAMION_CORE_NET_WS_SOCKET_H

// Interno del motor propio (WsStreamSocket.cpp): TCP sin bloqueo con Winsock o
// POSIX. Lo comparten el flujo sin cifrar y el de OpenSSL. El socket queda SIN
// bloqueo: leer y escribir esperan con poll en tramos cortos (y miran abort).

#include "WsStream.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace cramion::net::ws::sock {

#if defined(_WIN32)
using Fd = std::uintptr_t;  // SOCKET
inline constexpr Fd kNoFd = ~static_cast<Fd>(0);
#else
using Fd = int;
inline constexpr Fd kNoFd = -1;
#endif

// Resuelve, comprueba `policy` con todas las direcciones (sin conectar).
// false + error (fatal si es la politica).
bool checkPolicy(const std::string& host, int port, AddrPolicy policy, std::string& error, bool& fatal);

// Resuelve, comprueba `policy`, conecta (sin bloqueo, hasta timeout_ms o abort).
// Deja el socket sin bloqueo, con TCP_NODELAY y SO_KEEPALIVE, en `out`. Mientras
// conecta mira `aborted` cada 50 ms (el socket aun no es de nadie mas).
bool connectTcp(const std::string& host, int port, int timeout_ms, AddrPolicy policy, const std::atomic<bool>& aborted,
                Fd& out, std::string& error, bool& fatal);

// Espera a poder leer (write = false) o escribir. >0 listo, 0 tiempo, <0 error.
int waitFd(Fd fd, bool write, int timeout_ms);
// recv/send sin bloqueo: >=0 bytes (recv 0 = fin), -1 hay que esperar, -2 error.
long recvSome(Fd fd, char* buffer, std::size_t n);
long sendSome(Fd fd, const char* data, std::size_t n);
void shutdownFd(Fd fd);
void closeFd(Fd fd);
int lastError();

// Tramo de espera de poll (ms): con abort() se despierta antes (shutdown).
inline constexpr int kSliceMs = 100;

}  // namespace cramion::net::ws::sock

#endif
