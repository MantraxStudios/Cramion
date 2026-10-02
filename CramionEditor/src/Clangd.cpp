// Cliente LSP de clangd (ver Clangd.h): un proceso con sus tuberias, mensajes
// JSON-RPC con cabecera Content-Length y un hilo que lee las respuestas.

#include "Clangd.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <iostream>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace cramion::editor {

using json = nlohmann::json;

namespace {

std::string utf8(const std::filesystem::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

// El texto plano de un MarkupContent / MarkedString de LSP.
std::string plainText(const json& contents) {
    if (contents.is_string()) return contents.get<std::string>();
    if (contents.is_object()) return contents.value("value", std::string());
    if (contents.is_array()) {
        std::string out;
        for (const json& c : contents) {
            if (!out.empty()) out += "\n";
            out += plainText(c);
        }
        return out;
    }
    return {};
}

}  // namespace

ClangdClient::~ClangdClient() { stop(); }

std::string ClangdClient::uri(const std::filesystem::path& file) {
    std::string path = utf8(std::filesystem::absolute(file).lexically_normal());
    std::replace(path.begin(), path.end(), '\\', '/');
    std::string out = "file:///";
    static const char* hex = "0123456789ABCDEF";
    for (const char c : path) {
        const auto u = static_cast<unsigned char>(c);
        if (std::isalnum(u) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~' || c == ':') {
            out += c;
        } else {
            out += '%';
            out += hex[u >> 4];
            out += hex[u & 15];
        }
    }
    return out;
}

std::filesystem::path ClangdClient::fromUri(const std::string& u) {
    std::string s = u.rfind("file:///", 0) == 0 ? u.substr(8) : u;
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            out += static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return std::filesystem::path(std::u8string(out.begin(), out.end()));
}

std::string ClangdClient::key(const std::filesystem::path& file) const { return uri(file); }

namespace {
// La misma ruta escrita de otra forma (c: / C:, %3A, mayusculas): Windows no distingue.
std::string sameFile(const std::string& u) {
    std::string k = ClangdClient::uri(ClangdClient::fromUri(u));
    for (char& c : k) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return k;
}
}  // namespace

bool ClangdClient::start(const std::filesystem::path& clangd, const std::filesystem::path& root) {
    stop();
#if defined(_WIN32)
    std::error_code ec;
    if (clangd.empty() || !std::filesystem::exists(clangd, ec)) {
        status_ = "sin clangd (falta toolchain/bin/clangd.exe)";
        return false;
    }
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE in_read = nullptr, in_write = nullptr, out_read = nullptr, out_write = nullptr;
    if (!CreatePipe(&in_read, &in_write, &sa, 0) || !CreatePipe(&out_read, &out_write, &sa, 0)) {
        status_ = "no se pudo crear la tuberia de clangd";
        return false;
    }
    SetHandleInformation(in_write, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0);
    HANDLE null_err = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = in_read;
    si.hStdOutput = out_write;
    si.hStdError = null_err;
    // Pocos hilos y sin indice en segundo plano: no se come la RAM (equipos de 16 GB).
    std::wstring cmd = L"\"" + clangd.wstring() +
                       L"\" --log=error --header-insertion=never --completion-style=detailed --pch-storage=memory "
                       L"--background-index=false -j=2 --limit-results=400 --function-arg-placeholders=0 --completion-parse=always";
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr,
                                   root.wstring().c_str(), &si, &pi);
    CloseHandle(in_read);
    CloseHandle(out_write);
    if (null_err != INVALID_HANDLE_VALUE) CloseHandle(null_err);
    if (!ok) {
        CloseHandle(in_write);
        CloseHandle(out_read);
        status_ = "no se pudo arrancar clangd";
        return false;
    }
    CloseHandle(pi.hThread);
    process_ = pi.hProcess;
    stdin_write_ = in_write;
    stdout_read_ = out_read;
    running_ = true;
    reader_ = std::thread([this] { readLoop(); });
    json caps = {
        {"general", {{"positionEncodings", {"utf-8"}}}},
        {"offsetEncoding", {"utf-8"}},
        {"textDocument",
         {{"completion", {{"completionItem", {{"snippetSupport", false}, {"documentationFormat", {"plaintext"}}}}}},
          {"hover", {{"contentFormat", {"plaintext"}}}},
          {"signatureHelp", {{"signatureInformation", {{"documentationFormat", {"plaintext"}},
                                                       {"parameterInformation", {{"labelOffsetSupport", true}}}}}}},
          {"publishDiagnostics", {{"relatedInformation", false}}},
          {"synchronization", {{"didSave", false}}}}}};
    request("initialize",
            json{{"processId", static_cast<int>(GetCurrentProcessId())}, {"rootUri", uri(root)}, {"capabilities", caps},
                 {"initializationOptions", {{"clangdFileStatus", false}}}},
            [this](const json&) {
                notify("initialized", json::object());
                status_ = "clangd listo";
            });
    status_ = "clangd arrancando...";
    return true;
#else
    (void)clangd;
    (void)root;
    return false;
#endif
}

