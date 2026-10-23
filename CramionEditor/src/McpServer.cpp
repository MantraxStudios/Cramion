// Servidor HTTP minimo para MCP (ver McpServer.h). Winsock, una conexion
// cada vez (las IA hacen una peticion y esperan la respuesta).

#include "McpServer.h"

#include "Dialogs.h"
#include "LoadingScreen.h"  // localDataFolder

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
// Sockets BSD con los nombres de Winsock que usa este archivo.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

using SOCKET = int;
constexpr SOCKET INVALID_SOCKET = -1;
static int closesocket(SOCKET s) { return ::close(s); }
#endif

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <iostream>
#include <random>

namespace cramion::editor {

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool sendAll(SOCKET s, const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
#if defined(_WIN32)
        const int n = send(s, data.data() + sent, static_cast<int>(data.size() - sent), 0);
#else
        // MSG_NOSIGNAL: un cliente que cierra antes no mata al editor con SIGPIPE.
        const int n = static_cast<int>(::send(s, data.data() + sent, data.size() - sent, MSG_NOSIGNAL));
#endif
        if (n <= 0) return false;
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

// `cors`: la cabecera para las paginas locales (solo sin token exigido; con
// el token, ninguna pagina web entra).
void respond(SOCKET s, int status, const char* reason, const std::string& body,
             const char* type = "application/json", bool cors = false) {
    std::string head = "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\n";
    head += "Content-Type: ";
    head += type;
    head += "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
    if (cors) head += "Access-Control-Allow-Origin: http://localhost\r\n";
    head += "Connection: close\r\n\r\n";
    sendAll(s, head + body);
}

// Una Origin local (o ninguna: los clientes MCP no la mandan).
bool localOrigin(const std::string& origin) {
    if (origin.empty() || origin == "null") return origin.empty();
    const std::string o = lower(origin);
    for (const char* ok : {"http://localhost", "http://127.0.0.1", "https://localhost", "https://127.0.0.1"}) {
        const std::string prefix(ok);
        if (o.compare(0, prefix.size(), prefix) == 0 &&
            (o.size() == prefix.size() || o[prefix.size()] == ':' || o[prefix.size()] == '/')) {
            return true;
        }
    }
    return false;
}

// Host de la peticion: este PC (127.0.0.1 o localhost, con o sin puerto) o
// ninguno (HTTP/1.0). Otro nombre = una pagina con DNS rebinding.
bool localHost(const std::string& host) {
    if (host.empty()) return true;
    std::string h = lower(host);
    if (const std::size_t colon = h.rfind(':'); colon != std::string::npos) h.resize(colon);
    return h == "127.0.0.1" || h == "localhost";
}

// 256 bits en hexadecimal. std::random_device es rand_s en el STL de
// Microsoft (RtlGenRandom: criptografico).
std::string randomToken() {
    static constexpr char kHex[] = "0123456789abcdef";
    std::random_device device;
    std::string token;
    token.reserve(64);
    for (int i = 0; i < 8; ++i) {
        const std::uint32_t value = device();
        for (int b = 0; b < 8; ++b) token += kHex[(value >> (b * 4)) & 0xF];
    }
    return token;
}

// Comparacion en tiempo constante (no dice cuantos caracteres acierta).
bool sameToken(const std::string& presented, const std::string& expected) {
    if (expected.empty() || presented.size() != expected.size()) return false;
    unsigned char difference = 0;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        difference |= static_cast<unsigned char>(presented[i] ^ expected[i]);
    }
    return difference == 0;
}

// Temporal + rename: CramionMcp.exe nunca lee un token a medias.
bool writeTokenFile(const std::filesystem::path& file, const std::string& token) {
    std::filesystem::path temporary = file;
    temporary += ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
#if !defined(_WIN32)
        // Solo el usuario lo lee (la carpeta puede ser /tmp, compartida).
        std::error_code perm_ec;
        std::filesystem::permissions(temporary, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                     std::filesystem::perm_options::replace, perm_ec);
#endif
        out << token;
        if (!out) return false;
    }
    std::error_code ec;
    std::filesystem::rename(temporary, file, ec);
    return !ec;
}

}  // namespace

std::filesystem::path McpServer::tokenFile(std::uint16_t port) {
    return localDataFolder({}) / ("mcp-token-" + std::to_string(port));
}

McpServer::~McpServer() { stop(); }

bool McpServer::start(std::uint16_t port, std::string* error) {
    stop();
#if defined(_WIN32)
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        if (error) *error = "WSAStartup fallo";
        return false;
    }
