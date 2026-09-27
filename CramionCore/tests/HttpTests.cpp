// HTTPS desde el motor: direcciones permitidas, cabeceras, peticiones contra
// un servidor HTTP de prueba en 127.0.0.1 (GET, POST, estados, limites de
// tamano y de tiempo, redirecciones), el cliente asincrono y la API de Lua
// (Http y Json). Con --online tambien contra servidores reales: GitHub
// (https valido) y badssl.com (certificados malos: deben fallar).

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include "CramionCore/ecs/World.h"
#include "CramionCore/net/Http.h"
#include "CramionCore/scripting/Scripting.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using namespace cramion;

namespace {

int checks = 0;
int failures = 0;

void check(bool condition, const char* what) {
    ++checks;
    std::printf("  %s %s\n", condition ? "OK   " : "FALLO", what);
    if (!condition) ++failures;
}

// Servidor HTTP/1.1 minimo (una peticion por conexion).
class TestServer {
public:
    TestServer() {
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
        listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        bind(listener_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        int len = sizeof(addr);
        getsockname(listener_, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ntohs(addr.sin_port);
        listen(listener_, 16);
        thread_ = std::thread([this] { loop(); });
    }
    ~TestServer() {
        stop_ = true;
        closesocket(listener_);
        if (thread_.joinable()) thread_.join();
        WSACleanup();
    }
    std::string url(const std::string& path) const { return "http://127.0.0.1:" + std::to_string(port_) + path; }
    int port() const { return port_; }

private:
    SOCKET listener_ = INVALID_SOCKET;
    int port_ = 0;
    std::atomic<bool> stop_{false};
    std::thread thread_;

    void loop() {
        while (!stop_) {
            SOCKET c = accept(listener_, nullptr, nullptr);
            if (c == INVALID_SOCKET) break;
            std::thread([c] { handle(c); }).detach();
        }
    }

    static std::string header(const std::string& head, const std::string& name) {
        std::string lower = head;
        for (char& ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        const std::size_t at = lower.find("\r\n" + name + ":");
        if (at == std::string::npos) return {};
        std::size_t start = at + 3 + name.size();
        while (start < head.size() && head[start] == ' ') ++start;
        return head.substr(start, head.find("\r\n", start) - start);
    }

    static std::string escape(const std::string& s) {
        std::string out;
        for (const char ch : s) {
            if (ch == '"' || ch == '\\') out += '\\';
            out += ch;
        }
        return out;
    }

    static void handle(SOCKET c) {
        std::string data;
        char buffer[4096];
        std::size_t head_end = std::string::npos;
        while (head_end == std::string::npos) {
            const int got = recv(c, buffer, sizeof(buffer), 0);
            if (got <= 0) {
                closesocket(c);
                return;
            }
            data.append(buffer, static_cast<std::size_t>(got));
            head_end = data.find("\r\n\r\n");
        }
        const std::string head = data.substr(0, head_end);
        const std::size_t length = static_cast<std::size_t>(std::atoi(header(head, "content-length").c_str()));
        std::string body = data.substr(head_end + 4);
        while (body.size() < length) {
            const int got = recv(c, buffer, sizeof(buffer), 0);
            if (got <= 0) break;
            body.append(buffer, static_cast<std::size_t>(got));
        }
        const std::string line = head.substr(0, head.find("\r\n"));
        const std::string method = line.substr(0, line.find(' '));
        const std::string path = line.substr(method.size() + 1, line.rfind(' ') - method.size() - 1);

        int status = 200;
        std::string type = "text/plain";
        std::string out;
        std::string extra;
        if (path == "/hola") {
            out = "hola";
        } else if (path.rfind("/eco", 0) == 0) {
            type = "application/json";
            out = "{\"method\":\"" + method + "\",\"path\":\"" + escape(path) + "\",\"type\":\"" +
                  escape(header(head, "content-type")) + "\",\"prueba\":\"" + escape(header(head, "x-prueba")) +
                  "\",\"body\":\"" + escape(body) + "\",\"lista\":[1,2.5,\"tres\",true,null],\"sub\":{\"a\":1}}";
        } else if (path == "/grande") {
            out.assign(3u << 20, 'a');
        } else if (path == "/lento") {
            std::this_thread::sleep_for(std::chrono::milliseconds(2500));
            out = "tarde";
        } else if (path == "/redir") {
            status = 302;
            extra = "Location: http://127.0.0.1:1/otro\r\n";
        } else {
            status = 404;
            out = "no existe";
        }
        const std::string response = "HTTP/1.1 " + std::to_string(status) + (status == 200 ? " OK" : " X") +
                                     "\r\nContent-Type: " + type + "\r\nContent-Length: " + std::to_string(out.size()) +
                                     "\r\nX-Servidor: prueba\r\n" + extra + "Connection: close\r\n\r\n" + out;
        std::size_t sent = 0;
        while (sent < response.size()) {
            const int n = send(c, response.data() + sent, static_cast<int>(std::min<std::size_t>(response.size() - sent, 1 << 16)), 0);
            if (n <= 0) break;
            sent += static_cast<std::size_t>(n);
        }
        shutdown(c, SD_SEND);
        closesocket(c);
    }
};

std::string headerOf(const net::HttpResponse& r, const std::string& name) {
    for (const auto& [n, v] : r.headers) {
        if (n == name) return v;
    }
    return {};
}

bool hasLog(const std::vector<std::string>& log, const std::string& text) {
    for (const std::string& l : log) {
        if (l.find(text) != std::string::npos) return true;
    }
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    AddVectoredExceptionHandler(1, [](EXCEPTION_POINTERS* info) -> LONG {
        const DWORD code = info->ExceptionRecord->ExceptionCode;
        if (code != EXCEPTION_ACCESS_VIOLATION && code != 0xC0000409) return EXCEPTION_CONTINUE_SEARCH;
        const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        std::printf("CRASH 0x%lx en +0x%llx\n", code,
                    static_cast<unsigned long long>(info->ContextRecord->Rip - base));
        void* frames[40];
        const USHORT n = RtlCaptureStackBackTrace(0, 40, frames, nullptr);
        for (USHORT i = 0; i < n; ++i) {
            std::printf("  +0x%llx\n", static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(frames[i]) - base));
        }
        return EXCEPTION_CONTINUE_SEARCH;
    });
    const bool online = argc > 1 && std::strcmp(argv[1], "--online") == 0;

    std::printf("Direcciones y cabeceras\n");
    std::string error;
    check(net::checkUrl("https://api.ejemplo.com/v1/puntos?x=1", &error), "https:// se permite");
    check(!net::checkUrl("http://api.ejemplo.com/", &error) && error.find("https") != std::string::npos,
          "http:// a Internet no (sin cifrar)");
    check(net::checkUrl("http://localhost:8080/", &error) && net::checkUrl("http://127.0.0.1/", &error),
          "http:// a localhost si (pruebas)");
    check(!net::checkUrl("ftp://ejemplo.com/", &error) && !net::checkUrl("file:///C:/x.txt", &error), "otros esquemas no");
    check(!net::checkUrl("https://ejemplo.com/con espacio", &error) && !net::checkUrl("", &error),
          "espacios o vacia no");
    check(net::validHeader("Authorization", "Bearer abc") && !net::validHeader("X-A", "a\r\nX-B: b") &&
              !net::validHeader("Mal Nombre", "x") && !net::validHeader("", "x"),
          "cabeceras: sin saltos de linea (no se inyectan otras)");
    check(net::urlEncode("hola mundo/\xC3\xB1&=") == "hola%20mundo%2F%C3%B1%26%3D", "urlEncode (UTF-8)");

    TestServer server;
    std::printf("Peticiones (servidor de prueba en el puerto %d)\n", server.port());
    {
        net::HttpRequest req;
        req.url = server.url("/hola");
        const net::HttpResponse r = net::httpRequest(req);
        check(r.ok && r.status == 200 && r.body == "hola" && r.error.empty(), "GET: estado 200 y cuerpo");
        check(headerOf(r, "x-servidor") == "prueba" && headerOf(r, "content-type") == "text/plain",
              "cabeceras de la respuesta (en minusculas)");
    }
    {
        net::HttpRequest req;
        req.method = "POST";
        req.url = server.url("/eco?q=1");
        req.body = "{\"puntos\":42}";
        req.headers = {{"Content-Type", "application/json"}, {"X-Prueba", "valor 1"}};
        const net::HttpResponse r = net::httpRequest(req);
        check(r.ok && r.body.find("\"method\":\"POST\"") != std::string::npos &&
                  r.body.find("\"path\":\"/eco?q=1\"") != std::string::npos,
              "POST: metodo y ruta con la consulta");
        check(r.body.find("\"body\":\"{\\\"puntos\\\":42}\"") != std::string::npos &&
                  r.body.find("\"prueba\":\"valor 1\"") != std::string::npos &&
                  r.body.find("\"type\":\"application/json\"") != std::string::npos,
              "POST: cuerpo y cabeceras llegan al servidor");
    }
    {
        net::HttpRequest req;
        req.url = server.url("/nada");
        const net::HttpResponse r = net::httpRequest(req);
        check(!r.ok && r.status == 404 && r.error.empty() && r.body == "no existe", "404: respuesta sin ok (pero con cuerpo)");
    }
    {
        net::HttpRequest req;
        req.url = server.url("/grande");
        req.max_response_bytes = 1u << 20;
        const net::HttpResponse r = net::httpRequest(req);
        check(!r.ok && r.error.find("maximo") != std::string::npos, "respuesta de 3 MB con maximo 1 MB: se corta");
        req.max_response_bytes = 8u << 20;
        check(net::httpRequest(req).body.size() == (3u << 20), "y con 8 MB llega entera");
    }
    {
        net::HttpRequest req;
        req.url = server.url("/lento");
        req.timeout_ms = 1000;
        const net::HttpResponse r = net::httpRequest(req);
        std::printf("    lento: %d '%s' %.2f s\n", r.status, r.error.c_str(), r.seconds);
        check(!r.ok && r.status == 0 && r.error.find("tiempo") != std::string::npos && r.seconds < 2.4,
              "tiempo maximo: un servidor lento da error, no se queda colgado");
    }
    {
        net::HttpRequest req;
        req.url = server.url("/redir");
        const net::HttpResponse r = net::httpRequest(req);
        check(r.status == 302, "http a localhost: no sigue redirecciones (podrian sacar la peticion fuera)");
    }
    {
        net::HttpRequest req;
        req.url = "http://127.0.0.1:1/";
        const net::HttpResponse r = net::httpRequest(req);
        check(!r.ok && r.status == 0 && !r.error.empty(), "sin servidor: error con motivo");
        req.url = "http://ejemplo.com/";
        check(!net::httpRequest(req).error.empty(), "http a Internet: ni se intenta");
        req.url = server.url("/hola");
        req.method = "GET\r\nX";
        check(!net::httpRequest(req).error.empty(), "metodo con caracteres raros: no");
    }

    std::printf("Cliente asincrono\n");
    {
        net::HttpClient client(2);
        std::vector<std::uint32_t> ids;
        for (int i = 0; i < 5; ++i) {
            net::HttpRequest req;
            req.url = server.url("/eco/" + std::to_string(i));
            ids.push_back(client.send(req));
        }
        check(client.pending() == 5, "5 peticiones pendientes (2 a la vez)");
        std::vector<std::pair<std::uint32_t, net::HttpResponse>> got;
        for (int i = 0; i < 500 && got.size() < 5; ++i) {
            for (auto& r : client.poll()) got.push_back(std::move(r));
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        bool all = got.size() == 5;
        for (const auto& [id, r] : got) {
            const std::size_t index = static_cast<std::size_t>(std::find(ids.begin(), ids.end(), id) - ids.begin());
            all = all && r.ok && r.body.find("\"/eco/" + std::to_string(index) + "\"") != std::string::npos;
        }
        check(all, "llegan las 5, cada una con su id");
        net::HttpRequest slow;
        slow.url = server.url("/lento");
        client.send(slow);
        client.cancelAll();
        std::this_thread::sleep_for(std::chrono::milliseconds(3000));
        check(client.poll().empty(), "cancelAll: lo pendiente ya no se entrega");
    }

    std::printf("Lua: Http y Json\n");
    {
        const std::filesystem::path root = std::filesystem::temp_directory_path() / "cramion_http_tests";
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root / "Scripts");
        std::ofstream(root / "Scripts" / "Web.lua") << R"lua(local Web = {}
function Web:Start()
    local base = "http://127.0.0.1:" .. PORT
    Http.get(base .. "/hola", function(r) print("get " .. tostring(r.ok) .. " " .. r.status .. " " .. r.body) end)
    Http.post(base .. "/eco", { nombre = "Ana", puntos = 42, lista = {1, 2, 3} }, function(r)
        local e = r.data
        print("post " .. e.method .. " " .. e.type .. " prueba=" .. e.prueba)
        local b = Json.decode(e.body)
        print("cuerpo " .. b.nombre .. " " .. b.puntos .. " " .. #b.lista)
        print("data " .. #e.lista .. " " .. e.lista[3] .. " " .. e.sub.a .. " " .. r.headers["x-servidor"])
    end, { ["X-Prueba"] = "desde lua" })
    Http.request({ url = base .. "/nada", method = "delete" }, function(r)
        print("404 " .. tostring(r.ok) .. " " .. r.status .. " " .. r.error)
    end)
    local id, err = Http.get("http://ejemplo.com/", function() print("no deberia") end)
    print("inseguro " .. tostring(id) .. " " .. tostring(err ~= nil))
    local s = Json.encode({ a = 1, b = { true, "x" }, v = Vec3(1, 2, 3) })
    local t = Json.decode(s)
    print("json " .. t.a .. " " .. tostring(t.b[1]) .. " " .. t.b[2] .. " " .. t.v.y)
    print("malo " .. tostring(select(2, Json.decode("{mal"))))
    print("query " .. Http.query({ q = "hola mundo", page = 2 }))
    Http.get(base .. "/lento", function() print("lento llego") end)
    print("pendientes " .. tostring(Http.pending() >= 1))
end
return Web
)lua";
        ecs::World world;
        world.create("Web").add<scripting::Script>().file = "Scripts/Web.lua";
        scripting::ScriptSystem scripts;
        std::vector<std::string> log;
        scripts.setAssetsRoot(root);
        const bool live = std::getenv("HTTP_LOG") != nullptr;
        scripts.setLog([&](int, const std::string& m) {
            log.push_back(m);
            if (live) std::printf("    [lua] %s\n", m.c_str());
        });
        scripts.start(world);
        std::string out;
        scripts.run("PORT = " + std::to_string(server.port()), &out);
        // Http.get sin cuerpo ni callback (cerraba el programa).
        check(scripts.run("return Http.get('http://127.0.0.1:1/')", &out), "Http.get sin funcion ni cabeceras");
        // El Start corre en el primer update: PORT ya esta.
        for (int i = 0; i < 400 && !(hasLog(log, "get ") && hasLog(log, "data ") && hasLog(log, "404 ")); ++i) {
            scripts.update(world, 0.016f);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (std::getenv("HTTP_LOG")) {
            for (const auto& l : log) std::printf("    %s\n", l.c_str());
        }
        check(hasLog(log, "get true 200 hola"), "Http.get y su funcion con la respuesta");
        check(hasLog(log, "post POST application/json prueba=desde lua"), "Http.post con una tabla: JSON y cabeceras propias");
        check(hasLog(log, "cuerpo Ana 42 3"), "la tabla llega como JSON al servidor");
        check(hasLog(log, "data 4 tres 1 prueba"), "res.data: la respuesta JSON ya decodificada (null = nil); res.headers");
        check(hasLog(log, "404 false 404 El servidor respondio 404"), "Http.request con otro metodo; error con el estado");
        check(hasLog(log, "inseguro nil true") && !hasLog(log, "no deberia"), "http a Internet: nil y el motivo");
        check(hasLog(log, "json 1 true x 2"), "Json.encode / Json.decode (con Vec3)");
        check(hasLog(log, "malo Json.decode"), "Json.decode de algo que no es JSON: nil y el motivo");
        check(hasLog(log, "query page=2&q=hola%20mundo"), "Http.query");
        check(hasLog(log, "pendientes true"), "Http.pending");
        scripts.stop();
        std::this_thread::sleep_for(std::chrono::milliseconds(3000));
        check(!hasLog(log, "lento llego"), "al parar (salir de Play) lo pendiente se cancela");
        std::filesystem::remove_all(root);
    }

    if (online) {
        std::printf("En linea\n");
        net::HttpRequest req;
        req.url = "https://api.github.com/repos/MantraxStudios/Cramion";
        req.headers = {{"Accept", "application/vnd.github+json"}};
        const net::HttpResponse r = net::httpRequest(req);
        std::printf("    GitHub: %d %s (%.2f s)\n", r.status, r.error.c_str(), r.seconds);
        check(r.ok && r.body.find("\"name\"") != std::string::npos, "https real (GitHub): certificado valido y JSON");
        for (const char* bad : {"https://expired.badssl.com/", "https://wrong.host.badssl.com/", "https://self-signed.badssl.com/"}) {
            req.url = bad;
            req.headers.clear();
            const net::HttpResponse b = net::httpRequest(req);
            std::printf("    %s -> %s\n", bad, b.error.c_str());
            check(!b.ok && b.status == 0 && !b.error.empty(), "certificado malo: se rechaza");
        }
    }

    std::printf("\n%d comprobaciones, %d fallos\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
