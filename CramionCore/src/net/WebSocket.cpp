// Conexiones WebSocket (ver WebSocket.h): politica, conexion con su hilo de
// sesion y su hilo lector, vigilante de tiempos, reconexion y WebSocketHub.
//
// Hilos:
//   - Principal: open/send/close/poll. Nunca bloquea ni toca sockets.
//   - Sesion (uno por conexion): conecta, escribe, ping, reconecta. Es el unico
//     que cambia Conn::state.
//   - Lector (uno por intento): engine->receive() -> inbox (con tope).
//   - Vigilante (uno por proceso): plazos (conectar, escribir, cerrar, sin
//     datos) -> engine->abort().

#include "CramionCore/net/WebSocket.h"

#include "CramionCore/cvar/CVar.h"
#include "CramionCore/net/Encoding.h"
#include "NetCommon.h"
#include "ws/WsEngine.h"
#include "ws/WsProtocol.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <iostream>
#include <mutex>
#include <random>
#include <system_error>
#include <thread>
#include <unordered_map>

namespace cramion::net {

namespace {

cvar::CVar<int> g_ws_insecure("net.WsInsecure", 0, "ws:// sin cifrar: 0 solo este PC, 1 tambien red local (10/8, 172.16/12, 192.168/16, fc00::/7), 2 cualquiera (solo con trucos)", cvar::Saved, 0, 2);
cvar::CVar<int> g_ws_max_conn("net.WsMaxConnections", 16, "Conexiones WebSocket a la vez (todo el proceso)", cvar::Saved, 1, 256);
cvar::CVar<int> g_ws_max_msg_kb("net.WsMaxMessageKB", 4096, "Tamano maximo por defecto de un mensaje WebSocket recibido (KB)", cvar::Saved, 1, 65536);
cvar::CVar<int> g_ws_events("net.WsMaxEventsPerFrame", 512, "Eventos WebSocket entregados a los scripts por frame", cvar::None, 16, 100000);
cvar::CVar<int> g_ws_log("net.WsLog", 0, "Consola: 1 abrir/cerrar/errores, 2 tambien cada mensaje (200 bytes)", cvar::None, 0, 2);

// Copia de net.WsLog para los hilos (se actualiza en open, hilo principal).
std::atomic<int> g_log_level{0};
// Sin delete: un hilo suelto puede escribir mientras se destruyen los estaticos al salir.
std::mutex& logMutex() {
    static std::mutex* m = new std::mutex;
    return *m;
}

constexpr const char* kErrBadUrl = "direccion no valida (usa ws:// o wss://)";
constexpr const char* kErrNotLocalHost = "ws:// sin cifrar solo se permite a este PC (localhost); usa wss:// o cambia net.WsInsecure";
constexpr std::size_t kEventOverhead = 64;  // lo que cuenta un evento sin datos en inbox_bytes

std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::string lowerText(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool reservedHeader(const std::string& name) {
    static const char* const kReserved[] = {"host",
                                            "upgrade",
                                            "connection",
                                            "sec-websocket-key",
                                            "sec-websocket-version",
                                            "sec-websocket-accept",
                                            "sec-websocket-extensions",
                                            "sec-websocket-protocol",
                                            "content-length"};
    const std::string l = lowerText(name);
    for (const char* r : kReserved) {
        if (l == r) return true;
    }
    return false;
}

// RFC 7230 tchar.
bool tokenChar(unsigned char c) {
    if (std::isalnum(c) != 0) return true;
    return std::strchr("!#$%&'*+-.^_`|~", c) != nullptr && c != 0;
}

bool validToken(const std::string& t) {
    if (t.empty() || t.size() > 64) return false;
    return std::all_of(t.begin(), t.end(), [](unsigned char c) { return tokenChar(c); });
}

std::string clip(const std::string& text, std::size_t n) {
    return text.size() <= n ? text : truncateUtf8(text, n) + "...";
}

}  // namespace

namespace ws {

int wsLogLevel() { return g_log_level.load(std::memory_order_relaxed); }

void wsLogLine(const std::string& line) {
    std::lock_guard<std::mutex> lock(logMutex());
    std::cerr << line << "\n";
}

bool parseWsUrl(const std::string& url, WsUrl& out, std::string* error) {
    const auto fail = [&] {
        if (error != nullptr) *error = kErrBadUrl;
        return false;
    };
    out = WsUrl{};
    if (url.empty() || url.size() > 8192) return fail();
    for (const unsigned char c : url) {
        if (c < 0x21 || c == 0x7F) return fail();
    }
    const std::size_t scheme_end = url.find("://");
    if (scheme_end == std::string::npos) return fail();
    const std::string scheme = lowerText(url.substr(0, scheme_end));
    if (scheme == "wss") {
        out.secure = true;
    } else if (scheme != "ws") {
        return fail();
    }
    std::string rest = url.substr(scheme_end + 3);
    if (const std::size_t hash = rest.find('#'); hash != std::string::npos) rest = rest.substr(0, hash);  // sin fragmento
    const std::size_t path_at = rest.find_first_of("/?");
    const std::string authority = rest.substr(0, path_at);
    std::string path = path_at == std::string::npos ? std::string("/") : rest.substr(path_at);
    if (!path.empty() && path.front() == '?') path = "/" + path;
    if (authority.empty() || authority.find('@') != std::string::npos) return fail();

    std::string host;
    std::string port_text;
    if (authority.front() == '[') {
        const std::size_t close = authority.find(']');
        if (close == std::string::npos || close < 2) return fail();
        host = authority.substr(1, close - 1);
        const std::string after = authority.substr(close + 1);
        if (!after.empty()) {
            if (after.front() != ':') return fail();
            port_text = after.substr(1);
            if (port_text.empty()) return fail();
        }
        for (const unsigned char c : host) {
            if (std::isxdigit(c) == 0 && c != ':' && c != '.') return fail();
        }
    } else {
        const std::size_t colon = authority.find(':');
        host = authority.substr(0, colon);
        if (colon != std::string::npos) {
            port_text = authority.substr(colon + 1);
            if (port_text.empty()) return fail();
        }
        if (host.empty() || host.size() > 253) return fail();
        for (const unsigned char c : host) {
            if (std::isalnum(c) == 0 && c != '-' && c != '.' && c != '_') return fail();
        }
    }
    int port = out.secure ? 443 : 80;
    if (!port_text.empty()) {
        if (port_text.size() > 5) return fail();
        port = 0;
        for (const unsigned char c : port_text) {
            if (std::isdigit(c) == 0) return fail();
            port = port * 10 + (c - '0');
        }
        if (port < 1 || port > 65535) return fail();
    }
    out.host = host;
    out.port = port;
    out.path = path;
    return true;
}

}  // namespace ws

const char* wsStateName(WsState state) {
    switch (state) {
        case WsState::Connecting: return "connecting";
        case WsState::Open: return "open";
        case WsState::Closing: return "closing";
        case WsState::Closed: return "closed";
        case WsState::Reconnecting: return "reconnecting";
    }
    return "closed";
}

bool checkWebSocketUrl(const std::string& url, int insecure_level, std::string* error) {
    ws::WsUrl parts;
    if (!ws::parseWsUrl(url, parts, error)) return false;
    if (parts.secure || insecure_level >= 2) return true;
    if (detail::isLoopbackHost(parts.host)) return true;
    if (insecure_level <= 0) {
        if (error != nullptr) *error = kErrNotLocalHost;
        return false;
    }
    // Nivel 1: una IP escrita tiene que ser local; un nombre se mira al conectar.
    bool numeric = !parts.host.empty() && (parts.host.find(':') != std::string::npos ||
                                          std::all_of(parts.host.begin(), parts.host.end(), [](unsigned char c) {
                                              return std::isdigit(c) != 0 || c == '.';
                                          }));
    if (numeric && !detail::isPrivateIp(parts.host)) {
        if (error != nullptr) *error = detail::kErrNotLocal;
        return false;
    }
    return true;
}

namespace {

bool checkHeaders(const HttpHeaders& headers, std::string* error) {
    const auto fail = [&](const std::string& why) {
        if (error != nullptr) *error = why;
        return false;
    };
    if (headers.size() > kWsMaxHeaders) return fail("cabecera no valida: (mas de " + std::to_string(kWsMaxHeaders) + ")");
    std::size_t total = 0;
    for (const auto& [name, value] : headers) {
        if (!validHeader(name, value)) return fail("cabecera no valida: " + clip(name, 64));
        if (reservedHeader(name)) return fail("cabecera reservada del protocolo: " + name);
        total += name.size() + value.size() + 4;
    }
    if (total > (32u << 10)) return fail("cabecera no valida: (mas de 32 KB en total)");
    return true;
}

}  // namespace

bool checkWebSocketOptions(const WebSocketOptions& options, std::string* error) {
    if (!checkHeaders(options.headers, error)) return false;
    if (options.protocols.size() > kWsMaxProtocols) {
        if (error != nullptr) *error = "subprotocolo no valido: (mas de " + std::to_string(kWsMaxProtocols) + ")";
        return false;
    }
    for (std::size_t i = 0; i < options.protocols.size(); ++i) {
        const std::string& p = options.protocols[i];
        const auto end = options.protocols.begin() + static_cast<std::ptrdiff_t>(i);
        const bool repeated = std::find(options.protocols.begin(), end, p) != end;
        if (!validToken(p) || repeated) {
            if (error != nullptr) *error = "subprotocolo no valido: " + clip(p, 64);
            return false;
        }
    }
    return true;
}

// --- Conexion -----------------------------------------------------------------------

namespace {
enum AbortReason : int { kNone = 0, kConnectTimeout, kWriteTimeout, kCloseTimeout, kIdle, kUser };
}  // namespace

struct WebSocket::Conn {
    struct Out {
        enum class Kind : unsigned char { Text, Binary, Pong, Close } kind = Kind::Text;
        std::string data;
        int code = 0;
    };

