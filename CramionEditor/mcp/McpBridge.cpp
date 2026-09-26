// CramionMcp.exe: puente MCP por stdio para los clientes que lanzan un
// proceso (Claude Desktop y otros). Lee mensajes JSON-RPC (uno por linea) de
// la entrada, los manda al editor abierto (http://127.0.0.1:<puerto>/mcp) y
// escribe sus respuestas en la salida, una por linea.
//
//   CramionMcp.exe [--port 7777]
//
// Si el editor no esta abierto, contesta con un error que lo explica.

#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

// POST del cuerpo; devuelve el estado HTTP (0 = sin conexion) y el cuerpo.
int post(int port, const std::string& body, std::string& response) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return 0;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<u_short>(port));
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        closesocket(s);
        return 0;
    }
    std::string request = "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\n"
                          "Accept: application/json, text/event-stream\r\nContent-Length: " +
                          std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
    std::size_t sent = 0;
    while (sent < request.size()) {
        const int n = send(s, request.data() + sent, static_cast<int>(request.size() - sent), 0);
        if (n <= 0) {
            closesocket(s);
            return 0;
        }
        sent += static_cast<std::size_t>(n);
    }
    std::string data;
    char buffer[16384];
    for (int n; (n = recv(s, buffer, sizeof(buffer), 0)) > 0;) data.append(buffer, static_cast<std::size_t>(n));
    closesocket(s);
    const std::size_t head_end = data.find("\r\n\r\n");
    if (head_end == std::string::npos) return 0;
    const std::size_t space = data.find(' ');
    const int status = space != std::string::npos ? std::atoi(data.c_str() + space + 1) : 0;
    response = data.substr(head_end + 4);
    return status;
}

// El "id" del mensaje tal cual (para contestar el error), o vacio si es una notificacion.
std::string idOf(const std::string& message) {
    const std::size_t key = message.find("\"id\"");
    if (key == std::string::npos) return {};
    std::size_t start = message.find(':', key);
    if (start == std::string::npos) return {};
    ++start;
    while (start < message.size() && message[start] == ' ') ++start;
    std::size_t end = start;
    if (end < message.size() && message[end] == '"') {
        end = message.find('"', end + 1);
        return end == std::string::npos ? std::string{} : message.substr(start, end - start + 1);
    }
    while (end < message.size() && message[end] != ',' && message[end] != '}' && message[end] != ' ') ++end;
    return message.substr(start, end - start);
}

}  // namespace

int main(int argc, char** argv) {
    int port = 7777;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == "--port") port = std::atoi(argv[i + 1]);
    }
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);
    std::ios::sync_with_stdio(false);

    std::string line;
    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.find_first_not_of(" \t") == std::string::npos) continue;
        std::string response;
        const int status = post(port, line, response);
        if (status == 200 && !response.empty()) {
            // Una respuesta por linea (el JSON del editor no lleva saltos).
            std::cout << response << "\n" << std::flush;
            continue;
        }
        if (status == 202) continue;  // notificacion
        const std::string id = idOf(line);
        if (id.empty()) continue;
        const std::string why = status == 0 ? "Cramion Editor no esta abierto (o su servidor MCP esta apagado). Abre el editor "
                                              "con un proyecto y vuelve a intentarlo."
                                            : "el editor respondio con el estado HTTP " + std::to_string(status);
        std::cout << "{\"jsonrpc\":\"2.0\",\"id\":" << id << ",\"error\":{\"code\":-32000,\"message\":\"" << why << "\"}}\n"
                  << std::flush;
    }
    WSACleanup();
    return 0;
}
