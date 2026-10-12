// wss:// del motor propio en Linux con OpenSSL (si se compilo con
// CRAMION_HAS_OPENSSL). En Windows el TLS lo hace WinHTTP y en Android
// javax.net.ssl.SSLSocket (WsStreamJni.cpp).

#include "WsStream.h"

#if !defined(__ANDROID__)

#include "../NetCommon.h"

#if defined(CRAMION_HAS_OPENSSL)

#include "WsSocket.h"

#include <arpa/inet.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <atomic>
#include <chrono>
#include <climits>
#include <csignal>
#include <mutex>

namespace cramion::net::ws {

namespace {

using Clock = std::chrono::steady_clock;

struct Context {
    SSL_CTX* ctx = nullptr;
    std::string error;
};

// Un SSL_CTX para todo el proceso (no se libera: lo pueden usar hilos sueltos al salir).
const Context& context() {
    static const Context c = [] {
        Context out;
        // El BIO de socket de OpenSSL escribe con write() (sin MSG_NOSIGNAL): si el
        // servidor corta, SIGPIPE mataria el proceso. Se ignora si nadie lo maneja.
        struct sigaction old_action {};
        if (sigaction(SIGPIPE, nullptr, &old_action) == 0 && old_action.sa_handler == SIG_DFL &&
            (old_action.sa_flags & SA_SIGINFO) == 0) {
            std::signal(SIGPIPE, SIG_IGN);
        }
        SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
        if (ctx == nullptr) {
            out.error = "no se pudo iniciar OpenSSL";
            return out;
        }
        SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
        SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
        if (SSL_CTX_set_default_verify_paths(ctx) != 1) {
            // Sin certificados del sistema no se puede comprobar nada: todo fallara como certificado no valido.
            ERR_clear_error();
        }
        SSL_CTX_set_mode(ctx, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
#if defined(SSL_OP_IGNORE_UNEXPECTED_EOF)
        // OpenSSL 3: TCP cerrado sin close_notify = fin normal (ZERO_RETURN), no error.
        // El cierre de WebSocket ya va dentro del flujo (trama de cierre).
        SSL_CTX_set_options(ctx, SSL_OP_IGNORE_UNEXPECTED_EOF);
#endif
        out.ctx = ctx;
        return out;
    }();
    return c;
}

std::string sslErrorText() {
    const unsigned long e = ERR_get_error();
    ERR_clear_error();
    if (e == 0) return {};
    char buf[256];
    ERR_error_string_n(e, buf, sizeof buf);
    return buf;
}

bool isIpLiteral(const std::string& host) {
    unsigned char buf[16];
    return inet_pton(AF_INET, host.c_str(), buf) == 1 || inet_pton(AF_INET6, host.c_str(), buf) == 1;
}

class OpenSslStream final : public ByteStream {
public:
    ~OpenSslStream() override {
        {
            std::lock_guard<std::mutex> lock(ssl_mu_);
            if (ssl_ != nullptr) SSL_free(ssl_);
            ssl_ = nullptr;
        }
        sock::closeFd(fd_);
    }

    bool connect(const std::string& host, int port, int timeout_ms, AddrPolicy policy, std::string& error,
                 bool& fatal) override {
        fatal = false;
        const Context& ctx = context();
        if (ctx.ctx == nullptr) {
            error = ctx.error;
            return false;
        }
        if (timeout_ms <= 0) timeout_ms = 10000;
        const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);

        sock::Fd s = sock::kNoFd;
        if (!sock::connectTcp(host, port, timeout_ms, policy, aborted_, s, error, fatal)) return false;
        {
            std::lock_guard<std::mutex> lock(fd_mu_);
            fd_ = s;
        }
        if (aborted_.load()) {
            sock::shutdownFd(s);
            error = "conexion cancelada";
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(ssl_mu_);
            ERR_clear_error();
            ssl_ = SSL_new(ctx.ctx);
            if (ssl_ == nullptr) {
                error = "no se pudo iniciar TLS (" + sslErrorText() + ")";
                return false;
            }
            if (isIpLiteral(host)) {
                // Sin SNI para una IP; se comprueba la IP del certificado.
                X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(ssl_), host.c_str());
            } else {
                SSL_set_tlsext_host_name(ssl_, host.c_str());
                SSL_set1_host(ssl_, host.c_str());
            }
            SSL_set_fd(ssl_, s);
        }

        for (;;) {
            if (aborted_.load()) {
                error = "conexion cancelada";
                return false;
            }
            int rc;
            int err;
            long verify;
            {
                std::lock_guard<std::mutex> lock(ssl_mu_);
                ERR_clear_error();
                rc = SSL_connect(ssl_);
                err = rc == 1 ? SSL_ERROR_NONE : SSL_get_error(ssl_, rc);
                verify = SSL_get_verify_result(ssl_);
            }
            if (rc == 1) {
                if (verify != X509_V_OK) {
                    error = detail::kErrCert;
                    fatal = true;
                    return false;
                }
                return true;
            }
            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                const auto left =
                    std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
                if (left <= 0) {
                    error = detail::kErrTimeout;
                    return false;
                }
                const int ms = static_cast<int>(left < 50 ? left : 50);
                if (sock::waitFd(s, err == SSL_ERROR_WANT_WRITE, ms) < 0 && !aborted_.load()) {
                    error = detail::kErrCut;
                    return false;
                }
                continue;
            }
            if (aborted_.load()) {
                error = "conexion cancelada";
                return false;
            }
            if (verify != X509_V_OK) {
                error = detail::kErrCert;
                fatal = true;
                return false;
            }
            const std::string detail_text = sslErrorText();
            error = "fallo la conexion segura (TLS)";
            if (!detail_text.empty()) error += " (" + detail_text + ")";
            return false;
        }
    }

