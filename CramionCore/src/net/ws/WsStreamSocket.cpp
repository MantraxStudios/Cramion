// TCP del motor propio de WebSocket: Winsock en Windows, POSIX en Linux y
// Android. El socket va sin bloqueo y se espera con poll en tramos cortos:
// asi abort() (desde cualquier hilo) siempre lo despierta, tambien en Windows,
// donde shutdown() no despierta un recv bloqueado.

#include "WsSocket.h"

#include "../NetCommon.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstring>
#include <mutex>

namespace cramion::net::ws {

namespace sock {

namespace {

using Clock = std::chrono::steady_clock;

constexpr const char* kErrCancelled = "conexion cancelada";

void initOnce() {
#if defined(_WIN32)
    // Una vez por proceso y sin WSACleanup (ENet lleva su propia cuenta).
    static const bool ok = [] {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    (void)ok;
#endif
}

#if defined(_WIN32)
inline SOCKET S(Fd fd) { return static_cast<SOCKET>(fd); }
#endif

bool setNonBlocking(Fd fd, bool on) {
#if defined(_WIN32)
    u_long mode = on ? 1 : 0;
    return ioctlsocket(S(fd), FIONBIO, &mode) == 0;
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    flags = on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    return fcntl(fd, F_SETFL, flags) == 0;
#endif
}

// Texto numerico de la direccion (IPv4 mapeada en IPv6 -> IPv4).
std::string addrText(const sockaddr* sa) {
    char buf[INET6_ADDRSTRLEN + 1] = {};
    if (sa->sa_family == AF_INET) {
        const auto* in = reinterpret_cast<const sockaddr_in*>(sa);
        inet_ntop(AF_INET, const_cast<in_addr*>(&in->sin_addr), buf, sizeof buf);
        return buf;
    }
    if (sa->sa_family == AF_INET6) {
        const auto* in6 = reinterpret_cast<const sockaddr_in6*>(sa);
        const auto* b = reinterpret_cast<const unsigned char*>(&in6->sin6_addr);
        static const unsigned char kMapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
        if (std::memcmp(b, kMapped, 12) == 0) {
            return std::to_string(b[12]) + "." + std::to_string(b[13]) + "." + std::to_string(b[14]) + "." +
                   std::to_string(b[15]);
        }
        inet_ntop(AF_INET6, const_cast<in6_addr*>(&in6->sin6_addr), buf, sizeof buf);
        return buf;
    }
    return {};
}

struct AddrList {
    addrinfo* ai = nullptr;
    AddrList() = default;
    AddrList(const AddrList&) = delete;
    AddrList& operator=(const AddrList&) = delete;
    ~AddrList() {
        if (ai != nullptr) freeaddrinfo(ai);
    }
};

bool resolve(const std::string& host, int port, AddrList& list, std::string& error) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    const std::string service = std::to_string(port);
    const int rc = getaddrinfo(host.c_str(), service.c_str(), &hints, &list.ai);
    if (rc != 0 || list.ai == nullptr) {
        // EAI_NONAME / WSAHOST_NOT_FOUND / EAI_AGAIN (sin red): todos igual para el jugador.
        error = detail::kErrNoHost;
        return false;
    }
    return true;
}

bool policyOk(const addrinfo* list, AddrPolicy policy) {
    if (policy == AddrPolicy::Any) return true;
    for (const addrinfo* ai = list; ai != nullptr; ai = ai->ai_next) {
        const std::string ip = addrText(ai->ai_addr);
        if (ip.empty()) return false;
        if (policy == AddrPolicy::LoopbackOnly && !detail::isLoopbackIp(ip)) return false;
        if (policy == AddrPolicy::PrivateOnly && !detail::isPrivateIp(ip)) return false;
    }
    return true;
}

bool permissionError(int e) {
#if defined(_WIN32)
    return e == WSAEACCES;
#else
    return e == EACCES || e == EPERM;
#endif
}

bool inProgress(int e) {
#if defined(_WIN32)
    return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS;
#else
    return e == EINPROGRESS || e == EINTR || e == EAGAIN;
#endif
}

bool refused(int e) {
#if defined(_WIN32)
    return e == WSAECONNREFUSED;
#else
    return e == ECONNREFUSED;
#endif
}

bool timedOut(int e) {
#if defined(_WIN32)
    return e == WSAETIMEDOUT;
#else
    return e == ETIMEDOUT;
#endif
}

// Espera a que termine connect(): >0 terminado (mirar SO_ERROR), 0 tiempo, <0 error.
int waitConnect(Fd fd, int ms) {
#if defined(_WIN32)
    // select y no WSAPoll: WSAPoll no avisa de un connect fallido en Windows antiguos.
    fd_set wr;
    fd_set ex;
    FD_ZERO(&wr);
    FD_ZERO(&ex);
    FD_SET(S(fd), &wr);
    FD_SET(S(fd), &ex);
    timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    const int r = select(0, nullptr, &wr, &ex, &tv);
    return r == SOCKET_ERROR ? -1 : r;
#else
    return waitFd(fd, true, ms);
#endif
}

int socketError(Fd fd) {
    int err = 0;
#if defined(_WIN32)
    int len = sizeof err;
    if (getsockopt(S(fd), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len) != 0) return lastError();
#else
    socklen_t len = sizeof err;
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0) return lastError();
#endif
    return err;
}

void tuneSocket(Fd fd) {
    int one = 1;
#if defined(_WIN32)
    setsockopt(S(fd), IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof one);
    setsockopt(S(fd), SOL_SOCKET, SO_KEEPALIVE, reinterpret_cast<const char*>(&one), sizeof one);
#else
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one);
#if defined(SO_NOSIGPIPE)
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
#endif
}

}  // namespace

