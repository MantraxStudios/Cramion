#include "CramionCore/net/Encoding.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#elif defined(__ANDROID__)
#include <stdlib.h>  // arc4random_buf
#else
#include <cerrno>
#include <cstdio>
#include <sys/random.h>
#include <sys/types.h>
#endif

#include <cstring>
#include <utility>

namespace cramion::net {

namespace {

constexpr char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int b64Value(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

inline std::uint32_t rol(std::uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

}  // namespace

std::string base64Encode(std::string_view bytes) {
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    const auto* d = reinterpret_cast<const unsigned char*>(bytes.data());
    const std::size_t n = bytes.size();
    std::size_t i = 0;
    for (; i + 3 <= n; i += 3) {
        const std::uint32_t v = (std::uint32_t(d[i]) << 16) | (std::uint32_t(d[i + 1]) << 8) | d[i + 2];
        out += kB64[(v >> 18) & 63];
        out += kB64[(v >> 12) & 63];
        out += kB64[(v >> 6) & 63];
        out += kB64[v & 63];
    }
    if (i < n) {
        std::uint32_t v = std::uint32_t(d[i]) << 16;
        if (i + 1 < n) v |= std::uint32_t(d[i + 1]) << 8;
        out += kB64[(v >> 18) & 63];
        out += kB64[(v >> 12) & 63];
        out += i + 1 < n ? kB64[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

bool base64Decode(std::string_view text, std::string& out) {
    out.clear();
    // Relleno: solo al final, como mucho 2, y entonces la longitud es multiplo de 4.
    std::size_t len = text.size();
    std::size_t pad = 0;
    while (len > 0 && text[len - 1] == '=' && pad < 2) {
        --len;
        ++pad;
    }
    if (pad > 0 && text.size() % 4 != 0) return false;
    if (len % 4 == 1) return false;
    if (pad > 0 && (len % 4) + pad != 4) return false;
    std::string result;
    result.reserve(len / 4 * 3 + 2);
    std::uint32_t acc = 0;
    int bits = 0;
    for (std::size_t i = 0; i < len; ++i) {
        const int v = b64Value(static_cast<unsigned char>(text[i]));
        if (v < 0) return false;
        acc = (acc << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            result += static_cast<char>((acc >> bits) & 0xFF);
        }
    }
    out = std::move(result);
    return true;
}

bool validUtf8(std::string_view text) {
    const auto* s = reinterpret_cast<const unsigned char*>(text.data());
    const std::size_t n = text.size();
    std::size_t i = 0;
    while (i < n) {
        const unsigned char c = s[i];
        if (c < 0x80) {
            ++i;
            // Rapido por bloques ASCII.
            while (i + 8 <= n) {
                std::uint64_t w;
                std::memcpy(&w, s + i, 8);
                if (w & 0x8080808080808080ull) break;
                i += 8;
            }
            continue;
        }
        std::size_t len;
        std::uint32_t cp;
        if (c >= 0xC2 && c <= 0xDF) {
            len = 2;
            cp = c & 0x1F;
        } else if (c >= 0xE0 && c <= 0xEF) {
            len = 3;
            cp = c & 0x0F;
        } else if (c >= 0xF0 && c <= 0xF4) {
            len = 4;
            cp = c & 0x07;
        } else {
            return false;  // continuacion suelta, C0/C1 (sobrelargo) o F5+
        }
        if (i + len > n) return false;  // cortado
        for (std::size_t k = 1; k < len; ++k) {
            const unsigned char cc = s[i + k];
            if ((cc & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (len == 3 && cp < 0x800) return false;
        if (len == 4 && (cp < 0x10000 || cp > 0x10FFFF)) return false;
        if (cp >= 0xD800 && cp <= 0xDFFF) return false;
        i += len;
    }
    return true;
}

std::string truncateUtf8(std::string_view text, std::size_t max_bytes) {
    if (text.size() <= max_bytes) return std::string(text);
    std::size_t end = max_bytes;
    // No empezar el corte en mitad de un caracter: retroceder sobre las continuaciones.
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) --end;
    return std::string(text.substr(0, end));
}

std::array<std::uint8_t, 20> sha1(std::string_view data) {
    std::uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    auto block = [&h](const unsigned char* p) {
        std::uint32_t w[80];
        for (int t = 0; t < 16; ++t)
            w[t] = (std::uint32_t(p[t * 4]) << 24) | (std::uint32_t(p[t * 4 + 1]) << 16) |
                   (std::uint32_t(p[t * 4 + 2]) << 8) | std::uint32_t(p[t * 4 + 3]);
        for (int t = 16; t < 80; ++t) w[t] = rol(w[t - 3] ^ w[t - 8] ^ w[t - 14] ^ w[t - 16], 1);
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int t = 0; t < 80; ++t) {
            std::uint32_t f, k;
            if (t < 20) {
                f = (b & c) | (~b & d);
                k = 0x5A827999u;
            } else if (t < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1u;
            } else if (t < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDCu;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6u;
            }
            const std::uint32_t tmp = rol(a, 5) + f + e + k + w[t];
            e = d;
            d = c;
            c = rol(b, 30);
            b = a;
            a = tmp;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    };

    const auto* p = reinterpret_cast<const unsigned char*>(data.data());
    const std::size_t n = data.size();
    std::size_t i = 0;
    for (; i + 64 <= n; i += 64) block(p + i);
    unsigned char tail[128] = {};
    const std::size_t rest = n - i;
    if (rest > 0) std::memcpy(tail, p + i, rest);
    tail[rest] = 0x80;
    const std::size_t tail_len = rest + 1 + 8 <= 64 ? 64 : 128;
    const std::uint64_t bits = static_cast<std::uint64_t>(n) * 8u;
    for (int k = 0; k < 8; ++k) tail[tail_len - 1 - k] = static_cast<unsigned char>(bits >> (8 * k));
    block(tail);
    if (tail_len == 128) block(tail + 64);

    std::array<std::uint8_t, 20> out{};
    for (int k = 0; k < 5; ++k) {
        out[k * 4] = static_cast<std::uint8_t>(h[k] >> 24);
        out[k * 4 + 1] = static_cast<std::uint8_t>(h[k] >> 16);
        out[k * 4 + 2] = static_cast<std::uint8_t>(h[k] >> 8);
        out[k * 4 + 3] = static_cast<std::uint8_t>(h[k]);
    }
    return out;
}

bool secureRandom(void* out, std::size_t n) {
    if (n == 0) return true;
    auto* p = static_cast<unsigned char*>(out);
#if defined(_WIN32)
    while (n > 0) {
        const ULONG chunk = n > 0x40000000u ? 0x40000000u : static_cast<ULONG>(n);
        if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, p, chunk, BCRYPT_USE_SYSTEM_PREFERRED_RNG))) return false;
        p += chunk;
        n -= chunk;
    }
    return true;
#elif defined(__ANDROID__)
    arc4random_buf(p, n);
    return true;
#else
    while (n > 0) {
        const ssize_t r = getrandom(p, n, 0);
        if (r > 0) {
            p += r;
            n -= static_cast<std::size_t>(r);
            continue;
        }
        if (r < 0 && errno == EINTR) continue;
        break;  // ENOSYS (nucleo antiguo) u otro: /dev/urandom
    }
    if (n == 0) return true;
    std::FILE* f = std::fopen("/dev/urandom", "rb");
    if (f == nullptr) return false;
    const std::size_t got = std::fread(p, 1, n, f);
    std::fclose(f);
    return got == n;
#endif
}

}  // namespace cramion::net