void ClangdClient::stop() {
    if (!running_ && !reader_.joinable()) return;
#if defined(_WIN32)
    if (running_) {
        request("shutdown", nullptr, nullptr);
        notify("exit", nullptr);
    }
    running_ = false;
    if (process_ != nullptr) {
        if (WaitForSingleObject(process_, 500) == WAIT_TIMEOUT) TerminateProcess(process_, 0);
        CloseHandle(process_);
        process_ = nullptr;
    }
    if (stdin_write_ != nullptr) {
        CloseHandle(stdin_write_);
        stdin_write_ = nullptr;
    }
    if (reader_.joinable()) reader_.join();
    if (stdout_read_ != nullptr) {
        CloseHandle(stdout_read_);
        stdout_read_ = nullptr;
    }
#endif
    pending_.clear();
    versions_.clear();
    diagnostics_.clear();
    incoming_.clear();
}

void ClangdClient::send(const json& message) {
#if defined(_WIN32)
    if (!running_ || stdin_write_ == nullptr) return;
    const std::string body = message.dump();
    const std::string header = "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
    std::lock_guard lock(send_mutex_);
    DWORD written = 0;
    if (!WriteFile(stdin_write_, header.data(), static_cast<DWORD>(header.size()), &written, nullptr) ||
        !WriteFile(stdin_write_, body.data(), static_cast<DWORD>(body.size()), &written, nullptr)) {
        running_ = false;
        status_ = "clangd se cerro";
    }
#else
    (void)message;
#endif
}