int lastError() {
#if defined(_WIN32)
    return WSAGetLastError();
#else
    return errno;
#endif
}

int waitFd(Fd fd, bool write, int timeout_ms) {
#if defined(_WIN32)
    WSAPOLLFD p{};
    p.fd = S(fd);
    p.events = write ? POLLWRNORM : POLLRDNORM;
    const int r = WSAPoll(&p, 1, timeout_ms);
    return r == SOCKET_ERROR ? -1 : r;
#else
    pollfd p{};
    p.fd = fd;
    p.events = write ? POLLOUT : POLLIN;
    const int r = poll(&p, 1, timeout_ms);
    if (r < 0 && errno == EINTR) return 0;
    return r;
#endif
}

long recvSome(Fd fd, char* buffer, std::size_t n) {
    const std::size_t want = std::min<std::size_t>(n, INT_MAX);
#if defined(_WIN32)
    const int r = recv(S(fd), buffer, static_cast<int>(want), 0);
    if (r >= 0) return r;
    return WSAGetLastError() == WSAEWOULDBLOCK ? -1 : -2;
#else
    const ssize_t r = recv(fd, buffer, want, 0);
    if (r >= 0) return static_cast<long>(r);
    return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? -1 : -2;
#endif
}

long sendSome(Fd fd, const char* data, std::size_t n) {
    const std::size_t want = std::min<std::size_t>(n, INT_MAX);
#if defined(_WIN32)
    const int r = send(S(fd), data, static_cast<int>(want), 0);
    if (r > 0) return r;
    if (r == 0) return -1;
    return WSAGetLastError() == WSAEWOULDBLOCK ? -1 : -2;
#else
#if defined(MSG_NOSIGNAL)
    const int flags = MSG_NOSIGNAL;
#else
    const int flags = 0;
#endif
    const ssize_t r = send(fd, data, want, flags);
    if (r > 0) return static_cast<long>(r);
    if (r == 0) return -1;
    return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? -1 : -2;
#endif
}

void shutdownFd(Fd fd) {
    if (fd == kNoFd) return;
#if defined(_WIN32)
    shutdown(S(fd), SD_BOTH);
#else
    shutdown(fd, SHUT_RDWR);
#endif
}

void closeFd(Fd fd) {
    if (fd == kNoFd) return;
#if defined(_WIN32)
    closesocket(S(fd));
#else
    close(fd);
#endif
}

bool checkPolicy(const std::string& host, int port, AddrPolicy policy, std::string& error, bool& fatal) {
    initOnce();
    fatal = false;
    if (policy == AddrPolicy::Any) return true;
    AddrList list;
    if (!resolve(host, port, list, error)) return false;
    if (!policyOk(list.ai, policy)) {
        error = detail::kErrNotLocal;
        fatal = true;
        return false;
    }
    return true;
}

bool connectTcp(const std::string& host, int port, int timeout_ms, AddrPolicy policy, const std::atomic<bool>& aborted,
                Fd& out, std::string& error, bool& fatal) {
    initOnce();
    out = kNoFd;
    fatal = false;
    if (timeout_ms <= 0) timeout_ms = 10000;
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);

    // DNS: no se puede cancelar; abort se mira al volver.
    AddrList list;
    if (!resolve(host, port, list, error)) return false;
    if (aborted.load()) {
        error = kErrCancelled;
        return false;
    }
    // Politica con TODAS las direcciones (una sola publica basta para negar).
    if (!policyOk(list.ai, policy)) {
        error = detail::kErrNotLocal;
        fatal = true;
        return false;
    }

    std::string last = detail::kErrRefused;
    for (const addrinfo* ai = list.ai; ai != nullptr; ai = ai->ai_next) {
        if (aborted.load()) {
            error = kErrCancelled;
            return false;
        }
        if (Clock::now() >= deadline) {
            error = detail::kErrTimeout;
            return false;
        }
#if defined(_WIN32)
        const Fd s = static_cast<Fd>(socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol));
#else
        const Fd s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
