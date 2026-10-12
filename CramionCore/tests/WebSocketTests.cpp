// WebSocket del motor (net/WebSocket.h) contra un servidor de prueba en
// 127.0.0.1 (WsTestServer.h): eco de texto y binario, trozos con ping en medio,
// tamano maximo (1009), cierres del servidor y del cliente, cortes y
// reconexion, 401 sin bucle, subprotocolos, cabeceras, que nunca se queda
// colgado, sin datos (idle), rafagas con la cola de entrada llena, la politica
// de ws:// y 16 conexiones a la vez.
// En Windows todo se prueba dos veces: con WinHTTP y con el motor propio
// (force_rfc6455). En Linux, con el motor propio.
//   --online  tambien wss:// contra servidores reales (eco publico y un certificado caducado)
//   --lento   WinHTTP: 40 s de silencio sin idle timeout siguen abiertos

#include "WsTestServer.h"  // antes que nada (winsock2.h antes que windows.h)

#include "CramionCore/net/WebSocket.h"
#include "net/NetCommon.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace cramion;
using net::WsEvent;
using Kind = net::WsEvent::Kind;
using Clock = std::chrono::steady_clock;

namespace {

int checks = 0;
int failures = 0;
const char* g_engine = "";

void check(bool condition, const std::string& what) {
    ++checks;
    std::printf("  %s [%s] %s\n", condition ? "OK   " : "FALLO", g_engine, what.c_str());
    if (!condition) ++failures;
}

void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

long long msSince(Clock::time_point t0) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
}

const char* kindName(Kind k) {
    switch (k) {
        case Kind::Open: return "Open";
        case Kind::Text: return "Text";
        case Kind::Binary: return "Binary";
        case Kind::Close: return "Close";
        case Kind::Error: return "Error";
        case Kind::Reconnecting: return "Reconnecting";
    }
    return "?";
}

// Una conexion y todos sus eventos recogidos.
struct Client {
    std::shared_ptr<net::WebSocket> ws;
    std::string error;            // de open()
    std::vector<WsEvent> events;  // todo lo recogido con poll()
    std::size_t cursor = 0;       // lo ya consumido por next()

    explicit operator bool() const { return ws != nullptr; }

    void pump() {
        if (!ws) return;
        for (WsEvent& e : ws->poll()) {
            if (e.kind == Kind::Error || e.kind == Kind::Close || e.kind == Kind::Reconnecting) {
                std::printf("    evento %s code=%d status=%d clean=%d reconecta=%d '%s'\n", kindName(e.kind), e.code,
                            e.http_status, e.clean ? 1 : 0, e.will_reconnect ? 1 : 0,
                            e.data.size() > 120 ? (e.data.substr(0, 120) + "...").c_str() : e.data.c_str());
            }
            events.push_back(std::move(e));
        }
    }
    // El siguiente evento de ese tipo (salta los de otros tipos), esperando hasta ms.
    std::optional<WsEvent> next(Kind kind, int ms) {
        const auto end = Clock::now() + std::chrono::milliseconds(ms);
        for (;;) {
            pump();
            while (cursor < events.size()) {
                const std::size_t at = cursor++;
                if (events[at].kind == kind) return events[at];
            }
            if (Clock::now() >= end) return std::nullopt;
            sleepMs(5);
        }
    }
    int count(Kind kind) const {
        int n = 0;
        for (const WsEvent& e : events) n += e.kind == kind ? 1 : 0;
        return n;
    }
};

net::WebSocketOptions options(const std::string& url, bool rfc) {
    net::WebSocketOptions o;
    o.url = url;
    o.force_rfc6455 = rfc;
    return o;
}

// open() con reintento si aun quedan conexiones de antes cerrandose (net.WsMaxConnections).
Client openWs(net::WebSocketOptions o) {
    Client c;
    const auto end = Clock::now() + std::chrono::seconds(3);
    for (;;) {
        c.error.clear();
        c.ws = net::WebSocket::open(o, &c.error);
        if (c.ws || c.error.find("demasiadas") == std::string::npos || Clock::now() >= end) break;
        sleepMs(50);
    }
    if (!c.ws) std::printf("    open(%s): %s\n", o.url.c_str(), c.error.c_str());
    return c;
}

