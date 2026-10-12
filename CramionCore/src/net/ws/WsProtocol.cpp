#include "WsProtocol.h"

#include "CramionCore/net/Encoding.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <functional>
#include <thread>

namespace cramion::net::ws {

namespace {

std::string lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

// Lista separada por comas que contiene `token` (sin distinguir mayusculas).
bool hasToken(std::string_view list, std::string_view token) {
    const std::string want = lower(token);
    while (!list.empty()) {
        const std::size_t comma = list.find(',');
        const std::string_view item = trim(list.substr(0, comma));
        if (lower(item) == want) return true;
        if (comma == std::string_view::npos) break;
        list.remove_prefix(comma + 1);
    }
    return false;
}

const char* kErrBadHandshake = "respuesta del servidor no valida (no es un saludo WebSocket correcto)";
const char* kErrUtf8 = "el servidor mando texto que no es UTF-8 valido";

std::string protocolError(const char* detail) {
    return std::string("error de protocolo WebSocket del servidor (") + detail + ")";
}

}  // namespace

std::string makeKey() {
    unsigned char bytes[16];
    if (!secureRandom(bytes, sizeof bytes)) {
        // Sin generador del sistema (no deberia pasar): la clave no es secreta,
        // solo tiene que variar.
        std::uint64_t a = static_cast<std::uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count());
        std::uint64_t b = std::hash<std::thread::id>{}(std::this_thread::get_id()) ^ 0x9E3779B97F4A7C15ull;
        std::memcpy(bytes, &a, 8);
        std::memcpy(bytes + 8, &b, 8);
    }
    return base64Encode(std::string_view(reinterpret_cast<const char*>(bytes), sizeof bytes));
}

std::string acceptFor(std::string_view key) {
    std::string text(key);
    text += kGuid;
    const auto digest = sha1(text);
    return base64Encode(std::string_view(reinterpret_cast<const char*>(digest.data()), digest.size()));
}

std::string buildHandshake(const HandshakeRequest& request) {
    std::string host = request.host;
    if (host.find(':') != std::string::npos) host = "[" + host + "]";  // IPv6
    const bool default_port = (!request.secure && request.port == 80) || (request.secure && request.port == 443);
    if (!default_port && request.port > 0) host += ":" + std::to_string(request.port);

    std::string out;
    out.reserve(256);
    out += "GET ";
    out += request.path.empty() ? std::string("/") : request.path;
    out += " HTTP/1.1\r\n";
    out += "Host: " + host + "\r\n";
    out += "Upgrade: websocket\r\n";
    out += "Connection: Upgrade\r\n";
    out += "Sec-WebSocket-Version: 13\r\n";
    out += "Sec-WebSocket-Key: " + request.key + "\r\n";
    if (!request.protocols.empty()) {
        out += "Sec-WebSocket-Protocol: ";
        for (std::size_t i = 0; i < request.protocols.size(); ++i) {
            if (i > 0) out += ", ";
            out += request.protocols[i];
        }
        out += "\r\n";
    }
    for (const auto& [name, value] : request.headers) out += name + ": " + value + "\r\n";
    out += "\r\n";
    return out;
}

std::size_t headerEnd(std::string_view buffer) {
    const std::size_t pos = buffer.find("\r\n\r\n");
    return pos == std::string_view::npos ? std::string_view::npos : pos + 4;
}