int ClangdClient::request(const std::string& method, json params, std::function<void(const json&)> on_result) {
    const int id = next_id_++;
    if (on_result) pending_[id] = std::move(on_result);
    send(json{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", std::move(params)}});
    return id;
}

void ClangdClient::notify(const std::string& method, json params) {
    json m{{"jsonrpc", "2.0"}, {"method", method}};
    if (!params.is_null()) m["params"] = std::move(params);
    send(m);
}

void ClangdClient::readLoop() {
#if defined(_WIN32)
    std::string buffer;
    char chunk[16384];
    while (running_) {
        DWORD n = 0;
        if (!ReadFile(stdout_read_, chunk, sizeof(chunk), &n, nullptr) || n == 0) break;
        buffer.append(chunk, n);
        while (true) {
            const std::size_t header_end = buffer.find("\r\n\r\n");
            if (header_end == std::string::npos) break;
            std::size_t length = 0;
            const std::size_t at = buffer.find("Content-Length:");
            if (at != std::string::npos && at < header_end) length = static_cast<std::size_t>(std::strtoull(buffer.c_str() + at + 15, nullptr, 10));
            if (buffer.size() < header_end + 4 + length) break;
            json message = json::parse(buffer.substr(header_end + 4, length), nullptr, false);
            buffer.erase(0, header_end + 4 + length);
            if (!message.is_discarded()) {
                std::lock_guard lock(queue_mutex_);
                incoming_.push_back(std::move(message));
            }
        }
    }
    running_ = false;
#endif
}

void ClangdClient::poll() {
    std::deque<json> messages;
    {
        std::lock_guard lock(queue_mutex_);
        messages.swap(incoming_);
    }
    for (json& m : messages) {
        if (m.contains("id") && (m.contains("result") || m.contains("error")) && !m.contains("method")) {
            const int id = m["id"].is_number() ? m["id"].get<int>() : -1;
            const auto it = pending_.find(id);
            if (it == pending_.end()) continue;
            auto callback = std::move(it->second);
            pending_.erase(it);
            if (callback) callback(m.contains("result") ? m["result"] : json());
            continue;
        }
        const std::string method = m.value("method", std::string());
        if (method == "textDocument/publishDiagnostics") {
            const json& p = m["params"];
            std::vector<ClangdDiagnostic> list;
            for (const json& d : p.value("diagnostics", json::array())) {
                ClangdDiagnostic diag;
                diag.line = d["range"]["start"].value("line", 0);
                diag.column = d["range"]["start"].value("character", 0);
                diag.end_line = d["range"]["end"].value("line", 0);
                diag.end_column = d["range"]["end"].value("character", 0);
                diag.severity = d.value("severity", 1);
                diag.message = d.value("message", std::string());
                list.push_back(std::move(diag));
            }
            diagnostics_[sameFile(p.value("uri", std::string()))] = std::move(list);
            ++diagnostics_version_;
        } else if (m.contains("id") && !method.empty()) {
            // Peticiones del servidor (registrar capacidades, progreso...): se acepta.
            send(json{{"jsonrpc", "2.0"}, {"id", m["id"]}, {"result", nullptr}});
        }
    }
}

void ClangdClient::open(const std::filesystem::path& file, const std::string& text) {
    const std::string k = key(file);
    if (versions_.contains(k)) {
        change(file, text);
        return;
    }
    versions_[k] = 1;
    notify("textDocument/didOpen", json{{"textDocument", {{"uri", k}, {"languageId", "cpp"}, {"version", 1}, {"text", text}}}});
}

void ClangdClient::change(const std::filesystem::path& file, const std::string& text) {
    const std::string k = key(file);
    const auto it = versions_.find(k);
    if (it == versions_.end()) {
        open(file, text);
        return;
    }
    const int version = ++it->second;
    notify("textDocument/didChange",
           json{{"textDocument", {{"uri", k}, {"version", version}}}, {"contentChanges", json::array({json{{"text", text}}})}});
}

void ClangdClient::close(const std::filesystem::path& file) {
    const std::string k = key(file);
    if (versions_.erase(k) == 0) return;
    notify("textDocument/didClose", json{{"textDocument", {{"uri", k}}}});
    diagnostics_.erase(k);
}

bool ClangdClient::isOpen(const std::filesystem::path& file) const { return versions_.contains(key(file)); }

const std::vector<ClangdDiagnostic>& ClangdClient::diagnostics(const std::filesystem::path& file) const {
    static const std::vector<ClangdDiagnostic> none;
    const auto it = diagnostics_.find(sameFile(key(file)));
    return it != diagnostics_.end() ? it->second : none;
}

void ClangdClient::completion(const std::filesystem::path& file, int line, int column, CompletionCallback cb) {
    request("textDocument/completion",
            json{{"textDocument", {{"uri", key(file)}}}, {"position", {{"line", line}, {"character", column}}}},
            [cb = std::move(cb)](const json& result) {
                std::vector<ClangdCompletion> out;
                const json items = result.is_array() ? result : result.value("items", json::array());
                for (const json& item : items) {
                    ClangdCompletion c;
                    c.label = item.value("label", std::string());
                    // Quitar el espacio / punto que clangd pone delante en la etiqueta.
                    while (!c.label.empty() && (c.label[0] == ' ' || c.label[0] == '\xE2')) {
                        if (c.label[0] == '\xE2' && c.label.size() >= 3) c.label.erase(0, 3);
                        else c.label.erase(0, 1);
                    }
                    c.detail = item.value("detail", std::string());
                    c.kind = item.value("kind", 0);
                    if (item.contains("documentation")) c.documentation = plainText(item["documentation"]);
                    if (item.contains("textEdit")) {
                        const json& te = item["textEdit"];
                        c.insert = te.value("newText", std::string());
                        const json& range = te.contains("range") ? te["range"] : te.value("replace", json::object());
                        if (range.contains("start")) c.replace_start = range["start"].value("character", -1);
                    } else {
                        c.insert = item.value("insertText", c.label);
                    }
                    out.push_back(std::move(c));
                }
                cb(std::move(out));
            });
}

void ClangdClient::hover(const std::filesystem::path& file, int line, int column, HoverCallback cb) {
    request("textDocument/hover",
            json{{"textDocument", {{"uri", key(file)}}}, {"position", {{"line", line}, {"character", column}}}},
            [cb = std::move(cb)](const json& result) {
                cb(result.is_object() && result.contains("contents") ? plainText(result["contents"]) : std::string());
            });
}

void ClangdClient::signatureHelp(const std::filesystem::path& file, int line, int column, SignatureCallback cb) {
    request("textDocument/signatureHelp",
            json{{"textDocument", {{"uri", key(file)}}}, {"position", {{"line", line}, {"character", column}}}},
            [cb = std::move(cb)](const json& result) {
                std::vector<ClangdSignature> out;
                int active = 0;
                if (result.is_object()) {
                    active = result.value("activeSignature", 0);
                    const int active_param = result.value("activeParameter", 0);
                    for (const json& s : result.value("signatures", json::array())) {
                        ClangdSignature sig;
                        sig.label = s.value("label", std::string());
                        if (s.contains("documentation")) sig.documentation = plainText(s["documentation"]);
                        sig.active_parameter = s.value("activeParameter", active_param);
                        const json params = s.value("parameters", json::array());
                        if (sig.active_parameter >= 0 && sig.active_parameter < static_cast<int>(params.size())) {
                            const json& lab = params[static_cast<std::size_t>(sig.active_parameter)]["label"];
                            if (lab.is_array() && lab.size() == 2) {
                                sig.parameter_start = lab[0].get<int>();
                                sig.parameter_end = lab[1].get<int>();
                            } else if (lab.is_string()) {
                                const std::size_t at = sig.label.find(lab.get<std::string>());
                                if (at != std::string::npos) {
                                    sig.parameter_start = static_cast<int>(at);
                                    sig.parameter_end = static_cast<int>(at + lab.get<std::string>().size());
                                }
                            }
                        }
                        out.push_back(std::move(sig));
                    }
                }
                cb(std::move(out), active);
            });
}

void ClangdClient::definition(const std::filesystem::path& file, int line, int column, LocationCallback cb) {
    request("textDocument/definition",
            json{{"textDocument", {{"uri", key(file)}}}, {"position", {{"line", line}, {"character", column}}}},
            [cb = std::move(cb)](const json& result) {
                std::vector<ClangdLocation> out;
                const json list = result.is_array() ? result : (result.is_object() ? json::array({result}) : json::array());
                for (const json& l : list) {
                    const std::string u = l.contains("targetUri") ? l.value("targetUri", std::string()) : l.value("uri", std::string());
                    const json& range = l.contains("targetSelectionRange") ? l["targetSelectionRange"] : l.value("range", json::object());
                    ClangdLocation loc;
                    loc.file = fromUri(u);
                    if (range.contains("start")) {
                        loc.line = range["start"].value("line", 0);
                        loc.column = range["start"].value("character", 0);
                    }
                    out.push_back(std::move(loc));
                }
                cb(std::move(out));
            });
}

}  // namespace cramion::editor
