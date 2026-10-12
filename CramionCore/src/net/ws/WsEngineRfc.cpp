// Motor propio de WebSocket (RFC 6455) sobre un ByteStream: TCP, OpenSSL
// (Linux) o SSLSocket (Android). En Windows solo con force_rfc6455 (pruebas).

#include "WsEngine.h"
#include "WsProtocol.h"

#include "../NetCommon.h"

#include <algorithm>
#include <atomic>
#include <deque>
#include <mutex>

namespace cramion::net::ws {

namespace {

constexpr std::size_t kReadChunk = 64u << 10;
constexpr std::size_t kFragment = 1u << 20;  // mensajes de mas de 1 MB van en trozos de 1 MB
constexpr const char* kErrCancelled = "conexion cancelada";

class RfcEngine final : public Engine {
public:
    bool connect(const ConnectInfo& info, std::string& protocol, Failure& failure) override {
        failure = Failure{};
        protocol.clear();

        std::unique_ptr<ByteStream> stream;
        if (info.url.secure) {
            std::string error;
            stream = makeTlsStream(&error);
            if (!stream) {
                failure.text = error.empty() ? std::string(detail::kErrNoTls) : error;
                failure.fatal = true;
                return false;
            }
        } else {
            stream = makePlainStream();
        }
        ByteStream* s = stream.get();
        {
            std::lock_guard<std::mutex> lock(mu_);
            stream_ = std::move(stream);
        }
        if (aborted_.load()) {
            failure.text = kErrCancelled;
            return false;
        }

        std::string error;
        bool fatal = false;
        if (!s->connect(info.url.host, info.url.port, info.timeout_ms, info.policy, error, fatal)) {
            failure.text = aborted_.load() ? std::string(kErrCancelled) : error;
            failure.fatal = fatal && !aborted_.load();
            return false;
        }

        // Saludo HTTP.
        HandshakeRequest request;
        request.host = info.url.host;
        request.port = info.url.port;
        request.secure = info.url.secure;
        request.path = info.url.path.empty() ? std::string("/") : info.url.path;
        request.headers = info.headers;
        request.protocols = info.protocols;
        request.key = makeKey();
        const std::string hello = buildHandshake(request);
        if (!s->write(hello.data(), hello.size(), error)) {
            failure.text = aborted_.load() ? std::string(kErrCancelled) : (error.empty() ? detail::kErrCut : error);
            return false;
        }

        std::string buffer;
        std::size_t end = std::string::npos;
        char chunk[4096];
        while (end == std::string::npos) {
            const long n = s->read(chunk, sizeof chunk, error);
            if (n <= 0) {
                if (aborted_.load()) {
                    failure.text = kErrCancelled;
                } else {
                    failure.text = n == 0 || error.empty() ? std::string(detail::kErrCut) : error;
                }
                return false;
            }
            buffer.append(chunk, static_cast<std::size_t>(n));
            end = headerEnd(buffer);
            if (end == std::string::npos && buffer.size() > kMaxHandshakeBytes) {
                failure.text = "respuesta del servidor no valida (no es un saludo WebSocket correcto)";
                return false;
            }
        }

        const HandshakeResult result =
            verifyHandshake(std::string_view(buffer).substr(0, end), request.key, info.protocols);
        if (!result.ok) {
            failure.text = result.error;
            failure.http_status = result.status;
            failure.fatal = result.fatal;
            return false;
        }
        protocol = result.protocol;

        parser_ = std::make_unique<FrameParser>(info.max_message_bytes);
        // Lo que llego detras del saludo ya son tramas.
        if (buffer.size() > end) {
            std::vector<FrameParser::Item> items;
            parser_->feed(buffer.data() + end, buffer.size() - end, items);
            for (auto& item : items) pending_.push_back(std::move(item));
        }
        connected_ = true;
        return true;
    }

