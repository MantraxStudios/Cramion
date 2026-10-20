#ifndef CRAMION_EDITOR_MCP_SERVER_H
#define CRAMION_EDITOR_MCP_SERVER_H

// Servidor MCP (Model Context Protocol) del editor: cualquier IA compatible
// (Claude Code, Claude Desktop, Cursor, VS Code...) se conecta y trabaja en el
// motor con las herramientas de EditorMcp.cpp.
//
// Transporte "Streamable HTTP" de MCP: POST http://127.0.0.1:<puerto>/mcp con
// un mensaje JSON-RPC 2.0 y la respuesta en JSON. Solo escucha en 127.0.0.1 y
// solo acepta Host 127.0.0.1 o localhost (DNS rebinding). Para los clientes
// que solo hablan por stdio esta CramionMcp.exe, que hace de puente.
//
// Token de la sesion (ROADMAP 2 #5; "Exigir token MCP", activado por defecto):
// al encender el servidor se escribe un token aleatorio de 256 bits en
// %LOCALAPPDATA%/Cramion/mcp-token-<puerto> (la carpeta del usuario: otros
// usuarios del PC no lo leen) y cada POST tiene que traerlo en
// "Authorization: Bearer <token>" o en "X-Cramion-Token: <token>". Con el
// token exigido se rechaza cualquier peticion con cabecera Origin (las
// paginas web siempre la mandan). CramionMcp.exe lee el archivo y lo envia
// solo. El token dura lo que el editor (apagar y encender el servidor no lo
// cambia: las terminales abiertas siguen conectadas). GET /health no lo pide.
// Sin exigirlo, como antes: sin token y solo con Origin local o ninguna.
//
// El socket va en un hilo; cada peticion se pasa al hilo principal (el motor
// no es seguro entre hilos) y se contesta cuando poll() la ha resuelto.

#include <atomic>
#include <cstdint>
#include <deque>
#include <filesystem>
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
    // Peticiones rechazadas por el token, el Origin o el Host.
    std::uint64_t rejectedCount() const { return rejected_; }

    // Se puede cambiar con el servidor encendido (vale desde la peticion siguiente).
    void setRequireToken(bool on) { require_token_ = on; }
    bool requireToken() const { return require_token_; }
    // El token de esta sesion (vacio hasta el primer start()).
    const std::string& token() const { return token_; }
    // %LOCALAPPDATA%/Cramion/mcp-token-<puerto> (lo lee CramionMcp.exe).
    static std::filesystem::path tokenFile(std::uint16_t port);

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
    std::atomic<std::uint64_t> rejected_{0};
    std::atomic<bool> require_token_{true};
    // Lo escribe start() antes de crear el hilo del socket y no cambia mientras vive.
    std::string token_;
    std::filesystem::path token_file_;
};

}  // namespace cramion::editor

#endif  // CRAMION_EDITOR_MCP_SERVER_H
