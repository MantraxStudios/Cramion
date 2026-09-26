// Servidor HTTP minimo para MCP (ver McpServer.h). Winsock, una conexion
// cada vez (las IA hacen una peticion y esperan la respuesta).

#include "McpServer.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <iostream>

namespace cramion::editor {

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool sendAll(SOCKET s, const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const int n = send(s, data.data() + sent, static_cast<int>(data.size() - sent), 0);
        if (n <= 0) return false;
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

void respond(SOCKET s, int status, const char* reason, const std::string& body,
             const char* type = "application/json") {
    std::string head = "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\n";
    head += "Content-Type: ";
    head += type;
    head += "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
    head += "Access-Control-Allow-Origin: http://localhost\r\n";
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

}  // namespace

McpServer::~McpServer() { stop(); }

bool McpServer::start(std::uint16_t port, std::string* error) {
    stop();
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        if (error) *error = "WSAStartup fallo";
        return false;
    }
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) {
        if (error) *error = "no se pudo crear el socket";
        return false;
    }
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
    running_ = true;
    thread_ = std::thread([this] { acceptLoop(); });
    return true;
}

void McpServer::stop() {
    if (!running_) return;
    running_ = false;
    closesocket(static_cast<SOCKET>(listener_));  // despierta a accept()
    if (thread_.joinable()) thread_.join();
    listener_ = ~std::uintptr_t{0};
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
    DWORD timeout = 10000;
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));

    // Cabeceras y cuerpo (Content-Length).
    std::string data;
    char buffer[8192];
    std::size_t header_end = std::string::npos;
    while (header_end == std::string::npos) {
        const int n = recv(client, buffer, sizeof(buffer), 0);
        if (n <= 0) return;
        data.append(buffer, static_cast<std::size_t>(n));
        header_end = data.find("\r\n\r\n");
        if (data.size() > (1u << 20) && header_end == std::string::npos) return;
    }
    const std::string head = data.substr(0, header_end);
    std::string body = data.substr(header_end + 4);
    std::string method, path, origin;
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
            }
            pos = end + 2;
        }
    }
    if (length > (64u << 20)) {
        respond(client, 413, "Payload Too Large", "{}");
        return;
    }
    while (body.size() < length) {
        const int n = recv(client, buffer, sizeof(buffer), 0);
        if (n <= 0) return;
        body.append(buffer, static_cast<std::size_t>(n));
    }
    body.resize(std::min(body.size(), length));

    if (!localOrigin(origin)) {
        respond(client, 403, "Forbidden", R"({"error":"origen no permitido"})");
        return;
    }
    if (method == "GET" && (path == "/" || path == "/health")) {
        respond(client, 200, "OK", R"({"server":"cramion-editor","mcp":"/mcp"})");
        return;
    }
    if (path != "/mcp" && path != "/") {
        respond(client, 404, "Not Found", "{}");
        return;
    }
    if (method == "GET") {  // sin canal SSE: solo peticion/respuesta
        respond(client, 405, "Method Not Allowed", "{}");
        return;
    }
    if (method == "DELETE") {
        respond(client, 200, "OK", "{}");
        return;
    }
    if (method != "POST") {
        respond(client, 405, "Method Not Allowed", "{}");
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
        respond(client, 504, "Gateway Timeout", R"({"error":"el editor no respondio a tiempo"})");
        return;
    }
    const std::string answer = reply.get();
    if (answer.empty()) {
        respond(client, 202, "Accepted", "");
    } else {
        respond(client, 200, "OK", answer);
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