    long read(char* buffer, std::size_t n, std::string& error) override {
        const int want = static_cast<int>(n > INT_MAX ? INT_MAX : n);
        for (;;) {
            if (aborted_.load()) {
                error.clear();
                return -1;
            }
            int rc;
            int err;
            {
                std::lock_guard<std::mutex> lock(ssl_mu_);
                ERR_clear_error();
                rc = SSL_read(ssl_, buffer, want);
                err = rc > 0 ? SSL_ERROR_NONE : SSL_get_error(ssl_, rc);
            }
            if (rc > 0) return rc;
            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                // Sin el candado: el otro hilo puede escribir mientras tanto.
                if (sock::waitFd(fd_, err == SSL_ERROR_WANT_WRITE, sock::kSliceMs) < 0 && !aborted_.load()) {
                    error = detail::kErrCut;
                    return -1;
                }
                continue;
            }
            if (aborted_.load()) {
                error.clear();
                return -1;
            }
            if (err == SSL_ERROR_ZERO_RETURN) return 0;
            if (err == SSL_ERROR_SYSCALL && rc == 0) {
                ERR_clear_error();
                return 0;  // TCP cerrado sin close_notify: fin igualmente
            }
            const std::string detail_text = sslErrorText();
            error = detail::kErrCut;
            if (!detail_text.empty()) error += " (" + detail_text + ")";
            return -1;
        }
    }

    bool write(const char* data, std::size_t n, std::string& error) override {
        while (n > 0) {
            if (aborted_.load()) {
                error.clear();
                return false;
            }
            const int chunk = static_cast<int>(n > (1u << 30) ? (1u << 30) : n);
            int rc;
            int err;
            {
                std::lock_guard<std::mutex> lock(ssl_mu_);
                ERR_clear_error();
                rc = SSL_write(ssl_, data, chunk);
                err = rc > 0 ? SSL_ERROR_NONE : SSL_get_error(ssl_, rc);
            }
            if (rc > 0) {
                data += rc;
                n -= static_cast<std::size_t>(rc);
                continue;
            }
            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                if (sock::waitFd(fd_, err == SSL_ERROR_WANT_WRITE, sock::kSliceMs) < 0 && !aborted_.load()) {
                    error = detail::kErrCut;
                    return false;
                }
                continue;
            }
            if (aborted_.load()) {
                error.clear();
                return false;
            }
            const std::string detail_text = sslErrorText();
            error = detail::kErrCut;
            if (!detail_text.empty()) error += " (" + detail_text + ")";
            return false;
        }
        return true;
    }

    void abort() override {
        if (aborted_.exchange(true)) return;
        std::lock_guard<std::mutex> lock(fd_mu_);
        sock::shutdownFd(fd_);
    }

private:
    std::mutex ssl_mu_;  // alrededor de TODAS las llamadas SSL_*
    std::mutex fd_mu_;
    SSL* ssl_ = nullptr;
    sock::Fd fd_ = sock::kNoFd;
    std::atomic<bool> aborted_{false};
};

}  // namespace

std::unique_ptr<ByteStream> makeTlsStream(std::string* error) {
    const Context& ctx = context();
    if (ctx.ctx == nullptr) {
        if (error != nullptr) *error = ctx.error;
        return nullptr;
    }
    return std::make_unique<OpenSslStream>();
}

}  // namespace cramion::net::ws

#else  // sin OpenSSL

namespace cramion::net::ws {

std::unique_ptr<ByteStream> makeTlsStream(std::string* error) {
    if (error != nullptr) {
#if defined(_WIN32)
        *error = "wss:// con el motor propio no esta disponible en Windows (usa WinHTTP)";
#else
        *error = detail::kErrNoTls;
#endif
    }
    return nullptr;
}

}  // namespace cramion::net::ws

#endif  // CRAMION_HAS_OPENSSL

#endif  // !__ANDROID__