    WebSocketOptions options;  // headers: cambian con setHeader (bajo mu)
    ws::WsUrl url;
    ws::AddrPolicy policy = ws::AddrPolicy::Any;
    bool rfc = false;  // motor propio
    int idle_ms = 0;   // resuelto (0 = nunca)
    int log = 0;

    mutable std::mutex mu;
    std::condition_variable cv_session;
    std::condition_variable cv_inbox;

    std::deque<Out> out, priority;
    std::size_t out_bytes = 0;
    std::deque<WsEvent> inbox;
    std::size_t inbox_bytes = 0;

    WsState state = WsState::Connecting;
    bool user_closed = false;
    int user_code = 1000;
    std::string user_reason;
    // Del intento actual.
    bool close_queued = false;
    bool close_sent = false;
    bool close_received = false;
    bool reader_done = false;
    bool reader_stop = false;
    int peer_code = 0;
    std::string peer_reason;
    int protocol_error = 0;  // cierre que mandamos por error de protocolo (1002/1007/1009)
    ws::RxItem::Kind end_kind = ws::RxItem::Kind::Failed;
    std::string end_text;

    std::atomic<bool> aborting{false};
    std::atomic<bool> orphan{false};
    std::shared_ptr<ws::Engine> engine;
    std::uint32_t attempt_gen = 0;

    std::atomic<std::int64_t> deadline_connect{0};
    std::atomic<std::int64_t> deadline_write{0};
    std::atomic<std::int64_t> deadline_close{0};
    std::atomic<std::int64_t> last_rx{0};
    std::atomic<bool> inbox_paused{false};
    std::atomic<int> abort_reason{kNone};
    std::atomic<int> state_atomic{static_cast<int>(WsState::Connecting)};  // para el vigilante

    WsStats stats;
    bool final_close_emitted = false;
    bool session_ended = false;
    std::int64_t closed_at = 0;

