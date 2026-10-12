#pragma once
// Envoltorio comodo sobre WebSocket.* (Api.gen.h). Base64 sin llamar al motor.
//
//   WsClient ws;
//   ws.connect({{"url", "wss://mi-servidor/juego"}, {"name", "mmo"}});
//   ws.onMessage([this](const Value& ev) { Debug::log(ev["text"].asString()); });
//   ws.send("hola");
//
// La conexion es del motor, no de este objeto: al destruirlo se quitan sus
// escuchas pero la conexion sigue (cerrarla: close()). Tras una recarga en
// caliente o un cambio de escena, recupera la conexion por su nombre con
// attach("mmo") en onAfterReload/awake y vuelve a poner las escuchas.
// Con mucho trafico, mejor poll() cada frame: un solo viaje al motor.
//
// Lo incluye Script.h (despues de Api.gen.h).

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cramion {

// RFC 4648 con relleno '='.
inline std::string base64Encode(std::string_view bytes) {
    static constexpr char kTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 2 < bytes.size(); i += 3) {
        const std::uint32_t n = (std::uint32_t(std::uint8_t(bytes[i])) << 16) | (std::uint32_t(std::uint8_t(bytes[i + 1])) << 8) |
                                std::uint32_t(std::uint8_t(bytes[i + 2]));
        out += kTable[(n >> 18) & 63];
        out += kTable[(n >> 12) & 63];
        out += kTable[(n >> 6) & 63];
        out += kTable[n & 63];
    }
    if (i < bytes.size()) {
        std::uint32_t n = std::uint32_t(std::uint8_t(bytes[i])) << 16;
        const bool two = i + 1 < bytes.size();
        if (two) n |= std::uint32_t(std::uint8_t(bytes[i + 1])) << 8;
        out += kTable[(n >> 18) & 63];
        out += kTable[(n >> 12) & 63];
        out += two ? kTable[(n >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

// Con o sin relleno; cualquier otro caracter no vale. false si no es base64 (out vacio).
inline bool base64Decode(std::string_view text, std::string& out) {
    out.clear();
    std::size_t len = text.size();
    std::size_t pad = 0;
    while (len > 0 && text[len - 1] == '=' && pad < 2) {
        --len;
        ++pad;
    }
    if (pad > 0 && (len + pad) % 4 != 0) return false;
    if (len % 4 == 1) return false;
    const auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::string result;
    result.reserve(len / 4 * 3 + 2);
    std::uint32_t acc = 0;
    int bits = 0;
    for (std::size_t i = 0; i < len; ++i) {
        const int v = value(text[i]);
        if (v < 0) return false;
        acc = (acc << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            result += static_cast<char>((acc >> bits) & 0xFF);
        }
    }
    // Los bits que sobran al final tienen que ser cero (base64 canonico).
    if (bits > 0 && (acc & ((1u << bits) - 1u)) != 0) return false;
    out = std::move(result);
    return true;
}

class WsClient {
public:
    WsClient() = default;
    WsClient(const WsClient&) = delete;
    WsClient& operator=(const WsClient&) = delete;
    WsClient(WsClient&& other) noexcept : id_(std::move(other.id_)), tokens_(std::move(other.tokens_)) {
        other.id_ = Value();
        other.tokens_.clear();
    }
    WsClient& operator=(WsClient&& other) noexcept {
        if (this != &other) {
            forget();
            id_ = std::move(other.id_);
            tokens_ = std::move(other.tokens_);
            other.id_ = Value();
            other.tokens_.clear();
        }
        return *this;
    }
    // WebSocket.off de sus escuchas; NO cierra la conexion.
    ~WsClient() { forget(); }

    // WebSocket.connect; false si devolvio nil (el motivo sale en la consola).
    bool connect(const Value& options) {
        forget();
        id_ = WebSocket::connect(options);
        return id_.truthy();
    }
    // WebSocket.find (tras recarga o cambio de escena).
    bool attach(const std::string& name) {
        forget();
        id_ = WebSocket::find(name);
        return id_.truthy();
    }
    explicit operator bool() const { return id_.truthy(); }
    const Value& id() const { return id_; }

    bool send(const std::string& text) { return id_.truthy() && WebSocket::send(id_, text).truthy(); }
    bool sendJson(const Value& v) { return id_.truthy() && WebSocket::sendJson(id_, v).truthy(); }
    // Los bytes van en base64 (se codifican aqui).
    bool sendBinary(std::string_view bytes) {
        return id_.truthy() && WebSocket::sendBinary(id_, base64Encode(bytes)).truthy();
    }
    void close(int code = 1000, const std::string& reason = {}) {
        if (id_.truthy()) WebSocket::close(id_, code, reason);
    }
    std::string state() const { return id_.truthy() ? WebSocket::state(id_).asString() : std::string("closed"); }
    // Eventos pendientes (si no usas on*). Recomendado con mucho trafico: 1 viaje IPC.
    Values poll(int max = 0) {
        if (!id_.truthy()) return {};
        return WebSocket::poll(id_, max).items();
    }

    void onOpen(std::function<void(const Value&)> fn) { listen("open", std::move(fn)); }
    void onMessage(std::function<void(const Value&)> fn) { listen("message", std::move(fn)); }
    void onClose(std::function<void(const Value&)> fn) { listen("close", std::move(fn)); }
    void onError(std::function<void(const Value&)> fn) { listen("error", std::move(fn)); }
    void onReconnecting(std::function<void(const Value&)> fn) { listen("reconnecting", std::move(fn)); }

private:
    void listen(const char* kind, std::function<void(const Value&)> fn) {
        if (!id_.truthy() || !fn) return;
        Callback cb = [fn = std::move(fn)](const Values& args) { fn(args.empty() ? Value() : args[0]); };
        const Value token = WebSocket::on(id_, kind, Value(std::move(cb)));
        if (token.truthy()) tokens_.push_back(token);
    }
    void forget() noexcept {
        std::vector<Value> tokens;
        tokens.swap(tokens_);
        for (const Value& t : tokens) {
            try {
                WebSocket::off(t);
            } catch (...) {
            }
        }
    }

    Value id_;
    std::vector<Value> tokens_;
};

}  // namespace cramion
