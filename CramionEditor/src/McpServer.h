#ifndef CRAMION_EDITOR_MCP_SERVER_H
#define CRAMION_EDITOR_MCP_SERVER_H

// Servidor MCP (Model Context Protocol) del editor: cualquier IA compatible
// (Claude Code, Claude Desktop, Cursor, VS Code...) se conecta y trabaja en el
// motor con las herramientas de EditorMcp.cpp.
//
// Transporte "Streamable HTTP" de MCP: POST http://127.0.0.1:<puerto>/mcp con
// un mensaje JSON-RPC 2.0 y la respuesta en JSON. Solo escucha en 127.0.0.1 y
// rechaza peticiones con cabecera Origin que no sea local (una pagina web no
// puede mandar ordenes al editor). Para los clientes que solo hablan por
// stdio esta CramionMcp.exe, que hace de puente.
//
// El socket va en un hilo; cada peticion se pasa al hilo principal (el motor
// no es seguro entre hilos) y se contesta cuando poll() la ha resuelto.

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <thread>

namespace cramion::editor {

class McpServer {
public:
    ~McpServer();

    bool start(std::uint16_t port, std::string* error = nullptr);
    void stop();
    bool running() const { return running_; }
    std::uint16_t port() const { return port_; }

    // En el hilo principal, cada frame: resuelve las peticiones pendientes.
    // `handler` recibe el cuerpo JSON y devuelve la respuesta ("" = sin
    // respuesta, p. ej. una notificacion).
    void poll(const std::function<std::string(const std::string&)>& handler);

    // Peticiones atendidas (para la ventana del MCP).
    std::uint64_t requestCount() const { return requests_; }

private:
    struct Pending {
        std::string body;
        std::promise<std::string> reply;
    };
    void acceptLoop();
    void serve(std::uintptr_t client);

    std::atomic<bool> running_{false};
    std::uint16_t port_ = 0;
    std::uintptr_t listener_ = ~std::uintptr_t{0};
    std::thread thread_;
    std::mutex mutex_;
    std::deque<Pending> pending_;
    std::atomic<std::uint64_t> requests_{0};
};

}  // namespace cramion::editor

#endif  // CRAMION_EDITOR_MCP_SERVER_H