    // Bajo mu.
    void setState(WsState s) {
        state = s;
        state_atomic.store(static_cast<int>(s));
    }
    // Bajo mu. Los eventos de una conexion huerfana se tiran.
    void push(WsEvent ev) {
        if (orphan.load()) return;
        inbox_bytes += ev.data.size() + kEventOverhead;
        inbox.push_back(std::move(ev));
    }
    void pushError(std::string text, bool will, int http_status = 0) {
        WsEvent ev;
        ev.kind = WsEvent::Kind::Error;
        ev.data = std::move(text);
        ev.will_reconnect = will;
        ev.http_status = http_status;
        push(std::move(ev));
    }
    void pushClose(int code, std::string reason, bool clean, bool will, std::size_t dropped) {
        WsEvent ev;
        ev.kind = WsEvent::Kind::Close;
        ev.code = code;
        ev.data = std::move(reason);
        ev.clean = clean;
        ev.will_reconnect = will;
        ev.dropped = dropped;
        push(std::move(ev));
        if (!will) final_close_emitted = true;
    }
    std::size_t dropOut() {
        const std::size_t n = out.size();
        out.clear();
        out_bytes = 0;
        return n;
    }
    void log1(const std::string& text) const {
        if (log >= 1) ws::wsLogLine("[WebSocket] " + options.url + ": " + text);
    }
};

namespace {

using ConnPtr = std::shared_ptr<WebSocket::Conn>;

// Registro del proceso (nunca se destruye: los hilos pueden seguir al salir).
struct Registry {
    std::mutex mu;
    std::vector<std::weak_ptr<WebSocket::Conn>> conns;
    bool shut_down = false;
    bool watchdog_started = false;
    std::mutex threads_mu;
    std::condition_variable threads_cv;
    int threads = 0;
};

Registry& registry() {
    static Registry* r = new Registry;  // a proposito sin delete
    return *r;
}

std::vector<ConnPtr> liveConns() {
    Registry& r = registry();
    std::vector<ConnPtr> out;
    std::lock_guard<std::mutex> lock(r.mu);
    auto& v = r.conns;
    v.erase(std::remove_if(v.begin(), v.end(), [](const std::weak_ptr<WebSocket::Conn>& w) { return w.expired(); }),
            v.end());
    for (const auto& w : v) {
        if (auto c = w.lock()) out.push_back(std::move(c));
    }
    return out;
}

// Cuenta un hilo vivo mientras exista.
struct ThreadTicket {
    ThreadTicket() = default;
    ThreadTicket(const ThreadTicket&) = delete;
    ThreadTicket& operator=(const ThreadTicket&) = delete;
    ~ThreadTicket() {
        Registry& r = registry();
        {
            std::lock_guard<std::mutex> lock(r.threads_mu);
            --r.threads;
        }
        r.threads_cv.notify_all();
    }
};

void addThread() {
    Registry& r = registry();
    std::lock_guard<std::mutex> lock(r.threads_mu);
    ++r.threads;
}

void removeThread() { ThreadTicket ticket; }

bool waitThreads(int ms) {
    Registry& r = registry();
    std::unique_lock<std::mutex> lock(r.threads_mu);
    return r.threads_cv.wait_for(lock, std::chrono::milliseconds(std::max(0, ms)), [&] { return r.threads == 0; });
}

void wakeAll(WebSocket::Conn& c) {
    c.cv_session.notify_all();
    c.cv_inbox.notify_all();
}

// Corta el intento actual (cualquier hilo). only_gen != 0: solo si sigue siendo
// ese intento (el vigilante leyo un plazo de un intento que ya termino).
void abortEngine(WebSocket::Conn& c, int reason, std::uint32_t only_gen = 0) {
    std::shared_ptr<ws::Engine> engine;
    {
        std::lock_guard<std::mutex> lock(c.mu);
        if (only_gen != 0 && c.attempt_gen != only_gen) return;
        engine = c.engine;
        if (reason != kNone) {
            int expected = kNone;
            c.abort_reason.compare_exchange_strong(expected, reason);
        }
    }
    if (engine) engine->abort();
    wakeAll(c);
}

// --- Vigilante ----------------------------------------------------------------------

void watchdogLoop() {
#if defined(__ANDROID__)
    detail::JniThread jni;
#endif
    std::mutex sleep_mu;
    std::condition_variable sleep_cv;
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(sleep_mu);
            sleep_cv.wait_for(lock, std::chrono::milliseconds(100));
        }
        const std::int64_t now = nowMs();
        for (const ConnPtr& c : liveConns()) {
            int reason = kNone;
            std::uint32_t gen = 0;
            {
                std::lock_guard<std::mutex> lock(c->mu);
                gen = c->attempt_gen;  // antes que los plazos: si cambia, el plazo leido es viejo
            }
            if (gen == 0) continue;
            const std::int64_t dc = c->deadline_connect.load();
            const std::int64_t dw = c->deadline_write.load();
            const std::int64_t dl = c->deadline_close.load();
            if (dc != 0 && now > dc) {
                reason = kConnectTimeout;
            } else if (dw != 0 && now > dw) {
                reason = kWriteTimeout;
            } else if (dl != 0 && now > dl) {
                reason = kCloseTimeout;
            } else if (c->state_atomic.load() == static_cast<int>(WsState::Open) && c->idle_ms > 0 &&
                       !c->inbox_paused.load() && c->last_rx.load() != 0 && now - c->last_rx.load() > c->idle_ms) {
                reason = kIdle;
            }
            if (reason != kNone) abortEngine(*c, reason, gen);
        }
    }
}

void startWatchdog() {
    Registry& r = registry();
    std::lock_guard<std::mutex> lock(r.mu);
    if (r.watchdog_started) return;
    try {
        std::thread(watchdogLoop).detach();
        r.watchdog_started = true;
    } catch (const std::system_error&) {
        // Sin vigilante los plazos no se cumplen; se reintenta en el siguiente open.
    }
}

// --- Hilo lector --------------------------------------------------------------------

// gen: el intento de este lector. Si la sesion lo dio por perdido (2 s) y ya va
// por otro, este lector no toca nada del intento nuevo.
void readerLoop(ConnPtr c, std::shared_ptr<ws::Engine> engine, std::uint32_t gen) {
    ThreadTicket ticket;
#if defined(__ANDROID__)
    detail::JniThread jni;
#endif
    using Kind = ws::RxItem::Kind;
    for (;;) {
        ws::RxItem item = engine->receive();
        const std::int64_t now = nowMs();
        switch (item.kind) {
            case Kind::Message: {
                c->last_rx.store(now);
                std::unique_lock<std::mutex> lock(c->mu);
                const std::size_t size = item.data.size() + kEventOverhead;
                // Contrapresion: con la entrada llena se deja de leer (nada se pierde).
                while (!c->inbox.empty() && c->inbox_bytes + size > c->options.max_inbox_bytes && !c->aborting.load() &&
                       !c->orphan.load() && !c->reader_stop && c->abort_reason.load() == kNone && c->attempt_gen == gen) {
                    c->inbox_paused.store(true);
                    c->cv_inbox.wait_for(lock, std::chrono::milliseconds(100));
                }
                if (c->attempt_gen != gen) return;
                if (c->inbox_paused.exchange(false)) c->last_rx.store(nowMs());
                c->stats.received_messages++;
                c->stats.received_bytes += item.data.size();
                if (c->log >= 2) {
                    ws::wsLogLine("[WebSocket] " + c->options.url + " <- " +
                                  (item.binary ? std::string("(binario, ") + std::to_string(item.data.size()) + " bytes)"
                                               : clip(item.data, 200)));
                }
                WsEvent ev;
                ev.kind = item.binary ? WsEvent::Kind::Binary : WsEvent::Kind::Text;
                ev.data = std::move(item.data);
                c->push(std::move(ev));
                break;
            }
            case Kind::Ping: {
                c->last_rx.store(now);
                {
                    std::lock_guard<std::mutex> lock(c->mu);
                    if (c->attempt_gen != gen) return;
                    if (!c->close_sent && !c->close_queued) {
                        WebSocket::Conn::Out o;
                        o.kind = WebSocket::Conn::Out::Kind::Pong;
                        o.data = std::move(item.data);
                        c->priority.push_back(std::move(o));
                    }
                }
                c->cv_session.notify_all();
                break;
            }
            case Kind::Pong: {
                c->last_rx.store(now);
                if (item.data.size() == 8) {
                    std::int64_t sent = 0;
                    for (int i = 0; i < 8; ++i) sent = (sent << 8) | static_cast<unsigned char>(item.data[static_cast<std::size_t>(i)]);
                    if (sent > 0 && sent <= now) {
                        std::lock_guard<std::mutex> lock(c->mu);
                        c->stats.rtt_ms = static_cast<int>(std::min<std::int64_t>(now - sent, 1 << 30));
                    }
                }
                break;
            }
            case Kind::PeerClose: {
                c->last_rx.store(now);
                {
                    std::lock_guard<std::mutex> lock(c->mu);
                    if (c->attempt_gen != gen) return;
                    c->close_received = true;
                    c->peer_code = item.code == 0 ? 1005 : item.code;
                    c->peer_reason = std::move(item.data);
                    if (!c->close_sent && !c->close_queued) {
                        int echo = c->peer_code;
                        if (echo == 1005) echo = 1000;
                        else if (!ws::validReceivedCloseCode(echo)) echo = 1002;
                        WebSocket::Conn::Out o;
                        o.kind = WebSocket::Conn::Out::Kind::Close;
                        o.code = echo;
                        c->priority.push_back(std::move(o));
                        c->close_queued = true;
                    }
                }
                c->cv_session.notify_all();
                break;
            }
            case Kind::ProtocolError: {
                {
                    std::lock_guard<std::mutex> lock(c->mu);
                    if (c->attempt_gen != gen) return;
                    c->log1(item.failure.text);
                    c->pushError(item.failure.text.empty() ? std::string("error de protocolo WebSocket del servidor")
                                                           : item.failure.text,
                                 false);
                    if (c->protocol_error == 0) c->protocol_error = item.code;
                    if (!c->close_sent && !c->close_queued) {
                        WebSocket::Conn::Out o;
                        o.kind = WebSocket::Conn::Out::Kind::Close;
                        o.code = item.code;
                        c->priority.push_back(std::move(o));
                        c->close_queued = true;
                    }
                }
                c->cv_session.notify_all();
                break;
            }
            case Kind::Ended:
            case Kind::Failed:
            case Kind::Aborted: {
                {
                    std::lock_guard<std::mutex> lock(c->mu);
                    if (c->attempt_gen != gen) return;
                    c->reader_done = true;
                    c->end_kind = item.kind;
                    c->end_text = std::move(item.failure.text);
                }
                c->cv_session.notify_all();
                return;
            }
        }
    }
}

// --- Hilo de sesion -----------------------------------------------------------------

std::uint64_t jitterSeed() {
    std::uint64_t seed = 0;
    if (!secureRandom(&seed, sizeof(seed))) seed = static_cast<std::uint64_t>(nowMs());
    return seed;
}

bool stopRequested(WebSocket::Conn& c) {
    std::lock_guard<std::mutex> lock(c.mu);
    return c.user_closed || c.aborting.load();
}

// Reconectar tras un cierre de una conexion abierta (tabla del plan).
bool reconnectAfterClose(const WebSocket::Conn& c, bool close_received, int code, int abort_reason) {
    if (c.user_closed || c.aborting.load() || !c.options.reconnect.enabled) return false;
    if (c.protocol_error != 0) return false;  // 1002/1007/1009 nuestro
    if (close_received) {
        if (code == 1001 || code == 1006 || (code >= 1011 && code <= 1014)) return true;
        if (code >= 4000 && code <= 4999) return c.options.reconnect.on_app_codes;
        return false;  // 1000, 1002, 1003, 1005, 1007-1010, 3000-3999
    }
    (void)abort_reason;  // 1006, sin datos o envio bloqueado: si
    return true;
}

// Espera antes de reintentar. false = no hay que seguir (limite, close, abort).
bool backoff(WebSocket::Conn& c, int& attempt, std::mt19937_64& rng) {
    ++attempt;
    const WsReconnect& r = c.options.reconnect;
    if (r.max_tries >= 0 && attempt > r.max_tries) {
        std::lock_guard<std::mutex> lock(c.mu);
        c.pushError("no se pudo reconectar tras " + std::to_string(r.max_tries) + " intentos", false);
        return false;
    }
    const double base = std::max(0, r.min_delay_ms);
    const double cap = std::max(r.min_delay_ms, r.max_delay_ms);
    double delay = base * std::pow(2.0, std::min(attempt - 1, 30));
    delay = std::min(delay, cap);
    std::uniform_real_distribution<double> jitter(0.8, 1.2);
    const int delay_ms = static_cast<int>(std::max(0.0, delay * jitter(rng)));
    std::unique_lock<std::mutex> lock(c.mu);
    if (c.user_closed || c.aborting.load()) return false;
    c.setState(WsState::Reconnecting);
    c.stats.reconnects++;
    WsEvent ev;
    ev.kind = WsEvent::Kind::Reconnecting;
    ev.code = attempt;
    ev.delay_ms = delay_ms;
    c.push(std::move(ev));
    c.log1("reconectando (intento " + std::to_string(attempt) + ", " + std::to_string(delay_ms) + " ms)");
    c.cv_session.wait_for(lock, std::chrono::milliseconds(delay_ms), [&] { return c.user_closed || c.aborting.load(); });
    return !(c.user_closed || c.aborting.load());
}

void sessionLoop(ConnPtr c) {
    ThreadTicket ticket;
#if defined(__ANDROID__)
    detail::JniThread jni;
#endif
    using OutKind = WebSocket::Conn::Out::Kind;
    std::mt19937_64 rng(jitterSeed());
    int attempt = 0;
    const WebSocketOptions& o = c->options;

    for (;;) {
        if (stopRequested(*c)) break;
#if defined(_WIN32)
        std::shared_ptr<ws::Engine> engine = c->rfc ? ws::makeRfcEngine() : ws::makeWinHttpEngine();
#else
        std::shared_ptr<ws::Engine> engine = ws::makeRfcEngine();
#endif
        ws::ConnectInfo info;
        info.url = c->url;
        info.protocols = o.protocols;
        info.timeout_ms = o.connect_timeout_ms;
        info.policy = c->policy;
        info.max_message_bytes = o.max_message_bytes;
        info.keepalive_ms = o.ping_interval_ms > 0 ? o.ping_interval_ms : 20000;
        {
            std::lock_guard<std::mutex> lock(c->mu);
            info.headers = c->options.headers;
            c->engine = engine;
            ++c->attempt_gen;
            c->close_queued = c->close_sent = c->close_received = false;
            c->reader_done = c->reader_stop = false;
            c->peer_code = 0;
            c->peer_reason.clear();
            c->protocol_error = 0;
            c->end_kind = ws::RxItem::Kind::Failed;
            c->end_text.clear();
            c->priority.clear();
            c->abort_reason.store(kNone);
            c->deadline_write.store(0);
            c->deadline_close.store(0);
            c->last_rx.store(0);
            c->inbox_paused.store(false);
            c->setState(WsState::Connecting);
        }
        if (stopRequested(*c)) {
            engine->abort();
            break;
        }

        std::string protocol;
        ws::Failure failure;
        c->deadline_connect.store(nowMs() + std::max(1000, o.connect_timeout_ms));
        bool ok = engine->connect(info, protocol, failure);
        c->deadline_connect.store(0);
        if (ok && stopRequested(*c)) {
            engine->abort();
            ok = false;
        }
        if (!ok) {
            engine->abort();
            if (stopRequested(*c)) break;
            if (c->abort_reason.load() == kConnectTimeout) {
                failure.text = detail::kErrTimeout;
                failure.fatal = false;
            }
            if (failure.text.empty()) failure.text = detail::kErrCut;
            const bool will = o.reconnect.enabled && !failure.fatal;
            {
                std::lock_guard<std::mutex> lock(c->mu);
                c->log1(failure.text);
                c->pushError(failure.text, will, failure.http_status);
            }
            if (!will) break;
            if (!backoff(*c, attempt, rng)) break;
            continue;
        }

        // Abierta.
        {
            std::lock_guard<std::mutex> lock(c->mu);
            c->setState(WsState::Open);
            c->stats.protocol = protocol;
            WsEvent ev;
            ev.kind = WsEvent::Kind::Open;
            ev.protocol = protocol;
            c->push(std::move(ev));
            c->log1("abierta" + (protocol.empty() ? std::string() : " (" + protocol + ")"));
        }
        attempt = 0;
        c->last_rx.store(nowMs());
        addThread();
        try {
            std::thread(readerLoop, c, engine, c->attempt_gen).detach();
        } catch (const std::system_error&) {
            removeThread();
            engine->abort();
            std::lock_guard<std::mutex> lock(c->mu);
            c->reader_done = true;
            c->end_kind = ws::RxItem::Kind::Failed;
            c->end_text = "no se pudo crear el hilo lector";
        }

        // Escritura: prioridad (pong, cierre), datos, ping/latido.
        const bool pings = engine->supportsPing() && o.ping_interval_ms > 0;
        const bool heartbeat = !o.heartbeat_text.empty() && o.ping_interval_ms > 0;
        std::int64_t next_ping = (pings || heartbeat) ? nowMs() + o.ping_interval_ms : 0;
        bool clean_abort_done = false;
        for (;;) {
            WebSocket::Conn::Out item;
            bool have = false;
            bool do_ping = false;
            bool end_clean = false;
            {
                std::unique_lock<std::mutex> lock(c->mu);
                const auto ready = [&] {
                    if (c->reader_done || c->aborting.load() || c->reader_stop || c->abort_reason.load() != kNone) return true;
                    if (c->user_closed && !c->close_queued && !c->close_sent && c->out.empty()) return true;
                    if (c->user_closed && c->state == WsState::Open) return true;
                    if (c->close_received && c->state == WsState::Open) return true;
                    if (!c->priority.empty()) return true;
                    if (!c->out.empty() && !c->close_sent && !c->close_queued) return true;
                    if (c->close_sent && c->close_received && !engine->waitsForEof() && !clean_abort_done) return true;
                    return false;
                };
                std::int64_t wait_until = nowMs() + 250;
                if (next_ping != 0) wait_until = std::min(wait_until, next_ping);
                const std::int64_t wait = std::max<std::int64_t>(0, wait_until - nowMs());
                c->cv_session.wait_for(lock, std::chrono::milliseconds(wait), ready);
                // Motor ya cortado (envio fallido, vigilante, abort): al final del intento,
                // que espera al lector como mucho 2 s (aqui no hay limite si el lector no vuelve).
                if (c->reader_done || c->aborting.load() || c->reader_stop || c->abort_reason.load() != kNone) break;
                if (c->close_sent || c->close_queued) next_ping = 0;  // tras el cierre no hay ping ni latido
                if ((c->user_closed || c->close_received || c->close_queued) && c->state == WsState::Open) {
                    c->setState(WsState::Closing);
                }
                if (c->user_closed && !c->close_queued && !c->close_sent && c->out.empty()) {
                    WebSocket::Conn::Out close;
                    close.kind = OutKind::Close;
                    close.code = c->user_code;
                    close.data = c->user_reason;
                    c->priority.push_back(std::move(close));
                    c->close_queued = true;
                }
                if (c->close_sent && c->close_received && !engine->waitsForEof() && !clean_abort_done) {
                    end_clean = true;
                } else if (!c->priority.empty()) {
                    item = std::move(c->priority.front());
                    c->priority.pop_front();
                    have = true;
                } else if (!c->out.empty() && !c->close_sent && !c->close_queued) {
                    item = std::move(c->out.front());
                    c->out.pop_front();
                    c->out_bytes -= std::min(c->out_bytes, item.data.size());
                    have = true;
                } else if (next_ping != 0 && nowMs() >= next_ping && !c->close_sent && !c->close_queued) {
                    do_ping = true;
                }
            }
            if (end_clean) {
                // WinHTTP: los dos cierres hechos; se suelta la conexion (fin limpio).
                clean_abort_done = true;
                engine->abort();
                break;  // el final del intento espera al lector (como mucho 2 s)
            }
            std::string error;
            bool sent_ok = true;
            if (have) {
                if (item.kind == OutKind::Close) {
                    bool skip = false;
                    {
                        std::lock_guard<std::mutex> lock(c->mu);
                        skip = c->close_sent;
                    }
                    if (!skip) {
                        c->deadline_write.store(nowMs() + std::max(1000, o.send_timeout_ms));
                        sent_ok = engine->sendClose(item.code, item.data, error);
                        c->deadline_write.store(0);
                        std::lock_guard<std::mutex> lock(c->mu);
                        c->close_sent = true;
                        c->close_queued = true;
                        c->deadline_close.store(nowMs() + std::max(100, o.close_timeout_ms));
                        if (c->state == WsState::Open) c->setState(WsState::Closing);
                    }
                } else if (item.kind == OutKind::Pong) {
                    bool after_close = false;
                    {
                        std::lock_guard<std::mutex> lock(c->mu);
                        after_close = c->close_sent;
                    }
                    if (after_close) continue;  // nada despues de la trama de cierre
                    c->deadline_write.store(nowMs() + std::max(1000, o.send_timeout_ms));
                    sent_ok = engine->sendPong(item.data, error);
                    c->deadline_write.store(0);
                } else {
                    const bool binary = item.kind == OutKind::Binary;
                    if (c->log >= 2) {
                        ws::wsLogLine("[WebSocket] " + o.url + " -> " +
                                      (binary ? std::string("(binario, ") + std::to_string(item.data.size()) + " bytes)"
                                              : clip(item.data, 200)));
                    }
                    c->deadline_write.store(nowMs() + std::max(1000, o.send_timeout_ms));
                    sent_ok = engine->sendMessage(item.data, binary, error);
                    c->deadline_write.store(0);
                    if (sent_ok) {
                        std::lock_guard<std::mutex> lock(c->mu);
                        c->stats.sent_messages++;
                        c->stats.sent_bytes += item.data.size();
                    }
                }
            } else if (do_ping) {
                next_ping = nowMs() + o.ping_interval_ms;
                if (pings) {
                    const std::int64_t stamp = nowMs();
                    std::string payload(8, '\0');
                    for (int i = 0; i < 8; ++i) {
                        payload[static_cast<std::size_t>(i)] = static_cast<char>((stamp >> (56 - 8 * i)) & 0xFF);
                    }
                    c->deadline_write.store(nowMs() + std::max(1000, o.send_timeout_ms));
                    sent_ok = engine->sendPing(payload, error);
                    c->deadline_write.store(0);
                }
                if (sent_ok && heartbeat) {
                    c->deadline_write.store(nowMs() + std::max(1000, o.send_timeout_ms));
                    sent_ok = engine->sendMessage(o.heartbeat_text, false, error);
                    c->deadline_write.store(0);
                }
            }
            if (!sent_ok) {
                // Envio fallido: la conexion se da por cortada (1006).
                engine->abort();
                std::lock_guard<std::mutex> lock(c->mu);
                c->reader_stop = true;
                c->cv_inbox.notify_all();
            }
        }

        // Fin del intento: esperar al lector (como mucho 2 s; el motor ya esta cortado).
        engine->abort();
        {
            std::unique_lock<std::mutex> lock(c->mu);
            c->reader_stop = true;
            c->cv_inbox.notify_all();
            c->cv_session.wait_for(lock, std::chrono::seconds(2), [&] { return c->reader_done; });
        }
        c->deadline_write.store(0);
        c->deadline_close.store(0);

        bool will = false;
        {
            std::lock_guard<std::mutex> lock(c->mu);
            const int reason = c->abort_reason.load();
            const bool received = c->close_received;
            const bool clean = c->close_sent && received && reason != kCloseTimeout && reason != kWriteTimeout;
            // Cierre nuestro por error de protocolo (1002/1007/1009): el motor RFC deja de
            // parsear tras el error y no ve el eco del servidor, asi que se informa el
            // codigo que mandamos (como hace WinHTTP), no 1006.
            const int code = received ? c->peer_code
                                      : (c->protocol_error != 0 && c->close_sent ? c->protocol_error : 1006);
            std::string why = received ? c->peer_reason : std::string();
            will = reconnectAfterClose(*c, received, code, reason);
            if (!received && !c->user_closed && !c->aborting.load()) {
                // Por que se corto.
                std::string text;
                if (reason == kIdle) {
                    text = "el servidor no responde (sin datos en " + std::to_string((c->idle_ms + 999) / 1000) + " s)";
                } else if (reason == kWriteTimeout) {
                    text = detail::kErrTimeout;
                } else if (c->protocol_error == 0 && !c->end_text.empty() && c->end_text != detail::kErrCut) {
                    text = c->end_text;  // detalle del motor; "se corto" ya lo dice el Close 1006
                }
                if (!text.empty()) {
                    c->log1(text);
                    c->pushError(text, will);
                }
            }
            std::size_t dropped = 0;
            if (!(will && o.resend_queued)) dropped = c->dropOut();
            c->log1("cerrada (" + std::to_string(code) + (why.empty() ? std::string() : ", " + clip(why, 123)) + ")");
            c->pushClose(code, std::move(why), clean, will, dropped);
            c->engine.reset();
        }
        if (!will) break;
        if (!backoff(*c, attempt, rng)) break;
    }

    {
        std::lock_guard<std::mutex> lock(c->mu);
        c->setState(WsState::Closed);
        if (!c->final_close_emitted) c->pushClose(1006, std::string(), false, false, c->dropOut());
        c->dropOut();
        c->priority.clear();
        c->engine.reset();
        c->closed_at = nowMs();
        c->session_ended = true;
    }
    c->deadline_connect.store(0);
    c->deadline_write.store(0);
    c->deadline_close.store(0);
    wakeAll(*c);
}

// Cierre pedido desde fuera (hilo principal o shutdownWebSockets).
void closeConn(WebSocket::Conn& c, int code, std::string reason) {
    // 1001 (Going Away, RFC 6455 7.4.1) no lo puede pedir un script (lo filtra la
    // API), pero si el motor: cambio de escena, fin del juego, ~WebSocket.
    if (code != 1001 && !ws::validSendCloseCode(code)) code = 1000;
    reason = truncateUtf8(reason, ws::kMaxCloseReason);
    bool abort_connect = false;
    {
        std::lock_guard<std::mutex> lock(c.mu);
        if (c.user_closed || c.state == WsState::Closed) return;
        c.user_closed = true;
        c.user_code = code;
        c.user_reason = std::move(reason);
        // Sin abrir aun: se corta el intento (no hay saludo de cierre que hacer).
        abort_connect = c.state == WsState::Connecting || c.state == WsState::Reconnecting;
    }
    if (abort_connect) abortEngine(c, kUser);
    wakeAll(c);
}

// Primera vez que el hub vio cerrada cada conexion (para collect()). Sin
// destruir (un ~WebSocket puede llegar durante la destruccion de estaticos).
std::mutex& g_seen_mu = *new std::mutex;
std::unordered_map<const WebSocket*, std::int64_t>& g_closed_seen =
    *new std::unordered_map<const WebSocket*, std::int64_t>;

}  // namespace