#endif
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) {
        if (error) *error = "no se pudo crear el socket";
        return false;
    }
#if !defined(_WIN32)
    // Sin esto, al volver a encender el puerto sigue en TIME_WAIT. En Linux
    // SO_REUSEADDR no deja a dos procesos escuchar en el mismo puerto.
    const int reuse = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // solo este PC
    if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || listen(listener, 8) != 0) {
        closesocket(listener);
        if (error) *error = "el puerto " + std::to_string(port) + " esta ocupado (otro editor abierto?)";
        return false;
    }
    listener_ = static_cast<std::uintptr_t>(listener);
    port_ = port;
    // El token es de la sesion del editor: el mismo al volver a encender.
    if (token_.empty()) token_ = randomToken();
    token_file_ = tokenFile(port);
    if (!writeTokenFile(token_file_, token_)) {
        std::cerr << "[MCP] No se pudo escribir el token en " << dialogs::utf8(token_file_)
                  << ": con \"Exigir token MCP\" CramionMcp.exe no podra conectarse\n";
    }
    running_ = true;
    thread_ = std::thread([this] { acceptLoop(); });
    return true;
}

void McpServer::stop() {
    if (!running_) return;
    running_ = false;
#if defined(_WIN32)
    closesocket(static_cast<SOCKET>(listener_));  // despierta a accept()
#else
    // En Linux close() no despierta a accept(); shutdown() si.
    shutdown(static_cast<SOCKET>(listener_), SHUT_RDWR);
    closesocket(static_cast<SOCKET>(listener_));
#endif
    if (thread_.joinable()) thread_.join();
    listener_ = ~std::uintptr_t{0};
    // El token solo vale con el servidor encendido.
    if (!token_file_.empty()) {
        std::error_code ec;
        std::filesystem::remove(token_file_, ec);
        token_file_.clear();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    for (Pending& p : pending_) p.reply.set_value("");
    pending_.clear();
}

void McpServer::acceptLoop() {
    while (running_) {
        const SOCKET client = accept(static_cast<SOCKET>(listener_), nullptr, nullptr);
        if (client == INVALID_SOCKET) continue;
        serve(static_cast<std::uintptr_t>(client));
        closesocket(client);
    }
}

void McpServer::serve(std::uintptr_t handle) {
    const SOCKET client = static_cast<SOCKET>(handle);
#if defined(_WIN32)
    DWORD timeout = 10000;
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
    timeval timeout{};
    timeout.tv_sec = 10;
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#endif

    // Cabeceras y cuerpo (Content-Length).
    std::string data;
    char buffer[8192];
    std::size_t header_end = std::string::npos;
    while (header_end == std::string::npos) {
        const int n = static_cast<int>(recv(client, buffer, sizeof(buffer), 0));
        if (n <= 0) return;
        data.append(buffer, static_cast<std::size_t>(n));
        header_end = data.find("\r\n\r\n");
        if (data.size() > (1u << 20) && header_end == std::string::npos) return;
    }
    const std::string head = data.substr(0, header_end);
    std::string body = data.substr(header_end + 4);
    std::string method, path, origin, host, presented;
    std::size_t length = 0;
    {
        const std::size_t line_end = head.find("\r\n");
        const std::string request_line = head.substr(0, line_end);
        const std::size_t a = request_line.find(' ');
        const std::size_t b = request_line.find(' ', a + 1);
        method = request_line.substr(0, a);
        path = request_line.substr(a + 1, b - a - 1);
        std::size_t pos = line_end + 2;
        while (pos < head.size()) {
            std::size_t end = head.find("\r\n", pos);
            if (end == std::string::npos) end = head.size();
            const std::string line = head.substr(pos, end - pos);
            const std::size_t colon = line.find(':');
            if (colon != std::string::npos) {
                const std::string key = lower(line.substr(0, colon));
                std::string value = line.substr(colon + 1);
                value.erase(0, value.find_first_not_of(' '));
                if (key == "content-length") length = static_cast<std::size_t>(std::strtoull(value.c_str(), nullptr, 10));
                if (key == "origin") origin = value;
                if (key == "host") host = value;
                if (key == "x-cramion-token") presented = value;
                if (key == "authorization" && value.size() > 7 && lower(value.substr(0, 7)) == "bearer ") {
                    presented = value.substr(7);
                }
            }
            pos = end + 2;
        }
    }
    if (length > (64u << 20)) {
        respond(client, 413, "Payload Too Large", "{}");
        return;
    }
    while (body.size() < length) {
        const int n = static_cast<int>(recv(client, buffer, sizeof(buffer), 0));
        if (n <= 0) return;
        body.append(buffer, static_cast<std::size_t>(n));
    }
    body.resize(std::min(body.size(), length));

    while (!presented.empty() && (presented.back() == ' ' || presented.back() == '\t')) presented.pop_back();
    presented.erase(0, presented.find_first_not_of(" \t"));

    const bool require_token = require_token_;
    const bool cors = !require_token;
    const auto reject = [&](const char* why) {
        ++rejected_;
        respond(client, 403, "Forbidden", std::string(R"({"error":")") + why + "\"}", "application/json", cors);
    };
    if (!localHost(host)) {
        reject("Host no permitido: solo 127.0.0.1 o localhost");
        return;
    }
    // Con el token exigido, ninguna Origin: los clientes MCP no la mandan y
    // un navegador siempre (aunque la pagina sea de localhost).
    if (require_token ? !origin.empty() : !localOrigin(origin)) {
        reject("origen no permitido: las paginas web no pueden usar el MCP del editor");
        return;
    }
    if (method == "GET" && (path == "/" || path == "/health")) {
        respond(client, 200, "OK", R"({"server":"cramion-editor","mcp":"/mcp"})", "application/json", cors);
        return;
    }
    // 403 y no 401: con 401 los clientes HTTP de MCP intentan OAuth y el
    // error que ensenan no dice que falta el token.
    if (require_token && !sameToken(presented, token_)) {
#if defined(_WIN32)
        reject(presented.empty()
                   ? "falta el token del MCP de Cramion: mandalo en Authorization: Bearer <token> (archivo "
                     "%LOCALAPPDATA%/Cramion/mcp-token-<puerto>) o conecta con CramionMcp.exe, que lo manda solo"
                   : "token del MCP de Cramion no valido: cambia en cada sesion del editor, vuelve a leer "
                     "%LOCALAPPDATA%/Cramion/mcp-token-<puerto>");
#else
        reject(presented.empty()
                   ? "falta el token del MCP de Cramion: mandalo en Authorization: Bearer <token> (archivo "
                     "~/.local/share/Cramion/mcp-token-<puerto>, o $XDG_DATA_HOME/Cramion/...) "
                     "o conecta con CramionMcp, que lo manda solo"
                   : "token del MCP de Cramion no valido: cambia en cada sesion del editor, vuelve a leer "
                     "~/.local/share/Cramion/mcp-token-<puerto> (o $XDG_DATA_HOME/Cramion/...)");
#endif
        return;
    }
    if (path != "/mcp" && path != "/") {
        respond(client, 404, "Not Found", "{}", "application/json", cors);
        return;
    }
    if (method == "GET") {  // sin canal SSE: solo peticion/respuesta
        respond(client, 405, "Method Not Allowed", "{}", "application/json", cors);
        return;
    }
    if (method == "DELETE") {
        respond(client, 200, "OK", "{}", "application/json", cors);
        return;
    }
    if (method != "POST") {
        respond(client, 405, "Method Not Allowed", "{}", "application/json", cors);
        return;
    }

    ++requests_;
    std::future<std::string> reply;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.push_back(Pending{std::move(body), {}});
        reply = pending_.back().reply.get_future();
    }
    // El editor puede estar ocupado (importando): hasta 2 minutos.
    if (reply.wait_for(std::chrono::seconds(120)) != std::future_status::ready) {
        respond(client, 504, "Gateway Timeout", R"({"error":"el editor no respondio a tiempo"})", "application/json",
                cors);
        return;
    }
    const std::string answer = reply.get();
    if (answer.empty()) {
        respond(client, 202, "Accepted", "", "application/json", cors);
    } else {
        respond(client, 200, "OK", answer, "application/json", cors);
    }
}

void McpServer::poll(const std::function<std::string(const std::string&)>& handler) {
    std::deque<Pending> batch;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        batch.swap(pending_);
    }
    for (Pending& p : batch) {
        std::string answer;
        try {
            answer = handler(p.body);
        } catch (const std::exception& e) {
            answer = std::string(R"({"jsonrpc":"2.0","id":null,"error":{"code":-32603,"message":")") + e.what() + "\"}}";
        }
        p.reply.set_value(std::move(answer));
    }
}

}  // namespace cramion::editor
