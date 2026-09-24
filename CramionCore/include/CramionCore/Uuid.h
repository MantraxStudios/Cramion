#ifndef CRAMION_CORE_UUID_H
#define CRAMION_CORE_UUID_H

#include <cstdint>
#include <cstdio>
#include <functional>
#include <random>
#include <string>
#include <string_view>

namespace cramion {

// Identificador unico de 128 bits (UUID v4 aleatorio). Lo llevan los assets
// (.crdata, .crscene) y las entidades de las escenas: las referencias entre
// ellos van por UUID, no por ruta ni por puntero, asi que mover o renombrar un
// archivo no rompe nada.
//
// Texto: "xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx" (36 caracteres, minusculas).
struct Uuid {
    std::uint64_t high = 0;
    std::uint64_t low = 0;

    static Uuid generate() {
        static thread_local std::mt19937_64 engine{std::random_device{}() ^
                                                  (static_cast<std::uint64_t>(std::random_device{}()) << 32)};
        Uuid id{engine(), engine()};
        // Version 4 y variante RFC 4122.
        id.high = (id.high & 0xFFFFFFFFFFFF0FFFull) | 0x0000000000004000ull;
        id.low = (id.low & 0x3FFFFFFFFFFFFFFFull) | 0x8000000000000000ull;
        return id;
    }

    bool valid() const { return high != 0 || low != 0; }
    explicit operator bool() const { return valid(); }

    std::string toString() const {
        char text[37];
        std::snprintf(text, sizeof(text), "%08x-%04x-%04x-%04x-%012llx",
                      static_cast<unsigned>(high >> 32), static_cast<unsigned>((high >> 16) & 0xFFFF),
                      static_cast<unsigned>(high & 0xFFFF), static_cast<unsigned>(low >> 48),
                      static_cast<unsigned long long>(low & 0xFFFFFFFFFFFFull));
        return text;
    }

    // Uuid{} (invalido) si el texto no tiene el formato.
    static Uuid parse(std::string_view text) {
        if (text.size() != 36 || text[8] != '-' || text[13] != '-' || text[18] != '-' ||
            text[23] != '-') {
            return {};
        }
        Uuid id{};
        int nibbles = 0;
        for (const char c : text) {
            if (c == '-') {
                continue;
            }
            std::uint64_t value = 0;
            if (c >= '0' && c <= '9') {
                value = static_cast<std::uint64_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                value = static_cast<std::uint64_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                value = static_cast<std::uint64_t>(c - 'A' + 10);
            } else {
                return {};
            }
            std::uint64_t& half = nibbles < 16 ? id.high : id.low;
            half = (half << 4) | value;
            ++nibbles;
        }
        return id;
    }

    friend bool operator==(const Uuid&, const Uuid&) = default;
    friend auto operator<=>(const Uuid&, const Uuid&) = default;
};

}  // namespace cramion

template <>
struct std::hash<cramion::Uuid> {
    std::size_t operator()(const cramion::Uuid& id) const noexcept {
        return static_cast<std::size_t>(id.high ^ (id.low * 0x9E3779B97F4A7C15ull));
    }
};

#endif  // CRAMION_CORE_UUID_H