HandshakeResult verifyHandshake(std::string_view head, std::string_view key, const std::vector<std::string>& protocols) {
    HandshakeResult result;
    const std::size_t line_end = head.find("\r\n");
    const std::string_view status_line = head.substr(0, line_end);
    // "HTTP/1.1 101 Switching Protocols"
    if (status_line.size() < 12 || status_line.substr(0, 5) != "HTTP/") {
        result.error = kErrBadHandshake;
        return result;
    }
    const std::size_t sp = status_line.find(' ');
    if (sp == std::string_view::npos || sp + 4 > status_line.size()) {
        result.error = kErrBadHandshake;
        return result;
    }
    int status = 0;
    for (std::size_t i = sp + 1; i < sp + 4; ++i) {
        const char c = status_line[i];
        if (c < '0' || c > '9') {
            result.error = kErrBadHandshake;
            return result;
        }
        status = status * 10 + (c - '0');
    }
    result.status = status;
    if (status != 101) {
        if (status == 401 || status == 403) {
            result.error = "el servidor rechazo la conexion (HTTP " + std::to_string(status) + "): revisa el token o la sesion";
            result.fatal = true;
        } else {
            result.error = "el servidor no acepta WebSocket en esa ruta (respondio HTTP " + std::to_string(status) + ")";
            result.fatal = !(status >= 500 || status == 408 || status == 429);
        }
        return result;
    }

    // Cabeceras (nombres sin distinguir mayusculas; repetidas se juntan con ", ").
    std::string upgrade, connection, accept, extensions, protocol;
    bool has_protocol = false;
    std::string_view rest = line_end == std::string_view::npos ? std::string_view() : head.substr(line_end + 2);
    while (!rest.empty()) {
        const std::size_t eol = rest.find("\r\n");
        const std::string_view line = rest.substr(0, eol);
        rest = eol == std::string_view::npos ? std::string_view() : rest.substr(eol + 2);
        if (line.empty()) break;
        const std::size_t colon = line.find(':');
        if (colon == std::string_view::npos) continue;
        const std::string name = lower(trim(line.substr(0, colon)));
        const std::string_view value = trim(line.substr(colon + 1));
        auto add = [&value](std::string& field) {
            if (!field.empty()) field += ", ";
            field += value;
        };
        if (name == "upgrade") add(upgrade);
        else if (name == "connection") add(connection);
        else if (name == "sec-websocket-accept") add(accept);
        else if (name == "sec-websocket-extensions") add(extensions);
        else if (name == "sec-websocket-protocol") {
            add(protocol);
            has_protocol = true;
        }
    }

    if (lower(upgrade) != "websocket" || !hasToken(connection, "upgrade") || accept != acceptFor(key)) {
        result.error = kErrBadHandshake;
        result.fatal = true;
        return result;
    }
    if (!trim(extensions).empty()) {
        result.error = "el servidor pidio extensiones no soportadas (compresion)";
        result.fatal = true;
        return result;
    }
    if (has_protocol && !protocol.empty()) {
        if (std::find(protocols.begin(), protocols.end(), protocol) == protocols.end()) {
            result.error = "el servidor eligio un subprotocolo que no se pidio (" + protocol + ")";
            result.fatal = true;
            return result;
        }
        result.protocol = protocol;
    } else if (!protocols.empty()) {
        result.error = "el servidor no acepto ningun subprotocolo";
        result.fatal = true;
        return result;
    }
    result.ok = true;
    return result;
}

std::string encodeFrame(Opcode op, std::string_view payload, bool fin, std::uint32_t mask) {
    const std::size_t n = payload.size();
    std::string out;
    out.resize(2 + (n < 126 ? 0 : n <= 0xFFFF ? 2 : 8) + 4 + n);
    auto* p = reinterpret_cast<unsigned char*>(out.data());
    std::size_t pos = 0;
    p[pos++] = static_cast<unsigned char>((fin ? 0x80 : 0x00) | static_cast<unsigned char>(op));
    if (n < 126) {
        p[pos++] = static_cast<unsigned char>(0x80 | n);
    } else if (n <= 0xFFFF) {
        p[pos++] = 0x80 | 126;
        p[pos++] = static_cast<unsigned char>(n >> 8);
        p[pos++] = static_cast<unsigned char>(n);
    } else {
        p[pos++] = 0x80 | 127;
        const std::uint64_t len = n;
        for (int k = 7; k >= 0; --k) p[pos++] = static_cast<unsigned char>(len >> (8 * k));
    }
    const unsigned char key[4] = {static_cast<unsigned char>(mask >> 24), static_cast<unsigned char>(mask >> 16),
                                  static_cast<unsigned char>(mask >> 8), static_cast<unsigned char>(mask)};
    std::memcpy(p + pos, key, 4);
    pos += 4;
    const auto* src = reinterpret_cast<const unsigned char*>(payload.data());
    unsigned char* dst = p + pos;
    std::size_t i = 0;
    // De 8 en 8 bytes (la mascara repetida), el resto byte a byte.
    std::uint64_t key8;
    {
        unsigned char k8[8] = {key[0], key[1], key[2], key[3], key[0], key[1], key[2], key[3]};
        std::memcpy(&key8, k8, 8);
    }
    for (; i + 8 <= n; i += 8) {
        std::uint64_t w;
        std::memcpy(&w, src + i, 8);
        w ^= key8;
        std::memcpy(dst + i, &w, 8);
    }
    for (; i < n; ++i) dst[i] = src[i] ^ key[i & 3];
    return out;
}

