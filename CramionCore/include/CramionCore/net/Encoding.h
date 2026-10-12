#ifndef CRAMION_CORE_NET_ENCODING_H
#define CRAMION_CORE_NET_ENCODING_H

// Base64, UTF-8, SHA-1 y bytes aleatorios, sin dependencias. Los usan WebSocket,
// la API de scripts (Base64.*) y el editor (EditorMcp).

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace cramion::net {

// RFC 4648 con relleno '='.
std::string base64Encode(std::string_view bytes);
// Con o sin relleno; cualquier otro caracter (espacios, saltos) no vale.
// false si no es base64 valido (out queda vacio).
bool base64Decode(std::string_view text, std::string& out);
// UTF-8 bien formado: sin sobrelargos, sin sustitutos (D800-DFFF), <= U+10FFFF, sin cortar.
bool validUtf8(std::string_view text);
// Como mucho max_bytes sin partir un caracter UTF-8.
std::string truncateUtf8(std::string_view text, std::size_t max_bytes);
// SHA-1 (FIPS 180-4). Solo para el saludo WebSocket: no es para seguridad.
std::array<std::uint8_t, 20> sha1(std::string_view data);
// Bytes aleatorios del sistema (BCryptGenRandom / getrandom o /dev/urandom / arc4random_buf).
bool secureRandom(void* out, std::size_t n);

}  // namespace cramion::net

#endif  // CRAMION_CORE_NET_ENCODING_H