#endif
        if (s == kNoFd) {
            const int e = lastError();
            if (permissionError(e)) {
                error = detail::kErrPermission;
                fatal = true;
                return false;
            }
            last = "no se pudo crear el socket (error " + std::to_string(e) + ")";
            continue;
        }
        if (!setNonBlocking(s, true)) {
            last = "no se pudo preparar el socket (error " + std::to_string(lastError()) + ")";
            closeFd(s);
            continue;
        }

        bool ok = false;
        bool timeout = false;
        int err = 0;
#if defined(_WIN32)
        const int rc = ::connect(S(s), ai->ai_addr, static_cast<int>(ai->ai_addrlen));
#else
        const int rc = ::connect(s, ai->ai_addr, static_cast<socklen_t>(ai->ai_addrlen));
#endif
        if (rc == 0) {
            ok = true;
        } else {
            const int e = lastError();
            if (!inProgress(e)) {
                err = e;
            } else {
                for (;;) {
                    if (aborted.load()) break;
                    const auto left =
                        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
                    if (left <= 0) {
                        timeout = true;
                        break;
                    }
                    const int r = waitConnect(s, static_cast<int>(std::min<long long>(left, 50)));
                    if (r > 0) {
                        err = socketError(s);
                        ok = err == 0;
                        break;
                    }
                    if (r < 0) {
                        err = lastError();
                        break;
                    }
                }
            }
        }

        if (ok && !aborted.load()) {
            tuneSocket(s);
            out = s;
            return true;
        }
        closeFd(s);
        if (aborted.load()) {
            error = kErrCancelled;
            return false;
        }
        if (timeout || timedOut(err)) {
            error = detail::kErrTimeout;
            return false;
        }
        if (permissionError(err)) {
            error = detail::kErrPermission;
            fatal = true;
            return false;
        }
        if (refused(err)) {
            last = detail::kErrRefused;
        } else {
            last = "no se pudo conectar con el servidor (error " + std::to_string(err) + ")";
        }
    }
    error = last;
    return false;
}

}  // namespace sock

namespace {

class PlainStream final : public ByteStream {
public:
    ~PlainStream() override { sock::closeFd(fd_); }

    bool connect(const std::string& host, int port, int timeout_ms, AddrPolicy policy, std::string& error,
                 bool& fatal) override {
        sock::Fd s = sock::kNoFd;
        if (!sock::connectTcp(host, port, timeout_ms, policy, aborted_, s, error, fatal)) return false;
        {
            std::lock_guard<std::mutex> lock(mu_);
            fd_ = s;
        }
        // abort() pudo llegar justo antes de publicar el socket.
        if (aborted_.load()) {
            sock::shutdownFd(s);
            error = "conexion cancelada";
            fatal = false;
            return false;
        }
        return true;
    }

    long read(char* buffer, std::size_t n, std::string& error) override {
        for (;;) {
            if (aborted_.load()) {
                error.clear();
                return -1;
            }
            const long r = sock::recvSome(fd_, buffer, n);
            if (r > 0) return r;
            if (r == 0) {
                if (aborted_.load()) {
                    error.clear();
                    return -1;
                }
                return 0;
            }
            if (r == -2) {
                if (aborted_.load()) {
                    error.clear();
                    return -1;
                }
                error = std::string(detail::kErrCut) + " (error " + std::to_string(sock::lastError()) + ")";
                return -1;
            }
            if (sock::waitFd(fd_, false, sock::kSliceMs) < 0 && !aborted_.load()) {
                error = std::string(detail::kErrCut) + " (error " + std::to_string(sock::lastError()) + ")";
                return -1;
            }
        }
    }

    bool write(const char* data, std::size_t n, std::string& error) override {
        while (n > 0) {
            if (aborted_.load()) {
                error.clear();
                return false;
            }
            const long r = sock::sendSome(fd_, data, n);
            if (r > 0) {
                data += r;
                n -= static_cast<std::size_t>(r);
                continue;
            }
            if (r == -2) {
                if (aborted_.load()) {
                    error.clear();
                    return false;
                }
                error = std::string(detail::kErrCut) + " (error " + std::to_string(sock::lastError()) + ")";
                return false;
            }
            if (sock::waitFd(fd_, true, sock::kSliceMs) < 0 && !aborted_.load()) {
                error = std::string(detail::kErrCut) + " (error " + std::to_string(sock::lastError()) + ")";
                return false;
            }
        }
        return true;
    }

    void abort() override {
        if (aborted_.exchange(true)) return;
        std::lock_guard<std::mutex> lock(mu_);
        // shutdown despierta poll/recv/send; el cierre del socket, solo en el
        // destructor (un numero de socket reutilizado nunca se toca).
        sock::shutdownFd(fd_);
    }

private:
    std::mutex mu_;
    sock::Fd fd_ = sock::kNoFd;
    std::atomic<bool> aborted_{false};
};

}  // namespace

std::unique_ptr<ByteStream> makePlainStream() { return std::make_unique<PlainStream>(); }

}  // namespace cramion::net::ws
