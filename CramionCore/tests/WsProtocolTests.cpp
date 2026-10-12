// WebSocket sin red: SHA-1, base64, UTF-8 (net/Encoding), el saludo y las
// tramas del RFC 6455 (src/net/ws/WsProtocol). Cada caso del lector de tramas
// se prueba entero, byte a byte y cortado al azar (como llega por TCP).

#include "CramionCore/net/Encoding.h"
#include "net/ws/WsProtocol.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using namespace cramion;
using namespace cramion::net;
using namespace cramion::net::ws;

namespace {

int checks = 0;
int failures = 0;

void check(bool condition, const char* what) {
    ++checks;
    std::printf("  %s %s\n", condition ? "OK   " : "FALLO", what);
    if (!condition) ++failures;
}

std::string hex(const std::array<std::uint8_t, 20>& d) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (std::uint8_t b : d) {
        out += digits[b >> 4];
        out += digits[b & 15];
    }
    return out;
}

// Trama del servidor (sin mascara salvo que se pida). len_override: la longitud
// que se anuncia (para mentir); -1 = la del payload.
std::string frame(unsigned char b0, const std::string& payload, bool masked = false, long long len_override = -1) {
    std::string out;
    out += static_cast<char>(b0);
    const std::uint64_t len = len_override >= 0 ? static_cast<std::uint64_t>(len_override) : payload.size();
    const unsigned char m = masked ? 0x80 : 0x00;
    if (len < 126) {
        out += static_cast<char>(m | len);
    } else if (len <= 0xFFFF) {
        out += static_cast<char>(m | 126);
        out += static_cast<char>((len >> 8) & 0xFF);
        out += static_cast<char>(len & 0xFF);
    } else {
        out += static_cast<char>(m | 127);
        for (int k = 7; k >= 0; --k) out += static_cast<char>((len >> (8 * k)) & 0xFF);
    }
    if (masked) out += std::string("\x01\x02\x03\x04", 4);
    out += payload;
    return out;
}

constexpr unsigned char FIN = 0x80;

struct Expect {
    FrameParser::Item::Kind kind;
    int code = 0;
    std::string data;
};

struct Case {
    const char* name;
    std::string bytes;
    std::vector<Expect> expect;
    std::size_t max = 1u << 20;
};

using Kind = FrameParser::Item::Kind;

std::vector<FrameParser::Item> runSplit(const Case& c, std::mt19937* rng, bool byte_by_byte) {
    FrameParser parser(c.max);
    std::vector<FrameParser::Item> out;
    std::size_t pos = 0;
    while (pos < c.bytes.size()) {
        std::size_t take = c.bytes.size() - pos;
        if (byte_by_byte) {
            take = 1;
        } else if (rng != nullptr) {
            std::uniform_int_distribution<std::size_t> d(1, std::min<std::size_t>(take, 70000));
            take = d(*rng);
        }
        parser.feed(c.bytes.data() + pos, take, out);
        pos += take;
    }
    return out;
}

bool matches(const Case& c, const std::vector<FrameParser::Item>& got) {
    if (got.size() != c.expect.size()) return false;
    for (std::size_t i = 0; i < got.size(); ++i) {
        if (got[i].kind != c.expect[i].kind) return false;
        if (got[i].code != c.expect[i].code) return false;
        if (got[i].kind != Kind::Error && got[i].data != c.expect[i].data) return false;
    }
    return true;
}

std::string bytesOf(std::size_t n, unsigned char seed) {
    std::string s(n, '\0');
    for (std::size_t i = 0; i < n; ++i) s[i] = static_cast<char>((i * 31 + seed) & 0xFF);
    return s;
}

std::string textOf(std::size_t n) {
    std::string s(n, 'a');
    for (std::size_t i = 0; i < n; ++i) s[i] = static_cast<char>('a' + i % 26);
    return s;
}

