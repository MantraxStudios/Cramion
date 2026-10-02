#ifndef CRAMION_EDITOR_CLANGD_H
#define CRAMION_EDITOR_CLANGD_H

// IntelliSense de C++ en el editor de codigo: un cliente LSP de clangd (el
// incluido con el motor en toolchain/bin, o el del sistema).
//
//   ClangdClient clangd;
//   clangd.start(clangd_exe, carpeta_del_proyecto);   // usa su compile_commands.json
//   clangd.open(archivo, texto);  clangd.change(archivo, texto);
//   clangd.completion(archivo, linea, columna, [](items) {...});
//   cada frame: clangd.poll();    // respuestas y diagnosticos (hilo principal)
//
// Lineas y columnas desde 0, en bytes (UTF-8: se le pide a clangd).

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace cramion::editor {

struct ClangdDiagnostic {
    int line = 0, column = 0, end_line = 0, end_column = 0;
    int severity = 1;  // 1 error, 2 aviso, 3 info, 4 pista
    std::string message;
};

struct ClangdCompletion {
    std::string label;
    std::string insert;  // el texto a escribir (sin snippets)
    std::string detail;  // tipo / firma
    std::string documentation;
    int kind = 0;        // CompletionItemKind de LSP
    int replace_start = -1;  // columna donde empieza lo que sustituye (-1 = la palabra)
};

struct ClangdSignature {
    std::string label;
    std::string documentation;
    int active_parameter = 0;
    int parameter_start = -1, parameter_end = -1;  // del parametro activo en `label`
};

struct ClangdLocation {
    std::filesystem::path file;
    int line = 0, column = 0;
};

class ClangdClient {
public:
    ClangdClient() = default;
    ~ClangdClient();
    ClangdClient(const ClangdClient&) = delete;
    ClangdClient& operator=(const ClangdClient&) = delete;

    bool start(const std::filesystem::path& clangd, const std::filesystem::path& root);
    void stop();
    bool running() const { return running_; }
    const std::string& status() const { return status_; }

    void open(const std::filesystem::path& file, const std::string& text);
    void change(const std::filesystem::path& file, const std::string& text);
    void close(const std::filesystem::path& file);
    bool isOpen(const std::filesystem::path& file) const;

    using CompletionCallback = std::function<void(std::vector<ClangdCompletion>)>;
    using HoverCallback = std::function<void(std::string)>;
    using SignatureCallback = std::function<void(std::vector<ClangdSignature>, int active)>;
    using LocationCallback = std::function<void(std::vector<ClangdLocation>)>;
    void completion(const std::filesystem::path& file, int line, int column, CompletionCallback cb);
    void hover(const std::filesystem::path& file, int line, int column, HoverCallback cb);
    void signatureHelp(const std::filesystem::path& file, int line, int column, SignatureCallback cb);
    void definition(const std::filesystem::path& file, int line, int column, LocationCallback cb);

    // Respuestas y avisos que llegaron (en el hilo que llama).
    void poll();
    const std::vector<ClangdDiagnostic>& diagnostics(const std::filesystem::path& file) const;
    std::uint64_t diagnosticsVersion() const { return diagnostics_version_; }

    static std::string uri(const std::filesystem::path& file);
    static std::filesystem::path fromUri(const std::string& uri);

private:
    void send(const nlohmann::json& message);
    int request(const std::string& method, nlohmann::json params, std::function<void(const nlohmann::json&)> on_result);
    void notify(const std::string& method, nlohmann::json params);
    void readLoop();
    std::string key(const std::filesystem::path& file) const;

#if defined(_WIN32)
    void* process_ = nullptr;
    void* stdin_write_ = nullptr;
    void* stdout_read_ = nullptr;
#endif
    std::thread reader_;
    std::atomic<bool> running_{false};
    std::string status_;
    std::mutex send_mutex_;
    std::mutex queue_mutex_;
    std::deque<nlohmann::json> incoming_;
    int next_id_ = 1;
    std::unordered_map<int, std::function<void(const nlohmann::json&)>> pending_;
    std::map<std::string, int> versions_;  // abiertos y su version
    std::map<std::string, std::vector<ClangdDiagnostic>> diagnostics_;
    std::uint64_t diagnostics_version_ = 0;
};

}  // namespace cramion::editor

#endif  // CRAMION_EDITOR_CLANGD_H