std::string bytes(std::size_t n) {
    std::string s(n, '\0');
    for (std::size_t i = 0; i < n; ++i) s[i] = static_cast<char>((i * 31 + 7) & 0xFF);
    if (n > 0) s[0] = '\0';
    if (n > 2) s[n / 2] = '\0';
    return s;
}

// Todos los casos con un motor (rfc = force_rfc6455).
void runEngine(wstest::WsTestServer& server, bool rfc) {
    server.clear();

    std::printf("Eco\n");
    {
        Client c = openWs(options(server.url("/eco"), rfc));
        const auto opened = c.next(Kind::Open, 5000);
        check(opened.has_value(), "abre (Open)");
        const std::string text = "hola \xC3\xB1 y \xF0\x9F\x98\x80";
        std::string error;
        check(c && c.ws->send(text, false, &error), "send de texto");
        const auto echo = c.next(Kind::Text, 3000);
        check(echo && echo->data == text, "1. eco de texto con \xC3\xB1 y un emoji");

        bool all = true;
        for (const std::size_t n : {std::size_t(0), std::size_t(125), std::size_t(126), std::size_t(65535),
                                    std::size_t(65536), std::size_t(1) << 20}) {
            const std::string data = bytes(n);
            if (!c || !c.ws->send(data, true, &error)) {
                std::printf("    send binario de %zu: %s\n", n, error.c_str());
                all = false;
                continue;
            }
            const auto got = c.next(Kind::Binary, 5000);
            const bool same = got && got->data == data;
            if (!same) std::printf("    binario de %zu bytes: %s\n", n, got ? "distinto" : "no llego");
            all = all && same;
        }
        check(all, "2. eco binario de 0, 125, 126, 65535, 65536 y 1 MB (con \\0)");
        if (c) {
            const net::WsStats s = c.ws->stats();
            check(s.state == net::WsState::Open && s.sent_messages == 7 && s.received_messages == 7, "stats: 7 enviados, 7 recibidos");
        }
        check(server.unmaskedFrames() == 0, "todas las tramas del cliente con mascara");
    }

    std::printf("Trozos con ping en medio\n");
    {
        Client c = openWs(options(server.url("/frag"), rfc));
        const auto m = c.next(Kind::Text, 5000);
        check(m && m->data == "uno dos tres", "3. el mensaje en 3 trozos llega entero");
        if (rfc) {
            bool pong = false;
            for (int i = 0; i < 200 && !pong; ++i) {
                pong = server.sawPong("pp");
                if (!pong) sleepMs(10);
            }
            check(pong, "3. el ping de en medio tiene su pong con el mismo contenido");
        }
    }

    std::printf("Tamano maximo\n");
    {
        net::WebSocketOptions o = options(server.url("/grande?n=5000"), rfc);
        o.max_message_bytes = 1000;
        Client c = openWs(o);
        const auto e = c.next(Kind::Error, 5000);
        check(e.has_value() && !e->will_reconnect, "4. mensaje de 5000 con maxSize 1000: Error");
        const auto cl = c.next(Kind::Close, 5000);
        check(cl && cl->code == 1009 && !cl->will_reconnect, "4. y Close 1009, sin reconectar");
        sleepMs(800);
        c.pump();
        check(c.count(Kind::Reconnecting) == 0 && server.connections("/grande?n=5000") == 1, "4. una sola conexion");
    }

    std::printf("Cierre del servidor\n");
    {
        Client c = openWs(options(server.url("/cerrar4001"), rfc));
        const auto cl = c.next(Kind::Close, 5000);
        check(cl && cl->code == 4001 && cl->clean && cl->data == "baneado", "5. Close 4001 \"baneado\", clean");
        check(cl && !cl->will_reconnect, "5. 4000-4999 sin on_app_codes: no reconecta");
    }

    std::printf("Corte sin cierre\n");
    {
        Client c = openWs(options(server.url("/cortar"), rfc));
        check(c.next(Kind::Open, 5000).has_value(), "6. abre");
        const auto cl = c.next(Kind::Close, 3000);
        const auto t0 = Clock::now();
        check(cl && cl->code == 1006 && !cl->clean && cl->will_reconnect, "6. Close 1006 (cortada) y va a reconectar");
        const auto re = c.next(Kind::Reconnecting, 3000);
        check(re && re->code == 1 && re->delay_ms > 0, "6. Reconnecting (intento 1)");
        const auto again = c.next(Kind::Open, 3000);
        check(again.has_value() && msSince(t0) < 3000, "6. vuelve a abrir en menos de 3 s");
        if (c) c.ws->close();
    }

    std::printf("HTTP 401\n");
    {
        net::WebSocketOptions o = options(server.url("/401"), rfc);
        const auto t0 = Clock::now();
        Client c = openWs(o);
        const auto e = c.next(Kind::Error, 5000);
        check(e && e->http_status == 401 && !e->will_reconnect, "7. Error con http_status 401, sin reconectar");
        const long long left = 3000 - msSince(t0);
        if (left > 0) sleepMs(static_cast<int>(left));
        c.pump();
        check(server.connections("/401") == 1 && c.count(Kind::Open) == 0 && c.count(Kind::Reconnecting) == 0,
              "7. un solo intento en 3 s (no hay bucle)");
        const auto cl = c.next(Kind::Close, 1000);
        check(cl && cl->code == 1006 && !cl->will_reconnect, "7. ultimo evento: Close 1006");
    }

    std::printf("Subprotocolos\n");
    {
        net::WebSocketOptions o = options(server.url("/proto"), rfc);
        o.protocols = {"chat", "json"};
        Client c = openWs(o);
        const auto opened = c.next(Kind::Open, 5000);
        check(opened && opened->protocol == "chat", "8. /proto: el servidor elige \"chat\" (Open.protocol)");
        check(c && c.ws->stats().protocol == "chat", "8. y stats().protocol");
        check(server.lastHeader("/proto", "sec-websocket-protocol") == "chat, json", "8. se piden los dos");

        net::WebSocketOptions bad = options(server.url("/proto?mal"), rfc);
        bad.protocols = {"chat"};
        Client b = openWs(bad);
        const auto e = b.next(Kind::Error, 5000);
        check(e && !e->will_reconnect && b.count(Kind::Open) == 0, "8. /proto?mal: Error fatal (subprotocolo no pedido)");
    }

    std::printf("Cabeceras\n");
    {
        net::WebSocketOptions o = options(server.url("/cabeceras"), rfc);
        o.headers = {{"Authorization", "Bearer abc123"}, {"X-Juego", "Cramion"}};
        Client c = openWs(o);
        const auto m = c.next(Kind::Text, 5000);
        check(m && m->data.find("\"authorization\":\"Bearer abc123\"") != std::string::npos &&
                  m->data.find("\"x-juego\":\"Cramion\"") != std::string::npos,
              "9. Authorization y una cabecera propia llegan al servidor");
        check(server.lastHeader("/cabeceras", "sec-websocket-version") == "13", "9. Sec-WebSocket-Version: 13");

        std::string error;
        net::WebSocketOptions inject = options(server.url("/eco"), rfc);
        inject.headers = {{"X-Malo", "a\r\nX-Otra: b"}};
        check(!net::checkWebSocketOptions(inject, &error) && net::WebSocket::open(inject, &error) == nullptr && !error.empty(),
              "9. cabecera con \\r\\n: se rechaza al momento");
        net::WebSocketOptions host = options(server.url("/eco"), rfc);
        host.headers = {{"Host", "otro.com"}};
        error.clear();
        check(net::WebSocket::open(host, &error) == nullptr && error.find("reservada") != std::string::npos,
              "9. Host es reservada: se rechaza al momento");
        net::WebSocketOptions proto = options(server.url("/eco"), rfc);
        proto.protocols = {"con espacio"};
        error.clear();
        check(net::WebSocket::open(proto, &error) == nullptr && !error.empty(), "9. subprotocolo que no es un token: no");
    }

    std::printf("Cierre del cliente\n");
    {
        Client c = openWs(options(server.url("/eco"), rfc));
        check(c.next(Kind::Open, 5000).has_value(), "10. abre");
        if (c) c.ws->close(1000, "adios");
        const auto cl = c.next(Kind::Close, 5000);
        check(cl && cl->code == 1000 && cl->clean && !cl->will_reconnect, "10. Close 1000 clean, sin reconectar");
        bool seen = false;
        for (int i = 0; i < 200 && !seen; ++i) {
            seen = server.sawClose(1000);
            if (!seen) sleepMs(10);
        }
        check(seen, "10. el servidor recibe el cierre 1000");
        // El hilo de sesion pone Closed justo despues de dejar el evento Close.
        for (int i = 0; i < 100 && c && !c.ws->finished(); ++i) sleepMs(10);
        check(c && c.ws->state() == net::WsState::Closed, "10. state() = Closed");
        std::string error;
        check(c && !c.ws->send("tarde", false, &error) && !error.empty(), "10. send tras cerrar: false con motivo");
        check(c && c.ws->finished(), "10. finished() tras recoger el ultimo evento");
    }

    std::printf("Sin datos\n");
    {
        net::WebSocketOptions o = options(server.url("/mudo"), rfc);
        o.idle_timeout_ms = 1000;
        o.reconnect.enabled = false;
        Client c = openWs(o);
        check(c.next(Kind::Open, 5000).has_value(), "12. abre");
        const auto t0 = Clock::now();
        const auto cl = c.next(Kind::Close, 4000);
        const long long ms = msSince(t0);
        std::printf("    cerrada a los %lld ms\n", ms);
        check(cl && cl->code == 1006 && ms >= 800 && ms <= 2000, "12. idle 1 s: Close 1006 entre 0.8 y 2 s");
        check(c.count(Kind::Error) >= 1, "12. con un Error que dice por que");
    }

    std::printf("Rafaga\n");
    {
        net::WebSocketOptions o = options(server.url("/rafaga"), rfc);
        o.max_inbox_bytes = 64u << 10;
        o.reconnect.enabled = false;
        Client c = openWs(o);
        sleepMs(2000);  // sin recoger: el lector se para con la cola llena
        const std::size_t inbox = c ? c.ws->stats().inbox_bytes : 0;
        std::printf("    cola de entrada: %zu bytes\n", inbox);
        check(c && inbox <= (64u << 10), "13. 2 s sin poll: la cola de entrada no pasa del tope (64 KB)");
        std::vector<std::string> got;
        const auto end = Clock::now() + std::chrono::seconds(15);
        while (got.size() < 1000 && Clock::now() < end) {
            const auto m = c.next(Kind::Text, 50);
            if (m) got.push_back(m->data);
        }
        bool ordered = got.size() == 1000;
        for (std::size_t i = 0; ordered && i < got.size(); ++i) {
            char number[8];
            std::snprintf(number, sizeof(number), "%04zu", i);
            ordered = got[i].size() == 1000 && got[i].compare(0, 4, number) == 0;
        }
        std::printf("    recibidos: %zu\n", got.size());
        check(ordered, "13. llegan los 1000 mensajes, en orden (no se pierde nada)");
    }

    std::printf("16 a la vez\n");
    {
        std::vector<Client> clients;
        for (int k = 0; k < 16; ++k) clients.push_back(openWs(options(server.url("/eco"), rfc)));
        bool opened = true;
        for (Client& c : clients) opened = opened && c.next(Kind::Open, 5000).has_value();
        check(opened, "15. 16 conexiones abiertas");
        bool sent = true;
        for (int i = 0; i < 100; ++i) {
            for (int k = 0; k < 16; ++k) {
                if (!clients[k]) {
                    sent = false;
                    continue;
                }
                sent = clients[k].ws->send("c" + std::to_string(k) + "-" + std::to_string(i), false, nullptr) && sent;
            }
        }
        check(sent, "15. 1600 envios encolados");
        bool all = true;
        for (int k = 0; k < 16; ++k) {
            for (int i = 0; i < 100 && all; ++i) {
                const auto m = clients[k].next(Kind::Text, 5000);
                all = m && m->data == "c" + std::to_string(k) + "-" + std::to_string(i);
            }
        }
        check(all, "15. cada una recibe su eco de 100 mensajes, en orden");
        for (Client& c : clients) {
            if (c) c.ws->close();
        }
    }
    check(server.unmaskedFrames() == 0, "ninguna trama del cliente sin mascara");
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    bool online = false;
    bool slow = false;
    for (int i = 1; i < argc; ++i) {
        online = online || std::strcmp(argv[i], "--online") == 0;
        slow = slow || std::strcmp(argv[i], "--lento") == 0;
    }

    wstest::WsTestServer server;
    std::printf("Servidor de prueba en el puerto %d\n", server.port());

    std::printf("Direcciones (politica)\n");
    g_engine = "politica";
    {
        std::string error;
        check(net::checkWebSocketUrl("ws://localhost:8080/chat", 0, &error) && net::checkWebSocketUrl("ws://127.0.0.1/", 0, &error),
              "ws:// a este PC: si");
        check(net::checkWebSocketUrl("wss://juego.ejemplo.com/ws?t=1", 0, &error), "wss:// a Internet: si");
        error.clear();
        check(!net::checkWebSocketUrl("ws://8.8.8.8/", 0, &error) && !error.empty(), "14. ws://8.8.8.8 con nivel 0: no");
        check(!net::checkWebSocketUrl("http://localhost/", 0, &error) && !net::checkWebSocketUrl("ftp://x/", 0, &error),
              "otros esquemas: no");
        check(!net::checkWebSocketUrl("ws://usuario:clave@localhost/", 0, &error), "usuario:clave@ en la direccion: no");
        check(!net::checkWebSocketUrl("ws://localhost:0/", 0, &error) && !net::checkWebSocketUrl("ws://localhost:70000/", 0, &error),
              "puerto fuera de 1-65535: no");

        net::WebSocketOptions o = options("ws://8.8.8.8:80/", false);
        o.insecure_level = 1;
        o.reconnect.enabled = false;
        o.connect_timeout_ms = 3000;
        Client c = openWs(o);
        bool not_local = !c && c.error == net::detail::kErrNotLocal;
        if (c) {
            const auto e = c.next(Kind::Error, 5000);
            not_local = e && e->data == net::detail::kErrNotLocal && !e->will_reconnect;
        }
        check(not_local, "14. nivel 1 (red local) y una IP publica: kErrNotLocal");
    }

#if defined(_WIN32)
    g_engine = "WinHTTP";
    runEngine(server, false);
    g_engine = "RFC 6455";
    runEngine(server, true);
#else
    g_engine = "RFC 6455";
    runEngine(server, false);
#endif

    std::printf("Nunca se queda colgado\n");
    g_engine = "hub";
    {
        std::string error;
        long long destroy_ms = 0;
        {
            auto first = std::make_unique<net::WebSocketHub>();
            const std::uint32_t a = first->open(options(server.url("/lento"), false), "", &error);
            net::WebSocketOptions remote = options("ws://10.255.255.1:81/", false);
            remote.insecure_level = 1;
            const std::uint32_t b = first->open(remote, "", &error);
            check(a > 0 && b > 0 && first->count() == 2, "11. dos conectando (/lento y 10.255.255.1)");
            sleepMs(150);
            const auto t0 = Clock::now();
            first.reset();
            destroy_ms = msSince(t0);
        }
        std::printf("    destruir el hub: %lld ms\n", destroy_ms);
        check(destroy_ms < 50, "11. destruir un hub con conexiones a medias: < 50 ms");

        net::WebSocketHub hub;
        const std::uint32_t a = hub.open(options(server.url("/lento"), false), "lento", &error);
        net::WebSocketOptions remote = options("ws://10.255.255.1:81/", false);
        remote.insecure_level = 1;
        hub.open(remote, "", &error);
        bool reused = false;
        check(a > 0 && hub.open(options(server.url("/lento"), false), "lento", &error, &reused) == a && reused &&
                  hub.find("lento") == a && hub.name(a) == "lento",
              "11. con nombre: la segunda vez devuelve la misma (reused)");
        sleepMs(150);
        const auto t0 = Clock::now();
        hub.closeAll(1001, "Fin");
        const long long close_ms = msSince(t0);
        std::printf("    closeAll: %lld ms\n", close_ms);
        check(close_ms < 50, "11. closeAll con conexiones a medias: < 50 ms");
    }

    if (online) {
        std::printf("En linea\n");
        g_engine = "online";
        {
            net::WebSocketOptions o = options("wss://echo.websocket.org/", false);
            o.reconnect.enabled = false;
            Client c = openWs(o);
            const bool opened = c.next(Kind::Open, 15000).has_value();
            check(opened, "16. wss:// real (eco publico): abre");
            if (opened) c.ws->send("hola desde Cramion", false, nullptr);
            bool echo = false;
            const auto end = Clock::now() + std::chrono::seconds(10);
            while (opened && !echo && Clock::now() < end) {
                const auto m = c.next(Kind::Text, 200);
                echo = m && m->data == "hola desde Cramion";
            }
            check(echo, "16. y devuelve el eco");
            if (c) c.ws->close();
        }
        {
            net::WebSocketOptions o = options("wss://expired.badssl.com/", false);
            o.reconnect.enabled = false;
            Client c = openWs(o);
            const auto e = c.next(Kind::Error, 15000);
            std::printf("    expired.badssl.com -> %s\n", e ? e->data.c_str() : "(nada)");
            check(e && e->data == net::detail::kErrCert && !e->will_reconnect && c.count(Kind::Open) == 0,
                  "16. certificado caducado: kErrCert, sin reconectar");
        }
    }

#if defined(_WIN32)
    if (slow) {
        std::printf("40 s de silencio (WinHTTP, sin idle timeout)\n");
        g_engine = "WinHTTP";
        net::WebSocketOptions o = options(server.url("/mudo"), false);
        o.idle_timeout_ms = 0;
        o.reconnect.enabled = false;
        Client c = openWs(o);
        check(c.next(Kind::Open, 5000).has_value(), "12. abre");
        sleepMs(40000);
        c.pump();
        check(c && c.ws->state() == net::WsState::Open && c.count(Kind::Close) == 0,
              "12. 40 s sin datos: sigue abierta (WinHTTP no corta por RECEIVE_TIMEOUT)");
        if (c) c.ws->close();
    }
#else
    (void)slow;
#endif

    std::printf("Al salir del programa\n");
    g_engine = "shutdown";
    {
        Client lento = openWs(options(server.url("/lento"), false));
        Client eco = openWs(options(server.url("/eco"), false));
        eco.next(Kind::Open, 5000);
        sleepMs(100);
        const auto t0 = Clock::now();
        net::shutdownWebSockets(500);
        const long long ms = msSince(t0);
        std::printf("    shutdownWebSockets(500): %lld ms\n", ms);
        check(ms < 800, "11. shutdownWebSockets(500) vuelve en menos de 800 ms");
        check(eco && eco.ws->state() == net::WsState::Closed && lento && lento.ws->state() == net::WsState::Closed,
              "11. y todo queda cerrado");
        std::string error;
        check(net::WebSocket::open(options(server.url("/eco"), false), &error) == nullptr &&
                  error.find("ya se cerro") != std::string::npos,
              "despues, open() falla: \"la red del motor ya se cerro\"");
    }
    check(server.acceptMismatches() == 0, "Sec-WebSocket-Accept: net::sha1 = BCrypt (Windows)");

    server.stop();
    std::printf("\n%d comprobaciones, %d fallos\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