std::string closePayload(int code, std::string_view reason) {
    if (code == 0) return {};
    std::string out;
    out += static_cast<char>((code >> 8) & 0xFF);
    out += static_cast<char>(code & 0xFF);
    out += truncateUtf8(reason, kMaxCloseReason);
    return out;
}

bool validReceivedCloseCode(int code) {
    return (code >= 1000 && code <= 1003) || (code >= 1007 && code <= 1014) || (code >= 3000 && code <= 4999);
}

bool validSendCloseCode(int code) { return code == 1000 || (code >= 3000 && code <= 4999); }

// --- Masker ------------------------------------------------------------------

Masker::Masker() {
    std::uint64_t seed = 0;
    if (!secureRandom(&seed, sizeof seed)) {
        seed = static_cast<std::uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count()) ^
               (reinterpret_cast<std::uintptr_t>(this) * 0x9E3779B97F4A7C15ull);
    }
    s_ = seed != 0 ? seed : 0x2545F4914F6CDD1Dull;
}

std::uint32_t Masker::next() {
    s_ ^= s_ >> 12;
    s_ ^= s_ << 25;
    s_ ^= s_ >> 27;
    return static_cast<std::uint32_t>((s_ * 0x2545F4914F6CDD1Dull) >> 32);
}

// --- FrameParser ---------------------------------------------------------------

FrameParser::FrameParser(std::size_t max_message_bytes) : max_(max_message_bytes) {}

bool FrameParser::failed() const { return failed_; }

void FrameParser::fail(int code, std::string text, std::vector<Item>& out) {
    failed_ = true;
    msg_.clear();
    msg_.shrink_to_fit();
    ctrl_.clear();
    Item item;
    item.kind = Item::Kind::Error;
    item.code = code;
    item.data = std::move(text);
    out.push_back(std::move(item));
}

bool FrameParser::startFrame(std::vector<Item>& out) {
    const unsigned char b0 = hdr_[0];
    const unsigned char b1 = hdr_[1];
    frame_fin_ = (b0 & 0x80) != 0;
    const unsigned op = b0 & 0x0F;
    frame_op_ = static_cast<Opcode>(op);
    const bool control = (op & 0x08) != 0;

    std::uint64_t len = b1 & 0x7F;
    if (len == 126) {
        len = (std::uint64_t(hdr_[2]) << 8) | hdr_[3];
    } else if (len == 127) {
        len = 0;
        for (int k = 0; k < 8; ++k) len = (len << 8) | hdr_[2 + k];
        if (len >> 63) {
            fail(1002, protocolError("longitud de trama no valida"), out);
            return false;
        }
    }

    if (control) {
        if (len > kMaxControlPayload) {
            fail(1002, protocolError("trama de control de mas de 125 bytes"), out);
            return false;
        }
        ctrl_.clear();
        ctrl_.reserve(static_cast<std::size_t>(len));
    } else {
        if (frame_op_ == Opcode::Continuation) {
            if (!in_message_) {
                fail(1002, protocolError("continuacion sin mensaje empezado"), out);
                return false;
            }
        } else {
            if (in_message_) {
                fail(1002, protocolError("mensaje nuevo sin terminar el anterior"), out);
                return false;
            }
            in_message_ = true;
            msg_op_ = frame_op_;
            msg_.clear();
        }
        // Tamano ANTES de reservar: una longitud enorme no pide memoria.
        if (len > max_ || msg_.size() > max_ - static_cast<std::size_t>(len)) {
            fail(1009, "mensaje demasiado grande (mas de " + std::to_string(max_) + " bytes, maxSize)", out);
            return false;
        }
        // Crecimiento geometrico: muchos trozos pequenos no copian el mensaje
        // entero cada vez (reserve de libc++ pide justo lo pedido).
        const std::size_t need = msg_.size() + static_cast<std::size_t>(len);
        if (need > msg_.capacity()) msg_.reserve(std::min(max_, std::max(need, msg_.capacity() * 2)));
    }
    remaining_ = len;
    in_payload_ = true;
    return true;
}