// --- WebSocket ----------------------------------------------------------------------

WebSocket::WebSocket(std::shared_ptr<Conn> conn) : conn_(std::move(conn)) {}

std::shared_ptr<WebSocket> WebSocket::open(WebSocketOptions options, std::string* error) {
    const auto fail = [&](const std::string& why) -> std::shared_ptr<WebSocket> {
        if (error != nullptr) *error = why;
        return nullptr;
    };
    g_log_level.store(g_ws_log.get(), std::memory_order_relaxed);

    int level = options.insecure_level >= 0 ? options.insecure_level : g_ws_insecure.get();
    level = std::clamp(level, 0, 2);
    if (level == 2 && !cvar::Registry::instance().cheatsAllowed()) {
        static std::once_flag warned;
        std::call_once(warned, [] {
            ws::wsLogLine("[WebSocket] net.WsInsecure = 2 solo con trucos permitidos: se usa 1 (red local)");
        });
        level = 1;
    }
    if (!checkWebSocketOptions(options, error)) return nullptr;
    if (!checkWebSocketUrl(options.url, level, error)) return nullptr;
    auto conn = std::make_shared<Conn>();
    if (!ws::parseWsUrl(options.url, conn->url, error)) return nullptr;

    // Limites.
    const std::size_t default_max = WebSocketOptions{}.max_message_bytes;
    if (options.max_message_bytes == 0 || options.max_message_bytes == default_max) {
        options.max_message_bytes = static_cast<std::size_t>(g_ws_max_msg_kb.get()) << 10;
    }
    options.max_message_bytes = std::clamp<std::size_t>(options.max_message_bytes, 1, kWsHardMaxMessage);
    options.max_inbox_bytes = std::clamp<std::size_t>(options.max_inbox_bytes, 64u << 10, kWsHardMaxQueue);
    options.max_send_queue_bytes = std::clamp<std::size_t>(options.max_send_queue_bytes, 64u << 10, kWsHardMaxQueue);
    options.connect_timeout_ms = std::clamp(options.connect_timeout_ms, 1000, 300000);
    options.send_timeout_ms = std::clamp(options.send_timeout_ms, 1000, 600000);
    options.close_timeout_ms = std::clamp(options.close_timeout_ms, 100, 60000);
    if (options.ping_interval_ms < 0) options.ping_interval_ms = 0;
    if (options.ping_interval_ms > 0) options.ping_interval_ms = std::max(options.ping_interval_ms, 1000);

#if defined(_WIN32)
    conn->rfc = options.force_rfc6455;
#else
    conn->rfc = true;
#endif
    if (options.idle_timeout_ms < 0) {
        const int from_ping =
            static_cast<int>(std::min<long long>(static_cast<long long>(options.ping_interval_ms) * 5 / 2, 1 << 30));
        if (conn->rfc || (!options.heartbeat_text.empty() && options.ping_interval_ms > 0)) {
            conn->idle_ms = std::max(from_ping, 45000);
        } else {
            conn->idle_ms = 0;  // WinHTTP sin latido: no hay forma de saber si el servidor sigue
        }
    } else {
        conn->idle_ms = options.idle_timeout_ms;
    }

    if (conn->url.secure || level >= 2) {
        conn->policy = ws::AddrPolicy::Any;
    } else if (detail::isLoopbackHost(conn->url.host)) {
        conn->policy = ws::AddrPolicy::LoopbackOnly;
    } else {
        conn->policy = ws::AddrPolicy::PrivateOnly;
    }
    conn->log = g_ws_log.get();
    options.insecure_level = level;
    conn->options = std::move(options);

    Registry& r = registry();
    {
        std::lock_guard<std::mutex> lock(r.mu);
        if (r.shut_down) return fail("la red del motor ya se cerro");
        int live = 0;
        for (const auto& w : r.conns) {
            if (auto c = w.lock()) {
                std::lock_guard<std::mutex> clock(c->mu);
                if (c->state != WsState::Closed) ++live;
            }
        }
        const int max = g_ws_max_conn.get();
        if (live >= max) {
            return fail("demasiadas conexiones WebSocket (maximo " + std::to_string(max) + ", net.WsMaxConnections)");
        }
        r.conns.push_back(conn);
    }
    startWatchdog();
    addThread();
    try {
        std::thread(sessionLoop, conn).detach();
    } catch (const std::system_error&) {
        removeThread();
        std::lock_guard<std::mutex> lock(conn->mu);
        conn->setState(WsState::Closed);
        conn->session_ended = true;
        conn->final_close_emitted = true;
        return fail("no se pudo crear el hilo de la conexion");
    }
    return std::shared_ptr<WebSocket>(new WebSocket(conn));
}