// Deshace una trama del cliente (con mascara). false si la cabecera no cuadra.
bool unmaskClient(const std::string& f, Opcode& op, bool& fin, std::string& payload) {
    if (f.size() < 6) return false;
    const auto* p = reinterpret_cast<const unsigned char*>(f.data());
    fin = (p[0] & 0x80) != 0;
    op = static_cast<Opcode>(p[0] & 0x0F);
    if (!(p[1] & 0x80)) return false;
    std::uint64_t len = p[1] & 0x7F;
    std::size_t pos = 2;
    if (len == 126) {
        len = (std::uint64_t(p[2]) << 8) | p[3];
        if (len < 126) return false;
        pos = 4;
    } else if (len == 127) {
        len = 0;
        for (int k = 0; k < 8; ++k) len = (len << 8) | p[2 + k];
        if (len <= 0xFFFF) return false;
        pos = 10;
    }
    const unsigned char key[4] = {p[pos], p[pos + 1], p[pos + 2], p[pos + 3]};
    pos += 4;
    if (f.size() - pos != len) return false;
    payload.resize(static_cast<std::size_t>(len));
    for (std::size_t i = 0; i < len; ++i) payload[i] = static_cast<char>(p[pos + i] ^ key[i & 3]);
    return true;
}

const char* kKey = "dGhlIHNhbXBsZSBub25jZQ==";
// RFC 6455 seccion 1.3: base64(sha1(kKey + GUID)). El valor anterior
// ("...9kYGAzzGpuxq0+s=") estaba mal copiado y hacia fallar los saludos validos.
const char* kAccept = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=";

std::string response(const std::string& extra, const char* accept = kAccept, const char* status = "101 Switching Protocols") {
    std::string r = std::string("HTTP/1.1 ") + status + "\r\n";
    r += "upgrade: WebSocket\r\n";
    r += "CONNECTION: keep-alive, Upgrade\r\n";
    if (accept != nullptr) r += std::string("Sec-WebSocket-ACCEPT: ") + accept + "\r\n";
    r += extra;
    r += "\r\n";
    return r;
}

}  // namespace