    RxItem receive() override {
        RxItem rx;
        if (!connected_) {
            rx.kind = aborted_.load() ? RxItem::Kind::Aborted : RxItem::Kind::Failed;
            rx.failure.text = detail::kErrCut;
            return rx;
        }
        for (;;) {
            if (!pending_.empty()) {
                FrameParser::Item item = std::move(pending_.front());
                pending_.pop_front();
                switch (item.kind) {
                    case FrameParser::Item::Kind::Text:
                    case FrameParser::Item::Kind::Binary:
                        rx.kind = RxItem::Kind::Message;
                        rx.binary = item.kind == FrameParser::Item::Kind::Binary;
                        rx.data = std::move(item.data);
                        return rx;
                    case FrameParser::Item::Kind::Ping:
                        rx.kind = RxItem::Kind::Ping;
                        rx.data = std::move(item.data);
                        return rx;
                    case FrameParser::Item::Kind::Pong:
                        rx.kind = RxItem::Kind::Pong;
                        rx.data = std::move(item.data);
                        return rx;
                    case FrameParser::Item::Kind::Close:
                        close_received_.store(true);
                        rx.kind = RxItem::Kind::PeerClose;
                        rx.code = item.code;
                        rx.data = std::move(item.data);
                        return rx;
                    case FrameParser::Item::Kind::Error:
                        parser_failed_ = true;
                        rx.kind = RxItem::Kind::ProtocolError;
                        rx.code = item.code;
                        rx.failure.text = std::move(item.data);
                        rx.failure.fatal = true;
                        return rx;
                }
            }
            if (aborted_.load()) {
                rx.kind = RxItem::Kind::Aborted;
                return rx;
            }
            std::string error;
            if (rbuf_.size() != kReadChunk) rbuf_.resize(kReadChunk);
            const long n = stream_->read(rbuf_.data(), rbuf_.size(), error);
            if (n < 0) {
                if (aborted_.load()) {
                    rx.kind = RxItem::Kind::Aborted;
                    return rx;
                }
                rx.kind = RxItem::Kind::Failed;
                rx.failure.text = error.empty() ? std::string(detail::kErrCut) : error;
                return rx;
            }
            if (n == 0) {
                if (aborted_.load()) {
                    rx.kind = RxItem::Kind::Aborted;
                    return rx;
                }
                // Fin de TCP: normal despues de un cierre (o de un error de protocolo,
                // que ya mando su cierre); si no, la conexion se corto.
                if (close_received_.load() || close_sent_.load() || parser_failed_) {
                    rx.kind = RxItem::Kind::Ended;
                } else {
                    rx.kind = RxItem::Kind::Failed;
                    rx.failure.text = detail::kErrCut;
                }
                return rx;
            }
            // Tras un error o el cierre del servidor solo se busca el fin de TCP.
            if (parser_failed_ || close_received_.load()) continue;
            std::vector<FrameParser::Item> items;
            parser_->feed(rbuf_.data(), static_cast<std::size_t>(n), items);
            for (auto& item : items) pending_.push_back(std::move(item));
        }
    }

    bool sendMessage(std::string_view data, bool binary, std::string& error) override {
        if (data.size() <= kFragment) {
            return sendFrame(binary ? Opcode::Binary : Opcode::Text, data, true, error);
        }
        std::size_t offset = 0;
        bool first = true;
        while (offset < data.size()) {
            const std::size_t len = std::min(kFragment, data.size() - offset);
            const bool fin = offset + len == data.size();
            const Opcode op = first ? (binary ? Opcode::Binary : Opcode::Text) : Opcode::Continuation;
            if (!sendFrame(op, data.substr(offset, len), fin, error)) return false;
            offset += len;
            first = false;
        }
        return true;
    }

    bool sendPing(std::string_view payload, std::string& error) override {
        return sendFrame(Opcode::Ping, payload.substr(0, std::min(payload.size(), kMaxControlPayload)), true, error);
    }

    bool sendPong(std::string_view payload, std::string& error) override {
        return sendFrame(Opcode::Pong, payload.substr(0, std::min(payload.size(), kMaxControlPayload)), true, error);
    }

    bool sendClose(int code, std::string_view reason, std::string& error) override {
        if (close_sent_.exchange(true)) return true;  // una vez
        return sendFrame(Opcode::Close, closePayload(code, reason), true, error);
    }

    void abort() override {
        if (aborted_.exchange(true)) return;
        std::lock_guard<std::mutex> lock(mu_);
        if (stream_) stream_->abort();
    }

    bool supportsPing() const override { return true; }
    bool waitsForEof() const override { return true; }

private:
    bool sendFrame(Opcode op, std::string_view payload, bool fin, std::string& error) {
        if (aborted_.load()) {
            error = kErrCancelled;
            return false;
        }
        if (!connected_ || !stream_) {
            error = detail::kErrCut;
            return false;
        }
        const std::string frame = encodeFrame(op, payload, fin, masker_.next());
        if (!stream_->write(frame.data(), frame.size(), error)) {
            if (error.empty()) error = aborted_.load() ? kErrCancelled : detail::kErrCut;
            return false;
        }
        return true;
    }

    std::mutex mu_;                       // stream_ al publicarlo (abort desde otro hilo)
    std::unique_ptr<ByteStream> stream_;  // vive hasta el destructor
    std::atomic<bool> aborted_{false};
    std::atomic<bool> close_sent_{false};
    std::atomic<bool> close_received_{false};
    bool connected_ = false;              // lo escribe connect antes de arrancar el lector
    bool parser_failed_ = false;          // solo el lector
    std::unique_ptr<FrameParser> parser_;
    std::deque<FrameParser::Item> pending_;
    std::string rbuf_;
    Masker masker_;                       // solo el hilo de sesion
};

}  // namespace

std::shared_ptr<Engine> makeRfcEngine() { return std::make_shared<RfcEngine>(); }

}  // namespace cramion::net::ws