WebSocket::~WebSocket() {
    conn_->orphan.store(true);
    closeConn(*conn_, 1001, std::string());
    {
        std::lock_guard<std::mutex> lock(conn_->mu);
        conn_->inbox.clear();
        conn_->inbox_bytes = 0;
    }
    wakeAll(*conn_);
    std::lock_guard<std::mutex> lock(g_seen_mu);
    g_closed_seen.erase(this);
}

bool WebSocket::send(std::string data, bool binary, std::string* error) {
    const auto fail = [&](const std::string& why) {
        if (error != nullptr) *error = why;
        return false;
    };
    if (!binary && !validUtf8(data)) return fail("el texto no es UTF-8 valido; usa sendBinary");
    {
        std::lock_guard<std::mutex> lock(conn_->mu);
        Conn& c = *conn_;
        if (c.state != WsState::Open || c.user_closed || c.close_queued || c.close_sent || c.close_received) {
            const WsState shown = c.state == WsState::Open ? WsState::Closing : c.state;
            return fail(std::string("la conexion no esta abierta (estado: ") + wsStateName(shown) + ")");
        }
        if (c.out_bytes + data.size() > c.options.max_send_queue_bytes) {
            return fail("cola de envio llena (el servidor no lee o la red va lenta)");
        }
        Conn::Out o;
        o.kind = binary ? Conn::Out::Kind::Binary : Conn::Out::Kind::Text;
        c.out_bytes += data.size();
        o.data = std::move(data);
        c.out.push_back(std::move(o));
    }
    conn_->cv_session.notify_all();
    return true;
}