int main() {
    std::printf("SHA-1 y saludo\n");
    check(hex(sha1("abc")) == "a9993e364706816aba3e25717850c26c9cd0d89d", "sha1(\"abc\")");
    check(hex(sha1("")) == "da39a3ee5e6b4b0d3255bfef95601890afd80709", "sha1(\"\")");
    check(hex(sha1("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")) ==
              "84983e441c3bd26ebaae4aa1f95129e5e54670f1",
          "sha1 de 56 bytes (relleno en dos bloques)");
    check(hex(sha1(std::string(1000000, 'a'))) == "34aa973cd4c4daa4f61eeb2bdbad27316534016f", "sha1 de un millon de 'a'");
    check(acceptFor(kKey) == kAccept, "acceptFor: el ejemplo del RFC 6455");

    std::printf("Base64\n");
    {
        check(base64Encode("") == "" && base64Encode("f") == "Zg==" && base64Encode("fo") == "Zm8=" &&
                  base64Encode("foo") == "Zm9v" && base64Encode("foobar") == "Zm9vYmFy",
              "base64Encode: vectores del RFC 4648");
        std::string out;
        check(base64Decode("Zm9vYmFy", out) && out == "foobar", "base64Decode con relleno exacto");
        check(base64Decode("Zm9vYg==", out) && out == "foob", "base64Decode con '=='");
        check(base64Decode("Zm9vYg", out) && out == "foob", "base64Decode sin relleno");
        check(base64Decode("Zm9vYmE", out) && out == "fooba", "base64Decode sin relleno (1)");
        check(base64Decode("", out) && out.empty(), "base64Decode vacio");
        bool bad = true;
        for (const char* text : {"Zm9v!", "Z", "Zm9vY", "Zm=v", "Zm9vY===", "Zm9 v", "Zm9v\n", "Zg=", "=Zg="}) {
            std::string o = "x";
            if (base64Decode(text, o) || !o.empty()) bad = false;
        }
        check(bad, "base64Decode rechaza lo no valido (y deja out vacio)");
        std::mt19937 rng(1234);
        bool round = true;
        for (int i = 0; i < 300; ++i) {
            std::string data(static_cast<std::size_t>(i), '\0');
            for (char& ch : data) ch = static_cast<char>(rng() & 0xFF);
            std::string back;
            if (!base64Decode(base64Encode(data), back) || back != data) round = false;
        }
        check(round, "base64: ida y vuelta de bytes al azar (0..299)");
    }

    std::printf("UTF-8\n");
    {
        check(validUtf8("hola") && validUtf8("") && validUtf8("\xC3\xB1") && validUtf8("\xF0\x9F\x98\x80"),
              "validUtf8: ascii, vacio, n con tilde y emoji");
        check(validUtf8("\xE2\x82\xAC") && validUtf8("\xF4\x8F\xBF\xBF"), "validUtf8: euro y U+10FFFF");
        check(!validUtf8("\xC0\xAF") && !validUtf8("\xE0\x80\xAF") && !validUtf8("\xF0\x80\x80\xAF"),
              "validUtf8: sobrelargos no");
        check(!validUtf8("\xED\xA0\x80") && !validUtf8("\xED\xBF\xBF"), "validUtf8: sustitutos no");
        check(!validUtf8("\xF4\x90\x80\x80") && !validUtf8("\xF5\x80\x80\x80"), "validUtf8: mas alla de U+10FFFF no");
        check(!validUtf8("\xC3") && !validUtf8("a\xF0\x9F\x98") && !validUtf8("\x80"),
              "validUtf8: cortados y continuacion suelta no");
        check(truncateUtf8("abc", 10) == "abc" && truncateUtf8("abc", 2) == "ab", "truncateUtf8 ascii");
        check(truncateUtf8("a\xC3\xB1", 2) == "a" && truncateUtf8("\xF0\x9F\x98\x80z", 3) == "",
              "truncateUtf8 no parte un caracter");
    }

    std::printf("Tramas del cliente\n");
    {
        bool ok = true;
        Masker masker;
        for (std::size_t n : {0u, 1u, 125u, 126u, 65535u, 65536u, 70001u}) {
            const std::string payload = bytesOf(n, 7);
            const std::string f = encodeFrame(Opcode::Binary, payload, true, masker.next());
            Opcode op;
            bool fin;
            std::string back;
            if (!unmaskClient(f, op, fin, back) || op != Opcode::Binary || !fin || back != payload) ok = false;
        }
        check(ok, "encodeFrame: con mascara y longitudes 0, 125, 126, 65535, 65536");
        Opcode op;
        bool fin = true;
        std::string back;
        check(unmaskClient(encodeFrame(Opcode::Text, "trozo", false, 0xDEADBEEF), op, fin, back) && !fin &&
                  op == Opcode::Text && back == "trozo",
              "encodeFrame: trozo sin FIN");
        const std::uint32_t a = masker.next();
        const std::uint32_t b = masker.next();
        check(a != b, "Masker da mascaras distintas");

        const std::string cp = closePayload(1000, "adios");
        check(cp.size() == 7 && static_cast<unsigned char>(cp[0]) == 0x03 && static_cast<unsigned char>(cp[1]) == 0xE8 &&
                  cp.substr(2) == "adios",
              "closePayload(1000, \"adios\")");
        check(closePayload(0, "x").empty(), "closePayload sin codigo: vacio");
        std::string long_reason;
        for (int i = 0; i < 100; ++i) long_reason += "\xC3\xB1";  // 200 bytes
        const std::string cl = closePayload(4001, long_reason);
        check(cl.size() <= 2 + kMaxCloseReason && validUtf8(cl.substr(2)), "closePayload corta el motivo a 123 bytes UTF-8");
        check(validReceivedCloseCode(1000) && validReceivedCloseCode(1003) && validReceivedCloseCode(1007) &&
                  validReceivedCloseCode(1014) && validReceivedCloseCode(3000) && validReceivedCloseCode(4999),
              "codigos de cierre recibidos validos");
        check(!validReceivedCloseCode(999) && !validReceivedCloseCode(1004) && !validReceivedCloseCode(1005) &&
                  !validReceivedCloseCode(1006) && !validReceivedCloseCode(1015) && !validReceivedCloseCode(2999) &&
                  !validReceivedCloseCode(5000),
              "codigos de cierre recibidos no validos");
        check(validSendCloseCode(1000) && validSendCloseCode(3000) && validSendCloseCode(4999) &&
                  !validSendCloseCode(1001) && !validSendCloseCode(1006) && !validSendCloseCode(5000),
              "codigos de cierre que puede pedir un script");
    }

    std::printf("Tramas del servidor\n");
    std::vector<Case> cases;
    for (std::size_t n : {0u, 125u, 126u, 65535u, 65536u}) {
        const std::string t = textOf(n);
        cases.push_back({"texto", frame(FIN | 0x1, t), {{Kind::Text, 0, t}}});
    }
    {
        const std::string bin = bytesOf(65536, 3);
        cases.push_back({"binario de 65536 con ceros", frame(FIN | 0x2, bin), {{Kind::Binary, 0, bin}}});
    }
    cases.push_back({"trama con mascara -> 1002", frame(FIN | 0x1, "hola", true), {{Kind::Error, 1002, ""}}});
    cases.push_back({"bits RSV -> 1002", frame(FIN | 0x40 | 0x1, "hola"), {{Kind::Error, 1002, ""}}});
    cases.push_back({"opcode desconocido -> 1002", frame(FIN | 0x3, "x"), {{Kind::Error, 1002, ""}}});
    cases.push_back({"control de 126 bytes -> 1002", frame(FIN | 0x9, std::string(126, 'p')), {{Kind::Error, 1002, ""}}});
    cases.push_back({"ping partido -> 1002", frame(0x9, "p"), {{Kind::Error, 1002, ""}}});
    cases.push_back({"continuacion sin mensaje -> 1002", frame(FIN | 0x0, "x"), {{Kind::Error, 1002, ""}}});
    cases.push_back({"mensaje nuevo a medias -> 1002", frame(0x1, "a") + frame(FIN | 0x1, "b"), {{Kind::Error, 1002, ""}}});
    cases.push_back({"ping entre trozos",
                     frame(0x1, "Hola ") + frame(FIN | 0x9, "p") + frame(FIN | 0x0, "mundo"),
                     {{Kind::Ping, 0, "p"}, {Kind::Text, 0, "Hola mundo"}}});
    cases.push_back({"trozos con trozo vacio",
                     frame(0x2, "ab") + frame(0x0, "") + frame(FIN | 0x0, "cd") + frame(FIN | 0xA, "q"),
                     {{Kind::Binary, 0, "abcd"}, {Kind::Pong, 0, "q"}}});
    cases.push_back({"cierre de 1 byte -> 1002", frame(FIN | 0x8, "\x03"), {{Kind::Error, 1002, ""}}});
    cases.push_back({"cierre con codigo 1005 -> 1002", frame(FIN | 0x8, "\x03\xED"), {{Kind::Error, 1002, ""}}});
    cases.push_back({"cierre con codigo 999 -> 1002", frame(FIN | 0x8, "\x03\xE7"), {{Kind::Error, 1002, ""}}});
    cases.push_back({"cierre 4001 baneado", frame(FIN | 0x8, "\x0F\xA1" "baneado"), {{Kind::Close, 4001, "baneado"}}});
    cases.push_back({"cierre vacio -> 1005", frame(FIN | 0x8, ""), {{Kind::Close, 1005, ""}}});
    cases.push_back({"tras el cierre no sale nada", frame(FIN | 0x8, "\x03\xE8") + frame(FIN | 0x1, "x"),
                     {{Kind::Close, 1000, ""}}});
    cases.push_back({"longitud 2^62 -> 1009 sin reservar", frame(FIN | 0x1, "", false, 1LL << 62), {{Kind::Error, 1009, ""}}});
    cases.push_back({"longitud con el bit alto -> 1002",
                     std::string("\x81\x7F\x80\x00\x00\x00\x00\x00\x00\x01", 10), {{Kind::Error, 1002, ""}}});
    {
        Case big{"trozos que suman mas del maximo -> 1009",
                 frame(0x2, std::string(60, 'a')) + frame(FIN | 0x0, std::string(60, 'b')),
                 {{Kind::Error, 1009, ""}}};
        big.max = 100;
        cases.push_back(big);
        Case exact{"mensaje justo del maximo", frame(FIN | 0x2, std::string(100, 'a')), {{Kind::Binary, 0, std::string(100, 'a')}}};
        exact.max = 100;
        cases.push_back(exact);
    }
    cases.push_back({"UTF-8 roto entre trozos -> 1007", frame(0x1, "\xC3") + frame(FIN | 0x0, "\x28"),
                     {{Kind::Error, 1007, ""}}});
    cases.push_back({"UTF-8 partido entre trozos vale", frame(0x1, "a\xC3") + frame(FIN | 0x0, "\xB1"),
                     {{Kind::Text, 0, "a\xC3\xB1"}}});
    cases.push_back({"motivo de cierre no UTF-8 -> 1007", frame(FIN | 0x8, "\x03\xE8\xFF"), {{Kind::Error, 1007, ""}}});
    cases.push_back({"tras un error no sale nada", frame(FIN | 0x1, "x", true) + frame(FIN | 0x1, "y"),
                     {{Kind::Error, 1002, ""}}});
    cases.push_back({"varios mensajes seguidos",
                     frame(FIN | 0x1, "uno") + frame(FIN | 0x2, std::string("\0\1", 2)) + frame(FIN | 0x9, "") +
                         frame(FIN | 0x1, "dos"),
                     {{Kind::Text, 0, "uno"}, {Kind::Binary, 0, std::string("\0\1", 2)}, {Kind::Ping, 0, ""},
                      {Kind::Text, 0, "dos"}}});

    std::mt19937 rng(20240611);
    for (const Case& c : cases) {
        const bool whole = matches(c, runSplit(c, nullptr, false));
        const bool bytes = matches(c, runSplit(c, nullptr, true));
        const std::string label = std::string(c.name) + " (entero y byte a byte)";
        check(whole && bytes, label.c_str());
    }
    {
        bool ok = true;
        for (int i = 0; i < 1000; ++i) {
            const Case& c = cases[static_cast<std::size_t>(i) % cases.size()];
            if (!matches(c, runSplit(c, &rng, false))) {
                std::printf("    cortes al azar fallan en: %s (iteracion %d)\n", c.name, i);
                ok = false;
                break;
            }
        }
        check(ok, "todos los casos cortados al azar (1000 iteraciones)");
    }
    {
        FrameParser p(1u << 20);
        std::vector<FrameParser::Item> out;
        const std::string f = frame(FIN | 0x1, "x", true);
        p.feed(f.data(), f.size(), out);
        check(p.failed(), "failed() tras un error");
    }

    std::printf("Saludo\n");
    {
        HandshakeRequest req;
        req.host = "example.com";
        req.port = 8080;
        req.path = "/chat?sala=1";
        req.headers = {{"Authorization", "Bearer abc"}};
        req.protocols = {"chat", "v2"};
        req.key = kKey;
        const std::string h = buildHandshake(req);
        check(h.rfind("GET /chat?sala=1 HTTP/1.1\r\n", 0) == 0, "buildHandshake: linea GET con la query");
        check(h.find("Host: example.com:8080\r\n") != std::string::npos, "buildHandshake: Host con puerto");
        check(h.find("Upgrade: websocket\r\n") != std::string::npos && h.find("Connection: Upgrade\r\n") != std::string::npos &&
                  h.find("Sec-WebSocket-Version: 13\r\n") != std::string::npos &&
                  h.find(std::string("Sec-WebSocket-Key: ") + kKey + "\r\n") != std::string::npos,
              "buildHandshake: cabeceras del protocolo");
        check(h.find("Sec-WebSocket-Protocol: chat, v2\r\n") != std::string::npos, "buildHandshake: subprotocolos");
        check(h.find("Authorization: Bearer abc\r\n") != std::string::npos, "buildHandshake: cabecera propia");
        check(h.size() >= 4 && h.substr(h.size() - 4) == "\r\n\r\n" && headerEnd(h) == h.size(),
              "buildHandshake termina en linea vacia");
        req.port = 443;
        req.secure = true;
        req.protocols.clear();
        const std::string h2 = buildHandshake(req);
        check(h2.find("Host: example.com\r\n") != std::string::npos && h2.find("Sec-WebSocket-Protocol") == std::string::npos,
              "buildHandshake: sin puerto por defecto ni subprotocolos");
        req.host = "::1";
        req.port = 9000;
        req.secure = false;
        check(buildHandshake(req).find("Host: [::1]:9000\r\n") != std::string::npos, "buildHandshake: IPv6 entre corchetes");

        const std::string key = makeKey();
        std::string raw;
        check(key.size() == 24 && base64Decode(key, raw) && raw.size() == 16 && key != makeKey(),
              "makeKey: 16 bytes al azar en base64");
    }
    {
        const std::vector<std::string> none;
        const std::string head = response("");
        const std::string buffer = head + frame(FIN | 0x1, "hi");
        const std::size_t end = headerEnd(buffer);
        check(end == head.size(), "headerEnd: justo tras la linea vacia (sobra lo de detras)");
        check(headerEnd("HTTP/1.1 101 x\r\nA: b\r\n") == std::string::npos, "headerEnd: npos si falta");
        const HandshakeResult r = verifyHandshake(std::string_view(buffer).substr(0, end), kKey, none);
        check(r.ok && r.status == 101 && r.protocol.empty(), "verifyHandshake: valido (cabeceras sin distinguir mayusculas)");
        FrameParser p(1024);
        std::vector<FrameParser::Item> items;
        p.feed(buffer.data() + end, buffer.size() - end, items);
        check(items.size() == 1 && items[0].kind == Kind::Text && items[0].data == "hi", "lo que sobra tras el saludo son tramas");

        const HandshakeResult missing = verifyHandshake(response("", nullptr), kKey, none);
        check(!missing.ok && missing.fatal, "verifyHandshake: sin Sec-WebSocket-Accept no vale");
        const HandshakeResult wrong = verifyHandshake(response("", "AAAAAAAAAAAAAAAAAAAAAAAAAAA="), kKey, none);
        check(!wrong.ok && wrong.fatal, "verifyHandshake: accept mal no vale");
        const HandshakeResult ext = verifyHandshake(response("Sec-WebSocket-Extensions: permessage-deflate\r\n"), kKey, none);
        check(!ext.ok && ext.fatal && ext.error.find("extensiones") != std::string::npos, "verifyHandshake: extensiones no");
        const HandshakeResult unreq = verifyHandshake(response("Sec-WebSocket-Protocol: otro\r\n"), kKey, {"chat"});
        check(!unreq.ok && unreq.fatal && unreq.error.find("(otro)") != std::string::npos,
              "verifyHandshake: subprotocolo no pedido");
        const HandshakeResult unreq2 = verifyHandshake(response("Sec-WebSocket-Protocol: chat\r\n"), kKey, none);
        check(!unreq2.ok && unreq2.fatal, "verifyHandshake: subprotocolo sin haber pedido ninguno");
        const HandshakeResult chosen = verifyHandshake(response("sec-websocket-protocol: v2\r\n"), kKey, {"chat", "v2"});
        check(chosen.ok && chosen.protocol == "v2", "verifyHandshake: subprotocolo elegido");
        const HandshakeResult noproto = verifyHandshake(response(""), kKey, {"chat"});
        check(!noproto.ok && noproto.fatal, "verifyHandshake: se pidio subprotocolo y no eligio ninguno");

        const HandshakeResult s401 = verifyHandshake("HTTP/1.1 401 Unauthorized\r\nContent-Length: 0\r\n\r\n", kKey, none);
        check(!s401.ok && s401.fatal && s401.status == 401 && s401.error.find("revisa el token") != std::string::npos,
              "verifyHandshake: 401 fatal");
        const HandshakeResult s404 = verifyHandshake("HTTP/1.1 404 Not Found\r\n\r\n", kKey, none);
        check(!s404.ok && s404.fatal && s404.status == 404, "verifyHandshake: 404 fatal");
        const HandshakeResult s503 = verifyHandshake("HTTP/1.1 503 Service Unavailable\r\n\r\n", kKey, none);
        check(!s503.ok && !s503.fatal && s503.status == 503, "verifyHandshake: 503 no fatal");
        const HandshakeResult s429 = verifyHandshake("HTTP/1.1 429 Too Many\r\n\r\n", kKey, none);
        check(!s429.ok && !s429.fatal, "verifyHandshake: 429 no fatal");
        const HandshakeResult junk = verifyHandshake("SSH-2.0-OpenSSH\r\n\r\n", kKey, none);
        check(!junk.ok, "verifyHandshake: algo que no es HTTP");
    }

    std::printf("\n%d comprobaciones, %d fallos\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