void FrameParser::endFrame(std::vector<Item>& out) {
    in_payload_ = false;
    hdr_len_ = 0;
    Item item;
    switch (frame_op_) {
        case Opcode::Ping:
        case Opcode::Pong:
            item.kind = frame_op_ == Opcode::Ping ? Item::Kind::Ping : Item::Kind::Pong;
            item.data = std::move(ctrl_);
            ctrl_.clear();
            out.push_back(std::move(item));
            return;
        case Opcode::Close: {
            if (ctrl_.size() == 1) {
                fail(1002, protocolError("cierre de 1 byte"), out);
                return;
            }
            item.kind = Item::Kind::Close;
            if (ctrl_.empty()) {
                item.code = 1005;
            } else {
                const int code = (static_cast<unsigned char>(ctrl_[0]) << 8) | static_cast<unsigned char>(ctrl_[1]);
                if (!validReceivedCloseCode(code)) {
                    fail(1002, protocolError(("codigo de cierre no valido " + std::to_string(code)).c_str()), out);
                    return;
                }
                std::string_view reason(ctrl_);
                reason.remove_prefix(2);
                if (!validUtf8(reason)) {
                    fail(1007, kErrUtf8, out);
                    return;
                }
                item.code = code;
                item.data = std::string(reason);
            }
            ctrl_.clear();
            closed_ = true;
            out.push_back(std::move(item));
            return;
        }
        default: break;
    }
    // Trama de datos.
    if (!frame_fin_) return;  // faltan trozos
    in_message_ = false;
    if (msg_op_ == Opcode::Text) {
        if (!validUtf8(msg_)) {
            fail(1007, kErrUtf8, out);
            return;
        }
        item.kind = Item::Kind::Text;
    } else {
        item.kind = Item::Kind::Binary;
    }
    item.data = std::move(msg_);
    msg_ = std::string();
    out.push_back(std::move(item));
}

void FrameParser::feed(const char* data, std::size_t n, std::vector<Item>& out) {
    const auto* p = reinterpret_cast<const unsigned char*>(data);
    while (!failed_ && !closed_) {
        if (!in_payload_) {
            if (n == 0) return;
            while (hdr_len_ < 2 && n > 0) {
                hdr_[hdr_len_++] = *p++;
                --n;
            }
            if (hdr_len_ < 2) return;
            const unsigned char b0 = hdr_[0];
            const unsigned char b1 = hdr_[1];
            // Comprobaciones con los 2 primeros bytes (antes de esperar mas).
            if (b1 & 0x80) {
                fail(1002, protocolError("trama del servidor con mascara"), out);
                return;
            }
            if (b0 & 0x70) {
                fail(1002, protocolError("bits RSV sin extension"), out);
                return;
            }
            const unsigned op = b0 & 0x0F;
            if (!(op == 0x0 || op == 0x1 || op == 0x2 || op == 0x8 || op == 0x9 || op == 0xA)) {
                fail(1002, protocolError(("opcode desconocido " + std::to_string(op)).c_str()), out);
                return;
            }
            if ((op & 0x08) && !(b0 & 0x80)) {
                fail(1002, protocolError("trama de control partida en trozos"), out);
                return;
            }
            const unsigned len7 = b1 & 0x7F;
            if ((op & 0x08) && len7 > kMaxControlPayload) {
                fail(1002, protocolError("trama de control de mas de 125 bytes"), out);
                return;
            }
            const std::size_t need = 2 + (len7 == 126 ? 2 : len7 == 127 ? 8 : 0);
            while (hdr_len_ < need && n > 0) {
                hdr_[hdr_len_++] = *p++;
                --n;
            }
            if (hdr_len_ < need) return;
            if (!startFrame(out)) return;
            if (remaining_ == 0) {
                endFrame(out);
                continue;
            }
        }
        if (n == 0) return;
        const std::size_t take = static_cast<std::size_t>(std::min<std::uint64_t>(remaining_, n));
        if (frame_op_ == Opcode::Close || frame_op_ == Opcode::Ping || frame_op_ == Opcode::Pong)
            ctrl_.append(reinterpret_cast<const char*>(p), take);
        else
            msg_.append(reinterpret_cast<const char*>(p), take);
        p += take;
        n -= take;
        remaining_ -= take;
        if (remaining_ == 0) endFrame(out);
    }
}

}  // namespace cramion::net::ws