void WebSocket::close(int code, std::string reason) { closeConn(*conn_, code, std::move(reason)); }

void WebSocket::abort() {
    {
        std::lock_guard<std::mutex> lock(conn_->mu);
        if (conn_->state == WsState::Closed) return;
        conn_->user_closed = true;
    }
    conn_->aborting.store(true);
    abortEngine(*conn_, kUser);
}

bool WebSocket::setHeader(std::string name, std::string value, std::string* error) {
    if (!validHeader(name, value.empty() ? std::string("x") : value)) {
        if (error != nullptr) *error = "cabecera no valida: " + clip(name, 64);
        return false;
    }
    if (reservedHeader(name)) {
        if (error != nullptr) *error = "cabecera reservada del protocolo: " + name;
        return false;
    }
    std::lock_guard<std::mutex> lock(conn_->mu);
    HttpHeaders headers = conn_->options.headers;
    const std::string key = lowerText(name);
    headers.erase(std::remove_if(headers.begin(), headers.end(),
                                 [&](const std::pair<std::string, std::string>& h) { return lowerText(h.first) == key; }),
                  headers.end());
    if (!value.empty()) headers.emplace_back(std::move(name), std::move(value));
    if (!checkHeaders(headers, error)) return false;
    conn_->options.headers = std::move(headers);
    return true;
}

WsState WebSocket::state() const {
    std::lock_guard<std::mutex> lock(conn_->mu);
    return conn_->state;
}

WsStats WebSocket::stats() const {
    std::lock_guard<std::mutex> lock(conn_->mu);
    WsStats s = conn_->stats;
    s.state = conn_->state;
    s.queued_send_bytes = conn_->out_bytes;
    s.inbox_bytes = conn_->inbox_bytes;
    return s;
}

const std::string& WebSocket::url() const { return conn_->options.url; }  // no cambia tras open

std::vector<WsEvent> WebSocket::poll(std::size_t max) {
    std::vector<WsEvent> out;
    {
        std::lock_guard<std::mutex> lock(conn_->mu);
        Conn& c = *conn_;
        const std::size_t n = max == 0 ? c.inbox.size() : std::min(max, c.inbox.size());
        out.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            WsEvent& ev = c.inbox.front();
            c.inbox_bytes -= std::min(c.inbox_bytes, ev.data.size() + kEventOverhead);
            out.push_back(std::move(ev));
            c.inbox.pop_front();
        }
        if (c.inbox.empty()) c.inbox_bytes = 0;
    }
    if (!out.empty()) conn_->cv_inbox.notify_all();
    return out;
}

bool WebSocket::finished() const {
    std::lock_guard<std::mutex> lock(conn_->mu);
    return conn_->state == WsState::Closed && conn_->session_ended && conn_->inbox.empty();
}

// --- WebSocketHub -------------------------------------------------------------------

WebSocketHub::~WebSocketHub() { closeAll(1001, "Fin"); }

std::uint32_t WebSocketHub::open(WebSocketOptions options, const std::string& name, std::string* error, bool* reused) {
    if (reused != nullptr) *reused = false;
    if (!name.empty()) {
        if (const std::uint32_t id = find(name); id != 0) {
            if (reused != nullptr) *reused = true;
            return id;
        }
    }
    // net.WsMaxMessageKB si no se dio tamano (0 o el de serie); el resto lo recorta open().
    if (options.max_message_bytes == 0) options.max_message_bytes = WebSocketOptions{}.max_message_bytes;
    std::shared_ptr<WebSocket> ws = WebSocket::open(std::move(options), error);
    if (!ws) return 0;
    const std::uint32_t id = next_id_++;
    if (next_id_ == 0) next_id_ = 1;
    items_.emplace_back(id, Item{std::move(ws), name});
    return id;
}

std::shared_ptr<WebSocket> WebSocketHub::get(std::uint32_t id) const {
    for (const auto& [key, item] : items_) {
        if (key == id) return item.ws;
    }
    return nullptr;
}

std::uint32_t WebSocketHub::find(const std::string& name) const {
    if (name.empty()) return 0;
    for (const auto& [key, item] : items_) {
        if (item.name == name && item.ws->state() != WsState::Closed) return key;
    }
    return 0;
}

std::string WebSocketHub::name(std::uint32_t id) const {
    for (const auto& [key, item] : items_) {
        if (key == id) return item.name;
    }
    return {};
}

std::vector<std::uint32_t> WebSocketHub::ids() const {
    std::vector<std::uint32_t> out;
    out.reserve(items_.size());
    for (const auto& entry : items_) out.push_back(entry.first);
    return out;
}

void WebSocketHub::close(std::uint32_t id, int code, std::string reason) {
    if (auto ws = get(id)) ws->close(code, std::move(reason));
}

void WebSocketHub::closeAll(int code, const std::string& reason, bool unnamed_only) {
    for (const auto& [key, item] : items_) {
        if (unnamed_only && !item.name.empty()) continue;
        item.ws->close(code, reason);
    }
}

void WebSocketHub::collect() {
    const std::int64_t now = nowMs();
    // g_seen_mu solo dentro del predicado: al borrar se destruyen WebSocket y
    // ~WebSocket tambien la toma.
    items_.erase(std::remove_if(items_.begin(), items_.end(),
                                [&](const std::pair<std::uint32_t, Item>& entry) {
                                    const WebSocket* ws = entry.second.ws.get();
                                    if (ws->finished()) return true;
                                    if (ws->state() != WsState::Closed) return false;
                                    std::lock_guard<std::mutex> lock(g_seen_mu);
                                    auto [it, inserted] = g_closed_seen.emplace(ws, now);
                                    return !inserted && now - it->second > 30000;
                                }),
                 items_.end());
}

int WebSocketHub::count() const {
    int n = 0;
    for (const auto& entry : items_) {
        if (entry.second.ws->state() != WsState::Closed) ++n;
    }
    return n;
}

// --- Salida del programa ------------------------------------------------------------

void shutdownWebSockets(int wait_ms) {
    Registry& r = registry();
    {
        std::lock_guard<std::mutex> lock(r.mu);
        r.shut_down = true;
    }
    const std::vector<ConnPtr> conns = liveConns();
    for (const ConnPtr& c : conns) {
        c->orphan.store(true);
        closeConn(*c, 1001, "Fin del juego");
        {
            std::lock_guard<std::mutex> lock(c->mu);
            c->inbox.clear();
            c->inbox_bytes = 0;
        }
        wakeAll(*c);
    }
    if (waitThreads(wait_ms)) return;
    for (const ConnPtr& c : conns) {
        c->aborting.store(true);
        abortEngine(*c, kUser);
    }
    waitThreads(200);
}

}  // namespace cramion::net
